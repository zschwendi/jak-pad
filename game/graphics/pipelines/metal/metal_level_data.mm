/*!
 * @file metal_level_data.mm
 * See metal_level_data.h.
 */

#include "game/graphics/pipelines/metal/metal_level_data.h"

#include <algorithm>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/texture/TexturePool.h"

// ---------------------------------------------------------------------------
// Level registry
// ---------------------------------------------------------------------------

namespace {

std::unordered_map<std::string, std::unique_ptr<MetalLevelData>>& level_map() {
  static std::unordered_map<std::string, std::unique_ptr<MetalLevelData>> map;
  return map;
}

u64 next_load_id() {
  static u64 id = 1;
  return id++;
}

id<MTLBuffer> make_static_buffer(id<MTLDevice> device, const void* data, size_t bytes) {
  if (bytes == 0) {
    return nil;
  }
  // Shared storage: one copy in unified memory on Apple GPUs, which is what
  // both macOS on Apple silicon and iPadOS have. The GL loader's equivalent is
  // glBufferData(..., GL_STATIC_DRAW).
  return [device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
}

id<MTLTexture> make_time_of_day_texture(id<MTLDevice> device) {
  auto* desc = [[MTLTextureDescriptor alloc] init];
  desc.textureType = MTLTextureType1D;
  desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
  desc.width = kMetalTimeOfDayColorCount;
  desc.height = 1;
  desc.mipmapLevelCount = 1;
  desc.usage = MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  return [device newTextureWithDescriptor:desc];
}

// Uploads one tree's static geometry. Both buffers are immutable after this.
bool upload_tree(id<MTLDevice> device,
                 const void* verts,
                 size_t vert_bytes,
                 u32 vert_count,
                 const std::vector<u32>& indices,
                 const std::string& label,
                 MetalLevelData::TreeBuffers* out,
                 MetalLevelData* level,
                 std::string* error) {
  MetalLevelData::TreeBuffers tree;
  tree.vertices = make_static_buffer(device, verts, vert_bytes);
  if (vert_bytes && !tree.vertices) {
    *error = fmt::format("{} vertex buffer allocation failed ({} bytes)", label, vert_bytes);
    return false;
  }
  tree.indices = make_static_buffer(device, indices.data(), indices.size() * sizeof(u32));
  if (!indices.empty() && !tree.indices) {
    *error = fmt::format("{} index buffer allocation failed ({} bytes)", label,
                         indices.size() * sizeof(u32));
    return false;
  }
  tree.time_of_day = make_time_of_day_texture(device);
  if (!tree.time_of_day) {
    *error = fmt::format("{} time-of-day texture allocation failed ({} texels)", label,
                         kMetalTimeOfDayColorCount);
    return false;
  }
  tree.vertex_count = vert_count;
  tree.index_count = (u32)indices.size();
  level->vertex_bytes += vert_bytes;
  level->index_bytes += indices.size() * sizeof(u32);
  *out = std::move(tree);
  return true;
}

void release_level_textures(TexturePool& pool, MetalLevelData& data) {
  {
    std::lock_guard<std::mutex> pool_lock(pool.mutex());
    for (size_t i = 0; i < data.level->textures.size() && i < data.textures.size(); i++) {
      const auto& tex = data.level->textures[i];
      if (tex.load_to_pool && data.textures[i]) {
        pool.unload_texture(PcTextureId::from_combo_id(tex.combo_id), data.textures[i]);
      }
    }
  }
  for (u64 handle : data.textures) {
    metal_texture_release(handle);
  }
  data.textures.clear();
}

}  // namespace

bool metal_background_expect(bool condition,
                             const std::string& where,
                             const std::string& what,
                             MetalBackgroundState* bg) {
  if (condition) {
    return true;
  }
  if (bg) {
    bg->unexpected_dma++;
  }
  static std::unordered_map<std::string, bool> logged;
  const std::string key = where + "|" + what;
  if (!logged[key]) {
    logged[key] = true;
    lg::warn("Metal {}: expected {}; the bucket is skipped (logged once)", where, what);
  }
  return false;
}

void metal_finish_bucket(DmaFollower& dma, const MetalSharedRenderState& state) {
  while (dma.current_tag_offset() != state.next_bucket) {
    dma.read_and_advance();
  }
}

void MetalBackgroundState::reset_frame(
    bool enable_camera_trace,
    const metal_camera_trace::Snapshot* expected_camera,
    const metal_camera_trace::RenderSnapshot* expected_render_camera) {
  for (auto& vis : occlusion_vis) {
    vis.valid = false;
  }
  tfrag_draws = 0;
  tfrag_tris = 0;
  tie_draws = 0;
  tie_tris = 0;
  tie_envmap_second_draws = 0;
  tie_envmap_second_tris = 0;
  tie_wind_draws_skipped = 0;
  shrub_draws = 0;
  shrub_tris = 0;
  missing_levels = 0;
  missing_textures = 0;
  anim_slot_draws = 0;
  unexpected_dma = 0;
  camera_trace_enabled = enable_camera_trace;
  camera_trace.reset(enable_camera_trace ? expected_camera : nullptr);
  render_camera_trace.reset(enable_camera_trace ? expected_render_camera : nullptr);
  first_camera_mismatch_bucket.clear();
}

void MetalBackgroundState::observe_camera(const MetalGoalBackgroundCameraData& camera,
                                          const std::string& bucket) {
  if (!camera_trace_enabled) {
    return;
  }
  const auto snapshot = metal_camera_trace::make_snapshot(camera.camera, &camera.trans);
  const auto observation = camera_trace.observe(snapshot);
  const auto render_snapshot = metal_camera_trace::make_render_snapshot(
      camera.camera, &camera.hvdf_off, &camera.fog, &camera.trans, camera.rot,
      camera.perspective);
  const auto render_observation = render_camera_trace.observe(render_snapshot);
  if (first_camera_mismatch_bucket.empty() &&
      (observation.expected_mismatch_qwords || observation.packet_mismatch_qwords ||
       render_observation.expected_mismatch_qwords || render_observation.packet_mismatch_qwords)) {
    first_camera_mismatch_bucket = bucket;
  }
}

namespace metal_level_data {

MetalLevelData* load_fr3(id<MTLDevice> device,
                         id<MTLCommandQueue> queue,
                         TexturePool& pool,
                         const std::string& path,
                         bool is_common,
                         std::string* error) {
  Timer load_timer;
  if (!fs::exists(path)) {
    *error = "file does not exist: " + path;
    return nullptr;
  }

  auto compressed = file_util::read_binary_file(path);
  auto decomp = compression::decompress_zstd(compressed.data(), compressed.size());
  if (decomp.size() < 2) {
    *error = "fr3 is too small: " + path;
    return nullptr;
  }
  u16 version = 0;
  memcpy(&version, decomp.data(), 2);
  if (version != tfrag3::TFRAG3_VERSION) {
    *error = fmt::format("fr3 version {} does not match this build's {}", version,
                         tfrag3::TFRAG3_VERSION);
    return nullptr;
  }

  auto data = std::make_unique<MetalLevelData>();
  data->level = std::make_unique<tfrag3::Level>();
  Serializer ser(decomp.data(), decomp.size());
  data->level->serialize(ser);
  data->load_id = next_load_id();
  tfrag3::Level& level = *data->level;

  // textures: same order and same rule as the GL TextureLoaderStage.
  if (!metal_add_textures(device, queue, pool, level.textures, is_common, &data->textures, error)) {
    return nullptr;
  }

  // geometry. The unpack step (packed -> GPU vertices and the full index list)
  // is the shared tfrag3 code, unchanged.
  for (int geo = 0; geo < tfrag3::TFRAG_GEOS; geo++) {
    for (size_t tree_idx = 0; tree_idx < level.tfrag_trees[geo].size(); tree_idx++) {
      auto& tree = level.tfrag_trees[geo][tree_idx];
      tree.unpack();
      MetalLevelData::TreeBuffers buffers;
      if (!upload_tree(device, tree.unpacked.vertices.data(),
                       tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex),
                       (u32)tree.unpacked.vertices.size(), tree.unpacked.indices,
                       fmt::format("tfrag geo {} tree {}", geo, tree_idx), &buffers, data.get(),
                       error)) {
        release_level_textures(pool, *data);
        return nullptr;
      }
      data->tfrag[geo].push_back(std::move(buffers));
    }
  }

