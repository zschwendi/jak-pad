#include "game/kernel/jak2/method_set_guard.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "game/kernel/core/aot_method_set_policy.h"

namespace {

constexpr u32 kTypeType = 0x84004;
constexpr u32 kObjectType = 0x84044;
constexpr u32 kTarget = 0x84104;
constexpr u32 kChild = 0x84144;
constexpr u32 kGrandchild = 0x84184;
constexpr u32 kUnrelated = 0x841c4;
constexpr u32 kForward = 0x84244;
constexpr u32 kChainStart = 0x88004;
constexpr u32 kEeChild = 0x100004;
constexpr u32 kNodeStride = 0x20;
constexpr u32 kInvalidParent = 0xb9b01646;

int g_failures = 0;

void expect(bool ok, const char* message) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", message);
  if (!ok) {
    ++g_failures;
  }
}

void write_u32(std::vector<u8>& memory, u32 offset, u32 value) {
  std::memcpy(memory.data() + offset, &value, sizeof(value));
}

void make_type(std::vector<u8>& memory, u32 offset, u32 parent) {
  write_u32(memory, offset - BASIC_OFFSET, kTypeType);
  write_u32(memory, offset + sizeof(u32), parent);
}

jak2::MethodSetTypeChainResult check(const std::vector<u8>& memory,
                                     u32 candidate,
                                     u32 target,
                                     int* parent_reads = nullptr) {
  return jak2::check_method_set_type_chain(
      candidate, target, kObjectType, 0x100000, kTypeType,
      [&](u32 offset) {
        u32 value = 0;
        std::memcpy(&value, memory.data() + offset, sizeof(value));
        return value;
      },
      [&](u32 offset) {
        if (parent_reads) {
          ++*parent_reads;
        }
        u32 value = 0;
        std::memcpy(&value, memory.data() + offset + sizeof(u32), sizeof(value));
        return value;
      });
}

jak2::MethodSetTypeChainResult check_boundary(u32 candidate,
                                              int* type_tag_reads,
                                              int* parent_reads) {
  return jak2::check_method_set_type_chain(
      candidate, kTarget, kObjectType, 0x100000, kTypeType,
      [&](u32) {
        ++*type_tag_reads;
        return kTypeType;
      },
      [&](u32) {
        ++*parent_reads;
        return kTarget;
      });
}

}  // namespace

