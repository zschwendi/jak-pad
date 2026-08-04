#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 32;
constexpr int kPixelCount = kTargetSize * kTargetSize;
constexpr int kDepthReadbackRowBytes = 256;

enum class BackgroundPath { EtieBase, EtieShine };

struct ProofPipelines {
  id<MTLRenderPipelineState> base = nil;
  id<MTLRenderPipelineState> shine = nil;
};

struct ProofPass {
  BackgroundPath path;
  id<MTLTexture> texture = nil;
  bool write_depth = false;
  float depth_bias = 0.f;
  bool left_half_scissor = false;
};

struct ProofReadback {
  bool ok = false;
  std::array<u8, kPixelCount * 4> pixels = {};
  std::array<float, kPixelCount> depths = {};
};

struct CoverageReadback {
  int base = 0;
  int shine = 0;
  int overlap = 0;
  int identical_depth = 0;
  int shine_wins = 0;
  int base_only_holes = 0;
  int unexpected_color = 0;
  int preserved_base_depth = 0;
};

struct ProofContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  id<MTLDepthStencilState> write_depth = nil;
  id<MTLDepthStencilState> read_depth = nil;
  id<MTLTexture> time_of_day = nil;
  id<MTLTexture> red = nil;
  id<MTLTexture> green = nil;
  id<MTLTexture> blue = nil;
  id<MTLSamplerState> sampler = nil;
  ProofPipelines pipelines;
  std::array<tfrag3::PreloadedVertex, 4> vertices;
};

int g_failures = 0;

void check(bool condition, const char* what) {
  std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", what);
  if (!condition) {
    g_failures++;
  }
}

