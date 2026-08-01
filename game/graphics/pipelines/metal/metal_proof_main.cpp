/*!
 * @file metal_proof_main.cpp
 * Standalone proof that the Metal backend's frame scaffolding and texture path
 * work through the GfxRendererModule interface (the same seam the OpenGL
 * pipeline sits behind).
 *
 * It selects the Metal renderer, opens a real window, renders frames, and
 * verifies by pixel readback:
 *  - the offscreen game target: opaque, additive, alpha, reverse-subtract and
 *    color-write-masked draws resolve correctly through the PSO cache, textures
 *    sample correctly, and the PS2-style depth convention (clear 0, GEQUAL)
 *    rejects geometry behind what was already drawn
 *  - the PSO / depth-stencil caches: distinct states are each built exactly
 *    once and reused across frames
 *  - the present pass: the game frame appears centered and letterboxed at 1:1
 *    and scaled sizes, with black bars, and the pmode-alp blackout and
 *    brightness/contrast math match the GL renderer's PCRTC pass
 *  - the texture path: RGBA8888 uploads with GPU-generated mips, sampler
 *    filter/wrap/mip modes, PS2 texture formats (PSMT8/PSMT4 with PSMCT32/16
 *    CLUTs and PSMCT16) converted through the shared TextureConverter and
 *    sampled on the GPU, and the TexturePool VRAM bookkeeping driven through
 *    the module's texture_upload_now / texture_relocate hooks and the Metal
 *    TextureUploadHandler's DMA walk
 *  - optionally (argv[1] = path to an .fr3 file): real extracted level
 *    textures uploaded through the loader-mirror path and verified texel by
 *    texel against the CPU-side data. No game data is required or bundled.
 *
 * With `--replay <capture.bin>` it instead replays one frame of real Jak 1 DMA
 * captured by the runtime track's `__send-gfx-dma-chain` hook: the capture is
 * relocated into a fake EE memory, sent through the module's send_chain like
 * the game does, rendered by the bucket dispatch, verified by readback, and
 * saved as a PNG (`--replay-png <path>`, default `<capture>.png`). The
 * per-bucket payload inventory of the capture is printed so it is explicit
 * what the frame carried and what was skipped. Capture files come from the
 * player's own game data and must never be committed.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/texture/texture_conversion.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/background/background_common.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/opengl_renderer/sprite/sprite_common.h"
#include "game/graphics/pipelines/metal/metal_chain_replay.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"
#include "game/graphics/pipelines/metal/metal_texture_upload_handler.h"
#include "game/graphics/texture/TextureConverter.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/graphics/texture/jak1_tpage_dir.h"
#include "game/runtime.h"

namespace {

int g_fail_count = 0;

void check(bool ok, const char* what) {
  printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) {
    g_fail_count++;
  }
}

void check_pixel(const metal_renderer::FramePixels& frame,
                 int x,
                 int y,
                 int r,
                 int g,
                 int b,
                 const char* what) {
  const u8* p = &frame.rgba[(y * frame.width + x) * 4];
  const int tol = 10;
  bool ok = std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
  printf("[%s] %s at (%d,%d): expected ~(%d,%d,%d), got (%d,%d,%d)\n", ok ? "PASS" : "FAIL", what,
         x, y, r, g, b, p[0], p[1], p[2]);
  if (!ok) {
    g_fail_count++;
  }
}

// Renders the sample quad or records a failure.
bool sample_tex(const metal_renderer::TextureSampleSpec& spec,
                metal_renderer::FramePixels* out,
                const char* what) {
  if (!metal_renderer::read_texture_sample(spec, out)) {
    printf("[FAIL] %s: read_texture_sample failed\n", what);
    g_fail_count++;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// PS2 texture format helpers.
//
// TextureConverter::upload copies linear data into the emulated VRAM using the
// PSMCT32 layout at 128 pixels wide, exactly like the game's DMA texture
// uploads. To author test textures in the *swizzled* formats (PSMT8/PSMT4/
// PSMCT16 + CLUTs), VramWriter builds the inverse map (VRAM byte address ->
// linear input offset) so tests can place specific texels / CLUT entries at
// specific VRAM addresses using the real address translation functions from
// common/texture/texture_conversion.h.
// ---------------------------------------------------------------------------
struct VramWriter {
  std::vector<u8> input;
  u32 dest_words;
  std::unordered_map<u32, u32> vram_to_input;  // VRAM byte address -> input byte offset

  // covers VRAM bytes [dest_words*4, dest_words*4 + num_words*4)
  VramWriter(u32 dest_words_in, u32 num_words) : input(num_words * 4, 0), dest_words(dest_words_in) {
    for (u32 i = 0; i < num_words; i++) {
      u32 x = i % 128;
      u32 y = i / 128;
      u32 addr = psmct32_addr(x, y, 128) + dest_words * 4;
      for (u32 b = 0; b < 4; b++) {
        vram_to_input[addr + b] = i * 4 + b;
      }
    }
  }

  void set_byte(u32 vram_addr, u8 val) { input.at(vram_to_input.at(vram_addr)) = val; }
  void set_u16(u32 vram_addr, u16 val) {
    set_byte(vram_addr, val & 0xff);
    set_byte(vram_addr + 1, val >> 8);
  }
  void set_u32(u32 vram_addr, u32 val) {
    for (int b = 0; b < 4; b++) {
      set_byte(vram_addr + b, (val >> (8 * b)) & 0xff);
    }
  }
  // half-byte addressing for PSMT4 (odd address = high nibble, as in
  // TextureConverter::download_rgba8888)
  void set_nibble(u32 half_addr, u8 val) {
    u32 idx = vram_to_input.at(half_addr / 2);
    u8 cur = input.at(idx);
    if (half_addr & 1) {
      input.at(idx) = (cur & 0x0f) | (u8)(val << 4);
    } else {
      input.at(idx) = (cur & 0xf0) | (val & 0x0f);
    }
  }
  void upload(TextureConverter& conv) {
    conv.upload(input.data(), dest_words, (u32)(input.size() / 4));
  }
};

// CLUT entry position for IDTEX8 in CSM1 mode (GS manual 2.7.3), matching the
// lookup in TextureConverter::download_rgba8888.
void clut8_pos(u8 v, u32* clx, u32* cly) {
  u32 chunk = v / 16;
  u32 off = v % 16;
  *clx = (chunk & 1) ? 8 : 0;
  *cly = (chunk >> 1) * 2;
  if (off >= 8) {
    off -= 8;
    (*cly)++;
  }
  *clx += off;
}

// CLUT entry position for IDTEX4 in CSM1 mode.
void clut4_pos(u8 v, u32* clx, u32* cly) {
  *clx = v & 0x7;
  *cly = v >> 3;
}

// Compares a full GPU 1:1 nearest sample of `handle` against expected RGBA.
void check_gpu_matches(u64 handle, const std::vector<u8>& expected, int w, int h, const char* what) {
  metal_renderer::TextureSampleSpec spec;
  spec.texture = handle;
  spec.out_w = w;
  spec.out_h = h;
  metal_renderer::FramePixels frame;
  if (!sample_tex(spec, &frame, what)) {
    return;
  }
  int mismatches = 0;
  for (int i = 0; i < w * h * 4; i++) {
    if (frame.rgba[i] != expected[i]) {
      mismatches++;
    }
  }
  printf("[%s] %s: GPU sample matches CPU conversion (%d byte mismatches of %d)\n",
         mismatches == 0 ? "PASS" : "FAIL", what, mismatches, w * h * 4);
  if (mismatches != 0) {
    g_fail_count++;
  }
}

// ---------------------------------------------------------------------------
// Section: RGBA8888 upload, sampler modes, mip generation/selection
// ---------------------------------------------------------------------------
void test_upload_and_samplers() {
  printf("--- texture path: RGBA upload + sampler modes ---\n");

  // 4x4 texture in 2x2 quadrants: TL red, TR green, BL blue, BR white
  u8 quad_tex[4 * 4 * 4];
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      u8* p = &quad_tex[(y * 4 + x) * 4];
      bool right = x >= 2;
      bool bottom = y >= 2;
      p[0] = (!right && !bottom) ? 255 : (right && bottom) ? 255 : (right && !bottom) ? 0 : 0;
      p[1] = (right && !bottom) ? 255 : (right && bottom) ? 255 : 0;
      p[2] = (!right && bottom) ? 255 : (right && bottom) ? 255 : 0;
      p[3] = 255;
    }
  }
  u64 quad_handle = metal_renderer::upload_texture_rgba8(quad_tex, 4, 4);
  check(quad_handle != 0, "uploaded 4x4 RGBA texture");

  metal_renderer::FramePixels frame;
  metal_renderer::TextureSampleSpec spec;
  spec.texture = quad_handle;
  spec.out_w = 4;
  spec.out_h = 4;
  if (sample_tex(spec, &frame, "nearest 1:1 sample")) {
    check_pixel(frame, 0, 0, 255, 0, 0, "quadrant texture TL (red)");
    check_pixel(frame, 3, 0, 0, 255, 0, "quadrant texture TR (green)");
    check_pixel(frame, 0, 3, 0, 0, 255, "quadrant texture BL (blue)");
    check_pixel(frame, 3, 3, 255, 255, 255, "quadrant texture BR (white)");
  }

  // wrap: repeat over u in [0,2] shows the pattern twice
  spec.out_w = 8;
  spec.out_h = 4;
  spec.u1 = 2.f;
  spec.wrap_s_repeat = true;
  if (sample_tex(spec, &frame, "repeat wrap sample")) {
    check_pixel(frame, 0, 0, 255, 0, 0, "repeat: first tile left (red)");
    check_pixel(frame, 3, 0, 0, 255, 0, "repeat: first tile right (green)");
    check_pixel(frame, 4, 0, 255, 0, 0, "repeat: second tile left (red)");
    check_pixel(frame, 7, 0, 0, 255, 0, "repeat: second tile right (green)");
  }

  // wrap: clamp-to-edge extends the border texels for u outside [0,1]
  spec.u0 = -0.5f;
  spec.u1 = 1.5f;
  spec.wrap_s_repeat = false;
  if (sample_tex(spec, &frame, "clamp wrap sample")) {
    check_pixel(frame, 0, 0, 255, 0, 0, "clamp: left of texture clamps to red");
    check_pixel(frame, 7, 0, 0, 255, 0, "clamp: right of texture clamps to green");
  }

  // mag filter: nearest vs linear between a black and a white column
  u8 bw_tex[2 * 2 * 4];
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      u8 v = x ? 255 : 0;
      u8* p = &bw_tex[(y * 2 + x) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 255;
    }
  }
  u64 bw_handle = metal_renderer::upload_texture_rgba8(bw_tex, 2, 2);
  check(bw_handle != 0, "uploaded 2x2 black/white texture");
  metal_renderer::TextureSampleSpec bw;
  bw.texture = bw_handle;
  bw.out_w = 64;
  bw.out_h = 8;
  if (sample_tex(bw, &frame, "nearest mag filter")) {
    check_pixel(frame, 31, 4, 0, 0, 0, "nearest mag: just left of center is black");
    check_pixel(frame, 32, 4, 255, 255, 255, "nearest mag: just right of center is white");
  }
  bw.mag_linear = true;
  bw.min_linear = true;
  if (sample_tex(bw, &frame, "linear mag filter")) {
    check_pixel(frame, 32, 4, 132, 132, 132, "linear mag: center blends black and white");
  }

  // mips: a 64x64 1px red/blue checker averages to purple in the generated
  // mip chain; minified 16x it must hit a small mip level
  std::vector<u8> checker(64 * 64 * 4);
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 64; x++) {
      u8* p = &checker[(y * 64 + x) * 4];
      bool red = ((x + y) & 1) != 0;
      p[0] = red ? 255 : 0;
      p[1] = 0;
      p[2] = red ? 0 : 255;
      p[3] = 255;
    }
  }
  u64 checker_handle = metal_renderer::upload_texture_rgba8(checker.data(), 64, 64);
  check(checker_handle != 0, "uploaded 64x64 checker texture");
  metal_renderer::TextureSampleSpec mip;
  mip.texture = checker_handle;
  mip.out_w = 4;
  mip.out_h = 4;
  mip.min_linear = true;
  mip.mag_linear = true;
  mip.mip_mode = 2;  // trilinear
  if (sample_tex(mip, &frame, "trilinear minified sample")) {
    check_pixel(frame, 2, 2, 128, 0, 128, "mip selection: minified checker averages to purple");
  }
  mip.mip_mode = 0;  // mips off: base level sampling stays pure red or blue
  mip.min_linear = false;
  mip.mag_linear = false;
  if (sample_tex(mip, &frame, "mips-off minified sample")) {
    const u8* p = &frame.rgba[(2 * frame.width + 2) * 4];
    bool pure = (p[1] == 0) && ((p[0] == 255 && p[2] == 0) || (p[0] == 0 && p[2] == 255));
    printf("[%s] mips off: base level sample stays pure red/blue, got (%d,%d,%d)\n",
           pure ? "PASS" : "FAIL", p[0], p[1], p[2]);
    if (!pure) {
      g_fail_count++;
    }
  }
  // anisotropic sampler variant builds and behaves like trilinear on a
  // screen-aligned quad
  mip.mip_mode = 2;
  mip.min_linear = true;
  mip.mag_linear = true;
  mip.max_aniso = 4;
  if (sample_tex(mip, &frame, "anisotropic sample")) {
    check_pixel(frame, 2, 2, 128, 0, 128, "anisotropy 4: minified checker still purple");
  }
}

// ---------------------------------------------------------------------------
// Section: PS2 texture formats through TextureConverter + GPU sampling
// ---------------------------------------------------------------------------
constexpr int kPsW = 64;
constexpr int kPsH = 16;
constexpr u32 kTexBlocks = 0;    // texture data at VRAM block 0
constexpr u32 kClutBlocks = 64;  // CLUT at VRAM block 64 (byte 16384)

void run_ps2_case(const char* what,
                  PSM psm,
                  CPSM clut_psm,
                  const std::function<void(VramWriter&, VramWriter&, std::vector<u8>&)>& author) {
  TextureConverter conv;
  // each writer covers 16KB of VRAM starting at its block address
  VramWriter tex_writer(kTexBlocks * 64, 4096);
  VramWriter clut_writer(kClutBlocks * 64, 4096);
  std::vector<u8> expected(kPsW * kPsH * 4);
  author(tex_writer, clut_writer, expected);
  tex_writer.upload(conv);
  if (psm == PSM::PSMT8 || psm == PSM::PSMT4) {
    clut_writer.upload(conv);
  }

  std::vector<u8> converted(kPsW * kPsH * 4);
  u32 clut_arg = (psm == PSM::PSMCT16) ? 0 : (u32)clut_psm;
  conv.download_rgba8888(converted.data(), kTexBlocks, 1, kPsW, kPsH, (u32)psm, clut_arg,
                         kClutBlocks, (u32)converted.size());

  bool cpu_ok = converted == expected;
  printf("[%s] %s: CPU conversion matches authored texels\n", cpu_ok ? "PASS" : "FAIL", what);
  if (!cpu_ok) {
    g_fail_count++;
  }

  u64 handle = metal_renderer::upload_texture_rgba8(converted.data(), kPsW, kPsH);
  check(handle != 0, "uploaded converted PS2 texture");
  check_gpu_matches(handle, converted, kPsW, kPsH, what);
}

void test_ps2_formats() {
  printf("--- texture path: PS2 formats (TextureConverter -> Metal) ---\n");

  // PSMT8 + PSMCT32 CLUT: index (x+y*3)%256, distinct RGBA per CLUT entry
  run_ps2_case("PSMT8+PSMCT32", PSM::PSMT8, CPSM::PSMCT32,
               [](VramWriter& tex, VramWriter& clut, std::vector<u8>& expected) {
                 u32 clut_colors[256];
                 for (u32 v = 0; v < 256; v++) {
                   clut_colors[v] = (0x80u << 24) | (((v * 3) & 0xff) << 16) | ((255 - v) << 8) | v;
                   u32 clx, cly;
                   clut8_pos((u8)v, &clx, &cly);
                   clut.set_u32(psmct32_addr(clx, cly, 64) + kClutBlocks * 256, clut_colors[v]);
                 }
                 for (u32 y = 0; y < kPsH; y++) {
                   for (u32 x = 0; x < kPsW; x++) {
                     u8 idx = (u8)((x + y * 3) & 0xff);
                     tex.set_byte(psmt8_addr(x, y, kPsW) + kTexBlocks * 256, idx);
                     memcpy(&expected[(y * kPsW + x) * 4], &clut_colors[idx], 4);
                   }
                 }
               });

  // PSMT8 + PSMCT16 CLUT: 32 distinct 16-bit CLUT entries
  run_ps2_case("PSMT8+PSMCT16", PSM::PSMT8, CPSM::PSMCT16,
               [](VramWriter& tex, VramWriter& clut, std::vector<u8>& expected) {
                 u16 clut16[256];
                 for (u32 v = 0; v < 256; v++) {
                   clut16[v] = (u16)(0x8000 | (v & 31) | (((v * 7) & 31) << 5) | ((31 - (v & 31)) << 10));
                   u32 clx, cly;
                   clut8_pos((u8)v, &clx, &cly);
                   clut.set_u16(psmct16_addr(clx, cly, 64) + kClutBlocks * 256, clut16[v]);
                 }
                 for (u32 y = 0; y < kPsH; y++) {
                   for (u32 x = 0; x < kPsW; x++) {
                     u8 idx = (u8)((x * 5 + y) & 0xff);
                     tex.set_byte(psmt8_addr(x, y, kPsW) + kTexBlocks * 256, idx);
                     u32 rgba = rgba16_to_rgba32(clut16[idx]);
                     memcpy(&expected[(y * kPsW + x) * 4], &rgba, 4);
                   }
                 }
               });

  // PSMT4 + PSMCT16 CLUT: 16 entries, half-byte texel addressing
  run_ps2_case("PSMT4+PSMCT16", PSM::PSMT4, CPSM::PSMCT16,
               [](VramWriter& tex, VramWriter& clut, std::vector<u8>& expected) {
                 u16 clut16[16];
                 for (u32 v = 0; v < 16; v++) {
                   clut16[v] = (u16)(0x8000 | (v * 2) | ((31 - v * 2) << 5) | (v << 10));
                   u32 clx, cly;
                   clut4_pos((u8)v, &clx, &cly);
                   clut.set_u16(psmct16_addr(clx, cly, 64) + kClutBlocks * 256, clut16[v]);
                 }
                 for (u32 y = 0; y < kPsH; y++) {
                   for (u32 x = 0; x < kPsW; x++) {
                     u8 idx = (u8)((x + y) & 0xf);
                     tex.set_nibble(psmt4_addr_half_byte(x, y, kPsW) + kTexBlocks * 512, idx);
                     u32 rgba = rgba16_to_rgba32(clut16[idx]);
                     memcpy(&expected[(y * kPsW + x) * 4], &rgba, 4);
                   }
                 }
               });

  // PSMT4 + PSMCT32 CLUT
  run_ps2_case("PSMT4+PSMCT32", PSM::PSMT4, CPSM::PSMCT32,
               [](VramWriter& tex, VramWriter& clut, std::vector<u8>& expected) {
                 u32 clut_colors[16];
                 for (u32 v = 0; v < 16; v++) {
                   clut_colors[v] = (0x80u << 24) | ((v * 16) << 16) | ((255 - v * 16) << 8) | (v * 17);
                   u32 clx, cly;
                   clut4_pos((u8)v, &clx, &cly);
                   clut.set_u32(psmct32_addr(clx, cly, 64) + kClutBlocks * 256, clut_colors[v]);
                 }
                 for (u32 y = 0; y < kPsH; y++) {
                   for (u32 x = 0; x < kPsW; x++) {
                     u8 idx = (u8)((x * 3 + y * 2) & 0xf);
                     tex.set_nibble(psmt4_addr_half_byte(x, y, kPsW) + kTexBlocks * 512, idx);
                     memcpy(&expected[(y * kPsW + x) * 4], &clut_colors[idx], 4);
                   }
                 }
               });

  // PSMCT16 direct 16-bit texels, no CLUT
  run_ps2_case("PSMCT16", PSM::PSMCT16, CPSM(0),
               [](VramWriter& tex, VramWriter& /*clut*/, std::vector<u8>& expected) {
                 for (u32 y = 0; y < kPsH; y++) {
                   for (u32 x = 0; x < kPsW; x++) {
                     u16 texel = (u16)(0x8000 | (x & 31) | ((y & 31) << 5) | (((x + y) & 31) << 10));
                     tex.set_u16(psmct16_addr(x, y, kPsW) + kTexBlocks * 256, texel);
                     u32 rgba = rgba16_to_rgba32(texel);
                     memcpy(&expected[(y * kPsW + x) * 4], &rgba, 4);
                   }
                 }
               });
}

