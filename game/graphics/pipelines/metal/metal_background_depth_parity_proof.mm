#include <algorithm>
#include <array>
#include <cmath>
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
constexpr float kDepth24Max = 16777215.f;

enum class BackgroundPath { Tfrag3, EtieBase };

struct ProofPipelines {
  id<MTLRenderPipelineState> tfrag_d32 = nil;
  id<MTLRenderPipelineState> etie_d32 = nil;
  id<MTLRenderPipelineState> tfrag_d24 = nil;
  id<MTLRenderPipelineState> etie_d24 = nil;
};

struct ProofPass {
  BackgroundPath path;
  std::array<float, 4> color;
};

struct ProofReadback {
  bool ok = false;
  float depth = 0.f;
  u32 depth_bits = 0;
  std::array<u8, 4> bgra = {};
};

struct ProofContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  id<MTLDepthStencilState> depth_state = nil;
  id<MTLTexture> time_of_day = nil;
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

u32 depth24_key(float depth) {
  const float clamped = std::fmax(0.f, std::fmin(1.f, depth));
  return static_cast<u32>(std::lround(clamped * kDepth24Max));
}

bool is_red(const std::array<u8, 4>& bgra) {
  return bgra[0] <= 2 && bgra[1] <= 2 && bgra[2] >= 253 && bgra[3] >= 253;
}

bool is_blue(const std::array<u8, 4>& bgra) {
  return bgra[0] >= 253 && bgra[1] <= 2 && bgra[2] <= 2 && bgra[3] >= 253;
}

id<MTLRenderPipelineState> make_pipeline(id<MTLDevice> device,
                                         id<MTLLibrary> library,
                                         NSString* vertex_name,
                                         NSString* fragment_name) {
  id<MTLFunction> vertex = [library newFunctionWithName:vertex_name];
  id<MTLFunction> fragment = [library newFunctionWithName:fragment_name];
  if (!vertex || !fragment) {
    std::printf("[FAIL] missing Metal proof function %s/%s\n", vertex_name.UTF8String,
                fragment_name.UTF8String);
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
    std::printf("[FAIL] Metal depth-parity PSO %s/%s: %s\n", vertex_name.UTF8String,
                fragment_name.UTF8String, error.localizedDescription.UTF8String);
    g_failures++;
  }
  return pipeline;
}