u32 float_bits(float value) {
  u32 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

std::array<u8, 4> pixel_at(const ProofReadback& readback, int pixel) {
  std::array<u8, 4> out;
  std::copy_n(readback.pixels.data() + pixel * 4, 4, out.begin());
  return out;
}

bool is_red(const std::array<u8, 4>& bgra) {
  return bgra[0] <= 2 && bgra[1] <= 2 && bgra[2] >= 253 && bgra[3] >= 253;
}

bool is_green(const std::array<u8, 4>& bgra) {
  return bgra[0] <= 2 && bgra[1] >= 253 && bgra[2] <= 2 && bgra[3] >= 253;
}

bool is_blue(const std::array<u8, 4>& bgra) {
  return bgra[0] >= 253 && bgra[1] <= 2 && bgra[2] <= 2 && bgra[3] >= 253;
}

u64 pixel_hash(const ProofReadback& readback) {
  u64 hash = 1469598103934665603ull;
  for (u8 byte : readback.pixels) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

CoverageReadback classify_coverage(const ProofReadback& base,
                                   const ProofReadback& shine,
                                   const ProofReadback& composed) {
  CoverageReadback out;
  for (int pixel = 0; pixel < kPixelCount; pixel++) {
    const bool base_covered = is_red(pixel_at(base, pixel));
    const bool shine_covered = is_blue(pixel_at(shine, pixel));
    out.base += base_covered;
    out.shine += shine_covered;
    if (!base_covered || !shine_covered) {
      continue;
    }

    out.overlap++;
    out.identical_depth +=
        float_bits(base.depths[pixel]) == float_bits(shine.depths[pixel]);
    const auto winner = pixel_at(composed, pixel);
    const bool blue_won = is_blue(winner);
    const bool red_remained = is_red(winner);
    out.shine_wins += blue_won;
    out.base_only_holes += red_remained;
    out.unexpected_color += !blue_won && !red_remained;
    out.preserved_base_depth +=
        float_bits(composed.depths[pixel]) == float_bits(base.depths[pixel]);
  }
  return out;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                         id<MTLLibrary> library,
                                         NSString* vertex_name) {
  id<MTLFunction> vertex = [library newFunctionWithName:vertex_name];
  id<MTLFunction> fragment = [library newFunctionWithName:@"tfrag3_fs"];
  if (!vertex || !fragment) {
    std::printf("[FAIL] missing Metal invariant-proof function %s/tfrag3_fs\n",
                vertex_name.UTF8String);
    g_failures++;
    return nil;
  }

  auto* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  if (!pipeline) {
    std::printf("[FAIL] Metal invariant-proof PSO %s/tfrag3_fs: %s\n",
                vertex_name.UTF8String, error.localizedDescription.UTF8String);
    g_failures++;
  }
  return pipeline;
}

id<MTLDepthStencilState> make_depth_state(id<MTLDevice> device, bool write) {
  auto* descriptor = [[MTLDepthStencilDescriptor alloc] init];
  descriptor.depthCompareFunction = MTLCompareFunctionGreaterEqual;
  descriptor.depthWriteEnabled = write;
  return [device newDepthStencilStateWithDescriptor:descriptor];
}

id<MTLTexture> make_color_texture(id<MTLDevice> device, const std::array<u8, 4>& rgba) {
  auto* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  [texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
              mipmapLevel:0
                withBytes:rgba.data()
              bytesPerRow:rgba.size()];
  return texture;
}

MetalGoalBackgroundCameraData make_camera(float camera_offset) {
  const float x = camera_offset * 0.375f;
  const float y = camera_offset * -0.25f;
  const float z = camera_offset;
  MetalGoalBackgroundCameraData camera{};
  camera.rot[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
  camera.rot[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
  camera.rot[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
  camera.rot[3] = math::Vector4f(-x, -y, -z, 1.f);
  camera.trans = math::Vector4f(x, y, z, 1.f);
  camera.camera[3].w() = 255.f;
  camera.hvdf_off = math::Vector4f(2048.f, 2048.f, 16384.f, 0.f);
  camera.fog = math::Vector4f(-4096.f, 0.f, 255.f, 0.f);
  camera.perspective[0].x() = 256.f;
  camera.perspective[1].y() = 128.f;
  camera.perspective[2].w() = 4096.f;
  camera.perspective[3].z() = -1000.f;
  return camera;
}

ProofReadback render(const ProofContext& context,
                     float camera_offset,
                     const std::vector<ProofPass>& passes) {
  ProofReadback out;
  auto* color_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:kTargetSize
                                                        height:kTargetSize
                                                     mipmapped:NO];
  color_descriptor.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
  color_descriptor.storageMode = MTLStorageModeManaged;
#else
  color_descriptor.storageMode = MTLStorageModeShared;
#endif
  id<MTLTexture> color_target = [context.device newTextureWithDescriptor:color_descriptor];

  auto* depth_descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                   width:kTargetSize
                                  height:kTargetSize
                               mipmapped:NO];
  depth_descriptor.usage = MTLTextureUsageRenderTarget;
  depth_descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth_target = [context.device newTextureWithDescriptor:depth_descriptor];
  id<MTLBuffer> depth_readback = [context.device
      newBufferWithLength:kDepthReadbackRowBytes * kTargetSize
                  options:MTLResourceStorageModeShared];
  if (!color_target || !depth_target || !depth_readback) {
    return out;
  }

  id<MTLCommandBuffer> commands = [context.queue commandBuffer];
  auto* pass_descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  pass_descriptor.colorAttachments[0].texture = color_target;
  pass_descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass_descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass_descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
  pass_descriptor.depthAttachment.texture = depth_target;
  pass_descriptor.depthAttachment.loadAction = MTLLoadActionClear;
  pass_descriptor.depthAttachment.storeAction = MTLStoreActionStore;
  pass_descriptor.depthAttachment.clearDepth = 0.f;
  pass_descriptor.stencilAttachment.texture = depth_target;
  pass_descriptor.stencilAttachment.loadAction = MTLLoadActionClear;
  pass_descriptor.stencilAttachment.storeAction = MTLStoreActionDontCare;
  pass_descriptor.stencilAttachment.clearStencil = 0;

  id<MTLRenderCommandEncoder> encoder =
      [commands renderCommandEncoderWithDescriptor:pass_descriptor];
  if (!commands || !encoder) {
    return out;
  }
  [encoder setCullMode:MTLCullModeNone];

  MetalEtieVsParams etie_params{};
  metal_fill_etie_vs_params(make_camera(camera_offset), GameVersion::Jak1, &etie_params);
  const MetalBackgroundDrawParams draw_params{};
  MetalBackgroundFsParams fs_params{};
  fs_params.alpha_max = 10.f;

  for (const auto& proof_pass : passes) {
    [encoder setRenderPipelineState:proof_pass.path == BackgroundPath::EtieBase
                                        ? context.pipelines.base
                                        : context.pipelines.shine];
    [encoder setDepthStencilState:proof_pass.write_depth ? context.write_depth
                                                            : context.read_depth];
    [encoder setDepthBias:proof_pass.depth_bias slopeScale:0.f clamp:0.f];
    const MTLScissorRect scissor =
        proof_pass.left_half_scissor ? MTLScissorRect{0, 0, kTargetSize / 2, kTargetSize}
                                      : MTLScissorRect{0, 0, kTargetSize, kTargetSize};
    [encoder setScissorRect:scissor];
    [encoder setVertexBytes:context.vertices.data()
                     length:sizeof(context.vertices)
                    atIndex:0];
    [encoder setVertexBytes:&etie_params length:sizeof(etie_params) atIndex:1];
    [encoder setVertexBytes:&draw_params length:sizeof(draw_params) atIndex:2];
    [encoder setVertexTexture:context.time_of_day atIndex:1];
    [encoder setFragmentBytes:&fs_params length:sizeof(fs_params) atIndex:0];
    [encoder setFragmentTexture:proof_pass.texture atIndex:0];
    [encoder setFragmentSamplerState:context.sampler atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }
  [encoder endEncoding];

  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:depth_target
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(kTargetSize, kTargetSize, 1)
               toBuffer:depth_readback
      destinationOffset:0
 destinationBytesPerRow:kDepthReadbackRowBytes
destinationBytesPerImage:kDepthReadbackRowBytes * kTargetSize
                options:MTLBlitOptionDepthFromDepthStencil];
#if TARGET_OS_OSX
  [blit synchronizeResource:color_target];
#endif
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    if (commands.error) {
      std::printf("[FAIL] Metal invariant-proof command buffer: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    return out;
  }

  for (int y = 0; y < kTargetSize; y++) {
    std::memcpy(out.depths.data() + y * kTargetSize,
                static_cast<const u8*>(depth_readback.contents) + y * kDepthReadbackRowBytes,
                kTargetSize * sizeof(float));
  }
  [color_target getBytes:out.pixels.data()
              bytesPerRow:kTargetSize * 4
               fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
              mipmapLevel:0];
  out.ok = true;
  return out;
}

bool initialize(ProofContext* context) {
  context->device = MTLCreateSystemDefaultDevice();
  if (!context->device) {
    return false;
  }
  context->queue = [context->device newCommandQueue];
  dispatch_data_t library_data = dispatch_data_create(
      g_goalpad_metallib, g_goalpad_metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  context->library = [context->device newLibraryWithData:library_data error:&library_error];
  if (!context->queue || !context->library) {
    if (library_error) {
      std::printf("[FAIL] Metal invariant-proof library: %s\n",
                  library_error.localizedDescription.UTF8String);
    }
    return false;
  }

  context->pipelines.base = make_pipeline(context->device, context->library, @"etie_base_vs");
  context->pipelines.shine = make_pipeline(context->device, context->library, @"etie_vs");
  context->write_depth = make_depth_state(context->device, true);
  context->read_depth = make_depth_state(context->device, false);

  DrawMode base_mode{};
  base_mode.as_int() = 0;
  base_mode.set_depth_write_enable(true);
  base_mode.set_zt(true);
  base_mode.set_depth_test(GsTest::ZTest::GEQUAL);
  DrawMode shine_mode = base_mode;
  shine_mode.set_depth_write_enable(false);
  MetalFrameContext frame_context{};
  frame_context.color_format = MTLPixelFormatBGRA8Unorm;
  frame_context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  MetalBackgroundState background_state{};
  const auto base_settings = metal_background_settings_from_draw_mode(
      base_mode, MetalShaderId::ETIE_BASE, frame_context, &background_state);
  const auto shine_settings = metal_background_settings_from_draw_mode(
      shine_mode, MetalShaderId::ETIE, frame_context, &background_state);
  check(base_settings.depth.depth_test && base_settings.depth.depth_write &&
            base_settings.depth.compare == MTLCompareFunctionGreaterEqual,
        "production ETIE_BASE uses write-enabled D32 GEQUAL");
  check(shine_settings.depth.depth_test && !shine_settings.depth.depth_write &&
            shine_settings.depth.compare == MTLCompareFunctionGreaterEqual,
        "production ETIE shine uses read-only D32 GEQUAL");

  auto* tod_descriptor = [[MTLTextureDescriptor alloc] init];
  tod_descriptor.textureType = MTLTextureType1D;
  tod_descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
  tod_descriptor.width = 1;
  tod_descriptor.mipmapLevelCount = 1;
  tod_descriptor.usage = MTLTextureUsageShaderRead;
  tod_descriptor.storageMode = MTLStorageModeShared;
  context->time_of_day = [context->device newTextureWithDescriptor:tod_descriptor];
  const u32 white = 0xffffffffu;
  [context->time_of_day replaceRegion:MTLRegionMake1D(0, 1)
                          mipmapLevel:0
                            withBytes:&white
                          bytesPerRow:sizeof(white)];

  context->red = make_color_texture(context->device, {255, 0, 0, 255});
  context->green = make_color_texture(context->device, {0, 255, 0, 255});
  context->blue = make_color_texture(context->device, {0, 0, 255, 255});
  auto* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
  sampler_descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  context->sampler = [context->device newSamplerStateWithDescriptor:sampler_descriptor];

  constexpr float kHalfExtent = 6000.f;
  constexpr float kWorldZ = -10000.f;
  const float xs[4] = {-kHalfExtent, kHalfExtent, -kHalfExtent, kHalfExtent};
  const float ys[4] = {-kHalfExtent, -kHalfExtent, kHalfExtent, kHalfExtent};
  for (int i = 0; i < 4; i++) {
    auto& vertex = context->vertices[i];
    vertex.x = xs[i];
    vertex.y = ys[i];
    vertex.z = kWorldZ;
    vertex.r = vertex.g = vertex.b = vertex.a = 255;
    vertex.s = vertex.t = 0.5f;
    vertex.nor = 0;
    vertex.color_index = 0;
  }

  return context->queue && context->pipelines.base && context->pipelines.shine &&
         context->write_depth && context->read_depth && context->time_of_day && context->red &&
         context->green && context->blue && context->sampler;
}

}  // namespace

extern "C" bool goalpad_run_metal_background_depth_parity_proof() {
  @autoreleasepool {
    std::printf("--- Metal ETIE base/shine invariant D32 coverage proof ---\n");
    ProofContext context;
    check(initialize(&context), "initialized the asset-free Metal invariant proof");
    if (g_failures) {
      return false;
    }

    const std::vector<ProofPass> base_only = {
        {BackgroundPath::EtieBase, context.red, true}};
    // Isolated shine writes depth only so its rasterizer output can be read back. The composed
    // pass below uses the production read-only depth behavior.
    const std::vector<ProofPass> shine_only = {
        {BackgroundPath::EtieShine, context.blue, true}};
    const std::vector<ProofPass> composed = {
        {BackgroundPath::EtieBase, context.red, true},
        {BackgroundPath::EtieShine, context.blue, false}};

    int completed_cameras = 0;
    int empty_cameras = 0;
    int coverage_mismatch_cameras = 0;
    int depth_mismatch_pixels = 0;
    int base_only_holes = 0;
    int unexpected_colors = 0;
    int changed_depth_pixels = 0;
    int min_coverage = kPixelCount;
    int max_coverage = 0;
    constexpr int kSweepRadius = 32;
    constexpr float kSweepStep = 0.125f;
    for (int step = -kSweepRadius; step <= kSweepRadius; step++) {
      const float camera = step * kSweepStep;
      const auto base = render(context, camera, base_only);
      const auto shine = render(context, camera, shine_only);
      const auto result = render(context, camera, composed);
      if (!base.ok || !shine.ok || !result.ok) {
        continue;
      }
      completed_cameras++;
      const auto coverage = classify_coverage(base, shine, result);
      empty_cameras += coverage.base == 0 || coverage.shine == 0;
      coverage_mismatch_cameras +=
          coverage.base != coverage.shine || coverage.overlap != coverage.base;
      depth_mismatch_pixels += coverage.overlap - coverage.identical_depth;
      base_only_holes += coverage.base_only_holes;
      unexpected_colors += coverage.unexpected_color;
      changed_depth_pixels += coverage.overlap - coverage.preserved_base_depth;
      min_coverage = std::min(min_coverage, coverage.base);
      max_coverage = std::max(max_coverage, coverage.base);
      if (step == -kSweepRadius || step == 0 || step == kSweepRadius ||
          coverage.base_only_holes || coverage.identical_depth != coverage.overlap) {
        std::printf(
            "  camera=% .3f base/shine/overlap=%d/%d/%d identical-depth=%d "
            "shine/base-hole/other=%d/%d/%d hashes=%016llx/%016llx/%016llx\n",
            camera, coverage.base, coverage.shine, coverage.overlap,
            coverage.identical_depth, coverage.shine_wins, coverage.base_only_holes,
            coverage.unexpected_color, (unsigned long long)pixel_hash(base),
            (unsigned long long)pixel_hash(shine), (unsigned long long)pixel_hash(result));
      }
    }

    std::printf(
        "  sweep summary: cameras=%d coverage=%d..%d empty=%d mask-mismatch=%d "
        "D32-mismatch-pixels=%d base-only-holes=%d unexpected-colors=%d "
        "changed-depth-pixels=%d\n",
        completed_cameras, min_coverage, max_coverage, empty_cameras,
        coverage_mismatch_cameras, depth_mismatch_pixels, base_only_holes,
        unexpected_colors, changed_depth_pixels);
    check(completed_cameras == kSweepRadius * 2 + 1,
          "dense ETIE base/shine camera sweep completed every readback");
    check(empty_cameras == 0, "every swept camera produces non-empty ETIE coverage");
    check(coverage_mismatch_cameras == 0,
          "ETIE_BASE and ETIE shine coverage masks are bit-identical across the sweep");
    check(depth_mismatch_pixels == 0,
          "ETIE_BASE and ETIE shine rasterizer D32 depth is bit-identical across the sweep");
    check(base_only_holes == 0 && unexpected_colors == 0,
          "base-write/shine-GEQUAL-read-only composition has zero base-only holes");
    check(changed_depth_pixels == 0,
          "read-only ETIE shine preserves the ETIE_BASE depth buffer exactly");

    constexpr float kCameraA = -2.5f;
    constexpr float kCameraB = 2.5f;
    const auto base_a_first = render(context, kCameraA, base_only);
    const auto shine_a_first = render(context, kCameraA, shine_only);
    const auto composed_a_first = render(context, kCameraA, composed);
    const auto composed_b = render(context, kCameraB, composed);
    const auto base_a_second = render(context, kCameraA, base_only);
    const auto shine_a_second = render(context, kCameraA, shine_only);
    const auto composed_a_second = render(context, kCameraA, composed);
    check(base_a_first.ok && shine_a_first.ok && composed_a_first.ok && composed_b.ok &&
              base_a_second.ok && shine_a_second.ok && composed_a_second.ok,
          "ETIE production A/B/A readbacks completed");
    check(base_a_first.pixels == base_a_second.pixels &&
              base_a_first.depths == base_a_second.depths,
          "ETIE_BASE camera A color and D32 depth are byte-identical after camera B");
    check(shine_a_first.pixels == shine_a_second.pixels &&
              shine_a_first.depths == shine_a_second.depths,
          "ETIE shine camera A color and D32 depth are byte-identical after camera B");
    check(composed_a_first.pixels == composed_a_second.pixels &&
              composed_a_first.depths == composed_a_second.depths,
          "ETIE base/shine composition A is byte-identical after camera B");

    // A one-unit raster depth bias on a half-screen write is the negative control: it places an
    // adjacent closer occluder over the base, so the unbiased read-only shine must leave visible
    // green holes there while still drawing blue on the unoccluded half.
    const std::vector<ProofPass> occluder_control = {
        {BackgroundPath::EtieBase, context.red, true},
        {BackgroundPath::EtieBase, context.green, true, 1.f, true},
        {BackgroundPath::EtieShine, context.blue, false}};
    const auto occluded = render(context, 0.f, occluder_control);
    int green_holes = 0;
    int blue_visible = 0;
    if (occluded.ok) {
      for (int pixel = 0; pixel < kPixelCount; pixel++) {
        green_holes += is_green(pixel_at(occluded, pixel));
        blue_visible += is_blue(pixel_at(occluded, pixel));
      }
    }
    std::printf("  adjacent-depth occluder: green holes=%d, unoccluded blue=%d\n", green_holes,
                blue_visible);
    check(occluded.ok, "adjacent-depth occluder readback completed");
    check(green_holes > 0 && blue_visible > 0,
          "hole detector rejects an adjacent closer half-screen occluder");

    if (g_failures) {
      std::printf("METAL BACKGROUND INVARIANT PROOF FAILED: %d check(s) failed\n", g_failures);
      return false;
    }
    std::printf("METAL BACKGROUND INVARIANT PROOF PASSED\n");
    return true;
  }
}
