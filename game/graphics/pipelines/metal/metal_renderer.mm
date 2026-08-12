#include "metal_renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_generic2.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
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
  u64 last_stream_submission_completed = 0;
  int last_command_buffer_status = 0;
  s64 last_command_buffer_error_code = 0;
  metal_renderer::ExternalSubmissionGate external_submission_gate;
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

#if GOALPAD_VISION_STEREO_EYE_MARKERS
constexpr NSUInteger kStereoEyeMarkerBytesPerRow = 256;

id<MTLBuffer> make_stereo_eye_marker_buffer(id<MTLDevice> device,
                                            metal_renderer::StereoEyeMarkerEye eye) {
  constexpr NSUInteger kRows = metal_renderer::kStereoEyeMarkerLength;
  std::array<u8, kStereoEyeMarkerBytesPerRow * kRows> pixels = {};
  const auto width = eye == metal_renderer::StereoEyeMarkerEye::left
                         ? metal_renderer::kStereoEyeMarkerThickness
                         : metal_renderer::kStereoEyeMarkerLength;
  const auto height = eye == metal_renderer::StereoEyeMarkerEye::left
                          ? metal_renderer::kStereoEyeMarkerLength
                          : metal_renderer::kStereoEyeMarkerThickness;
  const std::array<u8, 4> bgra = eye == metal_renderer::StereoEyeMarkerEye::left
                                     ? std::array<u8, 4>{255, 0, 255, 255}
                                     : std::array<u8, 4>{255, 255, 0, 255};
  for (std::uint32_t y = 0; y < height; y++) {
    for (std::uint32_t x = 0; x < width; x++) {
      std::copy(bgra.begin(), bgra.end(), pixels.begin() + y * kStereoEyeMarkerBytesPerRow + x * 4);
    }
  }
  return [device newBufferWithBytes:pixels.data()
                             length:pixels.size()
                            options:MTLResourceStorageModeShared];
}

bool can_blit_stereo_eye_marker(id<MTLTexture> texture,
                                NSUInteger slice,
                                const metal_renderer::StereoEyeMarkerPlan& plan) {
  if (!texture) {
    return false;
  }
  const bool supported_format = texture.pixelFormat == MTLPixelFormatBGRA8Unorm ||
                                texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
  const bool valid_slice = texture.textureType == MTLTextureType2D
                               ? slice == 0
                               : texture.textureType == MTLTextureType2DArray &&
                                     slice < texture.arrayLength;
  return plan.valid() && supported_format && texture.sampleCount == 1 &&
         texture.storageMode != MTLStorageModeMemoryless && !texture.framebufferOnly && valid_slice &&
         plan.x + plan.width <= texture.width && plan.y + plan.height <= texture.height;
}
#endif

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
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

bool valid_external_slice(id<MTLTexture> texture, NSUInteger slice) {
  switch (texture.textureType) {
    case MTLTextureType2D:
      return slice == 0;
    case MTLTextureType2DArray:
      return slice < texture.arrayLength;
    default:
      return false;
  }
}

bool valid_external_target(id<MTLDevice> device,
                           const MetalRenderOptions& opts,
                           const MetalExternalRenderTargetDescriptor& target) {
  id<MTLTexture> color = target.color_texture;
  id<MTLTexture> depth = target.depth_texture;
  if (!target.view_id || !color || !depth || color.device != device || depth.device != device ||
      color.pixelFormat != kColorFormat || depth.pixelFormat != kDepthFormat ||
      color.sampleCount != 1 || depth.sampleCount != 1 || !color.width || !color.height ||
      color.width != depth.width || color.height != depth.height || color.framebufferOnly ||
      !(color.usage & MTLTextureUsageRenderTarget) ||
      !(depth.usage & MTLTextureUsageRenderTarget) ||
      !valid_external_slice(color, target.color_slice) ||
      !valid_external_slice(depth, target.depth_slice)) {
    return false;
  }

  const auto& viewport = target.viewport;
  return metal_renderer::is_finite(target.view_transform) && opts.game_res_w > 0 &&
         opts.game_res_h > 0 && std::isfinite(viewport.originX) &&
         std::isfinite(viewport.originY) && std::isfinite(viewport.width) &&
         std::isfinite(viewport.height) && std::isfinite(viewport.znear) &&
         std::isfinite(viewport.zfar) && std::isfinite(target.clear_depth) &&
         viewport.originX == 0.0 && viewport.originY == 0.0 && viewport.width == color.width &&
         viewport.height == color.height && viewport.width == opts.game_res_w &&
         viewport.height == opts.game_res_h && viewport.znear == 0.0 && viewport.zfar == 1.0 &&
         target.clear_depth == 0.0;
}

