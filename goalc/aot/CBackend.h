#pragma once

/*!
 * @file CBackend.h
 * Emit C from the GOAL compiler's IR, before register allocation, so that clang can supply the
 * ARM64 backend for ahead-of-time builds.
 */

#include <string>
#include <vector>

#include "common/versions/versions.h"

class FileEnv;
class FunctionEnv;
class TypeSystem;

namespace aot {

struct CBackendFunctionResult {
  std::string goal_name;
  std::string c_name;
  std::string prototype;
  bool ok = false;
  std::string error;
};

struct CBackendResult {
  /*! The file tag after mangling. The runtime looks statics and functions up by this exact name. */
  std::string tag;
  std::string source;
  std::string header;
  std::vector<CBackendFunctionResult> functions;

  int emitted_count() const;
  int total_count() const;
};

/*!
 * Translate every function in a compiled FileEnv into one C translation unit.
 * Functions that use an IR node the backend cannot express are skipped and reported in
 * CBackendResult::functions, so partial coverage is visible instead of hidden.
 */
CBackendResult emit_c_file(FileEnv& file,
                           const std::string& file_tag,
                           GameVersion version,
                           const TypeSystem& types);

}  // namespace aot