// ---------------------------------------------------------------------------
// Section: TexturePool bookkeeping through the module hooks and the Metal
// TextureUploadHandler's DMA walk. Builds a synthetic GOAL texture-page in a
// fake EE memory and drives it the way the runtime would.
// ---------------------------------------------------------------------------

// offsets within the fake EE memory
constexpr u32 kTpageAddr = 0x1000;
constexpr u32 kTex0Addr = 0x2000;
constexpr u32 kTex2Addr = 0x3000;
constexpr u32 kTpage2Addr = 0x4000;
constexpr u32 kNameAddr = 0x5000;
constexpr u32 kS7 = 0xDEAD0;
constexpr u32 kVramGiven = 0x200;    // slot for the texture the pool has
constexpr u32 kVramMissing = 0x300;  // slot for a texture the loader never gave
constexpr u32 kVramReloc = 0x400;
constexpr u32 kVramMt4hh = 0x500;
constexpr u32 kVramDma = 0x600;

// writes a GoalTexturePage with three textures: [given, #f, never-given]
void write_fake_tpage(std::vector<u8>& ee,
                      u32 tpage_addr,
                      u16 page_id,
                      u32 dest0,
                      u32 dest2,
                      s16 w = 16,
                      s16 h = 16) {
  GoalTexturePage page;
  memset(&page, 0, sizeof(page));
  page.id = page_id;
  page.length = 3;
  page.name_ptr = kNameAddr;
  memcpy(ee.data() + tpage_addr, &page, sizeof(page));

  u32 tex_ptrs[3] = {kTex0Addr, kS7, kTex2Addr};
  memcpy(ee.data() + tpage_addr + sizeof(GoalTexturePage), tex_ptrs, sizeof(tex_ptrs));

  GoalTexture tex;
  memset(&tex, 0, sizeof(tex));
  tex.w = w;
  tex.h = h;
  tex.num_mips = 1;
  tex.name_ptr = kNameAddr;
  tex.dest[0] = (u16)dest0;
  memcpy(ee.data() + kTex0Addr, &tex, sizeof(tex));
  tex.dest[0] = (u16)dest2;
  memcpy(ee.data() + kTex2Addr, &tex, sizeof(tex));

  const char name[] = "proof-tex";
  memcpy(ee.data() + kNameAddr + 4, name, sizeof(name));
}

void test_pool_and_hooks(const GfxRendererModule* mod, std::vector<u8>& ee_mem) {
  printf("--- texture path: TexturePool via module hooks ---\n");

  TexturePool* pool = metal_renderer::get_texture_pool();
  if (!pool) {
    printf("[FAIL] Metal pipeline has no texture pool\n");
    g_fail_count++;
    return;
  }

  // find a Jak 1 tpage with at least 3 texture slots
  const auto& dir = get_jak1_tpage_dir();
  u16 page_id = 0;
  for (u16 i = 0; i < (u16)dir.size(); i++) {
    if (dir[i] >= 3) {
      page_id = i;
      break;
    }
  }
  check(dir[page_id] >= 3, "found a Jak 1 tpage with >= 3 textures");

  // the loader-mirror path: upload a 16x16 texture and give it to the pool
  tfrag3::Texture tex;
  tex.w = 16;
  tex.h = 16;
  tex.combo_id = ((u32)page_id << 16) | 0;
  tex.load_to_pool = true;
  tex.debug_name = "proof-tex";
  tex.debug_tpage_name = "proof-page";
  tex.data.resize(16 * 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      tex.data[y * 16 + x] = 0xff000000u | ((u32)(y * 16) << 8) | (u32)(x * 16 + 8);
    }
  }
  u64 given_handle = metal_renderer::pool_add_texture(tex, false);
  check(given_handle != 0, "loader-mirror pool_add_texture uploaded and gave the texture");

  // fake EE memory with a GOAL texture-page, driven through the module's
  // texture_upload_now hook exactly like the runtime does
  write_fake_tpage(ee_mem, kTpageAddr, page_id, kVramGiven, kVramMissing);
  g_ee_main_mem = ee_mem.data();
  mod->texture_upload_now(ee_mem.data() + kTpageAddr, -1, kS7);

  auto found = pool->lookup(kVramGiven);
  check(found.has_value() && *found == given_handle,
        "texture_upload_now links the VRAM slot to the given texture");

  // and the texture is actually usable: sample the slot's handle
  if (found) {
    metal_renderer::TextureSampleSpec spec;
    spec.texture = *found;
    spec.out_w = 16;
    spec.out_h = 16;
    metal_renderer::FramePixels frame;
    if (sample_tex(spec, &frame, "sample via VRAM slot")) {
      check_pixel(frame, 8, 4, 8 * 16 + 8, 4 * 16, 0, "VRAM slot texture content");
    }
  }

  // a texture the loader never provided must resolve to the placeholder
  auto missing = pool->lookup(kVramMissing);
  check(missing.has_value() && *missing == pool->get_placeholder_texture(),
        "unloaded texture resolves to the placeholder");
  if (missing) {
    metal_renderer::TextureSampleSpec spec;
    spec.texture = *missing;
    spec.out_w = 16;
    spec.out_h = 16;
    metal_renderer::FramePixels frame;
    if (sample_tex(spec, &frame, "sample placeholder")) {
      check_pixel(frame, 1, 1, 48, 48, 48, "placeholder dark checker cell");
      check_pixel(frame, 5, 1, 224, 224, 224, "placeholder light checker cell");
    }
  }

  // texture_relocate: plain move, then the mt4hh (format 44) variant
  mod->texture_relocate(kVramReloc, kVramGiven, 0);
  auto reloc = pool->lookup(kVramReloc);
  check(reloc.has_value() && *reloc == given_handle, "texture_relocate moves the slot link");

  mod->texture_relocate(kVramMt4hh, kVramGiven, 44);
  auto mt4hh = pool->lookup_mt4hh(kVramMt4hh);
  check(mt4hh.has_value() && *mt4hh == given_handle, "texture_relocate format 44 fills mt4hh slot");

  // the TextureUploadHandler DMA walk: a second tpage referencing the same
  // texture at a new VRAM address, sent as a PC-port upload packet
  printf("--- texture path: TextureUploadHandler DMA walk ---\n");
  write_fake_tpage(ee_mem, kTpage2Addr, page_id, kVramDma, kVramMissing);

  struct UploadPacket {
    u64 page;
    s64 mode;
  };
  std::vector<u8> chain(64, 0);
  // CNT tag, qwc=1, vif0 = PC_PORT, vif1 = 3, then the 16-byte upload packet
  u64 tag0 = (1ull) | (1ull << 28);  // qwc 1, kind CNT
  memcpy(chain.data() + 0, &tag0, 8);
  u32 vif0 = (u32)VifCode::Kind::PC_PORT << 24;
  u32 vif1 = 3;
  memcpy(chain.data() + 8, &vif0, 4);
  memcpy(chain.data() + 12, &vif1, 4);
  UploadPacket packet{kTpage2Addr, -1};
  memcpy(chain.data() + 16, &packet, sizeof(packet));
  // empty CNT transfer (skipped by the handler), bucket ends at offset 48
  u64 tag1 = (0ull) | (1ull << 28);
  memcpy(chain.data() + 32, &tag1, 8);

  DmaFollower dma(chain.data(), 0);
  MetalTextureUploadHandler handler;
  auto stats = handler.process(dma, 48, *pool, ee_mem.data(), kS7);
  check(stats.uploads == 1, "DMA walk found exactly one upload packet");
  check(stats.skipped_eye_dma == 0 && stats.skipped_texture_anim == 0,
        "DMA walk skipped nothing unexpectedly");
  auto dma_slot = pool->lookup(kVramDma);
  check(dma_slot.has_value() && *dma_slot == given_handle,
        "DMA upload packet linked the new VRAM slot");

  // unloading the only copy turns every slot into the placeholder
  pool->unload_texture(PcTextureId(page_id, 0), given_handle);
  auto after_unload = pool->lookup(kVramGiven);
  check(after_unload.has_value() && *after_unload == pool->get_placeholder_texture(),
        "unload_texture falls back to the placeholder");
}

// ---------------------------------------------------------------------------
// Section: the DMA chain path. Builds a Jak 1-shaped frame chain (initial CALL
// to a default-registers buffer, 70 bucket slots, empty-bucket structure with
// CALL/RET bounces) carrying DirectRenderer GS packets, sky-blend packets, a
// sky draw, a texture upload packet and un-ported bucket content, sends it
// through the module's send_chain like the game does, and verifies the
// rendered frame by pixel readback plus the chain counters.
//
// This is constructed data (no captured retail chain is replayed): the packet
// patterns mirror what SkyRenderer / SkyBlendCPU / DirectRenderer assert about
// real Jak 1 chains, and every expected value is computed with the same math
// the GL shaders use.
// ---------------------------------------------------------------------------

// the chain must live above the copier's low-memory protect (512 kB); one
// full copier chunk starting at 1 MB holds everything
constexpr u32 kEeBase = 0x100000;
constexpr u32 kEeSize = kEeBase + 0x20000;
constexpr u32 kChainStart = kEeBase + 0x100;
constexpr u32 kBucketsBase = kChainStart + 16;
constexpr int kNumBuckets = 70;  // jak1::BucketId::MAX_BUCKETS
constexpr u32 kChainEnd = kBucketsBase + 16 * kNumBuckets;
constexpr u32 kDefaultRegs = kEeBase + 0x2000;
constexpr u32 kHeapStart = kEeBase + 0x3000;
constexpr u32 kHeapEnd = kEeBase + 0xF000;
// EE-side objects referenced by the chain (same buffer acts as EE memory)
constexpr u32 kChainTpageDirectTex = kEeBase + 0x10000;
constexpr u32 kChainTpageSkySrc = kEeBase + 0x11000;
constexpr u32 kChainTpageCloudSrc = kEeBase + 0x12000;
constexpr u32 kChainTpageDmaUpload = kEeBase + 0x13000;
// VRAM slots used by the chain test
constexpr u32 kVramDirectTex = 0x700;
constexpr u32 kVramSkySrc = 0x740;
constexpr u32 kVramCloudSrc = 0x760;
constexpr u32 kVramChainUpload = 0x780;

struct ChainBuilder {
  std::vector<u8>& mem;
  u32 cursor = kHeapStart;

  explicit ChainBuilder(std::vector<u8>& m) : mem(m) {
    // initial CALL to the default-registers chain (vifs must be nops)
    tag(kChainStart, DmaTag::Kind::CALL, 0, kDefaultRegs, 0, 0);
    // default regs: CNT with 10 quadwords (fog color at data byte 144) + RET
    tag(kDefaultRegs, DmaTag::Kind::CNT, 10, 0, 0, 0);
    mem[kDefaultRegs + 16 + 144 + 0] = 42;
    mem[kDefaultRegs + 16 + 144 + 1] = 43;
    mem[kDefaultRegs + 16 + 144 + 2] = 44;
    tag(kDefaultRegs + 16 + 160, DmaTag::Kind::RET, 0, 0, 0, 0);
    // chain terminator after the last bucket slot
    tag(kChainEnd, DmaTag::Kind::END, 0, 0, 0, 0);
    for (int i = 0; i < kNumBuckets; i++) {
      make_empty_bucket(i);
    }
  }

  u32 slot(int i) const { return kBucketsBase + 16 * i; }
  u32 next_of(int i) const { return i + 1 < kNumBuckets ? slot(i + 1) : kChainEnd; }

  void tag(u32 at, DmaTag::Kind kind, u16 qwc, u32 addr, u32 vif0, u32 vif1) {
    u64 t = (u64)qwc | ((u64)kind << 28) | ((u64)addr << 32);
    memcpy(&mem[at], &t, 8);
    memcpy(&mem[at + 8], &vif0, 4);
    memcpy(&mem[at + 12], &vif1, 4);
  }

  u32 alloc(u32 bytes) {
    u32 a = cursor;
    cursor += bytes;
    if (cursor > kHeapEnd) {
      printf("[FAIL] chain heap exhausted\n");
      exit(1);
    }
    return a;
  }

  // NEXT into the bucket, CALL to default regs, NEXT to the next bucket - the
  // structure MetalEmptyBucketRenderer asserts
  void make_empty_bucket(int i) {
    u32 c = alloc(32);
    tag(slot(i), DmaTag::Kind::NEXT, 0, c, 0, 0);
    tag(c, DmaTag::Kind::CALL, 0, kDefaultRegs, 0, 0);
    tag(c + 16, DmaTag::Kind::NEXT, 0, next_of(i), 0, 0);
  }

  struct Transfer {
    u32 vif0 = 0;
    u32 vif1 = 0;
    std::vector<u8> data;
    bool empty_next = false;  // a 0-qwc NEXT hop (the game's "empty" transfers)
  };

  // bucket content: the listed transfers, then the CALL default-regs tail
  void set_bucket_content(int i, const std::vector<Transfer>& transfers) {
    u32 total = 32;  // CALL + trailing NEXT
    for (auto& t : transfers) {
      total += 16 + (u32)t.data.size();
    }
    u32 c = alloc(total);
    tag(slot(i), DmaTag::Kind::NEXT, 0, c, 0, 0);
    u32 p = c;
    for (auto& t : transfers) {
      if (t.empty_next) {
        tag(p, DmaTag::Kind::NEXT, 0, p + 16, 0, 0);
      } else {
        tag(p, DmaTag::Kind::CNT, (u16)(t.data.size() / 16), 0, t.vif0, t.vif1);
        memcpy(&mem[p + 16], t.data.data(), t.data.size());
      }
      p += 16 + (u32)t.data.size();
    }
    tag(p, DmaTag::Kind::CALL, 0, kDefaultRegs, 0, 0);
    tag(p + 16, DmaTag::Kind::NEXT, 0, next_of(i), 0, 0);
  }
};

u32 vif_nop() {
  return 0;
}
u32 vif_direct(u32 qwc) {
  return ((u32)VifCode::Kind::DIRECT << 24) | (qwc & 0xffff);
}

// --- GIF payload builders ---------------------------------------------------

struct GifBuilder {
  std::vector<u8> data;

  void push_qw(u64 lo, u64 hi) {
    size_t at = data.size();
    data.resize(at + 16);
    memcpy(&data[at], &lo, 8);
    memcpy(&data[at + 8], &hi, 8);
  }

  void tag(u32 nloop,
           bool eop,
           const std::vector<GifTag::RegisterDescriptor>& regs,
           bool pre = false,
           u16 prim = 0) {
    u64 lo = (nloop & 0x7fff) | ((u64)(eop ? 1 : 0) << 15) | ((u64)(pre ? 1 : 0) << 46) |
             ((u64)(prim & 0x7ff) << 47) | ((u64)(regs.size() & 0xf) << 60);
    u64 hi = 0;
    for (size_t i = 0; i < regs.size(); i++) {
      hi |= (u64)regs[i] << (4 * i);
    }
    push_qw(lo, hi);
  }

  void ad(GsRegisterAddress addr, u64 val) { push_qw(val, (u64)addr); }

  void rgbaq(u8 r, u8 g, u8 b, u8 a) {
    u8 qw[16] = {};
    qw[0] = r;
    qw[4] = g;
    qw[8] = b;
    qw[12] = a;
    size_t at = data.size();
    data.resize(at + 16);
    memcpy(&data[at], qw, 16);
  }

  void st(float s, float t, float q) {
    u8 qw[16] = {};
    memcpy(qw, &s, 4);
    memcpy(qw + 4, &t, 4);
    memcpy(qw + 8, &q, 4);
    size_t at = data.size();
    data.resize(at + 16);
    memcpy(&data[at], qw, 16);
  }

  void uv(u32 u, u32 v) {  // 12.4 fixed
    u8 qw[16] = {};
    memcpy(qw, &u, 4);
    memcpy(qw + 4, &v, 4);
    size_t at = data.size();
    data.resize(at + 16);
    memcpy(&data[at], qw, 16);
  }

  void xyzf2(u32 x, u32 y, u32 z, u8 f = 0) {  // x/y are 12.4 fixed
    u64 upper = ((u64)z << 4) | ((u64)f << 36);
    push_qw((u64)x | ((u64)y << 32), upper);
  }
};

// --- GS register encoders (bit layouts from common/dma/gs.h) ----------------

u64 gs_test(bool ate,
            GsTest::AlphaTest atst,
            u8 aref,
            GsTest::AlphaFail afail,
            bool zte,
            GsTest::ZTest ztst) {
  return (ate ? 1ull : 0) | ((u64)atst << 1) | ((u64)aref << 4) | ((u64)afail << 12) |
         ((zte ? 1ull : 0) << 16) | ((u64)ztst << 17);
}
u64 gs_zbuf(u32 zbp, bool zmsk) {
  return zbp | (0b0001ull << 24) | ((zmsk ? 1ull : 0) << 32);  // PSMZ24
}
u64 gs_alpha(u32 a, u32 b, u32 c, u32 d, u8 fix = 0) {
  return a | (b << 2) | (c << 4) | (d << 6) | ((u64)fix << 32);
}
u16 gs_prim(GsPrim::Kind kind, bool iip, bool tme, bool abe, bool fst, bool fge = false) {
  return (u16)((u32)kind | ((iip ? 1 : 0) << 3) | ((tme ? 1 : 0) << 4) | ((fge ? 1 : 0) << 5) |
               ((abe ? 1 : 0) << 6) | ((fst ? 1 : 0) << 8));
}
u64 gs_tex0(u32 tbp, u32 tbw, u32 psm, u32 tw, u32 th, bool tcc, u32 tfx) {
  return tbp | ((u64)tbw << 14) | ((u64)psm << 20) | ((u64)tw << 26) | ((u64)th << 30) |
         ((tcc ? 1ull : 0) << 34) | ((u64)tfx << 35);
}
u64 gs_tex1_filt() {
  return (1ull << 5) | (1ull << 6);  // mmag/mmin = linear
}

// --- coordinate mapping (Jak 1, 640x480 game target) ------------------------
// The direct shader maps GS x 2048+-256 to full width and (half-height) GS y
// 2048+-112 to full height (see direct_basic.vert and shaders/direct.metal).

u32 fx(double px) {
  return (u32)llround(px * 16.0);  // 12.4 fixed
}
int gs_to_col(double gs_x) {
  double ndc = (gs_x - 2048.0) / 256.0;
  return (int)llround((ndc + 1.0) / 2.0 * 640.0);
}
int gs_to_row(double gs_y) {
  double ndc = -(gs_y - 2048.0) / 112.0;
  return (int)llround((1.0 - ndc) / 2.0 * 480.0);
}