  for (int geo = 0; geo < tfrag3::TIE_GEOS; geo++) {
    for (size_t tree_idx = 0; tree_idx < level.tie_trees[geo].size(); tree_idx++) {
      auto& tree = level.tie_trees[geo][tree_idx];
      tree.unpack();
      MetalLevelData::TreeBuffers buffers;
      if (!upload_tree(device, tree.unpacked.vertices.data(),
                       tree.unpacked.vertices.size() * sizeof(tfrag3::PreloadedVertex),
                       (u32)tree.unpacked.vertices.size(), tree.unpacked.indices,
                       fmt::format("tie geo {} tree {}", geo, tree_idx), &buffers, data.get(),
                       error)) {
        release_level_textures(pool, *data);
        return nullptr;
      }
      data->tie[geo].push_back(std::move(buffers));
    }
  }

  for (size_t tree_idx = 0; tree_idx < level.shrub_trees.size(); tree_idx++) {
    auto& tree = level.shrub_trees[tree_idx];
    tree.unpack();
    MetalLevelData::TreeBuffers buffers;
    if (!upload_tree(device, tree.unpacked.vertices.data(),
                     tree.unpacked.vertices.size() * sizeof(tfrag3::ShrubGpuVertex),
                     (u32)tree.unpacked.vertices.size(), tree.indices,
                     fmt::format("shrub tree {}", tree_idx), &buffers, data.get(), error)) {
      release_level_textures(pool, *data);
      return nullptr;
    }
    data->shrub.push_back(std::move(buffers));
  }

