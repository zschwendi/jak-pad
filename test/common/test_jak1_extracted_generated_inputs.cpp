#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/GoalDataObjectBuilder.h"

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"

namespace adapter = jak1_extracted_generated_inputs;
namespace artifacts = jak1_public_generated_artifacts;

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

void write_u32(std::vector<std::uint8_t>* output, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output->at(offset++) = static_cast<std::uint8_t>(value >> shift);
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

std::vector<std::uint8_t> directory_object() {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("texture-page-dir");
  builder.add_word(3);
  for (const auto length : {1u, 0x8007u, 0x8010u}) {
    builder.add_word(length);
    builder.add_symbol_link("#f");
    builder.add_symbol_link("#f");
  }
  return builder.generate_v4();
}

std::vector<std::uint8_t> game_count_object() {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-count-info");
  builder.add_word(2);
  builder.add_word(50);
  builder.add_word(7);
  builder.add_word(200);
  builder.add_word(11);
  builder.add_word(0x1234);
  builder.add_word(0x5678);
  return builder.generate_v4();
}

std::vector<std::uint8_t> odd_game_count_object() {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-count-info");
  builder.add_word(1);
  builder.add_word(50);
  builder.add_word(7);
  builder.add_word(0x1234);
  builder.add_word(0x5678);
  return builder.generate_v4();
}

std::vector<std::uint8_t> game_text_object(std::uint32_t language) {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(2);
  builder.add_word(language);
  builder.add_ref_to_string("common");
  builder.add_word(0x200);
  builder.add_ref_to_string("later-" + std::to_string(language));
  builder.add_word(0x100);
  builder.add_ref_to_string("earlier-" + std::to_string(language));
  return builder.generate_v2();
}

std::vector<std::uint8_t> large_game_text_object(std::uint32_t language) {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(300);
  builder.add_word(language);
  builder.add_ref_to_string("common");
  for (std::uint32_t id = 300; id > 0; --id) {
    builder.add_word(id - 1);
    builder.add_ref_to_string("line-" + std::to_string(id - 1));
  }
  return builder.generate_v2();
}

std::vector<std::uint8_t> misaligned_game_text_object(std::uint32_t language) {
  goal_data_object_builder::Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(2);
  builder.add_word(language);
  const auto group_pointer = builder.add_word(0);
  builder.add_word(0x100);
  builder.add_word(0);
  builder.add_word(0x200);
  builder.add_word(0);
  builder.add_word(0);
  builder.add_type_tag("string");
  const auto group_target = builder.add_word(6);
  builder.add_word(0x6d6d6f63);
  builder.add_word(0x00006e6f);
  builder.link_word_to_word(group_pointer, group_target);
  return builder.generate_v2();
}

std::vector<std::uint8_t> large_malformed_link_object() {
  constexpr std::size_t link_length = 1024 * 1024;
  std::vector<std::uint8_t> output(link_length + 16);
  write_u32(&output, 0, 0xffffffffu);
  write_u32(&output, 4, static_cast<std::uint32_t>(link_length));
  write_u32(&output, 8, 2);
  std::fill(output.begin() + 12, output.begin() + link_length, 0xff);
  return output;
}

adapter::PublicAdditions public_additions() {
  adapter::PublicAdditions additions;
  additions.game_text.push_back(
      {"0COMMON.TXT", 0, "common", {{0x180, "public-added"}, {0x200, "public-override"}}});
  for (std::uint32_t language = 0; language < 7; ++language) {
    additions.subtitles.push_back(
        {std::to_string(language) + "SUBTIT.TXT",
         language,
         {{"scene-z", true, 0, {{30, "line-z", "speaker", false}}},
          {"scene-a", false, 0x42, {{20, "hint-b", "", true}, {10, "hint-a", "", true}}}}});
  }
  std::reverse(additions.subtitles.begin(), additions.subtitles.end());
  return additions;
}

class Fixture {
 public:
  Fixture() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    root = std::filesystem::temp_directory_path() /
           ("opengoal-jak1-generated-input-test-" + std::to_string(stamp));
    std::filesystem::create_directories(root / "CGO");
    std::filesystem::create_directories(root / "TEXT");
    write(root / "CGO/GAME.CGO",
          make_dgo({{"dir-tpages", directory_object()}, {"game-cnt", game_count_object()}}));
    for (std::uint32_t language = 0; language < 7; ++language) {
      write(root / "TEXT" / (std::to_string(language) + "COMMON.TXT"), game_text_object(language));
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

  adapter::ValidatedTree tree() const { return {root, jak1_iso::default_revision()}; }

  std::filesystem::path root;
};

bool loads_and_canonicalizes_generated_inputs() {
  Fixture fixture;
  std::vector<adapter::Progress> progress;
  adapter::Options options;
  options.on_progress = [&](const auto& update) { progress.push_back(update); };
  const auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(result);
  CHECK(result.value().directory_tpages.lengths == std::vector<std::uint32_t>({1, 0x8007, 0x8010}));
  CHECK(result.value().game_count.entries.size() == 2);
  CHECK(result.value().game_count.entries[1].money == 200);
  CHECK(result.value().game_count.unknown_1 == 0x1234);
  CHECK(result.value().game_text.size() == 7);
  CHECK(result.value().game_text[0].lines.size() == 3);
  CHECK(result.value().game_text[0].lines[0].id == 0x100);
  CHECK(result.value().game_text[0].lines[1].id == 0x180);
  CHECK(result.value().game_text[0].lines[2].encoded_text == "public-override");
  CHECK(result.value().game_text[1].lines[0].encoded_text == "earlier-1");
  CHECK(result.value().subtitles.size() == 7);
  CHECK(result.value().subtitles[0].language_id == 0);
  CHECK(result.value().subtitles[0].scenes[0].name == "scene-a");
  CHECK(result.value().subtitles[0].scenes[0].lines[0].frame_start == 10);
  CHECK(result.value().subtitles[6].language_id == 6);
  CHECK(!progress.empty());
  CHECK(progress.front().stage == adapter::ProgressStage::opening_game_archive);
  CHECK(progress.back().stage == adapter::ProgressStage::complete);
  CHECK(progress.back().units_completed == progress.back().units_total);
  CHECK(std::is_sorted(progress.begin(), progress.end(), [](const auto& left, const auto& right) {
    return left.units_completed < right.units_completed;
  }));
  return true;
}

bool rejects_malformed_retail_pointer() {
  Fixture fixture;
  auto bytes = game_text_object(3);
  const auto code_offset = std::size_t(bytes[4]) | (std::size_t(bytes[5]) << 8) |
                           (std::size_t(bytes[6]) << 16) | (std::size_t(bytes[7]) << 24);
  const auto pointer_offset = code_offset + 3 * 4;
  std::fill(bytes.begin() + pointer_offset, bytes.begin() + pointer_offset + 4, 0xff);
  Fixture::write(fixture.root / "TEXT/3COMMON.TXT", bytes);
  const auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_pointer);
  CHECK(result.error().source_relative_path == "TEXT/3COMMON.TXT");
  CHECK(result.error().language_id == 3);
  return true;
}

bool decodes_long_link_runs_and_orders_text_ids() {
  Fixture fixture;
  Fixture::write(fixture.root / "TEXT/2COMMON.TXT", large_game_text_object(2));
  const auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(result);
  CHECK(result.value().game_text[2].lines.size() == 300);
  CHECK(result.value().game_text[2].lines.front().id == 0);
  CHECK(result.value().game_text[2].lines.back().id == 299);
  return true;
}

bool rejects_noncanonical_zero_extension() {
  Fixture fixture;
  auto bytes = game_text_object(5);
  bytes.resize(bytes.size() + 16);
  Fixture::write(fixture.root / "TEXT/5COMMON.TXT", bytes);
  const auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_data_object);
  CHECK(result.error().source_relative_path == "TEXT/5COMMON.TXT");
  return true;
}

