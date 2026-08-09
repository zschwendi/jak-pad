#pragma once

#include <cstdint>
#include <string>

namespace checked_file_identity {

struct Identity {
  std::string relative_path;
  std::uint64_t size = 0;
  std::uint64_t xxh64 = 0;

  bool operator==(const Identity&) const = default;
};

}  // namespace checked_file_identity
