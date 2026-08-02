#include "jak1_retail_object_catalog.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#define XXH_STATIC_LINKING_ONLY
#include "third-party/zstd/lib/common/xxhash.h"
#undef XXH_STATIC_LINKING_ONLY

namespace jak1_retail_object_catalog {
namespace {

constexpr std::size_t kLinkHeaderV2Bytes = 12;
constexpr std::size_t kLinkHeaderV4Bytes = 16;
constexpr std::size_t kLinkHeaderV3Bytes = 128;
constexpr std::size_t kV3NameOffset = 16;
constexpr std::size_t kV3NameBytes = 64;
constexpr std::size_t kV3SegmentInfoOffset = 80;
constexpr std::size_t kV3SegmentInfoBytes = 16;
constexpr std::size_t kObjectAlignment = 16;
constexpr std::size_t kV2LinkAlignment = 64;

struct Fingerprint {
  std::size_t byte_size = 0;
  std::uint64_t xxh64 = 0;
  ObjectVersion version = ObjectVersion::v2;

  bool operator==(const Fingerprint&) const = default;
};

Error make_error(ErrorCode code,
                 std::string message,
                 std::string source_path = {},
                 std::optional<std::uint32_t> object_index = {},
                 std::optional<std::uint16_t> version = {}) {
  Error error;
  error.code = code;
  error.source_archive_relative_path = std::move(source_path);
  error.archive_object_index = object_index;
  error.detected_object_version = version;
  error.message = std::move(message);
  return error;
}

std::uint16_t read_u16_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8);
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
         (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool align_up(std::size_t value, std::size_t alignment, std::size_t* result) {
  const auto remainder = value % alignment;
  return remainder == 0 ? (*result = value, true)
                        : checked_add(value, alignment - remainder, result);
}

std::optional<std::string> validate_source_path(const std::string& path, const Options& options) {
  if (path.empty() || path.size() > options.max_source_path_bytes || path.front() == '/' ||
      path.back() == '/') {
    return "The source archive path is empty, absolute, or exceeds its configured limit.";
  }
  std::size_t component_start = 0;
  for (std::size_t index = 0; index <= path.size(); ++index) {
    if (index != path.size() && path[index] != '/') {
      const auto byte = static_cast<unsigned char>(path[index]);
      const bool allowed = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                           byte == '.';
      if (!allowed) {
        return "The source archive path contains a nonportable character.";
      }
      continue;
    }
    const auto component = std::string_view(path).substr(component_start, index - component_start);
    if (component.empty() || component == "." || component == "..") {
      return "The source archive path contains an empty or traversal component.";
    }
    component_start = index + 1;
  }
  const auto separator = path.find_last_of('/');
  const auto basename =
      std::string_view(path).substr(separator == std::string::npos ? 0 : separator + 1);
  if (!basename.ends_with(".DGO") && !basename.ends_with(".CGO")) {
    return "The source archive path is not an uppercase retail DGO or CGO path.";
  }
  return {};
}

std::string archive_basename(const std::string& path) {
  const auto separator = path.find_last_of('/');
  return path.substr(separator == std::string::npos ? 0 : separator + 1);
}

enum class CancelState {
  continue_work,
  cancelled,
  callback_failed,
};

CancelState poll_cancel(const Options& options) {
  if (!options.should_cancel) {
    return CancelState::continue_work;
  }
  try {
    return options.should_cancel() ? CancelState::cancelled : CancelState::continue_work;
  } catch (...) {
    return CancelState::callback_failed;
  }
}

std::optional<Error> emit_progress(const Options& options, const Progress& progress) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress(progress);
    return {};
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The catalog progress callback failed.",
                      progress.source_archive_relative_path, progress.archive_object_index);
  }
}

