#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <string>

#include "common/custom_data/Jak1PublicGeneratedArtifacts.h"
#include "common/custom_data/Jak1PublicOutputGraph.h"

namespace artifacts = jak1_public_generated_artifacts;
namespace recipe = jak1_output_recipe;

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

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
      artifacts::GameTextBank bank;
      bank.destination_basename = flat.destination_basename;
      bank.language_id = text_language++;
      bank.group_name = "common";
      bank.lines = {{0x100, "synthetic line a"}, {0x200, "synthetic line b"}};
      inputs.game_text.push_back(std::move(bank));
    } else {
      artifacts::SubtitleBank bank;
      bank.destination_basename = flat.destination_basename;
      bank.language_id = subtitle_language++;
      bank.scenes = {
          {"scene-a", true, 0, {{30, "synthetic subtitle", "speaker", false}}},
          {"scene-b", false, 0x4321, {{10, "hint", "", true}, {20, "", "", true}}},
      };
      inputs.subtitles.push_back(std::move(bank));
    }
  }
  return inputs;
}

bool builds_complete_deterministic_public_set() {
  const auto graph = jak1_public_output_graph::decode();
  CHECK(graph);
  const auto inputs = synthetic_inputs(graph.value());
  const auto first = artifacts::build(graph.value(), inputs);
  const auto second = artifacts::build(graph.value(), inputs);
  CHECK(first);
  CHECK(second);
  CHECK(first.value() == second.value());
  CHECK(first.value().artifacts.size() == graph.value().generated_flat_files.size() + 2);
  CHECK(first.value().total_bytes > 0);

  std::set<std::string> paths;
  std::uint64_t total = 0;
  std::size_t objects = 0;
  std::size_t flat_files = 0;
  for (const auto& artifact : first.value().artifacts) {
    CHECK(!artifact.bytes.empty());
    CHECK(artifact.bytes.size() % 16 == 0);
    CHECK(artifact.xxh64 != 0);
    CHECK(paths.emplace(artifact.relative_path).second);
    total += artifact.bytes.size();
    if (artifact.storage == artifacts::ArtifactStorage::object) {
      ++objects;
      CHECK(artifact.object_kind.has_value());
      CHECK(!artifact.flat_file_kind.has_value());
      CHECK(artifact.relative_path.starts_with("obj/"));
    } else {
      ++flat_files;
      CHECK(!artifact.object_kind.has_value());
      CHECK(artifact.flat_file_kind.has_value());
      CHECK(artifact.relative_path.starts_with("iso/"));
    }
  }
  CHECK(objects == 2);
  CHECK(flat_files == graph.value().generated_flat_files.size());
  CHECK(total == first.value().total_bytes);
  return true;
}

bool builds_only_structurally_empty_subtitle_banks() {
  const auto graph = jak1_public_output_graph::decode_base_retail();
  CHECK(graph);
  auto inputs = synthetic_inputs(graph.value());
  for (auto& bank : inputs.subtitles) {
    bank.scenes.clear();
  }
  artifacts::Options options;
  options.subtitle_mode = artifacts::SubtitleMode::empty;
  const auto built = artifacts::build(graph.value(), inputs, options);
  CHECK(built);

  std::size_t subtitle_files = 0;
  for (const auto& artifact : built.value().artifacts) {
    if (artifact.flat_file_kind != recipe::GeneratedFlatFileKind::game_subtitle) {
      continue;
    }
    ++subtitle_files;
    CHECK(!artifact.bytes.empty());
    CHECK(std::search(artifact.bytes.begin(), artifact.bytes.end(),
                      reinterpret_cast<const std::uint8_t*>("synthetic subtitle"),
                      reinterpret_cast<const std::uint8_t*>("synthetic subtitle") + 18) ==
          artifact.bytes.end());
  }
  CHECK(subtitle_files == 7);

  auto rejected = artifacts::build(graph.value(), inputs);
  CHECK(!rejected);
  CHECK(rejected.error().code == artifacts::ErrorCode::invalid_input);
  inputs.subtitles.front().scenes = {{"scene", true, 0, {}}};
  rejected = artifacts::build(graph.value(), inputs, options);
  CHECK(!rejected);
  CHECK(rejected.error().code == artifacts::ErrorCode::invalid_input);
  return true;
}

bool rejects_incomplete_or_noncanonical_inputs() {
  const auto graph = jak1_public_output_graph::decode();
  CHECK(graph);
  auto inputs = synthetic_inputs(graph.value());
  CHECK(!inputs.game_text.empty());
  inputs.game_text.pop_back();
  auto result = artifacts::build(graph.value(), inputs);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::missing_input);

  inputs = synthetic_inputs(graph.value());
  std::reverse(inputs.game_text.front().lines.begin(), inputs.game_text.front().lines.end());
  result = artifacts::build(graph.value(), inputs);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_input);

  inputs = synthetic_inputs(graph.value());
  inputs.subtitles.front().destination_basename = "UNDECLARED.TXT";
  result = artifacts::build(graph.value(), inputs);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_input);

  inputs = synthetic_inputs(graph.value());
  ++inputs.game_text.front().language_id;
  result = artifacts::build(graph.value(), inputs);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_input);

  inputs = synthetic_inputs(graph.value());
  inputs.directory_tpages.lengths.front() = 0x1000;
  result = artifacts::build(graph.value(), inputs);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::invalid_input);
  return true;
}

bool enforces_limits_and_cancellation() {
  const auto graph = jak1_public_output_graph::decode();
  CHECK(graph);
  const auto inputs = synthetic_inputs(graph.value());

  artifacts::Options options;
  options.limits.max_tpage_entries = 3;
  auto result = artifacts::build(graph.value(), inputs, options);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::limit_exceeded);

  options = {};
  options.should_cancel = [] { return true; };
  result = artifacts::build(graph.value(), inputs, options);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::cancelled);

  options.should_cancel = []() -> bool { throw 1; };
  result = artifacts::build(graph.value(), inputs, options);
  CHECK(!result);
  CHECK(result.error().code == artifacts::ErrorCode::callback_failed);
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      builds_complete_deterministic_public_set,
      builds_only_structurally_empty_subtitle_banks,
      rejects_incomplete_or_noncanonical_inputs,
      enforces_limits_and_cancellation,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak 1 public generated-artifact tests passed\n";
  return 0;
}
