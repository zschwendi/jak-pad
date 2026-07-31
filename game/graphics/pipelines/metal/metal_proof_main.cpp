/*!
 * @file metal_proof_main.cpp
 * Standalone proof that the Metal backend's frame scaffolding works through the
 * GfxRendererModule interface (the same seam the OpenGL pipeline sits behind).
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
 */

#include <cstdio>
#include <cstdlib>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"

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

}  // namespace

int main() {
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

  display.reset();
  mod->exit();

  if (g_fail_count == 0) {
    printf("METAL PROOF PASSED\n");
    return 0;
  }
  printf("METAL PROOF FAILED: %d check(s) failed\n", g_fail_count);
  return 1;
}
