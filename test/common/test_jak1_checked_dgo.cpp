#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "decompiler/extractor/jak1_checked_dgo.h"

#include "third-party/lzokay/lzokay.hpp"

namespace {

using jak1_checked_dgo::ErrorCode;

#define CHECK(condition)                                                                   \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition \
                << '\n';                                                                   \
      return false;                                                                        \
    }                                                                                      \
  } while (false)

void append_u32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  output->push_back(value & 0xff);
  output->push_back((value >> 8) & 0xff);
  output->push_back((value >> 16) & 0xff);
  output->push_back(value >> 24);
}

void append_name(std::vector<std::uint8_t>* output, const std::string& name) {
  const auto begin = output->size();
  output->resize(begin + 60);
  std::copy(name.begin(), name.end(), output->begin() + begin);
}

struct FixtureObject {
  std::string name;
  std::vector<std::uint8_t> data;
};

std::vector<std::uint8_t> make_raw_dgo(const std::string& name,
                                       const std::vector<FixtureObject>& objects) {
  std::vector<std::uint8_t> output;
  append_u32(&output, objects.size());
  append_name(&output, name);
  for (const auto& object : objects) {
    append_u32(&output, object.data.size());
    append_name(&output, object.name);
    output.insert(output.end(), object.data.begin(), object.data.end());
    while (output.size() % 16) {
      output.push_back(0);
    }
  }
  return output;
}

std::vector<std::uint8_t> make_blzo(const std::vector<std::uint8_t>& expanded,
                                    bool force_first_raw = false) {
  constexpr std::size_t kBlockSize = 0x8000;
  std::vector<std::uint8_t> output{'o', 'Z', 'l', 'B'};
  append_u32(&output, expanded.size());
  std::size_t offset = 0;
  bool first = true;
  while (offset < expanded.size()) {
    const auto block_size = std::min(kBlockSize, expanded.size() - offset);
    if ((force_first_raw && first && block_size == kBlockSize)) {
      append_u32(&output, kBlockSize);
      output.insert(output.end(), expanded.begin() + offset,
                    expanded.begin() + offset + block_size);
    } else {
      std::vector<std::uint8_t> compressed(lzokay::compress_worst_size(block_size));
      auto compressed_size = compressed.size();
      const auto status = lzokay::compress(expanded.data() + offset, block_size, compressed.data(),
                                           compressed.size(), compressed_size);
      if (status != lzokay::EResult::Success || compressed_size >= kBlockSize) {
        if (block_size != kBlockSize) {
          return {};
        }
        append_u32(&output, kBlockSize);
        output.insert(output.end(), expanded.begin() + offset,
                      expanded.begin() + offset + block_size);
      } else {
        append_u32(&output, compressed_size);
        output.insert(output.end(), compressed.begin(), compressed.begin() + compressed_size);
      }
    }
    while (output.size() % 4) {
      output.push_back(0);
    }
    offset += block_size;
    first = false;
  }
  return output;
}

bool valid_raw_preserves_order_and_names() {
  const auto fixture =
      make_raw_dgo("VIL1.DGO", {{"first", {1, 2, 3}}, {"second", {4, 5, 6, 7, 8}}});
  const auto result = jak1_checked_dgo::read(fixture, "VIL1.DGO");
  CHECK(result);
  CHECK(!result.value().was_compressed);
  CHECK(result.value().input_size == fixture.size());
  CHECK(result.value().expanded_size == fixture.size());
  CHECK(result.value().internal_name == "VIL1.DGO");
  CHECK(result.value().objects.size() == 2);
  CHECK(result.value().objects[0].internal_name == "first");
  CHECK(result.value().objects[0].unique_name == "first");
  CHECK(result.value().objects[0].data == std::vector<std::uint8_t>({1, 2, 3}));
  CHECK(result.value().objects[1].internal_name == "second");
  CHECK(result.value().objects[1].data == std::vector<std::uint8_t>({4, 5, 6, 7, 8}));
  return true;
}

bool derives_art_group_and_duplicate_names() {
  const std::string marker = "/src/next/data/art-group6/plat-ag.go";
  std::vector<std::uint8_t> art_data(marker.begin(), marker.end());
  art_data.push_back(0);
  const auto fixture =
      make_raw_dgo("ART.DGO", {{"plat", art_data}, {"same", {1, 2}}, {"same", {3, 4, 5}}});
  const auto result = jak1_checked_dgo::read(fixture);
  CHECK(result);
  CHECK(result.value().objects[0].unique_name == "plat-ag");
  CHECK(result.value().objects[1].unique_name == "same");
  CHECK(result.value().objects[2].unique_name == "same-3");
  return true;
}

