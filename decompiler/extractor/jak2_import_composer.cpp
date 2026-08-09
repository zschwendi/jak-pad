#include "jak2_import_composer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "common/custom_data/Jak2OutputMaterializer.h"
#include "common/custom_data/Jak2PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "common/custom_data/Jak2SourceObjectPack.h"
#include "common/util/PosixFile.h"

#include "decompiler/extractor/jak2_extracted_generated_inputs.h"
#include "decompiler/extractor/jak2_fr3_preparer.h"
#include "decompiler/extractor/jak2_import_composer_internal.h"
#include "decompiler/extractor/jak2_iso_validation.h"
#include "decompiler/extractor/jak1_retail_object_catalog.h"
#include "goalc/make/Jak2OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak2_import_composer {
namespace {

namespace artifacts = jak2_public_generated_artifacts;
namespace fs = std::filesystem;
namespace generator = jak2_output_recipe_generator;
namespace core_generator = jak1_output_recipe_generator;
namespace materializer = jak2_output_materializer;
namespace recipe = jak2_output_recipe;
namespace retail_catalog = jak1_retail_object_catalog;
namespace source_pack = jak2_source_object_pack;

static_assert(materializer::kNtscV2CompressedArchiveAlignmentBytes ==
              jak2_fr3::kNtscV2CompressedArchiveAlignmentBytes);
static_assert(materializer::kNtscV2ExpectedFr3Files == jak2_fr3::kNtscV2ExpectedFr3Files);

constexpr std::uint64_t kMaxManifestBytes = 4ull * 1024 * 1024;
constexpr std::size_t kIoChunkBytes = 256 * 1024;
constexpr std::uint64_t kMaxRetailArchiveBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t kMaxTotalRetailArchiveBytes = 512ull * 1024 * 1024;
constexpr std::size_t kMaxIndexedRetailEntries = 4096;
constexpr std::size_t kMaxWorkCleanupDepth = 64;
constexpr std::size_t kMaxWorkCleanupEntries = 100000;
constexpr std::string_view kGraphIsoRoot = "iso_data/jak2";
constexpr std::string_view kWorkDirectoryName = ".opengoal-import";
constexpr std::string_view kPreparedDirectoryName = ".prepared";
constexpr std::string_view kRecipeFileName = "jak2-output-recipe.bin";

struct ProjectResource {
  std::string_view relative_path;
  std::uint64_t size;
  std::uint64_t xxh64;
};

constexpr std::array<ProjectResource, 29> kProjectResources = {{
    {"decompiler/config/jak2/all-types.gc", 2139642, 0xf1c9cc80ffeaacbeull},
    {"decompiler/config/jak2/jak2_config.jsonc", 6990, 0xecc26e8aea74739bull},
    {"decompiler/config/jak2/ntsc_v1/anonymous_function_types.jsonc", 74737,
     0x095ac93fd8222a52ull},
    {"decompiler/config/jak2/ntsc_v1/art-group-info.min.json", 159832,
     0x7c6e03754b65d0dcull},
    {"decompiler/config/jak2/ntsc_v1/art_info.jsonc", 2143, 0x71f626bb231c5126ull},
    {"decompiler/config/jak2/ntsc_v1/hacks.jsonc", 23769, 0xf9c820bfa5de3b00ull},
    {"decompiler/config/jak2/ntsc_v1/import_deps.jsonc", 3, 0x7d441b099c11d3baull},
    {"decompiler/config/jak2/ntsc_v1/inputs.jsonc", 16798, 0x568e154420dbe74aull},
    {"decompiler/config/jak2/ntsc_v1/joint-node-info.min.json", 196240,
     0x9ba6ffbeb767fce7ull},
    {"decompiler/config/jak2/ntsc_v1/label_types.jsonc", 51803, 0xf75baa979c51054full},
    {"decompiler/config/jak2/ntsc_v1/part-groups.min.json", 45988,
     0xdf195fead140ef13ull},
    {"decompiler/config/jak2/ntsc_v1/process_stack_size_overrides.jsonc", 85,
     0xe7272e44a2c46dcdull},
    {"decompiler/config/jak2/ntsc_v1/stack_structures.jsonc", 66887,
     0xf9b6f9fda9ecd31full},
    {"decompiler/config/jak2/ntsc_v1/tex-info.min.json", 1192038,
     0x6d2836f7d424886aull},
    {"decompiler/config/jak2/ntsc_v1/type_casts.jsonc", 360255,
     0x20f5980bc47c3aaeull},
    {"decompiler/config/jak2/ntsc_v1/var_names.jsonc", 116280,
     0x4ecfcd34d8a160d0ull},
    {"game/assets/fonts/jak2_jak3_korean_db.json", 474061, 0xe505eaa129be8496ull},
    {"game/assets/jak2/game_subtitle.gp", 7390, 0x3ce05a2fde3f4212ull},
    {"game/assets/jak2/game_text.gp", 2780, 0xa2e69ffc238c08d1ull},
    {"game/assets/jak2/subtitle/subtitle_lines_en-US.json", 167003,
     0xe0b7de5b2a33de9aull},
    {"game/assets/jak2/subtitle/subtitle_meta_en-US.json", 826192,
     0xe2a3c587bd8643d7ull},
    {"game/assets/jak2/text/game_custom_text_de-DE.json", 8105,
     0x7a1da60b33528337ull},
    {"game/assets/jak2/text/game_custom_text_en-GB.json", 7585,
     0x281176307c82a0c4ull},
    {"game/assets/jak2/text/game_custom_text_en-US.json", 7564,
     0x6625036101477415ull},
    {"game/assets/jak2/text/game_custom_text_es-ES.json", 8209,
     0xee85aa0a46909cb4ull},
    {"game/assets/jak2/text/game_custom_text_fr-FR.json", 8604,
     0xcbc42e843c0b956dull},
    {"game/assets/jak2/text/game_custom_text_it-IT.json", 8140,
     0xfc2cdbf146f799fbull},
    {"game/assets/jak2/text/game_custom_text_ja-JP.json", 9358,
     0xd67c825c7fa7437dull},
    {"game/assets/jak2/text/game_custom_text_ko-KR.json", 7564,
     0x6625036101477415ull},
}};

constexpr std::array<std::string_view, 13> kRequiredIsoFiles = {
    "KERNEL.CGO", "GAME.CGO",   "TITLE.DGO", "CWI.DGO",      "CTA.DGO",
    "PRI.DGO",    "FEA.DGO",    "INTROCST.DGO", "LDJAKBRN.DGO", "DEMO1.SBK",
    "CTYWIDE1.SBK", "FOREXIT1.SBK", "FOREXIT2.SBK",
};

constexpr std::array<std::string_view, 8> kRequiredFr3Files = {
    "GAME.fr3",    "title.fr3",   "ctywide.fr3", "ctysluma.fr3",
    "prison.fr3",  "forexita.fr3", "introcst.fr3", "ldjakbrn.fr3",
};

Error make_error(ErrorCode code, std::string message) {
  return {code, std::move(message), std::nullopt, std::nullopt};
}

Error make_filesystem_error(ErrorCode fallback,
                            std::string message,
                            const std::error_code& error) {
  const auto code = error == std::errc::no_space_on_device ? ErrorCode::insufficient_storage
                                                            : fallback;
  if (error) {
    message += ": " + error.message();
  }
  return make_error(code, std::move(message));
}

bool direct_directory(const fs::path& path) {
  std::error_code error;
  return fs::symlink_status(path, error).type() == fs::file_type::directory && !error;
}

bool direct_regular_file(const fs::path& path) {
  std::error_code error;
  return fs::symlink_status(path, error).type() == fs::file_type::regular && !error;
}

bool missing_path(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return (!error || error == std::errc::no_such_file_or_directory) &&
         status.type() == fs::file_type::not_found;
}

bool safe_relative_path(const fs::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_path() || path.lexically_normal() != path) {
    return false;
  }
  return std::all_of(path.begin(), path.end(), [](const auto& part) {
    return !part.empty() && part != "." && part != "..";
  });
}

bool safe_basename(std::string_view name) {
  if (name.empty() || name.size() > 128 || name == "." || name == "..") {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
  });
}

class CallbackForwarder {
 public:
  explicit CallbackForwarder(const Options& options) : m_options(options) {}

  bool poll_cancel() {
    if (m_callback_failed) {
      return true;
    }
    if (!m_options.should_cancel) {
      return false;
    }
    try {
      return m_options.should_cancel();
    } catch (...) {
      m_callback_failed = true;
      return true;
    }
  }

  bool report(Progress progress) {
    if (m_callback_failed) {
      return false;
    }
    if (!m_options.on_progress) {
      return true;
    }
    try {
      m_options.on_progress(progress);
      return true;
    } catch (...) {
      m_callback_failed = true;
      return false;
    }
  }

  Error cancellation_or_callback_error(std::string cancellation_message) const {
    return m_callback_failed
               ? make_error(ErrorCode::callback_failed,
                            "The Jak II import callback failed while composing the candidate.")
               : make_error(ErrorCode::cancelled, std::move(cancellation_message));
  }

  bool callback_failed() const { return m_callback_failed; }

 private:
  const Options& m_options;
  bool m_callback_failed = false;
};

Error callback_aware_error(CallbackForwarder& callbacks,
                           ErrorCode code,
                           std::string message,
                           bool underlying_cancelled) {
  if (callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  return underlying_cancelled ? callbacks.cancellation_or_callback_error(std::move(message))
                              : make_error(code, std::move(message));
}

struct FileCloser {
  void operator()(FILE* file) const {
    if (file) {
      std::fclose(file);
    }
  }
};

using OwnedFile = std::unique_ptr<FILE, FileCloser>;

Result<OwnedFile> open_iso_file(const fs::path& path) {
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO could not be opened safely",
        std::error_code(errno, std::generic_category())));
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO is not a direct regular file", error));
  }
  auto* stream = ::fdopen(descriptor, "rb");
  if (!stream) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO stream could not be opened", error));
  }
  return Result<OwnedFile>::success(OwnedFile(stream));
}

