/*! Test-only fixture: runs the CURRENT production translated Jak 1 sky generation
 * (game/mips2c/jak1_functions/sky_tng.cpp, included directly so anonymous caches are reachable)
 * against synthetic EE memory. No retail assets. Emits the original synthetic eight-face bipyramid
 * through the actual render_sky_tri; output bytes land at a caller-chosen EE address so a DMA
 * DIRECT payload can be built at the SAME address the native receipt recorded.
 *
 * Exactly ONE TU per binary may include this header (it includes sky_tng.cpp). Define
 * JAK1_SKY_FIXTURE_LINK_STUBS in that TU only when the binary does NOT link the kernel that
 * already defines g_ee_main_mem / jak1::intern_from_c / LinkedFunctionTable (see
 * game/kernel/core/jak1_mips2c_sky_register_state_test.cpp for the same convention).
 */
#pragma once

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "game/graphics/pipelines/metal/metal_immersive_camera.h"
#include "game/mips2c/jak1_functions/sky_tng.cpp"

#if defined(JAK1_SKY_FIXTURE_LINK_STUBS)
u8* g_ee_main_mem = nullptr;
extern "C" u64 g_goal_current_process = 0;
[[noreturn]] void private_assert_failed(const char*, const char*, int, const char*, const char*) {
  std::abort();
}
namespace jak1 {
Ptr<Symbol> intern_from_c(const char*) {
  return {};
}
}  // namespace jak1
namespace Mips2C {
LinkedFunctionTable gLinkedFunctionTable;
void LinkedFunctionTable::reg(const std::string&, u64 (*)(void*), u32) {}
}  // namespace Mips2C
#endif

