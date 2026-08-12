#pragma once

/*!
 * @file metal_generic2.h
 * Generic2 foreground renderer for the Metal backend. Objective-C++ only.
 *
 * Port of game/graphics/opengl_renderer/foreground/Generic2*.cpp - the "generic"
 * VU1 fallback path, whose buckets sit next to merc's in every level slot. The
 * DMA walk (VIF unpack emulation), the adgif -> draw-mode / bucket bookkeeping
 * and the index-buffer build are the GL logic unchanged; what changes is the
 * drawing: the per-draw GL blend/depth/filter state becomes PSO, depth-stencil
 * and sampler keys, and the vertex/index data goes into the frame's stream
 * buffer instead of a re-uploaded GL_STREAM_DRAW buffer.
 *
 * Mode::NORMAL is ported for Jak 1 and Jak 2. Jak 2 WARP is retained as a
 * private proof path; no production bucket selects it yet. LIGHTNING / PRIM and
 * the Jak 3 DMA layout are not ported.
 */

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"

#import <Metal/Metal.h>

class MetalGeneric2 {
 public:
  struct Stats {
    int fragments = 0;
    int vertices = 0;
    int adgifs = 0;
    int draw_buckets = 0;
    int draw_calls = 0;
    int triangles = 0;
    int missing_textures = 0;
    int missing_warp_publications = 0;
    int unsupported_blends = 0;
    int unexpected_dma = 0;  // a bucket did not match: consumed and reported
    int overflow = 0;        // more data than the fixed buffers hold: reported

    void add(const Stats& o);
  };

  MetalGeneric2(u32 num_verts = 500000,
                u32 num_frags = 10000,
                u32 num_adgif = 10000,
                u32 num_buckets = 800);

  enum class Mode { NORMAL, WARP };

  // Production entry point. Both bound game paths use NORMAL.
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx,
              Stats* stats);

  // Private proof-only entry point. WARP is not wired to bucket 317.
  void render_in_mode(DmaFollower& dma,
                      MetalSharedRenderState* render_state,
                      MetalFrameContext& ctx,
                      Mode mode,
                      Stats* stats);

  // Must match GenericVertexIn in shaders/generic.metal (and Generic2::Vertex).
  struct Vertex {
    math::Vector<float, 3> xyz;
    math::Vector<u8, 4> rgba;
    math::Vector<float, 2> st;
    u8 tex_unit;
    u8 flags;
    u8 adc;
    u8 pad0;
    u32 pad1;
  };
  static_assert(sizeof(Vertex) == 32);

 private:
  struct DrawingConfig {
    bool zmsk = false;
    math::Vector4f hvdf_offset;
    float pfog0 = 0.f;
    float fog_min = 0.f, fog_max = 0.f;
    math::Vector3f proj_scale;
    float proj_mat_23 = 0.f, proj_mat_32 = 0.f;

    math::Vector3f hud_scale;
    float hud_mat_23 = 0.f, hud_mat_32 = 0.f, hud_mat_33 = 0.f;

    bool uses_hud = false;
  } m_drawing_config;

  struct GsState {
    DrawMode as_mode;
    u16 tbp = 0;
    GsTest gs_test;
    GsTex0 gs_tex0;
    GsPrim gs_prim;
    GsAlpha gs_alpha;
    u8 tex_unit = 0;

    u8 vertex_flags = 0;
    void set_tcc_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 1; }
    void set_decal_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 2; }
    void set_fog_flag(bool value) { vertex_flags ^= (-(u8)value ^ vertex_flags) & 4; }
  } m_gs;

  static constexpr u32 FRAG_HEADER_SIZE = 16 * 7;
  struct Fragment {
    u8 header[FRAG_HEADER_SIZE];
    u32 adgif_idx = 0;
    u32 adgif_count = 0;
    u32 vtx_idx = 0;
    u32 vtx_count = 0;
    u8 mscal_addr = 0;
    bool uses_hud = false;
  };

  struct Adgif {
    AdGifData data;
    DrawMode mode;
    u32 tbp = 0;
    u32 fix = 0;
    u8 vtx_flags = 0;
    u32 frag = 0;
    u32 vtx_idx = 0;
    u32 vtx_count = 0;
    bool uses_hud = false;
    u32 next = -2;

    u64 key() const {
      u64 result = mode.as_int();
      result |= (((u64)tbp) << 32);
      result |= (((u64)fix) << 48);
      result |= (((u64)uses_hud ? 1ull : 0ull) << 62);
      return result;
    }
  };

  struct Bucket {
    DrawMode mode;
    u32 tbp = 0;
    u32 start = UINT32_MAX;
    u32 last = UINT32_MAX;
    u32 idx_idx = 0;
    u32 idx_count = 0;
    u32 tri_count = 0;
  };

  // --- DMA (mirror of Generic2_DMA.cpp, NORMAL paths) ---
  bool check_for_end_of_generic_data(DmaFollower& dma, u32 next_bucket);
  bool handle_bucket_setup_dma(DmaFollower& dma, u32 next_bucket);
  void process_dma_jak1(DmaFollower& dma, u32 next_bucket);
  void process_dma_jak2(DmaFollower& dma, u32 next_bucket);
  u32 handle_fragments_after_unpack_v4_32(const u8* data,
                                          u32 off,
                                          u32 first_unpack_bytes,
                                          u32 end_of_vif,
                                          Fragment* frag,
                                          bool loop);

  // --- build (mirror of Generic2_Build.cpp) ---
  void setup_draws(bool enable_at, bool default_fog);
  void determine_draw_modes(bool enable_at, bool default_fog);
  void link_adgifs_back_to_frags();
  void draws_to_buckets();
  void process_matrices();
  void final_vertex_update();
  void build_index_buffer();

  // --- drawing ---
  void do_draws(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void draw_bucket(const Bucket& bucket,
                   const Adgif& first,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx,
                   id<MTLBuffer> index_buffer,
                   u32 index_base);

  void reset_buffers();

  // A chain that does not match is consumed and reported, never asserted on.
  bool expect(bool condition, const char* what);
  bool m_failed = false;

  u32 m_next_free_frag = 0;
  std::vector<Fragment> m_fragments;
  u32 m_next_free_vert = 0;
  std::vector<Vertex> m_verts;
  u32 m_next_free_adgif = 0;
  std::vector<Adgif> m_adgifs;
  u32 m_next_free_bucket = 0;
  std::vector<Bucket> m_buckets;
  u32 m_next_free_idx = 0;
  std::vector<u32> m_indices;

  Fragment* next_frag();
  Adgif* next_adgif();
  bool alloc_vtx(u32 count);

  Stats* m_stats = nullptr;
  Mode m_mode = Mode::NORMAL;
  std::unordered_map<std::string, bool> m_logged;
};

/*!
 * One MetalGeneric2 is shared by every generic bucket, as in the GL table.
 */
class MetalGeneric2BucketRenderer : public MetalBucketRenderer {
 public:
  MetalGeneric2BucketRenderer(const std::string& name,
                              int my_id,
                              std::shared_ptr<MetalGeneric2> generic)
      : MetalBucketRenderer(name, my_id), m_generic(std::move(generic)) {}
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const MetalGeneric2::Stats& stats() const { return m_stats; }

 private:
  std::shared_ptr<MetalGeneric2> m_generic;
  MetalGeneric2::Stats m_stats;
};
