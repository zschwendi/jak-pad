#include "jak2_import_composer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
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

std::optional<Error> remove_partial_residue(const fs::path& work_root);
Error preserve_error(Error error, const internal::WorkPaths& paths);
std::optional<Error> promote_directory(const fs::path& source, const fs::path& destination);
std::optional<Error> write_file_atomically(const fs::path& destination,
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
  explicit OwnedPartialFile(fs::path path) : m_path(std::move(path)) {}
  ~OwnedPartialFile() {
    if (m_descriptor >= 0) {
      ::close(m_descriptor);
    }
    if (m_exists) {
      std::error_code ignored;
      fs::remove(m_path, ignored);
    }
  }
  void set_descriptor(int descriptor) { m_descriptor = descriptor; }
  void mark_exists() { m_exists = true; }
  void release() { m_exists = false; }
  std::optional<std::string> close_checked() {
    const auto descriptor = m_descriptor;
    m_descriptor = -1;
    if (descriptor >= 0 && ::close(descriptor) != 0) {
      return "Could not close an import partial file: " +
             std::error_code(errno, std::generic_category()).message();
    }
    return {};
  }

 private:
  fs::path m_path;
  int m_descriptor = -1;
  bool m_exists = false;
};

std::optional<Error> write_file_atomically(const fs::path& destination,
                                           std::span<const std::uint8_t> bytes,
                                           CallbackForwarder& callbacks) {
  if (bytes.empty() || !direct_directory(destination.parent_path()) ||
      !missing_path(destination)) {
    return make_error(ErrorCode::work_write_failed,
                      "An import work-file destination is invalid or already exists.");
  }
  const auto partial = fs::path(destination.string() + ".partial");
  OwnedPartialFile file(partial);
  const auto descriptor = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not create an exclusive import partial file",
                                 std::error_code(errno, std::generic_category()));
  }
  file.set_descriptor(descriptor);
  file.mark_exists();
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (callbacks.poll_cancel()) {
      return callbacks.cancellation_or_callback_error(
          "Jak II import was cancelled while writing generated data.");
    }
    const auto count = std::min(kIoChunkBytes, bytes.size() - offset);
    const auto written = ::write(descriptor, bytes.data() + offset, count);
    if (written <= 0) {
      return make_filesystem_error(ErrorCode::work_write_failed,
                                   "Could not write a complete import work file",
                                   std::error_code(errno, std::generic_category()));
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not synchronize an import work file",
                                 std::error_code(errno, std::generic_category()));
  }
  if (const auto close_error = file.close_checked()) {
    return make_error(ErrorCode::work_write_failed, *close_error);
  }
  std::error_code error;
  fs::rename(partial, destination, error);
  if (error) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not install an import work file", error);
  }
  file.release();
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
    if (const auto write_error = write_file_atomically(root / relative, artifact.bytes, callbacks)) {
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
          write_file_atomically(paths.work_root / kRecipeFileName, wire.value(), callbacks)) {
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

Result<std::set<std::string>> exact_regular_file_set(const fs::path& root,
                                                     ErrorCode code,
                                                     std::string_view description) {
  if (!direct_directory(root)) {
    return Result<std::set<std::string>>::failure(
        make_error(code, std::string(description) + " is missing or linked."));
  }
  std::set<std::string> files;
  std::error_code error;
  for (fs::directory_iterator iterator(root, error), end; !error && iterator != end;
       iterator.increment(error)) {
    const auto status = iterator->symlink_status(error);
    const auto name = iterator->path().filename().string();
    if (error || status.type() != fs::file_type::regular || !safe_basename(name) ||
        !files.emplace(name).second) {
      return Result<std::set<std::string>>::failure(make_error(
          code, std::string(description) + " contains an unsafe or duplicate entry."));
    }
  }
  if (error) {
    return Result<std::set<std::string>>::failure(
        make_error(code, std::string(description) + " changed during inspection."));
  }
  return Result<std::set<std::string>>::success(std::move(files));
}

std::optional<Error> validate_output_tree(const fs::path& root,
                                          const internal::FinalContract& contract,
                                          ErrorCode code) {
  const auto iso = exact_regular_file_set(root / "iso", code, "The candidate ISO directory");
  if (!iso) {
    return iso.error();
  }
  const auto fr3 = exact_regular_file_set(root / "fr3", code, "The candidate FR3 directory");
  if (!fr3) {
    return fr3.error();
  }
  if (iso.value() != std::set<std::string>(contract.iso_basenames.begin(),
                                           contract.iso_basenames.end()) ||
      fr3.value() != std::set<std::string>(contract.fr3_basenames.begin(),
                                           contract.fr3_basenames.end())) {
    return make_error(code, "The candidate does not exactly match its checked output contract.");
  }
  std::set<std::string> entries;
  std::error_code error;
  for (fs::directory_iterator iterator(root, error), end; !error && iterator != end;
       iterator.increment(error)) {
    entries.emplace(iterator->path().filename().string());
  }
  if (error || entries != std::set<std::string>{"fr3", "iso"}) {
    return make_error(code, "The candidate contains unexpected top-level entries.");
  }
  return {};
}

std::optional<Error> remove_partial_residue(const fs::path& work_root) {
  if (!direct_directory(work_root)) {
    return {};
  }
  std::error_code error;
  fs::recursive_directory_iterator iterator(work_root, error);
  const fs::recursive_directory_iterator end;
  for (; !error && iterator != end; iterator.increment(error)) {
    if (!iterator->path().filename().string().ends_with(".partial")) {
      continue;
    }
    const auto status = iterator->symlink_status(error);
    if (error || status.type() != fs::file_type::regular ||
        !fs::remove(iterator->path(), error) || error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "Could not remove an import partial file from the preserved candidate.");
    }
  }
  if (error) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "The preserved candidate changed during partial cleanup.");
  }
  return {};
}

