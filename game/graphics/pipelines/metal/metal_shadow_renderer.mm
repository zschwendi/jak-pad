#include "metal_shadow_renderer.h"

#include <atomic>

#include "common/log/log.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"

namespace {

constexpr float kGameHeightJak1 = 448.f;

std::atomic<bool> g_jak1_shadow_output_enabled{true};

// Must match ShadowVsParams in shaders/shadow.metal.
struct ShadowVsParams {
  float scissor_adjust;
  float pad[3];
};
static_assert(sizeof(ShadowVsParams) == 16);

}  // namespace

namespace metal_renderer {

void set_jak1_shadow_output_enabled(bool enabled) {
  g_jak1_shadow_output_enabled.store(enabled, std::memory_order_relaxed);
}

bool jak1_shadow_output_enabled() {
  return g_jak1_shadow_output_enabled.load(std::memory_order_relaxed);
}

}  // namespace metal_renderer

bool MetalShadowRenderer::expect(bool condition, const char* what) {
  if (condition) {
    return true;
  }
  m_failed = true;
  m_stats.unexpected_dma++;
  if (!m_warned) {
    m_warned = true;
    lg::warn("Metal shadow: expected {}; the bucket is skipped (logged once)", what);
  }
  return false;
}

/*!
 * Mirror of ShadowRenderer::render. The GL asserts become log-and-count
 * reports, since this reads the player's own game data.
 */
void MetalShadowRenderer::render(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx) {
  m_stats = Stats();
  m_failed = false;
  m_next_vertex = 0;
  m_next_back_index = 0;
  m_next_front_index = 0;

  auto data0 = dma.read_and_advance();
  if (!expect(data0.vif1() == 0 && data0.vif0() == 0 && data0.size_bytes == 0,
              "the empty bucket-entry transfer")) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // an empty bucket: the renderer did not run this frame
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  // three VU data uploads, then the init program
  struct {
    u16 addr;
    u16 num;
    const char* what;
  } uploads[3] = {{Vu1Data::CONSTANTS, 13, "the 13-quadword constants upload"},
                  {Vu1Data::GIF_CONSTANTS, 4, "the 4-quadword GIF constants upload"},
                  {Vu1Data::MATRIX, 4, "the 4-quadword matrix upload"}};
  for (auto& up : uploads) {
    auto constants = dma.read_and_advance();
    auto v0 = constants.vifcode0();
    auto v1 = constants.vifcode1();
    if (!expect(v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
                    v1.kind == VifCode::Kind::UNPACK_V4_32 && v1.immediate == up.addr &&
                    v1.num == up.num,
                up.what)) {
      while (dma.current_tag_offset() != render_state->next_bucket) {
        dma.read_and_advance();
      }
      return;
    }
    memcpy(m_vu_data + v1.immediate, constants.data, v1.num * 16);
  }

  {
    auto mscal = dma.read_and_advance();
    if (!expect(mscal.vifcode1().kind == VifCode::Kind::FLUSHE &&
                    mscal.vifcode0().kind == VifCode::Kind::MSCALF &&
                    mscal.vifcode0().immediate == Vu1Code::INIT,
                "the MSCALF that runs the init program")) {
      while (dma.current_tag_offset() != render_state->next_bucket) {
        dma.read_and_advance();
      }
      return;
    }
    run_mscal10_vu2c();
  }

  dma.read_and_advance();  // init gs direct

  while (dma.current_tag().kind != DmaTag::Kind::CALL) {
    if (dma.current_tag_offset() == render_state->next_bucket) {
      expect(false, "the closing CALL before the end of the bucket");
      return;
    }
    auto next = dma.read_and_advance();
    auto v1 = next.vifcode1();
    const auto v0k = next.vifcode0().kind;

    if ((v0k == VifCode::Kind::FLUSHA || v0k == VifCode::Kind::NOP) &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      VifCodeUnpack unpack(v1);
      if (!expect(!unpack.use_tops_flag && (u32)unpack.addr_qw + v1.num < 1024 &&
                      v1.num * 16 == next.size_bytes,
                  "an in-range V4_32 VU upload")) {
        break;
      }
      memcpy(m_vu_data + unpack.addr_qw, next.data, v1.num * 16);
    } else if (v0k == VifCode::Kind::NOP && v1.kind == VifCode::Kind::UNPACK_V4_8) {
      VifCodeUnpack up(v1);
      if (!expect(!up.use_tops_flag && up.is_unsigned && up.addr_qw + v1.num <= 1024 &&
                      4u * v1.num + 16 == next.size_bytes,
                  "an in-range unsigned V4_8 VU upload with a trailing MSCALF")) {
        break;
      }
      u16 addr = up.addr_qw;
      u32 temp[4];
      for (u32 i = 0; i < v1.num; i++) {
        for (u32 j = 0; j < 4; j++) {
          temp[j] = next.data[4 * i + j];
        }
        memcpy(m_vu_data + addr + i, temp, 16);
      }

      u32 offset = 4 * v1.num;
      u32 after[4];
      memcpy(&after, next.data + offset, 16);
      VifCode mscal(after[3]);
      if (!expect(after[0] == 0 && after[1] == 0 && after[2] == 0 &&
                      mscal.kind == VifCode::Kind::MSCALF,
                  "the MSCALF that runs a shadow volume")) {
        break;
      }
      const u32 before = m_next_vertex;
      run_mscal_vu2c(mscal.immediate);
      if (m_next_vertex > before) {
        m_stats.volumes++;
      }
    } else if (v0k == VifCode::Kind::FLUSHA && v1.kind == VifCode::Kind::DIRECT) {
      // four direct transfers set up various registers; only the one with the
      // color value matters
      auto xfer1 = dma.read_and_advance();
      dma.read_and_advance();
      dma.read_and_advance();
      if (!expect(xfer1.size_bytes >= 28, "a color-carrying DIRECT setup transfer")) {
        break;
      }
      m_color.x() = *(xfer1.data + 24) / 255.0f;
      m_color.y() = *(xfer1.data + 25) / 255.0f;
      m_color.z() = *(xfer1.data + 26) / 255.0f;
      m_color.w() = *(xfer1.data + 27) / 128.0f;
    } else {
      expect(false, "a VU upload, an MSCALF or a DIRECT register setup");
      break;
    }
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }

  m_stats.vertices = (int)m_next_vertex;
  m_stats.front_indices = (int)m_next_front_index;
  m_stats.back_indices = (int)m_next_back_index;

  if (!m_failed && metal_renderer::jak1_shadow_output_enabled()) {
    draw(render_state, ctx);
  }
}

