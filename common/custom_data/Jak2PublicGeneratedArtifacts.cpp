#include "Jak2PublicGeneratedArtifacts.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string_view>

#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "common/custom_data/PublicGeneratedDataObjectCompiler.h"
#include "common/goos/Reader.h"
#include "common/serialization/subtitles/subtitles_v2.h"
#include "common/serialization/text/text_ser.h"
#include "common/util/font/font_utils.h"
#include "common/util/json_util.h"

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak2_public_generated_artifacts {
namespace {

namespace extracted = jak2_extracted_generated_inputs;
namespace graph = jak1_output_graph;
namespace recipe = jak1_output_recipe;

constexpr std::uint64_t kProvenRevisionContentsHash = 18208811100399420450ull;
constexpr std::uint32_t kRetailLanguageCount = 8;
constexpr std::size_t kExpectedArtifactCount = 10;
constexpr std::size_t kProgressUnits = 40;

struct PublicSource {
  std::string_view relative_path;
  std::uint64_t size;
  std::uint64_t xxh64;
};

constexpr std::array<PublicSource, 12> kPublicSources = {{
    {"game/assets/jak2/game_text.gp", 2780, 0xa2e69ffc238c08d1ull},
    {"game/assets/jak2/text/game_custom_text_en-US.json", 7564, 0x6625036101477415ull},
    {"game/assets/jak2/text/game_custom_text_fr-FR.json", 8604, 0xcbc42e843c0b956dull},
    {"game/assets/jak2/text/game_custom_text_de-DE.json", 8105, 0x7a1da60b33528337ull},
    {"game/assets/jak2/text/game_custom_text_es-ES.json", 8209, 0xee85aa0a46909cb4ull},
    {"game/assets/jak2/text/game_custom_text_it-IT.json", 8140, 0xfc2cdbf146f799fbull},
    {"game/assets/jak2/text/game_custom_text_ja-JP.json", 9358, 0xd67c825c7fa7437dull},
    {"game/assets/jak2/text/game_custom_text_ko-KR.json", 7564, 0x6625036101477415ull},
    {"game/assets/jak2/text/game_custom_text_en-GB.json", 7585, 0x281176307c82a0c4ull},
    {"game/assets/jak2/game_subtitle.gp", 7390, 0x3ce05a2fde3f4212ull},
    {"game/assets/jak2/subtitle/subtitle_lines_en-US.json", 167003, 0xe0b7de5b2a33de9aull},
    {"game/assets/jak2/subtitle/subtitle_meta_en-US.json", 826192, 0xe2a3c587bd8643d7ull},
}};

constexpr PublicSource kKoreanDatabase = {"game/assets/fonts/jak2_jak3_korean_db.json", 474061,
                                          0xe505eaa129be8496ull};

constexpr std::array<std::string_view, kRetailLanguageCount> kTextSources = {
    "game/assets/jak2/text/game_custom_text_en-US.json",
    "game/assets/jak2/text/game_custom_text_fr-FR.json",
    "game/assets/jak2/text/game_custom_text_de-DE.json",
    "game/assets/jak2/text/game_custom_text_es-ES.json",
    "game/assets/jak2/text/game_custom_text_it-IT.json",
    "game/assets/jak2/text/game_custom_text_ja-JP.json",
    "game/assets/jak2/text/game_custom_text_ko-KR.json",
    "game/assets/jak2/text/game_custom_text_en-GB.json",
};

Error make_error(ErrorCode code,
                 std::string message,
                 std::string relative_path = {},
                 std::optional<std::uint32_t> language_id = {},
                 std::optional<std::size_t> byte_offset = {}) {
  return {code, std::move(message), std::move(relative_path), language_id, byte_offset};
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
                                        const std::string& relative_path = {},
                                        std::optional<std::uint32_t> language_id = {}) {
  const auto state = poll_cancel(options);
  if (state == CallbackState::continue_work) {
    return {};
  }
  return make_error(
      state == CallbackState::cancelled ? ErrorCode::cancelled : ErrorCode::callback_failed,
      state == CallbackState::cancelled ? "Jak II generated-artifact compilation was cancelled."
                                        : "The cancellation callback failed.",
      relative_path, language_id);
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
                      progress.relative_path, progress.language_id);
  }
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_public_source_bytes >= kKoreanDatabase.size &&
         limits.max_total_public_source_bytes >= limits.max_public_source_bytes &&
         limits.max_retail_input_bytes > 0 &&
         limits.max_total_retail_input_bytes >= limits.max_retail_input_bytes &&
         limits.max_artifact_bytes > 0 &&
         limits.max_total_artifact_bytes >= limits.max_artifact_bytes &&
         limits.file_read_chunk_bytes > 0 &&
         limits.file_read_chunk_bytes <=
             static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) &&
         limits.max_tpage_entries > 0 && limits.max_text_lines_per_bank > 0 &&
         limits.max_name_bytes > 0 && limits.max_string_bytes > 0 &&
         limits.max_bank_string_bytes > 0;
}

