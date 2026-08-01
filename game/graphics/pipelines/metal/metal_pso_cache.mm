#include "metal_pso_cache.h"

#include "common/log/log.h"

namespace {

struct ShaderFunctionNames {
  const char* vertex;
  const char* fragment;
};

constexpr ShaderFunctionNames kShaderFunctions[(int)MetalShaderId::COUNT] = {
    {"scaffold_vs", "scaffold_fs"},            // SCAFFOLD
    {"present_vs", "present_fs"},              // PRESENT
    {"scaffold_vs", "sample_fs"},              // SAMPLE
    {"direct_basic_vs", "direct_basic_fs"},    // DIRECT_BASIC
    {"direct_textured_vs", "direct_textured_fs"},  // DIRECT_TEXTURED
};

size_t hash_combine(size_t seed, size_t v) {
  return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
}

}  // namespace

size_t MetalPsoCache::PsoKeyHash::operator()(const MetalPsoKey& k) const {
  size_t h = (size_t)k.shader;
  h = hash_combine(h, k.sample_count);
  h = hash_combine(h, k.color_format);
  h = hash_combine(h, k.depth_format);
  h = hash_combine(h, (size_t)k.blend_enable << 8 | k.color_write_mask);
  h = hash_combine(h, (size_t)k.blend_op_rgb << 24 | (size_t)k.blend_op_alpha << 16 |
                          (size_t)k.blend_src_rgb << 8 | k.blend_dst_rgb);
  h = hash_combine(h, (size_t)k.blend_src_alpha << 8 | k.blend_dst_alpha);
  return h;
}

size_t MetalPsoCache::DepthKeyHash::operator()(const MetalDepthStencilKey& k) const {
  return ((size_t)k.depth_test << 9) | ((size_t)k.depth_write << 8) | k.compare;
}

bool MetalPsoCache::init(id<MTLDevice> device, id<MTLLibrary> library) {
  m_device = device;
  for (int i = 0; i < (int)MetalShaderId::COUNT; i++) {
    m_vertex_fns[i] = [library newFunctionWithName:@(kShaderFunctions[i].vertex)];
    m_fragment_fns[i] = [library newFunctionWithName:@(kShaderFunctions[i].fragment)];
    if (!m_vertex_fns[i] || !m_fragment_fns[i]) {
      lg::error("Metal: shader functions {}/{} not found in metallib",
                kShaderFunctions[i].vertex, kShaderFunctions[i].fragment);
      return false;
    }
  }
  return true;
}

id<MTLRenderPipelineState> MetalPsoCache::get_pipeline(const MetalPsoKey& key) {
  auto it = m_pipelines.find(key);
  if (it != m_pipelines.end()) {
    m_pipeline_hits++;
    return it->second;
  }
  m_pipeline_misses++;

  auto* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = m_vertex_fns[(int)key.shader];
  desc.fragmentFunction = m_fragment_fns[(int)key.shader];
  desc.rasterSampleCount = key.sample_count;

  auto* color = desc.colorAttachments[0];
  color.pixelFormat = (MTLPixelFormat)key.color_format;
  color.writeMask = key.color_write_mask;
  color.blendingEnabled = key.blend_enable;
  if (key.blend_enable) {
    color.rgbBlendOperation = (MTLBlendOperation)key.blend_op_rgb;
    color.alphaBlendOperation = (MTLBlendOperation)key.blend_op_alpha;
    color.sourceRGBBlendFactor = (MTLBlendFactor)key.blend_src_rgb;
    color.destinationRGBBlendFactor = (MTLBlendFactor)key.blend_dst_rgb;
    color.sourceAlphaBlendFactor = (MTLBlendFactor)key.blend_src_alpha;
    color.destinationAlphaBlendFactor = (MTLBlendFactor)key.blend_dst_alpha;
  }
  if (key.depth_format != MTLPixelFormatInvalid) {
    desc.depthAttachmentPixelFormat = (MTLPixelFormat)key.depth_format;
    if (key.depth_format == MTLPixelFormatDepth32Float_Stencil8) {
      desc.stencilAttachmentPixelFormat = (MTLPixelFormat)key.depth_format;
    }
  }

  NSError* error = nil;
  id<MTLRenderPipelineState> pso = [m_device newRenderPipelineStateWithDescriptor:desc
                                                                            error:&error];
  if (!pso) {
    lg::error("Metal: PSO creation failed: {}", [[error localizedDescription] UTF8String]);
    return nil;
  }
  m_pipelines.emplace(key, pso);
  return pso;
}

id<MTLDepthStencilState> MetalPsoCache::get_depth_stencil(const MetalDepthStencilKey& key) {
  auto it = m_depth_states.find(key);
  if (it != m_depth_states.end()) {
    return it->second;
  }
  auto* desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction =
      key.depth_test ? (MTLCompareFunction)key.compare : MTLCompareFunctionAlways;
  desc.depthWriteEnabled = key.depth_write;
  id<MTLDepthStencilState> state = [m_device newDepthStencilStateWithDescriptor:desc];
  m_depth_states.emplace(key, state);
  return state;
}
