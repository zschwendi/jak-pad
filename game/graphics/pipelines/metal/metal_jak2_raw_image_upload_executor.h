#pragma once

#include <memory>
#include <string>

#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_plan.h"

#import <Metal/Metal.h>

class MetalPoolTexture;
class TexturePool;

namespace metal_renderer {

class Jak2RawImageUploadExecutor {
 public:
  struct Stats {
    u64 publications = 0;
    u64 texture_handle = 0;
    u64 pixel_count = 0;
  };

  Jak2RawImageUploadExecutor(id<MTLDevice> device,
                             id<MTLCommandQueue> queue,
                             TexturePool* pool);
  ~Jak2RawImageUploadExecutor();

  Jak2RawImageUploadExecutor(const Jak2RawImageUploadExecutor&) = delete;
  Jak2RawImageUploadExecutor& operator=(const Jak2RawImageUploadExecutor&) = delete;

  bool execute(const Jak2RawImageUploadPlan& plan);
  void detach_pool();

  const Stats& stats() const { return m_stats; }
  const char* last_error() const { return m_last_error.c_str(); }

 private:
  bool fail(std::string error);

  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_pool = nullptr;
  std::unique_ptr<MetalPoolTexture> m_texture;
  Stats m_stats;
  std::string m_last_error;
  bool m_detached = false;
  bool m_failed = false;
};

}  // namespace metal_renderer