MetalGoalBackgroundCameraData make_camera(float camera_z) {
  MetalGoalBackgroundCameraData camera{};
  camera.rot[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
  camera.rot[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
  camera.rot[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
  camera.rot[3] = math::Vector4f(0.f, 0.f, -camera_z, 1.f);
  camera.trans = math::Vector4f(0.f, 0.f, camera_z, 1.f);
  camera.camera[3].w() = 255.f;
  camera.hvdf_off = math::Vector4f(2048.f, 2048.f, 16384.f, 0.f);
  camera.fog = math::Vector4f(-4096.f, 0.f, 255.f, 0.f);
  camera.perspective[0].x() = 256.f;
  camera.perspective[1].y() = 128.f;
  camera.perspective[2].w() = 4096.f;
  camera.perspective[3].z() = -1000.f;
  return camera;
}

id<MTLRenderPipelineState> pipeline_for(const ProofContext& context,
                                        BackgroundPath path,
                                        bool quantize_depth) {
  if (path == BackgroundPath::Tfrag3) {
    return quantize_depth ? context.pipelines.tfrag_d24 : context.pipelines.tfrag_d32;
  }
  return quantize_depth ? context.pipelines.etie_d24 : context.pipelines.etie_d32;
}

ProofReadback render(const ProofContext& context,
                     float camera_z,
                     bool quantize_depth,
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
  id<MTLBuffer> depth_readback =
      [context.device newBufferWithLength:256 options:MTLResourceStorageModeShared];
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
  [encoder setDepthStencilState:context.depth_state];

  const auto camera = make_camera(camera_z);
  MetalBackgroundVsParams tfrag_params{};
  MetalEtieVsParams etie_params{};
  metal_fill_background_vs_params(camera, GameVersion::Jak1, &tfrag_params);
  metal_fill_etie_vs_params(camera, GameVersion::Jak1, &etie_params);
  const MetalBackgroundDrawParams draw_params{};

  for (const auto& proof_pass : passes) {
    [encoder setRenderPipelineState:pipeline_for(context, proof_pass.path, quantize_depth)];
    [encoder setVertexBytes:context.vertices.data()
                     length:sizeof(context.vertices)
                    atIndex:0];
    if (proof_pass.path == BackgroundPath::Tfrag3) {
      [encoder setVertexBytes:&tfrag_params length:sizeof(tfrag_params) atIndex:1];
    } else {
      [encoder setVertexBytes:&etie_params length:sizeof(etie_params) atIndex:1];
    }
    [encoder setVertexBytes:&draw_params length:sizeof(draw_params) atIndex:2];
    [encoder setVertexTexture:context.time_of_day atIndex:1];
    [encoder setFragmentBytes:proof_pass.color.data()
                       length:sizeof(proof_pass.color)
                      atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }
  [encoder endEncoding];

  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:depth_target
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(kTargetSize / 2, kTargetSize / 2, 0)
             sourceSize:MTLSizeMake(1, 1, 1)
               toBuffer:depth_readback
      destinationOffset:0
 destinationBytesPerRow:256
destinationBytesPerImage:256
                options:MTLBlitOptionDepthFromDepthStencil];
#if TARGET_OS_OSX
  [blit synchronizeResource:color_target];
#endif
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) {
    if (commands.error) {
      std::printf("[FAIL] Metal depth-parity command buffer: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    return out;
  }

  std::memcpy(&out.depth, depth_readback.contents, sizeof(out.depth));
  out.depth_bits = float_bits(out.depth);
  std::array<u8, kTargetSize * kTargetSize * 4> pixels{};
  [color_target getBytes:pixels.data()
              bytesPerRow:kTargetSize * 4
               fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
              mipmapLevel:0];
  const size_t center = ((kTargetSize / 2) * kTargetSize + kTargetSize / 2) * 4;
  std::copy_n(pixels.data() + center, 4, out.bgra.begin());
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
      std::printf("[FAIL] Metal depth-parity library: %s\n",
                  library_error.localizedDescription.UTF8String);
    }
    return false;
  }

  context->pipelines.tfrag_d32 = make_pipeline(
      context->device, context->library, @"tfrag3_vs", @"background_depth32_proof_fs");
  context->pipelines.etie_d32 = make_pipeline(
      context->device, context->library, @"etie_base_vs", @"background_depth32_proof_fs");
  context->pipelines.tfrag_d24 = make_pipeline(
      context->device, context->library, @"tfrag3_vs", @"background_depth24_proof_fs");
  context->pipelines.etie_d24 = make_pipeline(
      context->device, context->library, @"etie_base_vs", @"background_depth24_proof_fs");

  DrawMode mode{};
  mode.as_int() = 0;
  mode.set_depth_write_enable(true);
  mode.set_zt(true);
  mode.set_depth_test(GsTest::ZTest::GEQUAL);
  MetalFrameContext frame_context{};
  frame_context.color_format = MTLPixelFormatBGRA8Unorm;
  frame_context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  MetalBackgroundState background_state{};
  const auto tfrag_settings = metal_background_settings_from_draw_mode(
      mode, MetalShaderId::TFRAG3, frame_context, &background_state);
  const auto etie_settings = metal_background_settings_from_draw_mode(
      mode, MetalShaderId::ETIE_BASE, frame_context, &background_state);
  check(tfrag_settings.depth == etie_settings.depth,
        "production TFRAG3 and ETIE_BASE resolve to the same depth state");
  check(tfrag_settings.depth.depth_test && tfrag_settings.depth.depth_write &&
            tfrag_settings.depth.compare == MTLCompareFunctionGreaterEqual,
        "production opaque background depth state is GEQUAL with writes enabled");

  auto* depth_descriptor = [[MTLDepthStencilDescriptor alloc] init];
  depth_descriptor.depthCompareFunction =
      static_cast<MTLCompareFunction>(tfrag_settings.depth.compare);
  depth_descriptor.depthWriteEnabled = tfrag_settings.depth.depth_write;
  context->depth_state = [context->device newDepthStencilStateWithDescriptor:depth_descriptor];

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
    vertex.color_index = 0;
  }

  return context->depth_state && context->time_of_day && context->pipelines.tfrag_d32 &&
         context->pipelines.etie_d32 && context->pipelines.tfrag_d24 &&
         context->pipelines.etie_d24;
}

}  // namespace

