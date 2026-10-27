#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_sprite_texture_upload_plan.h"

namespace metal_renderer {

constexpr std::size_t kJak2MapIndexedLayerMaximum = 4;
constexpr u16 kJak2MapIndexedLayerWidth = 512;
constexpr u16 kJak2MapIndexedLayerHeight = 208;
constexpr std::size_t kJak2MapIndexedLayerIndexBytes =
    static_cast<std::size_t>(kJak2MapIndexedLayerWidth) * kJak2MapIndexedLayerHeight;
constexpr std::size_t kJak2MapIndexedLayerClutWords = 256;
constexpr std::size_t kJak2MapMixedUploadMaximumOps =
    kJak2MapIndexedLayerMaximum + kJak2MapTextureUploadMaximumGroups;
constexpr u32 kJak2MapMixedNoTransfer = 0xffffffff;

/*!
 * One pc-upload-map-texture layer from bigmap.gc draw-non-city-map. The plan owns copies of the
 * 512x208 PSMT8 indices and the 16x16 PSMCT32 CLUT words; the source offsets are EE byte offsets
 * retained only as metadata. The pure converter consumes indices/clut/width/height only.
 */
struct Jak2MapIndexedLayerUpload {
  std::vector<u8> indices;
  std::array<u32, kJak2MapIndexedLayerClutWords> clut = {};
  u16 width = 0;
  u16 height = 0;
  std::optional<u32> index_source_offset;
  std::optional<u32> clut_source_offset;
};

/*! Rectangle from the source sprite-dma that follows each layer, in GS 12.4 fixed point. */
struct Jak2MapLayerSpriteRect {
  s32 x0 = 0;
  s32 y0 = 0;
  s32 x1 = 0;
  s32 y1 = 0;
};

enum class Jak2MapUploadOpKind : u8 { AnimationLayer, OrdinaryUpload };

/*!
 * One host-visible callback in DMA order. Animation layers carry the transfer ordinals of their
 * START/CLUT/GENERIC/FINISH PC_PORT markers; ordinary uploads carry their descriptor ordinal. All
 * ordinals index the planner's walk from the bucket entry (ordinal 0).
 */
struct Jak2MapUploadOp {
  Jak2MapUploadOpKind kind = Jak2MapUploadOpKind::AnimationLayer;
  u8 index = 0;
  std::array<u32, 4> marker_transfer_ordinals = {kJak2MapMixedNoTransfer, kJak2MapMixedNoTransfer,
                                                 kJak2MapMixedNoTransfer, kJak2MapMixedNoTransfer};
  std::array<u32, 4> marker_tag_offsets = {};
  u8 marker_count = 0;
};

/*!
 * Typed bucket-319 plan. `mixed == false` wraps the existing ordinary-only planner unchanged.
 * `mixed == true` is the source-shaped non-city bigmap form: GS setup, exactly four indexed layers,
 * two inert NEXT links, one to eight ordinary groups, FLUSHA tail, final NEXT. Between the two
 * NEXT links the bigmap.gc:710-825 icon chunk may appear: an optional mini-map-icons ADGIF followed
 * by its draw-from-minimap sprites, then an optional map-target-marker ADGIF + TRI_STRIP pair.
 * Icon sprites without the preceding icon ADGIF (the source's missing-texture error path) are not
 * accepted. Direct GS state, sprite and icon draws are validated against their GOAL templates but
 * remain normal Direct replay; this plan does not execute them. City maps (bigmap-index 20) fail
 * closed.
 */
struct Jak2MapTextureUploadTypedPlan {
  bool present = false;
  bool mixed = false;
  std::array<Jak2MapIndexedLayerUpload, kJak2MapIndexedLayerMaximum> layers = {};
  std::size_t layer_count = 0;
  std::array<Jak2MapLayerSpriteRect, kJak2MapIndexedLayerMaximum> layer_sprites = {};
  std::array<Jak2Bucket4OrdinaryUploadPlan, kJak2MapTextureUploadMaximumGroups> ordinary = {};
  std::size_t ordinary_count = 0;
  std::array<Jak2MapUploadOp, kJak2MapMixedUploadMaximumOps> ops = {};
  std::size_t op_count = 0;
  // Parity expectations for later copied-marker comparison.
  std::size_t transfer_count = 0;
  std::size_t pc_port_marker_count = 0;
  std::size_t direct_replay_count = 0;
};

enum class Jak2MapMixedRejection : u8 {
  None,
  BucketRange,
  Chain,
  InitialBoundary,
  GsSetup,
  LayerMarker,
  LayerUploadRecord,
  LayerSourceRange,
  LayerSourceRelation,
  LayerGsState,
  LayerSprite,
  LayerGeometry,
  LayerCount,
  Transition,
  OrdinaryGroup,
  OrdinaryContents,
  GroupLimit,
  Tail,
  FinalBoundary,
  BucketEnd,
  Legacy,
  IconAdgif,
  IconSprite,
  IconTarget,
  IconOrder,
  IconShape,
};

struct Jak2MapMixedDiagnostic {
  Jak2MapMixedRejection rejection = Jak2MapMixedRejection::None;
  u32 transfer_ordinal = kJak2MapMixedNoTransfer;
};

const char* jak2_map_mixed_rejection_name(Jak2MapMixedRejection rejection);

/*!
 * Plan TEX_ALL_MAP. Walks the snapshot chain from the bucket entry through the next bucket entry.
 * Forms not beginning with the bigmap GS setup are delegated to plan_jak2_map_texture_upload.
 * Layer index and CLUT bytes are copied from live EE memory after exact range checks; no pointer
 * is retained. Any unsupported form returns no plan.
 */
std::optional<Jak2MapTextureUploadTypedPlan> plan_jak2_map_texture_upload_typed(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2MapMixedDiagnostic* diagnostic = nullptr);

/*!
 * Ordered consumer for a typed plan. A callback of the wrong kind, index, or past the end returns
 * false without advancing. complete() is true only after every op has been consumed.
 */
class Jak2MapUploadCursor {
 public:
  explicit Jak2MapUploadCursor(const Jak2MapTextureUploadTypedPlan& plan) : m_plan(plan) {}

  const Jak2MapUploadOp* peek() const;
  bool consume_animation_layer(std::size_t layer_index);
  bool consume_ordinary_upload(std::size_t ordinary_index);
  bool complete() const { return m_plan.op_count <= m_plan.ops.size() && m_position == m_plan.op_count; }
  std::size_t position() const { return m_position; }
  std::size_t rejected_count() const { return m_rejected; }

 private:
  bool consume(Jak2MapUploadOpKind kind, std::size_t index);

  const Jak2MapTextureUploadTypedPlan& m_plan;
  std::size_t m_position = 0;
  std::size_t m_rejected = 0;
};

}  // namespace metal_renderer
