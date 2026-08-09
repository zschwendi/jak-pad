#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#include "common/dma/dma_chain_validation.h"
#include "common/dma/dma_copy.h"

namespace {

int failures = 0;

void expect(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    failures++;
  }
}

void write_tag(std::vector<u8>* memory,
               u32 offset,
               DmaTag::Kind kind,
               u16 qwc = 0,
               u32 address = 0,
               bool scratchpad = false) {
  const u64 raw = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32) |
                  (static_cast<u64>(scratchpad) << 63);
  std::memcpy(memory->data() + offset, &raw, sizeof(raw));
}

void expect_error(const std::vector<u8>& memory,
                  u32 start,
                  DmaChainValidationError expected,
                  const char* description) {
  const auto result = validate_dma_chain(memory.data(), memory.size(), start);
  expect(!result && result.error == expected, description);
}

template <typename Function>
void expect_follower_throw(Function&& function, const char* description) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception&) {
    rejected = true;
  }
  expect(rejected, description);
}

void test_valid_tag_forms() {
  {
    std::vector<u8> memory(48);
    write_tag(&memory, 0, DmaTag::Kind::CNT, 1);
    write_tag(&memory, 32, DmaTag::Kind::END);
    const auto result = validate_dma_chain(memory.data(), memory.size(), 0);
    expect(result && result.tags_processed == 2, "CNT inline payload reaches END");
  }
  for (const auto kind : {DmaTag::Kind::REF, DmaTag::Kind::REFS}) {
    std::vector<u8> memory(96);
    write_tag(&memory, 0, kind, 1, 64);
    write_tag(&memory, 16, DmaTag::Kind::END);
    expect(static_cast<bool>(validate_dma_chain(memory.data(), memory.size(), 0)),
           kind == DmaTag::Kind::REF ? "REF payload reaches sequential END"
                                     : "REFS payload reaches sequential END");
  }
  {
    std::vector<u8> memory(96);
    write_tag(&memory, 0, DmaTag::Kind::REFE, 1, 64);
    expect(static_cast<bool>(validate_dma_chain(memory.data(), memory.size(), 0)),
           "REFE payload terminates");
  }
  {
    std::vector<u8> memory(96);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 1, 64);
    write_tag(&memory, 64, DmaTag::Kind::END);
    expect(static_cast<bool>(validate_dma_chain(memory.data(), memory.size(), 0)),
           "NEXT validates inline payload and target");
  }
}

void test_call_return_revisit() {
  {
    std::vector<u8> memory(96);
    write_tag(&memory, 0, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 16, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 32, DmaTag::Kind::END);
    write_tag(&memory, 64, DmaTag::Kind::RET);

    const auto result = validate_dma_chain(memory.data(), memory.size(), 0);
    expect(result && result.tags_processed == 5,
           "CALL/RET may revisit one tag with a different active return stack");
  }
  {
    std::vector<u8> memory(112);
    write_tag(&memory, 0, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 16, DmaTag::Kind::END);
    write_tag(&memory, 64, DmaTag::Kind::CALL, 0, 96);
    write_tag(&memory, 80, DmaTag::Kind::RET);
    write_tag(&memory, 96, DmaTag::Kind::RET);

    const auto result = validate_dma_chain(memory.data(), memory.size(), 0);
    expect(result && result.tags_processed == 5, "two nested CALL entries return in LIFO order");
  }
}

void test_bounds_and_alignment() {
  {
    std::vector<u8> memory(15);
    expect_error(memory, 0, DmaChainValidationError::TagHeaderOutOfRange,
                 "truncated tag header is rejected");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::END, 2);
    expect_error(memory, 0, DmaChainValidationError::InlinePayloadOutOfRange,
                 "truncated inline payload is rejected");
  }
  {
    std::vector<u8> memory(64);
    write_tag(&memory, 0, DmaTag::Kind::REFE, 2, 48);
    expect_error(memory, 0, DmaChainValidationError::ReferencePayloadOutOfRange,
                 "truncated reference payload is rejected");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 32);
    expect_error(memory, 0, DmaChainValidationError::ControlTargetOutOfRange,
                 "NEXT target past the last complete header is rejected");
  }
  {
    std::vector<u8> memory(64);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 18);
    expect_error(memory, 0, DmaChainValidationError::MisalignedTarget,
                 "misaligned NEXT target is rejected");
    expect_error(memory, 1, DmaChainValidationError::MisalignedTag,
                 "misaligned starting tag is rejected before reading");
  }
  {
    std::vector<u8> memory(64);
    write_tag(&memory, 0, DmaTag::Kind::END, std::numeric_limits<u16>::max());
    expect_error(memory, 0, DmaChainValidationError::InlinePayloadOutOfRange,
                 "maximum QWC byte range is checked without wrapping");
  }
}

