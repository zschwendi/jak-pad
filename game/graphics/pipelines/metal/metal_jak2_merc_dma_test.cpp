#include "game/graphics/pipelines/metal/metal_jak2_merc_dma.h"
#include "game/graphics/pipelines/metal/metal_merc_dma_dialect.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr u32 kOpening = 0x100;
constexpr u32 kBoundary = 0x200;
constexpr u32 kSetup = 0x400;
constexpr u32 kGsSetup = kSetup + 16 + 10 * 16;
constexpr u32 kSetupPatch = kGsSetup + 16 + 3 * 16;
constexpr u32 kModel0 = 0x800;
constexpr u32 kModel1 = 0x1200;
constexpr u32 kTerminal = 0x1800;
constexpr u32 kBone0 = 0x3000;
constexpr u32 kBone1 = 0x3080;
constexpr u32 kEffect0 = 0x3200;
constexpr std::size_t kEeSize = 0x4000;

int g_failures = 0;

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

void put_u32(std::vector<u8>* memory, std::size_t offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, std::size_t offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void append_u32(std::vector<u8>* data, u32 value) {
  const std::size_t offset = data->size();
  data->resize(offset + sizeof(value));
  put_u32(data, offset, value);
}

void append_u64(std::vector<u8>* data, u64 value) {
  const std::size_t offset = data->size();
  data->resize(offset + sizeof(value));
  put_u64(data, offset, value);
}

std::vector<u8> make_setup() {
  std::vector<u8> data(10 * 16, 0);
  put_u32(&data, 0, vif(VifCode::Kind::BASE, 442));
  put_u32(&data, 4, vif(VifCode::Kind::OFFSET, static_cast<u16>(-442)));
  put_u32(&data, 8, 0);
  put_u32(&data, 12, vif(VifCode::Kind::UNPACK_V4_32, 0, 8));
  put_u32(&data, 9 * 16, vif(VifCode::Kind::FLUSHE));
  put_u32(&data, 9 * 16 + 12, vif(VifCode::Kind::MSCAL));
  return data;
}

struct ModelOptions {
  bool jak1_water_slot = false;
  u16 matrix_count = 2;
  bool terminate_matrix_slots = true;
  u8 effect_count = 1;
  u8 bit_flags = 0;
  u64 enable_mask = 1;
  u64 ignore_alpha_mask = 0;
  u32 effect_pointer = kEffect0;
};

std::vector<u8> make_model_packet(const char* name, const ModelOptions& options = {}) {
  std::vector<u8> data(128, 0);
  std::memcpy(data.data(), name, std::strlen(name));
  data.resize(data.size() + 7 * 16, 0);  // lights
  if (options.jak1_water_slot) {
    data.resize(data.size() + 16, 0);
  }

  const std::size_t matrix_slots = data.size();
  data.resize(data.size() + 128, 0xff);
  for (u16 i = 0; i < options.matrix_count; i++) {
    data[matrix_slots + i] = static_cast<u8>(i);
  }
  if (!options.terminate_matrix_slots) {
    for (u16 i = options.matrix_count; i < 128; i++) {
      data[matrix_slots + i] = static_cast<u8>(i);
    }
  }

  for (u16 i = 0; i < options.matrix_count; i++) {
    append_u32(&data, i == 1 ? kBone1 : kBone0);
    data.resize(data.size() + 12, 0);
  }

  append_u64(&data, options.enable_mask);
  append_u64(&data, options.ignore_alpha_mask);
  data.push_back(options.effect_count);
  data.push_back(options.bit_flags);
  data.resize(data.size() + 14, 0);
  if (options.bit_flags & 4) {
    data.resize(data.size() + metal_jak2_merc_dma::kBlercBytes, 0);
  }

  const std::size_t effect_quadwords = (options.effect_count + 3) / 4;
  data.resize(data.size() + effect_quadwords * 16, 0);  // padded fades
  const std::size_t pointers_offset = data.size();
  data.resize(data.size() + effect_quadwords * 16, 0);
  for (u8 i = 0; i < options.effect_count; i++) {
    put_u32(&data, pointers_offset + i * 4, options.effect_pointer);
  }
  return data;
}

bool parse_packet(const std::vector<u8>& packet,
                  metal_jak2_merc_dma::ModelPacket* parsed = nullptr,
                  std::string* error = nullptr) {
  metal_jak2_merc_dma::ModelPacket local;
  return metal_jak2_merc_dma::parse_model_packet(
      packet.data(), packet.size(), 0, metal_jak2_merc_dma::kPcPortVif, kEeSize,
      parsed ? parsed : &local, error);
}

struct Fixture {
  std::vector<u8> memory = std::vector<u8>(kEeSize, 0);
  u32 model0_patch = 0;
  u32 model1_patch = 0;
  u32 model0_data = kModel0 + 16;

  Fixture() {
    put_tag(&memory, kOpening, DmaTag::Kind::NEXT, 0, kSetup, 0, 0);

    const auto setup = make_setup();
    put_tag(&memory, kSetup, DmaTag::Kind::CNT, 10, 0, vif(VifCode::Kind::STCYCL, 0x404),
            vif(VifCode::Kind::STMOD));
    std::memcpy(memory.data() + kSetup + 16, setup.data(), setup.size());

    put_tag(&memory, kGsSetup, DmaTag::Kind::CNT, 3, 0, 0,
            metal_jak2_merc_dma::kDirect3Vif);
    put_tag(&memory, kSetupPatch, DmaTag::Kind::NEXT, 0, kModel0, 0, 0);

    model0_patch = put_model(kModel0, "jak2-merc-a", kModel1);
    model1_patch = put_model(kModel1, "jak2-merc-b", kTerminal);
    put_tag(&memory, kTerminal, DmaTag::Kind::NEXT, 0, kBoundary, 0, 0);
  }

  u32 put_model(u32 offset, const char* name, u32 next) {
    const auto packet = make_model_packet(name);
    put_tag(&memory, offset, DmaTag::Kind::CNT, static_cast<u16>(packet.size() / 16), 0, 0,
            metal_jak2_merc_dma::kPcPortVif);
    std::memcpy(memory.data() + offset + 16, packet.data(), packet.size());
    const u32 patch = offset + 16 + static_cast<u32>(packet.size());
    put_tag(&memory, patch, DmaTag::Kind::NEXT, 0, next, 0, 0);
    return patch;
  }

  bool validate(metal_jak2_merc_dma::Bucket* bucket = nullptr,
                std::string* error = nullptr,
                std::size_t copy_size = kEeSize,
                u32 start = kOpening,
                u32 boundary = kBoundary) const {
    metal_jak2_merc_dma::Bucket local;
    return metal_jak2_merc_dma::validate_bucket(memory.data(), copy_size, start, boundary,
                                                memory.size(), bucket ? bucket : &local, error);
  }
};

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    g_failures++;
  }
}