bool external_targets_are_distinct(const MetalExternalRenderTargetDescriptor& a,
                                   const MetalExternalRenderTargetDescriptor& b) {
  const bool color_alias = a.color_texture == b.color_texture && a.color_slice == b.color_slice;
  const bool depth_alias = a.depth_texture == b.depth_texture && a.depth_slice == b.depth_slice;
  return a.view_id != b.view_id && !color_alias && !depth_alias;
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

  for (const auto& descriptor : table) {
    const auto bucket_id = static_cast<std::size_t>(descriptor.id);
    const int batch_size = metal_renderer::jak2_metal_direct_batch_size(bucket_id);
    if (batch_size != 0) {
      ASSERT(descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::DeferredSkip);
      const char* name = "direct";
      switch (static_cast<jak2::BucketId>(bucket_id)) {
        case jak2::BucketId::SKY_DRAW:
          name = "sky-draw";
          break;
        case jak2::BucketId::SCREEN_FILTER:
          name = "screen-filter";
          break;
        case jak2::BucketId::DEBUG2:
          name = "debug2";
          break;
        case jak2::BucketId::DEBUG_NO_ZBUF2:
          name = "debug-no-zbuf2";
          break;
        case jak2::BucketId::DEBUG3:
          name = "debug3";
          break;
        default:
          ASSERT(false);
      }
      m_bucket_renderers[bucket_id] =
          std::make_unique<MetalDirectRenderer>(name, descriptor.id, batch_size);
    } else if (descriptor.behavior == metal_renderer::Jak2MetalBucketBehavior::DeferredSkip) {
      m_bucket_renderers[bucket_id] = std::make_unique<MetalSkipRenderer>(
          fmt::format("jak2-deferred-{}", bucket_id), descriptor.id);
    } else {
      m_bucket_renderers[bucket_id] =
          std::make_unique<MetalEmptyBucketRenderer>(fmt::format("bucket-{}", bucket_id),
                                                     descriptor.id);
    }
  }
}

void MetalRenderer::init_bucket_renderers(TexturePool* pool, GameVersion version) {
  m_texture_pool = pool;
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
#if GOALPAD_VISION_STEREO_EYE_MARKERS
  m_left_eye_marker =
      make_stereo_eye_marker_buffer(device, metal_renderer::StereoEyeMarkerEye::left);
  m_right_eye_marker =
      make_stereo_eye_marker_buffer(device, metal_renderer::StereoEyeMarkerEye::right);
  if (!m_left_eye_marker || !m_right_eye_marker) {
    lg::warn("Metal: could not allocate the diagnostic stereo eye marker buffers");
  }
#endif
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
  }
}

void MetalRenderer::encode_game_passes(id<MTLCommandBuffer> cmds) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = m_game_color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  // Jak 1 clears the game framebuffer to transparent black and depth to 0
  // (OpenGLRenderer::setup_frame)
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = m_game_depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  pass.depthAttachment.clearDepth = 0.0;
  pass.stencilAttachment.texture = m_game_depth;
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
                                        const MetalRenderOptions& opts) {
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
  [enc setFragmentTexture:m_game_color atIndex:0];
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

    setup_frame(opts);
    encode_game_passes(cmds);
#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:m_game_color];
      [blit endEncoding];
    }
#endif

    id<CAMetalDrawable> drawable = [layer nextDrawable];
    if (drawable) {
      encode_present_pass(cmds, drawable.texture, opts);
      schedule_present(cmds, drawable, opts);
    }

    [cmds commit];
    {
      std::lock_guard<std::mutex> lock(m_frame_mutex);
      m_last_internal_frame_cmds = cmds;
      m_last_internal_frame_submission = ++m_command_submission_count;
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
    if (m_shared_state.secondary_view &&
        (dynamic_cast<MetalTextureBucketRenderer*>(renderer.get()) ||
         dynamic_cast<MetalEyeRenderer*>(renderer.get()))) {
      metal_consume_bucket_without_side_effects(dma, m_shared_state);
    } else {
      renderer->render(dma, &m_shared_state, ctx);
    }
    // should have ended at the start of the next bucket
    ASSERT(dma.current_tag_offset() == m_shared_state.next_bucket);
    m_shared_state.next_bucket += 16;
    if (!m_shared_state.secondary_view) {
      m_chain_stats.last_buckets_dispatched++;
      m_chain_stats.last_frame_global_callbacks++;
      metal_vif_interrupt_callback((int)bucket_id);
    }
  }
}

