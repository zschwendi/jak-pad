#pragma once

#include <memory>

#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture.h"

struct MetalFrameContext;
class MetalBucketRenderer;

namespace metal_renderer {

class MetalJak2Shadow2Renderer;

enum class Jak2Shadow195CaptureDispatchRoute : u32 {
  DeferredPrivate = 0,
  ProductionDirect = 1,
  Unsupported = 2,
};

Jak2Shadow195CaptureDispatchRoute route_jak2_shadow195_frame_capture(
    MetalBucketRenderer* configured,
    MetalJak2Shadow2Renderer* private_renderer,
    MetalBucketRenderer** selected);

class Jak2Shadow195AttachmentCapture {
 public:
  static std::unique_ptr<Jak2Shadow195AttachmentCapture> begin(
      Jak2Shadow195FrameCapture* capture,
      MetalFrameContext& context,
      u64 view_id,
      bool external_target,
      double render_scale_x,
      double render_scale_y);

  ~Jak2Shadow195AttachmentCapture();

  void record_post_volume(MetalFrameContext& context);
  void finish(MetalFrameContext& context,
              const Jak2Shadow195CapturedRendererStats& renderer_stats);

 private:
  struct Impl;

  explicit Jak2Shadow195AttachmentCapture(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> m_impl;
};

}  // namespace metal_renderer