  const std::string name = level.level_name;
  unload(pool, name);
  auto* raw = data.get();
  level_map()[name] = std::move(data);
  lg::info("Metal level '{}' loaded in {:.1f}ms: {} textures, {:.1f} MB verts, {:.1f} MB indices",
           name, load_timer.getMs(), (int)raw->textures.size(),
           raw->vertex_bytes / (1024.f * 1024.f), raw->index_bytes / (1024.f * 1024.f));
  return raw;
}

MetalLevelData* get(const std::string& name) {
  auto it = level_map().find(name);
  return it == level_map().end() ? nullptr : it->second.get();
}

size_t level_count() {
  return level_map().size();
}

bool unload(TexturePool& pool, const std::string& name) {
  auto it = level_map().find(name);
  if (it == level_map().end()) {
    return false;
  }
  MetalLevelData& data = *it->second;
  // The pool's contract: give/unload run under its published mutex, never
  // across a GPU wait. unload_texture repoints any slot still holding this
  // texture at the placeholder.
  release_level_textures(pool, data);
  // The buffers and time-of-day textures go with the MetalLevelData. Command
  // buffers already committed retain what they reference, so an in-flight
  // frame keeps its resources alive until the GPU is done with them.
  level_map().erase(it);
  return true;
}

void clear(TexturePool& pool) {
  while (!level_map().empty()) {
    unload(pool, level_map().begin()->first);
  }
}

}  // namespace metal_level_data

// ---------------------------------------------------------------------------
// DrawMode -> pipeline state (setup_opengl_from_draw_mode + setup_tfrag_shader)
// ---------------------------------------------------------------------------