Result<ObjectVersion> inspect_v2_or_v4_header(std::span<const std::uint8_t> data,
                                              const std::string& source_path,
                                              std::uint32_t object_index) {
  if (data.size() < kLinkHeaderV2Bytes) {
    return Result<ObjectVersion>::failure(make_error(ErrorCode::invalid_object_header,
                                                     "The retail object link header is truncated.",
                                                     source_path, object_index));
  }
  const auto type_tag = read_u32_le(data, 0);
  const auto link_length = static_cast<std::size_t>(read_u32_le(data, 4));
  const auto version32 = read_u32_le(data, 8);
  const auto version16 = read_u16_le(data, 8);
  if (version32 != version16) {
    return Result<ObjectVersion>::failure(make_error(
        ErrorCode::invalid_object_header, "The retail object version field is malformed.",
        source_path, object_index, version16));
  }
  if (version32 == 2) {
    if (type_tag != std::numeric_limits<std::uint32_t>::max() || link_length < kLinkHeaderV2Bytes ||
        link_length >= data.size() || link_length % kV2LinkAlignment != 0) {
      return Result<ObjectVersion>::failure(make_error(
          ErrorCode::invalid_object_header, "The Jak 1 V2 data-object header is invalid.",
          source_path, object_index, version16));
    }
    return Result<ObjectVersion>::success(ObjectVersion::v2);
  }
  if (version32 != 4) {
    return Result<ObjectVersion>::failure(
        make_error(ErrorCode::unsupported_object_version,
                   "The retail object version is not a Jak 1 V2 or V4 data object.", source_path,
                   object_index, version16));
  }
  if (data.size() < kLinkHeaderV4Bytes || type_tag != std::numeric_limits<std::uint32_t>::max() ||
      link_length < kLinkHeaderV2Bytes || link_length % kV2LinkAlignment != 0) {
    return Result<ObjectVersion>::failure(make_error(ErrorCode::invalid_object_header,
                                                     "The Jak 1 V4 data-object header is invalid.",
                                                     source_path, object_index, version16));
  }

  const auto code_size = static_cast<std::size_t>(read_u32_le(data, 12));
  std::size_t trailing_header_offset = 0;
  std::size_t object_end = 0;
  if (code_size == 0 || code_size % kObjectAlignment != 0 ||
      !checked_add(kLinkHeaderV4Bytes, code_size, &trailing_header_offset) ||
      !checked_add(trailing_header_offset, link_length, &object_end) || object_end != data.size() ||
      data.size() - trailing_header_offset < kLinkHeaderV2Bytes ||
      read_u32_le(data, trailing_header_offset) != std::numeric_limits<std::uint32_t>::max() ||
      read_u32_le(data, trailing_header_offset + 4) != link_length ||
      read_u32_le(data, trailing_header_offset + 8) != 2) {
    return Result<ObjectVersion>::failure(make_error(
        ErrorCode::invalid_object_header,
        "The Jak 1 V4 object does not contain a matching bounded trailing V2 link header.",
        source_path, object_index, version16));
  }
  return Result<ObjectVersion>::success(ObjectVersion::v4);
}

