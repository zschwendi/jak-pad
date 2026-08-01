#pragma once

/*!
 * @file metal_eye_renderer.h
 * Eye renderer for the Metal backend. Objective-C++ only.
 *
 * Port of game/graphics/opengl_renderer/EyeRenderer.{h,cpp}: the game sends a
 * bucket of GS sprite draws that compose each character's eye (background,
 * iris, pupil, eyelid) into a small texture, which merc then samples through
 * the texture pool's VRAM slots. The DMA decode and the four-draw composition
 * are the GL logic unchanged.
 *
 * Two structural differences:
 *  - GL's FramebufferTexturePair becomes an MTLTexture per eye with one render
 *    pass per eye. The pass's clear replaces GL's glClearBufferfv.
 *  - Those passes must be encoded while the frame's game-target encoder is
 *    open, so - exactly like the generated ocean texture - they run on their
 *    own command buffer, which is committed and waited on before the frame's.
 */

#include <optional>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"

#import <Metal/Metal.h>

struct GpuTexture;

constexpr int METAL_EYE_BASE_BLOCK_JAK1 = 8160;
constexpr int METAL_NUM_EYE_PAIRS = 20;
constexpr int METAL_SINGLE_EYE_SIZE = 32;
// note: eye texture increased to 128x128 (originally 32x32) here, like GL.
constexpr int METAL_EYE_TEX_SIZE = 128;

class MetalEyeRenderer : public MetalBucketRenderer {
 public:
  struct Stats {
    int eyes = 0;            // single eyes composed this frame
    int draw_calls = 0;
    int triangles = 0;
    int missing_textures = 0;  // adgif named a VRAM slot with nothing in it
    int unexpected_dma = 0;    // bucket did not match: consumed and reported
    u64 first_texture = 0;     // registry handle of the first eye composed
  };

  MetalEyeRenderer(const std::string& name,
                   int my_id,
                   id<MTLDevice> device,
                   id<MTLCommandQueue> queue);

  void init_textures(TexturePool& texture_pool, GameVersion version);
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;

  // Eye DMA that rides in a texture bucket (the GL handler forwards it to
  // EyeRenderer::handle_eye_dma2): decode and compose, dma left after the chunk.
  void render_from_texture_bucket(DmaFollower& dma,
                                  MetalSharedRenderState* render_state,
                                  MetalFrameContext& ctx);

  // Eyes are composed from the texture buckets as well as this renderer's own
  // bucket, so the per-frame stats reset happens at frame start, not per bucket.
  void start_frame() { m_stats = Stats(); }

  // Merc resolves its eye draws through these, like the GL renderer does.
  std::optional<u64> lookup_eye_texture(u8 eye_id);
  std::optional<u64> lookup_eye_texture_hash(u64 hash, bool lr);

  const Stats& stats() const { return m_stats; }

  // --- DMA decode (mirrors the GL structs) ---
  struct SpriteInfo {
    u8 a;
    u64 uv0;  // stores hashed name of merc-ctrl that reads this eye
    u32 uv1[2];
    u32 xyz0[3];
    u32 xyz1[3];
  };

  struct ScissorInfo {
    int x0, x1;
    int y0, y1;
  };

  struct EyeDraw {
    SpriteInfo sprite;
    ScissorInfo scissor;
  };

 private:
  struct GpuEyeTex {
    id<MTLTexture> texture = nil;
    u64 handle = 0;
    GpuTexture* gpu_tex = nullptr;
    u32 tbp = 0;
    u64 fnv_name_hash = 0;
    bool lr = false;
  };

  struct SingleEyeDraws {
    u64 fnv_name_hash = 0;
    int lr = 0;
    int pair = 0;
    bool using_64 = false;

    int tex_slot() const { return pair * 2 + lr; }
    EyeDraw iris{};
    bool has_iris = false;
    u64 iris_tex_handle = 0;

    EyeDraw pupil{};
    bool has_pupil = false;
    u64 pupil_tex_handle = 0;

    EyeDraw lid{};
    bool has_lid = false;
    u64 lid_tex_handle = 0;
  };

  // Throws on a chain that does not match; the caller consumes the bucket and
  // counts it rather than aborting (these renderers read user game data).
  struct EyeDmaMismatch {};

  bool handle_eye_dma2(DmaFollower& dma, MetalSharedRenderState* render_state);
  std::vector<SingleEyeDraws> get_draws(DmaFollower& dma, MetalSharedRenderState* render_state);
  void run_gpu(const std::vector<SingleEyeDraws>& draws,
               MetalSharedRenderState* render_state,
               MetalFrameContext& ctx);

  id<MTLDevice> m_device;
  id<MTLCommandQueue> m_queue;
  GpuEyeTex m_gpu_eye_textures[METAL_NUM_EYE_PAIRS * 2];

  // xyst per vertex, 4 vertices per square, 4 draws per eye, all eyes.
  static constexpr int VTX_BUFFER_FLOATS = 4 * 4 * 4 * METAL_NUM_EYE_PAIRS * 2;
  float m_cpu_vertex_buffer[VTX_BUFFER_FLOATS];
  id<MTLBuffer> m_vertex_buffer = nil;

  Stats m_stats;
  bool m_warned_dma = false;
};