MetalBackgroundDrawSettings metal_background_settings_from_draw_mode(DrawMode mode,
                                                                    MetalShaderId shader,
                                                                    const MetalFrameContext& ctx,
                                                                    MetalBackgroundState* bg) {
  MetalBackgroundDrawSettings out;
  // The GL setup_opengl_from_draw_mode asserts on every GS mode it has not
  // seen; here they are reported and the nearest safe state is kept, so an
  // unexpected level draws (visibly wrong, and counted) instead of aborting.
  auto expect = [&](bool ok, const char* what) {
    return metal_background_expect(ok, "background draw mode", what, bg);
  };
  out.pso.shader = shader;
  out.pso.color_format = ctx.color_format;
  out.pso.depth_format = ctx.depth_format;

  if (mode.get_zt_enable()) {
    out.depth.depth_test = true;
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        out.depth.compare = MTLCompareFunctionNever;
        break;
      case GsTest::ZTest::ALWAYS:
        out.depth.compare = MTLCompareFunctionAlways;
        break;
      case GsTest::ZTest::GEQUAL:
        out.depth.compare = MTLCompareFunctionGreaterEqual;
        break;
      case GsTest::ZTest::GREATER:
        out.depth.compare = MTLCompareFunctionGreater;
        break;
      default:
        expect(false, "a known GS depth test");
        break;
    }
  } else {
    out.depth.depth_test = false;
    out.depth.compare = MTLCompareFunctionAlways;
  }

  bool blend_enable =
      mode.get_ab_enable() && mode.get_alpha_blend() != DrawMode::AlphaBlend::DISABLED;
  if (blend_enable) {
    out.pso.blend_src_alpha = MTLBlendFactorOne;
    out.pso.blend_dst_alpha = MTLBlendFactorZero;
    out.pso.blend_op_rgb = MTLBlendOperationAdd;
    out.pso.blend_op_alpha = MTLBlendOperationAdd;
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_SRC_SRC_SRC:
        // (SRC - SRC) * alpha + SRC = SRC: no blend
        blend_enable = false;
        break;
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        out.pso.blend_src_rgb = MTLBlendFactorOne;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        // Cv = (Cs - Cd) * FIX + Cd, with the GL renderer's constant of 0.5
        out.pso.blend_src_rgb = MTLBlendFactorBlendColor;
        out.pso.blend_dst_rgb = MTLBlendFactorBlendColor;
        out.needs_blend_color = true;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        out.pso.blend_src_rgb = MTLBlendFactorSourceAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_op_rgb = MTLBlendOperationReverseSubtract;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        out.pso.blend_src_rgb = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_rgb = MTLBlendFactorOne;
        out.pso.blend_src_alpha = MTLBlendFactorDestinationAlpha;
        out.pso.blend_dst_alpha = MTLBlendFactorOne;
        out.color_mult = 0.5f;
        break;
      default:
        expect(false, "a known GS alpha-blend mode");
        blend_enable = false;
        break;
    }
  }
  out.pso.blend_enable = blend_enable;

  out.sampler.wrap_s =
      mode.get_clamp_s_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  out.sampler.wrap_t =
      mode.get_clamp_t_enable() ? MTLSamplerAddressModeClampToEdge : MTLSamplerAddressModeRepeat;
  if (mode.get_filt_enable()) {
    out.sampler.min_filter = MTLSamplerMinMagFilterLinear;
    out.sampler.mag_filter = MTLSamplerMinMagFilterLinear;
    // the GL background path passes mipmap = true to setup_opengl_from_draw_mode
    out.sampler.mip_filter = MTLSamplerMipFilterLinear;
  } else {
    out.sampler.min_filter = MTLSamplerMinMagFilterNearest;
    out.sampler.mag_filter = MTLSamplerMinMagFilterNearest;
    out.sampler.mip_filter = MTLSamplerMipFilterNotMipmapped;
  }

  // the game sets atest NEVER + FB_ONLY to mean "no depth writes"
  bool alpha_hack_to_disable_z_write = false;
  float alpha_min = 0.f;
  if (mode.get_at_enable()) {
    switch (mode.get_alpha_test()) {
      case DrawMode::AlphaTest::ALWAYS:
        break;
      case DrawMode::AlphaTest::GEQUAL:
        alpha_min = mode.get_aref() / 127.f;
        switch (mode.get_alpha_fail()) {
          case GsTest::AlphaFail::KEEP:
            break;
          case GsTest::AlphaFail::FB_ONLY:
            if (mode.get_depth_write_enable()) {
              out.afail_double_draw = true;
              out.aref_second = alpha_min;
            } else {
              alpha_min = 0.f;
            }
            break;
          default:
            expect(false, "a known GS alpha-fail mode");
            break;
        }
        break;
      case DrawMode::AlphaTest::NEVER:
        if (mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
          alpha_hack_to_disable_z_write = true;
        } else {
          expect(false, "atest NEVER to be paired with alpha-fail FB_ONLY");
        }
        break;
      default:
        expect(false, "a known GS alpha test");
        break;
    }
  }
  out.depth.depth_write = mode.get_depth_write_enable() && !alpha_hack_to_disable_z_write;
  out.aref_first = alpha_min;
  return out;
}

