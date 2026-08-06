#pragma once

/*!
 * @file metal_merc.h
 * Merc2 foreground renderer for the Metal backend. Objective-C++ only.
 *
 * Port of game/graphics/opengl_renderer/foreground/Merc2.{h,cpp} and
 * Merc2BucketRenderer: the main foreground renderer, which draws characters,
 * collectables and the title-screen models. The DMA walk, the model/effect/draw
 * bookkeeping and the bone math are the GL logic unchanged; what changes is
 * where state lives:
 *
 *  - The GL bone UBO (a 512 kB GL_UNIFORM_BUFFER with a per-draw
 *    `glBindBufferRange(..., 16 * draw.first_bone, 128 * sizeof(ShaderMercMat))`)
 *    becomes one per-flush allocation out of the frame's stream buffer with a
 *    per-draw `setVertexBuffer:offset:`. The std140 padding of the bone struct
 *    is reproduced exactly, so the CPU-side layout is unchanged.
 *  - The GL per-draw blend/depth/filter state becomes PSO, depth-stencil and
 *    sampler keys (the Metal analog of setup_opengl_from_draw_mode).
 *  - Geometry comes from the merc-scoped model pool (metal_merc_model_pool.h)
 *    instead of the GL-only streaming Loader.
 *
 * Jak 1's eight Merc buckets and Jak 2's six normal opaque Merc buckets each
 * route to one shared MetalMerc2, as in the GL renderer. Jak 2's producer DMA
 * is validated transactionally before any model or GPU state is published.
 */

#include <memory>
#include <string>

#include "common/math/Vector.h"

#include "game/graphics/pipelines/metal/metal_bucket_renderer.h"
#include "game/graphics/pipelines/metal/metal_merc_model_pool.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"

#import <Metal/Metal.h>

class MetalMerc2 {
 public:
  // Counters the replay/proof reads.
  struct Stats {
    int models = 0;
    int missing_models = 0;
    int effects = 0;
    int draws = 0;
    int triangles = 0;
    int envmap_draws = 0;
    int bone_vectors = 0;
    int lights = 0;
    int mod_vtx_uploads = 0;  // effects whose blerc / mod-vertex update was uploaded
    int mod_vtx_skipped = 0;  // effects that asked for one but could not be updated (reported)
    int eye_draws = 0;        // draws whose texture the eye renderer composed
    int missing_textures = 0;
    int malformed_dma = 0;      // rejected Jak 2 source-grammar buckets
    int bad_bone_pointers = 0;  // bone address outside EE memory: identity used
    int bad_draw_ranges = 0;    // draw range outside the level's index buffer: skipped
    int missing_bone_slots = 0;  // unique weighted slots absent from model packets
    int models_with_missing_bone_slots = 0;
    int nonfinite_bone_matrices = 0;
    int degenerate_bone_matrices = 0;
    int incoherent_bone_sources = 0;
    int models_with_palette_health_issues = 0;
    int eichar_palette_health_issues = 0;
    int eichar_transform_discontinuities = 0;
    int eichar_provenance_events = 0;
    int eichar_output_composition_mismatches = 0;
    int eichar_target_control_events = 0;
    int eichar_target_control_divergences = 0;
    int eichar_target_control_attack_boundaries = 0;
    int eichar_target_control_capture_attempts = 0;
    int eichar_target_control_valid_observations = 0;
    metal_merc_skin_trace::SkinStats eichar_weighted_skin;
    metal_merc_skin_trace::DuplicationStats eichar_duplication;
    jak1_target_control_capture::Stage last_eichar_target_control_capture_stage =
        jak1_target_control_capture::Stage::NOT_ATTEMPTED;
    jak1_target_control_capture::Result last_eichar_target_control_capture_result =
        jak1_target_control_capture::Result::NOT_ATTEMPTED;
    metal_merc_transform_trace::TargetControlObservation
        last_eichar_target_control_observation;
    metal_renderer::MercPaletteHealthEvent first_palette_health_event;
    metal_renderer::MercPaletteHealthEvent last_palette_health_event;
    metal_renderer::MercPaletteHealthEvent first_eichar_palette_health_event;
    metal_renderer::MercPaletteHealthEvent last_eichar_palette_health_event;
    metal_merc_transform_trace::Event first_eichar_transform_discontinuity;
    metal_merc_transform_trace::Event last_eichar_transform_discontinuity;
    metal_merc_transform_trace::Event first_eichar_provenance_event;
    metal_merc_transform_trace::Event last_eichar_provenance_event;
    metal_merc_transform_trace::Event first_eichar_output_composition_mismatch;
    metal_merc_transform_trace::Event last_eichar_output_composition_mismatch;
    metal_merc_transform_trace::TargetControlEvent first_eichar_target_control_event;
    metal_merc_transform_trace::TargetControlEvent last_eichar_target_control_event;