Result<std::vector<std::uint8_t>> read_direct_file(const fs::path& path,
                                                   std::uint64_t expected_or_max_size,
                                                   bool exact_size,
                                                   ErrorCode error_code,
                                                   std::string_view description,
                                                   CallbackForwarder& callbacks) {
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return Result<std::vector<std::uint8_t>>::failure(make_filesystem_error(
        error_code, std::string(description) + " could not be opened safely",
        std::error_code(errno, std::generic_category())));
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size <= 0) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<std::vector<std::uint8_t>>::failure(make_filesystem_error(
        error_code, std::string(description) + " is not a nonempty direct regular file", error));
  }
  const auto file_size = static_cast<std::uint64_t>(status.st_size);
  if ((exact_size && file_size != expected_or_max_size) ||
      (!exact_size && file_size > expected_or_max_size) ||
      file_size > std::numeric_limits<std::size_t>::max()) {
    ::close(descriptor);
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " has an unexpected size."));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (callbacks.poll_cancel()) {
      ::close(descriptor);
      return Result<std::vector<std::uint8_t>>::failure(callbacks.cancellation_or_callback_error(
          "Jak II import was cancelled while reading " + std::string(description) + "."));
    }
    const auto count = std::min(kIoChunkBytes, bytes.size() - offset);
    const auto read_count = ::read(descriptor, bytes.data() + offset, count);
    if (read_count <= 0) {
      const std::error_code error(errno, std::generic_category());
      ::close(descriptor);
      return Result<std::vector<std::uint8_t>>::failure(make_filesystem_error(
          error_code, std::string(description) + " could not be read exactly", error));
    }
    offset += static_cast<std::size_t>(read_count);
  }
  std::uint8_t extra = 0;
  const auto extra_count = ::read(descriptor, &extra, 1);
  const auto close_result = ::close(descriptor);
  if (extra_count != 0 || close_result != 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " changed while it was read."));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

bool directory_contains(const fs::path& root,
                        const fs::path& candidate_parent,
                        std::error_code* inspection_error) {
  auto cursor = candidate_parent;
  while (true) {
    std::error_code error;
    if (fs::equivalent(root, cursor, error)) {
      return true;
    }
    if (error) {
      *inspection_error = error;
      return false;
    }
    const auto parent = cursor.parent_path();
    if (parent == cursor) {
      return false;
    }
    cursor = parent;
  }
}

Result<Request> validate_request(const Request& request) {
  const std::array<const fs::path*, 4> paths = {
      &request.iso_path,
      &request.source_object_pack_root,
      &request.project_resource_root,
      &request.candidate_root,
  };
  if (std::any_of(paths.begin(), paths.end(),
                  [](const auto* path) { return path->empty() || !path->is_absolute(); }) ||
      request.candidate_root.filename().empty() || !direct_regular_file(request.iso_path) ||
      !direct_directory(request.source_object_pack_root) ||
      !direct_directory(request.project_resource_root) ||
      !direct_directory(request.candidate_root.parent_path()) ||
      !missing_path(request.candidate_root)) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The ISO, source pack, project resources, or fresh candidate path is invalid."));
  }

  Request resolved = request;
  std::error_code error;
  resolved.iso_path = fs::canonical(request.iso_path, error);
  if (error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument, "The ISO path could not be canonicalized."));
  }
  resolved.source_object_pack_root = fs::canonical(request.source_object_pack_root, error);
  if (error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The source-object-pack root could not be canonicalized."));
  }
  resolved.project_resource_root = fs::canonical(request.project_resource_root, error);
  if (error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The project-resource root could not be canonicalized."));
  }
  const auto candidate_parent = fs::canonical(request.candidate_root.parent_path(), error);
  if (error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The import candidate parent could not be canonicalized."));
  }
  resolved.candidate_root = candidate_parent / request.candidate_root.filename();
  if (!missing_path(resolved.candidate_root)) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument, "The canonical import candidate already exists."));
  }

  std::error_code containment_error;
  const bool inside_source =
      directory_contains(resolved.source_object_pack_root, candidate_parent, &containment_error);
  if (containment_error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The candidate containment check could not be completed."));
  }
  const bool inside_resources =
      directory_contains(resolved.project_resource_root, candidate_parent, &containment_error);
  if (containment_error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The candidate containment check could not be completed."));
  }
  if (inside_source || inside_resources) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The import candidate cannot be created inside an input resource directory."));
  }
  return Result<Request>::success(std::move(resolved));
}

std::optional<Error> validate_project_resources(const fs::path& root,
                                                const Options& options) {
  std::set<std::string> expected_files;
  std::set<std::string> expected_directories;
  for (const auto& resource : kProjectResources) {
    expected_files.emplace(resource.relative_path);
    fs::path parent = fs::path(resource.relative_path).parent_path();
    while (!parent.empty()) {
      expected_directories.emplace(parent.generic_string());
      parent = parent.parent_path();
    }
  }

  std::set<std::string> actual_files;
  std::set<std::string> actual_directories;
  std::error_code error;
  fs::recursive_directory_iterator iterator(root, error);
  const fs::recursive_directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    const auto status = iterator->symlink_status(error);
    if (error) {
      break;
    }
    const auto relative = iterator->path().lexically_relative(root).generic_string();
    if (status.type() == fs::file_type::directory) {
      actual_directories.emplace(relative);
    } else if (status.type() == fs::file_type::regular) {
      actual_files.emplace(relative);
    } else {
      return make_error(ErrorCode::project_resources_failed,
                        "The project-resource bundle contains a link or unsupported entry.");
    }
  }
  if (error || actual_files != expected_files || actual_directories != expected_directories) {
    return make_error(ErrorCode::project_resources_failed,
                      "The project-resource bundle does not match its exact 29-file closure.");
  }

  CallbackForwarder callbacks(options);
  std::uint64_t bytes_hashed = 0;
  for (std::size_t index = 0; index < kProjectResources.size(); ++index) {
    const auto& resource = kProjectResources[index];
    auto bytes = read_direct_file(root / resource.relative_path, resource.size, true,
                                  ErrorCode::project_resources_failed,
                                  "A checked project resource", callbacks);
    if (!bytes) {
      return bytes.error();
    }
    if (XXH64(bytes.value().data(), bytes.value().size(), 0) != resource.xxh64) {
      return make_error(ErrorCode::project_resources_failed,
                        "A checked project resource has drifted: " +
                            std::string(resource.relative_path));
    }
    bytes_hashed += bytes.value().size();
    if (!callbacks.report({Phase::validating_project_resources, index + 1,
                           kProjectResources.size(), bytes_hashed,
                           std::string(resource.relative_path)})) {
      return callbacks.cancellation_or_callback_error({});
    }
  }
  return {};
}

bool supported_revision(const jak2_iso::Revision& revision) {
  const auto& expected = jak2_iso::import_revision();
  return revision.serial == expected.serial && revision.elf_hash == expected.elf_hash &&
         revision.contents_hash == expected.contents_hash &&
         revision.file_count == expected.file_count &&
         revision.decomp_config_version == expected.decomp_config_version &&
         revision.territory == expected.territory;
}

struct PipelineState {
  generator::Graph graph;
  source_pack::Summary source_pack_summary;
  OwnedFile iso_file;
  std::optional<jak2_iso::StagedExtraction> extraction;
  std::optional<artifacts::Build> generated_artifacts;
  std::vector<materializer::GeneratedObjectArtifact> generated_objects;
  std::vector<materializer::GeneratedFlatArtifact> generated_flat_files;
  std::vector<generator::RetailCatalogObject> retail_objects;
  std::vector<std::string> verified_flat_paths;
  std::vector<std::string> expected_fr3_basenames;
  std::vector<checked_file_identity::Identity> prepared_fr3_files;
  std::vector<std::uint8_t> output_recipe_wire;
  std::optional<recipe::Recipe> output_recipe;
  std::optional<Summary> summary;
  std::optional<internal::FinalContract> final_contract;
};

Error preserve_error_at(Error error, const internal::WorkPaths& paths, int work_directory);
std::optional<Error> write_file_atomically_impl(const fs::path& destination,
                                                std::span<const std::uint8_t> bytes,
                                                CallbackForwarder& callbacks);

std::optional<Error> extract_iso_stage(const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  CallbackForwarder callbacks(options);
  iso_file::Options iso_options;
  iso_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  iso_options.on_progress = [&](const iso_file::Progress& progress) {
    callbacks.report({Phase::extracting_iso, progress.files_completed, progress.files_total,
                      progress.bytes_completed, progress.current_path});
  };
  auto extracted = jak2_iso::extract_and_validate(
      state->iso_file.get(), paths.work_root / "extracted-iso", iso_options);
  if (!extracted) {
    const bool cancelled = extracted.error().code == jak2_iso::ValidationErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::iso_validation_failed,
        cancelled ? "Jak II ISO extraction was cancelled."
                  : "The selected Jak II ISO was rejected: " + extracted.error().message,
        cancelled);
  }
  if (callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  auto extraction = extracted.take_value();
  if (!supported_revision(extraction.match.revision)) {
    return make_error(ErrorCode::unsupported_revision,
                      "On-device preparation supports only SCUS-97265 NTSC-U v2.");
  }
  state->extraction.emplace(std::move(extraction));
  return {};
}

std::optional<Error> generate_data_stage(const Request& request,
                                         const Options& options,
                                         PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::generated_data_failed,
                      "The generated-data stage has no validated extraction.");
  }
  CallbackForwarder callbacks(options);
  jak2_extracted_generated_inputs::Options input_options;
  input_options.limits.max_validated_files = state->extraction->match.revision.file_count;
  input_options.require_validated_file_identities = true;
  input_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  input_options.on_progress = [&](const jak2_extracted_generated_inputs::Progress& progress) {
    callbacks.report({Phase::generating_data, progress.units_completed, progress.units_total, 0,
                      progress.source_relative_path});
  };
  auto inputs = jak2_extracted_generated_inputs::build(
      {state->extraction->staging_directory, state->extraction->match.revision,
       state->extraction->files},
      state->graph, input_options);
  if (!inputs) {
    const bool cancelled =
        inputs.error().code == jak2_extracted_generated_inputs::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::generated_data_failed,
        cancelled ? "Jak II generated-input loading was cancelled."
                  : "Could not load checked Jak II generated inputs: " + inputs.error().message,
        cancelled);
  }

  artifacts::Options artifact_options;
  artifact_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  artifact_options.on_progress = [&](const artifacts::Progress& progress) {
    callbacks.report({Phase::generating_data, progress.units_completed, progress.units_total, 0,
                      progress.relative_path});
  };
  auto built = artifacts::build(inputs.value(), state->graph, request.project_resource_root,
                                artifact_options);
  if (!built) {
    const bool cancelled = built.error().code == artifacts::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::generated_data_failed,
        cancelled ? "Jak II generated-artifact building was cancelled."
                  : "Could not build checked Jak II generated data: " + built.error().message,
        cancelled);
  }
  if (callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  if (built.value().artifacts.size() != 10) {
    return make_error(ErrorCode::generated_data_failed,
                      "The checked Jak II generator did not produce exactly ten artifacts.");
  }
  state->generated_artifacts.emplace(built.take_value());
  return {};
}

