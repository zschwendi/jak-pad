#pragma once

#include "common/dma/gs.h"
#include "common/math/Vector.h"

struct SpriteGlowData {
  float pos[3];
  float size_x;

  float size_probe;
  float z_offset;
  float rot_angle;
  float size_y;

  float color[4];

  float fade_a;
  float fade_b;
  u32 tex_id;
  u32 dummy;
};
static_assert(sizeof(SpriteGlowData) == 16 * 4);

/*!
 * Post-transformation description of a sprite glow - passed to a renderer.
 */
struct SpriteGlowOutput {
  math::Vector4f first_clear_pos[2];   // 8, 9
  math::Vector4f second_clear_pos[2];  // 11, 12 corners for the second clear draw
  math::Vector2f offscreen_uv[2];      // 24, 26
  math::Vector4f flare_xyzw[4];
  AdGifData adgif;                  // 68, 69, 70, 71, 72
  math::Vector4f flare_draw_color;  // 75
  float perspective_q;
};

struct SpriteGlowConsts {
  math::Vector4f camera[4];
  math::Vector4f perspective[4];
  math::Vector4f hvdf;
  math::Vector4f hmge;
  float pfog0;
  float deg_to_rad;
  float min_scale;
  float inv_area;
  math::Vector4f sincos[5];
  math::Vector4f basis_x;
  math::Vector4f basis_y;
  math::Vector4f xy_array[4];
  math::Vector4f clamp_min;
  math::Vector4f clamp_max;
};
static_assert(sizeof(SpriteGlowConsts) == 0x180);

/*!
 * Transformation math from the sprite-glow vu1 program.
 * Populates the SpriteGlowOutput struct with the same data that would get filled into the
 * output template on VU1. Excludes float to int conversions.
 * Returns false without modifying out when the sprite is clipped or the inputs cannot produce
 * finite output.
 */
bool glow_math(const SpriteGlowConsts* consts,
               bool skip_uv_clamp,
               const void* vec_data,
               const void* adgif_data,
               SpriteGlowOutput* out);
