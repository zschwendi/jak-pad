#include "game/graphics/pipelines/metal/metal_jak2_ocean_grammar.h"

#include <cstdio>

namespace {

int failures = 0;

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

DmaTransfer transfer(u32 size_bytes, u32 vif0, u32 vif1) {
  DmaTransfer result;
  result.size_bytes = size_bytes;
  result.transferred_tag = static_cast<u64>(vif0) | (static_cast<u64>(vif1) << 32);
  return result;
}

void check(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    failures++;
  }
}

}  // namespace

int main() {
  using namespace metal_renderer::jak2_ocean_grammar;

  const auto valid_upload =
      transfer(192 * 16, vif(VifCode::Kind::STCYCL, 0x404),
               vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 192));
  check(unpack_v4_32(valid_upload, 192 * 16, 0x404, 0, true),
        "the exact Jak II 192-qword TOPS upload is accepted with assertions disabled");

  const auto short_upload =
      transfer(16, vif(VifCode::Kind::STCYCL, 0x404),
               vif(VifCode::Kind::UNPACK_V4_32, 0x8000, 192));
  check(!unpack_v4_32(short_upload, 192 * 16, 0x404, 0, true),
        "a short transfer cannot authorize the fixed 3072-byte copy");

  const auto wrong_address =
      transfer(192 * 16, vif(VifCode::Kind::STCYCL, 0x404),
               vif(VifCode::Kind::UNPACK_V4_32, 0x8001, 192));
  check(!unpack_v4_32(wrong_address, 192 * 16, 0x404, 0, true),
        "an unexpected VU address fails the exact upload grammar");

  check(direct(transfer(64, vif(VifCode::Kind::NOP), vif(VifCode::Kind::DIRECT, 4)), 64, 4) &&
            !direct(transfer(48, vif(VifCode::Kind::NOP), vif(VifCode::Kind::DIRECT, 4)), 64, 4),
        "Direct setup validation binds opcode, qword count, and payload size");
  check(base_offset(transfer(0, vif(VifCode::Kind::BASE),
                             vif(VifCode::Kind::OFFSET, 0xc0)),
                    0, 0xc0) &&
            !base_offset(transfer(0, vif(VifCode::Kind::BASE),
                                  vif(VifCode::Kind::OFFSET, 0xbf)),
                         0, 0xc0),
        "the Jak II ocean VU buffer setup rejects the wrong offset");
  check(mscalf_stmod(transfer(0, vif(VifCode::Kind::MSCALF, 2),
                              vif(VifCode::Kind::STMOD)),
                     2) &&
            !mscalf_stmod(transfer(0, vif(VifCode::Kind::MSCALF, 3),
                                   vif(VifCode::Kind::STMOD)),
                          2),
        "the exact ocean VU call selector rejects an unexpected program entry");

  if (failures) {
    std::printf("FAIL: %d Jak II ocean grammar check(s) failed\n", failures);
    return 1;
  }
  std::puts("PASS: NO_ASSERT Jak II ocean grammar rejects malformed fixed-copy transfers");
  return 0;
}
