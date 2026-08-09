#include <filesystem>

#include "decompiler/extractor/jak2_fr3_preparer.h"

int main() {
  const auto result = jak2_fr3::prepare(std::filesystem::path(), std::filesystem::path(),
                                        std::filesystem::path());
  return result ? 1 : 0;
}
