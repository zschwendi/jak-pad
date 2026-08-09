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
  float4x4 view_clip_from_game_clip;
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
  float4 envmap_tod_tint;  // only the envmap second draw uses this
  float4x4 view_clip_from_game_clip;
};

// Must match MetalBackgroundDrawParams in metal_level_data.h.
struct BackgroundDrawParams {
  int decal;
  int etie_shine;
  int pad[2];
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
  out.pos = p.view_clip_from_game_clip * to_metal_clip_depth(transformed);

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

static float quantize_background_depth24(float depth) {
  constexpr float kDepth24Max = 16777215.0;
  return round(clamp(depth, 0.0, 1.0) * kDepth24Max) / kDepth24Max;
}

struct BackgroundDepth24Out {
  float4 color [[color(0)]];
  float depth [[depth(any)]];
};

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

// Proof-only fragment entry points used by metal-proof. They let the existing
// TFRAG3 and ETIE_BASE vertex paths compete once at native D32 precision and
// once after fragment-stage D24 quantization. Production pipelines never
// reference either function.
fragment float4 background_depth32_proof_fs(BackgroundVSOut in [[stage_in]],
                                             constant float4& color [[buffer(0)]]) {
  (void)in;
  return color;
}

fragment BackgroundDepth24Out background_depth24_proof_fs(
    BackgroundVSOut in [[stage_in]], constant float4& color [[buffer(0)]]) {
  BackgroundDepth24Out out;
  out.color = color;
  out.depth = quantize_background_depth24(in.pos.z);
  return out;
}

// ---------------------------------------------------------------------------
// etie: the base and shiny envmap draws share one vertex entry point. Position
// math runs before the mode branch so both passes produce the same raster input.
// ---------------------------------------------------------------------------

vertex BackgroundVSOut etie_shared_vs(uint vid [[vertex_id]],
                                      const device BackgroundVertexIn* verts [[buffer(0)]],
                                      constant EtieVsParams& p [[buffer(1)]],
                                      constant BackgroundDrawParams& d [[buffer(2)]],
                                      texture1d<float> tod [[texture(1)]]) {
  BackgroundVertexIn v = verts[vid];
  BackgroundVSOut out;
  float3 position_in = float3(v.position);

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
  out.pos = p.view_clip_from_game_clip * transformed;

  if (d.etie_shine != 0) {
    // GL_INT_2_10_10_10_REV, normalized: three sign-extended 10-bit fields over 511
    int3 packed_nor;
    packed_nor.x = int(v.nor << 22) >> 22;
    packed_nor.y = int(v.nor << 12) >> 22;
    packed_nor.z = int(v.nor << 2) >> 22;
    float3 normal = max(float3(packed_nor) / 511.0, float3(-1.0));
    out.fogginess = 0.0;

    // rotate the normal
    float3 nrm_vf23 = p.cam_no_persp[0].xyz * normal.x + p.cam_no_persp[1].xyz * normal.y +
                      p.cam_no_persp[2].xyz * normal.z;

    // the ETIE math
    // nrm.z -= 1                     subw.z vf23, vf23, vf00
    nrm_vf23.z -= 1.0;
    // dot = nrm.xyz * pt.xyz         mul.xyz vf13, vf17, vf23 / esum / mfp
    float nrm_dot = dot(vf17.xyz, nrm_vf23);
    // rfl = pt.xyz * nrm.z           mulz.xyz vf14, vf17, vf23
    float3 rfl_vf14 = vf17.xyz * nrm_vf23.z;
    // Q_envmap = vf02.w / norm(rfl)  esadd / mfp / rsqrt
    float Q_envmap = -0.5 / length(rfl_vf14);
    // nrm.xy *= dot.x                mulx.xy vf23, vf23, vf13
    nrm_vf23.xy *= nrm_dot;
    // nrm.xy += rfl.xy               add.xy vf23, vf23, vf14
    nrm_vf23.xy += rfl_vf14.xy;
    // nrm.z = 1.0                    addw.z vf23, vf00, vf00
    nrm_vf23.z = 1.0;
    // nrm.xy *= Q_envmap             mul.xy vf23, vf23, Q
    nrm_vf23.xy *= Q_envmap;
    // nrm.xy += vf03.w               addw.xy vf23, vf23, vf03
    nrm_vf23.xy += 0.5;
    out.tex_coord = nrm_vf23;
    float4 proto_tint = float4(v.rgba) / 255.0;
    out.fragment_color = proto_tint * p.envmap_tod_tint;
  } else {
    float fog1 = p.camera[3].w + p.camera[0].w * position_in.x +
                 p.camera[1].w * position_in.y + p.camera[2].w * position_in.z;
    out.fogginess = 255.0 - clamp(fog1 + p.hvdf_offset.w, p.fog_min, p.fog_max);
    if (d.decal == 1) {
      out.fragment_color = float4(1.0, 1.0, 1.0, 1.0);
    } else {
      float4 color = tod.read(uint(v.color_index));
      color *= 2.0;
      color.a *= 2.0;
      out.fragment_color = color;
    }
    out.tex_coord = float3(float2(v.st), 0.0);
  }
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
  out.pos = p.view_clip_from_game_clip * to_metal_clip_depth(transformed);

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
