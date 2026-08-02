#include "jak1_import_composer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <variant>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "common/custom_data/Jak1OutputMaterializer.h"
#include "common/custom_data/Jak1PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak1PublicOutputGraph.h"
#include "common/custom_data/Jak1SourceObjectPack.h"

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"
#include "decompiler/extractor/jak1_fr3_preparer.h"
#include "decompiler/extractor/jak1_import_composer_internal.h"
#include "decompiler/extractor/jak1_iso_validation.h"
#include "decompiler/extractor/jak1_retail_object_catalog.h"
#include "goalc/make/Jak1OutputRecipeGenerator.h"

namespace jak1_import_composer {
namespace {

namespace artifacts = jak1_public_generated_artifacts;
namespace fs = std::filesystem;
namespace generator = jak1_output_recipe_generator;
namespace materializer = jak1_output_materializer;
namespace recipe = jak1_output_recipe;
namespace retail_catalog = jak1_retail_object_catalog;
namespace source_pack = jak1_source_object_pack;

constexpr std::uint64_t kMaxManifestBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kMaxArchiveBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t kMaxTotalArchiveBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaxCatalogObjectBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::size_t kMaxCatalogEntries = 16384;
constexpr std::size_t kIoChunkBytes = 256 * 1024;
constexpr std::string_view kGraphIsoRoot = "iso_data/jak1";
constexpr std::string_view kWorkDirectoryName = ".opengoal-import";
constexpr std::string_view kPreparedDirectoryName = ".prepared";
constexpr std::string_view kRecipeFileName = "jak1-output-recipe.bin";

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
  if (path.empty() || path.is_absolute() || path.has_root_path()) {
    return false;
  }
  const auto normalized = path.lexically_normal();
  if (normalized != path || normalized.empty()) {
    return false;
  }
  for (const auto& component : normalized) {
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
  }
  return true;
}

bool safe_archive_basename(std::string_view basename, std::string_view extension) {
  if (basename.empty() || basename.size() > 128 || !basename.ends_with(extension)) {
    return false;
  }
  return std::all_of(basename.begin(), basename.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
  });
}

struct CallbackForwarder {
  explicit CallbackForwarder(const Options& input_options) : options(input_options) {}

  bool poll_cancel() {
    if (callback_failed) {
      return true;
    }
    if (!options.should_cancel) {
      return false;
    }
    try {
      return options.should_cancel();
    } catch (...) {
      callback_failed = true;
      return true;
    }
  }

  bool report(Progress progress) {
    if (callback_failed) {
      return false;
    }
    if (!options.on_progress) {
      return true;
    }
    try {
      options.on_progress(progress);
      return true;
    } catch (...) {
      callback_failed = true;
      return false;
    }
  }

  Error cancellation_or_callback_error(std::string cancellation_message) const {
    return callback_failed
               ? make_error(ErrorCode::callback_failed,
                            "The Jak 1 import callback failed while composing the candidate.")
               : make_error(ErrorCode::cancelled, std::move(cancellation_message));
  }

  const Options& options;
  bool callback_failed = false;
};

struct FileCloser {
  void operator()(FILE* file) const {
    if (file) {
      std::fclose(file);
    }
  }
};

using OwnedFile = std::unique_ptr<FILE, FileCloser>;

Result<OwnedFile> open_iso_file(const fs::path& path) {
  const auto descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    const std::error_code error(errno, std::generic_category());
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO could not be opened safely", error));
  }

  struct stat status {};
  if (::fstat(descriptor, &status) != 0) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO could not be inspected", error));
  }
  if (!S_ISREG(status.st_mode)) {
    ::close(descriptor);
    return Result<OwnedFile>::failure(
        make_error(ErrorCode::iso_open_failed,
                   "The selected ISO is not a direct regular file."));
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

struct PipelineState {
  generator::Graph graph;
  source_pack::Summary source_pack_summary;
  OwnedFile iso_file;
  std::optional<jak1_iso::StagedExtraction> extraction;
  std::string source_pack_manifest;
  std::vector<std::string> verified_flat_paths;
  std::vector<generator::RetailCatalogObject> retail_objects;
  std::optional<artifacts::Build> generated_artifacts;
  std::vector<materializer::GeneratedObjectArtifact> generated_objects;
  std::vector<materializer::GeneratedFlatArtifact> generated_flat_files;
  std::vector<std::string> expected_fr3_basenames;
  std::optional<recipe::Recipe> output_recipe;
  std::optional<Summary> summary;
};

recipe::RevisionProvenance recipe_revision(const jak1_iso::Revision& revision) {
  return {std::string(revision.serial),
          revision.elf_hash,
          revision.contents_hash,
          revision.file_count,
          std::string(revision.decomp_config_version),
          revision.territory,
          revision.black_label};
}

bool is_default_preparable_revision(const jak1_iso::Revision& revision) {
  const auto& expected = jak1_iso::default_revision();
  return revision.serial == expected.serial && revision.elf_hash == expected.elf_hash &&
         revision.contents_hash == expected.contents_hash &&
         revision.file_count == expected.file_count &&
         revision.decomp_config_version == expected.decomp_config_version &&
         revision.territory == expected.territory && revision.black_label == expected.black_label;
}

std::string unsupported_revision_message(const jak1_iso::Revision& revision) {
  return "The validated disc is " + std::string(revision.canonical_name) + " (" +
         std::string(revision.serial) + ", config " +
         std::string(revision.decomp_config_version) +
         "), but on-device preparation currently supports only SCUS-97124 ntsc_v1 Black Label.";
}

Error callback_aware_error(CallbackForwarder& callbacks,
                           ErrorCode code,
                           std::string message,
                           bool underlying_cancelled) {
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }
  return underlying_cancelled
             ? callbacks.cancellation_or_callback_error(std::move(message))
             : make_error(code, std::move(message));
}

