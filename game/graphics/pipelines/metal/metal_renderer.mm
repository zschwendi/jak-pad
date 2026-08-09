#include "metal_renderer.h"

#include "common/dma/dma_chain_validation.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <stdexcept>

#include "fmt/format.h"

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_jak2_blit_display_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"
#include "game/graphics/pipelines/metal/metal_shadow_renderer.h"
#include "game/graphics/pipelines/metal/metal_kernel_bridge.h"
#include "game/graphics/pipelines/metal/metal_merc.h"
#include "game/graphics/pipelines/metal/metal_shrub.h"
#include "game/graphics/pipelines/metal/metal_ocean_renderer.h"
#include "game/graphics/pipelines/metal/metal_sky_renderer.h"
#include "game/graphics/pipelines/metal/metal_sprite_renderer.h"
#include "game/graphics/pipelines/metal/metal_tfrag.h"
#include "game/graphics/pipelines/metal/metal_tie.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#include <TargetConditionals.h>

struct MetalPresentationState {
  std::mutex mutex;
  std::condition_variable command_buffer_cv;
  std::condition_variable presentation_cv;
  metal_camera_trace::PresentationOrderTrace order;
  metal_camera_trace::ProducerAlternationTrace producer_camera;
  metal_camera_trace::RenderAlternationTrace render_camera;
  u64 last_drawable_id = 0;
  u64 last_engine_frame_id = 0;
  u64 last_host_tick_id = 0;
  u64 last_chain_ordinal = 0;
  u64 command_buffers_completed = 0;
  u64 command_buffer_errors = 0;
  int last_command_buffer_status = 0;
  s64 last_command_buffer_error_code = 0;
  bool mismatch_reported = false;
  bool command_buffer_error_reported = false;
  bool producer_alternation_reported = false;
  bool render_alternation_reported = false;
};

// Precompiled Metal shader library, embedded at build time from
// shaders/*.metal (see game/CMakeLists.txt and embed_metallib.cmake).
extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

// Replay/test only: a capture carries the s7 its frame ran with, and a replay
// has no booted GOAL kernel to ask.
static u32 g_s7_override = 0;

void metal_set_s7_override(u32 s7_ptr) {
  g_s7_override = s7_ptr;
}

namespace {

constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;

void schedule_present(id<MTLCommandBuffer> cmds,
                      id<CAMetalDrawable> drawable,
                      const MetalRenderOptions& opts) {
  if (opts.presentation_time > 0.0) {
    [cmds presentDrawable:drawable atTime:opts.presentation_time];
#if !TARGET_OS_SIMULATOR
  } else if (opts.min_present_duration > 0.0) {
    [cmds presentDrawable:drawable afterMinimumDuration:opts.min_present_duration];
#endif
  } else {
    [cmds presentDrawable:drawable];
  }
}

id<MTLTexture> make_color_target(id<MTLDevice> device, int w, int h, bool sampled) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
                                                                  width:w
                                                                 height:h
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | (sampled ? MTLTextureUsageShaderRead : 0);
#if TARGET_OS_OSX
  desc.storageMode = MTLStorageModeManaged;
#else
  desc.storageMode = MTLStorageModeShared;
#endif
  return [device newTextureWithDescriptor:desc];
}

id<MTLTexture> make_depth_target(id<MTLDevice> device, int w, int h) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                  width:w
                                                                 height:h
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

id<MTLTexture> make_checker_texture(id<MTLDevice> device) {
  constexpr int kSize = 8;
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                  width:kSize
                                                                 height:kSize
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> tex = [device newTextureWithDescriptor:desc];
  u8 pixels[kSize * kSize * 4];
  for (int y = 0; y < kSize; y++) {
    for (int x = 0; x < kSize; x++) {
      u8* p = &pixels[(y * kSize + x) * 4];
      bool red = ((x + y) & 1) != 0;
      p[0] = 255;
      p[1] = red ? 0 : 255;
      p[2] = red ? 0 : 255;
      p[3] = 255;
    }
  }
  [tex replaceRegion:MTLRegionMake2D(0, 0, kSize, kSize)
         mipmapLevel:0
           withBytes:pixels
         bytesPerRow:kSize * 4];
  return tex;
}

// Matches PresentParams in shaders/scaffold.metal.
struct PresentParams {
  float color_mult[4];
  float color_add[4];
};

// Same math as OpenGLRenderer::do_pcrtc_effects.
PresentParams make_present_params(int brightness_contrast_color, int brightness_contrast_alpha) {
  float color = (float)brightness_contrast_color / 128.0f;
  float alpha = (float)brightness_contrast_alpha / 128.0f;
  PresentParams params{{1.f, 1.f, 1.f, alpha}, {0.f, 0.f, 0.f, 0.f}};
  if (brightness_contrast_color < 0) {
    // subtractive blend - note that color is already negative
    float color_neg = color * alpha;
    params.color_add[0] = params.color_add[1] = params.color_add[2] = color_neg;
  } else {
    params.color_add[0] = params.color_add[1] = params.color_add[2] = color;
  }
  return params;
}

int add_quad(std::vector<ScaffoldVertex>& out,
             float x0,
             float y0,
             float x1,
             float y1,
             float z,
             float r,
             float g,
             float b,
             float a,
             float use_tex) {
  int first = (int)out.size();
  // v is 0 at the top edge (y1) so texel row 0 lands at the top
  auto vert = [&](float x, float y) {
    ScaffoldVertex v{};
    v.pos[0] = x;
    v.pos[1] = y;
    v.pos[2] = z;
    v.uv[0] = (x - x0) / (x1 - x0);
    v.uv[1] = 1.f - (y - y0) / (y1 - y0);
    v.color[0] = r;
    v.color[1] = g;
    v.color[2] = b;
    v.color[3] = a;
    v.use_tex = use_tex;
    return v;
  };
  out.push_back(vert(x0, y0));
  out.push_back(vert(x1, y0));
  out.push_back(vert(x1, y1));
  out.push_back(vert(x0, y0));
  out.push_back(vert(x1, y1));
  out.push_back(vert(x0, y1));
  return first;
}

}  // namespace

/*!
 * Build the Jak 1 bucket renderer table. Ported renderers: TextureUploadHandler
 * for the texture buckets, SkyRenderer / SkyBlendHandler + SkyBlendCPU for the
 * sky, DirectRenderer for the debug/subtitle buckets. Buckets whose GL
 * renderer is not ported yet get a MetalSkipRenderer named after it, which
 * counts and logs the content it consumes. Buckets the GL table leaves empty
 * get the asserting empty renderer.
 */
