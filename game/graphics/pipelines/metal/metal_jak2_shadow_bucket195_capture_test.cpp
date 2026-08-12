#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kMemorySize = 2 << 20;
constexpr u32 kDataOffset = 0x4000;
using Role = metal_renderer::Jak2ShadowBucket195TransferRole;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 vif(VifCode::Kind kind, u8 num = 0, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

void put_u32(std::vector<u8>* memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1,
             u8 fill = 0) {
  put_u64(memory, offset, static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                              (static_cast<u64>(address) << 32));
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
  std::memset(memory->data() + offset + 16, fill, static_cast<std::size_t>(qwc) * 16);
}

u32 bucket_offset(u32 chain_offset = kChainOffset) {
  return chain_offset + metal_renderer::kJak2ShadowBucket195 * 16;
}

struct ChainBuilder {
  std::vector<u8> memory = std::vector<u8>(kMemorySize);
  u32 chain_offset = kChainOffset;
  u32 cursor = kDataOffset;

  explicit ChainBuilder(u32 chain = kChainOffset, u32 data = kDataOffset)
      : chain_offset(chain), cursor(data) {
    put_tag(&memory, bucket_offset(chain_offset), DmaTag::Kind::NEXT, 0, cursor,
            vif(VifCode::Kind::MARK), 0);
  }

  u32 append(u16 qwc, u32 vif0, u32 vif1, u8 fill = 0) {
    const u32 result = cursor;
    put_tag(&memory, cursor, DmaTag::Kind::CNT, qwc, 0, vif0, vif1, fill);
    cursor += 16 + static_cast<u32>(qwc) * 16;
    return result;
  }

  void finish() {
    put_tag(&memory, cursor, DmaTag::Kind::NEXT, 0,
            chain_offset + (metal_renderer::kJak2ShadowBucket195 + 1) * 16, 0, 0);
  }
};

metal_renderer::Jak2ShadowBucket195Capture capture(const std::vector<u8>& memory,
                                                    u32 chain_offset = kChainOffset) {
  return metal_renderer::capture_jak2_shadow_bucket195(
      memory.data(), memory.size(), chain_offset, metal_renderer::kJak2ShadowBucket195);
}

void append_fixed_prefix(ChainBuilder* chain, bool include_direct35) {
  chain->append(13, vif(VifCode::Kind::STCYCL, 0, 0x404),
                vif(VifCode::Kind::UNPACK_V4_32, 13, 0x370), 0x11);
  chain->append(4, vif(VifCode::Kind::STCYCL, 0, 0x404),
                vif(VifCode::Kind::UNPACK_V4_32, 4, 0x3ac), 0x22);
  chain->append(4, vif(VifCode::Kind::STCYCL, 0, 0x404),
                vif(VifCode::Kind::UNPACK_V4_32, 4, 0), 0x33);
  chain->append(0, vif(VifCode::Kind::MSCALF, 0, 10), vif(VifCode::Kind::FLUSHE));
  chain->append(0, 0, 0);
  if (include_direct35) {
    chain->append(35, 0, vif(VifCode::Kind::DIRECT, 0, 35), 0x44);
  }
}

u32 append_v4_8(ChainBuilder* chain, u8 count, u16 address, u16 mscalf) {
  const u16 qwc = static_cast<u16>((static_cast<u32>(count) * 4 + 16) / 16);
  const u32 offset = chain->append(qwc, vif(VifCode::Kind::FLUSH),
                                   vif(VifCode::Kind::UNPACK_V4_8, count,
                                       static_cast<u16>(address | (1 << 14))),
                                   0x55);
  put_u32(&chain->memory, offset + 16 + static_cast<u32>(count) * 4, 0);
  put_u32(&chain->memory, offset + 20 + static_cast<u32>(count) * 4, 0);
  put_u32(&chain->memory, offset + 24 + static_cast<u32>(count) * 4, 0);
  put_u32(&chain->memory, offset + 28 + static_cast<u32>(count) * 4,
          vif(VifCode::Kind::MSCALF, 0, mscalf));
  return offset;
}

void test_empty_and_source_shaped_observations() {
  std::vector<u8> empty(kMemorySize);
  put_tag(&empty, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent = capture(empty);
  check(absent.status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Absent &&
            absent.reached_boundary && absent.transfer_count == 1 &&
            absent.boundary_transfer_index == 0 && absent.semantic_fingerprint != 0,
        "the exact canonical empty bucket is observed as absent");

  ChainBuilder chain;
  append_fixed_prefix(&chain, true);
  const u32 top_offset = chain.append(2, vif(VifCode::Kind::FLUSH),
                                      vif(VifCode::Kind::UNPACK_V4_32, 2, 4), 0x66);
  chain.append(2, 0, vif(VifCode::Kind::UNPACK_V4_32, 2, 174), 0x77);
  const u32 cap_offset = append_v4_8(&chain, 4, 344, 2);
  append_v4_8(&chain, 8, 600, 4);
  chain.append(35, vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 0, 35), 0x88);
  chain.finish();
  const auto observed = capture(chain.memory);
  check(observed.status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Observed &&
            observed.reached_boundary && observed.transfer_count == 13 &&
            observed.v4_32_transfer_count == 5 && observed.v4_32_unpack_count == 25 &&
            observed.v4_8_transfer_count == 2 && observed.v4_8_unpack_count == 12 &&
            observed.direct_transfer_count == 2 && observed.direct_payload_bytes == 1120 &&
            observed.flusha_direct_payload_bytes == 560 && observed.total_payload_bytes == 1600,
        "the source-shaped fixed, dynamic, and Direct scalar totals are retained in order");
  check(observed.transfers[1].role == Role::Constants &&
            observed.transfers[2].role == Role::Mystery &&
            observed.transfers[3].role == Role::Matrix &&
            observed.transfers[4].role == Role::Mscalf10 &&
            observed.transfers[6].role == Role::Direct35 &&
            observed.transfers[7].role == Role::TopVertices &&
            observed.transfers[8].role == Role::BottomVertices &&
            observed.transfers[9].role == Role::CapIndices &&
            observed.transfers[9].trailing_zero_words == 3 &&
            observed.transfers[9].trailing_mscalf_immediate == 2 &&
            observed.transfers[10].role == Role::WallIndices &&
            observed.transfers[10].trailing_mscalf_immediate == 4 &&
            observed.transfers[11].role == Role::FlushaDirect &&
            observed.transfers[12].boundary_after &&
            observed.terminal_tag_kind == static_cast<u8>(DmaTag::Kind::NEXT),
        "each required fixed, upload, trailing marker, and boundary scalar stays distinguishable");

  ChainBuilder relocated(kChainOffset + 0x1000, kDataOffset + 0x8000);
  append_fixed_prefix(&relocated, true);
  relocated.append(2, vif(VifCode::Kind::FLUSH),
                   vif(VifCode::Kind::UNPACK_V4_32, 2, 4), 0x66);
  relocated.append(2, 0, vif(VifCode::Kind::UNPACK_V4_32, 2, 174), 0x77);
  append_v4_8(&relocated, 4, 344, 2);
  append_v4_8(&relocated, 8, 600, 4);
  relocated.append(35, vif(VifCode::Kind::FLUSHA),
                   vif(VifCode::Kind::DIRECT, 0, 35), 0x88);
  relocated.finish();
  const auto relocated_observed = capture(relocated.memory, relocated.chain_offset);
  check(relocated_observed.status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Observed &&
            relocated_observed.semantic_fingerprint == observed.semantic_fingerprint,
        "numeric fingerprints are independent of packet and chain relocation");

  auto changed_geometry = chain.memory;
  changed_geometry[top_offset + 16] ^= 1;
  changed_geometry[cap_offset + 16] ^= 1;
  check(capture(changed_geometry).semantic_fingerprint == observed.semantic_fingerprint,
        "vertex and index payload bytes are neither retained nor fingerprinted");
  put_u32(&changed_geometry, cap_offset + 16 + 4 * 4 + 12,
          vif(VifCode::Kind::MSCALF, 0, 6));
  check(capture(changed_geometry).semantic_fingerprint != observed.semantic_fingerprint,
        "the bounded trailing MSCALF scalar remains part of the numeric fingerprint");
}

void test_top_only_mscalf6_stays_observational() {
  ChainBuilder chain;
  append_fixed_prefix(&chain, false);
  chain.append(3, 0, vif(VifCode::Kind::UNPACK_V4_32, 3, 4), 0x66);
  append_v4_8(&chain, 4, 344, 6);
  chain.finish();
  const auto observed = capture(chain.memory);
  check(observed.status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Observed &&
            observed.reached_boundary && observed.v4_32_transfer_count == 4 &&
            observed.v4_8_transfer_count == 1 &&
            observed.transfers[7].role == Role::CapIndices &&
            observed.transfers[7].trailing_marker_in_bounds &&
            observed.transfers[7].trailing_mscalf_immediate == 6,
        "source-reachable top-only MSCALF 6 is safely recorded without executable validation");
}

void test_malformed_and_limits() {
  check(metal_renderer::capture_jak2_shadow_bucket195(nullptr, kMemorySize, kChainOffset,
                                                       metal_renderer::kJak2ShadowBucket195)
                .status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Malformed,
        "null snapshots are malformed");
  std::vector<u8> short_memory(bucket_offset() + 8);
  check(capture(short_memory).status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Malformed,
        "an out-of-bounds bucket tag is malformed");

  ChainBuilder out_of_bounds;
  put_tag(&out_of_bounds.memory, out_of_bounds.cursor, DmaTag::Kind::REF, 2,
          kMemorySize - 16, 0, 0);
  check(capture(out_of_bounds.memory).status ==
            metal_renderer::Jak2ShadowBucket195CaptureStatus::Malformed,
        "an out-of-bounds referenced payload is malformed");

  ChainBuilder cycle;
  put_tag(&cycle.memory, cycle.cursor, DmaTag::Kind::NEXT, 0, cycle.cursor, 0, 0);
  check(capture(cycle.memory).status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Malformed,
        "a cyclic NEXT chain is malformed");

  ChainBuilder transfer_limit;
  for (std::size_t i = 0; i < metal_renderer::kJak2ShadowBucket195MaximumTransfers; ++i) {
    transfer_limit.append(0, 0, 0);
  }
  transfer_limit.finish();
  check(capture(transfer_limit.memory).status ==
            metal_renderer::Jak2ShadowBucket195CaptureStatus::LimitExceeded,
        "a chain exceeding the fixed transfer table reports LimitExceeded");

  ChainBuilder payload_limit;
  payload_limit.append(4097, 0, 0);
  payload_limit.finish();
  check(capture(payload_limit.memory).status ==
            metal_renderer::Jak2ShadowBucket195CaptureStatus::LimitExceeded,
        "a single oversized payload reports LimitExceeded before metadata retention");

  ChainBuilder count_limit;
  for (u32 i = 0; i < 129; ++i) {
    count_limit.append(0, 0, vif(VifCode::Kind::UNPACK_V4_32, 255, 4));
  }
  count_limit.finish();
  check(capture(count_limit.memory).status ==
            metal_renderer::Jak2ShadowBucket195CaptureStatus::LimitExceeded,
        "cumulative unpack counts are independently bounded");

  std::vector<u8> exact(kMemorySize);
  put_tag(&exact, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  check(metal_renderer::capture_jak2_shadow_bucket195(exact.data(), exact.size(), kChainOffset,
                                                       metal_renderer::kJak2ShadowBucket195 + 119)
                .status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Malformed,
        "the capture does not broaden its allowlist to bucket 314");
}

}  // namespace

int main() {
  test_empty_and_source_shaped_observations();
  test_top_only_mscalf6_stays_observational();
  test_malformed_and_limits();
  std::puts("jak2 shadow bucket-195 passive capture tests passed");
  return 0;
}
