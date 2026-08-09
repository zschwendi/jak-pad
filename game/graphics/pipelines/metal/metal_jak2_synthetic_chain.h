#pragma once

#include <cstddef>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"

namespace metal_renderer {

constexpr std::size_t kJak2SyntheticBucketCount =
    static_cast<std::size_t>(jak2::BucketId::MAX_BUCKETS);
static_assert(kJak2SyntheticBucketCount == 327);

inline void put_jak2_synthetic_tag(std::vector<u8>& chain,
                                   std::size_t offset,
                                   DmaTag::Kind kind,
                                   u16 qwc = 0,
                                   u32 address = 0) {
  const u64 value = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                    (static_cast<u64>(address) << 32);
  std::memcpy(chain.data() + offset, &value, sizeof(value));
}

// Public, deterministic Jak II chain used by development proofs. It traverses every bucket,
// gives one DeferredSkip bucket a 16-byte marker, and gives SCREEN_FILTER four VIF NOPs. It
// contains no game data and intentionally encodes no draw.
inline std::vector<u8> make_jak2_synthetic_metal_chain() {
  constexpr std::size_t kDeferredBucket =
      static_cast<std::size_t>(jak2::BucketId::OCEAN_MID_FAR);
  constexpr std::size_t kDirectBucket = static_cast<std::size_t>(jak2::BucketId::SCREEN_FILTER);
  constexpr std::size_t kDeferredPayloadOffset = (kJak2SyntheticBucketCount + 1) * 16;
  constexpr std::size_t kDirectPayloadOffset = kDeferredPayloadOffset + 48;

  std::vector<u8> chain(kDirectPayloadOffset + 48, 0);
  for (std::size_t bucket = 0; bucket < kJak2SyntheticBucketCount; bucket++) {
    put_jak2_synthetic_tag(chain, bucket * 16, DmaTag::Kind::CNT);
  }
  put_jak2_synthetic_tag(chain, kJak2SyntheticBucketCount * 16, DmaTag::Kind::END);

  put_jak2_synthetic_tag(chain, kDeferredBucket * 16, DmaTag::Kind::NEXT, 0,
                         kDeferredPayloadOffset);
  put_jak2_synthetic_tag(chain, kDeferredPayloadOffset, DmaTag::Kind::CNT, 1);
  std::memset(chain.data() + kDeferredPayloadOffset + 16, 0xa5, 16);
  put_jak2_synthetic_tag(chain, kDeferredPayloadOffset + 32, DmaTag::Kind::NEXT, 0,
                         static_cast<u32>((kDeferredBucket + 1) * 16));

  put_jak2_synthetic_tag(chain, kDirectBucket * 16, DmaTag::Kind::NEXT, 0,
                         kDirectPayloadOffset);
  put_jak2_synthetic_tag(chain, kDirectPayloadOffset, DmaTag::Kind::CNT, 1);
  put_jak2_synthetic_tag(chain, kDirectPayloadOffset + 32, DmaTag::Kind::NEXT, 0,
                         static_cast<u32>((kDirectBucket + 1) * 16));
  return chain;
}

}  // namespace metal_renderer
