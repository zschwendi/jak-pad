#include "Jak1SourceObjectPack.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <new>
#include <set>
#include <string_view>
#include <vector>

#include "common/custom_data/Jak1PublicOutputGraph.h"

#include "goalc/make/Jak1OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_source_object_pack {
namespace {

namespace fs = std::filesystem;
namespace generator = jak1_output_recipe_generator;

Error make_error(ErrorCode code,
                 std::string message,
                 std::optional<std::uint32_t> entry_index = {}) {
  return {code, std::move(message), entry_index};
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_manifest_bytes > 0 &&
         limits.max_manifest_bytes <= std::numeric_limits<std::size_t>::max() &&
         limits.max_object_bytes > 0 && limits.max_total_object_bytes > 0 &&
         limits.io_chunk_bytes > 0 &&
         limits.io_chunk_bytes <=
             static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) &&
         (!options.expected_identity ||
          (options.expected_identity->object_count == kExpectedObjectCount &&
           options.expected_identity->aggregate_xxh64 != 0));
}

std::optional<Error> cancelled(const Options& options,
                               std::optional<std::uint32_t> entry_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, "Source-object-pack validation was cancelled.",
                        entry_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed,
                      "The source-object-pack cancellation callback failed.", entry_index);
  }
  return {};
}

std::optional<Error> report(const Options& options, Progress progress) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress(progress);
  } catch (...) {
    return make_error(ErrorCode::callback_failed,
                      "The source-object-pack progress callback failed.");
  }
  return {};
}

ErrorCode map_manifest_error(generator::ErrorCode code) {
  switch (code) {
    case generator::ErrorCode::cancelled:
      return ErrorCode::cancelled;
    case generator::ErrorCode::callback_failed:
      return ErrorCode::callback_failed;
    case generator::ErrorCode::allocation_failed:
      return ErrorCode::allocation_failed;
    default:
      return ErrorCode::manifest_invalid;
  }
}

Result<std::string> read_manifest(const fs::path& root, const Options& options) {
  const auto path = root / kManifestName;
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || status.type() == fs::file_type::not_found) {
    return Result<std::string>::failure(
        make_error(ErrorCode::manifest_missing, "The source-object-pack manifest is missing."));
  }
  if (status.type() != fs::file_type::regular) {
    return Result<std::string>::failure(make_error(
        ErrorCode::unsafe_entry, "The source-object-pack manifest is not a direct regular file."));
  }
  const auto size = fs::file_size(path, error);
  if (error) {
    return Result<std::string>::failure(make_error(
        ErrorCode::manifest_read_failed, "The source-object-pack manifest size is unreadable."));
  }
  if (size == 0 || size > options.limits.max_manifest_bytes ||
      size > std::numeric_limits<std::size_t>::max() ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    return Result<std::string>::failure(make_error(
        ErrorCode::manifest_too_large,
        "The source-object-pack manifest is empty or exceeds its configured byte limit."));
  }
  if (const auto callback_error =
          report(options, {Phase::reading_manifest, 0, 1, 0, std::string(kManifestName)})) {
    return Result<std::string>::failure(*callback_error);
  }
  if (const auto cancel_error = cancelled(options)) {
    return Result<std::string>::failure(*cancel_error);
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::string>::failure(make_error(
        ErrorCode::manifest_read_failed, "The source-object-pack manifest could not be opened."));
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || !input) {
    return Result<std::string>::failure(make_error(
        ErrorCode::manifest_read_failed, "The source-object-pack manifest changed or truncated."));
  }
  char extra = 0;
  if (input.get(extra)) {
    return Result<std::string>::failure(make_error(
        ErrorCode::manifest_read_failed, "The source-object-pack manifest changed while read."));
  }
  if (const auto callback_error =
          report(options, {Phase::reading_manifest, 1, 1, size, std::string(kManifestName)})) {
    return Result<std::string>::failure(*callback_error);
  }
  return Result<std::string>::success(std::move(bytes));
}

