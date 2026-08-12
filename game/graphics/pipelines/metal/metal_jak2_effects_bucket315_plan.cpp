#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_plan.h"

#include <limits>

namespace metal_renderer {
namespace {

using VifKind = Jak2EffectsBucket315VifKind;

constexpr u64 kOffsetBasis = 14695981039346656037ull;
constexpr u64 kPrime = 1099511628211ull;

bool matches(const Jak2EffectsBucket315Transfer& transfer,
             u32 payload_bytes,
             VifKind vif0,
             VifKind vif1) {
  return transfer.payload_bytes == payload_bytes && transfer.vif0 == vif0 &&
         transfer.vif1 == vif1;
}

void hash_u32(u64* hash, u32 value) {
  for (int i = 0; i < 4; ++i) {
    *hash ^= static_cast<u8>(value >> (i * 8));
    *hash *= kPrime;
  }
}

void hash_transfer(u64* hash, const Jak2EffectsBucket315Transfer& transfer) {
  hash_u32(hash, transfer.payload_bytes);
  *hash ^= static_cast<u8>(transfer.vif0);
  *hash *= kPrime;
  *hash ^= static_cast<u8>(transfer.vif1);
  *hash *= kPrime;
}

}  // namespace

std::optional<Jak2EffectsBucket315Plan> plan_jak2_effects_bucket315(
    const Jak2EffectsBucket315Transfer* transfers,
    std::size_t transfer_count,
    u32 bucket_id) {
  if (bucket_id != kJak2EffectsBucket || (!transfers && transfer_count != 0)) {
    return std::nullopt;
  }
  if (transfer_count == 1 && matches(transfers[0], 0, VifKind::Nop, VifKind::Nop)) {
    return Jak2EffectsBucket315Plan{};
  }

  constexpr std::size_t kFixedTransferCount = 7;
  const std::size_t max_transfer_count =
      kFixedTransferCount + static_cast<std::size_t>(kJak2EffectsLightningMaxFragments) * 3;
  if (transfer_count < kFixedTransferCount || transfer_count > max_transfer_count ||
      (!matches(transfers[0], 0, VifKind::Mark, VifKind::Nop) &&
       !matches(transfers[0], 0, VifKind::Nop, VifKind::Nop)) ||
      !matches(transfers[1], 32, VifKind::Nop, VifKind::Direct) ||
      !matches(transfers[2], 128, VifKind::Stcycl, VifKind::UnpackV4_32) ||
      !matches(transfers[3], 32, VifKind::Mscalf, VifKind::Stmod) ||
      !matches(transfers[4], 0, VifKind::Nop, VifKind::Nop) ||
      !matches(transfers[transfer_count - 2], 160, VifKind::Nop, VifKind::Direct) ||
      !matches(transfers[transfer_count - 1], 0, VifKind::Nop, VifKind::Nop) ||
      (transfer_count - kFixedTransferCount) % 3 != 0) {
    return std::nullopt;
  }

  Jak2EffectsBucket315Plan result;
  result.variant = Jak2EffectsBucket315Variant::Lightning;
  result.transfer_count = static_cast<u32>(transfer_count);
  u64 fingerprint = kOffsetBasis;
  for (std::size_t i = 0; i < 5; ++i) {
    hash_transfer(&fingerprint, transfers[i]);
    result.payload_bytes += transfers[i].payload_bytes;
  }

  for (std::size_t i = 5; i + 2 < transfer_count; i += 3) {
    const auto& header = transfers[i];
    const auto& vertices = transfers[i + 1];
    const auto& mscal = transfers[i + 2];
    if (!matches(header, kJak2EffectsLightningHeaderBytes, VifKind::Nop,
                 VifKind::UnpackV4_32) ||
        vertices.vif0 != VifKind::Nop || vertices.vif1 != VifKind::UnpackV4_32 ||
        vertices.payload_bytes % kJak2EffectsLightningVertexBytes != 0 ||
        !matches(mscal, 0, VifKind::Nop, VifKind::Mscal)) {
      return std::nullopt;
    }
    const u32 vertices_in_fragment = vertices.payload_bytes / kJak2EffectsLightningVertexBytes;
    if (vertices_in_fragment > kJak2EffectsLightningMaxVertices - result.vertex_count) {
      return std::nullopt;
    }
    result.vertex_count += vertices_in_fragment;
    ++result.fragment_count;
    hash_transfer(&fingerprint, header);
    hash_transfer(&fingerprint, vertices);
    hash_transfer(&fingerprint, mscal);
    result.payload_bytes += header.payload_bytes + vertices.payload_bytes + mscal.payload_bytes;
  }

  for (std::size_t i = transfer_count - 2; i < transfer_count; ++i) {
    hash_transfer(&fingerprint, transfers[i]);
    result.payload_bytes += transfers[i].payload_bytes;
  }
  if (result.fragment_count > kJak2EffectsLightningMaxFragments ||
      result.payload_bytes > std::numeric_limits<u32>::max()) {
    return std::nullopt;
  }
  result.semantic_fingerprint = fingerprint;
  return result;
}

}  // namespace metal_renderer