std::optional<Error> prepare_fr3_stage(const Request& request,
                                       const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  if (!state->extraction || !supported_revision(state->extraction->match.revision)) {
    return make_error(ErrorCode::fr3_failed,
                      "The FR3 stage has no supported validated extraction.");
  }
  CallbackForwarder callbacks(options);
  jak2_fr3::Options fr3_options;
  fr3_options.validated_extracted_files = state->extraction->files;
  fr3_options.require_validated_file_identities = true;
  fr3_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  fr3_options.report_progress = [&](const jak2_fr3::Progress& progress) {
    callbacks.report({Phase::preparing_fr3, progress.completed, progress.total, 0,
                      progress.current_item});
  };
  const auto fr3_work = paths.work_root / "fr3-work";
  auto prepared = jak2_fr3::prepare(request.project_resource_root,
                                    state->extraction->staging_directory, fr3_work, fr3_options);
  if (!prepared) {
    const bool cancelled = prepared.error().code == jak2_fr3::ErrorCode::cancelled;
    const bool insufficient = prepared.error().code == jak2_fr3::ErrorCode::output_limit_exceeded &&
                              prepared.error().message.find("less free space") != std::string::npos;
    return callback_aware_error(
        callbacks, insufficient ? ErrorCode::insufficient_storage : ErrorCode::fr3_failed,
        cancelled ? "Jak II FR3 preparation was cancelled."
                  : "Jak II FR3 preparation failed: " + prepared.error().message,
        cancelled);
  }
  if (callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }

  const auto fr3_root = fr3_work / "fr3";
  std::set<std::string> basenames;
  std::error_code error;
  for (fs::directory_iterator iterator(fr3_root, error), end; !error && iterator != end;
       iterator.increment(error)) {
    const auto status = iterator->symlink_status(error);
    const auto basename = iterator->path().filename().string();
    if (error || status.type() != fs::file_type::regular || !safe_basename(basename) ||
        !basename.ends_with(".fr3") || !basenames.emplace(basename).second) {
      return make_error(ErrorCode::fr3_failed,
                        "FR3 preparation returned an unsafe or duplicate output file.");
    }
  }
  if (error || basenames.empty() || basenames.size() != prepared.value().levels_written) {
    return make_error(ErrorCode::fr3_failed,
                      "FR3 preparation returned an incomplete output set.");
  }
  if (prepared.value().fr3_files.size() != basenames.size()) {
    return make_error(ErrorCode::fr3_failed,
                      "FR3 preparation returned an incomplete checked identity set.");
  }
  state->expected_fr3_basenames.assign(basenames.begin(), basenames.end());
  state->prepared_fr3_files = prepared.value().fr3_files;
  return {};
}

std::optional<Error> adapt_flat_paths(PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::recipe_failed,
                      "The output-recipe stage has no validated extraction.");
  }
  std::set<std::string> verified;
  const fs::path graph_root(kGraphIsoRoot);
  for (const auto& copy : state->graph.flat_file_copies) {
    const auto source = fs::path(copy.source_path).lexically_normal();
    const auto relative = source.lexically_relative(graph_root);
    if (!safe_relative_path(relative) ||
        !direct_regular_file(state->extraction->staging_directory / relative)) {
      return make_error(ErrorCode::graph_failed,
                        "A checked flat-file source is unsafe, missing, or linked.");
    }
    verified.emplace(relative.generic_string());
  }
  state->verified_flat_paths.assign(verified.begin(), verified.end());
  return {};
}

std::optional<Error> catalog_retail_stage(const Options& options, PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::retail_catalog_failed,
                      "The retail-catalog stage has no validated extraction.");
  }
  if (!direct_directory(state->extraction->staging_directory)) {
    return make_error(ErrorCode::retail_catalog_failed,
                      "The validated extraction root is no longer a direct directory.");
  }
  auto requirements = internal::derive_retail_requirements(state->graph, options);
  if (!requirements) {
    return requirements.error();
  }
  if (requirements.value().occurrence_count != internal::kNtscV2RetailOccurrenceCount ||
      requirements.value().objects.size() != internal::kNtscV2RetailObjectCount ||
      requirements.value().source_archive_relative_paths.size() !=
          internal::kNtscV2RetailArchiveCount) {
    return make_error(ErrorCode::graph_failed,
                      "The checked Jak II graph has an unexpected retail-object closure.");
  }

  CallbackForwarder callbacks(options);
  std::vector<retail_catalog::Entry> entries;
  entries.reserve(kMaxIndexedRetailEntries);
  std::uint64_t total_archive_bytes = 0;
  std::uint64_t total_expanded_archive_bytes = 0;
  std::uint64_t total_all_object_bytes = 0;
  const auto& archive_paths = requirements.value().source_archive_relative_paths;
  for (std::size_t index = 0; index < archive_paths.size(); ++index) {
    if (callbacks.poll_cancel()) {
      return callbacks.cancellation_or_callback_error(
          "Jak II retail cataloging was cancelled.");
    }
    const auto& relative_path = archive_paths[index];
    const auto archive_path = state->extraction->staging_directory / relative_path;
    if (!direct_directory(archive_path.parent_path()) || !direct_regular_file(archive_path)) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "A graph-required retail archive is missing, linked, or unsafe.");
    }
    if (total_archive_bytes >= kMaxTotalRetailArchiveBytes) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The graph-required retail archives exhaust their aggregate input cap.");
    }
    const auto remaining_archive_bytes = kMaxTotalRetailArchiveBytes - total_archive_bytes;
    auto bytes = read_direct_file(archive_path,
                                  std::min(kMaxRetailArchiveBytes, remaining_archive_bytes), false,
                                  ErrorCode::retail_catalog_failed,
                                  "A graph-required retail archive", callbacks);
    if (!bytes) {
      return bytes.error();
    }
    const auto identity = std::find_if(
        state->extraction->files.begin(), state->extraction->files.end(),
        [&](const auto& candidate) { return candidate.relative_path == relative_path; });
    if (identity == state->extraction->files.end() || bytes.value().size() != identity->size ||
        XXH64(bytes.value().data(), bytes.value().size(), 0) != identity->xxh64) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "A graph-required retail archive does not match its validated identity.");
    }
    if (bytes.value().size() > kMaxTotalRetailArchiveBytes - total_archive_bytes) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The graph-required retail archives exceed their aggregate input cap.");
    }
    total_archive_bytes += bytes.value().size();

    if (total_expanded_archive_bytes >= jak2_fr3::kNtscV2TotalExpandedArchiveBytes ||
        total_all_object_bytes >= jak2_fr3::kNtscV2TotalExpandedArchiveBytes) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The graph-required retail archives exhaust their expanded-data cap.");
    }
    const auto remaining_expanded_bytes =
        jak2_fr3::kNtscV2TotalExpandedArchiveBytes - total_expanded_archive_bytes;
    const auto remaining_all_object_bytes =
        jak2_fr3::kNtscV2TotalExpandedArchiveBytes - total_all_object_bytes;

    const retail_catalog::ArchiveSource source{relative_path, bytes.value()};
    retail_catalog::Options catalog_options;
    catalog_options.max_archives = 1;
    catalog_options.max_entries = kMaxIndexedRetailEntries;
    catalog_options.max_total_object_bytes = remaining_all_object_bytes;
    catalog_options.max_archive_input_bytes =
        std::min<std::uint64_t>(kMaxRetailArchiveBytes, remaining_archive_bytes);
    catalog_options.max_total_archive_input_bytes = remaining_archive_bytes;
    catalog_options.max_archive_compressed_bytes = kMaxRetailArchiveBytes;
    catalog_options.max_archive_expanded_bytes = remaining_expanded_bytes;
    catalog_options.max_total_expanded_archive_bytes = remaining_expanded_bytes;
    catalog_options.compressed_trailing_alignment_bytes =
        jak2_fr3::kNtscV2CompressedArchiveAlignmentBytes;
    catalog_options.game_version = GameVersion::Jak2;
    catalog_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    auto catalog = retail_catalog::build(
        std::span<const retail_catalog::ArchiveSource>(&source, 1), catalog_options);
    if (!catalog) {
      const bool cancelled = catalog.error().code == retail_catalog::ErrorCode::cancelled;
      return callback_aware_error(
          callbacks, ErrorCode::retail_catalog_failed,
          cancelled ? "Jak II retail cataloging was cancelled."
                    : "The checked Jak II retail catalog failed: " + catalog.error().message,
          cancelled);
    }
    if (catalog.value().entries().size() > kMaxIndexedRetailEntries - entries.size()) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The checked Jak II retail catalog exceeds its entry cap.");
    }
    if (catalog.value().expanded_archive_bytes() > remaining_expanded_bytes ||
        catalog.value().all_object_payload_bytes() > remaining_all_object_bytes) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The checked Jak II retail catalog exceeded its remaining byte budget.");
    }
    total_expanded_archive_bytes += catalog.value().expanded_archive_bytes();
    total_all_object_bytes += catalog.value().all_object_payload_bytes();
    for (const auto& entry : catalog.value().entries()) {
      entries.push_back(entry);
    }
    if (!callbacks.report({Phase::cataloging_retail, index + 1, archive_paths.size(),
                           total_archive_bytes, relative_path})) {
      return callbacks.cancellation_or_callback_error({});
    }
  }

  auto selected = internal::select_exact_retail_catalog(
      requirements.value(), entries, jak2_fr3::kNtscV2TotalExpandedArchiveBytes, options);
  if (!selected) {
    return selected.error();
  }
  if (selected.value().size() != internal::kNtscV2RetailObjectCount) {
    return make_error(ErrorCode::retail_catalog_failed,
                      "The checked Jak II retail catalog is incomplete.");
  }
  state->retail_objects = selected.take_value();
  return callbacks.callback_failed()
             ? std::optional<Error>(callbacks.cancellation_or_callback_error({}))
             : std::nullopt;
}

class OwnedPartialFile {
 public:
  OwnedPartialFile(int parent,
                   posix_file::OwnedFd descriptor,
                   std::string name,
                   posix_file::Identity identity)
      : m_parent(parent),
        m_descriptor(std::move(descriptor)),
        m_name(std::move(name)),
        m_identity(identity) {}
  ~OwnedPartialFile() {
    (void)cleanup();
  }
  int descriptor() const { return m_descriptor.get(); }
  const std::string& name() const { return m_name; }
  const posix_file::Identity& identity() const { return m_identity; }
  void release() { m_linked = false; }
  std::optional<std::string> close_checked() {
    const auto descriptor = m_descriptor.release();
    if (descriptor >= 0 && ::close(descriptor) != 0) {
      return "Could not close an import partial file: " +
             std::error_code(errno, std::generic_category()).message();
    }
    return {};
  }
  std::optional<Error> cleanup() {
    if (!m_linked) {
      return {};
    }
    if (!posix_file::entry_identity(m_parent, m_name, m_identity)) {
      return make_error(ErrorCode::work_write_failed,
                        "An import partial file changed before cleanup and was preserved.");
    }
    if (::unlinkat(m_parent, m_name.c_str(), 0) != 0) {
      return make_filesystem_error(ErrorCode::work_write_failed,
                                   "Could not remove an exact import partial file",
                                   std::error_code(errno, std::generic_category()));
    }
    m_linked = false;
    return {};
  }

 private:
  int m_parent = -1;
  posix_file::OwnedFd m_descriptor;
  std::string m_name;
  posix_file::Identity m_identity;
  bool m_linked = true;
};

