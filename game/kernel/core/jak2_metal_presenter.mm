#include "game/kernel/core/jak2_metal_presenter.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_jak2_synthetic_chain.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <TargetConditionals.h>

namespace {

constexpr uint64_t kMaximumRenderAttempts = 1;
constexpr double kCompletionTimeoutSeconds = 5.0;

struct PresenterState {
  CAMetalLayer* layer = nil;
  std::unique_ptr<MetalRenderer> renderer;
  std::unique_ptr<TexturePool> texture_pool;
  std::vector<u8> chain;
  uint64_t render_attempts = 0;
  std::string error;
};

PresenterState g_presenter;

bool fail(const std::string& error) {
  g_presenter.error = error;
  return false;
}

int fail_result(const std::string& error) {
  fail(error);
  return 0;
}

goal_jak2_metal_stats copy_stats() {
  goal_jak2_metal_stats out = {};
  out.render_attempts = g_presenter.render_attempts;
  if (!g_presenter.renderer) {
    return out;
  }
  const auto stats = g_presenter.renderer->chain_stats();
  out.chains_rendered = stats.chains_rendered;
  out.buckets_dispatched = stats.last_buckets_dispatched;
  out.drawables_acquired = stats.drawables_acquired;
  out.drawable_misses = stats.drawable_misses;
  out.command_buffers_committed = stats.command_buffers_committed;
  out.command_buffers_completed = stats.command_buffers_completed;
  out.command_buffer_errors = stats.command_buffer_errors;
  out.late_present_submissions = stats.late_present_submissions;
  out.submissions = stats.submissions;
  out.presentations_completed = stats.presentations_completed;
  out.presentation_drops = stats.presentation_drops;
  out.presentation_order_mismatches = stats.presentation_order_mismatches;
  out.skipped_bucket_bytes = stats.skipped_bucket_bytes;
  out.draw_calls = stats.draw_calls;
  out.triangles = stats.triangles;
  out.last_command_buffer_status = stats.last_command_buffer_status;
  out.last_command_buffer_error_code = stats.last_command_buffer_error_code;
  return out;
}

bool counters_are_valid(const goal_jak2_metal_stats& stats) {
  if (stats.render_attempts > kMaximumRenderAttempts ||
      stats.chains_rendered != stats.render_attempts ||
      stats.command_buffers_committed != stats.render_attempts ||
      stats.drawables_acquired != stats.submissions ||
      stats.command_buffers_completed + stats.command_buffer_errors >
          stats.command_buffers_committed ||
      stats.presentations_completed > stats.submissions) {
    return fail("Metal presentation counters violated their bounded invariants.");
  }
  if (stats.buckets_dispatched != metal_renderer::kJak2SyntheticBucketCount) {
    return fail("The synthetic Jak II chain did not dispatch all 327 buckets.");
  }
  if (stats.drawable_misses != 0 || stats.late_present_submissions != 0 ||
      stats.command_buffer_errors != 0 || stats.presentation_drops != 0 ||
      stats.presentation_order_mismatches != 0) {
    return fail("Metal reported a drawable, scheduling, command-buffer, or presentation error.");
  }
  if (stats.skipped_bucket_bytes != 16 || stats.draw_calls != 0 || stats.triangles != 0) {
    return fail("The public Jak II proof chain changed its audited no-draw behavior.");
  }
  return true;
}

}  // namespace

int goal_jak2_metal_presenter_start(goal_jak2_metal_layer layer) {
  @autoreleasepool {
    if (g_presenter.renderer) {
      return fail_result("The Jak II Metal presenter is already running.");
    }
    if (!layer) {
      return fail_result("The Jak II Metal presenter requires a CAMetalLayer.");
    }
    if (layer.drawableSize.width < 1.0 || layer.drawableSize.height < 1.0) {
      return fail_result("The Jak II CAMetalLayer has no drawable size.");
    }

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      return fail_result("No default Metal device is available.");
    }

    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.presentsWithTransaction = NO;
    layer.allowsNextDrawableTimeout = YES;

    auto renderer = std::make_unique<MetalRenderer>();
    if (!renderer->init(device)) {
      return fail_result("The existing Metal renderer did not initialize.");
    }
    auto texture_pool = std::make_unique<TexturePool>(GameVersion::Jak2);
    if (!metal_setup_placeholder(renderer->device(), renderer->queue(), *texture_pool)) {
      return fail_result("The Jak II Metal placeholder texture did not initialize.");
    }
    renderer->init_bucket_renderers(texture_pool.get(), GameVersion::Jak2);

    g_presenter.layer = layer;
    g_presenter.renderer = std::move(renderer);
    g_presenter.texture_pool = std::move(texture_pool);
    g_presenter.chain = metal_renderer::make_jak2_synthetic_metal_chain();
    g_presenter.render_attempts = 0;
    g_presenter.error.clear();
    return 1;
  }
}

