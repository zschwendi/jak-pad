#include <filesystem>

#include "common/custom_data/Jak2SourceObjectPack.h"

int main() {
  const auto result = jak2_source_object_pack::validate_recorded(std::filesystem::path());
  return result ? 1 : 0;
}
