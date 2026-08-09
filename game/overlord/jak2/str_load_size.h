#pragma once

#include <string_view>

#include "common/common_types.h"

namespace jak2 {

constexpr std::string_view kTitleRawScreenBasename = "demo-screens";
constexpr s32 kTitleRawScreenSection = 27;
constexpr u32 kTitleRawScreenLength = 512 * 416 * sizeof(u32);

enum class StrLoadSizeStatus {
  Allowed,
  ExceedsDestination,
  TitleRawScreenLengthMismatch,
};

constexpr StrLoadSizeStatus validate_str_load_size(std::string_view basename,
                                                   s32 section,
                                                   u32 maxlen,
                                                   u32 read_length) {
  if (read_length > maxlen) {
    return StrLoadSizeStatus::ExceedsDestination;
  }

  const bool is_title_raw_screen =
      basename == kTitleRawScreenBasename && section == kTitleRawScreenSection;
  if (is_title_raw_screen && read_length != kTitleRawScreenLength) {
    return StrLoadSizeStatus::TitleRawScreenLengthMismatch;
  }

  return StrLoadSizeStatus::Allowed;
}

}  // namespace jak2
