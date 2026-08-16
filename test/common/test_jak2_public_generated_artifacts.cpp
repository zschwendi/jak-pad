#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include "common/custom_data/GoalDataObjectBuilder.h"
#include "common/custom_data/Jak2PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak2PublicOutputGraph.h"
#include "common/custom_data/PublicGeneratedDataObjectCompiler.h"
#include "common/serialization/subtitles/subtitles_v2.h"
#include "common/serialization/text/text_ser.h"
#include "common/util/FileUtil.h"
#include "common/util/font/font_utils.h"
#include "common/util/json_util.h"

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"
#include "decompiler/extractor/jak2_extracted_generated_inputs.h"
#include "goalc/data_compiler/DataObjectGenerator.h"
#include "goalc/data_compiler/game_text_common.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace artifacts = jak2_public_generated_artifacts;
namespace extracted = jak2_extracted_generated_inputs;
namespace recipe = jak1_output_recipe;

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

constexpr std::array<const char*, 13> kPublicSources = {
    "game/assets/jak2/game_text.gp",
    "game/assets/jak2/text/game_custom_text_en-US.json",
    "game/assets/jak2/text/game_custom_text_fr-FR.json",
    "game/assets/jak2/text/game_custom_text_de-DE.json",
    "game/assets/jak2/text/game_custom_text_es-ES.json",
    "game/assets/jak2/text/game_custom_text_it-IT.json",
    "game/assets/jak2/text/game_custom_text_ja-JP.json",
    "game/assets/jak2/text/game_custom_text_ko-KR.json",
    "game/assets/jak2/text/game_custom_text_en-GB.json",
    "game/assets/jak2/game_subtitle.gp",
    "game/assets/jak2/subtitle/subtitle_lines_en-US.json",
    "game/assets/jak2/subtitle/subtitle_meta_en-US.json",
    "game/assets/fonts/jak2_jak3_korean_db.json",
};

const jak2_iso::Revision& proven_revision() {
  for (const auto& revision : jak2_iso::supported_revisions()) {
    if (revision.contents_hash == 18208811100399420450ull) {
      return revision;
    }
  }
  throw std::runtime_error("proven Jak II revision missing");
}

std::vector<std::uint8_t> make_directory() {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("texture-page-dir");
  builder.add_word(3);
  for (const auto length : {1u, 2u, 3u}) {
    builder.add_word(length);
    builder.add_symbol_link("#f");
    builder.add_symbol_link("#f");
  }
  auto bytes = builder.generate_v4();
  const auto code_size = std::uint32_t(bytes[12]) | (std::uint32_t(bytes[13]) << 8) |
                         (std::uint32_t(bytes[14]) << 16) | (std::uint32_t(bytes[15]) << 24);
  bytes.at(16 + code_size - 4) = 0xa5;
  return bytes;
}

std::vector<std::uint8_t> make_text(std::uint32_t language) {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(2);
  builder.add_word(language);
  builder.add_ref_to_string("common");
  builder.add_word(0x1000 + language);
  builder.add_ref_to_string("RETAIL-A");
  builder.add_word(0x2000 + language);
  builder.add_ref_to_string("RETAIL-B");
  auto bytes = builder.generate_v2();
  bytes.pop_back();
  return bytes;
}

std::vector<std::uint8_t> make_expanding_text(std::uint32_t language) {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(1);
  builder.add_word(language);
  builder.add_ref_to_string("common");
  builder.add_word(0x1000);
  builder.add_ref_to_string(std::string(16, '\x01'));
  return builder.generate_v2();
}

extracted::Inputs make_inputs() {
  extracted::Inputs inputs;
  inputs.revision = proven_revision();
  auto directory = make_directory();
  inputs.retail_bytes += directory.size();
  inputs.retail.push_back({extracted::RetailSourceKind::archive_object,
                           recipe::GeneratedDataKind::directory_tpages,
                           {},
                           "CGO/GAME.CGO",
                           4,
                           "dir-tpages",
                           "dir-tpages.go",
                           "obj/dir-tpages.go",
                           std::move(directory)});
  for (std::uint32_t language = 0; language < 8; ++language) {
    auto bytes = make_text(language);
    inputs.retail_bytes += bytes.size();
    const auto destination = std::to_string(language) + "COMMON.TXT";
    inputs.retail.push_back({extracted::RetailSourceKind::direct_file,
                             {},
                             recipe::GeneratedFlatFileKind::game_text,
                             "TEXT/" + destination,
                             {},
                             {},
                             destination,
                             "iso/" + destination,
                             std::move(bytes)});
  }
  inputs.public_subtitle_v2 = {recipe::GeneratedFlatFileKind::game_subtitle, "0SUBTI2.TXT",
                               "iso/0SUBTI2.TXT", "game/assets/jak2/game_subtitle.gp",
                               "subtitle-v2"};
  return inputs;
}