// ---------------------------------------------------------------------------
// Camera math (make_new_cam_mat / first_tfrag_draw_setup /
// init_etie_cam_uniforms)
// ---------------------------------------------------------------------------

namespace {

// Verbatim port of make_new_cam_mat (background_common.cpp). The comments there
// explain each term; this must not drift from it.
std::array<math::Vector4f, 4> make_new_cam_mat(const math::Vector4f cam_T_w[4],
                                               const math::Vector4f persp[4],
                                               float fog_constant,
                                               float hvdf_z) {
  const float game_pxx = persp[0][0];
  const float pc_pxx = fog_constant * game_pxx / 256.f;
  const float game_pyy = persp[1][1];
  const float pc_pyy = -fog_constant * game_pyy / 128.f;
  const float depth_scale = fog_constant * persp[2][2] / 8388608;
  const float game_pzw = persp[2][3];
  const float game_depth_offset = persp[3][2];

  math::Vector3f persp_scale(pc_pxx, pc_pyy, depth_scale);
  const float pc_z_offset = (hvdf_z / 8388608.f - 1.f);
  persp_scale.z() += pc_z_offset * game_pzw;

  std::array<math::Vector4f, 4> result;
  for (auto& x : result) {
    x.set_zero();
  }
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      result[row][col] = cam_T_w[row][col] * persp_scale[col];
    }
  }
  for (int row = 0; row < 3; row++) {
    result[row][3] = cam_T_w[row][2] * game_pzw;
  }
  result[3][2] = fog_constant * game_depth_offset / 8388608;
  return result;
}

constexpr PerGameVersion<int> kScissorHeight(448, 416, 416, 416);

}  // namespace

float metal_height_scale(GameVersion version) {
  return version == GameVersion::Jak1 ? 1.f : 0.5f;
}

float metal_scissor_adjust(GameVersion version) {
  return 512.f / (float)kScissorHeight[version];
}

void metal_fill_background_vs_params(const MetalGoalBackgroundCameraData& camera,
                                     GameVersion version,
                                     MetalBackgroundVsParams* out) {
  metal_fill_background_vs_params(camera, version, {}, out);
}