void MetalRenderer::dispatch_buckets_jak2(DmaFollower dma, MetalFrameContext& ctx) {
  m_shared_state.buckets_base = dma.current_tag_offset();
  m_shared_state.next_bucket = m_shared_state.buckets_base + 16;
  m_shared_state.default_regs_buffer = 0;

  for (size_t bucket_id = 0; bucket_id < m_bucket_renderers.size(); bucket_id++) {
    auto& renderer = m_bucket_renderers[bucket_id];
    if (m_shared_state.secondary_view &&
        (dynamic_cast<MetalTextureBucketRenderer*>(renderer.get()) ||
         dynamic_cast<MetalEyeRenderer*>(renderer.get()))) {
      metal_consume_bucket_without_side_effects(dma, m_shared_state);
    } else {
      renderer->render(dma, &m_shared_state, ctx);
    }
    ASSERT(dma.current_tag_offset() == m_shared_state.next_bucket);
    m_shared_state.next_bucket += 16;
    if (!m_shared_state.secondary_view) {
      m_chain_stats.last_buckets_dispatched++;
      m_chain_stats.last_frame_global_callbacks++;
      metal_vif_interrupt_callback((int)bucket_id + 1);
    }
  }
  if (!m_shared_state.secondary_view) {
    m_chain_stats.last_frame_global_callbacks++;
    metal_vif_interrupt_callback((int)m_bucket_renderers.size());
  }
}

bool MetalRenderer::render_chain_frame(const MetalRenderOptions& opts,
                                       CAMetalLayer* layer,
                                       const u8* chain_data,
                                       u32 chain_offset) {
  return render_chain_frame_impl(opts, layer, nil, 0, nil, 0, nullptr, 0.0, 0, {}, true, nil,
                                 chain_data, chain_offset);
}

bool MetalRenderer::render_chain_frame_to_external_target(
    const MetalRenderOptions& opts,
    const MetalExternalRenderTargetDescriptor& target,
    const u8* chain_data,
    u32 chain_offset) {
  if (!chain_data || !valid_external_target(m_device, opts, target)) {
    return false;
  }
  return render_chain_frame_impl(opts, nil, target.color_texture, target.color_slice,
                                 target.depth_texture, target.depth_slice, &target.viewport,
                                 target.clear_depth, target.view_id, target.view_transform, true,
                                 nil, chain_data, chain_offset);
}

MetalExternalFrameReservation MetalRenderer::reserve_external_stereo_frame() {
  if (!m_presentation_state) {
    return MetalExternalFrameReservation::failed;
  }

  u64 last_stream_submission;
  {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (m_external_stereo_disabled) {
      return MetalExternalFrameReservation::failed;
    }
    last_stream_submission = m_last_stream_submission;
  }
  bool prior_stream_busy;
  {
    std::lock_guard<std::mutex> lock(m_presentation_state->mutex);
    prior_stream_busy = last_stream_submission &&
                        m_presentation_state->last_stream_submission_completed <
                            last_stream_submission;
  }
  switch (m_presentation_state->external_submission_gate.reserve(prior_stream_busy)) {
    case metal_renderer::ExternalSubmissionReservation::busy:
      return MetalExternalFrameReservation::busy;
    case metal_renderer::ExternalSubmissionReservation::failed:
      return MetalExternalFrameReservation::failed;
    case metal_renderer::ExternalSubmissionReservation::reserved:
      break;
  }
  m_stream.reset();
  return MetalExternalFrameReservation::reserved;
}

void MetalRenderer::cancel_external_stereo_frame() {
  if (m_presentation_state) {
    m_presentation_state->external_submission_gate.cancel();
  }
}

