#include "jak2_extracted_generated_inputs.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string_view>

#include "common/custom_data/Jak2PublicOutputGraph.h"

namespace jak2_extracted_generated_inputs {
namespace {

namespace graph = jak1_output_graph;
namespace recipe = jak1_output_recipe;

constexpr std::string_view kGameArchivePath = "CGO/GAME.CGO";
constexpr std::string_view kGameArchiveName = "GAME.CGO";
constexpr std::string_view kDirectoryInternalName = "dir-tpages";
constexpr std::string_view kDirectoryDestination = "dir-tpages.go";
constexpr std::string_view kPublicSubtitleDestination = "0SUBTI2.TXT";
constexpr std::string_view kPublicSubtitleProject = "game/assets/jak2/game_subtitle.gp";
constexpr std::string_view kPublicSubtitleTool = "subtitle-v2";
constexpr std::uint32_t kRetailLanguageCount = 8;
constexpr std::uint64_t kProvenRevisionContentsHash = 18208811100399420450ull;
constexpr std::size_t kProgressUnits = 10;

Error make_error(ErrorCode code,
                 std::string message,
                 std::string source_path = {},
                 std::optional<std::uint32_t> object_index = {},
                 std::optional<std::uint32_t> language_id = {},
                 std::optional<std::size_t> byte_offset = {}) {
  return {code,        std::move(message), std::move(source_path), object_index, language_id,
          byte_offset, std::nullopt};
}

enum class CallbackState {
  continue_work,
  cancelled,
  failed,
};

CallbackState poll_cancel(const Options& options) {
  if (!options.should_cancel) {
    return CallbackState::continue_work;
  }
  try {
    return options.should_cancel() ? CallbackState::cancelled : CallbackState::continue_work;
  } catch (...) {
    return CallbackState::failed;
  }
}

std::optional<Error> cancellation_error(const Options& options,
                                        const std::string& source_path = {},
                                        std::optional<std::uint32_t> language_id = {}) {
  const auto state = poll_cancel(options);
  if (state == CallbackState::continue_work) {
    return {};
  }
  return make_error(
      state == CallbackState::cancelled ? ErrorCode::cancelled : ErrorCode::callback_failed,
      state == CallbackState::cancelled ? "Jak II generated-input loading was cancelled."
                                        : "The cancellation callback failed.",
      source_path, {}, language_id);
}

std::optional<Error> emit_progress(const Options& options, Progress progress) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress(progress);
    return {};
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The progress callback failed.",
                      progress.source_relative_path, {}, progress.language_id);
  }
}

bool same_revision(const jak2_iso::Revision& left, const jak2_iso::Revision& right) {
  return left.serial == right.serial && left.elf_hash == right.elf_hash &&
         left.canonical_name == right.canonical_name && left.territory == right.territory &&
         left.file_count == right.file_count && left.contents_hash == right.contents_hash &&
         left.decomp_config_version == right.decomp_config_version;
}

bool proven_revision(const jak2_iso::Revision& revision) {
  const auto supported = jak2_iso::supported_revisions();
  const auto expected = std::find_if(supported.begin(), supported.end(), [](const auto& candidate) {
    return candidate.contents_hash == kProvenRevisionContentsHash && candidate.file_count == 593 &&
           candidate.decomp_config_version == "ntsc_v1";
  });
  return expected != supported.end() && same_revision(revision, *expected);
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_game_archive_input_bytes > 0 && limits.max_game_archive_compressed_bytes > 0 &&
         limits.max_game_archive_expanded_bytes > 0 && limits.max_archive_object_bytes > 0 &&
         limits.max_archive_total_object_bytes >= limits.max_archive_object_bytes &&
         limits.max_direct_input_bytes > 0 &&
         limits.max_total_retail_bytes >= limits.max_direct_input_bytes &&
         limits.file_read_chunk_bytes > 0 &&
         limits.file_read_chunk_bytes <=
             static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) &&
         limits.max_archive_objects > 0 && limits.max_archive_expansion_ratio > 0;
}