Result<std::vector<std::uint8_t>> read_bounded_regular_file(const fs::path& path,
                                                            std::uint64_t byte_limit,
                                                            CallbackForwarder& callbacks,
                                                            ErrorCode error_code,
                                                            std::string_view description) {
  if (!direct_regular_file(path)) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " is missing or not a direct file."));
  }
  std::error_code error;
  const auto file_size = fs::file_size(path, error);
  if (error || file_size == 0 || file_size > byte_limit ||
      file_size > std::numeric_limits<std::size_t>::max() ||
      file_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " exceeds its bounded input size."));
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " could not be opened."));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (callbacks.poll_cancel()) {
      return Result<std::vector<std::uint8_t>>::failure(
          callbacks.cancellation_or_callback_error("Jak 1 import was cancelled while reading " +
                                                   std::string(description) + "."));
    }
    const auto chunk = std::min(kIoChunkBytes, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset),
               static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(error_code, std::string(description) + " could not be read exactly."));
    }
    offset += chunk;
  }
  char extra = 0;
  if (input.get(extra)) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(error_code, std::string(description) + " changed while it was read."));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::optional<Error> remove_partial_residue(const fs::path& work_root);
Error preserve_error(Error error, const internal::WorkPaths& paths);
std::optional<Error> promote_directory(const fs::path& source, const fs::path& destination);
std::optional<Error> validate_successful_candidate(const internal::WorkPaths& paths);
std::optional<Error> write_file_atomically(const fs::path& destination,
                                           std::span<const std::uint8_t> bytes,
                                           CallbackForwarder& callbacks);
std::optional<Error> persist_generated_artifacts(const artifacts::Build& build,
                                                 const fs::path& root,
                                                 PipelineState* state,
                                                 CallbackForwarder& callbacks);

}  // namespace

namespace {

std::optional<Error> generate_data_stage(const Request&,
                                         const internal::WorkPaths&,
                                         const Options& options,
                                         PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::generated_data_failed,
                      "The generated-data stage has no validated extraction.");
  }
  CallbackForwarder callbacks(options);
  jak1_extracted_generated_inputs::Options input_options;
  input_options.subtitle_mode = artifacts::SubtitleMode::empty;
  input_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  input_options.on_progress = [&](const jak1_extracted_generated_inputs::Progress& progress) {
    callbacks.report({Phase::generating_data, progress.units_completed, progress.units_total, 0,
                      progress.source_relative_path});
  };
  auto inputs = jak1_extracted_generated_inputs::build(
      {state->extraction->staging_directory, state->extraction->match.revision}, {},
      input_options);
  if (!inputs) {
    const bool cancelled =
        inputs.error().code == jak1_extracted_generated_inputs::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::generated_data_failed,
        cancelled ? "Jak 1 generated-data loading was cancelled."
                  : "Could not load checked generated-data inputs: " + inputs.error().message,
        cancelled);
  }

  artifacts::Options artifact_options;
  artifact_options.subtitle_mode = artifacts::SubtitleMode::empty;
  artifact_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  auto built = artifacts::build(state->graph, inputs.value(), artifact_options);
  if (!built) {
    const bool cancelled = built.error().code == artifacts::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::generated_data_failed,
        cancelled ? "Jak 1 generated-data building was cancelled."
                  : "Could not build checked generated data: " + built.error().message,
        cancelled);
  }
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }
  state->generated_artifacts.emplace(built.take_value());
  return {};
}

