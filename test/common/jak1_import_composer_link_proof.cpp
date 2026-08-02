#include "decompiler/extractor/jak1_import_composer.h"

int main() {
  const auto compose_function = &jak1_import_composer::compose;
  return compose_function &&
                 jak1_import_composer::error_code_name(
                     jak1_import_composer::ErrorCode::invalid_argument) &&
                 jak1_import_composer::phase_name(
                     jak1_import_composer::Phase::validating_source_pack)
             ? 0
             : 1;
}