bool MetalRenderer::render_chain_frame_to_external_stereo_targets(
    const MetalRenderOptions& opts,
    const MetalExternalRenderTargetDescriptor& left,
    const MetalExternalRenderTargetDescriptor& right,
    id<MTLCommandBuffer> command_buffer,
    const u8* chain_data,
    u32 chain_offset) {
  const bool reserved = m_presentation_state &&
                        m_presentation_state->external_submission_gate.is_reserved();
  if (!reserved || command_buffer == nil || command_buffer.device != m_device ||
      command_buffer.status != MTLCommandBufferStatusNotEnqueued || !chain_data ||
      !valid_external_target(m_device, opts, left) ||
      !valid_external_target(m_device, opts, right) ||
      !external_targets_are_distinct(left, right)) {
    return false;
  }

  const bool left_rendered =
      render_chain_frame_impl(opts, nil, left.color_texture, left.color_slice, left.depth_texture,
                              left.depth_slice, &left.viewport, left.clear_depth, left.view_id,
                              left.view_transform, true, command_buffer, chain_data, chain_offset);
  const bool right_rendered =
      left_rendered && render_chain_frame_impl(
                           opts, nil, right.color_texture, right.color_slice, right.depth_texture,
                           right.depth_slice, &right.viewport, right.clear_depth, right.view_id,
                           right.view_transform, false, command_buffer, chain_data, chain_offset);
  if (right_rendered) {
    m_chain_stats.last_views_rendered = 2;
#if GOALPAD_VISION_STEREO_EYE_MARKERS
    encode_stereo_eye_markers(command_buffer, left, right);
#endif
  }
  return right_rendered;
}

#if GOALPAD_VISION_STEREO_EYE_MARKERS
void MetalRenderer::encode_stereo_eye_markers(
    id<MTLCommandBuffer> command_buffer,
    const MetalExternalRenderTargetDescriptor& left,
    const MetalExternalRenderTargetDescriptor& right) {
  const auto left_plan = metal_renderer::stereo_eye_marker_plan(
      static_cast<std::uint32_t>(left.color_texture.width),
      static_cast<std::uint32_t>(left.color_texture.height),
      metal_renderer::StereoEyeMarkerEye::left);
  const auto right_plan = metal_renderer::stereo_eye_marker_plan(
      static_cast<std::uint32_t>(right.color_texture.width),
      static_cast<std::uint32_t>(right.color_texture.height),
      metal_renderer::StereoEyeMarkerEye::right);
  const bool mark_left = m_left_eye_marker &&
                         can_blit_stereo_eye_marker(left.color_texture, left.color_slice, left_plan);
  const bool mark_right = m_right_eye_marker && can_blit_stereo_eye_marker(
                                                     right.color_texture, right.color_slice,
                                                     right_plan);
  if (!mark_left && !mark_right) {
    return;
  }

  id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
  if (!encoder) {
    return;
  }
  encoder.label = @"Eco diagnostic stereo eye markers";
  const auto encode = [&](id<MTLBuffer> source,
                          id<MTLTexture> destination,
                          NSUInteger slice,
                          const metal_renderer::StereoEyeMarkerPlan& plan) {
    [encoder copyFromBuffer:source
               sourceOffset:0
          sourceBytesPerRow:kStereoEyeMarkerBytesPerRow
        sourceBytesPerImage:kStereoEyeMarkerBytesPerRow * plan.height
                 sourceSize:MTLSizeMake(plan.width, plan.height, 1)
                  toTexture:destination
           destinationSlice:slice
           destinationLevel:0
          destinationOrigin:MTLOriginMake(plan.x, plan.y, 0)];
  };
  if (mark_left) {
    encode(m_left_eye_marker, left.color_texture, left.color_slice, left_plan);
  }
  if (mark_right) {
    encode(m_right_eye_marker, right.color_texture, right.color_slice, right_plan);
  }
  [encoder endEncoding];
}
#endif

bool MetalRenderer::submit_external_stereo_frame(id<MTLCommandBuffer> command_buffer) {
  if (command_buffer == nil || command_buffer.device != m_device ||
      command_buffer.status != MTLCommandBufferStatusNotEnqueued || !m_presentation_state) {
    cancel_external_stereo_frame();
    return false;
  }

  u64 command_submission_id;
  {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    command_submission_id = m_command_submission_count + 1;
    if (!m_presentation_state->external_submission_gate.submit(command_submission_id)) {
      cancel_external_stereo_frame();
      return false;
    }
    m_command_submission_count = command_submission_id;
    m_last_stream_submission = command_submission_id;
    m_last_external_stream_submission = command_submission_id;
  }

  const auto completion_state = m_presentation_state;
  [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
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
      completion_state->last_stream_submission_completed =
          std::max(completion_state->last_stream_submission_completed, command_submission_id);
    }
    completion_state->external_submission_gate.complete(
        command_submission_id, status == MTLCommandBufferStatusCompleted);
    if (report_error) {
      lg::error("Metal stereo command buffer failed with status {} and error code {}",
                static_cast<int>(status), error_code);
    }
    completion_state->command_buffer_cv.notify_all();
  }];
  [command_buffer commit];
  m_chain_stats.command_buffers_committed++;
  return true;
}

