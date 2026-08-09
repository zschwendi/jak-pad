#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

namespace metal_renderer {

constexpr u32 kJak2CommonTfragTextureUploadBucket = 187;
constexpr std::array<u32, 6> kJak2NormalTfragTextureUploadBuckets = {7, 18, 29, 40, 51, 62};
constexpr std::array<u32, 6> kJak2NormalShrubTextureUploadBuckets = {73, 82, 91, 100, 109, 118};
constexpr std::array<u32, 6> kJak2AlphaTextureUploadBuckets = {127, 137, 147, 157, 167, 177};
constexpr std::array<u32, 6> kJak2WaterTextureUploadBuckets = {252, 261, 270, 279, 288, 297};
constexpr std::size_t kJak2CommonTfragTextureUploadMaximumTransfers = 64;
constexpr std::size_t kJak2CommonTfragTextureAnimatorOpcodeCount = 44;

enum class Jak2CommonTfragTextureUploadClass : u8 {
  Malformed,
  Absent,
  OrdinaryOnly,
  AnimatorOnly,
  OrdinaryAndAnimator,
  EyeOrOther,
  GsSetupOnly,
};

struct Jak2CommonTfragTransferMetadata {
  u32 relative_tag_offset = 0;
  u32 payload_bytes = 0;
  u16 qwc = 0;
  u16 vif0_immediate = 0;
  u16 vif1_immediate = 0;
  u8 tag_kind = 0;
  u8 vif0_kind = 0;
  u8 vif1_kind = 0;
};

struct Jak2CommonTfragTextureUploadCapture {
  bool valid = false;
  bool present = false;
  Jak2CommonTfragTextureUploadClass classification =
      Jak2CommonTfragTextureUploadClass::Malformed;

  std::array<Jak2CommonTfragTransferMetadata,
             kJak2CommonTfragTextureUploadMaximumTransfers>
      transfers = {};
  std::size_t transfer_count = 0;
  u64 total_payload_bytes = 0;
  u32 inert_transfers = 0;
  u32 ordinary_descriptors = 0;
  u32 direct_setup_transfers = 0;
  u32 gs_setup_transfers = 0;
  u32 animator_arrays = 0;
  u32 animator_body_transfers = 0;
  u64 animator_payload_bytes = 0;
  std::array<u32, kJak2CommonTfragTextureAnimatorOpcodeCount> opcode_counts = {};
  u32 eye_markers = 0;
  u32 other_transfers = 0;
  u32 malformed_transfers = 0;
};

struct Jak2NormalTfragTextureUploadPlan {
  u32 bucket_id = 0;
  bool present = false;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
};

using Jak2WaterTextureUploadPlan = Jak2NormalTfragTextureUploadPlan;

struct Jak2NormalShrubTextureUploadPlan {
  u32 bucket_id = 0;
  bool present = false;
};

using Jak2AlphaTextureUploadPlan = Jak2NormalShrubTextureUploadPlan;

struct Jak2Opcode27LayerValues {
  std::array<float, 4> color = {};
  std::array<float, 2> scale = {};
  std::array<float, 2> offset = {};
  std::array<float, 2> st_scale = {};
  std::array<float, 2> st_offset = {};
  std::array<float, 4> qs = {};
  float rot = 0;
  float st_rot = 0;
  std::array<u8, 8> source_padding = {};
};
static_assert(sizeof(Jak2Opcode27LayerValues) == 80);

struct Jak2Opcode27LayerTransition {
  Jak2Opcode27LayerValues start;
  Jak2Opcode27LayerValues end;
};
static_assert(sizeof(Jak2Opcode27LayerTransition) == 160);

struct Jak2Opcode27SkullGemPlan {
  float time = 0;
  u32 destination_tbp = 0;
  std::array<u8, 8> source_header_tail = {};
  std::array<Jak2Opcode27LayerTransition, 3> layers = {};
};
static_assert(sizeof(Jak2Opcode27SkullGemPlan) == 496);

struct Jak2CommonTfragTextureUploadPlan {
  bool present = false;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2Opcode27SkullGemPlan skull_gem;
};

/*!
 * Inspect one audited Jak II TFRAG or SHRUB texture-setup bucket using tag and VIF metadata only.
 * The capture never reads transfer payload contents, retains no source pointers, and performs no
 * texture-pool or renderer mutation. The fixed-size result owns every recorded scalar and is safe
 * after the source snapshot is reused. Tag locations are relative to the bucket-table entry. Eye
 * DMA and otherwise unclassified work are reported as EyeOrOther rather than treated as executable
 * texture uploads. A valid result means only that the bounded metadata envelope was traversed
 * safely; it does not make animator or eye work executable.
 */
Jak2CommonTfragTextureUploadCapture capture_jak2_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id);

/*!
 * Plan the exact normal TFRAG texture-upload envelope observed from the Jak II PC writer: ordinary
 * descriptor first, then the FLUSHA/DIRECT setup. The Direct payload remains inert, matching
 * TextureUploadHandler with add_direct disabled. The returned ordinary upload owns its page header
 * and retains no host pointers.
 */
std::optional<Jak2NormalTfragTextureUploadPlan> plan_jak2_normal_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

/*!
 * Plan the water page upload written by upload-vram-pages-pris-pc. The writer
 * and mode are identical to normal TFRAG; *texture-page-translate* supplies
 * only the water category and per-level destination bucket.
 */
std::optional<Jak2WaterTextureUploadPlan> plan_jak2_water_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

/*!
 * Plan the exact Direct-only normal SHRUB setup written by Jak II. Both GS payloads are inert for
 * the matching GL TextureUploadHandler, which is constructed without add_direct; no page pointer
 * is read and no texture-pool mutation is planned.
 */
std::optional<Jak2NormalShrubTextureUploadPlan> plan_jak2_normal_shrub_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

/*!
 * Plan the alpha setup written by upload-vram-pages-pris. On PC that writer is
 * the same Direct-only inert GS setup used by normal SHRUB; category and level
 * change only the destination bucket selected by *texture-page-translate*.
 */
std::optional<Jak2AlphaTextureUploadPlan> plan_jak2_alpha_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

/*!
 * Plan the exact live TEX_LCOM_TFRAG envelope: one ordinary page descriptor, one opcode-27
 * skull-gem update, and the inert terminal GS reset. The plan owns the live page header and all
 * meaningful animator scalars. pc-update-fixed-anim writes only time and destination in its
 * 16-byte header, so the remaining source bytes are retained without interpretation. Its GOAL
 * writer copies five complete vectors per layer endpoint; the final eight source padding bytes are
 * likewise retained but are not consumed as floats. No texture-pool or renderer mutation occurs.
 */
std::optional<Jak2CommonTfragTextureUploadPlan> plan_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

/*! Preserve the original TEX_LCOM_TFRAG (bucket 187) capture seam. */
Jak2CommonTfragTextureUploadCapture capture_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset);

}  // namespace metal_renderer
