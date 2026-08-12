#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"

#include <limits>

namespace metal_renderer {
namespace {

constexpr u32 kPrisGrammarAliasBucket = 196;
constexpr u32 kPrisGrammarAliasChainDelta =
    (kJak2Pris2TextureUploadBucket - kPrisGrammarAliasBucket) * 16;

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

std::optional<Jak2Pris2Bucket228Plan> plan_jak2_pris2_bucket228(
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
  if (bucket_id != kJak2Pris2TextureUploadBucket) {
    set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::UnsupportedBucket);
    return std::nullopt;
  }
  if (chain_offset > std::numeric_limits<u32>::max() - kPrisGrammarAliasChainDelta) {
    set_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::CaptureInvalid);
    return std::nullopt;
  }

  // Moving the chain base forward by the bucket-number delta makes the established bucket-196
  // grammar start at the exact bucket-228 byte. This reuses its bounded GIF/source validation
  // without broadening the common parser's audited bucket set.
  const u32 alias_chain_offset = chain_offset + kPrisGrammarAliasChainDelta;
  const auto source_plan = plan_jak2_pris_eye_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, alias_chain_offset,
      kPrisGrammarAliasBucket, live_ee_memory, live_ee_memory_size, out_capture,
      out_rejection);
  if (!source_plan || !source_plan->present || source_plan->has_prison_jak_animator ||
      source_plan->chunk_count > 1) {
    if (source_plan && out_rejection &&
        out_rejection->reason == Jak2PrisEyeTextureUploadRejectReason::None) {
      set_rejection(out_rejection,
                    source_plan->present ? Jak2PrisEyeTextureUploadRejectReason::Counts
                                         : Jak2PrisEyeTextureUploadRejectReason::AbsentEnvelope);
    }
    return std::nullopt;
  }

  const bool ordinary_only = source_plan->chunk_count == 0;
  Jak2Pris2Bucket228Plan plan;
  plan.variant = ordinary_only ? Jak2Pris2Bucket228Variant::OrdinaryOnly
                               : Jak2Pris2Bucket228Variant::OneEyeChunk;
  plan.ordinary = source_plan->ordinary;
  if (!ordinary_only) {
    plan.eye_chunk = source_plan->chunks[0];
  }
  plan.direct_reset_transfer_index = source_plan->direct_reset_transfer_index;
  plan.direct_reset_relative_tag_offset = source_plan->direct_reset_relative_tag_offset;
  plan.terminal_transfer_index = source_plan->terminal_transfer_index;
  plan.terminal_relative_tag_offset = source_plan->terminal_relative_tag_offset;
  plan.eye_slot_mask = source_plan->eye_slot_mask;
  plan.semantic_fingerprint = source_plan->semantic_fingerprint;
  return plan;
}

bool jak2_pris2_bucket228_plans_match(const Jak2Pris2Bucket228Plan& live,
                                      const Jak2Pris2Bucket228Plan& copied) {
  if (live.bucket_id != kJak2Pris2TextureUploadBucket ||
      copied.bucket_id != kJak2Pris2TextureUploadBucket || live.variant != copied.variant ||
      live.ordinary.page_offset != copied.ordinary.page_offset ||
      live.ordinary.mode != copied.ordinary.mode ||
      live.ordinary.page_header != copied.ordinary.page_header ||
      live.direct_reset_transfer_index != copied.direct_reset_transfer_index ||
      live.terminal_transfer_index != copied.terminal_transfer_index ||
      live.eye_slot_mask != copied.eye_slot_mask ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  if (live.variant == Jak2Pris2Bucket228Variant::OrdinaryOnly) {
    return true;
  }
  return live.variant == Jak2Pris2Bucket228Variant::OneEyeChunk &&
         eye_chunks_match(live.eye_chunk, copied.eye_chunk);
}

}  // namespace metal_renderer