void MetalRenderer::init_bucket_renderers_jak1() {
  using namespace jak1;
  m_bucket_renderers.resize((int)BucketId::MAX_BUCKETS);

  auto set = [&](BucketId id, std::unique_ptr<MetalBucketRenderer> r) {
    m_bucket_renderers[(int)id] = std::move(r);
  };
  auto skip = [&](BucketId id, const std::string& name) {
    set(id, std::make_unique<MetalSkipRenderer>(name, (int)id));
  };
  auto tex = [&](BucketId id, const std::string& name) {
    set(id, std::make_unique<MetalTextureBucketRenderer>(name, (int)id));
  };
  // --- level geometry (see metal_level_data.h) ---
  const std::vector<tfrag3::TFragmentTreeKind> normal_tfrags = {
      tfrag3::TFragmentTreeKind::NORMAL, tfrag3::TFragmentTreeKind::LOWRES};
  const std::vector<tfrag3::TFragmentTreeKind> dirt_tfrags = {tfrag3::TFragmentTreeKind::DIRT};
  const std::vector<tfrag3::TFragmentTreeKind> ice_tfrags = {tfrag3::TFragmentTreeKind::ICE};
  auto tfrag = [&](BucketId id, const std::string& name,
                   const std::vector<tfrag3::TFragmentTreeKind>& kinds, int level_id) {
    // the game hangs the occlusion strings for every level off TFRAG_LEVEL0
    // (SharedRenderState::bucket_for_vis_copy)
    const bool vis_copy = id == BucketId::TFRAG_LEVEL0;
    set(id, std::make_unique<MetalTFragment>(name, (int)id, kinds, level_id, vis_copy));
  };
  // --- end level geometry ---

  auto sky_cpu_blender = std::make_shared<MetalSkyBlendCPU>(m_device);
  sky_cpu_blender->init_textures(*m_texture_pool, GameVersion::Jak1);

  // eight buckets share one Merc2, as in the GL table
  auto merc = std::make_shared<MetalMerc2>(m_device, m_queue, m_texture_pool);
  auto merc_bucket = [&](BucketId id, const std::string& name) {
    set(id, std::make_unique<MetalMercBucketRenderer>(name, (int)id, merc));
  };

  // every generic bucket shares one Generic2, as in the GL table
  auto generic = std::make_shared<MetalGeneric2>();
  auto generic_bucket = [&](BucketId id, const std::string& name) {
    set(id, std::make_unique<MetalGeneric2BucketRenderer>(name, (int)id, generic));
  };

  set(BucketId::SKY_DRAW, std::make_unique<MetalSkyRenderer>("sky", (int)BucketId::SKY_DRAW));
  {
    auto ocean = std::make_unique<MetalOceanMidAndFar>("ocean-mid-far",
                                                       (int)BucketId::OCEAN_MID_AND_FAR, m_device,
                                                       m_queue);
    ocean->init_textures(*m_texture_pool, GameVersion::Jak1);
    set(BucketId::OCEAN_MID_AND_FAR, std::move(ocean));
  }

  tex(BucketId::TFRAG_TEX_LEVEL0, "l0-tfrag-tex");
  tfrag(BucketId::TFRAG_LEVEL0, "l0-tfrag-tfrag", normal_tfrags, 0);
  set(BucketId::TIE_LEVEL0,
      std::make_unique<MetalTie3>("l0-tfrag-tie", (int)BucketId::TIE_LEVEL0, 0));
  merc_bucket(BucketId::MERC_TFRAG_TEX_LEVEL0, "l0-tfrag-merc");
  generic_bucket(BucketId::GENERIC_TFRAG_TEX_LEVEL0, "l0-tfrag-generic");
  tex(BucketId::TFRAG_TEX_LEVEL1, "l1-tfrag-tex");
  tfrag(BucketId::TFRAG_LEVEL1, "l1-tfrag-tfrag", normal_tfrags, 1);
  set(BucketId::TIE_LEVEL1,
      std::make_unique<MetalTie3>("l1-tfrag-tie", (int)BucketId::TIE_LEVEL1, 1));
  merc_bucket(BucketId::MERC_TFRAG_TEX_LEVEL1, "l1-tfrag-merc");
  generic_bucket(BucketId::GENERIC_TFRAG_TEX_LEVEL1, "l1-tfrag-generic");

  tex(BucketId::SHRUB_TEX_LEVEL0, "l0-shrub-tex");
  set(BucketId::SHRUB_NORMAL_LEVEL0,
      std::make_unique<MetalShrub>("l0-shrub", (int)BucketId::SHRUB_NORMAL_LEVEL0));
  generic_bucket(BucketId::SHRUB_GENERIC_LEVEL0, "l0-shrub-generic");
  tex(BucketId::SHRUB_TEX_LEVEL1, "l1-shrub-tex");
  set(BucketId::SHRUB_NORMAL_LEVEL1,
      std::make_unique<MetalShrub>("l1-shrub", (int)BucketId::SHRUB_NORMAL_LEVEL1));
  generic_bucket(BucketId::SHRUB_GENERIC_LEVEL1, "l1-shrub-generic");

  tex(BucketId::ALPHA_TEX_LEVEL0, "l0-alpha-tex");
  {
    auto handler = std::make_unique<MetalSkyBlendHandler>(
        "l0-alpha-sky-blend-and-tfrag-trans",
        (int)BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, 0, sky_cpu_blender);
    m_sky_blend_handlers[0] = handler.get();
    set(BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, std::move(handler));
  }
  tfrag(BucketId::TFRAG_DIRT_LEVEL0, "l0-alpha-tfrag-dirt", dirt_tfrags, 0);
  tfrag(BucketId::TFRAG_ICE_LEVEL0, "l0-alpha-tfrag-ice", ice_tfrags, 0);
  tex(BucketId::ALPHA_TEX_LEVEL1, "l1-alpha-tex");
  {
    auto handler = std::make_unique<MetalSkyBlendHandler>(
        "l1-alpha-sky-blend-and-tfrag-trans",
        (int)BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, 1, sky_cpu_blender);
    m_sky_blend_handlers[1] = handler.get();
    set(BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1, std::move(handler));
  }
  tfrag(BucketId::TFRAG_DIRT_LEVEL1, "l1-alpha-tfrag-dirt", dirt_tfrags, 1);
  tfrag(BucketId::TFRAG_ICE_LEVEL1, "l1-alpha-tfrag-ice", ice_tfrags, 1);

  merc_bucket(BucketId::MERC_AFTER_ALPHA, "common-alpha-merc");
  generic_bucket(BucketId::GENERIC_ALPHA, "common-alpha-generic");
  set(BucketId::SHADOW, std::make_unique<MetalShadowRenderer>("shadow", (int)BucketId::SHADOW));

  tex(BucketId::PRIS_TEX_LEVEL0, "l0-pris-tex");
  merc_bucket(BucketId::MERC_PRIS_LEVEL0, "l0-pris-merc");
  generic_bucket(BucketId::GENERIC_PRIS_LEVEL0, "l0-pris-generic");
  tex(BucketId::PRIS_TEX_LEVEL1, "l1-pris-tex");
  merc_bucket(BucketId::MERC_PRIS_LEVEL1, "l1-pris-merc");
  generic_bucket(BucketId::GENERIC_PRIS_LEVEL1, "l1-pris-generic");
  {
    // merc samples what this composes, so it is published on the shared state
    // the same way the GL table publishes render_state->eye_renderer
    auto eyes = std::make_unique<MetalEyeRenderer>(
        "common-pris-eyes", (int)BucketId::MERC_EYES_AFTER_PRIS, m_device, m_queue);
    eyes->init_textures(*m_texture_pool, GameVersion::Jak1);
    m_shared_state.eye_renderer = eyes.get();
    set(BucketId::MERC_EYES_AFTER_PRIS, std::move(eyes));
  }
  merc_bucket(BucketId::MERC_AFTER_PRIS, "common-pris-merc");
  generic_bucket(BucketId::GENERIC_PRIS, "common-pris-generic");

  tex(BucketId::WATER_TEX_LEVEL0, "l0-water-tex");
  merc_bucket(BucketId::MERC_WATER_LEVEL0, "l0-water-merc");
  generic_bucket(BucketId::GENERIC_WATER_LEVEL0, "l0-water-generic");
  tex(BucketId::WATER_TEX_LEVEL1, "l1-water-tex");
  merc_bucket(BucketId::MERC_WATER_LEVEL1, "l1-water-merc");
  generic_bucket(BucketId::GENERIC_WATER_LEVEL1, "l1-water-generic");
  {
    auto ocean =
        std::make_unique<MetalOceanNear>("ocean-near", (int)BucketId::OCEAN_NEAR, m_device, m_queue);
    ocean->init_textures(*m_texture_pool, GameVersion::Jak1);
    set(BucketId::OCEAN_NEAR, std::move(ocean));
  }
  skip(BucketId::DEPTH_CUE, "depth-cue");

  tex(BucketId::PRE_SPRITE_TEX, "common-tex");
  set(BucketId::SPRITE, std::make_unique<MetalSpriteRenderer>("sprite", (int)BucketId::SPRITE));

  set(BucketId::DEBUG,
      std::make_unique<MetalDirectRenderer>("debug", (int)BucketId::DEBUG, 0x20000));
  set(BucketId::DEBUG_NO_ZBUF,
      std::make_unique<MetalDirectRenderer>("debug-no-zbuf", (int)BucketId::DEBUG_NO_ZBUF, 0x8000));
  set(BucketId::SUBTITLE,
      std::make_unique<MetalDirectRenderer>("subtitle", (int)BucketId::SUBTITLE, 6000));

  for (size_t i = 0; i < m_bucket_renderers.size(); i++) {
    if (!m_bucket_renderers[i]) {
      m_bucket_renderers[i] =
          std::make_unique<MetalEmptyBucketRenderer>(fmt::format("bucket-{}", i), (int)i);
    }
  }
}

void MetalRenderer::init_bucket_renderers_jak2() {
  const auto& table = metal_renderer::jak2_metal_bucket_table();
  ASSERT(table.size() == static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS));
  m_bucket_renderers.resize(table.size());

  constexpr auto first_tfrag = static_cast<std::size_t>(jak2::BucketId::TFRAG_L0_TFRAG);
  constexpr auto tfrag_stride = static_cast<std::size_t>(jak2::BucketId::TFRAG_L1_TFRAG) -
                                first_tfrag;
  constexpr auto first_shrub = static_cast<std::size_t>(jak2::BucketId::SHRUB_L0_SHRUB);
  constexpr auto shrub_stride = static_cast<std::size_t>(jak2::BucketId::SHRUB_L1_SHRUB) -
                                first_shrub;
  constexpr auto first_tie = static_cast<std::size_t>(jak2::BucketId::TIE_L0_TFRAG);
  constexpr auto tie_stride = static_cast<std::size_t>(jak2::BucketId::TIE_L1_TFRAG) - first_tie;
  constexpr auto first_etie = static_cast<std::size_t>(jak2::BucketId::ETIE_L0_TFRAG);
  constexpr auto etie_stride = static_cast<std::size_t>(jak2::BucketId::ETIE_L1_TFRAG) -
                               first_etie;
  constexpr auto first_merc = static_cast<std::size_t>(jak2::BucketId::MERC_L0_TFRAG);
  constexpr auto merc_stride = static_cast<std::size_t>(jak2::BucketId::MERC_L1_TFRAG) -
                               first_merc;
  const std::vector<tfrag3::TFragmentTreeKind> normal_tfrags = {
      tfrag3::TFragmentTreeKind::NORMAL};
  std::array<MetalTie3*, jak2::LEVEL_MAX> normal_ties = {};
  auto merc = std::make_shared<MetalMerc2>(m_device, m_queue, m_texture_pool);

  for (const auto& descriptor : table) {
    const auto bucket_id = static_cast<std::size_t>(descriptor.id);
    const int batch_size = metal_renderer::jak2_metal_direct_batch_size(bucket_id);
    if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Visibility) {
      ASSERT(bucket_id == static_cast<std::size_t>(jak2::BucketId::BUCKET_2));
      ASSERT(batch_size == 0);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalVisibilityBucketRenderer>(
          "jak2-vis-data", descriptor.id, jak2::LEVEL_MAX);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::BlitDisplay) {
      ASSERT(bucket_id == static_cast<std::size_t>(jak2::BucketId::BUCKET_3));
      ASSERT(batch_size == 0);
      auto renderer = std::make_unique<MetalJak2BlitDisplayRenderer>(
          "blit-display", descriptor.id, m_texture_pool);
      m_jak2_blit_display = renderer.get();
      m_bucket_renderers[bucket_id] = std::move(renderer);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Sprite) {
      ASSERT(bucket_id == static_cast<std::size_t>(jak2::BucketId::PARTICLES));
      ASSERT(batch_size == 0);
      m_bucket_renderers[bucket_id] =
          std::make_unique<MetalSpriteRenderer>("jak2-particles", descriptor.id);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::TFragment) {
      ASSERT(batch_size == 0);
      ASSERT(bucket_id >= first_tfrag && (bucket_id - first_tfrag) % tfrag_stride == 0);
      const int level_id = static_cast<int>((bucket_id - first_tfrag) / tfrag_stride);
      ASSERT(level_id >= 0 && level_id < jak2::LEVEL_MAX);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalTFragment>(
          fmt::format("tfrag-l{}-tfrag", level_id), descriptor.id, normal_tfrags, level_id,
          false);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Shrub) {
      ASSERT(batch_size == 0);
      ASSERT(bucket_id >= first_shrub && (bucket_id - first_shrub) % shrub_stride == 0);
      const int level_id = static_cast<int>((bucket_id - first_shrub) / shrub_stride);
      ASSERT(level_id >= 0 && level_id < jak2::LEVEL_MAX);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalShrub>(
          fmt::format("shrub-l{}-shrub", level_id), descriptor.id);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Tie) {
      ASSERT(batch_size == 0);
      ASSERT(bucket_id >= first_tie && (bucket_id - first_tie) % tie_stride == 0);
      const int level_id = static_cast<int>((bucket_id - first_tie) / tie_stride);
      ASSERT(level_id >= 0 && level_id < jak2::LEVEL_MAX);
      auto renderer = std::make_unique<MetalTie3>(fmt::format("tie-l{}-tfrag", level_id),
                                                  descriptor.id, level_id);
      normal_ties[level_id] = renderer.get();
      m_bucket_renderers[bucket_id] = std::move(renderer);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::TieEnvmap) {
      ASSERT(batch_size == 0);
      ASSERT(bucket_id >= first_etie && (bucket_id - first_etie) % etie_stride == 0);
      const int level_id = static_cast<int>((bucket_id - first_etie) / etie_stride);
      ASSERT(level_id >= 0 && level_id < jak2::LEVEL_MAX);
      ASSERT(normal_ties[level_id]);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalTieEnvmap>(
          fmt::format("etie-l{}-tfrag", level_id), descriptor.id, normal_ties[level_id]);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Merc) {
      ASSERT(batch_size == 0);
      ASSERT(bucket_id >= first_merc && (bucket_id - first_merc) % merc_stride == 0);
      const int level_id = static_cast<int>((bucket_id - first_merc) / merc_stride);
      ASSERT(level_id >= 0 && level_id < jak2::LEVEL_MAX);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalMercBucketRenderer>(
          fmt::format("merc-l{}-tfrag", level_id), descriptor.id, merc);
    } else if (descriptor.behavior ==
               metal_renderer::Jak2MetalBucketBehavior::HostTextureUploadDirect) {
      ASSERT(bucket_id == static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF1) ||
             bucket_id == static_cast<std::size_t>(jak2::BucketId::TEX_ALL_MAP));
      ASSERT(batch_size == 1024 * 6);
      if (m_host_texture_uploads) {
        const char* name = bucket_id == static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF1)
                               ? "debug-no-zbuf1"
                               : "tex-all-map";
        const auto callback_point =
            bucket_id == static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF1)
                ? MetalHostTextureUploadDirectRenderer::CallbackPoint::PcPort12
                : MetalHostTextureUploadDirectRenderer::CallbackPoint::BucketEntry;
        m_bucket_renderers[bucket_id] = std::make_unique<MetalHostTextureUploadDirectRenderer>(
            name, descriptor.id, batch_size, callback_point);
      } else {
        m_bucket_renderers[bucket_id] = std::make_unique<MetalSkipRenderer>(
            "jak2-host-texture-upload-direct-unavailable", descriptor.id);
      }
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::Direct) {
      ASSERT(batch_size != 0);
      const char* name = "direct";
      switch (static_cast<jak2::BucketId>(bucket_id)) {
        case jak2::BucketId::SKY_DRAW:
          name = "sky-draw";
          break;
        case jak2::BucketId::SCREEN_FILTER:
          name = "screen-filter";
          break;
        case jak2::BucketId::PROGRESS:
          name = "progress";
          break;
        case jak2::BucketId::DEBUG_NO_ZBUF2:
          name = "debug-no-zbuf2";
          break;
        default:
          ASSERT(false);
      }
      m_bucket_renderers[bucket_id] =
          std::make_unique<MetalDirectRenderer>(name, descriptor.id, batch_size);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::DeferredSkip) {
      ASSERT(batch_size == 0);
      m_bucket_renderers[bucket_id] = std::make_unique<MetalSkipRenderer>(
          fmt::format("jak2-deferred-{}", bucket_id), descriptor.id);
    } else if (descriptor.behavior ==
               metal_renderer::Jak2MetalBucketBehavior::HostTextureUpload) {
      ASSERT(batch_size == 0);
      if (m_host_texture_uploads) {
        m_bucket_renderers[bucket_id] = std::make_unique<MetalHostHandledRenderer>(
            "jak2-host-texture-upload", descriptor.id);
      } else {
        m_bucket_renderers[bucket_id] = std::make_unique<MetalSkipRenderer>(
            "jak2-host-texture-upload-unavailable", descriptor.id);
      }
    } else {
      ASSERT(descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::StrictEmpty);
      ASSERT(batch_size == 0);
      m_bucket_renderers[bucket_id] =
          std::make_unique<MetalEmptyBucketRenderer>(fmt::format("bucket-{}", bucket_id),
                                                     descriptor.id);
    }
  }
}

