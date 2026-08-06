#include "metal_texture.h"

#include <mutex>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"

namespace {

// The Metal analog of the GL texture-name namespace: owns the id<MTLTexture>
// references behind the opaque u64 handles stored in the TexturePool. Shared
// between the loader thread and the render thread.
struct TextureRegistry {
  std::mutex mutex;
  std::unordered_map<u64, id<MTLTexture>> textures;
  u64 next_handle = 1;
};

TextureRegistry& registry() {
  static TextureRegistry r;
  return r;
}

bool replace_registered_texture(u64 handle, id<MTLTexture> replacement) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.textures.find(handle);
  if (it == r.textures.end() || !replacement) {
    return false;
  }
  it->second = replacement;
  return true;
}

id<MTLTexture> make_ready_rgba8_texture(id<MTLDevice> device,
                                        id<MTLCommandQueue> queue,
                                        const u8* data,
                                        u32 w,
                                        u32 h,
                                        bool mipmapped) {
  if (!device || !queue || !data || w == 0 || h == 0) {
    return nil;
  }
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                  width:w
                                                                 height:h
                                                              mipmapped:mipmapped];
  desc.usage = MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:desc];
  if (!texture) {
    lg::error("Metal: texture allocation failed ({}x{})", w, h);
    return nil;
  }

  id<MTLCommandBuffer> commands = nil;
  id<MTLBlitCommandEncoder> blit = nil;
  if (texture.mipmapLevelCount > 1) {
    commands = [queue commandBuffer];
    if (!commands) {
      return nil;
    }
    blit = [commands blitCommandEncoder];
    if (!blit) {
      return nil;
    }
  }

  const NSUInteger bytes_per_row = static_cast<NSUInteger>(w) * 4;
  [texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
             mipmapLevel:0
               withBytes:data
             bytesPerRow:bytes_per_row];
  if (blit) {
    [blit generateMipmapsForTexture:texture];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted) {
      lg::error("Metal: texture mip generation failed ({}x{})", w, h);
      return nil;
    }
  }
  return texture;
}

}  // namespace

u64 metal_texture_register(id<MTLTexture> tex) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  u64 handle = r.next_handle++;
  r.textures.emplace(handle, tex);
  return handle;
}

id<MTLTexture> metal_texture_lookup(u64 handle) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  auto it = r.textures.find(handle);
  return it == r.textures.end() ? nil : it->second;
}

void metal_texture_release(u64 handle) {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.textures.erase(handle);
}

size_t metal_texture_live_count() {
  auto& r = registry();
  std::lock_guard<std::mutex> lock(r.mutex);
  return r.textures.size();
}

u64 metal_upload_texture_rgba8(id<MTLDevice> device,
                               id<MTLCommandQueue> queue,
                               const u8* data,
                               u32 w,
                               u32 h) {
  @autoreleasepool {
    id<MTLTexture> texture = make_ready_rgba8_texture(device, queue, data, w, h, true);
    return texture ? metal_texture_register(texture) : 0;
  }
}

bool metal_update_texture_rgba8(u64 handle,
                                id<MTLCommandQueue> queue,
                                const u8* data,
                                u32 w,
                                u32 h) {
  @autoreleasepool {
    id<MTLTexture> current = metal_texture_lookup(handle);
    if (!current || !queue || !data || w == 0 || h == 0 ||
        current.pixelFormat != MTLPixelFormatRGBA8Unorm || current.width != w ||
        current.height != h) {
      return false;
    }
    id<MTLTexture> replacement =
        make_ready_rgba8_texture(queue.device, queue, data, w, h, current.mipmapLevelCount > 1);
    return replacement && replace_registered_texture(handle, replacement);
  }
}

u64 metal_add_texture(id<MTLDevice> device,
                      id<MTLCommandQueue> queue,
                      TexturePool& pool,
                      const tfrag3::Texture& tex,
                      bool is_common) {
  // The upload runs outside the pool's lock. TexturePool does not lock inside give_texture - it
  // publishes its mutex and expects the caller to hold it (the GL loader does exactly this in
  // LoaderStages.cpp and Loader.cpp), while the game thread takes the same lock from
  // handle_upload_now and relocate. Holding it across the GPU wait below would put the game
  // thread behind every texture upload, so the lock is taken only for the registration.
  u64 handle = metal_upload_texture_rgba8(device, queue, (const u8*)tex.data.data(), tex.w, tex.h);
  if (handle && tex.load_to_pool) {
    std::lock_guard<std::mutex> pool_lock(pool.mutex());
    TextureInput in;
    in.debug_page_name = tex.debug_tpage_name;
    in.debug_name = tex.debug_name;
    in.w = tex.w;
    in.h = tex.h;
    in.gpu_texture = handle;
    in.common = is_common;
    in.id = PcTextureId::from_combo_id(tex.combo_id);
    in.src_data = (const u8*)tex.data.data();
    pool.give_texture(in);
  }
  return handle;
}

void metal_add_textures(id<MTLDevice> device,
                        id<MTLCommandQueue> queue,
                        TexturePool& pool,
                        const std::vector<tfrag3::Texture>& textures,
                        bool is_common,
                        std::vector<u64>* out) {
  out->clear();
  out->reserve(textures.size());
  for (const auto& tex : textures) {
    out->push_back(metal_upload_texture_rgba8(device, queue, (const u8*)tex.data.data(), tex.w,
                                              tex.h));
  }
  std::lock_guard<std::mutex> pool_lock(pool.mutex());
  for (size_t i = 0; i < textures.size(); i++) {
    const auto& tex = textures[i];
    if (!(*out)[i] || !tex.load_to_pool) {
      continue;
    }
    TextureInput in;
    in.debug_page_name = tex.debug_tpage_name;
    in.debug_name = tex.debug_name;
    in.w = tex.w;
    in.h = tex.h;
    in.gpu_texture = (*out)[i];
    in.common = is_common;
    in.id = PcTextureId::from_combo_id(tex.combo_id);
    in.src_data = (const u8*)tex.data.data();
    pool.give_texture(in);
  }
}

bool metal_setup_placeholder(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool& pool) {
  const auto& data = pool.placeholder_data();
  u64 handle = metal_upload_texture_rgba8(device, queue, (const u8*)data.data(), 16, 16);
  if (!handle) {
    return false;
  }
  std::lock_guard<std::mutex> pool_lock(pool.mutex());
  pool.set_placeholder(handle);
  return true;
}

id<MTLSamplerState> MetalSamplerCache::get(const MetalSamplerKey& key) {
  auto it = m_samplers.find(key);
  if (it != m_samplers.end()) {
    return it->second;
  }
  auto* desc = [[MTLSamplerDescriptor alloc] init];
  desc.minFilter = (MTLSamplerMinMagFilter)key.min_filter;
  desc.magFilter = (MTLSamplerMinMagFilter)key.mag_filter;
  desc.mipFilter = (MTLSamplerMipFilter)key.mip_filter;
  desc.sAddressMode = (MTLSamplerAddressMode)key.wrap_s;
  desc.tAddressMode = (MTLSamplerAddressMode)key.wrap_t;
  desc.maxAnisotropy = key.max_anisotropy;
  id<MTLSamplerState> state = [m_device newSamplerStateWithDescriptor:desc];
  m_samplers.emplace(key, state);
  return state;
}
