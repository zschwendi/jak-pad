#pragma once

/*!
 * @file metal_pso_cache.h
 * Pipeline-state and depth-stencil-state caches for the Metal backend.
 *
 * The GL renderers mutate blend / mask / depth state freely between draws; in
 * Metal that state is baked into pipeline state objects. These caches let ported
 * renderers request state per draw (GL-style) while PSOs are created once per
 * distinct combination and reused. Objective-C++ only.
 */

#include <unordered_map>

#include "common/common_types.h"

#import <Metal/Metal.h>

// Shader programs available in the embedded metallib. Every PSO is built from
// one of these vertex/fragment function pairs.
enum class MetalShaderId : u16 {
  SCAFFOLD = 0,         // colored/textured validation geometry
  PRESENT = 1,          // PCRTC-style final blit
  SAMPLE = 2,           // textured quad with an API-supplied sampler state
  DIRECT_BASIC = 3,     // DirectRenderer, untextured (direct_basic.{vert,frag})
  DIRECT_TEXTURED = 4,  // DirectRenderer, textured (direct_basic_textured.{vert,frag})
  SPRITE3 = 5,          // Sprite3 2D/HUD/3D sprites (sprite3_3d.{vert,frag})
  TFRAG3 = 6,           // tfrag terrain + TIE base draw (tfrag3.{vert,frag})
  ETIE_BASE = 7,        // base draw of an envmapped TIE (etie_base.{vert,frag})
  SHRUB = 8,            // shrub vegetation (shrub.{vert,frag})
  OCEAN_TEXTURE = 9,          // generated ocean texture (ocean_texture.{vert,frag})
  OCEAN_TEXTURE_MIPMAP = 10,   // its mip chain (ocean_texture_mipmap.{vert,frag})
  OCEAN_COMMON = 11,           // ocean near/mid mesh (ocean_common.{vert,frag})
  MERC2 = 12,           // Merc2 skinned foreground models (merc2.{vert,frag})
  EMERC = 13,           // Merc2 envmap pass (emerc.{vert,frag})
  EYE = 14,             // EyeRenderer's eye composition (eye.{vert,frag})
  GENERIC = 15,         // Generic2 VU1 fallback path (generic.{vert,frag})
  SHADOW = 16,          // shadow volumes (shadow.{vert,frag})
  ETIE = 17,            // TIE envmap second draw (etie.{vert,frag})
  SPRITE_DISTORT = 18,  // sprite distorter heat shimmer (sprite_distort.{vert,frag})
  SPRITE_GLOW_DEPTH_COPY = 19,
  SPRITE_GLOW_PROBE = 20,
  SPRITE_GLOW_DOWNSAMPLE = 21,
  SPRITE_GLOW_DRAW = 22,
  OCEAN_ENVMAP_HAZE = 23,
  OCEAN_ENVMAP_RADIAL = 24,
  SHADOW2 = 25,         // Jak II Shadow2 projection and final color passes
  COUNT,
};

// Everything that actually varies between draws and must be baked into a Metal
// PSO: the shader program, the render-target formats / sample count, the blend
// configuration, and the color write mask. Depth test/write state is NOT here -
// Metal keeps that in separate (cheap) MTLDepthStencilState objects, cached below.
struct MetalPsoKey {
  MetalShaderId shader = MetalShaderId::SCAFFOLD;
  u16 sample_count = 1;
  u32 color_format = MTLPixelFormatBGRA8Unorm;  // MTLPixelFormat
  u32 depth_format = MTLPixelFormatInvalid;     // MTLPixelFormat; Invalid = no depth attachment
  bool blend_enable = false;
  u8 blend_op_rgb = MTLBlendOperationAdd;         // MTLBlendOperation
  u8 blend_op_alpha = MTLBlendOperationAdd;       // MTLBlendOperation
  u8 blend_src_rgb = MTLBlendFactorOne;           // MTLBlendFactor
  u8 blend_dst_rgb = MTLBlendFactorZero;          // MTLBlendFactor
  u8 blend_src_alpha = MTLBlendFactorOne;         // MTLBlendFactor
  u8 blend_dst_alpha = MTLBlendFactorZero;        // MTLBlendFactor
  u8 color_write_mask = MTLColorWriteMaskAll;     // MTLColorWriteMask

  bool operator==(const MetalPsoKey& o) const {
    return shader == o.shader && sample_count == o.sample_count &&
           color_format == o.color_format && depth_format == o.depth_format &&
           blend_enable == o.blend_enable && blend_op_rgb == o.blend_op_rgb &&
           blend_op_alpha == o.blend_op_alpha && blend_src_rgb == o.blend_src_rgb &&
           blend_dst_rgb == o.blend_dst_rgb && blend_src_alpha == o.blend_src_alpha &&
           blend_dst_alpha == o.blend_dst_alpha && color_write_mask == o.color_write_mask;
  }
};

// Depth/stencil state that GL renderers set per draw (glDepthFunc /
// glDepthMask, plus glStencilFunc / glStencilOp for the shadow volumes).
struct MetalDepthStencilKey {
  bool depth_test = false;  // false = compare Always (glDisable(GL_DEPTH_TEST))
  u8 compare = MTLCompareFunctionAlways;  // MTLCompareFunction
  bool depth_write = false;
  // stencil (glEnable(GL_STENCIL_TEST) + glStencilFunc/glStencilOp)
  bool stencil_test = false;
  u8 stencil_compare = MTLCompareFunctionAlways;        // MTLCompareFunction
  u8 stencil_depth_pass_op = MTLStencilOperationKeep;   // MTLStencilOperation
  u8 stencil_read_mask = 0xff;
  u8 stencil_write_mask = 0xff;

  bool operator==(const MetalDepthStencilKey& o) const {
    return depth_test == o.depth_test && compare == o.compare &&
           depth_write == o.depth_write && stencil_test == o.stencil_test &&
           stencil_compare == o.stencil_compare &&
           stencil_depth_pass_op == o.stencil_depth_pass_op &&
           stencil_read_mask == o.stencil_read_mask &&
           stencil_write_mask == o.stencil_write_mask;
  }
};

class MetalPsoCache {
 public:
  // library must contain the functions named in the MetalShaderId table.
  bool init(id<MTLDevice> device, id<MTLLibrary> library);

  // Returns the PSO for this state combination, creating it on first use.
  // Returns nil (and logs) if pipeline creation fails.
  id<MTLRenderPipelineState> get_pipeline(const MetalPsoKey& key);
  id<MTLDepthStencilState> get_depth_stencil(const MetalDepthStencilKey& key);

  // stats for tests / debugging
  size_t pipeline_count() const { return m_pipelines.size(); }
  size_t depth_stencil_count() const { return m_depth_states.size(); }
  u64 pipeline_misses() const { return m_pipeline_misses; }
  u64 pipeline_hits() const { return m_pipeline_hits; }

 private:
  struct PsoKeyHash {
    size_t operator()(const MetalPsoKey& k) const;
  };
  struct DepthKeyHash {
    size_t operator()(const MetalDepthStencilKey& k) const;
  };

  id<MTLDevice> m_device;
  id<MTLFunction> m_vertex_fns[(int)MetalShaderId::COUNT];
  id<MTLFunction> m_fragment_fns[(int)MetalShaderId::COUNT];
  std::unordered_map<MetalPsoKey, id<MTLRenderPipelineState>, PsoKeyHash> m_pipelines;
  std::unordered_map<MetalDepthStencilKey, id<MTLDepthStencilState>, DepthKeyHash> m_depth_states;
  u64 m_pipeline_misses = 0;
  u64 m_pipeline_hits = 0;
};