std::optional<Error> prepare_fr3_stage(const Request& request,
                                       const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::fr3_failed,
                      "The FR3 stage has no validated extraction.");
  }
  CallbackForwarder callbacks(options);
  jak1_fr3::Options fr3_options;
  fr3_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  fr3_options.report_progress = [&](const jak1_fr3::Progress& progress) {
    callbacks.report({Phase::preparing_fr3, progress.completed, progress.total, 0,
                      progress.current_item});
  };
  const auto fr3_work = paths.work_root / "fr3-work";
  auto prepared = jak1_fr3::prepare(request.project_resource_root,
                                    state->extraction->staging_directory, fr3_work,
                                    state->extraction->match.revision, fr3_options);
  if (!prepared) {
    const bool cancelled = prepared.error().code == jak1_fr3::ErrorCode::cancelled;
    const bool insufficient = prepared.error().code == jak1_fr3::ErrorCode::output_limit_exceeded &&
                              prepared.error().message.find("less free space") != std::string::npos;
    return callback_aware_error(
        callbacks, insufficient ? ErrorCode::insufficient_storage : ErrorCode::fr3_failed,
        cancelled ? "Jak 1 FR3 preparation was cancelled."
                  : "Jak 1 FR3 preparation failed: " + prepared.error().message,
        cancelled);
  }
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }

  const auto fr3_root = fr3_work / "fr3";
  if (!direct_directory(fr3_root)) {
    return make_error(ErrorCode::fr3_failed,
                      "FR3 preparation did not produce its checked output directory.");
  }
  std::set<std::string> basenames;
  std::error_code error;
  for (fs::directory_iterator iterator(fr3_root, error), end; iterator != end;
       iterator.increment(error)) {
    if (error) {
      break;
    }
    const auto status = iterator->symlink_status(error);
    const auto basename = iterator->path().filename().string();
    if (error || status.type() != fs::file_type::regular || basename.empty() ||
        !basename.ends_with(".fr3") || !basenames.emplace(basename).second) {
      return make_error(ErrorCode::fr3_failed,
                        "FR3 preparation returned an unsafe or duplicate output file.");
    }
  }
  if (error || basenames.empty() || basenames.size() != prepared.value().levels_written) {
    return make_error(ErrorCode::fr3_failed,
                      "FR3 preparation returned an incomplete output set.");
  }
  state->expected_fr3_basenames.assign(basenames.begin(), basenames.end());
  return {};
}

std::optional<Error> generate_recipe_stage(const Request&,
                                           const internal::WorkPaths& paths,
                                           const Options& options,
                                           PipelineState* state) {
  if (!state->extraction || !state->generated_artifacts ||
      state->source_pack_manifest.empty() || state->expected_fr3_basenames.empty()) {
    return make_error(ErrorCode::recipe_failed,
                      "The output-recipe stage is missing a checked input.");
  }
  CallbackForwarder callbacks(options);
  generator::VerifiedInputs inputs;
  inputs.revision = recipe_revision(state->extraction->match.revision);
  inputs.extracted_iso_root = kGraphIsoRoot;
  inputs.verified_extracted_iso_relative_paths = state->verified_flat_paths;
  inputs.retail_catalog = state->retail_objects;
  inputs.expected_fr3_basenames = state->expected_fr3_basenames;

  generator::Options generator_options;
  generator_options.output_profile = recipe::OutputProfile::jak1_base_retail;
  generator_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  auto output = generator::generate_from_graph(state->graph, state->source_pack_manifest, inputs,
                                                generator_options);
  if (!output) {
    const bool cancelled = output.error().code == generator::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::recipe_failed,
        cancelled ? "Jak 1 output-recipe generation was cancelled."
                  : "Could not generate the checked base-retail recipe: " +
                        output.error().message,
        cancelled);
  }

  recipe::Options recipe_options;
  recipe_options.expected_revision = inputs.revision;
  recipe_options.expected_source_object_pack = state->source_pack_summary.identity;
  recipe_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  auto wire = recipe::encode(output.value(), recipe_options);
  if (!wire) {
    const bool cancelled = wire.error().code == recipe::ErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::recipe_failed,
        cancelled ? "Jak 1 output-recipe encoding was cancelled."
                  : "Could not encode the checked base-retail recipe: " + wire.error().message,
        cancelled);
  }
  if (const auto write_error =
          write_file_atomically(paths.work_root / kRecipeFileName, wire.value(), callbacks)) {
    return write_error;
  }
  if (const auto artifact_error =
          persist_generated_artifacts(*state->generated_artifacts,
                                      paths.work_root / "generated", state, callbacks)) {
    return artifact_error;
  }
  state->generated_artifacts.reset();
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }
  state->output_recipe.emplace(output.take_value());
  return {};
}

std::optional<Error> materialize_stage(const Request& request,
                                       const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  if (!state->extraction || !state->output_recipe) {
    return make_error(ErrorCode::materialization_failed,
                      "The materializer stage is missing a checked recipe or extraction.");
  }
  materializer::Inputs inputs;
  inputs.recipe_file = paths.work_root / kRecipeFileName;
  inputs.source_object_pack_root = request.source_object_pack_root;
  inputs.extracted_iso_root = state->extraction->staging_directory;
  inputs.generated_artifact_root = paths.work_root / "generated";
  inputs.prepared_fr3_root = paths.work_root / "fr3-work/fr3";
  inputs.generated_objects = state->generated_objects;
  inputs.generated_flat_files = state->generated_flat_files;

  CallbackForwarder callbacks(options);
  materializer::Options materializer_options;
  materializer_options.expected_revision = recipe_revision(state->extraction->match.revision);
  materializer_options.expected_source_object_pack = state->source_pack_summary.identity;
  materializer_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  materializer_options.on_progress = [&](const materializer::Progress& progress) {
    callbacks.report({Phase::materializing_output, progress.completed, progress.total,
                      progress.bytes_written, progress.current_item});
  };
  auto materialized = materializer::materialize(inputs, paths.prepared_root, materializer_options);
  if (!materialized) {
    const bool cancelled = materialized.error().code == materializer::ErrorCode::cancelled;
    const bool insufficient =
        materialized.error().code == materializer::ErrorCode::output_limit_exceeded;
    return callback_aware_error(
        callbacks, insufficient ? ErrorCode::insufficient_storage
                                : ErrorCode::materialization_failed,
        cancelled ? "Jak 1 output materialization was cancelled."
                  : "Could not materialize the checked base-retail output: " +
                        materialized.error().message,
        cancelled);
  }
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }
  state->summary = Summary{materialized.value().archives_written,
                           materialized.value().objects_written,
                           materialized.value().flat_files_written,
                           materialized.value().fr3_files_written,
                           materialized.value().output_bytes};
  return {};
}

}  // namespace