Result<std::filesystem::path> checked_input_path(const ValidatedTree& tree,
                                                 std::string_view relative_path) {
  const auto fail = [&](ErrorCode code, std::string message) {
    return Result<std::filesystem::path>::failure(
        make_error(code, std::move(message), std::string(relative_path)));
  };
  if (tree.root.empty() || !tree.root.is_absolute() || tree.root != tree.root.lexically_normal()) {
    return fail(ErrorCode::invalid_extracted_tree,
                "The validated extraction root must be an absolute normalized path.");
  }

  const std::filesystem::path relative(relative_path);
  if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal() ||
      std::any_of(relative.begin(), relative.end(),
                  [](const auto& component) { return component == "." || component == ".."; })) {
    return fail(ErrorCode::invalid_argument, "A required input path is not a safe relative path.");
  }

  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(tree.root, error);
  if (error || root_status.type() != std::filesystem::file_type::directory) {
    return fail(ErrorCode::invalid_extracted_tree,
                "The validated extraction root is not a plain directory.");
  }

  auto current = tree.root;
  for (auto component = relative.begin(); component != relative.end(); ++component) {
    current /= *component;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
      return fail(ErrorCode::missing_input, "A required extracted-disc input is missing.");
    }
    const bool final = std::next(component) == relative.end();
    if ((!final && status.type() != std::filesystem::file_type::directory) ||
        (final && status.type() != std::filesystem::file_type::regular)) {
      return fail(ErrorCode::invalid_extracted_tree,
                  "A required extracted-disc path is not a plain file hierarchy.");
    }
  }
  return Result<std::filesystem::path>::success(std::move(current));
}