namespace jak1_sky_fixture {
constexpr u32 kMemSize = 0x80000;
constexpr u32 kCamera = 0x1004;  // basic tag offset: vector fields at +572 are 16-byte aligned
constexpr u32 kSkyData = 0x3004;
constexpr u32 kGiftag = 0x4000;
constexpr u32 kDma = 0x8000;
constexpr u32 kOutput = 0x9000;  // default emission address (chain DIRECT payload address)
constexpr u32 kOutputEnd = 0x1f000;
constexpr u32 kSpad = 0x20000;
constexpr u32 kSource = 0x30000;
constexpr u32 kStack = 0x70000;
constexpr u32 kCameraSnapshot = 1024;

inline u32 g_cam_word = kCamera;
inline u32 g_sky_word = kSkyData;
inline u32 g_spad_word = kSpad;
inline u32 g_dummy_word = 0;
inline std::vector<u8> g_mem;

inline void put_f4(u32 addr, float x, float y, float z, float w) {
  const float v[4] = {x, y, z, w};
  std::memcpy(g_ee_main_mem + addr, v, 16);
}
inline void put_f(u32 addr, float x) {
  std::memcpy(g_ee_main_mem + addr, &x, 4);
}

// Nontrivial original packet: copy of the frozen bridge br_make_packet / GPU make_jak1_packet
// (trans {1234,-567,9101}, yaw 0.4). packet.rot[world axis][camera axis] is the GOAL row.
// Callers pass this SAME packet to production derive_immersive_view_transform.
inline metal_renderer::ImmersivePacketCamera packet(float yaw = 0.4f) {
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kNear = 1024.f;
  constexpr float kFar = 40960000.f;
  constexpr float kFogStart = 40960.f;
  constexpr float kFogEnd = 819200.f;
  constexpr float kFogMinimum = 150.f;
  constexpr float kFogMaximum = 255.f;
  constexpr float kDepthMinimum = 100.f;
  constexpr float kDepthMaximum = 16760631.f;
  const float x_ratio = std::tan(32.f * kPi / 180.f);
  const float y_ratio = x_ratio * 0.75f;
  const float fog_slope = kNear * (kFogMinimum - kFogMaximum) / (kFogEnd - kFogStart);
  const float depth_half_range = -0.5f * (kDepthMaximum - kDepthMinimum);
  const float half_depth_slope = depth_half_range / (kNear * (kFar - kNear));
  metal_renderer::ImmersivePacketCamera p;
  p.hvdf_off = {2048.f, 2048.f, 0.5f * (kDepthMaximum + kDepthMinimum), 17.f};
  p.fog = {fog_slope, kFogMinimum, kFogMaximum, 0.f};
  p.trans = {1234.f, -567.f, 9101.f, 1.f};
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  p.rot = {{{c, 0.f, -s, 0.f}, {0.f, 1.f, 0.f, 0.f}, {s, 0.f, c, 0.f}, {0.f, 0.f, 0.f, 1.f}}};
  p.perspective[0][0] = -256.f / (x_ratio * kNear);
  p.perspective[1][1] = -112.f / (y_ratio * kNear);
  p.perspective[2][2] = (kFar + kNear) * half_depth_slope;
  p.perspective[2][3] = fog_slope / kNear;
  p.perspective[3][2] = -2.f * half_depth_slope * kFar * kNear;
  return p;
}

inline void setup_memory() {
  g_mem.assign(kMemSize, 0);
  g_ee_main_mem = g_mem.data();
  const std::array<u64, 2> giftag = {0x302ec00000008001ull, 0x0000000000000412ull};
  std::memcpy(g_ee_main_mem + kGiftag, giftag.data(), 16);
}

// Writes camera-rot (+364), perspective (+156) and camera-temp (+572) from ONE packet, plus
// hmge/inv-hmge/hvdf/fog exactly as frozen production-coverage COVERAGE-REVIEWED.cpp:89-112.
// camera-temp = (rot * I) * perspective with the SAME loop/accumulation order as native
// sky_generation::consume_init, so the H = I transformed rows are bit-identical to ordinary.
inline void configure_camera(const metal_renderer::ImmersivePacketCamera& p) {
  float rot[16], persp[16], identity[16] = {}, rk[16], temp[16];
  for (int r = 0; r < 4; r++) {
    identity[r * 4 + r] = 1.f;
    for (int c = 0; c < 4; c++) {
      rot[r * 4 + c] = p.rot[r][c];
      persp[r * 4 + c] = p.perspective[r][c];
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++) sum += rot[r * 4 + k] * identity[k * 4 + c];
      rk[r * 4 + c] = sum;
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++) sum += rk[r * 4 + k] * persp[k * 4 + c];
      temp[r * 4 + c] = sum;
    }
  }
  for (int i = 0; i < 4; i++) {
    put_f4(kCamera + 364 + 16 * i, rot[i * 4], rot[i * 4 + 1], rot[i * 4 + 2], rot[i * 4 + 3]);
    put_f4(kCamera + 156 + 16 * i, persp[i * 4], persp[i * 4 + 1], persp[i * 4 + 2],
           persp[i * 4 + 3]);
    put_f4(kCamera + 572 + 16 * i, temp[i * 4], temp[i * 4 + 1], temp[i * 4 + 2], temp[i * 4 + 3]);
  }
  const float fog = p.fog[0];
  const float xclip = 1024.f, yclip = 448.f;
  const float dbhr = -0.5f * (16760631.f - 100.f);
  put_f4(kCamera + 700, 1.f / xclip, 1.f / yclip, 1.f / dbhr, 1.f / fog);
  put_f4(kCamera + 716, xclip, yclip, dbhr, fog);
  put_f4(kCamera + 732, p.hvdf_off[0], p.hvdf_off[1], p.hvdf_off[2], p.hvdf_off[3]);
  put_f(kCamera + 848, p.trans[1]);
  put_f(kCamera + 828, p.fog[0]);
  put_f(kCamera + 128, 150.f);
  put_f(kCamera + 124, 255.f);
}

inline void setup_caches() {
  using namespace Mips2C::jak1;
  init_sky_regs::cache.math_camera = &g_cam_word;
  init_sky_regs::cache.sky_tng_data = &g_sky_word;
  render_sky_quad::cache.math_camera = &g_cam_word;
  render_sky_quad::cache.draw_large_polygon = &g_dummy_word;
  render_sky_quad::cache.fake_scratchpad_data = &g_spad_word;
  render_sky_tri::cache.draw_large_polygon = &g_dummy_word;
  render_sky_tri::cache.fake_scratchpad_data = &g_spad_word;
  draw_large_polygon::cache.clip_polygon_against_positive_hyperplane = &g_dummy_word;
  draw_large_polygon::cache.clip_polygon_against_negative_hyperplane = &g_dummy_word;
}

inline Mips2C::ExecutionContext* fresh_context() {
  static Mips2C::ExecutionContext ctx;
  ctx = {};
  ctx.gprs[Mips2C::sp].du64[0] = kStack;
  return &ctx;
}

// Actual translated init_sky_regs + set_sky_vf27 (native consume_init runs inside init_sky_regs).
inline void init_registers() {
  using namespace Mips2C;
  Mips2C::jak1::sky_regs_vfs = {};
  const float one[4] = {0.f, 0.f, 0.f, 1.f};
  std::memcpy(&Mips2C::jak1::sky_regs_vfs.vfs[vf0].f[0], one, 16);
  Mips2C::jak1::init_sky_regs::execute(fresh_context());
  auto* c = fresh_context();
  c->gprs[a0].du64[0] = kGiftag;
  Mips2C::jak1::set_sky_vf27::execute(c);
}