std::optional<Error> write_file_atomically_impl(const fs::path& destination,
                                                std::span<const std::uint8_t> bytes,
                                                CallbackForwarder& callbacks) {
  if (bytes.empty() || !destination.is_absolute() ||
      !safe_basename(destination.filename().string())) {
    return make_error(ErrorCode::work_write_failed,
                      "An import work-file destination is invalid.");
  }
  auto parent = posix_file::open_directory(destination.parent_path().c_str());
  posix_file::Identity parent_identity;
  if (!parent || !posix_file::descriptor_identity(parent.get(), &parent_identity)) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not retain an import work-file parent",
                                 std::error_code(errno, std::generic_category()));
  }
  struct stat destination_status {};
  const auto destination_name = destination.filename().string();
  if (::fstatat(parent.get(), destination_name.c_str(), &destination_status,
                AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) {
    return make_error(ErrorCode::work_write_failed,
                      "An import work-file destination already exists or is unreadable.");
  }
  static std::atomic<std::uint64_t> next_id{0};
  posix_file::OwnedFd descriptor;
  std::string partial_name;
  for (std::size_t attempt = 0; attempt < 64; ++attempt) {
    partial_name = ".opengoal-work-" +
                   std::to_string(static_cast<unsigned long long>(::getpid())) + "-" +
                   std::to_string(static_cast<unsigned long long>(
                       next_id.fetch_add(1, std::memory_order_relaxed))) +
                   ".partial";
    descriptor = posix_file::open_file_at(
        parent.get(), partial_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (descriptor || errno != EEXIST) {
      break;
    }
  }
  if (!descriptor) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not create an exclusive import partial file",
                                 std::error_code(errno, std::generic_category()));
  }
  posix_file::Identity partial_identity;
  struct stat partial_status {};
  if (!posix_file::descriptor_identity(descriptor.get(), &partial_identity, &partial_status) ||
      !S_ISREG(partial_status.st_mode) || partial_status.st_nlink != 1) {
    return make_error(ErrorCode::work_write_failed,
                      "The exclusive import partial is not a private regular file.");
  }
  OwnedPartialFile file(parent.get(), std::move(descriptor), partial_name, partial_identity);
  const auto fail_owned = [&](Error error) {
    if (const auto cleanup = file.cleanup()) {
      return *cleanup;
    }
    return error;
  };
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (callbacks.poll_cancel()) {
      return fail_owned(callbacks.cancellation_or_callback_error(
          "Jak II import was cancelled while writing generated data."));
    }
    const auto count = std::min(kIoChunkBytes, bytes.size() - offset);
    const auto written = ::write(file.descriptor(), bytes.data() + offset, count);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return fail_owned(make_filesystem_error(
          ErrorCode::work_write_failed, "Could not write a complete import work file",
          std::error_code(errno, std::generic_category())));
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(file.descriptor()) != 0) {
    return fail_owned(make_filesystem_error(
        ErrorCode::work_write_failed, "Could not synchronize an import work file",
        std::error_code(errno, std::generic_category())));
  }
  struct stat final_status {};
  std::vector<std::uint8_t> buffer(std::min(kIoChunkBytes, bytes.size()));
  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);
  std::size_t verified = 0;
  while (verified < bytes.size()) {
    const auto count = std::min(buffer.size(), bytes.size() - verified);
    const auto read = ::pread(file.descriptor(), buffer.data(), count,
                              static_cast<off_t>(verified));
    if (read < 0 && errno == EINTR) {
      continue;
    }
    if (read != static_cast<ssize_t>(count)) {
      return fail_owned(make_error(ErrorCode::work_write_failed,
                                   "Could not re-read an exact import work file."));
    }
    XXH64_update(&hash_state, buffer.data(), count);
    verified += count;
  }
  if (!posix_file::descriptor_identity(file.descriptor(), nullptr, &final_status) ||
      !S_ISREG(final_status.st_mode) || final_status.st_nlink != 1 ||
      final_status.st_size != static_cast<off_t>(bytes.size()) ||
      !posix_file::entry_identity(parent.get(), file.name(), file.identity()) ||
      XXH64_digest(&hash_state) != XXH64(bytes.data(), bytes.size(), 0)) {
    return fail_owned(make_error(
        ErrorCode::work_write_failed,
        "The exact import work file changed before descriptor-relative installation."));
  }
  if (const auto close_error = file.close_checked()) {
    return fail_owned(make_error(ErrorCode::work_write_failed, *close_error));
  }
  struct stat parent_status {};
  if (::lstat(destination.parent_path().c_str(), &parent_status) != 0 ||
      !posix_file::same_identity(parent_status, parent_identity) ||
      posix_file::exclusive_rename_at(parent.get(), file.name(), parent.get(),
                                      destination_name) != 0) {
    return fail_owned(make_filesystem_error(
        ErrorCode::work_write_failed, "Could not exclusively install an import work file",
        std::error_code(errno, std::generic_category())));
  }
  file.release();
  if (!posix_file::entry_identity(parent.get(), destination_name, partial_identity)) {
    return make_error(ErrorCode::work_write_failed,
                      "The installed import work-file identity changed.");
  }
  return {};
}

std::optional<Error> persist_generated_artifacts(const artifacts::Build& build,
                                                 const fs::path& root,
                                                 PipelineState* state,
                                                 CallbackForwarder& callbacks) {
  std::error_code error;
  if (!fs::create_directory(root, error) || error) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not create the generated-artifact root", error);
  }
  for (const auto& artifact : build.artifacts) {
    const fs::path relative(artifact.output_relative_path);
    if (!safe_relative_path(relative) || relative.parent_path().empty()) {
      return make_error(ErrorCode::generated_data_failed,
                        "A generated artifact returned an unsafe relative path.");
    }
    const auto directory = root / relative.parent_path();
    if (!fs::exists(directory, error)) {
      if (!fs::create_directories(directory, error) || error) {
        return make_filesystem_error(ErrorCode::work_write_failed,
                                     "Could not create a generated-artifact directory", error);
      }
    } else if (error || !direct_directory(directory)) {
      return make_error(ErrorCode::work_write_failed,
                        "A generated-artifact directory is unsafe.");
    }
    if (const auto write_error =
            write_file_atomically_impl(root / relative, artifact.bytes, callbacks)) {
      return write_error;
    }
    if (artifact.kind == artifacts::ArtifactKind::directory_tpages) {
      state->generated_objects.push_back({recipe::GeneratedDataKind::directory_tpages,
                                          "dir-tpages", artifact.output_relative_path,
                                          artifact.bytes.size(), artifact.xxh64});
    } else {
      const auto kind = artifact.kind == artifacts::ArtifactKind::game_text
                            ? recipe::GeneratedFlatFileKind::game_text
                            : recipe::GeneratedFlatFileKind::game_subtitle;
      state->generated_flat_files.push_back({kind, artifact.destination_basename,
                                             artifact.output_relative_path,
                                             artifact.bytes.size(), artifact.xxh64});
    }
  }
  if (state->generated_objects.size() != 1 || state->generated_flat_files.size() != 9) {
    return make_error(ErrorCode::generated_data_failed,
                      "The generated-artifact catalog is incomplete.");
  }
  return {};
}

Result<internal::FinalContract> make_final_contract(const recipe::Recipe& output) {
  std::set<std::string> iso;
  std::set<std::string> fr3;
  const auto add_iso = [&](const std::string& name) {
    return safe_basename(name) && iso.emplace(name).second;
  };
  for (const auto& archive : output.archives) {
    if (!add_iso(archive.destination_basename)) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe contains an unsafe ISO destination."));
    }
  }
  for (const auto& copy : output.flat_file_copies) {
    if (!add_iso(copy.destination_basename)) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe contains a duplicate ISO destination."));
    }
  }
  for (const auto& generated : output.generated_flat_files) {
    if (!add_iso(generated.destination_basename)) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe contains a duplicate generated output."));
    }
  }
  for (const auto& name : output.expected_fr3_basenames) {
    if (!safe_basename(name) || !name.ends_with(".fr3") || !fr3.emplace(name).second) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe contains an unsafe FR3 destination."));
    }
  }
  if (iso.size() != output.archives.size() + output.flat_file_copies.size() +
                        output.generated_flat_files.size() ||
      fr3.size() != output.expected_fr3_basenames.size()) {
    return Result<internal::FinalContract>::failure(
        make_error(ErrorCode::recipe_failed, "The output recipe destination set is incomplete."));
  }
  for (const auto name : kRequiredIsoFiles) {
    if (!iso.contains(std::string(name))) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe omits a required Jak II launch file."));
    }
  }
  for (const auto name : kRequiredFr3Files) {
    if (!fr3.contains(std::string(name))) {
      return Result<internal::FinalContract>::failure(make_error(
          ErrorCode::recipe_failed, "The output recipe omits a required Jak II launch FR3."));
    }
  }
  return Result<internal::FinalContract>::success(
      {{iso.begin(), iso.end()}, {fr3.begin(), fr3.end()}});
}

std::optional<Error> generate_recipe_stage(const Request& request,
                                           const internal::WorkPaths& paths,
                                           const Options& options,
                                           PipelineState* state) {
  if (!state->extraction || !state->generated_artifacts || state->retail_objects.empty() ||
      state->expected_fr3_basenames.empty()) {
    return make_error(ErrorCode::recipe_failed,
                      "The output-recipe stage is missing a checked input.");
  }
  CallbackForwarder callbacks(options);
  auto manifest = read_direct_file(request.source_object_pack_root / source_pack::kManifestName,
                                   kMaxManifestBytes, false, ErrorCode::source_pack_failed,
                                   "The source-object-pack manifest", callbacks);
  if (!manifest) {
    return manifest.error();
  }
  if (const auto flat_error = adapt_flat_paths(state)) {
    return flat_error;
  }

  generator::VerifiedInputs inputs;
  inputs.extracted_iso_root = kGraphIsoRoot;
  inputs.verified_extracted_iso_relative_paths = state->verified_flat_paths;
  inputs.retail_catalog = state->retail_objects;
  inputs.expected_fr3_basenames = state->expected_fr3_basenames;
  generator::Options generator_options;
  generator_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  const std::string_view manifest_view(reinterpret_cast<const char*>(manifest.value().data()),
                                       manifest.value().size());
  auto output = generator::generate_from_graph(state->graph, manifest_view,
                                                state->extraction->match.revision, inputs,
                                                generator_options);
  if (!output) {
    const bool cancelled = output.error().code == generator::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::recipe_failed,
        cancelled ? "Jak II output-recipe generation was cancelled."
                  : "Could not generate the checked Jak II recipe: " + output.error().message,
        cancelled);
  }
  if (output.value().source_object_pack != state->source_pack_summary.identity) {
    return make_error(ErrorCode::source_pack_failed,
                      "The source-object-pack manifest changed after validation.");
  }
  recipe::Options recipe_options;
  recipe_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  auto wire = recipe::encode(output.value(), state->extraction->match.revision, recipe_options);
  if (!wire) {
    const bool cancelled = wire.error().code == recipe::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::recipe_failed,
        cancelled ? "Jak II output-recipe encoding was cancelled."
                  : "Could not encode the checked Jak II recipe: " + wire.error().message,
        cancelled);
  }
  if (const auto write_error =
          write_file_atomically_impl(paths.work_root / kRecipeFileName, wire.value(), callbacks)) {
    return write_error;
  }
  if (const auto artifact_error = persist_generated_artifacts(
          *state->generated_artifacts, paths.work_root / "generated", state, callbacks)) {
    return artifact_error;
  }
  auto contract = make_final_contract(output.value());
  if (!contract) {
    return contract.error();
  }
  state->generated_artifacts.reset();
  state->final_contract.emplace(contract.take_value());
  state->output_recipe_wire = wire.take_value();
  state->output_recipe.emplace(output.take_value());
  return {};
}