void check_rejected(const Fixture& fixture,
                    const char* message,
                    std::size_t copy_size = kEeSize,
                    u32 start = kOpening,
                    u32 boundary = kBoundary) {
  std::string error;
  check(!fixture.validate(nullptr, &error, copy_size, start, boundary) && !error.empty(), message);
}

void check_packet_rejected(const std::vector<u8>& packet, const char* message) {
  std::string error;
  check(!parse_packet(packet, nullptr, &error) && !error.empty(), message);
}

void check_rejected_and_recovered(const Fixture& fixture, const char* message) {
  std::string error;
  metal_jak2_merc_dma::Bucket bucket;
  DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
  const bool rejected = !metal_jak2_merc_dma::validate_bucket(
      fixture.memory.data(), fixture.memory.size(), kOpening, kBoundary, fixture.memory.size(),
      &bucket, &error);
  const bool recovered = metal_jak2_merc_dma::recover_to_boundary(
      &dma, fixture.memory.data(), fixture.memory.size(), kBoundary);
  check(rejected && !error.empty() && recovered && dma.current_tag_offset() == kBoundary,
        message);
}

void test_source_shaped_bucket() {
  Fixture fixture;
  metal_jak2_merc_dma::Bucket bucket;
  std::string error;
  check(fixture.validate(&bucket, &error), "the source-shaped two-model bucket is accepted");
  check(!bucket.empty && bucket.model_count == 2 && bucket.models.size() == 2,
        "the source-shaped bucket retains both model packets");
  check(bucket.models[0].name == "jak2-merc-a" && bucket.models[1].name == "jak2-merc-b" &&
            bucket.models[0].matrix_count == 2 && bucket.models[0].effect_count == 1,
        "the exact names, matrices, and effect count are decoded");
  check(bucket.models[0].effect_pointers_offset + 16 ==
            400 + 16 * bucket.models[0].matrix_count + 32,
        "the packet uses Jak 2's 400-byte fixed payload formula");
}