struct EmitRange {
  u32 begin = 0;
  u32 end = 0;
};

struct Generated {
  bool ok = false;
  std::vector<EmitRange> ranges;  // one per actual render_sky_tri call, EE addresses
  u32 begin = 0, end = 0;         // contiguous [begin,end) of all emitted GIF bytes
  std::vector<u8> bytes;          // copy of g_ee_main_mem[begin,end)
};

// Original synthetic eight-face bipyramid: ring E0..E3, roof apex, base apex; each face in source
// order (Ei, Ei+1, apex), st {(0,0),(1,0),(.5,1)}, w = 0. Emits via actual render_sky_tri,
// chaining the DMA pointer so all faces are contiguous starting at out_addr.
inline Generated emit_bipyramid(u32 out_addr = kOutput) {
  using namespace Mips2C;
  static const float ring[4][3] = {
      {0, 0, -40960000}, {40960000, 0, 0}, {0, 0, 40960000}, {-40960000, 0, 0}};
  static const float apex[2][3] = {{0, 10240000, 0}, {0, -40960000, 0}};
  static const float st[3][2] = {{0, 0}, {1, 0}, {.5f, 1}};
  Generated g;
  std::memset(g_ee_main_mem + out_addr, 0, kOutputEnd - out_addr);
  u32 out = out_addr;
  for (int a = 0; a < 2; a++) {
    for (int i = 0; i < 4; i++) {
      const float* v[3] = {ring[i], ring[(i + 1) & 3], apex[a]};
      for (int k = 0; k < 3; k++) {
        const u32 base = kSource + k * 48;
        put_f4(base + 0, v[k][0], v[k][1], v[k][2], 0.f);
        put_f4(base + 16, st[k][0], st[k][1], 1.f, 0.f);
        put_f4(base + 32, 128.f, 128.f, 128.f, 128.f);
      }
      std::memcpy(g_ee_main_mem + kDma + 4, &out, 4);
      auto* c = fresh_context();
      c->gprs[a0].du64[0] = kSource;
      c->gprs[a1].du64[0] = kDma;
      Mips2C::jak1::render_sky_tri::execute(c);
      u32 end = 0;
      std::memcpy(&end, g_ee_main_mem + kDma + 4, 4);
      if (end < out || end > kOutputEnd) {
        return g;
      }
      if (end > out) {
        g.ranges.push_back({out, end});
      }
      out = end;
    }
  }
  g.ok = true;
  g.begin = out_addr;
  g.end = out;
  g.bytes.assign(g_ee_main_mem + out_addr, g_ee_main_mem + out);
  return g;
}

// Actual translated render_sky_quad on a +-0.25*far quad straight ahead (w = 0).
inline Generated emit_quad(u32 out_addr = kOutput) {
  using namespace Mips2C;
  Generated g;
  std::memset(g_ee_main_mem + out_addr, 0, kOutputEnd - out_addr);
  const float qx[4] = {-1.f, 1.f, 1.f, -1.f}, qy[4] = {-1.f, -1.f, 1.f, 1.f};
  const float qs[4] = {0.f, 1.f, 1.f, 0.f}, qt[4] = {0.f, 0.f, 1.f, 1.f};
  for (int i = 0; i < 4; i++) {
    const u32 v = kSource + i * 48;
    put_f4(v + 0, qx[i] * 10240000.f, qy[i] * 10240000.f, 40960000.f, 0.f);
    put_f4(v + 16, qs[i], qt[i], 1.f, 0.f);
    put_f4(v + 32, 128.f, 128.f, 128.f, 128.f);
  }
  u32 out = out_addr;
  std::memcpy(g_ee_main_mem + kDma + 4, &out, 4);
  auto* c = fresh_context();
  c->gprs[a0].du64[0] = kSource;
  c->gprs[a1].du64[0] = kDma;
  Mips2C::jak1::render_sky_quad::execute(c);
  u32 end = 0;
  std::memcpy(&end, g_ee_main_mem + kDma + 4, 4);
  if (end < out || end > kOutputEnd) return g;
  if (end > out) g.ranges.push_back({out, end});
  g.ok = true;
  g.begin = out_addr;
  g.end = end;
  g.bytes.assign(g_ee_main_mem + out_addr, g_ee_main_mem + end);
  return g;
}

inline std::vector<u8> camera_snapshot() {
  return std::vector<u8>(g_ee_main_mem + kCamera, g_ee_main_mem + kCamera + kCameraSnapshot);
}
}  // namespace jak1_sky_fixture