std::optional<Error> materialize_stage(const Request& request,
                                       const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  if (!state->extraction || !state->output_recipe || state->output_recipe_wire.empty() ||
      !state->final_contract || state->prepared_fr3_files.empty()) {
    return make_error(ErrorCode::materialization_failed,
                      "The materializer stage is missing checked inputs.");
  }
  materializer::Inputs inputs;
  inputs.recipe_file = paths.work_root / kRecipeFileName;
  inputs.source_object_pack_root = request.source_object_pack_root;
  inputs.extracted_iso_root = state->extraction->staging_directory;
  inputs.generated_artifact_root = paths.work_root / "generated";
  inputs.prepared_fr3_root = paths.work_root / "fr3-work/fr3";
  inputs.generated_objects = state->generated_objects;
  inputs.generated_flat_files = state->generated_flat_files;
  inputs.validated_extracted_files = state->extraction->files;
  inputs.validated_fr3_files = state->prepared_fr3_files;

  CallbackForwarder callbacks(options);
  materializer::Options materializer_options;
  materializer_options.expected_recipe_bytes = state->output_recipe_wire;
  materializer_options.require_validated_file_identities = true;
  materializer_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  materializer_options.on_progress = [&](const materializer::Progress& progress) {
    callbacks.report({Phase::materializing_output, progress.completed, progress.total,
                      progress.bytes_written, progress.current_item});
  };
  auto materialized = materializer::materialize(
      inputs, paths.prepared_root, state->extraction->match.revision, materializer_options);
  if (!materialized) {
    const bool cancelled = materialized.error().code == materializer::ErrorCode::cancelled;
    const bool insufficient =
        materialized.error().code == materializer::ErrorCode::output_limit_exceeded;
    return callback_aware_error(
        callbacks, insufficient ? ErrorCode::insufficient_storage
                                : ErrorCode::materialization_failed,
        cancelled ? "Jak II output materialization was cancelled."
                  : "Could not materialize the checked Jak II output: " +
                        materialized.error().message,
        cancelled);
  }
  if (callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  const auto& summary = materialized.value();
  if (summary.archives_written + summary.flat_files_written !=
          state->final_contract->iso_basenames.size() ||
      summary.fr3_files_written != state->final_contract->fr3_basenames.size()) {
    return make_error(ErrorCode::materialization_failed,
                      "The materializer summary does not match the exact output contract.");
  }
  state->summary = Summary{summary.archives_written, summary.objects_written,
                           summary.flat_files_written, summary.fr3_files_written,
                           summary.output_bytes};
  return {};
}

struct CapturedPreparedOutput {
  posix_file::OwnedFd root;
  posix_file::OwnedFd iso;
  posix_file::OwnedFd fr3;
  posix_file::Identity root_identity;
  posix_file::Identity iso_identity;
  posix_file::Identity fr3_identity;
  std::map<std::string, checked_file_identity::Identity> iso_files;
  std::map<std::string, checked_file_identity::Identity> fr3_files;
};

Result<checked_file_identity::Identity> capture_file_at(int directory,
                                                       const std::string& name) {
  auto file = posix_file::open_file_at(directory, name, O_RDONLY);
  posix_file::Identity identity;
  struct stat before {};
  if (!file || !posix_file::descriptor_identity(file.get(), &identity, &before) ||
      !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size <= 0 ||
      static_cast<std::uint64_t>(before.st_size) > 8ull * 1024 * 1024 * 1024) {
    return Result<checked_file_identity::Identity>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "A prepared output is not a bounded private regular file."));
  }
  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);
  std::vector<std::uint8_t> buffer(64 * 1024);
  std::uint64_t offset = 0;
  const auto size = static_cast<std::uint64_t>(before.st_size);
  while (offset < size) {
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), size - offset));
    const auto count = ::pread(file.get(), buffer.data(), chunk, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count != static_cast<ssize_t>(chunk)) {
      return Result<checked_file_identity::Identity>::failure(make_error(
          ErrorCode::candidate_finalize_failed,
          "Could not completely read a prepared output through its descriptor."));
    }
    XXH64_update(&hash_state, buffer.data(), chunk);
    offset += chunk;
  }
  struct stat after {};
  if (!posix_file::descriptor_identity(file.get(), nullptr, &after) ||
      !posix_file::same_identity(after, identity) || before.st_size != after.st_size ||
      !posix_file::entry_identity(directory, name, identity)) {
    return Result<checked_file_identity::Identity>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "A prepared output changed while its identity was captured."));
  }
  return Result<checked_file_identity::Identity>::success(
      {name, size, XXH64_digest(&hash_state)});
}

Result<std::map<std::string, checked_file_identity::Identity>> capture_directory_at(
    int directory,
    const std::set<std::string>& expected) {
  auto enumeration = posix_file::open_directory_at(directory, ".");
  if (!enumeration) {
    return Result<std::map<std::string, checked_file_identity::Identity>>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "Could not reopen a prepared output directory."));
  }
  DIR* stream = ::fdopendir(enumeration.release());
  if (!stream) {
    return Result<std::map<std::string, checked_file_identity::Identity>>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "Could not enumerate a prepared output directory."));
  }
  std::map<std::string, checked_file_identity::Identity> result;
  int read_error = 0;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream);
    if (!entry) {
      read_error = errno;
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!safe_basename(name) || !expected.contains(name) || result.contains(name)) {
      ::closedir(stream);
      return Result<std::map<std::string, checked_file_identity::Identity>>::failure(make_error(
          ErrorCode::candidate_finalize_failed,
          "A prepared output directory differs from its exact contract."));
    }
    auto identity = capture_file_at(directory, name);
    if (!identity) {
      ::closedir(stream);
      return Result<std::map<std::string, checked_file_identity::Identity>>::failure(
          identity.error());
    }
    result.emplace(name, identity.take_value());
  }
  ::closedir(stream);
  if (read_error != 0 || result.size() != expected.size()) {
    return Result<std::map<std::string, checked_file_identity::Identity>>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "A prepared output directory is incomplete or changed during enumeration."));
  }
  return Result<std::map<std::string, checked_file_identity::Identity>>::success(
      std::move(result));
}

Result<CapturedPreparedOutput> capture_prepared_output(
    int work_directory,
    const internal::FinalContract& contract) {
  CapturedPreparedOutput output;
  output.root = posix_file::open_directory_at(work_directory, kPreparedDirectoryName);
  if (!output.root ||
      !posix_file::descriptor_identity(output.root.get(), &output.root_identity)) {
    return Result<CapturedPreparedOutput>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "Could not retain the exact prepared-output root."));
  }
  output.iso = posix_file::open_directory_at(output.root.get(), "iso");
  output.fr3 = posix_file::open_directory_at(output.root.get(), "fr3");
  if (!output.iso || !output.fr3 ||
      !posix_file::descriptor_identity(output.iso.get(), &output.iso_identity) ||
      !posix_file::descriptor_identity(output.fr3.get(), &output.fr3_identity)) {
    return Result<CapturedPreparedOutput>::failure(make_error(
        ErrorCode::candidate_finalize_failed,
        "Could not retain the exact prepared output directories."));
  }
  const std::set<std::string> expected_iso(contract.iso_basenames.begin(),
                                           contract.iso_basenames.end());
  const std::set<std::string> expected_fr3(contract.fr3_basenames.begin(),
                                           contract.fr3_basenames.end());
  auto iso = capture_directory_at(output.iso.get(), expected_iso);
  auto fr3 = capture_directory_at(output.fr3.get(), expected_fr3);
  if (!iso || !fr3) {
    return Result<CapturedPreparedOutput>::failure(!iso ? iso.error() : fr3.error());
  }
  output.iso_files = iso.take_value();
  output.fr3_files = fr3.take_value();
  return Result<CapturedPreparedOutput>::success(std::move(output));
}

bool exact_child_directories(
    int directory,
    const std::map<std::string, posix_file::Identity>& expected) {
  auto enumeration = posix_file::open_directory_at(directory, ".");
  if (!enumeration) {
    return false;
  }
  DIR* stream = ::fdopendir(enumeration.release());
  if (!stream) {
    return false;
  }
  std::set<std::string> found;
  int read_error = 0;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream);
    if (!entry) {
      read_error = errno;
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    const auto expected_entry = expected.find(name);
    struct stat status {};
    if (expected_entry == expected.end() ||
        !posix_file::entry_identity(directory, name, expected_entry->second, &status) ||
        !S_ISDIR(status.st_mode) || !found.emplace(name).second) {
      ::closedir(stream);
      return false;
    }
  }
  ::closedir(stream);
  return read_error == 0 && found.size() == expected.size();
}

struct WorkCleanupBudget {
  std::size_t entries = 0;
};

struct WorkEntry {
  posix_file::Identity identity;
  bool directory = false;
};

using WorkManifest = std::map<std::string, WorkEntry>;

bool same_work_manifest(const WorkManifest& left, const WorkManifest& right) {
  if (left.size() != right.size()) {
    return false;
  }
  auto left_entry = left.begin();
  auto right_entry = right.begin();
  while (left_entry != left.end()) {
    if (left_entry->first != right_entry->first ||
        left_entry->second.directory != right_entry->second.directory ||
        left_entry->second.identity.device != right_entry->second.identity.device ||
        left_entry->second.identity.inode != right_entry->second.identity.inode) {
      return false;
    }
    ++left_entry;
    ++right_entry;
  }
  return true;
}

