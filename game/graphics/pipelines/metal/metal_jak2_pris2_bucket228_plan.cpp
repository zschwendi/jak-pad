#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"

#include <algorithm>
#include <limits>

namespace metal_renderer {
namespace {

constexpr u32 kPrisGrammarAliasBucket = 196;

bool is_pris2_texture_bucket(u32 bucket_id) {
  return std::find(kJak2Pris2TextureUploadBuckets.begin(),
                   kJak2Pris2TextureUploadBuckets.end(), bucket_id) !=
         kJak2Pris2TextureUploadBuckets.end();
}

void set_rejection(Jak2PrisEyeTextureUploadRejection* rejection,
                   Jak2PrisEyeTextureUploadRejectReason reason) {
  if (rejection) {
    *rejection = {};
    rejection->reason = reason;
  }
}

bool eye_chunks_match(const Jak2PrisEyeChunkPlan& live,
                      const Jak2PrisEyeChunkPlan& copied) {
  return live.resolution == copied.resolution && live.pair_index == copied.pair_index &&
         live.start_transfer_index == copied.start_transfer_index &&
         live.linker_transfer_index == copied.linker_transfer_index &&
         live.transfer_count == copied.transfer_count &&
         live.payload_bytes == copied.payload_bytes &&
         live.eye_slot_mask == copied.eye_slot_mask &&
         live.semantic_fingerprint == copied.semantic_fingerprint;
}

}  // namespace

std::optional<Jak2Pris2Bucket228Plan> plan_jak2_pris2_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture,
    Jak2PrisEyeTextureUploadRejection* out_rejection) {
  if (out_capture) {
    *out_capture = {};
  }
  if (out_rejection) {
    *out_rejection = {};
  }
  if (!is_pris2_texture_bucket(bucket_id)) {
    set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::UnsupportedBucket);
    return std::nullopt;
  }
  const u32 alias_chain_delta = (bucket_id - kPrisGrammarAliasBucket) * 16;
  if (chain_offset > std::numeric_limits<u32>::max() - alias_chain_delta) {
    set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::CaptureInvalid);
    return std::nullopt;
  }

  // Moving the chain base forward by the bucket-number delta makes the established bucket-196
  // grammar start at the exact PRIS2 bucket byte. This reuses its bounded GIF/source validation
  // without broadening the common parser's audited bucket set.
  const u32 alias_chain_offset = chain_offset + alias_chain_delta;
  const auto source_plan = plan_jak2_pris_eye_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, alias_chain_offset,
      kPrisGrammarAliasBucket, live_ee_memory, live_ee_memory_size, out_capture,
      out_rejection);
  if (!source_plan || source_plan->has_prison_jak_animator ||
      source_plan->chunk_count > kJak2PrisEyeMaximumChunks) {
    if (source_plan && out_rejection &&
        out_rejection->reason == Jak2PrisEyeTextureUploadRejectReason::None) {
      set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Counts);
    }
    return std::nullopt;
  }

  Jak2Pris2Bucket228Plan plan;
  plan.bucket_id = bucket_id;
  if (!source_plan->present) {
    plan.variant = Jak2Pris2Bucket228Variant::Absent;
    return plan;
  }
  const bool ordinary_only = source_plan->chunk_count == 0;
  plan.variant = ordinary_only
                     ? Jak2Pris2Bucket228Variant::OrdinaryOnly
                     : source_plan->chunk_count == 1
                           ? Jak2Pris2Bucket228Variant::OneEyeChunk
                           : Jak2Pris2Bucket228Variant::TwoEyeChunks;
  plan.ordinary = source_plan->ordinary;
  plan.chunks = source_plan->chunks;
  plan.chunk_count = source_plan->chunk_count;
  plan.direct_reset_transfer_index = source_plan->direct_reset_transfer_index;
  plan.direct_reset_relative_tag_offset = source_plan->direct_reset_relative_tag_offset;
  plan.terminal_transfer_index = source_plan->terminal_transfer_index;
  plan.terminal_relative_tag_offset = source_plan->terminal_relative_tag_offset;
  plan.eye_slot_mask = source_plan->eye_slot_mask;
  plan.semantic_fingerprint = source_plan->semantic_fingerprint;
  return plan;
}