void test_control_flow_failures() {
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 0);
    expect_error(memory, 0, DmaChainValidationError::Cycle, "self-NEXT cycle is rejected");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 16);
    write_tag(&memory, 16, DmaTag::Kind::NEXT, 0, 0);
    expect_error(memory, 0, DmaChainValidationError::Cycle,
                 "multi-node NEXT cycle is rejected");
  }
  {
    std::vector<u8> memory(160);
    write_tag(&memory, 0, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 64, DmaTag::Kind::CALL, 0, 96);
    write_tag(&memory, 96, DmaTag::Kind::CALL, 0, 128);
    write_tag(&memory, 128, DmaTag::Kind::END);
    expect_error(memory, 0, DmaChainValidationError::CallStackOverflow,
                 "third nested CALL is rejected");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::RET);
    expect_error(memory, 0, DmaChainValidationError::ReturnStackUnderflow,
                 "RET without CALL is rejected");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::CNT, 0, 16);
    expect_error(memory, 0, DmaChainValidationError::CntAddress,
                 "CNT with an address is rejected like DmaFollower");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::END, 0, 0, true);
    expect_error(memory, 0, DmaChainValidationError::ScratchpadAddress,
                 "scratchpad DMA is rejected like DmaFollower");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::CNT);
    write_tag(&memory, 16, DmaTag::Kind::END);
    const auto result = validate_dma_chain(memory.data(), memory.size(), 0, 1);
    expect(!result && result.error == DmaChainValidationError::TagBudgetExceeded,
           "finite tag budget fails closed");
  }
}

void test_default_validation_has_no_policy_cap() {
  constexpr std::size_t kFormerDefaultLimit = 1u << 20;
  constexpr std::size_t kTagCount = kFormerDefaultLimit + 2;
  std::vector<u8> memory(kTagCount * 16);
  for (std::size_t tag = 0; tag + 1 < kTagCount; ++tag) {
    write_tag(&memory, static_cast<u32>(tag * 16), DmaTag::Kind::CNT);
  }
  write_tag(&memory, static_cast<u32>((kTagCount - 1) * 16), DmaTag::Kind::END);

  const auto result = validate_dma_chain(memory.data(), memory.size(), 0);
  expect(result && result.tags_processed == kTagCount,
         "default validation accepts an acyclic chain beyond the former policy cap");
}

void test_fixed_chunk_copier_preflight() {
  std::vector<u8> memory(FixedChunkDmaCopier::chunk_size);
  write_tag(&memory, 0, DmaTag::Kind::END);
  FixedChunkDmaCopier copier(static_cast<u32>(memory.size()));
  const auto& copied = copier.run(memory.data(), 0);
  expect(static_cast<bool>(
             validate_dma_chain(copied.data.data(), copied.data.size(), copied.start_offset)),
         "fixed-chunk copier output remains a valid bounded chain");

  write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 0);
  bool rejected = false;
  try {
    copier.run(memory.data(), 0);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  expect(rejected, "fixed-chunk copier rejects a cycle before following its first tag");
}

void test_bounded_follower_stops_after_terminal_tag() {
  std::vector<u8> memory(16);
  write_tag(&memory, 0, DmaTag::Kind::REFE);
  DmaFollower follower(memory.data(), 0, memory.size());
  follower.read_and_advance();

  bool stopped = false;
  try {
    follower.read_and_advance();
  } catch (const std::logic_error&) {
    stopped = true;
  }
  expect(stopped, "bounded DmaFollower rejects a read after terminal REFE before tag access");
}

