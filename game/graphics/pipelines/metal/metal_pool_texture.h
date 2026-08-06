#pragma once

#include <cstddef>
#include <string>

#include "common/common_types.h"

#include "game/graphics/texture/TextureID.h"

#import <Metal/Metal.h>

struct GpuTexture;
class TexturePool;

// Renderer-owned fixed-size RGBA texture with stable registry and TexturePool
// identities. Publication is synchronous. The owner must call detach_pool()
// while the pool is alive before either object is destroyed.
class MetalPoolTexture {
 public:
  MetalPoolTexture(id<MTLDevice> device,
                   id<MTLCommandQueue> queue,
                   TexturePool* pool,
                   u32 width,
                   u32 height,
                   u32 vram_slot,
                   std::string debug_name);
  ~MetalPoolTexture();

  MetalPoolTexture(const MetalPoolTexture&) = delete;
  MetalPoolTexture& operator=(const MetalPoolTexture&) = delete;

  bool publish(const u32* rgba, std::size_t pixel_count);
  void detach_pool();

  u64 handle() const { return m_handle; }
  u64 publications() const { return m_publications; }
  u32 width() const { return m_width; }
  u32 height() const { return m_height; }
  u32 vram_slot() const { return m_vram_slot; }

 private:
  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_pool = nullptr;
  u32 m_width = 0;
  u32 m_height = 0;
  u32 m_vram_slot = 0;
  std::string m_debug_name;
  GpuTexture* m_pool_texture = nullptr;
  PcTextureId m_texture_id;
  u64 m_handle = 0;
  u64 m_publications = 0;
};
