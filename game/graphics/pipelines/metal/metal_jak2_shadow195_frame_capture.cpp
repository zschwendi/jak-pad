#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace metal_renderer {
namespace {

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr u64 kBytesPerPixel = 4;

bool valid_dimensions(u32 width, u32 height) {
  if (!width || !height || width > kJak2Shadow195FrameCaptureMaxDimension ||
      height > kJak2Shadow195FrameCaptureMaxDimension) {
    return false;
  }
  const u64 byte_count = static_cast<u64>(width) * height * kBytesPerPixel;
  return byte_count <= kJak2Shadow195FrameCaptureMaxReadbackBytes;
}

void include_pixel(Jak2Shadow195PixelBounds* bounds, u32 x, u32 y) {
  if (!bounds->valid) {
    *bounds = {1, x, y, x + 1, y + 1};
    return;
  }
  bounds->min_x = std::min(bounds->min_x, x);
  bounds->min_y = std::min(bounds->min_y, y);
  bounds->max_x_exclusive = std::max(bounds->max_x_exclusive, x + 1);
  bounds->max_y_exclusive = std::max(bounds->max_y_exclusive, y + 1);
}

bool append_readback_plane(Jak2Shadow195ReadbackPlaneLayout* plane,
                           u32 width,
                           u32 height,
                           u64 bytes_per_pixel,
                           u64* cursor) {
  const u64 raw_row_bytes = static_cast<u64>(width) * bytes_per_pixel;
  const u64 row_bytes =
      (raw_row_bytes + kJak2Shadow195FrameCaptureReadbackAlignment - 1) &
      ~(kJak2Shadow195FrameCaptureReadbackAlignment - 1);
  const u64 plane_bytes = row_bytes * height;
  if (*cursor > kJak2Shadow195FrameCaptureMaxReadbackBytes ||
      plane_bytes > kJak2Shadow195FrameCaptureMaxReadbackBytes - *cursor) {
    return false;
  }
  plane->offset = *cursor;
  plane->row_bytes = row_bytes;
  *cursor += plane_bytes;
  return true;
}

u64 hash_rows(const u8* bytes,
              u32 width,
              u32 height,
              std::size_t bytes_per_pixel,
              std::size_t bytes_per_row) {
  u64 hash = kFnvOffsetBasis;
  for (u32 y = 0; y < height; ++y) {
    const u8* row = bytes + y * bytes_per_row;
    for (std::size_t byte = 0; byte < static_cast<std::size_t>(width) * bytes_per_pixel; ++byte) {
      hash ^= row[byte];
      hash *= kFnvPrime;
    }
  }
  return hash;
}

Jak2Shadow195PixelSummary summarize_color(const u8* bytes,
                                          u32 width,
                                          u32 height,
                                          std::size_t bytes_per_row) {
  Jak2Shadow195PixelSummary result;
  if (!bytes || !valid_dimensions(width, height) || bytes_per_row < width * kBytesPerPixel) {
    return result;
  }
  result.hash = hash_rows(bytes, width, height, kBytesPerPixel, bytes_per_row);
  for (u32 y = 0; y < height; ++y) {
    const u8* row = bytes + y * bytes_per_row;
    for (u32 x = 0; x < width; ++x) {
      const u8* pixel = row + x * kBytesPerPixel;
      if (pixel[0] || pixel[1] || pixel[2] || pixel[3]) {
        ++result.nonzero_pixels;
        include_pixel(&result.nonzero_bounds, x, y);
      }
    }
  }
  return result;
}

Jak2Shadow195PixelDifference diff_pixels(const u8* before,
                                        std::size_t before_row_bytes,
                                        const u8* after,
                                        std::size_t after_row_bytes,
                                        u32 width,
                                        u32 height,
                                        std::size_t bytes_per_pixel) {
  Jak2Shadow195PixelDifference result;
  if (!before || !after || !valid_dimensions(width, height) ||
      before_row_bytes < width * bytes_per_pixel ||
      after_row_bytes < width * bytes_per_pixel) {
    return result;
  }
  for (u32 y = 0; y < height; ++y) {
    const u8* before_row = before + y * before_row_bytes;
    const u8* after_row = after + y * after_row_bytes;
    for (u32 x = 0; x < width; ++x) {
      if (std::memcmp(before_row + x * bytes_per_pixel, after_row + x * bytes_per_pixel,
                      bytes_per_pixel) != 0) {
        ++result.changed_pixels;
        include_pixel(&result.changed_bounds, x, y);
      }
    }
  }
  return result;
}

Jak2Shadow195DepthSummary summarize_depth(const u8* bytes,
                                         u32 width,
                                         u32 height,
                                         std::size_t bytes_per_row) {
  Jak2Shadow195DepthSummary result;
  if (!bytes || !valid_dimensions(width, height) || bytes_per_row < width * sizeof(float)) {
    return result;
  }
  result.hash = hash_rows(bytes, width, height, sizeof(float), bytes_per_row);
  float min_finite = std::numeric_limits<float>::max();
  float max_finite = std::numeric_limits<float>::lowest();
  for (u32 y = 0; y < height; ++y) {
    const u8* row = bytes + y * bytes_per_row;
    for (u32 x = 0; x < width; ++x) {
      float value = 0.f;
      std::memcpy(&value, row + x * sizeof(float), sizeof(value));
      if (std::isfinite(value)) {
        ++result.finite_pixels;
        min_finite = std::min(min_finite, value);
        max_finite = std::max(max_finite, value);
      } else {
        ++result.nonfinite_pixels;
      }
    }
  }
  if (result.finite_pixels) {
    result.finite_range_valid = 1;
    result.min_finite = min_finite;
    result.max_finite = max_finite;
  }
  return result;
}

Jak2Shadow195StencilSummary summarize_stencil(const u8* bytes,
                                             u32 width,
                                             u32 height,
                                             std::size_t bytes_per_row) {
  Jak2Shadow195StencilSummary result;
  if (!bytes || !valid_dimensions(width, height) || bytes_per_row < width) {
    return result;
  }
  result.hash = hash_rows(bytes, width, height, 1, bytes_per_row);
  for (u32 y = 0; y < height; ++y) {
    const u8* row = bytes + y * bytes_per_row;
    for (u32 x = 0; x < width; ++x) {
      if (row[x]) {
        ++result.nonzero_pixels;
        include_pixel(&result.nonzero_bounds, x, y);
      }
    }
  }
  return result;
}

}  // namespace

