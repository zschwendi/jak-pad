#include "game/graphics/pipelines/metal/metal_jak2_sky_post_texture_upload_plan.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kSetupOffset = 0x4000;
constexpr u32 kResetOffset = 0x5000;
constexpr u32 kAnimatorOffset = 0x6000;
constexpr u32 kPageOffset = 0x8000;
constexpr u32 kAlternatePageOffset = 0xa000;
constexpr std::size_t kMemorySize = 0xc000;
constexpr u8 kDmaCnt = 1;
constexpr u8 kDmaNext = 2;
constexpr u32 kPcPortVif = 8u << 24;
constexpr u32 kFlushaVif = 19u << 24;
constexpr u32 kDirectVif = 80u << 24;

using Plan = metal_renderer::Jak2SkyPostTextureUploadPlan;
using Variant = metal_renderer::Jak2SkyPostTextureUploadVariant;

void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 bucket_offset(u32 chain_offset = kChainOffset) {
  return chain_offset + metal_renderer::kJak2SkyPostTextureUploadBucket * 16;
}

void put_u32(std::vector<u8> *memory, u32 offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8> *memory, u32 offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8> *memory, u32 offset, u8 kind, u16 qwc, u32 address,
             u32 vif0, u32 vif1) {
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void put_page_header(std::vector<u8> *memory, u32 page_offset, u8 seed) {
  for (std::size_t i = 0;
       i < metal_renderer::kJak2SkyPostTexturePageHeaderBytes; ++i) {
    (*memory)[page_offset + i] = static_cast<u8>(seed + i);
  }
  put_u32(memory, page_offset + 12, 1);
  put_u32(memory, page_offset + 124, page_offset + 0x200);
}

std::vector<u8> make_absent_fixture(u32 chain_offset = kChainOffset) {
  std::vector<u8> memory(kMemorySize);
  put_tag(&memory, bucket_offset(chain_offset), kDmaNext, 0,
          bucket_offset(chain_offset) + 16, 0, 0);
  return memory;
}

std::vector<u8> make_present_fixture(u32 chain_offset = kChainOffset,
                                     u32 relocation = 0) {
  std::vector<u8> memory(kMemorySize);
  const u32 setup = kSetupOffset + relocation;
  const u32 reset = kResetOffset + relocation;
  put_tag(&memory, bucket_offset(chain_offset), kDmaNext, 0, setup, 0, 0);
  put_tag(&memory, setup, kDmaCnt, 2, 0, 0, kDirectVif | 2);
  for (u32 i = 0; i < 32; ++i) {
    memory[setup + 16 + i] = static_cast<u8>(0x10 + i);
  }
  put_tag(&memory, setup + 48, kDmaCnt, 1, 0, kPcPortVif, 3);
  put_u64(&memory, setup + 64, kPageOffset);
  put_u64(&memory, setup + 72, static_cast<u64>(-1));
  put_tag(&memory, setup + 80, kDmaNext, 0, reset, 0, 0);
  put_tag(&memory, reset, kDmaCnt, 10, 0, kFlushaVif, kDirectVif | 10);
  for (u32 i = 0; i < 160; ++i) {
    memory[reset + 16 + i] = static_cast<u8>(0x80 + i);
  }
  put_tag(&memory, reset + 176, kDmaNext, 0, bucket_offset(chain_offset) + 16,
          0, 0);
  put_page_header(&memory, kPageOffset, 0x21);
  put_page_header(&memory, kAlternatePageOffset, 0x41);
  return memory;
}

std::optional<Plan> parse(const std::vector<u8> &memory,
                          u32 chain_offset = kChainOffset) {
  return metal_renderer::plan_jak2_sky_post_texture_upload(
      memory.data(), memory.size(), chain_offset,
      metal_renderer::kJak2SkyPostTextureUploadBucket, memory.data(),
      memory.size());
}

void test_exact_present_and_absent() {
  auto memory = make_present_fixture();
  const auto plan = parse(memory);
  check(plan.has_value() && plan->variant == Variant::Present &&
            plan->transfer_count == 6 && plan->total_payload_bytes == 208 &&
            plan->page_offset == kPageOffset && plan->mode == -1 &&
            plan->transfers[0].tag_kind == kDmaNext &&
            plan->transfers[1].qwc == 2 &&
            plan->transfers[1].payload_bytes == 32 &&
            plan->transfers[2].qwc == 1 &&
            plan->transfers[2].payload_bytes == 16 &&
            plan->transfers[4].qwc == 10 &&
            plan->transfers[4].payload_bytes == 160,
        "the exact six-transfer/208-byte sky-post envelope produces an owned "
        "plan");

  const u8 owned_byte = plan->page_header[0];
  memory[kPageOffset] ^= 0xff;
  check(plan->page_header[0] == owned_byte,
        "the sky-post plan owns its 128-byte texture-page header");

  const auto absent = parse(make_absent_fixture());
  check(absent.has_value() && absent->variant == Variant::Absent &&
            absent->transfer_count == 1 && absent->total_payload_bytes == 0,
        "the exact one-link empty bucket produces an absent plan");
}

void test_relocated_semantic_match() {
  const auto live_memory = make_present_fixture();
  constexpr u32 kRelocation = 0x1000;
  const auto copied_memory =
      make_present_fixture(kChainOffset + kRelocation, kRelocation);
  const auto live = parse(live_memory);
  const auto copied = parse(copied_memory, kChainOffset + kRelocation);
  check(live.has_value() && copied.has_value() &&
            metal_renderer::jak2_sky_post_texture_upload_plans_match(*live,
                                                                     *copied),
        "DMA tag-address and packet-offset relocation does not change plan "
        "semantics");
}

void test_semantic_mutations_do_not_match() {
  const auto baseline_memory = make_present_fixture();
  const auto baseline = parse(baseline_memory);
  check(baseline.has_value(), "the semantic-mutation baseline parses");

  auto header_memory = make_present_fixture();
  header_memory[kPageOffset] ^= 0x40;
  const auto changed_header = parse(header_memory);
  check(changed_header.has_value() &&
            !metal_renderer::jak2_sky_post_texture_upload_plans_match(
                *baseline, *changed_header),
        "a changed owned page-header byte fails semantic matching");

  auto descriptor_memory = make_present_fixture();
  put_u64(&descriptor_memory, kSetupOffset + 64, kAlternatePageOffset);
  const auto changed_descriptor = parse(descriptor_memory);
  check(changed_descriptor.has_value() &&
            !metal_renderer::jak2_sky_post_texture_upload_plans_match(
                *baseline, *changed_descriptor),
        "a changed valid page descriptor fails semantic matching");

  auto gs_payload_memory = make_present_fixture();
  gs_payload_memory[kSetupOffset + 16] ^= 1;
  const auto changed_gs_payload = parse(gs_payload_memory);
  check(changed_gs_payload.has_value() &&
            !metal_renderer::jak2_sky_post_texture_upload_plans_match(
                *baseline, *changed_gs_payload),
        "a changed GS payload fails semantic matching");

  auto reset_payload_memory = make_present_fixture();
  reset_payload_memory[kResetOffset + 16] ^= 1;
  const auto changed_reset_payload = parse(reset_payload_memory);
  check(changed_reset_payload.has_value() &&
            !metal_renderer::jak2_sky_post_texture_upload_plans_match(
                *baseline, *changed_reset_payload),
        "a changed reset payload fails semantic matching");
}

void test_malformed_envelopes_rejected() {
  auto wrong_mode = make_present_fixture();
  put_u64(&wrong_mode, kSetupOffset + 72, static_cast<u64>(-2));
  check(!parse(wrong_mode).has_value(), "mode other than -1 is rejected");

  auto wrong_vif = make_present_fixture();
  put_u32(&wrong_vif, kSetupOffset + 48 + 8, kPcPortVif | 1);
  check(!parse(wrong_vif).has_value(),
        "a nonzero descriptor PC_PORT immediate is rejected");

  auto wrong_qwc = make_present_fixture();
  put_tag(&wrong_qwc, kSetupOffset, kDmaCnt, 1, 0, 0, kDirectVif | 2);
  check(!parse(wrong_qwc).has_value(), "the wrong GS setup qwc is rejected");

  auto wrong_link = make_present_fixture();
  put_tag(&wrong_link, kSetupOffset + 80, kDmaNext, 0, bucket_offset() + 16, 0,
          0);
  check(!parse(wrong_link).has_value(),
        "a link that skips the reset is rejected");

  auto wrong_reset = make_present_fixture();
  put_tag(&wrong_reset, kResetOffset, kDmaCnt, 10, 0, 0, kDirectVif | 10);
  check(!parse(wrong_reset).has_value(), "a reset without FLUSHA is rejected");

  auto extra_animator = make_present_fixture();
  put_tag(&extra_animator, kSetupOffset + 80, kDmaNext, 0, kAnimatorOffset, 0,
          0);
  put_tag(&extra_animator, kAnimatorOffset, kDmaCnt, 0, 0, kPcPortVif | 12, 0);
  put_tag(&extra_animator, kAnimatorOffset + 16, kDmaNext, 0, kResetOffset, 0,
          0);
  check(!parse(extra_animator).has_value(),
        "an inserted animator sequence is rejected");

  auto invalid_header = make_present_fixture();
  put_u32(&invalid_header, kPageOffset + 12, 0xffffffff);
  check(!parse(invalid_header).has_value(),
        "a negative texture-page length is rejected");

  const auto wrong_bucket = metal_renderer::plan_jak2_sky_post_texture_upload(
      wrong_mode.data(), wrong_mode.size(), kChainOffset, 308,
      wrong_mode.data(), wrong_mode.size());
  check(!wrong_bucket.has_value(), "a bucket other than 309 is rejected");
}

} // namespace

int main() {
  test_exact_present_and_absent();
  test_relocated_semantic_match();
  test_semantic_mutations_do_not_match();
  test_malformed_envelopes_rejected();
  std::puts("PASS: Jak 2 sky-post bucket-309 typed plan");
  return 0;
}
