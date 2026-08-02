#include <cstddef>
#include <iostream>
#include <string>

#include "common/versions/jak1_iso_revisions.h"
#include "decompiler/extractor/extractor_util.h"

namespace {

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition << '\n';             \
      return 1;                                                                                  \
    }                                                                                            \
  } while (false)

}  // namespace

int main() {
  const auto& database = extractor_iso_database();
  std::size_t jak1_entries = 0;
  for (const auto& serial_entry : database) {
    for (const auto& executable_entry : serial_entry.second) {
      const auto& metadata = executable_entry.second;
      if (metadata.game_name == "jak1") {
        ++jak1_entries;
      }
    }
  }
  CHECK(jak1_entries == jak1_iso::supported_revisions().size());

  for (const auto& revision : jak1_iso::supported_revisions()) {
    const auto serial_entry = database.find(std::string(revision.serial));
    CHECK(serial_entry != database.end());
    const auto executable_entry = serial_entry->second.find(revision.elf_hash);
    CHECK(executable_entry != serial_entry->second.end());

    const auto& metadata = executable_entry->second;
    CHECK(metadata.canonical_name == revision.canonical_name);
    CHECK(metadata.region == static_cast<int>(revision.territory));
    CHECK(metadata.num_files == static_cast<int>(revision.file_count));
    CHECK(metadata.contents_hash.size() == 1);
    CHECK(metadata.contents_hash.contains(revision.contents_hash));
    CHECK(metadata.decomp_config_version == revision.decomp_config_version);
    CHECK(metadata.game_name == "jak1");
    CHECK(metadata.flags.size() == (revision.black_label ? 1 : 0));
    if (revision.black_label) {
      CHECK(metadata.flags.front() == "jak1-black-label");
    }

    const auto caller_result =
        get_version_info_from_build_info({std::string(revision.serial), revision.elf_hash});
    CHECK(caller_result.has_value());
    CHECK(caller_result->canonical_name == metadata.canonical_name);
    CHECK(caller_result->contents_hash == metadata.contents_hash);
  }

  CHECK(!get_version_info_from_build_info({"SCUS-97124", 1}).has_value());
  CHECK(!get_version_info_from_build_info({"UNKNOWN", 1}).has_value());
  return 0;
}