void MetalRenderer::init_bucket_renderers(TexturePool* pool,
                                          GameVersion version,
                                          bool host_texture_uploads) {
  m_texture_pool = pool;
  m_host_texture_uploads = host_texture_uploads;
  m_jak2_blit_display = nullptr;
  m_shared_state.version = version;
  switch (version) {
    case GameVersion::Jak1:
      init_bucket_renderers_jak1();
      break;
    case GameVersion::Jak2:
      init_bucket_renderers_jak2();
      break;
    default:
      ASSERT_MSG(false, "Metal bucket renderers only support Jak 1 and Jak 2");
  }
}

bool MetalRenderer::init(id<MTLDevice> device) {
  m_device = device;
  m_queue = [device newCommandQueue];
  m_stream.init(device);
  m_presentation_state = std::make_shared<MetalPresentationState>();

  dispatch_data_t lib_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                  nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithData:lib_data error:&error];
  if (!library) {
    lg::error("Metal: could not load embedded metallib: {}",
              [[error localizedDescription] UTF8String]);
    return false;
  }
  if (!m_pso_cache.init(device, library)) {
    return false;
  }
  m_sampler_cache.init(device);

  m_checker_texture = make_checker_texture(device);
  build_validation_scene();
  return true;
}

/*!
 * Fixed validation scene. Depth follows the GL renderer's PS2 convention: depth
 * clears to 0.0 and larger z is closer (GEQUAL). Each draw carries the GL-style
 * state that varies per draw in the real renderers, so multiple distinct PSOs
 * and depth-stencil states flow through the caches every frame.
 */
void MetalRenderer::build_validation_scene() {
  std::vector<ScaffoldVertex> verts;

  MetalDepthStencilKey depth_rw{true, MTLCompareFunctionGreaterEqual, true};
  MetalDepthStencilKey depth_ro{true, MTLCompareFunctionGreaterEqual, false};
  MetalDepthStencilKey depth_off{false, MTLCompareFunctionAlways, false};

  MetalPsoKey opaque;
  opaque.shader = MetalShaderId::SCAFFOLD;

  // 1) base: opaque mid-gray quad at z=0.5
  int first = add_quad(verts, -0.75f, -0.75f, 0.75f, 0.75f, 0.5f, 0.5f, 0.5f, 0.5f, 1.f, 0.f);
  m_scene_draws.push_back({first, 6, opaque, depth_rw});

  // 2) behind the base (z=0.25 fails GEQUAL vs 0.5): must be invisible.
  //    same PSO and depth state as draw 1 - an in-frame cache hit.
  first = add_quad(verts, -0.6f, -0.2f, -0.2f, 0.2f, 0.25f, 1.f, 0.f, 0.f, 1.f, 0.f);
  m_scene_draws.push_back({first, 6, opaque, depth_rw});

  // 3) checkerboard texture in front (z=0.9), same opaque PSO
  first = add_quad(verts, -0.7f, 0.3f, -0.3f, 0.7f, 0.9f, 1.f, 1.f, 1.f, 1.f, 1.f);
  m_scene_draws.push_back({first, 6, opaque, depth_rw});

  // 4) additive blend (ONE, ONE), depth test off: gray + (0,0,0.25)
  MetalPsoKey additive = opaque;
  additive.blend_enable = true;
  additive.blend_src_rgb = additive.blend_dst_rgb = MTLBlendFactorOne;
  additive.blend_src_alpha = MTLBlendFactorOne;
  additive.blend_dst_alpha = MTLBlendFactorZero;
  first = add_quad(verts, 0.2f, -0.6f, 0.6f, -0.2f, 0.6f, 0.f, 0.f, 0.25f, 1.f, 0.f);
  m_scene_draws.push_back({first, 6, additive, depth_off});

  // 5) standard alpha blend, depth tested but not written: 0.5*red over gray
  MetalPsoKey alpha_blend = opaque;
  alpha_blend.blend_enable = true;
  alpha_blend.blend_src_rgb = MTLBlendFactorSourceAlpha;
  alpha_blend.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
  alpha_blend.blend_src_alpha = MTLBlendFactorOne;
  alpha_blend.blend_dst_alpha = MTLBlendFactorZero;
  first = add_quad(verts, 0.2f, 0.2f, 0.6f, 0.6f, 0.6f, 1.f, 0.f, 0.f, 0.5f, 0.f);
  m_scene_draws.push_back({first, 6, alpha_blend, depth_ro});

  // 6) reverse subtract (dst - src): gray - 0.25
  MetalPsoKey rev_sub = opaque;
  rev_sub.blend_enable = true;
  rev_sub.blend_op_rgb = MTLBlendOperationReverseSubtract;
  rev_sub.blend_src_rgb = rev_sub.blend_dst_rgb = MTLBlendFactorOne;
  rev_sub.blend_src_alpha = MTLBlendFactorOne;
  rev_sub.blend_dst_alpha = MTLBlendFactorZero;
  first = add_quad(verts, -0.6f, -0.6f, -0.2f, -0.2f, 0.6f, 0.25f, 0.25f, 0.25f, 1.f, 0.f);
  m_scene_draws.push_back({first, 6, rev_sub, depth_off});

  // 7) color write mask: write white to the red channel only over gray
  MetalPsoKey red_only = opaque;
  red_only.color_write_mask = MTLColorWriteMaskRed;
  first = add_quad(verts, -0.1f, -0.1f, 0.1f, 0.1f, 0.6f, 1.f, 1.f, 1.f, 1.f, 0.f);
  m_scene_draws.push_back({first, 6, red_only, depth_off});

  m_scene_vertices = [m_device newBufferWithBytes:verts.data()
                                           length:verts.size() * sizeof(ScaffoldVertex)
                                          options:MTLResourceStorageModeShared];
}

/*!
 * Metal analog of OpenGLRenderer::setup_frame: make sure the offscreen game
 * render target matches the requested internal resolution.
 */
void MetalRenderer::setup_frame(const MetalRenderOptions& opts) {
  if (!m_game_color || (int)m_game_color.width != opts.game_res_w ||
      (int)m_game_color.height != opts.game_res_h) {
    lg::info("Metal game target setup: {}x{}", opts.game_res_w, opts.game_res_h);
    m_game_color = make_color_target(m_device, opts.game_res_w, opts.game_res_h, true);
    m_game_depth = make_depth_target(m_device, opts.game_res_w, opts.game_res_h);
    m_game_target_fresh = true;
  }
}

void MetalRenderer::encode_game_passes(id<MTLCommandBuffer> cmds,
                                       id<MTLTexture> color,
                                       id<MTLTexture> depth) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  // Jak 1 clears the game framebuffer to transparent black and depth to 0
  // (OpenGLRenderer::setup_frame)
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  pass.depthAttachment.clearDepth = 0.0;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
  pass.stencilAttachment.clearStencil = 0;

  id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
  [enc setCullMode:MTLCullModeNone];
  [enc setVertexBuffer:m_scene_vertices offset:0 atIndex:0];
  [enc setFragmentTexture:m_checker_texture atIndex:0];
  for (auto& draw : m_scene_draws) {
    MetalPsoKey key = draw.pso;
    key.color_format = kColorFormat;
    key.depth_format = kDepthFormat;
    [enc setRenderPipelineState:m_pso_cache.get_pipeline(key)];
    [enc setDepthStencilState:m_pso_cache.get_depth_stencil(draw.depth)];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:draw.first_vertex
            vertexCount:draw.vertex_count];
  }
  [enc endEncoding];
}

