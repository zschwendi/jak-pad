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

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FrameLimiter.h"
#include "common/util/Timer.h"

#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_merc_model_pool.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

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
  // Which thread renders. A host that renders on the thread it sends from (the proof, and any
  // single-threaded display-link driver) must not be made to wait for itself.
  std::atomic<bool> render_thread_known{false};
  std::atomic<std::thread::id> render_thread{};
};
ChainSync g_chain;

// The level art the running game asks for through __pc-set-levels, and what is
// on the GPU because of it. The game thread writes `wanted`; the render thread
// reads it and does the loading and evicting, at the top of a frame, before
// that frame's render pass opens. Doing both there rather than on a loader
// thread is what keeps this correct with no locking around the level registry
// or the texture pool: nothing is drawing while a level appears or disappears.
// The cost is that the renderer stalls for the length of a load, which the
// game sees as one very long frame, the same way it sees any slow frame.
struct LoadedLevelArt {
  std::string name;
  bool ok = false;                // failed loads stay listed so they are not retried
  int frames_since_wanted = 0;    // 0 while __pc-set-levels names the level
};

struct LevelArt {
  std::mutex mutex;
  std::string directory;
  std::vector<std::string> wanted;
  bool wanted_changed = false;
  bool reported_no_directory = false;
  bool common_loaded = false;
  std::vector<LoadedLevelArt> loaded;  // in load order, including ones that failed
  metal_renderer::LevelArtStats stats;
};
LevelArt g_level_art;

// The GL Loader's residency rule (Loader::update / get_most_unloadable_level),
// with the same numbers: art is evicted only once the game has not wanted the
// level for 180 frames and at least as many levels are resident as the game
// has level slots (fr3_level_count, jak1::LEVEL_TOTAL). The wanted set is the
// load state's own slots, so residency is bounded by those slots plus this
// small cache, not by how many levels a session visits.
constexpr int kMaxResidentLevels = jak1::LEVEL_TOTAL;
constexpr int kEvictAfterFrames = 180;

// Frame pacing and its measurement. `g_present_min_duration` is what the render
// thread asks Metal to hold each drawable for; the samples are the intervals
// between the render thread's own passes, which is the cadence the game is
// paced by.
double g_present_min_duration = 0.0;
std::mutex g_timing_mutex;
std::vector<double> g_present_intervals_ms;

std::string join_plus(const std::vector<std::string>& names) {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) {
      out += '+';
    }
    out += name;
  }
  return out;
}

std::string join_plus(const std::vector<LoadedLevelArt>& levels) {
  std::string out;
  for (const auto& level : levels) {
    if (!out.empty()) {
      out += '+';
    }
    out += level.name;
  }
  return out;
}

// Loads one level's art into both places it has to go: the background
// renderers' level registry (tfrag / tie / shrub geometry plus the level's
// textures) and the merc model pool (the same level's character geometry).
// A level that will not load is reported, never treated as loaded.
bool load_level_art(const std::string& name, bool is_common) {
  const std::string path = g_level_art.directory + "/" + name + ".fr3";
  Timer timer;
  auto result = metal_renderer::load_level_fr3(path, is_common);
  if (!result.ok) {
    lg::error("Metal: could not load level art {}: {}", path, result.error);
    return false;
  }
  metal_renderer::MercLevelLoad merc;
  std::string error;
  if (!metal_renderer::merc_load_fr3(path, is_common, &merc, &error)) {
    metal_level_data::unload(*g_texture_pool, result.level_name);
    lg::error("Metal: could not load merc models from {}: {}", path, error);
    return false;
  }
  const double ms = timer.getMs();
  lg::info(
      "Metal: loaded level art {} in {:.0f} ms - {} textures, {}/{}/{} tfrag/tie/shrub trees, "
      "{} merc models, {:.1f} MB vertices",
      name, ms, result.textures, result.tfrag_trees, result.tie_trees, result.shrub_trees,
      merc.models, result.vertex_bytes / (1024.0 * 1024.0));
  g_level_art.stats.last_load_ms = ms;
  return true;
}