bool add_estimate(std::uint64_t* estimate, std::uint64_t amount, std::uint64_t cap) {
  if (*estimate > cap || amount > cap - *estimate) {
    return false;
  }
  *estimate += amount;
  return true;
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

std::string collision_key(std::string_view value) {
  std::string key(value);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  return key;
}

Result<std::filesystem::path> checked_public_path(const std::filesystem::path& root,
                                                  std::string_view relative_path) {
  const auto fail = [&](ErrorCode code, std::string message) {
    return Result<std::filesystem::path>::failure(
        make_error(code, std::move(message), std::string(relative_path)));
  };
  if (root.empty() || !root.is_absolute() || root != root.lexically_normal()) {
    return fail(ErrorCode::invalid_resource_root,
                "The public resource root must be an absolute normalized path.");
  }
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(root, error);
  if (error || root_status.type() != std::filesystem::file_type::directory) {
    return fail(ErrorCode::invalid_resource_root,
                "The public resource root is not a plain directory.");
  }
  const std::filesystem::path relative(relative_path);
  if (relative.empty() || relative.is_absolute() || relative != relative.lexically_normal() ||
      std::any_of(relative.begin(), relative.end(),
                  [](const auto& component) { return component == "." || component == ".."; })) {
    return fail(ErrorCode::unsafe_public_source,
                "A required public source is not a safe relative path.");
  }
  auto current = root;
  for (auto component = relative.begin(); component != relative.end(); ++component) {
    current /= *component;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
      return fail(ErrorCode::missing_public_source, "A required public source is missing.");
    }
    const bool final = std::next(component) == relative.end();
    if ((!final && status.type() != std::filesystem::file_type::directory) ||
        (final && status.type() != std::filesystem::file_type::regular)) {
      return fail(ErrorCode::unsafe_public_source,
                  "A required public source path is not a plain file hierarchy.");
    }
  }
  return Result<std::filesystem::path>::success(std::move(current));
}

