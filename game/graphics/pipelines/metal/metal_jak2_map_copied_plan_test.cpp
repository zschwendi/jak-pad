#include "game/graphics/pipelines/metal/metal_jak2_map_copied_plan.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {

using namespace metal_renderer;

// Fixture pattern copied from metal_jak2_map_texture_upload_plan_test.cpp, relocated so the chain
// and body chunks live in separate gapped 128 KiB chunks and the EE sources live elsewhere.
constexpr u32 kChunk = 0x20000;
constexpr u32 kChainOffset = 0x80000;  // chunk 4
constexpr u32 kBucketEntry = kChainOffset + 319 * 16;
constexpr u32 kBucketEnd = kBucketEntry + 16;
constexpr u32 kChunkA = 0xc0000;   // chunk 6
constexpr u32 kChunkB = 0x140000;  // chunk 10
constexpr u32 kImage0 = 0x180000;
constexpr u32 kClutA = 0x200000;
constexpr u32 kPage0 = 0x220000;
constexpr u32 kPage1 = 0x220200;
constexpr std::size_t kMemorySize = 0x240000;
constexpr u32 kDirect = 0x50u << 24;
constexpr u32 kPcPort = 0x08u << 24;
constexpr u32 kFlusha = 0x13u << 24;
constexpr u64 kAdGif1 = 0x1000000000008001ull;
constexpr u64 kAdGif3 = 0x3000000000008001ull;
constexpr u64 kAdRegs = 0xeeeeeeeeeeeeeeeeull;
constexpr u64 kTest = 0x33001, kAlpha = 0x44, kClamp = 0x5;
constexpr u64 kTex0 = 0x2000000265320008ull, kTex0Tcc = 0x2000000665320008ull, kTex1 = 0x60;
constexpr u64 kSpriteGif = 0x50ab400000008001ull, kSpriteRegs = 0x53531;

int g_failures = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) {                                                          \
      std::fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); \
      g_failures++;                                                      \
    }                                                                    \
  } while (0)

template <typename T>
void put(std::vector<u8>& m, u32 at, T v) { std::memcpy(m.data() + at, &v, sizeof(v)); }
template <typename T>
T get(const std::vector<u8>& m, u32 at) { T v; std::memcpy(&v, m.data() + at, sizeof(v)); return v; }

struct Fixture {
  std::vector<u8> mem = std::vector<u8>(kMemorySize);
  std::vector<u32> tags;
  u32 cursor = 0;

