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

#include <condition_variable>
#include <mutex>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/graphics/pipelines/metal/metal_level_data.h"
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

// When set, make_display creates a hidden window (see metal_renderer::
// set_window_hidden). Must be set before make_display.
bool g_hidden_window = false;

// texture pool for the Metal pipeline (the analog of GraphicsData::texture_pool
// in the GL pipeline). Created with the display, once the device exists.
std::shared_ptr<TexturePool> g_texture_pool;

// DMA chain handoff between the game thread (send_chain / vsync / sync_path)
// and the render thread. Mirrors the GraphicsData sync model in
// game/graphics/pipelines/opengl.cpp: dma_mutex/dma_cv guard the copied chain,
// sync_mutex/sync_cv guard the frame counter for vsync.
struct ChainSync {
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  std::unique_ptr<FixedChunkDmaCopier> copier;
  float pmode_alp = 1.f;
};
ChainSync g_chain;

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

  // wait briefly for a copied chain, like GLDisplay's render_game_frame; if
  // none arrives, render the validation scene so the window stays responsive
  bool got_chain = false;
  {
    std::unique_lock<std::mutex> lock(g_chain.dma_mutex);
    got_chain = g_chain.dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                        [] { return g_chain.has_data_to_render; });
  }

  if (got_chain) {
    g_chain.frame_idx_of_input_data = g_chain.frame_idx;
    MetalRenderOptions opts;
    opts.game_res_w = Gfx::g_global_settings.game_res_w;
    opts.game_res_h = Gfx::g_global_settings.game_res_h;
    if (opts.game_res_w <= 0 || opts.game_res_h <= 0) {
      opts.game_res_w = 640;
      opts.game_res_h = 480;
    }
    opts.draw_region_w = Gfx::g_global_settings.lbox_w;
    opts.draw_region_h = Gfx::g_global_settings.lbox_h;
    opts.pmode_alp = g_chain.pmode_alp;
    opts.brightness_contrast_color = Gfx::g_global_settings.brightness_contrast_color;
    opts.brightness_contrast_alpha = Gfx::g_global_settings.brightness_contrast_alpha;

    const auto& chain = g_chain.copier->get_last_result();
    g_renderer->render_chain_frame(opts, m_layer, chain.data.data(), chain.start_offset);
  } else {
    MetalRenderOptions opts;
    compute_draw_region(fb_w, fb_h, &opts.draw_region_w, &opts.draw_region_h);
    g_renderer->render_frame(opts, m_layer);
  }

  // mark the chain as rendered so sync_path can return (GL does this under the
  // dma mutex with the sync cv; mirrored here)
  {
    std::unique_lock<std::mutex> lock(g_chain.dma_mutex);
    g_chain.has_data_to_render = false;
    g_chain.sync_cv.notify_all();
  }

  // toggle even/odd and wake up the engine waiting on vsync
  {
    std::unique_lock<std::mutex> lock(g_chain.sync_mutex);
    g_chain.frame_idx++;
    g_chain.sync_cv.notify_all();
  }
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

ChainStats get_chain_stats() {
  return g_renderer ? g_renderer->chain_stats() : ChainStats{};
}

void set_s7_override(u32 s7_ptr) {
  metal_set_s7_override(s7_ptr);
}