// Ages every loaded level against the wanted set and evicts at most one per
// frame, the way the GL Loader's update does. Runs on the render thread
// between frames, before anything draws, so no draw can be reading a level
// that disappears; command buffers already committed retain what they
// reference, so in-flight GPU work keeps its resources until it retires.
void evict_unwanted_level_art(const std::vector<std::string>& wanted) {
  std::string victim;
  int victim_age = 0;
  {
    std::lock_guard<std::mutex> lock(g_level_art.mutex);
    int resident = 0;
    for (auto& entry : g_level_art.loaded) {
      const bool is_wanted =
          std::find(wanted.begin(), wanted.end(), entry.name) != wanted.end();
      entry.frames_since_wanted = is_wanted ? 0 : entry.frames_since_wanted + 1;
      if (entry.ok) {
        resident++;
      }
    }
    if (resident < kMaxResidentLevels) {
      return;
    }
    auto oldest = g_level_art.loaded.end();
    for (auto it = g_level_art.loaded.begin(); it != g_level_art.loaded.end(); ++it) {
      if (it->ok && it->frames_since_wanted > kEvictAfterFrames &&
          (oldest == g_level_art.loaded.end() ||
           it->frames_since_wanted > oldest->frames_since_wanted)) {
        oldest = it;
      }
    }
    if (oldest == g_level_art.loaded.end()) {
      return;
    }
    victim = oldest->name;
    victim_age = oldest->frames_since_wanted;
    g_level_art.loaded.erase(oldest);
    g_level_art.stats.levels_evicted++;
    g_level_art.stats.loaded = join_plus(g_level_art.loaded);
  }

  // Outside the bookkeeping lock: the unloads take the texture pool's own
  // mutex, and hold no GPU wait under it.
  metal_level_data::unload(*g_texture_pool, victim);
  metal_merc_models().remove_level(victim);
  lg::info("Metal: evicted level art {} after {} frames unwanted", victim, victim_age);
}

// Render-thread half of __pc-set-levels. Loads whatever the game has asked for
// and does not have yet, and evicts what the game has stopped asking for (see
// evict_unwanted_level_art). Runs at the top of every frame.
void service_level_requests() {
  std::vector<std::string> wanted;
  bool load_pass = false;
  {
    std::lock_guard<std::mutex> lock(g_level_art.mutex);
    if (g_level_art.directory.empty()) {
      if (!g_level_art.wanted.empty() && !g_level_art.reported_no_directory) {
        g_level_art.reported_no_directory = true;
        lg::error(
            "Metal: the game asked for level art ({}) but no level art directory is set; "
            "frames will draw with placeholder textures and no world geometry",
            join_plus(g_level_art.wanted));
      }
      return;
    }
    wanted = g_level_art.wanted;
    if (g_level_art.wanted_changed || !g_level_art.common_loaded) {
      g_level_art.wanted_changed = false;
      load_pass = true;
    }
  }

  if (load_pass) {
    for (const auto& name : wanted) {
      {
        std::lock_guard<std::mutex> lock(g_level_art.mutex);
        if (std::find_if(g_level_art.loaded.begin(), g_level_art.loaded.end(),
                         [&](const LoadedLevelArt& l) { return l.name == name; }) !=
            g_level_art.loaded.end()) {
          continue;
        }
      }
      const bool ok = load_level_art(name, false);
      std::lock_guard<std::mutex> lock(g_level_art.mutex);
      // Remembered either way, so a level whose art will not load is reported
      // once rather than retried every frame.
      g_level_art.loaded.push_back({name, ok, 0});
      if (ok) {
        g_level_art.stats.levels_loaded++;
        g_level_art.stats.loaded = join_plus(g_level_art.loaded);
      } else {
        g_level_art.stats.load_failures++;
      }
    }
  }

  evict_unwanted_level_art(wanted);
}

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
 * Display for the Metal pipeline: an SDL window and its CAMetalLayer.
 *
 * Unlike GLDisplay this owns no DisplayManager and no InputManager. Those two read and *write*
 * the PC port's own display and input settings files, and their window management (saved display
 * mode, saved monitor and window position) belongs to a launcher rather than to a renderer
 * backend. A host reads its own input - `game/goalpad_play.cpp` does, straight from SDL, and the
 * iPadOS bridge will do it from GameController - so nothing here needs them. `get_*_manager`
 * therefore answer null, which is what "this display does not manage input" has to look like
 * through the GfxDisplay interface.
 */