bool rejects_noncanonical_string_alignment_and_odd_game_count() {
  Fixture fixture;
  Fixture::write(fixture.root / "TEXT/0COMMON.TXT", misaligned_game_text_object(0));
  auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_pointer);
  CHECK(result.error().source_relative_path == "TEXT/0COMMON.TXT");

  Fixture second;
  Fixture::write(second.root / "CGO/GAME.CGO", make_dgo({{"dir-tpages", directory_object()},
                                                         {"game-cnt", odd_game_count_object()}}));
  result = adapter::build(second.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_data_object);
  CHECK(result.error().source_relative_path == "CGO/GAME.CGO");
  return true;
}

bool preserves_checked_dgo_error() {
  Fixture fixture;
  Fixture::write(fixture.root / "CGO/GAME.CGO", {0, 1, 2, 3});
  const auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::checked_dgo_failed);
  CHECK(result.error().checked_dgo_error.has_value());
  CHECK(result.error().checked_dgo_error->code == jak1_checked_dgo::ErrorCode::truncated_header);
  return true;
}

bool rejects_missing_or_non_plain_inputs() {
  Fixture fixture;
  std::filesystem::remove(fixture.root / "TEXT/4COMMON.TXT");
  auto result = adapter::build(fixture.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::missing_input);

  Fixture second;
  std::filesystem::remove(second.root / "TEXT/4COMMON.TXT");
  std::filesystem::create_symlink(second.root / "TEXT/3COMMON.TXT",
                                  second.root / "TEXT/4COMMON.TXT");
  result = adapter::build(second.tree(), public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_extracted_tree);
  return true;
}