Result<std::vector<std::uint8_t>> read_public_source(const std::filesystem::path& root,
                                                     const PublicSource& source,
                                                     const Options& options) {
  auto checked = checked_public_path(root, source.relative_path);
  if (!checked) {
    return Result<std::vector<std::uint8_t>>::failure(checked.error());
  }
  std::ifstream input(checked.value(), std::ios::binary | std::ios::ate);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::public_source_open_failed, "Could not open a public source.",
                   std::string(source.relative_path)));
  }
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) != source.size ||
      source.size > options.limits.max_public_source_bytes) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::public_source_drift,
                   "A public source size differs from the checked source manifest.",
                   std::string(source.relative_path)));
  }
  input.seekg(0);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::public_source_read_failed, "Could not seek a public source.",
                   std::string(source.relative_path)));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(source.size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (const auto error = cancellation_error(options, std::string(source.relative_path))) {
      return Result<std::vector<std::uint8_t>>::failure(*error);
    }
    const auto chunk = std::min(options.limits.file_read_chunk_bytes, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      return Result<std::vector<std::uint8_t>>::failure(make_error(
          ErrorCode::public_source_read_failed, "Could not read a complete public source.",
          std::string(source.relative_path), {}, offset));
    }
    offset += chunk;
  }
  char trailing = 0;
  input.read(&trailing, 1);
  if (input.gcount() != 0 || XXH64(bytes.data(), bytes.size(), 0) != source.xxh64) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::public_source_drift,
                   "A public source hash differs from the checked source manifest.",
                   std::string(source.relative_path)));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::string as_string(const std::vector<std::uint8_t>& bytes) {
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Result<std::map<std::string, std::vector<std::uint8_t>>> load_public_sources(
    const std::filesystem::path& root,
    const Options& options) {
  std::map<std::string, std::vector<std::uint8_t>> result;
  std::uint64_t total = 0;
  std::size_t unit = 1;
  const auto load = [&](const PublicSource& source)
      -> std::optional<Result<std::map<std::string, std::vector<std::uint8_t>>>> {
    if (const auto error = emit_progress(options, {ProgressStage::reading_public_source,
                                                   unit++,
                                                   kProgressUnits,
                                                   std::string(source.relative_path),
                                                   {}})) {
      return Result<std::map<std::string, std::vector<std::uint8_t>>>::failure(*error);
    }
    if (source.size > options.limits.max_total_public_source_bytes - total) {
      return Result<std::map<std::string, std::vector<std::uint8_t>>>::failure(make_error(
          ErrorCode::limit_exceeded, "The checked public-source set exceeds its aggregate bound.",
          std::string(source.relative_path)));
    }
    auto bytes = read_public_source(root, source, options);
    if (!bytes) {
      return Result<std::map<std::string, std::vector<std::uint8_t>>>::failure(bytes.error());
    }
    total += bytes.value().size();
    if (!result.emplace(std::string(source.relative_path), bytes.take_value()).second) {
      return Result<std::map<std::string, std::vector<std::uint8_t>>>::failure(
          make_error(ErrorCode::invalid_public_source,
                     "The checked public-source manifest contains a duplicate path.",
                     std::string(source.relative_path)));
    }
    return {};
  };
  for (const auto& source : kPublicSources) {
    if (auto failure = load(source)) {
      return std::move(*failure);
    }
  }
  if (auto failure = load(kKoreanDatabase)) {
    return std::move(*failure);
  }
  return Result<std::map<std::string, std::vector<std::uint8_t>>>::success(std::move(result));
}

std::optional<Error> validate_graph(const graph::Graph& public_graph) {
  auto encoded = graph::encode(public_graph, jak2_public_output_graph::default_options());
  if (!encoded) {
    return make_error(ErrorCode::invalid_public_graph,
                      "The checked Jak II public graph is invalid.");
  }
  auto embedded = jak2_public_output_graph::decode_base_retail();
  if (!embedded) {
    return make_error(ErrorCode::invalid_public_graph,
                      "The embedded Jak II base-retail graph is invalid.");
  }
  if (public_graph != embedded.value()) {
    return make_error(ErrorCode::unsupported_public_graph,
                      "The Jak II public graph differs from the checked base-retail graph.");
  }
  return {};
}

std::optional<Error> validate_input_shape(const extracted::Inputs& inputs,
                                          const Options& options,
                                          std::array<const extracted::RetailInput*, 8>* text,
                                          const extracted::RetailInput** directory) {
  if (!proven_revision(inputs.revision)) {
    return make_error(ErrorCode::unsupported_revision,
                      "Generated inputs do not retain the proven Jak II NTSC-U v2 revision.");
  }
  if (inputs.public_subtitle_v2.output_kind != recipe::GeneratedFlatFileKind::game_subtitle ||
      inputs.public_subtitle_v2.destination_basename != "0SUBTI2.TXT" ||
      inputs.public_subtitle_v2.output_relative_path != "iso/0SUBTI2.TXT" ||
      inputs.public_subtitle_v2.project_relative_path != "game/assets/jak2/game_subtitle.gp" ||
      inputs.public_subtitle_v2.compiler_tool != "subtitle-v2") {
    return make_error(ErrorCode::invalid_public_requirement,
                      "The public subtitle-v2 requirement differs from the checked contract.");
  }
  if (inputs.retail.size() != 9) {
    return make_error(ErrorCode::invalid_retail_inputs,
                      "The generated-input set must contain exactly nine retail inputs.");
  }
  std::uint64_t total = 0;
  std::set<std::string> outputs;
  for (const auto& input : inputs.retail) {
    if (input.bytes.empty() || input.bytes.size() > options.limits.max_retail_input_bytes ||
        input.bytes.size() > options.limits.max_total_retail_input_bytes - total ||
        !outputs.insert(collision_key(input.output_relative_path)).second) {
      return make_error(ErrorCode::invalid_retail_inputs,
                        "A retail generated input is empty, duplicated, or exceeds its bound.",
                        input.source_relative_path);
    }
    total += input.bytes.size();
    if (input.source_kind == extracted::RetailSourceKind::archive_object && input.object_kind &&
        *input.object_kind == recipe::GeneratedDataKind::directory_tpages &&
        input.source_relative_path == "CGO/GAME.CGO" && input.internal_name == "dir-tpages" &&
        input.destination_basename == "dir-tpages.go" &&
        input.output_relative_path == "obj/dir-tpages.go" && !*directory) {
      *directory = &input;
      continue;
    }
    if (input.source_kind != extracted::RetailSourceKind::direct_file || input.object_kind ||
        input.flat_file_kind != recipe::GeneratedFlatFileKind::game_text ||
        input.archive_object_index || !input.internal_name.empty()) {
      return make_error(ErrorCode::invalid_retail_inputs,
                        "A retail input does not match a supported Jak II generated source.",
                        input.source_relative_path);
    }
    bool matched = false;
    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      const auto destination = std::to_string(language) + "COMMON.TXT";
      if (input.source_relative_path == "TEXT/" + destination &&
          input.destination_basename == destination &&
          input.output_relative_path == "iso/" + destination && !text->at(language)) {
        text->at(language) = &input;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return make_error(ErrorCode::invalid_retail_inputs,
                        "A retail game-text input is missing, duplicated, or misrouted.",
                        input.source_relative_path);
    }
  }
  if (!*directory ||
      std::any_of(text->begin(), text->end(), [](const auto* item) { return !item; }) ||
      total != inputs.retail_bytes) {
    return make_error(ErrorCode::invalid_retail_inputs,
                      "The retail generated-input set is incomplete or has invalid accounting.");
  }
  return {};
}

jak1_extracted_generated_inputs::Options parser_options(const Options& options) {
  jak1_extracted_generated_inputs::Options result;
  result.limits.max_archive_object_bytes = options.limits.max_retail_input_bytes;
  result.limits.max_tpage_entries = options.limits.max_tpage_entries;
  result.limits.max_text_lines_per_bank = options.limits.max_text_lines_per_bank;
  result.limits.max_name_bytes = options.limits.max_name_bytes;
  result.limits.max_string_bytes = options.limits.max_string_bytes;
  result.limits.max_generated_bank_string_bytes = options.limits.max_bank_string_bytes;
  result.should_cancel = options.should_cancel;
  return result;
}

Error parser_error(const jak1_extracted_generated_inputs::Error& error) {
  return make_error(
      error.code == jak1_extracted_generated_inputs::ErrorCode::cancelled ? ErrorCode::cancelled
      : error.code == jak1_extracted_generated_inputs::ErrorCode::callback_failed
          ? ErrorCode::callback_failed
          : ErrorCode::invalid_retail_object,
      "The checked GOAL data-object parser rejected a Jak II generated input: " + error.message,
      error.source_relative_path, error.language_id, error.byte_offset);
}

std::optional<Error> add_artifact(Build* output, Artifact artifact, const Options& options) {
  if (artifact.bytes.empty() || artifact.bytes.size() > options.limits.max_artifact_bytes ||
      artifact.bytes.size() > options.limits.max_total_artifact_bytes - output->total_bytes) {
    return make_error(ErrorCode::limit_exceeded,
                      "A generated Jak II artifact exceeds its configured output bound.",
                      artifact.output_relative_path);
  }
  artifact.xxh64 = XXH64(artifact.bytes.data(), artifact.bytes.size(), 0);
  output->total_bytes += artifact.bytes.size();
  output->artifacts.push_back(std::move(artifact));
  return {};
}

std::optional<Error> check_artifact_estimate(const Build& output,
                                             std::uint64_t estimate,
                                             std::string relative_path,
                                             const Options& options) {
  if (estimate > options.limits.max_artifact_bytes ||
      estimate > options.limits.max_total_artifact_bytes - output.total_bytes) {
    return make_error(ErrorCode::limit_exceeded,
                      "A generated Jak II artifact would exceed its configured output bound.",
                      std::move(relative_path));
  }
  return {};
}

std::optional<Error> check_directory_estimate(
    const Build& output,
    const jak1_public_generated_artifacts::DirectoryTpages& directory,
    const Options& options) {
  std::uint64_t estimate = 4096;
  if (!add_estimate(&estimate, directory.lengths.size() * 64ull,
                    options.limits.max_artifact_bytes)) {
    return make_error(ErrorCode::limit_exceeded,
                      "The canonical texture-page directory exceeds its output bound.",
                      "obj/dir-tpages.go");
  }
  return check_artifact_estimate(output, estimate, "obj/dir-tpages.go", options);
}

std::optional<Error> check_text_estimate(const Build& output,
                                         const GameTextBank& bank,
                                         std::string relative_path,
                                         const Options& options) {
  std::uint64_t estimate = 4096;
  for (const auto& [id, line] : bank.lines()) {
    (void)id;
    if (line.size() > options.limits.max_string_bytes ||
        !add_estimate(&estimate, 64ull + line.size(), options.limits.max_artifact_bytes)) {
      return make_error(ErrorCode::limit_exceeded,
                        "A merged Jak II game-text bank exceeds its output bound.",
                        std::move(relative_path), static_cast<std::uint32_t>(bank.lang()));
    }
  }
  return check_artifact_estimate(output, estimate, std::move(relative_path), options);
}

std::optional<Error> check_subtitle_estimate(const Build& output,
                                             const GameSubtitleBank& bank,
                                             const Options& options) {
  constexpr std::string_view kPath = "iso/0SUBTI2.TXT";
  std::uint64_t estimate = 4096;
  for (const auto& [speaker, localized] : bank.m_speakers) {
    if (speaker.size() > options.limits.max_name_bytes ||
        localized.size() > options.limits.max_string_bytes ||
        !add_estimate(&estimate, 64ull + speaker.size() + localized.size(),
                      options.limits.max_artifact_bytes)) {
      return make_error(ErrorCode::limit_exceeded,
                        "The Jak II subtitle speaker table exceeds its output bound.",
                        std::string(kPath), 0);
    }
  }
  for (const auto& [name, scene] : bank.m_scenes) {
    if (name.size() > options.limits.max_name_bytes ||
        !add_estimate(&estimate, 64ull + name.size(), options.limits.max_artifact_bytes)) {
      return make_error(ErrorCode::limit_exceeded,
                        "A Jak II subtitle scene exceeds its output bound.", std::string(kPath), 0);
    }
    for (const auto& line : scene.m_lines) {
      if (line.text.size() > options.limits.max_string_bytes ||
          line.metadata.speaker.size() > options.limits.max_name_bytes ||
          !add_estimate(&estimate, 96ull + line.text.size() + line.metadata.speaker.size(),
                        options.limits.max_artifact_bytes)) {
        return make_error(ErrorCode::limit_exceeded,
                          "A Jak II subtitle line exceeds its output bound.", std::string(kPath),
                          0);
      }
    }
  }
  return check_artifact_estimate(output, estimate, std::string(kPath), options);
}

}  // namespace

