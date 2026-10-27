#include "game/graphics/pipelines/metal/metal_jak2_map_texture_upload_plan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "common/dma/dma.h"

namespace {

using namespace metal_renderer;

// Authored from the 36-row bucket-319 capture pattern and bigmap.gc/bigmap-h.gc/dma-buffer.gc.
constexpr u32 kChainOffset = 0x100;
constexpr u32 kBucketEntry = kChainOffset + 319 * 16;
constexpr u32 kBucketEnd = kBucketEntry + 16;
constexpr u32 kChunkA = 0x2000;
constexpr u32 kChunkB = 0x4000;
constexpr u32 kPage0 = 0x90000;
constexpr u32 kPage1 = 0x90200;
constexpr u32 kImage0 = 0xa0000;
constexpr u32 kClutA = 0x120000;
constexpr std::size_t kMemorySize = 0x180000;
constexpr u32 kDirect = 0x50u << 24;
constexpr u32 kPcPort = 0x08u << 24;
constexpr u32 kFlusha = 0x13u << 24;

// Hand-computed GS values (not shared with the implementation).
constexpr u64 kAdGif1 = 0x1000000000008001ull;
constexpr u64 kAdGif3 = 0x3000000000008001ull;
constexpr u64 kAdRegs = 0xeeeeeeeeeeeeeeeeull;  // dma-buffer.gc:337, sixteen A+D
constexpr u64 kTest = 0x33001, kAlpha = 0x44, kClamp = 0x5;
constexpr u64 kTex0 = 0x2000000265320008ull, kTex0Tcc = 0x2000000665320008ull, kTex1 = 0x60;
constexpr u64 kSpriteGif = 0x50ab400000008001ull, kSpriteRegs = 0x53531;

int g_failures = 0;
#define CHECK(c)                                                  \
  do {                                                            \
    if (!(c)) {                                                   \
      std::fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); \
      g_failures++;                                               \
    }                                                             \
  } while (0)

struct Fixture {
  std::vector<u8> mem = std::vector<u8>(kMemorySize);
  std::vector<u32> tags;  // tag offset per ordinal
  u32 cursor = 0;

  template <typename T>
  void put(u32 at, T v) { std::memcpy(mem.data() + at, &v, sizeof(v)); }
  template <typename T>
  T get(u32 at) const { T v; std::memcpy(&v, mem.data() + at, sizeof(v)); return v; }

  void tag(u32 at, u32 id, u16 qwc, u32 addr, u32 vif0, u32 vif1) {
    put<u64>(at, qwc | (u64(id) << 28) | (u64(addr) << 32));
    put<u32>(at + 8, vif0);
    put<u32>(at + 12, vif1);
    tags.push_back(at);
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
    put<u64>(d, n == 1 ? kAdGif1 : kAdGif3);
    put<u64>(d + 8, kAdRegs);
    for (auto& [reg, value] : regs) {
      d += 16;
      put<u64>(d, value);
      put<u64>(d + 8, reg);
    }
  }
  void upload(u32 imm, u32 data, u16 w, u16 h, u32 dest, u8 fmt, u8 force) {
    const u32 d = cnt(1, kPcPort | imm, 0);
    put<u32>(d, data); put<u16>(d + 4, w); put<u16>(d + 6, h);
    put<u32>(d + 8, dest); put<u8>(d + 12, fmt); put<u8>(d + 13, force);
  }
  void sprite(s32 x0, s32 y0, s32 x1, s32 y1) {
    const u32 d = cnt(6, 0, kDirect | 6);
    put<u64>(d, kSpriteGif); put<u64>(d + 8, kSpriteRegs);
    const s32 q[5][4] = {{128, 128, 128, 128}, {0, 0, 0, 0}, {x0 * 16, y0 * 16, 0xfffff0, 0},
                         {8192, 3328, 0, 0}, {x1 * 16, y1 * 16, 0xfffff0, 0}};
    std::memcpy(mem.data() + d + 16, q, sizeof(q));
  }
  void ordinary_groups(u32 at) {
    cursor = at;
    for (u32 page : {kPage0, kPage1}) {
      cnt(2, 0, kDirect | 2);
      const u32 d = cnt(1, kPcPort, 3);
      put<u64>(d, page); put<s64>(d + 8, -1);
      next(cursor + 16);
    }
    cnt(10, kFlusha, kDirect | 10);
    next(kBucketEnd);
    put<u32>(kPage0 + 4, 0xabcd);
  }

