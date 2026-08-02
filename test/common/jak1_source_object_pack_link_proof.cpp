#include <filesystem>

#include "common/custom_data/Jak1SourceObjectPack.h"

int main() {
  const auto result = jak1_source_object_pack::validate(std::filesystem::path());
  return result ? 1 : 0;
}
