// DirectRenderer shaders for the Metal backend.
//
// MSL ports of game/graphics/opengl_renderer/shaders/direct_basic.{vert,frag}
// and direct_basic_textured.{vert,frag}. The math is kept identical; the only
// deliberate differences are the target conventions:
//  - Metal clip-space z is [0,1], so the GL `z * 2 - 1` becomes plain `z`.
//  - Metal's fragment position y runs top-down while gl_FragCoord runs
//    bottom-up, so the GS scissor compare is rewritten in top-down form
//    (y_metal = viewport_h - y_gl; same pass/fail set).
// The GL HEIGHT_SCALE / SCISSOR_ADJUST compile-time substitutions arrive as
// uniform values in DirectVsParams instead.

#include <metal_stdlib>
using namespace metal;

// Must match MetalDirectRenderer::Vertex (64 bytes, same layout as the GL
// DirectRenderer::Vertex).
struct DirectVertexIn {
  packed_float4 xyzf;     // 0
  packed_float3 stq;      // 16
  uchar4 rgba;            // 28
  uchar tex_unit;         // 32
  uchar tcc;              // 33
  uchar decal;            // 34
  uchar fog_enable;       // 35
  uchar use_uv;           // 36
  uchar pad[11];          // 37
  packed_float4 scissor;  // 48
};

struct DirectVsParams {
  float height_scale;    // 1.0 for Jak 1
  float scissor_adjust;  // 512/448 for Jak 1
  int offscreen_mode;
};

struct DirectFsParams {
  float4 fog_color;   // rgb = fog color, a = fog intensity / 255
  float4 game_sizes;  // game width, game height, viewport width, viewport height
  float alpha_min;
  float alpha_max;
  float color_mult;
  float alpha_mult;
  float ta0;
  int scissor_enable;
  int greater;
};

struct DirectBasicVSOut {
  float4 pos [[position]];
  float4 fragment_color;
  float4 gs_scissor;
};

struct DirectTexturedVSOut {
  float4 pos [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fog;
  float4 gs_scissor;
  uint4 tex_info [[flat]];  // x = unit, y = tcc, z = decal, w = fog_enable
  uint use_uv [[flat]];
};

// GS scissor test, top-down fragment coordinates. Same pass set as the GL
// direct_basic.frag / direct_basic_textured.frag scissor blocks.
static bool scissor_discard(float4 pos, float4 gs_scissor, constant DirectFsParams& params) {
  if (params.scissor_enable != 1) {
    return false;
  }
  float x = pos.x;
  float y = pos.y;
  float w = params.game_sizes.z / params.game_sizes.x;
  float h = params.game_sizes.w / params.game_sizes.y;
  float scax0 = gs_scissor.x * w + 0.5;
  float scax1 = gs_scissor.y * w + 0.5;
  // gs_scissor.z = scay0 (top), gs_scissor.w = scay1 (bottom), GS y runs down
  float scay0 = gs_scissor.z * h - 0.5;
  float scay1 = gs_scissor.w * h - 0.5;
  return x < scax0 || x > scax1 || y < scay0 || y > scay1;
}

vertex DirectBasicVSOut direct_basic_vs(uint vid [[vertex_id]],
                                        const device DirectVertexIn* verts [[buffer(0)]],
                                        constant DirectVsParams& params [[buffer(1)]]) {
  DirectVertexIn v = verts[vid];
  DirectBasicVSOut out;
  // y is multiplied by 32 instead of 16 to undo the game's half-height (see
  // direct_basic.vert)
  out.pos = float4((v.xyzf.x - 0.5) * 16.0,
                   -(v.xyzf.y - 0.5) * 32.0 * params.height_scale * params.scissor_adjust,
                   v.xyzf.z, 1.0);
  float4 rgba = float4(v.rgba) / 255.0;
  out.fragment_color = float4(rgba.xyz, rgba.w * 2.0);
  out.gs_scissor = v.scissor;
  return out;
}

fragment float4 direct_basic_fs(DirectBasicVSOut in [[stage_in]],
                                constant DirectFsParams& params [[buffer(0)]]) {
  if (scissor_discard(in.pos, in.gs_scissor, params)) {
    discard_fragment();
  }
  return in.fragment_color;
}

vertex DirectTexturedVSOut direct_textured_vs(uint vid [[vertex_id]],
                                              const device DirectVertexIn* verts [[buffer(0)]],
                                              constant DirectVsParams& params [[buffer(1)]]) {
  DirectVertexIn v = verts[vid];
  DirectTexturedVSOut out;
  if (params.offscreen_mode == 1) {
    out.pos = float4((v.xyzf.x - 0.453125) * 64.0, -(v.xyzf.y - 0.5 + (2.25 / 64.0)) * 64.0,
                     v.xyzf.z, 1.0);
  } else {
    out.pos = float4((v.xyzf.x - 0.5) * 16.0,
                     -(v.xyzf.y - 0.5) * 32.0 * params.height_scale * params.scissor_adjust,
                     v.xyzf.z, 1.0);
  }
  float4 rgba = float4(v.rgba) / 255.0;
  out.fragment_color = float4(rgba.xyz, rgba.w * 2.0);
  out.tex_coord = float3(v.stq);
  out.tex_info = uint4(v.tex_unit, v.tcc, v.decal, v.fog_enable);
  out.fog = 255.0 - v.xyzf.w;
  out.use_uv = v.use_uv;
  out.gs_scissor = v.scissor;
  return out;
}

fragment float4 direct_textured_fs(DirectTexturedVSOut in [[stage_in]],
                                   constant DirectFsParams& params [[buffer(0)]],
                                   texture2d<float> tex [[texture(0)]],
                                   sampler samp [[sampler(0)]]) {
  if (scissor_discard(in.pos, in.gs_scissor, params)) {
    discard_fragment();
  }

  float4 T0;
  if (in.use_uv == 1) {
    // pixel-space UV: fractional texels and filtering still apply, perspective
    // correction does not (q = 1 on the current uses)
    float2 coord_px = in.tex_coord.xy / 16.0;
    float2 tex_size = float2(tex.get_width(), tex.get_height());
    T0 = tex.sample(samp, coord_px / tex_size);
  } else {
    T0 = tex.sample(samp, in.tex_coord.xy / in.tex_coord.z);
  }
  if (T0.w == 0.0) {
    T0.w = params.ta0;
  }

  float4 color;
  if (in.tex_info.y == 0) {
    if (in.tex_info.z == 0) {
      // modulate + no tcc
      color = float4(in.fragment_color.xyz * T0.xyz, in.fragment_color.w);
    } else {
      // decal + no tcc
      color = float4(T0.xyz * 0.5, in.fragment_color.w);
    }
  } else {
    if (in.tex_info.z == 0) {
      // modulate + tcc
      color = in.fragment_color * T0;
    } else {
      // decal + tcc
      color = float4(T0.xyz * 0.5, T0.w);
    }
  }
  color *= 2.0;
  color.xyz *= params.color_mult;
  color.w *= params.alpha_mult;
  if (params.greater != 0) {
    // pass if alpha > min; alpha values equal to aref pass only one half of a
    // double-draw (see direct_basic_textured.frag)
    if (color.a <= params.alpha_min || color.a > params.alpha_max) {
      discard_fragment();
    }
  } else {
    if (color.a < params.alpha_min || color.a >= params.alpha_max) {
      discard_fragment();
    }
  }
  if (in.tex_info.w == 1) {
    color.xyz = mix(color.xyz, params.fog_color.rgb, clamp(params.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
