// Ocean shaders for the Metal backend.
//
// MSL ports of game/graphics/opengl_renderer/shaders/ocean_texture.{vert,frag},
// ocean_texture_mipmap.{vert,frag} and ocean_common.{vert,frag}. The math is
// kept line for line; the deliberate differences are the target conventions:
//  - Metal clip-space z is [0,1] while GL's is [-1,1], so the GL `z * 2 - 1`
//    becomes plain `z`.
//  - Metal render targets are top-down while GL framebuffers are bottom-up.
//    That only matters for the *generated* ocean texture, which is rendered and
//    then sampled: ocean_texture_vs negates y so the texture ends up in the
//    orientation the ocean mesh's s/t coordinates expect. On-screen geometry
//    (ocean_common) needs no flip - the same clip-space math shows the same
//    image in both APIs.
//  - The GL SCISSOR_ADJUST / HEIGHT_SCALE text substitutions arrive as a
//    uniform.

#include <metal_stdlib>
using namespace metal;

// --- generated ocean texture -----------------------------------------------

// Must match OceanTextureVu::Vertex (16 bytes). Positions come from a separate
// static buffer, exactly like the GL renderer's two vertex buffers.
struct OceanTextureDynamicIn {
  packed_float2 st;
  uchar4 rgba;
  uint pad;
};

struct OceanTextureVSOut {
  float4 pos [[position]];
  float4 fragment_color;
  float2 tex_coord;
};

vertex OceanTextureVSOut ocean_texture_vs(uint vid [[vertex_id]],
                                          const device packed_float2* positions [[buffer(0)]],
                                          const device OceanTextureDynamicIn* dyn [[buffer(1)]]) {
  OceanTextureVSOut out;
  // inputs are 0 - 2048
  float2 p = float2(positions[vid]);
  out.pos = float4((p.x - 1024.0) / 1024.0, -(p.y - 1024.0) / 1024.0, 0.5, 1.0);
  float4 rgba = float4(dyn[vid].rgba) / 255.0;
  out.fragment_color = float4(rgba.rgb * 2.0, 1.0);
  out.tex_coord = float2(dyn[vid].st);
  return out;
}

fragment float4 ocean_texture_fs(OceanTextureVSOut in [[stage_in]],
                                 texture2d<float> tex_T0 [[texture(0)]],
                                 sampler samp [[sampler(0)]]) {
  return in.fragment_color * tex_T0.sample(samp, in.tex_coord);
}

// --- ocean texture mip chain ------------------------------------------------

struct OceanMipmapVSOut {
  float4 pos [[position]];
  float2 tex_coord;
};

/*!
 * Full-target quad. The GL version scales the quad by 1/(1<<level) because it
 * keeps the level-0 viewport for every mip level; in Metal the render pass
 * targets the level directly, so the viewport is already the level's size and
 * the quad is a plain 1:1 copy. Same pixels either way.
 */
vertex OceanMipmapVSOut ocean_texture_mipmap_vs(uint vid [[vertex_id]]) {
  float2 xy[4] = {float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)};
  float2 uv[4] = {float2(0, 1), float2(0, 0), float2(1, 1), float2(1, 0)};
  OceanMipmapVSOut out;
  out.pos = float4(xy[vid], 0.5, 1.0);
  out.tex_coord = uv[vid];
  return out;
}

fragment float4 ocean_texture_mipmap_fs(OceanMipmapVSOut in [[stage_in]],
                                        constant float& alpha_intensity [[buffer(0)]],
                                        texture2d<float> tex_T0 [[texture(0)]],
                                        sampler samp [[sampler(0)]]) {
  float4 tex = tex_T0.sample(samp, in.tex_coord);
  tex.w *= alpha_intensity;
  return tex;
}

// --- ocean mesh (near / mid) ------------------------------------------------

// Must match MetalCommonOceanRenderer::Vertex (32 bytes, same layout as the GL
// CommonOceanRenderer::Vertex).
struct OceanCommonVertexIn {
  packed_float3 xyz;  // 0
  uchar4 rgba;        // 12
  packed_float3 stq;  // 16
  uchar4 fog;         // 28
};

struct OceanCommonParams {
  float4 fog_color;
  int bucket;
  float scissor_adjust;  // SCISSOR_ADJUST * HEIGHT_SCALE
  float4x4 view_clip_from_game_clip;
};

