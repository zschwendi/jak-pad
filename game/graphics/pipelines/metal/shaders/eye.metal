// Eye renderer shader for the Metal backend.
//
// MSL port of game/graphics/opengl_renderer/shaders/eye.{vert,frag}, line for
// line. Two deliberate target differences:
//  - Metal clip-space z is [0,1] while GL's is [-1,1]; the GL shader writes
//    z = 0, which is inside both ranges, so the value is kept.
//  - Metal render targets are top-down while GL framebuffers are bottom-up.
//    The eye texture is rendered and then sampled (by merc, through the texture
//    pool), so y is negated here, exactly like the generated ocean texture.

#include <metal_stdlib>
using namespace metal;

struct EyeVSOut {
  float4 pos [[position]];
  float2 st;
};

vertex EyeVSOut eye_vs(uint vid [[vertex_id]],
                       const device packed_float4* xyst_in [[buffer(0)]]) {
  float4 v = float4(xyst_in[vid]);
  EyeVSOut out;
  out.pos = float4((v.x - 768.0) / 256.0, -(v.y - 768.0) / 256.0, 0.0, 1.0);
  out.st = v.zw;
  return out;
}

fragment float4 eye_fs(EyeVSOut in [[stage_in]],
                       texture2d<float> tex_T0 [[texture(0)]],
                       sampler samp [[sampler(0)]]) {
  float4 color = tex_T0.sample(samp, in.st);
  color.w *= 2.0;
  return color;
}
