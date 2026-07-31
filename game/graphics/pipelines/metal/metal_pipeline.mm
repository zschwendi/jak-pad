/*!
 * @file metal_pipeline.mm
 * Experimental Metal implementation of the GfxRendererModule interface.
 *
 * Scope: this is the first executable step of the Metal backend. It creates a
 * real Metal device/window through SDL, renders a fixed validation frame
 * (a textured quad in front of a solid quad, with depth testing), presents it,
 * and exposes a pixel-readback hook so tests can verify the output.
 * It does not render the game's DMA chain yet.
 */

#include "metal_pipeline.h"

#include <mutex>

#include "common/log/log.h"

#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/SDL/include/SDL3/SDL_metal.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <TargetConditionals.h>

namespace {

// One vertex of the validation scene. Layout must match VertexIn in the MSL below.
struct TestVertex {
  float pos[3];
  float uv[2];
  float color[4];
  float use_tex;
};

constexpr int kOffscreenWidth = 640;
constexpr int kOffscreenHeight = 480;

constexpr const char* kTestShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
  packed_float3 pos;
  packed_float2 uv;
  packed_float4 color;
  float use_tex;
};

struct VSOut {
  float4 pos [[position]];
  float2 uv;
  float4 color;
  float use_tex;
};

vertex VSOut vs_main(uint vid [[vertex_id]], const device VertexIn* verts [[buffer(0)]]) {
  VertexIn v = verts[vid];
  VSOut out;
  out.pos = float4(v.pos, 1.0);
  out.uv = v.uv;
  out.color = v.color;
  out.use_tex = v.use_tex;
  return out;
}

fragment float4 fs_main(VSOut in [[stage_in]], texture2d<float> tex [[texture(0)]]) {
  constexpr sampler s(mag_filter::nearest, min_filter::nearest);
  return mix(in.color, tex.sample(s, in.uv), in.use_tex);
}
)";

struct MetalState {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  id<MTLRenderPipelineState> pipeline;
  id<MTLDepthStencilState> depth_state;
  id<MTLBuffer> vertex_buffer;
  id<MTLTexture> checker_texture;

  // fixed-size offscreen target used for verification readback
  id<MTLTexture> offscreen_color;
  id<MTLTexture> offscreen_depth;

  // depth buffer matching the current drawable size
  id<MTLTexture> window_depth;

  id<MTLCommandBuffer> last_frame_cmds;
  std::mutex frame_mutex;
  bool has_rendered_frame = false;
  int vertex_count = 0;
};

MetalState* g_metal = nullptr;
bool g_metal_inited = false;

std::vector<TestVertex> make_test_scene() {
  // Quad helper: two triangles, corners (x0,y0)-(x1,y1) at depth z.
  // v coordinate is 0 at the top edge so texel row 0 lands at the top.
  auto add_quad = [](std::vector<TestVertex>& out, float x0, float y0, float x1, float y1, float z,
                     float r, float g, float b, float use_tex) {
    auto vert = [&](float x, float y) {
      TestVertex v{};
      v.pos[0] = x;
      v.pos[1] = y;
      v.pos[2] = z;
      v.uv[0] = (x - x0) / (x1 - x0);
      v.uv[1] = 1.f - (y - y0) / (y1 - y0);
      v.color[0] = r;
      v.color[1] = g;
      v.color[2] = b;
      v.color[3] = 1.f;
      v.use_tex = use_tex;
      return v;
    };
    out.push_back(vert(x0, y0));
    out.push_back(vert(x1, y0));
    out.push_back(vert(x1, y1));
    out.push_back(vert(x0, y0));
    out.push_back(vert(x1, y1));
    out.push_back(vert(x0, y1));
  };

  std::vector<TestVertex> verts;
  // near quad: checkerboard texture, drawn FIRST at z = 0.25
  add_quad(verts, -0.5f, -0.5f, 0.5f, 0.5f, 0.25f, 1.f, 1.f, 1.f, 1.f);
  // far quad: solid green, drawn SECOND at z = 0.75. If depth testing works it
  // must lose to the near quad everywhere they overlap.
  add_quad(verts, -0.9f, -0.9f, 0.9f, 0.9f, 0.75f, 0.f, 1.f, 0.f, 0.f);
  return verts;
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

id<MTLTexture> make_color_target(id<MTLDevice> device, int w, int h) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                  width:w
                                                                 height:h
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
  desc.storageMode = MTLStorageModeManaged;
#else
  desc.storageMode = MTLStorageModeShared;
#endif
  return [device newTextureWithDescriptor:desc];
}

id<MTLTexture> make_depth_target(id<MTLDevice> device, int w, int h) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                  width:w
                                                                 height:h
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

