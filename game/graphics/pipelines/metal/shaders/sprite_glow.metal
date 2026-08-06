// Diagnostic Jak 2 final glow-flare pass. The depth probe and downsample
// stages are intentionally absent: visibility and glow boost are both one.

#include <metal_stdlib>
using namespace metal;

// Must match GlowVertex in metal_glow_renderer.mm.
struct GlowVertexIn {
  packed_float4 position;
  packed_float4 color;
  packed_float2 uv;
};

struct GlowVsParams {
  float height_scale;
  float scissor_adjust;
};

struct GlowVSOut {
  float4 position [[position]];
  float4 color [[flat]];
  float2 uv;
};

vertex GlowVSOut sprite_glow_vs(uint vertex_id [[vertex_id]],
                                const device GlowVertexIn* vertices [[buffer(0)]],
                                constant GlowVsParams& params [[buffer(1)]]) {
  const GlowVertexIn v = vertices[vertex_id];
  GlowVSOut out;
  float4 transformed = float4(v.position);
  transformed.xy -= 2048.0;
  // GL maps z / 8388608 - 1 into [-1, 1]. Metal's equivalent [0, 1]
  // conversion is z / 16777216.
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= params.scissor_adjust * params.height_scale;
  out.position = transformed;
  out.color = float4(v.color);
  out.uv = float2(v.uv);
  return out;
}

fragment float4 sprite_glow_fs(GlowVSOut in [[stage_in]],
                               texture2d<float> texture [[texture(0)]],
                               sampler texture_sampler [[sampler(0)]]) {
  const float4 texture_color = texture.sample(texture_sampler, in.uv);
  float4 color;
  color.rgb = texture_color.rgb * in.color.rgb * (2.0 / 128.0);
  color.a = in.color.a * texture_color.a;
  return color;
}
