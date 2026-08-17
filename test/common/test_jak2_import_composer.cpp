#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "common/custom_data/Jak2PublicOutputGraph.h"

#include "decompiler/extractor/jak2_fr3_preparer.h"
#include "decompiler/extractor/jak2_import_composer_internal.h"

namespace {

namespace composer = jak2_import_composer;
namespace core_generator = jak1_output_recipe_generator;
namespace fs = std::filesystem;
namespace generator = jak2_output_recipe_generator;
namespace retail_catalog = jak1_retail_object_catalog;

#define CHECK(condition)                                                           \
  do {                                                                             \
    if (!(condition)) {                                                            \
      std::cerr << "CHECK failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                \
    }                                                                              \
  } while (false)

class TemporaryRoot {
 public:
  TemporaryRoot() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    root = fs::temp_directory_path() / ("jak2-import-composer-test-" + std::to_string(nonce));
    fs::create_directory(root);
  }
  ~TemporaryRoot() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  fs::path root;
};

void write_text(const fs::path& path, const std::string& text = "synthetic") {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << text;
}

void write_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = value & 0xff;
  (*bytes)[offset + 1] = (value >> 8) & 0xff;
  (*bytes)[offset + 2] = (value >> 16) & 0xff;
  (*bytes)[offset + 3] = value >> 24;
}

void append_u32(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 4);
  write_u32(bytes, offset, value);
}

void append_fixed_name(std::vector<std::uint8_t>* bytes, const std::string& name) {
  const auto offset = bytes->size();
  bytes->resize(offset + 60);
  std::fill(bytes->begin() + offset, bytes->end(), 0);
  std::copy(name.begin(), name.end(), bytes->begin() + offset);
}

std::vector<std::uint8_t> make_catalog_object(std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(144);
  write_u32(&bytes, 0, 0xffffffff);
  write_u32(&bytes, 4, 64);
  write_u32(&bytes, 8, 4);
  write_u32(&bytes, 12, 64);
  for (std::size_t index = 16; index < 80; ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  write_u32(&bytes, 80, 0xffffffff);
  write_u32(&bytes, 84, 64);
  write_u32(&bytes, 88, 2);
  return bytes;
}

std::vector<std::uint8_t> make_catalog_archive(const std::string& archive_name,
                                               std::size_t object_count) {
  std::vector<std::uint8_t> bytes;
  append_u32(&bytes, static_cast<std::uint32_t>(object_count));
  append_fixed_name(&bytes, archive_name);
  for (std::size_t index = 0; index < object_count; ++index) {
    const auto object = make_catalog_object(static_cast<std::uint8_t>(index));
    append_u32(&bytes, static_cast<std::uint32_t>(object.size()));
    append_fixed_name(&bytes, "synthetic-" + std::to_string(index));
    bytes.insert(bytes.end(), object.begin(), object.end());
    while (bytes.size() % 16) {
      bytes.push_back(0);
    }
  }
  return bytes;
}

std::string read_text(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::set<std::string> direct_entries(const fs::path& root) {
  std::set<std::string> entries;
  for (const auto& entry : fs::directory_iterator(root)) {
    entries.emplace(entry.path().filename().string());
  }
  return entries;
}

bool has_partial(const fs::path& root) {
  if (!fs::exists(root)) {
    return false;
  }
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.path().filename().string().ends_with(".partial")) {
      return true;
    }
  }
  return false;
}

std::optional<fs::path> composer_partial(const fs::path& root) {
  for (const auto& entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().string();
    if (name.starts_with(".opengoal-work-") && name.ends_with(".partial")) {
      return entry.path();
    }
  }
  return std::nullopt;
}

composer::internal::FinalContract launch_contract() {
  return {{"KERNEL.CGO", "GAME.CGO", "TITLE.DGO", "CWI.DGO", "CTA.DGO", "PRI.DGO",
           "FEA.DGO", "INTROCST.DGO", "LDJAKBRN.DGO", "DEMO1.SBK", "CTYWIDE1.SBK",
           "FOREXIT1.SBK", "FOREXIT2.SBK"},
          {"GAME.fr3", "title.fr3", "ctywide.fr3", "ctysluma.fr3", "prison.fr3",
           "forexita.fr3", "introcst.fr3", "ldjakbrn.fr3"}};
}