class MetalDisplay : public GfxDisplay {
 public:
  MetalDisplay(SDL_Window* window, SDL_MetalView view, CAMetalLayer* layer, bool is_main)
      : m_window(window), m_view(view), m_layer(layer) {
    m_main = is_main;
  }

  virtual ~MetalDisplay() {
    SDL_Metal_DestroyView(m_view);
    SDL_DestroyWindow(m_window);
  }

  std::shared_ptr<DisplayManager> get_display_manager() const override { return nullptr; }
  std::shared_ptr<InputManager> get_input_manager() const override { return nullptr; }

  void render() override;
  void init_splash() override {}
  void draw_splash(int /*fb_w*/, int /*fb_h*/) override {}

 private:
  // Drains the queue so SDL keeps its keyboard and gamepad state current (which is what the host
  // reads) and so the window stays responsive. Closing the window asks the game to stop, the same
  // way GLDisplay::process_sdl_events does.
  void process_sdl_events() {
    SDL_Event evt;
    while (SDL_PollEvent(&evt) != 0) {
      if (evt.type == SDL_EVENT_QUIT ||
          (evt.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
           evt.window.windowID == SDL_GetWindowID(m_window))) {
        MasterExit = RuntimeExitStatus::EXIT;
        std::unique_lock<std::mutex> lock(g_chain.sync_mutex);
        g_chain.sync_cv.notify_all();
      }
    }
  }

  SDL_Window* m_window;
  SDL_MetalView m_view;
  CAMetalLayer* m_layer;
};

