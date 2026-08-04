#pragma once

/*!
 * @file metal_level_data.h
 * Level-geometry (fr3) GPU stage for the Metal backend. Objective-C++ only.
 *
 * The background renderers (tfrag / tie / shrub) get almost nothing from the
 * DMA chain: the chain carries a camera, fog and visibility control block, and
 * the actual geometry lives in the extracted `.fr3` files in `tfrag3` format.
 * On the GL side, `loader/Loader` walks a level through `LoaderStages` and
 * leaves GPU buffers in a `LevelData` (game/graphics/opengl_renderer/loader/
 * common.h) that the bucket renderers reference. This file is the Metal analog
 * of that `LevelData` plus the loader stages that fill it:
 *
 *   fr3 file -> tfrag3::Level (shared code) -> MTLBuffers + pool textures
 *
 * Streaming is not ported: a level is loaded in one call rather than in
 * time-budgeted chunks across frames. The buffers, their contents and the
 * per-tree bookkeeping are the same, so the renderers are the same.
 *
 * Also here: the shared background helpers, the Metal analogs of
 * game/graphics/opengl_renderer/background/background_common.{h,cpp} - the
 * time-of-day interpolation, the BVH/occlusion culling, the visibility ->
 * draw-run conversion (GL's multidraw), the camera math and the
 * DrawMode -> pipeline-state mapping. They are kept separate from the GL file
 * because that one pulls in the whole GL renderer header chain; the
 * pure-computation ones are checked against it, byte for byte, by metal-proof.
 */

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"
#include "game/graphics/pipelines/metal/metal_camera_trace.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_pso_cache.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

#import <Metal/Metal.h>

class TexturePool;

// ---------------------------------------------------------------------------
// The control block the game sends per frame.
//
// Layout mirror of GoalBackgroundCameraData / TfragPcPortData in
// game/graphics/opengl_renderer/background/background_common.h. The GOAL code
// assumes this memory layout, so it is duplicated rather than included: the GL
// header pulls in the entire OpenGL renderer chain (glad included), which the
// Metal path deliberately does not link against. metal-proof asserts the sizes
// against the GL definitions.
// ---------------------------------------------------------------------------

struct MetalGoalBackgroundCameraData {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};

struct MetalTfragPcPortData {
  MetalGoalBackgroundCameraData camera;
  char level_name[32];
};
static_assert(sizeof(MetalTfragPcPortData) == 16 * 25, "TfragPcPortData size");

struct MetalTfragRenderSettings {
  MetalGoalBackgroundCameraData camera;
  int tree_idx = 0;
  const u8* occlusion_culling = nullptr;
};

// ---------------------------------------------------------------------------
// Per-frame state shared between the background bucket renderers: the
// occlusion-visibility strings the tfrag bucket copies out of the chain, and
// the debug toggles. Mirror of the SharedRenderState fields TFragment / Tie3 /
// Shrub read (game/graphics/opengl_renderer/BucketRenderer.h).
// ---------------------------------------------------------------------------

struct MetalLevelVis {
  bool valid = false;
  u8 data[2048];
};

struct MetalBackgroundState {
  static constexpr int kMaxLevels = 32;
  MetalLevelVis occlusion_vis[kMaxLevels];

  // GL debug toggles, same defaults as the GL renderer.
  bool use_occlusion_culling = true;
  bool debug_all_visible = false;

  // per-frame stats, read back by tests
  int tfrag_draws = 0;
  int tfrag_tris = 0;
  int tie_draws = 0;
  int tie_tris = 0;
  int tie_envmap_second_draws = 0;  // the shiny reflective pass
  int tie_envmap_second_tris = 0;
  int tie_wind_draws_skipped = 0;   // wind instancing is not ported
  int shrub_draws = 0;
  int shrub_tris = 0;
  int missing_levels = 0;   // a bucket named a level that is not loaded
  int missing_textures = 0; // a draw's texture index had no MTLTexture
  int anim_slot_draws = 0;  // draws asking for a texture-animator slot (Jak 2/3)
  int unexpected_dma = 0;   // a bucket's DMA did not match this renderer

  // One frame's camera provenance. The producer snapshot contains exactly the four camera-temp
  // qwords and one trans qword available from the live GOAL producer. The render snapshot also
  // covers hvdf-off, fog.x, camera-rot, and perspective as consumed by the Metal vertex shaders.
  metal_camera_trace::FrameTrace camera_trace;
  metal_camera_trace::RenderFrameTrace render_camera_trace;
  std::string first_camera_mismatch_bucket;

  void reset_frame();
  void observe_camera(const MetalGoalBackgroundCameraData& camera, const std::string& bucket);
};

// A structural expectation about a background bucket's DMA, or about the level
// data behind it. These renderers consume user-supplied game data, so a
// mismatch is reported (logged once per site, counted in the frame stats, and
// the bucket is then skipped whole) rather than aborting the process. Returns
// the condition.
bool metal_background_expect(bool condition,
                             const std::string& where,
                             const std::string& what,
                             MetalBackgroundState* bg);