void test_legacy_follower_valid_behavior() {
  {
    std::vector<u8> memory(48);
    write_tag(&memory, 0, DmaTag::Kind::CNT, 1);
    write_tag(&memory, 32, DmaTag::Kind::END);
    DmaFollower follower(memory.data(), 0);
    const auto cnt = follower.read_and_advance();
    const bool cnt_ok = cnt.data_offset == 16 && cnt.size_bytes == 16 &&
                        follower.current_tag_offset() == 32 && !follower.ended();
    follower.read_and_advance();
    expect(cnt_ok && follower.ended() && follower.current_tag_offset() == 32,
           "legacy two-argument follower preserves CNT and END offsets");
  }
  {
    std::vector<u8> memory(96);
    write_tag(&memory, 0, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 16, DmaTag::Kind::END);
    write_tag(&memory, 64, DmaTag::Kind::RET);
    DmaFollower follower(memory.data(), 0);
    follower.read_and_advance();
    const bool called = follower.current_tag_offset() == 64;
    follower.read_and_advance();
    const bool returned = follower.current_tag_offset() == 16;
    follower.read_and_advance();
    expect(called && returned && follower.ended(),
           "legacy two-argument follower preserves CALL and RET behavior");
  }
}

void test_bounded_follower_rejects_malformed_chains_without_asserts() {
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::CNT, 1);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects a truncated CNT payload");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 1, 0);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects a truncated NEXT payload");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::REF, 2, 16);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects a truncated REF payload");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 32);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects an out-of-range NEXT target");
  }
  {
    std::vector<u8> memory(64);
    write_tag(&memory, 0, DmaTag::Kind::NEXT, 0, 18);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects a misaligned NEXT target");

    DmaFollower misaligned_start(memory.data(), 1, memory.size());
    expect_follower_throw([&]() { misaligned_start.current_tag(); },
                          "bounded follower rejects a misaligned starting tag");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::END, 0, 0, true);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects scratchpad DMA without ASSERT");
  }
  {
    std::vector<u8> memory(32);
    write_tag(&memory, 0, DmaTag::Kind::CNT, 0, 16);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects a CNT address without ASSERT");
  }
  {
    std::vector<u8> memory(160);
    write_tag(&memory, 0, DmaTag::Kind::CALL, 0, 64);
    write_tag(&memory, 64, DmaTag::Kind::CALL, 0, 96);
    write_tag(&memory, 96, DmaTag::Kind::CALL, 0, 128);
    write_tag(&memory, 128, DmaTag::Kind::END);
    DmaFollower follower(memory.data(), 0, memory.size());
    follower.read_and_advance();
    follower.read_and_advance();
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects CALL stack overflow without ASSERT");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::RET);
    DmaFollower follower(memory.data(), 0, memory.size());
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects RET stack underflow without ASSERT");
  }
  {
    std::vector<u8> memory(16);
    write_tag(&memory, 0, DmaTag::Kind::END);
    DmaFollower follower(memory.data(), 0, memory.size());
    follower.read_and_advance();
    expect_follower_throw([&]() { follower.current_tag(); },
                          "bounded follower rejects current_tag after END");
    expect_follower_throw([&]() { follower.current_tag_vif0(); },
                          "bounded follower rejects current_tag_vif0 after END");
    expect_follower_throw([&]() { follower.current_tag_vif1(); },
                          "bounded follower rejects current_tag_vif1 after END");
    expect_follower_throw([&]() { follower.current_tag_vifcode0(); },
                          "bounded follower rejects current_tag_vifcode0 after END");
    expect_follower_throw([&]() { follower.current_tag_vifcode1(); },
                          "bounded follower rejects current_tag_vifcode1 after END");
    expect_follower_throw([&]() { follower.read_and_advance(); },
                          "bounded follower rejects read_and_advance after END");
  }
}

}  // namespace

int main() {
  test_valid_tag_forms();
  test_call_return_revisit();
  test_bounds_and_alignment();
  test_control_flow_failures();
  test_default_validation_has_no_policy_cap();
  test_fixed_chunk_copier_preflight();
  test_bounded_follower_stops_after_terminal_tag();
  test_legacy_follower_valid_behavior();
  test_bounded_follower_rejects_malformed_chains_without_asserts();

  if (failures) {
    std::printf("FAIL: %d bounded DMA validation checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: bounded DMA validation and copier preflight\n");
  return 0;
}