  void build_mixed() {
    cursor = kBucketEntry;
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
    for (u32 i = 0; i < 512; i++) put<u32>(kClutA + 4 * i, 0x80000000u | (i * 0x010203));
  }
  // bigmap.gc:710-825 icon chunk between the two NEXT links (Build 261 39-transfer form).
  void adgif(u64 tex0) {
    const u32 d = cnt(6, 0, kDirect | 6);
    put<u64>(d, 0x1000000000008005ull); put<u64>(d + 8, 0xe);
    const u64 w[5][2] = {{tex0, 0x06}, {0x60, 0x14}, {0, 0x34}, {0x5, 0x08}, {0x44, 0x42}};
    std::memcpy(mem.data() + d + 16, w, sizeof(w));
  }
  void icon(s32 cx, s32 cy, s32 x, s32 y) {
    const u32 d = cnt(6, 0, kDirect | 6);
    put<u64>(d, kSpriteGif); put<u64>(d + 8, kSpriteRegs);
    const s32 u = 320 * cx + 8, v = 320 * cy + 8;
    const s32 q[5][4] = {{255, 64, 0, 128}, {u, v, 0, 0}, {x * 16, y * 16, 0xffffff, 0},
                         {u + 312, v + 312, 0, 0}, {x * 16 + 333, y * 16 + 333, 0xffffff, 0}};
    std::memcpy(mem.data() + d + 16, q, sizeof(q));
  }
  void target_draw() {
    const u32 d = cnt(10, 0, kDirect | 10);
    put<u64>(d, 0x90aa400000008001ull); put<u64>(d + 8, 0x535353531ull);
    const s32 q[9][4] = {{0, 255, 255, 128}, {0, 0, 0, 0},     {28001, 30003, 0xffffff, 0},
                         {256, 0, 0, 0},     {28113, 29891, 0xffffff, 0}, {0, 256, 0, 0},
                         {27889, 30115, 0xffffff, 0}, {256, 256, 0, 0}, {28001, 30227, 0xffffff, 0}};
    std::memcpy(mem.data() + d + 16, q, sizeof(q));
  }
  static constexpr u64 kIconTex0 = 0x100 | (4ull << 14) | (6ull << 26) | (6ull << 30) | (1ull << 34) | (1ull << 61);
  void build_icons(int sprites) {
    build_mixed();
    tags.resize(27);  // keep through the first transition NEXT; rebuild the rest.
    cursor = tags[26] + 16;
    adgif(kIconTex0);
    for (int i = 0; i < sprites; i++) icon(i % 3, 1, 100 + 20 * i, 200);
    adgif(kIconTex0);
    target_draw();
    next(kChunkB);
    ordinary_groups(kChunkB);
  }
  void build_legacy() {
    cursor = kBucketEntry;
    next(kChunkB);
    ordinary_groups(kChunkB);
  }
  // Payload byte offset for transfer ordinal.
  u32 data(std::size_t ordinal) const { return tags.at(ordinal) + 16; }

  std::optional<Jak2MapTextureUploadTypedPlan> plan(Jak2MapMixedDiagnostic* d = nullptr,
                                                    std::size_t size = kMemorySize) const {
    return plan_jak2_map_texture_upload_typed(mem.data(), size, kChainOffset, mem.data(), size, d);
  }
};

void expect_reject(const char* name,
                   const std::function<void(Fixture&)>& mutate,
                   Jak2MapMixedRejection expected) {
  Fixture f;
  f.build_mixed();
  mutate(f);
  Jak2MapMixedDiagnostic d;
  const auto plan = f.plan(&d);
  if (plan || d.rejection != expected) {
    std::fprintf(stderr, "reject %s: present=%d got=%s want=%s ordinal=%u\n", name, plan ? 1 : 0,
                 jak2_map_mixed_rejection_name(d.rejection),
                 jak2_map_mixed_rejection_name(expected), d.transfer_ordinal);
    g_failures++;
  }
}