// registers a solid-color texture with the pool and links it to a VRAM slot
// through the module's texture_upload_now, like the game does. The tfrag3
// texture is kept alive by the caller (the pool references its pixels).
u64 give_and_link_texture(const GfxRendererModule* mod,
                          std::vector<u8>& ee,
                          tfrag3::Texture& tex,
                          u16 page_id,
                          u32 tpage_addr,
                          u32 vram_slot) {
  tex.combo_id = ((u32)page_id << 16) | 0;
  tex.load_to_pool = true;
  u64 handle = metal_renderer::pool_add_texture(tex, false);
  check(handle != 0, "chain: texture given to pool");
  write_fake_tpage(ee, tpage_addr, page_id, vram_slot, vram_slot + 0x20, (s16)tex.w, (s16)tex.h);
  mod->texture_upload_now(ee.data() + tpage_addr, -1, kS7);
  auto linked = metal_renderer::get_texture_pool()->lookup(vram_slot);
  check(linked.has_value() && *linked == handle, "chain: VRAM slot linked");
  return handle;
}

// finds distinct Jak 1 tpages with at least 3 texture slots
std::vector<u16> find_tpages(size_t count) {
  const auto& dir = get_jak1_tpage_dir();
  std::vector<u16> pages;
  for (u16 i = 0; i < (u16)dir.size() && pages.size() < count; i++) {
    if (dir[i] >= 3) {
      pages.push_back(i);
    }
  }
  return pages;
}

void test_dma_chain(const GfxRendererModule* mod,
                    std::shared_ptr<GfxDisplay>& display,
                    const tfrag3::Level* level) {
  printf("--- DMA chain: send_chain -> bucket dispatch -> Direct/Sky ---\n");
  using namespace jak1;

  TexturePool* pool = metal_renderer::get_texture_pool();
  auto pages = find_tpages(5);
  if (pages.size() < 5) {
    printf("[FAIL] not enough Jak 1 tpages for the chain test\n");
    g_fail_count++;
    return;
  }
  // the pool tests used pages[0]; use fresh pages here
  u16 page_direct = pages[1], page_sky = pages[2], page_cloud = pages[3], page_upload = pages[4];

  // chain memory doubles as EE memory
  std::vector<u8> mem(kEeSize, 0);
  g_ee_main_mem = mem.data();

  // --- textures the chain references ---------------------------------------
  // direct-renderer texture: 16x16 quadrants (TL red, TR green, BL blue,
  // BR white), alpha 255 on the left half and 32 on the right half
  tfrag3::Texture direct_tex;
  direct_tex.w = 16;
  direct_tex.h = 16;
  direct_tex.debug_name = "chain-direct";
  direct_tex.debug_tpage_name = "chain-page";
  direct_tex.data.resize(16 * 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      bool right = x >= 8;
      bool bottom = y >= 8;
      u32 r = (!right && !bottom) || (right && bottom) ? 255 : 0;
      u32 g = right ? 255 : 0;
      u32 b = bottom ? 255 : 0;
      u32 a = right ? 32 : 255;
      direct_tex.data[y * 16 + x] = (a << 24) | (b << 16) | (g << 8) | r;
    }
  }
  give_and_link_texture(mod, mem, direct_tex, page_direct, kChainTpageDirectTex, kVramDirectTex);

  // sky source: 32x32. Solid by default; with an .fr3 given, real texels from
  // an extracted sky texture drive the same path.
  tfrag3::Texture sky_src;
  sky_src.w = 32;
  sky_src.h = 32;
  sky_src.debug_name = "chain-sky-src";
  sky_src.debug_tpage_name = "chain-page";
  sky_src.data.resize(32 * 32, 0xff00c864u | (100u));  // a=255 b=0 g=200... set below
  for (auto& px : sky_src.data) {
    px = 0xffc82864u;  // a=255, b=200, g=40, r=100
  }
  bool used_real_sky = false;
  if (level) {
    for (const auto& t : level->textures) {
      if (t.debug_name.find("sky") != std::string::npos && t.w >= 32 && t.h >= 32) {
        for (int y = 0; y < 32; y++) {
          for (int x = 0; x < 32; x++) {
            sky_src.data[y * 32 + x] = t.data[y * t.w + x];
          }
        }
        printf("chain: sky-blend source uses real texture '%s' (%dx%d)\n", t.debug_name.c_str(),
               t.w, t.h);
        used_real_sky = true;
        break;
      }
    }
  }
  if (!used_real_sky) {
    printf("chain: sky-blend source uses synthetic texels\n");
  }
  give_and_link_texture(mod, mem, sky_src, page_sky, kChainTpageSkySrc, kVramSkySrc);

  // cloud source: 64x64 solid
  tfrag3::Texture cloud_src;
  cloud_src.w = 64;
  cloud_src.h = 64;
  cloud_src.debug_name = "chain-cloud-src";
  cloud_src.debug_tpage_name = "chain-page";
  cloud_src.data.resize(64 * 64, 0xff5ac81eu);  // a=255 b=90 g=200 r=30
  give_and_link_texture(mod, mem, cloud_src, page_cloud, kChainTpageCloudSrc, kVramCloudSrc);

  // a texture for the in-chain upload packet (links kVramChainUpload during
  // the chain walk itself)
  tfrag3::Texture upload_tex;
  upload_tex.w = 16;
  upload_tex.h = 16;
  upload_tex.debug_name = "chain-upload";
  upload_tex.debug_tpage_name = "chain-page";
  upload_tex.data.resize(16 * 16, 0xff112233u);
  upload_tex.combo_id = ((u32)page_upload << 16) | 0;
  upload_tex.load_to_pool = true;
  u64 upload_handle = metal_renderer::pool_add_texture(upload_tex, false);
  check(upload_handle != 0, "chain: upload-packet texture given to pool");
  write_fake_tpage(mem, kChainTpageDmaUpload, page_upload, kVramChainUpload,
                   kVramChainUpload + 0x20);

  // --- build the chain ------------------------------------------------------
  ChainBuilder chain(mem);

  // bucket 5 (TFRAG_TEX_LEVEL0): one PC-port texture upload packet
  {
    ChainBuilder::Transfer t;
    t.vif0 = (u32)VifCode::Kind::PC_PORT << 24;
    t.vif1 = 3;
    t.data.resize(16, 0);
    struct {
      u64 page;
      s64 mode;
    } packet{kChainTpageDmaUpload, -1};
    memcpy(t.data.data(), &packet, sizeof(packet));
    chain.set_bucket_content((int)BucketId::TFRAG_TEX_LEVEL0, {t});
  }

  // DEPTH_CUE (not ported): junk payload that MetalSkipRenderer must count
  // rather than silently drop
  {
    ChainBuilder::Transfer t;
    t.data.resize(64, 0xAB);
    chain.set_bucket_content((int)BucketId::DEPTH_CUE, {t});
  }

  // bucket 3 (SKY_DRAW): setup + qwc-5 draw setup + one draw packet
  {
    GifBuilder setup;  // 4 qw
    setup.tag(3, true, {GifTag::RegisterDescriptor::AD});
    setup.ad(GsRegisterAddress::TEST_1,
             gs_test(false, GsTest::AlphaTest::ALWAYS, 0, GsTest::AlphaFail::KEEP, true,
                     GsTest::ZTest::ALWAYS));
    setup.ad(GsRegisterAddress::ZBUF_1, gs_zbuf(448, true));  // no depth writes
    setup.ad(GsRegisterAddress::ALPHA_1, gs_alpha(0, 1, 0, 1));

    u16 sky_prim = gs_prim(GsPrim::Kind::TRI_STRIP, true, true, false, false);
    GifBuilder draw_setup;  // 5 qw
    draw_setup.tag(4, true, {GifTag::RegisterDescriptor::AD});
    draw_setup.ad(GsRegisterAddress::TEX0_1,
                  gs_tex0(SKY_TEXTURE_VRAM_ADDRS[0], 1, 0, 5, 5, true, 0));
    draw_setup.ad(GsRegisterAddress::TEX1_1, gs_tex1_filt());
    draw_setup.ad(GsRegisterAddress::CLAMP_1, 0b101);
    draw_setup.ad(GsRegisterAddress::PRIM, sky_prim);

    GifBuilder draw;  // 13 qw: full sky quad as a 4-vertex strip
    draw.tag(4, true,
             {GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::RGBAQ,
              GifTag::RegisterDescriptor::XYZF2},
             true, sky_prim);
    const u32 z_sky = 0x100000;
    struct V {
      double x, y;
      float s, t;
    } verts[4] = {{1936, 1980, 0.f, 0.f},
                  {2160, 1980, 1.f, 0.f},
                  {1936, 2120, 0.f, 1.f},
                  {2160, 2120, 1.f, 1.f}};
    for (auto& v : verts) {
      draw.st(v.s, v.t, 1.f);
      draw.rgbaq(0x80, 0x80, 0x80, 0x80);
      draw.xyzf2(fx(v.x), fx(v.y), z_sky);
    }

    std::vector<ChainBuilder::Transfer> transfers;
    transfers.push_back({0, 0, setup.data, false});
    transfers.push_back({0, 0, draw_setup.data, false});
    transfers.push_back({vif_nop(), vif_direct((u32)draw.data.size() / 16), draw.data, false});
    transfers.push_back({0, 0, {}, true});  // the "empty" hop before the CALL tail
    chain.set_bucket_content((int)BucketId::SKY_DRAW, transfers);
  }

  // bucket 32 (sky blend): set-display, sky draw+blend, cloud draw, resets
  {
    auto adgif_pair = [&](u32 src_tbp, bool abe, u32 intensity,
                          u32 coord) -> std::vector<ChainBuilder::Transfer> {
      GifBuilder setup;  // 6 qw: giftag prefix + 5 qw adgif
      setup.tag(5, true, {GifTag::RegisterDescriptor::AD});
      setup.ad(GsRegisterAddress::TEX0_1, gs_tex0(src_tbp, 1, 0, 5, 5, true, 0));
      setup.ad(GsRegisterAddress::TEX1_1, gs_tex1_filt());
      setup.ad(GsRegisterAddress::MIPTBP1_1, 0);
      setup.ad(GsRegisterAddress::CLAMP_1, 0b101);
      setup.ad(GsRegisterAddress::ALPHA_1, 0x8000000068);  // Cs + Cd

      GifBuilder draw;  // 6 qw; only the fields SkyBlendCPU reads matter
      draw.tag(5, true, {GifTag::RegisterDescriptor::AD}, true,
               gs_prim(GsPrim::Kind::SPRITE, false, true, abe, true));
      draw.ad((GsRegisterAddress)0, intensity);  // data+16: intensity
      draw.ad((GsRegisterAddress)0, 0);
      draw.ad((GsRegisterAddress)0, 0);
      draw.ad((GsRegisterAddress)0, 0);
      draw.ad((GsRegisterAddress)0, coord);  // data+80: draw coordinate
      return {{0, 0, setup.data, false}, {0, 0, draw.data, false}};
    };

    std::vector<ChainBuilder::Transfer> transfers;
    transfers.push_back({0, 0, std::vector<u8>(8 * 16, 0), false});  // set-display-gs-state
    for (auto& t : adgif_pair(kVramSkySrc, false, 128, 0x200)) {
      transfers.push_back(t);  // sky first draw
    }
    for (auto& t : adgif_pair(kVramSkySrc, true, 64, 0x200)) {
      transfers.push_back(t);  // sky blend, same source at half intensity
    }
    for (auto& t : adgif_pair(kVramCloudSrc, false, 128, 0x400)) {
      transfers.push_back(t);  // cloud first draw
    }
    transfers.push_back({0, 0, std::vector<u8>(2 * 16, 0), false});  // reset alpha
    transfers.push_back({0, 0, std::vector<u8>(8 * 16, 0), false});  // reset gs
    transfers.push_back({0, 0, {}, true});                           // empty hop
    chain.set_bucket_content((int)BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0, transfers);
  }

  // bucket 67 (DEBUG, DirectRenderer): state setup, strips, sprites, alpha
  // test, scissor
  {
    u16 strip_prim = gs_prim(GsPrim::Kind::TRI_STRIP, true, false, false, false);
    u16 strip_prim_abe = gs_prim(GsPrim::Kind::TRI_STRIP, true, false, true, false);
    u16 sprite_prim = gs_prim(GsPrim::Kind::SPRITE, false, true, false, true);

    auto strip_quad = [&](GifBuilder& g, u16 prim, double x0, double y0, double x1, double y1,
                          u32 z, u8 r, u8 gg, u8 b, u8 a) {
      g.tag(4, true, {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::XYZF2}, true,
            prim);
      const double xs[4] = {x0, x1, x0, x1};
      const double ys[4] = {y0, y0, y1, y1};
      for (int i = 0; i < 4; i++) {
        g.rgbaq(r, gg, b, a);
        g.xyzf2(fx(xs[i]), fx(ys[i]), z);
      }
    };

    // T1: AD state: GEQUAL depth, depth writes on, standard alpha regs, and
    // the GS scissor (set up front - the per-vertex scissor is captured when
    // vertices are pushed, exactly like the GL renderer)
    GifBuilder t1;
    t1.tag(4, true, {GifTag::RegisterDescriptor::AD});
    t1.ad(GsRegisterAddress::TEST_1,
          gs_test(false, GsTest::AlphaTest::ALWAYS, 0, GsTest::AlphaFail::KEEP, true,
                  GsTest::ZTest::GEQUAL));
    t1.ad(GsRegisterAddress::ZBUF_1, gs_zbuf(448, false));
    t1.ad(GsRegisterAddress::ALPHA_1, gs_alpha(0, 1, 0, 1));
    // scax 0..511, scay 0..335 (full-height GS coordinates)
    t1.ad(GsRegisterAddress::SCISSOR_1, 0ull | (511ull << 16) | (0ull << 32) | (335ull << 48));

    // T2: quad A, opaque
    GifBuilder t2;
    strip_quad(t2, strip_prim, 1920, 1992, 2176, 2104, 0x600000, 200, 60, 20, 0x80);
    // T3: quad B behind A (GEQUAL rejects it)
    GifBuilder t3;
    strip_quad(t3, strip_prim, 1984, 2020, 2112, 2076, 0x200000, 0, 255, 0, 0x80);
    // T4: alpha-blended quad over A
    GifBuilder t4;
    strip_quad(t4, strip_prim_abe, 2080, 2010, 2160, 2060, 0x700000, 0, 0, 255, 0x40);

    // T5: texture state for the sprites
    GifBuilder t5;
    t5.tag(3, true, {GifTag::RegisterDescriptor::AD});
    t5.ad(GsRegisterAddress::TEX0_1, gs_tex0(kVramDirectTex, 1, 0, 4, 4, true, 0));
    t5.ad(GsRegisterAddress::TEX1_1, gs_tex1_filt());
    t5.ad(GsRegisterAddress::CLAMP_1, 0b101);

    auto sprite = [&](GifBuilder& g, double x0, double y0, double x1, double y1, u32 z) {
      g.tag(2, true,
            {GifTag::RegisterDescriptor::RGBAQ, GifTag::RegisterDescriptor::UV,
             GifTag::RegisterDescriptor::XYZF2},
            true, sprite_prim);
      g.rgbaq(0x80, 0x80, 0x80, 0x80);
      g.uv(0, 0);
      g.xyzf2(fx(x0), fx(y0), z);
      g.rgbaq(0x80, 0x80, 0x80, 0x80);
      g.uv(fx(16), fx(16));  // full 16x16 texture in 12.4 texels
      g.xyzf2(fx(x1), fx(y1), z);
    };

    // T6: textured sprite, no alpha test (top right)
    GifBuilder t6;
    sprite(t6, 2176, 1936, 2288, 1992, 0x680000);

    // T7: enable alpha test GREATER aref 0x40 (keep on fail)
    GifBuilder t7;
    t7.tag(1, true, {GifTag::RegisterDescriptor::AD});
    t7.ad(GsRegisterAddress::TEST_1,
          gs_test(true, GsTest::AlphaTest::GREATER, 0x40, GsTest::AlphaFail::KEEP, true,
                  GsTest::ZTest::GEQUAL));

    // T8: alpha-tested sprite over A: left half kept, right half discarded
    GifBuilder t8;
    sprite(t8, 1936, 2080, 2064, 2100, 0x780000);

    // T9: a strip crossing scay1 - the scissor from T1 clips its bottom
    GifBuilder t9;
    strip_quad(t9, strip_prim, 1824, 2048, 1888, 2148, 0x600000, 250, 240, 10, 0x80);

    // T10+T11+T12: afail FB_ONLY (the double-draw path): alpha-failing
    // fragments write color but not depth. The sprite draws on both halves;
    // the following lower-z quad only lands where depth was not written.
    GifBuilder t10;
    t10.tag(1, true, {GifTag::RegisterDescriptor::AD});
    t10.ad(GsRegisterAddress::TEST_1,
           gs_test(true, GsTest::AlphaTest::GEQUAL, 0xFF, GsTest::AlphaFail::FB_ONLY, true,
                   GsTest::ZTest::GEQUAL));
    GifBuilder t11;
    sprite(t11, 2200, 1990, 2264, 2010, 0x600000);
    GifBuilder t12;
    strip_quad(t12, strip_prim, 2200, 1990, 2264, 2010, 0x300000, 20, 180, 220, 0x80);

    std::vector<ChainBuilder::Transfer> transfers;
    for (GifBuilder* g : {&t1, &t2, &t3, &t4, &t5, &t6, &t7, &t8, &t9, &t10, &t11, &t12}) {
      transfers.push_back({vif_nop(), vif_direct((u32)g->data.size() / 16), g->data, false});
    }
    chain.set_bucket_content((int)BucketId::DEBUG, transfers);
  }

  // --- send the chain like the game does, twice (the sky texture blended in
  // frame N is drawn in frame N+1, since the sky-draw bucket precedes the
  // blend bucket) ------------------------------------------------------------
  for (int frame = 0; frame < 2; frame++) {
    mod->send_chain(mem.data(), kChainStart);
    display->render();
  }

  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    printf("[FAIL] could not read back chain frame\n");
    g_fail_count++;
    return;
  }
  printf("[PASS] read back %dx%d chain frame\n", frame.width, frame.height);

  // --- expected sky blend, computed with the GL SkyBlendCPU semantics -------
  auto blend_expect = [](const std::vector<u32>& src, int n) {
    std::vector<u8> out(n * 4);
    const u8* in = (const u8*)src.data();
    for (int i = 0; i < n * 4; i++) {
      u32 first = ((u32)in[i] * 128) >> 7;
      u32 second = std::min<u32>(255, ((u32)in[i] * 64) >> 7);
      out[i] = (u8)std::min<u32>(255, std::min<u32>(first, 255) + second);
    }
    return out;
  };
  auto cloud_expect = [](const std::vector<u32>& src, int n) {
    std::vector<u8> out(n * 4);
    const u8* in = (const u8*)src.data();
    for (int i = 0; i < n * 4; i++) {
      out[i] = (u8)std::min<u32>(255, ((u32)in[i] * 128) >> 7);
    }
    return out;
  };

  // the pool's sky slots hold the blended textures
  auto sky_out = pool->lookup(SKY_TEXTURE_VRAM_ADDRS[0]);
  check(sky_out.has_value(), "chain: blended sky texture is in the sky VRAM slot");
  if (sky_out) {
    check_gpu_matches(*sky_out, blend_expect(sky_src.data, 32 * 32), 32, 32,
                      "chain: sky blend (draw + accumulate)");
  }
  auto cloud_out = pool->lookup(SKY_TEXTURE_VRAM_ADDRS[1]);
  check(cloud_out.has_value(), "chain: blended cloud texture is in the cloud VRAM slot");
  if (cloud_out) {
    check_gpu_matches(*cloud_out, cloud_expect(cloud_src.data, 64 * 64), 64, 64,
                      "chain: cloud blend (draw)");
  }

  // --- frame pixel checks ---------------------------------------------------
  // sky draw: modulate by rgba 0x80 leaves the texture color; sample points
  // outside the quads drawn on top of it
  std::vector<u8> sky_px = blend_expect(sky_src.data, 32 * 32);
  // texel sampled at (300,100): u ~ (300-250)/250 of the quad; with a solid
  // synthetic source every texel matches. With a real sky texture, compute the
  // exact texel: quad covers cols 250..500, rows 94..394, mapped to 32x32.
  auto sky_texel_at = [&](int col, int row, int* r, int* g, int* b) {
    int c0 = gs_to_col(1936), c1 = gs_to_col(2160);
    int r0 = gs_to_row(1980), r1 = gs_to_row(2120);
    double u = (col + 0.5 - c0) / double(c1 - c0);
    double v = (row + 0.5 - r0) / double(r1 - r0);
    // linear-filtered lookup is approximated by the nearest texel; solid
    // sources are exact, real sources get pixel tolerance
    int tx = std::clamp((int)(u * 32.0), 0, 31);
    int ty = std::clamp((int)(v * 32.0), 0, 31);
    const u8* px = &sky_px[(ty * 32 + tx) * 4];
    *r = px[0];
    *g = px[1];
    *b = px[2];
  };
  {
    int r, g, b;
    sky_texel_at(300, 100, &r, &g, &b);
    check_pixel(frame, 300, 100, r, g, b, "chain: sky draw above quad A");
    sky_texel_at(300, 380, &r, &g, &b);
    check_pixel(frame, 300, 380, r, g, b, "chain: sky draw below quad A");
  }

  // direct renderer content
  check_pixel(frame, 5, 5, 0, 0, 0, "chain: clear color outside all geometry");
  check_pixel(frame, 200, 240, 200, 60, 20, "chain: opaque quad A");
  check_pixel(frame, 300, 240, 200, 60, 20, "chain: quad B rejected by GEQUAL depth");
  check_pixel(frame, 400, 200, 100, 30, 137, "chain: alpha-blended quad over A");
  check_pixel(frame, 510, 30, 255, 0, 0, "chain: sprite TL texel (red)");
  check_pixel(frame, 590, 30, 0, 255, 0, "chain: sprite TR texel (green)");
  check_pixel(frame, 510, 90, 0, 0, 255, "chain: sprite BL texel (blue)");
  check_pixel(frame, 220, 340, 0, 0, 255, "chain: alpha-tested sprite left half kept (blue)");
  check_pixel(frame, 300, 340, 200, 60, 20, "chain: alpha-tested sprite right half discarded");
  check_pixel(frame, 60, 300, 250, 240, 10, "chain: scissored strip inside scissor");
  check_pixel(frame, 60, 420, 0, 0, 0, "chain: scissored strip clipped below scay1");
  // FB_ONLY double draw: sprite covers cols 510..590, rows 116..159; the
  // alpha-passing left half wrote depth (cover quad rejected), the failing
  // right half wrote only color (cover quad wins)
  check_pixel(frame, 525, 140, 0, 0, 255, "chain: FB_ONLY pass half keeps sprite + depth");
  check_pixel(frame, 575, 140, 20, 180, 220, "chain: FB_ONLY fail half has no depth (covered)");

  // in-chain texture upload packet linked its VRAM slot
  auto upload_slot = pool->lookup(kVramChainUpload);
  check(upload_slot.has_value() && *upload_slot == upload_handle,
        "chain: texture bucket upload packet linked the VRAM slot");

  // chain counters
  auto stats = metal_renderer::get_chain_stats();
  printf(
      "chain stats: %llu chains, %d draws, %d tris, %d uploads, sky d/b %d/%d, cloud d/b %d/%d, "
      "skipped %llu bucket + %llu tfrag bytes, %d unsupported blends\n",
      (unsigned long long)stats.chains_rendered, stats.draw_calls, stats.triangles,
      stats.tex_uploads, stats.sky_draws, stats.sky_blends, stats.cloud_draws, stats.cloud_blends,
      (unsigned long long)stats.skipped_bucket_bytes, (unsigned long long)stats.skipped_tfrag_bytes,
      stats.direct_unsupported_blends);
  check(stats.chains_rendered == 2, "chain: two chains rendered");
  check(stats.draw_calls >= 6, "chain: bucket draws were encoded");
  check(stats.tex_uploads == 1, "chain: texture bucket found one upload packet");
  check(stats.sky_draws == 1 && stats.sky_blends == 1, "chain: sky blended once per frame");
  check(stats.cloud_draws == 1 && stats.cloud_blends == 0, "chain: cloud drawn once per frame");
  check(stats.skipped_bucket_bytes == 2 * 64, "chain: un-ported bucket content counted (64B x2)");
  check(stats.direct_unsupported_blends == 0, "chain: no unsupported blend modes hit");

  g_ee_main_mem = nullptr;
}