void write_prepared_tree(const composer::internal::WorkPaths& paths,
                         const composer::internal::FinalContract& contract) {
  fs::create_directories(paths.prepared_root / "iso");
  fs::create_directory(paths.prepared_root / "fr3");
  for (const auto& name : contract.iso_basenames) {
    write_text(paths.prepared_root / "iso" / name);
  }
  for (const auto& name : contract.fr3_basenames) {
    write_text(paths.prepared_root / "fr3" / name);
  }
}

bool exact_prepared_tree_is_promoted() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "complete.candidate";
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;
  std::vector<composer::Phase> phases;
  std::optional<composer::Progress> last_progress;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) {
    phases.push_back(progress.phase);
    last_progress = progress;
  };
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         return {};
       }},
  }};

  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, options, stages, &summary, &produced_contract);
  CHECK(result);
  CHECK(result.value().output_bytes == 1234);
  CHECK(direct_entries(candidate) == std::set<std::string>({"fr3", "iso"}));
  CHECK(direct_entries(candidate / "iso") ==
        std::set<std::string>(contract.iso_basenames.begin(), contract.iso_basenames.end()));
  CHECK(direct_entries(candidate / "fr3") ==
        std::set<std::string>(contract.fr3_basenames.begin(), contract.fr3_basenames.end()));
  CHECK(!fs::exists(candidate / ".opengoal-import"));
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::finalizing_candidate);
  CHECK(last_progress.has_value());
  CHECK(last_progress->phase == composer::Phase::finalizing_candidate);
  CHECK(last_progress->completed == 2);
  CHECK(last_progress->total == 2);
  return true;
}

bool atomic_work_files_preserve_callback_injections() {
  TemporaryRoot temporary;
  const std::vector<std::uint8_t> bytes(300000, 0x41);
  const auto destination = temporary.root / "generated.bin";
  bool injected = false;
  composer::Options options;
  options.should_cancel = [&] {
    if (!injected) {
      write_text(destination, "destination stays");
      injected = true;
    }
    return false;
  };
  auto error = composer::internal::write_file_atomically(destination, bytes, options);
  CHECK(injected);
  CHECK(error.has_value());
  CHECK(error->code == composer::ErrorCode::work_write_failed);
  CHECK(read_text(destination) == "destination stays");
  CHECK(!composer_partial(temporary.root));

  const auto second_destination = temporary.root / "second.bin";
  const auto held = temporary.root / "owned-held";
  std::optional<fs::path> replacement;
  options = {};
  options.should_cancel = [&] {
    if (replacement) {
      return false;
    }
    const auto partial = composer_partial(temporary.root);
    if (!partial) {
      return false;
    }
    std::error_code rename_error;
    fs::rename(*partial, held, rename_error);
    if (rename_error) {
      return false;
    }
    write_text(*partial, "replacement stays");
    replacement = *partial;
    return false;
  };
  error = composer::internal::write_file_atomically(second_destination, bytes, options);
  CHECK(replacement.has_value());
  CHECK(error.has_value());
  CHECK(error->code == composer::ErrorCode::work_write_failed);
  CHECK(read_text(*replacement) == "replacement stays");
  CHECK(fs::is_regular_file(held));
  CHECK(!fs::exists(second_destination));
  return true;
}

bool incomplete_or_extra_output_never_succeeds() {
  TemporaryRoot temporary;
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;

  const auto missing_candidate = temporary.root / "missing.candidate";
  const std::array<composer::internal::StageAction, 1> missing_stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         fs::remove(paths.prepared_root / "iso/KERNEL.CGO");
         return {};
       }},
  }};
  auto result = composer::internal::compose_in_fresh_candidate(
      missing_candidate, {}, missing_stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == missing_candidate);
  CHECK(!fs::exists(missing_candidate / "iso"));

  const auto extra_candidate = temporary.root / "extra.candidate";
  const std::array<composer::internal::StageAction, 1> extra_stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         write_text(paths.prepared_root / "iso/unexpected.bin");
         return {};
       }},
  }};
  result = composer::internal::compose_in_fresh_candidate(
      extra_candidate, {}, extra_stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == extra_candidate);
  return true;
}

bool linked_output_is_rejected() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "linked.candidate";
  const auto contract = launch_contract();
  std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
  std::optional<composer::internal::FinalContract> produced_contract = contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::materializing_output,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_prepared_tree(paths, contract);
         fs::remove(paths.prepared_root / "fr3/title.fr3");
         fs::create_symlink("GAME.fr3", paths.prepared_root / "fr3/title.fr3");
         return {};
       }},
  }};
  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &produced_contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  CHECK(result.error().preserved_candidate_root == candidate);
  return true;
}