// Consumes whatever is left of the current bucket, so the top-level dispatch
// still lands exactly on the next one.
void metal_finish_bucket(DmaFollower& dma, const MetalSharedRenderState& state);

// ---------------------------------------------------------------------------
// GPU-resident level data (the Metal LevelData).
// ---------------------------------------------------------------------------

struct MetalLevelData {
  std::unique_ptr<tfrag3::Level> level;
  // Pool handles for level->textures, parallel and same order. Mirror of
  // LevelData::textures.
  std::vector<u64> textures;
  u64 load_id = 0;

  // One tree's static GPU data. `indices` holds the full unpacked index list
  // (the GL "multidraw" index buffer); visibility selects ranges of it.
  struct TreeBuffers {
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> indices = nil;
    // 1D RGBA8 palette the vertex shaders sample, refreshed per frame from the
    // tree's packed time-of-day colors (GL: a GL_TEXTURE_1D per tree).
    id<MTLTexture> time_of_day = nil;
    u32 vertex_count = 0;
    u32 index_count = 0;
  };

  std::array<std::vector<TreeBuffers>, tfrag3::TFRAG_GEOS> tfrag;
  std::array<std::vector<TreeBuffers>, tfrag3::TIE_GEOS> tie;
  std::vector<TreeBuffers> shrub;

  u64 vertex_bytes = 0;
  u64 index_bytes = 0;
};

namespace metal_level_data {

// Loads one extracted level: textures into the pool (mirror of the GL loader's
// TextureLoaderStage) and tfrag / tie / shrub geometry into MTLBuffers (mirror
// of Tfrag/Tie/ShrubLoadStage). Replaces a level already loaded under the same
// name. Returns nullptr and fills `error` on failure.
MetalLevelData* load_fr3(id<MTLDevice> device,
                         id<MTLCommandQueue> queue,
                         TexturePool& pool,
                         const std::string& path,
                         bool is_common,
                         std::string* error);

// The Metal analog of Loader::get_tfrag3_level: nullptr when the level the
// chain named is not loaded.
MetalLevelData* get(const std::string& name);

// Number of levels currently loaded.
size_t level_count();

// Releases one level: its pool textures are unloaded (the pool repoints any
// VRAM slot still naming them at the placeholder, exactly what the GL Loader's
// unload path does), its registry textures are released, and its buffers go
// with the MetalLevelData. Must be called on the thread that loads and draws
// levels, between frames. Returns false when no such level is loaded.
bool unload(TexturePool& pool, const std::string& name);

// Releases every loaded level (buffers, textures stay in the pool's registry
// exactly like the GL loader's unload path leaves them to the pool).
void clear();

}  // namespace metal_level_data

// ---------------------------------------------------------------------------
// Shader constant blocks. These must match the structs in
// shaders/background.metal.
// ---------------------------------------------------------------------------

// tfrag3.vert / shrub.vert constants (per tree).
struct MetalBackgroundVsParams {
  float pc_camera[16];
  float hvdf_offset[4];
  float cam_trans[4];
  float fog_min = 0.f;
  float fog_max = 0.f;
  float height_scale = 1.f;
  float scissor_adjust = 1.f;
};
static_assert(sizeof(MetalBackgroundVsParams) == 112, "MetalBackgroundVsParams size");

// etie_base.vert constants (per tree).
struct MetalEtieVsParams {
  float cam_no_persp[16];
  float camera[16];
  float persp0[4];
  float persp1[4];
  float hvdf_offset[4];
  float fog_min = 0.f;
  float fog_max = 0.f;
  float height_scale = 1.f;
  float scissor_adjust = 1.f;
  // only the envmap second draw uses this (Tie3's envmap_tod_tint)
  float envmap_tod_tint[4] = {1.f, 1.f, 1.f, 1.f};
};
static_assert(sizeof(MetalEtieVsParams) == 208, "MetalEtieVsParams size");

// The GL `decal` uniform plus the Metal ETIE pass selector, set per draw.
struct MetalBackgroundDrawParams {
  int decal = 0;
  int etie_shine = 0;
  int pad[2] = {0, 0};
};
static_assert(sizeof(MetalBackgroundDrawParams) == 16, "MetalBackgroundDrawParams size");

// tfrag3.frag / shrub.frag constants.
struct MetalBackgroundFsParams {
  float fog_color[4];
  float alpha_min = 0.f;
  float alpha_max = 10.f;
  int gfx_hack_no_tex = 0;
  int pad = 0;
};
static_assert(sizeof(MetalBackgroundFsParams) == 32, "MetalBackgroundFsParams size");

// ---------------------------------------------------------------------------
// background_common analogs.
// ---------------------------------------------------------------------------

