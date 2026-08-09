#pragma once

/*!
 * @file metal_sprite_renderer.h
 * Metal port of the Sprite3 bucket renderer
 * (game/graphics/opengl_renderer/sprite/Sprite3.cpp). Objective-C++ only.
 *
 * Sprites are the game's 2D/3D quads: HUD and menu art, the title screen's
 * "Press Start" text, and every particle. All of their data comes from the DMA
 * chain - per-frame VU constants, then chunks of up to 48 sprites each carrying
 * vector data and an adgif shader - so this renderer needs nothing from the
 * level loader beyond the textures the adgifs point at.
 *
 * The DMA walk, the GS state machine (adgif -> DrawMode), the per-(texture,
 * mode) bucketing and the four-vertex-per-sprite expansion are the GL logic
 * unchanged. The GL-specific parts are replaced at the flush boundary: the
 * DrawMode becomes a PSO key, a depth-stencil key and a sampler key
 * (the Metal analog of setup_opengl_from_draw_mode), and the strips are drawn
 * with an indexed draw whose 0xFFFFFFFF restart index Metal honours natively.
 *
 * The distorter (heat shimmer) is ported in its non-instanced GL form: the
 * same DMA walk and sine-table vertex build, drawn against a snapshot of the
 * frame so far (the game pass is split where GL calls glBlitFramebuffer).
 *
 * The normal Jak 2 Sprite3 path is also supported. Its constants-led glow DMA
 * is verified and transformed into backend-neutral records, then submitted to
 * a diagnostic final-flare pass that deliberately treats every valid flare as
 * visible. The source-faithful probe and downsample passes remain pending.
 * Control-led tails without constants remain explicitly unsupported. Jak 3
 * remains unsupported.
 */

#include <map>
#include <string>
#include <vector>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/sprite/sprite_common.h"
#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_glow_renderer.h"
#include "game/graphics/sprite_glow_math.h"

class MetalSpriteRenderer : public MetalBucketRenderer {
 public:
  MetalSpriteRenderer(const std::string& name, int my_id);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  static constexpr int SPRITES_PER_CHUNK = 48;

  struct Stats {
    int blocks_2d_grp0 = 0;
    int count_2d_grp0 = 0;
    int blocks_2d_grp1 = 0;
    int count_2d_grp1 = 0;
    int sprites_3d = 0;
    int normal_sprites_submitted = 0;
    int glow_marked_sprites = 0;
    int glow_sprites_skipped = 0;
    int glow_sprites_parsed = 0;
    int glow_sprites_accepted = 0;
    int glow_sprites_rejected = 0;
    int glow_invalid_records = 0;
    // Legacy telemetry names retained for the host ABI. These counters now
    // describe the visibility-tested final flare pass, not force-visible output.
    int glow_force_visible_submitted = 0;
    int glow_force_visible_drawn = 0;
    int glow_force_visible_draw_calls = 0;
    int glow_force_visible_triangles = 0;
    int glow_force_visible_missing_textures = 0;
    int draw_calls = 0;
    int triangles = 0;
    int distort_sprites = 0;
    int missing_textures = 0;
    int glow_transfers_skipped = 0;
    u64 glow_bytes_skipped = 0;
    int post_glow_residual_transfers = 0;
    u64 post_glow_residual_bytes = 0;
    u64 unsupported_bytes = 0;
  };
  const Stats& stats() const { return m_stats; }
  const std::vector<SpriteGlowOutput>& pending_glow_outputs() const {
    return m_pending_glow_outputs;
  }
  u64 unsupported_bytes_total() const { return m_unsupported_bytes_total; }

  // Vertex handed to the sprite shader: one per corner, four per sprite. Same
  // 64-byte layout as the GL Sprite3::SpriteVertex3D and shaders/sprite.metal.
  struct SpriteVertex3D {
    math::Vector4f xyz_sx;
    math::Vector4f quat_sy;
    math::Vector4f rgba;
    math::Vector<u16, 2> flags_matrix;
    math::Vector<u16, 4> info;
    math::Vector<u8, 4> pad;
  };
  static_assert(sizeof(SpriteVertex3D) == 64);

  // --- distorter data (mirrors the Sprite3 structs of the same names) ---
  struct SpriteDistorterSineTables {
    math::Vector4f entry[128];
    math::Vector<u32, 4> ientry[9];
    GifTag gs_gif_tag;
    math::Vector<u32, 4> color;
  };
  static_assert(sizeof(SpriteDistorterSineTables) == 0x8b * 16);