bool descriptor_promotion_rejects_callback_races() {
  const auto contract = launch_contract();
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "early-injection.candidate";
    const auto external = temporary.root / "external-directory";
    fs::create_directory(external);
    write_text(external / "sentinel", "external directory stays");
    std::optional<composer::Summary> summary;
    std::optional<composer::internal::FinalContract> produced_contract;
    bool injected = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!injected && progress.phase == composer::Phase::extracting_iso &&
          progress.current_item == "inside-stage") {
        std::error_code error;
        fs::rename(external, candidate / ".opengoal-import/injected", error);
        injected = !error;
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::extracting_iso,
         [](const composer::internal::WorkPaths&,
            const composer::Options& stage_options) -> std::optional<composer::Error> {
           try {
             stage_options.on_progress(
                 {composer::Phase::extracting_iso, 1, 2, 0, "inside-stage"});
           } catch (...) {
             return composer::Error{composer::ErrorCode::callback_failed,
                                    "synthetic guarded callback failure", std::nullopt,
                                    std::nullopt, std::nullopt};
           }
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(injected);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
    CHECK(read_text(candidate / ".opengoal-import/injected/sentinel") ==
          "external directory stays");
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "symlink-swap.candidate";
    const auto outside = temporary.root / "outside";
    fs::create_directory(outside);
    write_text(outside / "sentinel", "outside stays");
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool swapped = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!swapped && progress.phase == composer::Phase::materializing_output &&
          progress.completed == 1) {
        const auto prepared = candidate / ".opengoal-import/.prepared";
        std::error_code error;
        fs::rename(prepared / "iso", prepared / "iso-held", error);
        if (!error) {
          fs::create_directory_symlink(outside, prepared / "iso", error);
          swapped = !error;
        }
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(swapped);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
    CHECK(read_text(outside / "sentinel") == "outside stays");
    CHECK(!fs::exists(candidate / "iso"));
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "destination-race.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool raced = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!raced && progress.phase == composer::Phase::finalizing_candidate &&
          progress.completed == 0) {
        std::error_code error;
        raced = fs::create_directory(candidate / "iso", error) && !error;
        if (raced) {
          write_text(candidate / "iso/sentinel", "race stays");
        }
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(raced);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
    CHECK(read_text(candidate / "iso/sentinel") == "race stays");
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "work-cleanup-race.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool raced = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!raced && progress.phase == composer::Phase::finalizing_candidate &&
          progress.completed == 1) {
        const auto work = candidate / ".opengoal-import";
        std::error_code error;
        fs::rename(work, candidate / ".opengoal-import-held", error);
        if (!error) {
          raced = fs::create_directory(work, error) && !error;
          if (raced) {
            write_text(work / "sentinel", "replacement stays");
          }
        }
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(raced);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_cleanup_failed);
    CHECK(read_text(candidate / ".opengoal-import/sentinel") == "replacement stays");
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "cleanup-depth.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool expanded = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!expanded && progress.phase == composer::Phase::finalizing_candidate &&
          progress.completed == 1) {
        auto cursor = candidate / ".opengoal-import";
        std::error_code error;
        for (std::size_t depth = 0; depth < 65 && !error; ++depth) {
          cursor /= "nested-" + std::to_string(depth);
          fs::create_directory(cursor, error);
        }
        expanded = !error;
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(expanded);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_cleanup_failed);
    CHECK(!result.error().cleanup_error.has_value());
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "terminal-mutation.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool mutated = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!mutated && progress.phase == composer::Phase::finalizing_candidate &&
          progress.completed == 2) {
        write_text(candidate / "iso/KERNEL.CGO", "mutation!");
        mutated = true;
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(mutated);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_finalize_failed);
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "terminal-throw.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (progress.phase == composer::Phase::finalizing_candidate && progress.completed == 2) {
        throw std::runtime_error("terminal failure");
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::callback_failed);
  }
  return true;
}

bool failure_preserves_all_unregistered_work_files() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "failed.candidate";
  const auto active = temporary.root / "active";
  fs::create_directory(active);
  write_text(active / "sentinel", "active stays untouched");
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths& paths) -> std::optional<composer::Error> {
         write_text(paths.work_root / "recoverable.marker", "keep me");
         write_text(paths.work_root / "artifact.partial", "preserve me");
         return composer::Error{composer::ErrorCode::iso_validation_failed, "synthetic failure",
                                std::nullopt, std::nullopt, std::nullopt};
       }},
  }};
  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::iso_validation_failed);
  CHECK(result.error().preserved_candidate_root == candidate);
  CHECK(fs::is_regular_file(candidate / ".opengoal-import/recoverable.marker"));
  CHECK(has_partial(candidate));
  CHECK(read_text(candidate / ".opengoal-import/artifact.partial") == "preserve me");
  CHECK(read_text(active / "sentinel") == "active stays untouched");
  return true;
}

