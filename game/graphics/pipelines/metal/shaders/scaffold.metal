// Scaffolding shaders for the Metal backend.
//
// These are checked-in MSL sources compiled at build time into a metallib that
// is embedded in the runtime (see game/CMakeLists.txt and embed_metallib.cmake).
// No shader source is compiled at runtime; the app stays signing-clean for iPadOS.

#include <metal_stdlib>
using namespace metal;

// ---------------------------------------------------------------------------
// "scaffold" program: draws colored / textured geometry at explicit depth.
// Used by the validation scene to exercise the PSO cache and depth-stencil path.
// Vertex layout must match ScaffoldVertex in metal_renderer.h.
// ---------------------------------------------------------------------------

struct ScaffoldVertexIn {
  packed_float3 pos;
  packed_float2 uv;
  packed_float4 color;
  float use_tex;
};

struct ScaffoldVSOut {
  float4 pos [[position]];
  float2 uv;
  float4 color;
  float use_tex;
};

vertex ScaffoldVSOut scaffold_vs(uint vid [[vertex_id]],
                                 const device ScaffoldVertexIn* verts [[buffer(0)]]) {
  ScaffoldVertexIn v = verts[vid];
  ScaffoldVSOut out;
  out.pos = float4(v.pos, 1.0);
  out.uv = v.uv;
  out.color = v.color;
  out.use_tex = v.use_tex;
  return out;
}

fragment float4 scaffold_fs(ScaffoldVSOut in [[stage_in]],
                            texture2d<float> tex [[texture(0)]]) {
  constexpr sampler s(mag_filter::nearest, min_filter::nearest);
  return mix(in.color, tex.sample(s, in.uv), in.use_tex);
}

// Samples with an API-supplied sampler state (filter / wrap / mip modes from
// the MetalSamplerCache). Used by the texture-path verification and by ported
// renderers that need GL-style per-draw sampler control.
fragment float4 sample_fs(ScaffoldVSOut in [[stage_in]],
                          texture2d<float> tex [[texture(0)]],
                          sampler s [[sampler(0)]]) {
  return tex.sample(s, in.uv);
}

// ---------------------------------------------------------------------------
// "present" program: the PCRTC-style final blit. Draws the offscreen game
// frame into the letterboxed draw region of the window, applying the same
// brightness/contrast math as the OpenGL POST_PROCESSING shader
// (game/graphics/opengl_renderer/shaders/post_processing.frag).
// Drawn as a 4-vertex triangle strip with no vertex buffer.
// ---------------------------------------------------------------------------

struct PresentVSOut {
  float4 pos [[position]];
  float2 uv;
};

struct PresentParams {
  float4 color_mult;
  float4 color_add;
};

vertex PresentVSOut present_vs(uint vid [[vertex_id]]) {
  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0);
  PresentVSOut out;
  out.pos = float4(p, 0.0, 1.0);
  // NDC y=+1 is the top of the viewport and texture row 0 is the top of the
  // game frame, so v runs opposite to y.
  out.uv = float2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
  return out;
}

fragment float4 present_fs(PresentVSOut in [[stage_in]],
                           texture2d<float> tex [[texture(0)]],
                           constant PresentParams& params [[buffer(0)]]) {
  constexpr sampler s(mag_filter::linear, min_filter::linear, address::clamp_to_edge);
  return float4(tex.sample(s, in.uv).rgb * params.color_mult.rgb * params.color_mult.a, 1.0) +
         params.color_add;
}
