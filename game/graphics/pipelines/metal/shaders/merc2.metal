// Merc2 / emerc shaders for the Metal backend.
//
// MSL port of game/graphics/opengl_renderer/shaders/merc2.{vert,frag} and
// emerc.{vert,frag}, line for line. The deliberate differences are the target
// conventions:
//  - Metal clip-space z is [0,1] while GL's is [-1,1]. The GL shaders produce
//    `z / 8388608 - 1`; the same depth in Metal's range is `z / 16777216`.
//  - The GL HEIGHT_SCALE / SCISSOR_ADJUST compile-time substitutions arrive as
//    uniform values instead.
//  - The GL `layout (std140) uniform ub_bones { MercMatrixData bones[128]; }`
//    becomes a constant-address-space pointer into the shared bone buffer; the
//    per-draw `glBindBufferRange` is a per-draw `setVertexBufferOffset:`. The
//    std140 padding (mat3 columns padded to float4, plus the trailing vec4) is
//    reproduced exactly so the CPU-side layout is unchanged.

#include <metal_stdlib>
using namespace metal;

// Must match tfrag3::MercVertex (64 bytes). packed_ types keep the GL offsets.
struct MercVertexIn {
  packed_float3 pos;      // 0
  float pad0;             // 12
  packed_float3 normal;   // 16
  float pad1;             // 28
  packed_float3 weights;  // 32
  float pad2;             // 44
  packed_float2 st;       // 48
  uchar4 rgba;            // 56
  uchar4 mats;            // 60: three bone indices + pad
};

// std140 layout of the GL `MercMatrixData` (mat4 X; mat3 R; vec4 pad).
struct MercMatrixData {
  float4 X[4];
  float4 R[3];
  float4 pad;
};

struct MercVsParams {
  float4x4 perspective;
  float4 hvdf_offset;
  float4 fog_constants;
  float4 light_dir0_fade;
  float4 light_dir1_fade_en;
  float4 light_dir2;
  float4 light_col0;
  float4 light_col1;
  float4 light_col2;
  float4 light_ambient;
  float4 fade;  // emerc only
  float height_scale;    // 1.0 for Jak 1
  float scissor_adjust;  // 512/448 for Jak 1
  float pad[2];
};

struct MercFsParams {
  float4 fog_color;
  float4 light_dir0_fade;
  float4 light_dir1_fade_en;
  int ignore_alpha;
  int decal_enable;
  int gfx_hack_no_tex;
  int pad;
};

struct MercVSOut {
  float4 pos [[position]];
  float4 vtx_color;
  float2 vtx_st;
  float fog;
};

static float4 merc_bone_transform(constant MercMatrixData& b, float4 p) {
  return b.X[0] * p.x + b.X[1] * p.y + b.X[2] * p.z + b.X[3] * p.w;
}

static float3 merc_bone_rotate(constant MercMatrixData& b, float3 n) {
  return b.R[0].xyz * n.x + b.R[1].xyz * n.y + b.R[2].xyz * n.z;
}

// The skinning the two vertex programs share.
static void merc_skin(MercVertexIn v,
                      constant MercMatrixData* bones,
                      thread float4& vtx_pos,
                      thread float3& rotated_nrm) {
  float4 p = float4(float3(v.pos), 1.0);
  float3 normal = float3(v.normal);
  float3 weights = float3(v.weights);
  uint3 mats = uint3(v.mats.x, v.mats.y, v.mats.z);

  vtx_pos = -merc_bone_transform(bones[mats.x], p) * weights.x;
  rotated_nrm = merc_bone_rotate(bones[mats.x], normal) * weights.x;

  // game may send garbage bones if the weight is 0, don't let NaNs sneak in.
  if (weights.y > 0) {
    vtx_pos += -merc_bone_transform(bones[mats.y], p) * weights.y;
    rotated_nrm += merc_bone_rotate(bones[mats.y], normal) * weights.y;
  }
  if (weights.z > 0) {
    vtx_pos += -merc_bone_transform(bones[mats.z], p) * weights.z;
    rotated_nrm += merc_bone_rotate(bones[mats.z], normal) * weights.z;
  }
}

vertex MercVSOut merc2_vs(uint vid [[vertex_id]],
                          const device MercVertexIn* verts [[buffer(0)]],
                          constant MercVsParams& p [[buffer(1)]],
                          constant MercMatrixData* bones [[buffer(2)]]) {
  MercVertexIn v = verts[vid];
  float4 vtx_pos;
  float3 rotated_nrm;
  merc_skin(v, bones, vtx_pos, rotated_nrm);

  float4 transformed = p.perspective * vtx_pos;

  rotated_nrm = normalize(rotated_nrm);
  float3 light_intensity = p.light_dir0_fade.xyz * rotated_nrm.x +
                           p.light_dir1_fade_en.xyz * rotated_nrm.y +
                           p.light_dir2.xyz * rotated_nrm.z;
  light_intensity = max(light_intensity, float3(0.0));

  float4 light_color = p.light_ambient + light_intensity.x * p.light_col0 +
                       light_intensity.y * p.light_col1 + light_intensity.z * p.light_col2;

  float Q = p.fog_constants.x / transformed[3];

  MercVSOut out;
  out.fog = 255.0 - clamp(-transformed.w + p.hvdf_offset.w, p.fog_constants.y, p.fog_constants.z);

  transformed.xyz *= Q;
  transformed.xyz += p.hvdf_offset.xyz;
  transformed.xy -= 2048.0;
  // GL: z / 8388608 - 1 into [-1, 1]; the same depth in Metal's [0, 1] range
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = transformed;

  out.vtx_color = (float4(v.rgba) / 255.0) * light_color;
  out.vtx_st = float2(v.st);
  return out;
}

