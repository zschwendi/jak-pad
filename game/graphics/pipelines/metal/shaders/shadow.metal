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