  void tag(u32 at, u32 id, u16 qwc, u32 addr, u32 vif0, u32 vif1, bool record = true) {
    put<u64>(mem, at, qwc | (u64(id) << 28) | (u64(addr) << 32));
    put<u32>(mem, at + 8, vif0);
    put<u32>(mem, at + 12, vif1);
    if (record) tags.push_back(at);
  }
  u32 cnt(u16 qwc, u32 vif0, u32 vif1) {
    const u32 at = cursor;
    tag(at, 1, qwc, 0, vif0, vif1);
    cursor += 16 + qwc * 16;
    return at + 16;
  }
  void next(u32 addr) { tag(cursor, 2, 0, addr, 0, 0); cursor = addr; }
  void gs(std::initializer_list<std::pair<u64, u64>> regs) {
    const u16 n = static_cast<u16>(regs.size());
    u32 d = cnt(n + 1, 0, kDirect | (n + 1));
    put<u64>(mem, d, n == 1 ? kAdGif1 : kAdGif3);
    put<u64>(mem, d + 8, kAdRegs);
    for (auto& [reg, value] : regs) {
      d += 16;
      put<u64>(mem, d, value);
      put<u64>(mem, d + 8, reg);
    }
  }
  void upload(u32 imm, u32 data, u16 w, u16 h, u32 dest, u8 fmt, u8 force) {
    const u32 d = cnt(1, kPcPort | imm, 0);
    put<u32>(mem, d, data); put<u16>(mem, d + 4, w); put<u16>(mem, d + 6, h);
    put<u32>(mem, d + 8, dest); put<u8>(mem, d + 12, fmt); put<u8>(mem, d + 13, force);
  }
  void sprite(s32 x0, s32 y0, s32 x1, s32 y1) {
    const u32 d = cnt(6, 0, kDirect | 6);
    put<u64>(mem, d, kSpriteGif); put<u64>(mem, d + 8, kSpriteRegs);
    const s32 q[5][4] = {{128, 128, 128, 128}, {0, 0, 0, 0}, {x0 * 16, y0 * 16, 0xfffff0, 0},
                         {8192, 3328, 0, 0}, {x1 * 16, y1 * 16, 0xfffff0, 0}};
    std::memcpy(mem.data() + d + 16, q, sizeof(q));
  }
  void ordinary_groups(u32 at) {
    cursor = at;
    for (u32 page : {kPage0, kPage1}) {
      const u32 q2 = cnt(2, 0, kDirect | 2);
      for (u32 i = 0; i < 32; i++) mem[q2 + i] = u8(0x30 + i);  // arbitrary DIRECT q2 bytes
      const u32 d = cnt(1, kPcPort, 3);
      put<u64>(mem, d, page); put<s64>(mem, d + 8, -1);
      next(cursor + 16);
    }
    const u32 t = cnt(10, kFlusha, kDirect | 10);
    for (u32 i = 0; i < 160; i++) mem[t + i] = u8(0x90 ^ i);  // arbitrary tail bytes
    next(kBucketEnd);
    put<u32>(mem, kPage0 + 4, 0xabcd);
  }
  void build(bool mixed) {
    // Whole-chain start NEXT -> bucket 319 and END at bucket 320 for a copier-equivalent walk.
    tag(kChainOffset, 2, 0, kBucketEntry, 0, 0, false);
    tag(kBucketEnd, 7, 0, 0, 0, 0, false);
    cursor = kBucketEntry;
    if (!mixed) {
      next(kChunkB);
      ordinary_groups(kChunkB);
      return;
    }
    next(kChunkA);
    gs({{0x47, kTest}, {0x42, kAlpha}, {0x08, kClamp}});
    const u32 clut[4] = {kClutA, kClutA, kClutA + 1024, kClutA + 1024};
    for (u32 layer = 0; layer < 4; layer++) {
      cnt(0, kPcPort | 12, 0);
      upload(15, clut[layer], 16, 16, 0, 0x00, 0);
      upload(16, kImage0 + layer * 0x1a000, 512, 208, 8, 0x13, 1);
      cnt(0, kPcPort | 13, 0);
      if (layer == 0) gs({{0x06, kTex0}, {0x14, kTex1}, {0x3f, 1}});
      if (layer == 1) gs({{0x3f, 0}});
      if (layer == 2) gs({{0x06, kTex0Tcc}, {0x14, kTex1}, {0x3f, 1}});
      if (layer == 3) gs({{0x3f, 1}});
      if (layer % 2 == 0) sprite(10, 20, 400, 120); else sprite(10, 120, 400, 220);
    }
    next(cursor + 16);
    next(kChunkB);
    ordinary_groups(kChunkB);
    for (u32 i = 0; i < 4 * 0x1a000; i++) mem[kImage0 + i] = u8((i * 7 + i / 0x1a000 * 31) & 0xff);
    for (u32 i = 0; i < 512; i++) put<u32>(mem, kClutA + 4 * i, 0x80000000u | (i * 0x010203));
  }
};

// Test-local re-implementation of FixedChunkDmaCopier::run (dma_copy.cpp:92-196) for CNT/NEXT/END:
// mark tag, NEXT target, and payload chunks; compact marked chunks ascending; rewrite only the
// 4 bytes at tag+4 of NEXT tags with nonzero ADDR. Not the real copier.
struct Copied {
  std::vector<u8> data;
  std::vector<u32> map;  // source chunk -> dest chunk or UINT32_MAX
  u32 start = 0;
  u32 reloc(u32 off) const { return map.at(off / kChunk) * kChunk + off % kChunk; }
};