Result<std::vector<std::uint8_t>> read_direct_input(const ValidatedTree& tree,
                                                    const std::string& relative_path,
                                                    const Options& options,
                                                    std::uint32_t language_id) {
  auto checked_path = checked_input_path(tree, relative_path);
  if (!checked_path) {
    auto error = checked_path.error();
    error.language_id = language_id;
    return Result<std::vector<std::uint8_t>>::failure(std::move(error));
  }

  std::ifstream input(checked_path.value(), std::ios::binary | std::ios::ate);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_open_failed, "Could not open a retail game-text input.",
                   relative_path, {}, language_id));
  }
  const auto end = input.tellg();
  if (end < 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_read_failed, "Could not determine a game-text input size.",
                   relative_path, {}, language_id));
  }
  const auto input_size = static_cast<std::uintmax_t>(end);
  if (input_size == 0 || input_size > options.limits.max_direct_input_bytes ||
      input_size > std::numeric_limits<std::size_t>::max()) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_too_large,
                   "A retail game-text input is empty or exceeds the configured limit.",
                   relative_path, {}, language_id));
  }
  if (const auto error = cancellation_error(options, relative_path, language_id)) {
    return Result<std::vector<std::uint8_t>>::failure(*error);
  }

  input.seekg(0);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_read_failed, "Could not seek to a game-text input start.",
                   relative_path, {}, language_id));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (const auto error = cancellation_error(options, relative_path, language_id)) {
      return Result<std::vector<std::uint8_t>>::failure(*error);
    }
    const auto chunk = std::min(options.limits.file_read_chunk_bytes, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      return Result<std::vector<std::uint8_t>>::failure(make_error(
          ErrorCode::input_read_failed, "Could not read a complete retail game-text input.",
          relative_path, {}, language_id, offset));
    }
    offset += chunk;
  }
  char trailing = 0;
  input.read(&trailing, 1);
  if (input.gcount() != 0) {
    return Result<std::vector<std::uint8_t>>::failure(make_error(
        ErrorCode::input_read_failed, "A retail game-text input changed while it was read.",
        relative_path, {}, language_id, bytes.size()));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

struct GraphContract {
  std::string directory_internal_name;
  std::string directory_destination;
  std::array<std::string, kRetailLanguageCount> game_text_destinations;
  PublicInputRequirement public_subtitle;
};

Result<GraphContract> inspect_graph(const graph::Graph& public_graph) {
  auto graph_options = jak2_public_output_graph::default_options();
  if (!graph::encode(public_graph, graph_options)) {
    return Result<GraphContract>::failure(
        make_error(ErrorCode::invalid_public_graph, "The checked Jak II public graph is invalid."));
  }

  std::optional<std::pair<std::string, std::string>> directory;
  for (const auto& archive : public_graph.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer == graph::ObjectProducerKind::game_count) {
        return Result<GraphContract>::failure(
            make_error(ErrorCode::unsupported_public_graph,
                       "The Jak II public graph unexpectedly requires a game-count object."));
      }
      if (object.producer != graph::ObjectProducerKind::directory_tpages) {
        continue;
      }
      const auto candidate = std::pair(object.internal_name, object.prepared_basename);
      if (directory && *directory != candidate) {
        return Result<GraphContract>::failure(
            make_error(ErrorCode::unsupported_public_graph,
                       "The Jak II public graph has conflicting directory-tpage outputs."));
      }
      directory = candidate;
    }
  }
  if (!directory || directory->first != kDirectoryInternalName ||
      directory->second != kDirectoryDestination) {
    return Result<GraphContract>::failure(
        make_error(ErrorCode::unsupported_public_graph,
                   "The Jak II public graph lacks the proven dir-tpages generated output."));
  }

  std::map<std::string, recipe::GeneratedFlatFileKind> generated;
  for (const auto& flat : public_graph.generated_flat_files) {
    if (!generated.emplace(flat.destination_basename, flat.kind).second) {
      return Result<GraphContract>::failure(make_error(
          ErrorCode::invalid_public_graph, "The Jak II public graph repeats a generated output."));
    }
  }
  if (generated.size() != kRetailLanguageCount + 1) {
    return Result<GraphContract>::failure(
        make_error(ErrorCode::unsupported_public_graph,
                   "The Jak II public graph does not have the proven generated flat-output set."));
  }

  GraphContract contract;
  contract.directory_internal_name = directory->first;
  contract.directory_destination = directory->second;
  for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
    const auto destination = std::to_string(language) + "COMMON.TXT";
    const auto found = generated.find(destination);
    if (found == generated.end() || found->second != recipe::GeneratedFlatFileKind::game_text) {
      return Result<GraphContract>::failure(
          make_error(ErrorCode::unsupported_public_graph,
                     "The Jak II public graph lacks a proven retail game-text output."));
    }
    contract.game_text_destinations[language] = destination;
  }
  const auto subtitle = generated.find(std::string(kPublicSubtitleDestination));
  if (subtitle == generated.end() ||
      subtitle->second != recipe::GeneratedFlatFileKind::game_subtitle) {
    return Result<GraphContract>::failure(
        make_error(ErrorCode::unsupported_public_graph,
                   "The Jak II public graph lacks its public-only subtitle-v2 output."));
  }
  contract.public_subtitle = {
      recipe::GeneratedFlatFileKind::game_subtitle, std::string(kPublicSubtitleDestination),
      "iso/" + std::string(kPublicSubtitleDestination), std::string(kPublicSubtitleProject),
      std::string(kPublicSubtitleTool)};
  return Result<GraphContract>::success(std::move(contract));
}

bool add_retail_bytes(std::uint64_t* total, std::size_t amount, const Options& options) {
  return *total <= options.limits.max_total_retail_bytes &&
         amount <= options.limits.max_total_retail_bytes - *total && (*total += amount, true);
}

}  // namespace