namespace internal {

Result<Summary> compose_in_fresh_candidate(const fs::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages,
                                           const std::optional<Summary>* produced_summary) {
  WorkPaths paths{candidate_root, candidate_root / kWorkDirectoryName,
                  candidate_root / kWorkDirectoryName / kPreparedDirectoryName};
  bool candidate_created = false;
  try {
    if (!candidate_root.is_absolute() || candidate_root.filename().empty() ||
        !direct_directory(candidate_root.parent_path()) || !missing_path(candidate_root) ||
        stages.empty() || !produced_summary) {
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
                "Jak 1 import was cancelled before the next preparation stage."),
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

    if (!produced_summary->has_value() || !direct_directory(paths.prepared_root / "iso") ||
        !direct_directory(paths.prepared_root / "fr3")) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The composition stages did not produce a complete prepared tree."),
          paths));
    }
    std::set<std::string> prepared_entries;
    for (fs::directory_iterator iterator(paths.prepared_root, error), end; iterator != end;
         iterator.increment(error)) {
      if (error) {
        break;
      }
      prepared_entries.emplace(iterator->path().filename().string());
    }
    if (error || prepared_entries != std::set<std::string>{"fr3", "iso"}) {
      return Result<Summary>::failure(preserve_error(
          make_error(ErrorCode::candidate_finalize_failed,
                     "The prepared tree contains unexpected entries."),
          paths));
    }

    if (callbacks.poll_cancel()) {
      return Result<Summary>::failure(preserve_error(
          callbacks.cancellation_or_callback_error(
              "Jak 1 import was cancelled before candidate finalization."),
          paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 0, 2, 0, "iso"})) {
      return Result<Summary>::failure(
          preserve_error(callbacks.cancellation_or_callback_error({}), paths));
    }
    if (const auto promote_error =
            promote_directory(paths.prepared_root / "iso", candidate_root / "iso")) {
      return Result<Summary>::failure(preserve_error(*promote_error, paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 1, 2, 0, "fr3"})) {
      return Result<Summary>::failure(
          preserve_error(callbacks.cancellation_or_callback_error({}), paths));
    }
    if (const auto promote_error =
            promote_directory(paths.prepared_root / "fr3", candidate_root / "fr3")) {
      return Result<Summary>::failure(preserve_error(*promote_error, paths));
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
    if (const auto validation_error = validate_successful_candidate(paths)) {
      return Result<Summary>::failure(preserve_error(*validation_error, paths));
    }
    if (!callbacks.report({Phase::finalizing_candidate, 2, 2,
                           produced_summary->value().output_bytes, {}})) {
      auto callback_error = callbacks.cancellation_or_callback_error({});
      callback_error.preserved_candidate_root = candidate_root;
      return Result<Summary>::failure(std::move(callback_error));
    }
    return Result<Summary>::success(produced_summary->value());
  } catch (const std::bad_alloc&) {
    auto error = make_error(ErrorCode::allocation_failed,
                            "Jak 1 import composition ran out of memory.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (const std::exception& exception) {
    auto error = make_error(ErrorCode::unexpected_failure,
                            "Jak 1 import composition failed unexpectedly: " +
                                std::string(exception.what()));
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (...) {
    auto error = make_error(ErrorCode::unexpected_failure,
                            "Jak 1 import composition failed unexpectedly.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  }
}

}  // namespace internal

namespace {

class OwnedPartialFile {
 public:
  explicit OwnedPartialFile(fs::path path) : m_path(std::move(path)) {}
  OwnedPartialFile(const OwnedPartialFile&) = delete;
  OwnedPartialFile& operator=(const OwnedPartialFile&) = delete;

  ~OwnedPartialFile() {
    if (m_descriptor >= 0) {
      ::close(m_descriptor);
    }
    if (m_exists) {
      std::error_code ignored;
      fs::remove(m_path, ignored);
    }
  }

  int descriptor() const { return m_descriptor; }
  void set_descriptor(int descriptor) { m_descriptor = descriptor; }

  std::optional<std::string> close_checked() {
    if (m_descriptor < 0) {
      return {};
    }
    const auto descriptor = m_descriptor;
    m_descriptor = -1;
    if (::close(descriptor) != 0) {
      return "Could not close an import partial file: " +
             std::error_code(errno, std::generic_category()).message();
    }
    return {};
  }

  void mark_exists() { m_exists = true; }
  void release() { m_exists = false; }

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
  if (!missing_path(partial)) {
    return make_error(ErrorCode::work_write_failed,
                      "An import work-file partial path already exists.");
  }

  OwnedPartialFile file(partial);
  const auto descriptor = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    const std::error_code error(errno, std::generic_category());
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not create an exclusive import partial file", error);
  }
  file.set_descriptor(descriptor);
  file.mark_exists();

  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (callbacks.poll_cancel()) {
      return callbacks.cancellation_or_callback_error(
          "Jak 1 import was cancelled while writing generated data.");
    }
    const auto chunk = std::min(kIoChunkBytes, bytes.size() - offset);
    const auto written = ::write(descriptor, bytes.data() + offset, chunk);
    if (written <= 0) {
      const std::error_code error(errno, std::generic_category());
      return make_filesystem_error(ErrorCode::work_write_failed,
                                   "Could not write a complete import work file", error);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    const std::error_code error(errno, std::generic_category());
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not synchronize an import work file", error);
  }
  if (const auto close_error = file.close_checked()) {
    return make_error(ErrorCode::work_write_failed, *close_error);
  }

  std::error_code rename_error;
  fs::rename(partial, destination, rename_error);
  if (rename_error) {
    return make_filesystem_error(ErrorCode::work_write_failed,
                                 "Could not install an import work file", rename_error);
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
                                 "Could not create the generated-artifact work root", error);
  }
  for (const auto& artifact : build.artifacts) {
    const fs::path relative(artifact.relative_path);
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
            write_file_atomically(root / relative, artifact.bytes, callbacks)) {
      return write_error;
    }
    if (artifact.storage == artifacts::ArtifactStorage::object) {
      if (!artifact.object_kind || artifact.flat_file_kind) {
        return make_error(ErrorCode::generated_data_failed,
                          "A generated object has inconsistent storage metadata.");
      }
      state->generated_objects.push_back({*artifact.object_kind,
                                          artifact.internal_name,
                                          artifact.relative_path,
                                          artifact.bytes.size(),
                                          artifact.xxh64});
    } else {
      if (!artifact.flat_file_kind || artifact.object_kind) {
        return make_error(ErrorCode::generated_data_failed,
                          "A generated flat file has inconsistent storage metadata.");
      }
      state->generated_flat_files.push_back({*artifact.flat_file_kind,
                                             artifact.destination_basename,
                                             artifact.relative_path,
                                             artifact.bytes.size(),
                                             artifact.xxh64});
    }
  }
  return {};
}

