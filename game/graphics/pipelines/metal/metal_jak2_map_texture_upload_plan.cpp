#include "game/graphics/pipelines/metal/metal_jak2_map_texture_upload_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"
#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr std::size_t kMaximumTransfers = 64;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
constexpr s64 kOrdinaryUploadMode = -1;
constexpr std::size_t kPageHeaderLengthOffset = 12;

// texture-anim-pc values from texture-anim.gc.
constexpr u32 kAnimStart = 12;
constexpr u32 kAnimFinish = 13;
constexpr u32 kAnimClut16x16 = 15;
constexpr u32 kAnimGenericVram = 16;
constexpr u8 kPsmCt32 = 0x00;
constexpr u8 kPsmMt8 = 0x13;
constexpr u32 kImageDestination = 8;
constexpr u32 kClutDestination = 0;
constexpr u32 kClutBytes = kJak2MapIndexedLayerClutWords * sizeof(u32);
// draw-non-city-map image stride (#x1a000) and second CLUT (+1024).
constexpr u32 kLayerImageStride = 0x1a000;

// GS register addresses.
constexpr u64 kGsTest1 = 0x47;
constexpr u64 kGsAlpha1 = 0x42;
constexpr u64 kGsClamp1 = 0x08;
constexpr u64 kGsTex0_1 = 0x06;
constexpr u64 kGsTex1_1 = 0x14;
constexpr u64 kGsTexflush = 0x3f;
// dma-buffer.gc:336-337 always writes sixteen A+D register nibbles.
constexpr u64 kGsSetAdRegs = 0xeeeeeeeeeeeeeeeeull;

// bigmap.gc :700-703 and draw-non-city-map gs-set templates.
constexpr u64 kSetupTest = 0x1 | (0x3ull << 12) | (0x1ull << 16) | (0x1ull << 17);
constexpr u64 kSetupAlpha = (0x1ull << 2) | (0x1ull << 6);
constexpr u64 kSetupClamp = 0x1 | (0x1ull << 2);
constexpr u64 kLayerTex0 = 0x8 | (0x8ull << 14) | (0x13ull << 20) | (0x9ull << 26) |
                           (0x9ull << 30) | (0x1ull << 61);
constexpr u64 kLayerTex0Tcc = kLayerTex0 | (0x1ull << 34);
constexpr u64 kLayerTex1 = (0x1ull << 5) | (0x1ull << 6);

// bigmap-h.gc sprite-tmpl.
constexpr u64 kSpriteGifTag = 0x1 | (0x1ull << 15) | (0x1ull << 46) | (0x156ull << 47) |
                              (0x5ull << 60);
constexpr u64 kSpriteGifRegs = 0x53531;
constexpr s32 kSpriteZ = 0xfffff0;
constexpr s32 kSpriteU1 = 8192;
constexpr s32 kSpriteV1 = 3328;

struct GsWrite {
  u64 reg;
  u64 value;
};

struct Transfer {
  DmaTag tag{0};
  u32 ordinal = 0;
  u32 tag_offset = 0;
  const u8* data = nullptr;
  u32 size_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
};

template <typename T>
T read(const u8* address) {
  T result;
  std::memcpy(&result, address, sizeof(result));
  return result;
}

