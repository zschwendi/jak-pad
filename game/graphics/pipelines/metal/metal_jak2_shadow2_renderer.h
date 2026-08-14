#pragma once

#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"

namespace metal_renderer {

struct Jak2ShadowBucket195Plan;

class MetalJak2Shadow2Renderer final : public MetalBucketRenderer {
 public:
  struct Stats {
    u32 executions = 0;
    u32 absent = 0;
    u32 ready = 0;
    u32 accepted_deferred_no_draw = 0;
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
    bool reached_boundary = false;
  };

  MetalJak2Shadow2Renderer(const std::string& name, int my_id)
      : MetalBucketRenderer(name, my_id) {}

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  const Stats& stats() const { return m_stats; }

 private:
  struct Vertex {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    u32 pad = 0;
  };

  struct Geometry {
    std::vector<Vertex> front;
    std::vector<Vertex> back;
  };

  void consume_exact_plan(DmaFollower& dma,
                          const MetalSharedRenderState& render_state,
                          const Jak2ShadowBucket195Plan& plan);
  bool build_geometry(const Jak2ShadowBucket195Plan& plan, Geometry* geometry);
  bool projection_is_finite(const Jak2ShadowBucket195Plan& plan,
                            const Geometry& geometry) const;
  void draw(const Jak2ShadowBucket195Plan& plan,
            const Geometry& geometry,
            MetalFrameContext& ctx);

  Stats m_stats;
};

}  // namespace metal_renderer