    void add(const Stats& o);
  };

  MetalMerc2(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* texture_pool);

  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx,
              Stats* stats);

 private:
  enum MercDataMemory {
    LOW_MEMORY = 0,
    BUFFER_BASE = 442,
    // this negative offset is what broke jak graphics in Dobiestation for a long time.
    BUFFER_OFFSET = -442
  };

  struct LowMemory {
    u8 tri_strip_tag[16];
    u8 ad_gif_tag[16];
    math::Vector4f hvdf_offset;
    math::Vector4f perspective[4];
    math::Vector4f fog;
  } m_low_memory;
  static_assert(sizeof(LowMemory) == 0x80);

  struct VuLights {
    math::Vector3f direction0;
    u32 w0;
    math::Vector3f direction1;
    u32 w1;
    math::Vector3f direction2;
    u32 w2;
    math::Vector4f color0;
    math::Vector4f color1;
    math::Vector4f color2;
    math::Vector4f ambient;
  };
  static_assert(sizeof(VuLights) == 7 * 16);

  struct MercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
  };

  // std140 layout of the shader's MercMatrixData (mat4 + mat3 + vec4 pad).
  struct ShaderMercMat {
    math::Vector4f tmat[4];
    math::Vector4f nmat[3];
    math::Vector4f pad;
  };

  static constexpr int kMaxEffect = 64;
  static constexpr int kMaxBlerc = 40;  // blend-shape weights per model in the DMA
  static constexpr int MAX_MOD_VTX = UINT16_MAX;
  static constexpr int MAX_SKEL_BONES = 128;
  static constexpr int MAX_SHADER_BONE_VECTORS = 1024 * 32;
  static constexpr int MAX_LEVELS = 3;
  static constexpr int MAX_DRAWS_PER_LEVEL = 2048 * 2;
  static constexpr int MAX_LIGHTS = 1024;
  // The GL alignment comes from GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, in units of
  // 16-byte bone vectors. 16 vectors = 256 bytes satisfies every Metal buffer
  // offset requirement.
  static constexpr u32 kBoneVectorAlignment = 16;

  enum DrawFlags {
    IGNORE_ALPHA = 1,
    MOD_VTX = 2,
    NO_TEXTURE = 4,
  };

  // The Metal form of the GL renderer's per-effect mod-vertex GL buffer: a
  // range of the frame's stream buffer holding this frame's updated vertices.
  struct ModBuffers {
    id<MTLBuffer> buffer = nil;
    u32 offset = 0;
    u32 vertex_count = 0;
  };

  struct Draw {
    u32 first_index;
    u32 index_count;
    DrawMode mode;
    s32 texture;
    u32 num_triangles;
    u16 first_bone;
    u16 light_idx;
    u8 flags;
    u8 fade[4];
    u8 no_strip;
    ModBuffers mod_vtx;  // vertices for this draw when MOD_VTX is set
    const metal_merc_skin_trace::DrawProfile* skin_profile;
    u64 trace_source_base;
    u64 trace_packet_palette_hash;
    u32 trace_packet_sequence;
    u16 trace_bone_count;
    u16 trace_effect_index;
    bool trace_source_base_valid;
  };

  struct LevelDrawBucket {
    const MetalMercLevel* level = nullptr;
    std::vector<Draw> draws;
    std::vector<Draw> envmap_draws;
    u32 next_free_draw = 0;
    u32 next_free_envmap_draw = 0;

    void reset() {
      level = nullptr;
      next_free_draw = 0;
      next_free_envmap_draw = 0;
    }
  };

  struct DrawArgs {
    LevelDrawBucket* lev_bucket;
    const u8* fade;
    bool jak1_water_mode;
    bool ignore_alpha;
    bool disable_fog;
    u32 lights;
    u32 first_bone;
    const metal_merc_skin_trace::DrawProfile* skin_profile;
    u64 trace_source_base;
    u64 trace_packet_palette_hash;
    u32 trace_packet_sequence;
    u16 trace_bone_count;
    u16 trace_effect_index;
    bool trace_source_base_valid;
  };

  void handle_all_dma(DmaFollower& dma,
                      MetalSharedRenderState* render_state,
                      MetalFrameContext& ctx,
                      Stats* stats);
  void handle_setup_dma(DmaFollower& dma, MetalSharedRenderState* render_state);
  void handle_merc_chain(DmaFollower& dma,
                         MetalSharedRenderState* render_state,
                         MetalFrameContext& ctx,
                         Stats* stats);
  void handle_pc_model(const DmaTransfer& setup,
                       MetalSharedRenderState* render_state,
                       MetalFrameContext& ctx,
                       Stats* stats);

  // Sub-allocates this frame's vertices for one modified effect out of the
  // stream buffer, returning the CPU-writable pointer. Null (and a reported
  // skip) if the effect cannot be updated; the caller then draws the
  // unmodified vertices instead.
  void* alloc_mod_vtx_buffer(size_t vertex_count,
                             const char* model_name,
                             MetalFrameContext& ctx,
                             ModBuffers* out,
                             Stats* stats);
  void model_mod_blerc_draws(int num_effects,
                             const tfrag3::MercModel* model,
                             MetalFrameContext& ctx,
                             ModBuffers* mod_buffers,
                             const float* blerc_weights,
                             Stats* stats);
  void model_mod_draws(int num_effects,
                       const tfrag3::MercModel* model,
                       const u8* input_data,
                       const u8* ee0,
                       MetalFrameContext& ctx,
                       ModBuffers* mod_buffers,
                       Stats* stats);

  u32 alloc_lights(const VuLights& lights);
  u32 alloc_bones(int count, ShaderMercMat* data);
  Draw* alloc_normal_draw(const tfrag3::MercDraw& mdraw, const DrawArgs& args);
  Draw* try_alloc_envmap_draw(const tfrag3::MercDraw& mdraw,
                              const DrawMode& envmap_mode,
                              u32 envmap_texture,
                              const DrawArgs& args);

  void flush_draw_buckets(MetalSharedRenderState* render_state,
                          MetalFrameContext& ctx,
                          Stats* stats);
  void do_draws(const Draw* draw_array,
                u32 num_draws,
                const MetalMercLevel* lev,
                bool envmap,
                MetalSharedRenderState* render_state,
                MetalFrameContext& ctx,
                id<MTLBuffer> bone_buffer,
                u32 bone_base,
                Stats* stats);

  math::Vector4f m_shader_bone_vector_buffer[MAX_SHADER_BONE_VECTORS];
  VuLights m_lights_buffer[MAX_LIGHTS];
  std::vector<LevelDrawBucket> m_level_draw_buckets;
  // scratch for the mod-vertex unpack, mirroring the GL renderer's
  struct UnpackTempVtx {
    float pos[4];
    float nrm[4];
    float uv[2];
  };
  std::vector<UnpackTempVtx> m_mod_vtx_unpack_temp;
  u32 m_next_free_light = 0;
  u32 m_next_free_bone_vector = 0;
  u32 m_next_free_level_bucket = 0;
  bool m_warned_mod_skip = false;
  bool m_warned_eyes = false;
  bool m_warned_no_ee = false;
  bool m_warned_bad_bone = false;
  bool m_warned_malformed_dma = false;
  bool m_reported_missing_bone_slots = false;
  bool m_reported_palette_health_issue = false;
  bool m_reported_eichar_transform_discontinuity = false;
  metal_merc_transform_trace::Tracker m_eichar_transform_tracker;
  metal_merc_transform_trace::TargetControlTracker m_eichar_target_control_tracker;
  metal_merc_skin_trace::FrameTracker m_eichar_skin_tracker;
};

/*!
 * Mirror of Merc2BucketRenderer: several buckets share one MetalMerc2, each
 * reporting the stats of its own render call.
 */
class MetalMercBucketRenderer : public MetalBucketRenderer {
 public:
  MetalMercBucketRenderer(const std::string& name, int my_id, std::shared_ptr<MetalMerc2> merc)
      : MetalBucketRenderer(name, my_id), m_renderer(std::move(merc)) {}
  void render(DmaFollower& dma,
              MetalSharedRenderState* render_state,
              MetalFrameContext& ctx) override;
  const MetalMerc2::Stats& stats() const { return m_stats; }

 private:
  std::shared_ptr<MetalMerc2> m_renderer;
  MetalMerc2::Stats m_stats;
};
