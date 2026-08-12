#pragma once

#include <cstddef>
#include <optional>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2EffectsBucket = 315;
constexpr u32 kJak2EffectsLightningHeaderBytes = 16 * 7 + 5 * 16;
constexpr u32 kJak2EffectsLightningVertexBytes = 16 * 3;
constexpr u32 kJak2EffectsLightningMaxFragments = 10000;
constexpr u32 kJak2EffectsLightningMaxVertices = 500000;

enum class Jak2EffectsBucket315VifKind : u8 {
  Nop,
  Mark,
  Direct,
  Stcycl,
  UnpackV4_32,
  Mscalf,
  Stmod,
  Mscal,
  Flusha,
};

struct Jak2EffectsBucket315Transfer {
  u32 payload_bytes = 0;
  Jak2EffectsBucket315VifKind vif0 = Jak2EffectsBucket315VifKind::Nop;
  Jak2EffectsBucket315VifKind vif1 = Jak2EffectsBucket315VifKind::Nop;
};

enum class Jak2EffectsBucket315Variant : u8 {
  Absent,
  Lightning,
};

struct Jak2EffectsBucket315Plan {
  u32 bucket_id = kJak2EffectsBucket;
  Jak2EffectsBucket315Variant variant = Jak2EffectsBucket315Variant::Absent;
  u32 transfer_count = 0;
  u32 fragment_count = 0;
  u32 vertex_count = 0;
  u64 payload_bytes = 0;
  u64 semantic_fingerprint = 0;
};

/*
 * Preflight an immutable transfer summary for Jak II's EFFECTS bucket. This is a passive,
 * non-executing diagnostic seam: it owns only source-visible counts and no DMA, EE, Metal, or
 * renderer pointers. It accepts the exact unused NOP slot or the Lightning-mode grammar read by
 * Generic2::process_dma_lightning: fixed setup, zero or more header/vertex/MSCAL triples, then
 * the fixed FLUSHA/DIRECT terminator. Unknown VIF kinds, malformed sizes, excess source limits,
 * and trailing transfers fail closed.
 */
std::optional<Jak2EffectsBucket315Plan> plan_jak2_effects_bucket315(
    const Jak2EffectsBucket315Transfer* transfers,
    std::size_t transfer_count,
    u32 bucket_id);

}  // namespace metal_renderer
