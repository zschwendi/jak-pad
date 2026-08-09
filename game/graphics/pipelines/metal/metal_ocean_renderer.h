#pragma once

/*!
 * @file metal_ocean_renderer.h
 * Metal port of the Jak 1 and Jak II ocean paths
 * (game/graphics/opengl_renderer/ocean/). Objective-C++ only.
 *
 * Two buckets carry the ocean: ocean-mid-and-far runs the ocean-texture
 * generator, the plain-GIF ocean-far, and the ocean-mid mesh; ocean-near runs
 * the generator again and the high-detail near mesh. Neither needs the level
 * loader - everything is DMA plus VU1 emulation.
 *
 * The VU1 programs are not reimplemented here: OceanTextureVu / OceanMidVu /
 * OceanNearVu (game/graphics/opengl_renderer/ocean/OceanVu.h) hold that state
 * and code, and both backends derive from them. What this file ports is the DMA
 * walks, the GIF/GS state machines and the drawing.
 *
 * The one structural difference from the GL renderer: OceanTexture needs 1 + 8
 * offscreen render passes, and MetalFrameContext hands bucket renderers an
 * already-open encoder for the game target. The generator therefore runs on its
 * own command buffer, committed and waited on while the frame's encoder is
 * still recording. The frame's command buffer is committed later, so the
 * generated texture is always complete before anything samples it - the same
 * ordering the immediate-mode GL renderer gets for free.
 */

#include <string>
#include <vector>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/ocean/OceanVu.h"
#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/texture/TextureID.h"

/*!
 * Jak II ocean-method-89 prefix renderer. A normal ocean-mid-far chain either
 * starts with this prefix when the sky is active, or starts directly with the
 * 128x128 ocean-texture setup when it is not.
 */
class MetalOceanEnvmap {
 public:
  static constexpr u32 kWidth = 64;
  static constexpr u32 kHeight = 64;
  static constexpr u32 kVramSlot = 0xf80;

  MetalOceanEnvmap(id<MTLDevice> device, id<MTLCommandQueue> queue);
  ~MetalOceanEnvmap();

  static MetalSamplerKey radial_sampler_key();
  bool init_textures(TexturePool& pool, GameVersion version);
  void detach_pool();
  void reset_stats() { m_stats = {}; }
  bool handle_ocean_envmap_jak2(DmaFollower& dma,
                                MetalSharedRenderState* render_state,
                                MetalFrameContext& ctx);

  struct Stats {
    bool prefix_present = false;
    bool found_sky_color = false;
    u8 sky_color[4] = {0, 0, 0, 255};
    int setup_64_count = 0;
    int direct_draw_calls = 0;
    int direct_unsupported_blends = 0;
    MetalDirectRenderer::LastBatchStats direct_batch;
    int haze_draw_calls = 0;
    int radial_draw_calls = 0;
    int transfers_consumed = 0;
    bool published = false;
    u32 published_vram_slot = 0;
    bool scissor_restored = false;
    bool stopped_before_ocean_texture = false;
    u32 stop_offset = 0;
  };

  const Stats& stats() const { return m_stats; }
  const MetalDirectRenderer& direct_renderer() const { return m_direct; }
  id<MTLTexture> first_pass_texture() const { return m_first_pass_texture; }
  id<MTLTexture> result_texture() const { return m_result_texture; }
  u64 result_handle() const { return m_result_handle; }

 private:
  bool render_haze(const u8* gif_data,
                   u32 size,
                   MetalFrameContext& ctx,
                   id<MTLRenderCommandEncoder> encoder);
  bool render_radial(MetalFrameContext& ctx, id<MTLCommandBuffer> commands);

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  id<MTLTexture> m_first_pass_texture = nil;
  id<MTLTexture> m_result_texture = nil;
  u64 m_result_handle = 0;
  TexturePool* m_pool = nullptr;
  GpuTexture* m_pool_texture = nullptr;
  PcTextureId m_texture_id;
  MetalDirectRenderer m_direct;
  Stats m_stats;
};

/*!
 * Generates the 128x128 ocean texture that the ocean meshes sample, and
 * publishes it to the ocean VRAM slot in the TexturePool.
 *
 * Faithfulness note kept from the GL renderer: both ocean buckets own one of
 * these and both publish to the same slot (8160 on Jak 1), so the last bucket
 * of the frame wins.
 */