bool init_metal_objects(MetalState* s) {
  s->device = MTLCreateSystemDefaultDevice();
  if (!s->device) {
    lg::error("Metal: no default device available");
    return false;
  }
  s->queue = [s->device newCommandQueue];

  NSError* error = nil;
  id<MTLLibrary> library = [s->device newLibraryWithSource:@(kTestShaderSource)
                                                   options:nil
                                                     error:&error];
  if (!library) {
    lg::error("Metal: shader compile failed: {}", [[error localizedDescription] UTF8String]);
    return false;
  }

  auto* pipeline_desc = [[MTLRenderPipelineDescriptor alloc] init];
  pipeline_desc.vertexFunction = [library newFunctionWithName:@"vs_main"];
  pipeline_desc.fragmentFunction = [library newFunctionWithName:@"fs_main"];
  pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  pipeline_desc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  s->pipeline = [s->device newRenderPipelineStateWithDescriptor:pipeline_desc error:&error];
  if (!s->pipeline) {
    lg::error("Metal: pipeline creation failed: {}", [[error localizedDescription] UTF8String]);
    return false;
  }

  auto* depth_desc = [[MTLDepthStencilDescriptor alloc] init];
  depth_desc.depthCompareFunction = MTLCompareFunctionLess;
  depth_desc.depthWriteEnabled = YES;
  s->depth_state = [s->device newDepthStencilStateWithDescriptor:depth_desc];

  auto verts = make_test_scene();
  s->vertex_count = (int)verts.size();
  s->vertex_buffer = [s->device newBufferWithBytes:verts.data()
                                            length:verts.size() * sizeof(TestVertex)
                                           options:MTLResourceStorageModeShared];
  s->checker_texture = make_checker_texture(s->device);
  s->offscreen_color = make_color_target(s->device, kOffscreenWidth, kOffscreenHeight);
  s->offscreen_depth = make_depth_target(s->device, kOffscreenWidth, kOffscreenHeight);
  return true;
}

void encode_test_frame(MetalState* s,
                       id<MTLCommandBuffer> cmds,
                       id<MTLTexture> color,
                       id<MTLTexture> depth) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.2, 0.4, 1.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  pass.depthAttachment.clearDepth = 1.0;

  id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
  [enc setRenderPipelineState:s->pipeline];
  [enc setDepthStencilState:s->depth_state];
  [enc setCullMode:MTLCullModeNone];
  [enc setVertexBuffer:s->vertex_buffer offset:0 atIndex:0];
  [enc setFragmentTexture:s->checker_texture atIndex:0];
  [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:s->vertex_count];
  [enc endEncoding];
}

}  // namespace

/*!
 * Display for the Metal pipeline. Mirrors GLDisplay's structure (SDL window plus
 * display/input managers) without the imgui integration.
 */
class MetalDisplay : public GfxDisplay {
 public:
  MetalDisplay(SDL_Window* window, SDL_MetalView view, CAMetalLayer* layer, bool is_main)
      : m_window(window),
        m_view(view),
        m_layer(layer),
        m_display_manager(std::make_shared<DisplayManager>(window)),
        m_input_manager(std::make_shared<InputManager>(window)) {
    m_main = is_main;
    m_display_manager->set_input_manager(m_input_manager);
  }

  virtual ~MetalDisplay() {
    m_display_manager.reset();
    m_input_manager.reset();
    SDL_Metal_DestroyView(m_view);
    SDL_DestroyWindow(m_window);
  }

  std::shared_ptr<DisplayManager> get_display_manager() const override {
    return m_display_manager;
  }
  std::shared_ptr<InputManager> get_input_manager() const override { return m_input_manager; }

  void render() override;
  void init_splash() override {}
  void draw_splash(int /*fb_w*/, int /*fb_h*/) override {}

 private:
  void process_sdl_events() {
    SDL_Event evt;
    while (SDL_PollEvent(&evt) != 0) {
      m_display_manager->process_sdl_event(evt);
      m_input_manager->process_sdl_event(evt);
    }
  }

  SDL_Window* m_window;
  SDL_MetalView m_view;
  CAMetalLayer* m_layer;
  std::shared_ptr<DisplayManager> m_display_manager;
  std::shared_ptr<InputManager> m_input_manager;
};

