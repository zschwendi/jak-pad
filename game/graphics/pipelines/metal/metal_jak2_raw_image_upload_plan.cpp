#include "game/graphics/pipelines/metal/metal_jak2_raw_image_upload_plan.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <limits>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/goal_constants.h"

#include "game/graphics/pipelines/metal/metal_jak2_chain_validation.h"

namespace metal_renderer {
namespace {

constexpr u32 vif(VifCode::Kind kind, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | immediate;
}

template <typename T>
T read_unaligned(const u8* data) {
  T result;
  std::memcpy(&result, data, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

struct CheckedTransfer {
  DmaTag tag{0};
  DmaTransfer data;
};

bool read_transfer(DmaFollower* dma, CheckedTransfer* out) {
  if (!dma || !out || dma->ended()) {
    return false;
  }
  out->tag = dma->current_tag();
  out->data = dma->read_and_advance();
  return !out->tag.spr;
}

bool is_strict_empty(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.addr == 0 &&
         transfer.data.size_bytes == 0 && transfer.data.vif0() == 0 &&
         transfer.data.vif1() == 0;
}

bool is_boundary(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::NEXT && transfer.data.size_bytes == 0 &&
         transfer.data.vif0() == 0 && transfer.data.vif1() == 0;
}

bool is_cnt(const CheckedTransfer& transfer, u16 qwc, u32 vif0, u32 vif1) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.addr == 0 &&
         transfer.tag.qwc == qwc && transfer.data.size_bytes == static_cast<u32>(qwc) * 16 &&
         transfer.data.vif0() == vif0 && transfer.data.vif1() == vif1;
}

bool has_registers(const GifTag& tag,
                   std::initializer_list<GifTag::RegisterDescriptor> registers) {
  if (tag.nreg() != registers.size()) {
    return false;
  }
  u32 index = 0;
  for (const auto reg : registers) {
    if (tag.reg(index++) != reg) {
      return false;
    }
  }
  return true;
}

bool is_black_sprite(const CheckedTransfer& transfer) {
  if (!is_cnt(transfer, 3, 0, vif(VifCode::Kind::DIRECT, 3))) {
    return false;
  }
  const GifTag tag(transfer.data.data);
  if (tag.nloop() != 1 || !tag.eop() || tag.pre() ||
      tag.flg() != GifTag::Format::REGLIST ||
      !has_registers(tag, {GifTag::RegisterDescriptor::PRIM,
                           GifTag::RegisterDescriptor::RGBAQ,
                           GifTag::RegisterDescriptor::XYZF2,
                           GifTag::RegisterDescriptor::XYZF2})) {
    return false;
  }
  const GsPrim prim(read_unaligned<u64>(transfer.data.data + 16));
  const u64 color = read_unaligned<u64>(transfer.data.data + 24);
  return prim.kind() == GsPrim::Kind::SPRITE && prim.abe() && !prim.tme() &&
         color == 0x80000000ull;
}

bool is_raw_image_state(const CheckedTransfer& transfer) {
  if (!is_cnt(transfer, 7, 0, vif(VifCode::Kind::DIRECT, 7))) {
    return false;
  }
  const GifTag tag(transfer.data.data);
  if (tag.nloop() != 1 || !tag.eop() || tag.pre() ||
      tag.flg() != GifTag::Format::PACKED ||
      !has_registers(tag, {GifTag::RegisterDescriptor::AD,
                           GifTag::RegisterDescriptor::AD,
                           GifTag::RegisterDescriptor::AD,
                           GifTag::RegisterDescriptor::AD,
                           GifTag::RegisterDescriptor::AD,
                           GifTag::RegisterDescriptor::AD})) {
    return false;
  }

  constexpr GsRegisterAddress kAddresses[] = {
      GsRegisterAddress::TEST_1, GsRegisterAddress::ALPHA_1,
      GsRegisterAddress::TEX0_1, GsRegisterAddress::TEX1_1,
      GsRegisterAddress::CLAMP_1, GsRegisterAddress::TEXFLUSH,
  };
  u64 values[6] = {};
  for (u32 i = 0; i < 6; ++i) {
    const u8* ad = transfer.data.data + 16 + i * 16;
    values[i] = read_unaligned<u64>(ad);
    if (read_unaligned<u64>(ad + 8) != static_cast<u64>(kAddresses[i])) {
      return false;
    }
  }

  const GsTest test(values[0]);
  const GsTex0 tex0(values[2]);
  const GsTex1 tex1(values[3]);
  return test.alpha_test_enable() && test.alpha_test() == GsTest::AlphaTest::ALWAYS &&
         test.afail() == GsTest::AlphaFail::RGB_ONLY && test.zte() &&
         test.ztest() == GsTest::ZTest::ALWAYS && values[1] == 0 &&
         tex0.tbp0() == kJak2RawImageDestination && tex0.tbw() == 8 &&
         tex0.psm() == GsTex0::PSM::PSMCT32 && tex0.tw() == 9 && tex0.th() == 9 &&
         tex0.tcc() == 1 && tex0.tfx() == GsTex0::TextureFunction::MODULATE &&
         tex1.mmag() && tex1.mmin() == 1 && values[4] == 0b101 && values[5] == 0;
}

bool is_raw_image_sprite(const CheckedTransfer& transfer) {
  if (!is_cnt(transfer, 6, 0, vif(VifCode::Kind::DIRECT, 6))) {
    return false;
  }
  const GifTag tag(transfer.data.data);
  const GsPrim prim(tag.prim());
  if (tag.nloop() != 1 || !tag.eop() || !tag.pre() ||
      tag.flg() != GifTag::Format::PACKED || prim.kind() != GsPrim::Kind::SPRITE ||
      !prim.tme() || !prim.fst() || prim.abe() ||
      !has_registers(tag, {GifTag::RegisterDescriptor::RGBAQ,
                           GifTag::RegisterDescriptor::UV,
                           GifTag::RegisterDescriptor::XYZ2,
                           GifTag::RegisterDescriptor::UV,
                           GifTag::RegisterDescriptor::XYZ2})) {
    return false;
  }

  const u8* rgba = transfer.data.data + 16;
  const u8* uv0 = transfer.data.data + 32;
  const u8* uv1 = transfer.data.data + 64;
  return rgba[0] == 0x80 && rgba[4] == 0x80 && rgba[8] == 0x80 && rgba[12] == 0x80 &&
         read_unaligned<u32>(uv0) == 0 && read_unaligned<u32>(uv0 + 4) == 0 &&
         read_unaligned<u32>(uv1) == kJak2RawImageWidth * 16 &&
         read_unaligned<u32>(uv1 + 4) == kJak2RawImageHeight * 16;
}

}  // namespace

std::optional<Jak2RawImageUploadPlan> plan_jak2_raw_image_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  const std::size_t packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  const std::size_t live_size = std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  const u64 bucket_offset64 =
      static_cast<u64>(chain_offset) + static_cast<u64>(kJak2RawImageUploadBucket) * 16;
  const u64 bucket_end64 = bucket_offset64 + 16;
  if (!dma_packet_snapshot || !live_ee_memory || bucket_end64 > packet_size ||
      bucket_end64 > std::numeric_limits<u32>::max() ||
      !validate_jak2_metal_dma_chain(dma_packet_snapshot, packet_size, chain_offset)) {
    return std::nullopt;
  }

