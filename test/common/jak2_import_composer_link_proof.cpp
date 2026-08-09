#include "decompiler/extractor/jak2_import_composer.h"

int main() {
  const auto compose_function = &jak2_import_composer::compose;
  return compose_function &&
                 jak2_import_composer::error_code_name(
                     jak2_import_composer::ErrorCode::candidate_finalize_failed) &&
                 jak2_import_composer::phase_name(
                     jak2_import_composer::Phase::materializing_output)
             ? 0
             : 1;
}
