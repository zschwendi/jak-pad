#include "decompiler/extractor/jak2_import_composer.h"

int main() {
  const auto compose_function = &jak2_import_composer::compose;
  return compose_function &&
                 jak2_import_composer::error_code_name(
                     jak2_import_composer::ErrorCode::retail_catalog_failed) &&
                 jak2_import_composer::phase_name(
                     jak2_import_composer::Phase::cataloging_retail)
             ? 0
             : 1;
}