std::optional<Error> remove_partial_residue(const fs::path& work_root) {
  std::error_code error;
  if (!direct_directory(work_root)) {
    return {};
  }
  fs::recursive_directory_iterator iterator(work_root, error);
  const fs::recursive_directory_iterator end;
  if (error) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "Could not inspect the preserved candidate for partial files.");
  }
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "The preserved candidate changed while partial files were removed.");
    }
    const auto name = iterator->path().filename().string();
    if (!name.ends_with(".partial")) {
      continue;
    }
    const auto status = iterator->symlink_status(error);
    if (error || status.type() != fs::file_type::regular ||
        !fs::remove(iterator->path(), error) || error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "Could not remove an import partial file from the preserved candidate.");
    }
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
                      "A prepared output directory is missing or its destination already exists.");
  }
#if defined(__APPLE__)
  if (::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) != 0) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "Could not exclusively promote a prepared output directory: " +
                          std::error_code(errno, std::generic_category()).message());
  }
#else
  std::error_code error;
  fs::rename(source, destination, error);
  if (error) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "Could not promote a prepared output directory: " + error.message());
  }
#endif
  return {};
}

std::optional<Error> validate_successful_candidate(const internal::WorkPaths& paths) {
  if (!direct_directory(paths.candidate_root / "iso") ||
      !direct_directory(paths.candidate_root / "fr3")) {
    return make_error(ErrorCode::candidate_finalize_failed,
                      "The finalized candidate is missing its iso or fr3 directory.");
  }
  std::set<std::string> entries;
  std::error_code error;
  for (fs::directory_iterator iterator(paths.candidate_root, error), end; iterator != end;
       iterator.increment(error)) {
    if (error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "The candidate changed during final inspection.");
    }
    entries.emplace(iterator->path().filename().string());
  }
  if (error || entries != std::set<std::string>{"fr3", "iso"}) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "A successful candidate contains unexpected work entries.");
  }
  return {};
}

}  // namespace

namespace {

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
  if (std::any_of(paths.begin(), paths.end(), [](const auto* path) {
        return !path->is_absolute() || path->empty();
      }) ||
      request.candidate_root.filename().empty() ||
      !direct_directory(request.source_object_pack_root) ||
      !direct_directory(request.project_resource_root) ||
      !direct_directory(request.candidate_root.parent_path()) ||
      !missing_path(request.candidate_root)) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The ISO, verified source pack, project resources, or fresh candidate path is invalid."));
  }

  Request resolved = request;
  std::error_code error;
  resolved.source_object_pack_root = fs::canonical(request.source_object_pack_root, error);
  if (error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The source-object-pack root could not be canonicalized."));
  }
  resolved.project_resource_root = fs::canonical(request.project_resource_root, error);
  if (error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The project-resource root could not be canonicalized."));
  }
  const auto candidate_parent = fs::canonical(request.candidate_root.parent_path(), error);
  if (error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The import candidate parent could not be canonicalized."));
  }
  resolved.candidate_root = candidate_parent / request.candidate_root.filename();
  if (!missing_path(resolved.candidate_root)) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The canonical import candidate already exists."));
  }

  std::error_code containment_error;
  const bool inside_source_pack = directory_contains(
      resolved.source_object_pack_root, candidate_parent, &containment_error);
  if (containment_error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The import candidate containment check could not be completed."));
  }
  const bool inside_project_resources = directory_contains(
      resolved.project_resource_root, candidate_parent, &containment_error);
  if (containment_error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The import candidate containment check could not be completed."));
  }
  if (inside_source_pack || inside_project_resources) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument,
        "The import candidate cannot be created inside an input resource directory."));
  }
  return Result<Request>::success(std::move(resolved));
}