void set_window_hidden(bool hidden) {
  g_hidden_window = hidden;
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

LevelLoadResult load_level_fr3(const std::string& path, bool is_common) {
  LevelLoadResult result;
  if (!g_renderer || !g_texture_pool) {
    result.error = "no Metal display has been created";
    return result;
  }
  auto* level = metal_level_data::load_fr3(g_renderer->device(), g_renderer->queue(),
                                           *g_texture_pool, path, is_common, &result.error);
  if (!level) {
    return result;
  }
  result.ok = true;
  result.level_name = level->level->level_name;
  result.textures = (int)level->textures.size();
  result.tfrag_trees = (int)level->tfrag[0].size();
  result.tie_trees = (int)level->tie[0].size();
  result.shrub_trees = (int)level->shrub.size();
  result.vertex_bytes = level->vertex_bytes;
  result.index_bytes = level->index_bytes;
  return result;
}

void unload_all_levels() {
  metal_level_data::clear();
}

BackgroundStats get_background_stats() {
  BackgroundStats out;
  if (!g_renderer) {
    return out;
  }
  const auto& bg = g_renderer->background_state();
  out.tfrag_draws = bg.tfrag_draws;
  out.tfrag_tris = bg.tfrag_tris;
  out.tie_draws = bg.tie_draws;
  out.tie_tris = bg.tie_tris;
  out.shrub_draws = bg.shrub_draws;
  out.shrub_tris = bg.shrub_tris;
  out.missing_levels = bg.missing_levels;
  out.missing_textures = bg.missing_textures;
  out.anim_slot_draws = bg.anim_slot_draws;
  out.unexpected_dma = bg.unexpected_dma;
  return out;
}

void interp_time_of_day_for_test(const math::Vector<s32, 4> itimes[4],
                                 const tfrag3::PackedTimeOfDay& colors,
                                 math::Vector<u8, 4>* out) {
  metal_interp_time_of_day(itimes, colors, out);
}

void cull_check_all_slow_for_test(const math::Vector4f* planes,
                                  const std::vector<tfrag3::VisNode>& nodes,
                                  const u8* level_occlusion_string,
                                  u8* out) {
  metal_cull_check_all_slow(planes, nodes, level_occlusion_string, out);
}

size_t sizeof_pc_port_data_mirror() {
  return sizeof(MetalTfragPcPortData);
}

size_t sizeof_camera_data_mirror() {
  return sizeof(MetalGoalBackgroundCameraData);
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
  SDL_WindowFlags flags =
      SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  if (g_hidden_window) {
    // headless: the CAMetalLayer still renders and can be read back, but no
    // window appears or takes focus. Used by the proof/replay tools, which
    // verify by readback rather than by looking at the screen.
    flags |= SDL_WINDOW_HIDDEN;
  }
  SDL_Window* window = SDL_CreateWindow(title, width, height, flags);
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
    g_renderer->init_bucket_renderers(g_texture_pool.get(), version);
  }
  if (!g_chain.copier) {
    g_chain.copier = std::make_unique<FixedChunkDmaCopier>(EE_MAIN_MEM_SIZE);
  }

  return std::make_shared<MetalDisplay>(window, view, layer, is_main);
}

static void metal_exit() {
  g_texture_pool.reset();
  g_chain.copier.reset();
  g_chain.has_data_to_render = false;
  delete g_renderer;
  g_renderer = nullptr;
}

/*!
 * Wait for the next vsync. Returns 0 or 1 depending on if frame is even or odd.
 * Called from the game thread. Mirror of gl_vsync.
 */
static u32 metal_vsync() {
  if (!g_renderer) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_chain.sync_mutex);
  auto init_frame = g_chain.frame_idx_of_input_data;
  g_chain.sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_chain.frame_idx > init_frame;
  });
  return g_chain.frame_idx & 1;
}

/*!
 * Mirror of gl_sync_path: block the game thread until the renderer consumed
 * the pending chain.
 */
static u32 metal_sync_path() {
  if (!g_renderer) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_chain.sync_mutex);
  if (!g_chain.has_data_to_render) {
    return 0;
  }
  g_chain.sync_cv.wait(lock, [] { return !g_chain.has_data_to_render; });
  return 0;
}

/*!
 * Send DMA to the renderer. Called from the game thread. Like gl_send_chain,
 * but the chain copy always runs: the renderer works from a stable snapshot
 * that is much smaller than the whole game memory.
 */
static void metal_send_chain(const void* data, u32 offset) {
  if (!g_renderer || !g_chain.copier) {
    return;
  }
  std::unique_lock<std::mutex> lock(g_chain.dma_mutex);
  if (g_chain.has_data_to_render) {
    lg::error(
        "Gfx::send_chain called when the Metal renderer has pending data. Was this called "
        "multiple times per frame?");
    return;
  }

  g_chain.copier->set_input_data(data, offset, /*run_copy*/ true);
  g_chain.has_data_to_render = true;
  g_chain.dma_cv.notify_all();
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
static void metal_set_pmode_alp(float alp) {
  g_chain.pmode_alp = alp;
}

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
