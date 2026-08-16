#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace jak2_iso {

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
};

std::span<const Revision> supported_revisions();
const Revision& default_revision();
/// Exact revision accepted by the checked Jak II import and preparation pipeline.
const Revision& import_revision();

}  // namespace jak2_iso