Error preserve_error(Error error, const internal::WorkPaths& paths) {
  error.preserved_candidate_root = paths.candidate_root;
  if (const auto cleanup = remove_partial_residue(paths.work_root)) {
    error.cleanup_error = cleanup->message;
  }
  return error;
}

std::optional<Error> promote_directory(const fs::path& source, const fs::path& destination) {
  if (!direct_directory(source) || !missing_path(destination)) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "A prepared output is missing or its destination already exists.");
  }
#if defined(__APPLE__)
  if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) != 0) {
    return make_filesystem_error(ErrorCode::candidate_finalize_failed,
                                 "Could not exclusively promote a prepared output directory",
                                 std::error_code(errno, std::generic_category()));
  }
#else
  std::error_code error;
  fs::rename(source, destination, error);
  if (error) {
    return make_filesystem_error(ErrorCode::candidate_finalize_failed,
                                 "Could not promote a prepared output directory", error);
  }
#endif
  return {};
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
  try {
    if (!candidate_root.is_absolute() || candidate_root.filename().empty() ||
        !direct_directory(candidate_root.parent_path()) || !missing_path(candidate_root) ||
        stages.empty() || !produced_summary || !produced_contract) {
      return Result<Summary>::failure(make_error(
          ErrorCode::invalid_argument,
          "The composer requires a fresh absolute candidate under a direct directory."));
    }
    for (const auto& stage : stages) {
      if (!stage.run) {
        return Result<Summary>::failure(
            make_error(ErrorCode::invalid_argument, "The composer stage list is incomplete."));
      }
    }
    std::error_code error;
    if (!fs::create_directory(candidate_root, error) || error) {
      return Result<Summary>::failure(make_filesystem_error(
          ErrorCode::candidate_create_failed, "Could not exclusively create the import candidate",
          error));
    }
    candidate_created = true;
    if (!fs::create_directory(paths.work_root, error) || error) {
      return Result<Summary>::failure(preserve_error(
          make_filesystem_error(ErrorCode::candidate_create_failed,
                                "Could not create the hidden import work directory", error),
          paths));
    }

    CallbackForwarder callbacks(options);
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
      if (const auto stage_error = stage.run(paths)) {
        return Result<Summary>::failure(preserve_error(*stage_error, paths));
      }
      if (!callbacks.report({stage.phase, 1, 1, 0, {}})) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error({}), paths));
      }
    }

    if (!produced_summary->has_value() || !produced_contract->has_value()) {
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
    if (const auto validation = validate_output_tree(
            paths.prepared_root, contract, ErrorCode::candidate_finalize_failed)) {
      return Result<Summary>::failure(preserve_error(*validation, paths));
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
    if (const auto promote = promote_directory(paths.prepared_root / "iso", candidate_root / "iso")) {
      return Result<Summary>::failure(preserve_error(*promote, paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 1, 2, 0, "fr3"})) {
      return Result<Summary>::failure(
          preserve_error(callbacks.cancellation_or_callback_error({}), paths));
    }
    if (const auto promote = promote_directory(paths.prepared_root / "fr3", candidate_root / "fr3")) {
      return Result<Summary>::failure(preserve_error(*promote, paths));
    }
    if (!fs::remove(paths.prepared_root, error) || error) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_cleanup_failed,
                     "Could not remove the empty prepared-output directory."),
          paths));
    }
    fs::remove_all(paths.work_root, error);
    if (error) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_cleanup_failed,
                     "Could not remove the completed import work directory."),
          paths));
    }
    if (const auto validation = validate_output_tree(
            candidate_root, contract, ErrorCode::candidate_finalize_failed)) {
      return Result<Summary>::failure(preserve_error(*validation, paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 2, 2,
                           summary.output_bytes, {}})) {
      auto callback_error = callbacks.cancellation_or_callback_error({});
      callback_error.preserved_candidate_root = candidate_root;
      return Result<Summary>::failure(std::move(callback_error));
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
         [&](const internal::WorkPaths& paths) {
           return extract_iso_stage(paths, options, &state);
         }},
        {Phase::cataloging_retail,
         [&](const internal::WorkPaths&) {
           return catalog_retail_stage(options, &state);
         }},
        {Phase::generating_data,
         [&](const internal::WorkPaths&) {
           return generate_data_stage(resolved, options, &state);
         }},
        {Phase::preparing_fr3,
         [&](const internal::WorkPaths& paths) {
           return prepare_fr3_stage(resolved, paths, options, &state);
         }},
        {Phase::generating_recipe,
         [&](const internal::WorkPaths& paths) {
           return generate_recipe_stage(resolved, paths, options, &state);
         }},
        {Phase::materializing_output,
         [&](const internal::WorkPaths& paths) {
           return materialize_stage(resolved, paths, options, &state);
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
