#include "game/graphics/pipelines/metal/metal_jak2_subtitle_bucket322_plan.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/goal_constants.h"

#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"

namespace metal_renderer {
namespace {

constexpr u16 kStartAnimatorArray = 12;
constexpr u16 kFinishAnimatorArray = 13;
constexpr u16 kUploadGenericVram = 16;
constexpr u8 kPsmct32 = static_cast<u8>(GsTex0::PSM::PSMCT32);
constexpr u8 kPsmt4 = static_cast<u8>(GsTex0::PSM::PSMT4);
constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;

struct CheckedTransfer {
  DmaTag tag{0};
  DmaTransfer data;
  u32 tag_offset = 0;
};

struct UploadRecord {
  u32 source_offset = 0;
  u16 width = 0;
  u16 height = 0;
  u32 destination = 0;
  u8 format = 0;
  u8 force_to_gpu = 0;
};

template <typename T>
T read_unaligned(const u8* data) {
  T result;
  std::memcpy(&result, data, sizeof(result));
  return result;
}

void set_rejection(Jak2SubtitleBucket322RejectReason* out,
                   u32* out_transfer_index,
                   Jak2SubtitleBucket322RejectReason reason,
                   u32 transfer_index = kJak2SubtitleNoTransferIndex) {
  if (out) {
    *out = reason;
  }
  if (out_transfer_index) {
    *out_transfer_index = transfer_index;
  }
}

bool is_inert(const CheckedTransfer& transfer) {
  return (transfer.tag.kind == DmaTag::Kind::CNT || transfer.tag.kind == DmaTag::Kind::NEXT) &&
         transfer.tag.qwc == 0 && transfer.data.size_bytes == 0 && transfer.data.vif0() == 0 &&
         transfer.data.vif1() == 0;
}

bool is_cnt(const CheckedTransfer& transfer,
            u16 qwc,
            VifCode::Kind vif0_kind,
            u16 vif0_immediate,
            VifCode::Kind vif1_kind,
            u16 vif1_immediate) {
  const auto vif0 = transfer.data.vifcode0();
  const auto vif1 = transfer.data.vifcode1();
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.addr == 0 &&
         transfer.tag.qwc == qwc && transfer.data.size_bytes == static_cast<u32>(qwc) * 16 &&
         vif0.kind == vif0_kind && vif0.immediate == vif0_immediate && vif0.num == 0 &&
         !vif0.interrupt && vif1.kind == vif1_kind && vif1.immediate == vif1_immediate &&
         vif1.num == 0 && !vif1.interrupt;
}

bool is_direct(const CheckedTransfer& transfer, u16 qwc) {
  return qwc != 0 && is_cnt(transfer, qwc, VifCode::Kind::NOP, 0, VifCode::Kind::DIRECT, qwc);
}

bool gif_header_matches(const u8* payload,
                        u32 nloop,
                        bool pre,
                        GsPrim::Kind prim_kind,
                        bool textured,
                        bool alpha_blend,
                        bool fixed_uv,
                        u32 nreg,
                        const std::initializer_list<GifTag::RegisterDescriptor>& registers) {
  const GifTag gif(payload);
  const GsPrim prim(gif.prim());
  if (gif.nloop() != nloop || !gif.eop() || gif.pre() != pre ||
      gif.flg() != GifTag::Format::PACKED || gif.nreg() != nreg || prim.kind() != prim_kind ||
      prim.tme() != textured || prim.abe() != alpha_blend || prim.fst() != fixed_uv ||
      registers.size() != nreg) {
    return false;
  }
  u32 index = 0;
  for (const auto expected : registers) {
    if (gif.reg(index++) != expected) {
      return false;
    }
  }
  return true;
}

bool packed_rgba_matches(const u8* payload, u8 red, u8 green, u8 blue, u8 alpha) {
  return read_unaligned<u32>(payload) == red && read_unaligned<u32>(payload + 4) == green &&
         read_unaligned<u32>(payload + 8) == blue && read_unaligned<u32>(payload + 12) == alpha;
}

u32 ceil_log2(u16 value) {
  if (value <= 1) {
    return 0;
  }
  u32 result = 0;
  u32 remaining = static_cast<u32>(value) - 1;
  while (remaining != 0) {
    result++;
    remaining >>= 1;
  }
  return result;
}

bool image_setup_matches(const CheckedTransfer& transfer, u16 width, u16 height) {
  if (!is_direct(transfer, 7) || !transfer.data.data || height == 0) {
    return false;
  }
  const u8* payload = transfer.data.data;
  const GifTag gif(payload);
  if (gif.nloop() != 1 || !gif.eop() || gif.pre() || gif.flg() != GifTag::Format::PACKED ||
      gif.nreg() != 6) {
    return false;
  }
  for (u32 i = 0; i < 6; ++i) {
    if (gif.reg(i) != GifTag::RegisterDescriptor::AD) {
      return false;
    }
  }
  constexpr std::array<GsRegisterAddress, 6> kAddresses = {
      GsRegisterAddress::TEST_1, GsRegisterAddress::ALPHA_1, GsRegisterAddress::TEX0_1,
      GsRegisterAddress::TEX1_1, GsRegisterAddress::CLAMP_1, GsRegisterAddress::TEXFLUSH};
  std::array<u64, 6> values = {};
  for (u32 i = 0; i < kAddresses.size(); ++i) {
    values[i] = read_unaligned<u64>(payload + 16 + i * 16);
    if (read_unaligned<u64>(payload + 24 + i * 16) != static_cast<u64>(kAddresses[i])) {
      return false;
    }
  }
  constexpr u64 kTest = 1ull | (1ull << 1) | (3ull << 12) | (1ull << 16) | (1ull << 17);
  const u32 texture_log2 = ceil_log2(height) & 0xf;
  const u64 expected_tex0 = 1ull | ((static_cast<u64>(width) >> 6) & 0x3f) << 14 |
                            static_cast<u64>(kPsmt4) << 20 | static_cast<u64>(texture_log2) << 26 |
                            static_cast<u64>(texture_log2) << 30 | (1ull << 34) | (1ull << 61);
  return values[0] == kTest && values[1] == 0x44 && values[2] == expected_tex0 &&
         values[3] == 0x60 && values[4] == 5 && values[5] == 0;
}

bool image_sprite_matches(const CheckedTransfer& transfer, u16 width, u16 height, bool foreground) {
  if (!is_direct(transfer, 6) || !transfer.data.data) {
    return false;
  }
  const u8* payload = transfer.data.data;
  if (!gif_header_matches(payload, 1, true, GsPrim::Kind::SPRITE, true, true, true, 5,
                          {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::UV,
                           GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::UV,
                           GifTag::RegisterDescriptor::XYZ2}) ||
      !packed_rgba_matches(payload + 16, foreground ? 128 : 0, foreground ? 128 : 0,
                           foreground ? 128 : 0, 128) ||
      read_unaligned<u64>(payload + 32) != 0 || read_unaligned<u64>(payload + 40) != 0 ||
      read_unaligned<u32>(payload + 56) != 0 || read_unaligned<u32>(payload + 60) != 0 ||
      read_unaligned<u32>(payload + 64) != static_cast<u32>(width) * 16 ||
      read_unaligned<u32>(payload + 68) != static_cast<u32>(height) * 16 ||
      read_unaligned<u32>(payload + 72) != 0 || read_unaligned<u32>(payload + 76) != 0 ||
      read_unaligned<u32>(payload + 88) != 0 || read_unaligned<u32>(payload + 92) != 0) {
    return false;
  }
  return true;
}

bool hud_sprite_pair_matches(const CheckedTransfer& adgif, const CheckedTransfer& draw) {
  if (!is_direct(adgif, 6) || !is_direct(draw, 13) || !adgif.data.data || !draw.data.data) {
    return false;
  }
  const GifTag shader(adgif.data.data);
  if (shader.nloop() != 5 || !shader.eop() || shader.pre() ||
      shader.flg() != GifTag::Format::PACKED || shader.nreg() != 1 ||
      shader.reg(0) != GifTag::RegisterDescriptor::AD) {
    return false;
  }
  constexpr std::array<GsRegisterAddress, 5> kAddresses = {
      GsRegisterAddress::TEX0_1, GsRegisterAddress::TEX1_1, GsRegisterAddress::MIPTBP1_1,
      GsRegisterAddress::CLAMP_1, GsRegisterAddress::ALPHA_1};
  for (u32 i = 0; i < kAddresses.size(); ++i) {
    if (read_unaligned<u64>(adgif.data.data + 24 + i * 16) != static_cast<u64>(kAddresses[i])) {
      return false;
    }
  }
  return gif_header_matches(draw.data.data, 1, true, GsPrim::Kind::TRI_STRIP, true, true, false, 12,
                            {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::ST,
                             GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::RGBAQ,
                             GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::XYZ2,
                             GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::ST,
                             GifTag::RegisterDescriptor::XYZ2, GifTag::RegisterDescriptor::RGBAQ,
                             GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::XYZ2});
}

UploadRecord read_upload_record(const CheckedTransfer& transfer) {
  UploadRecord result;
  result.source_offset = read_unaligned<u32>(transfer.data.data);
  result.width = read_unaligned<u16>(transfer.data.data + 4);
  result.height = read_unaligned<u16>(transfer.data.data + 6);
  result.destination = read_unaligned<u32>(transfer.data.data + 8);
  result.format = transfer.data.data[12];
  result.force_to_gpu = transfer.data.data[13];
  return result;
}

void hash_bytes(u64* hash, const void* bytes, std::size_t size) {
  const auto* input = static_cast<const u8*>(bytes);
  for (std::size_t i = 0; i < size; ++i) {
    *hash ^= input[i];
    *hash *= kFnvPrime;
  }
}

bool contains_pc_port(const CheckedTransfer& transfer) {
  return transfer.data.vifcode0().kind == VifCode::Kind::PC_PORT ||
         transfer.data.vifcode1().kind == VifCode::Kind::PC_PORT;
}

}  // namespace

std::optional<Jak2SubtitleBucket322Plan> plan_jak2_subtitle_bucket322(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    Jak2SubtitleBucket322RejectReason* out_rejection,
    u32* out_rejection_transfer_index) {
  set_rejection(out_rejection, out_rejection_transfer_index,
                Jak2SubtitleBucket322RejectReason::None);
  const std::size_t packet_size = std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  const u64 bucket_offset64 =
      static_cast<u64>(chain_offset) + static_cast<u64>(kJak2SubtitleBucket322) * 16;
  const u64 bucket_end64 = bucket_offset64 + 16;
  if (!dma_packet_snapshot || bucket_end64 > packet_size ||
      bucket_end64 > std::numeric_limits<u32>::max() ||
      !validate_jak2_metal_dma_chain(dma_packet_snapshot, packet_size, chain_offset)) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2SubtitleBucket322RejectReason::Chain);
    return std::nullopt;
  }

  try {
    const u32 bucket_offset = static_cast<u32>(bucket_offset64);
    const u32 bucket_end = static_cast<u32>(bucket_end64);
    DmaFollower dma(dma_packet_snapshot, bucket_offset, packet_size);
    std::array<CheckedTransfer, kJak2SubtitleMaximumTransfers> transfers = {};
    std::size_t transfer_count = 0;
    while (dma.current_tag_offset() != bucket_end) {
      if (dma.ended()) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2SubtitleBucket322RejectReason::Chain,
                      static_cast<u32>(transfer_count));
        return std::nullopt;
      }
      if (transfer_count == transfers.size()) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2SubtitleBucket322RejectReason::TransferLimit,
                      static_cast<u32>(transfer_count));
        return std::nullopt;
      }
      auto& transfer = transfers[transfer_count++];
      transfer.tag_offset = dma.current_tag_offset();
      transfer.tag = dma.current_tag();
      transfer.data = dma.read_and_advance();
    }

    Jak2SubtitleBucket322Plan plan;
    plan.transfer_count = static_cast<u32>(transfer_count);
    u64 fingerprint = kFnvOffsetBasis;
    for (std::size_t i = 0; i < transfer_count; ++i) {
      hash_bytes(&fingerprint, dma_packet_snapshot + transfers[i].tag_offset, 16);
      hash_bytes(&fingerprint, transfers[i].data.data, transfers[i].data.size_bytes);
    }

    std::size_t index = 0;
    while (index < transfer_count) {
      const auto& transfer = transfers[index];
      if (is_inert(transfer)) {
        plan.linker_transfers++;
        index++;
        continue;
      }

      const bool image_start =
          is_cnt(transfer, 0, VifCode::Kind::PC_PORT, kStartAnimatorArray, VifCode::Kind::NOP, 0);
      if (image_start) {
        if (plan.image_upload_count == plan.image_uploads.size()) {
          set_rejection(out_rejection, out_rejection_transfer_index,
                        Jak2SubtitleBucket322RejectReason::TransferLimit,
                        static_cast<u32>(index));
          return std::nullopt;
        }
        if (index + 7 > transfer_count ||
            !is_cnt(transfers[index + 1], 1, VifCode::Kind::PC_PORT, kUploadGenericVram,
                    VifCode::Kind::NOP, 0) ||
            !is_cnt(transfers[index + 2], 1, VifCode::Kind::PC_PORT, kUploadGenericVram,
                    VifCode::Kind::NOP, 0) ||
            !is_cnt(transfers[index + 3], 0, VifCode::Kind::PC_PORT, kFinishAnimatorArray,
                    VifCode::Kind::NOP, 0)) {
          set_rejection(out_rejection, out_rejection_transfer_index,
                        Jak2SubtitleBucket322RejectReason::UploadGrammar,
                        static_cast<u32>(index));
          return std::nullopt;
        }
        const auto clut = read_upload_record(transfers[index + 1]);
        const auto image = read_upload_record(transfers[index + 2]);
        if (clut.width != 2 || clut.height != 8 || clut.destination != 0 ||
            clut.format != kPsmct32 || clut.force_to_gpu != 0 || image.width == 0 ||
            image.height == 0 || image.destination != 1 || image.format != kPsmt4 ||
            image.force_to_gpu != 1) {
          set_rejection(out_rejection, out_rejection_transfer_index,
                        Jak2SubtitleBucket322RejectReason::UploadMetadata,
                        static_cast<u32>(index));
          return std::nullopt;
        }
        if (!image_setup_matches(transfers[index + 4], image.width, image.height) ||
            !image_sprite_matches(transfers[index + 5], image.width, image.height, false) ||
            !image_sprite_matches(transfers[index + 6], image.width, image.height, true)) {
          set_rejection(out_rejection, out_rejection_transfer_index,
                        Jak2SubtitleBucket322RejectReason::ImageDrawGrammar,
                        static_cast<u32>(index));
          return std::nullopt;
        }
        auto& image_plan = plan.image_uploads[plan.image_upload_count++];
        image_plan.clut_source_offset = clut.source_offset;
        image_plan.image_source_offset = image.source_offset;
        image_plan.width = image.width;
        image_plan.height = image.height;
        image_plan.start_transfer_index = static_cast<u32>(index);
        image_plan.start_relative_tag_offset = transfer.tag_offset - bucket_offset;
        plan.direct_transfers += 3;
        plan.direct_payload_bytes += transfers[index + 4].data.size_bytes +
                                     transfers[index + 5].data.size_bytes +
                                     transfers[index + 6].data.size_bytes;
        index += 7;
        continue;
      }

      if (contains_pc_port(transfer)) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2SubtitleBucket322RejectReason::UploadGrammar,
                      static_cast<u32>(index));
        return std::nullopt;
      }

      if (index + 2 <= transfer_count && hud_sprite_pair_matches(transfer, transfers[index + 1])) {
        plan.hud_sprite_pairs++;
        plan.direct_transfers += 2;
        plan.direct_payload_bytes +=
            transfer.data.size_bytes + transfers[index + 1].data.size_bytes;
        index += 2;
        continue;
      }

      if (!is_direct(transfer, transfer.tag.qwc)) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2SubtitleBucket322RejectReason::TransferEnvelope,
                      static_cast<u32>(index));
        return std::nullopt;
      }
      plan.direct_transfers++;
      plan.opaque_direct_transfers++;
      plan.direct_payload_bytes += transfer.data.size_bytes;
      index++;
    }

    const u32 producer_families = static_cast<u32>(plan.opaque_direct_transfers != 0) +
                                  static_cast<u32>(plan.hud_sprite_pairs != 0) +
                                  static_cast<u32>(plan.image_upload_count != 0);
    if (producer_families == 0) {
      plan.variant = Jak2SubtitleBucket322Variant::Absent;
    } else if (producer_families > 1) {
      plan.variant = Jak2SubtitleBucket322Variant::Mixed;
    } else if (plan.image_upload_count != 0) {
      plan.variant = Jak2SubtitleBucket322Variant::SubtitleImage;
    } else if (plan.hud_sprite_pairs != 0) {
      plan.variant = Jak2SubtitleBucket322Variant::HudSpriteDirect;
    } else {
      plan.variant = Jak2SubtitleBucket322Variant::OpaqueDirect;
    }
    plan.semantic_fingerprint = fingerprint;
    return plan;
  } catch (const std::exception&) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2SubtitleBucket322RejectReason::Chain);
  } catch (...) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2SubtitleBucket322RejectReason::Chain);
  }
  return std::nullopt;
}