void test_mixed_positive_and_ownership() {
  Fixture f;
  f.build_mixed();
  CHECK(f.tags.size() == 36);
  Jak2MapMixedDiagnostic d;
  const auto plan = f.plan(&d);
  CHECK(plan.has_value());
  if (!plan) {
    std::fprintf(stderr, "mixed rejected: %s ordinal=%u\n",
                 jak2_map_mixed_rejection_name(d.rejection), d.transfer_ordinal);
    return;
  }
  CHECK(plan->present && plan->mixed);
  CHECK(plan->layer_count == 4 && plan->ordinary_count == 2 && plan->op_count == 6);
  CHECK(plan->transfer_count == 36);
  CHECK(plan->pc_port_marker_count == 18);
  CHECK(plan->direct_replay_count == 12);
  for (std::size_t i = 0; i < 4; i++) {
    const auto& op = plan->ops[i];
    CHECK(op.kind == Jak2MapUploadOpKind::AnimationLayer && op.index == i && op.marker_count == 4);
    CHECK(op.marker_transfer_ordinals[0] == 2 + 6 * i && op.marker_transfer_ordinals[3] == 5 + 6 * i);
    CHECK(op.marker_tag_offsets[1] == f.tags[3 + 6 * i]);
    const auto& l = plan->layers[i];
    CHECK(l.width == 512 && l.height == 208 && l.indices.size() == 512 * 208);
    CHECK(*l.index_source_offset == kImage0 + i * 0x1a000);
    CHECK(*l.clut_source_offset == kClutA + (i >= 2 ? 1024 : 0));
    CHECK(std::memcmp(l.indices.data(), f.mem.data() + kImage0 + i * 0x1a000, 512 * 208) == 0);
    CHECK(std::memcmp(l.clut.data(), f.mem.data() + *l.clut_source_offset, 1024) == 0);
  }
  CHECK(plan->layers[0].indices != plan->layers[1].indices);
  CHECK(plan->layers[0].clut != plan->layers[2].clut);
  CHECK(plan->layer_sprites[0].y1 == plan->layer_sprites[1].y0);
  CHECK(plan->ops[4].kind == Jak2MapUploadOpKind::OrdinaryUpload && plan->ops[4].index == 0);
  CHECK(plan->ops[4].marker_transfer_ordinals[0] == 29 && plan->ops[5].marker_transfer_ordinals[0] == 32);
  CHECK(plan->ops[5].marker_tag_offsets[0] == f.tags[32]);
  CHECK(plan->ordinary[0].page_offset == kPage0 && plan->ordinary[1].page_offset == kPage1);

  // Ownership: mutate EE/snapshot after planning.
  const auto copy = *plan;
  std::memset(f.mem.data() + kImage0, 0x5a, 4 * 0x1a000);
  std::memset(f.mem.data() + kClutA, 0xa5, 2048);
  f.put<u32>(kPage0 + 4, 0);
  for (std::size_t i = 0; i < 4; i++) {
    CHECK(plan->layers[i].indices == copy.layers[i].indices);
    CHECK(plan->layers[i].clut == copy.layers[i].clut);
    CHECK(plan->layers[i].indices[1] != 0x5a || plan->layers[i].indices[2] != 0x5a);
  }
  u32 name = 0;
  std::memcpy(&name, plan->ordinary[0].page_header.data() + 4, 4);
  CHECK(name == 0xabcd);
}

void test_empty_and_legacy() {
  Fixture f;
  f.tag(kBucketEntry, 1, 0, 0, 0, 0);
  const auto empty = f.plan();
  CHECK(empty.has_value() && !empty->present && !empty->mixed && empty->op_count == 0);

  Fixture l;
  l.build_legacy();
  const auto legacy = l.plan();
  CHECK(legacy.has_value() && legacy->present && !legacy->mixed);
  if (legacy) {
    CHECK(legacy->ordinary_count == 2 && legacy->layer_count == 0 && legacy->op_count == 2);
    CHECK(legacy->ops[1].kind == Jak2MapUploadOpKind::OrdinaryUpload && legacy->ops[1].index == 1);
  }
}