bool derives_jak2_art_group_names_without_changing_the_default() {
  const std::string marker = "/src/jak2/final/art-group7/plat-ag.go";
  std::vector<std::uint8_t> art_data(marker.begin(), marker.end());
  art_data.push_back(0);
  const auto fixture = make_raw_dgo("ART.DGO", {{"plat", art_data}});

  const auto jak1_result = jak1_checked_dgo::read(fixture);
  CHECK(jak1_result);
  CHECK(jak1_result.value().objects[0].unique_name == "plat");

  jak1_checked_dgo::Options options;
  options.game_version = GameVersion::Jak2;
  const auto jak2_result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(jak2_result);
  CHECK(jak2_result.value().objects[0].unique_name == "plat-ag");
  return true;
}

bool rejects_ambiguous_duplicate_names() {
  const auto fixture =
      make_raw_dgo("DUP.DGO", {{"same", {1, 2}}, {"same", {3, 4}}, {"same", {5, 6}}});
  const auto result = jak1_checked_dgo::read(fixture);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::duplicate_object_name);
  CHECK(result.error().object_index == 2);
  return true;
}

bool valid_compressed_fixture() {
  const auto raw = make_raw_dgo("COMP.CGO", {{"alpha", std::vector<std::uint8_t>(700, 0x41)}});
  const auto compressed = make_blzo(raw);
  CHECK(!compressed.empty());
  const auto result = jak1_checked_dgo::read(compressed, "COMP.CGO");
  CHECK(result);
  CHECK(result.value().was_compressed);
  CHECK(result.value().input_size == compressed.size());
  CHECK(result.value().expanded_size == raw.size());
  CHECK(result.value().objects[0].data == std::vector<std::uint8_t>(700, 0x41));
  return true;
}

bool valid_compressed_raw_chunk_fixture() {
  std::vector<std::uint8_t> payload(40000);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>((i * 131) & 0xff);
  }
  const auto raw = make_raw_dgo("RAW.CGO", {{"large", payload}});
  const auto compressed = make_blzo(raw, true);
  CHECK(!compressed.empty());
  const auto result = jak1_checked_dgo::read(compressed);
  CHECK(result);
  CHECK(result.value().objects[0].data == payload);
  return true;
}

bool rejects_truncation_and_trailing_data() {
  auto truncated = make_raw_dgo("BAD.DGO", {{"tiny", {1, 2, 3}}});
  truncated.pop_back();
  auto result = jak1_checked_dgo::read(truncated);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_alignment);

  auto oversized = make_raw_dgo("BAD.DGO", {{"tiny", {1, 2, 3}}});
  oversized[64] = 0xff;
  oversized[65] = 0xff;
  result = jak1_checked_dgo::read(oversized);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::truncated_object ||
        result.error().code == ErrorCode::object_size_limit_exceeded);

  auto trailing = make_raw_dgo("BAD.DGO", {{"tiny", {1, 2, 3}}});
  trailing.push_back(1);
  result = jak1_checked_dgo::read(trailing);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::trailing_data);
  return true;
}

bool rejects_bad_headers_and_names() {
  std::vector<std::uint8_t> short_header(63);
  auto result = jak1_checked_dgo::read(short_header);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::truncated_header);

  auto unterminated = make_raw_dgo("BAD.DGO", {{"tiny", {1}}});
  std::fill(unterminated.begin() + 4, unterminated.begin() + 64, 'A');
  result = jak1_checked_dgo::read(unterminated);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_name);

  auto dirty_padding = make_raw_dgo("BAD.DGO", {{"tiny", {1}}});
  dirty_padding[4 + std::string("BAD.DGO").size() + 1] = 1;
  result = jak1_checked_dgo::read(dirty_padding);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_name);

  const auto valid = make_raw_dgo("BAD.DGO", {{"tiny", {1}}});
  result = jak1_checked_dgo::read(valid, "OTHER.DGO");
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::unexpected_archive_name);
  return true;
}

bool rejects_malformed_art_group_marker_and_reserved_name() {
  const std::string marker = "/src/next/data/art-group6/other-ag.go";
  std::vector<std::uint8_t> data(marker.begin(), marker.end());
  data.push_back(0);
  auto fixture = make_raw_dgo("ART.DGO", {{"plat", data}});
  auto result = jak1_checked_dgo::read(fixture);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_art_group_marker);

  fixture = make_raw_dgo("ART.DGO", {{"plat-ag", {1, 2, 3}}});
  result = jak1_checked_dgo::read(fixture);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_name);

  const std::string jak2_marker = "/src/jak2/final/art-group7/other-ag.go";
  data.assign(jak2_marker.begin(), jak2_marker.end());
  data.push_back(0);
  fixture = make_raw_dgo("ART.DGO", {{"plat", data}});
  jak1_checked_dgo::Options jak2_options;
  jak2_options.game_version = GameVersion::Jak2;
  result = jak1_checked_dgo::read(fixture, {}, jak2_options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_art_group_marker);
  return true;
}