// Mirror of DoubleDraw + the OpenGL state setup_opengl_from_draw_mode performs,
// expressed as the state keys a Metal encoder needs.
struct MetalBackgroundDrawSettings {
  MetalPsoKey pso;
  MetalDepthStencilKey depth;
  MetalSamplerKey sampler;
  bool needs_blend_color = false;  // SRC_DST_FIX_DST uses a constant of 0.5
  bool afail_double_draw = false;
  float aref_first = 0.f;
  float aref_second = 0.f;
  float color_mult = 1.f;
};

MetalBackgroundDrawSettings metal_background_settings_from_draw_mode(DrawMode mode,
                                                                    MetalShaderId shader,
                                                                    const MetalFrameContext& ctx,
                                                                    MetalBackgroundState* bg);

// Mirror of first_tfrag_draw_setup: builds the per-tree vertex constants,
// including make_new_cam_mat's PC perspective matrix.
void metal_fill_background_vs_params(const MetalGoalBackgroundCameraData& camera,
                                     GameVersion version,
                                     MetalBackgroundVsParams* out);

// Same, for the etie_base shader (init_etie_cam_uniforms in Tie3.cpp).
void metal_fill_etie_vs_params(const MetalGoalBackgroundCameraData& camera,
                               GameVersion version,
                               MetalEtieVsParams* out);

// Mirror of the fog uniforms first_tfrag_draw_setup fills from render state.
void metal_fill_background_fs_params(const MetalSharedRenderState& state,
                                     MetalBackgroundFsParams* out);

// Portable port of interp_time_of_day. The GL version is hand-written SSE
// (background_common.cpp); this reproduces its arithmetic exactly, including
// the saturating 16-bit accumulation tree and the alpha-saturates-at-128 rule.
// metal-proof compares the two byte for byte on real level data.
void metal_interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                              const tfrag3::PackedTimeOfDay& packed_colors,
                              math::Vector<u8, 4>* out);

// Portable ports of sphere_in_view_ref / cull_check_all_slow.
bool metal_sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes);
void metal_cull_check_all_slow(const math::Vector4f* planes,
                               const std::vector<tfrag3::VisNode>& nodes,
                               const u8* level_occlusion_string,
                               u8* out);

// One range of the tree's index buffer to draw. GL packs these into a single
// glMultiDrawElements per StripDraw; Metal issues one drawIndexedPrimitives per
// run (Metal has no multidraw; indirect command buffers are a later,
// measured change).
struct MetalDrawRun {
  u32 first_index = 0;
  u32 index_count = 0;
};

// Mirror of make_multidraws_from_vis_string. `draw_runs_out[i]` is
// (first run, run count) for draws[i]; runs land in `runs_out`. Returns the
// number of visible triangles; `tris_per_draw_out` (optional) breaks that down
// per draw, which the category-based TIE renderer reports.
u32 metal_make_draw_runs_from_vis_string(std::pair<u32, u32>* draw_runs_out,
                                         MetalDrawRun* runs_out,
                                         const std::vector<tfrag3::StripDraw>& draws,
                                         const std::vector<u8>& vis_data,
                                         u32* tris_per_draw_out = nullptr);

// Mirror of make_all_visible_multidraws (StripDraw): one run per draw.
u32 metal_make_all_visible_draw_runs(std::pair<u32, u32>* draw_runs_out,
                                     MetalDrawRun* runs_out,
                                     const std::vector<tfrag3::StripDraw>& draws,
                                     u32* tris_per_draw_out = nullptr);

// Mirror of make_all_visible_multidraws (ShrubDraw): shrub has no visibility
// data, so every draw is one run.
void metal_make_all_visible_draw_runs(std::pair<u32, u32>* draw_runs_out,
                                      MetalDrawRun* runs_out,
                                      const std::vector<tfrag3::ShrubDraw>& draws);

// Uploads `count` interpolated colors into a tree's 1D palette texture (the
// analog of glTexSubImage1D on the GL_TEXTURE_1D).
void metal_update_time_of_day_texture(id<MTLTexture> tex,
                                      const math::Vector<u8, 4>* colors,
                                      u32 count);

// Resolves a draw's texture index to an MTLTexture, following the GL
// convention that a negative index selects a texture-animator slot (Jak 2/3
// only - counted and drawn with the placeholder here). Never returns nil.
id<MTLTexture> metal_background_texture(const MetalLevelData& level,
                                        s32 tree_tex_id,
                                        MetalSharedRenderState* render_state,
                                        MetalBackgroundState* bg);

// The number of time-of-day colors the shaders' palette texture holds. Same
// fixed maximum the GL renderers use so color indices line up.
constexpr int kMetalTimeOfDayColorCount = 8192;

// The GL shaders get HEIGHT_SCALE / SCISSOR_ADJUST as compile-time
// substitutions (game/graphics/opengl_renderer/Shader.cpp); the Metal shaders
// take them as uniforms, from the same per-version constants.
float metal_height_scale(GameVersion version);
float metal_scissor_adjust(GameVersion version);
