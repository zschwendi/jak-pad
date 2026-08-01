// Generic2 shader for the Metal backend.
//
// MSL port of game/graphics/opengl_renderer/shaders/generic.{vert,frag}, line
// for line, including the VU1 comments the GL source carries. Deliberate
// differences:
//  - Metal clip-space z is [0,1] while GL's is [-1,1], so the GL
//    `z / 8388608 - 1` becomes `z / 16777216` (the same rule the sprite, merc
//    and background ports use).
//  - The GL SCISSOR_ADJUST / HEIGHT_SCALE / SCISSOR_HEIGHT text substitutions
//    arrive as uniforms.
//  - Vertices are read from a device buffer by vertex_id, so the CPU-side
//    Generic2 vertex struct is used with its exact layout.

#include <metal_stdlib>
using namespace metal;

// Must match MetalGeneric2::Vertex (32 bytes), which is Generic2::Vertex.
struct GenericVertexIn {
  packed_float3 xyz;
  uchar4 rgba;
  packed_float2 st;
  uchar tex_unit;
  uchar flags;
  uchar adc;
  uchar pad0;
  uint pad1;
};

// Must match GenericVsParams in metal_generic2.mm.
struct GenericVsParams {
  float4 scale;
  float4 hvdf_offset;
  float4 full_matrix[4];
  float4 fog_constants;  // xyz = pfog0, fog_min, fog_max
  float mat_23;
  float mat_32;
  float mat_33;
  uint use_full_matrix;
  uint warp_sample_mode;
  float height_scale;
  float scissor_adjust;
  float warp_off;  // 1 - SCISSOR_HEIGHT / 512
};

// Must match GenericFsParams in metal_generic2.mm.
struct GenericFsParams {
  float4 fog_color;
  float alpha_reject;
  float color_mult;
  int gfx_hack_no_tex;
  uint warp_sample_mode;
};

struct GenericVSOut {
  float4 pos [[position]];
  float2 tex_coord;
  float4 fragment_color;
  float fog;
  uint2 tex_info [[flat]];
};

vertex GenericVSOut generic_vs(uint vid [[vertex_id]],
                               const device GenericVertexIn* verts [[buffer(0)]],
                               constant GenericVsParams& p [[buffer(1)]]) {
  const device GenericVertexIn& v = verts[vid];
  float3 position_in = float3(v.xyz);
  GenericVSOut out;

  // mulaw.xyzw ACC, vf11, vf00   matrix multiply W
  // maddax.xyzw ACC, vf08, vf16  matrix multiply X
  float4 transformed;
  if (p.use_full_matrix != 0) {
    transformed = -p.full_matrix[3];
    transformed -= p.full_matrix[0] * position_in.x;
    transformed -= p.full_matrix[1] * position_in.y;
    transformed -= p.full_matrix[2] * position_in.z;
  } else {
    transformed.xyz = position_in * p.scale.xyz;
    transformed.z += p.mat_32;
    transformed.w = p.mat_23 * position_in.z + p.mat_33;
    transformed *= -1.0;
  }

  // div Q, vf01.x, vf12.w        perspective divide
  float Q = p.fog_constants.x / transformed.w;

  out.fog = 255.0 - clamp(-transformed.w + p.hvdf_offset.w, p.fog_constants.y, p.fog_constants.z);

  // itof12.xyz vf18, vf22        texture int to float
  out.tex_coord = float2(v.st) / 4096.0;
  if (p.warp_sample_mode == 1) {
    out.tex_coord = float2(out.tex_coord.x, (1.0 - out.tex_coord.y - p.warp_off) * p.scissor_adjust);
  }

  // mul.xyz vf12, vf12, Q        perspective divide
  transformed.xyz *= Q;

  // add.xyzw vf12, vf12, vf04    apply hvdf
  transformed.xyz += p.hvdf_offset.xyz;

  // correct xy offset
  transformed.xy -= 2048.0;

  // correct z scale (Metal's [0,1] clip depth)
  transformed.z /= 16777216.0;

  // correct xy scale
  transformed.x /= 256.0;
  transformed.y /= -128.0;

  // hack
  transformed.xyz *= transformed.w;

  out.pos = transformed;
  // scissoring area adjust
  out.pos.y *= p.scissor_adjust * p.height_scale;

  float4 rgba_in = float4(v.rgba) / 255.0;
  out.fragment_color = float4(rgba_in.rgb, rgba_in.a * 2.0);
  out.tex_info = uint2(v.tex_unit, v.flags);
  return out;
}

fragment float4 generic_fs(GenericVSOut in [[stage_in]],
                           constant GenericFsParams& p [[buffer(0)]],
                           texture2d<float> tex_T0 [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  // 0x1 is tcc, 0x2 is decal, 0x4 is fog
  float4 color;
  if (p.warp_sample_mode == 1 || p.gfx_hack_no_tex == 0) {
    float4 T0 = tex_T0.sample(samp, in.tex_coord);
    if ((in.tex_info.y & 1u) == 0) {
      if ((in.tex_info.y & 2u) == 0) {
        // modulate + no tcc
        color.rgb = in.fragment_color.rgb * T0.rgb;
        color.a = in.fragment_color.a;
      } else {
        // decal + no tcc
        color.rgb = T0.rgb * 0.5;
        color.a = in.fragment_color.a;
      }
    } else {
      if ((in.tex_info.y & 2u) == 0) {
        // modulate + tcc
        color = in.fragment_color * T0;
      } else {
        // decal + tcc
        color.rgb = T0.rgb * 0.5;
        color.a = T0.a;
      }
    }
    color *= 2.0;
  } else {
    if ((in.tex_info.y & 1u) == 0) {
      if ((in.tex_info.y & 2u) == 0) {
        color.rgb = in.fragment_color.rgb;
        color.a = in.fragment_color.a * 2.0;
      } else {
        color.rgb = float3(1.0);
        color.a = in.fragment_color.a * 2.0;
      }
    } else {
      if ((in.tex_info.y & 2u) == 0) {
        color = in.fragment_color;
      } else {
        color.rgb = float3(0.5);
        color.a = 1.0;
      }
    }
  }
  color.rgb *= p.color_mult;

  if (color.a < p.alpha_reject) {
    discard_fragment();
  }
  if ((in.tex_info.y & 4u) != 0) {
    color.xyz = mix(color.xyz, p.fog_color.rgb, clamp(p.fog_color.a * in.fog, 0.0, 1.0));
  }
  return color;
}
