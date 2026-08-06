#pragma once

/*!
 * @file metal_jak2_merc_dma.h
 * Fail-closed validation for Jak 2's normal opaque Merc PC_PORT chain.
 *
 * This is header-only so the parser can be used by focused, asset-free tests
 * without adding a second product source until the central Metal target wiring
 * is updated. The accepted layout mirrors foreground.gc's pc-merc-draw-request
 * and merc.gc's merc-vu1-init-buffer exactly.
 */

#include <array>
#include <cstring>
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
  u8 matrix_count = 0;
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

inline bool fail(std::string* error, const char* message) {
  if (error) {
    *error = message;
  }
  return false;
}

inline bool is_zero_next(const DmaFollower& dma) {
  const auto tag = dma.current_tag();
  return tag.kind == DmaTag::Kind::NEXT && tag.qwc == 0 && !tag.spr &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}

inline bool is_opening_vif0(u32 vif) {
  return vif == 0 || VifCode(vif).kind == VifCode::Kind::NOP ||
         VifCode(vif).kind == VifCode::Kind::MARK;
}

inline bool is_opening_vif1(u32 vif) {
  return vif == 0 || VifCode(vif).kind == VifCode::Kind::NOP;
}

// A rejected model qwc cannot be drained with DmaFollower: the corrupt qwc
// would move the follower to a made-up inline tag. Recover the chain base from
// the still-unconsumed source-shaped bucket opening and place the caller at the
// already-known next bucket instead.
inline bool recover_to_boundary(DmaFollower* dma, u32 next_bucket) {
  if (!dma) {
    return false;
  }
  const auto opening = dma->current_tag();
  if (opening.spr ||
      (opening.kind != DmaTag::Kind::CNT && opening.kind != DmaTag::Kind::NEXT) ||
      (opening.kind == DmaTag::Kind::CNT && opening.addr != 0)) {
    return false;
  }
  DmaFollower origin = *dma;
  const auto first = origin.read_and_advance();
  const u8* chain_base = first.data - first.data_offset;
  *dma = DmaFollower(chain_base, next_bucket);
  return true;
}

