#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "game/graphics/pipelines/metal/metal_pool_texture.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::array<u32, 4> read_pixels(u64 handle) {
  id<MTLTexture> texture = metal_texture_lookup(handle);
  check(texture != nil, "published registry handle resolves to an MTLTexture");
  std::array<u32, 4> result = {};
  [texture getBytes:result.data()
          bytesPerRow:2 * sizeof(u32)
           fromRegion:MTLRegionMake2D(0, 0, 2, 2)
          mipmapLevel:0];
  return result;
}

std::array<u32, 4> read_pixels(id<MTLTexture> texture) {
  std::array<u32, 4> result = {};
  check(texture != nil, "direct MTLTexture readback has a texture");
  [texture getBytes:result.data()
          bytesPerRow:2 * sizeof(u32)
           fromRegion:MTLRegionMake2D(0, 0, 2, 2)
          mipmapLevel:0];
  return result;
}

u32 read_mip_pixel(u64 handle) {
  u32 result = 0;
  id<MTLTexture> texture = metal_texture_lookup(handle);
  check(texture != nil && texture.mipmapLevelCount == 2,
        "the 2x2 publication owns a complete mip chain");
  [texture getBytes:&result
          bytesPerRow:sizeof(result)
           fromRegion:MTLRegionMake2D(0, 0, 1, 1)
          mipmapLevel:1];
  return result;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "Metal command queue is available");

    const std::size_t initial_live = metal_texture_live_count();
    constexpr std::array<u32, 4> first = {
        0x04030201, 0x08070605, 0x0c0b0a09, 0x100f0e0d};
    check(metal_upload_texture_rgba8(device, nil, reinterpret_cast<const u8*>(first.data()), 2,
                                     2) == 0 &&
              metal_texture_live_count() == initial_live,
          "an unavailable upload queue fails before registry publication");
    std::unique_ptr<MetalPoolTexture> publication;
    {
      auto pool = std::make_unique<TexturePool>(GameVersion::Jak2);
      publication = std::make_unique<MetalPoolTexture>(device, queue, pool.get(), 2, 2, 128,
                                                       "pool-publication-proof");
      check(publication->publish(first.data(), first.size()),
            "first fixed-size texture publication succeeds");
      const u64 stable_handle = publication->handle();
      check(stable_handle != 0 && publication->publications() == 1 &&
                metal_texture_live_count() == initial_live + 1,
            "first publication owns exactly one registry handle");
      check(pool->lookup(128).value_or(0) == stable_handle && read_pixels(stable_handle) == first &&
                read_mip_pixel(stable_handle) == 0x0a090807u,
            "first publication updates the pool slot, exact RGBA words, and generated mip");
      id<MTLTexture> first_texture = metal_texture_lookup(stable_handle);
      GpuTexture* stable_pool_texture = pool->lookup_gpu_texture(128);

      constexpr u32 displaced_pixel = 0xff010203;
      const u64 displaced_handle = metal_upload_texture_rgba8(
          device, queue, reinterpret_cast<const u8*>(&displaced_pixel), 1, 1);
      check(displaced_handle != 0, "ordinary-page displacement texture uploads");
      PcTextureId displaced_id;
      {
        std::lock_guard<std::mutex> pool_lock(pool->mutex());
        TextureInput displaced;
        displaced.debug_page_name = "TEST";
        displaced.debug_name = "ordinary-page-displacement";
        displaced.id = pool->allocate_pc_port_texture(GameVersion::Jak2);
        displaced_id = displaced.id;
        displaced.gpu_texture = displaced_handle;
        displaced.w = 1;
        displaced.h = 1;
        pool->give_texture_and_load_to_vram(displaced, 128);
      }
      check(pool->lookup(128).value_or(0) == displaced_handle,
            "an ordinary-page publication can displace the animator slot");

      constexpr std::array<u32, 4> second = {
          0x44332211, 0x88776655, 0xccbbaa99, 0x00ffeedd};
      check(publication->publish(second.data(), second.size()),
            "same-size texture update succeeds");
      check(publication->handle() == stable_handle && publication->publications() == 2 &&
                metal_texture_live_count() == initial_live + 2 &&
                pool->lookup(128).value_or(0) == stable_handle &&
                pool->lookup_gpu_texture(128) == stable_pool_texture &&
                metal_texture_lookup(stable_handle) != first_texture &&
                read_pixels(first_texture) == first && read_pixels(stable_handle) == second &&
                read_mip_pixel(stable_handle) == 0x66998877u,
            "repeat publication reclaims its slot and atomically swaps exact pixels and mip data");
      {
        std::lock_guard<std::mutex> pool_lock(pool->mutex());
        pool->unload_texture(displaced_id, displaced_handle);
      }
      metal_texture_release(displaced_handle);
      check(!publication->publish(nullptr, second.size()) &&
                !publication->publish(second.data(), second.size() - 1) &&
                publication->publications() == 2,
            "invalid updates leave the prior publication untouched");

      MetalPoolTexture oversized(device, queue, pool.get(), 65536, 1, 129,
                                 "oversized-publication");
      std::array<u32, 65536> oversized_pixels = {};
      MetalPoolTexture invalid_slot(device, queue, pool.get(), 2, 2,
                                    static_cast<u32>(pool->all_textures().size()),
                                    "invalid-slot-publication");
      check(!oversized.publish(oversized_pixels.data(), oversized_pixels.size()) &&
                !invalid_slot.publish(second.data(), second.size()) && oversized.handle() == 0 &&
                invalid_slot.handle() == 0 && metal_texture_live_count() == initial_live + 1,
            "unrepresentable dimensions and VRAM slots fail before allocating a texture");

      publication->detach_pool();
      check(publication->handle() == stable_handle &&
                pool->lookup_gpu_texture(128)->is_placeholder &&
                pool->lookup(128).value_or(1) == pool->get_placeholder_texture() &&
                !publication->publish(second.data(), second.size()),
            "explicit detach repoints the live pool and prevents later publication");
    }
    publication.reset();
    check(metal_texture_live_count() == initial_live,
          "renderer teardown releases the registry handle without touching the destroyed pool");
    std::puts("PASS: Metal fixed-size TexturePool publication");
  }
  return 0;
}