Result<Inputs> build(const ValidatedTree& tree, const Options& options) {
  if (!valid_options(options)) {
    return Result<Inputs>::failure(
        make_error(ErrorCode::invalid_argument, "The generated-input options are invalid."));
  }
  if (const auto error = cancellation_error(options)) {
    return Result<Inputs>::failure(*error);
  }
  auto graph = jak2_public_output_graph::decode_base_retail();
  if (!graph) {
    return Result<Inputs>::failure(make_error(ErrorCode::invalid_public_graph,
                                              "The embedded Jak II base-retail graph is invalid."));
  }
  return build(tree, graph.value(), options);
}

Result<Inputs> build(const ValidatedTree& tree,
                     const graph::Graph& public_graph,
                     const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<Inputs>::failure(
          make_error(ErrorCode::invalid_argument, "The generated-input options are invalid."));
    }
    if (!proven_revision(tree.revision)) {
      return Result<Inputs>::failure(
          make_error(ErrorCode::unsupported_revision,
                     "Only the proven SCUS-97265 Jak II NTSC-U v2 extracted layout is supported."));
    }
    if (const auto error = cancellation_error(options)) {
      return Result<Inputs>::failure(*error);
    }
    if (const auto error = emit_progress(
            options, {ProgressStage::validating_public_graph, 0, kProgressUnits, {}, {}})) {
      return Result<Inputs>::failure(*error);
    }
    auto contract = inspect_graph(public_graph);
    if (!contract) {
      return Result<Inputs>::failure(contract.error());
    }

    if (const auto error = emit_progress(options, {ProgressStage::opening_game_archive,
                                                   0,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<Inputs>::failure(*error);
    }
    auto game_path = checked_input_path(tree, kGameArchivePath);
    if (!game_path) {
      return Result<Inputs>::failure(game_path.error());
    }
    if (const auto error = emit_progress(options, {ProgressStage::reading_game_archive,
                                                   0,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<Inputs>::failure(*error);
    }

    bool cancellation_callback_failed = false;
    jak1_checked_dgo::Options dgo_options;
    dgo_options.max_input_bytes = options.limits.max_game_archive_input_bytes;
    dgo_options.max_compressed_bytes = options.limits.max_game_archive_compressed_bytes;
    dgo_options.max_expanded_bytes = options.limits.max_game_archive_expanded_bytes;
    dgo_options.max_object_bytes = options.limits.max_archive_object_bytes;
    dgo_options.max_total_object_bytes = options.limits.max_archive_total_object_bytes;
    dgo_options.max_objects = options.limits.max_archive_objects;
    dgo_options.max_expansion_ratio = options.limits.max_archive_expansion_ratio;
    dgo_options.file_read_chunk_bytes = options.limits.file_read_chunk_bytes;
    dgo_options.should_cancel = [&]() {
      const auto state = poll_cancel(options);
      cancellation_callback_failed = state == CallbackState::failed;
      return state != CallbackState::continue_work;
    };
    auto archive =
        jak1_checked_dgo::read_file(game_path.value(), std::string(kGameArchiveName), dgo_options);
    if (!archive) {
      if (cancellation_callback_failed) {
        return Result<Inputs>::failure(
            make_error(ErrorCode::callback_failed, "The cancellation callback failed.",
                       std::string(kGameArchivePath), archive.error().object_index));
      }
      if (archive.error().code == jak1_checked_dgo::ErrorCode::cancelled) {
        return Result<Inputs>::failure(
            make_error(ErrorCode::cancelled, "Retail GAME.CGO loading was cancelled.",
                       std::string(kGameArchivePath), archive.error().object_index));
      }
      auto error = make_error(ErrorCode::checked_dgo_failed,
                              "The checked DGO reader rejected retail GAME.CGO.",
                              std::string(kGameArchivePath), archive.error().object_index);
      error.checked_dgo_error = archive.error();
      return Result<Inputs>::failure(std::move(error));
    }

    const jak1_checked_dgo::Object* directory_object = nullptr;
    std::optional<std::uint32_t> directory_index;
    for (std::size_t index = 0; index < archive.value().objects.size(); ++index) {
      const auto& object = archive.value().objects[index];
      if (object.internal_name != contract.value().directory_internal_name) {
        continue;
      }
      if (directory_object) {
        return Result<Inputs>::failure(
            make_error(ErrorCode::duplicate_retail_object, "GAME.CGO repeats dir-tpages.",
                       std::string(kGameArchivePath), static_cast<std::uint32_t>(index)));
      }
      directory_object = &object;
      directory_index = static_cast<std::uint32_t>(index);
    }
    if (!directory_object || directory_object->data.empty()) {
      return Result<Inputs>::failure(make_error(ErrorCode::missing_retail_object,
                                                "GAME.CGO does not contain dir-tpages.",
                                                std::string(kGameArchivePath)));
    }

    Inputs output;
    output.public_subtitle_v2 = contract.value().public_subtitle;
    output.retail.reserve(kRetailLanguageCount + 1);
    if (!add_retail_bytes(&output.retail_bytes, directory_object->data.size(), options)) {
      return Result<Inputs>::failure(make_error(
          ErrorCode::limit_exceeded, "Retail generated inputs exceed the aggregate byte limit.",
          std::string(kGameArchivePath), directory_index));
    }
    output.retail.push_back({RetailSourceKind::archive_object,
                             recipe::GeneratedDataKind::directory_tpages,
                             {},
                             std::string(kGameArchivePath),
                             directory_index,
                             contract.value().directory_internal_name,
                             contract.value().directory_destination,
                             "obj/" + contract.value().directory_destination,
                             directory_object->data});

    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      const auto& destination = contract.value().game_text_destinations[language];
      const auto relative_path = "TEXT/" + destination;
      if (const auto error = emit_progress(options, {ProgressStage::reading_game_text, 1 + language,
                                                     kProgressUnits, relative_path, language})) {
        return Result<Inputs>::failure(*error);
      }
      auto bytes = read_direct_input(tree, relative_path, options, language);
      if (!bytes) {
        return Result<Inputs>::failure(bytes.error());
      }
      if (!add_retail_bytes(&output.retail_bytes, bytes.value().size(), options)) {
        return Result<Inputs>::failure(make_error(
            ErrorCode::limit_exceeded, "Retail generated inputs exceed the aggregate byte limit.",
            relative_path, {}, language));
      }
      output.retail.push_back({RetailSourceKind::direct_file,
                               {},
                               recipe::GeneratedFlatFileKind::game_text,
                               relative_path,
                               {},
                               {},
                               destination,
                               "iso/" + destination,
                               bytes.take_value()});
    }

    if (const auto error = emit_progress(
            options, {ProgressStage::complete, kProgressUnits, kProgressUnits, {}, {}})) {
      return Result<Inputs>::failure(*error);
    }
    return Result<Inputs>::success(std::move(output));
  } catch (const std::bad_alloc&) {
    return Result<Inputs>::failure(make_error(
        ErrorCode::allocation_failed, "Could not allocate the generated-input working set."));
  } catch (const std::length_error&) {
    return Result<Inputs>::failure(make_error(
        ErrorCode::allocation_failed, "A generated-input container exceeded its platform limit."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::invalid_public_graph:
      return "invalid_public_graph";
    case ErrorCode::unsupported_public_graph:
      return "unsupported_public_graph";
    case ErrorCode::invalid_extracted_tree:
      return "invalid_extracted_tree";
    case ErrorCode::missing_input:
      return "missing_input";
    case ErrorCode::input_open_failed:
      return "input_open_failed";
    case ErrorCode::input_read_failed:
      return "input_read_failed";
    case ErrorCode::input_too_large:
      return "input_too_large";
    case ErrorCode::checked_dgo_failed:
      return "checked_dgo_failed";
    case ErrorCode::missing_retail_object:
      return "missing_retail_object";
    case ErrorCode::duplicate_retail_object:
      return "duplicate_retail_object";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
  }
  return "unknown";
}

}  // namespace jak2_extracted_generated_inputs