inline bool parse_model_packet(const DmaTransfer& transfer,
                               std::size_t ee_memory_size,
                               ModelPacket* out,
                               std::string* error) {
  if (!out) {
    return fail(error, "a model packet output");
  }
  *out = {};
  if (!transfer.data || transfer.vif0() != 0 || transfer.vif1() != kPcPortVif) {
    return fail(error, "an exact zero/PC_PORT model transfer");
  }

  constexpr std::size_t kFixedBytes =
      kNameBytes + kLightBytes + kMatrixSlotBytes + kFlagsBytes;
  if (transfer.size_bytes < kFixedBytes) {
    return fail(error, "a complete Jak 2 Merc model packet");
  }

  const auto* name_end = static_cast<const u8*>(
      std::memchr(transfer.data, 0, kNameBytes));
  if (!name_end) {
    return fail(error, "a NUL-terminated model name within 128 bytes");
  }
  out->name.assign(reinterpret_cast<const char*>(transfer.data),
                   static_cast<std::size_t>(name_end - transfer.data));

  const u8* matrix_slots = transfer.data + out->matrix_slots_offset;
  const auto* matrix_end =
      static_cast<const u8*>(std::memchr(matrix_slots, 0xff, kMatrixSlotBytes));
  if (!matrix_end) {
    return fail(error, "a 0xff-terminated matrix-slot string within 128 bytes");
  }
  const std::size_t matrix_count = static_cast<std::size_t>(matrix_end - matrix_slots);
  std::array<bool, 128> seen_slots = {};
  for (std::size_t i = 0; i < matrix_count; i++) {
    const u8 slot = matrix_slots[i];
    if (slot >= seen_slots.size() || seen_slots[slot]) {
      return fail(error, "unique matrix slots below 128");
    }
    seen_slots[slot] = true;
  }
  out->matrix_count = static_cast<u8>(matrix_count);
  out->flags_offset = out->matrix_pointers_offset + matrix_count * 16;
  if (out->flags_offset + kFlagsBytes > transfer.size_bytes) {
    return fail(error, "all matrix pointers and the Merc flags block");
  }

  for (std::size_t i = 0; i < matrix_count; i++) {
    u32 address = 0;
    std::memcpy(&address, transfer.data + out->matrix_pointers_offset + i * 16,
                sizeof(address));
    if (!address || address > ee_memory_size ||
        kMercMatrixBytes > ee_memory_size - address) {
      return fail(error, "matrix pointers bounded by EE main memory");
    }
  }

  std::memcpy(&out->enable_mask, transfer.data + out->flags_offset, sizeof(out->enable_mask));
  std::memcpy(&out->ignore_alpha_mask, transfer.data + out->flags_offset + 8,
              sizeof(out->ignore_alpha_mask));
  out->effect_count = transfer.data[out->flags_offset + 16];
  out->bit_flags = transfer.data[out->flags_offset + 17];
  if (out->effect_count >= kMaxEffectCount) {
    return fail(error, "an effect count below 64");
  }
  if (out->bit_flags & ~kKnownFlagMask) {
    return fail(error, "only known Jak 2 Merc flag bits");
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
  const std::size_t expected_bytes = out->effect_pointers_offset + effect_quadwords * 16;
  if (transfer.size_bytes != expected_bytes) {
    return fail(error, "the exact source-computed PC_PORT qwc");
  }

  if (out->bit_flags & 1) {
    for (std::size_t i = 0; i < out->effect_count; i++) {
      u32 address = 0;
      std::memcpy(&address, transfer.data + out->effect_pointers_offset + i * 4,
                  sizeof(address));
      if (!address || address > ee_memory_size ||
          kMercEffectMinimumBytes > ee_memory_size - address) {
        return fail(error, "modified-effect pointers bounded by EE main memory");
      }
    }
  }

  return true;
}

inline bool validate_setup_packet(const DmaTransfer& transfer, std::string* error) {
  if (transfer.size_bytes != 10 * 16) {
    return fail(error, "a 160-byte Merc VU setup packet");
  }
  if (transfer.vif0() != vif(VifCode::Kind::STCYCL, 0x404) ||
      transfer.vif1() != vif(VifCode::Kind::STMOD)) {
    return fail(error, "the Merc STCYCL/STMOD setup VIF pair");
  }

  std::array<u32, 4> first_vifs = {};
  std::array<u32, 4> last_vifs = {};
  std::memcpy(first_vifs.data(), transfer.data, sizeof(first_vifs));
  std::memcpy(last_vifs.data(), transfer.data + 9 * 16, sizeof(last_vifs));
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

inline bool validate_bucket(DmaFollower dma,
                            u32 next_bucket,
                            std::size_t ee_memory_size,
                            Bucket* out,
                            std::string* error) {
  if (!out) {
    return fail(error, "a bucket output");
  }
  *out = {};
  const auto opening = dma.current_tag();
  const bool empty = opening.kind == DmaTag::Kind::CNT && opening.qwc == 0 &&
                     opening.addr == 0 && !opening.spr && dma.current_tag_vif0() == 0 &&
                     dma.current_tag_vif1() == 0;
  if (empty) {
    dma.read_and_advance();
    if (dma.current_tag_offset() != next_bucket) {
      return fail(error, "an empty CNT to land exactly at the bucket boundary");
    }
    out->empty = true;
    return true;
  }

  if (opening.kind != DmaTag::Kind::NEXT || opening.qwc != 0 || opening.spr ||
      !is_opening_vif0(dma.current_tag_vif0()) ||
      !is_opening_vif1(dma.current_tag_vif1())) {
    return fail(error, "a source-shaped populated NEXT bucket opening");
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() == next_bucket) {
    return fail(error, "Merc setup data after the populated opening");
  }

  const auto setup_tag = dma.current_tag();
  if (setup_tag.kind != DmaTag::Kind::CNT || setup_tag.qwc != 10 || setup_tag.addr != 0 ||
      setup_tag.spr) {
    return fail(error, "an exact CNT qwc10 Merc VU setup tag");
  }
  if (!validate_setup_packet(dma.read_and_advance(), error)) {
    return false;
  }

  const auto gs_tag = dma.current_tag();
  if (gs_tag.kind != DmaTag::Kind::CNT || gs_tag.qwc != 3 || gs_tag.addr != 0 || gs_tag.spr ||
      dma.current_tag_vif0() != 0 || dma.current_tag_vif1() != kDirect3Vif) {
    return fail(error, "an exact 48-byte NOP/DIRECT-3 GS test/zbuf packet");
  }
  dma.read_and_advance();

  if (!is_zero_next(dma)) {
    return fail(error, "one exact zero-qwc zero-VIF NEXT after Merc setup");
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() == next_bucket) {
    return fail(error, "at least one model after a populated Merc setup");
  }

  std::vector<u32> visited_offsets;
  for (u32 guard = 0; guard < 1024; guard++) {
    const u32 offset = dma.current_tag_offset();
    for (u32 visited : visited_offsets) {
      if (visited == offset) {
        return fail(error, "an acyclic Merc model chain");
      }
    }
    visited_offsets.push_back(offset);

    const auto model_tag = dma.current_tag();
    if (model_tag.kind == DmaTag::Kind::NEXT && model_tag.qwc == 0 && !model_tag.spr &&
        dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0) {
      dma.read_and_advance();
      if (dma.current_tag_offset() != next_bucket) {
        return fail(error, "the terminal NEXT to land exactly at the bucket boundary");
      }
      if (out->model_count == 0) {
        return fail(error, "at least one model in a populated Merc bucket");
      }
      return true;
    }

    if (model_tag.kind != DmaTag::Kind::CNT || model_tag.addr != 0 || model_tag.spr ||
        dma.current_tag_vif0() != 0 || dma.current_tag_vif1() != kPcPortVif) {
      return fail(error, "an exact CNT zero/PC_PORT model tag");
    }
    ModelPacket packet;
    if (!parse_model_packet(dma.read_and_advance(), ee_memory_size, &packet, error)) {
      return false;
    }
    out->model_count++;
    out->models.push_back(std::move(packet));

    if (dma.current_tag_offset() == next_bucket || !is_zero_next(dma)) {
      return fail(error, "one exact zero-qwc zero-VIF NEXT patch per model");
    }
    dma.read_and_advance();
    if (dma.current_tag_offset() == next_bucket) {
      return fail(error, "a terminal NEXT after the final model patch");
    }
  }
  return fail(error, "fewer than 1024 Merc model links");
}

}  // namespace metal_jak2_merc_dma