bool iso_reader_failures_reach_composer_with_detail() {
  jak2_iso::ValidationError read_failure;
  read_failure.code = jak2_iso::ValidationErrorCode::iso_reader_failed;
  read_failure.message = "The ISO could not be extracted into a private staging directory.";
  read_failure.reader_error = iso_file::Error{
      iso_file::ErrorCode::read_failed, 0x12345678,
      "Could not read the requested ISO bytes: Input/output error",
      std::error_code(EIO, std::generic_category())};

  TemporaryRoot temporary;
  const auto candidate = temporary.root / "read-failure.candidate";
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [&](const composer::internal::WorkPaths&) -> std::optional<composer::Error> {
         return composer::internal::map_iso_extraction_failure(read_failure);
       }},
  }};
  const auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::iso_validation_failed);
  CHECK(result.error().preserved_candidate_root == candidate);
  CHECK(result.error().iso_reader_error);
  CHECK(result.error().iso_reader_error->code == iso_file::ErrorCode::read_failed);
  CHECK(result.error().iso_reader_error->image_offset == 0x12345678);
  CHECK(result.error().iso_reader_error->system_error == std::errc::io_error);
  CHECK(result.error().message.find("Reader read_failed at ISO byte offset 305419896") !=
        std::string::npos);
  CHECK(result.error().message.find("Input/output error") != std::string::npos);

  jak2_iso::ValidationError output_failure;
  output_failure.code = jak2_iso::ValidationErrorCode::iso_reader_failed;
  output_failure.message = read_failure.message;
  output_failure.reader_error = iso_file::Error{
      iso_file::ErrorCode::output_write_failed, 4096, "Could not write output file GAME.CGO",
      std::error_code(ENOSPC, std::generic_category())};
  const auto no_space = composer::internal::map_iso_extraction_failure(output_failure);
  CHECK(no_space.code == composer::ErrorCode::insufficient_storage);
  CHECK(no_space.iso_reader_error);
  CHECK(no_space.iso_reader_error->system_error == std::errc::no_space_on_device);
  CHECK(no_space.message.find("not enough free space") != std::string::npos);
  CHECK(no_space.message.find("Reader output_write_failed at ISO byte offset 4096") !=
        std::string::npos);

  jak2_iso::ValidationError cancelled;
  cancelled.code = jak2_iso::ValidationErrorCode::cancelled;
  cancelled.message = "The ISO could not be extracted into a private staging directory.";
  cancelled.reader_error = iso_file::Error{iso_file::ErrorCode::cancelled, 8192,
                                           "ISO extraction was cancelled.", {}};
  const auto cancellation = composer::internal::map_iso_extraction_failure(cancelled);
  CHECK(cancellation.code == composer::ErrorCode::cancelled);
  CHECK(cancellation.iso_reader_error);
  CHECK(cancellation.message == "Jak II ISO extraction was cancelled.");
  return true;
}

bool cancellation_and_callback_failures_are_typed() {
  TemporaryRoot temporary;
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};
  composer::Options cancelled;
  cancelled.should_cancel = [] { return true; };
  const auto cancelled_candidate = temporary.root / "cancelled.candidate";
  auto result = composer::internal::compose_in_fresh_candidate(
      cancelled_candidate, cancelled, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::cancelled);
  CHECK(result.error().preserved_candidate_root == cancelled_candidate);

  composer::Options cancel_callback;
  cancel_callback.should_cancel = []() -> bool {
    throw std::runtime_error("synthetic cancellation callback failure");
  };
  const auto cancel_callback_candidate = temporary.root / "cancel-callback.candidate";
  result = composer::internal::compose_in_fresh_candidate(
      cancel_callback_candidate, cancel_callback, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::callback_failed);
  CHECK(result.error().preserved_candidate_root == cancel_callback_candidate);

  composer::Options callback;
  callback.on_progress = [](const composer::Progress&) {
    throw std::runtime_error("synthetic callback failure");
  };
  const auto callback_candidate = temporary.root / "callback.candidate";
  result = composer::internal::compose_in_fresh_candidate(
      callback_candidate, callback, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::callback_failed);
  CHECK(result.error().preserved_candidate_root == callback_candidate);
  return true;
}