/*!
 * Metal analog of OpenGLRenderer::do_pcrtc_effects: clear the window to black,
 * then draw the offscreen game frame into the centered draw region with the
 * post-processing brightness/contrast math, and the pmode-alp blackout on top.
 */
void MetalRenderer::encode_present_pass(id<MTLCommandBuffer> cmds,
                                        id<MTLTexture> target,
                                        const MetalRenderOptions& opts,
                                        id<MTLTexture> source) {
  int target_w = (int)target.width;
  int target_h = (int)target.height;
  int region_w = opts.draw_region_w;
  int region_h = opts.draw_region_h;
  if (region_w <= 0 || region_h <= 0) {
    // trying to draw to 0 size region (same fallback as the GL renderer)
    region_w = 320;
    region_h = 240;
  }
  region_w = std::min(region_w, target_w);
  region_h = std::min(region_h, target_h);
  // center the letterbox
  int offset_x = (target_w - region_w) / 2;
  int offset_y = (target_h - region_h) / 2;

  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
  [enc setViewport:{(double)offset_x, (double)offset_y, (double)region_w, (double)region_h, 0.0,
                    1.0}];

  MetalPsoKey present_key;
  present_key.shader = MetalShaderId::PRESENT;
  present_key.color_format = (u32)target.pixelFormat;
  [enc setRenderPipelineState:m_pso_cache.get_pipeline(present_key)];
  PresentParams params =
      make_present_params(opts.brightness_contrast_color, opts.brightness_contrast_alpha);
  [enc setFragmentBytes:&params length:sizeof(params) atIndex:0];
  [enc setFragmentTexture:source atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

  if (opts.pmode_alp < 1.f) {
    // blackout: alpha-blended black quad over the draw region
    // (OpenGLRenderer::do_pcrtc_effects / BlackoutRenderer)
    MetalPsoKey blackout_key;
    blackout_key.shader = MetalShaderId::SCAFFOLD;
    blackout_key.color_format = (u32)target.pixelFormat;
    blackout_key.blend_enable = true;
    blackout_key.blend_src_rgb = MTLBlendFactorSourceAlpha;
    blackout_key.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
    blackout_key.blend_src_alpha = MTLBlendFactorOne;
    blackout_key.blend_dst_alpha = MTLBlendFactorZero;
    [enc setRenderPipelineState:m_pso_cache.get_pipeline(blackout_key)];

    std::vector<ScaffoldVertex> quad;
    add_quad(quad, -1.f, -1.f, 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f - opts.pmode_alp, 0.f);
    [enc setVertexBytes:quad.data() length:quad.size() * sizeof(ScaffoldVertex) atIndex:0];
    [enc setFragmentTexture:m_checker_texture atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
  }
  [enc endEncoding];
}

void MetalRenderer::render_frame(const MetalRenderOptions& opts, CAMetalLayer* layer) {
  @autoreleasepool {
    id<MTLCommandBuffer> cmds = [m_queue commandBuffer];

    const bool jak2_fallback = m_shared_state.version == GameVersion::Jak2;
    id<MTLTexture> color = nil;
    id<MTLTexture> depth = nil;
    if (jak2_fallback) {
      if (!m_jak2_fallback_color || (int)m_jak2_fallback_color.width != opts.game_res_w ||
          (int)m_jak2_fallback_color.height != opts.game_res_h) {
        m_jak2_fallback_color =
            make_color_target(m_device, opts.game_res_w, opts.game_res_h, true);
        m_jak2_fallback_depth = make_depth_target(m_device, opts.game_res_w, opts.game_res_h);
      }
      color = m_jak2_fallback_color;
      depth = m_jak2_fallback_depth;
    } else {
      setup_frame(opts);
      color = m_game_color;
      depth = m_game_depth;
    }
    encode_game_passes(cmds, color, depth);
#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:color];
      [blit endEncoding];
    }
#endif

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable) {
      encode_present_pass(cmds, drawable.texture, opts, color);
      schedule_present(cmds, drawable, opts);
    }

    [cmds commit];
    if (!jak2_fallback) {
      m_game_target_fresh = false;
    }
    {
      std::lock_guard<std::mutex> lock(m_frame_mutex);
      m_last_frame_cmds = cmds;
      m_frame_count++;
    }
  }
}

/*!
 * Walk the frame's DMA chain and dispatch each bucket. Mirror of
 * OpenGLRenderer::dispatch_buckets_jak1: the chain starts with a CALL to the
 * common default-registers chain (which carries the fog color), then the
 * bucket array follows.
 */
void MetalRenderer::dispatch_buckets_jak1(DmaFollower dma, MetalFrameContext& ctx) {
  m_shared_state.buckets_base = dma.current_tag_offset() + 16;  // 1 qw for the initial call
  m_shared_state.next_bucket = m_shared_state.buckets_base;

  // find the default regs buffer
  auto initial_call_tag = dma.current_tag();
  ASSERT(initial_call_tag.kind == DmaTag::Kind::CALL);
  auto initial_call_default_regs = dma.read_and_advance();
  ASSERT(initial_call_default_regs.transferred_tag == 0);  // should be a nop
  m_shared_state.default_regs_buffer = dma.current_tag_offset();
  auto default_regs_tag = dma.current_tag();
  ASSERT(default_regs_tag.kind == DmaTag::Kind::CNT);
  ASSERT(default_regs_tag.qwc == 10);
  auto default_data = dma.read_and_advance();
  ASSERT(default_data.size_bytes > 148);
  memcpy(m_shared_state.fog_color.data(), default_data.data + 144, 4);
  auto default_ret_tag = dma.current_tag();
  ASSERT(default_ret_tag.qwc == 0);
  ASSERT(default_ret_tag.kind == DmaTag::Kind::RET);
  dma.read_and_advance();

  // now we should point to the first bucket!
  ASSERT(dma.current_tag_offset() == m_shared_state.next_bucket);
  m_shared_state.next_bucket += 16;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    renderer->render(dma, &m_shared_state, ctx);
    // should have ended at the start of the next bucket
    ASSERT(dma.current_tag_offset() == m_shared_state.next_bucket);
    m_shared_state.next_bucket += 16;
    m_chain_stats.last_buckets_dispatched++;
    metal_vif_interrupt_callback((int)bucket_id);
  }
}

void MetalRenderer::dispatch_buckets_jak2(DmaFollower dma, MetalFrameContext& ctx) {
  m_shared_state.buckets_base = dma.current_tag_offset();
  m_shared_state.next_bucket = m_shared_state.buckets_base + 16;
  m_shared_state.default_regs_buffer = 0;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    renderer->render(dma, &m_shared_state, ctx);
    ASSERT(dma.current_tag_offset() == m_shared_state.next_bucket);
    m_shared_state.next_bucket += 16;
    m_chain_stats.last_buckets_dispatched++;
    metal_vif_interrupt_callback((int)bucket_id + 1);
  }
  metal_vif_interrupt_callback((int)m_bucket_renderers.size());
}