  try {
    const u32 bucket_offset = static_cast<u32>(bucket_offset64);
    const u32 bucket_end = static_cast<u32>(bucket_end64);
    DmaFollower dma(dma_packet_snapshot, bucket_offset, packet_size);
    CheckedTransfer transfer;
    if (!read_transfer(&dma, &transfer)) {
      return std::nullopt;
    }
    if (is_strict_empty(transfer)) {
      return dma.current_tag_offset() == bucket_end ? std::optional(Jak2RawImageUploadPlan{})
                                                    : std::nullopt;
    }
    if (!is_boundary(transfer) || !read_transfer(&dma, &transfer) ||
        !is_black_sprite(transfer) || !read_transfer(&dma, &transfer) ||
        !is_cnt(transfer, 0, vif(VifCode::Kind::PC_PORT, 12), 0) ||
        !read_transfer(&dma, &transfer) ||
        !is_cnt(transfer, 1, vif(VifCode::Kind::PC_PORT, 16), 0)) {
      return std::nullopt;
    }

    Jak2RawImageUploadPlan plan;
    plan.source_offset = read_unaligned<u32>(transfer.data.data);
    plan.width = read_unaligned<u16>(transfer.data.data + 4);
    plan.height = read_unaligned<u16>(transfer.data.data + 6);
    plan.destination = read_unaligned<u32>(transfer.data.data + 8);
    plan.format = transfer.data.data[12];
    plan.force_to_gpu = transfer.data.data[13];
    const u64 pixel_count = static_cast<u64>(plan.width) * plan.height;
    const u64 pixel_bytes = pixel_count * sizeof(u32);
    if (plan.width != kJak2RawImageWidth || plan.height != kJak2RawImageHeight ||
        plan.destination != kJak2RawImageDestination || plan.format != kJak2RawImagePsmct32 ||
        plan.force_to_gpu != 1 || pixel_count > std::numeric_limits<std::size_t>::max() ||
        !range_is_valid(plan.source_offset, pixel_bytes, live_size)) {
      return std::nullopt;
    }

    if (!read_transfer(&dma, &transfer) ||
        !is_cnt(transfer, 0, vif(VifCode::Kind::PC_PORT, 13), 0) ||
        !read_transfer(&dma, &transfer) || !is_raw_image_state(transfer) ||
        !read_transfer(&dma, &transfer) || !is_raw_image_sprite(transfer) ||
        !read_transfer(&dma, &transfer) || !is_boundary(transfer) ||
        dma.current_tag_offset() != bucket_end) {
      return std::nullopt;
    }

    plan.rgba.resize(static_cast<std::size_t>(pixel_count));
    std::memcpy(plan.rgba.data(), live_ee_memory + plan.source_offset,
                static_cast<std::size_t>(pixel_bytes));
    plan.present = true;
    return plan;
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace metal_renderer
