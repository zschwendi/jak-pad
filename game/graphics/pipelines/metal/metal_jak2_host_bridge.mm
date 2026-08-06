#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <cmath>
#include <exception>
#include <limits>
#include <string>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

struct goal_jak2_metal_host {
  MetalRenderer renderer;
  TexturePool textures{GameVersion::Jak2};
  FixedChunkDmaCopier copier{EE_MAIN_MEM_SIZE};
  goal_gfx_host callbacks = {};
  goal_jak2_metal_host_metrics metrics = {};
  MetalRenderOptions options;
  CAMetalLayer* layer = nil;
  std::string error;
};

namespace {

goal_jak2_metal_host* g_active_host = nullptr;

goal_jak2_metal_host* active_host() {
  return g_active_host;
}

void record_failure(goal_jak2_metal_host* host, const char* message) {
  host->metrics.failed_chains++;
  host->error = message;
}

void copy_renderer_metrics(goal_jak2_metal_host* host) {
  const auto stats = host->renderer.chain_stats();
  host->metrics.last_buckets_dispatched = stats.last_buckets_dispatched;
  host->metrics.command_buffers_committed = stats.command_buffers_committed;
  host->metrics.command_buffers_completed = stats.command_buffers_completed;
  host->metrics.command_buffer_errors = stats.command_buffer_errors;
  host->metrics.drawables_acquired = stats.drawables_acquired;
  host->metrics.drawable_misses = stats.drawable_misses;
  host->metrics.late_present_submissions = stats.late_present_submissions;
  host->metrics.draws = stats.draw_calls;
  host->metrics.triangles = stats.triangles;
  host->metrics.last_screen_filter_draws = stats.jak2_screen_filter_draws;
  host->metrics.last_screen_filter_triangles = stats.jak2_screen_filter_triangles;
  host->metrics.last_debug_no_zbuf2_draws = stats.jak2_debug_no_zbuf2_draws;
  host->metrics.last_debug_no_zbuf2_triangles = stats.jak2_debug_no_zbuf2_triangles;
  host->metrics.submissions = stats.submissions;
  host->metrics.presentations = stats.presentations_completed;
  host->metrics.presentation_drops = stats.presentation_drops;
  host->metrics.presentation_order_mismatches = stats.presentation_order_mismatches;
  host->metrics.skipped_bucket_bytes = stats.skipped_bucket_bytes;
  host->metrics.unsupported_blends = stats.direct_unsupported_blends;
  host->metrics.last_command_buffer_status = stats.last_command_buffer_status;
  host->metrics.last_command_buffer_error_code = stats.last_command_buffer_error_code;
}

bool update_draw_region(goal_jak2_metal_host* host) {
  if (!host->layer) {
    return true;
  }
  const CGSize size = host->layer.drawableSize;
  if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width < 1.0 ||
      size.height < 1.0) {
    return false;
  }
  const int width = static_cast<int>(size.width);
  const int height = static_cast<int>(size.height);
  if (width * 3 >= height * 4) {
    host->options.draw_region_h = height;
    host->options.draw_region_w = height * 4 / 3;
  } else {
    host->options.draw_region_w = width;
    host->options.draw_region_h = width * 3 / 4;
  }
  return host->options.draw_region_w > 0 && host->options.draw_region_h > 0;
}