bool MetalRenderer::render_chain_frame(const MetalRenderOptions& opts,
                                       CAMetalLayer* layer,
                                       const u8* chain_data,
                                       u32 chain_offset,
                                       std::size_t chain_size) {
  if (m_shared_state.version == GameVersion::Jak2) {
    const auto validation =
        metal_renderer::validate_jak2_metal_dma_chain(chain_data, chain_size, chain_offset);
    if (!validation) {
      if (validation.error == metal_renderer::Jak2MetalChainValidationError::DmaChain) {
        throw std::runtime_error(fmt::format(
            "Jak 2 Metal DMA chain validation failed at {:#x}: {}",
            validation.dma.error_offset,
            dma_chain_validation_error_message(validation.dma.error)));
      }
      throw std::runtime_error(fmt::format(
          "Jak 2 Metal DMA chain validation failed before bucket {}: {}",
          validation.failed_bucket + 1,
          metal_renderer::jak2_metal_chain_validation_error_message(validation.error)));
    }
  } else {
    const auto validation = validate_dma_chain(chain_data, chain_size, chain_offset);
    if (!validation) {
      throw std::runtime_error(fmt::format("Metal DMA chain validation failed at {:#x}: {}",
                                           validation.error_offset,
                                           dma_chain_validation_error_message(validation.error)));
    }
  }
  bool drawable_acquired = false;
  @autoreleasepool {
    ASSERT_MSG(!m_bucket_renderers.empty(), "init_bucket_renderers was not called");
    // the stream buffer pages are reused in place, so the previous frame's GPU
    // work must be done with them (correctness first; pipelining is a later,
    // measured change)
    id<MTLCommandBuffer> prev;
    {
      std::lock_guard<std::mutex> lock(m_frame_mutex);
      prev = m_last_frame_cmds;
    }
    if (prev) {
      [prev waitUntilCompleted];
    }
    m_stream.reset();

    setup_frame(opts);
    // mirror of SharedRenderState::reset for the background state
    m_background.reset_frame();
    m_background.camera_trace.reset(opts.expected_camera_valid ? &opts.expected_camera : nullptr);
    m_background.render_camera_trace.reset(
        opts.expected_render_camera_valid ? &opts.expected_render_camera : nullptr);
    if (m_shared_state.eye_renderer) {
      m_shared_state.eye_renderer->start_frame();
    }
    m_shared_state.background = &m_background;
    m_shared_state.texture_pool = m_texture_pool;
    m_shared_state.dma_copy_base = chain_data;
    m_shared_state.dma_copy_size = chain_size;
    m_shared_state.ee_memory = g_ee_main_mem;
    m_shared_state.offset_of_s7 = g_s7_override ? g_s7_override : metal_offset_of_s7();
    m_shared_state.engine_frame_id = opts.engine_frame_id;
    m_shared_state.game_res_w = opts.game_res_w;
    m_shared_state.game_res_h = opts.game_res_h;
    m_shared_state.animated_texture_slots = opts.animated_texture_slots;
    m_shared_state.animated_texture_slot_count = opts.animated_texture_slot_count;
    m_shared_state.host_bucket_context = opts.host_bucket_context;
    m_shared_state.host_bucket_callback = opts.host_bucket_callback;
    struct HostBucketCallbackScope {
      MetalSharedRenderState* state;
      ~HostBucketCallbackScope() {
        state->animated_texture_slots = nullptr;
        state->animated_texture_slot_count = 0;
        state->host_bucket_context = nullptr;
        state->host_bucket_callback = nullptr;
      }
    } host_bucket_callback_scope{&m_shared_state};

    id<MTLCommandBuffer> cmds = [m_queue commandBuffer];

    // Start the game-target pass. Jak 2 retains color across submitted frames
    // until bucket 3 snapshots it and restarts with a clear; Jak 1 and newly
    // allocated targets keep the original frame-start clear.
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = m_game_color;
    pass.colorAttachments[0].loadAction =
        m_shared_state.version == GameVersion::Jak2 && !m_game_target_fresh ? MTLLoadActionLoad
                                                                           : MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    pass.depthAttachment.texture = m_game_depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    // stored, not discarded: the sprite distorter may split the pass and
    // reload depth/stencil (MetalFrameContext::resume_pass_with_framebuffer_copy)
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = 0.0;
    pass.stencilAttachment.texture = m_game_depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
    [enc setCullMode:MTLCullModeNone];

    MetalFrameContext ctx;
    ctx.enc = enc;
    ctx.pso_cache = &m_pso_cache;
    ctx.sampler_cache = &m_sampler_cache;
    ctx.stream = &m_stream;
    ctx.color_format = kColorFormat;
    ctx.depth_format = kDepthFormat;
    ctx.cmds = cmds;
    ctx.game_color = m_game_color;
    ctx.game_depth = m_game_depth;

    m_chain_stats.last_buckets_dispatched = 0;
    switch (m_shared_state.version) {
      case GameVersion::Jak1:
        dispatch_buckets_jak1(DmaFollower(chain_data, chain_offset, chain_size), ctx);
        break;
      case GameVersion::Jak2:
        dispatch_buckets_jak2(DmaFollower(chain_data, chain_offset, chain_size), ctx);
        break;
      default:
        ASSERT_MSG(false, "Metal DMA dispatch only supports Jak 1 and Jak 2");
    }
    if (m_shared_state.version == GameVersion::Jak2) {
      ASSERT(m_jak2_blit_display);
      // OpenGLRenderer::render calls BlitDisplays::do_copy_back only after
      // dispatch_buckets, so opcode 0x11 intentionally restores over every
      // later bucket before the frame is presented.
      m_jak2_blit_display->finish_frame(ctx);
    }
    [ctx.enc endEncoding];

    m_chain_stats.last_host_tick_id = opts.host_tick_id;
    m_chain_stats.last_chain_ordinal = opts.chain_ordinal;
    m_chain_stats.last_engine_frame_id = opts.engine_frame_id;
    m_chain_stats.last_camera_fingerprint =
        m_background.camera_trace.first_packet_fingerprint();
    m_chain_stats.last_camera_packets = m_background.camera_trace.packet_count();
    m_chain_stats.last_live_camera_mismatches =
        m_background.camera_trace.expected_mismatches();
    m_chain_stats.last_packet_camera_mismatches =
        m_background.camera_trace.packet_mismatches();
    m_chain_stats.last_live_camera_mismatch_qwords =
        m_background.camera_trace.expected_mismatch_qwords();
    m_chain_stats.last_packet_camera_mismatch_qwords =
        m_background.camera_trace.packet_mismatch_qwords();
    m_chain_stats.live_camera_mismatches += m_chain_stats.last_live_camera_mismatches;
    m_chain_stats.packet_camera_mismatches += m_chain_stats.last_packet_camera_mismatches;
    m_chain_stats.last_render_camera_fingerprint =
        m_background.render_camera_trace.first_packet_fingerprint();
    m_chain_stats.last_render_camera_live_mismatches =
        m_background.render_camera_trace.expected_mismatches();
    m_chain_stats.last_render_camera_packet_mismatches =
        m_background.render_camera_trace.packet_mismatches();
    m_chain_stats.last_render_camera_live_mismatch_qwords =
        m_background.render_camera_trace.expected_mismatch_qwords();
    m_chain_stats.last_render_camera_packet_mismatch_qwords =
        m_background.render_camera_trace.packet_mismatch_qwords();
    m_chain_stats.render_camera_live_mismatches +=
        m_chain_stats.last_render_camera_live_mismatches;
    m_chain_stats.render_camera_packet_mismatches +=
        m_chain_stats.last_render_camera_packet_mismatches;

    metal_camera_trace::RenderAlternationObservation render_camera;
    bool report_render_camera_alternation = false;
    metal_camera_trace::ProducerAlternationObservation producer_camera;
    bool report_producer_alternation = false;
    if (opts.expected_camera_valid && m_presentation_state) {
      std::lock_guard<std::mutex> lock(m_presentation_state->mutex);
      if (m_background.render_camera_trace.has_packet()) {
        render_camera = m_presentation_state->render_camera.observe(
            opts.engine_frame_id, m_background.render_camera_trace.first_packet());
        m_chain_stats.render_camera_alternations =
            m_presentation_state->render_camera.alternations();
        metal_camera_trace::retain_alternation_observation(
            render_camera, opts.host_tick_id, opts.chain_ordinal,
            m_chain_stats.last_render_camera_fingerprint,
            m_chain_stats.last_render_camera_live_mismatch_qwords,
            m_chain_stats.last_render_camera_packet_mismatch_qwords,
            m_chain_stats.last_render_camera_alternation);
        if (render_camera.alternation &&
            !m_presentation_state->render_alternation_reported) {
          m_presentation_state->render_alternation_reported = true;
          report_render_camera_alternation = true;
        }
      }
      producer_camera =
          m_presentation_state->producer_camera.observe(opts.engine_frame_id, opts.expected_camera);
      m_chain_stats.producer_camera_alternations =
          m_presentation_state->producer_camera.alternations();
      metal_camera_trace::latch_render_mismatch_qwords_on_alternation(
          producer_camera.alternation, m_chain_stats.last_render_camera_live_mismatch_qwords,
          m_chain_stats.last_render_camera_packet_mismatch_qwords,
          m_chain_stats.last_camera_alternation_render_live_mismatch_qwords,
          m_chain_stats.last_camera_alternation_render_packet_mismatch_qwords);
      if (producer_camera.alternation) {
        m_chain_stats.last_camera_alternation_older_frame_id = producer_camera.older_frame_id;
        m_chain_stats.last_camera_alternation_previous_frame_id =
            producer_camera.previous_frame_id;
        m_chain_stats.last_camera_alternation_current_frame_id = producer_camera.current_frame_id;
        m_chain_stats.last_camera_alternation_older_fingerprint =
            producer_camera.older_fingerprint;
        m_chain_stats.last_camera_alternation_previous_fingerprint =
            producer_camera.previous_fingerprint;
        m_chain_stats.last_camera_alternation_current_fingerprint =
            producer_camera.current_fingerprint;
        m_chain_stats.last_camera_alternation_host_tick_id = opts.host_tick_id;
        m_chain_stats.last_camera_alternation_chain_ordinal = opts.chain_ordinal;
        m_chain_stats.last_camera_alternation_packet_fingerprint =
            m_chain_stats.last_camera_fingerprint;
        m_chain_stats.last_camera_alternation_live_mismatch_qwords =
            m_chain_stats.last_live_camera_mismatch_qwords;
        m_chain_stats.last_camera_alternation_packet_mismatch_qwords =
            m_chain_stats.last_packet_camera_mismatch_qwords;
        m_chain_stats.last_camera_older_to_previous_distance =
            producer_camera.older_to_previous_distance;
        m_chain_stats.last_camera_previous_to_current_distance =
            producer_camera.previous_to_current_distance;
        m_chain_stats.last_camera_older_to_current_distance =
            producer_camera.older_to_current_distance;
      }
      if (producer_camera.alternation &&
          !m_presentation_state->producer_alternation_reported) {
        m_presentation_state->producer_alternation_reported = true;
        report_producer_alternation = true;
      }
    }
    if (report_render_camera_alternation) {
      lg::error(
          "Metal render-camera return pattern at engine frames {}/{}/{}: normalized distances "
          "{:.9g}/{:.9g}/{:.9g}, full view/projection fingerprints {:#x}/{:#x}/{:#x}",
          render_camera.older_frame_id, render_camera.previous_frame_id,
          render_camera.current_frame_id, render_camera.older_to_previous_distance,
          render_camera.previous_to_current_distance, render_camera.older_to_current_distance,
          render_camera.older_fingerprint, render_camera.previous_fingerprint,
          render_camera.current_fingerprint);
    }
    if (report_producer_alternation) {
      lg::error(
          "Metal camera producer return pattern at engine frames {}/{}/{}, host tick {}, chain {}: "
          "normalized distances {:.9g}/{:.9g}/{:.9g}, fingerprints {:#x}/{:#x}/{:#x}",
          producer_camera.older_frame_id, producer_camera.previous_frame_id,
          producer_camera.current_frame_id, opts.host_tick_id, opts.chain_ordinal,
          producer_camera.older_to_previous_distance,
          producer_camera.previous_to_current_distance,
          producer_camera.older_to_current_distance, producer_camera.older_fingerprint,
          producer_camera.previous_fingerprint, producer_camera.current_fingerprint);
    }
    if (!m_reported_camera_mismatch &&
        (m_chain_stats.last_live_camera_mismatches ||
         m_chain_stats.last_packet_camera_mismatches ||
         m_chain_stats.last_render_camera_live_mismatches ||
         m_chain_stats.last_render_camera_packet_mismatches)) {
      m_reported_camera_mismatch = true;
      lg::error(
          "Metal camera provenance mismatch at engine frame {}, host tick {}, chain {} in '{}': "
          "{} live mismatches (qwords {:#x}), {} producer-subset packet mismatches (qwords "
          "{:#x}), {} full live view/projection mismatches (qwords {:#x}), {} full "
          "view/projection packet mismatches (qwords {:#x})",
          opts.engine_frame_id, opts.host_tick_id, opts.chain_ordinal,
          m_background.first_camera_mismatch_bucket,
          m_chain_stats.last_live_camera_mismatches,
          m_chain_stats.last_live_camera_mismatch_qwords,
          m_chain_stats.last_packet_camera_mismatches,
          m_chain_stats.last_packet_camera_mismatch_qwords,
          m_chain_stats.last_render_camera_live_mismatches,
          m_chain_stats.last_render_camera_live_mismatch_qwords,
          m_chain_stats.last_render_camera_packet_mismatches,
          m_chain_stats.last_render_camera_packet_mismatch_qwords);
    }

#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:m_game_color];
      [blit endEncoding];
    }
#endif

    id<CAMetalDrawable> drawable = layer ? [layer nextDrawable] : nil;
    if (drawable) {
      drawable_acquired = true;
      m_chain_stats.drawables_acquired++;
      encode_present_pass(cmds, drawable.texture, opts, m_game_color);
      if (opts.presentation_time > 0.0 && opts.presentation_time <= CACurrentMediaTime()) {
        m_chain_stats.late_present_submissions++;
      }
      const u64 submission_id = ++m_submission_count;
#if TARGET_OS_SIMULATOR
      // The simulator SDK omits MTLDrawable's presentation callback, time, and
      // ID APIs. Keep submission provenance available there; physical-device
      // builds retain the actual presentation-order trace below.
      const u64 drawable_id = 0;
#else
      const u64 drawable_id = static_cast<u64>(drawable.drawableID);
#endif
      m_chain_stats.submissions = submission_id;
      m_chain_stats.last_submission_id = submission_id;
      m_chain_stats.last_drawable_id = drawable_id;
      m_chain_stats.last_requested_presentation_time = opts.presentation_time;
#if !TARGET_OS_SIMULATOR
      const u64 engine_frame_id = opts.engine_frame_id;
      const u64 host_tick_id = opts.host_tick_id;
      const u64 chain_ordinal = opts.chain_ordinal;
      const auto presentation_state = m_presentation_state;
      [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
        const double presented_time = presented.presentedTime;
        const u64 presented_drawable_id = static_cast<u64>(presented.drawableID);
        std::lock_guard<std::mutex> lock(presentation_state->mutex);
        if (!presentation_state->order.observe(submission_id, presented_time)) {
          if (!presentation_state->mismatch_reported) {
            presentation_state->mismatch_reported = true;
            lg::error(
                "Metal presentation order mismatch: submission {} drawable {} at {:.6f} "
                "disagrees with retained successful presentation history",
                submission_id, presented_drawable_id, presented_time);
          }
        }
        if (presented_time > 0.0 &&
            presentation_state->order.last_submission_id() == submission_id) {
          presentation_state->last_drawable_id = presented_drawable_id;
          presentation_state->last_engine_frame_id = engine_frame_id;
          presentation_state->last_host_tick_id = host_tick_id;
          presentation_state->last_chain_ordinal = chain_ordinal;
        }
        presentation_state->presentation_cv.notify_all();
      }];
#endif
      schedule_present(cmds, drawable, opts);
    } else if (layer) {
      m_chain_stats.drawable_misses++;
    }

    if (layer) {
      const auto completion_state = m_presentation_state;
      [cmds addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const MTLCommandBufferStatus status = completed.status;
        const s64 error_code = completed.error ? completed.error.code : 0;
        bool report_error = false;
        {
          std::lock_guard<std::mutex> lock(completion_state->mutex);
          completion_state->last_command_buffer_status = static_cast<int>(status);
          completion_state->last_command_buffer_error_code = error_code;
          if (status == MTLCommandBufferStatusCompleted) {
            completion_state->command_buffers_completed++;
          } else {
            completion_state->command_buffer_errors++;
            if (!completion_state->command_buffer_error_reported) {
              completion_state->command_buffer_error_reported = true;
              report_error = true;
            }
          }
        }
        if (report_error) {
          lg::error("Metal command buffer failed with status {} and error code {}",
                    static_cast<int>(status), error_code);
        }
        completion_state->command_buffer_cv.notify_all();
      }];
      [cmds commit];
      m_game_target_fresh = false;
      m_chain_stats.command_buffers_committed++;
      {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        m_last_frame_cmds = cmds;
        m_frame_count++;
      }
    }

    // frame stats for tests / debugging
    m_chain_stats.chains_rendered++;
    m_chain_stats.draw_calls = ctx.draw_calls;
    m_chain_stats.triangles = ctx.triangles;
    m_chain_stats.jak2_sky_draw_draws = 0;
    m_chain_stats.jak2_sky_draw_triangles = 0;
    m_chain_stats.jak2_sky_draw_last_batch = {};
    m_chain_stats.jak2_blit_display_plan_valid = false;
    m_chain_stats.jak2_blit_display_snapshot_requested = false;
    m_chain_stats.jak2_blit_display_copy_back_requested = false;
    m_chain_stats.jak2_blit_display_copy_back_performed = false;
    m_chain_stats.jak2_blit_display_texture_lookup_hit = false;
    m_chain_stats.jak2_blit_display_used_placeholder = false;
    m_chain_stats.jak2_blit_display_texture_handle = 0;
    m_chain_stats.jak2_blit_display_texture_tbp = 0;
    m_chain_stats.jak2_blit_display_unsupported_pc_ports = 0;
    m_chain_stats.jak2_screen_filter_draws = 0;
    m_chain_stats.jak2_screen_filter_triangles = 0;
    m_chain_stats.jak2_progress_draws = 0;
    m_chain_stats.jak2_progress_triangles = 0;
    m_chain_stats.jak2_progress_textured_draws = 0;
    m_chain_stats.jak2_progress_missing_texture_draws = 0;
    m_chain_stats.jak2_debug_no_zbuf1_draws = 0;
    m_chain_stats.jak2_debug_no_zbuf1_triangles = 0;
    m_chain_stats.jak2_debug_no_zbuf1_textured_draws = 0;
    m_chain_stats.jak2_debug_no_zbuf1_missing_texture_draws = 0;
    m_chain_stats.jak2_debug_no_zbuf2_draws = 0;
    m_chain_stats.jak2_debug_no_zbuf2_triangles = 0;
    int uploads = 0;
    u64 skipped = 0;
    m_chain_stats.last_skipped_bucket_count = 0;
    m_chain_stats.last_skipped_bucket_ids.fill(0);
    m_chain_stats.last_skipped_bucket_bytes.fill(0);
    const auto track_skipped_bucket = [this](u32 bucket_id, u64 bytes) {
      if (bytes == 0) {
        return;
      }
      std::size_t insert_at = 0;
      while (insert_at < metal_renderer::kTrackedDeferredBuckets &&
             m_chain_stats.last_skipped_bucket_bytes[insert_at] >= bytes) {
        insert_at++;
      }
      if (insert_at == metal_renderer::kTrackedDeferredBuckets) {
        return;
      }
      for (std::size_t i = metal_renderer::kTrackedDeferredBuckets - 1; i > insert_at; i--) {
        m_chain_stats.last_skipped_bucket_ids[i] =
            m_chain_stats.last_skipped_bucket_ids[i - 1];
        m_chain_stats.last_skipped_bucket_bytes[i] =
            m_chain_stats.last_skipped_bucket_bytes[i - 1];
      }
      m_chain_stats.last_skipped_bucket_ids[insert_at] = bucket_id;
      m_chain_stats.last_skipped_bucket_bytes[insert_at] = bytes;
      m_chain_stats.last_skipped_bucket_count = std::min<int>(
          m_chain_stats.last_skipped_bucket_count + 1,
          static_cast<int>(metal_renderer::kTrackedDeferredBuckets));
    };
    int unsupported_blends = 0;
    m_chain_stats.ocean_draws = 0;
    m_chain_stats.ocean_triangles = 0;
    m_chain_stats.ocean_missing_textures = 0;
    MetalMerc2::Stats merc_stats;
    MetalGeneric2::Stats generic_stats;
    if (m_shared_state.version == GameVersion::Jak2 && m_jak2_blit_display) {
      const auto& stats = m_jak2_blit_display->stats();
      m_chain_stats.jak2_blit_display_plan_valid = stats.plan_valid;
      m_chain_stats.jak2_blit_display_snapshot_requested = stats.snapshot_requested;
      m_chain_stats.jak2_blit_display_copy_back_requested = stats.copy_back_requested;
      m_chain_stats.jak2_blit_display_copy_back_performed = stats.copy_back_performed;
      m_chain_stats.jak2_blit_display_texture_lookup_hit = stats.texture_lookup_hit;
      m_chain_stats.jak2_blit_display_used_placeholder = stats.used_placeholder;
      m_chain_stats.jak2_blit_display_texture_handle = stats.texture_handle;
      m_chain_stats.jak2_blit_display_texture_tbp = stats.texture_tbp;
      m_chain_stats.jak2_blit_display_unsupported_pc_ports =
          stats.unsupported_pc_port_count;
    }
    for (std::size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
      auto& r = m_bucket_renderers[bucket_id];
      if (auto* t = dynamic_cast<MetalTextureBucketRenderer*>(r.get())) {
        uploads += t->last_stats().uploads;
      } else if (auto* s = dynamic_cast<MetalSkipRenderer*>(r.get())) {
        skipped += s->skipped_bytes();
        track_skipped_bucket(static_cast<u32>(bucket_id), s->last_skipped_bytes());
      } else if (auto* d = dynamic_cast<MetalDirectRenderer*>(r.get())) {
        unsupported_blends += d->stats().unsupported_blends;
        if (m_shared_state.version == GameVersion::Jak2 &&
            bucket_id == static_cast<std::size_t>(jak2::BucketId::SKY_DRAW)) {
          const auto& stats = d->stats();
          const auto& batch = stats.last_batch;
          auto& sky_batch = m_chain_stats.jak2_sky_draw_last_batch;
          m_chain_stats.jak2_sky_draw_draws = stats.draw_calls;
          m_chain_stats.jak2_sky_draw_triangles = stats.triangles;
          sky_batch.valid = batch.valid;
          sky_batch.textured = batch.textured;
          sky_batch.vertices = batch.vertices;
          sky_batch.nonzero_rgb_vertices = batch.nonzero_rgb_vertices;
          sky_batch.tex0_tbp = batch.tex0_tbp;
          sky_batch.tex0_tcc = batch.tex0_tcc;
          sky_batch.tex0_decal = batch.tex0_decal;
          sky_batch.texture_lookup_hit = batch.texture_lookup_hit;
          sky_batch.used_placeholder = batch.used_placeholder;
          sky_batch.write_rgb = batch.write_rgb;
          sky_batch.blend_enabled = batch.blend_enabled;
          sky_batch.blend_a = batch.blend_a;
          sky_batch.blend_b = batch.blend_b;
          sky_batch.blend_c = batch.blend_c;
          sky_batch.blend_d = batch.blend_d;
          sky_batch.alpha_test_enabled = batch.alpha_test_enabled;
          sky_batch.alpha_test_mode = batch.alpha_test_mode;
          sky_batch.alpha_aref = batch.alpha_aref;
          sky_batch.alpha_afail = batch.alpha_afail;
        } else if (m_shared_state.version == GameVersion::Jak2 &&
                   bucket_id == static_cast<std::size_t>(jak2::BucketId::PROGRESS)) {
          m_chain_stats.jak2_progress_draws = d->stats().draw_calls;
          m_chain_stats.jak2_progress_triangles = d->stats().triangles;
          m_chain_stats.jak2_progress_textured_draws = d->stats().textured_draw_calls;
          m_chain_stats.jak2_progress_missing_texture_draws =
              d->stats().missing_texture_draw_calls;
        } else if (m_shared_state.version == GameVersion::Jak2 &&
                   bucket_id == static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER)) {
          m_chain_stats.jak2_screen_filter_draws = d->stats().draw_calls;
          m_chain_stats.jak2_screen_filter_triangles = d->stats().triangles;
        } else if (m_shared_state.version == GameVersion::Jak2 &&
                   bucket_id == static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF1)) {
          m_chain_stats.jak2_debug_no_zbuf1_draws = d->stats().draw_calls;
          m_chain_stats.jak2_debug_no_zbuf1_triangles = d->stats().triangles;
          m_chain_stats.jak2_debug_no_zbuf1_textured_draws = d->stats().textured_draw_calls;
          m_chain_stats.jak2_debug_no_zbuf1_missing_texture_draws =
              d->stats().missing_texture_draw_calls;
        } else if (m_shared_state.version == GameVersion::Jak2 &&
                   bucket_id == static_cast<std::size_t>(jak2::BucketId::DEBUG_NO_ZBUF2)) {
          m_chain_stats.jak2_debug_no_zbuf2_draws = d->stats().draw_calls;
          m_chain_stats.jak2_debug_no_zbuf2_triangles = d->stats().triangles;
        }
      } else if (auto* sky = dynamic_cast<MetalSkyRenderer*>(r.get())) {
        unsupported_blends += sky->direct_stats().unsupported_blends;
      } else if (auto* omf = dynamic_cast<MetalOceanMidAndFar*>(r.get())) {
        m_chain_stats.ocean_texture_verts = omf->texture_stats().vertices;
        m_chain_stats.ocean_mid_verts = omf->mid_stats().vertices;
        m_chain_stats.ocean_draws += omf->texture_stats().draw_calls + omf->mid_stats().draw_calls;
        m_chain_stats.ocean_triangles +=
            omf->texture_stats().triangles + omf->mid_stats().triangles;
        m_chain_stats.ocean_missing_textures +=
            omf->texture_stats().missing_textures + omf->mid_stats().missing_textures;
        m_chain_stats.ocean_mid_texture = omf->texture_handle();
        unsupported_blends += omf->direct_stats().unsupported_blends;
      } else if (auto* on = dynamic_cast<MetalOceanNear*>(r.get())) {
        m_chain_stats.ocean_near_verts = on->near_stats().vertices;
        m_chain_stats.ocean_draws += on->texture_stats().draw_calls + on->near_stats().draw_calls;
        m_chain_stats.ocean_triangles +=
            on->texture_stats().triangles + on->near_stats().triangles;
        m_chain_stats.ocean_missing_textures +=
            on->texture_stats().missing_textures + on->near_stats().missing_textures;
        m_chain_stats.ocean_near_texture = on->texture_handle();
      } else if (auto* sp = dynamic_cast<MetalSpriteRenderer*>(r.get())) {
        const auto& ss = sp->stats();
        m_chain_stats.sprites_2d = ss.count_2d_grp0 - ss.sprites_3d;
        m_chain_stats.sprites_3d = ss.sprites_3d;
        m_chain_stats.sprites_hud = ss.count_2d_grp1;
        m_chain_stats.sprites_distort = ss.distort_sprites;
        m_chain_stats.sprite_normal_submitted = ss.normal_sprites_submitted;
        m_chain_stats.sprite_glow_marked = ss.glow_marked_sprites;
        m_chain_stats.sprite_glow_skipped = ss.glow_sprites_skipped;
        m_chain_stats.sprite_glow_parsed = ss.glow_sprites_parsed;
        m_chain_stats.sprite_glow_accepted = ss.glow_sprites_accepted;
        m_chain_stats.sprite_glow_rejected = ss.glow_sprites_rejected;
        m_chain_stats.sprite_glow_invalid_records = ss.glow_invalid_records;
        m_chain_stats.sprite_glow_force_visible_submitted = ss.glow_force_visible_submitted;
        m_chain_stats.sprite_glow_force_visible_drawn = ss.glow_force_visible_drawn;
        m_chain_stats.sprite_glow_force_visible_draws = ss.glow_force_visible_draw_calls;
        m_chain_stats.sprite_glow_force_visible_triangles = ss.glow_force_visible_triangles;
        m_chain_stats.sprite_glow_force_visible_missing_textures =
            ss.glow_force_visible_missing_textures;
        m_chain_stats.sprite_draws = ss.draw_calls;
        m_chain_stats.sprite_triangles = ss.triangles;
        m_chain_stats.sprite_missing_textures = ss.missing_textures;
        m_chain_stats.sprite_unsupported_bytes = ss.unsupported_bytes;
        skipped += sp->unsupported_bytes_total();
      } else if (auto* mc = dynamic_cast<MetalMercBucketRenderer*>(r.get())) {
        merc_stats.add(mc->stats());
      } else if (auto* sh = dynamic_cast<MetalShadowRenderer*>(r.get())) {
        const auto& ss = sh->stats();
        m_chain_stats.shadow_volumes = ss.volumes;
        m_chain_stats.shadow_vertices = ss.vertices;
        m_chain_stats.shadow_draws = ss.draw_calls;
        m_chain_stats.shadow_triangles = ss.triangles;
        m_chain_stats.shadow_unexpected_dma = ss.unexpected_dma;
      } else if (auto* gn = dynamic_cast<MetalGeneric2BucketRenderer*>(r.get())) {
        generic_stats.add(gn->stats());
      } else if (auto* ey = dynamic_cast<MetalEyeRenderer*>(r.get())) {
        const auto& es = ey->stats();
        m_chain_stats.eyes_composed = es.eyes;
        m_chain_stats.eye_draws = es.draw_calls;
        m_chain_stats.eye_triangles = es.triangles;
        m_chain_stats.eye_missing_textures = es.missing_textures;
        m_chain_stats.eye_unexpected_dma = es.unexpected_dma;
        m_chain_stats.eye_texture = es.first_texture;
      }
    }
    m_chain_stats.generic_fragments = generic_stats.fragments;
    m_chain_stats.generic_vertices = generic_stats.vertices;
    m_chain_stats.generic_adgifs = generic_stats.adgifs;
    m_chain_stats.generic_draw_buckets = generic_stats.draw_buckets;
    m_chain_stats.generic_draws = generic_stats.draw_calls;
    m_chain_stats.generic_triangles = generic_stats.triangles;
    m_chain_stats.generic_missing_textures = generic_stats.missing_textures;
    m_chain_stats.generic_unsupported_blends = generic_stats.unsupported_blends;
    m_chain_stats.generic_unexpected_dma = generic_stats.unexpected_dma;
    m_chain_stats.generic_overflow = generic_stats.overflow;
    m_chain_stats.merc_models = merc_stats.models;
    m_chain_stats.merc_missing_models = merc_stats.missing_models;
    m_chain_stats.merc_malformed_dma = merc_stats.malformed_dma;
    m_chain_stats.merc_draws = merc_stats.draws;
    m_chain_stats.merc_triangles = merc_stats.triangles;
    m_chain_stats.merc_envmap_draws = merc_stats.envmap_draws;
    m_chain_stats.merc_bone_vectors = merc_stats.bone_vectors;
    m_chain_stats.merc_mod_vtx_uploads = merc_stats.mod_vtx_uploads;
    m_chain_stats.merc_mod_vtx_skipped = merc_stats.mod_vtx_skipped;
    m_chain_stats.merc_eye_draws = merc_stats.eye_draws;
    m_chain_stats.merc_missing_textures = merc_stats.missing_textures;
    m_chain_stats.merc_bad_bone_pointers = merc_stats.bad_bone_pointers;
    m_chain_stats.merc_bad_draw_ranges = merc_stats.bad_draw_ranges;
    m_chain_stats.merc_missing_bone_slots = merc_stats.missing_bone_slots;
    m_chain_stats.merc_models_with_missing_bone_slots =
        merc_stats.models_with_missing_bone_slots;
    m_chain_stats.merc_nonfinite_bone_matrices = merc_stats.nonfinite_bone_matrices;
    m_chain_stats.merc_degenerate_bone_matrices = merc_stats.degenerate_bone_matrices;
    m_chain_stats.merc_incoherent_bone_sources = merc_stats.incoherent_bone_sources;
    m_chain_stats.merc_models_with_palette_health_issues =
        merc_stats.models_with_palette_health_issues;
    m_chain_stats.merc_eichar_palette_health_issues = merc_stats.eichar_palette_health_issues;
    m_chain_stats.merc_eichar_transform_discontinuities =
        merc_stats.eichar_transform_discontinuities;
    m_chain_stats.merc_eichar_provenance_events = merc_stats.eichar_provenance_events;
    m_chain_stats.merc_eichar_output_composition_mismatches =
        merc_stats.eichar_output_composition_mismatches;
    m_chain_stats.merc_eichar_target_control_events = merc_stats.eichar_target_control_events;
    m_chain_stats.merc_eichar_target_control_divergences =
        merc_stats.eichar_target_control_divergences;
    m_chain_stats.merc_eichar_target_control_attack_boundaries =
        merc_stats.eichar_target_control_attack_boundaries;
    m_chain_stats.merc_eichar_target_control_capture_attempts =
        merc_stats.eichar_target_control_capture_attempts;
    m_chain_stats.merc_eichar_target_control_valid_observations =
        merc_stats.eichar_target_control_valid_observations;
    m_chain_stats.merc_eichar_weighted_skin = merc_stats.eichar_weighted_skin;
    m_chain_stats.merc_eichar_duplication = merc_stats.eichar_duplication;
    if (merc_stats.eichar_target_control_capture_attempts > 0) {
      m_chain_stats.last_merc_eichar_target_control_capture_stage =
          merc_stats.last_eichar_target_control_capture_stage;
      m_chain_stats.last_merc_eichar_target_control_capture_result =
          merc_stats.last_eichar_target_control_capture_result;
    }
    if (merc_stats.eichar_target_control_valid_observations > 0) {
      m_chain_stats.last_merc_eichar_target_control_observation =
          merc_stats.last_eichar_target_control_observation;
    }
    if (!m_chain_stats.first_merc_palette_health_event.valid() &&
        merc_stats.first_palette_health_event.valid()) {
      m_chain_stats.first_merc_palette_health_event = merc_stats.first_palette_health_event;
    }
    if (merc_stats.last_palette_health_event.valid()) {
      m_chain_stats.last_merc_palette_health_event = merc_stats.last_palette_health_event;
    }
    if (!m_chain_stats.first_merc_eichar_palette_health_event.valid() &&
        merc_stats.first_eichar_palette_health_event.valid()) {
      m_chain_stats.first_merc_eichar_palette_health_event =
          merc_stats.first_eichar_palette_health_event;
    }
    if (merc_stats.last_eichar_palette_health_event.valid()) {
      m_chain_stats.last_merc_eichar_palette_health_event =
          merc_stats.last_eichar_palette_health_event;
    }
    if (!m_chain_stats.first_merc_eichar_transform_discontinuity.valid() &&
        merc_stats.first_eichar_transform_discontinuity.valid()) {
      m_chain_stats.first_merc_eichar_transform_discontinuity =
          merc_stats.first_eichar_transform_discontinuity;
    }
    if (merc_stats.last_eichar_transform_discontinuity.valid()) {
      m_chain_stats.last_merc_eichar_transform_discontinuity =
          merc_stats.last_eichar_transform_discontinuity;
    }
    if (!m_chain_stats.first_merc_eichar_provenance_event.valid() &&
        merc_stats.first_eichar_provenance_event.valid()) {
      m_chain_stats.first_merc_eichar_provenance_event =
          merc_stats.first_eichar_provenance_event;
    }
    if (merc_stats.last_eichar_provenance_event.valid()) {
      m_chain_stats.last_merc_eichar_provenance_event = merc_stats.last_eichar_provenance_event;
    }
    if (!m_chain_stats.first_merc_eichar_output_composition_mismatch.valid() &&
        merc_stats.first_eichar_output_composition_mismatch.valid()) {
      m_chain_stats.first_merc_eichar_output_composition_mismatch =
          merc_stats.first_eichar_output_composition_mismatch;
    }
    if (merc_stats.last_eichar_output_composition_mismatch.valid()) {
      m_chain_stats.last_merc_eichar_output_composition_mismatch =
          merc_stats.last_eichar_output_composition_mismatch;
    }
    if (!m_chain_stats.first_merc_eichar_target_control_event.valid() &&
        merc_stats.first_eichar_target_control_event.valid()) {
      m_chain_stats.first_merc_eichar_target_control_event =
          merc_stats.first_eichar_target_control_event;
    }
    if (merc_stats.last_eichar_target_control_event.valid()) {
      m_chain_stats.last_merc_eichar_target_control_event =
          merc_stats.last_eichar_target_control_event;
    }
    m_chain_stats.tex_uploads = uploads;
    m_chain_stats.skipped_bucket_bytes = skipped;
    m_chain_stats.direct_unsupported_blends = unsupported_blends;
    SkyBlendStats blend_stats;
    for (auto* handler : m_sky_blend_handlers) {
      if (handler) {
        blend_stats.sky_draws += handler->last_stats().sky_draws;
        blend_stats.sky_blends += handler->last_stats().sky_blends;
        blend_stats.cloud_draws += handler->last_stats().cloud_draws;
        blend_stats.cloud_blends += handler->last_stats().cloud_blends;
      }
    }
    m_chain_stats.sky_draws = blend_stats.sky_draws;
    m_chain_stats.sky_blends = blend_stats.sky_blends;
    m_chain_stats.cloud_draws = blend_stats.cloud_draws;
    m_chain_stats.cloud_blends = blend_stats.cloud_blends;
    m_chain_stats.skipped_tfrag_bytes = 0;
  }
  return drawable_acquired;
}