std::optional<Error> capture_work_manifest_at(int directory,
                                              std::string_view prefix,
                                              std::size_t depth,
                                              WorkCleanupBudget* budget,
                                              WorkManifest* manifest) {
  if (!budget || depth > kMaxWorkCleanupDepth) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "The import work tree exceeds its ownership depth limit.");
  }
  auto enumeration = posix_file::open_directory_at(directory, ".");
  if (!enumeration) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "Could not reopen an import work directory for ownership capture.");
  }
  DIR* stream = ::fdopendir(enumeration.release());
  if (!stream) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "Could not enumerate an import work directory for ownership capture.");
  }
  int read_error = 0;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream);
    if (!entry) {
      read_error = errno;
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (budget->entries == kMaxWorkCleanupEntries) {
      ::closedir(stream);
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "The import work tree exceeds its ownership entry limit.");
    }
    ++budget->entries;
    struct stat status {};
    if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
      ::closedir(stream);
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "An import work entry changed during ownership capture.");
    }
    const posix_file::Identity identity{status.st_dev, status.st_ino};
    const auto relative =
        prefix.empty() ? name : std::string(prefix) + "/" + name;
    const bool is_directory = S_ISDIR(status.st_mode);
    if ((!is_directory && (!S_ISREG(status.st_mode) || status.st_nlink != 1)) ||
        !manifest || !manifest->emplace(relative, WorkEntry{identity, is_directory}).second) {
      ::closedir(stream);
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "The import work tree contains an unsafe or ambiguous entry.");
    }
    if (S_ISDIR(status.st_mode)) {
      auto child = posix_file::open_directory_at(directory, name);
      if (!child || !posix_file::entry_identity(directory, name, identity)) {
        ::closedir(stream);
        return make_error(ErrorCode::candidate_cleanup_failed,
                          "An import work directory changed during ownership capture.");
      }
      if (const auto error = capture_work_manifest_at(
              child.get(), relative, depth + 1, budget, manifest)) {
        ::closedir(stream);
        return error;
      }
      if (!posix_file::entry_identity(directory, name, identity)) {
        ::closedir(stream);
        return make_error(ErrorCode::candidate_cleanup_failed,
                          "An import work directory changed during ownership capture.");
      }
    } else if (!posix_file::entry_identity(directory, name, identity)) {
      ::closedir(stream);
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "An import work file changed during ownership capture.");
    }
  }
  ::closedir(stream);
  if (read_error != 0) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "Could not completely capture the import work tree.");
  }
  return {};
}

Result<WorkManifest> capture_work_manifest_at(int directory) {
  WorkCleanupBudget budget;
  WorkManifest manifest;
  if (const auto error =
          capture_work_manifest_at(directory, {}, 0, &budget, &manifest)) {
    return Result<WorkManifest>::failure(*error);
  }
  return Result<WorkManifest>::success(std::move(manifest));
}

Result<posix_file::OwnedFd> open_work_parent_at(int root,
                                               std::string_view relative,
                                               const WorkManifest& manifest) {
  auto current = posix_file::open_directory_at(root, ".");
  if (!current) {
    return Result<posix_file::OwnedFd>::failure(make_error(
        ErrorCode::candidate_cleanup_failed,
        "Could not retain the import work root during exact cleanup."));
  }
  std::string prefix;
  std::size_t offset = 0;
  while (true) {
    const auto separator = relative.find('/', offset);
    if (separator == std::string_view::npos) {
      break;
    }
    const auto component = relative.substr(offset, separator - offset);
    prefix = prefix.empty() ? std::string(component) : prefix + "/" + std::string(component);
    const auto expected = manifest.find(prefix);
    auto child = posix_file::open_directory_at(current.get(), component);
    posix_file::Identity descriptor_identity;
    if (expected == manifest.end() || !expected->second.directory || !child ||
        !posix_file::descriptor_identity(child.get(), &descriptor_identity) ||
        descriptor_identity.device != expected->second.identity.device ||
        descriptor_identity.inode != expected->second.identity.inode ||
        !posix_file::entry_identity(current.get(), component, expected->second.identity)) {
      return Result<posix_file::OwnedFd>::failure(make_error(
          ErrorCode::candidate_cleanup_failed,
          "An owned import work directory changed during exact cleanup."));
    }
    current = std::move(child);
    offset = separator + 1;
  }
  return Result<posix_file::OwnedFd>::success(std::move(current));
}

std::optional<Error> remove_owned_work_manifest_at(int directory,
                                                   const WorkManifest& expected) {
  auto actual = capture_work_manifest_at(directory);
  if (!actual || !same_work_manifest(actual.value(), expected)) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "The import work tree contains an unowned or changed entry.");
  }
  std::vector<std::string> paths;
  paths.reserve(expected.size());
  for (const auto& [path, entry] : expected) {
    (void)entry;
    paths.push_back(path);
  }
  std::sort(paths.begin(), paths.end(), [](const auto& left, const auto& right) {
    const auto left_depth = std::count(left.begin(), left.end(), '/');
    const auto right_depth = std::count(right.begin(), right.end(), '/');
    return left_depth != right_depth ? left_depth > right_depth : left > right;
  });
  for (const auto& path : paths) {
    auto parent = open_work_parent_at(directory, path, expected);
    if (!parent) {
      return parent.error();
    }
    const auto name_offset = path.rfind('/');
    const auto name = name_offset == std::string::npos
                          ? std::string_view(path)
                          : std::string_view(path).substr(name_offset + 1);
    const auto& entry = expected.at(path);
    if (!posix_file::entry_identity(parent.value().get(), name, entry.identity) ||
        ::unlinkat(parent.value().get(), std::string(name).c_str(),
                   entry.directory ? AT_REMOVEDIR : 0) != 0) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "Could not remove an exact owned import work entry.");
    }
  }
  return {};
}

class WorkOwnershipGuard {
 public:
  WorkOwnershipGuard(int work_directory, const Options& external_options)
      : m_work_directory(work_directory), m_external_options(external_options) {}

  Options guarded_options() {
    Options guarded;
    guarded.should_cancel = m_external_options.should_cancel;
    if (m_external_options.on_progress) {
      guarded.on_progress = [this](const Progress& progress) {
        auto before = capture_work_manifest_at(m_work_directory);
        if (!before) {
          m_callback_failed = true;
          throw std::runtime_error("Could not capture work ownership before a callback.");
        }
        m_owned = before.take_value();
        try {
          m_external_options.on_progress(progress);
        } catch (...) {
          m_callback_failed = true;
          throw;
        }
        auto after = capture_work_manifest_at(m_work_directory);
        if (!after || !same_work_manifest(after.value(), m_owned)) {
          m_callback_failed = true;
          throw std::runtime_error("A callback changed the import work ownership set.");
        }
      };
    }
    return guarded;
  }

  std::optional<Error> accept_importer_changes() {
    auto current = capture_work_manifest_at(m_work_directory);
    if (!current) {
      return current.error();
    }
    m_owned = current.take_value();
    return {};
  }

  const WorkManifest& owned() const { return m_owned; }
  bool callback_failed() const { return m_callback_failed; }

 private:
  int m_work_directory = -1;
  const Options& m_external_options;
  WorkManifest m_owned;
  bool m_callback_failed = false;
};

std::optional<Error> validate_captured_directory(
    int directory,
    const std::map<std::string, checked_file_identity::Identity>& expected) {
  std::set<std::string> names;
  for (const auto& [name, identity] : expected) {
    names.emplace(name);
    auto current = capture_file_at(directory, name);
    if (!current || current.value() != identity) {
      return make_error(ErrorCode::candidate_finalize_failed,
                        "A prepared output changed after its exact identity was captured.");
    }
  }
  auto current = capture_directory_at(directory, names);
  if (!current || current.value() != expected) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "A prepared output directory changed after capture.");
  }
  return {};
}

std::optional<Error> validate_captured_output(const CapturedPreparedOutput& output,
                                              int containing_directory,
                                              std::string_view root_name) {
  if (!posix_file::entry_identity(containing_directory, root_name, output.root_identity) ||
      !posix_file::entry_identity(output.root.get(), "iso", output.iso_identity) ||
      !posix_file::entry_identity(output.root.get(), "fr3", output.fr3_identity) ||
      !exact_child_directories(output.root.get(),
                               {{"fr3", output.fr3_identity}, {"iso", output.iso_identity}})) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "The prepared output directory identity changed before promotion.");
  }
  if (const auto error = validate_captured_directory(output.iso.get(), output.iso_files)) {
    return error;
  }
  return validate_captured_directory(output.fr3.get(), output.fr3_files);
}

std::optional<Error> promote_captured_directory(int prepared_root,
                                                int candidate_root,
                                                std::string_view name,
                                                const posix_file::Identity& identity) {
  if (!posix_file::entry_identity(prepared_root, name, identity)) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "A prepared output directory changed before promotion.");
  }
  if (posix_file::exclusive_rename_at(prepared_root, name, candidate_root, name) != 0 ||
      !posix_file::entry_identity(candidate_root, name, identity)) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "Could not exclusively promote the exact prepared output directory.");
  }
  return {};
}

Error preserve_error_at(Error error, const internal::WorkPaths& paths, int work_directory) {
  (void)work_directory;
  error.preserved_candidate_root = paths.candidate_root;
  return error;
}

Error map_source_pack_failure(const source_pack::Error& error, CallbackForwarder& callbacks) {
  if (error.code == source_pack::ErrorCode::callback_failed || callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  if (error.code == source_pack::ErrorCode::cancelled) {
    return make_error(ErrorCode::cancelled, "Jak II source-pack validation was cancelled.");
  }
  return make_error(ErrorCode::source_pack_failed,
                    "The checked Jak II source-object pack was rejected: " + error.message);
}

}  // namespace

