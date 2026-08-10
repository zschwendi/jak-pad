#pragma once

#include <algorithm>
#include <cstdint>

#ifndef GOALPAD_VISION_STEREO_EYE_MARKERS
#define GOALPAD_VISION_STEREO_EYE_MARKERS 0
#endif

namespace metal_renderer {

enum class StereoEyeMarkerEye {
  left,
  right,
};

struct StereoEyeMarkerPlan {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;

  constexpr bool valid() const { return width != 0 && height != 0; }
};

constexpr std::uint32_t kStereoEyeMarkerInset = 14;
constexpr std::uint32_t kStereoEyeMarkerLength = 16;
constexpr std::uint32_t kStereoEyeMarkerThickness = 3;

constexpr StereoEyeMarkerPlan stereo_eye_marker_plan(std::uint32_t width,
                                                     std::uint32_t height,
                                                     StereoEyeMarkerEye eye) {
  if (width <= kStereoEyeMarkerInset || height <= kStereoEyeMarkerInset) {
    return {};
  }

  const auto available_width = width - kStereoEyeMarkerInset;
  const auto available_height = height - kStereoEyeMarkerInset;
  switch (eye) {
    case StereoEyeMarkerEye::left:
      return {kStereoEyeMarkerInset, kStereoEyeMarkerInset,
              std::min(kStereoEyeMarkerThickness, available_width),
              std::min(kStereoEyeMarkerLength, available_height)};
    case StereoEyeMarkerEye::right:
      return {kStereoEyeMarkerInset, kStereoEyeMarkerInset,
              std::min(kStereoEyeMarkerLength, available_width),
              std::min(kStereoEyeMarkerThickness, available_height)};
  }
  return {};
}

}  // namespace metal_renderer