bool callbacks_remain_typed_with_stage_boundary_ownership() {
  const auto contract = launch_contract();
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "repeated-callbacks.candidate";
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    std::size_t cancel_polls = 0;
    std::size_t progress_reports = 0;
    composer::Options options;
    options.should_cancel = [&] {
      ++cancel_polls;
      return false;
    };
    options.on_progress = [&](const composer::Progress&) { ++progress_reports; };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths,
             const composer::Options& stage_options) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           for (std::size_t index = 0; index < 1000; ++index) {
             if (stage_options.should_cancel()) {
               return composer::Error{composer::ErrorCode::cancelled,
                                      "synthetic cancellation", std::nullopt, std::nullopt,
                                      std::nullopt};
             }
             stage_options.on_progress(
                 {composer::Phase::materializing_output, index, 1000, 0, "synthetic"});
           }
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(result);
    CHECK(cancel_polls >= 1000);
    CHECK(progress_reports >= 1000);
  }
  {
    TemporaryRoot temporary;
    const auto candidate = temporary.root / "unsafe-callback-work.candidate";
    const auto outside = temporary.root / "outside";
    write_text(outside, "outside stays");
    std::optional<composer::Summary> summary = composer::Summary{2, 4, 11, 8, 1234};
    std::optional<composer::internal::FinalContract> produced_contract = contract;
    bool injected = false;
    composer::Options options;
    options.on_progress = [&](const composer::Progress& progress) {
      if (!injected && progress.current_item == "inject-unsafe-link") {
        std::error_code error;
        fs::create_symlink(outside, candidate / ".opengoal-import/unsafe-link", error);
        injected = !error;
      }
    };
    const std::array<composer::internal::StageAction, 1> stages = {{
        {composer::Phase::materializing_output,
         [&](const composer::internal::WorkPaths& paths,
             const composer::Options& stage_options) -> std::optional<composer::Error> {
           write_prepared_tree(paths, contract);
           stage_options.on_progress(
               {composer::Phase::materializing_output, 1, 2, 0, "inject-unsafe-link"});
           return {};
         }},
    }};
    const auto result = composer::internal::compose_in_fresh_candidate(
        candidate, options, stages, &summary, &produced_contract);
    CHECK(injected);
    CHECK(!result);
    CHECK(result.error().code == composer::ErrorCode::candidate_cleanup_failed);
    CHECK(result.error().preserved_candidate_root == candidate);
    CHECK(read_text(outside) == "outside stays");
  }
  return true;
}

bool existing_candidate_and_input_containment_are_rejected() {
  TemporaryRoot temporary;
  const auto candidate = temporary.root / "existing.candidate";
  fs::create_directory(candidate);
  write_text(candidate / "sentinel", "original");
  std::optional<composer::Summary> summary;
  std::optional<composer::internal::FinalContract> contract;
  const std::array<composer::internal::StageAction, 1> stages = {{
      {composer::Phase::extracting_iso,
       [](const composer::internal::WorkPaths&) -> std::optional<composer::Error> { return {}; }},
  }};
  auto result = composer::internal::compose_in_fresh_candidate(
      candidate, {}, stages, &summary, &contract);
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::invalid_argument);
  CHECK(!result.error().preserved_candidate_root);
  CHECK(read_text(candidate / "sentinel") == "original");

  const auto iso = temporary.root / "disc.iso";
  const auto source = temporary.root / "source-pack";
  const auto resources = temporary.root / "resources";
  fs::create_directory(source);
  fs::create_directory(resources);
  write_text(iso);
  const auto contained_candidate = source / "candidate";
  result = composer::compose({iso, source, resources, contained_candidate});
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::invalid_argument);
  CHECK(!fs::exists(contained_candidate));
  return true;
}

bool invalid_source_pack_fails_before_candidate_creation() {
  TemporaryRoot temporary;
  const auto iso = temporary.root / "disc.iso";
  const auto source = temporary.root / "source-pack";
  const auto resources = temporary.root / "resources";
  const auto candidate = temporary.root / "candidate";
  write_text(iso);
  fs::create_directory(source);
  fs::create_directory(resources);
  const auto result = composer::compose({iso, source, resources, candidate});
  CHECK(!result);
  CHECK(result.error().code == composer::ErrorCode::source_pack_failed);
  CHECK(!result.error().preserved_candidate_root);
  CHECK(!fs::exists(candidate));
  return true;
}