  struct SpriteDistortFrameData {
    math::Vector3f xyz;  // position
    float num_255;       // always 255.0
    math::Vector2f st;   // texture coords
    float num_1;         // always 1.0
    u32 flag;            // 'resolution' of the sprite
    math::Vector4f rgba; // scales, not a color (see the GL setup)
  };
  static_assert(sizeof(SpriteDistortFrameData) == 16 * 3);

  // Must match SpriteDistortVertexIn in shaders/sprite.metal.
  struct SpriteDistortVertex {
    math::Vector3f xyz;
    math::Vector2f st;
  };
  static_assert(sizeof(SpriteDistortVertex) == 20);

 private:
  // DMA walk (mirrors Sprite3's methods of the same names)
  void render_jak1(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  void render_jak2(DmaFollower& dma,
                   MetalSharedRenderState* render_state,
                   MetalFrameContext& ctx);
  bool render_normal_path(DmaFollower& dma,
                          MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx);
  void parse_jak2_glow_and_residual(DmaFollower& dma, MetalSharedRenderState* render_state);
  bool render_direct(DmaFollower& dma,
                     MetalSharedRenderState* render_state,
                     MetalFrameContext& ctx);
  void distort_dma(GameVersion version, DmaFollower& dma);
  void distort_setup();
  void distort_draw(MetalSharedRenderState* render_state, MetalFrameContext& ctx);
  void handle_sprite_frame_setup(GameVersion version, DmaFollower& dma);
  void render_3d(DmaFollower& dma);
  void render_2d_group0(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);
  void render_fake_shadow(DmaFollower& dma);
  void render_2d_group1(DmaFollower& dma,
                        MetalSharedRenderState* render_state,
                        MetalFrameContext& ctx);

  enum SpriteMode { Mode2D = 1, ModeHUD = 2, Mode3D = 3 };
  void do_block_common(SpriteMode mode,
                       u32 count,
                       MetalSharedRenderState* render_state,
                       MetalFrameContext& ctx);

  // GS register handling (identical to Sprite3)
  void handle_tex0(u64 val);
  void handle_tex1(u64 val);
  void handle_zbuf(GameVersion version, u64 val);
  void handle_clamp(u64 val);
  void handle_alpha(u64 val);

  void flush_sprites(MetalSharedRenderState* render_state,
                     MetalFrameContext& ctx,
                     bool double_draw);

  MetalDirectRenderer m_direct;
  MetalGlowRenderer m_glow_renderer;

  u64 m_sprite_direct_setup[3 * 16 / 8];
  SpriteFrameData m_frame_data;
  Sprite3DMatrixData m_3d_matrix_data;
  SpriteHudMatrixData m_hud_matrix_data;

  SpriteVecData2d m_vec_data_2d[SPRITES_PER_CHUNK];
  AdGifData m_adgif[SPRITES_PER_CHUNK];

  std::vector<SpriteVertex3D> m_vertices_3d;
  std::vector<u32> m_index_buffer_data;
  std::vector<SpriteGlowOutput> m_pending_glow_outputs;

  DrawMode m_current_mode, m_default_mode;
  u32 m_current_tbp = 0;

  struct Bucket {
    std::vector<u32> ids;
    u32 offset_in_idx_buffer = 0;
    u64 key = -1;
  };
  std::map<u64, Bucket> m_sprite_buckets;
  std::vector<Bucket*> m_bucket_list;
  u64 m_last_bucket_key = UINT64_MAX;
  Bucket* m_last_bucket = nullptr;
  u64 m_sprite_idx = 0;

  // distorter state for the current frame
  SpriteDistorterSineTables m_distort_sine_tables;
  std::vector<SpriteDistortFrameData> m_distort_frame_data;
  std::vector<SpriteDistortVertex> m_distort_vertices;
  std::vector<u32> m_distort_indices;
  int m_distort_sprite_count = 0;
  int m_distort_tri_count = 0;
  id<MTLTexture> m_distort_snapshot = nil;

  Stats m_stats;
  u64 m_unsupported_bytes_total = 0;
  bool m_warned_distort_overflow = false;
  bool m_warned_unsupported_glow = false;
  bool m_warned_rejected_glow_math = false;
};
