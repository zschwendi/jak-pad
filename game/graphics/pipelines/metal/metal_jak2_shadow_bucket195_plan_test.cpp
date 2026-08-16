#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_plan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#include "common/dma/dma.h"

namespace {

constexpr u32 kChainOffset = 0x100;
constexpr u32 kDataOffset = 0x4000;
constexpr std::size_t kMemorySize = 2 << 20;

using CommandKind = metal_renderer::Jak2ShadowBucket195CommandKind;
using Disposition = metal_renderer::Jak2ShadowBucket195PlanDisposition;
using Plan = metal_renderer::Jak2ShadowBucket195Plan;

constexpr u64 kSerializedHashOffset = 14695981039346656037ull;
constexpr u64 kSerializedHashPrime = 1099511628211ull;
constexpr std::size_t kSerializedHeaderBytes = 32;

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

void put_little_u32(std::vector<u8>* bytes, std::size_t offset, u32 value) {
  for (u32 i = 0; i < 4; ++i) {
    (*bytes)[offset + i] = static_cast<u8>(value >> (i * 8));
  }
}

void rewrite_serialized_hash(std::vector<u8>* bytes) {
  u64 hash = kSerializedHashOffset;
  for (std::size_t i = kSerializedHeaderBytes; i < bytes->size(); ++i) {
    hash ^= (*bytes)[i];
    hash *= kSerializedHashPrime;
  }
  for (u32 i = 0; i < 8; ++i) {
    (*bytes)[24 + i] = static_cast<u8>(hash >> (i * 8));
  }
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  put_u64(
      memory, offset,
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32));
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

u32 bucket_offset(u32 chain_offset = kChainOffset) {
  return chain_offset + metal_renderer::kJak2ShadowBucket195PlanBucket * 16;
}

struct Fixture {
  std::vector<u8> memory = std::vector<u8>(kMemorySize);
  u32 chain_offset = kChainOffset;
  u32 cursor = kDataOffset;
  u32 first_index_payload = 0;
  u32 reset_tag = 0;
  u32 default_end_tag = 0;

  explicit Fixture(u32 chain = kChainOffset, u32 data = kDataOffset)
      : chain_offset(chain), cursor(data) {
    put_tag(&memory, bucket_offset(chain_offset), DmaTag::Kind::NEXT, 0, cursor,
            vif(VifCode::Kind::MARK), 0);
  }

  u32 append(const std::vector<u8>& payload,
             u32 vif0,
             u32 vif1,
             DmaTag::Kind kind = DmaTag::Kind::CNT,
             u32 address = 0) {
    check(payload.size() % 16 == 0, "fixture payloads are qword aligned");
    const u32 tag_offset = cursor;
    put_tag(&memory, cursor, kind, static_cast<u16>(payload.size() / 16), address, vif0, vif1);
    if (!payload.empty()) {
      std::memcpy(memory.data() + cursor + 16, payload.data(), payload.size());
    }
    cursor += 16 + static_cast<u32>(payload.size());
    return tag_offset;
  }

  void fixed_prefix(bool initial_direct35) {
    append(std::vector<u8>(208, 0x11), vif(VifCode::Kind::STCYCL, 0, 0x404),
           vif(VifCode::Kind::UNPACK_V4_32, 13, 0x370));
    append(std::vector<u8>(64, 0x22), vif(VifCode::Kind::STCYCL, 0, 0x404),
           vif(VifCode::Kind::UNPACK_V4_32, 4, 0x3ac));
    append(std::vector<u8>(64, 0xff), vif(VifCode::Kind::STCYCL, 0, 0x404),
           vif(VifCode::Kind::UNPACK_V4_32, 4, 0));
    append({}, vif(VifCode::Kind::MSCALF, 0, 10), vif(VifCode::Kind::FLUSHE));
    append({}, 0, 0);
    if (initial_direct35) {
      append(std::vector<u8>(560, 0x44), 0, vif(VifCode::Kind::DIRECT, 0, 35));
    }
  }