Result<ObjectVersion> inspect_v3_header(std::span<const std::uint8_t> data,
                                        const std::string& source_path,
                                        std::uint32_t object_index,
                                        const std::string& internal_name) {
  if (data.size() < kLinkHeaderV3Bytes || read_u32_le(data, 0) != 0 || read_u32_le(data, 8) != 3 ||
      read_u32_le(data, 12) != 3) {
    return Result<ObjectVersion>::failure(make_error(ErrorCode::invalid_object_header,
                                                     "The Jak 1 V3 object header is invalid.",
                                                     source_path, object_index, 3));
  }
  const auto link_length = static_cast<std::size_t>(read_u32_le(data, 4));
  if (link_length < kLinkHeaderV3Bytes || link_length > data.size() ||
      link_length % kObjectAlignment != 0) {
    return Result<ObjectVersion>::failure(make_error(ErrorCode::invalid_object_header,
                                                     "The Jak 1 V3 link-data bound is invalid.",
                                                     source_path, object_index, 3));
  }

  const auto name_begin = data.begin() + kV3NameOffset;
  const auto name_end = name_begin + kV3NameBytes;
  const auto terminator = std::find(name_begin, name_end, std::uint8_t{0});
  if (terminator == name_end ||
      std::string(reinterpret_cast<const char*>(&*name_begin), terminator - name_begin) !=
          internal_name ||
      std::any_of(terminator + 1, name_end, [](std::uint8_t byte) { return byte != 0; })) {
    return Result<ObjectVersion>::failure(
        make_error(ErrorCode::invalid_object_header,
                   "The Jak 1 V3 embedded object name does not match its DGO object name.",
                   source_path, object_index, 3));
  }

  std::array<std::size_t, 3> data_offsets{};
  std::array<std::size_t, 3> data_sizes{};
  bool has_data = false;
  for (std::size_t segment = 0; segment < 3; ++segment) {
    const auto info = kV3SegmentInfoOffset + segment * kV3SegmentInfoBytes;
    const auto reloc = static_cast<std::size_t>(read_u32_le(data, info));
    const auto relative_data = static_cast<std::size_t>(read_u32_le(data, info + 4));
    const auto size = static_cast<std::size_t>(read_u32_le(data, info + 8));
    const auto magic = read_u32_le(data, info + 12);
    if (reloc >= link_length || magic != 0 ||
        !checked_add(link_length, relative_data, &data_offsets[segment]) ||
        data_offsets[segment] > data.size() || size > data.size() - data_offsets[segment]) {
      return Result<ObjectVersion>::failure(make_error(
          ErrorCode::invalid_object_header, "A Jak 1 V3 segment descriptor is out of bounds.",
          source_path, object_index, 3));
    }
    data_sizes[segment] = size;
    has_data = has_data || size != 0;
  }
  for (std::size_t segment = 0; segment < 2; ++segment) {
    std::size_t segment_end = 0;
    std::size_t aligned_end = 0;
    if (!checked_add(data_offsets[segment], data_sizes[segment], &segment_end) ||
        !align_up(segment_end, kObjectAlignment, &aligned_end) ||
        aligned_end != data_offsets[segment + 1]) {
      return Result<ObjectVersion>::failure(
          make_error(ErrorCode::invalid_object_header,
                     "The Jak 1 V3 segment layout is inconsistent.", source_path, object_index, 3));
    }
  }
  std::size_t final_end = 0;
  std::size_t aligned_final_end = 0;
  if (!has_data || !checked_add(data_offsets[2], data_sizes[2], &final_end) ||
      !align_up(final_end, kObjectAlignment, &aligned_final_end) ||
      aligned_final_end != data.size()) {
    return Result<ObjectVersion>::failure(make_error(
        ErrorCode::invalid_object_header, "The Jak 1 V3 object data extent is inconsistent.",
        source_path, object_index, 3));
  }
  return Result<ObjectVersion>::failure(make_error(
      ErrorCode::code_bearing_v3_object,
      "Jak 1 V3 objects are code-bearing retail objects and cannot enter the data catalog.",
      source_path, object_index, 3));
}

Result<ObjectVersion> inspect_object_header(std::span<const std::uint8_t> data,
                                            const std::string& source_path,
                                            std::uint32_t object_index,
                                            const std::string& internal_name) {
  if (data.size() < kLinkHeaderV2Bytes) {
    return Result<ObjectVersion>::failure(make_error(ErrorCode::invalid_object_header,
                                                     "The retail object link header is truncated.",
                                                     source_path, object_index));
  }
  if (read_u32_le(data, 8) == 3) {
    return inspect_v3_header(data, source_path, object_index, internal_name);
  }
  return inspect_v2_or_v4_header(data, source_path, object_index);
}

Result<std::uint64_t> hash_object(std::span<const std::uint8_t> data,
                                  const std::string& source_path,
                                  std::uint32_t object_index,
                                  const Options& options) {
  XXH64_state_t state;
  if (XXH64_reset(&state, 0) == XXH_ERROR) {
    return Result<std::uint64_t>::failure(
        make_error(ErrorCode::hash_failed, "XXH64 could not initialize the retail object hash.",
                   source_path, object_index));
  }
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto cancel_state = poll_cancel(options);
    if (cancel_state != CancelState::continue_work) {
      return Result<std::uint64_t>::failure(
          make_error(cancel_state == CancelState::cancelled ? ErrorCode::cancelled
                                                            : ErrorCode::callback_failed,
                     cancel_state == CancelState::cancelled ? "Retail object hashing was cancelled."
                                                            : "The cancellation callback failed.",
                     source_path, object_index));
    }
    const auto chunk = std::min(options.hash_chunk_bytes, data.size() - offset);
    if (XXH64_update(&state, data.data() + offset, chunk) == XXH_ERROR) {
      return Result<std::uint64_t>::failure(
          make_error(ErrorCode::hash_failed, "XXH64 could not update the retail object hash.",
                     source_path, object_index));
    }
    offset += chunk;
  }
  return Result<std::uint64_t>::success(XXH64_digest(&state));
}

