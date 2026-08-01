// Background (tfrag / tie / shrub) shaders for the Metal backend.
//
// MSL ports of game/graphics/opengl_renderer/shaders/tfrag3.{vert,frag},
// etie_base.{vert,frag} and shrub.{vert,frag}. The math is kept line for line;
// the deliberate differences are the target conventions:
//  - Metal clip-space z is [0,1] while GL's is [-1,1]. The GL shaders produce a
//    z whose perspective divide lands in [-1,1]; the same depth in Metal's
//    range is (z + w) / 2 (equivalently, etie's `z / 8388608 - 1` becomes
//    `z / 16777216`, matching the ported sprite shader).
//  - The GL HEIGHT_SCALE / SCISSOR_ADJUST compile-time substitutions arrive as
//    uniform values instead.
//  - GL's `sampler1D` time-of-day palette with texelFetch becomes a
//    `texture1d` with read(). Both are unfiltered integer lookups, so the
//    sampled colors are the same.
//  - Vertices are read from a device buffer by vertex_id instead of through a
//    vertex descriptor, so the fr3 vertex structs are used with their exact
//    on-disk layout (no repacking at load time).

#include <metal_stdlib>
using namespace metal;

// Must match tfrag3::PreloadedVertex (32 bytes) - used by tfrag and tie.
struct BackgroundVertexIn {
  packed_float3 position;  // 0
  uchar4 rgba;             // 12 (envmap tint; unused by these shaders)
  packed_float2 st;        // 16
  uint nor;                // 24
  ushort color_index;      // 28
  ushort pad;              // 30
};

// Must match tfrag3::ShrubGpuVertex (32 bytes).
struct ShrubVertexIn {
  packed_float3 position;  // 0
  packed_float2 st;        // 12
  uint pad0;               // 20
  ushort color_index;      // 24
  ushort pad1;             // 26
  uchar4 rgba_base;        // 28 (rgb + pad)
};

// Must match MetalBackgroundVsParams in metal_level_data.h.
struct BackgroundVsParams {
  float4x4 pc_camera;
  float4 hvdf_offset;
  float4 cam_trans;
  float fog_min;
  float fog_max;
  float height_scale;
  float scissor_adjust;
};

// Must match MetalEtieVsParams in metal_level_data.h.
struct EtieVsParams {
  float4x4 cam_no_persp;
  float4x4 camera;
  float4 persp0;
  float4 persp1;
  float4 hvdf_offset;
  float fog_min;
  float fog_max;
  float height_scale;
  float scissor_adjust;
};

// Must match MetalBackgroundDrawParams in metal_level_data.h.
struct BackgroundDrawParams {
  int decal;
  int pad[3];
};

// Must match MetalBackgroundFsParams in metal_level_data.h.
struct BackgroundFsParams {
  float4 fog_color;
  float alpha_min;
  float alpha_max;
  int gfx_hack_no_tex;
  int pad;
};

struct BackgroundVSOut {
  float4 pos [[position]];
  float4 fragment_color;
  float3 tex_coord;
  float fogginess;
};

// GL's depth lands in [-1, 1] after the perspective divide; Metal wants [0, 1].
static float4 to_metal_clip_depth(float4 clip) {
  clip.z = (clip.z + clip.w) * 0.5;
  return clip;
}

// ---------------------------------------------------------------------------
// tfrag3 (also the TIE base draw)
// ---------------------------------------------------------------------------

vertex BackgroundVSOut tfrag3_vs(uint vid [[vertex_id]],
                                 const device BackgroundVertexIn* verts [[buffer(0)]],
                                 constant BackgroundVsParams& p [[buffer(1)]],
                                 constant BackgroundDrawParams& d [[buffer(2)]],
                                 texture1d<float> tod [[texture(1)]]) {
  BackgroundVertexIn v = verts[vid];
  BackgroundVSOut out;

  float3 vert = float3(v.position) - p.cam_trans.xyz;
  float4 transformed = -p.pc_camera[3];
  transformed.w = 0;
  transformed -= p.pc_camera[0] * vert.x;
  transformed -= p.pc_camera[1] * vert.y;
  transformed -= p.pc_camera[2] * vert.z;

  out.fogginess = 255.0 - clamp(-transformed.w + p.hvdf_offset.w, p.fog_min, p.fog_max);

  // scissoring area adjust
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = to_metal_clip_depth(transformed);

  // time of day lookup
  float4 color = tod.read(uint(v.color_index));
  color *= 2.0;
  color.a *= 2.0;
  if (d.decal == 1) {
    // tfrag/tie always use TCC=RGB, so even with decal, alpha comes from the
    // fragment.
    color.xyz = float3(1.0, 1.0, 1.0);
  }
  out.fragment_color = color;

  out.tex_coord = float3(float2(v.st), 0.0);
  return out;
}

