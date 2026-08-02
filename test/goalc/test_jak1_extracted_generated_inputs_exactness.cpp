#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak1PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak1PublicOutputGraph.h"

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"
#include "goalc/data_compiler/DataObjectGenerator.h"

namespace adapter = jak1_extracted_generated_inputs;
namespace artifacts = jak1_public_generated_artifacts;
namespace recipe = jak1_output_recipe;

namespace {

void append_u32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_name(std::vector<std::uint8_t>* output, const std::string& name) {
  output->insert(output->end(), name.begin(), name.end());
  output->resize(output->size() + 60 - name.size());
}

std::vector<std::uint8_t> make_dgo(
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> objects) {
  std::vector<std::uint8_t> output;
  append_u32(&output, static_cast<std::uint32_t>(objects.size()));
  append_name(&output, "GAME.CGO");
  for (auto& [name, bytes] : objects) {
    append_u32(&output, static_cast<std::uint32_t>(bytes.size()));
    append_name(&output, name);
    output.insert(output.end(), bytes.begin(), bytes.end());
    while (output.size() % 16) {
      output.push_back(0);
    }
  }
  return output;
}

std::vector<std::uint8_t> reference_directory_tpages(const artifacts::DirectoryTpages& input) {
  DataObjectGenerator generator;
  generator.add_type_tag("texture-page-dir");
  generator.add_word(static_cast<std::uint32_t>(input.lengths.size()));
  for (const auto length : input.lengths) {
    generator.add_word(length);
    generator.add_symbol_link("#f");
    generator.add_symbol_link("#f");
  }
  return generator.generate_v4();
}

std::vector<std::uint8_t> reference_game_count(const artifacts::GameCount& input) {
  DataObjectGenerator generator;
  generator.add_type_tag("game-count-info");
  generator.add_word(static_cast<std::uint32_t>(input.entries.size()));
  for (const auto& entry : input.entries) {
    generator.add_word(entry.money);
    generator.add_word(entry.buzzer);
  }
  generator.add_word(input.unknown_1);
  generator.add_word(input.unknown_2);
  return generator.generate_v4();
}

std::vector<std::uint8_t> reference_game_text(const artifacts::GameTextBank& input) {
  DataObjectGenerator generator;
  generator.add_type_tag("game-text-info");
  generator.add_word(static_cast<std::uint32_t>(input.lines.size()));
  generator.add_word(input.language_id);
  generator.add_ref_to_string_in_pool(input.group_name);
  for (const auto& line : input.lines) {
    generator.add_word(line.id);
    generator.add_ref_to_string_in_pool(line.encoded_text);
  }
  return generator.generate_v2();
}

std::vector<std::uint8_t> reference_subtitles(const artifacts::SubtitleBank& input) {
  DataObjectGenerator generator;
  generator.add_type_tag("subtitle-text-info");
  generator.add_word(static_cast<std::uint32_t>(input.scenes.size()));
  generator.add_word(input.language_id);
  generator.add_word(0);
  std::vector<int> line_links;
  for (const auto& scene : input.scenes) {
    generator.add_word((scene.is_cutscene ? 0u : 2u) |
                       (static_cast<std::uint32_t>(scene.lines.size()) << 16));
    line_links.push_back(generator.add_word(0));
    generator.add_ref_to_string_in_pool(scene.name);
    generator.add_word(scene.hint_id);
  }
  for (std::size_t scene_index = 0; scene_index < input.scenes.size(); ++scene_index) {
    generator.link_word_to_word(line_links[scene_index], generator.words());
    for (const auto& line : input.scenes[scene_index].lines) {
      generator.add_word(line.frame_start);
      generator.add_ref_to_string_in_pool(line.encoded_text);
      generator.add_ref_to_string_in_pool(line.encoded_speaker);
      generator.add_word(line.offscreen ? 1 : 0);
    }
  }
  return generator.generate_v2();
}

class TemporaryTree {
 public:
  TemporaryTree() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("opengoal-jak1-generated-input-exactness-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "CGO");
    std::filesystem::create_directories(root / "TEXT");
  }