struct OceanCommonVSOut {
  float4 pos [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fog;
};

vertex OceanCommonVSOut ocean_common_vs(uint vid [[vertex_id]],
                                        const device OceanCommonVertexIn* verts [[buffer(0)]],
                                        constant OceanCommonParams& p [[buffer(1)]]) {
  OceanCommonVertexIn v = verts[vid];
  float3 position_in = float3(v.xyz);
  float4 rgba_in = float4(v.rgba) / 255.0;

  OceanCommonVSOut out;
  float4 game_clip =
      float4((position_in.x - 0.5) * 16.0, -(position_in.y - 0.5) * 32.0, position_in.z, 1.0);
  // scissoring area adjust
  game_clip.y *= p.scissor_adjust;
  out.pos = p.view_clip_from_game_clip * game_clip;
  out.fragment_color = float4(rgba_in.rgb, rgba_in.a * 2.0);
  out.tex_coord = float3(v.stq);
  out.fog = 255.0 - float(v.fog.x);

  if (p.bucket == 0) {
    out.fragment_color.rgb *= 2.0;
  } else if (p.bucket == 1 || p.bucket == 3) {
    out.fragment_color *= 2.0;
  } else if (p.bucket == 4) {
    out.fragment_color.a = 0.0;
  }
  return out;
}

fragment float4 ocean_common_fs(OceanCommonVSOut in [[stage_in]],
                                constant OceanCommonParams& p [[buffer(0)]],
                                texture2d<float> tex_T0 [[texture(0)]],
                                sampler samp [[sampler(0)]]) {
  float4 T0 = tex_T0.sample(samp, in.tex_coord.xy / in.tex_coord.z);
  float4 color = float4(0.0);
  if (p.bucket == 0) {
    color.rgb = in.fragment_color.rgb * T0.rgb;
    color.a = in.fragment_color.a;
    color.rgb = mix(color.rgb, p.fog_color.rgb, clamp(p.fog_color.a * in.fog, 0.0, 1.0));
  } else if (p.bucket == 1 || p.bucket == 2 || p.bucket == 4) {
    color = in.fragment_color * T0;
  } else if (p.bucket == 3) {
    color = in.fragment_color * T0;
    color.rgb = mix(color.rgb, p.fog_color.rgb, clamp(p.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}

// --- Jak II ocean envmap standalone proof ---------------------------------

struct OceanEnvmapHazeVertexIn {
  packed_float2 pos;
  packed_float4 color;
};

struct OceanEnvmapHazeVSOut {
  float4 pos [[position]];
  float4 color;
};

vertex OceanEnvmapHazeVSOut ocean_envmap_haze_vs(
    uint vid [[vertex_id]],
    const device OceanEnvmapHazeVertexIn* verts [[buffer(0)]]) {
  OceanEnvmapHazeVSOut out;
  out.pos = float4(float2(verts[vid].pos), 0.0, 1.0);
  out.color = float4(verts[vid].color);
  return out;
}

fragment float4 ocean_envmap_haze_fs(OceanEnvmapHazeVSOut in [[stage_in]]) {
  return in.color;
}

struct OceanEnvmapRadialVSOut {
  float4 pos [[position]];
  float2 tex_coord;
};

vertex OceanEnvmapRadialVSOut ocean_envmap_radial_vs(uint vid [[vertex_id]]) {
  float2 xy[4] = {float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)};
  OceanEnvmapRadialVSOut out;
  out.pos = float4(xy[vid], 0.0, 1.0);
  // GL's framebuffer coordinates are bottom-up; Metal texture coordinates are
  // top-down. Carry GL-equivalent coordinates and invert only at sampling.
  out.tex_coord = (xy[vid] + 1.0) * 0.5;
  return out;
}

fragment float4 ocean_envmap_radial_fs(OceanEnvmapRadialVSOut in [[stage_in]],
                                       texture2d<float> tex_T1 [[texture(0)]],
                                       sampler samp [[sampler(0)]]) {
  constexpr float kPi = 3.14159265358979;
  float theta = (0.5 - in.tex_coord.x) * 2.0 * kPi;
  float t = 1.0 - abs(2.0 * in.tex_coord.y - 1.0);
  float2 st = float2(0.5) + t * 0.5 * float2(sin(theta), cos(theta));
  return tex_T1.sample(samp, float2(st.x, 1.0 - st.y));
}