void MetalRenderer::prepare_external_stereo_fallback() {
  if (!m_presentation_state) {
    return;
  }

  std::scoped_lock lock(m_frame_mutex, m_presentation_state->mutex);
  const u64 external_submission = m_last_external_stream_submission;
  if (!external_submission || m_last_stream_submission != external_submission ||
      m_presentation_state->last_stream_submission_completed >= external_submission) {
    return;
  }
  m_stream.quarantine_in_flight_pages();
  m_last_stream_submission = 0;
  m_external_stereo_disabled = true;
}

bool MetalRenderer::render_chain_frame_impl(const MetalRenderOptions& opts,
                                            CAMetalLayer* layer,
                                            id<MTLTexture> game_color,
                                            NSUInteger color_slice,
                                            id<MTLTexture> game_depth,
                                            NSUInteger depth_slice,
                                            const MTLViewport* viewport,
                                            double clear_depth,
                                            u64 view_id,
                                            const metal_renderer::ViewTransform& view_transform,
                                            bool frame_global_side_effects,
                                            id<MTLCommandBuffer> borrowed_command_buffer,
                                            const u8* chain_data,
                                            u32 chain_offset) {
  bool drawable_acquired = false;
  const bool external_target = view_id != 0;
  @autoreleasepool {
    ASSERT_MSG(!m_bucket_renderers.empty(), "init_bucket_renderers was not called");
    if (borrowed_command_buffer == nil) {
      // The ordinary renderer preserves its existing correctness-first wait. External stereo
      // reserves this allocator before the game tick and returns busy instead of entering here.
      u64 previous_stream_submission;
      {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        previous_stream_submission = m_last_stream_submission;
      }
      if (previous_stream_submission) {
        std::unique_lock<std::mutex> lock(m_presentation_state->mutex);
        m_presentation_state->command_buffer_cv.wait(lock, [&] {
          return m_presentation_state->last_stream_submission_completed >=
                 previous_stream_submission;
        });
        lock.unlock();
        std::lock_guard<std::mutex> frame_lock(m_frame_mutex);
        m_stream_reuse_wait_count++;
      }
      m_stream.reset();
    }

    if (!external_target) {
      setup_frame(opts);
      game_color = m_game_color;
      game_depth = m_game_depth;
    }
    // Secondary replay gets scratch bookkeeping, so the primary frame remains the one reported to
    // the host even though both views decode the same chain.
    MetalBackgroundState secondary_background;
    MetalBackgroundState* background =
        frame_global_side_effects ? &m_background : &secondary_background;
    background->reset_frame();
    background->camera_trace.reset(
        frame_global_side_effects && opts.expected_camera_valid ? &opts.expected_camera : nullptr);
    background->render_camera_trace.reset(frame_global_side_effects &&
                                                  opts.expected_render_camera_valid
                                              ? &opts.expected_render_camera
                                              : nullptr);
    if (frame_global_side_effects && m_shared_state.eye_renderer) {
      m_shared_state.eye_renderer->start_frame();
    }
    m_shared_state.background = background;
    m_shared_state.texture_pool = m_texture_pool;
    m_shared_state.ee_memory = g_ee_main_mem;
    m_shared_state.offset_of_s7 = g_s7_override ? g_s7_override : metal_offset_of_s7();
    m_shared_state.engine_frame_id = opts.engine_frame_id;
    m_shared_state.game_res_w = opts.game_res_w;
    m_shared_state.game_res_h = opts.game_res_h;
    m_shared_state.view_transform = view_transform;
    m_shared_state.detailed_frame_stats_enabled = m_detailed_frame_stats_enabled;
    m_shared_state.secondary_view = !frame_global_side_effects;

    id<MTLCommandBuffer> cmds = borrowed_command_buffer ?: [m_queue commandBuffer];
    if (cmds == nil) {
      return false;
    }
    if (external_target && borrowed_command_buffer == nil) {
      cmds.label = [NSString stringWithFormat:@"OpenGOAL external view %llu",
                                              static_cast<unsigned long long>(view_id)];
    }

    // one render pass over the game target for all buckets, cleared like
    // Jak 1's setup_frame (color 0, depth 0, PS2 reversed depth)
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = game_color;
    pass.colorAttachments[0].slice = color_slice;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    pass.depthAttachment.texture = game_depth;
    pass.depthAttachment.slice = depth_slice;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    // stored, not discarded: the sprite distorter may split the pass and
    // reload depth/stencil (MetalFrameContext::resume_pass_with_framebuffer_copy)
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.depthAttachment.clearDepth = clear_depth;
    pass.stencilAttachment.texture = game_depth;
    pass.stencilAttachment.slice = depth_slice;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.clearStencil = 0;

    id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
    [enc setCullMode:MTLCullModeNone];
    const MTLViewport frame_viewport =
        viewport ? *viewport
                 : MTLViewport{0.0, 0.0, (double)game_color.width, (double)game_color.height, 0.0,
                               1.0};
    [enc setViewport:frame_viewport];

    MetalFrameContext ctx;
    ctx.enc = enc;
    ctx.pso_cache = &m_pso_cache;
    ctx.sampler_cache = &m_sampler_cache;
    ctx.stream = &m_stream;
    ctx.color_format = kColorFormat;
    ctx.depth_format = kDepthFormat;
    ctx.cmds = cmds;
    ctx.game_color = game_color;
    ctx.game_color_slice = color_slice;
    ctx.game_depth = game_depth;
    ctx.game_depth_slice = depth_slice;
    ctx.game_viewport = frame_viewport;

    if (frame_global_side_effects) {
      m_chain_stats.last_buckets_dispatched = 0;
      m_chain_stats.last_frame_global_callbacks = 0;
    }
    switch (m_shared_state.version) {
      case GameVersion::Jak1:
        dispatch_buckets_jak1(DmaFollower(chain_data, chain_offset), ctx);
        break;
      case GameVersion::Jak2:
        dispatch_buckets_jak2(DmaFollower(chain_data, chain_offset), ctx);
        break;
      default:
        ASSERT_MSG(false, "Metal DMA dispatch only supports Jak 1 and Jak 2");
    }
    [ctx.enc endEncoding];

    if (frame_global_side_effects) {
      m_chain_stats.last_host_tick_id = opts.host_tick_id;
      m_chain_stats.last_chain_ordinal = opts.chain_ordinal;
      m_chain_stats.last_engine_frame_id = opts.engine_frame_id;
      m_chain_stats.last_camera_fingerprint = m_background.camera_trace.first_packet_fingerprint();
      m_chain_stats.last_camera_packets = m_background.camera_trace.packet_count();
      m_chain_stats.last_live_camera_mismatches = m_background.camera_trace.expected_mismatches();
      m_chain_stats.last_packet_camera_mismatches = m_background.camera_trace.packet_mismatches();
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
          if (render_camera.alternation && !m_presentation_state->render_alternation_reported) {
            m_presentation_state->render_alternation_reported = true;
            report_render_camera_alternation = true;
          }
        }
        producer_camera = m_presentation_state->producer_camera.observe(opts.engine_frame_id,
                                                                        opts.expected_camera);
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
        if (producer_camera.alternation && !m_presentation_state->producer_alternation_reported) {
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
        lg::error("Metal camera producer return pattern at engine frames {}/{}/{}, host tick {}, "
                  "chain {}: "
                  "normalized distances {:.9g}/{:.9g}/{:.9g}, fingerprints {:#x}/{:#x}/{:#x}",
                  producer_camera.older_frame_id, producer_camera.previous_frame_id,
                  producer_camera.current_frame_id, opts.host_tick_id, opts.chain_ordinal,
                  producer_camera.older_to_previous_distance,
                  producer_camera.previous_to_current_distance,
                  producer_camera.older_to_current_distance, producer_camera.older_fingerprint,
                  producer_camera.previous_fingerprint, producer_camera.current_fingerprint);
      }
      if (!m_reported_camera_mismatch && (m_chain_stats.last_live_camera_mismatches ||
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
            m_background.first_camera_mismatch_bucket, m_chain_stats.last_live_camera_mismatches,
            m_chain_stats.last_live_camera_mismatch_qwords,
            m_chain_stats.last_packet_camera_mismatches,
            m_chain_stats.last_packet_camera_mismatch_qwords,
            m_chain_stats.last_render_camera_live_mismatches,
            m_chain_stats.last_render_camera_live_mismatch_qwords,
            m_chain_stats.last_render_camera_packet_mismatches,
            m_chain_stats.last_render_camera_packet_mismatch_qwords);
      }
    }

#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:game_color];
      [blit endEncoding];
    }