void metal_fill_background_vs_params(const MetalGoalBackgroundCameraData& camera,
                                     GameVersion version,
                                     const metal_renderer::ViewTransform& view_transform,
                                     MetalBackgroundVsParams* out) {
  auto newcam =
      make_new_cam_mat(camera.rot, camera.perspective, camera.fog.x(), camera.hvdf_off.z());
  memcpy(out->pc_camera, newcam[0].data(), sizeof(out->pc_camera));
  memcpy(out->hvdf_offset, camera.hvdf_off.data(), sizeof(out->hvdf_offset));
  memcpy(out->cam_trans, camera.trans.data(), sizeof(out->cam_trans));
  out->fog_min = camera.fog.y();
  out->fog_max = camera.fog.z();
  out->height_scale = metal_height_scale(version);
  out->scissor_adjust = metal_scissor_adjust(version);
  memcpy(out->view_clip_from_game_clip, view_transform.clip_from_game_clip.data(),
         sizeof(out->view_clip_from_game_clip));
}

void metal_fill_etie_vs_params(const MetalGoalBackgroundCameraData& camera,
                               GameVersion version,
                               MetalEtieVsParams* out) {
  metal_fill_etie_vs_params(camera, version, {}, out);
}

void metal_fill_etie_vs_params(const MetalGoalBackgroundCameraData& camera,
                               GameVersion version,
                               const metal_renderer::ViewTransform& view_transform,
                               MetalEtieVsParams* out) {
  memcpy(out->cam_no_persp, camera.rot[0].data(), sizeof(out->cam_no_persp));
  memcpy(out->camera, camera.camera[0].data(), sizeof(out->camera));
  memcpy(out->hvdf_offset, camera.hvdf_off.data(), sizeof(out->hvdf_offset));

  // init_etie_cam_uniforms (Tie3.cpp), unchanged.
  const float inv_fog = 1.f / camera.fog[0];
  const auto& hvdf_off = camera.hvdf_off;
  const float pxx = camera.perspective[0].x();
  const float pyy = camera.perspective[1].y();
  const float pzz = camera.perspective[2].z();
  const float pzw = camera.perspective[2].w();
  const float pwz = camera.perspective[3].z();
  const float scale = pzw * inv_fog;
  out->persp0[0] = scale * hvdf_off.x();
  out->persp0[1] = scale * hvdf_off.y();
  out->persp0[2] = scale * hvdf_off.z() + pzz;
  out->persp0[3] = scale;
  out->persp1[0] = pxx;
  out->persp1[1] = pyy;
  out->persp1[2] = pwz;
  out->persp1[3] = 0.f;

  out->fog_min = camera.fog.y();
  out->fog_max = camera.fog.z();
  out->height_scale = metal_height_scale(version);
  out->scissor_adjust = metal_scissor_adjust(version);
  memcpy(out->view_clip_from_game_clip, view_transform.clip_from_game_clip.data(),
         sizeof(out->view_clip_from_game_clip));
}

void metal_fill_background_fs_params(const MetalSharedRenderState& state,
                                     MetalBackgroundFsParams* out) {
  out->fog_color[0] = state.fog_color[0] / 255.f;
  out->fog_color[1] = state.fog_color[1] / 255.f;
  out->fog_color[2] = state.fog_color[2] / 255.f;
  out->fog_color[3] = state.fog_intensity / 255.f;
  out->alpha_min = 0.f;
  out->alpha_max = 10.f;
  out->gfx_hack_no_tex = 0;
}

// ---------------------------------------------------------------------------
// Time of day
// ---------------------------------------------------------------------------

namespace {

// _mm_adds_epi16: signed saturating add on the 16-bit lanes.
inline u16 sat_add_s16(u16 a, u16 b) {
  s32 sum = (s32)(s16)a + (s32)(s16)b;
  sum = std::clamp(sum, -32768, 32767);
  return (u16)(s16)sum;
}

}  // namespace

