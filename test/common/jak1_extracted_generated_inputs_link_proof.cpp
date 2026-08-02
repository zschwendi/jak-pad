#include <filesystem>

#include "decompiler/extractor/jak1_extracted_generated_inputs.h"

int main() {
  const jak1_extracted_generated_inputs::ValidatedTree tree{std::filesystem::path(),
                                                            jak1_iso::default_revision()};
  const auto result = jak1_extracted_generated_inputs::build(tree, {});
  return result ? 1 : 0;
}