class PublicFixture {
 public:
  PublicFixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("opengoal-jak2-public-artifacts-" + std::to_string(stamp));
    for (const auto* relative : kPublicSources) {
      const auto destination = root / relative;
      std::filesystem::create_directories(destination.parent_path());
      std::filesystem::copy_file(std::filesystem::path(OPENGOAL_SOURCE_DIR) / relative,
                                 destination);
    }
  }

  ~PublicFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path root;
};

bool builds_exact_declared_output_set() {
  PublicFixture fixture;
  std::vector<artifacts::Progress> progress;
  artifacts::Options options;
  options.on_progress = [&](const auto& update) { progress.push_back(update); };
  const auto first = artifacts::build(make_inputs(), fixture.root, options);
  CHECK(first);
  const auto second = artifacts::build(make_inputs(), fixture.root);
  CHECK(second);
  CHECK(first.value() == second.value());
  CHECK(first.value().artifacts.size() == 10);
  CHECK(first.value().artifacts.front().kind == artifacts::ArtifactKind::directory_tpages);
  CHECK(first.value().artifacts.front().output_relative_path == "obj/dir-tpages.go");
  for (std::uint32_t language = 0; language < 8; ++language) {
    const auto& artifact = first.value().artifacts.at(language + 1);
    const auto destination = std::to_string(language) + "COMMON.TXT";
    CHECK(artifact.kind == artifacts::ArtifactKind::game_text);
    CHECK(artifact.destination_basename == destination);
    CHECK(artifact.output_relative_path == "iso/" + destination);
    CHECK(!artifact.bytes.empty());
    CHECK(artifact.xxh64 != 0);
  }
  CHECK(first.value().artifacts.back().kind == artifacts::ArtifactKind::subtitle_v2);
  CHECK(first.value().artifacts.back().destination_basename == "0SUBTI2.TXT");
  CHECK(first.value().artifacts.back().output_relative_path == "iso/0SUBTI2.TXT");
  CHECK(!first.value().artifacts.back().bytes.empty());
  CHECK(progress.front().stage == artifacts::ProgressStage::validating_inputs);
  CHECK(progress.back().stage == artifacts::ProgressStage::complete);
  return true;
}

bool rejects_revision_graph_and_input_drift() {
  PublicFixture fixture;
  auto wrong_revision = make_inputs();
  wrong_revision.revision = jak2_iso::default_revision();
  auto result = artifacts::build(wrong_revision, fixture.root);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::unsupported_revision);

  auto decoded = jak2_public_output_graph::decode_base_retail();
  CHECK(decoded);
  auto graph = decoded.take_value();
  graph.generated_flat_files.pop_back();
  result = artifacts::build(make_inputs(), graph, fixture.root);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::unsupported_public_graph);

  auto duplicate = make_inputs();
  duplicate.retail.push_back(duplicate.retail.back());
  duplicate.retail_bytes += duplicate.retail.back().bytes.size();
  result = artifacts::build(duplicate, fixture.root);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_retail_inputs);

  auto wrong_game = make_inputs();
  wrong_game.retail.at(1).bytes = make_text(7);
  wrong_game.retail_bytes = 0;
  for (const auto& input : wrong_game.retail) {
    wrong_game.retail_bytes += input.bytes.size();
  }
  result = artifacts::build(wrong_game, fixture.root);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_retail_object);
  return true;
}