bool checked_graph_contains_the_iso_launch_contract() {
  const auto graph = jak2_public_output_graph::decode_base_retail();
  CHECK(graph);
  const auto retail = composer::internal::derive_retail_requirements(graph.value());
  CHECK(retail);
  CHECK(retail.value().occurrence_count == composer::internal::kNtscV2RetailOccurrenceCount);
  CHECK(retail.value().objects.size() == composer::internal::kNtscV2RetailObjectCount);
  CHECK(retail.value().source_archive_relative_paths.size() ==
        composer::internal::kNtscV2RetailArchiveCount);
  CHECK(std::set<std::string>(retail.value().source_archive_relative_paths.begin(),
                              retail.value().source_archive_relative_paths.end())
            .size() == composer::internal::kNtscV2RetailArchiveCount);
  std::size_t tpage_1606_occurrences = 0;
  for (const auto& archive : graph.value().archives) {
    for (const auto& object : archive.objects) {
      if (object.prepared_basename != "tpage-1606.go") {
        continue;
      }
      ++tpage_1606_occurrences;
      CHECK(object.producer == core_generator::ObjectProducerKind::verified_retail);
      CHECK(object.internal_name == "tpage-1606");
      CHECK(object.retail_source_archive == "DGO/ATE.DGO");
    }
  }
  CHECK(tpage_1606_occurrences == 1);
  std::set<std::string> destinations;
  for (const auto& archive : graph.value().archives) {
    destinations.emplace(archive.destination_basename);
  }
  for (const auto& copy : graph.value().flat_file_copies) {
    destinations.emplace(copy.destination_basename);
  }
  for (const auto& generated : graph.value().generated_flat_files) {
    destinations.emplace(generated.destination_basename);
  }
  const auto contract = launch_contract();
  for (const auto& required : contract.iso_basenames) {
    CHECK(destinations.contains(required));
  }
  return true;
}

bool retail_catalog_progress_is_truthful_and_bounded() {
  const auto graph = jak2_public_output_graph::decode_base_retail();
  CHECK(graph);
  const auto requirements = composer::internal::derive_retail_requirements(graph.value());
  CHECK(requirements);
  const auto& archive_paths = requirements.value().source_archive_relative_paths;
  CHECK(archive_paths.size() == composer::internal::kNtscV2RetailArchiveCount);
  CHECK(archive_paths.size() >= 2);

  std::vector<composer::Progress> updates;
  std::uint64_t completed_archive_bytes = 0;
  const std::array object_counts = {std::size_t{260}, std::size_t{2}};
  for (std::size_t archive_index = 0; archive_index < object_counts.size(); ++archive_index) {
    const auto& relative_path = archive_paths[archive_index];
    updates.push_back({composer::Phase::cataloging_retail, archive_index, archive_paths.size(),
                       completed_archive_bytes, relative_path});

    const auto bytes = make_catalog_archive(fs::path(relative_path).filename().string(),
                                            object_counts[archive_index]);
    const retail_catalog::ArchiveSource source{relative_path, bytes};
    retail_catalog::Options options;
    options.game_version = GameVersion::Jak2;
    options.on_progress = [&](const retail_catalog::Progress& progress) {
      if ((progress.stage != retail_catalog::ProgressStage::indexed_object &&
           progress.stage != retail_catalog::ProgressStage::skipped_code_object) ||
          !progress.archive_object_index) {
        return;
      }
      auto milestone = static_cast<std::uint64_t>(*progress.archive_object_index) + 1;
      while (milestone > 1 && milestone % 16 == 0) {
        milestone /= 16;
      }
      if (milestone == 1) {
        updates.push_back(
            {composer::Phase::cataloging_retail, archive_index, archive_paths.size(),
             completed_archive_bytes,
             relative_path + " (object " +
                 std::to_string(*progress.archive_object_index) + ")"});
      }
    };
    const auto catalog = retail_catalog::build(
        std::span<const retail_catalog::ArchiveSource>(&source, 1), options);
    CHECK(catalog);
    completed_archive_bytes += bytes.size();
    updates.push_back({composer::Phase::cataloging_retail, archive_index + 1,
                       archive_paths.size(), completed_archive_bytes, relative_path});
  }

  CHECK(updates.size() == 8);
  CHECK(updates[0].completed == 0);
  CHECK(updates[0].total == composer::internal::kNtscV2RetailArchiveCount);
  CHECK(updates[0].bytes_completed == 0);
  CHECK(updates[0].current_item == archive_paths[0]);
  CHECK(updates[1].current_item == archive_paths[0] + " (object 0)");
  CHECK(updates[2].current_item == archive_paths[0] + " (object 15)");
  CHECK(updates[3].current_item == archive_paths[0] + " (object 255)");
  CHECK(updates[4].completed == 1);
  CHECK(updates[4].current_item == archive_paths[0]);
  CHECK(updates[5].completed == 1);
  CHECK(updates[5].bytes_completed == updates[4].bytes_completed);
  CHECK(updates[5].current_item == archive_paths[1]);
  CHECK(updates[6].current_item == archive_paths[1] + " (object 0)");
  CHECK(updates[7].completed == 2);
  CHECK(updates[7].current_item == archive_paths[1]);
  for (std::size_t index = 1; index < updates.size(); ++index) {
    CHECK(updates[index].completed >= updates[index - 1].completed);
    CHECK(updates[index].bytes_completed >= updates[index - 1].bytes_completed);
    CHECK(updates[index].total == composer::internal::kNtscV2RetailArchiveCount);
  }

  const auto callback_archive =
      make_catalog_archive(fs::path(archive_paths[0]).filename().string(), 2);
  const retail_catalog::ArchiveSource callback_source{archive_paths[0], callback_archive};
  retail_catalog::Options callback_failure;
  callback_failure.game_version = GameVersion::Jak2;
  callback_failure.on_progress = [](const retail_catalog::Progress& progress) {
    if (progress.stage == retail_catalog::ProgressStage::indexed_object) {
      throw std::runtime_error("synthetic progress callback failure");
    }
  };
  auto callback_result = retail_catalog::build(
      std::span<const retail_catalog::ArchiveSource>(&callback_source, 1), callback_failure);
  CHECK(!callback_result);
  CHECK(callback_result.error().code == retail_catalog::ErrorCode::callback_failed);

  retail_catalog::Options cancellation;
  cancellation.game_version = GameVersion::Jak2;
  cancellation.should_cancel = [] { return true; };
  callback_result = retail_catalog::build(
      std::span<const retail_catalog::ArchiveSource>(&callback_source, 1), cancellation);
  CHECK(!callback_result);
  CHECK(callback_result.error().code == retail_catalog::ErrorCode::cancelled);
  return true;
}

