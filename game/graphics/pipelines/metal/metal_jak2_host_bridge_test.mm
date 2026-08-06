#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_fixture.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/kernel/core/kernel_core.h"
#include "game/runtime.h"

namespace {

constexpr u32 kChainOffset = 0x100000;
constexpr u32 kBucketCount = static_cast<u32>(jak2::BucketId::MAX_BUCKETS);
constexpr u32 kSkyDrawBucket = static_cast<u32>(jak2::BucketId::SKY_DRAW);
constexpr u32 kScreenFilterBucket = static_cast<u32>(jak2::BucketId::SCREEN_FILTER);
constexpr u32 kDebugNoZbuf2Bucket = static_cast<u32>(jak2::BucketId::DEBUG_NO_ZBUF2);
constexpr u32 kSkyDrawPayloadOffset = kChainOffset + 0x4000;
constexpr u32 kScreenFilterPayloadOffset = kChainOffset + 0x5000;
constexpr u32 kDebugNoZbuf2PayloadOffset = kChainOffset + 0x6000;
constexpr std::size_t kGifQwords = 7;
constexpr std::size_t kGifBytes = kGifQwords * 16;
constexpr u16 kTexturePageId = 11;
constexpr u32 kTexturePageOffset = 0x200000;
constexpr u32 kTextureObjectOffset = 0x201000;
constexpr u32 kTextureNameOffset = 0x202000;
constexpr u32 kTextureVram = 0x700;
constexpr u32 kRelocatedTextureVram = 0x720;
constexpr u32 kSyntheticS7 = 0x7f00000;

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

void put_tag(u32 offset,
             DmaTag::Kind kind,
             u16 qwc = 0,
             u32 address = 0,
             u32 vif0 = 0,
             u32 vif1 = 0) {
  const u64 value =
      static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset, &value, sizeof(value));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + offset + 12, &vif1, sizeof(vif1));
}

void make_empty_chain() {
  static_assert(kBucketCount == 327);
  std::memset(static_cast<u8*>(g_ee_main_mem) + kChainOffset, 0, (kBucketCount + 1) * 16);
  for (u32 bucket = 0; bucket < kBucketCount; bucket++) {
    put_tag(kChainOffset + bucket * 16, DmaTag::Kind::CNT);
  }
  put_tag(kChainOffset + kBucketCount * 16, DmaTag::Kind::END);
}

void put_u64(std::array<u8, kGifBytes>& payload, std::size_t offset, u64 value) {
  std::memcpy(payload.data() + offset, &value, sizeof(value));
}

void put_rgbaq(std::array<u8, kGifBytes>& payload, std::size_t offset) {
  constexpr std::array<u32, 4> kGreen = {0, 255, 0, 128};
  std::memcpy(payload.data() + offset, kGreen.data(), 16);
}

void put_xyzf2(std::array<u8, kGifBytes>& payload, std::size_t offset, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  std::memcpy(payload.data() + offset, &x, sizeof(x));
  std::memcpy(payload.data() + offset + 4, &y, sizeof(y));
  put_u64(payload, offset + 8, kZ << 4);
}

std::array<u8, kGifBytes> make_direct_triangle() {
  std::array<u8, kGifBytes> payload = {};
  constexpr u64 kNloop = 1;
  constexpr u64 kEop = 1ull << 15;
  constexpr u64 kPre = 1ull << 46;
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 6);
  constexpr u64 kNreg = 6ull << 60;
  put_u64(payload, 0, kNloop | kEop | kPre | (kPrim << 47) | kNreg);

  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters =
      kRgbaq | (kXyzf2 << 4) | (kRgbaq << 8) | (kXyzf2 << 12) | (kRgbaq << 16) | (kXyzf2 << 20);
  put_u64(payload, 8, kRegisters);

  put_rgbaq(payload, 16);
  put_xyzf2(payload, 32, 0x8000, 0x7800);
  put_rgbaq(payload, 48);
  put_xyzf2(payload, 64, 0x7800, 0x8800);
  put_rgbaq(payload, 80);
  put_xyzf2(payload, 96, 0x8800, 0x8800);
  return payload;
}