#endif

    id<CAMetalDrawable> drawable = layer ? [layer nextDrawable] : nil;
    if (drawable) {
      drawable_acquired = true;
      m_chain_stats.drawables_acquired++;
      encode_present_pass(cmds, drawable.texture, opts);
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

    if ((layer || external_target) && borrowed_command_buffer == nil) {
      u64 command_submission_id;
      {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        command_submission_id = ++m_command_submission_count;
        m_last_stream_submission = command_submission_id;
      }
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
          completion_state->last_stream_submission_completed =
              std::max(completion_state->last_stream_submission_completed,
                       command_submission_id);
        }
        if (report_error) {
          lg::error("Metal command buffer failed with status {} and error code {}",
                    static_cast<int>(status), error_code);
        }
        completion_state->command_buffer_cv.notify_all();
      }];
      [cmds commit];
      m_chain_stats.command_buffers_committed++;
      {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        if (!external_target) {
          m_last_internal_frame_cmds = cmds;
          m_last_internal_frame_submission = command_submission_id;
          m_frame_count++;
        }
      }
    }

    if (frame_global_side_effects) {
      m_chain_stats.chains_rendered++;
      m_chain_stats.last_views_rendered = 1;
      m_chain_stats.draw_calls = ctx.draw_calls;
      m_chain_stats.triangles = ctx.triangles;
    }

    // Detailed diagnostics require an RTTI walk over every bucket renderer. Hosts that do not
    // consume those diagnostics can skip the aggregation without changing rendering or essential
    // frame/submission accounting.
    if (frame_global_side_effects && m_detailed_frame_stats_enabled) {
      int uploads = 0;
      u64 skipped = 0;
      int unsupported_blends = 0;
      m_chain_stats.ocean_draws = 0;
      m_chain_stats.ocean_triangles = 0;
      m_chain_stats.ocean_missing_textures = 0;
      MetalMerc2::Stats merc_stats;
      MetalGeneric2::Stats generic_stats;
      for (auto& r : m_bucket_renderers) {
        if (auto* t = dynamic_cast<MetalTextureBucketRenderer*>(r.get())) {
          uploads += t->last_stats().uploads;
        } else if (auto* s = dynamic_cast<MetalSkipRenderer*>(r.get())) {
          skipped += s->skipped_bytes();
        } else if (auto* d = dynamic_cast<MetalDirectRenderer*>(r.get())) {
          unsupported_blends += d->stats().unsupported_blends;
        } else if (auto* sky = dynamic_cast<MetalSkyRenderer*>(r.get())) {
          unsupported_blends += sky->direct_stats().unsupported_blends;
        } else if (auto* omf = dynamic_cast<MetalOceanMidAndFar*>(r.get())) {
          m_chain_stats.ocean_texture_verts = omf->texture_stats().vertices;
          m_chain_stats.ocean_mid_verts = omf->mid_stats().vertices;
          m_chain_stats.ocean_draws +=
              omf->texture_stats().draw_calls + omf->mid_stats().draw_calls;
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
          m_chain_stats.sprite_draws = ss.draw_calls;
          m_chain_stats.sprite_missing_textures = ss.missing_textures;
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
      m_chain_stats.merc_models_with_missing_bone_slots = merc_stats.models_with_missing_bone_slots;
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
        m_chain_stats.first_merc_eichar_provenance_event = merc_stats.first_eichar_provenance_event;
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
    m_shared_state.background = &m_background;
    m_shared_state.secondary_view = false;
    m_shared_state.view_transform = {};
  }
  return external_target || drawable_acquired;
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
    cmds = m_last_internal_frame_cmds;
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
    encode_present_pass(cmds, target, opts);
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
    s.last_internal_frame_submission = m_last_internal_frame_submission;
    s.stream_reuse_waits = m_stream_reuse_wait_count;
  }
  s.pso_count = m_pso_cache.pipeline_count();
  s.depth_stencil_count = m_pso_cache.depth_stencil_count();
  s.pso_misses = m_pso_cache.pipeline_misses();
  s.pso_hits = m_pso_cache.pipeline_hits();
  return s;
}
