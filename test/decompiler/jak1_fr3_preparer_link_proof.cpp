#include <filesystem>

#include "common/versions/jak1_iso_revisions.h"
#include "decompiler/extractor/jak1_fr3_preparer.h"

int main() {
  const auto result = jak1_fr3::prepare(std::filesystem::path(), std::filesystem::path(),
                                        std::filesystem::path(), jak1_iso::default_revision());
  return result ? 1 : 0;
}
