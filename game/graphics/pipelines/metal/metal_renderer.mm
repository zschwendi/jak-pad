#include "metal_renderer.h"

#include <algorithm>

#include "common/log/log.h"

#include <TargetConditionals.h>

// Precompiled Metal shader library, embedded at build time from
// shaders/*.metal (see game/CMakeLists.txt and embed_metallib.cmake).
extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr MTLPixelFormat kColorFormat = MTLPixelFormatBGRA8Unorm;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float_Stencil8;

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

bool MetalRenderer::init(id<MTLDevice> device) {
  m_device = device;
  m_queue = [device newCommandQueue];

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
      [cmds presentDrawable:drawable];
    }

    [cmds commit];
    {
      std::lock_guard<std::mutex> lock(m_frame_mutex);
      m_last_frame_cmds = cmds;
      m_frame_count++;
    }
  }
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