bool range_ok(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

class Follower {
 public:
  Follower(const u8* memory, std::size_t size, u32 start)
      : m_memory(memory), m_size(std::min<std::size_t>(size, EE_MAIN_MEM_SIZE)), m_offset(start) {}

  u32 offset() const { return m_offset; }
  u32 count() const { return static_cast<u32>(m_count); }

  bool read_next(Transfer* out) {
    if (m_count == m_visited.size() || (m_offset & 15) != 0 || !range_ok(m_offset, 16, m_size) ||
        std::find(m_visited.begin(), m_visited.begin() + m_count, m_offset) !=
            m_visited.begin() + m_count) {
      return false;
    }
    const DmaTag tag(read<u64>(m_memory + m_offset));
    if (tag.spr) {
      return false;
    }
    const u64 bytes = static_cast<u64>(tag.qwc) * 16;
    const u64 data = static_cast<u64>(m_offset) + 16;
    u64 next = 0;
    if (tag.kind == DmaTag::Kind::CNT && tag.addr == 0) {
      next = data + bytes;
    } else if (tag.kind == DmaTag::Kind::NEXT && tag.addr != 0) {
      next = tag.addr;
    } else {
      return false;
    }
    if (!range_ok(data, bytes, m_size) || (next & 15) != 0 || !range_ok(next, 0, m_size) ||
        next > std::numeric_limits<u32>::max()) {
      return false;
    }
    out->tag = tag;
    out->ordinal = static_cast<u32>(m_count);
    out->tag_offset = m_offset;
    out->data = m_memory + data;
    out->size_bytes = static_cast<u32>(bytes);
    out->vif0 = read<u32>(m_memory + m_offset + 8);
    out->vif1 = read<u32>(m_memory + m_offset + 12);
    m_visited[m_count++] = m_offset;
    m_offset = static_cast<u32>(next);
    return true;
  }

 private:
  const u8* m_memory;
  std::size_t m_size;
  u32 m_offset;
  std::array<u32, kMaximumTransfers> m_visited = {};
  std::size_t m_count = 0;
};

struct Walk {
  Follower dma;
  Jak2MapMixedDiagnostic* diagnostic;
  Transfer t;

  bool fail(Jak2MapMixedRejection rejection, u32 ordinal) {
    if (diagnostic) {
      diagnostic->rejection = rejection;
      diagnostic->transfer_ordinal = ordinal;
    }
    return false;
  }
  bool next() {
    if (!dma.read_next(&t)) {
      return fail(Jak2MapMixedRejection::Chain, dma.count());
    }
    return true;
  }
};

bool is_inert_next(const Transfer& t) {
  return t.tag.kind == DmaTag::Kind::NEXT && t.size_bytes == 0 && t.vif0 == 0 && t.vif1 == 0;
}

bool is_cnt(const Transfer& t, u16 qwc, u32 vif0, u32 vif1) {
  return t.tag.kind == DmaTag::Kind::CNT && t.tag.qwc == qwc && t.vif0 == vif0 && t.vif1 == vif1;
}

bool is_anim_marker(const Transfer& t, u32 kind, u16 qwc) {
  return is_cnt(t, qwc, kPcPortVif | kind, 0);
}

bool is_direct(const Transfer& t, u32 vif0, u16 qwc) {
  return is_cnt(t, qwc, vif0, kDirectVif | qwc);
}

// dma-buffer-add-gs-set: one A+D GIF tag followed by exact register writes.
template <std::size_t N>
bool is_gs_set(const Transfer& t, const std::array<GsWrite, N>& writes) {
  if (!is_direct(t, 0, N + 1)) {
    return false;
  }
  if (read<u64>(t.data) != (0x1 | (0x1ull << 15) | (static_cast<u64>(N) << 60)) ||
      read<u64>(t.data + 8) != kGsSetAdRegs) {
    return false;
  }
  for (std::size_t i = 0; i < N; i++) {
    if (read<u64>(t.data + 16 * (i + 1)) != writes[i].value ||
        read<u64>(t.data + 16 * (i + 1) + 8) != writes[i].reg) {
      return false;
    }
  }
  return true;
}

bool layer_state_ok(const Transfer& t, std::size_t layer) {
  switch (layer) {
    case 0:
      return is_gs_set<3>(t, {{{kGsTex0_1, kLayerTex0}, {kGsTex1_1, kLayerTex1}, {kGsTexflush, 1}}});
    case 1:
      return is_gs_set<1>(t, {{{kGsTexflush, 0}}});
    case 2:
      return is_gs_set<3>(t,
                          {{{kGsTex0_1, kLayerTex0Tcc}, {kGsTex1_1, kLayerTex1}, {kGsTexflush, 1}}});
    default:
      return is_gs_set<1>(t, {{{kGsTexflush, 1}}});
  }
}

bool position_ok(s32 v) {
  return v >= 0 && v <= 0xffff && (v & 15) == 0;
}

// sprite-dma: template GIF tag, color, uv0, xyz0, uv1, xyz1.
bool sprite_ok(const Transfer& t, Jak2MapLayerSpriteRect* rect) {
  if (!is_direct(t, 0, 6) || read<u64>(t.data) != kSpriteGifTag ||
      read<u64>(t.data + 8) != kSpriteGifRegs) {
    return false;
  }
  for (int i = 0; i < 4; i++) {
    const s32 c = read<s32>(t.data + 16 + 4 * i);
    if (c < 0 || c > 0xff) {
      return false;
    }
  }
  const auto q = [&](int quad, int lane) { return read<s32>(t.data + 16 * quad + 4 * lane); };
  if (q(2, 0) != 0 || q(2, 1) != 0 || q(2, 2) != 0 || q(2, 3) != 0 || q(4, 0) != kSpriteU1 ||
      q(4, 1) != kSpriteV1 || q(4, 2) != 0 || q(4, 3) != 0 || q(3, 2) != kSpriteZ ||
      q(3, 3) != 0 || q(5, 2) != kSpriteZ || q(5, 3) != 0) {
    return false;
  }
  rect->x0 = q(3, 0);
  rect->y0 = q(3, 1);
  rect->x1 = q(5, 0);
  rect->y1 = q(5, 1);
  return position_ok(rect->x0) && position_ok(rect->y0) && position_ok(rect->x1) &&
         position_ok(rect->y1);
}

struct UploadRecord {
  u32 data;
  u16 width;
  u16 height;
  u32 dest;
  u8 format;
  u8 force;
};

UploadRecord upload_record(const Transfer& t) {
  return {read<u32>(t.data),     read<u16>(t.data + 4), read<u16>(t.data + 6),
          read<u32>(t.data + 8), read<u8>(t.data + 12), read<u8>(t.data + 13)};
}

bool parse_layer(Walk* w,
                 const u8* ee,
                 std::size_t ee_size,
                 std::size_t layer,
                 Jak2MapIndexedLayerUpload* out,
                 Jak2MapLayerSpriteRect* rect,
                 Jak2MapUploadOp* op) {
  constexpr std::array<std::pair<u32, u16>, 4> kMarkers = {
      {{kAnimStart, 0}, {kAnimClut16x16, 1}, {kAnimGenericVram, 1}, {kAnimFinish, 0}}};
  UploadRecord clut{};
  UploadRecord image{};
  for (std::size_t i = 0; i < kMarkers.size(); i++) {
    if (i != 0 && !w->next()) {
      return false;
    }
    if (!is_anim_marker(w->t, kMarkers[i].first, kMarkers[i].second)) {
      return w->fail(Jak2MapMixedRejection::LayerMarker, w->t.ordinal);
    }
    op->marker_transfer_ordinals[i] = w->t.ordinal;
    op->marker_tag_offsets[i] = w->t.tag_offset;
    if (i == 1) {
      clut = upload_record(w->t);
      if (clut.width != 16 || clut.height != 16 || clut.dest != kClutDestination ||
          clut.format != kPsmCt32 || clut.force != 0 || clut.data == 0) {
        return w->fail(Jak2MapMixedRejection::LayerUploadRecord, w->t.ordinal);
      }
      if (clut.data < EE_MAIN_MEM_LOW_PROTECT || !range_ok(clut.data, kClutBytes, ee_size)) {
        return w->fail(Jak2MapMixedRejection::LayerSourceRange, w->t.ordinal);
      }
    } else if (i == 2) {
      image = upload_record(w->t);
      if (image.width != kJak2MapIndexedLayerWidth || image.height != kJak2MapIndexedLayerHeight ||
          image.dest != kImageDestination || image.format != kPsmMt8 || image.force != 1 ||
          image.data == 0) {
        return w->fail(Jak2MapMixedRejection::LayerUploadRecord, w->t.ordinal);
      }
      if (image.data < EE_MAIN_MEM_LOW_PROTECT ||
          !range_ok(image.data, kJak2MapIndexedLayerIndexBytes, ee_size)) {
        return w->fail(Jak2MapMixedRejection::LayerSourceRange, w->t.ordinal);
      }
    }
  }
  op->kind = Jak2MapUploadOpKind::AnimationLayer;
  op->index = static_cast<u8>(layer);
  op->marker_count = 4;

  if (!w->next()) {
    return false;
  }
  if (!layer_state_ok(w->t, layer)) {
    return w->fail(Jak2MapMixedRejection::LayerGsState, w->t.ordinal);
  }
  if (!w->next()) {
    return false;
  }
  if (!sprite_ok(w->t, rect)) {
    return w->fail(Jak2MapMixedRejection::LayerSprite, w->t.ordinal);
  }

  out->width = image.width;
  out->height = image.height;
  out->indices.assign(ee + image.data, ee + image.data + kJak2MapIndexedLayerIndexBytes);
  std::memcpy(out->clut.data(), ee + clut.data, kClutBytes);
  out->index_source_offset = image.data;
  out->clut_source_offset = clut.data;
  return true;
}

// draw-non-city-map address arithmetic: image layers at +0/+#x1a000/+#x34000/+#x4e000 from the
// first image; CLUT A for layers 0-1, CLUT A+1024 for layers 2-3. Sprites alternate the y0..y2
// and y2..y1 halves over one x0..x1 span.
bool layers_related(const Jak2MapTextureUploadTypedPlan& plan, Jak2MapMixedRejection* why) {
  const auto& l = plan.layers;
  const u64 image = *l[0].index_source_offset;
  const u64 clut = *l[0].clut_source_offset;
  for (std::size_t i = 1; i < kJak2MapIndexedLayerMaximum; i++) {
    if (*l[i].index_source_offset != image + kLayerImageStride * i) {
      *why = Jak2MapMixedRejection::LayerSourceRelation;
      return false;
    }
  }
  if (*l[1].clut_source_offset != clut || *l[2].clut_source_offset != clut + kClutBytes ||
      *l[3].clut_source_offset != clut + kClutBytes) {
    *why = Jak2MapMixedRejection::LayerSourceRelation;
    return false;
  }
  const auto& s = plan.layer_sprites;
  const bool same_upper = s[0].x0 == s[2].x0 && s[0].y0 == s[2].y0 && s[0].x1 == s[2].x1 &&
                          s[0].y1 == s[2].y1;
  const bool same_lower = s[1].x0 == s[3].x0 && s[1].y0 == s[3].y0 && s[1].x1 == s[3].x1 &&
                          s[1].y1 == s[3].y1;
  if (!same_upper || !same_lower || s[0].x0 != s[1].x0 || s[0].x1 != s[1].x1 ||
      s[0].y1 != s[1].y0) {
    *why = Jak2MapMixedRejection::LayerGeometry;
    return false;
  }
  return true;
}

bool parse_ordinary(Walk* w,
                    const u8* ee,
                    std::size_t ee_size,
                    Jak2Bucket4OrdinaryUploadPlan* out,
                    Transfer* descriptor) {
  if (!w->next()) {
    return false;
  }
  if (!is_cnt(w->t, 1, kPcPortVif, 3)) {
    return w->fail(Jak2MapMixedRejection::OrdinaryGroup, w->t.ordinal);
  }
  *descriptor = w->t;
  const u64 page = read<u64>(w->t.data);
  const s64 mode = read<s64>(w->t.data + 8);
  const std::size_t size = std::min<std::size_t>(ee_size, EE_MAIN_MEM_SIZE);
  if (mode != kOrdinaryUploadMode || page < EE_MAIN_MEM_LOW_PROTECT ||
      !range_ok(page, kJak2Bucket4OrdinaryPageHeaderBytes, size)) {
    return w->fail(Jak2MapMixedRejection::OrdinaryContents, w->t.ordinal);
  }
  const s32 length = read<s32>(ee + page + kPageHeaderLengthOffset);
  if (length < 0 || !range_ok(page + kJak2Bucket4OrdinaryPageHeaderBytes,
                              static_cast<u64>(length) * sizeof(u32), size)) {
    return w->fail(Jak2MapMixedRejection::OrdinaryContents, w->t.ordinal);
  }
  out->page_offset = page;
  out->mode = mode;
  std::memcpy(out->page_header.data(), ee + page, out->page_header.size());
  if (!w->next()) {
    return false;
  }
  if (!is_inert_next(w->t)) {
    return w->fail(Jak2MapMixedRejection::OrdinaryGroup, w->t.ordinal);
  }
  return true;
}

std::optional<Jak2MapTextureUploadTypedPlan> wrap_legacy(const u8* snapshot,
                                                         std::size_t snapshot_size,
                                                         u32 chain_offset,
                                                         const u8* ee,
                                                         std::size_t ee_size,
                                                         Jak2MapMixedDiagnostic* diagnostic) {
  Jak2MapTextureUploadDiagnostic legacy_diagnostic;
  const auto legacy = plan_jak2_map_texture_upload(snapshot, snapshot_size, chain_offset, ee,
                                                   ee_size, &legacy_diagnostic);
  if (!legacy) {
    if (diagnostic) {
      diagnostic->rejection = Jak2MapMixedRejection::Legacy;
      diagnostic->transfer_ordinal = legacy_diagnostic.rejected_transfer;
    }
    return std::nullopt;
  }
  Jak2MapTextureUploadTypedPlan plan;
  plan.present = legacy->present;
  plan.ordinary_count = legacy->upload_count;
  for (std::size_t i = 0; i < legacy->upload_count; i++) {
    plan.ordinary[i] = legacy->uploads[i];
    auto& op = plan.ops[plan.op_count++];
    op.kind = Jak2MapUploadOpKind::OrdinaryUpload;
    op.index = static_cast<u8>(i);
  }
  plan.pc_port_marker_count = legacy->upload_count;
  return plan;
}

}  // namespace