void send_chain(const void* ee_base, uint32_t chain_offset) {
  auto* host = active_host();
  if (!host) {
    return;
  }
  ASSERT(ee_base == g_ee_main_mem);
  ASSERT(chain_offset != 0);

  host->metrics.chains++;
  try {
    host->options.host_tick_id = host->metrics.chains;
    host->options.chain_ordinal = host->metrics.chains;
    host->options.engine_frame_id = host->metrics.chains;
    if (!update_draw_region(host)) {
      record_failure(host, "Jak 2 CAMetalLayer has no finite drawable size");
      return;
    }
    const auto& copied = host->copier.run(ee_base, chain_offset, false);
    host->metrics.last_copied_bytes = static_cast<uint32_t>(copied.data.size());

    const bool acquired = host->renderer.render_chain_frame(
        host->options, host->layer, copied.data.data(), copied.start_offset);
    copy_renderer_metrics(host);
    if (host->metrics.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount) {
      record_failure(host, "Jak 2 Metal renderer violated its audited bucket policy");
      return;
    }
    const bool exact_presenting_commit_count =
        host->metrics.command_buffers_committed == host->metrics.chains;
    if (!host->layer) {
      if (acquired || host->metrics.command_buffers_committed != 0 ||
          host->metrics.command_buffers_completed != 0 || host->metrics.command_buffer_errors != 0 ||
          host->metrics.drawables_acquired != 0 || host->metrics.drawable_misses != 0 ||
          host->metrics.submissions != 0 || host->metrics.presentations != 0 ||
          host->metrics.presentation_drops != 0 ||
          host->metrics.presentation_order_mismatches != 0) {
        record_failure(host, "Jak 2 nil-layer renderer violated the submission-free dispatch gate");
        return;
      }
    } else if (!acquired || host->metrics.unsupported_blends != 0 ||
               !exact_presenting_commit_count ||
               host->metrics.drawables_acquired != host->metrics.chains ||
               host->metrics.drawable_misses != 0 ||
               host->metrics.submissions != host->metrics.chains ||
               host->metrics.late_present_submissions != 0 ||
               host->metrics.command_buffer_errors != 0 || host->metrics.presentation_drops != 0 ||
               host->metrics.presentation_order_mismatches != 0) {
      record_failure(host, acquired ? "Jak 2 layer-backed submission counters violated their gate"
                                    : "Jak 2 CAMetalLayer did not provide a drawable");
      return;
    }
    host->metrics.completed_chains++;
  } catch (const std::exception& error) {
    record_failure(host, error.what());
  } catch (...) {
    record_failure(host, "Jak 2 Metal send-chain threw an unknown exception");
  }
}

uint32_t vsync() {
  auto* host = active_host();
  if (!host) {
    return 0;
  }
  host->metrics.vsyncs++;
  return static_cast<uint32_t>(host->metrics.vsyncs & 1);
}

uint32_t sync_path() {
  auto* host = active_host();
  if (host) {
    host->metrics.sync_paths++;
  }
  return 0;
}

void texture_upload(const uint8_t*, int, uint32_t) {
  if (auto* host = active_host()) {
    host->metrics.texture_uploads++;
  }
}

void texture_relocate(uint32_t, uint32_t, uint32_t) {
  if (auto* host = active_host()) {
    host->metrics.texture_relocations++;
  }
}

void set_levels(const char* const*, int) {}
void set_active_levels(const char* const*, int) {}

void set_pmode_alpha(float alpha) {
  if (auto* host = active_host()) {
    host->options.pmode_alp = alpha;
  }
}

bool policy_table_is_audited() {
  const auto& table = metal_renderer::jak2_metal_bucket_table();
  if (table.size() != metal_renderer::kJak2MetalBucketCount ||
      metal_renderer::jak2_metal_bucket_table_fingerprint() !=
          metal_renderer::kJak2MetalBucketExpectedFingerprint) {
    return false;
  }
  for (const auto& descriptor : table) {
    if (descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::DeferredSkip &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::StrictEmpty &&
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::Direct) {
      return false;
    }
  }
  return true;
}

goal_jak2_metal_host* create_host(CAMetalLayer* layer, bool presenting) {
  if (g_active_host || !policy_table_is_audited() || (presenting && !layer)) {
    return nullptr;
  }
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    return nullptr;
  }

  if (presenting) {
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.opaque = YES;
    layer.presentsWithTransaction = NO;
    layer.allowsNextDrawableTimeout = YES;
  }

  auto* host = new goal_jak2_metal_host();
  host->layer = presenting ? layer : nil;
  if (!host->renderer.init(device) || !update_draw_region(host)) {
    delete host;
    return nullptr;
  }
  if (presenting &&
      !metal_setup_placeholder(host->renderer.device(), host->renderer.queue(), host->textures)) {
    delete host;
    return nullptr;
  }
  host->renderer.init_bucket_renderers(&host->textures, GameVersion::Jak2);
  host->callbacks.send_chain = send_chain;
  host->callbacks.vsync = vsync;
  host->callbacks.sync_path = sync_path;
  host->callbacks.texture_upload_now = texture_upload;
  host->callbacks.texture_relocate = texture_relocate;
  host->callbacks.set_levels = set_levels;
  host->callbacks.set_pmode_alp = set_pmode_alpha;
  host->callbacks.set_active_levels = set_active_levels;
  g_active_host = host;
  return host;
}

}  // namespace

