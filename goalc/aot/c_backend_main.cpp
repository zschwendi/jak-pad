/*!
 * @file c_backend_main.cpp
 * Compile GOAL source files with the normal goalc front end and emit C for the last one, so that
 * clang can provide the ARM64 backend for ahead-of-time builds.
 */

#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "goalc/aot/CBackend.h"
#include "goalc/compiler/Compiler.h"

namespace {

struct Options {
  std::vector<std::string> inputs;
  std::string output_path;
  std::string header_path;
  std::string report_path;
  std::string tag = "aot";
  std::string project_path;
};

void print_usage() {
  std::fprintf(stderr,
               "Usage: goalc-cbackend --output OUT.c [--tag NAME] [--report OUT.txt]\n"
               "                     [--project-path OPENGOAL_ROOT] SOURCE.gc [SOURCE.gc ...]\n"
               "\n"
               "Every source is compiled in order into one compiler instance (matching how the\n"
               "make system builds the game). C is emitted for the last source only.\n");
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; i++) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return false;
    }
    if (argument == "--output" || argument == "--tag" || argument == "--report" ||
        argument == "--header" || argument == "--project-path") {
      if (++i == argc) {
        std::fprintf(stderr, "Missing value for %s\n", argument.c_str());
        return false;
      }
      if (argument == "--output") {
        options->output_path = argv[i];
      } else if (argument == "--header") {
        options->header_path = argv[i];
      } else if (argument == "--tag") {
        options->tag = argv[i];
      } else if (argument == "--report") {
        options->report_path = argv[i];
      } else {
        options->project_path = argv[i];
      }
      continue;
    }
    options->inputs.push_back(argument);
  }

  if (options->inputs.empty() || options->output_path.empty()) {
    print_usage();
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, &options)) {
    return 2;
  }

  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  std::optional<fs::path> project_path;
  if (!options.project_path.empty()) {
    project_path = fs::path(options.project_path);
  }
  if (!file_util::setup_project_path(project_path, true)) {
    std::fprintf(stderr, "Could not locate the OpenGOAL project root.\n");
    return 1;
  }

  try {
    Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::X86);

    FileEnv* last = nullptr;
    for (const auto& input : options.inputs) {
      const auto source = file_util::read_text_file(input);
      auto code = compiler.get_goos().reader.read_from_string(source, true);
      last = compiler.compile_object_file(input, std::move(code), true);
    }

    auto result = aot::emit_c_file(*last, options.tag, GameVersion::Jak1, compiler.type_system());
    file_util::write_text_file(options.output_path, result.source);
    if (!options.header_path.empty()) {
      file_util::write_text_file(options.header_path, result.header);
    }

    std::string report;
    report += fmt::format("{}: {}/{} functions emitted\n", options.inputs.back(),
                          result.emitted_count(), result.total_count());
    for (const auto& f : result.functions) {
      if (!f.ok) {
        report += fmt::format("  FAILED {}: {}\n", f.goal_name, f.error);
      }
    }
    std::fputs(report.c_str(), stdout);
    if (!options.report_path.empty()) {
      file_util::write_text_file(options.report_path, report);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "goalc-cbackend: %s\n", error.what());
    return 1;
  }

  return 0;
}