std::optional<Jak2MapTextureUploadTypedPlan> plan_jak2_map_texture_upload_typed(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2MapMixedDiagnostic* diagnostic) {
  if (diagnostic) {
    *diagnostic = {};
  }
  const u64 bucket = static_cast<u64>(chain_offset) + kJak2MapTextureUploadBucket * 16;
  const u64 end = bucket + 16;
  const std::size_t snapshot_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  const std::size_t ee_size = std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  Walk w{Follower(dma_packet_snapshot, snapshot_size, static_cast<u32>(bucket)), diagnostic, {}};
  if (!dma_packet_snapshot || !live_ee_memory || end > std::numeric_limits<u32>::max() ||
      !range_ok(bucket, 16, snapshot_size)) {
    w.fail(Jak2MapMixedRejection::BucketRange, kJak2MapMixedNoTransfer);
    return std::nullopt;
  }

  // Only a NEXT entry followed by the bigmap GS setup selects the mixed form.
  if (!w.next()) {
    return std::nullopt;
  }
  const bool entry_next = is_inert_next(w.t);
  const bool setup_shape = entry_next && w.next() && is_direct(w.t, 0, 4);
  if (!setup_shape) {
    if (diagnostic) {
      *diagnostic = {};
    }
    return wrap_legacy(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                       live_ee_memory, live_ee_memory_size, diagnostic);
  }
  if (!is_gs_set<3>(w.t, {{{kGsTest1, kSetupTest}, {kGsAlpha1, kSetupAlpha}, {kGsClamp1, kSetupClamp}}})) {
    w.fail(Jak2MapMixedRejection::GsSetup, w.t.ordinal);
    return std::nullopt;
  }

  Jak2MapTextureUploadTypedPlan plan;
  plan.mixed = true;
  std::size_t direct = 1;
  for (std::size_t layer = 0; layer < kJak2MapIndexedLayerMaximum; layer++) {
    if (!w.next()) {
      return std::nullopt;
    }
    if (is_inert_next(w.t)) {
      w.fail(Jak2MapMixedRejection::LayerCount, w.t.ordinal);
      return std::nullopt;
    }
    if (!parse_layer(&w, live_ee_memory, ee_size, layer, &plan.layers[layer],
                     &plan.layer_sprites[layer], &plan.ops[plan.op_count])) {
      return std::nullopt;
    }
    plan.op_count++;
    plan.layer_count++;
    direct += 2;
  }
  Jak2MapMixedRejection why = Jak2MapMixedRejection::None;
  if (!layers_related(plan, &why)) {
    w.fail(why, plan.ops[0].marker_transfer_ordinals[0]);
    return std::nullopt;
  }

  // with-dma-buffer-add-bucket closes the bigmap chunk with two inert links.
  for (int i = 0; i < 2; i++) {
    if (!w.next()) {
      return std::nullopt;
    }
    if (!is_inert_next(w.t)) {
      w.fail(i == 0 && is_anim_marker(w.t, kAnimStart, 0) ? Jak2MapMixedRejection::LayerCount
                                                           : Jak2MapMixedRejection::Transition,
             w.t.ordinal);
      return std::nullopt;
    }
  }

  // Legacy ordinary tail: Direct q2, PC_PORT descriptor, inert NEXT; then FLUSHA Direct q10.
  if (!w.next()) {
    return std::nullopt;
  }
  while (is_direct(w.t, 0, 2)) {
    if (plan.ordinary_count == kJak2MapTextureUploadMaximumGroups) {
      w.fail(Jak2MapMixedRejection::GroupLimit, w.t.ordinal);
      return std::nullopt;
    }
    Transfer descriptor;
    if (!parse_ordinary(&w, live_ee_memory, live_ee_memory_size,
                        &plan.ordinary[plan.ordinary_count], &descriptor)) {
      return std::nullopt;
    }
    auto& op = plan.ops[plan.op_count++];
    op.kind = Jak2MapUploadOpKind::OrdinaryUpload;
    op.index = static_cast<u8>(plan.ordinary_count);
    op.marker_transfer_ordinals[0] = descriptor.ordinal;
    op.marker_tag_offsets[0] = descriptor.tag_offset;
    op.marker_count = 1;
    plan.ordinary_count++;
    direct++;
    if (!w.next()) {
      return std::nullopt;
    }
  }
  if (plan.ordinary_count == 0) {
    w.fail(Jak2MapMixedRejection::OrdinaryGroup, w.t.ordinal);
    return std::nullopt;
  }
  if (!is_direct(w.t, kFlushaVif, 10)) {
    w.fail(Jak2MapMixedRejection::Tail, w.t.ordinal);
    return std::nullopt;
  }
  direct++;
  if (!w.next()) {
    return std::nullopt;
  }
  if (!is_inert_next(w.t)) {
    w.fail(Jak2MapMixedRejection::FinalBoundary, w.t.ordinal);
    return std::nullopt;
  }
  if (w.dma.offset() != static_cast<u32>(end)) {
    w.fail(Jak2MapMixedRejection::BucketEnd, w.t.ordinal);
    return std::nullopt;
  }
  plan.present = true;
  plan.transfer_count = w.dma.count();
  plan.pc_port_marker_count = plan.layer_count * 4 + plan.ordinary_count;
  plan.direct_replay_count = direct;
  return plan;
}