std::optional<Error> verify_exact_contents(const fs::path& root,
                                           const generator::SourceObjectPackManifest& manifest) {
  std::set<std::string> expected = {kManifestName};
  for (const auto& entry : manifest.entries) {
    expected.emplace(entry.bundle_relative_path);
  }

  std::set<std::string> actual;
  std::error_code error;
  fs::directory_iterator iterator(root, error);
  const fs::directory_iterator end;
  if (error) {
    return make_error(ErrorCode::root_missing,
                      "The source-object-pack root could not be enumerated.");
  }
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return make_error(ErrorCode::contents_mismatch,
                        "The source-object-pack root changed while it was enumerated.");
    }
    const auto status = iterator->symlink_status(error);
    if (error || status.type() != fs::file_type::regular) {
      return make_error(ErrorCode::unsafe_entry,
                        "The source-object pack contains a non-regular direct entry.");
    }
    actual.emplace(iterator->path().filename().string());
  }
  if (error || actual != expected) {
    return make_error(ErrorCode::contents_mismatch,
                      "The source-object-pack contents do not exactly match its manifest.");
  }
  return {};
}

Result<std::uint64_t> hash_object(const fs::path& path,
                                  const generator::SourceObjectPackEntry& entry,
                                  std::uint32_t entry_index,
                                  std::uint64_t bytes_before,
                                  const Options& options) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || status.type() != fs::file_type::regular) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::unsafe_entry,
                   "A source object is missing or is not a direct regular file.", entry_index));
  }
  const auto size = fs::file_size(path, error);
  if (error || size != entry.size) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::object_hash_mismatch,
                   "A source object does not match its manifested byte size.", entry_index));
  }
  if (size > options.limits.max_object_bytes) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::object_too_large,
                   "A source object exceeds its configured byte limit.", entry_index));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::uint64_t>::failure(make_error(
        ErrorCode::object_read_failed, "A source object could not be opened.", entry_index));
  }
  XXH64_state_t state;
  if (XXH64_reset(&state, 0) == XXH_ERROR) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::object_read_failed, "A source object hash could not be initialized.",
                   entry_index));
  }
  std::vector<char> buffer(options.limits.io_chunk_bytes);
  std::uint64_t consumed = 0;
  while (consumed < size) {
    if (const auto cancel_error = cancelled(options, entry_index)) {
      return Result<std::uint64_t>::failure(*cancel_error);
    }
    const auto chunk =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - consumed));
    input.read(buffer.data(), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk) || !input ||
        XXH64_update(&state, buffer.data(), chunk) == XXH_ERROR) {
      return Result<std::uint64_t>::failure(
          make_error(ErrorCode::object_read_failed, "A source object could not be hashed exactly.",
                     entry_index));
    }
    consumed += chunk;
    if (const auto callback_error =
            report(options, {Phase::validating_objects, entry_index, kExpectedObjectCount,
                             bytes_before + consumed, entry.bundle_relative_path})) {
      return Result<std::uint64_t>::failure(*callback_error);
    }
  }
  char extra = 0;
  if (input.get(extra) || XXH64_digest(&state) != entry.xxh64) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::object_hash_mismatch,
                   "A source object does not match its manifested hash.", entry_index));
  }
  return Result<std::uint64_t>::success(size);
}

}  // namespace