void MetalDisplay::render() {
  g_chain.render_thread = std::this_thread::get_id();
  g_chain.render_thread_known = true;
  process_sdl_events();
  if (!g_renderer) {
    return;
  }
  int fb_w = 0;
  int fb_h = 0;
  SDL_GetWindowSizeInPixels(m_window, &fb_w, &fb_h);

  // Whatever level art the game has asked for since the last frame, before
  // anything starts drawing from it.
  service_level_requests();

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
    // The largest centred 4:3 region that fits the window, in drawable pixels. Upstream the game
    // sets this itself through `pc-set-letterbox`, which this machine layer does not implement;
    // until it does, the window decides, which is what a host with no settings menu should do.
    // The game frame is scaled into it, so the internal resolution and the window size are
    // independent - exactly the pair the iPad app needs.
    compute_draw_region(fb_w, fb_h, &opts.draw_region_w, &opts.draw_region_h);
    opts.pmode_alp = g_chain.pmode_alp;
    opts.brightness_contrast_color = Gfx::g_global_settings.brightness_contrast_color;
    opts.brightness_contrast_alpha = Gfx::g_global_settings.brightness_contrast_alpha;
    opts.min_present_duration = g_present_min_duration;

    const auto& chain = g_chain.copier->get_last_result();
    g_renderer->render_chain_frame(opts, m_layer, chain.data.data(), chain.start_offset);
  } else {
    MetalRenderOptions opts;
    compute_draw_region(fb_w, fb_h, &opts.draw_region_w, &opts.draw_region_h);
    g_renderer->render_frame(opts, m_layer);
  }

  // Hold the frame to the target rate before releasing the game thread, exactly where the GL
  // pipeline runs it: the game's clock is `syncv` returning, so pacing here is what makes the
  // game run at its own speed on a display of any refresh rate. Without it a 120 Hz display runs
  // Jak at twice speed.
  if (Gfx::g_global_settings.framelimiter) {
    static FrameLimiter limiter;
    limiter.run(Gfx::g_global_settings.target_fps, Gfx::g_global_settings.experimental_accurate_lag,
                Gfx::g_global_settings.sleep_in_frame_limiter, 1.0 / 60.0);
  }
  {
    static Timer since_last_present;
    static bool have_previous = false;
    const double ms = since_last_present.getMs();
    since_last_present.start();
    if (have_previous) {
      std::lock_guard<std::mutex> lock(g_timing_mutex);
      g_present_intervals_ms.push_back(ms);
    }
    have_previous = true;
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

bool render_last_chain_to_external_target(int width,
                                          int height,
                                          ExternalRenderTargetProofResult* out) {
  if (!g_renderer || !g_chain.copier || !out || width <= 0 || height <= 0) {
    return false;
  }
  *out = {};
  const auto& chain = g_chain.copier->get_last_result();
  if (chain.data.empty()) {
    return false;
  }

  @autoreleasepool {
    auto* color_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    color_desc.textureType = MTLTextureType2DArray;
    color_desc.arrayLength = 3;
    color_desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
    color_desc.storageMode = MTLStorageModeManaged;
#else
    color_desc.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> color = [g_renderer->device() newTextureWithDescriptor:color_desc];

    auto* depth_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    depth_desc.textureType = MTLTextureType2DArray;
    depth_desc.arrayLength = 3;
    depth_desc.usage = MTLTextureUsageRenderTarget;
    depth_desc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> depth = [g_renderer->device() newTextureWithDescriptor:depth_desc];
    if (!color || !depth) {
      return false;
    }

    constexpr u8 kSentinelBgra[4] = {0x35, 0x6a, 0xa5, 0xff};
    std::vector<u8> sentinel((size_t)width * height * 4);
    for (size_t i = 0; i < sentinel.size(); i += 4) {
      std::memcpy(sentinel.data() + i, kSentinelBgra, sizeof(kSentinelBgra));
    }
    [color replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
                   slice:0
               withBytes:sentinel.data()
             bytesPerRow:width * 4
           bytesPerImage:sentinel.size()];

    // Exercise the same pass split the sprite distorter uses. The first pass clears slice 1,
    // resume_pass_with_framebuffer_copy must copy that slice and reopen the selected attachments.
    auto* snapshot_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    snapshot_desc.usage = MTLTextureUsageShaderRead;
#if TARGET_OS_OSX
    snapshot_desc.storageMode = MTLStorageModeManaged;
#else
    snapshot_desc.storageMode = MTLStorageModeShared;
#endif
    id<MTLTexture> snapshot = [g_renderer->device() newTextureWithDescriptor:snapshot_desc];
    id<MTLCommandBuffer> split_cmds = [g_renderer->queue() commandBuffer];
    auto* split_pass = [MTLRenderPassDescriptor renderPassDescriptor];
    split_pass.colorAttachments[0].texture = color;
    split_pass.colorAttachments[0].slice = 1;
    split_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    split_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    split_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.125, 0.25, 0.5, 1.0);
    split_pass.depthAttachment.texture = depth;
    split_pass.depthAttachment.slice = 1;
    split_pass.depthAttachment.loadAction = MTLLoadActionClear;
    split_pass.depthAttachment.storeAction = MTLStoreActionStore;
    split_pass.depthAttachment.clearDepth = 0.0;
    split_pass.stencilAttachment.texture = depth;
    split_pass.stencilAttachment.slice = 1;
    split_pass.stencilAttachment.loadAction = MTLLoadActionClear;
    split_pass.stencilAttachment.storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> split_enc =
        [split_cmds renderCommandEncoderWithDescriptor:split_pass];
    MetalFrameContext split_ctx;
    split_ctx.enc = split_enc;
    split_ctx.cmds = split_cmds;
    split_ctx.game_color = color;
    split_ctx.game_color_slice = 1;
    split_ctx.game_depth = depth;
    split_ctx.game_depth_slice = 1;
    split_ctx.game_viewport = {0.0, 0.0, (double)width, (double)height, 0.0, 1.0};
    [split_enc setViewport:split_ctx.game_viewport];
    split_ctx.resume_pass_with_framebuffer_copy(snapshot);
    [split_ctx.enc endEncoding];
#if TARGET_OS_OSX
    id<MTLBlitCommandEncoder> split_sync = [split_cmds blitCommandEncoder];
    [split_sync synchronizeResource:snapshot];
    [split_sync endEncoding];
#endif
    [split_cmds commit];
    [split_cmds waitUntilCompleted];
    u8 split_pixel[4] = {};
    [snapshot getBytes:split_pixel
            bytesPerRow:sizeof(split_pixel)
             fromRegion:MTLRegionMake2D(0, 0, 1, 1)
            mipmapLevel:0];
    out->framebuffer_copy_used_selected_slice =
        split_cmds.status == MTLCommandBufferStatusCompleted && split_pixel[0] == 128 &&
        split_pixel[1] == 64 && split_pixel[2] == 32 && split_pixel[3] == 255;

    MetalRenderOptions opts;
    opts.game_res_w = width;
    opts.game_res_h = height;

    MetalExternalRenderTargetDescriptor target;
    target.view_id = 0x4255494c44313336ull;  // "BUILD136"
    target.color_texture = color;
    target.color_slice = 1;
    target.depth_texture = depth;
    target.depth_slice = 1;
    target.viewport = {0.0, 0.0, (double)width, (double)height, 0.0, 1.0};

    FramePixels internal_before;
    const bool read_internal_before = g_renderer->read_game_frame(&internal_before);
    const ScaffoldStats readback_before = g_renderer->stats();

    MTLTextureDescriptor* missing_color_usage_desc = [color_desc copy];
    missing_color_usage_desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> missing_color_usage =
        [g_renderer->device() newTextureWithDescriptor:missing_color_usage_desc];
    MTLTextureDescriptor* missing_depth_usage_desc = [depth_desc copy];
    missing_depth_usage_desc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> missing_depth_usage =
        [g_renderer->device() newTextureWithDescriptor:missing_depth_usage_desc];

    const ChainStats before_invalid = g_renderer->chain_stats();
    auto bad_bounds = target;
    bad_bounds.color_slice = color.arrayLength;
    auto bad_viewport = target;
    bad_viewport.viewport.originX = 1.0;
    auto bad_depth_clear = target;
    bad_depth_clear.clear_depth = 1.0;
    auto bad_color_usage = target;
    bad_color_usage.color_texture = missing_color_usage;
    auto bad_depth_usage = target;
    bad_depth_usage.depth_texture = missing_depth_usage;
    const bool rejected =
        missing_color_usage && missing_depth_usage &&
        !g_renderer->render_chain_frame_to_external_target(
            opts, bad_bounds, chain.data.data(), chain.start_offset) &&
        !g_renderer->render_chain_frame_to_external_target(
            opts, bad_viewport, chain.data.data(), chain.start_offset) &&
        !g_renderer->render_chain_frame_to_external_target(
            opts, bad_depth_clear, chain.data.data(), chain.start_offset) &&
        !g_renderer->render_chain_frame_to_external_target(
            opts, bad_color_usage, chain.data.data(), chain.start_offset) &&
        !g_renderer->render_chain_frame_to_external_target(
            opts, bad_depth_usage, chain.data.data(), chain.start_offset);
    const ChainStats after_invalid = g_renderer->chain_stats();
    static_assert(std::is_trivially_copyable_v<ChainStats>);

    out->invalid_descriptors_rejected = rejected;
    out->invalid_descriptors_preserved_stats =
        std::memcmp(&before_invalid, &after_invalid, sizeof(ChainStats)) == 0;
    const bool first_external = g_renderer->render_chain_frame_to_external_target(
        opts, target, chain.data.data(), chain.start_offset);
    const ScaffoldStats after_first_external = g_renderer->stats();
    const bool second_external =
        first_external && g_renderer->render_chain_frame_to_external_target(
                              opts, target, chain.data.data(), chain.start_offset);
    const ScaffoldStats after_second_external = g_renderer->stats();
    if (!out->framebuffer_copy_used_selected_slice || !rejected ||
        !out->invalid_descriptors_preserved_stats || !second_external ||
        !g_renderer->wait_for_last_chain_frame(5.0)) {
      return false;
    }
    out->view_id = target.view_id;

    FramePixels internal_after;
    const bool read_internal_after = g_renderer->read_game_frame(&internal_after);
    const ScaffoldStats readback_after = g_renderer->stats();
    out->internal_readback_identity_preserved =
        readback_before.last_internal_frame_submission != 0 &&
        readback_before.last_internal_frame_submission ==
            after_first_external.last_internal_frame_submission &&
        readback_before.last_internal_frame_submission ==
            after_second_external.last_internal_frame_submission &&
        readback_before.last_internal_frame_submission ==
            readback_after.last_internal_frame_submission;
    out->internal_readback_pixels_preserved =
        read_internal_before && read_internal_after && internal_before.width == internal_after.width &&
        internal_before.height == internal_after.height &&
        internal_before.rgba == internal_after.rgba;
    out->internal_readback_bookkeeping_preserved =
        readback_before.frames_rendered == after_first_external.frames_rendered &&
        readback_before.frames_rendered == after_second_external.frames_rendered &&
        readback_before.frames_rendered == readback_after.frames_rendered;
    out->stream_reuse_synchronized =
        after_second_external.stream_reuse_waits == after_first_external.stream_reuse_waits + 1;

    auto stereo_left = target;
    stereo_left.view_id = 0x4255494c4431344cull;  // "BUILD14L"
    auto stereo_right = target;
    stereo_right.view_id = 0x4255494c44313452ull;  // "BUILD14R"
    stereo_right.color_slice = 2;
    stereo_right.depth_slice = 2;

    auto aliased_right = stereo_right;
    aliased_right.color_slice = stereo_left.color_slice;
    aliased_right.depth_slice = stereo_left.depth_slice;
    const ChainStats before_invalid_batch = g_renderer->chain_stats();
    const bool aliased_batch_rejected = !g_renderer->render_chain_frame_to_external_stereo_targets(
        opts, stereo_left, aliased_right, chain.data.data(), chain.start_offset);
    auto nonfinite_right = stereo_right;
    nonfinite_right.view_transform.clip_from_game_clip[0] = std::numeric_limits<float>::quiet_NaN();
    const bool nonfinite_batch_rejected =
        !g_renderer->render_chain_frame_to_external_stereo_targets(
            opts, stereo_left, nonfinite_right, chain.data.data(), chain.start_offset);
    const ChainStats after_invalid_batch = g_renderer->chain_stats();
    out->stereo_invalid_batch_rejected =
        aliased_batch_rejected && nonfinite_batch_rejected &&
        std::memcmp(&before_invalid_batch, &after_invalid_batch, sizeof(ChainStats)) == 0;

    std::vector<u8> stereo_chain = chain.data;
    const ChainStats before_stereo = g_renderer->chain_stats();
    const bool stereo_rendered = g_renderer->render_chain_frame_to_external_stereo_targets(
        opts, stereo_left, stereo_right, stereo_chain.data(), chain.start_offset);
    const ChainStats after_stereo = g_renderer->chain_stats();
    std::fill(stereo_chain.begin(), stereo_chain.end(), 0xa5);
    const bool stereo_completed = stereo_rendered && g_renderer->wait_for_last_chain_frame(5.0);
    out->stereo_side_effects_single_shot =
        stereo_rendered && after_stereo.chains_rendered == before_stereo.chains_rendered + 1 &&
        after_stereo.last_views_rendered == 2 &&
        after_stereo.last_frame_global_callbacks == after_stereo.last_buckets_dispatched;

    std::vector<u8> slice_zero(sentinel.size());
    [color getBytes:slice_zero.data()
         bytesPerRow:width * 4
       bytesPerImage:slice_zero.size()
          fromRegion:MTLRegionMake2D(0, 0, width, height)
         mipmapLevel:0
               slice:0];
    out->color_slice_zero_preserved = slice_zero == sentinel;

    std::vector<u8> bgra(sentinel.size());
    [color getBytes:bgra.data()
         bytesPerRow:width * 4
       bytesPerImage:bgra.size()
          fromRegion:MTLRegionMake2D(0, 0, width, height)
         mipmapLevel:0
               slice:1];
    out->rendered_slice.width = width;
    out->rendered_slice.height = height;
    out->rendered_slice.rgba.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
      out->rendered_slice.rgba[i + 0] = bgra[i + 2];
      out->rendered_slice.rgba[i + 1] = bgra[i + 1];
      out->rendered_slice.rgba[i + 2] = bgra[i + 0];
      out->rendered_slice.rgba[i + 3] = bgra[i + 3];
    }

    std::vector<u8> stereo_right_bgra(sentinel.size());
    [color getBytes:stereo_right_bgra.data()
          bytesPerRow:width * 4
        bytesPerImage:stereo_right_bgra.size()
           fromRegion:MTLRegionMake2D(0, 0, width, height)
          mipmapLevel:0
                slice:2];
    out->stereo_right_slice.width = width;
    out->stereo_right_slice.height = height;
    out->stereo_right_slice.rgba.resize(stereo_right_bgra.size());
    for (size_t i = 0; i < stereo_right_bgra.size(); i += 4) {
      out->stereo_right_slice.rgba[i + 0] = stereo_right_bgra[i + 2];
      out->stereo_right_slice.rgba[i + 1] = stereo_right_bgra[i + 1];
      out->stereo_right_slice.rgba[i + 2] = stereo_right_bgra[i + 0];
      out->stereo_right_slice.rgba[i + 3] = stereo_right_bgra[i + 3];
    }
    out->stereo_identity_preserved = stereo_completed &&
                                     out->rendered_slice.rgba == internal_before.rgba &&
                                     out->stereo_right_slice.rgba == internal_before.rgba;
    out->stereo_poison_after_encode_preserved =
        stereo_completed && out->rendered_slice.rgba == internal_before.rgba &&
        out->stereo_right_slice.rgba == internal_before.rgba;
    return true;
  }
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