fragment float4 tfrag3_fs(BackgroundVSOut in [[stage_in]],
                          constant BackgroundFsParams& p [[buffer(0)]],
                          texture2d<float> tex [[texture(0)]],
                          sampler samp [[sampler(0)]]) {
  float4 color;
  if (p.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.tex_coord.xy);
    color = in.fragment_color * T0;
  } else {
    color = in.fragment_color / 2.0;
  }

  if (color.a < p.alpha_min || color.a > p.alpha_max) {
    discard_fragment();
  }

  color.rgb = mix(color.rgb, p.fog_color.rgb, clamp(in.fogginess * p.fog_color.a, 0.0, 1.0));
  return color;
}

// ---------------------------------------------------------------------------
// etie_base: the base draw of an envmapped TIE. Same output as tfrag3_vs, but
// it reproduces the game's own perspective math so the base and the shiny
// second draw land on exactly the same pixels.
// ---------------------------------------------------------------------------

vertex BackgroundVSOut etie_base_vs(uint vid [[vertex_id]],
                                    const device BackgroundVertexIn* verts [[buffer(0)]],
                                    constant EtieVsParams& p [[buffer(1)]],
                                    constant BackgroundDrawParams& d [[buffer(2)]],
                                    texture1d<float> tod [[texture(1)]]) {
  BackgroundVertexIn v = verts[vid];
  BackgroundVSOut out;
  float3 position_in = float3(v.position);

  float fog1 = p.camera[3].w + p.camera[0].w * position_in.x + p.camera[1].w * position_in.y +
               p.camera[2].w * position_in.z;
  out.fogginess = 255.0 - clamp(fog1 + p.hvdf_offset.w, p.fog_min, p.fog_max);

  float4 vf17 = p.cam_no_persp[3];
  vf17 += p.cam_no_persp[0] * position_in.x;
  vf17 += p.cam_no_persp[1] * position_in.y;
  vf17 += p.cam_no_persp[2] * position_in.z;
  float4 p_proj = float4(p.persp1.x * vf17.x, p.persp1.y * vf17.y, p.persp1.z, p.persp1.w);
  p_proj += p.persp0 * vf17.z;

  float pQ = 1.0 / p_proj.w;
  float4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

  transformed.xy -= 2048.0;
  // GL: z / 8388608 - 1 into [-1, 1]; the same depth in Metal's [0, 1] range
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = transformed;

  if (d.decal == 1) {
    out.fragment_color = float4(1.0, 1.0, 1.0, 1.0);
  } else {
    float4 color = tod.read(uint(v.color_index));
    color *= 2.0;
    color.a *= 2.0;
    out.fragment_color = color;
  }

  out.tex_coord = float3(float2(v.st), 0.0);
  return out;
}

// ---------------------------------------------------------------------------
// shrub
// ---------------------------------------------------------------------------

vertex BackgroundVSOut shrub_vs(uint vid [[vertex_id]],
                                const device ShrubVertexIn* verts [[buffer(0)]],
                                constant BackgroundVsParams& p [[buffer(1)]],
                                constant BackgroundDrawParams& d [[buffer(2)]],
                                texture1d<float> tod [[texture(1)]]) {
  ShrubVertexIn v = verts[vid];
  BackgroundVSOut out;

  float3 vert = float3(v.position) - p.cam_trans.xyz;
  float4 transformed = -p.pc_camera[3];
  transformed -= p.pc_camera[0] * vert.x;
  transformed -= p.pc_camera[1] * vert.y;
  transformed -= p.pc_camera[2] * vert.z;

  out.fogginess = 255.0 - clamp(-transformed.w + p.hvdf_offset.w, p.fog_min, p.fog_max);

  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = to_metal_clip_depth(transformed);

  // start with the vertex color (only rgb, VIF filled in the 255)
  float4 color = float4(float3(v.rgba_base.xyz) / 255.0, 1.0);
  float4 tod_color = tod.read(uint(v.color_index));
  color *= tod_color * 4.0;
  if (d.decal == 1) {
    color.xyz = float3(1.0, 1.0, 1.0);
  }
  out.fragment_color = color;

  out.tex_coord = float3(float2(v.st) / 4096.0, 0.0);
  return out;
}

fragment float4 shrub_fs(BackgroundVSOut in [[stage_in]],
                         constant BackgroundFsParams& p [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
  float4 color;
  if (p.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.tex_coord.xy);
    color = in.fragment_color * T0;
  } else {
    color = in.fragment_color;
  }

  if (color.a < p.alpha_min || color.a > p.alpha_max) {
    discard_fragment();
  }

  color.xyz = mix(color.xyz, p.fog_color.rgb, clamp(in.fogginess * p.fog_color.a, 0.0, 1.0));
  return color;
}
