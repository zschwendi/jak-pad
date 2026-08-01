#pragma once

/*!
 * @file metal_texture.h
 * Texture path for the Metal backend. Objective-C++ only.
 *
 * The shared TexturePool stores opaque u64 handles. For the Metal backend those
 * handles come from the registry below, which owns the id<MTLTexture> references
 * (the Metal analog of GL's global texture-name namespace). Renderers resolve a
 * pool handle to an MTLTexture with metal_texture_lookup at draw time.
 *
 * Uploads mirror the GL loader path (add_texture in
 * game/graphics/opengl_renderer/loader/LoaderStages.cpp): RGBA8888 pixel data
 * becomes an RGBA8Unorm texture with a full mip chain generated on the GPU.
 * Textures use shared storage - a single copy in unified memory on Apple GPUs
 * (both macOS on Apple silicon and iPadOS), with no staging duplicates.
 *
 * GL renderers set filtering/wrap per draw with glTexParameteri; in Metal those
 * are immutable MTLSamplerState objects, so MetalSamplerCache bakes each
 * distinct combination once (same pattern as the PSO cache).
 */

#include <unordered_map>

#include "common/common_types.h"

#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

namespace tfrag3 {
struct Texture;
}

// --- texture registry ------------------------------------------------------

// Registers a texture and returns its handle (never 0).
u64 metal_texture_register(id<MTLTexture> tex);

// Returns nil if the handle is unknown.
id<MTLTexture> metal_texture_lookup(u64 handle);

// Drops the registry's reference (the Metal analog of glDeleteTextures).
void metal_texture_release(u64 handle);

size_t metal_texture_live_count();

// --- uploads ---------------------------------------------------------------

// RGBA8888 pixels -> RGBA8Unorm texture with a full GPU-generated mip chain.
// Blocks until mip generation completes (like the synchronous GL upload).
// Returns the registry handle, or 0 on failure.
u64 metal_upload_texture_rgba8(id<MTLDevice> device,
                               id<MTLCommandQueue> queue,
                               const u8* data,
                               u32 w,
                               u32 h);

// Mirror of the GL loader's add_texture: upload, then give to the pool if the
// texture is flagged for it. Returns the registry handle.
u64 metal_add_texture(id<MTLDevice> device,
                      id<MTLCommandQueue> queue,
                      TexturePool& pool,
                      const tfrag3::Texture& tex,
                      bool is_common);

// Uploads the pool's 16x16 placeholder pattern and registers it with the pool.
bool metal_setup_placeholder(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool& pool);

// --- sampler cache ---------------------------------------------------------

// The states GL renderers set per draw/texture: nearest/linear min-mag filters,
// an optional mip filter, clamp-to-edge/repeat wrap, and anisotropy.
struct MetalSamplerKey {
  u8 min_filter = MTLSamplerMinMagFilterNearest;   // MTLSamplerMinMagFilter
  u8 mag_filter = MTLSamplerMinMagFilterNearest;   // MTLSamplerMinMagFilter
  u8 mip_filter = MTLSamplerMipFilterNotMipmapped; // MTLSamplerMipFilter
  u8 wrap_s = MTLSamplerAddressModeClampToEdge;    // MTLSamplerAddressMode
  u8 wrap_t = MTLSamplerAddressModeClampToEdge;    // MTLSamplerAddressMode
  u8 max_anisotropy = 1;                           // 1..16

  bool operator==(const MetalSamplerKey& o) const {
    return min_filter == o.min_filter && mag_filter == o.mag_filter &&
           mip_filter == o.mip_filter && wrap_s == o.wrap_s && wrap_t == o.wrap_t &&
           max_anisotropy == o.max_anisotropy;
  }
};

class MetalSamplerCache {
 public:
  void init(id<MTLDevice> device) { m_device = device; }
  id<MTLSamplerState> get(const MetalSamplerKey& key);
  size_t count() const { return m_samplers.size(); }

 private:
  struct KeyHash {
    size_t operator()(const MetalSamplerKey& k) const {
      return ((size_t)k.min_filter << 40) ^ ((size_t)k.mag_filter << 32) ^
             ((size_t)k.mip_filter << 24) ^ ((size_t)k.wrap_s << 16) ^ ((size_t)k.wrap_t << 8) ^
             k.max_anisotropy;
    }
  };
  id<MTLDevice> m_device;
  std::unordered_map<MetalSamplerKey, id<MTLSamplerState>, KeyHash> m_samplers;
};