bool MetalRenderer::wait_for_last_chain_frame(double timeout_seconds) {
  if (!m_presentation_state || timeout_seconds <= 0.0) {
    return false;
  }
  const u64 expected_completions = m_chain_stats.command_buffers_committed;
  if (expected_completions == 0) {
    return false;
  }
  std::unique_lock<std::mutex> lock(m_presentation_state->mutex);
  const bool finished = m_presentation_state->command_buffer_cv.wait_for(
      lock, std::chrono::duration<double>(timeout_seconds), [&] {
        return m_presentation_state->command_buffers_completed +
                   m_presentation_state->command_buffer_errors >=
               expected_completions;
      });
  return finished && m_presentation_state->command_buffer_errors == 0;
}

bool MetalRenderer::wait_for_last_presentation(double timeout_seconds) {
  if (!m_presentation_state || timeout_seconds <= 0.0 || m_submission_count == 0) {
    return false;
  }
  const u64 expected_presentations = m_submission_count;
  std::unique_lock<std::mutex> lock(m_presentation_state->mutex);
  const bool finished = m_presentation_state->presentation_cv.wait_for(
      lock, std::chrono::duration<double>(timeout_seconds), [&] {
        return m_presentation_state->order.presentations() +
                   m_presentation_state->order.drops() >=
               expected_presentations;
      });
  return finished && m_presentation_state->order.presentations() == expected_presentations &&
         m_presentation_state->order.drops() == 0 &&
         m_presentation_state->order.mismatches() == 0;
}