void test_mutations() {
  using R = Jak2MapMixedRejection;
  expect_reject("setup gif regs only low nibbles", [](Fixture& f) { f.put<u64>(f.data(1) + 8, 0xeee); }, R::GsSetup);
  expect_reject("setup test value", [](Fixture& f) { f.put<u64>(f.data(1) + 16, 0x33000); }, R::GsSetup);
  expect_reject("setup register address", [](Fixture& f) { f.put<u64>(f.data(1) + 40, 0x43); }, R::GsSetup);
  expect_reject("setup vif mismatch", [](Fixture& f) { f.put<u32>(f.tags[1] + 12, kDirect | 5); }, R::Legacy);
  expect_reject("layer tex0", [](Fixture& f) { f.put<u64>(f.data(6) + 16, kTex0Tcc); }, R::LayerGsState);
  expect_reject("layer3 tex0 missing tcc", [](Fixture& f) { f.put<u64>(f.data(18) + 16, kTex0); }, R::LayerGsState);
  expect_reject("layer gif nreg", [](Fixture& f) { f.put<u64>(f.data(12), kAdGif3); }, R::LayerGsState);
  expect_reject("layer texflush value", [](Fixture& f) { f.put<u64>(f.data(12) + 16, 1); }, R::LayerGsState);
  expect_reject("sprite gif", [](Fixture& f) { f.put<u64>(f.data(7), kSpriteGif ^ 1); }, R::LayerSprite);
  expect_reject("sprite uv", [](Fixture& f) { f.put<s32>(f.data(7) + 64, 4096); }, R::LayerSprite);
  expect_reject("sprite z", [](Fixture& f) { f.put<s32>(f.data(13) + 56, 0); }, R::LayerSprite);
  expect_reject("sprite subpixel x", [](Fixture& f) { f.put<s32>(f.data(13) + 48, 168); }, R::LayerSprite);
  expect_reject("sprite vif", [](Fixture& f) { f.put<u32>(f.tags[7] + 12, kDirect | 7); }, R::LayerSprite);
  expect_reject("sprite geometry", [](Fixture& f) { f.put<s32>(f.data(19) + 52, 32 * 16); }, R::LayerGeometry);
  expect_reject("clut/generic reordered", [](Fixture& f) {
    f.put<u32>(f.tags[3] + 8, kPcPort | 16); f.put<u32>(f.tags[4] + 8, kPcPort | 15); }, R::LayerMarker);
  expect_reject("missing finish", [](Fixture& f) { f.put<u32>(f.tags[11] + 8, kPcPort | 12); }, R::LayerMarker);
  expect_reject("missing start", [](Fixture& f) { f.put<u32>(f.tags[14] + 8, kPcPort | 13); }, R::LayerMarker);
  expect_reject("marker qwc", [](Fixture& f) { f.put<u16>(f.tags[9], 2); }, R::LayerMarker);
  expect_reject("marker vif1", [](Fixture& f) { f.put<u32>(f.tags[10] + 12, 3); }, R::LayerMarker);
  expect_reject("image width", [](Fixture& f) { f.put<u16>(f.data(4) + 4, 256); }, R::LayerUploadRecord);
  expect_reject("image height", [](Fixture& f) { f.put<u16>(f.data(10) + 6, 256); }, R::LayerUploadRecord);
  expect_reject("image format", [](Fixture& f) { f.put<u8>(f.data(4) + 12, 0x14); }, R::LayerUploadRecord);
  expect_reject("image force", [](Fixture& f) { f.put<u8>(f.data(4) + 13, 0); }, R::LayerUploadRecord);
  expect_reject("image dest", [](Fixture& f) { f.put<u32>(f.data(4) + 8, 9); }, R::LayerUploadRecord);
  expect_reject("clut dims", [](Fixture& f) { f.put<u16>(f.data(3) + 4, 8); }, R::LayerUploadRecord);
  expect_reject("clut dest", [](Fixture& f) { f.put<u32>(f.data(3) + 8, 1); }, R::LayerUploadRecord);
  expect_reject("clut format", [](Fixture& f) { f.put<u8>(f.data(3) + 12, 0x13); }, R::LayerUploadRecord);
  expect_reject("clut force", [](Fixture& f) { f.put<u8>(f.data(3) + 13, 1); }, R::LayerUploadRecord);
  expect_reject("image out of range", [](Fixture& f) { f.put<u32>(f.data(22), kMemorySize - 100); }, R::LayerSourceRange);
  expect_reject("image overflow", [](Fixture& f) { f.put<u32>(f.data(4), 0xfffffff0u); }, R::LayerSourceRange);
  expect_reject("image protected EE range", [](Fixture& f) { f.put<u32>(f.data(4), 0x10000); }, R::LayerSourceRange);
  expect_reject("clut protected EE range", [](Fixture& f) { f.put<u32>(f.data(3), 0x10000); }, R::LayerSourceRange);
  expect_reject("clut out of range", [](Fixture& f) { f.put<u32>(f.data(3), kMemorySize - 1000); }, R::LayerSourceRange);
  expect_reject("image relation", [](Fixture& f) { f.put<u32>(f.data(10), kImage0 + 0x1a010); }, R::LayerSourceRelation);
  expect_reject("clut relation", [](Fixture& f) { f.put<u32>(f.data(21), kClutA); }, R::LayerSourceRelation);
  expect_reject("fifth layer", [](Fixture& f) {
    f.put<u64>(f.tags[26], 0x10000000ull); f.put<u32>(f.tags[26] + 8, kPcPort | 12); }, R::LayerCount);
  expect_reject("three layers", [](Fixture& f) {
    f.put<u64>(f.tags[20], 0x20000000ull | (u64(f.tags[21]) << 32)); f.put<u64>(f.tags[20] + 8, 0); }, R::LayerCount);
  expect_reject("transition not inert", [](Fixture& f) { f.put<u32>(f.tags[27] + 12, 1); }, R::Transition);
  expect_reject("cycle", [](Fixture& f) { f.put<u32>(f.tags[27] + 4, kChunkA); }, R::Chain);
  expect_reject("misaligned link", [](Fixture& f) { f.put<u32>(f.tags[27] + 4, kChunkB + 8); }, R::Chain);
  expect_reject("ordinary mode", [](Fixture& f) { f.put<s64>(f.data(29) + 8, 0); }, R::OrdinaryContents);
  expect_reject("ordinary page range", [](Fixture& f) { f.put<u64>(f.data(32), kMemorySize - 8); }, R::OrdinaryContents);
  expect_reject("ordinary protected EE range", [](Fixture& f) { f.put<u64>(f.data(29), 0x10000); }, R::OrdinaryContents);
  expect_reject("ordinary descriptor vif", [](Fixture& f) { f.put<u32>(f.tags[29] + 12, 4); }, R::OrdinaryGroup);
  expect_reject("missing tail", [](Fixture& f) {
    f.put<u64>(f.tags[34], 0x20000000ull | (u64(f.tags[35]) << 32)); f.put<u64>(f.tags[34] + 8, 0); }, R::Tail);
  expect_reject("tail vif0", [](Fixture& f) { f.put<u32>(f.tags[34] + 8, 0); }, R::Tail);
  expect_reject("wrong final link", [](Fixture& f) {
    f.put<u32>(f.tags[35] + 4, 0x6000); f.put<u64>(0x6000, 0x20000000ull | (u64(kBucketEnd) << 32)); }, R::BucketEnd);

  Fixture t;
  t.build_mixed();
  Jak2MapMixedDiagnostic d;
  // Snapshot truncated inside chunk B; live EE intact.
  CHECK(!plan_jak2_map_texture_upload_typed(t.mem.data(), kChunkB + 20, kChainOffset, t.mem.data(),
                                            kMemorySize, &d)
              .has_value() &&
        d.rejection == R::Chain);
}