void metal_interp_time_of_day(const math::Vector<s32, 4> itimes[4],
                              const tfrag3::PackedTimeOfDay& packed_colors,
                              math::Vector<u8, 4>* out) {
  // weight extraction: identical to the GL version.
  u16 weights[8][4];
  for (int component = 0; component < 8; component++) {
    int quad_idx = component / 2;
    int word_off = (component % 2 * 2);
    for (int channel = 0; channel < 4; channel++) {
      int word = word_off + (channel / 2);
      int hw_off = channel % 2;
      u32 word_val = itimes[quad_idx][word];
      u32 hw_val = hw_off ? (word_val >> 16) : word_val;
      weights[component][channel] = (u16)(hw_val & 0xff);
    }
  }

  // The SSE version processes whole quads of 4 colors and leaves a partial
  // trailing quad alone; matched here so both produce the same buffer.
  const u8* data = packed_colors.data.data();
  for (u32 color_quad = 0; color_quad < packed_colors.color_count / 4; color_quad++) {
    const u8* base = data + color_quad * 128;
    for (u32 color = 0; color < 4; color++) {
      auto& o = out[color_quad * 4 + color];
      for (u32 channel = 0; channel < 4; channel++) {
        // _mm_mullo_epi16 keeps the low 16 bits of each product.
        u16 p[8];
        for (u32 component = 0; component < 8; component++) {
          u16 c = base[component * 16 + color * 4 + channel];
          p[component] = (u16)(weights[component][channel] * c);
        }
        // same addition tree as the SSE version - saturating adds are not
        // associative, so the order matters at the extremes.
        u16 a = sat_add_s16(p[0], p[1]);
        u16 b = sat_add_s16(p[2], p[3]);
        u16 c = sat_add_s16(p[4], p[5]);
        u16 d = sat_add_s16(p[6], p[7]);
        a = sat_add_s16(a, b);
        c = sat_add_s16(c, d);
        a = sat_add_s16(a, c);
        // weights are scaled by 2^6; alpha saturates at 128, color at 255.
        u16 v = (u16)(a >> 6);
        u16 limit = (channel == 3) ? 128 : 255;
        o[channel] = (u8)std::min(v, limit);
      }
    }
  }
}

void metal_update_time_of_day_texture(id<MTLTexture> tex,
                                      const math::Vector<u8, 4>* colors,
                                      u32 count) {
  if (!tex || count == 0) {
    return;
  }
  count = std::min<u32>(count, kMetalTimeOfDayColorCount);
  // bytesPerRow must be 0 for 1D textures.
  [tex replaceRegion:MTLRegionMake1D(0, count) mipmapLevel:0 withBytes:colors bytesPerRow:0];
}

// ---------------------------------------------------------------------------
// Culling and visibility -> draw runs
// ---------------------------------------------------------------------------

bool metal_sphere_in_view_ref(const math::Vector4f& sphere, const math::Vector4f* planes) {
  math::Vector4f acc =
      planes[0] * sphere.x() + planes[1] * sphere.y() + planes[2] * sphere.z() - planes[3];
  return acc.x() > -sphere.w() && acc.y() > -sphere.w() && acc.z() > -sphere.w() &&
         acc.w() > -sphere.w();
}