int main() {
  constexpr u32 kBootLinkFlags = LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;
  const auto boot_policy = aot_boot_method_set_policy();
  expect(boot_policy.enable_method_set && !boot_policy.force_fast_link,
         "direct AOT top-level APIs preserve boot method propagation");

  const auto package_policy = aot_method_set_link_policy(kBootLinkFlags, false, false);
  expect(!package_policy.enable_method_set && !package_policy.force_fast_link,
         "non-fast package AOT top-levels do not infer boot propagation");

  const auto level_policy =
      aot_method_set_link_policy(kBootLinkFlags | LINK_FLAG_FORCE_FAST_LINK, false, false);
  expect(!level_policy.enable_method_set && level_policy.force_fast_link,
         "fast-linked level AOT top-levels do not enable method propagation");

  const auto debug_level_policy = aot_method_set_link_policy(
      kBootLinkFlags | LINK_FLAG_FORCE_FAST_LINK | LINK_FLAG_FORCE_DEBUG, true, false);
  expect(debug_level_policy.enable_method_set && debug_level_policy.force_fast_link,
         "forced-debug level AOT top-levels retain klink's keep-debug propagation");

  const auto debug_package_policy =
      aot_method_set_link_policy(kBootLinkFlags | LINK_FLAG_FORCE_DEBUG, true, false);
  expect(debug_package_policy.enable_method_set && !debug_package_policy.force_fast_link,
         "non-fast FORCE_DEBUG package enables propagation in MasterDebug");

  const auto disk_boot_debug_policy =
      aot_method_set_link_policy(kBootLinkFlags | LINK_FLAG_FORCE_DEBUG, true, true);
  expect(!disk_boot_debug_policy.enable_method_set && !disk_boot_debug_policy.force_fast_link,
         "disk boot suppresses non-fast FORCE_DEBUG package propagation");

  int boundary_tag_reads = 0;
  int boundary_parent_reads = 0;
  expect(
      check_boundary(EE_MAIN_MEM_SIZE - 12, &boundary_tag_reads, &boundary_parent_reads).status ==
              jak2::MethodSetTypeChainStatus::SUBTYPE &&
          boundary_tag_reads == 1 && boundary_parent_reads == 1,
      "last safe EE BASIC type permits the full parent read");
  boundary_tag_reads = 0;
  boundary_parent_reads = 0;
  expect(
      check_boundary(EE_MAIN_MEM_SIZE - BASIC_OFFSET, &boundary_tag_reads, &boundary_parent_reads)
                  .status == jak2::MethodSetTypeChainStatus::INVALID_RANGE &&
          boundary_tag_reads == 0 && boundary_parent_reads == 0,
      "top EE BASIC pointer is rejected before any read");

  boundary_tag_reads = 0;
  boundary_parent_reads = 0;
  expect(check_boundary(0x100000 - 12, &boundary_tag_reads, &boundary_parent_reads).status ==
                 jak2::MethodSetTypeChainStatus::SUBTYPE &&
             boundary_tag_reads == 1 && boundary_parent_reads == 1,
         "last safe kernel BASIC type permits the full parent read");
  boundary_tag_reads = 0;
  boundary_parent_reads = 0;
  expect(
      check_boundary(0x100000 - BASIC_OFFSET, &boundary_tag_reads, &boundary_parent_reads).status ==
              jak2::MethodSetTypeChainStatus::INVALID_RANGE &&
          boundary_tag_reads == 0 && boundary_parent_reads == 0,
      "top kernel BASIC pointer is rejected before any read");

  std::vector<u8> memory(0x101000);
  make_type(memory, kTypeType, kObjectType);
  make_type(memory, kObjectType, 0);
  make_type(memory, kTarget, kObjectType);
  make_type(memory, kChild, kTarget);
  make_type(memory, kGrandchild, kChild);
  make_type(memory, kUnrelated, kObjectType);
  make_type(memory, kForward, 0);
  make_type(memory, kEeChild, kTarget);

  expect(check(memory, kGrandchild, kTarget).status == jak2::MethodSetTypeChainStatus::SUBTYPE,
         "valid multi-level inheritance propagates");
  expect(check(memory, kEeChild, kTarget).status == jak2::MethodSetTypeChainStatus::SUBTYPE,
         "valid EE-memory type inheritance propagates");
  expect(check(memory, kUnrelated, kTarget).status == jak2::MethodSetTypeChainStatus::UNRELATED,
         "unrelated valid type does not propagate");
  int forward_parent_reads = 0;
  expect(check(memory, kForward, kTarget, &forward_parent_reads).status ==
                 jak2::MethodSetTypeChainStatus::UNRELATED &&
             forward_parent_reads == 1,
         "forward type with no parent is unrelated after one parent read");

  make_type(memory, kChild, kInvalidParent);
  int parent_reads = 0;
  const auto invalid_parent = check(memory, kChild, kTarget, &parent_reads);
  expect(invalid_parent.status == jak2::MethodSetTypeChainStatus::INVALID_RANGE &&
             invalid_parent.bad_parent == kInvalidParent && parent_reads == 1,
         "out-of-range parent is rejected before dereference");

  constexpr u32 kMisalignedParent = kTarget + sizeof(u32);
  make_type(memory, kChild, kMisalignedParent);
  expect(check(memory, kChild, kGrandchild).status ==
             jak2::MethodSetTypeChainStatus::INVALID_BASIC_ALIGNMENT,
         "non-BASIC-aligned parent is rejected");

  constexpr u32 kBadTagParent = 0x84204;
  make_type(memory, kChild, kBadTagParent);
  write_u32(memory, kBadTagParent - BASIC_OFFSET, kObjectType);
  expect(
      check(memory, kChild, kGrandchild).status == jak2::MethodSetTypeChainStatus::INVALID_TYPE_TAG,
      "parent without the type tag is rejected");

  make_type(memory, kChild, kGrandchild);
  make_type(memory, kGrandchild, kChild);
  const auto cycle = check(memory, kChild, kTarget);
  expect(cycle.status == jak2::MethodSetTypeChainStatus::CYCLE && cycle.bad_parent == kChild,
         "parent cycle is rejected");

  for (std::size_t i = 0; i <= jak2::METHOD_SET_TYPE_PARENT_LIMIT; ++i) {
    const u32 offset = kChainStart + i * kNodeStride;
    make_type(memory, offset, offset + kNodeStride);
  }
  const auto depth = check(memory, kChainStart, kTarget);
  expect(depth.status == jak2::MethodSetTypeChainStatus::DEPTH_LIMIT &&
             depth.bad_parent == kChainStart + jak2::METHOD_SET_TYPE_PARENT_LIMIT * kNodeStride,
         "overlong parent chain is bounded");

  std::printf(
      "\n%s (%d failures)\n",
      g_failures ? "JAK 2 METHOD-SET GUARD TEST FAILED" : "JAK 2 METHOD-SET GUARD TEST PASSED",
      g_failures);
  return g_failures ? 1 : 0;
}