void test_empty_bucket() {
  std::vector<u8> memory(0x200, 0);
  put_tag(&memory, 0x100, DmaTag::Kind::CNT, 0, 0, 0, 0);
  metal_jak2_merc_dma::Bucket bucket;
  std::string error;
  check(metal_jak2_merc_dma::validate_bucket(memory.data(), memory.size(), 0x100, 0x110,
                                             memory.size(), &bucket, &error) &&
            bucket.empty && bucket.model_count == 0,
        "a strict empty Jak 2 CNT bucket is accepted");
}

void test_bounded_chain_rejections() {
  {
    Fixture fixture;
    check_rejected(fixture, "an unaligned bucket start is rejected before reading its tag",
                   fixture.memory.size(), kOpening + 1);
  }
  {
    Fixture fixture;
    check_rejected(fixture, "an unaligned next-bucket boundary is rejected before recovery",
                   fixture.memory.size(), kOpening, kBoundary + 1);
    DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
    check(!metal_jak2_merc_dma::recover_to_boundary(
              &dma, fixture.memory.data(), fixture.memory.size(), kBoundary + 1) &&
              dma.current_tag_offset() == kOpening,
          "direct recovery refuses an unaligned boundary without moving the follower");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kOpening, DmaTag::Kind::NEXT, 0, kSetup + 1, 0, 0);
    check_rejected(fixture, "an unaligned opening NEXT target is rejected before following");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model0_patch, DmaTag::Kind::NEXT, 0, kModel1 + 1, 0, 0);
    check_rejected(fixture, "an unaligned model NEXT target is rejected before following");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kOpening, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size()), 0, 0);
    check_rejected(fixture, "an out-of-copy opening NEXT target is rejected before following");
  }
  {
    Fixture fixture;
    check_rejected(fixture, "a truncated opening tag is rejected before reading its header",
                   fixture.memory.size(), static_cast<u32>(fixture.memory.size() - 8));
  }
  {
    Fixture fixture;
    check_rejected(fixture, "a truncated boundary tag makes recovery unavailable", kBoundary + 8);
    DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
    check(!metal_jak2_merc_dma::recover_to_boundary(&dma, fixture.memory.data(), kBoundary + 8,
                                                    kBoundary),
          "direct recovery refuses a boundary header outside the compacted copy");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kBoundary, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size()), 0, 0);
    DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
    const bool recovered = metal_jak2_merc_dma::recover_to_boundary(
        &dma, fixture.memory.data(), fixture.memory.size(), kBoundary);
    bool rejected = false;
    try {
      dma.read_and_advance();
    } catch (const std::exception&) {
      rejected = true;
    }
    check(recovered && rejected && dma.current_tag_offset() == kBoundary,
          "recovery preserves the compacted-copy bound for the next bucket");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kOpening, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size() - 8), 0, 0);
    check_rejected(fixture, "a truncated setup target header is rejected before following");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kSetup, DmaTag::Kind::CNT, 0xffff, 0,
            vif(VifCode::Kind::STCYCL, 0x404), vif(VifCode::Kind::STMOD));
    check_rejected(fixture, "an out-of-copy setup payload is rejected before parsing");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kSetupPatch, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size()), 0, 0);
    check_rejected(fixture, "an out-of-copy setup patch target is rejected before following");
  }
  {
    Fixture fixture;
    check_rejected(fixture, "a compacted copy truncated inside the model payload is rejected",
                   kModel0 + 32);
  }
  {
    Fixture fixture;
    u64 tag = 0;
    std::memcpy(&tag, fixture.memory.data() + kModel0, sizeof(tag));
    tag = (tag & ~0xffffull) | 0xffffull;
    put_u64(&fixture.memory, kModel0, tag);
    check_rejected_and_recovered(
        fixture, "an out-of-copy model qwc is rejected and directly recovers to the boundary");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model0_patch, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size()), 0, 0);
    check_rejected(fixture, "an out-of-copy model patch target is rejected before following");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kTerminal, DmaTag::Kind::NEXT, 0,
            static_cast<u32>(fixture.memory.size()), 0, 0);
    check_rejected(fixture, "an out-of-copy terminal target is rejected before following");
  }
}