bool jak2_subtitle_bucket322_plans_match(const Jak2SubtitleBucket322Plan& live,
                                          const Jak2SubtitleBucket322Plan& copied) {
  // FixedChunkDmaCopier rewrites DMA tag addresses while compacting chunks. The fingerprint keeps
  // the immutable live packet identity for telemetry, but cannot prove copied semantic equality.
  if (live.variant != copied.variant || live.image_upload_count != copied.image_upload_count ||
      live.transfer_count != copied.transfer_count ||
      live.linker_transfers != copied.linker_transfers ||
      live.direct_transfers != copied.direct_transfers ||
      live.opaque_direct_transfers != copied.opaque_direct_transfers ||
      live.hud_sprite_pairs != copied.hud_sprite_pairs ||
      live.direct_payload_bytes != copied.direct_payload_bytes) {
    return false;
  }
  for (std::size_t i = 0; i < live.image_upload_count; ++i) {
    const auto& a = live.image_uploads[i];
    const auto& b = copied.image_uploads[i];
    if (a.clut_source_offset != b.clut_source_offset || a.image_source_offset != b.image_source_offset ||
        a.width != b.width || a.height != b.height ||
        a.start_transfer_index != b.start_transfer_index ||
        a.start_relative_tag_offset != b.start_relative_tag_offset) {
      return false;
    }
  }
  return true;
}

}  // namespace metal_renderer
