#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "common/custom_data/Jak2SourceObjectPack.h"
#include "common/util/FileUtil.h"

#include "goalc/make/MakeSystem.h"
#include "goalc/make/Tool.h"

namespace {

namespace stdfs = std::filesystem;

class InspectOnlyCompilerTool final : public Tool {
 public:
  InspectOnlyCompilerTool() : Tool("goalc") {}
  bool run(const ToolInput&, const PathMap&) override { return false; }
};

struct CommandLine {
  std::string pack_root;
  std::optional<fs::path> project_root;
  std::optional<std::uint64_t> expected_aggregate;
};

void print_usage() {
  std::fprintf(stderr,
               "Usage: jak2-source-object-pack-verify PACK_ROOT "
               "[--project-path OPENGOAL_ROOT] [--expected-aggregate 16_HEX_DIGITS]\n");
}

bool parse_aggregate(std::string_view text, std::uint64_t* aggregate) {
  if (text.size() != 16) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *aggregate, 16);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && *aggregate != 0;
}

bool parse_command_line(int argc, char** argv, CommandLine* command) {
  if (argc < 2) {
    return false;
  }
  command->pack_root = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view option = argv[index];
    if (++index == argc) {
      return false;
    }
    if (option == "--project-path") {
      command->project_root = fs::path(argv[index]);
    } else if (option == "--expected-aggregate") {
      std::uint64_t aggregate = 0;
      if (!parse_aggregate(argv[index], &aggregate)) {
        return false;
      }
      command->expected_aggregate = aggregate;
    } else {
      return false;
    }
  }
  return true;
}

std::vector<std::string> ordered_goal_sources(const MakeSystem& make) {
  std::vector<std::string> sources;
  for (const auto& output : make.get_dependencies("GROUP:all-code")) {
    const auto* step = make.find_step(output);
    if (!step || step->tool != "goalc") {
      continue;
    }
    if (step->input.size() != 1) {
      throw std::runtime_error("A Jak II GROUP:all-code compiler step has invalid inputs.");
    }
    sources.push_back(step->input.front());
  }
  return sources;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    print_usage();
    return 0;
  }
  CommandLine command;
  if (!parse_command_line(argc, argv, &command)) {
    print_usage();
    return 2;
  }

  if (!file_util::setup_project_path(command.project_root, true)) {
    std::fputs("Could not locate the OpenGOAL project root.\n", stderr);
    return 1;
  }

  try {
    MakeSystem make_system(std::nullopt);
    make_system.add_tool(std::make_shared<InspectOnlyCompilerTool>());
    make_system.load_project_file(file_util::get_file_path({"goal_src/jak2/game.gp"}));
    const auto sources = ordered_goal_sources(make_system);
    if (sources.size() != jak2_source_object_pack::kExpectedObjectCount) {
      std::fprintf(stderr, "Expected %u Jak II GROUP:all-code sources, found %zu.\n",
                   jak2_source_object_pack::kExpectedObjectCount, sources.size());
      return 1;
    }

    std::error_code error;
    const auto root = stdfs::absolute(command.pack_root, error);
    if (error) {
      std::fprintf(stderr, "Source object-pack verification failed (invalid_argument): %s\n",
                   error.message().c_str());
      return 1;
    }
    jak2_source_object_pack::Options options;
    if (command.expected_aggregate) {
      options.expected_identity = {jak2_source_object_pack::kExpectedObjectCount,
                                   *command.expected_aggregate};
    }
    const auto result = jak2_source_object_pack::validate(root, sources, options);
    if (!result) {
      std::fprintf(stderr, "Source object-pack verification failed (%s): %s\n",
                   jak2_source_object_pack::error_code_name(result.error().code),
                   result.error().message.c_str());
      return 1;
    }

    std::printf("Verified %u Jak II source objects (aggregate %016llx, %llu bytes).\n",
                result.value().identity.object_count,
                static_cast<unsigned long long>(result.value().identity.aggregate_xxh64),
                static_cast<unsigned long long>(result.value().total_object_bytes));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Source object-pack verification failed: %s\n", error.what());
    return 1;
  }
}