class MetalOceanTexture : public OceanTextureVu {
 public:
  MetalOceanTexture(bool generate_mipmaps, id<MTLDevice> device, id<MTLCommandQueue> queue);
  ~MetalOceanTexture() override;
  void init_textures(TexturePool& pool, GameVersion version);
  void detach_pool();
  void reset_stats() { m_stats = {}; }
  void handle_ocean_texture_jak1(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx);
  bool handle_ocean_texture_jak2(DmaFollower& dma,
                                 MetalSharedRenderState* render_state,
                                 MetalFrameContext& ctx);
  void force_command_buffer_failure_for_testing(bool enabled) {
    m_force_command_buffer_failure_for_testing = enabled;
  }
  static u32 vram_slot(GameVersion version);

  struct Stats {
    int vertices = 0;  // VU-produced vertices (2112 when the generator ran)
    int draw_calls = 0;
    int triangles = 0;
    int missing_textures = 0;
    int transfers_consumed = 0;
    u32 source_tbp = 0;
    u64 source_handle = 0;
    u32 published_vram_slot = 0;
    bool vu_buffer_setup_valid = false;
    int command_buffers_committed = 0;
    int command_buffers_completed = 0;
    int command_buffer_errors = 0;
    int last_command_buffer_status = 0;
  };
  const Stats& stats() const { return m_stats; }
  u64 result_handle() const { return m_result_handle; }

 private:
  bool run_gpu_passes(MetalSharedRenderState* render_state, MetalFrameContext& ctx);

  bool m_generate_mipmaps;
  id<MTLDevice> m_device;
  id<MTLCommandQueue> m_queue;
  id<MTLTexture> m_result_texture;  // published to the pool
  id<MTLTexture> m_temp_texture;    // mip source when generating mipmaps
  id<MTLBuffer> m_position_buffer;  // static, mirrors the GL static vertex buffer
  id<MTLBuffer> m_dynamic_buffer;   // VU output, rewritten each frame
  id<MTLBuffer> m_index_buffer;     // static
  u64 m_result_handle = 0;
  TexturePool* m_pool = nullptr;
  PcTextureId m_texture_id;
  GpuTexture* m_tex0_gpu = nullptr;
  u32 m_tbp = 0;
  bool m_force_command_buffer_failure_for_testing = false;
  Stats m_stats;
};

/*!
 * Port of CommonOceanRenderer: turns the GIF packets the ocean VU programs kick
 * into triangle strips, sorted into the three GS-state buckets the GL renderer
 * uses, and draws them with the ocean_common shader.
 */
class MetalCommonOceanRenderer {
 public:
  MetalCommonOceanRenderer();

  void init_for_near();
  void kick_from_near(const u8* data);
  void flush_near(MetalSharedRenderState* render_state, MetalFrameContext& ctx);

  void init_for_mid();
  void kick_from_mid(const u8* data);
  void flush_mid(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  static float effective_scissor_adjust(GameVersion version);

  struct Stats {
    int vertices = 0;
    int draw_calls = 0;
    int triangles = 0;
    int missing_textures = 0;
    u32 ocean_texture_tbp = 0;
    float scissor_adjust = 0.f;
  };
  const Stats& stats() const { return m_stats; }

 private:
  void handle_near_vertex_gif_data_fan(const u8* data, u32 offset, u32 loop);
  void handle_near_vertex_gif_data_strip(const u8* data, u32 offset, u32 loop);
  void handle_near_adgif(const u8* data, u32 offset, u32 count);
  void handle_mid_adgif(const u8* data, u32 offset);

  // Binds the texture at `tbp` (placeholder if missing) plus a sampler, and
  // uploads the shader's per-bucket uniforms.
  void bind_bucket(MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx,
                   u32 tbp,
                   int shader_bucket,
                   bool mipmap);

  enum VertexBucket {
    RGB_TEXTURE = 0,
    ALPHA = 1,
    ENV_MAP = 2,
  };
  u32 m_current_bucket = VertexBucket::RGB_TEXTURE;

  // same 32-byte layout as the GL CommonOceanRenderer::Vertex; must match
  // OceanCommonVertexIn in shaders/ocean.metal
  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 3> stq;
    u8 fog;
    u8 pad[3];
  };
  static_assert(sizeof(Vertex) == 32);

  static constexpr int NUM_BUCKETS = 3;

  std::vector<Vertex> m_vertices;
  u32 m_next_free_vertex = 0;

  std::vector<u32> m_indices[NUM_BUCKETS];
  u32 m_next_free_index[NUM_BUCKETS] = {0};

  u32 m_envmap_tex = 0;
  Stats m_stats;
};