std::optional<Jak2Pris2Bucket228Plan> plan_jak2_pris2_bucket228(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture,
    Jak2PrisEyeTextureUploadRejection* out_rejection) {
  if (bucket_id != kJak2Pris2TextureUploadBucket) {
    set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::UnsupportedBucket);
    return std::nullopt;
  }
  return plan_jak2_pris2_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id,
      live_ee_memory, live_ee_memory_size, out_capture, out_rejection);
}

bool jak2_pris2_texture_upload_plans_match(const Jak2Pris2Bucket228Plan& live,
                                           const Jak2Pris2Bucket228Plan& copied) {
  if (!is_pris2_texture_bucket(live.bucket_id) || live.bucket_id != copied.bucket_id ||
      live.variant != copied.variant || live.chunk_count != copied.chunk_count ||
      live.ordinary.page_offset != copied.ordinary.page_offset ||
      live.ordinary.mode != copied.ordinary.mode ||
      live.ordinary.page_header != copied.ordinary.page_header ||
      live.direct_reset_transfer_index != copied.direct_reset_transfer_index ||
      live.terminal_transfer_index != copied.terminal_transfer_index ||
      live.eye_slot_mask != copied.eye_slot_mask ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  if (live.variant == Jak2Pris2Bucket228Variant::Absent) {
    return true;
  }
  for (std::size_t i = 0; i < live.chunk_count; ++i) {
    if (!eye_chunks_match(live.chunks[i], copied.chunks[i])) {
      return false;
    }
  }
  return true;
}

Jak2PrisEyeTextureUploadPlan adapt_jak2_pris2_to_pris_eye_plan(
    const Jak2Pris2Bucket228Plan& source) {
  Jak2PrisEyeTextureUploadPlan result;
  result.bucket_id = source.bucket_id;
  if (!is_pris2_texture_bucket(source.bucket_id) ||
      source.variant == Jak2Pris2Bucket228Variant::Absent) {
    return result;
  }
  result.present = true;
  result.ordinary = source.ordinary;
  result.chunks = source.chunks;
  result.chunk_count = source.chunk_count;
  result.direct_reset_transfer_index = source.direct_reset_transfer_index;
  result.direct_reset_relative_tag_offset = source.direct_reset_relative_tag_offset;
  result.terminal_transfer_index = source.terminal_transfer_index;
  result.terminal_relative_tag_offset = source.terminal_relative_tag_offset;
  result.eye_slot_mask = source.eye_slot_mask;
  result.semantic_fingerprint = source.semantic_fingerprint;
  return result;
}

bool jak2_pris_eye_slot_masks_are_disjoint(
    const Jak2PrisEyeTextureUploadPlan* per_level_plans,
    std::size_t per_level_plan_count,
    const Jak2CommonPrisTextureUploadPlan& common_plan,
    const Jak2Pris2Bucket228Plan* pris2_plans,
    std::size_t pris2_plan_count) {
  if ((!per_level_plans && per_level_plan_count != 0) ||
      (!pris2_plans && pris2_plan_count != 0)) {
    return false;
  }
  u64 claimed = 0;
  const auto claim = [&claimed](u64 mask) {
    if ((claimed & mask) != 0) {
      return false;
    }
    claimed |= mask;
    return true;
  };
  for (std::size_t i = 0; i < per_level_plan_count; ++i) {
    if (!claim(per_level_plans[i].eye_slot_mask)) {
      return false;
    }
  }
  if (!claim(common_plan.eye_slot_mask)) {
    return false;
  }
  for (std::size_t i = 0; i < pris2_plan_count; ++i) {
    if (!claim(pris2_plans[i].eye_slot_mask)) {
      return false;
    }
  }
  return true;
}

}  // namespace metal_renderer
