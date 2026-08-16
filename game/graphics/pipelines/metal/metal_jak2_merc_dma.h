#pragma once

/*!
 * @file metal_jak2_merc_dma.h
 * Bounded, fail-closed validation for Jak 2's normal opaque Merc PC_PORT chain.
 *
 * The validator never uses DmaFollower. Every tag header, inline CNT payload,
 * and NEXT target is checked against the compacted DMA copy before it is read.
 * This is header-only so focused asset-free tests do not require shared CMake
 * registration.
 */

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "common/dma/dma_chain_read.h"

namespace metal_jak2_merc_dma {

constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kDirect3Vif = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | 3;
constexpr std::size_t kNameBytes = 128;
constexpr std::size_t kLightBytes = 7 * 16;
constexpr std::size_t kMatrixSlotBytes = 128;
constexpr std::size_t kFlagsBytes = 2 * 16;
constexpr std::size_t kBlercBytes = 40 * sizeof(float);
constexpr std::size_t kMercMatrixBytes = 7 * 16;
constexpr std::size_t kMercEffectMinimumBytes = 22;
constexpr u8 kMaxEffectCount = 64;
constexpr u8 kKnownFlagMask = 0xf;

constexpr u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

struct ModelPacket {
  std::string name;
  u64 enable_mask = 0;
  u64 ignore_alpha_mask = 0;
  u16 matrix_count = 0;
  u8 effect_count = 0;
  u8 bit_flags = 0;
  std::size_t lights_offset = kNameBytes;
  std::size_t matrix_slots_offset = kNameBytes + kLightBytes;
  std::size_t matrix_pointers_offset = kNameBytes + kLightBytes + kMatrixSlotBytes;
  std::size_t flags_offset = 0;
  std::size_t blerc_offset = 0;
  std::size_t fades_offset = 0;
  std::size_t effect_pointers_offset = 0;
};

struct Bucket {
  bool empty = false;
  u32 model_count = 0;
  std::vector<ModelPacket> models;
};

// Stable wire values exported through the host metrics. Keep existing values fixed.
enum class PreflightRejectReason : u32 {
  None = 0,
  CompactedCopyBounds = 1,
  OpeningTag = 2,
  VuSetupTag = 3,
  VuSetupPayload = 4,
  GsSetupTag = 5,
  SetupPatch = 6,
  ModelChain = 7,
  ModelPacket = 8,
  ModelPatch = 9,
  LoadedModelMismatch = 10,
};

struct TagView {
  DmaTag::Kind kind = DmaTag::Kind::REFE;
  u16 qwc = 0;
  u32 address = 0;
  bool spr = false;
  u32 vif0 = 0;
  u32 vif1 = 0;
  u32 payload_offset = 0;
  std::size_t payload_size = 0;
  u32 inline_end = 0;
};

inline bool fail(std::string* error, const char* message) {
  if (error) {
    *error = message;
  }
  return false;
}

inline bool span_is_bounded(std::size_t copy_size,
                            std::size_t offset,
                            std::size_t size) {
  return offset <= copy_size && size <= copy_size - offset;
}

constexpr bool is_qword_aligned(u32 offset) {
  return (offset & 0xf) == 0;
}

inline bool read_tag(const u8* copy_base,
                     std::size_t copy_size,
                     u32 offset,
                     TagView* out,
                     std::string* error) {
  if (!is_qword_aligned(offset)) {
    return fail(error, "a 16-byte-aligned DMA tag offset");
  }
  if (!copy_base || !out || !span_is_bounded(copy_size, offset, 16)) {
    return fail(error, "a DMA tag header inside the compacted copy");
  }

  u64 raw_tag = 0;
  std::memcpy(&raw_tag, copy_base + offset, sizeof(raw_tag));
  DmaTag tag(raw_tag);
  if (tag.spr) {
    return fail(error, "Merc DMA without scratchpad addresses");
  }

  const std::size_t payload_offset = static_cast<std::size_t>(offset) + 16;
  const std::size_t payload_size = static_cast<std::size_t>(tag.qwc) * 16;
  if (!span_is_bounded(copy_size, payload_offset, payload_size)) {
    return fail(error, "a complete inline DMA payload inside the compacted copy");
  }
  const std::size_t inline_end = payload_offset + payload_size;
  if (inline_end > std::numeric_limits<u32>::max()) {
    return fail(error, "a DMA inline end representable by DmaFollower");
  }
  if (tag.kind == DmaTag::Kind::NEXT) {
    if (!is_qword_aligned(tag.addr)) {
      return fail(error, "a 16-byte-aligned NEXT target");
    }
    if (!span_is_bounded(copy_size, static_cast<std::size_t>(tag.addr), 16)) {
      return fail(error, "a NEXT target header inside the compacted copy");
    }
  }

  out->kind = tag.kind;
  out->qwc = tag.qwc;
  out->address = tag.addr;
  out->spr = tag.spr;
  std::memcpy(&out->vif0, copy_base + offset + 8, sizeof(out->vif0));
  std::memcpy(&out->vif1, copy_base + offset + 12, sizeof(out->vif1));
  out->payload_offset = static_cast<u32>(payload_offset);
  out->payload_size = payload_size;
  out->inline_end = static_cast<u32>(inline_end);
  return true;
}

inline bool is_zero_next(const TagView& tag) {
  return tag.kind == DmaTag::Kind::NEXT && tag.qwc == 0 && !tag.spr && tag.vif0 == 0 &&
         tag.vif1 == 0;
}

inline bool is_opening_vif0(u32 value) {
  return value == 0 || VifCode(value).kind == VifCode::Kind::NOP ||
         VifCode(value).kind == VifCode::Kind::MARK;
}

inline bool is_opening_vif1(u32 value) {
  return value == 0 || VifCode(value).kind == VifCode::Kind::NOP;
}

inline bool recover_to_boundary(DmaFollower* dma,
                                const u8* copy_base,
                                std::size_t copy_size,
                                u32 next_bucket) {
  if (!dma || !copy_base || !is_qword_aligned(next_bucket) ||
      !span_is_bounded(copy_size, next_bucket, 16)) {
    return false;
  }
  *dma = DmaFollower(copy_base, next_bucket, copy_size);
  return true;
}

inline bool parse_model_packet(const u8* data,
                               std::size_t size,
                               u32 transferred_vif0,
                               u32 transferred_vif1,
                               std::size_t ee_memory_size,
                               ModelPacket* out,
                               std::string* error) {
  if (!out) {
    return fail(error, "a model packet output");
  }
  *out = {};
  if (!data || transferred_vif0 != 0 || transferred_vif1 != kPcPortVif) {
    return fail(error, "an exact zero/PC_PORT model transfer");
  }

  constexpr std::size_t kFixedBytes =
      kNameBytes + kLightBytes + kMatrixSlotBytes + kFlagsBytes;
  if (size < kFixedBytes) {
    return fail(error, "a complete Jak 2 Merc model packet");
  }

  const auto* name_end = static_cast<const u8*>(std::memchr(data, 0, kNameBytes));
  if (!name_end) {
    return fail(error, "a NUL-terminated model name within 128 bytes");
  }
  out->name.assign(reinterpret_cast<const char*>(data),
                   static_cast<std::size_t>(name_end - data));

  const u8* matrix_slots = data + out->matrix_slots_offset;
  const auto* matrix_end =
      static_cast<const u8*>(std::memchr(matrix_slots, 0xff, kMatrixSlotBytes));
  const std::size_t matrix_count =
      matrix_end ? static_cast<std::size_t>(matrix_end - matrix_slots) : kMatrixSlotBytes;
  std::array<bool, 128> seen_slots = {};
  for (std::size_t i = 0; i < matrix_count; i++) {
    const u8 slot = matrix_slots[i];
    if (slot >= seen_slots.size() || seen_slots[slot]) {
      return fail(error, "unique matrix slots below 128");
    }
    seen_slots[slot] = true;
  }
  out->matrix_count = static_cast<u16>(matrix_count);
  out->flags_offset = out->matrix_pointers_offset + matrix_count * 16;
  if (!span_is_bounded(size, out->flags_offset, kFlagsBytes)) {
    return fail(error, "all matrix pointers and the Merc flags block");
  }

  for (std::size_t i = 0; i < matrix_count; i++) {
    u32 address = 0;
    std::memcpy(&address, data + out->matrix_pointers_offset + i * 16, sizeof(address));
    if (!address || address > ee_memory_size ||
        kMercMatrixBytes > ee_memory_size - address) {
      return fail(error, "matrix pointers bounded by EE main memory");
    }
  }

  std::memcpy(&out->enable_mask, data + out->flags_offset, sizeof(out->enable_mask));
  std::memcpy(&out->ignore_alpha_mask, data + out->flags_offset + 8,
              sizeof(out->ignore_alpha_mask));
  out->effect_count = data[out->flags_offset + 16];
  out->bit_flags = data[out->flags_offset + 17];
  if (out->effect_count >= kMaxEffectCount) {
    return fail(error, "an effect count below 64");
  }
  if (out->bit_flags & ~kKnownFlagMask) {
    return fail(error, "only known Jak 2 Merc flag bits");
  }
  if ((out->bit_flags & 0x5) == 0x5) {
    return fail(error, "mutually exclusive update-verts and pc-blerc flags");
  }

  const u64 valid_effect_mask =
      out->effect_count == 0 ? 0 : ((1ull << out->effect_count) - 1);
  if ((out->enable_mask | out->ignore_alpha_mask) & ~valid_effect_mask) {
    return fail(error, "effect masks bounded by the effect count");
  }

  out->blerc_offset = out->flags_offset + kFlagsBytes;
  out->fades_offset = out->blerc_offset + ((out->bit_flags & 4) ? kBlercBytes : 0);
  const std::size_t effect_quadwords = (out->effect_count + 3) / 4;
  out->effect_pointers_offset = out->fades_offset + effect_quadwords * 16;
  const std::size_t expected_size = out->effect_pointers_offset + effect_quadwords * 16;
  if (size != expected_size) {
    return fail(error, "the exact source-computed PC_PORT qwc");
  }

  if (out->bit_flags & 1) {
    for (std::size_t i = 0; i < out->effect_count; i++) {
      u32 address = 0;
      std::memcpy(&address, data + out->effect_pointers_offset + i * 4, sizeof(address));
      if (!address || address > ee_memory_size ||
          kMercEffectMinimumBytes > ee_memory_size - address) {
        return fail(error, "modified-effect pointers bounded by EE main memory");
      }
    }
  }

  return true;
}

inline bool validate_setup_packet(const u8* data,
                                  std::size_t size,
                                  u32 transferred_vif0,
                                  u32 transferred_vif1,
                                  std::string* error) {
  if (!data || size != 10 * 16) {
    return fail(error, "a 160-byte Merc VU setup packet");
  }
  if (transferred_vif0 != vif(VifCode::Kind::STCYCL, 0x404) ||
      transferred_vif1 != vif(VifCode::Kind::STMOD)) {
    return fail(error, "the Merc STCYCL/STMOD setup VIF pair");
  }

  std::array<u32, 4> first_vifs = {};
  std::array<u32, 4> last_vifs = {};
  std::memcpy(first_vifs.data(), data, sizeof(first_vifs));
  std::memcpy(last_vifs.data(), data + 9 * 16, sizeof(last_vifs));
  if (first_vifs[0] != vif(VifCode::Kind::BASE, 442) ||
      first_vifs[1] != vif(VifCode::Kind::OFFSET, static_cast<u16>(-442)) ||
      first_vifs[2] != 0 || first_vifs[3] != vif(VifCode::Kind::UNPACK_V4_32, 0, 8)) {
    return fail(error, "the Merc low-memory VIF setup");
  }
  if (last_vifs[0] != vif(VifCode::Kind::FLUSHE) || last_vifs[1] != 0 ||
      last_vifs[2] != 0 || last_vifs[3] != vif(VifCode::Kind::MSCAL)) {
    return fail(error, "the Merc FLUSHE/MSCAL setup tail");
  }
  return true;
}

inline bool validate_bucket(const u8* copy_base,
                            std::size_t copy_size,
                            u32 start_offset,
                            u32 next_bucket,
                            std::size_t ee_memory_size,
                            Bucket* out,
                            std::string* error,
                            PreflightRejectReason* rejection_reason = nullptr) {
  const auto set_reason = [rejection_reason](PreflightRejectReason reason) {
    if (rejection_reason) {
      *rejection_reason = reason;
    }
  };
  set_reason(PreflightRejectReason::CompactedCopyBounds);
  if (!out) {
    return fail(error, "a bucket output");
  }
  *out = {};
  if (!is_qword_aligned(next_bucket)) {
    return fail(error, "a 16-byte-aligned next bucket boundary");
  }
  if (!copy_base || !span_is_bounded(copy_size, next_bucket, 16)) {
    return fail(error, "the next bucket header inside the compacted copy");
  }

  set_reason(PreflightRejectReason::OpeningTag);
  TagView opening;
  if (!read_tag(copy_base, copy_size, start_offset, &opening, error)) {
    return false;
  }
  const bool empty = opening.kind == DmaTag::Kind::CNT && opening.qwc == 0 &&
                     opening.address == 0 && opening.vif0 == 0 && opening.vif1 == 0;
  if (empty) {
    if (opening.inline_end != next_bucket) {
      return fail(error, "an empty CNT to land exactly at the bucket boundary");
    }
    out->empty = true;
    set_reason(PreflightRejectReason::None);
    return true;
  }

  if (opening.kind != DmaTag::Kind::NEXT || opening.qwc != 0 ||
      !is_opening_vif0(opening.vif0) || !is_opening_vif1(opening.vif1)) {
    return fail(error, "a source-shaped populated NEXT bucket opening");
  }
  if (opening.address == next_bucket) {
    return fail(error, "Merc setup data after the populated opening");
  }

  set_reason(PreflightRejectReason::VuSetupTag);
  TagView setup;
  if (!read_tag(copy_base, copy_size, opening.address, &setup, error)) {
    return false;
  }
  if (setup.kind != DmaTag::Kind::CNT || setup.qwc != 10 || setup.address != 0) {
    return fail(error, "an exact CNT qwc10 Merc VU setup tag");
  }
  set_reason(PreflightRejectReason::VuSetupPayload);
  if (!validate_setup_packet(copy_base + setup.payload_offset, setup.payload_size, setup.vif0,
                             setup.vif1, error)) {
    return false;
  }

  set_reason(PreflightRejectReason::GsSetupTag);
  TagView gs;
  if (!read_tag(copy_base, copy_size, setup.inline_end, &gs, error)) {
    return false;
  }
  if (gs.kind != DmaTag::Kind::CNT || gs.qwc != 3 || gs.address != 0 || gs.vif0 != 0 ||
      gs.vif1 != kDirect3Vif) {
    return fail(error, "an exact 48-byte NOP/DIRECT-3 GS test/zbuf packet");
  }

  set_reason(PreflightRejectReason::SetupPatch);
  TagView setup_patch;
  if (!read_tag(copy_base, copy_size, gs.inline_end, &setup_patch, error)) {
    return false;
  }
  if (!is_zero_next(setup_patch)) {
    return fail(error, "one exact zero-qwc zero-VIF NEXT after Merc setup");
  }
  if (setup_patch.address == next_bucket) {
    return fail(error, "at least one model after a populated Merc setup");
  }

  u32 current = setup_patch.address;
  // Every model/terminal target is aligned and has a complete header in this
  // copy. There can therefore be no more distinct targets than qword slots.
  std::vector<bool> visited_offsets(copy_size / 16, false);
  for (std::size_t link_count = 0; link_count < visited_offsets.size(); link_count++) {
    set_reason(PreflightRejectReason::ModelChain);
    const std::size_t current_slot = current / 16;
    if (visited_offsets[current_slot]) {
      return fail(error, "an acyclic Merc model chain");
    }
    visited_offsets[current_slot] = true;

    TagView model;
    if (!read_tag(copy_base, copy_size, current, &model, error)) {
      return false;
    }
    if (is_zero_next(model)) {
      if (model.address == next_bucket) {
        if (out->model_count == 0) {
          return fail(error, "at least one model in a populated Merc bucket");
        }
        set_reason(PreflightRejectReason::None);
        return true;
      }
      current = model.address;
      continue;
    }

    const bool default_end = out->model_count > 0 && model.kind == DmaTag::Kind::CNT &&
                             model.qwc == 10 && model.address == 0 &&
                             model.vif0 == vif(VifCode::Kind::FLUSHA) &&
                             model.vif1 == vif(VifCode::Kind::DIRECT, 10);
    if (default_end) {
      TagView tail_next;
      if (!read_tag(copy_base, copy_size, model.inline_end, &tail_next, error)) {
        return false;
      }
      if (!is_zero_next(tail_next) || tail_next.address != next_bucket) {
        return fail(error, "an exact NEXT to the bucket boundary after the default-end tail");
      }
      set_reason(PreflightRejectReason::None);
      return true;
    }

    if (model.kind != DmaTag::Kind::CNT || model.address != 0 || model.vif0 != 0 ||
        model.vif1 != kPcPortVif) {
      return fail(error, "an exact CNT zero/PC_PORT model tag");
    }
    ModelPacket packet;
    set_reason(PreflightRejectReason::ModelPacket);
    if (!parse_model_packet(copy_base + model.payload_offset, model.payload_size, model.vif0,
                            model.vif1, ee_memory_size, &packet, error)) {
      return false;
    }
    out->model_count++;
    out->models.push_back(std::move(packet));

    set_reason(PreflightRejectReason::ModelPatch);
    TagView model_patch;
    if (!read_tag(copy_base, copy_size, model.inline_end, &model_patch, error)) {
      return false;
    }
    if (!is_zero_next(model_patch)) {
      return fail(error, "one exact zero-qwc zero-VIF NEXT patch per model");
    }
    if (model_patch.address == next_bucket) {
      return fail(error, "a terminal NEXT after the final model patch");
    }
    current = model_patch.address;
  }
  return fail(error, "model links bounded by qword slots in the compacted copy");
}

enum class PreflightAction { Render, SkipEmpty, SkipMalformed };

struct PreflightOutcome {
  PreflightAction action = PreflightAction::SkipMalformed;
  PreflightRejectReason rejection_reason = PreflightRejectReason::None;
  Bucket packet;
  std::string error;
  bool recovered = false;