int goal_jak2_metal_presenter_render(double target_presentation_time) {
  @autoreleasepool {
    if (!g_presenter.renderer || !g_presenter.layer) {
      return fail_result("The Jak II Metal presenter is not running.");
    }
    if (g_presenter.render_attempts >= kMaximumRenderAttempts) {
      return fail_result("The Jak II Metal presenter accepts exactly one proof frame.");
    }
    if (!std::isfinite(target_presentation_time) || target_presentation_time != 0.0) {
      return fail_result("The deterministic proof requires immediate Metal presentation time.");
    }

    const CGSize drawable_size = g_presenter.layer.drawableSize;
    if (drawable_size.width < 1.0 || drawable_size.height < 1.0) {
      return fail_result("The Jak II CAMetalLayer lost its drawable size.");
    }
    const int drawable_width = static_cast<int>(drawable_size.width);
    const int drawable_height = static_cast<int>(drawable_size.height);

    MetalRenderOptions options;
    if (drawable_width * 3 >= drawable_height * 4) {
      options.draw_region_h = drawable_height;
      options.draw_region_w = drawable_height * 4 / 3;
    } else {
      options.draw_region_w = drawable_width;
      options.draw_region_h = drawable_width * 3 / 4;
    }
    options.presentation_time = target_presentation_time;
    options.host_tick_id = g_presenter.render_attempts + 1;
    options.chain_ordinal = g_presenter.render_attempts + 1;
    options.engine_frame_id = g_presenter.render_attempts + 1;

    g_presenter.render_attempts++;
    const bool acquired = g_presenter.renderer->render_chain_frame(
        options, g_presenter.layer, g_presenter.chain.data(), 0);
    const auto stats = copy_stats();
    if (!acquired) {
      return fail_result("CAMetalLayer did not provide a drawable.");
    }
    return counters_are_valid(stats) ? 1 : 0;
  }
}

int goal_jak2_metal_presenter_wait_for_completion(void) {
  if (!g_presenter.renderer || g_presenter.render_attempts != 1) {
    return fail_result("No Jak II Metal proof frame is pending completion.");
  }
  if (!g_presenter.renderer->wait_for_last_chain_frame(kCompletionTimeoutSeconds)) {
    return fail_result(
        "The Jak II Metal proof frame did not complete successfully within five seconds.");
  }
#if !TARGET_OS_SIMULATOR
  if (!g_presenter.renderer->wait_for_last_presentation(kCompletionTimeoutSeconds)) {
    return fail_result(
        "The Jak II Metal proof drawable did not present successfully within five seconds.");
  }
#endif
  const auto stats = copy_stats();
  if (!counters_are_valid(stats) || stats.render_attempts != 1 || stats.chains_rendered != 1 ||
      stats.drawables_acquired != 1 || stats.command_buffers_committed != 1 ||
      stats.command_buffers_completed != 1 || stats.command_buffer_errors != 0 ||
      stats.submissions != 1) {
    return fail_result("The completed Jak II Metal proof frame did not satisfy exact counters.");
  }
#if TARGET_OS_SIMULATOR
  if (stats.presentations_completed != 0) {
    return fail_result("The simulator unexpectedly reported an unavailable drawable callback.");
  }
#else
  if (stats.presentations_completed != 1) {
    return fail_result("The physical drawable did not report exactly one presentation.");
  }
#endif
  return 1;
}

int goal_jak2_metal_presenter_get_stats(goal_jak2_metal_stats* out) {
  if (!out || !g_presenter.renderer) {
    return 0;
  }
  *out = copy_stats();
  return 1;
}

const char* goal_jak2_metal_presenter_last_error(void) {
  return g_presenter.error.c_str();
}

void goal_jak2_metal_presenter_shutdown(void) {
  g_presenter.renderer.reset();
  g_presenter.texture_pool.reset();
  g_presenter.layer = nil;
  g_presenter.chain.clear();
  g_presenter.render_attempts = 0;
  g_presenter.error.clear();
}