bool retail_catalog_selection_is_exact_and_cancellable() {
  generator::Graph graph;
  graph.archives = {{"OUT.DGO",
                     {{"retail.go", "retail", core_generator::ObjectProducerKind::verified_retail,
                       "DGO/A.DGO"}}}};
  auto requirements = composer::internal::derive_retail_requirements(graph);
  CHECK(requirements);
  CHECK(requirements.value().occurrence_count == 1);
  CHECK(requirements.value().objects.size() == 1);
  CHECK(requirements.value().source_archive_relative_paths ==
        std::vector<std::string>({"DGO/A.DGO"}));

  const retail_catalog::Entry entry{{"DGO/A.DGO", 7, "retail", "retail", 144,
                                     0x123456789abcdef0ull,
                                     retail_catalog::ObjectVersion::v4}};
  auto selected = composer::internal::select_exact_retail_catalog(
      requirements.value(), std::span<const retail_catalog::Entry>(&entry, 1), 144);
  CHECK(selected);
  CHECK(selected.value().size() == 1);
  CHECK(selected.value()[0].source_archive_relative_path == "DGO/A.DGO");
  CHECK(selected.value()[0].archive_object_index == 7);
  CHECK(selected.value()[0].xxh64 == 0x123456789abcdef0ull);

  selected = composer::internal::select_exact_retail_catalog(requirements.value(), {}, 144);
  CHECK(!selected);
  CHECK(selected.error().code == composer::ErrorCode::retail_catalog_failed);

  auto wrong_archive = entry;
  wrong_archive.provenance.source_archive_relative_path = "DGO/B.DGO";
  selected = composer::internal::select_exact_retail_catalog(
      requirements.value(), std::span<const retail_catalog::Entry>(&wrong_archive, 1), 144);
  CHECK(!selected);
  CHECK(selected.error().code == composer::ErrorCode::retail_catalog_failed);

  const std::array duplicate = {entry, entry};
  selected = composer::internal::select_exact_retail_catalog(requirements.value(), duplicate, 288);
  CHECK(!selected);
  CHECK(selected.error().code == composer::ErrorCode::retail_catalog_failed);

  selected = composer::internal::select_exact_retail_catalog(
      requirements.value(), std::span<const retail_catalog::Entry>(&entry, 1), 143);
  CHECK(!selected);
  CHECK(selected.error().code == composer::ErrorCode::retail_catalog_failed);

  composer::Options cancelled;
  cancelled.should_cancel = [] { return true; };
  selected = composer::internal::select_exact_retail_catalog(
      requirements.value(), std::span<const retail_catalog::Entry>(&entry, 1), 144, cancelled);
  CHECK(!selected);
  CHECK(selected.error().code == composer::ErrorCode::cancelled);
  CHECK(std::string(composer::phase_name(composer::Phase::cataloging_retail)) ==
        "cataloging_retail");
  CHECK(std::string(composer::error_code_name(composer::ErrorCode::retail_catalog_failed)) ==
        "retail_catalog_failed");
  return true;
}