// ---------------------------------------------------------------------------
// Section: the sprite bucket. Builds the Jak 1 sprite DMA the way the game
// does - distorter GS setup + sine tables, sprite frame data, the 3D matrix,
// world-space (group 0) chunks, the fake-shadow flush, then HUD (group 1)
// chunks - and verifies the rendered quads by pixel readback.
//
// Constructed data again: every packet matches what Sprite3 asserts about a
// real chain. The VU constants are chosen so the transform reduces to the same
// GS-space -> screen mapping the DirectRenderer test already uses
// (gs_to_col / gs_to_row), which makes the expected quad corners exact.
// ---------------------------------------------------------------------------

u32 vif_code(VifCode::Kind kind, u16 imm, u8 num = 0) {
  return ((u32)kind << 24) | ((u32)num << 16) | imm;
}
u32 vif_stcycl(u16 cl, u16 wl) {
  return vif_code(VifCode::Kind::STCYCL, (u16)(cl | (wl << 8)));
}
u32 vif_unpack_v4_32(u32 qwc, u32 addr, bool flg) {
  return vif_code(VifCode::Kind::UNPACK_V4_32, (u16)(addr | (flg ? (1 << 15) : 0)), (u8)qwc);
}

// VRAM slots and tpages for the sprite test
constexpr u32 kVramSpriteSolid = 0x7a0;
constexpr u32 kVramSpriteQuad = 0x7c0;
constexpr u32 kSpriteTpageSolid = kEeBase + 0x14000;
constexpr u32 kSpriteTpageQuad = kEeBase + 0x15000;

void push_f(std::vector<u8>& v, float f) {
  size_t at = v.size();
  v.resize(at + 4);
  memcpy(&v[at], &f, 4);
}
void push_i(std::vector<u8>& v, s32 i) {
  size_t at = v.size();
  v.resize(at + 4);
  memcpy(&v[at], &i, 4);
}
void push_u64(std::vector<u8>& v, u64 x) {
  size_t at = v.size();
  v.resize(at + 8);
  memcpy(&v[at], &x, 8);
}

// one sprite's worth of sprite-vec-data-2d (48 bytes)
void push_vec_data(std::vector<u8>& v,
                   float px,
                   float py,
                   float pz,
                   float sx,
                   s32 flag,
                   s32 matrix,
                   float rot,
                   float sy,
                   float r,
                   float g,
                   float b,
                   float a) {
  push_f(v, px);
  push_f(v, py);
  push_f(v, pz);
  push_f(v, sx);
  push_i(v, flag);
  push_i(v, matrix);
  push_f(v, rot);
  push_f(v, sy);
  push_f(v, r);
  push_f(v, g);
  push_f(v, b);
  push_f(v, a);
}

// one sprite's adgif shader (80 bytes)
void push_adgif(std::vector<u8>& v, u32 tbp, bool tcc, bool filt) {
  push_u64(v, gs_tex0(tbp, 1, 0, 4, 4, tcc, 0));  // psm PSMCT32, 16x16, MODULATE
  push_u64(v, (u64)GsRegisterAddress::TEX0_1);
  push_u64(v, filt ? gs_tex1_filt() : 0ull);
  push_u64(v, (u64)GsRegisterAddress::TEX1_1);
  push_u64(v, 0);
  push_u64(v, (u64)GsRegisterAddress::MIPTBP1_1);
  push_u64(v, 0b101);  // clamp s and t
  push_u64(v, (u64)GsRegisterAddress::CLAMP_1);
  push_u64(v, gs_alpha(0, 1, 0, 1));  // SOURCE, DEST, SOURCE, DEST
  push_u64(v, (u64)GsRegisterAddress::ALPHA_1);
}

// The sprite frame data (SpriteFrameDataJak1, 0x290 bytes). The constants are
// picked so that a sprite at GS position p with scale S covers the GS-space
// square p +- S: identity basis, no rotation, no perspective, no fade.
std::vector<u8> make_sprite_frame_data(float sprite_half_size) {
  std::vector<u8> d;
  // xy_array[8]: corner offsets for vertex ids 0..3 (the second four are the
  // flag != 0 variants, unused here but must exist)
  const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  for (int i = 0; i < 8; i++) {
    push_f(d, corners[i & 3][0]);
    push_f(d, corners[i & 3][1]);
    push_f(d, 0);
    push_f(d, 0);
  }
  // st_array[4]: uv per vertex id, matching the corners above
  const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  for (int i = 0; i < 4; i++) {
    push_f(d, uvs[i][0]);
    push_f(d, uvs[i][1]);
    push_f(d, 1.f);
    push_f(d, 0.f);
  }
  // xyz_array[4]: 3D sprite corner offsets. x is scaled by sx and z by sy, y is
  // used as-is, so the y offsets carry the size directly.
  for (int i = 0; i < 4; i++) {
    push_f(d, corners[i][0]);
    push_f(d, corners[i][1] * sprite_half_size);
    push_f(d, 0.f);
    push_f(d, 0.f);
  }
  push_f(d, 1.f);  // hmge_scale
  push_f(d, 1.f);
  push_f(d, 1.f);
  push_f(d, 1.f);
  push_f(d, 1.f);     // pfog0: no perspective divide
  push_f(d, 0.f);     // deg_to_rad: rotation is zero anyway
  push_f(d, 0.f);     // min_scale
  push_f(d, 1000.f);  // inv_area: the area fade saturates at 1
  for (int i = 0; i < 3; i++) {
    push_u64(d, 0);  // adgif / sprite_2d / sprite_2d_2 giftags
    push_u64(d, 0);
  }
  for (int i = 0; i < 5; i++) {  // sincos[5]
    push_f(d, 0);
    push_f(d, 0);
    push_f(d, 0);
    push_f(d, 0);
  }
  push_f(d, 1.f);  // basis_x
  push_f(d, 0.f);
  push_f(d, 0.f);
  push_f(d, 0.f);
  push_f(d, 0.f);  // basis_y
  push_f(d, 1.f);
  push_f(d, 0.f);
  push_f(d, 0.f);
  push_u64(d, 0);  // sprite_3d_giftag
  push_u64(d, 0);
  d.resize(d.size() + 5 * 16, 0);  // screen_shader (AdGifData)
  push_u64(d, 0);                  // clipped_giftag
  push_u64(d, 0);
  d.resize(d.size() + 4 * 16, 0);  // inv_hmge_scale, stq_offset, stq_scale, rgba_plain
  push_u64(d, 0);                  // warp_giftag
  push_u64(d, 0);
  push_f(d, 0.f);     // fog_min
  push_f(d, 1000.f);  // fog_max
  push_f(d, 1000.f);  // max_scale
  push_f(d, 0.f);     // bonus
  ASSERT(d.size() == 0x290);
  return d;
}

// World-space 2D sprites transform through `camera`, 3D sprites through
// `-camera`, so a frame that leaves positions unchanged uses +identity for the
// 2D pass and -identity for the 3D pass. (A real frame's camera is a view
// matrix that works for both; this test drives one mode per frame so each
// expected position stays exact.)
std::vector<u8> make_3d_matrix_data(float sign) {
  std::vector<u8> d;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      push_f(d, col == row ? sign : 0.f);
    }
  }
  for (int i = 0; i < 4; i++) {
    push_f(d, 0.f);  // hvdf_offset
  }
  ASSERT(d.size() == 5 * 16);
  return d;
}

// HUD matrix data: identity matrix, zero hvdf offset, and one user hvdf entry
// that shifts a sprite left by 64 GS units.
std::vector<u8> make_hud_matrix_data(float user0_x_shift) {
  std::vector<u8> d;
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      push_f(d, col == row ? 1.f : 0.f);
    }
  }
  for (int i = 0; i < 4; i++) {
    push_f(d, 0.f);  // hvdf_offset
  }
  for (int i = 0; i < 75; i++) {
    push_f(d, i == 0 ? user0_x_shift : 0.f);
    push_f(d, 0.f);
    push_f(d, 0.f);
    push_f(d, 0.f);
  }
  ASSERT(d.size() == 80 * 16);
  return d;
}

void test_sprite_chain(const GfxRendererModule* mod, std::shared_ptr<GfxDisplay>& display) {
  printf("--- DMA chain: sprite bucket -> MetalSpriteRenderer ---\n");
  using namespace jak1;

  auto pages = find_tpages(8);
  if (pages.size() < 8) {
    printf("[FAIL] not enough Jak 1 tpages for the sprite test\n");
    g_fail_count++;
    return;
  }
  u16 page_solid = pages[5], page_quad = pages[6];

  std::vector<u8> mem(kEeSize, 0);
  g_ee_main_mem = mem.data();

  // solid texture for the world-space and 3D sprites
  tfrag3::Texture solid;
  solid.w = 16;
  solid.h = 16;
  solid.debug_name = "sprite-solid";
  solid.debug_tpage_name = "sprite-page";
  solid.data.resize(16 * 16, 0xff3264c8u);  // a=255 b=50 g=100 r=200
  give_and_link_texture(mod, mem, solid, page_solid, kSpriteTpageSolid, kVramSpriteSolid);

  // quadrant texture for the HUD sprites: TL red, TR green, BL blue, BR white
  tfrag3::Texture quad;
  quad.w = 16;
  quad.h = 16;
  quad.debug_name = "sprite-quad";
  quad.debug_tpage_name = "sprite-page";
  quad.data.resize(16 * 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      bool right = x >= 8, bottom = y >= 8;
      u32 r = (!right && !bottom) || (right && bottom) ? 255 : 0;
      u32 g = right ? 255 : 0;
      u32 b = bottom ? 255 : 0;
      quad.data[y * 16 + x] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
  }
  give_and_link_texture(mod, mem, quad, page_quad, kSpriteTpageQuad, kVramSpriteQuad);

  constexpr float kHalf = 32.f;      // sprite half-size, GS units
  constexpr float kZ = 8388608.f;    // depth 0.5 in Metal's [0, 1] clip range
  constexpr float kUserShift = -64;  // hud_hvdf_user[0] x offset

  constexpr float kHalfPxX = kHalf * 640.f / 512.f;  // GS x unit -> pixels
  constexpr float kHalfPxY = kHalf * 480.f / 224.f;  // GS y unit -> pixels
  (void)kHalfPxX;
  (void)kHalfPxY;

  // Builds one sprite bucket. `three_d` selects which mode the frame drives:
  // the 2D/HUD frame uses a +identity camera, the 3D frame a -identity one.
  auto build_sprite_bucket = [&](bool three_d) {
    std::vector<ChainBuilder::Transfer> sprite;
    // the NEXT hop into the sprite data
    sprite.push_back({0, 0, {}, true});

    // distorter GS setup: giftag(nloop 1, eop, nreg 6, AD) + 6 A+D pairs
    {
      GifBuilder g;
      g.tag(1, true, std::vector<GifTag::RegisterDescriptor>(6, GifTag::RegisterDescriptor::AD));
      g.push_qw(gs_zbuf(0x1c0, true), (u64)GsRegisterAddress::ZBUF_1);
      g.push_qw(gs_tex0(0, 8, 0, 9, 8, false, 0), (u64)GsRegisterAddress::TEX0_1);
      g.push_qw(gs_tex1_filt(), (u64)GsRegisterAddress::TEX1_1);
      g.push_qw(0, (u64)GsRegisterAddress::MIPTBP1_1);
      g.push_qw(0, (u64)GsRegisterAddress::CLAMP_1);
      g.push_qw(gs_alpha(0, 1, 0, 1), (u64)GsRegisterAddress::ALPHA_1);
      ASSERT(g.data.size() == 7 * 16);  // the giftag is one of the 7 quadwords
      sprite.push_back({vif_nop(), vif_direct(7), g.data, false});
    }
    // sine-table aspect (PC only)
    sprite.push_back(
        {vif_nop(), vif_code(VifCode::Kind::PC_PORT, 0), std::vector<u8>(16, 0), false});
    // sine tables: the only field the renderer checks is the gs giftag's prim
    {
      std::vector<u8> tables(0x8b * 16, 0);
      // the giftag's PRIM field lives at bit 47 (see GifTag in common/dma/gs.h)
      u64 giftag_lo = ((u64)GsPrim::Kind::TRI_STRIP) << 47;
      memcpy(&tables[(128 + 9) * 16], &giftag_lo, 8);
      sprite.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(0x8b, 0x160, false), tables, false});
    }

    // sprite frame setup: direct data, frame data, mscalf, base/offset
    {
      std::vector<u8> direct_setup;
      push_u64(direct_setup, 0x2000000000008001ull);
      push_u64(direct_setup, 0xEEEEEEEEEEEEEEEEull);
      push_u64(direct_setup, 0x000000000005126Bull);
      push_u64(direct_setup, 0x0000000000000047ull);
      push_u64(direct_setup, 0x0000000000000005ull);
      push_u64(direct_setup, 0x0000000000000008ull);
      sprite.push_back({vif_nop(), vif_direct(3), direct_setup, false});
    }
    sprite.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(0x29, 980, false),
                      make_sprite_frame_data(kHalf), false});
    sprite.push_back(
        {vif_code(VifCode::Kind::MSCALF, 0), vif_code(VifCode::Kind::FLUSHE, 0), {}, false});
    sprite.push_back(
        {vif_code(VifCode::Kind::BASE, 0), vif_code(VifCode::Kind::OFFSET, 400), {}, false});

    // 3D matrix data (the camera the 2D and 3D paths share)
    sprite.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(5, 900, false),
                      make_3d_matrix_data(three_d ? -1.f : 1.f), false});

    auto push_chunk = [&](int count, int prog, const std::vector<u8>& vecs,
                          const std::vector<u8>& adgifs) {
      std::vector<u8> header(16, 0);
      header[0] = (u8)count;
      sprite.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(1, 0, true), header, false});
      sprite.push_back({vif_nop(), vif_unpack_v4_32((u32)vecs.size() / 16, 1, true), vecs, false});
      sprite.push_back(
          {vif_nop(), vif_unpack_v4_32((u32)adgifs.size() / 16, 145, true), adgifs, false});
      sprite.push_back({vif_nop(), vif_code(VifCode::Kind::MSCAL, (u16)prog), {}, false});
    };

    if (three_d) {
      // group 0, 3D: one sprite at GS (2176, 1992)
      std::vector<u8> vecs, adgifs;
      push_vec_data(vecs, 2176, 1992, kZ, kHalf, 0, 0, 0, kHalf, 64, 64, 64, 64);
      push_adgif(adgifs, kVramSpriteSolid, true, false);
      push_chunk(1, SpriteProgMem::Sprites3d, vecs, adgifs);
    } else {
      // group 0, world-space 2D: one sprite at GS (1920, 1992)
      std::vector<u8> vecs, adgifs;
      push_vec_data(vecs, 1920, 1992, kZ, kHalf, 0, 0, 0, kHalf, 64, 64, 64, 64);
      push_adgif(adgifs, kVramSpriteSolid, true, false);
      push_chunk(1, SpriteProgMem::Sprites2dGrp0, vecs, adgifs);
    }

    // the fake-shadow flush ends group 0
    sprite.push_back({vif_nop(), vif_code(VifCode::Kind::FLUSHE, 0), {}, false});

    // group 1 (HUD): the matrix upload always happens; the 3D frame sends no
    // HUD sprites after it
    sprite.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(80, 900, false),
                      make_hud_matrix_data(kUserShift), false});
    if (!three_d) {
      std::vector<u8> vecs, adgifs;
      // matrix 0 -> hud_hvdf_offset, at GS (1920, 2104)
      push_vec_data(vecs, 1920, 2104, kZ, kHalf, 0, 0, 0, kHalf, 127, 127, 127, 64);
      push_adgif(adgifs, kVramSpriteQuad, true, false);
      // matrix 1 -> hud_hvdf_user[0], which shifts it left by 64 GS units
      push_vec_data(vecs, 2176, 2104, kZ, kHalf, 0, 1, 0, kHalf, 127, 127, 127, 64);
      push_adgif(adgifs, kVramSpriteQuad, true, false);
      push_chunk(2, SpriteProgMem::Sprites2dHud_Jak1, vecs, adgifs);
    }
    return sprite;
  };

  // ---- frame 1: world-space 2D + HUD --------------------------------------
  {
    ChainBuilder cb(mem);
    cb.set_bucket_content((int)BucketId::SPRITE, build_sprite_bucket(false));
    mod->send_chain(mem.data(), kChainStart);
    display->render();

    metal_renderer::FramePixels frame;
    if (!metal_renderer::read_last_frame(&frame)) {
      printf("[FAIL] could not read back the 2D/HUD sprite frame\n");
      g_fail_count++;
      g_ee_main_mem = nullptr;
      return;
    }
    auto stats = metal_renderer::get_chain_stats();
    printf("sprite stats (2d/hud frame): %d 2d + %d 3d + %d hud sprites in %d draws, %d missing\n",
           stats.sprites_2d, stats.sprites_3d, stats.sprites_hud, stats.sprite_draws,
           stats.sprite_missing_textures);
    check(stats.sprites_2d == 1, "sprite: one world-space 2D sprite");
    check(stats.sprites_hud == 2, "sprite: two HUD sprites");
    check(stats.sprite_missing_textures == 0, "sprite: every bucket found its texture");
    check(stats.sprite_draws == 2, "sprite: one draw per (texture, mode) bucket");

    // The shader doubles the vertex color (and doubles alpha again), so a
    // vertex rgba of 64 is 0.502 with alpha 1.004 (opaque). The solid texture
    // is (200, 100, 50), so the 2D quad reads back (100, 50, 25).
    const int col_2d = gs_to_col(1920), row_2d = gs_to_row(1992);
    check_pixel(frame, col_2d, row_2d, 100, 50, 25, "sprite: world-space 2D quad center");
    // the quad spans +-32 GS units: +-40 px in x, +-68 px in y
    check_pixel(frame, col_2d - 35, row_2d, 100, 50, 25, "sprite: 2D quad left edge inside");
    check_pixel(frame, col_2d - 45, row_2d, 0, 0, 0, "sprite: 2D quad left edge outside");
    check_pixel(frame, col_2d, row_2d - 60, 100, 50, 25, "sprite: 2D quad top inside");
    check_pixel(frame, col_2d, row_2d - 75, 0, 0, 0, "sprite: 2D quad top outside");

    // HUD sprites carry the quadrant texture at vertex color 127 (~0.996)
    const int col_h0 = gs_to_col(1920), row_h0 = gs_to_row(2104);
    check_pixel(frame, col_h0 - 20, row_h0 - 34, 249, 0, 0, "sprite: HUD quad TL texel (red)");
    check_pixel(frame, col_h0 + 20, row_h0 - 34, 0, 249, 0, "sprite: HUD quad TR texel (green)");
    check_pixel(frame, col_h0 - 20, row_h0 + 34, 0, 0, 249, "sprite: HUD quad BL texel (blue)");
    check_pixel(frame, col_h0 + 20, row_h0 + 34, 249, 249, 249,
                "sprite: HUD quad BR texel (white)");
    // the second HUD sprite used hud_hvdf_user[0], which moved it left
    const int col_h1 = gs_to_col(2176 + kUserShift);
    check_pixel(frame, col_h1 - 20, row_h0 - 34, 249, 0, 0,
                "sprite: HUD user-hvdf quad moved left");
    check_pixel(frame, col_h1 + 20, row_h0 + 34, 249, 249, 249,
                "sprite: HUD user-hvdf quad BR texel");
    check_pixel(frame, gs_to_col(2176), row_h0, 0, 0, 0,
                "sprite: nothing left where the user-hvdf quad would have been");
  }

  // ---- frame 2: 3D sprites ------------------------------------------------
  {
    ChainBuilder cb(mem);
    cb.set_bucket_content((int)BucketId::SPRITE, build_sprite_bucket(true));
    mod->send_chain(mem.data(), kChainStart);
    display->render();

    metal_renderer::FramePixels frame;
    if (!metal_renderer::read_last_frame(&frame)) {
      printf("[FAIL] could not read back the 3D sprite frame\n");
      g_fail_count++;
      g_ee_main_mem = nullptr;
      return;
    }
    auto stats = metal_renderer::get_chain_stats();
    printf("sprite stats (3d frame): %d 2d + %d 3d + %d hud sprites in %d draws, %d missing\n",
           stats.sprites_2d, stats.sprites_3d, stats.sprites_hud, stats.sprite_draws,
           stats.sprite_missing_textures);
    check(stats.sprites_3d == 1, "sprite: one 3D sprite");
    check(stats.sprites_hud == 0, "sprite: no HUD sprites in the 3D frame");
    check(stats.sprite_draws == 1, "sprite: one 3D draw");

    const int col_3d = gs_to_col(2176), row_3d = gs_to_row(1992);
    check_pixel(frame, col_3d, row_3d, 100, 50, 25, "sprite: 3D quad center");
    check_pixel(frame, col_3d - 35, row_3d, 100, 50, 25, "sprite: 3D quad left edge inside");
    check_pixel(frame, col_3d - 45, row_3d, 0, 0, 0, "sprite: 3D quad left edge outside");
    check_pixel(frame, gs_to_col(1920), gs_to_row(2104), 0, 0, 0,
                "sprite: the 3D frame drew no HUD sprite");
  }

  g_ee_main_mem = nullptr;
}