Copied copy_chain(const std::vector<u8>& mem, u32 offset) {
  std::vector<bool> mask(mem.size() / kChunk);
  std::vector<std::pair<u32, u32>> fixups;  // tag offset, target
  u32 at = offset;
  for (int guard = 0; guard < 1000; guard++) {
    const u64 t = get<u64>(mem, at);
    const u32 qwc = t & 0xffff, id = (t >> 28) & 7, addr = u32(t >> 32) & 0x7fffffff;
    mask.at(at / kChunk) = true;
    if (id == 2 && addr) { mask.at(addr / kChunk) = true; fixups.push_back({at, addr}); }
    if (qwc) for (u32 c = (at + 16) / kChunk; c <= (at + 16 + qwc * 16 - 1) / kChunk; c++) mask.at(c) = true;
    if (id == 7) break;
    at = id == 1 ? at + 16 + qwc * 16 : addr;
  }
  Copied out;
  u32 n = 0;
  for (bool b : mask) out.map.push_back(b ? n++ : UINT32_MAX);
  out.data.resize(std::size_t(n) * kChunk);
  for (u32 c = 0; c < out.map.size(); c++)
    if (out.map[c] != UINT32_MAX) std::memcpy(out.data.data() + out.map[c] * kChunk, mem.data() + c * kChunk, kChunk);
  for (auto& [tag, target] : fixups) put<u32>(out.data, out.reloc(tag) + 4, out.reloc(target));
  out.start = out.reloc(offset);
  return out;
}

struct Case {
  Fixture f;
  Copied c;
  Jak2MapTextureUploadTypedPlan live;
  explicit Case(bool mixed = true) {
    f.build(mixed);
    auto p = plan_jak2_map_texture_upload_typed(f.mem.data(), kMemorySize, kChainOffset,
                                                f.mem.data(), kMemorySize);
    if (!p) { std::fprintf(stderr, "live plan failed\n"); g_failures++; return; }
    live = *p;
    c = copy_chain(f.mem, kChainOffset);
  }
  u32 cd(std::size_t ordinal) const { return c.reloc(f.tags.at(ordinal)) + 16; }
  u32 ct(std::size_t ordinal) const { return c.reloc(f.tags.at(ordinal)); }
  Jak2MapCopiedParityResult run(std::size_t copied_size = 0, u32 copied_start = UINT32_MAX) const {
    return validate_jak2_map_copied_plan(live, f.mem.data(), kMemorySize, kChainOffset,
                                         c.data.data(), copied_size ? copied_size : c.data.size(),
                                         copied_start == UINT32_MAX ? c.start : copied_start,
                                         f.mem.data(), kMemorySize);
  }
};

void expect(const char* name, const std::function<void(Case&)>& mutate,
            Jak2MapCopiedParityRejection want, std::size_t size = 0) {
  Case k;
  mutate(k);
  const auto r = k.run(size);
  if (r.plan || r.rejection != want) {
    std::fprintf(stderr, "reject %s: plan=%d got=%s want=%s ordinal=%u replan=%s\n", name,
                 r.plan ? 1 : 0, jak2_map_copied_parity_rejection_name(r.rejection),
                 jak2_map_copied_parity_rejection_name(want), r.transfer_ordinal,
                 jak2_map_mixed_rejection_name(r.replan_diagnostic.rejection));
    g_failures++;
  }
}