bool optional_real_import_oracle() {
  constexpr std::array variables = {
      "OPENGOAL_JAK2_IMPORT_TEST_ISO",
      "OPENGOAL_JAK2_IMPORT_TEST_SOURCE_PACK",
      "OPENGOAL_JAK2_IMPORT_TEST_PROJECT_RESOURCES",
      "OPENGOAL_JAK2_IMPORT_TEST_CANDIDATE",
  };
  std::array<const char*, variables.size()> values{};
  std::size_t configured = 0;
  for (std::size_t index = 0; index < variables.size(); ++index) {
    values[index] = std::getenv(variables[index]);
    configured += values[index] && values[index][0] != '\0';
  }
  if (configured == 0) {
    std::cout << "Jak II real-import oracle skipped (environment unset)\n";
    return true;
  }
  CHECK(configured == variables.size());

  std::vector<composer::Phase> phases;
  composer::Options options;
  options.on_progress = [&](const composer::Progress& progress) {
    phases.push_back(progress.phase);
  };
  const composer::Request request{fs::path(values[0]), fs::path(values[1]), fs::path(values[2]),
                                  fs::path(values[3])};
  const auto result = composer::compose(request, options);
  if (!result) {
    std::cerr << "real-import oracle failed: " << composer::error_code_name(result.error().code)
              << ": " << result.error().message << '\n';
    return false;
  }
  CHECK(result.value().archives_written == 150);
  CHECK(result.value().flat_files_written == 417);
  CHECK(result.value().objects_written > 0);
  CHECK(result.value().fr3_files_written == jak2_fr3::kNtscV2ExpectedFr3Files);
  CHECK(result.value().output_bytes > 0);
  CHECK(!fs::exists(request.candidate_root / ".opengoal-import"));
  CHECK(direct_entries(request.candidate_root) == std::set<std::string>({"fr3", "iso"}));
  CHECK(direct_entries(request.candidate_root / "iso").size() == 567);
  CHECK(direct_entries(request.candidate_root / "fr3").size() ==
        result.value().fr3_files_written);
  const auto contract = launch_contract();
  for (const auto& name : contract.iso_basenames) {
    CHECK(fs::symlink_status(request.candidate_root / "iso" / name).type() ==
          fs::file_type::regular);
  }
  for (const auto& name : contract.fr3_basenames) {
    CHECK(fs::symlink_status(request.candidate_root / "fr3" / name).type() ==
          fs::file_type::regular);
  }
  CHECK(!phases.empty());
  CHECK(phases.back() == composer::Phase::finalizing_candidate);
  std::cout << "Jak II base-retail real-import oracle passed: "
            << result.value().archives_written
            << " archives, " << result.value().objects_written << " objects, "
            << result.value().flat_files_written << " flat files, "
            << result.value().fr3_files_written << " FR3 files\n";
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      exact_prepared_tree_is_promoted,
      atomic_work_files_preserve_callback_injections,
      incomplete_or_extra_output_never_succeeds,
      linked_output_is_rejected,
      descriptor_promotion_rejects_callback_races,
      failure_preserves_all_unregistered_work_files,
      iso_reader_failures_reach_composer_with_detail,
      cancellation_and_callback_failures_are_typed,
      callbacks_remain_typed_with_stage_boundary_ownership,
      existing_candidate_and_input_containment_are_rejected,
      invalid_source_pack_fails_before_candidate_creation,
      checked_graph_contains_the_iso_launch_contract,
      retail_catalog_progress_is_truthful_and_bounded,
      retail_catalog_selection_is_exact_and_cancellable,
      optional_real_import_oracle,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak II import composer tests passed\n";
  return 0;
}