metal_renderer::ChainStats MetalRenderer::chain_stats() {
  auto out = m_chain_stats;
  if (m_presentation_state) {
    std::lock_guard<std::mutex> lock(m_presentation_state->mutex);
    out.command_buffers_completed = m_presentation_state->command_buffers_completed;
    out.command_buffer_errors = m_presentation_state->command_buffer_errors;
    out.last_command_buffer_status = m_presentation_state->last_command_buffer_status;
    out.last_command_buffer_error_code =
        m_presentation_state->last_command_buffer_error_code;
    out.presentations_completed = m_presentation_state->order.presentations();
    out.presentation_drops = m_presentation_state->order.drops();
    out.presentation_order_mismatches = m_presentation_state->order.mismatches();
    out.last_presented_submission_id = m_presentation_state->order.last_submission_id();
    out.last_dropped_submission_id =
        m_presentation_state->order.last_dropped_submission_id();
    out.last_presented_drawable_id = m_presentation_state->last_drawable_id;
    out.last_presented_engine_frame_id = m_presentation_state->last_engine_frame_id;
    out.last_presented_host_tick_id = m_presentation_state->last_host_tick_id;
    out.last_presented_chain_ordinal = m_presentation_state->last_chain_ordinal;
    out.last_actual_presentation_time = m_presentation_state->order.last_presented_time();
  }
  return out;
}

