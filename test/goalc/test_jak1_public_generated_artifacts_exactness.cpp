#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "common/custom_data/Jak1PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak1PublicOutputGraph.h"

#include "goalc/data_compiler/DataObjectGenerator.h"

namespace artifacts = jak1_public_generated_artifacts;
namespace recipe = jak1_output_recipe;

namespace {

artifacts::Inputs synthetic_inputs(const jak1_output_graph::Graph& graph) {
  artifacts::Inputs inputs;
  inputs.directory_tpages.lengths = {1, 7, 16, 255};
  inputs.game_count.entries = {{50, 7}, {200, 11}, {0, 0}};
  inputs.game_count.unknown_1 = 0x1234;
  inputs.game_count.unknown_2 = 0x5678;

  std::uint32_t text_language = 0;
  std::uint32_t subtitle_language = 0;
  for (const auto& flat : graph.generated_flat_files) {
    if (flat.kind == recipe::GeneratedFlatFileKind::game_text) {
      inputs.game_text.push_back({flat.destination_basename,
                                  text_language++,
                                  "common",
                                  {{0x100, "synthetic line a"}, {0x200, "synthetic line b"}}});
    } else {
      inputs.subtitles.push_back(
          {flat.destination_basename,
           subtitle_language++,
           {{"scene-a", true, 0, {{30, "synthetic subtitle", "speaker", false}}},
            {"scene-b", false, 0x4321, {{10, "hint", "", true}, {20, "", "", true}}}}});
    }
  }
  return inputs;
}

std::vector<std::uint8_t> reference_directory_tpages(const artifacts::DirectoryTpages& input) {
  DataObjectGenerator generator;
  generator.add_type_tag("texture-page-dir");
  generator.add_word(input.lengths.size());
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
  generator.add_word(input.entries.size());
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
  generator.add_word(input.lines.size());
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
  generator.add_word(input.scenes.size());
  generator.add_word(input.language_id);
  generator.add_word(0);

  std::vector<int> array_links;
  for (const auto& scene : input.scenes) {
    generator.add_word((scene.is_cutscene ? 0u : 2u) | (scene.lines.size() << 16));
    array_links.push_back(generator.add_word(0));
    generator.add_ref_to_string_in_pool(scene.name);
    generator.add_word(scene.hint_id);
  }
  for (std::size_t scene_index = 0; scene_index < input.scenes.size(); ++scene_index) {
    generator.link_word_to_word(array_links[scene_index], generator.words());
    for (const auto& line : input.scenes[scene_index].lines) {
      generator.add_word(line.frame_start);
      generator.add_ref_to_string_in_pool(line.encoded_text);
      generator.add_ref_to_string_in_pool(line.encoded_speaker);
      generator.add_word(line.offscreen);
    }
  }
  return generator.generate_v2();
}

}  // namespace

int main() {
  const auto graph = jak1_public_output_graph::decode();
  if (!graph) {
    std::fputs("Could not decode the checked public graph.\n", stderr);
    return 1;
  }
  const auto inputs = synthetic_inputs(graph.value());
  const auto built = artifacts::build(graph.value(), inputs);
  if (!built) {
    std::fprintf(stderr, "Portable generation failed: %s\n", built.error().message.c_str());
    return 1;
  }

  std::map<std::string, std::vector<std::uint8_t>> expected;
  for (const auto& archive : graph.value().archives) {
    for (const auto& object : archive.objects) {
      if (object.producer == jak1_output_graph::ObjectProducerKind::directory_tpages) {
        expected.emplace("obj/" + object.prepared_basename,
                         reference_directory_tpages(inputs.directory_tpages));
      } else if (object.producer == jak1_output_graph::ObjectProducerKind::game_count) {
        expected.emplace("obj/" + object.prepared_basename,
                         reference_game_count(inputs.game_count));
      }
    }
  }
  for (const auto& bank : inputs.game_text) {
    expected.emplace("iso/" + bank.destination_basename, reference_game_text(bank));
  }
  for (const auto& bank : inputs.subtitles) {
    expected.emplace("iso/" + bank.destination_basename, reference_subtitles(bank));
  }

  if (expected.size() != built.value().artifacts.size()) {
    std::fputs("The portable builder did not produce the desktop output set.\n", stderr);
    return 1;
  }
  for (const auto& artifact : built.value().artifacts) {
    const auto found = expected.find(artifact.relative_path);
    if (found == expected.end() || found->second != artifact.bytes) {
      std::fputs("A portable artifact differs from the desktop data-object generator.\n", stderr);
      return 1;
    }
  }

  const auto base_graph = jak1_public_output_graph::decode_base_retail();
  if (!base_graph) {
    std::fputs("Could not decode the checked base-retail graph.\n", stderr);
    return 1;
  }
  auto empty_inputs = synthetic_inputs(base_graph.value());
  for (auto& bank : empty_inputs.subtitles) {
    bank.scenes.clear();
  }
  artifacts::Options empty_options;
  empty_options.subtitle_mode = artifacts::SubtitleMode::empty;
  const auto empty_built = artifacts::build(base_graph.value(), empty_inputs, empty_options);
  if (!empty_built) {
    std::fprintf(stderr, "Empty-subtitle generation failed: %s\n",
                 empty_built.error().message.c_str());
    return 1;
  }
  std::size_t empty_subtitle_files = 0;
  for (const auto& artifact : empty_built.value().artifacts) {
    if (artifact.flat_file_kind != recipe::GeneratedFlatFileKind::game_subtitle) {
      continue;
    }
    const auto bank = std::find_if(
        empty_inputs.subtitles.begin(), empty_inputs.subtitles.end(), [&](const auto& candidate) {
          return candidate.destination_basename == artifact.destination_basename;
        });
    if (bank == empty_inputs.subtitles.end() || artifact.bytes != reference_subtitles(*bank)) {
      std::fputs("An empty subtitle bank differs from the desktop data-object generator.\n",
                 stderr);
      return 1;
    }
    ++empty_subtitle_files;
  }
  if (empty_subtitle_files != 7) {
    std::fputs("The base-retail output does not contain seven empty subtitle banks.\n", stderr);
    return 1;
  }
  std::printf("All %zu checked public artifacts exactly match the desktop generator.\n",
              built.value().artifacts.size());
  return 0;
}