  void vertices(u16 address, u8 count, VifCode::Kind first, u8 fill) {
    append(std::vector<u8>(static_cast<std::size_t>(count) * 16, fill), vif(first),
           vif(VifCode::Kind::UNPACK_V4_32, count, address));
  }

  u32 indices(u16 address,
              u16 mscalf,
              const std::vector<std::array<u8, 4>>& records,
              bool top_only_header = false) {
    check(!records.empty() && records.size() <= 255, "fixture index record count is encodable");
    const u32 unpack_count = (static_cast<u32>(records.size()) + 1 + 3) & ~3u;
    std::vector<u8> payload(unpack_count * 4 + 16);
    const u32 header = static_cast<u32>(records.size()) | (top_only_header ? 0x100 : 0);
    std::memcpy(payload.data(), &header, sizeof(header));
    for (std::size_t i = 0; i < records.size(); ++i) {
      std::memcpy(payload.data() + 4 + i * 4, records[i].data(), 4);
    }
    const u32 marker = vif(VifCode::Kind::MSCALF, 0, mscalf);
    std::memcpy(payload.data() + unpack_count * 4 + 12, &marker, sizeof(marker));
    const u8 encoded_count = unpack_count == 256 ? 0 : static_cast<u8>(unpack_count);
    const u32 tag = append(
        payload, 0,
        vif(VifCode::Kind::UNPACK_V4_8, encoded_count, static_cast<u16>(address | (1 << 14))));
    if (first_index_payload == 0) {
      first_index_payload = tag + 16;
    }
    return tag;
  }

  void tail(std::array<u8, 4> color = {0x12, 0x34, 0x56, 0x78}) {
    append(std::vector<u8>(96, 0x66), vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 0, 6));
    std::vector<u8> color_payload(560, 0x77);
    std::memcpy(color_payload.data() + 24, color.data(), color.size());
    append(color_payload, vif(VifCode::Kind::FLUSHA), vif(VifCode::Kind::DIRECT, 0, 35));
    reset_tag = append(std::vector<u8>(128, 0x88), vif(VifCode::Kind::FLUSHA),
                       vif(VifCode::Kind::DIRECT, 0, 8));
    const u32 default_end_offset = cursor + 16;
    append({}, 0, 0, DmaTag::Kind::NEXT, default_end_offset);
    default_end_tag = append(std::vector<u8>(160, 0x99), vif(VifCode::Kind::FLUSHA),
                             vif(VifCode::Kind::DIRECT, 0, 10));
    append({}, 0, 0, DmaTag::Kind::NEXT,
           chain_offset + (metal_renderer::kJak2ShadowBucket195PlanBucket + 1) * 16);
  }
};

std::optional<Plan> plan(const Fixture& fixture,
                         u32 bucket_id = metal_renderer::kJak2ShadowBucket195PlanBucket) {
  return metal_renderer::plan_jak2_shadow_bucket195(fixture.memory.data(), fixture.memory.size(),
                                                    fixture.chain_offset, bucket_id);
}

Fixture normal_fixture(bool initial_direct35 = true,
                       u32 chain_offset = kChainOffset,
                       u32 data_offset = kDataOffset) {
  Fixture fixture(chain_offset, data_offset);
  fixture.fixed_prefix(initial_direct35);
  fixture.vertices(4, 4, VifCode::Kind::FLUSH, 0xff);
  fixture.vertices(174, 4, VifCode::Kind::NOP, 0x7f);
  fixture.indices(344, 2, {{{0, 1, 2, 1}}});
  fixture.indices(600, 4, {{{1, 2, 1, 0xa5}}});
  fixture.indices(344, 6, {{{2, 1, 0, 1}}});
  fixture.vertices(4, 0, VifCode::Kind::FLUSH, 0);
  fixture.vertices(174, 0, VifCode::Kind::NOP, 0);
  fixture.tail();
  return fixture;
}

