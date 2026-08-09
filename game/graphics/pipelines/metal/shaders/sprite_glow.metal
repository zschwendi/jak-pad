// Jak 2 glow depth probe, per-cell downsample, and final flare draw.

#include <metal_stdlib>
using namespace metal;

// Must match GlowVertex in metal_glow_renderer.mm.
struct GlowVertexIn {
  packed_float4 position;
  packed_float4 color;
  packed_float2 uv;
  packed_float2 probe_uv;
};

struct GlowProbeVertexIn {
  packed_float3 position;
  packed_float2 sample_uv;
};

struct GlowVsParams {
  float height_scale;
  float scissor_adjust;
};

struct GlowVSOut {
  float4 position [[position]];
  float4 color [[flat]];
  float2 uv;
  float2 probe_uv;
};

struct GlowProbeVSOut {
  float4 position [[position]];
  float2 sample_uv;
};

struct GlowDepthCopyOut {
  float4 color [[color(0)]];
  float depth [[depth(any)]];
};

vertex GlowProbeVSOut sprite_glow_depth_copy_vs(
    uint vertex_id [[vertex_id]],
    const device GlowProbeVertexIn* vertices [[buffer(0)]]) {
  const GlowProbeVertexIn v = vertices[vertex_id];
  GlowProbeVSOut out;
  out.position = float4(v.position.x * 2.0 - 1.0,
                        1.0 - v.position.y * 2.0,
                        0.0,
                        1.0);
  out.sample_uv = float2(v.sample_uv);
  return out;
}

fragment GlowDepthCopyOut sprite_glow_depth_copy_fs(
    GlowProbeVSOut in [[stage_in]],
    depth2d<float> game_depth [[texture(0)]],
    sampler depth_sampler [[sampler(0)]]) {
  GlowDepthCopyOut out;
  out.color = 0.0;
  if (any(in.sample_uv < 0.0) || any(in.sample_uv > 1.0)) {
    out.depth = 1.0;
  } else {
    out.depth = game_depth.sample(depth_sampler, in.sample_uv);
  }
  return out;
}

vertex GlowProbeVSOut sprite_glow_probe_vs(
    uint vertex_id [[vertex_id]],
    const device GlowProbeVertexIn* vertices [[buffer(0)]]) {
  const GlowProbeVertexIn v = vertices[vertex_id];
  GlowProbeVSOut out;
  out.position = float4(v.position.x * 2.0 - 1.0,
                        1.0 - v.position.y * 2.0,
                        v.position.z,
                        1.0);
  out.sample_uv = float2(v.sample_uv);
  return out;
}

fragment float4 sprite_glow_probe_fs(GlowProbeVSOut in [[stage_in]]) {
  return float4(0.0, 0.5, 1.0, 1.0);
}

vertex GlowProbeVSOut sprite_glow_downsample_vs(
    uint vertex_id [[vertex_id]],
    const device GlowProbeVertexIn* vertices [[buffer(0)]]) {
  const GlowProbeVertexIn v = vertices[vertex_id];
  GlowProbeVSOut out;
  out.position = float4(v.position.x * 2.0 - 1.0,
                        1.0 - v.position.y * 2.0,
                        0.0,
                        1.0);
  out.sample_uv = v.position.xy;
  return out;
}

fragment float4 sprite_glow_downsample_fs(
    GlowProbeVSOut in [[stage_in]],
    texture2d<float> source [[texture(0)]],
    sampler source_sampler [[sampler(0)]]) {
  return source.sample(source_sampler, in.sample_uv);
}

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
  out.probe_uv = float2(v.probe_uv);
  return out;
}

fragment float4 sprite_glow_fs(GlowVSOut in [[stage_in]],
                               texture2d<float> texture [[texture(0)]],
                               texture2d<float> visibility_texture [[texture(1)]],
                               sampler texture_sampler [[sampler(0)]],
                               sampler visibility_sampler [[sampler(1)]]) {
  const float4 texture_color = texture.sample(texture_sampler, in.uv);
  const float visibility = visibility_texture.sample(visibility_sampler, in.probe_uv).a;
  float4 color;
  color.rgb = texture_color.rgb * in.color.rgb * (2.0 / 128.0) * visibility;
  color.a = in.color.a * texture_color.a;
  return color;
}
