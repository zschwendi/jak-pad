#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kMemorySize = 0x20000;
constexpr u32 kDataOffset = 0x4000;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0, bool interrupt = false) {
  return (static_cast<u32>(interrupt) << 31) | (static_cast<u32>(kind) << 24) |
         (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_unpack_v4_32(u16 address, u8 count) {
  return vif(VifCode::Kind::UNPACK_V4_32, address, count);
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
  return chain_offset + metal_renderer::kJak2EffectsBucket * 16;
}

std::vector<u8> make_chain(u32 fragments,
                           u32 vertices,
                           u32 tex1_mmin = 1,
                           u32 tex1_mxl = 0,
                           u32 chain_offset = kChainOffset,
                           u32 data_offset = kDataOffset) {
  std::vector<u8> memory(kMemorySize);
  put_tag(&memory, bucket_offset(chain_offset), DmaTag::Kind::NEXT, 0, data_offset,
          vif(VifCode::Kind::MARK), 0);
  u32 cursor = data_offset;
  auto append = [&](u16 qwc, u32 vif0, u32 vif1, u8 fill = 0) {
    put_tag(&memory, cursor, DmaTag::Kind::CNT, qwc, 0, vif0, vif1, fill);
    cursor += 16 + qwc * 16;
  };
  append(2, 0, vif(VifCode::Kind::DIRECT, 2, 0, true));
  const u64 gif_tag = 1ull | (1ull << 15) | (1ull << 60);
  put_u64(&memory, cursor - 32, gif_tag);
  put_u64(&memory, cursor - 24, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  put_u64(&memory, cursor - 16, 0x130ull | (1ull << 24) | (1ull << 32));
  put_u64(&memory, cursor - 8, static_cast<u64>(GsRegisterAddress::ZBUF_1));
  append(8, vif(VifCode::Kind::STCYCL, 0x404), vif_unpack_v4_32(897, 8));
  append(2, vif(VifCode::Kind::MSCALF, 0, 0, true), vif(VifCode::Kind::STMOD));
  append(0, 0, 0);
  u16 header_address = 837;
  u16 vertex_address = 9;
  for (u32 i = 0; i < fragments; ++i) {
    append(12, 0, vif_unpack_v4_32(header_address, 12));
    const u32 header_payload = cursor - 192;
    put_u32(&memory, header_payload + 64,
            (1u << 14) |
                ((static_cast<u32>(GsPrim::Kind::TRI_FAN) | (1u << 3) | (1u << 4) |
                  (1u << 6))
                 << 15) |
                (3u << 28));
    put_u32(&memory, header_payload + 68,
            (1u << 14) |
                ((static_cast<u32>(GsPrim::Kind::TRI_STRIP) | (1u << 3) | (1u << 4) |
                  (1u << 6))
                 << 15) |
                (3u << 28));
    put_u32(&memory, header_payload + 72,
            static_cast<u32>(GifTag::RegisterDescriptor::ST) |
                (static_cast<u32>(GifTag::RegisterDescriptor::RGBAQ) << 4) |
                (static_cast<u32>(GifTag::RegisterDescriptor::XYZF2) << 8));
    put_u32(&memory, header_payload + 76, 1);
    put_u32(&memory, header_payload + 88, 0x7f);
    put_u32(&memory, header_payload + 92, vertices);
    put_u32(&memory, header_payload + 104, 0x7f);
    AdGifData adgif = {};
    adgif.tex0_data = 0x700ull | (1ull << 14) | (2ull << 26) | (2ull << 30) |
                      (1ull << 34) | (1ull << 61);
    adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
    adgif.tex1_data = (static_cast<u64>(tex1_mxl) << 2) | (1ull << 5) |
                      (static_cast<u64>(tex1_mmin) << 6);
    adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1) |
                      (static_cast<u64>(0x8000u | vertices) << 32);
    adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
    adgif.clamp_data = 0b0101;
    adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
    adgif.alpha_data = (2ull << 2) | (1ull << 6) | (0x80ull << 32);
    adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
    std::memcpy(memory.data() + header_payload + 112, &adgif, sizeof(adgif));
    append(static_cast<u16>(vertices * 3), 0,
           vif_unpack_v4_32(vertex_address, static_cast<u8>(vertices * 3)));
    append(0, 0, vif(VifCode::Kind::MSCAL, 6, 0, true));
    header_address = 1704 - header_address;
    vertex_address += 279;
    if (vertex_address > 567) {
      vertex_address = 9;
    }
  }
  append(0, 0, 0);
  append(10, vif(VifCode::Kind::FLUSHA, 0, 0, true),
         vif(VifCode::Kind::DIRECT, 10, 0, true));
  put_tag(&memory, cursor, DmaTag::Kind::NEXT, 0,
          chain_offset + (metal_renderer::kJak2EffectsBucket + 1) * 16, 0, 0);
  return memory;
}

metal_renderer::Jak2EffectsBucket315Capture capture(const std::vector<u8>& memory,
                                                     u32 chain_offset = kChainOffset) {
  return metal_renderer::capture_jak2_effects_bucket315(memory.data(), memory.size(),
                                                         chain_offset,
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
            city_capture.transfer_count == 8 && city_capture.total_payload_bytes == 352 &&
            city_capture.fragment_count == 0 && city_capture.semantic_fingerprint != 0,
        "the 352-byte source-shaped Lightning envelope is captured before execution");

  auto active = make_chain(2, 4);
  const auto active_capture = capture(active);
  check(active_capture.valid && active_capture.transfer_count == 14 &&
            active_capture.total_payload_bytes == 1120 && active_capture.fragment_count == 2 &&
            active_capture.vertex_count == 8 && active_capture.adgif_count == 2 &&
            active_capture.draw_count == 1 &&
            active_capture.transfers[6].payload_fingerprint != 0,
        "the 3808-byte envelope owns transfer boundaries and payload fingerprints");
  check(!metal_renderer::jak2_effects_bucket315_captures_match(city_capture, active_capture),
        "different live/copy semantic input never compares equal");
  const auto active_copy = capture(active);
  check(metal_renderer::jak2_effects_bucket315_captures_match(active_capture, active_copy),
        "independent equivalent captures compare without retaining packets");
  const auto relocated_capture = capture(make_chain(2, 4, 1, 0, kChainOffset + 0x1000,
                                                     kDataOffset + 0x2000),
                                         kChainOffset + 0x1000);
  check(active_capture.transfers[1].relative_tag_offset !=
            relocated_capture.transfers[1].relative_tag_offset &&
            metal_renderer::jak2_effects_bucket315_captures_match(active_capture,
                                                                    relocated_capture),
        "relocated live and copied envelopes compare by semantics, not tag offsets");
  active[active_capture.transfers[5].relative_tag_offset + bucket_offset() + 16] ^= 1;
  const auto mutated_copy = capture(active);
  check(mutated_copy.valid &&
            mutated_copy.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            !metal_renderer::jak2_effects_bucket315_captures_match(active_capture, mutated_copy),
        "an independently valid copied payload mutation fails live/copy semantic matching");

  const auto large_capture = capture(make_chain(12, 82));
  check(large_capture.valid && large_capture.classification ==
             metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            large_capture.total_payload_bytes == 49888 && large_capture.fragment_count == 12 &&
            large_capture.vertex_count == 984,
        "a large exact source envelope stays within the bounded preflight budget");
  const auto over_budget_capture = capture(make_chain(84, 4));
  check(!over_budget_capture.valid && over_budget_capture.classification ==
             metal_renderer::Jak2EffectsBucket315CaptureClass::Malformed,
        "the 256-transfer telemetry budget rejects an over-budget source envelope");

  const auto title_capture = capture(make_chain(1, 4, 4, 0));
  check(title_capture.valid && title_capture.classification ==
                                    metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            title_capture.fragment_count == 1 && title_capture.vertex_count == 4 &&
            title_capture.adgif_count == 1 && title_capture.draw_count == 1,
        "an active title Lightning fragment accepts source MMAG=1 MMIN=4 MXL=0");

  metal_renderer::Jak2EffectsBucket315Telemetry telemetry;
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, absent_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, city_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, active_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, large_capture);
  metal_renderer::observe_jak2_effects_bucket315_capture(&telemetry, over_budget_capture);
  check(telemetry.captures == 5 && telemetry.valid_captures == 4 &&
            telemetry.absent_captures == 1 && telemetry.lightning_captures == 3 &&
            telemetry.malformed_captures == 1 &&
            telemetry.captured_payload_bytes == 51360 && telemetry.last_payload_bytes == 49888 &&
            telemetry.last_fragment_count == 12 && telemetry.last_vertex_count == 984,
        "numeric telemetry exposes absent, setup-only, active, and bounded capture state");
}

void test_absent_and_rejections() {
  std::vector<u8> absent(kMemorySize);
  put_tag(&absent, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_capture = capture(absent);
  check(absent_capture.valid && !absent_capture.present &&
            absent_capture.classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Absent &&
            absent_capture.total_payload_bytes == 0,
        "the exact empty source slot is captured as absent");

  auto malformed = make_chain(1, 4);
  put_u32(&malformed, kDataOffset + 12, vif(VifCode::Kind::NOP));
  check(capture(malformed).classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "a structurally safe but non-Lightning VIF form stays non-executable");
  check(absent_capture.valid && capture(malformed).valid,
        "Absent and Other remain diagnostic observations, not execution authorization");

  const auto wrong_mmin = capture(make_chain(1, 4, 2, 0));
  check(wrong_mmin.valid &&
            wrong_mmin.classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "an active Lightning fragment with non-source MMIN fails closed");
  const auto wrong_mxl = capture(make_chain(1, 4, 4, 1));
  check(wrong_mxl.valid &&
            wrong_mxl.classification == metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "MMIN=4 Lightning with nonzero MXL fails closed");
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
