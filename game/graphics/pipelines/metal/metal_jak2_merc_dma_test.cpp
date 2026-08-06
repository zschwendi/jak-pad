#include "game/graphics/pipelines/metal/metal_jak2_merc_dma.h"

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
constexpr std::size_t kEeSize = 0x4000;

int g_failures = 0;

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
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
  std::memcpy(data->data() + offset, &value, sizeof(value));
}

void append_u64(std::vector<u8>* data, u64 value) {
  const std::size_t offset = data->size();
  data->resize(offset + sizeof(value));
  std::memcpy(data->data() + offset, &value, sizeof(value));
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

std::vector<u8> make_model_packet(const char* name, bool jak1_water_slot = false) {
  std::vector<u8> data(128, 0);
  std::memcpy(data.data(), name, std::strlen(name));
  data.resize(data.size() + 7 * 16, 0);  // lights
  if (jak1_water_slot) {
    data.resize(data.size() + 16, 0);
  }

  const std::size_t matrix_slots = data.size();
  data.resize(data.size() + 128, 0xff);
  data[matrix_slots] = 0;
  data[matrix_slots + 1] = 1;

  append_u32(&data, kBone0);
  data.resize(data.size() + 12, 0);
  append_u32(&data, kBone1);
  data.resize(data.size() + 12, 0);

  append_u64(&data, 1);  // enabled effect 0
  append_u64(&data, 0);  // no ignore-alpha effects
  data.push_back(1);     // effect count
  data.push_back(0);     // flags
  data.resize(data.size() + 14, 0);
  data.resize(data.size() + 16, 0);  // one padded fade
  append_u32(&data, 0x3200);         // one padded effect pointer
  data.resize(data.size() + 12, 0);
  return data;
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
                std::string* error = nullptr) const {
    metal_jak2_merc_dma::Bucket local;
    return metal_jak2_merc_dma::validate_bucket(DmaFollower(memory.data(), kOpening), kBoundary,
                                                memory.size(), bucket ? bucket : &local, error);
  }
};

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    g_failures++;
  }
}

void check_rejected(const Fixture& fixture, const char* message) {
  std::string error;
  check(!fixture.validate(nullptr, &error) && !error.empty(), message);
}

void check_rejected_and_recovered(const Fixture& fixture, const char* message) {
  std::string error;
  metal_jak2_merc_dma::Bucket bucket;
  DmaFollower dma(fixture.memory.data(), kOpening);
  const bool rejected = !metal_jak2_merc_dma::validate_bucket(
      dma, kBoundary, fixture.memory.size(), &bucket, &error);
  const bool recovered = metal_jak2_merc_dma::recover_to_boundary(&dma, kBoundary);
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
  check(metal_jak2_merc_dma::validate_bucket(DmaFollower(memory.data(), 0x100), 0x110,
                                             memory.size(), &bucket, &error) &&
            bucket.empty && bucket.model_count == 0,
        "a strict empty Jak 2 CNT bucket is accepted");
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
    std::memset(fixture.memory.data() + fixture.model0_data + 128 + 7 * 16, 1, 128);
    check_rejected(fixture, "an unterminated matrix-slot string is rejected");
  }
  {
    Fixture fixture;
    u64 tag = 0;
    std::memcpy(&tag, fixture.memory.data() + kModel0, sizeof(tag));
    tag = (tag & ~0xffffull) | ((tag + 1) & 0xffffull);
    put_u64(&fixture.memory, kModel0, tag);
    check_rejected_and_recovered(
        fixture, "a model qwc mismatch is rejected and recovers to the exact boundary");
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

void test_jak1_packet_is_not_reinterpreted() {
  const auto packet = make_model_packet("jak1-water-slot", true);
  DmaTransfer transfer;
  transfer.data = packet.data();
  transfer.size_bytes = static_cast<u32>(packet.size());
  transfer.transferred_tag = static_cast<u64>(metal_jak2_merc_dma::kPcPortVif) << 32;
  metal_jak2_merc_dma::ModelPacket parsed;
  std::string error;
  check(!metal_jak2_merc_dma::parse_model_packet(transfer, kEeSize, &parsed, &error),
        "a Jak 1 water-slot packet is not silently reinterpreted as Jak 2");
}

}  // namespace

int main() {
  test_source_shaped_bucket();
  test_empty_bucket();
  test_malformed_buckets();
  test_jak1_packet_is_not_reinterpreted();
  if (g_failures) {
    std::printf("FAIL: %d Jak 2 Merc DMA checks failed\n", g_failures);
    return 1;
  }
  std::printf("PASS: Jak 2 Merc DMA grammar checks\n");
  return 0;
}