void expect_icon_reject(const char* name, int sprites,
                        const std::function<void(Fixture&)>& mutate, Jak2MapMixedRejection expected) {
  Fixture f;
  f.build_icons(sprites);
  mutate(f);
  Jak2MapMixedDiagnostic d;
  const auto plan = f.plan(&d);
  if (plan || d.rejection != expected) {
    std::fprintf(stderr, "icon reject %s: present=%d got=%s want=%s ordinal=%u\n", name,
                 plan ? 1 : 0, jak2_map_mixed_rejection_name(d.rejection),
                 jak2_map_mixed_rejection_name(expected), d.transfer_ordinal);
    g_failures++;
  }
}

void test_icon_chunk() {
  for (int sprites : {0, 1, 5, 25}) {
    Fixture f;
    f.build_icons(sprites);
    const std::size_t n = 39 + sprites;
    CHECK(f.tags.size() == n);
    Jak2MapMixedDiagnostic d;
    const auto plan = f.plan(&d);
    CHECK(plan.has_value());
    if (!plan) {
      std::fprintf(stderr, "icons(%d) rejected: %s ordinal=%u\n", sprites,
                   jak2_map_mixed_rejection_name(d.rejection), d.transfer_ordinal);
      continue;
    }
    CHECK(plan->transfer_count == n && plan->pc_port_marker_count == 18);
    CHECK(plan->direct_replay_count == 15 + std::size_t(sprites));
    CHECK(plan->op_count == 6 && plan->layer_count == 4 && plan->ordinary_count == 2);
    CHECK(plan->ops[4].marker_transfer_ordinals[0] == 32 + sprites);
    CHECK(plan->ops[5].marker_transfer_ordinals[0] == 35 + sprites);
    CHECK(plan->ops[5].marker_tag_offsets[0] == f.tags[35 + sprites]);
  }
  using R = Jak2MapMixedRejection;
  expect_icon_reject("65-transfer icon chain", 26, [](Fixture&) {}, R::Chain);
  // Observed 39 ordinals: 27 icon ADGIF, 28 target ADGIF, 29 target draw, 30 NEXT.
  expect_icon_reject("adgif nloop", 0, [](Fixture& f) { f.put<u8>(f.data(27), 4); }, R::IconShape);
  expect_icon_reject("adgif nreg", 0, [](Fixture& f) { f.put<u64>(f.data(27), 0x2000000000008005ull); }, R::IconShape);
  expect_icon_reject("adgif regs", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 8, 0xee); }, R::IconAdgif);
  expect_icon_reject("adgif address", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 24, 0x07); }, R::IconAdgif);
  expect_icon_reject("adgif order", 0, [](Fixture& f) {
    f.put<u64>(f.data(27) + 72, 0x42); f.put<u64>(f.data(27) + 88, 0x08); }, R::IconAdgif);
  expect_icon_reject("adgif clamp", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 64, 0x0); }, R::IconAdgif);
  expect_icon_reject("adgif alpha", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 80, 0x48); }, R::IconAdgif);
  expect_icon_reject("tex0 tfx", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 16, Fixture::kIconTex0 | (1ull << 35)); }, R::IconAdgif);
  expect_icon_reject("tex0 psm", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 16, Fixture::kIconTex0 | (0x3ull << 20)); }, R::IconAdgif);
  expect_icon_reject("tex0 tw", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 16, Fixture::kIconTex0 | (0xfull << 26)); }, R::IconAdgif);
  expect_icon_reject("tex1 reserved", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 32, 0x60 | (1ull << 12)); }, R::IconAdgif);
  expect_icon_reject("tex1 lcm not written", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 32, 0x61); }, R::IconAdgif);
  expect_icon_reject("tex1 mtba not written", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 32, 0x60 | (1ull << 9)); }, R::IconAdgif);
  expect_icon_reject("tex1 k not written", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 32, 0x60 | (1ull << 32)); }, R::IconAdgif);
  expect_icon_reject("miptbp reserved", 0, [](Fixture& f) { f.put<u64>(f.data(27) + 48, 1ull << 62); }, R::IconAdgif);
  expect_icon_reject("target prim", 0, [](Fixture& f) { f.put<u64>(f.data(29), 0x90a6000000008001ull); }, R::IconTarget);
  expect_icon_reject("wrong untextured target prim", 0, [](Fixture& f) { f.put<u64>(f.data(29), 0x90a6400000008001ull); }, R::IconTarget);
  expect_icon_reject("target regs", 0, [](Fixture& f) { f.put<u64>(f.data(29) + 8, 0x535353533ull); }, R::IconTarget);
  expect_icon_reject("target uv", 0, [](Fixture& f) { f.put<s32>(f.data(29) + 64, 255); }, R::IconTarget);
  expect_icon_reject("target z", 0, [](Fixture& f) { f.put<s32>(f.data(29) + 56, 0xfffff0); }, R::IconTarget);
  expect_icon_reject("target w", 0, [](Fixture& f) { f.put<s32>(f.data(29) + 60, 1); }, R::IconTarget);
  expect_icon_reject("target color", 0, [](Fixture& f) { f.put<s32>(f.data(29) + 16, 1); }, R::IconTarget);
  expect_icon_reject("target xy range", 0, [](Fixture& f) { f.put<s32>(f.data(29) + 48, -1); }, R::IconTarget);
  expect_icon_reject("unknown direct q6", 0, [](Fixture& f) { f.put<u64>(f.data(27), 0x1000000000008003ull); }, R::IconShape);
  expect_icon_reject("unknown direct q8", 0, [](Fixture& f) {
    f.put<u32>(f.tags[27] + 12, kDirect | 8); }, R::IconShape);
  expect_icon_reject("unpaired target adgif", 0, [](Fixture& f) {
    f.put<u64>(f.data(29), 0x1000000000008005ull); }, R::IconTarget);
  expect_icon_reject("stray icon without adgif", 1, [](Fixture& f) {
    f.put<u64>(f.data(27), 0x50ab400000008001ull); f.put<u64>(f.data(27) + 8, 0x53531); }, R::IconOrder);
  expect_icon_reject("sprite uv cell", 1, [](Fixture& f) { f.put<s32>(f.data(28) + 32, 9); }, R::IconSprite);
  expect_icon_reject("sprite uv span", 1, [](Fixture& f) { f.put<s32>(f.data(28) + 64, 8 + 311); }, R::IconSprite);
  expect_icon_reject("sprite alpha", 1, [](Fixture& f) { f.put<s32>(f.data(28) + 28, 127); }, R::IconSprite);
  expect_icon_reject("sprite z", 1, [](Fixture& f) { f.put<s32>(f.data(28) + 56, 0xfffff0); }, R::IconSprite);
  expect_icon_reject("sprite xy0 align", 1, [](Fixture& f) { f.put<s32>(f.data(28) + 48, 1601); }, R::IconSprite);
  expect_icon_reject("item after target", 0, [](Fixture& f) {
    f.put<u64>(f.tags[30], 0x10000000ull | (6ull)); f.put<u32>(f.tags[30] + 12, kDirect | 6);
    f.put<u64>(f.data(30), 0x1000000000008005ull); }, R::IconOrder);
  expect_icon_reject("closing link cycle", 0, [](Fixture& f) { f.put<u32>(f.tags[30] + 4, kChunkA); }, R::Chain);
}