std::optional<Error> extract_iso_stage(const Request&,
                                       const internal::WorkPaths& paths,
                                       const Options& options,
                                       PipelineState* state) {
  CallbackForwarder callbacks(options);
  iso_file::Options iso_options;
  iso_options.should_cancel = [&] { return callbacks.poll_cancel(); };
  iso_options.on_progress = [&](const iso_file::Progress& progress) {
    callbacks.report({Phase::extracting_iso, progress.files_completed, progress.files_total,
                      progress.bytes_completed, progress.current_path});
  };
  auto extracted = jak1_iso::extract_and_validate(
      state->iso_file.get(), paths.work_root / "extracted-iso", iso_options);
  if (!extracted) {
    const bool cancelled =
        extracted.error().code == jak1_iso::ValidationErrorCode::cancelled;
    return callback_aware_error(
        callbacks, ErrorCode::iso_validation_failed,
        cancelled ? "Jak 1 ISO extraction was cancelled."
                  : "The selected ISO was rejected: " + extracted.error().message,
        cancelled);
  }
  if (callbacks.callback_failed) {
    return callbacks.cancellation_or_callback_error({});
  }
  auto extraction = extracted.take_value();
  if (!is_default_preparable_revision(extraction.match.revision)) {
    return make_error(ErrorCode::unsupported_revision,
                      unsupported_revision_message(extraction.match.revision));
  }
  state->extraction.emplace(std::move(extraction));
  return {};
}

Result<fs::path> resolve_extracted_regular_file(const fs::path& root,
                                                const std::string& relative_path,
                                                ErrorCode error_code,
                                                std::string_view description) {
  const fs::path relative(relative_path);
  if (!safe_relative_path(relative)) {
    return Result<fs::path>::failure(
        make_error(error_code, std::string(description) + " has an unsafe relative path."));
  }
  const auto selected = root / relative;
  if (!direct_regular_file(selected)) {
    return Result<fs::path>::failure(
        make_error(error_code, std::string(description) + " is missing or linked."));
  }
  return Result<fs::path>::success(selected);
}

Result<std::vector<std::string>> selected_archive_paths(const generator::Graph& graph) {
  std::set<std::string> paths;
  std::unordered_set<std::string> collision_keys;
  for (const auto& archive : graph.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer != generator::ObjectProducerKind::verified_retail) {
        continue;
      }
      const auto& path = object.retail_source_archive;
      const auto separator = path.find('/');
      if (separator == std::string::npos || path.find('/', separator + 1) != std::string::npos) {
        return Result<std::vector<std::string>>::failure(make_error(
            ErrorCode::graph_failed, "The base-retail graph contains an unsafe archive path."));
      }
      const auto directory = std::string_view(path).substr(0, separator);
      const auto basename = std::string_view(path).substr(separator + 1);
      if (!((directory == "DGO" && safe_archive_basename(basename, ".DGO")) ||
            (directory == "CGO" && safe_archive_basename(basename, ".CGO")))) {
        return Result<std::vector<std::string>>::failure(make_error(
            ErrorCode::graph_failed, "The base-retail graph contains an unsafe archive name."));
      }
      auto collision = path;
      std::transform(collision.begin(), collision.end(), collision.begin(),
                     [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
      if (paths.emplace(path).second && !collision_keys.emplace(std::move(collision)).second) {
        return Result<std::vector<std::string>>::failure(make_error(
            ErrorCode::graph_failed, "The base-retail graph contains case-colliding archives."));
      }
    }
  }
  if (paths.empty()) {
    return Result<std::vector<std::string>>::failure(
        make_error(ErrorCode::graph_failed, "The base-retail graph selects no retail archives."));
  }
  return Result<std::vector<std::string>>::success({paths.begin(), paths.end()});
}

std::optional<Error> adapt_flat_paths(const fs::path& extracted_root, PipelineState* state) {
  std::set<std::string> verified;
  const fs::path graph_root(kGraphIsoRoot);
  for (const auto& copy : state->graph.flat_file_copies) {
    const auto source = fs::path(copy.source_path).lexically_normal();
    const auto relative = source.lexically_relative(graph_root);
    if (!safe_relative_path(relative)) {
      return make_error(ErrorCode::graph_failed,
                        "A base-retail flat-file source escapes the extracted ISO root.");
    }
    const auto resolved = resolve_extracted_regular_file(
        extracted_root, relative.generic_string(), ErrorCode::retail_catalog_failed,
        "A selected retail flat file");
    if (!resolved) {
      return resolved.error();
    }
    verified.emplace(relative.generic_string());
  }
  state->verified_flat_paths.assign(verified.begin(), verified.end());
  return {};
}

