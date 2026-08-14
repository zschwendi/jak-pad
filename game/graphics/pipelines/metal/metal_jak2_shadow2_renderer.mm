#include "game/graphics/pipelines/metal/metal_jak2_shadow2_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <stdexcept>

#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_plan.h"

namespace metal_renderer {
namespace {

constexpr u32 kMaximumOutputVertices = 8192 * 3 * 2;
constexpr float kJak2HeightScale = 0.5f;
constexpr float kJak2ScissorAdjust = 512.f / 416.f;

struct Shadow2VsParams {
  float perspective[16] = {};
  float hvdf_offset[4] = {};
  float fog = 0.f;
  float height_scale = kJak2HeightScale;
  float scissor_adjust = kJak2ScissorAdjust;
  u32 clear_mode = 0;
};
static_assert(sizeof(Shadow2VsParams) == 96);
static_assert(sizeof(MetalJak2Shadow2Renderer::Stats) > 0);

template <typename T>
T read_unaligned(const u8* source) {
  T value;
  std::memcpy(&value, source, sizeof(value));
  return value;
}

template <typename Vertex>
bool finite_vertex(const Vertex& vertex) {
  return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z);
}

template <typename Vertex>
float normal_dot_eye(const Vertex& a, const Vertex& b, const Vertex& c) {
  const float abx = b.x - a.x;
  const float aby = b.y - a.y;
  const float abz = b.z - a.z;
  const float acx = c.x - a.x;
  const float acy = c.y - a.y;
  const float acz = c.z - a.z;
  const float nx = aby * acz - abz * acy;
  const float ny = abz * acx - abx * acz;
  const float nz = abx * acy - aby * acx;
  return nx * a.x + ny * a.y + nz * a.z;
}

}  // namespace

void MetalJak2Shadow2Renderer::consume_exact_plan(
    DmaFollower& dma,
    const MetalSharedRenderState& render_state,
    const Jak2ShadowBucket195Plan& plan) {
  const u64 expected_offset = static_cast<u64>(render_state.buckets_base) +
                              static_cast<u64>(m_my_id) * 16;
  if (expected_offset > std::numeric_limits<u32>::max() ||
      dma.current_tag_offset() != static_cast<u32>(expected_offset) || plan.transfer_count == 0) {
    ++m_stats.unexpected_dma;
    throw std::runtime_error("Jak 2 Shadow2 copied DMA did not start at bucket 195");
  }
  for (u32 transfer = 0; transfer < plan.transfer_count; ++transfer) {
    if (dma.current_tag_offset() == render_state.next_bucket || dma.ended()) {
      ++m_stats.unexpected_dma;
      throw std::runtime_error("Jak 2 Shadow2 copied DMA ended before its typed plan");
    }
    dma.read_and_advance();
  }
  if (dma.current_tag_offset() != render_state.next_bucket) {
    ++m_stats.unexpected_dma;
    throw std::runtime_error("Jak 2 Shadow2 copied DMA exceeded its typed plan");
  }
  m_stats.reached_boundary = true;
}