fragment float4 merc2_fs(MercVSOut in [[stage_in]],
                         constant MercFsParams& p [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
  float4 color;
  if (p.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.vtx_st);
    // all merc is tcc=rgba and modulate
    if (p.decal_enable == 0) {
      color = in.vtx_color * T0 * 2.0;
    } else {
      color = T0;
    }
    color.a *= 2.0;
  } else {
    color.rgb = in.vtx_color.rgb;
    if (p.decal_enable == 0) {
      color.a = in.vtx_color.a * 2.0;
    } else {
      color.a = 1.0;
    }
  }

  if (p.light_dir1_fade_en.w > 0) {
    color.a = p.light_dir0_fade.w;
  } else if (p.light_dir1_fade_en.w < 0) {
    color.a *= p.light_dir0_fade.w;
  }

  if (p.ignore_alpha == 0 && color.w < 0.128) {
    discard_fragment();
  }

  color.xyz = mix(color.xyz, p.fog_color.rgb, clamp(p.fog_color.a * in.fog, 0.0, 1.0));
  return color;
}

vertex MercVSOut emerc_vs(uint vid [[vertex_id]],
                          const device MercVertexIn* verts [[buffer(0)]],
                          constant MercVsParams& p [[buffer(1)]],
                          constant MercMatrixData* bones [[buffer(2)]]) {
  MercVertexIn v = verts[vid];
  float4 vtx_pos;
  float3 rotated_nrm;
  merc_skin(v, bones, vtx_pos, rotated_nrm);

  float4 transformed = p.perspective * vtx_pos;

  rotated_nrm = normalize(rotated_nrm);

  float Q = p.fog_constants.x / transformed[3];

  MercVSOut out;
  out.fog = 255.0 - clamp(-transformed.w + p.hvdf_offset.w, p.fog_constants.y, p.fog_constants.z);

  // emerc
  float2 st_mod = float2(v.st);
  {
    float4 vf10 = float4(rotated_nrm, 1.0);
    float4 vf08 = transformed;
    // unperspect (1/P(0, 0), 1/P(1, 1), 0.5, 1/P(2, 3))
    float4 vf23 = float4(1.0 / p.perspective[0][0], 1.0 / p.perspective[1][1], 0.5,
                         1.0 / p.perspective[2][3]);
    // mul.xyzw vf09, vf08, vf23 ;; do unperspect
    float4 vf09 = vf08 * vf23;
    // subw.z vf10, vf10, vf00 ;; subtract 1 from z
    vf10.z -= 1.0;
    // addw.z vf09, vf00, vf09 ;; xyww the unperspected thing
    vf09.z = vf09.w;
    // mul.xyz vf15, vf09, vf10
    float3 vf15 = vf09.xyz * vf10.xyz;
    // adday.xyzw vf15, vf15 / maddz.x vf15, vf21, vf15
    float vf15_x = vf15.x + vf15.y + vf15.z;
    // div Q, vf15.x, vf10.z
    float qq = vf15_x / vf10.z;
    // mulaw.xyzw ACC, vf09, vf00
    float4 ACC = vf09;
    // mul.xyzw vf09, vf08, vf23
    vf09 = vf08 * vf23;
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qq;
    // eleng.xyz P, vf10
    float P = length(vf10.xyz);
    // div Q, vf23.z, vf10.w
    float qqq = vf23.z / P;
    // addaz.xyzw vf00, vf23
    ACC = float4(vf23.z, vf23.z, vf23.z, vf23.z + 1.0);
    // madd.xyzw vf10, vf10, Q
    vf10 = ACC + vf10 * qqq;

    // this is required to make jak 1's envmapping look right; otherwise it
    // behaves like the envmap texture is mirrored, because vtx_pos is flipped.
    st_mod.x = 1.0 - vf10.x;
    st_mod.y = 1.0 - vf10.y;
  }

  transformed.xyz *= Q;
  transformed.xyz += p.hvdf_offset.xyz;
  transformed.xy -= 2048.0;
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = transformed;

  out.vtx_color = float4(p.fade.xyz, 1.0);
  out.vtx_st = st_mod;
  return out;
}

fragment float4 emerc_fs(MercVSOut in [[stage_in]],
                         constant MercFsParams& p [[buffer(0)]],
                         texture2d<float> tex [[texture(0)]],
                         sampler samp [[sampler(0)]]) {
  float4 color;
  if (p.gfx_hack_no_tex == 0) {
    float4 T0 = tex.sample(samp, in.vtx_st);
    color.a = T0.a;
    color.rgb = T0.rgb * in.vtx_color.rgb;
    color *= 2.0;
  } else {
    color.rgb = in.vtx_color.rgb;
    color.a = 1.0;
  }
  return color;
}
