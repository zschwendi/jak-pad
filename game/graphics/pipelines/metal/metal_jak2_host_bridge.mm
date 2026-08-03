#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <exception>
#include <string>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#import <Metal/Metal.h>

struct goal_jak2_metal_host {
  MetalRenderer renderer;
  TexturePool textures{GameVersion::Jak2};
  FixedChunkDmaCopier copier{EE_MAIN_MEM_SIZE};
  goal_gfx_host callbacks = {};
  goal_jak2_metal_host_metrics metrics = {};
  MetalRenderOptions options;
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
    const auto& copied = host->copier.run(ee_base, chain_offset, false);
    host->metrics.last_copied_bytes = static_cast<uint32_t>(copied.data.size());

    const bool acquired = host->renderer.render_chain_frame(
        host->options, nil, copied.data.data(), copied.start_offset);
    if (acquired || !host->renderer.wait_for_last_frame()) {
      record_failure(host, acquired ? "nil-layer render acquired a drawable"
                                    : "offscreen Metal command buffer did not complete");
      return;
    }

    const auto stats = host->renderer.chain_stats();
    host->metrics.last_buckets_dispatched = stats.last_buckets_dispatched;
    host->metrics.command_buffers_committed = stats.command_buffers_committed;
    host->metrics.drawables_acquired = stats.drawables_acquired;
    host->metrics.draws = stats.draw_calls;
    host->metrics.triangles = stats.triangles;
    host->metrics.submissions = stats.submissions;
    host->metrics.presentations = stats.presentations_completed;
    if (stats.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount ||
        stats.draw_calls != 0 || stats.triangles != 0 || stats.drawables_acquired != 0 ||
        stats.submissions != 0 || stats.presentations_completed != 0) {
      record_failure(host, "Jak 2 nil-layer renderer violated the policy-only dispatch gate");
      return;
    }
    host->metrics.completed_command_buffers++;
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
    if (host->metrics.chains > 0 && !host->renderer.wait_for_last_frame()) {
      record_failure(host, "sync-path observed an incomplete Metal command buffer");
    }
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
        descriptor.behavior != metal_renderer::Jak2MetalBucketBehavior::StrictEmpty) {
      return false;
    }
  }
  return true;
}

}  // namespace

extern "C" {

goal_jak2_metal_host* goal_jak2_metal_host_create(void) {
  if (g_active_host || !policy_table_is_audited()) {
    return nullptr;
  }
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    return nullptr;
  }

  auto* host = new goal_jak2_metal_host();
  if (!host->renderer.init(device)) {
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
  *out = host->metrics;
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