// ---------------------------------------------------------------------------
// Section: the level-geometry stage's portable ports of background_common.
//
// The Metal background renderers cannot include the GL background_common.h
// (it pulls in the whole OpenGL renderer header chain), so the pure
// computations are re-implemented in metal_level_data.mm. This section runs the
// GL originals and the ports side by side on constructed data and requires
// byte-identical results, which is what keeps the ports honest. Note that the
// GL interp_time_of_day is hand-written SSE that reaches arm64 through
// sse2neon, so this also checks the portable version against real SIMD output.
// ---------------------------------------------------------------------------

void test_background_common_parity() {
  printf("--- level-geometry stage: background_common parity ---\n");

  // The DMA control block is duplicated by layout, so its size must match.
  check(metal_renderer::sizeof_pc_port_data_mirror() == sizeof(TfragPcPortData),
        "TfragPcPortData layout mirror has the same size as the GL struct");
  check(metal_renderer::sizeof_camera_data_mirror() == sizeof(GoalBackgroundCameraData),
        "GoalBackgroundCameraData layout mirror has the same size as the GL struct");

  // --- time of day ---
  // A packed palette is groups of 4 colors x 8 palettes x 4 channels. Fill one
  // with a spread of values, including the extremes that exercise the
  // saturating accumulation.
  constexpr u32 kColors = 256;
  tfrag3::PackedTimeOfDay packed;
  packed.color_count = kColors;
  packed.data.resize((kColors / 4) * 128);
  for (size_t i = 0; i < packed.data.size(); i++) {
    packed.data[i] = (u8)((i * 37 + (i >> 3) * 11) & 0xff);
  }

  // itimes pack 8 palette weights x 4 channels into 4 quadwords of s32.
  auto run_itimes = [&](const char* label, const math::Vector<s32, 4> itimes[4]) {
    std::vector<math::Vector<u8, 4>> gl_out(kColors), metal_out(kColors);
    memset(gl_out.data(), 0xcd, gl_out.size() * 4);
    memset(metal_out.data(), 0xcd, metal_out.size() * 4);
    interp_time_of_day(itimes, packed, gl_out.data());
    metal_renderer::interp_time_of_day_for_test(itimes, packed, metal_out.data());
    bool same = memcmp(gl_out.data(), metal_out.data(), kColors * 4) == 0;
    check(same, fmt::format("interp_time_of_day matches the GL/SSE version ({})", label).c_str());
    if (!same) {
      for (u32 i = 0; i < kColors; i++) {
        if (memcmp(&gl_out[i], &metal_out[i], 4) != 0) {
          printf("  first difference at color %d: GL %d,%d,%d,%d vs Metal %d,%d,%d,%d\n", i,
                 gl_out[i][0], gl_out[i][1], gl_out[i][2], gl_out[i][3], metal_out[i][0],
                 metal_out[i][1], metal_out[i][2], metal_out[i][3]);
          break;
        }
      }
    }
  };

  {
    // a plausible mid-blend: two palettes at half weight
    math::Vector<s32, 4> itimes[4];
    memset(itimes, 0, sizeof(itimes));
    itimes[0][0] = 0x00400040;
    itimes[0][1] = 0x00400040;
    itimes[1][0] = 0x00400040;
    itimes[1][1] = 0x00400040;
    run_itimes("half blend of two palettes", itimes);
  }
  {
    // every palette at full weight: the accumulation saturates
    math::Vector<s32, 4> itimes[4];
    for (int i = 0; i < 4; i++) {
      for (int j = 0; j < 4; j++) {
        itimes[i][j] = (s32)0x00ff00ff;
      }
    }
    run_itimes("all palettes saturated", itimes);
  }
  {
    // one palette only, and an odd set of weights per channel
    math::Vector<s32, 4> itimes[4];
    memset(itimes, 0, sizeof(itimes));
    itimes[2][0] = 0x0011007f;
    itimes[2][1] = 0x00030025;
    run_itimes("single palette, uneven channels", itimes);
  }

  // --- culling ---
  std::vector<tfrag3::VisNode> nodes(64);
  for (size_t i = 0; i < nodes.size(); i++) {
    auto& n = nodes[i];
    n.bsphere = math::Vector4f((float)(i % 8) * 3000.f - 12000.f, (float)(i / 8) * 2500.f,
                               (float)(i * 700) - 4000.f, 1500.f + (i % 5) * 400.f);
    n.my_id = (u16)(i * 3);  // sparse ids, like a real BVH
    n.child_id = 0;
    n.num_kids = 0;
    n.flags = 0;
  }
  nodes[7].my_id = 0xffff;  // the "no occlusion id" case

  math::Vector4f planes[4] = {
      math::Vector4f(0.6f, 0.1f, 0.79f, 0.f), math::Vector4f(-0.6f, 0.1f, 0.79f, 0.f),
      math::Vector4f(0.f, 0.75f, 0.66f, 0.f), math::Vector4f(0.f, -0.75f, 0.66f, 0.f)};
  // the fourth entry of the GL plane math is a per-plane offset row
  planes[3] = math::Vector4f(500.f, -500.f, 250.f, -250.f);

  std::vector<u8> occlusion(2048);
  for (size_t i = 0; i < occlusion.size(); i++) {
    occlusion[i] = (u8)((i * 73) & 0xff);
  }

  for (int with_occlusion = 0; with_occlusion < 2; with_occlusion++) {
    const u8* occ = with_occlusion ? occlusion.data() : nullptr;
    std::vector<u8> gl_out(nodes.size(), 0xcd), metal_out(nodes.size(), 0xcd);
    cull_check_all_slow(planes, nodes, occ, gl_out.data());
    metal_renderer::cull_check_all_slow_for_test(planes, nodes, occ, metal_out.data());
    check(gl_out == metal_out,
          fmt::format("cull_check_all_slow matches the GL version ({} occlusion string)",
                      with_occlusion ? "with" : "without")
              .c_str());
    int visible = 0;
    for (u8 v : metal_out) {
      visible += v ? 1 : 0;
    }
    // a meaningless test if everything (or nothing) passes
    check(visible > 0 && visible < (int)nodes.size(),
          fmt::format("cull test actually culls ({} of {} nodes visible)", visible,
                      (int)nodes.size())
              .c_str());
  }
}

// ---------------------------------------------------------------------------
// Section: the merc buckets. Builds the Jak 1 merc DMA the way the game does
// (the 10-quadword setup packet, then PC_PORT model packets carrying the model
// name, lights, the matrix-slot string, EE pointers to bone matrices, flags and
// fades), registers a synthetic level with the merc model pool for the geometry
// the chain does not carry, and verifies the result by pixel readback.
//
// Constructed data again: every packet matches what Merc2 asserts about a real
// chain, and the VU constants are chosen so the transform reduces to the same
// GS-space -> screen mapping the other chain tests use (gs_to_col/gs_to_row).
// Two instances of one model with different bone matrices land at different
// screen positions, which is what proves the per-draw bone-buffer offset.
// ---------------------------------------------------------------------------

constexpr const char* kMercModelName = "proof-merc";
constexpr float kMercQuadHalf = 32.f;  // GS units
constexpr float kMercZ = 8388608.f;    // depth 0.5 in Metal's [0, 1] clip range

// A level with one solid texture and one two-triangle model, in the layout the
// GL loader would have produced from an .fr3.
std::unique_ptr<tfrag3::Level> make_merc_test_level(bool with_envmap) {
  auto level = std::make_unique<tfrag3::Level>();
  level->level_name = "metal-proof-merc";

  tfrag3::Texture tex;
  tex.w = 16;
  tex.h = 16;
  tex.debug_name = "merc-solid";
  tex.debug_tpage_name = "merc-page";
  tex.load_to_pool = false;  // merc indexes the level's own texture array
  tex.data.resize(16 * 16, 0xff3264c8u);  // a=255 b=50 g=100 r=200
  level->textures.push_back(tex);

  // a quad as a triangle strip, in GS coordinates around the origin
  auto& merc = level->merc_data;
  merc.vertices.resize(4);
  const float xs[4] = {-kMercQuadHalf, kMercQuadHalf, -kMercQuadHalf, kMercQuadHalf};
  const float ys[4] = {-kMercQuadHalf, -kMercQuadHalf, kMercQuadHalf, kMercQuadHalf};
  for (int i = 0; i < 4; i++) {
    auto& v = merc.vertices[i];
    memset(&v, 0, sizeof(v));
    v.pos[0] = xs[i];
    v.pos[1] = ys[i];
    v.pos[2] = kMercZ;
    v.normal[2] = 1.f;
    v.weights[0] = 1.f;
    v.st[0] = 0.5f;
    v.st[1] = 0.5f;
    for (int j = 0; j < 4; j++) {
      v.rgba[j] = 128;
    }
    v.rgba[3] = 255;
    v.mats[0] = v.mats[1] = v.mats[2] = 0;
  }
  merc.indices = {0, 1, 2, 3};

  tfrag3::MercDraw draw;
  draw.mode.set_depth_write_enable(true);
  draw.mode.set_zt(true);
  draw.mode.set_depth_test(GsTest::ZTest::GEQUAL);
  draw.mode.set_ab(false);
  draw.mode.set_at(false);
  draw.mode.set_fog(false);
  draw.mode.set_decal(false);
  draw.mode.set_filt_enable(false);
  draw.mode.set_clamp_s_enable(true);
  draw.mode.set_clamp_t_enable(true);
  draw.tree_tex_id = 0;
  draw.eye_id = 0xff;
  draw.first_index = 0;
  draw.index_count = 4;
  draw.num_triangles = 2;
  draw.no_strip = false;

  tfrag3::MercEffect effect;
  effect.all_draws.push_back(draw);
  effect.has_envmap = with_envmap;
  effect.has_mod_draw = false;
  effect.envmap_texture = 0;
  if (with_envmap) {
    effect.envmap_mode = draw.mode;
    // the envmap pass is always this blend (Merc2::do_draws asserts it)
    effect.envmap_mode.set_ab(true);
    effect.envmap_mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
  }

  tfrag3::MercModel model;
  model.name = kMercModelName;
  model.effects.push_back(effect);
  model.max_draws = 1;
  model.max_bones = 1;
  model.st_vif_add = 0;
  model.xyz_scale = 1.f;
  model.st_magic = 0.f;
  merc.models.push_back(model);
  return level;
}

// The 10-quadword merc setup packet. The low-memory block holds an identity
// perspective matrix and an hvdf offset of 0, so a vertex position in GS
// coordinates lands where gs_to_col/gs_to_row say it does.
std::vector<u8> make_merc_setup_data() {
  std::vector<u8> d;
  // qw0: BASE, OFFSET, NOP, UNPACK_V4_32(8 qw to address 0)
  push_i(d, (s32)vif_code(VifCode::Kind::BASE, 442));
  push_i(d, (s32)vif_code(VifCode::Kind::OFFSET, (u16)(s16)-442));
  push_i(d, 0);
  push_i(d, (s32)vif_unpack_v4_32(8, 0, false));
  // qw1..8: LowMemory
  for (int i = 0; i < 8; i++) {  // tri_strip_tag + ad_gif_tag
    push_i(d, 0);
  }
  push_f(d, 0.f);  // hvdf_offset
  push_f(d, 0.f);
  push_f(d, 0.f);
  push_f(d, 0.f);
  for (int col = 0; col < 4; col++) {  // perspective: identity, column-major
    for (int row = 0; row < 4; row++) {
      push_f(d, col == row ? 1.f : 0.f);
    }
  }
  push_f(d, 1.f);    // fog: pfog0 = 1 so Q = 1
  push_f(d, 0.f);    // fog min
  push_f(d, 255.f);  // fog max
  push_f(d, 0.f);
  // qw9: FLUSHE, 0, 0, MSCAL 0
  push_i(d, (s32)vif_code(VifCode::Kind::FLUSHE, 0));
  push_i(d, 0);
  push_i(d, 0);
  push_i(d, (s32)vif_code(VifCode::Kind::MSCAL, 0));
  ASSERT(d.size() == 10 * 16);
  return d;
}

// One bone matrix, written into EE memory the way the game's `bones` does.
// tmat is -identity with a -(tx, ty) translation, so the shader's
// `-bones[i].X * p` is `p + (tx, ty, 0)`.
void write_merc_bone(std::vector<u8>& mem, u32 addr, float tx, float ty) {
  float m[7 * 4] = {};
  m[0] = -1.f;   // tmat column 0
  m[5] = -1.f;   // column 1
  m[10] = -1.f;  // column 2
  m[12] = -tx;   // column 3
  m[13] = -ty;
  m[15] = -1.f;
  m[16] = 1.f;  // nmat column 0
  m[21] = 1.f;  // column 1
  m[26] = 1.f;  // column 2
  memcpy(&mem[addr], m, sizeof(m));
}

// The PC_PORT model packet Merc2::handle_pc_model parses.
std::vector<u8> make_merc_model_packet(u32 bone0_addr, u32 bone1_addr, const u8 fade[4]) {
  std::vector<u8> d;
  // name (128 bytes)
  d.resize(128, 0);
  memcpy(d.data(), kMercModelName, strlen(kMercModelName));
  // lights (7 qw): directions and colors zero, ambient 0.5 -> light_color 0.5
  for (int i = 0; i < 3; i++) {  // direction0/1/2 + w
    push_f(d, 0.f);
    push_f(d, 0.f);
    push_f(d, 0.f);
    push_i(d, 0);
  }
  for (int i = 0; i < 3; i++) {  // color0/1/2
    for (int j = 0; j < 4; j++) {
      push_f(d, 0.f);
    }
  }
  for (int j = 0; j < 4; j++) {  // ambient
    push_f(d, 0.5f);
  }
  // jak 1 water flag quadword
  push_u64(d, 0);
  push_u64(d, 0);
  // matrix slot string (128 bytes): slots 0 and 1, then the 0xff terminator
  size_t slot_string = d.size();
  d.resize(slot_string + 128, 0);
  d[slot_string + 0] = 0;
  d[slot_string + 1] = 1;
  d[slot_string + 2] = 0xff;
  // matrix pointers: one quadword each, the EE address in the first word
  push_i(d, (s32)bone0_addr);
  push_i(d, 0);
  push_i(d, 0);
  push_i(d, 0);
  push_i(d, (s32)bone1_addr);
  push_i(d, 0);
  push_i(d, 0);
  push_i(d, 0);
  // flags (32 bytes): one enabled effect, no ignore-alpha, no mod/blerc
  push_u64(d, 1);  // enable_mask
  push_u64(d, 0);  // ignore_alpha_mask
  d.push_back(1);  // effect_count
  d.push_back(0);  // bitflags
  d.resize(d.size() + 14, 0);
  // fades: one effect, padded to a quadword
  for (int i = 0; i < 4; i++) {
    d.push_back(fade[i]);
  }
  d.resize(d.size() + 12, 0);
  ASSERT(d.size() % 16 == 0);
  return d;
}