void MetalDisplay::render() {
  @autoreleasepool {
    process_sdl_events();
    MetalState* s = g_metal;
    if (!s) {
      return;
    }

    id<MTLCommandBuffer> cmds = [s->queue commandBuffer];

    // 1) render the validation frame offscreen, so tests can read it back at a
    //    fixed resolution regardless of window size.
    encode_test_frame(s, cmds, s->offscreen_color, s->offscreen_depth);
#if TARGET_OS_OSX
    {
      id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
      [blit synchronizeResource:s->offscreen_color];
      [blit endEncoding];
    }
#endif

    // 2) render the same frame to the window.
    id<CAMetalDrawable> drawable = [m_layer nextDrawable];
    if (drawable) {
      int w = (int)drawable.texture.width;
      int h = (int)drawable.texture.height;
      if (!s->window_depth || (int)s->window_depth.width != w ||
          (int)s->window_depth.height != h) {
        s->window_depth = make_depth_target(s->device, w, h);
      }
      encode_test_frame(s, cmds, drawable.texture, s->window_depth);
      [cmds presentDrawable:drawable];
    }

    [cmds commit];
    {
      std::lock_guard<std::mutex> lock(s->frame_mutex);
      s->last_frame_cmds = cmds;
      s->has_rendered_frame = true;
    }
  }
}

namespace metal_renderer {

bool read_last_frame(FramePixels* out) {
  MetalState* s = g_metal;
  if (!s) {
    return false;
  }
  id<MTLCommandBuffer> cmds;
  {
    std::lock_guard<std::mutex> lock(s->frame_mutex);
    if (!s->has_rendered_frame) {
      return false;
    }
    cmds = s->last_frame_cmds;
  }
  [cmds waitUntilCompleted];

  const int w = kOffscreenWidth;
  const int h = kOffscreenHeight;
  std::vector<u8> bgra(w * h * 4);
  [s->offscreen_color getBytes:bgra.data()
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

}  // namespace metal_renderer

static int metal_init(GfxGlobalSettings& /*settings*/) {
  // The same SDL video setup the OpenGL pipeline uses, minus the GL attributes.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    lg::error("Metal pipeline could not initialize SDL: {}", SDL_GetError());
    return 1;
  }
  return 0;
}

static std::shared_ptr<GfxDisplay> metal_make_display(int width,
                                                      int height,
                                                      const char* title,
                                                      GfxGlobalSettings& /*settings*/,
                                                      GameVersion /*version*/,
                                                      bool is_main) {
  SDL_Window* window = SDL_CreateWindow(
      title, width, height, SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    lg::error("Metal pipeline could not create window: {}", SDL_GetError());
    return NULL;
  }

  SDL_MetalView view = SDL_Metal_CreateView(window);
  if (!view) {
    lg::error("Metal pipeline could not create Metal view: {}", SDL_GetError());
    SDL_DestroyWindow(window);
    return NULL;
  }
  CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
  if (!layer) {
    lg::error("Metal pipeline could not get CAMetalLayer");
    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    return NULL;
  }

  if (!g_metal_inited) {
    g_metal = new MetalState();
    if (!init_metal_objects(g_metal)) {
      delete g_metal;
      g_metal = nullptr;
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return NULL;
    }
    g_metal_inited = true;
    lg::info("Metal initialized - device: {}", [[g_metal->device name] UTF8String]);
  }
  layer.device = g_metal->device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

  return std::make_shared<MetalDisplay>(window, view, layer, is_main);
}

static void metal_exit() {
  if (g_metal) {
    delete g_metal;
    g_metal = nullptr;
  }
  g_metal_inited = false;
}

static u32 metal_vsync() {
  return 0;
}

static u32 metal_sync_path() {
  return 0;
}

static void metal_send_chain(const void* /*data*/, u32 /*offset*/) {
  static bool warned = false;
  if (!warned) {
    lg::warn("Metal pipeline does not render DMA chains yet; ignoring send_chain");
    warned = true;
  }
}

static void metal_texture_upload_now(const u8* /*tpage*/, int /*mode*/, u32 /*s7_ptr*/) {}
static void metal_texture_relocate(u32 /*dst*/, u32 /*src*/, u32 /*format*/) {}
static void metal_set_levels(const std::vector<std::string>& /*levels*/) {}
static void metal_set_active_levels(const std::vector<std::string>& /*levels*/) {}
static void metal_force_reload_all() {}
static void metal_force_reload_level(const std::string& /*level*/) {}
static void metal_force_reload_common() {}
static void metal_set_pmode_alp(float /*alp*/) {}

const GfxRendererModule gRendererMetal = {
    metal_init,                // init
    metal_make_display,        // make_display
    metal_exit,                // exit
    metal_vsync,               // vsync
    metal_sync_path,           // sync_path
    metal_send_chain,          // send_chain
    metal_texture_upload_now,  // texture_upload_now
    metal_texture_relocate,    // texture_relocate
    metal_set_levels,          // set_levels
    metal_set_active_levels,   // set_active_levels
    metal_force_reload_all,    // force_reload_all
    metal_force_reload_level,  // force_reload_level
    metal_force_reload_common, // force_reload_common
    metal_set_pmode_alp,       // set_pmode_alp
    GfxPipeline::Metal,        // pipeline
    "Metal"                    // name
};