bool MetalJak2Shadow2Renderer::build_geometry(const Jak2ShadowBucket195Plan& plan,
                                              Geometry* geometry) {
  if (!geometry) {
    ++m_stats.invalid_plan;
    return false;
  }

  std::size_t executable_records = 0;
  for (const auto& batch : plan.batches) {
    if (!batch.has_top_upload || batch.top_only || !batch.has_bottom_upload ||
        batch.top_vertices.size() != batch.bottom_vertices.size()) {
      ++m_stats.invalid_plan;
      return false;
    }
    for (const auto& command : batch.commands) {
      if (command.records.size() > kMaximumOutputVertices / 6 - executable_records) {
        ++m_stats.overflow;
        return false;
      }
      executable_records += command.records.size();
    }
  }
  if (executable_records * 6 > kMaximumOutputVertices) {
    ++m_stats.overflow;
    return false;
  }
  geometry->front.reserve(executable_records * 3);
  geometry->back.reserve(executable_records * 3);

  const auto decode = [](const Jak2ShadowBucket195Vertex& source) {
    Vertex vertex;
    vertex.x = read_unaligned<float>(source.bytes.data());
    vertex.y = read_unaligned<float>(source.bytes.data() + 4);
    vertex.z = read_unaligned<float>(source.bytes.data() + 8);
    return vertex;
  };
  const auto append_triangle = [&](Vertex a, Vertex b, Vertex c, bool swap_winding) {
    if (!finite_vertex(a) || !finite_vertex(b) || !finite_vertex(c)) {
      ++m_stats.nonfinite_projection;
      return false;
    }
    if (swap_winding) {
      std::swap(b, c);
    }
    const float facing = normal_dot_eye(a, b, c);
    if (!std::isfinite(facing)) {
      ++m_stats.nonfinite_projection;
      return false;
    }
    auto& output = facing > 0.f ? geometry->back : geometry->front;
    if (geometry->front.size() + geometry->back.size() > kMaximumOutputVertices - 3) {
      ++m_stats.overflow;
      return false;
    }
    output.push_back(a);
    output.push_back(b);
    output.push_back(c);
    return true;
  };

  for (const auto& batch : plan.batches) {
    const auto vertex = [&](bool bottom, u8 index, Vertex* out) {
      const auto& source = bottom ? batch.bottom_vertices : batch.top_vertices;
      if (!out || index >= source.size()) {
        ++m_stats.invalid_plan;
        return false;
      }
      *out = decode(source[index]);
      return true;
    };
    for (const auto& command : batch.commands) {
      for (const auto& record : command.records) {
        Vertex top0, top1, top2, bottom0, bottom1, bottom2;
        if (command.kind == Jak2ShadowBucket195CommandKind::Walls) {
          if (record.bytes[2] > 1 || !vertex(false, record.bytes[0], &top0) ||
              !vertex(false, record.bytes[1], &top1) ||
              !vertex(true, record.bytes[0], &bottom0) ||
              !vertex(true, record.bytes[1], &bottom1)) {
            ++m_stats.invalid_plan;
            return false;
          }
          std::array<Vertex, 4> quad;
          if (record.bytes[2] == 0) {
            quad = {top1, top0, bottom0, bottom1};
          } else {
            quad = {top0, top1, bottom1, bottom0};
          }
          if (!append_triangle(quad[1], quad[0], quad[2], false) ||
              !append_triangle(quad[0], quad[2], quad[3], false)) {
            return false;
          }
          continue;
        }

        if (!vertex(false, record.bytes[0], &top0) ||
            !vertex(false, record.bytes[1], &top1) ||
            !vertex(false, record.bytes[2], &top2) ||
            !vertex(true, record.bytes[0], &bottom0) ||
            !vertex(true, record.bytes[1], &bottom1) ||
            !vertex(true, record.bytes[2], &bottom2)) {
          ++m_stats.invalid_plan;
          return false;
        }
        if (command.kind == Jak2ShadowBucket195CommandKind::Caps) {
          if (record.bytes[3] != 1 || !append_triangle(top0, top1, top2, false) ||
              !append_triangle(bottom0, bottom1, bottom2, true)) {
            ++m_stats.invalid_plan;
            return false;
          }
        } else {
          if (command.kind != Jak2ShadowBucket195CommandKind::FlippableCaps ||
              record.bytes[3] > 1 ||
              !append_triangle(top0, top1, top2, record.bytes[3] == 0) ||
              !append_triangle(bottom0, bottom1, bottom2, record.bytes[3] != 0)) {
            ++m_stats.invalid_plan;
            return false;
          }
        }
      }
    }
  }

  m_stats.output_vertices = static_cast<u32>(geometry->front.size() + geometry->back.size());
  m_stats.front_triangles = static_cast<u32>(geometry->front.size() / 3);
  m_stats.back_triangles = static_cast<u32>(geometry->back.size() / 3);
  return true;
}

bool MetalJak2Shadow2Renderer::projection_is_finite(
    const Jak2ShadowBucket195Plan& plan,
    const Geometry& geometry) const {
  Shadow2VsParams params;
  std::memcpy(params.perspective, plan.perspective_matrix.data(), sizeof(params.perspective));
  std::memcpy(params.hvdf_offset, plan.constants.data() + 64, sizeof(params.hvdf_offset));
  params.fog = read_unaligned<float>(plan.constants.data() + 80);
  if (!std::all_of(std::begin(params.perspective), std::end(params.perspective),
                   [](float value) { return std::isfinite(value); }) ||
      !std::all_of(std::begin(params.hvdf_offset), std::end(params.hvdf_offset),
                   [](float value) { return std::isfinite(value); }) ||
      !std::isfinite(params.fog)) {
    return false;
  }
  const auto finite = [&](const Vertex& vertex) {
    std::array<float, 4> transformed;
    for (u32 component = 0; component < 4; ++component) {
      transformed[component] = -params.perspective[12 + component] -
                               params.perspective[component] * vertex.x -
                               params.perspective[4 + component] * vertex.y -
                               params.perspective[8 + component] * vertex.z;
    }
    if (!std::isfinite(transformed[3]) || transformed[3] == 0.f) {
      return false;
    }
    for (u32 component = 0; component < 3; ++component) {
      transformed[component] *= params.fog / transformed[3];
      transformed[component] += params.hvdf_offset[component];
    }
    transformed[0] = (transformed[0] - 2048.f) / 256.f;
    transformed[1] = (transformed[1] - 2048.f) / -128.f;
    transformed[2] /= 16777216.f;
    transformed[0] *= transformed[3];
    transformed[1] *= transformed[3] * params.scissor_adjust * params.height_scale;
    transformed[2] *= transformed[3];
    return std::all_of(transformed.begin(), transformed.end(),
                       [](float value) { return std::isfinite(value); });
  };
  return std::all_of(geometry.front.begin(), geometry.front.end(), finite) &&
         std::all_of(geometry.back.begin(), geometry.back.end(), finite);
}

