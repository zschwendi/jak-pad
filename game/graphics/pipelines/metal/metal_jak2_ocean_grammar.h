#pragma once

#include "common/dma/dma_chain_read.h"

namespace metal_renderer::jak2_ocean_grammar {

inline bool direct(const DmaTransfer& transfer, u32 size_bytes, u16 qwords) {
  const auto v0 = transfer.vifcode0();
  const auto v1 = transfer.vifcode1();
  return transfer.size_bytes == size_bytes && v0.kind == VifCode::Kind::NOP &&
         !v0.interrupt && v0.num == 0 && v0.immediate == 0 &&
         v1.kind == VifCode::Kind::DIRECT && !v1.interrupt && v1.num == 0 &&
         v1.immediate == qwords;
}

inline bool base_offset(const DmaTransfer& transfer, u16 base, u16 offset) {
  const auto v0 = transfer.vifcode0();
  const auto v1 = transfer.vifcode1();
  return transfer.size_bytes == 0 && v0.kind == VifCode::Kind::BASE &&
         !v0.interrupt && v0.num == 0 && v0.immediate == base &&
         v1.kind == VifCode::Kind::OFFSET && !v1.interrupt && v1.num == 0 &&
         v1.immediate == offset;
}

inline bool unpack_v4_32(const DmaTransfer& transfer,
                         u32 size_bytes,
                         u16 stcycl,
                         u16 address,
                         bool use_tops) {
  if (size_bytes % 16 != 0 || size_bytes / 16 > 0xff) {
    return false;
  }
  const auto v0 = transfer.vifcode0();
  const auto v1 = transfer.vifcode1();
  const VifCodeUnpack unpack(v1);
  return transfer.size_bytes == size_bytes && v0.kind == VifCode::Kind::STCYCL &&
         !v0.interrupt && v0.num == 0 && v0.immediate == stcycl &&
         v1.kind == VifCode::Kind::UNPACK_V4_32 && !v1.interrupt &&
         v1.num == size_bytes / 16 && unpack.addr_qw == address &&
         unpack.use_tops_flag == use_tops;
}

inline bool mscalf_stmod(const DmaTransfer& transfer, u16 call) {
  const auto v0 = transfer.vifcode0();
  const auto v1 = transfer.vifcode1();
  return transfer.size_bytes == 0 && v0.kind == VifCode::Kind::MSCALF &&
         !v0.interrupt && v0.num == 0 && v0.immediate == call &&
         v1.kind == VifCode::Kind::STMOD && !v1.interrupt && v1.num == 0 &&
         v1.immediate == 0;
}

}  // namespace metal_renderer::jak2_ocean_grammar
