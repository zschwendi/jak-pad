#include "game/overlord/jak2/str_load_size.h"

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    failures++;
  }
}

}  // namespace

int main() {
  using jak2::StrLoadSizeStatus;
  using jak2::validate_str_load_size;

  constexpr u32 expected = jak2::kTitleRawScreenLength;
  check(expected == 0xd0000, "the title raw screen is exactly 512 x 416 x 4 bytes");
  check(
      validate_str_load_size("demo-screens", 27, expected, expected) == StrLoadSizeStatus::Allowed,
      "the complete title raw screen is accepted");
  check(validate_str_load_size("demo-screens", 27, expected, expected - 1) ==
            StrLoadSizeStatus::TitleRawScreenLengthMismatch,
        "a short title raw screen is rejected");
  check(validate_str_load_size("demo-screens", 27, expected, 0) ==
            StrLoadSizeStatus::TitleRawScreenLengthMismatch,
        "an empty title raw screen is rejected");
  check(validate_str_load_size("demo-screens", 26, expected, expected - 1) ==
            StrLoadSizeStatus::Allowed,
        "another section may remain shorter than its destination");
  check(validate_str_load_size("other-screens", 27, expected, expected - 1) ==
            StrLoadSizeStatus::Allowed,
        "another STR basename may remain shorter than its destination");
  check(validate_str_load_size("demo-screens", 27, expected + 64, expected) ==
            StrLoadSizeStatus::Allowed,
        "the complete title raw screen is accepted in a larger destination");
  check(validate_str_load_size("demo-screens", 27, expected + 64, expected - 1) ==
            StrLoadSizeStatus::TitleRawScreenLengthMismatch,
        "a larger destination does not hide a short title raw screen");
  check(validate_str_load_size("other-screens", 1, 1024, 1025) ==
            StrLoadSizeStatus::ExceedsDestination,
        "a chunk larger than its destination is rejected");

  std::printf("\n%s (%d failures)\n",
              failures ? "JAK 2 STR LOAD SIZE TEST FAILED" : "JAK 2 STR LOAD SIZE TEST PASSED",
              failures);
  return failures ? 1 : 0;
}