bool rejects_invalid_revision_and_public_data() {
  Fixture fixture;
  auto tree = fixture.tree();
  tree.revision.contents_hash ^= 1;
  auto result = adapter::build(tree, public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::unsupported_revision);

  tree = fixture.tree();
  tree.revision = jak1_iso::supported_revisions()[1];
  result = adapter::build(tree, public_additions());
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::unsupported_revision);

  auto additions = public_additions();
  additions.subtitles.pop_back();
  result = adapter::build(fixture.tree(), additions);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_public_data);

  additions = public_additions();
  additions.game_text[0].lines.push_back({0x180, "duplicate"});
  result = adapter::build(fixture.tree(), additions);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_public_data);

  additions = public_additions();
  additions.subtitles[0].scenes[0].lines.resize(65536);
  adapter::Options options;
  options.limits.max_subtitle_lines_per_scene = 65536;
  result = adapter::build(fixture.tree(), additions, options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::invalid_public_data);
  return true;
}

bool reports_cancellation_and_callback_failures() {
  Fixture fixture;
  adapter::Options options;
  options.should_cancel = [] { return true; };
  auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::cancelled);

  options.should_cancel = []() -> bool { throw std::runtime_error("cancel failed"); };
  result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::callback_failed);

  options.should_cancel = {};
  options.on_progress = [](const auto&) { throw std::runtime_error("progress failed"); };
  result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::callback_failed);
  return true;
}

