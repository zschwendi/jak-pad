#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"
#include "game/graphics/pipelines/metal/metal_renderer.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

enum class PendingOperationKind { Chain, SyncBarrier };

struct PendingOperation {
  PendingOperationKind kind = PendingOperationKind::SyncBarrier;
  DmaData chain;
  MetalRenderOptions options;
};

struct goal_jak2_metal_host {
  MetalRenderer renderer;
  TexturePool textures{GameVersion::Jak2};
  FixedChunkDmaCopier copier{EE_MAIN_MEM_SIZE};
  goal_gfx_host callbacks = {};
  goal_jak2_metal_host_metrics metrics = {};
  MetalRenderOptions options;
  std::string error;
  __strong CAMetalLayer* layer = nil;
  uint64_t observed_command_buffers = 0;
  std::vector<PendingOperation> pending_operations;
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

void refresh_renderer_metrics(goal_jak2_metal_host* host) {
  const auto stats = host->renderer.chain_stats();
  host->metrics.last_buckets_dispatched = stats.last_buckets_dispatched;
  host->metrics.command_buffers_committed = stats.command_buffers_committed;
  host->metrics.drawables_acquired = stats.drawables_acquired;
  host->metrics.drawable_misses = stats.drawable_misses;
  host->metrics.draws = stats.draw_calls;
  host->metrics.triangles = stats.triangles;
  host->metrics.submissions = stats.submissions;
  host->metrics.presentations = stats.presentations_completed;
  host->metrics.presentation_drops = stats.presentation_drops;
  host->metrics.presentation_order_mismatches = stats.presentation_order_mismatches;
  host->metrics.last_submission_id = stats.last_submission_id;
  host->metrics.last_presented_submission_id = stats.last_presented_submission_id;
}

bool wait_until_idle(goal_jak2_metal_host* host) {
  refresh_renderer_metrics(host);
  if (host->metrics.command_buffers_committed <= host->observed_command_buffers) {
    return host->metrics.command_buffer_errors == 0;
  }
  if (host->metrics.command_buffers_committed != host->observed_command_buffers + 1) {
    host->metrics.command_buffer_errors++;
    record_failure(host, "surface-backed Metal completion backlog was not observed individually");
    return false;
  }

  const auto completion = host->renderer.wait_for_last_frame();
  host->metrics.last_command_buffer_status = completion.status;
  host->metrics.last_command_buffer_error_code = completion.error_code;
  if (!completion.had_command_buffer || !completion.completed) {
    host->metrics.command_buffer_errors++;
    record_failure(host, "surface-backed Metal command buffer did not complete");
    return false;
  }
  host->observed_command_buffers++;
  host->metrics.completed_command_buffers++;
  refresh_renderer_metrics(host);
  return true;
}

bool flush_pending(goal_jak2_metal_host* host) {
  std::vector<PendingOperation> pending;
  pending.swap(host->pending_operations);
  bool succeeded = host->metrics.failed_chains == 0;
  for (auto& item : pending) {
    if (item.kind == PendingOperationKind::SyncBarrier) {
      if (host->layer && !wait_until_idle(host)) {
        succeeded = false;
      }
      continue;
    }
    try {
      const auto before = host->renderer.chain_stats();
      CAMetalLayer* layer = host->layer;
      const bool acquired = host->renderer.render_chain_frame(
          item.options, layer, item.chain.data.data(), item.chain.start_offset);
      const auto stats = host->renderer.chain_stats();
      refresh_renderer_metrics(host);
      const bool surface_dispatch = layer != nil;
      const bool expected_transport = surface_dispatch
                                          ? acquired &&
                                                stats.command_buffers_committed ==
                                                    before.command_buffers_committed + 1 &&
                                                stats.drawables_acquired ==
                                                    before.drawables_acquired + 1 &&
                                                stats.drawable_misses == before.drawable_misses &&
                                                stats.submissions == before.submissions + 1
                                          : !acquired &&
                                                stats.command_buffers_committed ==
                                                    before.command_buffers_committed &&
                                                stats.drawables_acquired == before.drawables_acquired &&
                                                stats.drawable_misses == before.drawable_misses &&
                                                stats.submissions == before.submissions;
      if (stats.last_buckets_dispatched != metal_renderer::kJak2MetalBucketCount ||
          !expected_transport || stats.draw_calls != 0 || stats.triangles != 0 ||
          stats.presentation_drops != 0 || stats.presentation_order_mismatches != 0) {
        record_failure(host, surface_dispatch
                                 ? "Jak 2 surface renderer violated the policy-only presentation gate"
                                 : "Jak 2 nil-layer renderer violated the policy-only dispatch gate");
        succeeded = false;
        continue;
      }
      if (surface_dispatch && !wait_until_idle(host)) {
        succeeded = false;
        continue;
      }
      host->metrics.completed_chains++;
    } catch (const std::exception& error) {
      record_failure(host, error.what());
      succeeded = false;
    } catch (...) {
      record_failure(host, "Jak 2 Metal pending-chain flush threw an unknown exception");
      succeeded = false;
    }
  }
  if (host->layer && !pending.empty() && !wait_until_idle(host)) {
    succeeded = false;
  }
  return succeeded;
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
    host->pending_operations.push_back(
        PendingOperation{PendingOperationKind::Chain, copied, host->options});
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
    try {
      host->pending_operations.push_back(PendingOperation{});
    } catch (const std::exception& error) {
      record_failure(host, error.what());
    } catch (...) {
      record_failure(host, "Jak 2 Metal sync-path queueing threw an unknown exception");
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
#if TARGET_OS_SIMULATOR
  host->metrics.presentation_observation_supported = 0;
#else
  host->metrics.presentation_observation_supported = 1;
#endif
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

int goal_jak2_metal_host_set_layer(goal_jak2_metal_host* host,
                                   goal_jak2_metal_layer_ref layer) {
  if (!host || host != g_active_host) {
    return 0;
  }
  if (!layer && host->layer && !wait_until_idle(host)) {
    return 0;
  }
  if (layer && host->layer && layer != host->layer) {
    record_failure(host, "replacing an attached Jak 2 Metal layer is not supported");
    return 0;
  }
  if (layer) {
    layer.device = host->renderer.device();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
  }
  host->layer = layer;
  host->metrics.surface_attached = layer ? 1 : 0;
  return 1;
}

int goal_jak2_metal_host_copy_gfx_host(goal_jak2_metal_host* host, goal_gfx_host* out) {
  if (!host || host != g_active_host || !out) {
    return 0;
  }
  *out = host->callbacks;
  return 1;
}

void goal_jak2_metal_host_set_presentation_time(goal_jak2_metal_host* host,
                                                double presentation_time) {
  if (host && host == g_active_host) {
    host->options.presentation_time = presentation_time > 0.0 ? presentation_time : 0.0;
  }
}

int goal_jak2_metal_host_flush_pending(goal_jak2_metal_host* host) {
  if (!host || host != g_active_host) {
    return 0;
  }
  return flush_pending(host) ? 1 : 0;
}

int goal_jak2_metal_host_discard_pending(goal_jak2_metal_host* host) {
  if (!host || host != g_active_host) {
    return 0;
  }
  const int discarded = static_cast<int>(host->pending_operations.size());
  host->pending_operations.clear();
  return discarded;
}

int goal_jak2_metal_host_wait_until_idle(goal_jak2_metal_host* host) {
  if (!host || host != g_active_host) {
    return 0;
  }
  return wait_until_idle(host) ? 1 : 0;
}

int goal_jak2_metal_host_get_metrics(goal_jak2_metal_host* host,
                                     goal_jak2_metal_host_metrics* out) {
  if (!host || host != g_active_host || !out) {
    return 0;
  }
  refresh_renderer_metrics(host);
  *out = host->metrics;
  return 1;
}

void goal_jak2_metal_host_destroy(goal_jak2_metal_host* host) {
  if (!host || host != g_active_host) {
    return;
  }
  if (host->layer) {
    wait_until_idle(host);
    host->layer = nil;
  }
  g_active_host = nullptr;
  delete host;
}

const char* goal_jak2_metal_host_last_error(goal_jak2_metal_host* host) {
  return host && host == g_active_host ? host->error.c_str() : "Jak 2 Metal host is not active";
}

}  // extern "C"