void make_direct_chain(u32 bucket, u32 payload_offset) {
  make_empty_chain();
  const auto payload = make_direct_triangle();
  const u32 bucket_offset = kChainOffset + bucket * 16;
  const u32 next_bucket_offset = bucket_offset + 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, payload_offset);
  const u32 direct = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | static_cast<u32>(kGifQwords);
  put_tag(payload_offset, DmaTag::Kind::CNT, static_cast<u16>(kGifQwords), 0, 0, direct);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + payload_offset + 16, payload.data(), payload.size());
  put_tag(payload_offset + 16 + kGifBytes, DmaTag::Kind::NEXT, 0, next_bucket_offset);
}

void make_screen_filter_chain() {
  make_direct_chain(kScreenFilterBucket, kScreenFilterPayloadOffset);
}

void make_sky_draw_chain() {
  static_assert(kSkyDrawBucket == 5);
  make_direct_chain(kSkyDrawBucket, kSkyDrawPayloadOffset);
}

void make_debug_no_zbuf2_chain() {
  static_assert(kDebugNoZbuf2Bucket == 325);
  make_direct_chain(kDebugNoZbuf2Bucket, kDebugNoZbuf2PayloadOffset);
}

bool is_zero(const goal_jak2_metal_frame_summary& summary) {
  return summary.width == 0 && summary.height == 0 && summary.byte_count == 0 &&
         summary.hash == 0 && summary.non_black_pixels == 0 &&
         summary.nonzero_alpha_pixels == 0 && summary.max_alpha == 0;
}

bool sky_batch_is_zero(const goal_jak2_metal_host_metrics& metrics) {
  return metrics.last_sky_draw_batch_valid == 0 &&
         metrics.last_sky_draw_batch_textured == 0 &&
         metrics.last_sky_draw_batch_vertices == 0 &&
         metrics.last_sky_draw_batch_nonzero_rgb_vertices == 0 &&
         metrics.last_sky_draw_batch_tex0_tbp == 0 &&
         metrics.last_sky_draw_batch_tex0_tcc == 0 &&
         metrics.last_sky_draw_batch_tex0_decal == 0 &&
         metrics.last_sky_draw_batch_texture_lookup_hit == 0 &&
         metrics.last_sky_draw_batch_used_placeholder == 0 &&
         metrics.last_sky_draw_batch_write_rgb == 0 &&
         metrics.last_sky_draw_batch_blend_enabled == 0 &&
         metrics.last_sky_draw_batch_blend_a == 0 &&
         metrics.last_sky_draw_batch_blend_b == 0 &&
         metrics.last_sky_draw_batch_blend_c == 0 &&
         metrics.last_sky_draw_batch_blend_d == 0 &&
         metrics.last_sky_draw_batch_alpha_test_enabled == 0 &&
         metrics.last_sky_draw_batch_alpha_test_mode == 0 &&
         metrics.last_sky_draw_batch_alpha_aref == 0 &&
         metrics.last_sky_draw_batch_alpha_afail == 0;
}

bool write_synthetic_fr3(const std::filesystem::path& path,
                         const std::string& level_name,
                         bool with_texture) {
  tfrag3::Level level;
  level.level_name = level_name;
  if (with_texture) {
    tfrag3::Texture texture;
    texture.w = 16;
    texture.h = 16;
    texture.combo_id = (static_cast<u32>(kTexturePageId) << 16);
    texture.data.resize(16 * 16, 0xff40c020);
    texture.debug_name = "host-residency-texture";
    texture.debug_tpage_name = "host-residency-page";
    texture.load_to_pool = true;
    level.textures.push_back(std::move(texture));
  }

  Serializer serializer;
  level.serialize(serializer);
  const auto serialized = serializer.get_save_result();
  const auto compressed = compression::compress_zstd(serialized.first, serialized.second);
  if (compressed.empty()) {
    return false;
  }
  file_util::write_binary_file(path, compressed.data(), compressed.size());
  return std::filesystem::exists(path);
}

void write_texture_page() {
  auto* ee = static_cast<u8*>(g_ee_main_mem);
  GoalTexturePage page = {};
  page.id = kTexturePageId;
  page.length = 1;
  page.name_ptr = kTextureNameOffset;
  std::memcpy(ee + kTexturePageOffset, &page, sizeof(page));
  std::memcpy(ee + kTexturePageOffset + sizeof(page), &kTextureObjectOffset,
              sizeof(kTextureObjectOffset));

  GoalTexture texture = {};
  texture.w = 16;
  texture.h = 16;
  texture.num_mips = 1;
  texture.name_ptr = kTextureNameOffset;
  texture.dest[0] = kTextureVram;
  std::memcpy(ee + kTextureObjectOffset, &texture, sizeof(texture));
  const char name[] = "host-residency-texture";
  std::memcpy(ee + kTextureNameOffset + 4, name, sizeof(name));
}

