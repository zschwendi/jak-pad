// Sprite3 shaders for the Metal backend.
//
// MSL port of game/graphics/opengl_renderer/shaders/sprite3_3d.{vert,frag},
// which is itself a transcription of the sprite VU1 program. The math is kept
// line for line; the deliberate differences are the target conventions:
//  - Metal clip-space z is [0,1] while GL's is [-1,1]. The GL shader produces
//    `z / 8388608 - 1`; the same depth in Metal's range is `z / 16777216`.
//  - The GL HEIGHT_SCALE / SCISSOR_ADJUST compile-time substitutions arrive as
//    uniform values instead.

#include <metal_stdlib>
using namespace metal;

// Must match MetalSpriteRenderer::SpriteVertex3D (64 bytes, same layout as the
// GL Sprite3::SpriteVertex3D). packed_ types keep the GL offsets exactly.
struct SpriteVertexIn {
  packed_float4 xyz_sx;         // 0:  position + x scale
  packed_float4 quat_sy;        // 16: quaternion + y scale
  packed_float4 rgba;           // 32: color
  packed_ushort2 flags_matrix;  // 48
  packed_ushort4 info;          // 52: [0] unused, [1] tcc, [2] vertex id, [3] mode
  uchar4 pad;                   // 60
};

struct SpriteVsParams {
  float4x4 camera;
  float4x4 hud_matrix;
  float4 hvdf_offset;
  float4 hud_hvdf_offset;
  float4 basis_x;
  float4 basis_y;
  float4 xy_array[8];
  float4 xyz_array[4];
  float4 st_array[4];
  float pfog0;
  float fog_min;
  float fog_max;
  float min_scale;
  float max_scale;
  float deg_to_rad;
  float inv_area;
  float height_scale;    // 1.0 for Jak 1
  float scissor_adjust;  // 512/448 for Jak 1
  float4x4 view_clip_from_game_clip;
};

struct SpriteFsParams {
  float alpha_min;
  float alpha_max;
};

struct SpriteVSOut {
  float4 pos [[position]];
  float4 fragment_color [[flat]];
  float3 tex_coord;
  uint2 tex_info [[flat]];
};

static float4 matrix_transform(float4x4 mtx, float3 pt) {
  return mtx[3] + mtx[0] * pt.x + mtx[1] * pt.y + mtx[2] * pt.z;
}

static float3x3 sprite_quat_to_rot(float3 quat) {
  float3x3 result;
  float qr = sqrt(abs(1.0 - (quat.x * quat.x + quat.y * quat.y + quat.z * quat.z)));
  result[0][0] = 1.0 - 2.0 * (quat.y * quat.y + quat.z * quat.z);
  result[1][0] = 2.0 * (quat.x * quat.y - quat.z * qr);
  result[2][0] = 2.0 * (quat.x * quat.z + quat.y * qr);
  result[0][1] = 2.0 * (quat.x * quat.y + quat.z * qr);
  result[1][1] = 1.0 - 2.0 * (quat.x * quat.x + quat.z * quat.z);
  result[2][1] = 2.0 * (quat.y * quat.z - quat.x * qr);
  result[0][2] = 2.0 * (quat.x * quat.z - quat.y * qr);
  result[1][2] = 2.0 * (quat.y * quat.z + quat.x * qr);
  result[2][2] = 1.0 - 2.0 * (quat.x * quat.x + quat.y * quat.y);
  return result;
}

static float4 sprite_transform2(float3 root,
                                float4 off,
                                float3x3 sprite_rot,
                                float sx,
                                float sy,
                                constant SpriteVsParams& p) {
  float3 pos = root;
  float3 offset = sprite_rot[0] * off.x * sx + sprite_rot[1] * off.y + sprite_rot[2] * off.z * sy;
  pos += offset;
  float4 transformed_pos = -matrix_transform(p.camera, pos);
  float Q = p.pfog0 / transformed_pos.w;
  transformed_pos.xyz *= Q;
  transformed_pos.xyz += p.hvdf_offset.xyz;
  return transformed_pos;
}