bool jak2_shadow195_frame_capture_selector_is_valid(
    const Jak2Shadow195FrameCaptureSelector& selector) {
  if (!selector.match || (selector.match & ~kJak2Shadow195FrameCaptureMatchMask)) {
    return false;
  }
  return !(selector.match & Jak2Shadow195MatchPlanFingerprint) || selector.plan_fingerprint != 0;
}

bool jak2_shadow195_frame_capture_selector_matches(
    const Jak2Shadow195FrameCaptureSelector& selector,
    const Jak2Shadow195FrameIdentity& identity) {
  return jak2_shadow195_frame_capture_selector_is_valid(selector) &&
         (!(selector.match & Jak2Shadow195MatchHostTick) ||
          selector.host_tick_id == identity.host_tick_id) &&
         (!(selector.match & Jak2Shadow195MatchEngineFrame) ||
          selector.engine_frame_id == identity.engine_frame_id) &&
         (!(selector.match & Jak2Shadow195MatchPlanFingerprint) ||
         selector.plan_fingerprint == identity.plan_fingerprint);
}

bool plan_jak2_shadow195_readback_layout(u32 width,
                                        u32 height,
                                        Jak2Shadow195ReadbackLayout* layout) {
  if (!layout) {
    return false;
  }
  *layout = {};
  if (!valid_dimensions(width, height)) {
    return false;
  }
  u64 cursor = 0;
  if (!append_readback_plane(&layout->before_color, width, height, 4, &cursor) ||
      !append_readback_plane(&layout->before_depth, width, height, 4, &cursor) ||
      !append_readback_plane(&layout->before_stencil, width, height, 1, &cursor) ||
      !append_readback_plane(&layout->volume_stencil, width, height, 1, &cursor) ||
      !append_readback_plane(&layout->after_color, width, height, 4, &cursor) ||
      !append_readback_plane(&layout->after_depth, width, height, 4, &cursor) ||
      !append_readback_plane(&layout->after_stencil, width, height, 1, &cursor)) {
    *layout = {};
    return false;
  }
  layout->total_bytes = cursor;
  return true;
}

