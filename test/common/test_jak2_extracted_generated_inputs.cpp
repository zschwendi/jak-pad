#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Jak2PublicOutputGraph.h"

#include "decompiler/extractor/jak2_extracted_generated_inputs.h"

namespace adapter = jak2_extracted_generated_inputs;
namespace graph = jak1_output_graph;
namespace recipe = jak1_output_recipe;

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

void append_u32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_name(std::vector<std::uint8_t>* output, const std::string& name) {
  output->insert(output->end(), name.begin(), name.end());
  output->resize(output->size() + 60 - name.size());
}

struct DgoObject {
  std::string name;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::uint8_t> make_dgo(std::vector<DgoObject> objects) {
  std::vector<std::uint8_t> output;
  append_u32(&output, static_cast<std::uint32_t>(objects.size()));
  append_name(&output, "GAME.CGO");
  for (auto& object : objects) {
    append_u32(&output, static_cast<std::uint32_t>(object.bytes.size()));
    append_name(&output, object.name);
    output.insert(output.end(), object.bytes.begin(), object.bytes.end());
    while (output.size() % 16) {
      output.push_back(0);
    }
  }
  return output;
}

const jak2_iso::Revision& proven_revision() {
  for (const auto& revision : jak2_iso::supported_revisions()) {
    if (revision.contents_hash == 18208811100399420450ull) {
      return revision;
    }
  }
  throw std::runtime_error("proven Jak II revision missing");
}

std::vector<std::uint8_t> text_bytes(std::uint32_t language) {
  return {0xff, 0xff, 0xff, 0xff, static_cast<std::uint8_t>(language), 0x43, 0x4f,
          0x4d, 0x4d, 0x4f, 0x4e};
}

class Fixture {
 public:
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("opengoal-jak2-generated-input-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "CGO");
    std::filesystem::create_directories(root / "TEXT");
    write(root / "CGO/GAME.CGO",
          make_dgo({{"unrelated", {4, 5, 6}}, {"dir-tpages", directory_bytes}}));
    for (std::uint32_t language = 0; language < 8; ++language) {
      write(root / "TEXT" / (std::to_string(language) + "COMMON.TXT"), text_bytes(language));
    }
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  static void write(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
      throw std::runtime_error("fixture write failed");
    }
  }

  adapter::ValidatedTree tree() const { return {root, proven_revision()}; }

  std::filesystem::path root;
  const std::vector<std::uint8_t> directory_bytes = {0xff, 0xff, 0xff, 0xff, 0x44, 0x49, 0x52};
};

bool loads_nine_retail_inputs_and_public_requirement() {
  Fixture fixture;
  std::vector<adapter::Progress> progress;
  adapter::Options options;
  options.on_progress = [&](const auto& update) { progress.push_back(update); };

  const auto first = adapter::build(fixture.tree(), options);
  CHECK(first);
  const auto second = adapter::build(fixture.tree());
  CHECK(second);
  CHECK(first.value() == second.value());
  CHECK(first.value().retail.size() == 9);
  CHECK(first.value().retail.front().source_kind == adapter::RetailSourceKind::archive_object);
  CHECK(first.value().retail.front().object_kind == recipe::GeneratedDataKind::directory_tpages);
  CHECK(!first.value().retail.front().flat_file_kind);
  CHECK(first.value().retail.front().source_relative_path == "CGO/GAME.CGO");
  CHECK(first.value().retail.front().internal_name == "dir-tpages");
  CHECK(first.value().retail.front().destination_basename == "dir-tpages.go");
  CHECK(first.value().retail.front().output_relative_path == "obj/dir-tpages.go");
  CHECK(first.value().retail.front().bytes == fixture.directory_bytes);
  CHECK(first.value().retail.front().archive_object_index == 1);

  std::uint64_t expected_bytes = fixture.directory_bytes.size();
  for (std::uint32_t language = 0; language < 8; ++language) {
    const auto& input = first.value().retail.at(language + 1);
    const auto destination = std::to_string(language) + "COMMON.TXT";
    CHECK(input.source_kind == adapter::RetailSourceKind::direct_file);
    CHECK(!input.object_kind);
    CHECK(input.flat_file_kind == recipe::GeneratedFlatFileKind::game_text);
    CHECK(input.source_relative_path == "TEXT/" + destination);
    CHECK(!input.archive_object_index);
    CHECK(input.internal_name.empty());
    CHECK(input.destination_basename == destination);
    CHECK(input.output_relative_path == "iso/" + destination);
    CHECK(input.bytes == text_bytes(language));
    expected_bytes += input.bytes.size();
  }
  CHECK(first.value().retail_bytes == expected_bytes);

  const auto& public_input = first.value().public_subtitle_v2;
  CHECK(public_input.output_kind == recipe::GeneratedFlatFileKind::game_subtitle);
  CHECK(public_input.destination_basename == "0SUBTI2.TXT");
  CHECK(public_input.output_relative_path == "iso/0SUBTI2.TXT");
  CHECK(public_input.project_relative_path == "game/assets/jak2/game_subtitle.gp");
  CHECK(public_input.compiler_tool == "subtitle-v2");
  CHECK(progress.size() == 12);
  CHECK(progress.front().stage == adapter::ProgressStage::validating_public_graph);
  CHECK(progress.back().stage == adapter::ProgressStage::complete);
  CHECK(progress.back().units_completed == progress.back().units_total);
  CHECK(progress.back().units_total == 10);
  return true;
}

bool rejects_unproven_revision_and_graph_shape() {
  Fixture fixture;
  auto wrong_tree = fixture.tree();
  wrong_tree.revision = jak2_iso::default_revision();
  auto result = adapter::build(wrong_tree);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::unsupported_revision);