void test_model_packet_edges() {
  {
    ModelOptions options;
    options.matrix_count = 128;
    options.terminate_matrix_slots = false;
    const auto packet = make_model_packet("all-128-matrices", options);
    metal_jak2_merc_dma::ModelPacket parsed;
    check(parse_packet(packet, &parsed) && parsed.matrix_count == 128,
          "all 128 unique matrix slots are accepted without a sentinel");
  }
  {
    auto packet = make_model_packet("duplicate-slot");
    constexpr std::size_t kSlots = 128 + 7 * 16;
    packet[kSlots + 1] = packet[kSlots];
    check_packet_rejected(packet, "duplicate matrix slots are rejected");
  }
  {
    auto packet = make_model_packet("high-slot");
    constexpr std::size_t kSlots = 128 + 7 * 16;
    packet[kSlots] = 128;
    check_packet_rejected(packet, "matrix slots above 127 are rejected");
  }
  {
    ModelOptions options;
    options.enable_mask = 2;
    check_packet_rejected(make_model_packet("wide-enable-mask", options),
                          "enable-mask bits beyond effect count are rejected");
  }
  {
    ModelOptions options;
    options.ignore_alpha_mask = 2;
    check_packet_rejected(make_model_packet("wide-ignore-mask", options),
                          "ignore-alpha-mask bits beyond effect count are rejected");
  }
  {
    ModelOptions options;
    options.bit_flags = 0x10;
    check_packet_rejected(make_model_packet("unknown-flag", options),
                          "unknown Jak 2 model flag bits are rejected");
  }
  {
    ModelOptions options;
    options.bit_flags = 0x5;
    check_packet_rejected(make_model_packet("two-update-modes", options),
                          "update-verts and pc-blerc cannot both be selected");
  }
  {
    ModelOptions options;
    options.bit_flags = 4;
    metal_jak2_merc_dma::ModelPacket parsed;
    check(parse_packet(make_model_packet("exact-blerc", options), &parsed) &&
              parsed.fades_offset == parsed.blerc_offset + metal_jak2_merc_dma::kBlercBytes,
          "the exact 160-byte blerc block is accepted when flag 4 is present");

    auto truncated = make_model_packet("truncated-blerc", options);
    truncated.resize(truncated.size() - 16);
    check_packet_rejected(truncated, "a short blerc-bearing packet is rejected by exact size");
  }
  {
    ModelOptions options;
    options.bit_flags = 1;
    auto packet = make_model_packet("mod-effect", options);
    metal_jak2_merc_dma::ModelPacket parsed;
    check(parse_packet(packet, &parsed), "an in-range modified-effect pointer is accepted");
    put_u32(&packet, parsed.effect_pointers_offset, 0);
    check_packet_rejected(packet, "a null modified-effect pointer is rejected");
    put_u32(&packet, parsed.effect_pointers_offset, static_cast<u32>(kEeSize - 8));
    check_packet_rejected(packet, "a truncated modified-effect object is rejected");
  }
}

void test_malformed_buckets() {
  {
    Fixture fixture;
    put_tag(&fixture.memory, kOpening, DmaTag::Kind::CNT, 1, 0, 0, 0);
    check_rejected(fixture, "a non-source-shaped opening is rejected");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kGsSetup, DmaTag::Kind::CNT, 2, 0, 0, 0);
    check_rejected(fixture, "Jak 1's old 32-byte GS setup is rejected by Jak 2");
  }
  {
    Fixture fixture;
    put_u32(&fixture.memory, kModel0 + 12, 0);
    check_rejected(fixture, "a model without the exact PC_PORT VIF is rejected");
  }
  {
    Fixture fixture;
    std::memset(fixture.memory.data() + fixture.model0_data, 'x', 128);
    check_rejected(fixture, "an unterminated 128-byte model name is rejected");
  }
  {
    Fixture fixture;
    constexpr u32 kSlotsOffset = 128 + 7 * 16;
    std::memset(fixture.memory.data() + fixture.model0_data + kSlotsOffset, 1, 128);
    check_rejected(fixture, "128 non-unique unsentinelized matrix slots are rejected");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model0_patch, DmaTag::Kind::CNT, 0, 0, 0, 0);
    check_rejected(fixture, "a non-NEXT model patch is rejected");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model1_patch, DmaTag::Kind::NEXT, 0, kBoundary, 0, 0);
    check_rejected(fixture, "a final model patch cannot replace the terminal boundary NEXT");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model0_patch, DmaTag::Kind::NEXT, 0, kModel0, 0, 0);
    check_rejected(fixture, "a self-referential model link is rejected");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, fixture.model1_patch, DmaTag::Kind::NEXT, 0, kModel0, 0, 0);
    check_rejected(fixture, "a two-model NEXT cycle is rejected");
  }
  {
    Fixture fixture;
    put_u32(&fixture.memory, fixture.model0_data + 128 + 7 * 16 + 128,
            static_cast<u32>(fixture.memory.size() - 100));
    check_rejected(fixture, "an out-of-range EE matrix pointer is rejected transactionally");
  }
  {
    Fixture fixture;
    constexpr u32 kFlagsOffset = 128 + 7 * 16 + 128 + 2 * 16;
    fixture.memory[fixture.model0_data + kFlagsOffset + 16] = 64;
    check_rejected(fixture, "an effect count of 64 is rejected");
  }
  {
    Fixture fixture;
    put_tag(&fixture.memory, kTerminal, DmaTag::Kind::NEXT, 0, 0x1a00, 0, 0);
    put_tag(&fixture.memory, 0x1a00, DmaTag::Kind::CNT, 0, 0, 0, 0);
    check_rejected(fixture, "a terminal NEXT that misses the exact boundary is rejected");
  }
}