namespace internal {

std::optional<Error> write_file_atomically(const fs::path& destination,
                                           std::span<const std::uint8_t> bytes,
                                           const Options& options) {
  CallbackForwarder callbacks(options);
  return write_file_atomically_impl(destination, bytes, callbacks);
}

namespace {

std::string retail_identity(std::string_view source_archive_relative_path,
                            std::string_view internal_name,
                            std::string_view unique_name) {
  std::string result;
  result.reserve(source_archive_relative_path.size() + internal_name.size() + unique_name.size() +
                 2);
  result.append(source_archive_relative_path);
  result.push_back('\n');
  result.append(internal_name);
  result.push_back('\n');
  result.append(unique_name);
  return result;
}

bool safe_retail_archive_path(std::string_view path) {
  const auto separator = path.find('/');
  if (separator == std::string_view::npos ||
      path.find('/', separator + 1) != std::string_view::npos) {
    return false;
  }
  const auto directory = path.substr(0, separator);
  const auto basename = path.substr(separator + 1);
  return safe_basename(basename) &&
         ((directory == "DGO" && basename.ends_with(".DGO")) ||
          (directory == "CGO" && basename.ends_with(".CGO")));
}

bool valid_public_name(std::string_view name, std::size_t cap) {
  if (name.empty() || name.size() > cap || name == "." || name == ".." || name.back() == '.') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

}  // namespace

Result<RetailRequirements> derive_retail_requirements(const generator::Graph& graph,
                                                       const Options& options) {
  try {
    CallbackForwarder callbacks(options);
    std::map<std::string, RetailObjectRequirement> objects;
    std::map<std::string, std::string> archives;
    const generator::Options generator_options;
    std::size_t occurrences = 0;
    for (const auto& archive : graph.archives) {
      for (const auto& object : archive.objects) {
        if (object.producer != core_generator::ObjectProducerKind::verified_retail) {
          continue;
        }
        if (callbacks.poll_cancel()) {
          return Result<RetailRequirements>::failure(callbacks.cancellation_or_callback_error(
              "Jak II retail-requirement inspection was cancelled."));
        }
        if (occurrences == std::numeric_limits<std::size_t>::max()) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains too many retail occurrences."));
        }
        if (!safe_retail_archive_path(object.retail_source_archive)) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains an unsafe retail archive path."));
        }
        if (!valid_public_name(object.prepared_basename,
                               generator_options.limits.max_name_bytes) ||
            !object.prepared_basename.ends_with(".go")) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains an unsafe prepared retail-object name."));
        }
        if (!valid_public_name(object.internal_name, generator_options.limits.max_name_bytes)) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains an unsafe internal retail-object name."));
        }
        ++occurrences;
        auto unique_name = object.prepared_basename;
        unique_name.resize(unique_name.size() - 3);
        if (unique_name.empty()) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains an empty retail-object identity."));
        }
        auto collision = object.retail_source_archive;
        std::transform(collision.begin(), collision.end(), collision.begin(),
                       [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
        const auto [archive_entry, inserted] =
            archives.emplace(std::move(collision), object.retail_source_archive);
        if (!inserted && archive_entry->second != object.retail_source_archive) {
          return Result<RetailRequirements>::failure(make_error(
              ErrorCode::graph_failed,
              "The checked Jak II graph contains case-colliding retail archives."));
        }
        RetailObjectRequirement requirement{object.retail_source_archive, object.internal_name,
                                            std::move(unique_name)};
        objects.emplace(retail_identity(requirement.source_archive_relative_path,
                                        requirement.internal_name, requirement.unique_name),
                        std::move(requirement));
      }
    }
    if (occurrences == 0 || archives.empty() || objects.empty()) {
      return Result<RetailRequirements>::failure(make_error(
          ErrorCode::graph_failed, "The checked Jak II graph selects no retail objects."));
    }
    RetailRequirements requirements;
    requirements.occurrence_count = occurrences;
    requirements.source_archive_relative_paths.reserve(archives.size());
    for (const auto& archive : archives) {
      requirements.source_archive_relative_paths.push_back(archive.second);
    }
    std::sort(requirements.source_archive_relative_paths.begin(),
              requirements.source_archive_relative_paths.end());
    requirements.objects.reserve(objects.size());
    for (auto& object : objects) {
      requirements.objects.push_back(std::move(object.second));
    }
    return Result<RetailRequirements>::success(std::move(requirements));
  } catch (const std::bad_alloc&) {
    return Result<RetailRequirements>::failure(make_error(
        ErrorCode::allocation_failed, "Jak II retail-requirement inspection ran out of memory."));
  }
}

Result<std::vector<generator::RetailCatalogObject>> select_exact_retail_catalog(
    const RetailRequirements& requirements,
    std::span<const retail_catalog::Entry> entries,
    std::uint64_t max_total_object_bytes,
    const Options& options) {
  try {
    if (requirements.occurrence_count == 0 ||
        requirements.source_archive_relative_paths.empty() || requirements.objects.empty() ||
        max_total_object_bytes == 0 || entries.size() > kMaxIndexedRetailEntries) {
      return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
          ErrorCode::retail_catalog_failed,
          "The Jak II retail-catalog selection inputs are invalid."));
    }
    CallbackForwarder callbacks(options);
    std::map<std::string, const retail_catalog::Provenance*> catalog;
    for (const auto& entry : entries) {
      if (callbacks.poll_cancel()) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(
            callbacks.cancellation_or_callback_error(
                "Jak II retail-catalog selection was cancelled."));
      }
      const auto& provenance = entry.provenance;
      const auto key = retail_identity(provenance.source_archive_relative_path,
                                       provenance.internal_name, provenance.unique_name);
      if (!catalog.emplace(key, &provenance).second) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
            ErrorCode::retail_catalog_failed,
            "The checked Jak II retail catalog contains a duplicate object identity."));
      }
    }

    std::vector<generator::RetailCatalogObject> selected;
    selected.reserve(requirements.objects.size());
    std::uint64_t total_object_bytes = 0;
    for (const auto& requirement : requirements.objects) {
      if (callbacks.poll_cancel()) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(
            callbacks.cancellation_or_callback_error(
                "Jak II retail-catalog selection was cancelled."));
      }
      const auto found = catalog.find(retail_identity(requirement.source_archive_relative_path,
                                                      requirement.internal_name,
                                                      requirement.unique_name));
      if (found == catalog.end()) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
            ErrorCode::retail_catalog_failed,
            "The checked Jak II retail catalog is missing a graph-required object."));
      }
      const auto& provenance = *found->second;
      if ((provenance.object_version != retail_catalog::ObjectVersion::v2 &&
           provenance.object_version != retail_catalog::ObjectVersion::v4)) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
            ErrorCode::retail_catalog_failed,
            "The checked Jak II retail catalog contains an unsupported object version."));
      }
      if (provenance.byte_size == 0 ||
          provenance.byte_size > max_total_object_bytes - total_object_bytes) {
        return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
            ErrorCode::retail_catalog_failed,
            "The checked Jak II retail catalog exceeds its object-byte limit."));
      }
      total_object_bytes += provenance.byte_size;
      selected.push_back({provenance.source_archive_relative_path,
                          provenance.archive_object_index,
                          provenance.internal_name,
                          provenance.unique_name,
                          static_cast<std::uint32_t>(provenance.object_version),
                          static_cast<std::uint64_t>(provenance.byte_size),
                          provenance.xxh64});
    }
    if (selected.size() != requirements.objects.size()) {
      return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
          ErrorCode::retail_catalog_failed,
          "The checked Jak II retail catalog selection is incomplete."));
    }
    return Result<std::vector<generator::RetailCatalogObject>>::success(std::move(selected));
  } catch (const std::bad_alloc&) {
    return Result<std::vector<generator::RetailCatalogObject>>::failure(make_error(
        ErrorCode::allocation_failed, "Jak II retail-catalog selection ran out of memory."));
  }
}

