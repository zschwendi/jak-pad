#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kMemorySize = 0x20000;
constexpr u32 kDataOffset = 0x4000;
// The source-bound envelope carries about 2.6 MB of payload.
constexpr u32 kMaximumEnvelopeMemorySize = 4 << 20;

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

struct ChainShape {
  std::vector<u32> fragment_vertices;
  u32 tex1_mmin = 1;
  u32 tex1_mxl = 0;
  u32 chain_offset = kChainOffset;
  u32 data_offset = kDataOffset;
  u32 memory_size = kMemorySize;
  // Nonzero restarts the vertex unpack address every N fragments, unlike the source chain.
  u32 vertex_address_reset_period = 0;
  u16 mscal_immediate = 6;
};

std::vector<u8> make_chain(const ChainShape& shape) {
  const u32 chain_offset = shape.chain_offset;
  const u32 data_offset = shape.data_offset;
  const u32 tex1_mmin = shape.tex1_mmin;
  const u32 tex1_mxl = shape.tex1_mxl;
  std::vector<u8> memory(shape.memory_size);
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
  for (u32 i = 0; i < shape.fragment_vertices.size(); ++i) {
    const u32 vertices = shape.fragment_vertices[i];
    if (shape.vertex_address_reset_period != 0 && i % shape.vertex_address_reset_period == 0) {
      vertex_address = 9;
    }
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
    append(0, 0, vif(VifCode::Kind::MSCAL, shape.mscal_immediate, 0, true));
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

std::vector<u8> make_chain(u32 fragments,
                           u32 vertices,
                           u32 tex1_mmin = 1,
                           u32 tex1_mxl = 0,
                           u32 chain_offset = kChainOffset,
                           u32 data_offset = kDataOffset,
                           u32 memory_size = kMemorySize) {
  ChainShape shape;
  shape.memory_size = memory_size;
  shape.fragment_vertices.assign(fragments, vertices);
  shape.tex1_mmin = tex1_mmin;
  shape.tex1_mxl = tex1_mxl;
  shape.chain_offset = chain_offset;
  shape.data_offset = data_offset;
  return make_chain(shape);
}

// Source maximum: 128 controls, 2 passes each, and points-to-draw 96 split into 82/82/32-vertex
// chunks. The unpack addresses continue across controls exactly as the producer emits them.
ChainShape source_maximum_shape(u32 extra_fragments = 0) {
  ChainShape shape;
  shape.memory_size = kMaximumEnvelopeMemorySize;
  for (u32 pass = 0; pass < 128 * 2; ++pass) {
    shape.fragment_vertices.insert(shape.fragment_vertices.end(), {82, 82, 32});
  }
  for (u32 i = 0; i < extra_fragments; ++i) {
    shape.fragment_vertices.push_back(4);
  }
  return shape;
}

// Test-only by-value helper; production keeps both captures and the scratch in host storage.
metal_renderer::Jak2EffectsBucket315Capture capture(const std::vector<u8>& memory,
                                                     u32 chain_offset = kChainOffset,
                                                     u32 bucket_id =
                                                         metal_renderer::kJak2EffectsBucket) {
  static auto scratch = std::make_unique<metal_renderer::Jak2EffectsBucket315CaptureScratch>();
  auto result = std::make_unique<metal_renderer::Jak2EffectsBucket315Capture>();
  metal_renderer::capture_jak2_effects_bucket315(memory.data(), memory.size(), chain_offset,
                                                 bucket_id, result.get(), scratch.get());
  return *result;
}

void test_source_maximum_envelope() {
  auto maximum = make_chain(source_maximum_shape());
  const auto maximum_capture = capture(maximum);
  check(maximum_capture.valid &&
            maximum_capture.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            maximum_capture.transfer_count == 2312 && maximum_capture.fragment_count == 768 &&
            maximum_capture.vertex_count == 50176 && maximum_capture.adgif_count == 768 &&
            maximum_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::None,
        "the source-maximum 128x2x(82,82,32) Lightning envelope is accepted");
  check(metal_renderer::kJak2EffectsBucket315MaximumTransfers == 2312,
        "the transfer budget is the source-derived 8 + 3 * 768");
  check(metal_renderer::jak2_effects_bucket315_captures_match(maximum_capture,
                                                              capture(maximum)),
        "independent source-maximum captures compare equal");
  const u32 last_vertices =
      bucket_offset() + maximum_capture.transfers[2312 - 5].relative_tag_offset + 16;
  check(maximum_capture.transfers[2312 - 5].payload_bytes == 32 * 48,
        "the fixture mutates the last 32-vertex payload");
  maximum[last_vertices] ^= 1;
  const auto mutated = capture(maximum);
  check(mutated.valid &&
            mutated.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Lightning &&
            !metal_renderer::jak2_effects_bucket315_captures_match(maximum_capture, mutated),
        "a payload mutation in the last source-maximum fragment fails live/copy matching");

  const auto one_past = capture(make_chain(source_maximum_shape(1)));
  check(!one_past.valid &&
            one_past.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Malformed &&
            one_past.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::TransferBudget &&
            one_past.transfer_count == 2312,
        "769 fragments, one past the source maximum, is a transfer-budget rejection");

  // Six-chunk controls realign with the three-slot unpack cycle, so use two-chunk passes.
  ChainShape reset_shape;
  reset_shape.fragment_vertices.assign(8, 82);
  reset_shape.vertex_address_reset_period = 4;
  const auto per_control_reset = capture(make_chain(reset_shape));
  check(per_control_reset.valid &&
            per_control_reset.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "restarting the vertex unpack address per control stays non-executable");
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
  const auto over_budget_capture =
      capture(make_chain(770, 4, 1, 0, kChainOffset, kDataOffset, kMaximumEnvelopeMemorySize));
  check(!over_budget_capture.valid && over_budget_capture.classification ==
             metal_renderer::Jak2EffectsBucket315CaptureClass::Malformed,
        "the 2312-transfer source budget rejects an over-budget envelope");
  check(over_budget_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::TransferBudget &&
            over_budget_capture.transfer_count ==
                metal_renderer::kJak2EffectsBucket315MaximumTransfers,
        "an over-budget envelope reports the transfer budget, not a DMA structure fault");
  const auto boundary_capture =
      capture(make_chain(768, 4, 1, 0, kChainOffset, kDataOffset, kMaximumEnvelopeMemorySize));
  check(boundary_capture.valid && boundary_capture.transfer_count == 2312 &&
            boundary_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::None,
        "768 fragments is the largest exact envelope inside the 2312-transfer budget");
  const auto first_over_capture =
      capture(make_chain(769, 4, 1, 0, kChainOffset, kDataOffset, kMaximumEnvelopeMemorySize));
  check(!first_over_capture.valid &&
            first_over_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::TransferBudget,
        "769 fragments, 2315 transfers, is the first budget rejection");
  auto bad_tag = make_chain(1, 4);
  const auto bad_tag_reference = capture(bad_tag);
  const u32 third_tag = bucket_offset() + bad_tag_reference.transfers[3].relative_tag_offset;
  bad_tag[third_tag + 3] = static_cast<u8>((bad_tag[third_tag + 3] & 0x8f) | 0x70);
  const auto bad_tag_capture = capture(bad_tag);
  check(!bad_tag_capture.valid &&
            bad_tag_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::DmaRead &&
            bad_tag_capture.transfer_count == 3 &&
            bad_tag_capture.malformed_relative_tag_offset ==
                bad_tag_reference.transfers[3].relative_tag_offset,
        "an unsupported tag kind reports DmaRead with the rejected tag index and offset");

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
            telemetry.last_fragment_count == 12 && telemetry.last_vertex_count == 984 &&
            telemetry.last_malformed_reason ==
                static_cast<u8>(metal_renderer::Jak2EffectsBucket315MalformedReason::TransferBudget) &&
            telemetry.last_malformed_transfer_count == 2312,
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
  const auto wrong_bucket =
      capture(malformed, kChainOffset, metal_renderer::kJak2EffectsBucket + 1);
  check(!wrong_bucket.valid && wrong_bucket.malformed_reason ==
                                   metal_renderer::Jak2EffectsBucket315MalformedReason::InvalidBucket,
        "the passive seam rejects every other bucket ID");

  ChainShape wrong_mscal;
  wrong_mscal.fragment_vertices = {4, 4};
  wrong_mscal.mscal_immediate = 4;
  const auto wrong_mscal_capture = capture(make_chain(wrong_mscal));
  check(wrong_mscal_capture.valid &&
            wrong_mscal_capture.classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Other,
        "a non-source MSCAL entry stays non-executable");

  auto cycle = make_chain(1, 4);
  const auto cycle_reference = capture(cycle);
  const u32 final_tag =
      bucket_offset() +
      cycle_reference.transfers[cycle_reference.transfer_count - 1].relative_tag_offset;
  put_tag(&cycle, final_tag, DmaTag::Kind::NEXT, 0, kDataOffset, 0, 0);
  const auto cycle_capture = capture(cycle);
  check(!cycle_capture.valid &&
            cycle_capture.malformed_reason ==
                metal_renderer::Jak2EffectsBucket315MalformedReason::DmaRead &&
            cycle_capture.transfer_count == cycle_reference.transfer_count,
        "a chain that revisits a tag fails closed as a DMA read fault");

  auto no_scratch = std::make_unique<metal_renderer::Jak2EffectsBucket315Capture>();
  metal_renderer::capture_jak2_effects_bucket315(cycle.data(), cycle.size(), kChainOffset,
                                                 metal_renderer::kJak2EffectsBucket,
                                                 no_scratch.get(), nullptr);
  check(!no_scratch->valid &&
            no_scratch->classification ==
                metal_renderer::Jak2EffectsBucket315CaptureClass::Malformed,
        "a capture without caller scratch fails closed");
}

}  // namespace

int main() {
  test_source_maximum_envelope();
  test_capture_and_telemetry();
  test_absent_and_rejections();
  std::puts("jak2 effects bucket-315 capture tests passed");
  return 0;
}
