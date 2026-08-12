#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kMemorySize = 0x20000;
constexpr u32 kDataOffset = 0x4000;
constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 vif(VifCode::Kind kind) {
  return static_cast<u32>(kind) << 24;
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

u32 bucket_offset() {
  return kChainOffset + metal_renderer::kJak2EffectsBucket * 16;
}

std::vector<u8> make_chain(u32 fragments, u32 vertices) {
  std::vector<u8> memory(kMemorySize);
  put_tag(&memory, bucket_offset(), DmaTag::Kind::NEXT, 0, kDataOffset,
          vif(VifCode::Kind::MARK), 0);
  u32 cursor = kDataOffset;
  auto append = [&](u16 qwc, u32 vif0, u32 vif1, u8 fill = 0) {
    put_tag(&memory, cursor, DmaTag::Kind::CNT, qwc, 0, vif0, vif1, fill);
    cursor += 16 + qwc * 16;
  };
  append(2, 0, kDirect | 2, 0x22);
  append(8, vif(VifCode::Kind::STCYCL), vif(VifCode::Kind::UNPACK_V4_32), 0x33);
  append(2, vif(VifCode::Kind::MSCALF), vif(VifCode::Kind::STMOD), 0x44);
  append(0, 0, 0);
  for (u32 i = 0; i < fragments; ++i) {
    append(12, 0, vif(VifCode::Kind::UNPACK_V4_32), static_cast<u8>(0x50 + i));
    append(vertices * 3, 0, vif(VifCode::Kind::UNPACK_V4_32), static_cast<u8>(0x70 + i));
    append(0, 0, vif(VifCode::Kind::MSCAL));
  }
  append(10, vif(VifCode::Kind::FLUSHA), kDirect | 10, 0xaa);
  put_tag(&memory, cursor, DmaTag::Kind::NEXT, 0,
          kChainOffset + (metal_renderer::kJak2EffectsBucket + 1) * 16, 0, 0);
  return memory;
}

metal_renderer::Jak2EffectsBucket315Capture capture(const std::vector<u8>& memory) {
  return metal_renderer::capture_jak2_effects_bucket315(memory.data(), memory.size(),
                                                         kChainOffset,
                                                         metal_renderer::kJak2EffectsBucket);
}

void test_capture_and_telemetry() {
  std::vector<u8> absent(kMemorySize);
  put_tag(&absent, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_capture = capture(absent);
  auto city = make_chain(0, 0);
  const auto city_capture = capture(city);
  check(city_capture.valid && city_capture.present &&
            city_capture.classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            city_capture.transfer_count == 7 && city_capture.total_payload_bytes == 352 &&
            city_capture.fragment_count == 0 && city_capture.semantic_fingerprint != 0,
        "the 352-byte source-shaped Lightning envelope is captured before execution");

  auto active = make_chain(2, 32);
  const auto active_capture = capture(active);
  check(active_capture.valid && active_capture.transfer_count == 13 &&
            active_capture.total_payload_bytes == 3808 && active_capture.fragment_count == 2 &&
            active_capture.vertex_count == 64 &&
            active_capture.transfers[6].payload_fingerprint != 0,
        "the 3808-byte envelope owns transfer boundaries and payload fingerprints");
  check(!metal_renderer::jak2_effects_bucket315_captures_match(city_capture, active_capture),
        "different live/copy semantic input never compares equal");
  const auto active_copy = capture(active);
  check(metal_renderer::jak2_effects_bucket315_captures_match(active_capture, active_copy),
        "independent equivalent captures compare without retaining packets");
  active[active_capture.transfers[6].relative_tag_offset + bucket_offset() + 16] ^= 1;
  check(!metal_renderer::jak2_effects_bucket315_captures_match(active_capture, capture(active)),
        "a payload mutation changes the owned capture fingerprint");

  const auto large_capture = capture(make_chain(12, 90));
  check(large_capture.valid && large_capture.classification ==
             metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            large_capture.total_payload_bytes == 54496 && large_capture.fragment_count == 12 &&
            large_capture.vertex_count == 1080,
        "a large source-shaped envelope stays bounded and capture-only");

  metal_renderer::Jak2EffectsBucket315Telemetry telemetry;
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, absent_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, city_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, active_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, large_capture);
  check(telemetry.captures == 4 && telemetry.valid_captures == 4 &&
            telemetry.absent_captures == 1 && telemetry.lightning_captures == 3 &&
            telemetry.captured_payload_bytes == 58656 && telemetry.last_payload_bytes == 54496 &&
            telemetry.last_fragment_count == 12 && telemetry.last_vertex_count == 1080,
        "numeric telemetry exposes absent/352/3808/large-ready capture state");
}

void test_absent_and_rejections() {
  std::vector<u8> absent(kMemorySize);
  put_tag(&absent, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_capture = capture(absent);
  check(absent_capture.valid && !absent_capture.present &&
            absent_capture.classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Absent &&
            absent_capture.total_payload_bytes == 0,
        "the exact empty source slot is captured as absent");

  auto malformed = make_chain(1, 32);
  put_u32(&malformed, kDataOffset + 12, vif(VifCode::Kind::NOP));
  check(capture(malformed).classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "a structurally safe but non-Lightning VIF form stays capture-only");
  check(!metal_renderer::capture_jak2_effects_bucket315(malformed.data(), malformed.size(),
                                                         kChainOffset,
                                                         metal_renderer::kJak2EffectsBucket + 1)
             .valid,
        "the passive seam rejects every other bucket ID");
}

}  // namespace

int main() {
  test_capture_and_telemetry();
  test_absent_and_rejections();
  std::puts("jak2 effects bucket-315 capture tests passed");
  return 0;
}