Result<Summary> compose_in_fresh_candidate(const fs::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages,
                                           const std::optional<Summary>* produced_summary,
                                           const std::optional<FinalContract>* produced_contract) {
  WorkPaths paths{candidate_root, candidate_root / kWorkDirectoryName,
                  candidate_root / kWorkDirectoryName / kPreparedDirectoryName};
  bool candidate_created = false;
  posix_file::OwnedFd candidate_parent;
  posix_file::OwnedFd candidate_directory;
  posix_file::OwnedFd work_directory;
  posix_file::Identity candidate_parent_identity;
  posix_file::Identity candidate_identity;
  posix_file::Identity work_identity;
  std::optional<CapturedPreparedOutput> captured_output;
  const auto preserve_error = [&](Error error, const WorkPaths& preserved_paths) {
    return preserve_error_at(std::move(error), preserved_paths, work_directory.get());
  };
  try {
    if (!candidate_root.is_absolute() || candidate_root.filename().empty() ||
        !direct_directory(candidate_root.parent_path()) || !missing_path(candidate_root) ||
        stages.empty() || !produced_summary || !produced_contract) {
      return Result<Summary>::failure(make_error(
          ErrorCode::invalid_argument,
          "The composer requires a fresh absolute candidate under a direct directory."));
    }
    for (const auto& stage : stages) {
      if (!stage.run && !stage.run_with_options) {
        return Result<Summary>::failure(
            make_error(ErrorCode::invalid_argument, "The composer stage list is incomplete."));
      }
    }
    candidate_parent = posix_file::open_directory(candidate_root.parent_path().c_str());
    if (!candidate_parent ||
        !posix_file::descriptor_identity(candidate_parent.get(), &candidate_parent_identity) ||
        ::mkdirat(candidate_parent.get(), candidate_root.filename().c_str(), 0700) != 0) {
      return Result<Summary>::failure(make_error(
          ErrorCode::candidate_create_failed,
          "Could not exclusively create the descriptor-owned import candidate."));
    }
    candidate_created = true;
    candidate_directory =
        posix_file::open_directory_at(candidate_parent.get(), candidate_root.filename().string());
    if (!candidate_directory ||
        !posix_file::descriptor_identity(candidate_directory.get(), &candidate_identity) ||
        ::mkdirat(candidate_directory.get(), std::string(kWorkDirectoryName).c_str(), 0700) != 0) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_create_failed,
                     "Could not create the descriptor-owned import work directory."),
          paths));
    }
    work_directory = posix_file::open_directory_at(candidate_directory.get(), kWorkDirectoryName);
    if (!work_directory ||
        !posix_file::descriptor_identity(work_directory.get(), &work_identity)) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_create_failed,
                     "Could not retain the descriptor-owned import work directory."),
          paths));
    }

    WorkOwnershipGuard ownership(work_directory.get(), options);
    const auto guarded_options = ownership.guarded_options();
    CallbackForwarder callbacks(guarded_options);
    for (const auto& stage : stages) {
      if (callbacks.poll_cancel()) {
        return Result<Summary>::failure(preserve_error(
            callbacks.cancellation_or_callback_error(
                "Jak II import was cancelled before the next preparation stage."),
            paths));
      }
      if (!callbacks.report({stage.phase, 0, 1, 0, {}})) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error({}), paths));
      }
      const auto stage_error = stage.run_with_options
                                   ? stage.run_with_options(paths, guarded_options)
                                   : stage.run(paths);
      if (stage_error) {
        return Result<Summary>::failure(preserve_error(
            ownership.callback_failed()
                ? make_error(ErrorCode::callback_failed,
                             "A Jak II import progress callback changed the owned work tree.")
                : *stage_error,
            paths));
      }
      if (stage.phase == Phase::materializing_output) {
        if (captured_output || !produced_contract->has_value()) {
          return Result<Summary>::failure(preserve_error(
              make_error(ErrorCode::candidate_finalize_failed,
                         "The materialization stage did not produce one checked contract."),
              paths));
        }
        auto captured = capture_prepared_output(work_directory.get(), produced_contract->value());
        if (!captured) {
          return Result<Summary>::failure(preserve_error(captured.error(), paths));
        }
        captured_output.emplace(captured.take_value());
      }
      if (const auto ownership_error = ownership.accept_importer_changes()) {
        return Result<Summary>::failure(preserve_error(*ownership_error, paths));
      }
      if (!callbacks.report({stage.phase, 1, 1, 0, {}})) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error({}), paths));
      }
    }

    if (!produced_summary->has_value() || !produced_contract->has_value() || !captured_output) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The composition stages did not produce a checked final contract."),
          paths));
    }
    const auto& summary = produced_summary->value();
    const auto& contract = produced_contract->value();
    const std::set<std::string> contract_iso(contract.iso_basenames.begin(),
                                             contract.iso_basenames.end());
    const std::set<std::string> contract_fr3(contract.fr3_basenames.begin(),
                                             contract.fr3_basenames.end());
    if (contract_iso.empty() || contract_fr3.empty() ||
        contract_iso.size() != contract.iso_basenames.size() ||
        contract_fr3.size() != contract.fr3_basenames.size() ||
        summary.archives_written + summary.flat_files_written != contract_iso.size() ||
        summary.fr3_files_written != contract_fr3.size()) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The composition summary does not match its exact final contract."),
          paths));
    }
    if (callbacks.poll_cancel()) {
      return Result<Summary>::failure(preserve_error(
          callbacks.cancellation_or_callback_error(
              "Jak II import was cancelled before candidate finalization."),
          paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 0, 2, 0, "iso"})) {
      return Result<Summary>::failure(
          preserve_error(callbacks.cancellation_or_callback_error({}), paths));
    }
    if (!posix_file::entry_identity(candidate_parent.get(), candidate_root.filename().string(),
                                    candidate_identity) ||
        !posix_file::entry_identity(candidate_directory.get(), kWorkDirectoryName, work_identity)) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The candidate or import work directory changed before finalization."),
          paths));
    }
    if (const auto validation = validate_captured_output(
            *captured_output, work_directory.get(), kPreparedDirectoryName)) {
      return Result<Summary>::failure(preserve_error(*validation, paths));
    }
    if (const auto promote = promote_captured_directory(
            captured_output->root.get(), candidate_directory.get(), "iso",
            captured_output->iso_identity)) {
      return Result<Summary>::failure(preserve_error(*promote, paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 1, 2, 0, "fr3"})) {
      return Result<Summary>::failure(
          preserve_error(callbacks.cancellation_or_callback_error({}), paths));
    }
    if (!posix_file::entry_identity(candidate_directory.get(), "iso",
                                    captured_output->iso_identity) ||
        !posix_file::entry_identity(captured_output->root.get(), "fr3",
                                    captured_output->fr3_identity) ||
        validate_captured_directory(captured_output->iso.get(), captured_output->iso_files) ||
        validate_captured_directory(captured_output->fr3.get(), captured_output->fr3_files)) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "A prepared output changed between directory promotions."),
          paths));
    }
    if (const auto promote = promote_captured_directory(
            captured_output->root.get(), candidate_directory.get(), "fr3",
            captured_output->fr3_identity)) {
      return Result<Summary>::failure(preserve_error(*promote, paths));
    }
    if (!posix_file::entry_identity(work_directory.get(), kPreparedDirectoryName,
                                    captured_output->root_identity) ||
        ::unlinkat(work_directory.get(), std::string(kPreparedDirectoryName).c_str(),
                   AT_REMOVEDIR) != 0) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_cleanup_failed,
                     "Could not remove the exact empty prepared-output directory."),
          paths));
    }
    if (!posix_file::entry_identity(candidate_directory.get(), kWorkDirectoryName,
                                    work_identity)) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_cleanup_failed,
                     "The import work directory changed before cleanup."),
          paths));
    }
    if (const auto ownership_error = ownership.accept_importer_changes()) {
      return Result<Summary>::failure(preserve_error(*ownership_error, paths));
    }
    if (const auto cleanup =
            remove_owned_work_manifest_at(work_directory.get(), ownership.owned())) {
      return Result<Summary>::failure(preserve_error(*cleanup, paths));
    }
    if (!posix_file::entry_identity(candidate_directory.get(), kWorkDirectoryName,
                                    work_identity) ||
        ::unlinkat(candidate_directory.get(), std::string(kWorkDirectoryName).c_str(),
                   AT_REMOVEDIR) != 0) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_cleanup_failed,
                     "Could not remove the exact completed import work directory."),
          paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 2, 2,
                           summary.output_bytes, {}})) {
      auto callback_error = callbacks.cancellation_or_callback_error({});
      callback_error.preserved_candidate_root = candidate_root;
      return Result<Summary>::failure(std::move(callback_error));
    }
    struct stat candidate_parent_status {};
    if (::lstat(candidate_root.parent_path().c_str(), &candidate_parent_status) != 0 ||
        !posix_file::same_identity(candidate_parent_status, candidate_parent_identity) ||
        !posix_file::entry_identity(candidate_parent.get(), candidate_root.filename().string(),
                                    candidate_identity) ||
        !exact_child_directories(candidate_directory.get(),
                                 {{"fr3", captured_output->fr3_identity},
                                  {"iso", captured_output->iso_identity}}) ||
        validate_captured_directory(captured_output->iso.get(), captured_output->iso_files) ||
        validate_captured_directory(captured_output->fr3.get(), captured_output->fr3_files)) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The candidate changed during terminal descriptor validation."),
          paths));
    }
    return Result<Summary>::success(summary);
  } catch (const std::bad_alloc&) {
    auto error = make_error(ErrorCode::allocation_failed,
                            "Jak II import composition ran out of memory.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (const std::exception& exception) {
    auto error = make_error(ErrorCode::unexpected_failure,
                            "Jak II import composition failed unexpectedly: " +
                                std::string(exception.what()));
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (...) {
    auto error = make_error(ErrorCode::unexpected_failure,
                            "Jak II import composition failed unexpectedly.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  }
}

}  // namespace internal

Result<Summary> compose(const Request& request, const Options& options) {
  try {
    auto validated_request = validate_request(request);
    if (!validated_request) {
      return Result<Summary>::failure(validated_request.error());
    }
    const auto resolved = validated_request.take_value();
    auto iso = open_iso_file(resolved.iso_path);
    if (!iso) {
      return Result<Summary>::failure(iso.error());
    }

    CallbackForwarder callbacks(options);
    if (callbacks.poll_cancel()) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error(
          "Jak II import was cancelled before source-pack validation."));
    }
    source_pack::Options source_options;
    source_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    source_options.on_progress = [&](const source_pack::Progress& progress) {
      callbacks.report({Phase::validating_source_pack, progress.completed, progress.total,
                        progress.bytes_hashed, progress.current_file});
    };
    auto verified_pack = source_pack::validate_recorded(resolved.source_object_pack_root,
                                                        source_options);
    if (!verified_pack) {
      return Result<Summary>::failure(map_source_pack_failure(verified_pack.error(), callbacks));
    }
    if (callbacks.callback_failed()) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error({}));
    }
    if (!callbacks.report({Phase::validating_project_resources, 0, kProjectResources.size(), 0,
                           {}})) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error({}));
    }
    if (const auto resource_error = validate_project_resources(
            resolved.project_resource_root, options)) {
      return Result<Summary>::failure(*resource_error);
    }

    jak1_output_graph::Options graph_options = jak2_public_output_graph::default_options();
    graph_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    auto graph = jak2_public_output_graph::decode_base_retail(graph_options);
    if (!graph) {
      const bool cancelled = graph.error().code == jak1_output_graph::ErrorCode::cancelled;
      return Result<Summary>::failure(callback_aware_error(
          callbacks, ErrorCode::graph_failed,
          cancelled ? "Jak II output-graph loading was cancelled."
                    : "The embedded Jak II output graph was rejected: " + graph.error().message,
          cancelled));
    }

    PipelineState state;
    state.graph = graph.take_value();
    state.source_pack_summary = verified_pack.take_value();
    state.iso_file = iso.take_value();
    const std::array<internal::StageAction, 6> stages = {{
        {Phase::extracting_iso,
         [&](const internal::WorkPaths& paths, const Options& stage_options) {
           return extract_iso_stage(paths, stage_options, &state);
         }},
        {Phase::cataloging_retail,
         [&](const internal::WorkPaths&, const Options& stage_options) {
           return catalog_retail_stage(stage_options, &state);
         }},
        {Phase::generating_data,
         [&](const internal::WorkPaths&, const Options& stage_options) {
           return generate_data_stage(resolved, stage_options, &state);
         }},
        {Phase::preparing_fr3,
         [&](const internal::WorkPaths& paths, const Options& stage_options) {
           return prepare_fr3_stage(resolved, paths, stage_options, &state);
         }},
        {Phase::generating_recipe,
         [&](const internal::WorkPaths& paths, const Options& stage_options) {
           return generate_recipe_stage(resolved, paths, stage_options, &state);
         }},
        {Phase::materializing_output,
         [&](const internal::WorkPaths& paths, const Options& stage_options) {
           return materialize_stage(resolved, paths, stage_options, &state);
         }},
    }};
    return internal::compose_in_fresh_candidate(resolved.candidate_root, options, stages,
                                                &state.summary, &state.final_contract);
  } catch (const std::bad_alloc&) {
    return Result<Summary>::failure(
        make_error(ErrorCode::allocation_failed, "Jak II import composition ran out of memory."));
  } catch (const std::exception& exception) {
    return Result<Summary>::failure(make_error(
        ErrorCode::unexpected_failure,
        "Jak II import composition failed unexpectedly: " + std::string(exception.what())));
  } catch (...) {
    return Result<Summary>::failure(make_error(
        ErrorCode::unexpected_failure, "Jak II import composition failed unexpectedly."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::cancelled: return "cancelled";
    case ErrorCode::callback_failed: return "callback_failed";
    case ErrorCode::source_pack_failed: return "source_pack_failed";
    case ErrorCode::project_resources_failed: return "project_resources_failed";
    case ErrorCode::iso_open_failed: return "iso_open_failed";
    case ErrorCode::iso_validation_failed: return "iso_validation_failed";
    case ErrorCode::unsupported_revision: return "unsupported_revision";
    case ErrorCode::graph_failed: return "graph_failed";
    case ErrorCode::generated_data_failed: return "generated_data_failed";
    case ErrorCode::fr3_failed: return "fr3_failed";
    case ErrorCode::retail_catalog_failed: return "retail_catalog_failed";
    case ErrorCode::recipe_failed: return "recipe_failed";
    case ErrorCode::candidate_create_failed: return "candidate_create_failed";
    case ErrorCode::work_write_failed: return "work_write_failed";
    case ErrorCode::insufficient_storage: return "insufficient_storage";
    case ErrorCode::materialization_failed: return "materialization_failed";
    case ErrorCode::candidate_finalize_failed: return "candidate_finalize_failed";
    case ErrorCode::candidate_cleanup_failed: return "candidate_cleanup_failed";
    case ErrorCode::allocation_failed: return "allocation_failed";
    case ErrorCode::unexpected_failure: return "unexpected_failure";
  }
  return "unknown";
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::validating_source_pack: return "validating_source_pack";
    case Phase::validating_project_resources: return "validating_project_resources";
    case Phase::extracting_iso: return "extracting_iso";
    case Phase::cataloging_retail: return "cataloging_retail";
    case Phase::generating_data: return "generating_data";
    case Phase::preparing_fr3: return "preparing_fr3";
    case Phase::generating_recipe: return "generating_recipe";
    case Phase::materializing_output: return "materializing_output";
    case Phase::finalizing_candidate: return "finalizing_candidate";
  }
  return "unknown";
}

}  // namespace jak2_import_composer
