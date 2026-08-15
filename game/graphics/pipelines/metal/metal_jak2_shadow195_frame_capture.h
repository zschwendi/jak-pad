#pragma once

#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "common/common_types.h"

namespace metal_renderer {

enum class Jak2Shadow195FrameCaptureStatus : u32 {
  Idle = 0,
  Armed = 1,
  Selected = 2,
  Complete = 3,
  Failed = 4,
};

enum class Jak2Shadow195FrameCaptureFailure : u32 {
  None = 0,
  InvalidContext = 1,
  UnsupportedTarget = 2,
  AllocationFailed = 3,
  RendererFailed = 4,
  CommandBufferFailed = 5,
  ReadbackFailed = 6,
  PlanNotReady = 7,
};

enum Jak2Shadow195FrameCaptureMatch : u32 {
  Jak2Shadow195MatchHostTick = 1u << 0,
  Jak2Shadow195MatchEngineFrame = 1u << 1,
  Jak2Shadow195MatchPlanFingerprint = 1u << 2,
};

inline constexpr u32 kJak2Shadow195FrameCaptureMatchMask =
    Jak2Shadow195MatchHostTick | Jak2Shadow195MatchEngineFrame |
    Jak2Shadow195MatchPlanFingerprint;
inline constexpr u32 kJak2Shadow195FrameCaptureMaxDimension = 4096;
inline constexpr u64 kJak2Shadow195FrameCaptureReadbackAlignment = 256;
inline constexpr u64 kJak2Shadow195FrameCaptureMaxReadbackBytes = 128ull * 1024 * 1024;

struct Jak2Shadow195ReadbackPlaneLayout {
  u64 offset = 0;
  u64 row_bytes = 0;
};

struct Jak2Shadow195ReadbackLayout {
  Jak2Shadow195ReadbackPlaneLayout before_color;
  Jak2Shadow195ReadbackPlaneLayout before_depth;
  Jak2Shadow195ReadbackPlaneLayout before_stencil;
  Jak2Shadow195ReadbackPlaneLayout volume_stencil;
  Jak2Shadow195ReadbackPlaneLayout after_color;
  Jak2Shadow195ReadbackPlaneLayout after_depth;
  Jak2Shadow195ReadbackPlaneLayout after_stencil;
  u64 total_bytes = 0;
};

struct Jak2Shadow195FrameCaptureSelector {
  u32 match = 0;
  u64 host_tick_id = 0;
  u64 engine_frame_id = 0;
  u64 plan_fingerprint = 0;
};

struct Jak2Shadow195FrameIdentity {
  u64 host_tick_id = 0;
  u64 chain_ordinal = 0;
  u64 engine_frame_id = 0;
  u64 plan_fingerprint = 0;
};

struct Jak2Shadow195PixelBounds {
  u32 valid = 0;
  u32 min_x = 0;
  u32 min_y = 0;
  u32 max_x_exclusive = 0;
  u32 max_y_exclusive = 0;
};

struct Jak2Shadow195PixelSummary {
  u64 hash = 0;
  u64 nonzero_pixels = 0;
  Jak2Shadow195PixelBounds nonzero_bounds;
};

struct Jak2Shadow195PixelDifference {
  u64 changed_pixels = 0;
  Jak2Shadow195PixelBounds changed_bounds;
};

struct Jak2Shadow195DepthSummary {
  u64 hash = 0;
  u64 finite_pixels = 0;
  u64 nonfinite_pixels = 0;
  u32 finite_range_valid = 0;
  float min_finite = 0.f;
  float max_finite = 0.f;
};

struct Jak2Shadow195StencilSummary {
  u64 hash = 0;
  u64 nonzero_pixels = 0;
  Jak2Shadow195PixelBounds nonzero_bounds;
};

struct Jak2Shadow195RenderTargetMetadata {
  u64 view_id = 0;
  u32 external_target = 0;
  u32 width = 0;
  u32 height = 0;
  u32 pixel_format = 0;
  u32 color_slice = 0;
  u32 depth_pixel_format = 0;
  u32 depth_slice = 0;
  u32 stencil_pixel_format = 0;
  u32 stencil_slice = 0;
  u32 texture_type = 0;
  u32 storage_mode = 0;
  u32 sample_count = 0;
  u32 array_length = 0;
  u32 mipmap_level_count = 0;
  double viewport_origin_x = 0.0;
  double viewport_origin_y = 0.0;
  double viewport_width = 0.0;
  double viewport_height = 0.0;
  double viewport_znear = 0.0;
  double viewport_zfar = 0.0;
  u32 scissor_x = 0;
  u32 scissor_y = 0;
  u32 scissor_width = 0;
  u32 scissor_height = 0;
  u32 scissor_explicit = 0;
  u32 color_load_action = 0;
  u32 color_store_action = 0;
  u32 depth_load_action = 0;
  u32 depth_store_action = 0;
  u32 stencil_load_action = 0;
  u32 stencil_store_action = 0;
  double render_scale_x = 0.0;
  double render_scale_y = 0.0;
};

struct Jak2Shadow195AttachmentReadback {
  const u8* before_color = nullptr;
  std::size_t before_color_row_bytes = 0;
  const u8* before_depth = nullptr;
  std::size_t before_depth_row_bytes = 0;
  const u8* before_stencil = nullptr;
  std::size_t before_stencil_row_bytes = 0;
  const u8* volume_stencil = nullptr;
  std::size_t volume_stencil_row_bytes = 0;
  const u8* after_color = nullptr;
  std::size_t after_color_row_bytes = 0;
  const u8* after_depth = nullptr;
  std::size_t after_depth_row_bytes = 0;
  const u8* after_stencil = nullptr;
  std::size_t after_stencil_row_bytes = 0;
};

struct Jak2Shadow195CapturedRendererStats {
  u32 executions = 0;
  u32 ready = 0;
  u32 input_batches = 0;
  u32 input_vertices = 0;
  u32 input_records = 0;
  u32 output_vertices = 0;
  u32 front_triangles = 0;
  u32 back_triangles = 0;
  u32 draw_calls = 0;
  u32 triangles = 0;
  u32 darken_draws = 0;
  u32 lighten_draws = 0;
  u32 unexpected_dma = 0;
  u32 invalid_plan = 0;
  u32 nonfinite_projection = 0;
  u32 overflow = 0;
  u32 pipeline_failures = 0;
  u32 reached_boundary = 0;
};

struct Jak2Shadow195FrameCaptureResult {
  Jak2Shadow195FrameCaptureStatus status = Jak2Shadow195FrameCaptureStatus::Idle;
  u32 failure_reason = 0;
  Jak2Shadow195FrameCaptureSelector selector;
  Jak2Shadow195FrameIdentity selected;
  Jak2Shadow195RenderTargetMetadata target;
  Jak2Shadow195PixelSummary before;
  Jak2Shadow195PixelSummary after;
  Jak2Shadow195PixelDifference difference;
  Jak2Shadow195DepthSummary before_depth;
  Jak2Shadow195DepthSummary after_depth;
  Jak2Shadow195PixelDifference depth_difference;
  Jak2Shadow195StencilSummary before_stencil;
  Jak2Shadow195StencilSummary volume_stencil;
  Jak2Shadow195StencilSummary after_stencil;
  Jak2Shadow195PixelDifference volume_stencil_difference;
  Jak2Shadow195PixelDifference final_stencil_difference;
  Jak2Shadow195CapturedRendererStats renderer;
};

bool jak2_shadow195_frame_capture_selector_is_valid(
    const Jak2Shadow195FrameCaptureSelector& selector);
bool jak2_shadow195_frame_capture_selector_matches(
    const Jak2Shadow195FrameCaptureSelector& selector,
    const Jak2Shadow195FrameIdentity& identity);
bool plan_jak2_shadow195_readback_layout(u32 width,
                                        u32 height,
                                        Jak2Shadow195ReadbackLayout* layout);
Jak2Shadow195PixelSummary summarize_jak2_shadow195_pixels(const u8* bytes,
                                                         u32 width,
                                                         u32 height);
Jak2Shadow195PixelDifference diff_jak2_shadow195_pixels(const u8* before,
                                                       const u8* after,
                                                       u32 width,
                                                       u32 height);
Jak2Shadow195DepthSummary summarize_jak2_shadow195_depth(const u8* bytes,
                                                        u32 width,
                                                        u32 height);
Jak2Shadow195StencilSummary summarize_jak2_shadow195_stencil(const u8* bytes,
                                                            u32 width,
                                                            u32 height);

class Jak2Shadow195FrameCapture
    : public std::enable_shared_from_this<Jak2Shadow195FrameCapture> {
 public:
  bool arm(const Jak2Shadow195FrameCaptureSelector& selector);
  bool try_select(const Jak2Shadow195FrameIdentity& identity);
  void record_target(const Jak2Shadow195RenderTargetMetadata& target);
  void record_renderer(const Jak2Shadow195CapturedRendererStats& renderer);
  void complete(const Jak2Shadow195AttachmentReadback& readback, u32 width, u32 height);
  void fail(u32 reason);
  bool wait_for_terminal(double timeout_seconds) const;
  Jak2Shadow195FrameCaptureResult result() const;

 private:
  mutable std::mutex m_mutex;
  mutable std::condition_variable m_condition;
  Jak2Shadow195FrameCaptureResult m_result;
};

}  // namespace metal_renderer