  ~TemporaryTree() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  void write(const std::filesystem::path& relative, const std::vector<std::uint8_t>& bytes) const {
    std::ofstream output(root / relative, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }

  std::filesystem::path root;
};

bool same_inputs(const artifacts::Inputs& left, const artifacts::Inputs& right) {
  if (left.directory_tpages.lengths != right.directory_tpages.lengths ||
      left.game_count.unknown_1 != right.game_count.unknown_1 ||
      left.game_count.unknown_2 != right.game_count.unknown_2 ||
      left.game_count.entries.size() != right.game_count.entries.size() ||
      left.game_text.size() != right.game_text.size() ||
      left.subtitles.size() != right.subtitles.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.game_count.entries.size(); ++index) {
    if (left.game_count.entries[index].money != right.game_count.entries[index].money ||
        left.game_count.entries[index].buzzer != right.game_count.entries[index].buzzer) {
      return false;
    }
  }
  for (std::size_t bank_index = 0; bank_index < left.game_text.size(); ++bank_index) {
    const auto& left_bank = left.game_text[bank_index];
    const auto& right_bank = right.game_text[bank_index];
    if (left_bank.destination_basename != right_bank.destination_basename ||
        left_bank.language_id != right_bank.language_id ||
        left_bank.group_name != right_bank.group_name ||
        left_bank.lines.size() != right_bank.lines.size()) {
      return false;
    }
    for (std::size_t line_index = 0; line_index < left_bank.lines.size(); ++line_index) {
      if (left_bank.lines[line_index].id != right_bank.lines[line_index].id ||
          left_bank.lines[line_index].encoded_text != right_bank.lines[line_index].encoded_text) {
        return false;
      }
    }
  }
  for (std::size_t bank_index = 0; bank_index < left.subtitles.size(); ++bank_index) {
    const auto& left_bank = left.subtitles[bank_index];
    const auto& right_bank = right.subtitles[bank_index];
    if (left_bank.destination_basename != right_bank.destination_basename ||
        left_bank.language_id != right_bank.language_id ||
        left_bank.scenes.size() != right_bank.scenes.size()) {
      return false;
    }
    for (std::size_t scene_index = 0; scene_index < left_bank.scenes.size(); ++scene_index) {
      const auto& left_scene = left_bank.scenes[scene_index];
      const auto& right_scene = right_bank.scenes[scene_index];
      if (left_scene.name != right_scene.name ||
          left_scene.is_cutscene != right_scene.is_cutscene ||
          left_scene.hint_id != right_scene.hint_id ||
          left_scene.lines.size() != right_scene.lines.size()) {
        return false;
      }
      for (std::size_t line_index = 0; line_index < left_scene.lines.size(); ++line_index) {
        const auto& left_line = left_scene.lines[line_index];
        const auto& right_line = right_scene.lines[line_index];
        if (left_line.frame_start != right_line.frame_start ||
            left_line.encoded_text != right_line.encoded_text ||
            left_line.encoded_speaker != right_line.encoded_speaker ||
            left_line.offscreen != right_line.offscreen) {
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace

int main() {
  const auto graph = jak1_public_output_graph::decode();
  if (!graph) {
    std::fputs("Could not decode the checked public graph.\n", stderr);
    return 1;
  }

  artifacts::DirectoryTpages directory{{1, 7, 16, 255}};
  artifacts::GameCount counts{{{50, 7}, {200, 11}}, 0x1234, 0x5678};
  artifacts::Inputs expected_inputs;
  expected_inputs.directory_tpages = directory;
  expected_inputs.game_count = counts;
  std::vector<artifacts::GameTextBank> retail_text;
  adapter::PublicAdditions additions;
  for (std::uint32_t language = 0; language < 7; ++language) {
    retail_text.push_back({std::to_string(language) + "COMMON.TXT",
                           language,
                           "common",
                           {{0x100, "retail-a-" + std::to_string(language)},
                            {0x200, "retail-b-" + std::to_string(language)}}});
    additions.game_text.push_back({std::to_string(language) + "COMMON.TXT",
                                   language,
                                   "common",
                                   {{0x180, "public-a-" + std::to_string(language)},
                                    {0x200, "public-b-" + std::to_string(language)}}});
    additions.subtitles.push_back(
        {std::to_string(language) + "SUBTIT.TXT",
         language,
         {{"scene-b", true, 0, {{30, "subtitle", "speaker", false}}},
          {"scene-a", false, 0x4321, {{10, "hint", "", true}, {20, "", "", true}}}}});
    expected_inputs.game_text.push_back({std::to_string(language) + "COMMON.TXT",
                                         language,
                                         "common",
                                         {{0x100, "retail-a-" + std::to_string(language)},
                                          {0x180, "public-a-" + std::to_string(language)},
                                          {0x200, "public-b-" + std::to_string(language)}}});
    expected_inputs.subtitles.push_back(
        {std::to_string(language) + "SUBTIT.TXT",
         language,
         {{"scene-a", false, 0x4321, {{10, "hint", "", true}, {20, "", "", true}}},
          {"scene-b", true, 0, {{30, "subtitle", "speaker", false}}}}});
  }

  TemporaryTree tree;
  tree.write("CGO/GAME.CGO", make_dgo({{"dir-tpages", reference_directory_tpages(directory)},
                                       {"game-cnt", reference_game_count(counts)}}));
  for (const auto& bank : retail_text) {
    tree.write(std::filesystem::path("TEXT") / bank.destination_basename,
               reference_game_text(bank));
  }

  const auto loaded = adapter::build({tree.root, jak1_iso::default_revision()}, additions);
  if (!loaded) {
    std::fprintf(stderr, "Portable input loading failed: %s\n", loaded.error().message.c_str());
    return 1;
  }
  if (!same_inputs(loaded.value(), expected_inputs)) {
    std::fputs("The portable adapter differs from the independently expected desktop inputs.\n",
               stderr);
    return 1;
  }
  const auto built = artifacts::build(graph.value(), loaded.value());
  if (!built) {
    std::fprintf(stderr, "Portable artifact generation failed: %s\n",
                 built.error().message.c_str());
    return 1;
  }

  std::map<std::string, std::vector<std::uint8_t>> expected;
  for (const auto& archive : graph.value().archives) {
    for (const auto& object : archive.objects) {
      if (object.producer == jak1_output_graph::ObjectProducerKind::directory_tpages) {
        expected.emplace("obj/" + object.prepared_basename,
                         reference_directory_tpages(expected_inputs.directory_tpages));
      } else if (object.producer == jak1_output_graph::ObjectProducerKind::game_count) {
        expected.emplace("obj/" + object.prepared_basename,
                         reference_game_count(expected_inputs.game_count));
      }
    }
  }
  for (const auto& bank : expected_inputs.game_text) {
    expected.emplace("iso/" + bank.destination_basename, reference_game_text(bank));
  }
  for (const auto& bank : expected_inputs.subtitles) {
    expected.emplace("iso/" + bank.destination_basename, reference_subtitles(bank));
  }

  if (expected.size() != built.value().artifacts.size()) {
    std::fputs("The adapter did not produce the complete public generated-input set.\n", stderr);
    return 1;
  }
  for (const auto& artifact : built.value().artifacts) {
    const auto found = expected.find(artifact.relative_path);
    if (found == expected.end() || found->second != artifact.bytes) {
      std::fputs("An adapter-fed artifact differs from the desktop data-object generator.\n",
                 stderr);
      return 1;
    }
  }

  const auto base_graph = jak1_public_output_graph::decode_base_retail();
  if (!base_graph) {
    std::fputs("Could not decode the checked base-retail graph.\n", stderr);
    return 1;
  }
  adapter::Options empty_load_options;
  empty_load_options.subtitle_mode = artifacts::SubtitleMode::empty;
  const auto empty_loaded =
      adapter::build({tree.root, jak1_iso::default_revision()}, {}, empty_load_options);
  if (!empty_loaded) {
    std::fprintf(stderr, "Empty-subtitle input loading failed: %s\n",
                 empty_loaded.error().message.c_str());
    return 1;
  }
  artifacts::Inputs expected_empty_inputs;
  expected_empty_inputs.directory_tpages = directory;
  expected_empty_inputs.game_count = counts;
  expected_empty_inputs.game_text = retail_text;
  for (std::uint32_t language = 0; language < 7; ++language) {
    expected_empty_inputs.subtitles.push_back(
        {std::to_string(language) + "SUBTIT.TXT", language, {}});
  }
  if (!same_inputs(empty_loaded.value(), expected_empty_inputs)) {
    std::fputs("The base-retail adapter changed retail text or retained subtitle content.\n",
               stderr);
    return 1;
  }
  artifacts::Options empty_build_options;
  empty_build_options.subtitle_mode = artifacts::SubtitleMode::empty;
  const auto empty_built =
      artifacts::build(base_graph.value(), empty_loaded.value(), empty_build_options);
  if (!empty_built) {
    std::fprintf(stderr, "Empty-subtitle artifact generation failed: %s\n",
                 empty_built.error().message.c_str());
    return 1;
  }
  std::size_t checked_empty_subtitles = 0;
  for (const auto& artifact : empty_built.value().artifacts) {
    if (artifact.flat_file_kind != recipe::GeneratedFlatFileKind::game_subtitle) {
      continue;
    }
    const auto bank =
        std::find_if(empty_loaded.value().subtitles.begin(), empty_loaded.value().subtitles.end(),
                     [&](const auto& candidate) {
                       return candidate.destination_basename == artifact.destination_basename;
                     });
    if (bank == empty_loaded.value().subtitles.end() ||
        artifact.bytes != reference_subtitles(*bank)) {
      std::fputs("An adapter-fed empty subtitle differs from the desktop generator.\n", stderr);
      return 1;
    }
    ++checked_empty_subtitles;
  }
  if (checked_empty_subtitles != 7) {
    std::fputs("The adapter did not generate seven empty subtitle banks.\n", stderr);
    return 1;
  }
  std::printf("All %zu adapter-fed artifacts exactly match the desktop generator.\n",
              built.value().artifacts.size());
  return 0;
}