void append_qword(std::vector<u8>* data, u64 low, u64 high) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &low, sizeof(low));
  std::memcpy(data->data() + offset + 8, &high, sizeof(high));
}

void append_gif_tag(std::vector<u8>* data,
                    u32 loops,
                    u64 registers,
                    u32 register_count,
                    bool eop,
                    bool pre,
                    u64 prim) {
  const u64 low = loops | (static_cast<u64>(eop) << 15) | (static_cast<u64>(pre) << 46) |
                  (prim << 47) | (static_cast<u64>(register_count) << 60);
  append_qword(data, low, registers);
}

void append_st(std::vector<u8>* data, float s, float t) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  const float q = 1.f;
  std::memcpy(data->data() + offset, &s, sizeof(s));
  std::memcpy(data->data() + offset + 4, &t, sizeof(t));
  std::memcpy(data->data() + offset + 8, &q, sizeof(q));
}

void append_rgbaq(std::vector<u8>* data) {
  constexpr std::array<u32, 4> kWhite = {128, 128, 128, 128};
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, kWhite.data(), 16);
}

void append_xyzf2(std::vector<u8>* data, u32 x, u32 y) {
  constexpr u64 kZ = 0xffffff;
  append_qword(data, static_cast<u64>(x) | (static_cast<u64>(y) << 32), kZ << 4);
}

void make_textured_sky_draw_chain(u32 texture_vram) {
  make_empty_chain();
  std::vector<u8> payload;

  constexpr u64 kAd = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  append_gif_tag(&payload, 3, kAd, 1, false, false, 0);
  const u64 tex0 = texture_vram | (1ull << 14) | (4ull << 26) | (4ull << 30) | (1ull << 34);
  append_qword(&payload, tex0, static_cast<u64>(GsRegisterAddress::TEX0_1));
  append_qword(&payload, (1ull << 5) | (1ull << 6),
               static_cast<u64>(GsRegisterAddress::TEX1_1));
  append_qword(&payload, 0b101, static_cast<u64>(GsRegisterAddress::CLAMP_1));

  constexpr u64 kSt = static_cast<u64>(GifTag::RegisterDescriptor::ST);
  constexpr u64 kRgbaq = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ);
  constexpr u64 kXyzf2 = static_cast<u64>(GifTag::RegisterDescriptor::XYZF2);
  constexpr u64 kRegisters = kSt | (kRgbaq << 4) | (kXyzf2 << 8);
  constexpr u64 kPrim = static_cast<u64>(GsPrim::Kind::TRI) | (1ull << 3) | (1ull << 4) |
                        (1ull << 6);
  append_gif_tag(&payload, 3, kRegisters, 3, true, true, kPrim);
  append_st(&payload, 0.f, 0.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x8000, 0x7800);
  append_st(&payload, 0.f, 1.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x7800, 0x8800);
  append_st(&payload, 1.f, 1.f);
  append_rgbaq(&payload);
  append_xyzf2(&payload, 0x8800, 0x8800);

  const u32 bucket_offset = kChainOffset + kSkyDrawBucket * 16;
  const u32 next_bucket_offset = bucket_offset + 16;
  put_tag(bucket_offset, DmaTag::Kind::NEXT, 0, kSkyDrawPayloadOffset);
  const u32 direct = (static_cast<u32>(VifCode::Kind::DIRECT) << 24) |
                     static_cast<u32>(payload.size() / 16);
  put_tag(kSkyDrawPayloadOffset, DmaTag::Kind::CNT, static_cast<u16>(payload.size() / 16), 0, 0,
          direct);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kSkyDrawPayloadOffset + 16, payload.data(),
              payload.size());
  put_tag(kSkyDrawPayloadOffset + 16 + payload.size(), DmaTag::Kind::NEXT, 0,
          next_bucket_offset);
}

}  // namespace