void MetalJak2Shadow2Renderer::draw(const Jak2ShadowBucket195Plan& plan,
                                    const Geometry& geometry,
                                    MetalFrameContext& ctx) {
  if (geometry.front.empty() && geometry.back.empty()) {
    return;
  }
  if (!ctx.enc || !ctx.pso_cache || !ctx.stream) {
    ++m_stats.pipeline_failures;
    throw std::runtime_error("Jak 2 Shadow2 Metal frame context is incomplete");
  }

  Shadow2VsParams params;
  std::memcpy(params.perspective, plan.perspective_matrix.data(), sizeof(params.perspective));
  std::memcpy(params.hvdf_offset, plan.constants.data() + 64, sizeof(params.hvdf_offset));
  params.fog = read_unaligned<float>(plan.constants.data() + 80);

  MetalPsoKey volume_key;
  volume_key.shader = MetalShaderId::SHADOW2;
  volume_key.color_format = ctx.color_format;
  volume_key.depth_format = ctx.depth_format;
  volume_key.color_write_mask = MTLColorWriteMaskNone;
  id<MTLRenderPipelineState> volume_pso = ctx.pso_cache->get_pipeline(volume_key);
  if (!volume_pso) {
    ++m_stats.pipeline_failures;
    throw std::runtime_error("Jak 2 Shadow2 volume PSO creation failed");
  }

  struct FinalPass {
    id<MTLRenderPipelineState> pso = nil;
    std::array<float, 4> color = {};
    bool lighten = false;
  };
  std::array<FinalPass, 2> final_passes;
  std::size_t final_pass_count = 0;
  constexpr std::array<u8, 3> channel_masks = {
      MTLColorWriteMaskRed, MTLColorWriteMaskGreen, MTLColorWriteMaskBlue};
  for (const bool lighten : {false, true}) {
    MetalPsoKey final_key = volume_key;
    final_key.color_write_mask = MTLColorWriteMaskNone;
    final_key.blend_enable = true;
    final_key.blend_op_rgb =
        lighten ? MTLBlendOperationAdd : MTLBlendOperationReverseSubtract;
    final_key.blend_src_rgb = MTLBlendFactorOne;
    final_key.blend_dst_rgb = MTLBlendFactorOne;
    final_key.blend_src_alpha = MTLBlendFactorOne;
    final_key.blend_dst_alpha = MTLBlendFactorZero;
    FinalPass pass;
    pass.lighten = lighten;
    for (u32 channel = 0; channel < 3; ++channel) {
      const bool selected = lighten ? plan.color[channel] > 128 : plan.color[channel] < 128;
      if (!selected) {
        continue;
      }
      final_key.color_write_mask |= channel_masks[channel];
      const int difference = lighten ? plan.color[channel] - plan.color[3]
                                     : plan.color[3] - plan.color[channel];
      pass.color[channel] = static_cast<float>(difference) / 256.f;
    }
    if (final_key.color_write_mask == MTLColorWriteMaskNone) {
      continue;
    }
    pass.pso = ctx.pso_cache->get_pipeline(final_key);
    if (!pass.pso) {
      ++m_stats.pipeline_failures;
      throw std::runtime_error("Jak 2 Shadow2 final-color PSO creation failed");
    }
    final_passes[final_pass_count++] = pass;
  }

  MetalDepthStencilKey volume_depth;
  volume_depth.depth_test = true;
  volume_depth.compare = MTLCompareFunctionGreaterEqual;
  volume_depth.depth_write = false;
  volume_depth.stencil_test = true;
  volume_depth.stencil_compare = MTLCompareFunctionAlways;
  [ctx.enc setRenderPipelineState:volume_pso];
  [ctx.enc setStencilReferenceValue:0];
  [ctx.enc setVertexBytes:&params length:sizeof(params) atIndex:1];
  const std::array<float, 4> volume_color = {};
  [ctx.enc setFragmentBytes:volume_color.data() length:sizeof(volume_color) atIndex:0];

  const auto draw_volume = [&](const std::vector<Vertex>& vertices, MTLStencilOperation op) {
    if (vertices.empty()) {
      return;
    }
    id<MTLBuffer> buffer = nil;
    u32 offset = 0;
    void* destination =
        ctx.stream->alloc(static_cast<u32>(vertices.size() * sizeof(Vertex)), &buffer, &offset);
    std::memcpy(destination, vertices.data(), vertices.size() * sizeof(Vertex));
    volume_depth.stencil_depth_pass_op = op;
    [ctx.enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(volume_depth)];
    [ctx.enc setVertexBuffer:buffer offset:offset atIndex:0];
    [ctx.enc drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:vertices.size()];
    ++m_stats.draw_calls;
    const u32 triangles = static_cast<u32>(vertices.size() / 3);
    m_stats.triangles += triangles;
    ++ctx.draw_calls;
    ctx.triangles += static_cast<int>(triangles);
  };
  draw_volume(geometry.front, MTLStencilOperationIncrementClamp);
  draw_volume(geometry.back, MTLStencilOperationDecrementClamp);

  constexpr std::array<Vertex, 6> clear_quad = {{
      {0.3f, 0.3f, 0.f, 0},
      {0.3f, 0.7f, 0.f, 0},
      {0.7f, 0.3f, 0.f, 0},
      {0.7f, 0.3f, 0.f, 0},
      {0.3f, 0.7f, 0.f, 0},
      {0.7f, 0.7f, 0.f, 0},
  }};
  id<MTLBuffer> clear_buffer = nil;
  u32 clear_offset = 0;
  void* clear_destination =
      ctx.stream->alloc(sizeof(clear_quad), &clear_buffer, &clear_offset);
  std::memcpy(clear_destination, clear_quad.data(), sizeof(clear_quad));
  params.clear_mode = 1;
  [ctx.enc setVertexBuffer:clear_buffer offset:clear_offset atIndex:0];
  [ctx.enc setVertexBytes:&params length:sizeof(params) atIndex:1];

  MetalDepthStencilKey final_depth;
  final_depth.depth_test = true;
  final_depth.compare = MTLCompareFunctionAlways;
  final_depth.depth_write = false;
  final_depth.stencil_test = true;
  final_depth.stencil_compare = MTLCompareFunctionNotEqual;
  final_depth.stencil_depth_pass_op = MTLStencilOperationKeep;
  [ctx.enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(final_depth)];
  [ctx.enc setStencilReferenceValue:0];

  for (std::size_t pass_index = 0; pass_index < final_pass_count; ++pass_index) {
    const auto& pass = final_passes[pass_index];
    [ctx.enc setRenderPipelineState:pass.pso];
    [ctx.enc setFragmentBytes:pass.color.data() length:sizeof(pass.color) atIndex:0];
    [ctx.enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:clear_quad.size()];
    ++m_stats.draw_calls;
    m_stats.triangles += 2;
    ++ctx.draw_calls;
    ctx.triangles += 2;
    if (pass.lighten) {
      ++m_stats.lighten_draws;
    } else {
      ++m_stats.darken_draws;
    }
  }
}