Jak2Shadow195PixelSummary summarize_jak2_shadow195_pixels(const u8* bytes,
                                                         u32 width,
                                                         u32 height) {
  return summarize_color(bytes, width, height, static_cast<std::size_t>(width) * kBytesPerPixel);
}

Jak2Shadow195PixelDifference diff_jak2_shadow195_pixels(const u8* before,
                                                       const u8* after,
                                                       u32 width,
                                                       u32 height) {
  const std::size_t row_bytes = static_cast<std::size_t>(width) * kBytesPerPixel;
  return diff_pixels(before, row_bytes, after, row_bytes, width, height, kBytesPerPixel);
}

Jak2Shadow195DepthSummary summarize_jak2_shadow195_depth(const u8* bytes,
                                                        u32 width,
                                                        u32 height) {
  return summarize_depth(bytes, width, height, static_cast<std::size_t>(width) * sizeof(float));
}

Jak2Shadow195StencilSummary summarize_jak2_shadow195_stencil(const u8* bytes,
                                                            u32 width,
                                                            u32 height) {
  return summarize_stencil(bytes, width, height, width);
}

bool Jak2Shadow195FrameCapture::arm(const Jak2Shadow195FrameCaptureSelector& selector) {
  if (!jak2_shadow195_frame_capture_selector_is_valid(selector)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status == Jak2Shadow195FrameCaptureStatus::Armed ||
      m_result.status == Jak2Shadow195FrameCaptureStatus::Selected) {
    return false;
  }
  m_result = {};
  m_result.status = Jak2Shadow195FrameCaptureStatus::Armed;
  m_result.selector = selector;
  return true;
}

bool Jak2Shadow195FrameCapture::try_select(const Jak2Shadow195FrameIdentity& identity) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status != Jak2Shadow195FrameCaptureStatus::Armed ||
      !jak2_shadow195_frame_capture_selector_matches(m_result.selector, identity)) {
    return false;
  }
  m_result.status = Jak2Shadow195FrameCaptureStatus::Selected;
  m_result.selected = identity;
  return true;
}

void Jak2Shadow195FrameCapture::record_target(
    const Jak2Shadow195RenderTargetMetadata& target) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status == Jak2Shadow195FrameCaptureStatus::Selected) {
    m_result.target = target;
  }
}

void Jak2Shadow195FrameCapture::record_renderer(
    const Jak2Shadow195CapturedRendererStats& renderer) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status == Jak2Shadow195FrameCaptureStatus::Selected) {
    m_result.renderer = renderer;
  }
}