void test_cursor() {
  Fixture f;
  f.build_mixed();
  const auto plan = f.plan();
  CHECK(plan.has_value());
  if (!plan) return;
  Jak2MapUploadCursor c(*plan);
  CHECK(!c.consume_ordinary_upload(0) && c.position() == 0);
  CHECK(!c.consume_animation_layer(1) && c.position() == 0);
  for (std::size_t i = 0; i < 4; i++) {
    CHECK(!c.complete());
    CHECK(c.consume_animation_layer(i));
  }
  CHECK(!c.consume_animation_layer(4) && c.position() == 4);
  CHECK(!c.consume_ordinary_upload(1) && c.position() == 4);
  CHECK(c.consume_ordinary_upload(0) && !c.complete());
  CHECK(c.consume_ordinary_upload(1) && c.complete());
  CHECK(!c.consume_ordinary_upload(2) && !c.consume_animation_layer(0) && c.position() == 6);
  CHECK(c.peek() == nullptr && c.rejected_count() == 6);

  auto oversized = *plan;
  oversized.op_count = oversized.ops.size() + 1;
  Jak2MapUploadCursor invalid(oversized);
  CHECK(invalid.peek() == nullptr && !invalid.complete());
  CHECK(!invalid.consume_animation_layer(0) && invalid.position() == 0);
}

}  // namespace

int main() {
  test_mixed_positive_and_ownership();
  test_empty_and_legacy();
  test_mutations();
  test_cursor();
  test_icon_chunk();
  if (g_failures) {
    std::fprintf(stderr, "metal_jak2_map_texture_upload_plan_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("metal_jak2_map_texture_upload_plan_test: ok\n");
  return 0;
}