/*!
 * Port of OceanMid: the ocean-mid VIF/VU dispatch loop.
 */
class MetalOceanMid : public OceanMidVu {
 public:
  enum class Jak2Call : u8 { Call0, Call73, Call107, Call275, Unsupported };

  struct Jak2CallStats {
    int call0 = 0;
    int call73 = 0;
    int call107 = 0;
    int call275 = 0;
  };

  void run(DmaFollower& dma, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void run_jak2(DmaFollower& dma, MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  const MetalCommonOceanRenderer::Stats& stats() const { return m_common_ocean_renderer.stats(); }
  int jak2_calls() const { return m_jak2_calls; }
  const Jak2CallStats& jak2_call_stats() const { return m_jak2_call_stats; }
  static Jak2Call classify_jak2_call(u16 call);
  void reset_frame_stats() {
    m_common_ocean_renderer.init_for_mid();
    m_jak2_call_stats = {};
  }

 private:
  void xgkick(u16 addr) override;
  void run_jak2_selected_call(u16 call);

  MetalCommonOceanRenderer m_common_ocean_renderer;
  int m_jak2_calls = 0;
  Jak2CallStats m_jak2_call_stats;
};

/*!
 * Port of OceanMidAndFar: ocean-texture generation, then ocean-far (plain GIF
 * through the DirectRenderer port), then the ocean-mid mesh.
 */
class MetalOceanMidAndFar : public MetalBucketRenderer {
 public:
  MetalOceanMidAndFar(const std::string& name,
                      int my_id,
                      id<MTLDevice> device,
                      id<MTLCommandQueue> queue);
  ~MetalOceanMidAndFar() override;
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  void init_textures(TexturePool& pool, GameVersion version);

  const MetalOceanTexture::Stats& texture_stats() const { return m_texture_renderer.stats(); }
  const MetalCommonOceanRenderer::Stats& mid_stats() const { return m_mid_renderer.stats(); }
  u64 texture_handle() const { return m_texture_renderer.result_handle(); }
  MetalDirectRenderer::Stats direct_stats() const {
    auto stats = m_direct.stats();
    stats.unsupported_blends += m_envmap_renderer.stats().direct_unsupported_blends;
    return stats;
  }
  const MetalOceanEnvmap::Stats& envmap_stats() const { return m_envmap_renderer.stats(); }
  int mid_jak2_calls() const { return m_mid_renderer.jak2_calls(); }
  const MetalOceanMid::Jak2CallStats& mid_jak2_call_stats() const {
    return m_mid_renderer.jak2_call_stats();
  }
  int phase_order() const { return m_phase_order; }

 private:
  void render_jak1(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void render_jak2(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void handle_ocean_far(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);
  void handle_ocean_mid(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);

  MetalDirectRenderer m_direct;
  MetalOceanEnvmap m_envmap_renderer;
  MetalOceanTexture m_texture_renderer;
  MetalOceanMid m_mid_renderer;
  int m_phase_order = 0;
};

/*!
 * Port of OceanNear: ocean-texture generation again (without mipmaps), then the
 * near mesh's VIF/VU dispatch loop.
 */
class MetalOceanNear : public MetalBucketRenderer, public OceanNearVu {
 public:
  enum class Jak2Call : u8 { Call0, Call39, Unsupported };

  struct Jak2CallStats {
    int call0 = 0;
    int call39 = 0;
  };

  MetalOceanNear(const std::string& name,
                 int my_id,
                 id<MTLDevice> device,
                 id<MTLCommandQueue> queue);
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  void init_textures(TexturePool& pool, GameVersion version);

  const MetalOceanTexture::Stats& texture_stats() const { return m_texture_renderer.stats(); }
  const MetalCommonOceanRenderer::Stats& near_stats() const {
    return m_common_ocean_renderer.stats();
  }
  u64 texture_handle() const { return m_texture_renderer.result_handle(); }
  int jak2_calls() const { return m_jak2_calls; }
  const Jak2CallStats& jak2_call_stats() const { return m_jak2_call_stats; }
  static Jak2Call classify_jak2_call(u16 call);
  int phase_order() const { return m_phase_order; }

 private:
  void render_jak1(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void render_jak2(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void run_jak2_selected_call(u16 call);
  void xgkick(u16 addr) override;

  MetalOceanTexture m_texture_renderer;
  MetalCommonOceanRenderer m_common_ocean_renderer;
  int m_jak2_calls = 0;
  Jak2CallStats m_jak2_call_stats;
  int m_phase_order = 0;
};