std::optional<Error> catalog_retail_stage(const Request& request,
                                          const internal::WorkPaths&,
                                          const Options& options,
                                          PipelineState* state) {
  if (!state->extraction) {
    return make_error(ErrorCode::retail_catalog_failed,
                      "The retail catalog stage has no validated extraction.");
  }
  CallbackForwarder callbacks(options);
  auto manifest = read_bounded_regular_file(request.source_object_pack_root / source_pack::kManifestName,
                                            kMaxManifestBytes, callbacks,
                                            ErrorCode::source_pack_failed,
                                            "The source-object-pack manifest");
  if (!manifest) {
    return manifest.error();
  }
  state->source_pack_manifest.assign(
      reinterpret_cast<const char*>(manifest.value().data()), manifest.value().size());
  auto parsed_manifest = generator::parse_source_object_pack_manifest(state->source_pack_manifest);
  if (!parsed_manifest ||
      parsed_manifest.value().identity != state->source_pack_summary.identity) {
    return make_error(ErrorCode::source_pack_failed,
                      "The source-object-pack manifest changed after validation.");
  }

  const auto extracted_root = state->extraction->staging_directory;
  if (const auto flat_error = adapt_flat_paths(extracted_root, state)) {
    return flat_error;
  }
  auto paths = selected_archive_paths(state->graph);
  if (!paths) {
    return paths.error();
  }

  state->retail_objects.clear();
  std::uint64_t total_bytes = 0;
  std::uint64_t total_object_bytes = 0;
  for (std::size_t index = 0; index < paths.value().size(); ++index) {
    if (callbacks.poll_cancel()) {
      return callbacks.cancellation_or_callback_error(
          "Jak 1 import was cancelled while loading retail archives.");
    }
    const auto& relative_path = paths.value()[index];
    auto resolved = resolve_extracted_regular_file(extracted_root, relative_path,
                                                   ErrorCode::retail_catalog_failed,
                                                   "A selected retail archive");
    if (!resolved) {
      return resolved.error();
    }
    auto bytes = read_bounded_regular_file(resolved.value(), kMaxArchiveBytes, callbacks,
                                           ErrorCode::retail_catalog_failed,
                                           "A selected retail archive");
    if (!bytes) {
      return bytes.error();
    }
    if (bytes.value().size() > kMaxTotalArchiveBytes - total_bytes) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The selected retail archives exceed the aggregate input limit.");
    }
    total_bytes += bytes.value().size();
    const retail_catalog::ArchiveSource source{relative_path, bytes.value()};
    retail_catalog::Options catalog_options;
    catalog_options.max_total_object_bytes = kMaxArchiveBytes;
    catalog_options.max_archive_input_bytes = kMaxArchiveBytes;
    catalog_options.max_archive_compressed_bytes = kMaxArchiveBytes;
    catalog_options.max_archive_expanded_bytes = kMaxArchiveBytes;
    catalog_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    auto catalog = retail_catalog::build(
        std::span<const retail_catalog::ArchiveSource>(&source, 1), catalog_options);
    if (!catalog) {
      const bool cancelled = catalog.error().code == retail_catalog::ErrorCode::cancelled;
      return callback_aware_error(
          callbacks, ErrorCode::retail_catalog_failed,
          cancelled ? "Jak 1 retail cataloging was cancelled."
                    : "The checked retail catalog failed: " + catalog.error().message,
          cancelled);
    }
    if (state->retail_objects.size() > kMaxCatalogEntries ||
        catalog.value().entries().size() > kMaxCatalogEntries - state->retail_objects.size()) {
      return make_error(ErrorCode::retail_catalog_failed,
                        "The retail provenance catalog exceeds its entry limit.");
    }
    for (const auto& entry : catalog.value().entries()) {
      const auto& provenance = entry.provenance;
      if (provenance.byte_size > kMaxCatalogObjectBytes - total_object_bytes) {
        return make_error(ErrorCode::retail_catalog_failed,
                          "The retail provenance catalog exceeds its object-byte limit.");
      }
      total_object_bytes += provenance.byte_size;
      state->retail_objects.push_back({provenance.source_archive_relative_path,
                                       provenance.archive_object_index,
                                       provenance.internal_name,
                                       provenance.unique_name,
                                       static_cast<std::uint32_t>(provenance.object_version),
                                       static_cast<std::uint64_t>(provenance.byte_size),
                                       provenance.xxh64});
    }
    if (!callbacks.report({Phase::cataloging_retail, index + 1, paths.value().size(), total_bytes,
                           relative_path})) {
      return callbacks.cancellation_or_callback_error({});
    }
  }
  return callbacks.callback_failed ? std::optional<Error>(callbacks.cancellation_or_callback_error({}))
                                   : std::nullopt;
}

}  // namespace