bool enforces_input_object_and_total_limits() {
  const auto fixture = make_raw_dgo("CAP.DGO", {{"one", {1, 2, 3}}, {"two", {4, 5, 6}}});

  jak1_checked_dgo::Options options;
  options.max_input_bytes = fixture.size() - 1;
  auto result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::input_too_large);

  options = {};
  options.max_objects = 1;
  result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::object_count_limit_exceeded);

  options = {};
  options.max_object_bytes = 2;
  result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::object_size_limit_exceeded);

  options = {};
  options.max_total_object_bytes = 5;
  result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::total_object_size_limit_exceeded);

  options = {};
  options.max_name_bytes = 3;
  result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_name);

  options = {};
  options.file_read_chunk_bytes = 0;
  result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_argument);
  return true;
}

bool enforces_compressed_limits_and_rejects_corruption() {
  const auto raw = make_raw_dgo("COMP.CGO", {{"alpha", std::vector<std::uint8_t>(700, 0x41)}});
  auto compressed = make_blzo(raw);
  CHECK(!compressed.empty());

  jak1_checked_dgo::Options options;
  options.max_compressed_bytes = compressed.size() - 1;
  auto result = jak1_checked_dgo::read(compressed, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::compressed_input_too_large);

  options = {};
  options.max_expanded_bytes = raw.size() - 1;
  result = jak1_checked_dgo::read(compressed, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::expanded_input_too_large);

  options = {};
  options.max_expansion_ratio = 1;
  result = jak1_checked_dgo::read(compressed, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::expansion_ratio_exceeded);

  compressed.resize(10);
  options = {};
  result = jak1_checked_dgo::read(compressed, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::invalid_compressed_chunk ||
        result.error().code == ErrorCode::decompression_failed);

  compressed = make_blzo(raw);
  compressed.insert(compressed.begin() + 8, 12, 0);
  options = {};
  options.max_compressed_padding_bytes = 8;
  result = jak1_checked_dgo::read(compressed, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::compressed_padding_limit_exceeded);
  return true;
}

bool cancellation_is_recoverable() {
  const auto fixture = make_raw_dgo("CANCEL.DGO", {{"one", {1}}, {"two", {2}}});
  jak1_checked_dgo::Options options;
  int calls = 0;
  options.should_cancel = [&]() { return ++calls >= 3; };
  const auto result = jak1_checked_dgo::read(fixture, {}, options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::cancelled);
  CHECK(result.error().object_index == 0);
  return true;
}

bool bounded_file_adapter() {
  const auto fixture = make_raw_dgo("FILE.DGO", {{"one", {1, 2, 3}}});
  const auto path = std::filesystem::temp_directory_path() /
                    ("opengoal-checked-dgo-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(fixture.data()), fixture.size());
  }

  jak1_checked_dgo::Options options;
  options.file_read_chunk_bytes = 7;
  auto result = jak1_checked_dgo::read_file(path, "FILE.DGO", options);
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);
  CHECK(result);
  CHECK(result.value().objects.size() == 1);

  const auto compressed = make_blzo(fixture);
  CHECK(!compressed.empty());
  {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(compressed.data()), compressed.size());
  }
  options = {};
  options.max_compressed_bytes = compressed.size() - 1;
  result = jak1_checked_dgo::read_file(path, "FILE.DGO", options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::compressed_input_too_large);

  result = jak1_checked_dgo::read_file(path);
  std::filesystem::remove(path, remove_error);
  CHECK(result);
  CHECK(result.value().was_compressed);

  result = jak1_checked_dgo::read_file(path);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::input_open_failed);
  return true;
}

bool error_names_are_stable() {
  CHECK(std::string(jak1_checked_dgo::error_code_name(ErrorCode::cancelled)) == "cancelled");
  CHECK(std::string(jak1_checked_dgo::error_code_name(ErrorCode::decompression_failed)) ==
        "decompression_failed");
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"valid_raw_preserves_order_and_names", valid_raw_preserves_order_and_names},
      {"derives_art_group_and_duplicate_names", derives_art_group_and_duplicate_names},
      {"derives_jak2_art_group_names_without_changing_the_default",
       derives_jak2_art_group_names_without_changing_the_default},
      {"rejects_ambiguous_duplicate_names", rejects_ambiguous_duplicate_names},
      {"valid_compressed_fixture", valid_compressed_fixture},
      {"valid_compressed_raw_chunk_fixture", valid_compressed_raw_chunk_fixture},
      {"rejects_truncation_and_trailing_data", rejects_truncation_and_trailing_data},
      {"rejects_bad_headers_and_names", rejects_bad_headers_and_names},
      {"rejects_malformed_art_group_marker_and_reserved_name",
       rejects_malformed_art_group_marker_and_reserved_name},
      {"enforces_input_object_and_total_limits", enforces_input_object_and_total_limits},
      {"enforces_compressed_limits_and_rejects_corruption",
       enforces_compressed_limits_and_rejects_corruption},
      {"cancellation_is_recoverable", cancellation_is_recoverable},
      {"bounded_file_adapter", bounded_file_adapter},
      {"error_names_are_stable", error_names_are_stable},
  };

  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
    ++passed;
  }
  std::cout << "Passed " << passed << " deterministic synthetic checked-DGO tests.\n";
  return 0;
}
