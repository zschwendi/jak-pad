#include "game/graphics/pipelines/metal/metal_jak2_bucket4_mixed_executor.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "game/graphics/pipelines/metal/metal_jak2_fog_texture_convert.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

metal_renderer::Jak2Bucket4MixedPlan make_plan() {
  metal_renderer::Jak2Bucket4MixedPlan plan;
  plan.sky.cloud_min = 0.2f;
  plan.sky.cloud_max = 0.75f;
  plan.sky.times = {9000.f, 1200.f, 600.f, 300.f, 150.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
  plan.sky.max_times = {4800.f, 2400.f, 1200.f, 600.f, 1.f, 1.f};
  plan.sky.scales = {0.49f, 0.19f, 0.145f, 0.015f, 0.f, 0.f};
  plan.sky.cloud_destination = 256;

  plan.erase.width = 16;
  plan.erase.height = 16;
  plan.erase.destination = 192;

  plan.fog.width = 256;
  plan.fog.height = 1;
  plan.fog.destination = 128;
  plan.fog.format = 19;
  plan.fog.force_to_gpu = 1;
  plan.fog.clut_destination = plan.erase.destination;
  for (std::size_t i = 0; i < plan.fog.indices.size(); ++i) {
    plan.fog.indices[i] = static_cast<u8>((i * 73 + 19) & 0xff);
  }
  std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> clut = {};
  for (std::size_t i = 0; i < clut.size(); ++i) {
    const u32 r = static_cast<u32>((i * 29 + 3) & 0xff);
    const u32 g = static_cast<u32>((i * 47 + 5) & 0xff);
    const u32 b = static_cast<u32>((i * 61 + 7) & 0xff);
    const u32 a = static_cast<u32>(0x80 | (i & 0x7f));
    clut[i] = (a << 24) | (b << 16) | (g << 8) | r;
  }
  std::memcpy(plan.fog.clut.data(), clut.data(), plan.fog.clut.size());
  return plan;
}

template <std::size_t PixelCount>
std::array<u32, PixelCount> read_pixels(u64 handle, u32 width, u32 height) {
  id<MTLTexture> texture = metal_texture_lookup(handle);
  check(texture != nil, "published pool handle resolves to a Metal texture");
  check(texture.width == width && texture.height == height,
        "published Metal texture has its fixed dimensions");
  std::array<u32, PixelCount> pixels = {};
  [texture getBytes:pixels.data()
        bytesPerRow:static_cast<NSUInteger>(width) * sizeof(u32)
         fromRegion:MTLRegionMake2D(0, 0, width, height)
        mipmapLevel:0];
  return pixels;
}

std::array<u32, metal_renderer::kJak2Opcode41CloudPixelCount> expected_cloud(
    const metal_renderer::Jak2Bucket4MixedPlan& plan) {
  metal_renderer::Jak2Opcode41CloudInput input;
  input.cloud_min = plan.sky.cloud_min;
  input.cloud_max = plan.sky.cloud_max;
  for (std::size_t i = 0; i < metal_renderer::kJak2Opcode41CloudLayerCount; ++i) {
    input.times[i] = plan.sky.times[i + 1];
    input.max_times[i] = plan.sky.max_times[i];
    input.scales[i] = plan.sky.scales[i];
  }
  metal_renderer::Jak2Opcode41CloudCpu generator;
  check(generator.generate(input), "independent expected cloud generation succeeds");
  return generator.rgba();
}

metal_renderer::Jak2FogRgbaPixels expected_fog(const metal_renderer::Jak2Bucket4MixedPlan& plan) {
  std::array<u32, metal_renderer::kJak2FogPsmct32ClutEntryCount> clut = {};
  std::memcpy(clut.data(), plan.fog.clut.data(), plan.fog.clut.size());
  const auto result = metal_renderer::convert_jak2_fog_psmt8_to_rgba(
      plan.fog.indices.data(), plan.fog.indices.size(), clut.data(), clut.size());
  check(result.has_value(), "independent expected fog conversion succeeds");
  return *result;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "Metal command queue is available");

    const std::size_t initial_live = metal_texture_live_count();
    auto pool = std::make_unique<TexturePool>(GameVersion::Jak2);
    auto executor =
        std::make_unique<metal_renderer::Jak2Bucket4MixedExecutor>(device, queue, pool.get());
    const auto plan = make_plan();
    const auto cloud_rgba = expected_cloud(plan);
    const auto fog_rgba = expected_fog(plan);

    check(executor->execute(plan), "the first owned mixed plan executes");
    const u64 cloud_handle = pool->lookup(plan.sky.cloud_destination).value_or(0);
    const u64 fog_handle = pool->lookup(plan.fog.destination).value_or(0);
    check(cloud_handle != 0 && fog_handle != 0 && cloud_handle != fog_handle,
          "cloud and fog publish to their planned VRAM slots");
    check(read_pixels<metal_renderer::kJak2Opcode41CloudPixelCount>(
              cloud_handle, metal_renderer::kJak2Opcode41CloudSize,
              metal_renderer::kJak2Opcode41CloudSize) == cloud_rgba,
          "cloud slot readback matches opcode-41 with Sky times[1..4]");
    check(read_pixels<metal_renderer::kJak2FogIndexedPixelCount>(
              fog_handle, metal_renderer::kJak2FogIndexedPixelCount, 1) == fog_rgba,
          "fog slot readback matches the alignment-safe indexed CLUT conversion");
    check(!pool->lookup(plan.erase.destination).has_value(),
          "opcode-14 erase destination is not independently published");

    check(executor->execute(plan), "a second frame republishes to stable destinations");
    const auto& repeated_stats = executor->stats();
    check(pool->lookup(plan.sky.cloud_destination).value_or(0) == cloud_handle &&
              pool->lookup(plan.fog.destination).value_or(0) == fog_handle &&
              repeated_stats.frames == 2 && repeated_stats.cloud_publications == 2 &&
              repeated_stats.fog_publications == 2 && repeated_stats.failures == 0,
          "repeat execution retains handles and reports both publications");

    auto moved = plan;
    moved.fog.destination++;
    check(!executor->execute(moved) &&
              std::string(executor->last_error()).find("changed across frames") !=
                  std::string::npos &&
              executor->stats().frames == 2 && executor->stats().failures == 1 &&
              !pool->lookup(moved.fog.destination).has_value(),
          "a destination change fails before publishing to the new slot");
    check(!executor->execute(plan) && executor->stats().frames == 2 &&
              executor->stats().failures == 1,
          "a destination mismatch leaves the executor failed closed");

    executor->detach_pool();
    check(pool->lookup_gpu_texture(plan.sky.cloud_destination)->is_placeholder &&
              pool->lookup_gpu_texture(plan.fog.destination)->is_placeholder &&
              pool->lookup(plan.sky.cloud_destination).value_or(1) ==
                  pool->get_placeholder_texture() &&
              pool->lookup(plan.fog.destination).value_or(1) == pool->get_placeholder_texture(),
          "explicit detach repoints both live pool publications to the placeholder");
    pool.reset();
    executor.reset();
    check(metal_texture_live_count() == initial_live,
          "detached executor destruction releases both Metal registry handles");
    std::puts("PASS: Jak II bucket-4 mixed animator executor");
  }
  return 0;
}