void test_absent_and_ready_plan() {
  Fixture absent;
  put_tag(&absent.memory, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto no_shadow = plan(absent);
  check(no_shadow && no_shadow->disposition == Disposition::Absent &&
            no_shadow->transfer_count == 1 && no_shadow->batches.empty(),
        "the canonical zero-CNT bucket is absent");

  const auto fixture = normal_fixture();
  const auto ready = plan(fixture);
  check(ready && ready->disposition == Disposition::Ready && ready->batches.size() == 2 &&
            ready->batches[0].top_vertices.size() == 4 &&
            ready->batches[0].bottom_vertices.size() == 4 &&
            ready->batches[0].commands.size() == 3 && ready->batches[1].has_top_upload &&
            ready->batches[1].has_bottom_upload && ready->batches[1].top_vertices.empty() &&
            ready->batches[1].bottom_vertices.empty(),
        "ordinary commands and zero-count terminal top/bottom markers form an owned ready plan");
  check(ready->batches[0].commands[0].kind == CommandKind::Caps &&
            ready->batches[0].commands[1].kind == CommandKind::Walls &&
            ready->batches[0].commands[1].records[0].bytes[3] == 0xa5 &&
            ready->batches[0].commands[2].kind == CommandKind::FlippableCaps,
        "cap, wall, flippable-cap, and unconstrained wall byte 3 remain typed and ordered");
  check(ready->has_initial_direct35 && ready->has_direct6_state && ready->has_color_direct35 &&
            ready->has_reset_display_state && ready->has_default_end_state &&
            ready->direct_transfer_count == 5 && ready->direct_payload_bytes == 1504 &&
            ready->color == std::array<u8, 4>{0x12, 0x34, 0x56, 0x78},
        "the optional setup and exact Direct6/color35/reset8/default-end10 states stay distinct");
  check(
      ready->vertex_count == 8 && ready->record_count == 3 && ready->perspective_matrix[0] == 0xff,
      "source payloads are retained without finite or projection predicates");

  const auto without_initial = plan(normal_fixture(false));
  check(without_initial && without_initial->disposition == Disposition::Ready &&
            !without_initial->has_initial_direct35 && without_initial->direct_transfer_count == 4,
        "the source/OpenGL optional initial Direct35 may be absent");

  const auto relocated = plan(normal_fixture(true, kChainOffset + 0x1000, kDataOffset + 0x8000));
  check(relocated && relocated->record_count == ready->record_count &&
            relocated->vertex_count == ready->vertex_count && relocated->color == ready->color &&
            metal_renderer::jak2_shadow_bucket195_plans_match(*ready, *relocated),
        "independent live/copy parses match semantically across DMA relocation");

  auto changed = *relocated;
  changed.batches[0].top_vertices[0].bytes[0] ^= 1;
  check(!metal_renderer::jak2_shadow_bucket195_plans_match(*ready, changed),
        "a changed owned vertex byte fails live/copy semantic matching");

  changed = *relocated;
  changed.color[2] ^= 1;
  check(!metal_renderer::jak2_shadow_bucket195_plans_match(*ready, changed),
        "a changed owned color byte fails live/copy semantic matching");
}

void test_retained_live_scalar_shape() {
  Fixture fixture;
  fixture.fixed_prefix(true);
  for (u8 batch = 0; batch < 15; ++batch) {
    const u8 vertex_count = batch == 14 ? 0 : 3;
    fixture.vertices(4, vertex_count, batch & 1 ? VifCode::Kind::NOP : VifCode::Kind::FLUSH, batch);
    fixture.vertices(174, vertex_count, batch & 1 ? VifCode::Kind::FLUSH : VifCode::Kind::NOP,
                     static_cast<u8>(batch + 1));
    if (vertex_count != 0) {
      fixture.indices(344, 2, {{{0, 1, 2, 1}}});
      fixture.indices(600, 4, {{{1, 2, static_cast<u8>(batch & 1), 0xa5}}});
      if (batch < 2) {
        fixture.indices(344, 6, {{{2, 1, 0, static_cast<u8>(batch & 1)}}});
      }
    }
  }
  fixture.tail();

  const auto shaped = plan(fixture);
  check(shaped && shaped->disposition == Disposition::Ready && shaped->batches.size() == 15 &&
            shaped->transfer_count == 73 && shaped->direct_transfer_count == 5 &&
            shaped->direct_payload_bytes == 1504 && shaped->v4_32_transfer_count == 33 &&
            shaped->v4_8_transfer_count == 30 && shaped->vertex_count == 84 &&
            shaped->record_count == 30 && shaped->batches.back().top_vertices.empty() &&
            shaped->batches.back().bottom_vertices.empty(),
        "the retained tick-600 transfer, V4_32, V4_8, Direct, and payload scalar shape parses "
        "exactly");
}

void test_retained_payload_envelope() {
  Fixture fixture;
  fixture.fixed_prefix(true);
  fixture.vertices(4, 128, VifCode::Kind::FLUSH, 0x21);
  fixture.vertices(174, 128, VifCode::Kind::NOP, 0x22);
  std::vector<std::array<u8, 4>> records(108, {0, 1, 2, 1});
  fixture.indices(344, 2, records);
  fixture.tail();

  const auto retained_size = plan(fixture);
  check(retained_size && retained_size->disposition == Disposition::Ready &&
            retained_size->payload_bytes == 6400 && retained_size->vertex_count == 256 &&
            retained_size->record_count == 108 && retained_size->direct_payload_bytes == 1504,
        "an exact 6400-byte retained-gameplay-sized source envelope stays fully owned");
}

void test_top_only_is_accepted_deferred() {
  Fixture fixture;
  fixture.fixed_prefix(false);
  fixture.vertices(4, 3, VifCode::Kind::NOP, 0x41);
  fixture.indices(344, 6, {{{0, 1, 2, 1}}}, true);
  fixture.tail();
  const auto deferred = plan(fixture);
  check(deferred && deferred->disposition == Disposition::AcceptedDeferredNoDraw &&
            deferred->batches.size() == 1 && deferred->batches[0].top_only &&
            deferred->batches[0].bottom_vertices.empty() &&
            deferred->batches[0].commands[0].kind == CommandKind::FlippableCaps,
        "source-emittable top-only MSCALF 6 is accepted but cannot authorize a draw");

  Fixture omitted;
  omitted.fixed_prefix(false);
  omitted.vertices(4, 0, VifCode::Kind::FLUSH, 0);
  omitted.tail();
  const auto no_indices = plan(omitted);
  check(no_indices && no_indices->disposition == Disposition::AcceptedDeferredNoDraw &&
            no_indices->batches.size() == 1 && no_indices->batches[0].commands.empty(),
        "an empty V4_8 upload is omitted while a top-only terminal upload remains safely deferred");
}

void test_source_exact_rejections() {
  auto bad_padding = normal_fixture();
  bad_padding.memory[bad_padding.first_index_payload + 8] = 1;
  check(!plan(bad_padding), "nonzero source padding before the terminal marker is rejected");

  Fixture trailing_padding;
  trailing_padding.fixed_prefix(false);
  trailing_padding.vertices(4, 3, VifCode::Kind::FLUSH, 0x31);
  trailing_padding.vertices(174, 3, VifCode::Kind::NOP, 0x32);
  trailing_padding.indices(344, 2, {{{0, 1, 2, 1}}, {{0, 0, 0, 0}}, {{0, 0, 0, 0}}});
  trailing_padding.tail();
  const auto padded = plan(trailing_padding);
  check(padded && padded->record_count == 3 && padded->batches[0].commands[0].records.size() == 1,
        "declared source padding records are skipped only at the command tail");

  Fixture misplaced_padding;
  misplaced_padding.fixed_prefix(false);
  misplaced_padding.vertices(4, 3, VifCode::Kind::FLUSH, 0x31);
  misplaced_padding.vertices(174, 3, VifCode::Kind::NOP, 0x32);
  misplaced_padding.indices(344, 2, {{{0, 1, 2, 1}}, {{0, 0, 0, 0}}, {{2, 1, 0, 1}}});
  misplaced_padding.tail();
  check(!plan(misplaced_padding), "a zero padding record cannot precede an executable record");

  auto flush_index = normal_fixture();
  put_u32(&flush_index.memory, flush_index.first_index_payload - 8, vif(VifCode::Kind::FLUSH));
  check(!plan(flush_index), "source V4_8 transfers use the NOP VIF0 template");

  Fixture reordered;
  reordered.fixed_prefix(false);
  reordered.vertices(4, 3, VifCode::Kind::FLUSH, 0x31);
  reordered.vertices(174, 3, VifCode::Kind::NOP, 0x32);
  reordered.indices(600, 4, {{{0, 1, 0, 0xa5}}});
  reordered.indices(344, 2, {{{0, 1, 2, 1}}});
  reordered.tail();
  check(!plan(reordered), "normal commands must retain the producer's 2/4/6/4 subsequence");

  Fixture mismatched_vertices;
  mismatched_vertices.fixed_prefix(false);
  mismatched_vertices.vertices(4, 3, VifCode::Kind::FLUSH, 0x31);
  mismatched_vertices.vertices(174, 2, VifCode::Kind::NOP, 0x32);
  mismatched_vertices.tail();
  check(!plan(mismatched_vertices), "a normal source batch has matching top and bottom uploads");

  Fixture late_bottom;
  late_bottom.fixed_prefix(false);
  late_bottom.vertices(4, 3, VifCode::Kind::FLUSH, 0x31);
  late_bottom.indices(344, 6, {{{0, 1, 2, 1}}}, true);
  late_bottom.vertices(174, 3, VifCode::Kind::NOP, 0x32);
  late_bottom.tail();
  check(!plan(late_bottom), "a top-only MSCALF 6 cannot acquire a late bottom upload");

  Fixture missing_bottom;
  missing_bottom.fixed_prefix(false);
  missing_bottom.vertices(4, 3, VifCode::Kind::FLUSH, 0x21);
  missing_bottom.indices(344, 2, {{{0, 1, 2, 1}}});
  missing_bottom.tail();
  check(!plan(missing_bottom), "MSCALF 2 cannot consume a missing bottom upload");

  Fixture wrong_top_only_header;
  wrong_top_only_header.fixed_prefix(false);
  wrong_top_only_header.vertices(4, 3, VifCode::Kind::FLUSH, 0x21);
  wrong_top_only_header.indices(344, 6, {{{0, 1, 2, 1}}}, false);
  wrong_top_only_header.tail();
  check(!plan(wrong_top_only_header), "top-only MSCALF 6 requires its source header flag");

  auto fake_nop_tail = normal_fixture();
  put_u32(&fake_nop_tail.memory, fake_nop_tail.reset_tag + 8, 0);
  put_u32(&fake_nop_tail.memory, fake_nop_tail.reset_tag + 12, 0);
  check(!plan(fake_nop_tail), "the reset-display Direct8 cannot be replaced by a NOP boundary");

  auto wrong_bucket = normal_fixture();
  check(!plan(wrong_bucket, 314),
        "the private plan does not broaden execution to sibling bucket 314");
}

void test_bounded_snapshot_rejections() {
  check(!metal_renderer::plan_jak2_shadow_bucket195(nullptr, 0, kChainOffset,
                                                    metal_renderer::kJak2ShadowBucket195PlanBucket),
        "a null snapshot is rejected");

  const auto fixture = normal_fixture();
  check(!metal_renderer::plan_jak2_shadow_bucket195(
            fixture.memory.data(), fixture.default_end_tag + 32, fixture.chain_offset,
            metal_renderer::kJak2ShadowBucket195PlanBucket),
        "a snapshot truncated inside the default-end Direct10 payload is rejected");

  auto loop = normal_fixture();
  put_tag(&loop.memory, bucket_offset(), DmaTag::Kind::NEXT, 0, bucket_offset(), 0, 0);
  check(!plan(loop), "a cyclic DMA chain is rejected within the transfer bound");
}

std::vector<u8> serialize(const Plan& plan) {
  auto bytes = metal_renderer::serialize_jak2_shadow_bucket195_plan(plan);
  check(bytes.has_value(), "a valid owned Shadow2 plan serializes");
  return std::move(*bytes);
}

void check_codec_round_trip(const Plan& plan, const char* message) {
  const auto first = serialize(plan);
  const auto second = serialize(plan);
  const auto decoded =
      metal_renderer::deserialize_jak2_shadow_bucket195_plan(first.data(), first.size());
  check(first == second &&
            first.size() <= metal_renderer::kJak2ShadowBucket195PlanMaximumSerializedBytes &&
            decoded && *decoded == plan &&
            metal_renderer::jak2_shadow_bucket195_plans_match(plan, *decoded),
        message);
}

void test_plan_codec_round_trips() {
  Fixture absent;
  put_tag(&absent.memory, bucket_offset(), DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto absent_plan = plan(absent);
  check(absent_plan.has_value(), "the codec absent fixture parses");
  check_codec_round_trip(*absent_plan, "an absent plan has one deterministic versioned round trip");

  const auto ready_plan = plan(normal_fixture());
  check(ready_plan.has_value(), "the codec ready fixture parses");
  check_codec_round_trip(*ready_plan,
                         "a ready plan retains every fixed field, batch, command, and byte");

  Fixture top_only;
  top_only.fixed_prefix(false);
  top_only.vertices(4, 3, VifCode::Kind::NOP, 0x41);
  top_only.indices(344, 6, {{{0, 1, 2, 1}}}, true);
  top_only.tail();
  const auto deferred_plan = plan(top_only);
  check(deferred_plan.has_value(), "the codec top-only fixture parses");
  check_codec_round_trip(*deferred_plan,
                         "a retained top-only plan round trips without authorizing a draw");

  Fixture retained;
  retained.fixed_prefix(true);
  retained.vertices(4, 128, VifCode::Kind::FLUSH, 0x21);
  retained.vertices(174, 128, VifCode::Kind::NOP, 0x22);
  retained.indices(344, 2, std::vector<std::array<u8, 4>>(108, {0, 1, 2, 1}));
  retained.tail();
  const auto retained_plan = plan(retained);
  check(retained_plan.has_value(), "the codec retained-envelope fixture parses");
  check_codec_round_trip(*retained_plan,
                         "the retained gameplay-sized owned plan remains bounded and exact");
}

void test_plan_codec_rejects_truncation_and_corruption() {
  const auto ready_plan = plan(normal_fixture());
  check(ready_plan.has_value(), "the codec corruption fixture parses");
  const auto bytes = serialize(*ready_plan);

  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(nullptr, bytes.size()),
        "the codec rejects a null input");
  for (std::size_t size = 0; size < bytes.size(); ++size) {
    check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(bytes.data(), size),
          "every truncated serialization is rejected");
  }

  auto trailing = bytes;
  trailing.push_back(0);
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(trailing.data(), trailing.size()),
        "trailing bytes are rejected instead of silently ignored");
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(
            bytes.data(), metal_renderer::kJak2ShadowBucket195PlanMaximumSerializedBytes + 1),
        "an oversized declared input is rejected before it is read");

  for (std::size_t offset = 0; offset < bytes.size(); ++offset) {
    auto corrupted = bytes;
    corrupted[offset] ^= 0x80;
    check(
        !metal_renderer::deserialize_jak2_shadow_bucket195_plan(corrupted.data(), corrupted.size()),
        "every single-byte corruption is detected");
  }

  auto unsupported_version = bytes;
  unsupported_version[8] =
      static_cast<u8>(metal_renderer::kJak2ShadowBucket195PlanSerializationVersion + 1);
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(unsupported_version.data(),
                                                                unsupported_version.size()),
        "an unsupported serialization version is rejected");

  auto invalid_disposition = bytes;
  invalid_disposition[kSerializedHeaderBytes + 4] = 3;
  rewrite_serialized_hash(&invalid_disposition);
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(invalid_disposition.data(),
                                                                invalid_disposition.size()),
        "a checksum-valid invalid disposition is rejected");

  auto invalid_flags = bytes;
  invalid_flags[kSerializedHeaderBytes + 5] |= 0x80;
  rewrite_serialized_hash(&invalid_flags);
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(invalid_flags.data(),
                                                                invalid_flags.size()),
        "checksum-valid reserved plan flags are rejected");

  auto excessive_batches = bytes;
  put_little_u32(&excessive_batches, kSerializedHeaderBytes + 48,
                 metal_renderer::kJak2ShadowBucket195PlanMaximumBatches + 1);
  rewrite_serialized_hash(&excessive_batches);
  check(!metal_renderer::deserialize_jak2_shadow_bucket195_plan(excessive_batches.data(),
                                                                excessive_batches.size()),
        "a checksum-valid excessive batch count is rejected before allocation");
}