void test_merc_chain(const GfxRendererModule* mod, std::shared_ptr<GfxDisplay>& display) {
  printf("--- DMA chain: merc buckets -> MetalMerc2 ---\n");
  using namespace jak1;

  // model geometry does not travel in the chain: it comes from the level, the
  // way the GL renderer gets it from the Loader
  {
    metal_renderer::MercLevelLoad load;
    std::string error;
    if (!metal_renderer::merc_add_level(make_merc_test_level(true), false, &load, &error)) {
      printf("[FAIL] merc: could not register the test level: %s\n", error.c_str());
      g_fail_count++;
      return;
    }
    check(load.models == 1 && load.vertices == 4 && load.indices == 4,
          "merc: test level registered with the model pool");
  }

  std::vector<u8> mem(kEeSize, 0);
  g_ee_main_mem = mem.data();

  // GS positions of the two model instances
  constexpr float kX0 = 1920.f, kY0 = 1992.f;
  constexpr float kX1 = 2176.f, kY1 = 2104.f;

  // Builds one merc bucket. `two_models` adds a second instance with its own
  // bone matrices; `fade` non-zero turns on the envmap (emerc) pass.
  auto build_merc_bucket = [&](ChainBuilder& cb, bool two_models, const u8 fade[4]) {
    u32 bone_a0 = cb.alloc(112), bone_a1 = cb.alloc(112);
    write_merc_bone(mem, bone_a0, kX0, kY0);
    write_merc_bone(mem, bone_a1, 0, 0);

    // the bucket's own NEXT tag is the "nothing" transfer merc reads first, so
    // the setup packet is the first transfer here
    std::vector<ChainBuilder::Transfer> merc;
    merc.push_back({vif_stcycl(4, 4), vif_code(VifCode::Kind::STMOD, 0), make_merc_setup_data(),
                    false});
    merc.push_back({0, 0, std::vector<u8>(32, 0), false});  // test register setup
    merc.push_back({0, 0, {}, true});

    merc.push_back({0, vif_code(VifCode::Kind::PC_PORT, 0),
                    make_merc_model_packet(bone_a0, bone_a1, fade), false});
    merc.push_back({0, 0, {}, true});
    merc.push_back({0, 0, {}, true});

    if (two_models) {
      u32 bone_b0 = cb.alloc(112), bone_b1 = cb.alloc(112);
      write_merc_bone(mem, bone_b0, kX1, kY1);
      write_merc_bone(mem, bone_b1, 0, 0);
      merc.push_back({0, vif_code(VifCode::Kind::PC_PORT, 0),
                      make_merc_model_packet(bone_b0, bone_b1, fade), false});
      merc.push_back({0, 0, {}, true});
      merc.push_back({0, 0, {}, true});
    }

    // a transfer that is neither PC_PORT nor FLUSHA ends the model loop
    merc.push_back({0, 0, {}, true});
    return merc;
  };

  const u8 no_fade[4] = {0, 0, 0, 0};
  const u8 half_fade[4] = {128, 128, 128, 255};

  // ---- frame 1: two instances of one model, no envmap ---------------------
  {
    ChainBuilder cb(mem);
    cb.set_bucket_content((int)BucketId::MERC_PRIS_LEVEL0, build_merc_bucket(cb, true, no_fade));
    mod->send_chain(mem.data(), kChainStart);
    display->render();

    metal_renderer::FramePixels frame;
    if (!metal_renderer::read_last_frame(&frame)) {
      printf("[FAIL] could not read back the merc frame\n");
      g_fail_count++;
      g_ee_main_mem = nullptr;
      return;
    }
    auto stats = metal_renderer::get_chain_stats();
    printf("merc stats: %d models (%d missing), %d draws (%d envmap), %d tris, %d bone vectors, "
           "%d deferred mod effects, %d eye draws, %d missing textures\n",
           stats.merc_models, stats.merc_missing_models, stats.merc_draws,
           stats.merc_envmap_draws, stats.merc_triangles, stats.merc_bone_vectors,
           stats.merc_mod_effects_deferred, stats.merc_eye_draws, stats.merc_missing_textures);
    check(stats.merc_models == 2, "merc: both model instances were built");
    check(stats.merc_missing_models == 0, "merc: every model name resolved in the model pool");
    check(stats.merc_draws == 2, "merc: one draw per instance");
    check(stats.merc_envmap_draws == 0, "merc: no envmap draws with a zero fade");
    check(stats.merc_triangles == 4, "merc: two triangles per instance");
    // 2 bones per instance, 8 vectors each, rounded up to the 16-vector
    // alignment the bone buffer views need
    check(stats.merc_bone_vectors == 32, "merc: bones allocated with the view alignment");

    // The shader path: vertex rgba 128/255 * ambient 0.5 = 0.251, times the
    // (200, 100, 50) texel, times 2 -> (100, 50, 25).
    const int col0 = gs_to_col(kX0), row0 = gs_to_row(kY0);
    check_pixel(frame, col0, row0, 100, 50, 25, "merc: first instance center");
    check_pixel(frame, col0 - 35, row0, 100, 50, 25, "merc: first instance left edge inside");
    check_pixel(frame, col0 - 45, row0, 0, 0, 0, "merc: first instance left edge outside");
    check_pixel(frame, col0, row0 - 60, 100, 50, 25, "merc: first instance top inside");
    check_pixel(frame, col0, row0 - 75, 0, 0, 0, "merc: first instance top outside");

    // the second instance only differs by its bone matrices, which is what the
    // per-draw bone-buffer offset selects
    const int col1 = gs_to_col(kX1), row1 = gs_to_row(kY1);
    check_pixel(frame, col1, row1, 100, 50, 25, "merc: second instance at its own bone offset");
    check_pixel(frame, col1, row0, 0, 0, 0, "merc: nothing between the two instances");
  }

  // ---- frame 2: one instance with the envmap (emerc) pass -----------------
  {
    ChainBuilder cb(mem);
    cb.set_bucket_content((int)BucketId::MERC_PRIS_LEVEL0, build_merc_bucket(cb, false, half_fade));
    mod->send_chain(mem.data(), kChainStart);
    display->render();

    metal_renderer::FramePixels frame;
    if (!metal_renderer::read_last_frame(&frame)) {
      printf("[FAIL] could not read back the merc envmap frame\n");
      g_fail_count++;
      g_ee_main_mem = nullptr;
      return;
    }
    auto stats = metal_renderer::get_chain_stats();
    printf("merc envmap stats: %d models, %d draws (%d envmap), %d tris\n", stats.merc_models,
           stats.merc_draws, stats.merc_envmap_draws, stats.merc_triangles);
    check(stats.merc_models == 1, "merc: one model instance in the envmap frame");
    check(stats.merc_envmap_draws == 1, "merc: a non-zero fade adds one envmap draw");
    check(stats.merc_draws == 2, "merc: the envmap effect draws twice (merc2 + emerc)");

    // emerc adds T0 * fade * 2 = (0.787, 0.394, 0.197) on top of the merc pass,
    // through the SRC_0_DST_DST blend (dst alpha is 1 after the first draw).
    const int col0 = gs_to_col(kX0), row0 = gs_to_row(kY0);
    check_pixel(frame, col0, row0, 255, 151, 75, "merc: envmap pass blended over the model");
    check_pixel(frame, gs_to_col(kX1), gs_to_row(kY1), 0, 0, 0,
                "merc: no second instance in the envmap frame");
  }

  g_ee_main_mem = nullptr;
}

// ---------------------------------------------------------------------------
// Section: a constructed generic bucket -> MetalGeneric2.
//
// None of the available captures carries generic *geometry* (their generic
// buckets hold only the 240-byte setup), so the drawing path is exercised with
// a bucket built to match every packet Generic2 asserts about a real chain:
// the 48-byte test/zbuf setup, the 160-byte VU constants unpack, the 32-byte
// VU register setup, then one fragment whose single VIF transfer carries the
// 7-quadword header, one adgif, and the STCYCL/UNPACK_V3_32 positions,
// UNPACK_V4_8 colors and UNPACK_V2_16 texture coordinates the VU consumes.
// ---------------------------------------------------------------------------

constexpr u32 kVramGenericTex = 0x7e0;
constexpr u32 kGenericTpage = kEeBase + 0x16000;

void test_generic2_chain(const GfxRendererModule* mod, std::shared_ptr<GfxDisplay>& display) {
  printf("--- DMA chain: generic bucket -> MetalGeneric2 ---\n");
  using namespace jak1;

  std::vector<u8> mem(kEeSize, 0);
  g_ee_main_mem = mem.data();

  auto pages = find_tpages(8);
  if (pages.size() < 8) {
    printf("[FAIL] not enough Jak 1 tpages for the generic2 test\n");
    g_fail_count++;
    g_ee_main_mem = nullptr;
    return;
  }

  // quadrant texture: TL red, TR green, BL blue, BR white - so the check also
  // proves the texture coordinates arrive in the right orientation
  tfrag3::Texture quad;
  quad.w = 16;
  quad.h = 16;
  quad.debug_name = "generic-quad";
  quad.debug_tpage_name = "generic-page";
  quad.data.resize(16 * 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      bool right = x >= 8, bottom = y >= 8;
      u32 r = (!right && !bottom) || (right && bottom) ? 255 : 0;
      u32 g = right ? 255 : 0;
      u32 b = bottom ? 255 : 0;
      quad.data[y * 16 + x] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
  }
  give_and_link_texture(mod, mem, quad, pages[7], kGenericTpage, kVramGenericTex);

  // The projection the fragment header carries: scale 1, mat_23 1, mat_32 0,
  // mat_33 0 (which is what marks it as the projection rather than the HUD
  // matrix). With a vertex z of -1 that reduces the shader's math to
  // ndc = (-px / 256, py / 112), the same screen mapping gs_to_col/gs_to_row use.
  auto ndc_col = [](double px) { return (int)llround((-px / 256.0 + 1.0) / 2.0 * 640.0); };
  auto ndc_row = [](double py) { return (int)llround((1.0 - py / 112.0) / 2.0 * 480.0); };

  auto build_generic_bucket = [&]() {
    std::vector<ChainBuilder::Transfer> gen;

    // setup packet 1: test + zbuf (zmsk 0 -> depth writes on)
    gen.push_back({0, 0, std::vector<u8>(48, 0), false});

    // setup packet 2: the VU constants. pfog0/fog_min/fog_max at 0/4/8,
    // hvdf_offset at 48.
    {
      std::vector<u8> d(160, 0);
      float pfog0 = 1.f, fog_min = 0.f, fog_max = 255.f;
      memcpy(&d[0], &pfog0, 4);
      memcpy(&d[4], &fog_min, 4);
      memcpy(&d[8], &fog_max, 4);
      float hvdf[4] = {2048.f, 2048.f, 8388607.f, 0.f};
      memcpy(&d[48], hvdf, 16);
      gen.push_back({vif_stcycl(4, 4), vif_unpack_v4_32(10, 0, true), d, false});
    }

    // setup packet 3: VU register setup
    gen.push_back({0, 0, std::vector<u8>(32, 0), false});

    // the fragment
    std::vector<u8> d;
    {
      // 7 quadword header: the 4x4 matrix, a giftag, and the "bonus" adgifs
      std::vector<u8> header(112, 0);
      float mat[16] = {0};
      mat[0] = 1.f;   // scale x
      mat[5] = 1.f;   // scale y
      mat[10] = 1.f;  // scale z
      mat[11] = 1.f;  // mat_23
      mat[14] = 0.f;  // mat_32
      mat[15] = 0.f;  // mat_33 == 0 marks this as the projection matrix
      memcpy(&header[0], mat, 64);
      // giftag with pre set and prim 0, so fge is 0 and the fog vertex flag clears
      u64 giftag_lo = ((u64)1 << 46);
      memcpy(&header[64], &giftag_lo, 8);
      // bonus adgifs: ALPHA then TEST
      u64 bonus[4];
      bonus[0] = 0;  // GsAlpha a=b=c=d=SOURCE -> SRC_SRC_SRC_SRC ("Cv = Cs")
      bonus[1] = (u64)GsRegisterAddress::ALPHA_1;
      bonus[2] = (1ull << 16) | ((u64)GsTest::ZTest::GEQUAL << 17);  // ate 0, zte 1, GEQUAL
      bonus[3] = (u64)GsRegisterAddress::TEST_1;
      memcpy(&header[80], bonus, 32);
      d.insert(d.end(), header.begin(), header.end());
    }
    {
      // one adgif. The game stores the fragment's vertex range in the unused
      // upper halves of the register addresses (link_adgifs_back_to_frags).
      AdGifData ad{};
      ad.tex0_data = gs_tex0(kVramGenericTex, 1, 0, 4, 4, true, 0);
      ad.tex0_addr = (u64)GsRegisterAddress::TEX0_1 | (0ull << 32);  // vertex offset * 3
      ad.tex1_data = 0;                                              // mmag 0 -> nearest
      ad.tex1_addr = (u64)GsRegisterAddress::TEX1_1 | (4ull << 32);  // 4 vertices
      ad.mip_data = 0;
      ad.mip_addr = (u64)GsRegisterAddress::MIPTBP1_1;
      ad.clamp_data = 0b101;  // clamp s and t
      ad.clamp_addr = (u64)GsRegisterAddress::CLAMP_1;
      ad.alpha_data = 0;
      ad.alpha_addr = (u64)GsRegisterAddress::MIPTBP2_1;  // alpha comes from the bonus adgif
      size_t at = d.size();
      d.resize(at + sizeof(AdGifData));
      memcpy(&d[at], &ad, sizeof(AdGifData));
    }
    const u32 first_unpack_bytes = (u32)d.size();

    // a 4-vertex triangle strip covering ndc x/y in [-0.5, 0.5]
    const float px[4] = {128.f, -128.f, 128.f, -128.f};
    const float py[4] = {56.f, 56.f, -56.f, -56.f};
    const s16 s[4] = {0, 4096, 0, 4096};
    const s16 t[4] = {0, 0, 4096, 4096};

    push_i(d, (s32)vif_stcycl(3, 1));  // STCYCL immediate 0x103
    push_i(d, (s32)vif_code(VifCode::Kind::UNPACK_V3_32, 0, 4));
    for (int i = 0; i < 4; i++) {
      push_f(d, px[i]);
      push_f(d, py[i]);
      push_f(d, -1.f);
    }
    push_i(d, (s32)vif_code(VifCode::Kind::UNPACK_V4_8, 0, 4));
    for (int i = 0; i < 4; i++) {
      d.push_back(128);
      d.push_back(128);
      d.push_back(128);
      d.push_back(128);
    }
    push_i(d, (s32)vif_code(VifCode::Kind::UNPACK_V2_16, 0, 4));
    for (int i = 0; i < 4; i++) {
      size_t at = d.size();
      d.resize(at + 4);
      memcpy(&d[at], &s[i], 2);
      memcpy(&d[at + 2], &t[i], 2);
    }
    push_i(d, (s32)vif_stcycl(4, 4));                          // the STCYCL reset
    push_i(d, (s32)vif_code(VifCode::Kind::MSCAL, 0x24, 0));   // run the VU program
    while (d.size() % 16) {
      push_i(d, 0);  // NOP padding to a whole quadword
    }
    gen.push_back(
        {vif_stcycl(4, 4), vif_unpack_v4_32(first_unpack_bytes / 16, 0, true), d, false});
    return gen;
  };

  ChainBuilder cb(mem);
  cb.set_bucket_content((int)BucketId::GENERIC_PRIS_LEVEL0, build_generic_bucket());
  mod->send_chain(mem.data(), kChainStart);
  display->render();

  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    printf("[FAIL] could not read back the generic2 frame\n");
    g_fail_count++;
    g_ee_main_mem = nullptr;
    return;
  }
  auto stats = metal_renderer::get_chain_stats();
  printf("generic2 stats: %d fragments, %d vertices, %d adgifs, %d draw buckets, %d draws, "
         "%d tris, %d missing textures, %d unsupported blends, %d unexpected-DMA, %d overflow\n",
         stats.generic_fragments, stats.generic_vertices, stats.generic_adgifs,
         stats.generic_draw_buckets, stats.generic_draws, stats.generic_triangles,
         stats.generic_missing_textures, stats.generic_unsupported_blends,
         stats.generic_unexpected_dma, stats.generic_overflow);
  check(stats.generic_unexpected_dma == 0, "generic2: the constructed bucket matched the walk");
  check(stats.generic_fragments == 1, "generic2: one fragment built from the chain");
  check(stats.generic_vertices == 4, "generic2: four vertices unpacked");
  check(stats.generic_adgifs == 1, "generic2: one adgif read from the fragment header");
  check(stats.generic_draw_buckets == 1, "generic2: the adgif landed in one draw bucket");
  check(stats.generic_draws == 1, "generic2: the draw bucket issued one draw");
  check(stats.generic_triangles == 2, "generic2: the 4-vertex strip is two triangles");
  check(stats.generic_missing_textures == 0, "generic2: the draw found its texture");

  // The fragment shader's modulate + tcc path: fragment_color.rgb is the
  // vertex color 128/255, times the texel, times 2 - so the quadrant colors
  // come through nearly unchanged. Their placement is what proves the texture
  // coordinates and the vertex order.
  const int left = ndc_col(64.f), right = ndc_col(-64.f);
  const int top = ndc_row(28.f), bottom = ndc_row(-28.f);
  check_pixel(frame, left, top, 255, 0, 0, "generic2: top-left quadrant is red");
  check_pixel(frame, right, top, 0, 255, 0, "generic2: top-right quadrant is green");
  check_pixel(frame, left, bottom, 0, 0, 255, "generic2: bottom-left quadrant is blue");
  check_pixel(frame, right, bottom, 255, 255, 255, "generic2: bottom-right quadrant is white");

  // the quad's edges land where the projection says
  check_pixel(frame, ndc_col(126.f), ndc_row(28.f), 255, 0, 0, "generic2: inside the left edge");
  check_pixel(frame, ndc_col(132.f), ndc_row(28.f), 0, 0, 0, "generic2: outside the left edge");
  check_pixel(frame, ndc_col(64.f), ndc_row(54.f), 255, 0, 0, "generic2: inside the top edge");
  check_pixel(frame, ndc_col(64.f), ndc_row(60.f), 0, 0, 0, "generic2: outside the top edge");

  g_ee_main_mem = nullptr;
}

