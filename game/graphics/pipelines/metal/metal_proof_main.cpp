/*!
 * @file metal_proof_main.cpp
 * Standalone proof that the Metal graphics backend renders correctly through the
 * GfxRendererModule interface (the same seam the OpenGL pipeline sits behind).
 *
 * It selects the Metal renderer, opens a real window, renders frames, then reads
 * back the offscreen frame and asserts:
 *  - the clear color is present outside all geometry
 *  - the far quad rendered where it is unoccluded
 *  - the checkerboard texture sampled correctly (two different checker cells)
 *  - the near quad occludes the far quad even though the far quad is drawn
 *    later (proving depth testing)
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

  // render some frames through the GfxDisplay interface, like Gfx::Loop does
  for (int i = 0; i < 30; i++) {
    display->render();
  }

  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    printf("[FAIL] could not read back rendered frame\n");
    display.reset();
    mod->exit();
    return 1;
  }
  printf("[PASS] read back %dx%d frame\n", frame.width, frame.height);

  // Scene layout in the 640x480 offscreen frame:
  //  - clear color (0.1, 0.2, 0.4) everywhere else
  //  - far green quad covering x in [32, 608], y in [24, 456], z = 0.75, drawn last
  //  - near checkerboard quad covering x in [160, 480], y in [120, 360], z = 0.25,
  //    drawn first; 8x8 checker cells of 40x30 px, red when (cx+cy) is odd
  check_pixel(frame, 5, 5, 26, 51, 102, "clear color outside all geometry");
  check_pixel(frame, 80, 240, 0, 255, 0, "far quad where unoccluded");
  check_pixel(frame, 320, 60, 0, 255, 0, "far quad above near quad");
  check_pixel(frame, 300, 240, 255, 0, 0, "texture red cell + depth occlusion");
  check_pixel(frame, 340, 240, 255, 255, 255, "texture white cell + depth occlusion");

  display.reset();
  mod->exit();

  if (g_fail_count == 0) {
    printf("METAL PROOF PASSED\n");
    return 0;
  }
  printf("METAL PROOF FAILED: %d check(s) failed\n", g_fail_count);
  return 1;
}
