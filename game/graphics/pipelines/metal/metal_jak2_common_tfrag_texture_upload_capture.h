#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

namespace metal_renderer {

constexpr u32 kJak2CommonTfragTextureUploadBucket = 187;
constexpr u32 kJak2CommonPrisTextureUploadBucket = 220;
constexpr std::array<u32, 6> kJak2NormalTfragTextureUploadBuckets = {7, 18, 29, 40, 51, 62};
constexpr std::array<u32, 7> kJak2NormalShrubTextureUploadBuckets = {
    73, 82, 91, 100, 109, 118, 191};
constexpr std::array<u32, 6> kJak2AlphaTextureUploadBuckets = {127, 137, 147, 157, 167, 177};
constexpr std::array<u32, 6> kJak2PrisTextureUploadBuckets = {196, 200, 204, 208, 212, 216};
// Observation-only Samos-hut probes. These buckets remain deferred in the Metal bucket table.
constexpr std::array<u32, 2> kJak2Pris2CaptureBuckets = {228, 229};
constexpr std::array<u32, 6> kJak2WaterTextureUploadBuckets = {252, 261, 270, 279, 288, 297};
constexpr std::size_t kJak2CommonTfragTextureUploadMaximumTransfers = 64;
constexpr std::size_t kJak2CommonTfragTextureAnimatorOpcodeCount = 44;
constexpr std::size_t kJak2PrisEyeMaximumChunks = 2;
constexpr u32 kJak2PrisEyeChunkTransferCount = 26;
constexpr u32 kJak2PrisEyeChunkPayloadBytes = 1856;
constexpr u16 kJak2CommonPrisDarkJakAnimatorOpcode = 22;
constexpr u32 kJak2CommonPrisDarkJakAnimatorBodyBytes = 32;
constexpr std::size_t kJak2CommonPrisDarkJakAnimatorTbpCount = 4;
constexpr u16 kJak2PrisPrisonJakAnimatorOpcode = 23;
constexpr u16 kJak2PrisPrisonJakAnimatorStartOpcode = 12;
constexpr u16 kJak2PrisPrisonJakAnimatorFinishOpcode = 13;
constexpr u32 kJak2PrisPrisonJakAnimatorBodyBytes = 48;
constexpr std::size_t kJak2PrisPrisonJakAnimatorTbpCount = 7;
constexpr std::size_t kJak2PrisPrisonJakAnimatorSourcePaddingBytes = 16;
constexpr u32 kJak2PrisPrisonJakAnimatorTbpUpperBound = 0x40000;
constexpr u32 kJak2PrisPrisonJakAnimatorMissingTbp = 0xffffffff;

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

enum class Jak2PrisEyeResolution : u8 {
  Eye32,
  Eye64,
};

enum class Jak2PrisEyeTextureUploadRejectReason : u8 {
  None,
  UnsupportedBucket,
  CaptureInvalid,
  AbsentEnvelope,
  Counts,
  Opening,
  Descriptor,
  Page,
  ChunkBounds,
  SetupTransfer,
  SetupTag,
  SetupValues,
  InitialTest,
  BodyAdgif,
  BodyScissor,
  BodySprite,
  BodyTest,
  ResetTransfer,
  ResetTag,
  ResetValues,
  Alpha,
  Linker,
  SourceFramebuffer,
  DuplicateEyeSlots,
  DefaultReset,
  Terminal,
  ChunkFingerprint,
};

constexpr u8 kJak2PrisEyeRejectIndexNotApplicable = 0xff;

struct Jak2PrisEyeTextureUploadRejection {
  Jak2PrisEyeTextureUploadRejectReason reason =
      Jak2PrisEyeTextureUploadRejectReason::None;
  u8 chunk_index = kJak2PrisEyeRejectIndexNotApplicable;
  u8 body_index = kJak2PrisEyeRejectIndexNotApplicable;
};

struct Jak2PrisEyeChunkPlan {
  Jak2PrisEyeResolution resolution = Jak2PrisEyeResolution::Eye32;
  u32 pair_index = 0;
  u32 start_transfer_index = 0;
  u32 start_relative_tag_offset = 0;
  u32 linker_transfer_index = 0;
  u32 linker_relative_tag_offset = 0;
  u32 transfer_count = 0;
  u32 payload_bytes = 0;
  u64 eye_slot_mask = 0;
  u64 semantic_fingerprint = 0;
};

struct Jak2CommonPrisDarkJakAnimatorPlan {
  float morph = 0.f;
  std::array<u8, 12> source_padding = {};
  std::array<u32, kJak2CommonPrisDarkJakAnimatorTbpCount> destination_tbps = {};
  u32 start_transfer_index = 0;
  u32 start_relative_tag_offset = 0;
  u32 body_transfer_index = 0;
  u32 body_relative_tag_offset = 0;
  u32 finish_transfer_index = 0;
  u32 finish_relative_tag_offset = 0;
  u32 linker_transfer_index = 0;
  u32 linker_relative_tag_offset = 0;
  u64 semantic_fingerprint = 0;
};

struct Jak2CommonPrisTextureUploadPlan {
  u32 bucket_id = kJak2CommonPrisTextureUploadBucket;
  bool present = false;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2CommonPrisDarkJakAnimatorPlan dark_jak_animator;
  std::array<Jak2PrisEyeChunkPlan, kJak2PrisEyeMaximumChunks> chunks = {};
  std::size_t chunk_count = 0;
  u32 direct_reset_transfer_index = 0;
  u32 direct_reset_relative_tag_offset = 0;
  u64 direct_reset_semantic_fingerprint = 0;
  u32 terminal_transfer_index = 0;
  u32 terminal_relative_tag_offset = 0;
  u64 eye_slot_mask = 0;
  u64 semantic_fingerprint = 0;
};

struct Jak2PrisPrisonJakAnimatorPlan {
  float morph = 0.f;
  std::array<u32, kJak2PrisPrisonJakAnimatorTbpCount> destination_tbps = {};
  // The source writes only morph.x and the seven TBPs. Keep the unspecified
  // vector tail/final four bytes opaque, but include them in semantic matching.
  std::array<u8, kJak2PrisPrisonJakAnimatorSourcePaddingBytes> source_padding = {};
  u32 start_transfer_index = 0;
  u32 start_relative_tag_offset = 0;
  u32 body_transfer_index = 0;
  u32 body_relative_tag_offset = 0;
  u32 finish_transfer_index = 0;
  u32 finish_relative_tag_offset = 0;
  u32 linker_transfer_index = 0;
  u32 linker_relative_tag_offset = 0;
  u64 semantic_fingerprint = 0;
};

struct Jak2PrisEyeTextureUploadPlan {
  u32 bucket_id = 0;
  bool present = false;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  bool has_prison_jak_animator = false;
  Jak2PrisPrisonJakAnimatorPlan prison_jak_animator;
  std::array<Jak2PrisEyeChunkPlan, kJak2PrisEyeMaximumChunks> chunks = {};
  std::size_t chunk_count = 0;
  u32 direct_reset_transfer_index = 0;
  u32 direct_reset_relative_tag_offset = 0;
  u32 terminal_transfer_index = 0;
  u32 terminal_relative_tag_offset = 0;
  u64 eye_slot_mask = 0;
  u64 semantic_fingerprint = 0;
};

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

struct Jak2Opcode30SecurityEnvironmentPlan {
  float time = 0;
  u32 destination_tbp = 0;
  std::array<u8, 8> source_header_tail = {};
  std::array<Jak2Opcode27LayerTransition, 2> layers = {};
};
static_assert(sizeof(Jak2Opcode30SecurityEnvironmentPlan) == 336);

struct Jak2Opcode30SecurityDotPlan {
  float time = 0;
  u32 destination_tbp = 0;
  std::array<u8, 8> source_header_tail = {};
  std::array<Jak2Opcode27LayerTransition, 3> layers = {};
};
static_assert(sizeof(Jak2Opcode30SecurityDotPlan) == 496);

struct Jak2Opcode30SecurityPlan {
  Jak2Opcode30SecurityEnvironmentPlan environment;
  Jak2Opcode30SecurityDotPlan dot;
};
static_assert(sizeof(Jak2Opcode30SecurityPlan) == 832);

enum class Jak2WaterTextureUploadVariant : u8 {
  Absent,
  DescriptorOnly,
  DescriptorAndStandardReset,
  DescriptorSecurityAndStandardReset,
};

struct Jak2WaterTextureUploadPlan {
  u32 bucket_id = 0;
  bool present = false;
  bool has_security_animator = false;
  Jak2WaterTextureUploadVariant variant = Jak2WaterTextureUploadVariant::Absent;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2Opcode30SecurityPlan security;
};

struct Jak2CommonTfragTextureUploadPlan {
  bool present = false;
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  Jak2Opcode27SkullGemPlan skull_gem;
};

/*!
 * Inspect one audited Jak II texture-setup bucket or exact PRIS2 diagnostic bucket using tag and
 * VIF metadata only.
 * The capture never reads transfer payload contents, retains no source pointers, and performs no
 * texture-pool or renderer mutation. The fixed-size result owns every recorded scalar and is safe
 * after the source snapshot is reused. Tag locations are relative to the bucket-table entry. Eye
 * DMA and otherwise unclassified work are reported as EyeOrOther rather than treated as executable
 * texture uploads. A valid result means only that the bounded metadata envelope was traversed
 * safely; it does not make animator, eye, PRIS2 texture, or PRIS2 Merc work executable.
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
 * Preflight common PRIS bucket 220. Apart from the exact empty form, only the two live source
 * forms are accepted: ordinary/Dark-Jak/reset and ordinary/Dark-Jak/two-eye-chunks/reset. The
 * returned plan owns the ordinary header and opcode-22 scalars and fingerprints every eye chunk.
 * No texture-pool or renderer mutation occurs.
 */
std::optional<Jak2CommonPrisTextureUploadPlan> plan_jak2_common_pris_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr);