bool rejects_public_source_drift_symlinks_and_limits() {
  {
    PublicFixture fixture;
    const auto path = fixture.root / "game/assets/jak2/game_text.gp";
    std::ofstream output(path, std::ios::binary | std::ios::app);
    output.put('x');
    output.close();
    const auto result = artifacts::build(make_inputs(), fixture.root);
    CHECK(!result);
    CHECK(result.error().code == artifacts::ErrorCode::public_source_drift);
  }
  {
    PublicFixture fixture;
    const auto path = fixture.root / "game/assets/jak2/game_text.gp";
    const auto outside = fixture.root.parent_path() / (fixture.root.filename().string() + "-gp");
    std::filesystem::copy_file(
        std::filesystem::path(OPENGOAL_SOURCE_DIR) / "game/assets/jak2/game_text.gp", outside);
    std::filesystem::remove(path);
    std::filesystem::create_symlink(outside, path);
    const auto result = artifacts::build(make_inputs(), fixture.root);
    CHECK(!result);
    CHECK(result.error().code == artifacts::ErrorCode::unsafe_public_source);
    std::error_code error;
    std::filesystem::remove(outside, error);
  }
  {
    PublicFixture fixture;
    artifacts::Options options;
    options.limits.max_retail_input_bytes = 128;
    options.limits.max_total_retail_input_bytes = 1024;
    const auto result = artifacts::build(make_inputs(), fixture.root, options);
    CHECK(!result);
    CHECK(result.error().code == artifacts::ErrorCode::invalid_retail_inputs);
  }
  {
    PublicFixture fixture;
    auto inputs = make_inputs();
    inputs.retail_bytes -= inputs.retail.at(1).bytes.size();
    inputs.retail.at(1).bytes = make_expanding_text(0);
    inputs.retail_bytes += inputs.retail.at(1).bytes.size();
    artifacts::Options options;
    options.limits.max_string_bytes = 32;
    const auto result = artifacts::build(inputs, fixture.root, options);
    CHECK(!result);
    CHECK(result.error().code == artifacts::ErrorCode::limit_exceeded);
    CHECK(result.error().relative_path == "TEXT/0COMMON.TXT");
  }
  {
    PublicFixture fixture;
    artifacts::Options options;
    options.limits.max_artifact_bytes = 4096;
    options.limits.max_total_artifact_bytes = 4096;
    bool reached_generation = false;
    options.on_progress = [&](const auto& progress) {
      reached_generation |= progress.stage == artifacts::ProgressStage::generating_artifact;
    };
    const auto result = artifacts::build(make_inputs(), fixture.root, options);
    CHECK(!result);
    CHECK(result.error().code == artifacts::ErrorCode::limit_exceeded);
    CHECK(result.error().relative_path == "obj/dir-tpages.go");
    CHECK(!reached_generation);
  }
  return true;
}

bool cancellation_and_callback_failures_are_explicit() {
  PublicFixture fixture;
  artifacts::Options cancelled;
  cancelled.should_cancel = [] { return true; };
  auto result = artifacts::build(make_inputs(), fixture.root, cancelled);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::cancelled);

  artifacts::Options cancel_failed;
  cancel_failed.should_cancel = []() -> bool { throw std::runtime_error("cancel failed"); };
  result = artifacts::build(make_inputs(), fixture.root, cancel_failed);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::callback_failed);

  artifacts::Options progress_failed;
  progress_failed.on_progress = [](const auto&) { throw std::runtime_error("progress failed"); };
  result = artifacts::build(make_inputs(), fixture.root, progress_failed);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::callback_failed);
  return true;
}

std::vector<std::uint8_t> desktop_text(std::string_view group, const GameTextBank& bank) {
  DataObjectGenerator generator;
  generator.add_type_tag("game-text-info");
  generator.add_word(static_cast<std::uint32_t>(bank.lines().size()));
  generator.add_word(static_cast<std::uint32_t>(bank.lang()));
  generator.add_ref_to_string_in_pool(std::string(group));
  for (const auto& [id, line] : bank.lines()) {
    generator.add_word(static_cast<std::uint32_t>(id));
    generator.add_ref_to_string_in_pool(line);
  }
  return generator.generate_v2();
}