const Jak2MapUploadOp* Jak2MapUploadCursor::peek() const {
  return m_plan.op_count <= m_plan.ops.size() && m_position < m_plan.op_count
             ? &m_plan.ops[m_position]
             : nullptr;
}

bool Jak2MapUploadCursor::consume(Jak2MapUploadOpKind kind, std::size_t index) {
  const Jak2MapUploadOp* op = peek();
  if (!op || op->kind != kind || op->index != index) {
    m_rejected++;
    return false;
  }
  m_position++;
  return true;
}

bool Jak2MapUploadCursor::consume_animation_layer(std::size_t layer_index) {
  return consume(Jak2MapUploadOpKind::AnimationLayer, layer_index);
}

bool Jak2MapUploadCursor::consume_ordinary_upload(std::size_t ordinary_index) {
  return consume(Jak2MapUploadOpKind::OrdinaryUpload, ordinary_index);
}

const char* jak2_map_mixed_rejection_name(Jak2MapMixedRejection rejection) {
  switch (rejection) {
    case Jak2MapMixedRejection::None: return "none";
    case Jak2MapMixedRejection::BucketRange: return "bucket_range";
    case Jak2MapMixedRejection::Chain: return "chain";
    case Jak2MapMixedRejection::InitialBoundary: return "initial_boundary";
    case Jak2MapMixedRejection::GsSetup: return "gs_setup";
    case Jak2MapMixedRejection::LayerMarker: return "layer_marker";
    case Jak2MapMixedRejection::LayerUploadRecord: return "layer_upload_record";
    case Jak2MapMixedRejection::LayerSourceRange: return "layer_source_range";
    case Jak2MapMixedRejection::LayerSourceRelation: return "layer_source_relation";
    case Jak2MapMixedRejection::LayerGsState: return "layer_gs_state";
    case Jak2MapMixedRejection::LayerSprite: return "layer_sprite";
    case Jak2MapMixedRejection::LayerGeometry: return "layer_geometry";
    case Jak2MapMixedRejection::LayerCount: return "layer_count";
    case Jak2MapMixedRejection::Transition: return "transition";
    case Jak2MapMixedRejection::OrdinaryGroup: return "ordinary_group";
    case Jak2MapMixedRejection::OrdinaryContents: return "ordinary_contents";
    case Jak2MapMixedRejection::GroupLimit: return "group_limit";
    case Jak2MapMixedRejection::Tail: return "tail";
    case Jak2MapMixedRejection::FinalBoundary: return "final_boundary";
    case Jak2MapMixedRejection::BucketEnd: return "bucket_end";
    case Jak2MapMixedRejection::Legacy: return "legacy";
  }
  return "unknown";
}

}  // namespace metal_renderer