// ---------------------------------------------------------------------------
// Section (optional): real extracted Jak 1 textures from a user-supplied .fr3.
// Never bundled; pass the path on the command line to enable.
// ---------------------------------------------------------------------------
std::unique_ptr<tfrag3::Level> test_real_fr3(const char* path) {
  printf("--- texture path: real fr3 textures (%s) ---\n", path);
  if (!fs::exists(path)) {
    printf("[FAIL] fr3 file does not exist: %s\n", path);
    g_fail_count++;
    return nullptr;
  }
  auto compressed = file_util::read_binary_file(std::string(path));
  auto decomp = compression::decompress_zstd(compressed.data(), compressed.size());
  u16 version = 0;
  memcpy(&version, decomp.data(), 2);
  if (version != tfrag3::TFRAG3_VERSION) {
    printf("[SKIP] fr3 version %d does not match this build's %d; skipping real-data test\n",
           version, tfrag3::TFRAG3_VERSION);
    return nullptr;
  }
  auto level = std::make_unique<tfrag3::Level>();
  Serializer ser(decomp.data(), decomp.size());
  level->serialize(ser);
  printf("loaded level '%s': %d textures\n", level->level_name.c_str(),
         (int)level->textures.size());
  check(!level->textures.empty(), "fr3 contains textures");

  int verified = 0;
  for (size_t i = 0; i < level->textures.size() && verified < 3; i++) {
    const auto& tex = level->textures[i];
    if (tex.w == 0 || tex.h == 0 || tex.w > 1024 || tex.h > 1024) {
      continue;
    }
    u64 handle = metal_renderer::upload_texture_rgba8((const u8*)tex.data.data(), tex.w, tex.h);
    if (!handle) {
      printf("[FAIL] upload of real texture '%s' (%dx%d) failed\n", tex.debug_name.c_str(), tex.w,
             tex.h);
      g_fail_count++;
      continue;
    }
    check_gpu_matches(handle, std::vector<u8>((const u8*)tex.data.data(),
                                              (const u8*)tex.data.data() + tex.w * tex.h * 4),
                      tex.w, tex.h,
                      ("real texture " + tex.debug_tpage_name + "/" + tex.debug_name + " " +
                       std::to_string(tex.w) + "x" + std::to_string(tex.h))
                          .c_str());
    verified++;
  }
  check(verified > 0, "verified at least one real texture through the Metal path");
  return level;
}

// The level-geometry stage on real data: load the same .fr3 through the Metal
// loader and check what it reports against the level the test parsed itself,
// then run the culling / time-of-day ports on the level's real BVH and palettes.
void test_real_fr3_geometry(const char* path, const tfrag3::Level* parsed) {
  printf("--- level-geometry stage: real fr3 (%s) ---\n", path);
  auto result = metal_renderer::load_level_fr3(path, false);
  check(result.ok, "Metal level loader accepted the fr3");
  if (!result.ok) {
    printf("  error: %s\n", result.error.c_str());
    return;
  }
  printf("uploaded: %d tfrag + %d tie + %d shrub trees (lod 0), %.1f MB verts, %.1f MB indices\n",
         result.tfrag_trees, result.tie_trees, result.shrub_trees,
         result.vertex_bytes / (1024.f * 1024.f), result.index_bytes / (1024.f * 1024.f));
  check(result.level_name == parsed->level_name, "loaded level keeps its name");
  check(result.textures == (int)parsed->textures.size(), "every texture reached the pool");
  check(result.tfrag_trees == (int)parsed->tfrag_trees[0].size(), "tfrag tree count matches");
  check(result.tie_trees == (int)parsed->tie_trees[0].size(), "tie tree count matches");
  check(result.shrub_trees == (int)parsed->shrub_trees.size(), "shrub tree count matches");
  check(result.vertex_bytes > 0 && result.index_bytes > 0, "geometry buffers are not empty");

  // The index list StripDraws point into is built by tfrag3's unpack(); verify
  // the offsets the draw-run builder relies on are consistent with it.
  {
    tfrag3::TfragTree tree_copy;
    bool checked = false;
    for (const auto& tree : parsed->tfrag_trees[0]) {
      tree_copy = tree;
      tree_copy.unpack();
      u32 expected = 0;
      bool ok = true;
      for (const auto& draw : tree_copy.draws) {
        ok = ok && draw.unpacked.idx_of_first_idx_in_full_buffer == expected;
        for (const auto& grp : draw.vis_groups) {
          expected += grp.num_inds;
        }
      }
      check(ok, "tfrag draws tile the tree's index buffer without gaps");
      check(expected == tree_copy.unpacked.indices.size(),
            "tfrag vis groups cover the whole index buffer");
      checked = true;
      break;
    }
    if (!checked) {
      printf("(level has no tfrag trees; index-layout check skipped)\n");
    }
  }

  // Real palettes and a real BVH through both implementations.
  for (const auto& tree : parsed->tfrag_trees[0]) {
    math::Vector<s32, 4> itimes[4];
    memset(itimes, 0, sizeof(itimes));
    itimes[0][0] = 0x00300030;
    itimes[0][1] = 0x00300030;
    itimes[3][2] = 0x00100010;
    std::vector<math::Vector<u8, 4>> gl_out(tree.colors.color_count + 4);
    std::vector<math::Vector<u8, 4>> metal_out(tree.colors.color_count + 4);
    memset(gl_out.data(), 0xcd, gl_out.size() * 4);
    memset(metal_out.data(), 0xcd, metal_out.size() * 4);
    interp_time_of_day(itimes, tree.colors, gl_out.data());
    metal_renderer::interp_time_of_day_for_test(itimes, tree.colors, metal_out.data());
    check(memcmp(gl_out.data(), metal_out.data(), gl_out.size() * 4) == 0,
          "interp_time_of_day matches on the level's real palettes");

    math::Vector4f planes[4] = {
        math::Vector4f(0.6f, 0.f, 0.8f, 0.f), math::Vector4f(-0.6f, 0.f, 0.8f, 0.f),
        math::Vector4f(0.f, 0.6f, 0.8f, 0.f), math::Vector4f(0.f, -0.6f, 0.8f, 0.f)};
    planes[3] = math::Vector4f(0.f, 0.f, 0.f, 0.f);
    std::vector<u8> gl_vis(tree.bvh.vis_nodes.size(), 0xcd);
    std::vector<u8> metal_vis(tree.bvh.vis_nodes.size(), 0xcd);
    cull_check_all_slow(planes, tree.bvh.vis_nodes, nullptr, gl_vis.data());
    metal_renderer::cull_check_all_slow_for_test(planes, tree.bvh.vis_nodes, nullptr,
                                                 metal_vis.data());
    check(gl_vis == metal_vis, "cull_check_all_slow matches on the level's real BVH");
    break;
  }

  metal_renderer::unload_all_levels();
}

// ---------------------------------------------------------------------------
// Section (optional): replay of one frame of captured game DMA (--replay).
// The capture is what the runtime track's `__send-gfx-dma-chain` hook writes: a
// version 1 file is the chain alone, a version 2 file adds the EE main memory
// snapshot the frame ran with. With the snapshot, the chain's PC-port texture
// upload packets resolve to the real GOAL texture-page structs, so the pool
// links real texture slots instead of placeholders - as long as the level's
// textures are in the pool, which `--replay-fr3` does (the Metal path does not
// run the streaming Loader yet, and the runtime's `__pc-set-levels` is stubbed,
// so the replay is told which levels' art the frame used).
// ---------------------------------------------------------------------------

// Loads one extracted level into the Metal renderer: textures into the pool the
// way the GL loader's TextureLoaderStage does, tfrag / tie / shrub geometry into
// GPU buffers the way its Tfrag/Tie/ShrubLoadStage do, and the level's merc
// models into the merc model pool the way its MercLoaderStage does. Returns
// false on failure.
bool load_fr3_level(const char* path, bool is_common) {
  auto result = metal_renderer::load_level_fr3(path, is_common);
  if (!result.ok) {
    printf("[FAIL] load level %s: %s\n", path, result.error.c_str());
    g_fail_count++;
    return false;
  }
  printf(
      "[PASS] loaded level '%s'%s: %d textures, geometry: %d tfrag + %d tie + %d shrub trees, "
      "%.1f MB verts + %.1f MB indices\n",
      result.level_name.c_str(), is_common ? " (common)" : "", result.textures,
      result.tfrag_trees, result.tie_trees, result.shrub_trees,
      result.vertex_bytes / (1024.f * 1024.f), result.index_bytes / (1024.f * 1024.f));

  // The background and merc paths keep separate level pools, so each parses the
  // .fr3 and gives its textures to the pool; TexturePool::give_texture treats
  // the second source of a texture id the way it treats a texture shared by two
  // levels.
  metal_renderer::MercLevelLoad merc;
  std::string error;
  if (!metal_renderer::merc_load_fr3(path, is_common, &merc, &error)) {
    printf("[FAIL] load merc models from %s: %s\n", path, error.c_str());
    g_fail_count++;
    return false;
  }
  printf("[PASS] loaded level '%s' merc models: %d models (%u vertices, %u indices)\n",
         merc.level_name.c_str(), merc.models, merc.vertices, merc.indices);
  return true;
}

// ---------------------------------------------------------------------------
// Ocean readback checks (replay only - the ocean is entirely DMA driven, so it
// only has content when a captured frame carries the ocean buckets).
//
// Three things are verified from pixels rather than counters:
//  1. the generated 128x128 ocean texture has real, varied content (the VU
//     program ran and its geometry rasterized),
//  2. the mipmapped copy fades its alpha out with level - the deliberate
//     `max(0, 1 - 0.51 * level)` trick that makes the ocean fade with distance -
//     while the single-level copy ocean-near publishes does not,
//  3. the frame's ocean band, which reads back solid black without this
//     renderer, is now drawn.
// ---------------------------------------------------------------------------

// Counts non-black pixels and the mean channel values of a readback.
void frame_summary(const metal_renderer::FramePixels& f,
                   int y0,
                   int y1,
                   int* lit,
                   int* mean_rgb,
                   int* mean_a) {
  long sum_rgb = 0, sum_a = 0;
  int count = 0;
  *lit = 0;
  for (int y = y0; y < y1; y++) {
    for (int x = 0; x < f.width; x++) {
      const u8* p = &f.rgba[(y * f.width + x) * 4];
      if (p[0] || p[1] || p[2]) {
        (*lit)++;
      }
      sum_rgb += p[0] + p[1] + p[2];
      sum_a += p[3];
      count++;
    }
  }
  *mean_rgb = count ? (int)(sum_rgb / (3 * count)) : 0;
  *mean_a = count ? (int)(sum_a / count) : 0;
}

void check_ocean(const metal_renderer::ChainStats& stats,
                 const metal_renderer::FramePixels& frame,
                 const std::string& png_path) {
  check(stats.ocean_texture_verts == (int)(32 * 66),
        "ocean: the texture VU program produced a full 32x66 vertex mesh");
  check(stats.ocean_missing_textures == 0, "ocean: every ocean draw found its texture");

  // the pool slot both generators publish to must hold the last one of the
  // frame (ocean-near's), like the GL renderer
  auto* pool = metal_renderer::get_texture_pool();
  auto slot = pool ? pool->lookup(8160) : std::nullopt;
  check(slot.has_value() && stats.ocean_near_texture != 0 && *slot == stats.ocean_near_texture,
        "ocean: the ocean VRAM slot holds the last generated texture of the frame");

  // The generated texture is an alpha mask, not a color map: the VU feeds the
  // GS an RGBAQ of (0, 0, 0, 0x80) for every vertex (the "vertex" quadwords of
  // the ocean-texture input are (0, 0, 0, scale)), so ocean_texture.frag writes
  // black with the envmap's alpha. The ocean's visible color comes from the
  // ocean-mid envmap pass, which that alpha gates. Alpha is therefore the
  // channel worth checking.
  metal_renderer::FramePixels tex;
  metal_renderer::TextureSampleSpec spec;
  spec.texture = stats.ocean_mid_texture;
  spec.out_w = 128;
  spec.out_h = 128;
  if (sample_tex(spec, &tex, "ocean: read back the generated texture")) {
    int drawn = 0;
    u8 lo = 255, hi = 0;
    long sum_a = 0;
    for (size_t i = 0; i < tex.rgba.size(); i += 4) {
      u8 a = tex.rgba[i + 3];
      sum_a += a;
      lo = std::min(lo, a);
      hi = std::max(hi, a);
      if (a) {
        drawn++;
      }
    }
    printf("generated ocean texture: %d/%d texels drawn, alpha %d-%d (mean %d)\n", drawn,
           tex.width * tex.height, lo, hi, (int)(sum_a / (tex.width * tex.height)));
    check(drawn > (tex.width * tex.height) / 2,
          "ocean: the generated texture is drawn, not an empty render target");
    check(hi - lo > 16, "ocean: the generated texture varies rather than being one flat value");

    if (!png_path.empty()) {
      // the mask is written with its alpha in every channel, so it is visible
      auto tex_png = fs::path(png_path).replace_extension("").string() + "-ocean-tex.png";
      std::vector<u8> px = tex.rgba;
      for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = px[i + 1] = px[i + 2] = px[i + 3];
        px[i + 3] = 255;
      }
      try {
        file_util::write_rgba_png(fs::path(tex_png), px.data(), tex.width, tex.height);
        printf("[PASS] wrote the generated ocean texture mask to %s\n", tex_png.c_str());
      } catch (const std::exception& e) {
        printf("[FAIL] could not write %s: %s\n", tex_png.c_str(), e.what());
        g_fail_count++;
      }
    }
  }

  // the mip chain's alpha fade: level 7's alpha_intensity is max(0, 1 - 0.51*7)
  // = 0, so minifying all the way down must read back fully transparent.
  metal_renderer::FramePixels mip;
  metal_renderer::TextureSampleSpec mip_spec;
  mip_spec.texture = stats.ocean_mid_texture;
  mip_spec.out_w = 1;
  mip_spec.out_h = 1;
  mip_spec.mip_mode = 1;  // nearest mip
  if (sample_tex(mip_spec, &mip, "ocean: read back the texture's smallest mip")) {
    printf("ocean texture top mip: rgba (%d,%d,%d,%d)\n", mip.rgba[0], mip.rgba[1], mip.rgba[2],
           mip.rgba[3]);
    check(mip.rgba[3] == 0, "ocean: the mip chain fades alpha to zero at the smallest level");
  }

  // ocean-near publishes a single-level texture, so the same minification has
  // nothing to fade: its alpha survives.
  metal_renderer::FramePixels near_mip;
  metal_renderer::TextureSampleSpec near_spec = mip_spec;
  near_spec.texture = stats.ocean_near_texture;
  if (sample_tex(near_spec, &near_mip, "ocean: read back the near texture's smallest mip")) {
    check(near_mip.rgba[3] > 0,
          "ocean: the single-level texture ocean-near publishes keeps its alpha");
  }

  // the band below the horizon: solid black before this renderer existed
  int lit = 0, mean_rgb = 0, mean_a = 0;
  frame_summary(frame, frame.height * 260 / 480, frame.height * 340 / 480, &lit, &mean_rgb,
                &mean_a);
  printf("ocean band (rows %d-%d): %d lit pixels, mean rgb %d\n", frame.height * 260 / 480,
         frame.height * 340 / 480, lit, mean_rgb);
  check(lit > frame.width * (frame.height * 80 / 480) / 2,
        "ocean: the frame's ocean band is drawn");
}