std::vector<std::uint8_t> desktop_subtitle_v2(GameSubtitleBank& bank) {
  auto font = get_font_bank(bank.m_text_version);
  DataObjectGenerator generator;
  generator.add_type_tag("subtitle2-text-info");
  generator.add_word((bank.m_scenes.size() & 0xffffu) | (1u << 16));
  generator.add_word((static_cast<std::uint32_t>(bank.m_lang_id) & 0xffffu) |
                     ((bank.m_speakers.size() + 1u) << 16));
  const auto speaker_array_link = generator.add_word(0);
  std::queue<int> line_array_links;
  for (const auto& [name, scene] : bank.m_scenes) {
    generator.add_ref_to_string_in_pool(name);
    generator.add_word(static_cast<std::uint32_t>(scene.m_lines.size()));
    line_array_links.push(generator.words());
    generator.add_word(0);
  }
  for (const auto& [name, scene] : bank.m_scenes) {
    (void)name;
    generator.link_word_to_word(line_array_links.front(), generator.words());
    line_array_links.pop();
    for (const auto& line : scene.m_lines) {
      generator.add_word_float(static_cast<float>(line.metadata.frame_start));
      generator.add_word_float(static_cast<float>(line.metadata.frame_end));
      if (line.metadata.merge) {
        generator.add_symbol_link("#f");
      } else {
        generator.add_ref_to_string_in_pool(font->convert_utf8_to_game(line.text));
      }
      const auto speaker = bank.speaker_enum_value_from_name(line.metadata.speaker);
      std::uint16_t flags = 0;
      flags |= static_cast<std::uint16_t>(line.metadata.offscreen) << 0;
      flags |= static_cast<std::uint16_t>(line.metadata.merge) << 1;
      generator.add_word(static_cast<std::uint32_t>(speaker) |
                         (static_cast<std::uint32_t>(flags) << 16));
    }
  }
  generator.link_word_to_word(speaker_array_link, generator.words());
  for (const auto& speaker : bank.speaker_names_ordered_by_enum_value()) {
    generator.add_ref_to_string_in_pool(font->convert_utf8_to_game(speaker));
  }
  return generator.generate_v2();
}

bool shared_emitters_are_byte_exact_with_desktop_generator() {
  PublicFixture fixture;
  CHECK(file_util::setup_project_path(fs::path(fixture.root.string()), true));
  GameTextBank text(1);
  text.set_line(0x1000, "SAME");
  text.set_line(0x2000, "SAME");
  text.set_line(0x3000, "DIFFERENT");
  CHECK(public_generated_data_object_compiler::build_game_text("common", text) ==
        desktop_text("common", text));
  GameTextDB desktop_text_database;
  desktop_text_database.add_bank("common", std::make_shared<GameTextBank>(text));
  compile_game_text({}, desktop_text_database, "desktop-wrapper-proof");
  CHECK(file_util::read_binary_file(
            file_util::get_file_path({"out", "desktop-wrapper-proof", "iso", "1COMMON.TXT"})) ==
        public_generated_data_object_compiler::build_game_text("common", text));

  GameSubtitleBank subtitle(0);
  subtitle.m_text_version = GameTextVersion::JAK2;
  const auto subtitle_lines = parse_commented_json(
      file_util::read_text_file(fixture.root /
                                "game/assets/jak2/subtitle/subtitle_lines_en-US.json"),
      "subtitle_lines_en-US.json");
  const auto subtitle_meta =
      parse_commented_json(file_util::read_text_file(
                               fixture.root / "game/assets/jak2/subtitle/subtitle_meta_en-US.json"),
                           "subtitle_meta_en-US.json");
  const auto subtitle_package = read_json_values_v2(subtitle_lines, subtitle_meta);
  subtitle.m_speakers = subtitle_package.combined_lines.speakers;
  subtitle.m_speakers.emplace("none", "none");
  subtitle.add_scenes_from_files(subtitle_package);
  CHECK(public_generated_data_object_compiler::build_subtitle_v2(subtitle) ==
        desktop_subtitle_v2(subtitle));
  GameSubtitleDB desktop_subtitle_database;
  desktop_subtitle_database.m_banks.emplace(0, std::make_shared<GameSubtitleBank>(subtitle));
  compile_game_subtitles({}, desktop_subtitle_database, "desktop-wrapper-proof");
  CHECK(file_util::read_binary_file(
            file_util::get_file_path({"out", "desktop-wrapper-proof", "iso", "0SUBTI2.TXT"})) ==
        public_generated_data_object_compiler::build_subtitle_v2(subtitle));

  KoreanLookupDatabase korean_database;
  parse_commented_json(
      file_util::read_text_file(fixture.root / "game/assets/fonts/jak2_jak3_korean_db.json"),
      "jak2_jak3_korean_db.json")
      .get_to(korean_database);
  auto font = get_font_bank(GameTextVersion::JAK2);
  const std::string sample = "TEST \xed\x95\x9c\xea\xb8\x80";
  CHECK(font->convert_utf8_to_game_korean(sample, korean_database) ==
        font->convert_utf8_to_game_korean(sample));
  return true;
}