void MetalJak2Shadow2Renderer::render(DmaFollower& dma,
                                      MetalSharedRenderState* render_state,
                                      MetalFrameContext& ctx) {
  m_stats = {};
  m_stats.executions = 1;
  if (!render_state || render_state->version != GameVersion::Jak2 ||
      !render_state->jak2_shadow_bucket195_plan ||
      static_cast<u32>(m_my_id) != kJak2ShadowBucket195PlanBucket) {
    ++m_stats.invalid_plan;
    throw std::runtime_error("Jak 2 Shadow2 renderer is missing its owned bucket-195 plan");
  }
  const auto& plan = *render_state->jak2_shadow_bucket195_plan;
  if (plan.bucket_id != static_cast<u32>(m_my_id)) {
    ++m_stats.invalid_plan;
    throw std::runtime_error("Jak 2 Shadow2 renderer received a plan for another bucket");
  }

  m_stats.input_batches = static_cast<u32>(plan.batches.size());
  m_stats.input_vertices = plan.vertex_count;
  m_stats.input_records = plan.record_count;
  consume_exact_plan(dma, *render_state, plan);

  switch (plan.disposition) {
    case Jak2ShadowBucket195PlanDisposition::Absent:
      ++m_stats.absent;
      return;
    case Jak2ShadowBucket195PlanDisposition::AcceptedDeferredNoDraw:
      ++m_stats.accepted_deferred_no_draw;
      return;
    case Jak2ShadowBucket195PlanDisposition::Ready:
      ++m_stats.ready;
      break;
  }

  Geometry geometry;
  if (!build_geometry(plan, &geometry)) {
    throw std::runtime_error("Jak 2 Shadow2 typed geometry plan is not executable");
  }
  if (!projection_is_finite(plan, geometry)) {
    ++m_stats.nonfinite_projection;
    throw std::runtime_error("Jak 2 Shadow2 projection produced a non-finite vertex");
  }
  draw(plan, geometry, ctx);
}

}  // namespace metal_renderer
