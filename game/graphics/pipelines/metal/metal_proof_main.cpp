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
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <unordered_map>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma_chain_read.h"
#include "common/log/log.h"
#include "common/texture/texture_conversion.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
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
void write_fake_tpage(std::vector<u8>& ee, u32 tpage_addr, u16 page_id, u32 dest0, u32 dest2) {
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
  tex.w = 16;
  tex.h = 16;
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
// Section (optional): real extracted Jak 1 textures from a user-supplied .fr3.
// Never bundled; pass the path on the command line to enable.
// ---------------------------------------------------------------------------
void test_real_fr3(const char* path) {
  printf("--- texture path: real fr3 textures (%s) ---\n", path);
  if (!fs::exists(path)) {
    printf("[FAIL] fr3 file does not exist: %s\n", path);
    g_fail_count++;
    return;
  }
  auto compressed = file_util::read_binary_file(std::string(path));
  auto decomp = compression::decompress_zstd(compressed.data(), compressed.size());
  u16 version = 0;
  memcpy(&version, decomp.data(), 2);
  if (version != tfrag3::TFRAG3_VERSION) {
    printf("[SKIP] fr3 version %d does not match this build's %d; skipping real-data test\n",
           version, tfrag3::TFRAG3_VERSION);
    return;
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
}

}  // namespace

int main(int argc, char** argv) {
  lg::initialize();
  // this proof needs no game data; any existing directory works as the project path
  file_util::setup_project_path(fs::current_path());

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

  auto display = mod->make_display(640, 480, "OpenGOAL Metal Proof", settings, GameVersion::Jak1,
                                   /*is_main=*/true);
  if (!display) {
    printf("[FAIL] Metal make_display failed\n");
    mod->exit();
    return 1;
  }
  printf("[PASS] Metal display created\n");

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
  if (argc > 1) {
    test_real_fr3(argv[1]);
  } else {
    printf("(no .fr3 path given; skipping optional real-texture test)\n");
  }
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