Result<Summary> compose(const Request& request, const Options& options) {
  try {
    auto validated_request = validate_request(request);
    if (!validated_request) {
      return Result<Summary>::failure(validated_request.error());
    }
    const auto resolved_request = validated_request.take_value();
    auto iso = open_iso_file(resolved_request.iso_path);
    if (!iso) {
      return Result<Summary>::failure(iso.error());
    }

    CallbackForwarder callbacks(options);
    if (callbacks.poll_cancel()) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error(
          "Jak 1 import was cancelled before source-pack validation."));
    }
    if (!callbacks.report({Phase::validating_source_pack, 0, source_pack::kExpectedObjectCount,
                           0, source_pack::kManifestName})) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error({}));
    }
    source_pack::Options source_options;
    source_options.expected_identity = recipe::SourceObjectPackIdentity{
        source_pack::kExpectedObjectCount, source_pack::kRecordedAggregateXXH64};
    source_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    source_options.on_progress = [&](const source_pack::Progress& progress) {
      callbacks.report({Phase::validating_source_pack, progress.completed, progress.total,
                        progress.bytes_hashed, progress.current_file});
    };
    auto verified_pack =
        source_pack::validate(resolved_request.source_object_pack_root, source_options);
    if (!verified_pack) {
      const bool cancelled = verified_pack.error().code == source_pack::ErrorCode::cancelled;
      return Result<Summary>::failure(callback_aware_error(
          callbacks, ErrorCode::source_pack_failed,
          cancelled ? "Jak 1 source-pack validation was cancelled."
                    : "The existing Jak 1 source-object pack was rejected: " +
                          verified_pack.error().message,
          cancelled));
    }
    if (callbacks.callback_failed) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error({}));
    }

    jak1_output_graph::Options graph_options;
    graph_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    auto graph = jak1_public_output_graph::decode_base_retail(graph_options);
    if (!graph) {
      const bool cancelled = graph.error().code == jak1_output_graph::ErrorCode::cancelled;
      return Result<Summary>::failure(callback_aware_error(
          callbacks, ErrorCode::graph_failed,
          cancelled ? "Jak 1 output-graph loading was cancelled."
                    : "The embedded Jak 1 base-retail graph was rejected: " +
                          graph.error().message,
          cancelled));
    }

    PipelineState state;
    state.graph = graph.take_value();
    state.source_pack_summary = verified_pack.take_value();
    state.iso_file = iso.take_value();
    const std::array<internal::StageAction, 6> stages = {{
        {Phase::extracting_iso,
         [&](const internal::WorkPaths& paths) {
           return extract_iso_stage(resolved_request, paths, options, &state);
         }},
        {Phase::cataloging_retail,
         [&](const internal::WorkPaths& paths) {
           return catalog_retail_stage(resolved_request, paths, options, &state);
         }},
        {Phase::generating_data,
         [&](const internal::WorkPaths& paths) {
           return generate_data_stage(resolved_request, paths, options, &state);
         }},
        {Phase::preparing_fr3,
         [&](const internal::WorkPaths& paths) {
           return prepare_fr3_stage(resolved_request, paths, options, &state);
         }},
        {Phase::generating_recipe,
         [&](const internal::WorkPaths& paths) {
           return generate_recipe_stage(resolved_request, paths, options, &state);
         }},
        {Phase::materializing_output,
         [&](const internal::WorkPaths& paths) {
           return materialize_stage(resolved_request, paths, options, &state);
         }},
    }};
    return internal::compose_in_fresh_candidate(resolved_request.candidate_root, options, stages,
                                                &state.summary);
  } catch (const std::bad_alloc&) {
    return Result<Summary>::failure(
        make_error(ErrorCode::allocation_failed, "Jak 1 import composition ran out of memory."));
  } catch (const std::exception& exception) {
    return Result<Summary>::failure(make_error(
        ErrorCode::unexpected_failure,
        "Jak 1 import composition failed unexpectedly: " + std::string(exception.what())));
  } catch (...) {
    return Result<Summary>::failure(make_error(
        ErrorCode::unexpected_failure, "Jak 1 import composition failed unexpectedly."));
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
    case ErrorCode::source_pack_failed:
      return "source_pack_failed";
    case ErrorCode::iso_open_failed:
      return "iso_open_failed";
    case ErrorCode::iso_validation_failed:
      return "iso_validation_failed";
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::graph_failed:
      return "graph_failed";
    case ErrorCode::retail_catalog_failed:
      return "retail_catalog_failed";
    case ErrorCode::generated_data_failed:
      return "generated_data_failed";
    case ErrorCode::fr3_failed:
      return "fr3_failed";
    case ErrorCode::recipe_failed:
      return "recipe_failed";
    case ErrorCode::candidate_create_failed:
      return "candidate_create_failed";
    case ErrorCode::work_write_failed:
      return "work_write_failed";
    case ErrorCode::insufficient_storage:
      return "insufficient_storage";
    case ErrorCode::materialization_failed:
      return "materialization_failed";
    case ErrorCode::candidate_finalize_failed:
      return "candidate_finalize_failed";
    case ErrorCode::candidate_cleanup_failed:
      return "candidate_cleanup_failed";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::unexpected_failure:
      return "unexpected_failure";
  }
  return "unknown";
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::validating_source_pack:
      return "validating_source_pack";
    case Phase::extracting_iso:
      return "extracting_iso";
    case Phase::cataloging_retail:
      return "cataloging_retail";
    case Phase::generating_data:
      return "generating_data";
    case Phase::preparing_fr3:
      return "preparing_fr3";
    case Phase::generating_recipe:
      return "generating_recipe";
    case Phase::materializing_output:
      return "materializing_output";
    case Phase::finalizing_candidate:
      return "finalizing_candidate";
  }
  return "unknown";
}

}  // namespace jak1_import_composer
