#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "game/graphics/opengl_renderer/buckets.h"

namespace metal_renderer {

enum class Jak2MetalBucketBehavior : std::uint8_t {
  DeferredSkip,
  StrictEmpty,
  Direct,
  HostTextureUpload,
  Visibility,
  Sprite,
  TFragment,
  TFragmentTrans,
  TFragmentWater,
  Shrub,
  Tie,
  TieEnvmap,
  TieTrans,
  TieTransEnvmap,
  TieWater,
  TieWaterEnvmap,
  Merc,
  HostTextureUploadDirect,
  BlitDisplay,
  MercAlpha,
  MercWater,
  Generic2,
};

struct Jak2MetalBucketDescriptor {
  std::uint16_t id = 0;
  Jak2MetalBucketBehavior behavior = Jak2MetalBucketBehavior::StrictEmpty;
};

inline constexpr std::size_t kJak2MetalBucketCount =
    static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS);
inline constexpr std::uint64_t kJak2MetalBucketExpectedFingerprint = 0xdf00f6881671c008ull;

const std::array<Jak2MetalBucketDescriptor, kJak2MetalBucketCount>& jak2_metal_bucket_table();

std::uint64_t jak2_metal_bucket_table_fingerprint();
bool jak2_metal_bucket_allows_content(std::size_t bucket_id);
int jak2_metal_direct_batch_size(std::size_t bucket_id);

}  // namespace metal_renderer
