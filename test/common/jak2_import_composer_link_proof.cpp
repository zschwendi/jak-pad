#include "decompiler/extractor/jak2_import_composer.h"

int main() {
  const auto compose_function = &jak2_import_composer::compose;
  return compose_function &&
                 jak2_import_composer::error_code_name(
                     jak2_import_composer::ErrorCode::prepared_output_unavailable) &&
                 jak2_import_composer::phase_name(
                     jak2_import_composer::Phase::validating_source_pack)
             ? 0
             : 1;
}