Result<Build> build(const extracted::Inputs& inputs,
                    const std::filesystem::path& project_resource_root,
                    const Options& options) {
  auto decoded = jak2_public_output_graph::decode_base_retail();
  if (!decoded) {
    return Result<Build>::failure(
        make_error(ErrorCode::invalid_public_graph,
                   "The embedded Jak II base-retail graph could not be decoded."));
  }
  return build(inputs, decoded.value(), project_resource_root, options);
}

Result<Build> build(const extracted::Inputs& inputs,
                    const graph::Graph& public_graph,
                    const std::filesystem::path& project_resource_root,
                    const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<Build>::failure(
          make_error(ErrorCode::invalid_argument, "Generated-artifact options are invalid."));
    }
    if (const auto error =
            emit_progress(options, {ProgressStage::validating_inputs, 0, kProgressUnits, {}, {}})) {
      return Result<Build>::failure(*error);
    }
    if (const auto error = cancellation_error(options)) {
      return Result<Build>::failure(*error);
    }
    if (const auto error = validate_graph(public_graph)) {
      return Result<Build>::failure(*error);
    }

    std::array<const extracted::RetailInput*, kRetailLanguageCount> retail_text{};
    const extracted::RetailInput* retail_directory = nullptr;
    if (const auto error = validate_input_shape(inputs, options, &retail_text, &retail_directory)) {
      return Result<Build>::failure(*error);
    }

    auto resources = load_public_sources(project_resource_root, options);
    if (!resources) {
      return Result<Build>::failure(resources.error());
    }
    const auto& source = resources.value();
    const auto parse_json = [&](std::string_view path) {
      return parse_commented_json(as_string(source.at(std::string(path))), std::string(path));
    };
    KoreanLookupDatabase korean_database;
    parse_json(kKoreanDatabase.relative_path).get_to(korean_database);

    const auto checked_options = parser_options(options);
    if (const auto error = emit_progress(options, {ProgressStage::parsing_retail_object,
                                                   14,
                                                   kProgressUnits,
                                                   retail_directory->source_relative_path,
                                                   {}})) {
      return Result<Build>::failure(*error);
    }
    auto directory = jak1_extracted_generated_inputs::parse_checked_directory_tpages(
        retail_directory->bytes, retail_directory->source_relative_path, checked_options,
        jak1_extracted_generated_inputs::CheckedObjectProfile::jak2_retail_extent);
    if (!directory) {
      return Result<Build>::failure(parser_error(directory.error()));
    }

    GameTextDB text_database;
    goos::Reader source_reader;
    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      if (const auto error = emit_progress(
              options, {ProgressStage::parsing_retail_object, 15 + language, kProgressUnits,
                        retail_text[language]->source_relative_path, language})) {
        return Result<Build>::failure(*error);
      }
      auto bank = jak1_extracted_generated_inputs::parse_checked_game_text(
          retail_text[language]->bytes, language, retail_text[language]->destination_basename,
          retail_text[language]->source_relative_path, checked_options,
          jak1_extracted_generated_inputs::CheckedObjectProfile::jak2_retail_extent);
      if (!bank) {
        return Result<Build>::failure(parser_error(bank.error()));
      }
      auto merged = std::make_shared<GameTextBank>(static_cast<int>(language));
      const auto font = get_font_bank(GameTextVersion::JAK2);
      std::size_t canonical_string_bytes = 0;
      std::size_t decoded_source_bytes = 0;
      for (std::size_t line_index = 0; line_index < bank.value().lines.size(); ++line_index) {
        if ((line_index & 0x3ffu) == 0) {
          if (const auto error = cancellation_error(
                  options, retail_text[language]->source_relative_path, language)) {
            return Result<Build>::failure(*error);
          }
        }
        const auto& line = bank.value().lines[line_index];
        const auto decoded = font->is_language_id_korean(language)
                                 ? font->convert_korean_game_to_utf8(line.encoded_text.c_str())
                                 : font->convert_game_to_utf8(line.encoded_text.c_str());
        if (decoded.size() > options.limits.max_string_bytes ||
            decoded_source_bytes > options.limits.max_bank_string_bytes - decoded.size()) {
          return Result<Build>::failure(
              make_error(ErrorCode::limit_exceeded,
                         "A decoded retail game-text source string exceeds its bound.",
                         retail_text[language]->source_relative_path, language));
        }
        decoded_source_bytes += decoded.size();
        // The reader only parses the desktop decompiler's escaped string form; it does not
        // evaluate GOOS code or add the parsed object to the reader's top level.
        const auto parsed_source =
            source_reader.read_from_string("\"" + decoded + "\"", false, "retail game text");
        if (!parsed_source.is_pair() || !parsed_source.as_pair()->car.is_string() ||
            !parsed_source.as_pair()->cdr.is_empty_list()) {
          return Result<Build>::failure(make_error(
              ErrorCode::invalid_retail_object,
              "A retail game-text string did not round-trip through the GOOS source reader.",
              retail_text[language]->source_relative_path, language));
        }
        const auto& source_string = parsed_source.as_pair()->car.as_string()->data;
        const auto canonical =
            font->is_language_id_korean(language)
                ? font->convert_utf8_to_game_korean(source_string, korean_database)
                : font->convert_utf8_to_game(source_string);
        if (canonical.size() > options.limits.max_string_bytes ||
            canonical_string_bytes > options.limits.max_bank_string_bytes - canonical.size()) {
          return Result<Build>::failure(
              make_error(ErrorCode::limit_exceeded,
                         "A canonical retail game-text bank exceeds its string bound.",
                         retail_text[language]->source_relative_path, language));
        }
        canonical_string_bytes += canonical.size();
        merged->set_line(static_cast<int>(line.id), canonical);
      }
      text_database.add_bank("common", std::move(merged));
    }

    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      if (const auto error = emit_progress(
              options, {ProgressStage::merging_public_text, 23 + language, kProgressUnits,
                        std::string(kTextSources[language]), language})) {
        return Result<Build>::failure(*error);
      }
      const GameTextDefinitionFile definition{GameTextDefinitionFile::Format::JSON,
                                              std::string(kTextSources[language]),
                                              static_cast<int>(language), "jak2", "common"};
      parse_text_json(parse_json(kTextSources[language]), text_database, definition,
                      &korean_database);
    }

    if (const auto error =
            emit_progress(options, {ProgressStage::compiling_subtitle_v2, 31, kProgressUnits,
                                    "game/assets/jak2/game_subtitle.gp", 0})) {
      return Result<Build>::failure(*error);
    }
    auto package =
        read_json_values_v2(parse_json("game/assets/jak2/subtitle/subtitle_lines_en-US.json"),
                            parse_json("game/assets/jak2/subtitle/subtitle_meta_en-US.json"));
    GameSubtitleBank subtitle_bank(0);
    subtitle_bank.m_text_version = GameTextVersion::JAK2;
    subtitle_bank.m_file_path = "game/assets/jak2/subtitle/subtitle_lines_en-US.json";
    subtitle_bank.m_speakers = package.combined_lines.speakers;
    subtitle_bank.m_speakers.emplace("none", "none");
    subtitle_bank.add_scenes_from_files(package);

    Build output;
    if (const auto error = check_directory_estimate(output, directory.value(), options)) {
      return Result<Build>::failure(*error);
    }
    if (const auto error = emit_progress(
            options,
            {ProgressStage::generating_artifact, 32, kProgressUnits, "obj/dir-tpages.go", {}})) {
      return Result<Build>::failure(*error);
    }
    if (const auto error =
            add_artifact(&output,
                         {ArtifactKind::directory_tpages, "dir-tpages.go", "obj/dir-tpages.go",
                          public_generated_data_object_compiler::build_directory_tpages(
                              directory.value().lengths)},
                         options)) {
      return Result<Build>::failure(*error);
    }

    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      const auto destination = std::to_string(language) + "COMMON.TXT";
      if (const auto error =
              emit_progress(options, {ProgressStage::generating_artifact, 33 + language,
                                      kProgressUnits, "iso/" + destination, language})) {
        return Result<Build>::failure(*error);
      }
      const auto bank = text_database.bank_by_id("common", static_cast<int>(language));
      if (!bank || bank->lines().empty() ||
          bank->lines().size() > options.limits.max_text_lines_per_bank) {
        return Result<Build>::failure(
            make_error(ErrorCode::invalid_public_source,
                       "A merged Jak II game-text bank is empty or exceeds its line bound.",
                       destination, language));
      }
      if (const auto error = check_text_estimate(output, *bank, "iso/" + destination, options)) {
        return Result<Build>::failure(*error);
      }
      if (const auto error = add_artifact(
              &output,
              {ArtifactKind::game_text, destination, "iso/" + destination,
               public_generated_data_object_compiler::build_game_text("common", *bank)},
              options)) {
        return Result<Build>::failure(*error);
      }
    }

    if (const auto error = check_subtitle_estimate(output, subtitle_bank, options)) {
      return Result<Build>::failure(*error);
    }
    if (const auto error =
            add_artifact(&output,
                         {ArtifactKind::subtitle_v2, "0SUBTI2.TXT", "iso/0SUBTI2.TXT",
                          public_generated_data_object_compiler::build_subtitle_v2(subtitle_bank)},
                         options)) {
      return Result<Build>::failure(*error);
    }
    if (output.artifacts.size() != kExpectedArtifactCount) {
      return Result<Build>::failure(make_error(
          ErrorCode::generation_failed, "Generated output did not contain exactly ten artifacts."));
    }
    if (const auto error = emit_progress(
            options, {ProgressStage::complete, kProgressUnits, kProgressUnits, {}, {}})) {
      return Result<Build>::failure(*error);
    }
    return Result<Build>::success(std::move(output));
  } catch (const nlohmann::json::exception& error) {
    return Result<Build>::failure(
        make_error(ErrorCode::invalid_public_source,
                   "A checked public JSON source is invalid: " + std::string(error.what())));
  } catch (const std::bad_alloc&) {
    return Result<Build>::failure(make_error(
        ErrorCode::allocation_failed, "Could not allocate the generated-artifact working set."));
  } catch (const std::length_error& error) {
    return Result<Build>::failure(make_error(
        ErrorCode::limit_exceeded,
        "A generated-artifact container exceeded its limit: " + std::string(error.what())));
  } catch (const std::exception& error) {
    return Result<Build>::failure(
        make_error(ErrorCode::generation_failed,
                   "Jak II generated-artifact compilation failed: " + std::string(error.what())));
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
    case ErrorCode::invalid_retail_inputs:
      return "invalid_retail_inputs";
    case ErrorCode::invalid_public_requirement:
      return "invalid_public_requirement";
    case ErrorCode::invalid_resource_root:
      return "invalid_resource_root";
    case ErrorCode::missing_public_source:
      return "missing_public_source";
    case ErrorCode::unsafe_public_source:
      return "unsafe_public_source";
    case ErrorCode::public_source_open_failed:
      return "public_source_open_failed";
    case ErrorCode::public_source_read_failed:
      return "public_source_read_failed";
    case ErrorCode::public_source_drift:
      return "public_source_drift";
    case ErrorCode::invalid_public_source:
      return "invalid_public_source";
    case ErrorCode::invalid_retail_object:
      return "invalid_retail_object";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::generation_failed:
      return "generation_failed";
  }
  return "unknown";
}

}  // namespace jak2_public_generated_artifacts
