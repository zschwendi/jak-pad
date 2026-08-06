#pragma once

#include <memory>
#include <string>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_opcode41_cloud_cpu.h"
#import <Metal/Metal.h>

class MetalPoolTexture;
class TexturePool;

namespace metal_renderer {

/*!
 * Executes the generated-texture portion of an owned Jak II bucket-4 mixed plan.
 * Ordinary page publication remains with the caller. The pool must outlive this
 * object, and detach_pool() must be called while the pool is still alive.
 */
class Jak2Bucket4MixedExecutor {
 public:
  struct Stats {
    u64 frames = 0;
    u64 cloud_publications = 0;
    u64 fog_publications = 0;
    u64 failures = 0;
  };

  Jak2Bucket4MixedExecutor(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* pool);
  ~Jak2Bucket4MixedExecutor();

  Jak2Bucket4MixedExecutor(const Jak2Bucket4MixedExecutor&) = delete;
  Jak2Bucket4MixedExecutor& operator=(const Jak2Bucket4MixedExecutor&) = delete;

  bool execute(const Jak2Bucket4MixedPlan& plan);
  void detach_pool();

  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_last_error.c_str(); }

 private:
  bool fail(std::string error);
  bool plan_shape_is_valid(const Jak2Bucket4MixedPlan& plan) const;

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_pool = nullptr;
  Jak2Opcode41CloudCpu m_cloud_cpu;
  std::unique_ptr<MetalPoolTexture> m_cloud_publication;
  std::unique_ptr<MetalPoolTexture> m_fog_publication;
  u32 m_cloud_destination = 0;
  u32 m_fog_destination = 0;
  bool m_destinations_bound = false;
  bool m_detached = false;
  bool m_failed = false;
  Stats m_stats;
  std::string m_last_error;
};

}  // namespace metal_renderer
