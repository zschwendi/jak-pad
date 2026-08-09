#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/util/PosixFile.h"
#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"

namespace {

namespace writer = jak1_checked_dgo_writer;

#define CHECK(condition)                                                                   \
  do {                                                                                     \
    if (!(condition)) {                                                                    \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " << #condition \
                << '\n';                                                                   \
      return false;                                                                        \
    }                                                                                      \
  } while (false)

struct TemporaryDirectory {
  std::filesystem::path path;

  TemporaryDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("opengoal-checked-dgo-writer-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) {
    return {};
  }
  const auto end = input.tellg();
  if (end < 0) {
    return {};
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
  input.seekg(0);
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return input ? bytes : std::vector<std::uint8_t>{};
}

std::size_t owned_stage_count(const std::filesystem::path& directory,
                              std::string_view destination_name) {
  const auto prefix = "." + std::string(destination_name) + ".opengoal-stage-";
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with(prefix)) {
      ++count;
    }
  }
  return count;
}

std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
         (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

bool owned_fd_move_and_reset_are_single_owner() {
  TemporaryDirectory temp;
  auto descriptor = posix_file::open_directory(temp.path.c_str());
  CHECK(descriptor);
  const int original = descriptor.get();
  posix_file::OwnedFd moved(std::move(descriptor));
  CHECK(!descriptor);
  CHECK(moved.get() == original);
  posix_file::OwnedFd assigned;
  assigned = std::move(moved);
  CHECK(!moved);
  CHECK(assigned.get() == original);
  assigned.reset();
  CHECK(!assigned);
  return true;
}

bool builds_exact_raw_archive_and_round_trips() {
  const std::vector<std::uint8_t> first{1, 2, 3};
  const std::vector<std::uint8_t> second{4, 5, 6, 7, 8};
  const std::vector<writer::ObjectRecord> objects{{"first", first}, {"second", second}};

  const auto result = writer::build("TEST.DGO", objects);
  CHECK(result);
  const auto& bytes = result.value();
  CHECK(bytes.size() == 64 + 64 + 16 + 64 + 16);
  CHECK(read_u32_le(bytes, 0) == 2);
  CHECK(std::string(reinterpret_cast<const char*>(bytes.data() + 4)) == "TEST.DGO");
  CHECK(read_u32_le(bytes, 64) == first.size());
  CHECK(std::string(reinterpret_cast<const char*>(bytes.data() + 68)) == "first");
  CHECK(std::equal(first.begin(), first.end(), bytes.begin() + 128));
  CHECK(std::all_of(bytes.begin() + 131, bytes.begin() + 144,
                    [](std::uint8_t byte) { return byte == 0; }));
  CHECK(read_u32_le(bytes, 144) == second.size());
  CHECK(std::string(reinterpret_cast<const char*>(bytes.data() + 148)) == "second");
  CHECK(std::equal(second.begin(), second.end(), bytes.begin() + 208));
  CHECK(std::all_of(bytes.begin() + 213, bytes.end(), [](std::uint8_t byte) { return byte == 0; }));

  const auto parsed = jak1_checked_dgo::read(bytes, "TEST.DGO");
  CHECK(parsed);
  CHECK(parsed.value().objects.size() == 2);
  CHECK(parsed.value().objects[0].internal_name == "first");
  CHECK(parsed.value().objects[0].data == first);
  CHECK(parsed.value().objects[1].internal_name == "second");
  CHECK(parsed.value().objects[1].data == second);
  return true;
}

bool rejects_invalid_names() {
  const std::vector<std::uint8_t> data{1};
  auto result = writer::build("", std::array<writer::ObjectRecord, 1>{{{"one", data}}});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_name);

  result = writer::build("../BAD.DGO", std::array<writer::ObjectRecord, 1>{{{"one", data}}});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_name);

  const std::string non_ascii("BAD\x80", 4);
  result = writer::build(non_ascii, std::array<writer::ObjectRecord, 1>{{{"one", data}}});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_name);

  result = writer::build("GOOD.DGO", std::array<writer::ObjectRecord, 1>{{{"bad/name", data}}});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_name);
  CHECK(result.error().object_index == 0);

  writer::Options options;
  options.max_name_bytes = 3;
  result = writer::build("LONG", std::array<writer::ObjectRecord, 1>{{{"one", data}}}, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_name);
  return true;
}