void test_preflight_transaction() {
  {
    Fixture fixture;
    u64 tag = 0;
    std::memcpy(&tag, fixture.memory.data() + kModel1, sizeof(tag));
    tag = (tag & ~0xffffull) | 0xffffull;
    put_u64(&fixture.memory, kModel1, tag);

    DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
    int malformed = 0;
    int model_checks = 0;
    auto outcome = metal_jak2_merc_dma::preflight_bucket(
        &dma, fixture.memory.data(), fixture.memory.size(), kOpening, kBoundary,
        fixture.memory.size(), &malformed,
        [&](const metal_jak2_merc_dma::ModelPacket&, std::string*) {
          model_checks++;
          return true;
        });

    int handle_all_dma_calls = 0;
    int draw_publications = 0;
    if (outcome.should_render()) {
      handle_all_dma_calls++;
      draw_publications++;
    }
    check(outcome.action == metal_jak2_merc_dma::PreflightAction::SkipMalformed &&
              !outcome.should_render() && outcome.recovered &&
              dma.current_tag_offset() == kBoundary && malformed == 1 && model_checks == 0 &&
              outcome.packet.model_count == 0 && outcome.packet.models.empty() &&
              handle_all_dma_calls == 0 && draw_publications == 0,
          "malformed preflight resets output, reports once, directly recovers, and cannot publish");
  }
  {
    Fixture fixture;
    DmaFollower dma(fixture.memory.data(), kOpening, fixture.memory.size());
    int malformed = 0;
    int model_checks = 0;
    auto outcome = metal_jak2_merc_dma::preflight_bucket(
        &dma, fixture.memory.data(), fixture.memory.size(), kOpening, kBoundary,
        fixture.memory.size(), &malformed,
        [&](const metal_jak2_merc_dma::ModelPacket&, std::string* error) {
          model_checks++;
          *error = "the packet effect count to match its loaded Merc model";
          return false;
        });
    check(!outcome.should_render() && outcome.recovered &&
              dma.current_tag_offset() == kBoundary && malformed == 1 && model_checks == 1 &&
              outcome.packet.models.empty(),
          "a loaded-model mismatch is also transactionally reset and directly recovered");
  }
}

void test_jak1_dialect_is_preserved() {
  constexpr auto jak1 = metal_merc_dma::dialect(GameVersion::Jak1);
  constexpr auto jak2 = metal_merc_dma::dialect(GameVersion::Jak2);
  static_assert(jak1.gs_setup_bytes == 32 && jak1.model_patch_count == 2 &&
                jak1.water_slot_bytes == 16 && jak1.matrix_slots_offset() == 256);
  static_assert(jak2.gs_setup_bytes == 48 && jak2.model_patch_count == 1 &&
                jak2.water_slot_bytes == 0 && jak2.matrix_slots_offset() == 240);
  check(jak1.gs_setup_bytes == 32 && jak1.model_patch_count == 2 &&
            jak1.water_slot_bytes == 16 && jak2.gs_setup_bytes == 48 &&
            jak2.model_patch_count == 1 && jak2.water_slot_bytes == 0,
        "the production Merc dialect preserves Jak 1 GS, link, and water-slot behavior");

  ModelOptions options;
  options.jak1_water_slot = true;
  const auto packet = make_model_packet("jak1-water-slot", options);
  metal_jak2_merc_dma::ModelPacket parsed;
  std::string error;
  check(!parse_packet(packet, &parsed, &error),
        "a Jak 1 water-slot packet is not silently reinterpreted as Jak 2");
}

}  // namespace

int main() {
  test_source_shaped_bucket();
  test_empty_bucket();
  test_bounded_chain_rejections();
  test_model_packet_edges();
  test_malformed_buckets();
  test_preflight_transaction();
  test_jak1_dialect_is_preserved();
  if (g_failures) {
    std::printf("FAIL: %d Jak 2 Merc DMA checks failed\n", g_failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Merc DMA grammar checks\n");
  return 0;
}