  bool should_render() const { return action == PreflightAction::Render; }
};

template <typename ModelValidator>
PreflightOutcome preflight_bucket(DmaFollower* dma,
                                  const u8* copy_base,
                                  std::size_t copy_size,
                                  u32 start_offset,
                                  u32 next_bucket,
                                  std::size_t ee_memory_size,
                                  int* malformed_count,
                                  ModelValidator&& validate_model) {
  PreflightOutcome result;
  bool valid = validate_bucket(copy_base, copy_size, start_offset, next_bucket, ee_memory_size,
                               &result.packet, &result.error, &result.rejection_reason);
  if (valid && !result.packet.empty) {
    for (const auto& model : result.packet.models) {
      if (!validate_model(model, &result.error)) {
        result.rejection_reason = PreflightRejectReason::LoadedModelMismatch;
        valid = false;
        break;
      }
    }
  }

  if (valid && !result.packet.empty) {
    result.action = PreflightAction::Render;
    return result;
  }

  result.recovered = recover_to_boundary(dma, copy_base, copy_size, next_bucket);
  if (valid) {
    result.action = PreflightAction::SkipEmpty;
    return result;
  }

  result.packet = {};
  if (malformed_count) {
    (*malformed_count)++;
  }
  return result;
}

}  // namespace metal_jak2_merc_dma
