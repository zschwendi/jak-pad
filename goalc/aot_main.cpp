#include <cstdio>
#include <exception>
#include <optional>
#include <string>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "goalc/aot/AOTWriter.h"
#include "goalc/compiler/Compiler.h"
#include "goalc/emitter/InstructionSet.h"

namespace {

struct Options {
  std::string input_path;
  std::string output_path;
  std::string project_path;
  std::string symbol = "goalpad_aot_entry";
  std::optional<std::string> function_name;
};

void print_usage() {
  std::fprintf(stderr,
               "Usage: goalc-aot --input SOURCE.gc --output BUILD_OUTPUT.s "
               "[--project-path OPENGOAL_ROOT] [--symbol C_SYMBOL] "
               "[--function GOAL_FUNCTION]\n");
}

bool parse_options(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; i++) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return false;
    }
    if (argument == "--input" || argument == "--output" || argument == "--project-path" ||
        argument == "--symbol" || argument == "--function") {
      if (++i == argc) {
        std::fprintf(stderr, "Missing value for %s\n", argument.c_str());
        return false;
      }
      if (argument == "--input") {
        options->input_path = argv[i];
      } else if (argument == "--output") {
        options->output_path = argv[i];
      } else if (argument == "--project-path") {
        options->project_path = argv[i];
      } else if (argument == "--symbol") {
        options->symbol = argv[i];
      } else {
        options->function_name = argv[i];
      }
      continue;
    }

    std::fprintf(stderr, "Unknown option: %s\n", argument.c_str());
    return false;
  }

  if (options->input_path.empty() || options->output_path.empty()) {
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

  lg::set_stdout_level(lg::level::info);
  lg::set_flush_level(lg::level::info);
  lg::initialize();

  std::optional<fs::path> project_path;
  if (!options.project_path.empty()) {
    project_path = fs::path(options.project_path);
    if (!fs::is_directory(*project_path)) {
      std::fprintf(stderr, "OpenGOAL project path is not a directory: %s\n",
                   options.project_path.c_str());
      return 1;
    }
  }

  if (!file_util::setup_project_path(project_path, true)) {
    std::fprintf(stderr, "Could not locate the OpenGOAL project root.\n");
    return 1;
  }

  try {
    Compiler compiler(GameVersion::Jak1, emitter::InstructionSet::ARM64);
    const auto source = file_util::read_text_file(options.input_path);
    const auto code =
        compiler.compile_arm64_aot_source(source, options.input_path, options.function_name);
    aot::write_apple_arm64_assembly(options.output_path, {options.symbol, code});
  } catch (const std::exception& error) {
    std::fprintf(stderr, "goalc-aot: %s\n", error.what());
    return 1;
  }

  return 0;
}