bool jak2_common_pris_texture_upload_plans_match(
    const Jak2CommonPrisTextureUploadPlan& live,
    const Jak2CommonPrisTextureUploadPlan& copied);

/*!
 * Preflight the exact source-written per-level PRIS envelopes observed in Jak II: one ordinary
 * upload, an optional prison-Jak clut-blender animator, zero to two complete different-eyes
 * chunks, and the standard inert Direct reset. The animator accepts only its qwc-3 opcode-23
 * body; its finite morph, seven source TBPs, and opaque source padding are owned by the plan.
 * Each eye chunk covers its qwc-8 display setup through its trailing qwc-2 ALPHA setup; the
 * following zero-qwc NEXT linker is identified separately. Relative offsets locate a separately
 * validated snapshot, while semantic matching deliberately ignores relocation of those offsets.
 * No texture-pool or renderer mutation occurs.
 */
std::optional<Jak2PrisEyeTextureUploadPlan> plan_jak2_pris_eye_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture = nullptr,
    Jak2PrisEyeTextureUploadRejection* out_rejection = nullptr);

const char* jak2_pris_eye_texture_upload_reject_reason_name(
    Jak2PrisEyeTextureUploadRejectReason reason);

/*!
 * Compare independently preflighted live and copied plans without requiring identical DMA
 * placement. This is the mutation barrier: execution may begin only after it returns true.
 */
bool jak2_pris_eye_texture_upload_plans_match(
    const Jak2PrisEyeTextureUploadPlan& live,
    const Jak2PrisEyeTextureUploadPlan& copied);

/*!
 * Plan a water page upload written by upload-vram-pages-pris-pc. The page writer itself emits only
 * the ordinary descriptor. A level can append its fixed texture animator to the same bucket, and
 * display-frame-finish appends the standard Direct GS reset to every nonempty normal bucket. The
 * accepted typed variants are descriptor-only, descriptor followed by that exact reset, and the
 * descriptor/security-animator/reset composite.
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
 * Plan the exact Direct-only normal/common SHRUB setup written by Jak II. Both GS payloads are
 * inert for the matching GL TextureUploadHandler, which is constructed without add_direct; no
 * page pointer is read and no texture-pool mutation is planned.
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