bool optional_local_oracle() {
  const char* extracted_root = std::getenv("OPENGOAL_JAK2_EXTRACTED_TREE");
  const char* prepared_root = std::getenv("OPENGOAL_JAK2_PREPARED_TREE");
  if (!extracted_root || !prepared_root) {
    return true;
  }
  const auto private_inputs =
      extracted::build({std::filesystem::path(extracted_root), proven_revision()});
  CHECK(private_inputs);
  const auto built = artifacts::build(private_inputs.value(), OPENGOAL_SOURCE_DIR);
  if (!built) {
    std::cerr << "oracle build failed: " << artifacts::error_code_name(built.error().code) << ": "
              << built.error().message << " [" << built.error().relative_path << "]";
    if (built.error().byte_offset) {
      std::cerr << " byte " << *built.error().byte_offset;
    }
    std::cerr << '\n';
  }
  CHECK(built);
  bool all_match = true;
  for (const auto& artifact : built.value().artifacts) {
    std::ifstream input(std::filesystem::path(prepared_root) / artifact.output_relative_path,
                        std::ios::binary | std::ios::ate);
    CHECK(input);
    const auto size = input.tellg();
    CHECK(size >= 0);
    std::vector<std::uint8_t> expected(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(expected.data()), size);
    CHECK(input || expected.empty());
    if (expected != artifact.bytes) {
      all_match = false;
      std::size_t first = 0;
      while (first < expected.size() && first < artifact.bytes.size() &&
             expected[first] == artifact.bytes[first]) {
        ++first;
      }
      std::cerr << "oracle mismatch " << artifact.output_relative_path
                << " expected=" << expected.size() << " actual=" << artifact.bytes.size()
                << " first=" << first << " expected-hash=" << std::hex
                << XXH64(expected.data(), expected.size(), 0) << " actual-hash=" << artifact.xxh64
                << std::dec << '\n';
      if (artifact.kind == artifacts::ArtifactKind::game_text) {
        const auto language =
            static_cast<std::uint32_t>(artifact.destination_basename.front() - '0');
        const auto expected_bank = jak1_extracted_generated_inputs::parse_checked_game_text(
            expected, language, artifact.destination_basename, artifact.output_relative_path);
        const auto actual_bank = jak1_extracted_generated_inputs::parse_checked_game_text(
            artifact.bytes, language, artifact.destination_basename, artifact.output_relative_path);
        if (expected_bank && actual_bank) {
          std::map<std::uint32_t, std::string> expected_lines;
          std::map<std::uint32_t, std::string> actual_lines;
          for (const auto& line : expected_bank.value().lines) {
            expected_lines.emplace(line.id, line.encoded_text);
          }
          for (const auto& line : actual_bank.value().lines) {
            actual_lines.emplace(line.id, line.encoded_text);
          }
          std::size_t differing = 0;
          for (const auto& [id, line] : expected_lines) {
            const auto found = actual_lines.find(id);
            if (found == actual_lines.end() || found->second != line) {
              ++differing;
            }
          }
          for (const auto& [id, line] : actual_lines) {
            (void)line;
            if (!expected_lines.contains(id)) {
              ++differing;
            }
          }
          std::cerr << "semantic counts expected=" << expected_lines.size()
                    << " actual=" << actual_lines.size() << " differing=" << differing << '\n';
        }
      }
    }
  }
  CHECK(all_match);
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      builds_exact_declared_output_set,
      rejects_revision_graph_and_input_drift,
      rejects_public_source_drift_symlinks_and_limits,
      cancellation_and_callback_failures_are_explicit,
      shared_emitters_are_byte_exact_with_desktop_generator,
      optional_local_oracle,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II public generated-artifact tests passed.\n";
  return 0;
}