bool polls_cancellation_during_link_decode_and_public_merge() {
  Fixture fixture;
  Fixture::write(fixture.root / "TEXT/0COMMON.TXT", large_malformed_link_object());

  bool inside_text = false;
  std::size_t polls = 0;
  adapter::Options options;
  options.limits.file_read_chunk_bytes = 2 * 1024 * 1024;
  options.on_progress = [&](const adapter::Progress& progress) {
    inside_text =
        progress.stage == adapter::ProgressStage::reading_game_text && progress.language_id == 0;
  };
  options.should_cancel = [&] { return inside_text && ++polls >= 6; };
  auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::cancelled);
  CHECK(result.error().source_relative_path == "TEXT/0COMMON.TXT");
  CHECK(result.error().language_id == 0);
  CHECK(result.error().byte_offset.has_value());

  inside_text = false;
  polls = 0;
  options.should_cancel = [&] {
    if (inside_text && ++polls >= 6) {
      throw std::runtime_error("cancel failed in decoder");
    }
    return false;
  };
  result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::callback_failed);
  CHECK(result.error().source_relative_path == "TEXT/0COMMON.TXT");
  CHECK(result.error().byte_offset.has_value());

  Fixture merge_fixture;
  auto additions = public_additions();
  additions.game_text[0].lines.clear();
  for (std::uint32_t id = 0; id < 2048; ++id) {
    additions.game_text[0].lines.push_back({0x1000 + id, "public"});
  }
  bool inside_merge = false;
  polls = 0;
  options = {};
  options.on_progress = [&](const adapter::Progress& progress) {
    inside_merge = progress.stage == adapter::ProgressStage::merging_public_data;
  };
  options.should_cancel = [&] { return inside_merge && ++polls >= 4; };
  result = adapter::build(merge_fixture.tree(), additions, options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::cancelled);
  CHECK(result.error().source_relative_path == "0COMMON.TXT");
  return true;
}

bool enforces_relocation_count_bounds() {
  Fixture fixture;
  adapter::Options options;
  options.limits.max_pointer_links = 2;
  auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::limit_exceeded);
  CHECK(result.error().source_relative_path == "TEXT/0COMMON.TXT");

  Fixture second;
  options = {};
  options.limits.max_named_links = 1;
  result = adapter::build(second.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::limit_exceeded);
  CHECK(result.error().source_relative_path == "CGO/GAME.CGO");
  return true;
}

bool enforces_direct_object_bound() {
  Fixture fixture;
  adapter::Options options;
  options.limits.max_direct_object_bytes = 16;
  const auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::input_too_large);
  CHECK(result.error().source_relative_path == "TEXT/0COMMON.TXT");
  return true;
}

bool enforces_generated_bank_string_bound() {
  Fixture fixture;
  adapter::Options options;
  options.limits.max_generated_bank_string_bytes = 12;
  const auto result = adapter::build(fixture.tree(), public_additions(), options);
  CHECK(!result);
  CHECK(result.error().code == adapter::ErrorCode::limit_exceeded);
  CHECK(result.error().source_relative_path == "0COMMON.TXT");
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      std::pair{"loads_and_canonicalizes_generated_inputs",
                loads_and_canonicalizes_generated_inputs},
      std::pair{"rejects_malformed_retail_pointer", rejects_malformed_retail_pointer},
      std::pair{"decodes_long_link_runs_and_orders_text_ids",
                decodes_long_link_runs_and_orders_text_ids},
      std::pair{"rejects_noncanonical_zero_extension", rejects_noncanonical_zero_extension},
      std::pair{"rejects_noncanonical_string_alignment_and_odd_game_count",
                rejects_noncanonical_string_alignment_and_odd_game_count},
      std::pair{"preserves_checked_dgo_error", preserves_checked_dgo_error},
      std::pair{"rejects_missing_or_non_plain_inputs", rejects_missing_or_non_plain_inputs},
      std::pair{"rejects_invalid_revision_and_public_data",
                rejects_invalid_revision_and_public_data},
      std::pair{"reports_cancellation_and_callback_failures",
                reports_cancellation_and_callback_failures},
      std::pair{"polls_cancellation_during_link_decode_and_public_merge",
                polls_cancellation_during_link_decode_and_public_merge},
      std::pair{"enforces_relocation_count_bounds", enforces_relocation_count_bounds},
      std::pair{"enforces_direct_object_bound", enforces_direct_object_bound},
      std::pair{"enforces_generated_bank_string_bound", enforces_generated_bank_string_bound},
  };
  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
  }
  std::cout << "All " << tests.size() << " extracted generated-input tests passed.\n";
  return 0;
}
