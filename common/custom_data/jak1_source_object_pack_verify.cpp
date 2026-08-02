#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <system_error>

#include "common/custom_data/Jak1SourceObjectPack.h"

namespace {

void print_usage() {
  std::fprintf(stderr,
               "Usage: jak1-source-object-pack-verify PACK_ROOT "
               "[--expected-aggregate 16_HEX_DIGITS]\n");
}

bool parse_aggregate(std::string_view text, std::uint64_t* aggregate) {
  if (text.size() != 16) {
    return false;
  }
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), *aggregate, 16);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    print_usage();
    return 0;
  }
  if (argc != 2 && argc != 4) {
    print_usage();
    return 2;
  }

  std::uint64_t expected_aggregate = jak1_source_object_pack::kRecordedAggregateXXH64;
  if (argc == 4) {
    if (std::string_view(argv[2]) != "--expected-aggregate" ||
        !parse_aggregate(argv[3], &expected_aggregate) || expected_aggregate == 0) {
      print_usage();
      return 2;
    }
  }

  std::error_code error;
  const auto root = std::filesystem::absolute(argv[1], error);
  if (error) {
    std::fprintf(stderr, "Source object-pack verification failed (invalid_argument): %s\n",
                 error.message().c_str());
    return 1;
  }
  jak1_source_object_pack::Options options;
  options.expected_identity = {jak1_source_object_pack::kExpectedObjectCount, expected_aggregate};
  const auto result = jak1_source_object_pack::validate(root, options);
  if (!result) {
    std::fprintf(stderr, "Source object-pack verification failed (%s): %s\n",
                 jak1_source_object_pack::error_code_name(result.error().code),
                 result.error().message.c_str());
    return 1;
  }

  std::printf("Verified %u source objects (aggregate %016llx, %llu bytes).\n",
              result.value().identity.object_count,
              static_cast<unsigned long long>(result.value().identity.aggregate_xxh64),
              static_cast<unsigned long long>(result.value().total_object_bytes));
  return 0;
}