void metal_cull_check_all_slow(const math::Vector4f* planes,
                               const std::vector<tfrag3::VisNode>& nodes,
                               const u8* level_occlusion_string,
                               u8* out) {
  if (level_occlusion_string) {
    for (size_t i = 0; i < nodes.size(); i++) {
      u16 my_id = nodes[i].my_id;
      bool not_occluded =
          my_id != 0xffff && level_occlusion_string[my_id / 8] & (1 << (7 - (my_id & 7)));
      out[i] = not_occluded && metal_sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  } else {
    for (size_t i = 0; i < nodes.size(); i++) {
      out[i] = metal_sphere_in_view_ref(nodes[i].bsphere, planes);
    }
  }
}

u32 metal_make_draw_runs_from_vis_string(std::pair<u32, u32>* draw_runs_out,
                                         MetalDrawRun* runs_out,
                                         const std::vector<tfrag3::StripDraw>& draws,
                                         const std::vector<u8>& vis_data,
                                         u32* tris_per_draw_out) {
  u32 run_idx = 0;
  u32 num_tris = 0;
  u32 sanity_check = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u32 iidx = draw.unpacked.idx_of_first_idx_in_full_buffer;
    // the draws should tile the tree's index buffer; metal-proof checks this on
    // real level data, so here it is a diagnostic rather than a hard stop
    metal_background_expect(sanity_check == iidx, "level index layout",
                            "the draws to tile the index buffer without gaps", nullptr);
    std::pair<u32, u32> ds{run_idx, 0};
    u32 draw_tris = 0;
    bool building_run = false;
    u32 run_start = 0;
    for (auto& grp : draw.vis_groups) {
      sanity_check += grp.num_inds;
      bool vis = grp.vis_idx_in_pc_bvh == UINT16_MAX || vis_data[grp.vis_idx_in_pc_bvh];
      if (vis) {
        num_tris += grp.num_tris;
        draw_tris += grp.num_tris;
      }
      if (building_run) {
        if (!vis) {
          building_run = false;
          runs_out[run_idx] = {run_start, iidx - run_start};
          ds.second++;
          run_idx++;
        }
      } else if (vis) {
        building_run = true;
        run_start = iidx;
      }
      iidx += grp.num_inds;
    }
    if (building_run) {
      runs_out[run_idx] = {run_start, iidx - run_start};
      ds.second++;
      run_idx++;
    }
    draw_runs_out[i] = ds;
    if (tris_per_draw_out) {
      tris_per_draw_out[i] = draw_tris;
    }
  }
  return num_tris;
}

u32 metal_make_all_visible_draw_runs(std::pair<u32, u32>* draw_runs_out,
                                     MetalDrawRun* runs_out,
                                     const std::vector<tfrag3::StripDraw>& draws,
                                     u32* tris_per_draw_out) {
  u32 run_idx = 0;
  u32 num_tris = 0;
  for (size_t i = 0; i < draws.size(); i++) {
    const auto& draw = draws[i];
    u32 num_inds = 0;
    u32 draw_tris = 0;
    for (auto& grp : draw.vis_groups) {
      draw_tris += grp.num_tris;
      num_inds += grp.num_inds;
    }
    num_tris += draw_tris;
    runs_out[run_idx] = {draw.unpacked.idx_of_first_idx_in_full_buffer, num_inds};
    draw_runs_out[i] = {run_idx, 1};
    run_idx++;
    if (tris_per_draw_out) {
      tris_per_draw_out[i] = draw_tris;
    }
  }
  return num_tris;
}

void metal_make_all_visible_draw_runs(std::pair<u32, u32>* draw_runs_out,
                                      MetalDrawRun* runs_out,
                                      const std::vector<tfrag3::ShrubDraw>& draws) {
  for (size_t i = 0; i < draws.size(); i++) {
    runs_out[i] = {draws[i].first_index_index, draws[i].num_indices};
    draw_runs_out[i] = {(u32)i, 1};
  }
}

id<MTLTexture> metal_background_texture(const MetalLevelData& level,
                                        s32 tree_tex_id,
                                        MetalSharedRenderState* render_state,
                                        MetalBackgroundState* bg) {
  u64 handle = 0;
  if (tree_tex_id >= 0) {
    if ((size_t)tree_tex_id < level.textures.size()) {
      handle = level.textures[tree_tex_id];
    }
  } else {
    // negative = texture-animator slot. The animator is a Jak 2/3 renderer; a
    // Jak 1 level should never ask for one, so this is counted rather than
    // guessed at.
    bg->anim_slot_draws++;
  }

  id<MTLTexture> tex = handle ? metal_texture_lookup(handle) : nil;
  if (!tex) {
    bg->missing_textures++;
    tex = metal_texture_lookup(render_state->texture_pool->get_placeholder_texture());
  }
  return tex;
}
