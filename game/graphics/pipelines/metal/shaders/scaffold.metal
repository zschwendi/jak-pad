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
  float2 inverse_source_size;
  uint modern_effects;
  uint grain_seed;
};

constant uint kModernFilmicColor = 1u << 0;
constant uint kModernEdgeSmoothing = 1u << 1;
constant uint kModernClarity = 1u << 2;
constant uint kModernSoftHighlights = 1u << 3;
constant uint kModernVignette = 1u << 4;
constant uint kModernFilmGrain = 1u << 5;
constant uint kModernAllEffects = (1u << 6) - 1u;

float present_luminance(float3 color) {
  return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float present_grain(uint2 pixel, uint seed) {
  uint value = pixel.x * 1973u + pixel.y * 9277u + seed * 26699u + 0x68bc21ebu;
  value = (value ^ (value >> 13u)) * 1274126177u;
  return float(value & 1023u) / 1023.0 - 0.5;
}

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
  float4 classic =
      float4(tex.sample(s, in.uv).rgb * params.color_mult.rgb * params.color_mult.a, 1.0) +
      params.color_add;
  uint effects = params.modern_effects & kModernAllEffects;
  if (effects == 0u) {
    return classic;
  }

  float3 color = saturate(classic.rgb);
  float3 source_color = color;
  uint neighborhood_effects =
      kModernEdgeSmoothing | kModernClarity | kModernSoftHighlights;
  if ((effects & neighborhood_effects) != 0u) {
    float2 texel = params.inverse_source_size;
    float3 present_mult = params.color_mult.rgb * params.color_mult.a;
    float3 present_add = params.color_add.rgb;
    float3 north = saturate(tex.sample(s, in.uv + float2(0.0, -texel.y)).rgb * present_mult +
                            present_add);
    float3 south = saturate(tex.sample(s, in.uv + float2(0.0, texel.y)).rgb * present_mult +
                            present_add);
    float3 west = saturate(tex.sample(s, in.uv + float2(-texel.x, 0.0)).rgb * present_mult +
                           present_add);
    float3 east = saturate(tex.sample(s, in.uv + float2(texel.x, 0.0)).rgb * present_mult +
                           present_add);
    float3 neighborhood = (north + south + west + east) * 0.25;

    if ((effects & kModernEdgeSmoothing) != 0u) {
      float center_luma = present_luminance(color);
      float north_luma = present_luminance(north);
      float south_luma = present_luminance(south);
      float west_luma = present_luminance(west);
      float east_luma = present_luminance(east);
      float luma_delta = max(max(abs(north_luma - center_luma),
                                 abs(south_luma - center_luma)),
                             max(abs(west_luma - center_luma),
                                 abs(east_luma - center_luma)));
      float horizontal_gradient = abs(west_luma - east_luma);
      float vertical_gradient = abs(north_luma - south_luma);
      float3 along_edge = vertical_gradient > horizontal_gradient
                              ? (west + east) * 0.5
                              : (north + south) * 0.5;
      color = mix(color, along_edge, smoothstep(0.035, 0.20, luma_delta) * 0.38);
    }
    if ((effects & kModernClarity) != 0u) {
      float3 detail = source_color - neighborhood;
      float detail_luma = abs(present_luminance(source_color) -
                              present_luminance(neighborhood));
      float detail_weight = smoothstep(0.008, 0.16, detail_luma);
      color = saturate(color + detail * detail_weight * 0.30);
    }
    if ((effects & kModernSoftHighlights) != 0u) {
      float highlight = smoothstep(0.45, 0.82, present_luminance(neighborhood));
      float3 screened = 1.0 - (1.0 - neighborhood) * (1.0 - neighborhood);
      color = saturate(color + max(screened - color, 0.0) * highlight * 0.32);
    }
  }

  if ((effects & kModernFilmicColor) != 0u) {
    float luma = present_luminance(color);
    float toned_luma = saturate(luma +
                                0.42 * luma * (1.0 - luma) * (2.0 * luma - 1.0));
    color = saturate(float3(toned_luma) + (color - float3(luma)) * 1.16);
  }
  if ((effects & kModernVignette) != 0u) {
    float2 centered = in.uv * 2.0 - 1.0;
    float radius = dot(centered * centered, float2(0.72, 1.0));
    color *= 1.0 - smoothstep(0.22, 1.20, radius) * 0.16;
  }
  if ((effects & kModernFilmGrain) != 0u) {
    float grain = present_grain(uint2(in.pos.xy), params.grain_seed);
    float shadow_weight = mix(1.0, 0.65, present_luminance(color));
    color = saturate(color + grain * shadow_weight * (6.0 / 255.0));
  }
  return float4(color, 1.0);
}