extern "C" {

goal_jak2_metal_host* goal_jak2_metal_host_create(void) {
  return create_host(nil, false);
}

goal_jak2_metal_host* goal_jak2_metal_host_create_presenting(
    goal_jak2_metal_host_layer layer) {
  return create_host(layer, true);
}

int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out) {
  if (!host || host != g_active_host || !out) {
    return 0;
  }
  *out = host->callbacks;
  return 1;
}

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out) {
  if (!host || host != g_active_host || !out) {
    return 0;
  }
  copy_renderer_metrics(host);
  *out = host->metrics;
  return 1;
}

int goal_jak2_metal_host_read_last_frame(goal_jak2_metal_host* host,
                                         goal_jak2_metal_frame_summary* out) {
  if (!out) {
    return 0;
  }
  *out = {};
  if (!host || host != g_active_host || !host->layer) {
    return 0;
  }

  try {
    metal_renderer::FramePixels frame;
    if (!host->renderer.read_game_frame(&frame) || frame.width <= 0 || frame.height <= 0) {
      return 0;
    }

    const auto width = static_cast<std::size_t>(frame.width);
    const auto height = static_cast<std::size_t>(frame.height);
    if (height > std::numeric_limits<std::size_t>::max() / width ||
        width * height > std::numeric_limits<std::size_t>::max() / 4) {
      return 0;
    }
    const std::size_t expected_bytes = width * height * 4;
    if (frame.rgba.size() != expected_bytes) {
      return 0;
    }

    constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kFnvPrime = 1099511628211ull;
    uint64_t hash = kFnvOffsetBasis;
    uint64_t non_black_pixels = 0;
    for (std::size_t offset = 0; offset < frame.rgba.size(); offset += 4) {
      hash ^= frame.rgba[offset];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 1];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 2];
      hash *= kFnvPrime;
      hash ^= frame.rgba[offset + 3];
      hash *= kFnvPrime;
      non_black_pixels +=
          frame.rgba[offset] != 0 || frame.rgba[offset + 1] != 0 || frame.rgba[offset + 2] != 0;
    }

    out->width = static_cast<uint32_t>(frame.width);
    out->height = static_cast<uint32_t>(frame.height);
    out->byte_count = frame.rgba.size();
    out->hash = hash;
    out->non_black_pixels = non_black_pixels;
    return 1;
  } catch (...) {
    *out = {};
    return 0;
  }
}

int goal_jak2_metal_host_wait_for_last_frame(goal_jak2_metal_host* host,
                                             double timeout_seconds,
                                             int require_presentation) {
  if (!host || host != g_active_host || !host->layer || timeout_seconds <= 0.0 ||
      host->metrics.chains == 0) {
    return 0;
  }
  if (!host->renderer.wait_for_last_chain_frame(timeout_seconds)) {
    copy_renderer_metrics(host);
    record_failure(host, "Jak 2 Metal command buffers did not complete successfully in time");
    return 0;
  }
  if (require_presentation && !host->renderer.wait_for_last_presentation(timeout_seconds)) {
    copy_renderer_metrics(host);
    record_failure(host, "Jak 2 Metal drawable presentation did not complete successfully in time");
    return 0;
  }
  copy_renderer_metrics(host);
  const bool exact =
      host->metrics.completed_chains == host->metrics.chains &&
      host->metrics.command_buffers_committed == host->metrics.chains &&
      host->metrics.command_buffers_completed == host->metrics.chains &&
      host->metrics.command_buffer_errors == 0 &&
      host->metrics.drawables_acquired == host->metrics.chains &&
      host->metrics.drawable_misses == 0 && host->metrics.submissions == host->metrics.chains &&
      host->metrics.late_present_submissions == 0 && host->metrics.presentation_drops == 0 &&
      host->metrics.presentation_order_mismatches == 0 &&
      (!require_presentation || host->metrics.presentations == host->metrics.submissions);
  if (!exact) {
    record_failure(host, "Jak 2 completed Metal frame counters violated their exact gate");
    return 0;
  }
  return 1;
}

void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host) {
  if (!host || host != g_active_host) {
    return;
  }
  g_active_host = nullptr;
  delete host;
}

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host) {
  return host && host == g_active_host ? host->error.c_str() : "Jak 2 Metal host is not active";
}

}  // extern "C"