Result<Summary> validate(const fs::path& root, const Options& options) {
  try {
    if (!valid_options(options) || !root.is_absolute()) {
      return Result<Summary>::failure(make_error(
          ErrorCode::invalid_argument, "The source-object-pack validator options are invalid."));
    }
    std::error_code error;
    const auto root_status = fs::symlink_status(root, error);
    if (error || root_status.type() != fs::file_type::directory) {
      return Result<Summary>::failure(
          make_error(ErrorCode::root_missing,
                     "The source-object-pack root is missing, linked, or not a directory."));
    }

    auto manifest_bytes = read_manifest(root, options);
    if (!manifest_bytes) {
      return Result<Summary>::failure(manifest_bytes.error());
    }
    generator::Options parse_options;
    parse_options.expected_source_object_count = kExpectedObjectCount;
    parse_options.limits.max_manifest_bytes =
        static_cast<std::size_t>(options.limits.max_manifest_bytes);
    parse_options.should_cancel = options.should_cancel;
    auto manifest =
        generator::parse_source_object_pack_manifest(manifest_bytes.value(), parse_options);
    if (!manifest) {
      return Result<Summary>::failure(
          make_error(map_manifest_error(manifest.error().code), manifest.error().message));
    }
    if (options.expected_identity && manifest.value().identity != *options.expected_identity) {
      return Result<Summary>::failure(
          make_error(ErrorCode::wrong_identity,
                     "The source-object-pack identity does not match the expected build input."));
    }

    jak1_output_graph::Options graph_options;
    graph_options.should_cancel = options.should_cancel;
    const auto graph = jak1_public_output_graph::decode(graph_options);
    if (!graph) {
      const auto code = graph.error().code == jak1_output_graph::ErrorCode::cancelled
                            ? ErrorCode::cancelled
                        : graph.error().code == jak1_output_graph::ErrorCode::callback_failed
                            ? ErrorCode::callback_failed
                            : ErrorCode::source_graph_mismatch;
      return Result<Summary>::failure(make_error(code, graph.error().message));
    }
    if (graph.value().ordered_source_files.size() != kExpectedObjectCount ||
        manifest.value().entries.size() != graph.value().ordered_source_files.size()) {
      return Result<Summary>::failure(
          make_error(ErrorCode::source_graph_mismatch,
                     "The source-object pack does not match the embedded public source graph."));
    }
    for (std::size_t index = 0; index < manifest.value().entries.size(); ++index) {
      if (manifest.value().entries[index].source_file !=
          graph.value().ordered_source_files[index]) {
        return Result<Summary>::failure(make_error(
            ErrorCode::source_graph_mismatch,
            "The source-object-pack order differs from the embedded public source graph.",
            static_cast<std::uint32_t>(index)));
      }
    }
    if (const auto contents_error = verify_exact_contents(root, manifest.value())) {
      return Result<Summary>::failure(*contents_error);
    }

    std::uint64_t total = 0;
    for (std::size_t index = 0; index < manifest.value().entries.size(); ++index) {
      const auto& entry = manifest.value().entries[index];
      if (entry.size > options.limits.max_total_object_bytes - total) {
        return Result<Summary>::failure(
            make_error(ErrorCode::object_too_large,
                       "The source-object pack exceeds its configured aggregate byte limit.",
                       static_cast<std::uint32_t>(index)));
      }
      auto hashed = hash_object(root / entry.bundle_relative_path, entry,
                                static_cast<std::uint32_t>(index), total, options);
      if (!hashed) {
        return Result<Summary>::failure(hashed.error());
      }
      total += hashed.value();
      if (const auto callback_error =
              report(options, {Phase::validating_objects, static_cast<std::uint32_t>(index + 1),
                               kExpectedObjectCount, total, entry.bundle_relative_path})) {
        return Result<Summary>::failure(*callback_error);
      }
    }
    return Result<Summary>::success({manifest.value().identity, total});
  } catch (const std::bad_alloc&) {
    return Result<Summary>::failure(make_error(ErrorCode::allocation_failed,
                                               "Source-object-pack validation ran out of memory."));
  } catch (...) {
    return Result<Summary>::failure(make_error(
        ErrorCode::object_read_failed, "Source-object-pack validation failed unexpectedly."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::root_missing:
      return "root_missing";
    case ErrorCode::unsafe_entry:
      return "unsafe_entry";
    case ErrorCode::manifest_missing:
      return "manifest_missing";
    case ErrorCode::manifest_too_large:
      return "manifest_too_large";
    case ErrorCode::manifest_read_failed:
      return "manifest_read_failed";
    case ErrorCode::manifest_invalid:
      return "manifest_invalid";
    case ErrorCode::wrong_identity:
      return "wrong_identity";
    case ErrorCode::source_graph_mismatch:
      return "source_graph_mismatch";
    case ErrorCode::contents_mismatch:
      return "contents_mismatch";
    case ErrorCode::object_too_large:
      return "object_too_large";
    case ErrorCode::object_read_failed:
      return "object_read_failed";
    case ErrorCode::object_hash_mismatch:
      return "object_hash_mismatch";
  }
  return "unknown";
}

}  // namespace jak1_source_object_pack
