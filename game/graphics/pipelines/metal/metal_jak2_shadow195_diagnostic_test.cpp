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

using T = metal_renderer::Jak2ShadowBucket195DiagnosticTermination;
using L = metal_renderer::Jak2ShadowBucket195DiagnosticLimit;
auto diag(const std::vector<u8>& m) {
  return metal_renderer::diagnose_jak2_shadow_bucket195(m.data(), m.size(), kChainOffset);
}
auto cap(const std::vector<u8>& m) {
  return metal_renderer::capture_jak2_shadow_bucket195(m.data(), m.size(), kChainOffset,
                                                       metal_renderer::kJak2ShadowBucket195);
}
}  // namespace

// The raw chain always contains the initial bucket NEXT tag and the finish NEXT tag, so a chain
// with `cap - 2` appended transfers is exactly at the normal-path transfer capacity.
constexpr u32 kCap = static_cast<u32>(metal_renderer::kJak2ShadowBucket195MaximumTransfers);
constexpr u32 kOverAppended = kCap + 20;
constexpr u32 kOverWalked = kOverAppended + 2;
constexpr u32 kBoundAppended = metal_renderer::kJak2ShadowBucket195DiagnosticMaximumTransfers + 76;
static_assert(kCap == 781);
static_assert(kOverWalked < metal_renderer::kJak2ShadowBucket195DiagnosticMaximumTransfers);
static_assert(kBoundAppended > metal_renderer::kJak2ShadowBucket195DiagnosticMaximumTransfers);
static_assert((kBoundAppended + 2) * 16 + kDataOffset < kMemorySize);

int main() {
  ChainBuilder ok;  // initial + (cap - 2) + finish = cap transfers: normal acceptance unchanged
  for (u32 i = 0; i < kCap - 2; ++i) ok.append(0, 0, 0);
  ok.finish();
  check(cap(ok.memory).status == metal_renderer::Jak2ShadowBucket195CaptureStatus::Observed,
        "cap-transfer chain still observed by strict capture");
  auto d = diag(ok.memory);
  check(d.termination == T::Boundary && d.first_limit == L::None && d.transfers_walked == kCap,
        "cap-transfer diag boundary, no limit");

  ChainBuilder over;  // cap + 20 transfers + finish: valid raw continuation beyond cap
  for (u32 i = 0; i < kOverAppended; ++i)
    over.append(1, 0, vif(VifCode::Kind::UNPACK_V4_32, 1, 4));
  over.finish();
  check(cap(over.memory).status == metal_renderer::Jak2ShadowBucket195CaptureStatus::LimitExceeded,
        "strict capture still LimitExceeded");
  d = diag(over.memory);
  check(d.termination == T::Boundary && d.transfers_walked == kOverWalked,
        "raw continuation beyond cap reaches boundary");
  check(d.first_limit == L::TransferTable && d.first_limit_transfer_index == kCap,
        "exact first limit index at cap");
  check(d.tail_row_count == metal_renderer::kJak2ShadowBucket195DiagnosticTailRows &&
            d.tail[0].index == kCap - 1 && d.tail[1].index == kCap,
        "bounded tail rows start at cap - 1");
  check(d.v4_32_unpack_count == kOverAppended && d.total_payload_bytes == kOverAppended * 16ull,
        "accounting beyond cap");

  ChainBuilder cyc;
  for (u32 i = 0; i < kOverAppended; ++i) cyc.append(0, 0, 0);
  put_tag(&cyc.memory, cyc.cursor, DmaTag::Kind::NEXT, 0, cyc.cursor, 0, 0);
  d = diag(cyc.memory);
  check(d.termination == T::DmaReadFailed &&
            d.follower_reject == metal_renderer::Jak2ShadowBucket195FollowerReject::Cycle &&
            d.first_limit == L::TransferTable && d.first_limit_transfer_index == kCap &&
            d.termination_index == kOverWalked,
        "cycle beyond cap not boundary");
  std::printf("cycle termination=%u index=%u\n", (unsigned)d.termination, d.termination_index);

  ChainBuilder span;
  for (u32 i = 0; i < kOverAppended; ++i) span.append(0, 0, 0);
  put_tag(&span.memory, span.cursor, DmaTag::Kind::NEXT, 0, kMemorySize + 0x1000, 0, 0);
  d = diag(span.memory);
  check(d.termination == T::DmaReadFailed && d.termination_index == kCap + 21 &&
            d.follower_reject ==
                metal_renderer::Jak2ShadowBucket195FollowerReject::InvalidTagOrPayloadSpan &&
            d.first_limit == L::TransferTable && d.first_limit_transfer_index == kCap,
        "out-of-span after cap");

  ChainBuilder bound;
  for (u32 i = 0; i < kBoundAppended; ++i) bound.append(0, 0, 0);
  bound.finish();
  d = diag(bound.memory);
  check(d.termination == T::WalkBound &&
            d.transfers_walked == metal_renderer::kJak2ShadowBucket195DiagnosticMaximumTransfers &&
            cap(bound.memory).status ==
                metal_renderer::Jak2ShadowBucket195CaptureStatus::LimitExceeded,
        "hard diagnostic bound");
  std::puts("jak2 shadow bucket-195 diagnostic tests passed");
  return 0;
}