  auto decoded = jak2_public_output_graph::decode_base_retail();
  CHECK(decoded);
  auto missing_subtitle = decoded.take_value();
  const auto removed = std::erase_if(missing_subtitle.generated_flat_files, [](const auto& flat) {
    return flat.destination_basename == "0SUBTI2.TXT";
  });
  CHECK(removed == 1);
  result = adapter::build(fixture.tree(), missing_subtitle);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::unsupported_public_graph);

  auto unexpected_count = jak2_public_output_graph::decode_base_retail();
  CHECK(unexpected_count);
  auto graph_with_count = unexpected_count.take_value();
  CHECK(!graph_with_count.archives.empty());
  graph_with_count.archives.front().objects.push_back(
      {"game-cnt.go", "game-cnt", graph::ObjectProducerKind::game_count, {}});
  result = adapter::build(fixture.tree(), graph_with_count);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::unsupported_public_graph);
  return true;
}

bool rejects_missing_symlinked_and_oversized_inputs() {
  {
    Fixture fixture;
    Fixture::write(fixture.root / "CGO/GAME.CGO", make_dgo({{"unrelated", {4, 5, 6}}}));
    const auto result = adapter::build(fixture.tree());
    CHECK(!result);
    CHECK(result.error().code == adapter::ErrorCode::missing_retail_object);
  }
  {
    Fixture fixture;
    std::filesystem::remove(fixture.root / "TEXT/7COMMON.TXT");
    const auto result = adapter::build(fixture.tree());
    CHECK(!result);
    CHECK(result.error().code == adapter::ErrorCode::missing_input);
    CHECK(result.error().source_relative_path == "TEXT/7COMMON.TXT");
  }
  {
    Fixture fixture;
    const auto outside =
        fixture.root.parent_path() / (fixture.root.filename().string() + "-outside");
    Fixture::write(outside, text_bytes(7));
    std::filesystem::remove(fixture.root / "TEXT/7COMMON.TXT");
    std::error_code error;
    std::filesystem::create_symlink(outside, fixture.root / "TEXT/7COMMON.TXT", error);
    CHECK(!error);
    const auto result = adapter::build(fixture.tree());
    CHECK(!result);
    CHECK(result.error().code == adapter::ErrorCode::invalid_extracted_tree);
    std::filesystem::remove(outside, error);
  }
  {
    Fixture fixture;
    adapter::Options options;
    options.limits.max_direct_input_bytes = 10;
    options.limits.max_total_retail_bytes = 100;
    const auto result = adapter::build(fixture.tree(), options);
    CHECK(!result);
    CHECK(result.error().code == adapter::ErrorCode::input_too_large);
  }
  return true;
}

bool cancellation_and_callback_failures_are_explicit() {
  Fixture fixture;
  adapter::Options cancelled;
  cancelled.should_cancel = [] { return true; };
  auto result = adapter::build(fixture.tree(), cancelled);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::cancelled);

  adapter::Options cancel_failed;
  cancel_failed.should_cancel = []() -> bool { throw std::runtime_error("cancel failure"); };
  result = adapter::build(fixture.tree(), cancel_failed);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::callback_failed);

  adapter::Options progress_failed;
  progress_failed.on_progress = [](const auto&) { throw std::runtime_error("progress failure"); };
  result = adapter::build(fixture.tree(), progress_failed);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::callback_failed);
  return true;
}

bool optional_local_oracle() {
  const char* root = std::getenv("OPENGOAL_JAK2_EXTRACTED_TREE");
  if (!root || !*root) {
    return true;
  }
  const auto result = adapter::build({std::filesystem::path(root), proven_revision()});
  CHECK(result);
  CHECK(result.value().retail.size() == 9);
  CHECK(result.value().retail_bytes > 0);
  CHECK(result.value().public_subtitle_v2.destination_basename == "0SUBTI2.TXT");
  std::cout << "Read-only Jak II v2 oracle accepted 9 retail generated inputs ("
            << result.value().retail_bytes
            << " bytes) plus the explicit public-only 0SUBTI2.TXT requirement.\n";
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      loads_nine_retail_inputs_and_public_requirement,
      rejects_unproven_revision_and_graph_shape,
      rejects_missing_symlinked_and_oversized_inputs,
      cancellation_and_callback_failures_are_explicit,
      optional_local_oracle,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II extracted-tree generated-input tests passed.\n";
  return 0;
}