void run_chain_replay(const GfxRendererModule* mod,
                      std::shared_ptr<GfxDisplay>& display,
                      const std::string& capture_path,
                      const std::string& png_path,
                      int frames,
                      const std::vector<std::string>& fr3_paths,
                      const std::string& common_fr3) {
  printf("--- captured chain replay: %s ---\n", capture_path.c_str());

  metal_chain_replay::LoadedCapture capture;
  std::string error;
  if (!metal_chain_replay::load_capture_file(capture_path, &capture, &error)) {
    printf("[FAIL] load capture: %s\n", error.c_str());
    g_fail_count++;
    return;
  }
  auto& chain = capture.chain;
  printf("[PASS] loaded capture v%d (frame %d): start offset %#x, %d bytes of chain chunks\n",
         capture.version, capture.frame, chain.start_offset, (int)chain.data.size());
  if (capture.has_ee_snapshot) {
    printf("[PASS] EE snapshot: %d of %d chunks stored, s7 %#x\n",
           (int)capture.ee_stored_chunks.size(),
           (int)(capture.ee_memory.size() / FixedChunkDmaCopier::chunk_size), capture.s7);
    check(capture.s7 != 0, "replay: capture carries a symbol-table pointer");
  } else {
    printf("(no EE snapshot in this capture; pointers into EE memory read as zeros)\n");
  }

  metal_chain_replay::ChainInventory inv;
  if (!metal_chain_replay::inventory_jak1(chain, &inv, &error)) {
    printf("[FAIL] chain inventory: %s\n", error.c_str());
    g_fail_count++;
    return;
  }
  printf("[PASS] chain has the Jak 1 frame structure (70 buckets, fog (%d, %d, %d))\n",
         inv.fog_color[0], inv.fog_color[1], inv.fog_color[2]);
  printf("bucket payload inventory:\n");
  bool any_payload = false;
  for (auto& b : inv.buckets) {
    if (b.payload_bytes > 0) {
      any_payload = true;
      printf("  [%2d] %-34s %8d bytes in %3d transfers%s\n", b.bucket,
             metal_chain_replay::jak1_bucket_name(b.bucket).c_str(), (int)b.payload_bytes,
             b.transfers,
             b.pc_port_uploads ? fmt::format(", {} texture uploads", b.pc_port_uploads).c_str()
                               : "");
    }
  }
  if (!any_payload) {
    printf("  (every bucket carries only the empty-bucket structure)\n");
  }
  printf("total payload: %d bytes, %d texture upload packets\n", (int)inv.total_payload,
         inv.total_pc_port_uploads);

  // The chain image goes into a run of EE memory the snapshot left empty, so
  // replaying it cannot land on captured data.
  if (!metal_chain_replay::place_chain_in_ee(&capture, &error)) {
    printf("[FAIL] place chain in EE memory: %s\n", error.c_str());
    g_fail_count++;
    return;
  }
  printf("[PASS] chain placed at %#x in the EE image\n", capture.chain_base);

  std::vector<u8>& ee_mem = capture.ee_memory;
  g_ee_main_mem = ee_mem.data();
  metal_renderer::set_s7_override(capture.s7);

  // The level art the frame used. The Metal path does not run the streaming
  // Loader yet, so the caller names the levels; without them the upload packets
  // resolve to the pool's placeholder, which is reported, never guessed.
  if (!common_fr3.empty()) {
    load_fr3_level(common_fr3.c_str(), true);
  }
  for (const auto& path : fr3_paths) {
    load_fr3_level(path.c_str(), false);
  }

  // A single frame only uploads the texture pages it touched, but the VRAM
  // slots it draws from were filled over many earlier frames. Prime the pool
  // with every texture-page still live in the EE snapshot, so slots the frame
  // reads but does not re-upload resolve to their real textures.
  if (capture.has_ee_snapshot && !inv.upload_page_addresses.empty()) {
    u32 type_pointer = 0;
    auto pages = metal_chain_replay::find_texture_pages(capture, inv.upload_page_addresses,
                                                        &type_pointer);
    for (u32 page : pages) {
      mod->texture_upload_now(ee_mem.data() + page, -1, capture.s7);
    }
    printf("[PASS] primed the pool with %d live texture-pages from the EE snapshot "
           "(type pointer %#x)\n",
           (int)pages.size(), type_pointer);
    check(pages.size() >= inv.upload_page_addresses.size(),
          "replay: the snapshot's pages include the ones the chain uploads");
  }

  // send the chain through the module hook like the game does, several times:
  // content that crosses frames (sky blended in frame N draws in frame N+1)
  // needs at least two
  auto before = metal_renderer::get_chain_stats();
  for (int i = 0; i < frames; i++) {
    mod->send_chain(ee_mem.data(), chain.start_offset);
    display->render();
  }

  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    printf("[FAIL] could not read back the replayed frame\n");
    g_fail_count++;
    g_ee_main_mem = nullptr;
    return;
  }
  auto stats = metal_renderer::get_chain_stats();
  check(stats.chains_rendered == before.chains_rendered + frames,
        "replay: every sent chain was rendered");
  printf(
      "replay stats: %d draws, %d tris, %d uploads, sky d/b %d/%d, cloud d/b %d/%d, "
      "skipped %d bucket + %d tfrag bytes, %d unsupported blends\n",
      stats.draw_calls, stats.triangles, stats.tex_uploads, stats.sky_draws, stats.sky_blends,
      stats.cloud_draws, stats.cloud_blends, (int)stats.skipped_bucket_bytes,
      (int)stats.skipped_tfrag_bytes, stats.direct_unsupported_blends);
  printf(
      "sprite bucket: %d 2d + %d 3d + %d hud sprites in %d draws, %d distort sprites consumed "
      "(drawing not ported), %d missing textures\n",
      stats.sprites_2d, stats.sprites_3d, stats.sprites_hud, stats.sprite_draws,
      stats.sprites_distort, stats.sprite_missing_textures);
  printf(
      "ocean buckets: %d texture verts, %d mid verts, %d near verts, %d draws, %d tris, "
      "%d missing textures\n",
      stats.ocean_texture_verts, stats.ocean_mid_verts, stats.ocean_near_verts, stats.ocean_draws,
      stats.ocean_triangles, stats.ocean_missing_textures);
  check(stats.direct_unsupported_blends == 0, "replay: no unsupported GS blend modes");
  printf(
      "merc buckets: %d models (%d missing), %d draws (%d envmap), %d tris, %d bone vectors, "
      "%d deferred mod effects, %d eye draws, %d missing textures, "
      "%d bad bone pointers, %d bad draw ranges\n",
      stats.merc_models, stats.merc_missing_models, stats.merc_draws, stats.merc_envmap_draws,
      stats.merc_triangles, stats.merc_bone_vectors, stats.merc_mod_effects_deferred,
      stats.merc_eye_draws, stats.merc_missing_textures, stats.merc_bad_bone_pointers,
      stats.merc_bad_draw_ranges);
  {
    u64 merc_payload = 0;
    for (auto& b : inv.buckets) {
      const auto name = metal_chain_replay::jak1_bucket_name(b.bucket);
      if (name.rfind("MERC_", 0) == 0 && name != "MERC_EYES_AFTER_PRIS") {
        merc_payload += b.payload_bytes;
      }
    }
    if (merc_payload > 0) {
      printf("(the frame's merc buckets carried %d bytes of control data)\n", (int)merc_payload);
      check(stats.merc_models > 0, "replay: the merc buckets built models from the chain");
      check(stats.merc_missing_models == 0,
            "replay: every merc model the frame named was in the loaded levels");
      check(stats.merc_draws > 0, "replay: merc issued draws");
      check(stats.merc_missing_textures == 0, "replay: every merc draw found its texture");
      check(stats.merc_bad_bone_pointers == 0,
            "replay: every merc bone pointer landed inside EE memory");
      check(stats.merc_bad_draw_ranges == 0,
            "replay: every merc draw range fit its level's index buffer");
    }
  }

  // --- generic2 ---
  {
    u64 generic_payload = 0;
    for (auto& b : inv.buckets) {
      if (metal_chain_replay::jak1_bucket_name(b.bucket).rfind("GENERIC", 0) == 0 ||
          metal_chain_replay::jak1_bucket_name(b.bucket) == "SHRUB_GENERIC_LEVEL0" ||
          metal_chain_replay::jak1_bucket_name(b.bucket) == "SHRUB_GENERIC_LEVEL1") {
        generic_payload += b.payload_bytes;
      }
    }
    printf("generic2 buckets: %d bytes of DMA, %d fragments, %d vertices, %d adgifs, "
           "%d draw buckets, %d draws, %d tris, %d missing textures, %d unsupported blends, "
           "%d unexpected-DMA reports, %d overflows\n",
           (int)generic_payload, stats.generic_fragments, stats.generic_vertices,
           stats.generic_adgifs, stats.generic_draw_buckets, stats.generic_draws,
           stats.generic_triangles, stats.generic_missing_textures,
           stats.generic_unsupported_blends, stats.generic_unexpected_dma,
           stats.generic_overflow);
    check(stats.generic_unexpected_dma == 0, "replay: every generic bucket matched its renderer");
    check(stats.generic_overflow == 0, "replay: generic2's buffers held the frame's data");
    check(stats.generic_missing_textures == 0, "replay: every generic2 draw found its texture");
    check(stats.generic_unsupported_blends == 0, "replay: no unsupported generic2 blend modes");
  }

  // --- eye renderer ---
  {
    u64 eye_payload = 0;
    for (auto& b : inv.buckets) {
      if (metal_chain_replay::jak1_bucket_name(b.bucket) == "MERC_EYES_AFTER_PRIS") {
        eye_payload += b.payload_bytes;
      }
    }
    printf("eye bucket: %d bytes of DMA, %d eyes composed, %d draws, %d tris, %d missing "
           "textures, %d unexpected-DMA reports\n",
           (int)eye_payload, stats.eyes_composed, stats.eye_draws, stats.eye_triangles,
           stats.eye_missing_textures, stats.eye_unexpected_dma);
    check(stats.eye_unexpected_dma == 0, "replay: the eye bucket matched its renderer");
    // A frame with no eyes on screen still sends the bucket's fixed structure
    // (the render-to-texture setup, the alpha setup and the GS restore), so the
    // trigger for the drawing checks is merc asking for eye textures.
    if (stats.merc_eye_draws > 0) {
      check(stats.eyes_composed > 0, "replay: the eye bucket composed eyes");
      check(stats.eye_draws > 0, "replay: the eye renderer issued draws");
      check(stats.eye_missing_textures == 0, "replay: every eye source texture was in VRAM");

      // the composed eye must be a real image: the pass clears to opaque-less
      // red, so a texture that is still all (255,0,0) means nothing drew.
      metal_renderer::FramePixels eye;
      metal_renderer::TextureSampleSpec eye_spec;
      eye_spec.texture = stats.eye_texture;
      eye_spec.out_w = 64;
      eye_spec.out_h = 64;
      if (sample_tex(eye_spec, &eye, "replay: read back a composed eye texture")) {
        int cleared = 0, lit = 0;
        u8 lo_g = 255, hi_g = 0;
        for (size_t i = 0; i < eye.rgba.size(); i += 4) {
          const u8 r = eye.rgba[i], g = eye.rgba[i + 1], b = eye.rgba[i + 2];
          if (r == 255 && g == 0 && b == 0) {
            cleared++;
          }
          if (r || g || b) {
            lit++;
          }
          lo_g = std::min(lo_g, g);
          hi_g = std::max(hi_g, g);
        }
        const int total = eye.width * eye.height;
        printf("composed eye texture: %d/%d texels drawn, %d still the debug clear, green %d-%d\n",
               lit, total, cleared, lo_g, hi_g);
        check(cleared == 0, "replay: the eye texture is fully covered (no debug-clear texels)");
        check(hi_g - lo_g > 8, "replay: the eye texture has real image content, not one flat color");

        if (!png_path.empty()) {
          auto eye_png = fs::path(png_path).replace_extension("").string() + "-eye.png";
          std::vector<u8> px = eye.rgba;
          for (size_t i = 0; i < px.size(); i += 4) {
            px[i + 3] = 255;
          }
          try {
            file_util::write_rgba_png(fs::path(eye_png), px.data(), eye.width, eye.height);
            printf("[PASS] wrote a composed eye texture to %s\n", eye_png.c_str());
          } catch (const std::exception& e) {
            printf("[FAIL] could not write %s: %s\n", eye_png.c_str(), e.what());
            g_fail_count++;
          }
        }
      }
    }
  }

  auto bg = metal_renderer::get_background_stats();
  printf(
      "level geometry: tfrag %d draws / %d tris, tie %d draws / %d tris, "
      "shrub %d draws / %d tris; %d bucket(s) named an unloaded level, "
      "%d missing textures, %d animator-slot draws, %d unexpected-DMA reports\n",
      bg.tfrag_draws, bg.tfrag_tris, bg.tie_draws, bg.tie_tris, bg.shrub_draws, bg.shrub_tris,
      bg.missing_levels, bg.missing_textures, bg.anim_slot_draws, bg.unexpected_dma);
  check(bg.unexpected_dma == 0, "replay: every level-geometry bucket matched its renderer");
  if (!fr3_paths.empty()) {
    // the named levels are loaded, so nothing should fall back
    check(bg.missing_levels == 0, "replay: every bucket's level was loaded");
    check(bg.missing_textures == 0, "replay: every level-geometry draw found its texture");
    check(bg.anim_slot_draws == 0, "replay: no Jak 2/3 texture-animator slots requested");
  }
  if (stats.ocean_texture_verts > 0) {
    check_ocean(stats, frame, png_path);
  }

  int lit = 0;
  for (int i = 0; i < frame.width * frame.height * 4; i += 4) {
    if (frame.rgba[i] || frame.rgba[i + 1] || frame.rgba[i + 2]) {
      lit++;
    }
  }
  printf("replayed frame: %dx%d, %d non-black pixels\n", frame.width, frame.height, lit);
  if (stats.draw_calls == 0) {
    check(lit == 0, "replay: a frame with no draws reads back black");
  } else if (lit == 0) {
    // Not a failure: a captured frame can legitimately draw only black - the
    // title screen's sky bucket draws a black quad when nothing was blended
    // into the sky texture that frame.
    printf("(the frame's %d draw(s) produced no lit pixels; see the inventory above)\n",
           stats.draw_calls);
  }

  // PNG for human inspection. Alpha is forced opaque: the game target's alpha
  // channel is GS framebuffer-alpha data, not image transparency.
  std::vector<u8> png_pixels = frame.rgba;
  for (size_t i = 3; i < png_pixels.size(); i += 4) {
    png_pixels[i] = 255;
  }
  try {
    file_util::write_rgba_png(fs::path(png_path), png_pixels.data(), frame.width, frame.height);
    printf("[PASS] wrote replayed frame to %s\n", png_path.c_str());
  } catch (const std::exception& e) {
    printf("[FAIL] could not write %s: %s\n", png_path.c_str(), e.what());
    g_fail_count++;
  }
  g_ee_main_mem = nullptr;
  metal_renderer::set_s7_override(0);
}

}  // namespace

int main(int argc, char** argv) {
  lg::initialize();
  // this proof needs no game data; any existing directory works as the project path
  file_util::setup_project_path(fs::current_path());

  std::string fr3_path;
  std::string replay_path;
  std::string replay_png;
  std::string replay_common_fr3;
  std::vector<std::string> replay_fr3;
  int replay_frames = 2;
  bool show_window = false;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--replay" && i + 1 < argc) {
      replay_path = argv[++i];
    } else if (arg == "--replay-png" && i + 1 < argc) {
      replay_png = argv[++i];
    } else if (arg == "--replay-frames" && i + 1 < argc) {
      replay_frames = std::max(1, atoi(argv[++i]));
    } else if (arg == "--replay-fr3" && i + 1 < argc) {
      replay_fr3.push_back(argv[++i]);
    } else if (arg == "--replay-common-fr3" && i + 1 < argc) {
      replay_common_fr3 = argv[++i];
    } else if (arg == "--show-window") {
      show_window = true;
    } else if (arg == "--fr3" && i + 1 < argc) {
      fr3_path = argv[++i];
    } else if (!arg.empty() && arg[0] != '-' && fr3_path.empty()) {
      fr3_path = arg;  // original positional .fr3 argument
    } else {
      printf(
          "usage: metal-proof [<level.fr3> | --fr3 <level.fr3>]\n"
          "                   [--replay <capture.gpdma> [--replay-png <out.png>]\n"
          "                    [--replay-frames <n>] [--replay-common-fr3 <GAME.fr3>]\n"
          "                    [--replay-fr3 <level.fr3>]...]\n"
          "                   [--show-window]\n");
      return 1;
    }
  }

  const GfxRendererModule* mod = Gfx::GetRenderer(GfxPipeline::Metal);
  if (!mod) {
    printf("[FAIL] could not get Metal renderer module\n");
    return 1;
  }
  printf("[PASS] got renderer module: %s\n", mod->name);

  GfxGlobalSettings settings;
  if (mod->init(settings) != 0) {
    printf("[FAIL] Metal renderer init failed\n");
    return 1;
  }
  printf("[PASS] Metal renderer init\n");

  // Headless by default: everything here is verified by reading the render
  // target back, so no window needs to appear or take focus. --show-window is
  // for a human who wants to watch.
  metal_renderer::set_window_hidden(!show_window);

  auto display = mod->make_display(640, 480, "OpenGOAL Metal Proof", settings, GameVersion::Jak1,
                                   /*is_main=*/true);
  if (!display) {
    printf("[FAIL] Metal make_display failed\n");
    mod->exit();
    return 1;
  }
  printf("[PASS] Metal display created\n");

  if (!replay_path.empty()) {
    // replay mode: only the captured chain runs, on an otherwise untouched
    // pipeline (empty texture pool, no synthetic scene state)
    run_chain_replay(mod, display,
                     replay_path, replay_png.empty() ? replay_path + ".png" : replay_png,
                     replay_frames, replay_fr3, replay_common_fr3);
    display.reset();
    mod->exit();
    if (g_fail_count == 0) {
      printf("METAL REPLAY PASSED\n");
      return 0;
    }
    printf("METAL REPLAY FAILED: %d check(s) failed\n", g_fail_count);
    return 1;
  }

  // render frames through the GfxDisplay interface, like Gfx::Loop does
  for (int i = 0; i < 30; i++) {
    display->render();
  }

  // ---- offscreen game target: blend / depth / mask states through the PSO cache ----
  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    printf("[FAIL] could not read back rendered frame\n");
    display.reset();
    mod->exit();
    return 1;
  }
  printf("[PASS] read back %dx%d game frame\n", frame.width, frame.height);

  // Scene layout in the 640x480 game frame (see MetalRenderer::build_validation_scene):
  //  gray base x[80,560] y[60,420]; checker x[96,224] y[72,168] (16x12 px cells);
  //  additive x[384,512] y[288,384]; alpha x[384,512] y[96,192];
  //  rev-sub x[128,256] y[288,384]; red-mask x[288,352] y[216,264];
  //  depth-rejected quad x[128,256] y[192,288] must not appear.
  check_pixel(frame, 5, 5, 0, 0, 0, "clear color outside all geometry");
  check_pixel(frame, 100, 400, 128, 128, 128, "opaque gray base");
  check_pixel(frame, 192, 240, 128, 128, 128, "GEQUAL depth reject (behind quad invisible)");
  check_pixel(frame, 104, 78, 255, 255, 255, "texture white cell");
  check_pixel(frame, 120, 78, 255, 0, 0, "texture red cell");
  check_pixel(frame, 448, 336, 128, 128, 191, "additive blend (ONE,ONE)");
  check_pixel(frame, 448, 144, 191, 64, 64, "alpha blend (SRC_ALPHA,1-SRC_ALPHA)");
  check_pixel(frame, 192, 336, 64, 64, 64, "reverse-subtract blend");
  check_pixel(frame, 320, 240, 255, 128, 128, "color write mask (red only)");

  // ---- PSO cache: distinct states built once, reused across frames ----
  auto s1 = metal_renderer::get_stats();
  for (int i = 0; i < 30; i++) {
    display->render();
  }
  auto s2 = metal_renderer::get_stats();
  printf("stats after %llu frames: %llu PSOs, %llu depth-stencil states, %llu misses, %llu hits\n",
         (unsigned long long)s2.frames_rendered, (unsigned long long)s2.pso_count,
         (unsigned long long)s2.depth_stencil_count, (unsigned long long)s2.pso_misses,
         (unsigned long long)s2.pso_hits);
  check(s1.pso_count >= 5, "PSO cache holds the distinct game-pass states");
  check(s1.depth_stencil_count == 3, "3 distinct depth-stencil states");
  check(s1.pso_misses == s1.pso_count, "each PSO built exactly once");
  check(s2.pso_count == s1.pso_count && s2.pso_misses == s1.pso_misses,
        "no new PSOs after 30 more frames");
  check(s2.pso_hits > s1.pso_hits, "PSOs reused across frames");
  check(s2.frames_rendered == s1.frames_rendered + 30, "frame counter advanced");

  // ---- present pass: letterbox at 1:1 ----
  metal_renderer::PresentTestOptions popts;
  popts.window_w = 800;
  popts.window_h = 600;
  popts.draw_region_w = 640;
  popts.draw_region_h = 480;
  metal_renderer::FramePixels present;
  if (!metal_renderer::read_present_frame(popts, &present)) {
    printf("[FAIL] could not read back present frame\n");
    display.reset();
    mod->exit();
    return 1;
  }
  printf("[PASS] read back %dx%d present frame (1:1 letterbox)\n", present.width, present.height);
  check_pixel(present, 40, 300, 0, 0, 0, "left letterbox bar");
  check_pixel(present, 770, 300, 0, 0, 0, "right letterbox bar");
  check_pixel(present, 400, 30, 0, 0, 0, "top letterbox bar");
  check_pixel(present, 161, 300, 128, 128, 128, "game content at 1:1 (gray base)");
  check_pixel(present, 400, 300, 255, 128, 128, "game center at 1:1 (masked quad)");
  check_pixel(present, 528, 204, 191, 64, 64, "game content at 1:1 (alpha quad)");

  // ---- present pass: scaled letterbox (1.5x, pillarboxed 1280x720) ----
  popts.window_w = 1280;
  popts.window_h = 720;
  popts.draw_region_w = 960;
  popts.draw_region_h = 720;
  if (metal_renderer::read_present_frame(popts, &present)) {
    printf("[PASS] read back %dx%d present frame (1.5x scale)\n", present.width, present.height);
    check_pixel(present, 80, 360, 0, 0, 0, "left pillarbox bar at 1.5x");
    check_pixel(present, 1200, 360, 0, 0, 0, "right pillarbox bar at 1.5x");
    check_pixel(present, 640, 360, 255, 128, 128, "game center scaled 1.5x");
    check_pixel(present, 310, 600, 128, 128, 128, "gray base scaled 1.5x");
  } else {
    check(false, "read back 1.5x present frame");
  }

  // ---- present pass: pmode-alp blackout ----
  popts.window_w = 800;
  popts.window_h = 600;
  popts.draw_region_w = 640;
  popts.draw_region_h = 480;
  popts.pmode_alp = 0.5f;
  if (metal_renderer::read_present_frame(popts, &present)) {
    check_pixel(present, 400, 300, 128, 64, 64, "pmode-alp 0.5 blackout halves content");
    check_pixel(present, 40, 300, 0, 0, 0, "letterbox bar stays black under blackout");
  } else {
    check(false, "read back blackout present frame");
  }
  popts.pmode_alp = 1.f;

  // ---- present pass: brightness/contrast (subtractive path) ----
  popts.brightness_contrast_color = -32;  // subtract 0.25
  if (metal_renderer::read_present_frame(popts, &present)) {
    check_pixel(present, 161, 300, 64, 64, 64, "brightness -32 subtracts 0.25 from gray");
  } else {
    check(false, "read back brightness present frame");
  }

  // the extra present readbacks reuse the same target formats: only the blackout
  // draw may have added one PSO
  auto s3 = metal_renderer::get_stats();
  check(s3.pso_count <= s2.pso_count + 1, "present verification added at most the blackout PSO");

  // ---- texture path ----
  test_upload_and_samplers();
  test_ps2_formats();
  std::vector<u8> fake_ee_mem(1 << 20, 0);
  test_pool_and_hooks(mod, fake_ee_mem);
  std::unique_ptr<tfrag3::Level> level;
  if (!fr3_path.empty()) {
    level = test_real_fr3(fr3_path.c_str());
  } else {
    printf("(no .fr3 path given; skipping optional real-texture test)\n");
  }

  // ---- level-geometry stage (stage 5) ----
  test_background_common_parity();
  if (level) {
    test_real_fr3_geometry(fr3_path.c_str(), level.get());
  }

  // ---- DMA chain path (stage 4) ----
  test_dma_chain(mod, display, level.get());
  test_sprite_chain(mod, display);
  test_merc_chain(mod, display);
  test_generic2_chain(mod, display);
  g_ee_main_mem = nullptr;

  display.reset();
  mod->exit();

  if (g_fail_count == 0) {
    printf("METAL PROOF PASSED\n");
    return 0;
  }
  printf("METAL PROOF FAILED: %d check(s) failed\n", g_fail_count);
  return 1;
}