size_t texture_registry_live_count() {
  return metal_texture_live_count();
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
  if (!g_texture_pool) {
    return;
  }
  metal_level_data::clear(*g_texture_pool);
  metal_merc_models().clear();
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
  out.tie_envmap_second_draws = bg.tie_envmap_second_draws;
  out.tie_envmap_second_tris = bg.tie_envmap_second_tris;
  out.tie_wind_draws_skipped = bg.tie_wind_draws_skipped;
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

void background_camera_matrix_for_test(const math::Vector4f rotation[4],
                                       const math::Vector4f perspective[4],
                                       float fog_constant,
                                       float hvdf_z,
                                       math::Vector4f out[4]) {
  MetalGoalBackgroundCameraData camera = {};
  std::memcpy(camera.rot, rotation, sizeof(camera.rot));
  std::memcpy(camera.perspective, perspective, sizeof(camera.perspective));
  camera.fog.x() = fog_constant;
  camera.hvdf_off.z() = hvdf_z;

  MetalBackgroundVsParams params = {};
  metal_fill_background_vs_params(camera, GameVersion::Jak1, &params);
  std::memcpy(out, params.pc_camera, sizeof(params.pc_camera));
}

size_t sizeof_pc_port_data_mirror() {
  return sizeof(MetalTfragPcPortData);
}

size_t sizeof_camera_data_mirror() {
  return sizeof(MetalGoalBackgroundCameraData);
}

bool merc_load_fr3(const std::string& path,
                   bool is_common,
                   MercLevelLoad* out,
                   std::string* error) {
  MetalMercModelPool::LoadResult result;
  if (!metal_merc_models().load_fr3(path, is_common, &result, error)) {
    return false;
  }
  out->level_name = result.level_name;
  out->textures = result.textures;
  out->models = result.models;
  out->vertices = result.vertices;
  out->indices = result.indices;
  return true;
}

bool merc_add_level(std::unique_ptr<tfrag3::Level> level,
                    bool is_common,
                    MercLevelLoad* out,
                    std::string* error) {
  MetalMercModelPool::LoadResult result;
  if (!metal_merc_models().add_level(std::move(level), is_common, &result, error)) {
    return false;
  }
  out->level_name = result.level_name;
  out->textures = result.textures;
  out->models = result.models;
  out->vertices = result.vertices;
  out->indices = result.indices;
  return true;
}

void set_present_pacing(double seconds) {
  g_present_min_duration = seconds;
}

PresentTiming take_present_timing() {
  std::vector<double> samples;
  {
    std::lock_guard<std::mutex> lock(g_timing_mutex);
    samples.swap(g_present_intervals_ms);
  }
  PresentTiming out;
  out.frames = (int)samples.size();
  if (samples.empty()) {
    return out;
  }
  out.min_ms = samples[0];
  out.max_ms = samples[0];
  double total = 0;
  for (double ms : samples) {
    total += ms;
    out.min_ms = std::min(out.min_ms, ms);
    out.max_ms = std::max(out.max_ms, ms);
  }
  out.mean_ms = total / samples.size();
  double variance = 0;
  for (double ms : samples) {
    variance += (ms - out.mean_ms) * (ms - out.mean_ms);
    if (ms > out.mean_ms * 1.5) {
      out.late_frames++;
    }
  }
  out.stddev_ms = std::sqrt(variance / samples.size());
  return out;
}

void set_level_art_directory(const std::string& path) {
  std::lock_guard<std::mutex> lock(g_level_art.mutex);
  g_level_art.directory = path;
}

bool load_common_level_art() {
  {
    std::lock_guard<std::mutex> lock(g_level_art.mutex);
    if (g_level_art.directory.empty() || g_level_art.common_loaded) {
      return g_level_art.common_loaded;
    }
    g_level_art.common_loaded = true;
  }
  const bool ok = load_level_art("GAME", true);
  std::lock_guard<std::mutex> lock(g_level_art.mutex);
  if (!ok) {
    g_level_art.common_loaded = false;
    g_level_art.stats.load_failures++;
  }
  return ok;
}

LevelArtStats get_level_art_stats() {
  std::lock_guard<std::mutex> lock(g_level_art.mutex);
  return g_level_art.stats;
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
  metal_renderer::unload_all_levels();
  metal_merc_models().shutdown();
  if (g_texture_pool) {
    metal_texture_release(g_texture_pool->get_placeholder_texture());
  }
  g_texture_pool.reset();
  {
    std::lock_guard<std::mutex> lock(g_level_art.mutex);
    g_level_art.directory.clear();
    g_level_art.wanted.clear();
    g_level_art.wanted_changed = false;
    g_level_art.reported_no_directory = false;
    g_level_art.common_loaded = false;
    g_level_art.loaded.clear();
    g_level_art.stats = {};
  }
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
  // `has_data_to_render` is written under dma_mutex, so it has to be waited on under dma_mutex:
  // waiting under sync_mutex left the predicate unsynchronized with the thread that clears it.
  std::unique_lock<std::mutex> lock(g_chain.dma_mutex);
  g_chain.sync_cv.wait(lock, [] { return !g_chain.has_data_to_render || !g_renderer; });
  return 0;
}

/*!
 * Send DMA to the renderer. Called from the game thread. Like gl_send_chain,
 * but the chain copy always runs: the renderer works from a stable snapshot
 * that is much smaller than the whole game memory.
 *
 * The copy is only *most* of the frame. Merc resolves its bone matrices by EE pointer out of live
 * game memory - it has to, because the bone arrays are referenced from inside a packet rather than
 * transferred by a DMA tag, so the copier never marked their chunks (`FixedChunkDmaCopier::run`
 * only marks what the tags touch). The GL pipeline gets away with the same read because it does
 * not copy at all (`run_dma_copy = false`): there, chain and bones are both read live, at the same
 * moment, while the game waits.
 *
 * Here they were read at two different moments, and measurement showed the skeleton had already
 * moved on by the time the frame was drawn - on about a third of frames, which is a character
 * visibly shivering on a still pose. So the game waits here for the frame it just built to be
 * consumed, which is the same guarantee GL has: nothing the renderer still needs is rewritten
 * underneath it. The cost is the game thread no longer running ahead of the renderer.
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

  // Hold the game here until the renderer has read the frame, so the bone matrices it resolves
  // live are the ones that belong to this chain. Only when something else renders: a host that
  // renders on this same thread already has the guarantee, and waiting would be waiting for
  // itself. Bounded, so a renderer that has stopped never strands the game thread.
  if (g_chain.render_thread_known.load() &&
      g_chain.render_thread.load() != std::this_thread::get_id()) {
    while (g_chain.has_data_to_render && MasterExit == RuntimeExitStatus::RUNNING) {
      g_chain.sync_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
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
/*!
 * The levels whose art the renderer should have ready, from the game's own
 * `__pc-set-levels`. Called from the game thread every frame; the loading
 * happens on the render thread (see service_level_requests).
 */
static void metal_set_levels(const std::vector<std::string>& levels) {
  std::lock_guard<std::mutex> lock(g_level_art.mutex);
  if (levels == g_level_art.wanted) {
    return;
  }
  g_level_art.wanted = levels;
  g_level_art.wanted_changed = true;
  g_level_art.stats.requests++;
  g_level_art.stats.wanted = join_plus(levels);
}
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
