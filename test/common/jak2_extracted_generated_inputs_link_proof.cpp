#include <filesystem>

#include "decompiler/extractor/jak2_extracted_generated_inputs.h"

int main() {
  const jak2_extracted_generated_inputs::ValidatedTree tree{std::filesystem::path(),
                                                            jak2_iso::default_revision()};
  const auto result = jak2_extracted_generated_inputs::build(tree);
  return result ? 1 : 0;
}