extern "C" bool goalpad_run_metal_background_depth_parity_proof() {
  @autoreleasepool {
    std::printf("--- Metal TFRAG3/ETIE D32 versus fragment-D24 depth parity ---\n");
    ProofContext context;
    check(initialize(&context), "initialized the asset-free Metal depth-parity proof");
    if (g_failures) {
      return false;
    }

    constexpr std::array<float, 5> kNearbyCameraZ = {-1.f, -0.5f, 0.f, 0.5f, 1.f};
    const std::array<float, 4> red = {1.f, 0.f, 0.f, 1.f};
    const std::array<float, 4> blue = {0.f, 0.f, 1.f, 1.f};
    const std::vector<ProofPass> tfrag_only = {{BackgroundPath::Tfrag3, red}};
    const std::vector<ProofPass> etie_only = {{BackgroundPath::EtieBase, blue}};
    const std::vector<ProofPass> competing = {
        {BackgroundPath::Tfrag3, red}, {BackgroundPath::EtieBase, blue}};

    int collapse_count = 0;
    std::vector<float> collapsed_cameras;
    for (float camera_z : kNearbyCameraZ) {
      const auto tfrag_d32 = render(context, camera_z, false, tfrag_only);
      const auto etie_d32 = render(context, camera_z, false, etie_only);
      const auto competing_d32 = render(context, camera_z, false, competing);
      const auto tfrag_d24 = render(context, camera_z, true, tfrag_only);
      const auto etie_d24 = render(context, camera_z, true, etie_only);
      const auto competing_d24 = render(context, camera_z, true, competing);
      if (!tfrag_d32.ok || !etie_d32.ok || !competing_d32.ok || !tfrag_d24.ok ||
          !etie_d24.ok || !competing_d24.ok) {
        check(false, "completed every depth-parity GPU readback");
        continue;
      }

      const u32 tfrag_key = depth24_key(tfrag_d32.depth);
      const u32 etie_key = depth24_key(etie_d32.depth);
      const bool collapsed = tfrag_d32.depth_bits != etie_d32.depth_bits &&
                             tfrag_key == etie_key &&
                             tfrag_d24.depth_bits == etie_d24.depth_bits;
      std::printf(
          "  camera z=% .2f: TFRAG3=% .9g (0x%08x) ETIE=% .9g (0x%08x) "
          "D24=%u/%u quantized=% .9g/% .9g raw-color=%u,%u,%u "
          "d24-color=%u,%u,%u%s\n",
          camera_z, tfrag_d32.depth, tfrag_d32.depth_bits, etie_d32.depth,
          etie_d32.depth_bits, tfrag_key, etie_key, tfrag_d24.depth, etie_d24.depth,
          competing_d32.bgra[2], competing_d32.bgra[1], competing_d32.bgra[0],
          competing_d24.bgra[2], competing_d24.bgra[1], competing_d24.bgra[0],
          collapsed ? " COLLAPSE" : "");
      if (collapsed) {
        collapse_count++;
        collapsed_cameras.push_back(camera_z);
        check(etie_d32.depth < tfrag_d32.depth && is_red(competing_d32.bgra),
              "native D32 GEQUAL rejects the slightly farther ETIE base");
        check(is_blue(competing_d24.bgra),
              "fragment-stage D24 quantization makes the same ETIE base pass GEQUAL");
      }
    }

    check(collapse_count >= 2,
          "multiple nearby cameras retain distinct D32 depths that collapse to one D24 value");
    if (collapsed_cameras.size() >= 2) {
      const float camera_a = collapsed_cameras.front();
      const float camera_b = collapsed_cameras.back();
      const auto a_first = render(context, camera_a, false, competing);
      const auto b = render(context, camera_b, false, competing);
      const auto a_second = render(context, camera_a, false, competing);
      check(a_first.ok && b.ok && a_second.ok,
            "A/B/A production-D32 camera sequence completed");
      check(a_first.depth_bits == a_second.depth_bits && a_first.bgra == a_second.bgra,
            "production-D32 camera A is byte-identical after nearby camera B");
    }

    if (g_failures) {
      std::printf("METAL BACKGROUND DEPTH PARITY PROOF FAILED: %d check(s) failed\n",
                  g_failures);
      return false;
    }
    std::printf("METAL BACKGROUND DEPTH PARITY PROOF PASSED\n");
    return true;
  }
}