void Jak2Shadow195FrameCapture::complete(const Jak2Shadow195AttachmentReadback& readback,
                                        u32 width,
                                        u32 height) {
  const std::size_t color_bytes = static_cast<std::size_t>(width) * kBytesPerPixel;
  const std::size_t depth_bytes = static_cast<std::size_t>(width) * sizeof(float);
  if (!valid_dimensions(width, height) || !readback.before_color || !readback.before_depth ||
      !readback.before_stencil || !readback.volume_stencil || !readback.after_color ||
      !readback.after_depth || !readback.after_stencil ||
      readback.before_color_row_bytes < color_bytes ||
      readback.before_depth_row_bytes < depth_bytes || readback.before_stencil_row_bytes < width ||
      readback.volume_stencil_row_bytes < width || readback.after_color_row_bytes < color_bytes ||
      readback.after_depth_row_bytes < depth_bytes || readback.after_stencil_row_bytes < width) {
    fail(static_cast<u32>(Jak2Shadow195FrameCaptureFailure::ReadbackFailed));
    return;
  }
  const auto before_summary =
      summarize_color(readback.before_color, width, height, readback.before_color_row_bytes);
  const auto after_summary =
      summarize_color(readback.after_color, width, height, readback.after_color_row_bytes);
  const auto difference =
      diff_pixels(readback.before_color, readback.before_color_row_bytes, readback.after_color,
                  readback.after_color_row_bytes, width, height, kBytesPerPixel);
  const auto before_depth =
      summarize_depth(readback.before_depth, width, height, readback.before_depth_row_bytes);
  const auto after_depth =
      summarize_depth(readback.after_depth, width, height, readback.after_depth_row_bytes);
  const auto depth_difference =
      diff_pixels(readback.before_depth, readback.before_depth_row_bytes, readback.after_depth,
                  readback.after_depth_row_bytes, width, height, sizeof(float));
  const auto before_stencil =
      summarize_stencil(readback.before_stencil, width, height, readback.before_stencil_row_bytes);
  const auto volume_stencil =
      summarize_stencil(readback.volume_stencil, width, height, readback.volume_stencil_row_bytes);
  const auto after_stencil =
      summarize_stencil(readback.after_stencil, width, height, readback.after_stencil_row_bytes);
  const auto volume_stencil_difference =
      diff_pixels(readback.before_stencil, readback.before_stencil_row_bytes,
                  readback.volume_stencil, readback.volume_stencil_row_bytes, width, height, 1);
  const auto final_stencil_difference =
      diff_pixels(readback.volume_stencil, readback.volume_stencil_row_bytes,
                  readback.after_stencil, readback.after_stencil_row_bytes, width, height, 1);
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status != Jak2Shadow195FrameCaptureStatus::Selected) {
    return;
  }
  m_result.before = before_summary;
  m_result.after = after_summary;
  m_result.difference = difference;
  m_result.before_depth = before_depth;
  m_result.after_depth = after_depth;
  m_result.depth_difference = depth_difference;
  m_result.before_stencil = before_stencil;
  m_result.volume_stencil = volume_stencil;
  m_result.after_stencil = after_stencil;
  m_result.volume_stencil_difference = volume_stencil_difference;
  m_result.final_stencil_difference = final_stencil_difference;
  m_result.status = Jak2Shadow195FrameCaptureStatus::Complete;
  m_condition.notify_all();
}

void Jak2Shadow195FrameCapture::fail(u32 reason) {
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_result.status != Jak2Shadow195FrameCaptureStatus::Selected) {
    return;
  }
  m_result.status = Jak2Shadow195FrameCaptureStatus::Failed;
  m_result.failure_reason = reason;
  m_condition.notify_all();
}

bool Jak2Shadow195FrameCapture::wait_for_terminal(double timeout_seconds) const {
  if (timeout_seconds <= 0.0) {
    return false;
  }
  std::unique_lock<std::mutex> lock(m_mutex);
  return m_condition.wait_for(lock, std::chrono::duration<double>(timeout_seconds), [&] {
    return m_result.status == Jak2Shadow195FrameCaptureStatus::Complete ||
           m_result.status == Jak2Shadow195FrameCaptureStatus::Failed;
  });
}

Jak2Shadow195FrameCaptureResult Jak2Shadow195FrameCapture::result() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_result;
}

}  // namespace metal_renderer
