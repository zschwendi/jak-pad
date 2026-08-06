#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <variant>

#include "common/common_types.h"

namespace metal_renderer {

struct Jak2Bucket4TextureUploadCapture;

constexpr std::size_t kJak2Bucket4OrdinaryPageHeaderBytes = 124;
constexpr std::size_t kJak2Bucket4SkyInputBytes = 108;
constexpr std::size_t kJak2Bucket4FogIndexBytes = 256;
constexpr std::size_t kJak2Bucket4ClutBytes = 16 * 16 * 4;

struct Jak2Bucket4OrdinaryUploadPlan {
  u64 page_offset = 0;
  std::array<u8, kJak2Bucket4OrdinaryPageHeaderBytes> page_header = {};
  s64 mode = 0;
};

struct Jak2Bucket4SkyInputPlan {
  std::array<u8, kJak2Bucket4SkyInputBytes> bytes = {};
  float fog_height = 0.f;
  float cloud_min = 0.f;
  float cloud_max = 0.f;
  std::array<float, 11> times = {};
  std::array<float, 6> max_times = {};
  std::array<float, 6> scales = {};
  s32 cloud_destination = 0;
};

struct Jak2Bucket4ErasePlan {
  std::array<u64, 9> setup_values = {};
  u32 width = 0;
  u32 height = 0;
  u32 destination = 0;
  u64 test = 0;
  u64 alpha = 0;
  u64 clamp = 0;
  std::array<u32, 4> clear = {};
};

struct Jak2Bucket4FogUploadPlan {
  std::array<u8, kJak2Bucket4FogIndexBytes> indices = {};
  u16 width = 0;
  u16 height = 0;
  u32 destination = 0;
  u8 format = 0;
  u8 force_to_gpu = 0;
  std::array<u8, kJak2Bucket4ClutBytes> clut = {};
  u32 clut_destination = 0;
};

struct Jak2Bucket4AbsentPlan {};

struct Jak2Bucket4OrdinaryOnlyPlan {
  Jak2Bucket4OrdinaryUploadPlan ordinary;
};

struct Jak2Bucket4MixedPlan {
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2Bucket4SkyInputPlan sky;
  Jak2Bucket4ErasePlan erase;
  Jak2Bucket4FogUploadPlan fog;
};

using Jak2Bucket4TextureUploadPlan =
    std::variant<Jak2Bucket4AbsentPlan, Jak2Bucket4OrdinaryOnlyPlan, Jak2Bucket4MixedPlan>;

/*!
 * Parse an immutable bucket-4 DMA snapshot and copy its bounded execution inputs from the live EE
 * domain. Malformed or unsupported packets return no plan. The returned plan contains no host
 * pointers. Sky, fog, and CLUT bytes are owned. The plan is frame-local: ordinary page execution
 * must remain synchronous with the validated live-EE page offset.
 */
std::optional<Jak2Bucket4TextureUploadPlan> plan_jak2_bucket4_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2Bucket4TextureUploadCapture* out_capture = nullptr);

}  // namespace metal_renderer