std::optional<Error> validate_options(const Options& options) {
  if (options.max_archives == 0 || options.max_entries == 0 ||
      options.max_total_object_bytes == 0 || options.max_archive_input_bytes == 0 ||
      options.max_archive_compressed_bytes == 0 || options.max_archive_expanded_bytes == 0 ||
      options.max_object_bytes == 0 || options.max_source_path_bytes == 0 ||
      options.max_internal_name_bytes == 0 || options.max_internal_name_bytes >= 60 ||
      options.hash_chunk_bytes == 0 || options.max_archive_expansion_ratio == 0) {
    return make_error(ErrorCode::invalid_argument,
                      "The retail object catalog options are invalid.");
  }
  return {};
}

}  // namespace

Result<const Entry*> Catalog::lookup(const Provenance& expected) const {
  const auto entry = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry& candidate) {
    return candidate.provenance.source_archive_relative_path ==
               expected.source_archive_relative_path &&
           candidate.provenance.archive_object_index == expected.archive_object_index;
  });
  if (entry == m_entries.end()) {
    return Result<const Entry*>::failure(make_error(
        ErrorCode::object_not_found, "No object exists at the requested archive path and index.",
        expected.source_archive_relative_path, expected.archive_object_index));
  }
  if (entry->provenance != expected) {
    return Result<const Entry*>::failure(
        make_error(ErrorCode::provenance_mismatch,
                   "The requested object provenance does not match every indexed provenance field.",
                   expected.source_archive_relative_path, expected.archive_object_index,
                   static_cast<std::uint16_t>(entry->provenance.object_version)));
  }
  return Result<const Entry*>::success(&*entry);
}