vertex SpriteVSOut sprite3_vs(uint vid [[vertex_id]],
                              const device SpriteVertexIn* verts [[buffer(0)]],
                              constant SpriteVsParams& p [[buffer(1)]],
                              constant float4* hud_hvdf_user [[buffer(2)]]) {
  SpriteVertexIn v = verts[vid];
  SpriteVSOut out;

  // STEP 1: unpack
  float4 xyz_sx = float4(v.xyz_sx);
  float4 quat_sy = float4(v.quat_sy);
  float3 position = xyz_sx.xyz;
  float sx = xyz_sx.w;
  float sy = quat_sy.w;
  out.fragment_color = float4(v.rgba);
  uint vert_id = v.info[2];
  uint rendermode = v.info[3];  // 1 = 2D, 2 = HUD, 3 = 3D
  float3 quat = quat_sy.xyz;
  uint matrix = v.flags_matrix[1];

  float4 transformed = float4(0.0);

  // STEP 2: perspective transform for distance
  float4 transformed_pos_vf02 =
      matrix_transform(rendermode == 2 ? p.hud_matrix : p.camera, position);
  float Q = p.pfog0 / transformed_pos_vf02.w;

  // STEP 3: fade out sprite
  float4 scales_vf01 = xyz_sx;
  scales_vf01.z = sy;
  scales_vf01.zw *= Q;
  scales_vf01.x = scales_vf01.z;
  scales_vf01.x *= scales_vf01.w;
  scales_vf01.x *= p.inv_area;
  out.fragment_color.w *= min(scales_vf01.x, 1.0);

  // STEP 4: vertex transformation
  if (rendermode == 3) {  // 3D sprites
    float3x3 rot = sprite_quat_to_rot(quat);
    transformed = sprite_transform2(position, p.xyz_array[vert_id], rot, sx, sy, p);
  } else if (rendermode == 1) {  // 2D sprites
    transformed_pos_vf02.xyz *= Q;
    float4 offset_pos_vf10 = transformed_pos_vf02 + p.hvdf_offset;
    offset_pos_vf10.w = max(offset_pos_vf10.w, p.fog_min);

    scales_vf01.z = clamp(scales_vf01.z, p.min_scale, p.max_scale);
    scales_vf01.w = clamp(scales_vf01.w, p.min_scale, p.max_scale);

    quat.z *= p.deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    float4 xy0_vf19 = p.xy_array[vert_id + (uint(v.flags_matrix[0]) & 15u)];
    float4 vf12_rotated = (p.basis_x * sp_cos) - (p.basis_y * sp_sin);
    float4 vf13_rotated_trans = (p.basis_x * sp_sin) + (p.basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;
  } else if (rendermode == 2) {  // HUD sprites
    transformed_pos_vf02.xyz *= Q;
    float4 offset_pos_vf10 =
        transformed_pos_vf02 + (matrix == 0 ? p.hud_hvdf_offset : hud_hvdf_user[matrix - 1]);

    // note: no max scale for hud
    scales_vf01.z = max(scales_vf01.z, p.min_scale);
    scales_vf01.w = max(scales_vf01.w, p.min_scale);

    quat.z *= p.deg_to_rad;
    float sp_sin = sin(quat.z);
    float sp_cos = cos(quat.z);

    float4 xy0_vf19 = p.xy_array[vert_id + (uint(v.flags_matrix[0]) & 15u)];
    float4 vf12_rotated = (p.basis_x * sp_cos) - (p.basis_y * sp_sin);
    float4 vf13_rotated_trans = (p.basis_x * sp_sin) + (p.basis_y * sp_cos);

    vf12_rotated *= scales_vf01.w;
    vf13_rotated_trans *= scales_vf01.z;

    transformed = offset_pos_vf10 + vf12_rotated * xy0_vf19.x + vf13_rotated_trans * xy0_vf19.y;
  }

  out.tex_coord = p.st_array[vert_id].xyz;

  // STEP 5: final adjustments
  transformed.xy -= 2048.0;
  // GL: z / 8388608 - 1 into [-1, 1]; the same depth in Metal's [0, 1] range
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.xyz *= transformed.w;
  transformed.y *= p.scissor_adjust * p.height_scale;
  out.pos = rendermode == 3 ? p.view_clip_from_game_clip * transformed : transformed;

  out.fragment_color *= 2.0;
  out.fragment_color.w *= 2.0;
  out.tex_info = uint2(v.info[0], v.info[1]);
  return out;
}

fragment float4 sprite3_fs(SpriteVSOut in [[stage_in]],
                           constant SpriteFsParams& params [[buffer(0)]],
                           texture2d<float> tex [[texture(0)]],
                           sampler samp [[sampler(0)]]) {
  float4 T0 = tex.sample(samp, in.tex_coord.xy);
  if (in.tex_info.y == 0) {
    T0.w = 1.0;
  }
  float4 color = in.fragment_color * T0;
  if (color.a < params.alpha_min || color.a > params.alpha_max) {
    discard_fragment();
  }
  return color;
}

// ---------------------------------------------------------------------------
// Sprite distorter (heat shimmer): MSL port of sprite_distort.{vert,frag}.
// The sprites resample a snapshot of the frame rendered so far through
// sine-table-warped texture coordinates.
// ---------------------------------------------------------------------------

// Must match MetalSpriteRenderer::SpriteDistortVertex (20 bytes, same layout
// as the GL Sprite3::SpriteDistortVertex).
struct SpriteDistortVertexIn {
  packed_float3 xyz;
  packed_float2 st;
};

struct SpriteDistortParams {
  float4 color;       // sine-table color / 255
  float height_scale; // 1.0 for Jak 1
  float fb_v_offset;  // (1 - SCISSOR_HEIGHT / 512) / 2, in Metal's top-down v
};

struct SpriteDistortVSOut {
  float4 pos [[position]];
  float4 fragment_color [[flat]];
  float2 tex_coord;
};

vertex SpriteDistortVSOut sprite_distort_vs(uint vid [[vertex_id]],
                                            const device SpriteDistortVertexIn* verts
                                            [[buffer(0)]],
                                            constant SpriteDistortParams& params [[buffer(1)]]) {
  SpriteDistortVertexIn v = verts[vid];
  SpriteDistortVSOut out;
  out.fragment_color = params.color;
  out.tex_coord = float2(v.st);
  float4 transformed = float4(float3(v.xyz), 1.0);
  transformed.xy -= 2048.0;
  // GL: z / 8388608 - 1 into [-1, 1]; the same depth in Metal's [0, 1] range
  transformed.z /= 16777216.0;
  transformed.x /= 256.0;
  transformed.y /= -128.0;
  transformed.y *= params.height_scale;
  out.pos = transformed;
  return out;
}

fragment float4 sprite_distort_fs(SpriteDistortVSOut in [[stage_in]],
                                  constant SpriteDistortParams& params [[buffer(0)]],
                                  texture2d<float> fb_tex [[texture(0)]],
                                  sampler fb_sampler [[sampler(0)]]) {
  float4 color = in.fragment_color * 2.0;
  // The GL shader samples at (x, (1 - y) - (1 - SCISSOR_HEIGHT/512)/2) with a
  // bottom-up texture; the Metal snapshot is top-down, so the same texel sits
  // at y + offset.
  float2 tc = float2(in.tex_coord.x, in.tex_coord.y + params.fb_v_offset);
  float4 framebuffer_sample = fb_tex.sample(fb_sampler, tc);
  // GL snapshots into an RGB texture, whose sampled alpha is always one.
  // Metal reads BGRA directly, so do not leak the scene target's alpha into
  // the source-alpha blend used by the distortion pass.
  framebuffer_sample.a = 1.0;
  return color * framebuffer_sample;
}