void test_positive() {
  Case k;
  CHECK(k.live.present && k.live.mixed && k.live.transfer_count == 36);
  // Chunks 4, 6, 10 compact to 0, 1, 2: nonuniform relocation.
  CHECK(k.c.data.size() == 3 * kChunk);
  CHECK(k.c.start == 0);
  CHECK(kChainOffset - k.c.reloc(kChainOffset) != kChunkA - k.c.reloc(kChunkA));
  CHECK(kChunkA - k.c.reloc(kChunkA) != kChunkB - k.c.reloc(kChunkB));
  const auto r = k.run();
  CHECK(r.rejection == Jak2MapCopiedParityRejection::None && r.plan.has_value());
  if (!r.plan) {
    std::fprintf(stderr, "positive: %s ordinal=%u replan=%s\n",
                 jak2_map_copied_parity_rejection_name(r.rejection), r.transfer_ordinal,
                 jak2_map_mixed_rejection_name(r.replan_diagnostic.rejection));
    return;
  }
  const auto& p = *r.plan;
  CHECK(p.layer_count == 4 && p.ordinary_count == 2 && p.op_count == 6);
  CHECK(p.transfer_count == 36 && p.pc_port_marker_count == 18 && p.direct_replay_count == 12);
  for (std::size_t i = 0; i < p.op_count; i++) {
    const auto& op = p.ops[i];
    CHECK(op.kind == k.live.ops[i].kind && op.index == k.live.ops[i].index);
    for (std::size_t m = 0; m < op.marker_count; m++) {
      const u32 ord = op.marker_transfer_ordinals[m];
      CHECK(op.marker_tag_offsets[m] == k.ct(ord));
      CHECK(op.marker_tag_offsets[m] != k.live.ops[i].marker_tag_offsets[m]);
      CHECK(k.live.ops[i].marker_tag_offsets[m] == k.f.tags[ord]);
    }
  }
  for (std::size_t i = 0; i < 4; i++) {
    CHECK(p.layers[i].indices == k.live.layers[i].indices);
    CHECK(p.layers[i].clut == k.live.layers[i].clut);
    CHECK(*p.layers[i].index_source_offset == kImage0 + i * 0x1a000);
    CHECK(*p.layers[i].index_source_offset >= k.c.data.size());  // read from live EE
    // Body pointer bytes are unrelocated in the copied snapshot.
    CHECK(get<u32>(k.c.data, k.cd(4 + 6 * i)) == kImage0 + i * 0x1a000);
  }
  CHECK(p.ordinary[0].page_offset == kPage0 && p.ordinary[1].page_offset == kPage1);
  CHECK(p.ordinary[0].page_header == k.live.ordinary[0].page_header);
  // Cursor consumes the verified plan in order.
  Jak2MapUploadCursor cur(p);
  for (std::size_t i = 0; i < 4; i++) CHECK(cur.consume_animation_layer(i));
  CHECK(cur.consume_ordinary_upload(0) && cur.consume_ordinary_upload(1) && cur.complete());
}