bool MetalRenderer::read_color_target(id<MTLTexture> tex, metal_renderer::FramePixels* out) {
  const int w = (int)tex.width;
  const int h = (int)tex.height;
  std::vector<u8> bgra(w * h * 4);
  [tex getBytes:bgra.data()
      bytesPerRow:w * 4
       fromRegion:MTLRegionMake2D(0, 0, w, h)
      mipmapLevel:0];
  out->width = w;
  out->height = h;
  out->rgba.resize(bgra.size());
  for (size_t i = 0; i < bgra.size(); i += 4) {
    out->rgba[i + 0] = bgra[i + 2];
    out->rgba[i + 1] = bgra[i + 1];
    out->rgba[i + 2] = bgra[i + 0];
    out->rgba[i + 3] = bgra[i + 3];
  }
  return true;
}

bool MetalRenderer::read_game_frame(metal_renderer::FramePixels* out) {
  id<MTLCommandBuffer> cmds;
  {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (m_frame_count == 0) {
      return false;
    }
    cmds = m_last_frame_cmds;
  }
  [cmds waitUntilCompleted];
  return read_color_target(m_game_color, out);
}

bool MetalRenderer::read_present_frame(int window_w,
                                       int window_h,
                                       const MetalRenderOptions& opts,
                                       metal_renderer::FramePixels* out) {
  @autoreleasepool {
    {
      std::lock_guard<std::mutex> lock(m_frame_mutex);
      if (m_frame_count == 0) {
        return false;
      }
    }
    id<MTLTexture> target = make_color_target(m_device, window_w, window_h, false);
    id<MTLCommandBuffer> cmds = [m_queue commandBuffer];
    encode_present_pass(cmds, target, opts, m_game_color);
#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:target];
      [blit endEncoding];
    }
#endif
    [cmds commit];
    [cmds waitUntilCompleted];
    return read_color_target(target, out);
  }
}

bool MetalRenderer::read_texture_sample(const metal_renderer::TextureSampleSpec& spec,
                                        metal_renderer::FramePixels* out) {
  @autoreleasepool {
    id<MTLTexture> tex = metal_texture_lookup(spec.texture);
    if (!tex) {
      lg::error("read_texture_sample: unknown texture handle {}", spec.texture);
      return false;
    }

    // full-target quad with the requested uv range; row 0 of the readback is
    // the v0 edge (NDC y=+1)
    ScaffoldVertex verts[6];
    auto vert = [&](float x, float y, float u, float v) {
      ScaffoldVertex vx{};
      vx.pos[0] = x;
      vx.pos[1] = y;
      vx.uv[0] = u;
      vx.uv[1] = v;
      return vx;
    };
    verts[0] = vert(-1.f, 1.f, spec.u0, spec.v0);
    verts[1] = vert(1.f, 1.f, spec.u1, spec.v0);
    verts[2] = vert(1.f, -1.f, spec.u1, spec.v1);
    verts[3] = vert(-1.f, 1.f, spec.u0, spec.v0);
    verts[4] = vert(1.f, -1.f, spec.u1, spec.v1);
    verts[5] = vert(-1.f, -1.f, spec.u0, spec.v1);

    MetalSamplerKey sampler_key;
    sampler_key.min_filter =
        spec.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sampler_key.mag_filter =
        spec.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sampler_key.mip_filter = spec.mip_mode == 0   ? MTLSamplerMipFilterNotMipmapped
                             : spec.mip_mode == 1 ? MTLSamplerMipFilterNearest
                                                  : MTLSamplerMipFilterLinear;
    sampler_key.wrap_s =
        spec.wrap_s_repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    sampler_key.wrap_t =
        spec.wrap_t_repeat ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    sampler_key.max_anisotropy = std::max(1, spec.max_aniso);

    id<MTLTexture> target = make_color_target(m_device, spec.out_w, spec.out_h, false);
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

    id<MTLCommandBuffer> cmds = [m_queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
    MetalPsoKey pso_key;
    pso_key.shader = MetalShaderId::SAMPLE;
    pso_key.color_format = (u32)target.pixelFormat;
    [enc setRenderPipelineState:m_pso_cache.get_pipeline(pso_key)];
    [enc setVertexBytes:verts length:sizeof(verts) atIndex:0];
    [enc setFragmentTexture:tex atIndex:0];
    [enc setFragmentSamplerState:m_sampler_cache.get(sampler_key) atIndex:0];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [enc endEncoding];
#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:target];
      [blit endEncoding];
    }
#endif
    [cmds commit];
    [cmds waitUntilCompleted];
    return read_color_target(target, out);
  }
}

metal_renderer::ScaffoldStats MetalRenderer::stats() {
  metal_renderer::ScaffoldStats s;
  {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    s.frames_rendered = m_frame_count;
  }
  s.pso_count = m_pso_cache.pipeline_count();
  s.depth_stencil_count = m_pso_cache.depth_stencil_count();
  s.pso_misses = m_pso_cache.pipeline_misses();
  s.pso_hits = m_pso_cache.pipeline_hits();
  return s;
}
