#pragma once

/*!
 * @file metal_direct_renderer.h
 * Metal port of the DirectRenderer bucket renderer
 * (game/graphics/opengl_renderer/DirectRenderer.h). Objective-C++ only.
 *
 * The GS packet state machine (GIF tag walk, A+D register handling, primitive
 * assembly) is kept identical to the GL implementation. The GL-specific parts
 * are replaced at the flush boundary: instead of mutating global GL state,
 * flush_pending derives a PSO key (blend mode, color mask), a depth-stencil
 * key (ztest / depth writes) and the shader uniforms from the current GS
 * register state, and encodes the batch into the frame's game-target encoder.
 *
 * GS blend modes the GL renderer rejects are counted and logged once instead
 * of logged per draw; GS states the GL renderer asserts on remain assertions
 * here too, so divergence from the reference renderer is loud rather than
 * silent.
 */

#include <array>
#include <string>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"

class MetalDirectRenderer : public MetalBucketRenderer {
 public:
  MetalDirectRenderer(const std::string& name, int my_id, int batch_size);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  // render directly from VIF data (vif tags optional, pass 0)
  void render_vif(u32 vif0,
                  u32 vif1,
                  const u8* data,
                  u32 size,
                  MetalSharedRenderState* render_state,
                  MetalFrameContext& ctx);

  // render directly from GIF data
  void render_gif(const u8* data,
                  u32 size,
                  MetalSharedRenderState* render_state,
                  MetalFrameContext& ctx);

  void reset_state();
  void flush_pending(MetalSharedRenderState* render_state, MetalFrameContext& ctx);

  struct LastBatchStats {
    bool valid = false;
    bool textured = false;
    int vertices = 0;
    int nonzero_rgb_vertices = 0;
    u32 tex0_tbp = 0;
    bool tex0_tcc = false;
    bool tex0_decal = false;
    bool texture_lookup_hit = false;
    bool used_placeholder = false;
    bool write_rgb = false;
    bool blend_enabled = false;
    u8 blend_a = 0;
    u8 blend_b = 0;
    u8 blend_c = 0;
    u8 blend_d = 0;
    bool alpha_test_enabled = false;
    u8 alpha_test_mode = 0;
    u8 alpha_aref = 0;
    u8 alpha_afail = 0;
  };

  struct Stats {
    int triangles = 0;
    int draw_calls = 0;
    int textured_draw_calls = 0;
    int missing_texture_draw_calls = 0;
    int flush_from_tex_0 = 0;
    int flush_from_zbuf = 0;
    int flush_from_test = 0;
    int flush_from_ta0 = 0;
    int flush_from_alpha = 0;
    int flush_from_prim = 0;
    int flush_from_state_exhaust = 0;
    int unsupported_blends = 0;  // draws whose GS blend mode has no mapping yet
    LastBatchStats last_batch;
  };
  const Stats& stats() const { return m_stats; }