void test_negative() {
  using R = Jak2MapCopiedParityRejection;
  {
    Case k(false);
    const auto r = k.run();
    CHECK(r.rejection == R::LivePlanNotMixed && !r.plan);
  }
  // Copied DMA tag/payload bytes.
  expect("marker imm", [](Case& k) { k.c.data[k.ct(2) + 8] ^= 1; }, R::TagMismatch);
  expect("marker vif1", [](Case& k) { k.c.data[k.ct(2) + 12] ^= 1; }, R::TagMismatch);
  expect("spr bit", [](Case& k) { k.c.data[k.ct(0) + 7] ^= 0x80; }, R::TagMismatch);
  expect("irq bit", [](Case& k) { k.c.data[k.ct(3) + 3] ^= 0x80; }, R::TagMismatch);
  expect("cnt pce bits", [](Case& k) { k.c.data[k.ct(3) + 3] ^= 0x0c; }, R::TagMismatch);
  expect("body format", [](Case& k) { k.c.data[k.cd(4) + 12] ^= 1; }, R::PayloadMismatch);
  expect("body force", [](Case& k) { k.c.data[k.cd(4) + 13] ^= 1; }, R::PayloadMismatch);
  expect("source ptr", [](Case& k) { put<u32>(k.c.data, k.cd(4), kImage0 + 16); }, R::PayloadMismatch);
  expect("width", [](Case& k) { k.c.data[k.cd(4) + 4] ^= 1; }, R::PayloadMismatch);
  expect("height", [](Case& k) { k.c.data[k.cd(4) + 6] ^= 1; }, R::PayloadMismatch);
  expect("dest", [](Case& k) { k.c.data[k.cd(4) + 8] ^= 1; }, R::PayloadMismatch);
  expect("gs payload", [](Case& k) { k.c.data[k.cd(1) + 16] ^= 1; }, R::PayloadMismatch);
  expect("sprite payload", [](Case& k) { k.c.data[k.cd(7) + 40] ^= 1; }, R::PayloadMismatch);
  expect("ordinary q2", [](Case& k) { k.c.data[k.cd(28) + 5] ^= 1; }, R::PayloadMismatch);
  expect("tail", [](Case& k) { k.c.data[k.cd(34) + 100] ^= 1; }, R::PayloadMismatch);
  expect("page ptr", [](Case& k) { k.c.data[k.cd(29)] ^= 0x10; }, R::PayloadMismatch);
  // Live EE mutated after the owned plan was built.
  expect("ee image", [](Case& k) { k.f.mem[kImage0 + 77] ^= 1; }, R::PlanMismatch);
  expect("ee clut", [](Case& k) { k.f.mem[kClutA + 1024 + 3] ^= 1; }, R::PlanMismatch);
  expect("ee page header", [](Case& k) { k.f.mem[kPage1 + 2] ^= 1; }, R::PlanMismatch);
  // Owned plan metadata.
  expect("layer count", [](Case& k) { k.live.layer_count = 3; }, R::PlanMismatch);
  expect("ordinary count", [](Case& k) { k.live.ordinary_count = 1; }, R::PlanMismatch);
  expect("transfer count", [](Case& k) { k.live.transfer_count = 35; }, R::TransferCount);
  expect("marker count", [](Case& k) { k.live.pc_port_marker_count = 17; }, R::PlanMismatch);
  expect("direct count", [](Case& k) { k.live.direct_replay_count = 11; }, R::PlanMismatch);
  expect("op kind", [](Case& k) { k.live.ops[4].kind = Jak2MapUploadOpKind::AnimationLayer; }, R::PlanMismatch);
  expect("op index", [](Case& k) { k.live.ops[1].index = 2; }, R::PlanMismatch);
  expect("op order", [](Case& k) { std::swap(k.live.ops[4], k.live.ops[5]); }, R::PlanMismatch);
  expect("op ordinal", [](Case& k) { k.live.ops[0].marker_transfer_ordinals[0] = 3; }, R::MarkerOffset);
  expect("op marker offset", [](Case& k) { k.live.ops[0].marker_tag_offsets[0] += 16; }, R::MarkerOffset);
  expect("layer source", [](Case& k) { k.live.layers[0].index_source_offset = kImage0 + 1; }, R::PlanMismatch);
  expect("layer bytes", [](Case& k) { k.live.layers[2].indices[9] ^= 1; }, R::PlanMismatch);
  expect("sprite rect", [](Case& k) { k.live.layer_sprites[1].y1 += 1; }, R::PlanMismatch);
  expect("ordinary mode", [](Case& k) { k.live.ordinary[0].mode = 0; }, R::PlanMismatch);
  expect("ordinary page", [](Case& k) { k.live.ordinary[1].page_offset = kPage0; }, R::PlanMismatch);
  // Owned counts past their maxima reject before any traversal.
  expect("op count max+1", [](Case& k) { k.live.op_count = kJak2MapMixedUploadMaximumOps + 1; }, R::OwnedCount);
  expect("op count SIZE_MAX", [](Case& k) { k.live.op_count = SIZE_MAX; }, R::OwnedCount);
  expect("layer count max+1", [](Case& k) { k.live.layer_count = kJak2MapIndexedLayerMaximum + 1; }, R::OwnedCount);
  expect("layer count SIZE_MAX", [](Case& k) { k.live.layer_count = SIZE_MAX; }, R::OwnedCount);
  expect("ordinary count max+1", [](Case& k) { k.live.ordinary_count = kJak2MapTextureUploadMaximumGroups + 1; }, R::OwnedCount);
  expect("ordinary count SIZE_MAX", [](Case& k) { k.live.ordinary_count = SIZE_MAX; }, R::OwnedCount);
  expect("transfer count 65", [](Case& k) { k.live.transfer_count = 65; }, R::OwnedCount);
  expect("transfer count SIZE_MAX", [](Case& k) { k.live.transfer_count = SIZE_MAX; }, R::OwnedCount);
  expect("pc marker count 65", [](Case& k) { k.live.pc_port_marker_count = 65; }, R::OwnedCount);
  expect("direct count SIZE_MAX", [](Case& k) { k.live.direct_replay_count = SIZE_MAX; }, R::OwnedCount);
  // Copied low/invalid embedded pointers fail on raw payload parity before replan dereference.
  constexpr u32 kLow = 512 * 1024 - 1;  // EE_MAIN_MEM_LOW_PROTECT - 1
  expect("copied image ptr 4", [](Case& k) { put<u32>(k.c.data, k.cd(4), 4); }, R::PayloadMismatch);
  expect("copied image ptr low-1", [](Case& k) { put<u32>(k.c.data, k.cd(4), kLow); }, R::PayloadMismatch);
  expect("copied clut ptr 4", [](Case& k) { put<u32>(k.c.data, k.cd(3), 4); }, R::PayloadMismatch);
  expect("copied clut ptr low-1", [](Case& k) { put<u32>(k.c.data, k.cd(3), kLow); }, R::PayloadMismatch);
  expect("copied page ptr 4", [](Case& k) { put<u64>(k.c.data, k.cd(29), 4); }, R::PayloadMismatch);
  expect("copied page ptr low-1", [](Case& k) { put<u64>(k.c.data, k.cd(29), kLow); }, R::PayloadMismatch);
  // Missing marker: PC_PORT marker turned into DIRECT empty.
  expect("missing marker", [](Case& k) { put<u32>(k.c.data, k.ct(5) + 8, 0); }, R::TagMismatch);
  // Extra marker spliced before the bucket end via free copied space.
  expect("extra marker", [](Case& k) {
    const u32 extra = 0x2000;  // free space in copied chain chunk
    put<u64>(k.c.data, extra, (1ull << 28));
    put<u32>(k.c.data, extra + 8, kPcPort | 13);
    put<u32>(k.c.data, extra + 12, 0);
    put<u64>(k.c.data, extra + 16, (2ull << 28) | (u64(k.c.reloc(kBucketEnd)) << 32));
    put<u32>(k.c.data, k.ct(35) + 4, extra);
  }, R::TransferCount);
  // Reordered markers: swap two CNT marker tags in copied (same chunk, both qwc 0).
  expect("reordered", [](Case& k) {
    u8 a[16], b[16];
    std::memcpy(a, &k.c.data[k.ct(2)], 16); std::memcpy(b, &k.c.data[k.ct(5)], 16);
    std::memcpy(&k.c.data[k.ct(2)], b, 16); std::memcpy(&k.c.data[k.ct(5)], a, 16);
  }, R::TagMismatch);
  // Bounds, cycles, bucket end, over-64.
  expect("truncated", [](Case&) {}, R::CopiedWalk, 2 * kChunk + 0x10);
  expect("cycle", [](Case& k) { put<u32>(k.c.data, k.ct(26) + 4, k.ct(26)); }, R::CopiedWalk);
  expect("bad bucket end", [](Case& k) { put<u32>(k.c.data, k.ct(35) + 4, k.c.reloc(kBucketEnd) + 16); }, R::CopiedWalk);
  expect("unaligned", [](Case& k) { put<u32>(k.c.data, k.ct(0) + 4, k.c.reloc(kChunkA) + 4); }, R::CopiedWalk);
  expect("out of range", [](Case& k) { put<u32>(k.c.data, k.ct(0) + 4, 0x7fff0000); }, R::CopiedWalk);
  expect("ref tag", [](Case& k) { k.c.data[k.ct(0) + 3] = (k.c.data[k.ct(0) + 3] & 0x8f) | 0x30; }, R::CopiedWalk);
  expect("over 64", [](Case& k) {
    u32 at = 0x2000;
    put<u32>(k.c.data, k.ct(35) + 4, at);
    for (int i = 0; i < 70; i++, at += 16) put<u64>(k.c.data, at, (1ull << 28));
    put<u64>(k.c.data, at, (2ull << 28) | (u64(k.c.reloc(kBucketEnd)) << 32));
  }, R::CopiedWalk);
  {
    Case k;
    const auto r = k.run(0, k.c.start + 16);
    CHECK(r.rejection == R::CopiedWalk && !r.plan);
    const auto r2 = validate_jak2_map_copied_plan(k.live, k.f.mem.data(), kMemorySize,
                                                  kChainOffset + 16, k.c.data.data(),
                                                  k.c.data.size(), k.c.start, k.f.mem.data(),
                                                  kMemorySize);
    CHECK(r2.rejection == R::LiveWalk && !r2.plan);
  }
}

}  // namespace

int main() {
  test_positive();
  test_negative();
  if (g_failures) {
    std::fprintf(stderr, "%d failures\n", g_failures);
    return 1;
  }
  std::printf("metal_jak2_map_copied_plan_test: all passed\n");
  return 0;
}
