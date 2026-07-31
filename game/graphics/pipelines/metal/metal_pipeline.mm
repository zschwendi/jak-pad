/*!
 * @file metal_pipeline.mm
 * Experimental Metal implementation of the GfxRendererModule interface.
 *
 * SDL window / CAMetalLayer setup and the GfxRendererModule glue. The frame
 * rendering itself (offscreen game target, PSO cache, present pass) lives in
 * MetalRenderer (metal_renderer.mm). The game passes currently render a fixed
 * validation scene; send_chain is still a logged no-op.
 */

#include "metal_pipeline.h"

#include "common/log/log.h"

#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"
#include "game/system/hid/display_manager.h"
#include "game/system/hid/input_manager.h"

#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/SDL/include/SDL3/SDL_metal.h"

namespace {

MetalRenderer* g_renderer = nullptr;

// texture pool for the Metal pipeline (the analog of GraphicsData::texture_pool
// in the GL pipeline). Created with the display, once the device exists.
std::shared_ptr<TexturePool> g_texture_pool;

// Largest centered region with the game's 4:3 aspect that fits the window.
// The real game supplies its own draw region sizes; this stands in until the
// DMA chain drives frames.
void compute_draw_region(int fb_w, int fb_h, int* w, int* h) {
  if (fb_w * 3 >= fb_h * 4) {
    *h = fb_h;
    *w = fb_h * 4 / 3;
  } else {
    *w = fb_w;
    *h = fb_w * 3 / 4;
  }
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
  process_sdl_events();
  if (!g_renderer) {
    return;
  }
  int fb_w = 0;
  int fb_h = 0;
  SDL_GetWindowSizeInPixels(m_window, &fb_w, &fb_h);

  MetalRenderOptions opts;
  compute_draw_region(fb_w, fb_h, &opts.draw_region_w, &opts.draw_region_h);
  g_renderer->render_frame(opts, m_layer);
}

namespace metal_renderer {

bool read_last_frame(FramePixels* out) {
  return g_renderer && g_renderer->read_game_frame(out);
}

bool read_present_frame(const PresentTestOptions& opts, FramePixels* out) {
  if (!g_renderer) {
    return false;
  }
  MetalRenderOptions render_opts;
  render_opts.draw_region_w = opts.draw_region_w;
  render_opts.draw_region_h = opts.draw_region_h;
  render_opts.pmode_alp = opts.pmode_alp;
  render_opts.brightness_contrast_color = opts.brightness_contrast_color;
  render_opts.brightness_contrast_alpha = opts.brightness_contrast_alpha;
  return g_renderer->read_present_frame(opts.window_w, opts.window_h, render_opts, out);
}

ScaffoldStats get_stats() {
  return g_renderer ? g_renderer->stats() : ScaffoldStats{};
}

bool read_texture_sample(const TextureSampleSpec& spec, FramePixels* out) {
  return g_renderer && g_renderer->read_texture_sample(spec, out);
}

TexturePool* get_texture_pool() {
  return g_texture_pool.get();
}

u64 upload_texture_rgba8(const u8* data, int w, int h) {
  if (!g_renderer) {
    return 0;
  }
  return metal_upload_texture_rgba8(g_renderer->device(), g_renderer->queue(), data, w, h);
}

u64 pool_add_texture(const tfrag3::Texture& tex, bool is_common) {
  if (!g_renderer || !g_texture_pool) {
    return 0;
  }
  return metal_add_texture(g_renderer->device(), g_renderer->queue(), *g_texture_pool, tex,
                           is_common);
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
                                                      GameVersion version,
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

  if (!g_renderer) {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      lg::error("Metal: no default device available");
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return NULL;
    }
    auto* renderer = new MetalRenderer();
    if (!renderer->init(device)) {
      delete renderer;
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return NULL;
    }
    g_renderer = renderer;
    lg::info("Metal initialized - device: {}", [[device name] UTF8String]);
  }
  layer.device = g_renderer->device();
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;

  if (!g_texture_pool) {
    g_texture_pool = std::make_shared<TexturePool>(version);
    if (!metal_setup_placeholder(g_renderer->device(), g_renderer->queue(), *g_texture_pool)) {
      lg::error("Metal: placeholder texture creation failed");
      g_texture_pool.reset();
      SDL_Metal_DestroyView(view);
      SDL_DestroyWindow(window);
      return NULL;
    }
  }

  return std::make_shared<MetalDisplay>(window, view, layer, is_main);
}

static void metal_exit() {
  g_texture_pool.reset();
  delete g_renderer;
  g_renderer = nullptr;
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

/*!
 * Upload a texture-page outside the main DMA chain. Same contract as the GL
 * pipeline's gl_texture_upload_now: the pool locks internally.
 */
static void metal_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_texture_pool) {
    g_texture_pool->handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr, false);
  }
}

/*!
 * VRAM-to-VRAM texture copy: pure pointer bookkeeping in the pool.
 */
static void metal_texture_relocate(u32 dst, u32 src, u32 format) {
  if (g_texture_pool) {
    g_texture_pool->relocate(dst, src, format);
  }
}
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