bool enforces_object_total_output_and_option_caps() {
  const std::vector<std::uint8_t> one{1, 2, 3};
  const std::vector<std::uint8_t> two{4, 5, 6};
  const std::array<writer::ObjectRecord, 2> objects{{{"one", one}, {"two", two}}};

  auto result = writer::build("CAP.DGO", std::span<const writer::ObjectRecord>{});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::empty_archive);

  writer::Options options;
  options.max_objects = 1;
  result = writer::build("CAP.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::object_count_limit_exceeded);

  options = {};
  options.max_object_bytes = 2;
  result = writer::build("CAP.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::object_size_limit_exceeded);

  options = {};
  options.max_total_object_bytes = 5;
  result = writer::build("CAP.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::total_object_size_limit_exceeded);

  options = {};
  options.max_output_bytes = 223;
  result = writer::build("CAP.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::output_size_limit_exceeded);

  options = {};
  options.write_chunk_bytes = 0;
  result = writer::build("CAP.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::invalid_argument);

  const std::vector<std::uint8_t> empty;
  result = writer::build("CAP.DGO", std::array<writer::ObjectRecord, 1>{{{"empty", empty}}});
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::object_size_limit_exceeded);
  return true;
}

bool applies_explicit_duplicate_policy() {
  const std::vector<std::uint8_t> one{1};
  const std::vector<std::uint8_t> two{2};
  const std::array<writer::ObjectRecord, 2> objects{{{"same", one}, {"same", two}}};

  auto result = writer::build("DUP.DGO", objects);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::duplicate_object_name);
  CHECK(result.error().object_index == 1);

  writer::Options options;
  options.duplicate_name_policy = writer::DuplicateNamePolicy::allow;
  result = writer::build("DUP.DGO", objects, options);
  CHECK(result);
  const auto parsed = jak1_checked_dgo::read(result.value());
  CHECK(parsed);
  CHECK(parsed.value().objects.size() == 2);
  CHECK(parsed.value().objects[0].internal_name == "same");
  CHECK(parsed.value().objects[1].internal_name == "same");
  return true;
}

bool cancellation_and_progress_are_recoverable() {
  const std::vector<std::uint8_t> data(64, 0x41);
  const std::array<writer::ObjectRecord, 1> objects{{{"one", data}}};
  std::vector<writer::Progress> progress;
  bool cancel = false;
  writer::Options options;
  options.write_chunk_bytes = 7;
  options.should_cancel = [&]() { return cancel; };
  options.on_progress = [&](const writer::Progress& update) {
    progress.push_back(update);
    if (update.phase == writer::ProgressPhase::writing && update.bytes_completed > 64) {
      cancel = true;
    }
  };
  const auto result = writer::build("CANCEL.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::cancelled);
  CHECK(result.error().object_index == 0);
  CHECK(std::any_of(progress.begin(), progress.end(), [](const writer::Progress& update) {
    return update.phase == writer::ProgressPhase::validating;
  }));
  CHECK(std::any_of(progress.begin(), progress.end(), [](const writer::Progress& update) {
    return update.phase == writer::ProgressPhase::writing;
  }));

  options = {};
  options.on_progress = [](const writer::Progress&) { throw 1; };
  const auto callback_result = writer::build("CALLBACK.DGO", objects, options);
  CHECK(!callback_result);
  CHECK(callback_result.error().code == writer::ErrorCode::callback_failed);
  return true;
}

bool writes_atomically_and_refuses_existing_destination() {
  TemporaryDirectory temp;
  const auto destination = temp.path / "OUTPUT.DGO";
  const std::vector<std::uint8_t> one{1, 2, 3};
  const std::vector<std::uint8_t> two{4, 5};
  const std::array<writer::ObjectRecord, 2> objects{{{"one", one}, {"two", two}}};

  auto unsafe = writer::write_file(temp.path / "BAD:NAME.DGO", "OK.DGO", objects);
  CHECK(!unsafe);
  CHECK(unsafe.error().code == writer::ErrorCode::invalid_argument);
  writer::Options name_limits;
  name_limits.max_name_bytes = 8;
  unsafe = writer::write_file(temp.path / "TOO-LONG.DGO", "OK.DGO", objects, name_limits);
  CHECK(!unsafe);
  CHECK(unsafe.error().code == writer::ErrorCode::invalid_argument);

  std::vector<writer::ProgressPhase> phases;
  writer::Options options;
  options.on_progress = [&](const writer::Progress& update) { phases.push_back(update.phase); };
  const auto result = writer::write_file(destination, "OUTPUT.DGO", objects, options);
  if (!result) {
    std::cerr << "write_file failed with " << writer::error_code_name(result.error().code) << ": "
              << result.error().message << '\n';
  }
  CHECK(result);
  CHECK(result.value().object_count == 2);
  CHECK(result.value().object_bytes == 5);
  CHECK(result.value().output_bytes == read_bytes(destination).size());
  CHECK(owned_stage_count(temp.path, destination.filename().string()) == 0);
  CHECK(std::find(phases.begin(), phases.end(), writer::ProgressPhase::installing) != phases.end());

  const auto parsed = jak1_checked_dgo::read_file(destination, "OUTPUT.DGO");
  CHECK(parsed);
  CHECK(parsed.value().objects[0].data == one);
  CHECK(parsed.value().objects[1].data == two);

  const auto before = read_bytes(destination);
  const auto second = writer::write_file(destination, "OUTPUT.DGO", objects);
  CHECK(!second);
  CHECK(second.error().code == writer::ErrorCode::destination_exists);
  CHECK(read_bytes(destination) == before);
  CHECK(owned_stage_count(temp.path, destination.filename().string()) == 0);

  const auto raced_destination = temp.path / "RACE.DGO";
  options = {};
  options.on_progress = [&](const writer::Progress& update) {
    if (update.phase == writer::ProgressPhase::installing) {
      std::fstream raced(raced_destination, std::ios::binary | std::ios::in | std::ios::out);
      char byte = 0;
      raced.read(&byte, 1);
      byte ^= 1;
      raced.seekp(0);
      raced.write(&byte, 1);
      raced.flush();
    }
  };
  const auto raced = writer::write_file(raced_destination, "RACE.DGO", objects, options);
  CHECK(!raced);
  CHECK(raced.error().code == writer::ErrorCode::atomic_install_failed);
  CHECK(!std::filesystem::exists(raced_destination));
  CHECK(owned_stage_count(temp.path, raced_destination.filename().string()) == 0);
  return true;
}

bool failure_cleans_only_its_owned_stage() {
  TemporaryDirectory temp;
  const auto destination = temp.path / "CANCEL.DGO";
  const auto unrelated = temp.path / ".CANCEL.DGO.opengoal-stage-preserve";
  {
    std::ofstream output(unrelated, std::ios::binary);
    output << "preserve";
  }
  const std::vector<std::uint8_t> data(128, 0x42);
  const std::array<writer::ObjectRecord, 1> objects{{{"one", data}}};

  bool cancel = false;
  writer::Options options;
  options.write_chunk_bytes = 8;
  options.should_cancel = [&]() { return cancel; };
  options.on_progress = [&](const writer::Progress& update) {
    if (update.phase == writer::ProgressPhase::writing) {
      cancel = true;
    }
  };
  const auto result = writer::write_file(destination, "CANCEL.DGO", objects, options);
  CHECK(!result);
  CHECK(result.error().code == writer::ErrorCode::cancelled);
  CHECK(!std::filesystem::exists(destination));
  CHECK(std::filesystem::exists(unrelated));
  CHECK(owned_stage_count(temp.path, destination.filename().string()) == 1);

  const auto missing_parent = temp.path / "missing" / "OUTPUT.DGO";
  const auto missing_result = writer::write_file(missing_parent, "OUTPUT.DGO", objects);
  CHECK(!missing_result);
  CHECK(missing_result.error().code == writer::ErrorCode::stage_create_failed);
  CHECK(!std::filesystem::exists(missing_parent));
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> tests = {
      {"owned_fd_move_and_reset_are_single_owner", owned_fd_move_and_reset_are_single_owner},
      {"builds_exact_raw_archive_and_round_trips", builds_exact_raw_archive_and_round_trips},
      {"rejects_invalid_names", rejects_invalid_names},
      {"enforces_object_total_output_and_option_caps",
       enforces_object_total_output_and_option_caps},
      {"applies_explicit_duplicate_policy", applies_explicit_duplicate_policy},
      {"cancellation_and_progress_are_recoverable", cancellation_and_progress_are_recoverable},
      {"writes_atomically_and_refuses_existing_destination",
       writes_atomically_and_refuses_existing_destination},
      {"failure_cleans_only_its_owned_stage", failure_cleans_only_its_owned_stage},
  };

  for (const auto& [name, test] : tests) {
    if (!test()) {
      std::cerr << "FAILED: " << name << '\n';
      return 1;
    }
    std::cout << "PASS: " << name << '\n';
  }
  return 0;
}
