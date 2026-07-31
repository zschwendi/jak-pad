#pragma once

/*!
 * @file metal_pipeline.h
 * Experimental Metal graphics pipeline for Apple platforms.
 *
 * This implements the same GfxRendererModule interface that the OpenGL pipeline
 * (game/graphics/pipelines/opengl.cpp) sits behind. It currently renders a fixed
 * validation frame (textured, depth-tested geometry) instead of the game's DMA
 * chain; it exists to prove the backend seam and the Metal presentation path.
 */

#include <vector>

#include "common/common_types.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"

extern const GfxRendererModule gRendererMetal;

namespace metal_renderer {

// RGBA8 copy of the most recently rendered offscreen frame, used by tests to
// verify that rendering actually happened. Origin is the top-left corner.
struct FramePixels {
  int width = 0;
  int height = 0;
  std::vector<u8> rgba;
};

// Blocks until the last submitted frame finishes on the GPU, then reads it back.
// Returns false if no frame has been rendered.
bool read_last_frame(FramePixels* out);

}  // namespace metal_renderer