Result<Catalog> build(std::span<const ArchiveSource> sources, const Options& options) {
  try {
    if (auto error = validate_options(options)) {
      return Result<Catalog>::failure(std::move(*error));
    }
    if (sources.empty()) {
      return Result<Catalog>::failure(
          make_error(ErrorCode::invalid_argument, "At least one retail archive is required."));
    }
    if (sources.size() > options.max_archives) {
      return Result<Catalog>::failure(
          make_error(ErrorCode::archive_limit_exceeded,
                     "The source archive count exceeds the configured catalog limit."));
    }

    std::vector<const ArchiveSource*> ordered_sources;
    ordered_sources.reserve(sources.size());
    for (const auto& source : sources) {
      if (auto path_error = validate_source_path(source.source_archive_relative_path, options)) {
        return Result<Catalog>::failure(make_error(ErrorCode::invalid_source_archive_path,
                                                   std::move(*path_error),
                                                   source.source_archive_relative_path));
      }
      ordered_sources.push_back(&source);
    }
    std::sort(ordered_sources.begin(), ordered_sources.end(),
              [](const auto* left, const auto* right) {
                return left->source_archive_relative_path < right->source_archive_relative_path;
              });
    for (std::size_t index = 1; index < ordered_sources.size(); ++index) {
      if (ordered_sources[index - 1]->source_archive_relative_path ==
          ordered_sources[index]->source_archive_relative_path) {
        return Result<Catalog>::failure(
            make_error(ErrorCode::duplicate_source_archive_path,
                       "The catalog contains the same source archive path more than once.",
                       ordered_sources[index]->source_archive_relative_path));
      }
    }

    Catalog catalog;
    catalog.m_entries.reserve(std::min(options.max_entries, sources.size() * std::size_t{64}));
    std::unordered_map<std::string, Fingerprint> fingerprints_by_name;
    std::size_t total_bytes = 0;

    for (std::size_t archive_index = 0; archive_index < ordered_sources.size(); ++archive_index) {
      const auto& source = *ordered_sources[archive_index];
      const auto cancel_state = poll_cancel(options);
      if (cancel_state != CancelState::continue_work) {
        return Result<Catalog>::failure(make_error(
            cancel_state == CancelState::cancelled ? ErrorCode::cancelled
                                                   : ErrorCode::callback_failed,
            cancel_state == CancelState::cancelled ? "Retail object cataloging was cancelled."
                                                   : "The cancellation callback failed.",
            source.source_archive_relative_path));
      }
      Progress progress{ProgressStage::starting_archive,
                        ordered_sources.size(),
                        archive_index,
                        catalog.m_entries.size(),
                        total_bytes,
                        source.source_archive_relative_path,
                        {}};
      if (auto error = emit_progress(options, progress)) {
        return Result<Catalog>::failure(std::move(*error));
      }

      bool cancellation_callback_failed = false;
      jak1_checked_dgo::Options dgo_options;
      dgo_options.max_input_bytes = options.max_archive_input_bytes;
      dgo_options.max_compressed_bytes = options.max_archive_compressed_bytes;
      dgo_options.max_expanded_bytes = options.max_archive_expanded_bytes;
      dgo_options.max_object_bytes = options.max_object_bytes;
      dgo_options.max_total_object_bytes = options.max_total_object_bytes;
      dgo_options.max_objects = static_cast<std::uint32_t>(
          std::min(options.max_entries,
                   static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())));
      dgo_options.max_name_bytes = options.max_internal_name_bytes;
      dgo_options.max_expansion_ratio = options.max_archive_expansion_ratio;
      dgo_options.should_cancel = [&]() {
        const auto state = poll_cancel(options);
        cancellation_callback_failed = state == CancelState::callback_failed;
        return state != CancelState::continue_work;
      };

      auto archive = jak1_checked_dgo::read(
          source.bytes, archive_basename(source.source_archive_relative_path), dgo_options);
      if (!archive) {
        if (cancellation_callback_failed) {
          return Result<Catalog>::failure(
              make_error(ErrorCode::callback_failed, "The cancellation callback failed.",
                         source.source_archive_relative_path, archive.error().object_index));
        }
        if (archive.error().code == jak1_checked_dgo::ErrorCode::cancelled) {
          return Result<Catalog>::failure(
              make_error(ErrorCode::cancelled, "Retail archive cataloging was cancelled.",
                         source.source_archive_relative_path, archive.error().object_index));
        }
        if (archive.error().code == jak1_checked_dgo::ErrorCode::object_count_limit_exceeded) {
          return Result<Catalog>::failure(
              make_error(ErrorCode::entry_limit_exceeded,
                         "A retail archive exceeds the configured catalog entry limit.",
                         source.source_archive_relative_path));
        }
        if (archive.error().code == jak1_checked_dgo::ErrorCode::total_object_size_limit_exceeded) {
          return Result<Catalog>::failure(
              make_error(ErrorCode::total_byte_limit_exceeded,
                         "A retail archive exceeds the configured aggregate catalog byte limit.",
                         source.source_archive_relative_path, archive.error().object_index));
        }
        auto error = make_error(ErrorCode::checked_dgo_failed,
                                "The checked DGO reader rejected the source archive.",
                                source.source_archive_relative_path, archive.error().object_index);
        error.checked_dgo_error = archive.error();
        return Result<Catalog>::failure(std::move(error));
      }

      for (std::size_t object_index = 0; object_index < archive.value().objects.size();
           ++object_index) {
        if (catalog.m_entries.size() >= options.max_entries) {
          return Result<Catalog>::failure(make_error(
              ErrorCode::entry_limit_exceeded,
              "The retail object count exceeds the configured catalog limit.",
              source.source_archive_relative_path, static_cast<std::uint32_t>(object_index)));
        }
        const auto& object = archive.value().objects[object_index];
        std::size_t new_total = 0;
        if (!checked_add(total_bytes, object.data.size(), &new_total) ||
            new_total > options.max_total_object_bytes) {
          return Result<Catalog>::failure(make_error(
              ErrorCode::total_byte_limit_exceeded,
              "Retail object payloads exceed the configured aggregate byte limit.",
              source.source_archive_relative_path, static_cast<std::uint32_t>(object_index)));
        }

        auto object_version =
            inspect_object_header(object.data, source.source_archive_relative_path,
                                  static_cast<std::uint32_t>(object_index), object.internal_name);
        if (!object_version) {
          return Result<Catalog>::failure(object_version.error());
        }
        auto hash = hash_object(object.data, source.source_archive_relative_path,
                                static_cast<std::uint32_t>(object_index), options);
        if (!hash) {
          return Result<Catalog>::failure(hash.error());
        }

        const Fingerprint fingerprint{object.data.size(), hash.value(), object_version.value()};
        const auto [existing, inserted] =
            fingerprints_by_name.emplace(object.internal_name, fingerprint);
        if (!inserted && existing->second != fingerprint) {
          return Result<Catalog>::failure(make_error(
              ErrorCode::ambiguous_duplicate_internal_name,
              "A duplicate internal object name has divergent size, hash, or object version.",
              source.source_archive_relative_path, static_cast<std::uint32_t>(object_index),
              static_cast<std::uint16_t>(object_version.value())));
        }

        Entry entry;
        entry.provenance.source_archive_relative_path = source.source_archive_relative_path;
        entry.provenance.archive_object_index = static_cast<std::uint32_t>(object_index);
        entry.provenance.internal_name = object.internal_name;
        entry.provenance.byte_size = object.data.size();
        entry.provenance.xxh64 = hash.value();
        entry.provenance.object_version = object_version.value();
        catalog.m_entries.push_back(std::move(entry));
        total_bytes = new_total;

        progress = {ProgressStage::indexed_object,
                    ordered_sources.size(),
                    archive_index,
                    catalog.m_entries.size(),
                    total_bytes,
                    source.source_archive_relative_path,
                    static_cast<std::uint32_t>(object_index)};
        if (auto error = emit_progress(options, progress)) {
          return Result<Catalog>::failure(std::move(*error));
        }
      }
    }

    const Progress complete{ProgressStage::complete,
                            ordered_sources.size(),
                            ordered_sources.size(),
                            catalog.m_entries.size(),
                            total_bytes,
                            {},
                            {}};
    if (auto error = emit_progress(options, complete)) {
      return Result<Catalog>::failure(std::move(*error));
    }
    return Result<Catalog>::success(std::move(catalog));
  } catch (const std::bad_alloc&) {
    return Result<Catalog>::failure(
        make_error(ErrorCode::allocation_failed, "Could not allocate the retail object catalog."));
  } catch (const std::length_error&) {
    return Result<Catalog>::failure(make_error(
        ErrorCode::allocation_failed, "A retail object catalog allocation exceeded its limit."));
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
    case ErrorCode::invalid_source_archive_path:
      return "invalid_source_archive_path";
    case ErrorCode::duplicate_source_archive_path:
      return "duplicate_source_archive_path";
    case ErrorCode::archive_limit_exceeded:
      return "archive_limit_exceeded";
    case ErrorCode::entry_limit_exceeded:
      return "entry_limit_exceeded";
    case ErrorCode::total_byte_limit_exceeded:
      return "total_byte_limit_exceeded";
    case ErrorCode::checked_dgo_failed:
      return "checked_dgo_failed";
    case ErrorCode::invalid_object_header:
      return "invalid_object_header";
    case ErrorCode::unsupported_object_version:
      return "unsupported_object_version";
    case ErrorCode::code_bearing_v3_object:
      return "code_bearing_v3_object";
    case ErrorCode::ambiguous_duplicate_internal_name:
      return "ambiguous_duplicate_internal_name";
    case ErrorCode::hash_failed:
      return "hash_failed";
    case ErrorCode::object_not_found:
      return "object_not_found";
    case ErrorCode::provenance_mismatch:
      return "provenance_mismatch";
  }
  return "unknown";
}

}  // namespace jak1_retail_object_catalog
