// Shadow renderer shader for the Metal backend.
//
// MSL port of game/graphics/opengl_renderer/shaders/shadow.{vert,frag}, line
// for line. Deliberate differences:
//  - Metal clip-space z is [0,1] while GL's is [-1,1], so the GL
//    `z * 2 - 1` becomes plain `z`.
//  - The GL SCISSOR_ADJUST text substitution arrives as a uniform.
//  - Vertices are read from a device buffer by vertex_id, so the CPU-side
//    ShadowVu::Vertex struct is used with its exact layout.

#include <metal_stdlib>
using namespace metal;

// Must match ShadowVu::Vertex (16 bytes).
struct ShadowVertexIn {
  packed_float3 xyz;
  uint flag;
};

// Must match ShadowVsParams in metal_shadow_renderer.mm.
struct ShadowVsParams {
  float scissor_adjust;
  float pad[3];
};

struct ShadowVSOut {
  float4 pos [[position]];
};

vertex ShadowVSOut shadow_vs(uint vid [[vertex_id]],
                             const device ShadowVertexIn* verts [[buffer(0)]],
                             constant ShadowVsParams& p [[buffer(1)]]) {
  float3 position_in = float3(verts[vid].xyz);
  ShadowVSOut out;
  // note: position.y is multiplied by 32 instead of 16 to undo the half-height
  // for interlacing stuff.
  out.pos = float4((position_in.x - 0.5) * 16.0, -(position_in.y - 0.5) * 32.0, position_in.z, 1.0);
  // scissoring area adjust
  out.pos.y *= p.scissor_adjust;
  return out;
}

fragment float4 shadow_fs(constant float4& color_uniform [[buffer(0)]]) {
  return color_uniform * 2.0;
}

// Exact Metal clip-space form of the Jak II OpenGL Shadow2 shader. The volume path keeps
// Shadow2's perspective/fog/HVDF transform; only the final z conversion changes from GL's
// [-w,+w] clip range to Metal's [0,+w] range.
struct Shadow2VsParams {
  float4 perspective[4];
  float4 hvdf_offset;
  float fog;
  float height_scale;
  float scissor_adjust;
  uint clear_mode;
};

vertex ShadowVSOut shadow2_vs(uint vid [[vertex_id]],
                              const device ShadowVertexIn* verts [[buffer(0)]],
                              constant Shadow2VsParams& p [[buffer(1)]]) {
  const float3 position_in = float3(verts[vid].xyz);
  ShadowVSOut out;
  if (p.clear_mode != 0) {
    out.pos = float4((position_in.x - 0.5) * 16.0,
                     -(position_in.y - 0.5) * 32.0,
                     position_in.z,
                     1.0);
    out.pos.y *= p.scissor_adjust;
    return out;
  }

  float4 transformed = -p.perspective[3];
  transformed -= p.perspective[0] * position_in.x;
  transformed -= p.perspective[1] * position_in.y;
  transformed -= p.perspective[2] * position_in.z;
  transformed.xyz *= p.fog / transformed.w;
  transformed.xyz += p.hvdf_offset.xyz;
  transformed.xy -= 2048.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.z /= 16777216.0;
  transformed.xyz *= transformed.w;
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = transformed;
  return out;
}

fragment float4 shadow2_fs(constant float4& color_uniform [[buffer(0)]]) {
  return color_uniform * 0.5;
}