int main() {
  goal_jak2_metal_host_metrics frame_gate = {};
  frame_gate.chains = 1;
  frame_gate.completed_chains = 1;
  frame_gate.last_buckets_dispatched = kBucketCount;
  frame_gate.command_buffers_committed = 1;
  frame_gate.command_buffers_completed = 1;
  frame_gate.drawables_acquired = 1;
  frame_gate.submissions = 1;
  frame_gate.presentation_drops = 1;
  frame_gate.presentation_order_mismatches = 1;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "presentation drops and ordering are diagnostic unless explicitly required");
  frame_gate.presentation_drops = 0;
  frame_gate.presentation_order_mismatches = 0;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a missing drawable callback passes only the GPU-completion gate");
  frame_gate.presentations = 1;
  check(goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "an exact drawable callback passes the presentation-required gate");
  frame_gate.command_buffer_errors = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a GPU failure is rejected by both frame gates");
  frame_gate.command_buffer_errors = 0;
  frame_gate.drawable_misses = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a drawable miss is rejected by both frame gates");
  frame_gate.drawable_misses = 0;
  frame_gate.late_present_submissions = 1;
  check(!goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 0) &&
            !goal_jak2_metal_host_metrics_pass_frame_gate(&frame_gate, 1),
        "a late submission is rejected by both frame gates");

  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
        "initialized the Jak 2 kernel arena for the copied chain");
  if (failures) {
    return 1;
  }
  make_empty_chain();

  const auto fixture_root =
      std::filesystem::temp_directory_path() / "goalpad-jak2-metal-host-residency-test";
  std::error_code fixture_error;
  std::filesystem::remove_all(fixture_root, fixture_error);
  fixture_error.clear();
  std::filesystem::create_directories(fixture_root / "fr3", fixture_error);
  std::filesystem::create_directories(fixture_root / "wrong", fixture_error);
  check(!fixture_error &&
            write_synthetic_fr3(fixture_root / "fr3/GAME.fr3", "synthetic-common-key", true) &&
            write_synthetic_fr3(fixture_root / "fr3/arena.fr3", "synthetic-arena-key", false) &&
            write_synthetic_fr3(fixture_root / "wrong/arena.fr3", "wrong-directory-key", false),
        "created public synthetic FR3 level-art fixtures");
  if (failures) {
    goal_kernel_core_shutdown();
    return 1;
  }

  const std::size_t initial_level_count = metal_level_data::level_count();
  const std::size_t initial_texture_count = metal_texture_live_count();

  check(goal_jak2_metal_host_create_presenting(nullptr) == nullptr,
        "rejected presenting mode without an app-owned CAMetalLayer");

  goal_jak2_metal_host* missing_host = goal_jak2_metal_host_create();
  check(missing_host != nullptr, "created a host for missing-directory rejection");
  check(missing_host &&
            !goal_jak2_metal_host_configure_level_art(
                missing_host, (fixture_root / "missing").string().c_str()),
        "rejected a missing FR3 directory");
  goal_gfx_host rejected_callbacks = {};
  check(missing_host && !goal_jak2_metal_host_copy_gfx_host(missing_host, &rejected_callbacks),
        "a failed common-art load cannot publish runtime callbacks");
  goal_jak2_metal_host_destroy(missing_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_texture_live_count() == initial_texture_count,
        "missing-directory rejection released its placeholder without leaking level art");

  goal_jak2_metal_host* wrong_host = goal_jak2_metal_host_create();
  check(wrong_host != nullptr, "created a host for wrong-directory rejection");
  check(wrong_host &&
            !goal_jak2_metal_host_configure_level_art(
                wrong_host, (fixture_root / "wrong").string().c_str()),
        "rejected an FR3 directory without GAME.fr3");
  goal_jak2_metal_host_destroy(wrong_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_texture_live_count() == initial_texture_count,
        "wrong-directory rejection left no global Metal resources");

  goal_jak2_metal_host* host = goal_jak2_metal_host_create();
  check(host != nullptr, "created the process-singleton Jak 2 Metal host");
  if (!host) {
    goal_kernel_core_shutdown();
    return 1;
  }
  check(goal_jak2_metal_host_create() == nullptr,
        "rejected a second live Jak 2 Metal host");
  const std::string fr3_directory = (fixture_root / "fr3").string();
  check(goal_jak2_metal_host_configure_level_art(host, fr3_directory.c_str()),
        "synchronously loaded synthetic GAME.fr3 before publishing callbacks");
  const std::size_t configured_texture_count = metal_texture_live_count();
  check(metal_level_data::level_count() == initial_level_count + 1 &&
            configured_texture_count == initial_texture_count + 2,
        "common art uses its serialized level key and owns one texture plus the placeholder");
  check(goal_jak2_metal_host_configure_level_art(host, fr3_directory.c_str()) &&
            metal_level_data::level_count() == initial_level_count + 1 &&
            metal_texture_live_count() == configured_texture_count,
        "same-directory configuration is idempotent");
  check(!goal_jak2_metal_host_configure_level_art(
            host, (fixture_root / "wrong").string().c_str()),
        "rejected reconfiguration to another FR3 directory");

  goal_gfx_host callbacks = {};
  check(goal_jak2_metal_host_copy_gfx_host(host, &callbacks),
        "copied the app-owned graphics callback table");
  check(callbacks.send_chain && callbacks.sync_path && callbacks.vsync,
        "the copied host contains every required synchronous callback");
  check(callbacks.texture_upload_now && callbacks.texture_relocate && callbacks.set_levels,
        "the copied host contains real texture-residency callbacks");

  const char* arena[] = {"arena"};
  callbacks.set_levels(arena, 1);
  check(metal_level_data::level_count() == initial_level_count + 2,
        "set-levels loaded one requested FR3 under its serialized key");
  callbacks.set_levels(arena, 1);
  check(metal_level_data::level_count() == initial_level_count + 2 &&
            metal_texture_live_count() == configured_texture_count,
        "set-levels loads each requested basename only once and retains it");

  write_texture_page();
  callbacks.texture_upload_now(static_cast<u8*>(g_ee_main_mem) + kTexturePageOffset, -1,
                               kSyntheticS7);
  callbacks.texture_relocate(kRelocatedTextureVram, kTextureVram, 0);

  goal_jak2_metal_frame_summary frame_summary = {1, 1, 1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer mode safely rejects readback before a frame exists");

  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  callbacks.sync_path();
  callbacks.vsync();
  goal_jak2_metal_host_metrics metrics = {};
  check(goal_jak2_metal_host_get_metrics(host, &metrics), "copied the host metrics after dispatch");
  check(metrics.chains == 1 && metrics.completed_chains == 1 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "one copied 327-bucket chain completed policy dispatch");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.draws == 0 && metrics.triangles == 0 && metrics.submissions == 0 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_screen_filter_draws == 0 && metrics.last_screen_filter_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0 &&
            metrics.presentations == 0 && metrics.presentation_drops == 0 &&
            metrics.presentation_order_mismatches == 0 && metrics.unsupported_blends == 0,
        "nil-layer lifecycle dispatches without committing, drawing, or presenting");
  check(sky_batch_is_zero(metrics), "an empty chain exposes no SKY_DRAW batch facts");
  check(!goal_jak2_metal_host_wait_for_last_frame(host, 0.01, 0),
        "nil-layer mode rejects a completion wait without changing its dispatch result");

  make_screen_filter_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic SCREEN_FILTER dispatch");
  check(metrics.chains == 2 && metrics.completed_chains == 2 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic SCREEN_FILTER chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 && metrics.last_screen_filter_draws == 1 &&
            metrics.last_screen_filter_triangles == 1 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0,
        "SCREEN_FILTER records its deterministic Direct draw and triangle");
  check(sky_batch_is_zero(metrics), "SCREEN_FILTER facts do not leak into SKY_DRAW metrics");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0,
        "nil-layer SCREEN_FILTER drawing remains submission- and presentation-free");
  frame_summary = {1, 1, 1, 1, 1, 1, 1};
  check(!goal_jak2_metal_host_read_last_frame(host, &frame_summary) && is_zero(frame_summary),
        "nil-layer SCREEN_FILTER encoding still exposes no completed frame readback");

  make_debug_no_zbuf2_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic DEBUG_NO_ZBUF2 dispatch");
  check(metrics.chains == 3 && metrics.completed_chains == 3 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic DEBUG_NO_ZBUF2 chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 &&
            metrics.last_debug_no_zbuf2_draws == 1 &&
            metrics.last_debug_no_zbuf2_triangles == 1 &&
            metrics.last_sky_draw_draws == 0 && metrics.last_sky_draw_triangles == 0 &&
            metrics.last_screen_filter_draws == 0 && metrics.last_screen_filter_triangles == 0,
        "DEBUG_NO_ZBUF2 owns the deterministic Direct draw and triangle exactly");
  check(sky_batch_is_zero(metrics), "DEBUG_NO_ZBUF2 facts do not leak into SKY_DRAW metrics");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0 &&
            metrics.unsupported_blends == 0,
        "nil-layer DEBUG_NO_ZBUF2 drawing remains submission- and presentation-free");

  make_sky_draw_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied the host metrics after synthetic SKY_DRAW dispatch");
  check(metrics.chains == 4 && metrics.completed_chains == 4 && metrics.failed_chains == 0 &&
            metrics.last_buckets_dispatched == kBucketCount,
        "the synthetic SKY_DRAW chain completed all 327 policy buckets");
  check(metrics.draws == 1 && metrics.triangles == 1 && metrics.last_sky_draw_draws == 1 &&
            metrics.last_sky_draw_triangles == 1 && metrics.last_screen_filter_draws == 0 &&
            metrics.last_screen_filter_triangles == 0 &&
            metrics.last_debug_no_zbuf2_draws == 0 &&
            metrics.last_debug_no_zbuf2_triangles == 0,
        "SKY_DRAW owns the deterministic Direct draw and triangle exactly");
  check(metrics.last_sky_draw_batch_valid == 1 &&
            metrics.last_sky_draw_batch_textured == 0 &&
            metrics.last_sky_draw_batch_vertices == 3 &&
            metrics.last_sky_draw_batch_nonzero_rgb_vertices == 3 &&
            metrics.last_sky_draw_batch_tex0_tbp == 0 &&
            metrics.last_sky_draw_batch_tex0_tcc == 0 &&
            metrics.last_sky_draw_batch_tex0_decal == 0 &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 0 &&
            metrics.last_sky_draw_batch_used_placeholder == 0 &&
            metrics.last_sky_draw_batch_write_rgb == 1,
        "SKY_DRAW exposes its basic vertex, TEX0, texture, and RGB-write facts");
  check(metrics.last_sky_draw_batch_blend_enabled == 1 &&
            metrics.last_sky_draw_batch_blend_a ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_b ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST) &&
            metrics.last_sky_draw_batch_blend_c ==
                static_cast<uint32_t>(GsAlpha::BlendMode::SOURCE) &&
            metrics.last_sky_draw_batch_blend_d ==
                static_cast<uint32_t>(GsAlpha::BlendMode::DEST) &&
            metrics.last_sky_draw_batch_alpha_test_enabled == 0 &&
            metrics.last_sky_draw_batch_alpha_test_mode ==
                static_cast<uint32_t>(GsTest::AlphaTest::NOTEQUAL) &&
            metrics.last_sky_draw_batch_alpha_aref == 0 &&
            metrics.last_sky_draw_batch_alpha_afail ==
                static_cast<uint32_t>(GsTest::AlphaFail::KEEP),
        "SKY_DRAW exposes its GS blend and alpha-test facts");
  check(metrics.command_buffers_committed == 0 && metrics.command_buffers_completed == 0 &&
            metrics.command_buffer_errors == 0 && metrics.drawables_acquired == 0 &&
            metrics.drawable_misses == 0 && metrics.late_present_submissions == 0 &&
            metrics.submissions == 0 && metrics.presentations == 0 &&
            metrics.presentation_drops == 0 && metrics.presentation_order_mismatches == 0 &&
            metrics.unsupported_blends == 0,
        "nil-layer SKY_DRAW drawing remains submission- and presentation-free");

  make_textured_sky_draw_chain(kRelocatedTextureVram);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics),
        "copied metrics after real upload and relocate callbacks");
  check(metrics.chains == 5 && metrics.completed_chains == 5 && metrics.failed_chains == 0 &&
            metrics.texture_uploads == 1 && metrics.texture_relocations == 1,
        "real texture upload and relocation preserved their host counters");
  check(metrics.last_sky_draw_batch_valid == 1 &&
            metrics.last_sky_draw_batch_textured == 1 &&
            metrics.last_sky_draw_batch_tex0_tbp == kRelocatedTextureVram &&
            metrics.last_sky_draw_batch_texture_lookup_hit == 1 &&
            metrics.last_sky_draw_batch_used_placeholder == 0,
        "SKY_DRAW resolved the configured GAME texture through upload and relocation mapping");

  const char* missing_level[] = {"missing-level"};
  callbacks.set_levels(missing_level, 1);
  make_empty_chain();
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(goal_jak2_metal_host_get_metrics(host, &metrics) && metrics.chains == 6 &&
            metrics.completed_chains == 5 && metrics.failed_chains == 1,
        "a requested FR3 load failure fails the next renderer chain closed");

  goal_jak2_metal_host_destroy(host);
  callbacks.send_chain(g_ee_main_mem, kChainOffset);
  check(!goal_jak2_metal_host_get_metrics(host, &metrics),
        "a destroyed host no longer exposes state while stale callbacks remain inert");
  check(metal_level_data::level_count() == initial_level_count &&
            metal_texture_live_count() == initial_texture_count,
        "destroy unloaded recorded serialized keys in reverse and released every texture handle");

  constexpr u32 kBucket4FixtureBase = 0x300000;
  auto bucket4_fixture =
      metal_renderer::make_jak2_bucket4_texture_upload_fixture(kBucket4FixtureBase);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.data() + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.size() - kBucket4FixtureBase);
  goal_jak2_metal_host* capture_host = goal_jak2_metal_host_create();
  goal_gfx_host capture_callbacks = {};
  check(capture_host && goal_jak2_metal_host_copy_gfx_host(capture_host, &capture_callbacks),
        "created a host for bucket-4 capture integration");
  if (capture_callbacks.send_chain) {
    capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  }
  goal_jak2_metal_host_metrics capture_metrics = {};
  check(capture_host && goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics),
        "copied metrics after valid bucket-4 capture");
  check(capture_metrics.chains == 1 && capture_metrics.completed_chains == 1 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.last_bucket4_texture_upload.valid == 1 &&
            capture_metrics.last_bucket4_texture_upload.present == 1 &&
            capture_metrics.last_bucket4_texture_upload.total_payload_bytes == 416 &&
            capture_metrics.last_bucket4_texture_upload.dma_transfers == 16 &&
            capture_metrics.bucket4_ordinary_uploads == 1 &&
            capture_metrics.bucket4_mixed_executions == 1 &&
            capture_metrics.bucket4_cloud_publications == 1 &&
            capture_metrics.bucket4_fog_publications == 1 &&
            capture_metrics.bucket4_cloud_texture != 0 &&
            capture_metrics.bucket4_fog_texture != 0 &&
            capture_metrics.bucket4_cloud_texture != capture_metrics.bucket4_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "valid bucket 4 executes the ordinary page and both stable animator publications");
  const uint64_t first_cloud_texture = capture_metrics.bucket4_cloud_texture;
  const uint64_t first_fog_texture = capture_metrics.bucket4_fog_texture;
  const uint32_t first_copied_bytes = capture_metrics.last_copied_bytes;

  auto ordinary_fixture =
      metal_renderer::make_jak2_bucket4_ordinary_only_texture_upload_fixture(kBucket4FixtureBase);
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              ordinary_fixture.ee_memory.data() + kBucket4FixtureBase,
              ordinary_fixture.ee_memory.size() - kBucket4FixtureBase);
  capture_callbacks.send_chain(g_ee_main_mem, ordinary_fixture.chain_offset);
  check(goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics) &&
            capture_metrics.chains == 2 && capture_metrics.completed_chains == 2 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.bucket4_ordinary_uploads == 2 &&
            capture_metrics.bucket4_mixed_executions == 1 &&
            capture_metrics.bucket4_cloud_publications == 1 &&
            capture_metrics.bucket4_fog_publications == 1 &&
            capture_metrics.bucket4_cloud_texture == first_cloud_texture &&
            capture_metrics.bucket4_fog_texture == first_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "ordinary-only bucket 4 executes without republishing animator textures");

  std::memcpy(static_cast<u8*>(g_ee_main_mem) + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.data() + kBucket4FixtureBase,
              bucket4_fixture.ee_memory.size() - kBucket4FixtureBase);
  capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  check(goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics) &&
            capture_metrics.chains == 3 && capture_metrics.completed_chains == 3 &&
            capture_metrics.failed_chains == 0 &&
            capture_metrics.bucket4_ordinary_uploads == 3 &&
            capture_metrics.bucket4_mixed_executions == 2 &&
            capture_metrics.bucket4_cloud_publications == 2 &&
            capture_metrics.bucket4_fog_publications == 2 &&
            capture_metrics.bucket4_cloud_texture == first_cloud_texture &&
            capture_metrics.bucket4_fog_texture == first_fog_texture &&
            capture_metrics.skipped_bucket_bytes == 0,
        "repeated mixed bucket 4 keeps stable registry handles while replacing texture objects");

  const u32 missing_finish = 0;
  std::memcpy(static_cast<u8*>(g_ee_main_mem) + bucket4_fixture.first_finish_tag_offset + 8,
              &missing_finish, sizeof(missing_finish));
  if (capture_callbacks.send_chain) {
    capture_callbacks.send_chain(g_ee_main_mem, bucket4_fixture.chain_offset);
  }
  check(capture_host && goal_jak2_metal_host_get_metrics(capture_host, &capture_metrics),
        "copied metrics after malformed bucket-4 capture");
  const char* capture_error = goal_jak2_metal_host_last_error(capture_host);
  check(capture_metrics.chains == 4 && capture_metrics.completed_chains == 3 &&
            capture_metrics.failed_chains == 1 &&
            capture_metrics.last_bucket4_texture_upload.valid == 0 &&
            capture_metrics.last_bucket4_texture_upload.present == 1 &&
            capture_metrics.bucket4_ordinary_uploads == 3 &&
            capture_metrics.bucket4_mixed_executions == 2 &&
            capture_metrics.last_copied_bytes == first_copied_bytes &&
            capture_metrics.skipped_bucket_bytes == 0 && capture_error &&
            std::strstr(capture_error,
                        "bucket 4 texture-upload capture rejected malformed DMA"),
        "malformed bucket 4 fails before mutation, copying, or dispatch");
  goal_jak2_metal_host_destroy(capture_host);

  goal_jak2_metal_host* replacement = goal_jak2_metal_host_create();
  check(replacement != nullptr, "host ownership can be re-established after destruction");
  check(replacement &&
            goal_jak2_metal_host_configure_level_art(replacement, fr3_directory.c_str()),
        "a recreated host can own the same FR3 directory after exact teardown");
  goal_jak2_metal_host_destroy(replacement);

  goal_jak2_metal_host* basename_host = goal_jak2_metal_host_create();
  goal_gfx_host basename_callbacks = {};
  check(basename_host &&
            goal_jak2_metal_host_configure_level_art(basename_host, fr3_directory.c_str()) &&
            goal_jak2_metal_host_copy_gfx_host(basename_host, &basename_callbacks),
        "created a configured host for basename validation");
  const char* escaped_level[] = {"../outside"};
  if (basename_callbacks.set_levels) {
    basename_callbacks.set_levels(escaped_level, 1);
  }
  const char* basename_error = goal_jak2_metal_host_last_error(basename_host);
  check(basename_error && std::strstr(basename_error, "non-basename"),
        "set-levels rejects path traversal instead of leaving the FR3 directory");
  goal_jak2_metal_host_destroy(basename_host);

  goal_jak2_metal_host* late_host = goal_jak2_metal_host_create();
  goal_gfx_host late_callbacks = {};
  check(late_host && goal_jak2_metal_host_copy_gfx_host(late_host, &late_callbacks),
        "created a callback-published host for late-configuration rejection");
  check(late_host &&
            !goal_jak2_metal_host_configure_level_art(late_host, fr3_directory.c_str()),
        "rejected level-art configuration after callbacks were published");
  goal_jak2_metal_host_destroy(late_host);
  check(metal_level_data::level_count() == initial_level_count &&
            metal_texture_live_count() == initial_texture_count,
        "host recreation and late rejection left no global Metal handles");

  goal_kernel_core_shutdown();
  std::filesystem::remove_all(fixture_root, fixture_error);

  if (failures) {
    std::printf("FAIL: %d Jak 2 Metal host lifecycle checks failed\n", failures);
    return 1;
  }
  std::printf("PASS: Jak 2 external Metal host copied, dispatched, synchronized, and released\n");
  return 0;
}