void test_plan_codec_rejects_invalid_owned_limits() {
  const auto parsed = plan(normal_fixture());
  check(parsed.has_value(), "the codec limit fixture parses");

  auto invalid = *parsed;
  invalid.bucket_id = 314;
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec cannot serialize a sibling bucket plan");

  invalid = *parsed;
  invalid.transfer_count = metal_renderer::kJak2ShadowBucket195PlanMaximumTransfers + 1;
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects transfer counts above the parser limit");

  invalid = *parsed;
  invalid.payload_bytes = metal_renderer::kJak2ShadowBucket195PlanMaximumPayloadBytes + 1;
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects payload counts above the parser limit");

  invalid = *parsed;
  invalid.batches.resize(metal_renderer::kJak2ShadowBucket195PlanMaximumBatches + 1);
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects batch counts above the parser limit");

  invalid = *parsed;
  invalid.batches[0].top_vertices.resize(metal_renderer::kJak2ShadowBucket195PlanMaximumVertices +
                                         1);
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects owned vertices above the parser limit");

  invalid = *parsed;
  invalid.batches[0].commands[0].records.resize(
      metal_renderer::kJak2ShadowBucket195PlanMaximumRecords + 1);
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects owned records above the parser limit");

  invalid = *parsed;
  invalid.batches[0].commands.resize(metal_renderer::kJak2ShadowBucket195PlanMaximumTransfers + 1);
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects command counts above the transfer limit");

  invalid = *parsed;
  invalid.vertex_count = 0;
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects scalar and owned-graph count disagreement");

  invalid = *parsed;
  invalid.batches[0].commands[0].records[0].bytes[0] = 0xff;
  check(!metal_renderer::serialize_jak2_shadow_bucket195_plan(invalid),
        "the codec rejects an owned record outside its vertex domain");
}

}  // namespace

int main() {
  test_absent_and_ready_plan();
  test_retained_live_scalar_shape();
  test_retained_payload_envelope();
  test_top_only_is_accepted_deferred();
  test_source_exact_rejections();
  test_bounded_snapshot_rejections();
  test_plan_codec_round_trips();
  test_plan_codec_rejects_truncation_and_corruption();
  test_plan_codec_rejects_invalid_owned_limits();
  std::puts("jak2 shadow bucket-195 source-exact plan tests passed");
  return 0;
}