/*!
 * Mirror of ShadowRenderer::draw. The GL renderer's global stencil/blend/mask
 * changes become PSO and depth-stencil keys; the three draws and their order
 * are unchanged.
 */
void MetalShadowRenderer::draw(MetalSharedRenderState* render_state, MetalFrameContext& ctx) {
  if (m_next_front_index == 0 && m_next_back_index == 0) {
    return;
  }
  if (m_next_vertex + 4 >= (u32)MAX_VERTICES || m_next_front_index + 6 >= (u32)MAX_INDICES) {
    expect(false, "the shadow volume to fit the fixed buffers");
    return;
  }

  // the full-screen quad the final pass darkens, appended after the volume
  const u32 clear_vertices = m_next_vertex;
  m_vertices[m_next_vertex++] = Vertex{math::Vector3f(0.3, 0.3, 0), 0};
  m_vertices[m_next_vertex++] = Vertex{math::Vector3f(0.3, 0.7, 0), 0};
  m_vertices[m_next_vertex++] = Vertex{math::Vector3f(0.7, 0.3, 0), 0};
  m_vertices[m_next_vertex++] = Vertex{math::Vector3f(0.7, 0.7, 0), 0};
  m_front_indices[m_next_front_index++] = clear_vertices;
  m_front_indices[m_next_front_index++] = clear_vertices + 1;
  m_front_indices[m_next_front_index++] = clear_vertices + 2;
  m_front_indices[m_next_front_index++] = clear_vertices + 3;
  m_front_indices[m_next_front_index++] = clear_vertices + 2;
  m_front_indices[m_next_front_index++] = clear_vertices + 1;

  id<MTLBuffer> vertex_buffer = nil;
  u32 vertex_offset = 0;
  void* vtx_dst =
      ctx.stream->alloc(m_next_vertex * sizeof(Vertex), &vertex_buffer, &vertex_offset);
  memcpy(vtx_dst, m_vertices, m_next_vertex * sizeof(Vertex));

  id<MTLBuffer> front_buffer = nil;
  u32 front_offset = 0;
  void* front_dst =
      ctx.stream->alloc(m_next_front_index * sizeof(u32), &front_buffer, &front_offset);
  memcpy(front_dst, m_front_indices, m_next_front_index * sizeof(u32));

  id<MTLBuffer> back_buffer = nil;
  u32 back_offset = 0;
  if (m_next_back_index) {
    void* back_dst =
        ctx.stream->alloc(m_next_back_index * sizeof(u32), &back_buffer, &back_offset);
    memcpy(back_dst, m_back_indices, m_next_back_index * sizeof(u32));
  }

  id<MTLRenderCommandEncoder> enc = ctx.enc;
  [enc setVertexBuffer:vertex_buffer offset:vertex_offset atIndex:0];
  ShadowVsParams vs = {};
  vs.scissor_adjust = 512.f / kGameHeightJak1;
  [enc setVertexBytes:&vs length:sizeof(vs) atIndex:1];

  // shared state for the two stencil passes: depth GEQUAL, no depth writes, no
  // blending, and no color writes at all
  MetalPsoKey volume_key;
  volume_key.shader = MetalShaderId::SHADOW;
  volume_key.color_format = ctx.color_format;
  volume_key.depth_format = ctx.depth_format;
  volume_key.color_write_mask = MTLColorWriteMaskNone;
  id<MTLRenderPipelineState> volume_pso = ctx.pso_cache->get_pipeline(volume_key);

  MetalDepthStencilKey volume_depth;
  volume_depth.depth_test = true;
  volume_depth.compare = MTLCompareFunctionGreaterEqual;
  volume_depth.depth_write = false;
  volume_depth.stencil_test = true;
  volume_depth.stencil_compare = MTLCompareFunctionAlways;

  if (!volume_pso) {
    return;
  }
  [enc setRenderPipelineState:volume_pso];

  // First pass: increment the stencil where front faces pass depth. The GL
  // renderer draws every front index except the six it just appended for the
  // final quad.
  {
    volume_depth.stencil_depth_pass_op = MTLStencilOperationIncrementClamp;
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(volume_depth)];
    const float color[4] = {0.0f, 128.0f / 256, 0.0f, 127.0f / 256};
    [enc setFragmentBytes:color length:sizeof(color) atIndex:0];
    const u32 count = m_next_front_index - 6;
    if (count) {
      [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:count
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:front_buffer
               indexBufferOffset:front_offset];
      ctx.draw_calls++;
      ctx.triangles += (int)(m_next_back_index / 3);
      m_stats.draw_calls++;
      m_stats.triangles += (int)(m_next_back_index / 3);
    }
  }

  // Second pass: same, decrementing where back faces pass depth.
  if (m_next_back_index) {
    volume_depth.stencil_depth_pass_op = MTLStencilOperationDecrementClamp;
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(volume_depth)];
    const float color[4] = {128.0f / 256, 0.0f, 0.0f, 130.0f / 256};
    [enc setFragmentBytes:color length:sizeof(color) atIndex:0];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:m_next_back_index
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:back_buffer
             indexBufferOffset:back_offset];
    ctx.draw_calls++;
    ctx.triangles += (int)(m_next_front_index / 3);
    m_stats.draw_calls++;
    m_stats.triangles += (int)(m_next_front_index / 3);
  }

  // Finally, draw the shadow: the appended quad, multiplied into the frame
  // wherever the stencil count is non-zero. Depth always passes, RGB is
  // written but alpha is not.
  {
    MetalPsoKey key = volume_key;
    key.color_write_mask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    key.blend_enable = true;
    key.blend_op_rgb = MTLBlendOperationAdd;
    key.blend_op_alpha = MTLBlendOperationAdd;
    key.blend_src_rgb = MTLBlendFactorDestinationColor;
    key.blend_dst_rgb = MTLBlendFactorZero;
    key.blend_src_alpha = MTLBlendFactorOne;
    key.blend_dst_alpha = MTLBlendFactorZero;
    id<MTLRenderPipelineState> pso = ctx.pso_cache->get_pipeline(key);
    if (!pso) {
      return;
    }
    MetalDepthStencilKey depth;
    depth.depth_test = true;
    depth.compare = MTLCompareFunctionAlways;
    depth.depth_write = false;
    depth.stencil_test = true;
    depth.stencil_compare = MTLCompareFunctionNotEqual;
    depth.stencil_depth_pass_op = MTLStencilOperationKeep;

    [enc setRenderPipelineState:pso];
    [enc setDepthStencilState:ctx.pso_cache->get_depth_stencil(depth)];
    [enc setStencilReferenceValue:0];
    const float color[4] = {m_color.x(), m_color.y(), m_color.z(), m_color.w()};
    [enc setFragmentBytes:color length:sizeof(color) atIndex:0];
    [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                    indexCount:6
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:front_buffer
             indexBufferOffset:front_offset + (m_next_front_index - 6) * sizeof(u32)];
    ctx.draw_calls++;
    ctx.triangles += 2;
    m_stats.draw_calls++;
    m_stats.triangles += 2;
  }
  (void)render_state;
}
