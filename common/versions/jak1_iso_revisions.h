#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace jak1_iso {

enum class Territory : int {
  scea = 0,
  scee = 1,
  scei = 2,
  scek = 3,
};

struct Revision {
  std::string_view serial;
  uint64_t elf_hash = 0;
  std::string_view canonical_name;
  Territory territory = Territory::scea;
  uint32_t file_count = 0;
  uint64_t contents_hash = 0;
  std::string_view decomp_config_version;
  bool black_label = false;
};

std::span<const Revision> supported_revisions();
const Revision& default_revision();

}  // namespace jak1_iso