 private:
  void handle_prim(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_ad(const u8* data, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_st_packed(const u8* data);
  void handle_rgbaq_packed(const u8* data);
  void handle_xyzf2_packed(const u8* data,
                           MetalSharedRenderState* render_state,
                           MetalFrameContext& ctx);
  void handle_xyz2_packed(const u8* data,
                          MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx);
  void handle_prim_packed(const u8* data,
                          MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx);
  void handle_tex0_1_packed(const u8* data);
  void handle_uv_packed(const u8* data);
  void handle_rgbaq(u64 val);
  void handle_xyzf2(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_xyzf2_common(u32 x,
                           u32 y,
                           u32 z,
                           u8 f,
                           MetalSharedRenderState* render_state,
                           MetalFrameContext& ctx,
                           bool advance);
  void handle_scissor(u64 val);
  void handle_zbuf1(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_test1(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_alpha1(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_pabe(u64 val);
  void handle_clamp1(u64 val);
  void handle_tex0_1(u64 val);
  void handle_tex1_1(u64 val);
  void handle_texa(u64 val, MetalSharedRenderState* render_state, MetalFrameContext& ctx);

  int get_texture_unit_for_current_reg(MetalSharedRenderState* render_state,
                                       MetalFrameContext& ctx);

  // flush-time translation of the GS state to Metal objects
  MetalPsoKey blend_to_pso_key(MetalFrameContext& ctx);
  MetalDepthStencilKey test_to_depth_key(bool depth_write_override, bool has_override);

  // --- GS register state (identical to the GL DirectRenderer) ---------------

  struct TestState {
    void from_register(GsTest reg);
    GsTest current_register;
    bool alpha_test_enable = false;
    GsTest::AlphaTest alpha_test = GsTest::AlphaTest::NOTEQUAL;
    u8 aref = 0;
    GsTest::AlphaFail afail = GsTest::AlphaFail::KEEP;
    bool date = false;
    bool datm = false;
    bool zte = true;
    GsTest::ZTest ztst = GsTest::ZTest::GEQUAL;
    bool write_rgb = true;
    bool depth_writes = true;
  } m_test_state;

  struct BlendState {
    void from_register(GsAlpha reg);
    GsAlpha current_register;
    GsAlpha::BlendMode a = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode b = GsAlpha::BlendMode::DEST;
    GsAlpha::BlendMode c = GsAlpha::BlendMode::SOURCE;
    GsAlpha::BlendMode d = GsAlpha::BlendMode::DEST;
    bool alpha_blend_enable = false;
    u8 fix = 0;
  } m_blend_state;

  struct PrimState {
    void from_register(GsPrim reg);
    GsPrim current_register;
    bool gouraud_enable = false;
    bool texture_enable = false;
    bool fogging_enable = false;
    bool aa_enable = false;
    bool use_uv = false;
    bool ctxt = false;
    bool fix = false;
    u32 ta0 = 0;
  } m_prim_state;

  static constexpr int TEXTURE_STATE_COUNT = 1;

  struct TextureState {
    GsTex0 current_register;
    u32 texture_base_ptr = 0;
    bool using_mt4hh = false;
    bool tcc = false;
    bool decal = false;
    bool enable_tex_filt = true;

    struct ClampState {
      u64 current_register = 0b101;
      bool clamp_s = true;
      bool clamp_t = true;
    } m_clamp_state;

    bool used = false;
  };

  TextureState m_buffered_tex_state[TEXTURE_STATE_COUNT];
  int m_next_free_tex_state = 0;
  TextureState m_tex_state_from_reg;
  int m_current_tex_state_idx = -1;

  struct PrimBuildState {
    GsPrim::Kind kind = GsPrim::Kind::PRIM_7;
    math::Vector<u8, 4> rgba_reg = math::Vector<u8, 4>{0, 0, 0, 0};
    math::Vector<float, 2> st_reg;

    std::array<math::Vector<u8, 4>, 3> building_rgba;
    std::array<math::Vector<u32, 4>, 3> building_vert;
    std::array<math::Vector<float, 3>, 3> building_stq;
    int building_idx = 0;
    int tri_strip_startup = 0;

    float Q = 1.0;
  } m_prim_building;

 public:
  // same 64-byte layout as the GL DirectRenderer::Vertex; must match
  // DirectVertexIn in shaders/direct.metal
  struct Vertex {
    math::Vector<float, 4> xyzf;
    math::Vector<float, 3> stq;
    math::Vector<u8, 4> rgba;
    u8 tex_unit;
    u8 tcc;
    u8 decal;
    u8 fog_enable;
    u8 use_uv;
    u8 pad[11];
    math::Vector<float, 4> scissor;
  };
  static_assert(sizeof(Vertex) == 64);
  static_assert(offsetof(Vertex, tex_unit) == 32);
  static_assert(offsetof(Vertex, scissor) == 48);

 private:
  struct PrimitiveBuffer {
    PrimitiveBuffer(int max_triangles);
    std::vector<Vertex> vertices;
    int vert_count = 0;
    int max_verts = 0;
    float x_off = 0;
    float y_off = 0;
    // leave room to always flush one last primitive
    bool is_full() { return max_verts < (vert_count + 18); }
    void push(const math::Vector<u8, 4>& rgba,
              const math::Vector<u32, 4>& vert,
              const math::Vector<float, 3>& stq,
              const math::Vector<float, 4>& scissor,
              int unit,
              bool tcc,
              bool decal,
              bool fog_enable,
              bool use_uv);
  } m_prim_buffer;

  // shared across buckets like the GL renderer's static ScissorState
  static struct ScissorState {
    u16 scax0 = 0, scay0 = 0;
    u16 scax1 = 0, scay1 = 0;
  } m_scissor;
  bool m_scissor_enable = false;

  float m_color_mult = 1.0f;
  float m_alpha_mult = 1.0f;
  bool m_test_state_needs_double_draw = false;
  float m_double_draw_aref = 0.f;

  Stats m_stats;
  bool m_warned_unsupported_blend = false;
};

/*!
 * Jak II TextureUploadHandler counterpart for the source buckets constructed with add_direct.
 * The host callback performs the already-validated ordinary uploads at this bucket boundary;
 * exact PC_PORT descriptors are then omitted while Direct payloads retain their source order.
 */
class MetalHostTextureUploadDirectRenderer : public MetalDirectRenderer {
 public:
  MetalHostTextureUploadDirectRenderer(const std::string& name, int my_id, int batch_size)
      : MetalDirectRenderer(name, my_id, batch_size) {}

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
};
