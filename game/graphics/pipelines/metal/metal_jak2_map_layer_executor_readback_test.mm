#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "game/graphics/pipelines/metal/metal_jak2_map_layer_executor.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

using namespace metal_renderer;

namespace {

constexpr u32 kW = kJak2MapIndexedLayerWidth;
constexpr u32 kH = kJak2MapIndexedLayerHeight;
constexpr u32 kTargetW = kW * 2;
constexpr u32 kTargetH = kH * 2;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

// Indices 1..4 have CSM1 bits 3/4 clear, so they are unaffected by any CLUT index swizzle.
constexpr std::array<u8, 4> kCornerIndex = {1, 2, 3, 4};  // TL, TR, BL, BR

u32 corner_color(std::size_t layer, std::size_t frame, std::size_t corner) {
  // Raw 0xAABBGGRR, distinct per layer/frame/corner and asymmetric across corners.
  return 0x80000000u | (static_cast<u32>(frame * 0x40 + layer * 0x10 + corner + 1) << 16) |
         (static_cast<u32>(0x20 + layer * 0x31 + corner * 0x07) << 8) |
         static_cast<u32>(0x11 * (corner + 1) + frame * 0x60);
}

Jak2MapTextureUploadTypedPlan make_plan(std::size_t frame) {
  Jak2MapTextureUploadTypedPlan plan;
  plan.present = true;
  plan.mixed = true;
  plan.layer_count = 4;
  for (std::size_t i = 0; i < 4; i++) {
    auto& layer = plan.layers[i];
    layer.width = kW;
    layer.height = kH;
    layer.indices.assign(kJak2MapIndexedLayerIndexBytes, 0);
    layer.indices[0] = kCornerIndex[0];
    layer.indices[kW - 1] = kCornerIndex[1];
    layer.indices[(kH - 1) * kW] = kCornerIndex[2];
    layer.indices[(kH - 1) * kW + kW - 1] = kCornerIndex[3];
    layer.clut[0] = 0xff000000u | static_cast<u32>(i + frame * 4);
    for (std::size_t c = 0; c < 4; c++) {
      layer.clut[kCornerIndex[c]] = corner_color(i, frame, c);
    }
    plan.ops[i].kind = Jak2MapUploadOpKind::AnimationLayer;
    plan.ops[i].index = static_cast<u8>(i);
  }
  plan.ordinary_count = 1;
  plan.ops[4].kind = Jak2MapUploadOpKind::OrdinaryUpload;
  plan.ops[4].index = 0;
  plan.op_count = 5;
  return plan;
}

const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 position [[position]]; float2 uv; };
vertex VOut map_vs(uint vid [[vertex_id]]) {
  const float2 p[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
  const float2 t[4] = {float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 0)};
  VOut o; o.position = float4(p[vid], 0, 1); o.uv = t[vid]; return o;
}
fragment float4 map_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]],
                       sampler s [[sampler(0)]]) {
  return tex.sample(s, in.uv);
}
)";

struct Frame {
  id<MTLTexture> target = nil;
  id<MTLBuffer> readback = nil;
  std::array<id<MTLTexture>, 4> sampled = {};
};

// Publishes each layer, resolves TBP 8 through the pool and registry, and encodes one draw into
// quadrant `layer` of the target. Nothing is committed here.
void encode_frame(id<MTLCommandBuffer> commands,
                  id<MTLRenderPipelineState> pipeline,
                  id<MTLSamplerState> sampler,
                  Jak2MapLayerExecutor& executor,
                  TexturePool& pool,
                  Frame& frame) {
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = frame.target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
  check(encoder != nil, "render encoder");
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentSamplerState:sampler atIndex:0];
  for (std::size_t i = 0; i < 4; i++) {
    const u64 before = executor.layer_publications(i);
    check(executor.publish_layer(i), "layer publishes in order");
    check(executor.layer_publications(i) == before + 1, "layer publishes exactly once");
    const u64 handle = pool.lookup(kJak2MapLayerVramSlot).value_or(0);
    check(handle != 0 && handle == executor.layer_handle(i), "TBP 8 resolves the layer's handle");
    id<MTLTexture> texture = metal_texture_lookup(handle);
    check(texture != nil, "registry resolves the layer texture");
    frame.sampled[i] = texture;
    MTLViewport viewport = {static_cast<double>((i % 2) * kW), static_cast<double>((i / 2) * kH),
                            static_cast<double>(kW), static_cast<double>(kH), 0.0, 1.0};
    [encoder setViewport:viewport];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
  }
  [encoder endEncoding];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:frame.target
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(kTargetW, kTargetH, 1)
                      toBuffer:frame.readback
             destinationOffset:0
        destinationBytesPerRow:kTargetW * 4
      destinationBytesPerImage:kTargetW * kTargetH * 4];
  [blit endEncoding];
}

void check_frame(const Frame& frame, std::size_t frame_index) {
  const u32* pixels = static_cast<const u32*>(frame.readback.contents);
  for (std::size_t i = 0; i < 4; i++) {
    const u32 x0 = (i % 2) * kW;
    const u32 y0 = (i / 2) * kH;
    const std::array<u32, 4> offsets = {y0 * kTargetW + x0, y0 * kTargetW + x0 + kW - 1,
                                        (y0 + kH - 1) * kTargetW + x0,
                                        (y0 + kH - 1) * kTargetW + x0 + kW - 1};
    for (std::size_t c = 0; c < 4; c++) {
      const u32 expected = corner_color(i, frame_index, c);
      if (pixels[offsets[c]] != expected) {
        std::fprintf(stderr, "frame %zu layer %zu corner %zu: got %08x expected %08x\n",
                     frame_index, i, c, pixels[offsets[c]], expected);
        check(false, "drawn corner matches its own layer palette");
      }
    }
  }
}

void check_rejected_publish(Jak2MapLayerExecutor& executor,
                            TexturePool& pool,
                            std::size_t index,
                            const char* message) {
  std::array<u64, 4> before = {};
  for (std::size_t i = 0; i < 4; i++) {
    before[i] = executor.layer_publications(i);
  }
  const auto slot_before = pool.lookup(kJak2MapLayerVramSlot);
  const u64 rejected_before = executor.stats().rejected_publishes;
  check(!executor.publish_layer(index), message);
  check(executor.stats().rejected_publishes == rejected_before + 1, "rejection is counted");
  for (std::size_t i = 0; i < 4; i++) {
    check(executor.layer_publications(i) == before[i], "rejected publish adds no publication");
  }
  check(pool.lookup(kJak2MapLayerVramSlot) == slot_before, "rejected publish leaves TBP 8");
}

void check_rejected_prepare(Jak2MapLayerExecutor& executor,
                            TexturePool& pool,
                            const Jak2MapTextureUploadTypedPlan& plan,
                            Jak2MapLayerExecutorError error,
                            const char* message) {
  const std::size_t live = metal_texture_live_count();
  const auto slot_before = pool.lookup(kJak2MapLayerVramSlot);
  const u64 prepared_before = executor.stats().prepared_frames;
  check(!executor.prepare(plan) && executor.last_error() == error, message);
  check(executor.stats().prepared_frames == prepared_before, "rejected prepare is not counted");
  check(metal_texture_live_count() == live && pool.lookup(kJak2MapLayerVramSlot) == slot_before,
        "rejected prepare leaves registry and pool unchanged");
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "Metal command queue is available");

    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kShader]
                                                  options:nil
                                                    error:&error];
    check(library != nil, "fixture shader compiles");
    auto* pipeline_desc = [MTLRenderPipelineDescriptor new];
    pipeline_desc.vertexFunction = [library newFunctionWithName:@"map_vs"];
    pipeline_desc.fragmentFunction = [library newFunctionWithName:@"map_fs"];
    pipeline_desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipeline_desc
                                                                                 error:&error];
    check(pipeline != nil, "fixture pipeline builds");
    auto* sampler_desc = [MTLSamplerDescriptor new];
    sampler_desc.minFilter = MTLSamplerMinMagFilterNearest;
    sampler_desc.magFilter = MTLSamplerMinMagFilterNearest;
    sampler_desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_desc];

    auto make_frame = [&]() {
      Frame frame;
      auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                      width:kTargetW
                                                                     height:kTargetH
                                                                  mipmapped:NO];
      desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      desc.storageMode = MTLStorageModePrivate;
      frame.target = [device newTextureWithDescriptor:desc];
      frame.readback = [device newBufferWithLength:kTargetW * kTargetH * 4
                                           options:MTLResourceStorageModeShared];
      check(frame.target != nil && frame.readback != nil, "frame resources");
      return frame;
    };

    const std::size_t initial_live = metal_texture_live_count();
    TexturePool pool(GameVersion::Jak2);
    {
      Jak2MapLayerExecutor executor(device, queue, &pool);
      const auto slot_initial = pool.lookup(kJak2MapLayerVramSlot);
      for (std::size_t i = 0; i < 4; i++) {
        check(executor.layer_handle(i) == 0, "no identity exists before first publication");
      }
      check_rejected_publish(executor, pool, 0, "publish before prepare is rejected");

      auto bad = make_plan(0);
      bad.present = false;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::PlanNotPresent,
                             "absent plan rejected");
      bad = make_plan(0);
      bad.mixed = false;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::PlanNotMixed,
                             "ordinary-only plan rejected");
      bad = make_plan(0);
      bad.layer_count = 3;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::LayerCount,
                             "three-layer plan rejected");
      bad = make_plan(0);
      bad.layers[2].width = kW - 1;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::LayerShape,
                             "bad layer width rejected");
      bad = make_plan(0);
      bad.layers[3].indices.pop_back();
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::LayerShape,
                             "short index span rejected");
      bad = make_plan(0);
      std::swap(bad.ops[1], bad.ops[2]);
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::OpOrder,
                             "reordered layer ops rejected");
      bad = make_plan(0);
      bad.ops[2].index = 1;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::OpOrder,
                             "duplicate layer op rejected");
      bad = make_plan(0);
      bad.ops[4].index = 1;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::OpOrder,
                             "out-of-range ordinary op rejected");

      // First prepare decodes on the CPU only.
      check(executor.prepare(make_plan(0)), "frame 0 prepares");
      check(metal_texture_live_count() == initial_live &&
                pool.lookup(kJak2MapLayerVramSlot) == slot_initial,
            "first prepare mutates neither registry nor pool");
      for (std::size_t i = 0; i < 4; i++) {
        check(executor.layer_handle(i) == 0 && executor.layer_publications(i) == 0,
              "prepare publishes nothing");
        check(executor.prepared_rgba(i)[0] == corner_color(i, 0, 0), "prepared RGBA is owned");
      }
      check_rejected_publish(executor, pool, 1, "out-of-order first publish rejected");
      check_rejected_publish(executor, pool, 4, "out-of-range publish rejected");

      // Frame 0: four draws in one uncommitted command buffer.
      id<MTLCommandBuffer> first = [queue commandBuffer];
      check(first.retainedReferences, "default command buffer retains its resources");
      Frame frame0 = make_frame();
      encode_frame(first, pipeline, sampler, executor, pool, frame0);
      check(executor.frame_complete(), "frame 0 published all four layers");
      check_rejected_publish(executor, pool, 3, "duplicate publish rejected");
      std::set<u64> handles;
      std::set<void*> objects;
      for (std::size_t i = 0; i < 4; i++) {
        handles.insert(executor.layer_handle(i));
        objects.insert((__bridge void*)frame0.sampled[i]);
      }
      check(handles.size() == 4 && objects.size() == 4, "four distinct handles and MTLTextures");
      check(metal_texture_live_count() == initial_live + 4, "exactly four registry handles");

      // Frame 1 before frame 0 commits: same handles, new MTLTexture objects.
      bad = make_plan(1);
      bad.layer_count = 5;
      check_rejected_prepare(executor, pool, bad, Jak2MapLayerExecutorError::LayerCount,
                             "invalid second prepare keeps frame 0 state");
      check(executor.prepare(make_plan(1)), "frame 1 prepares");
      check_rejected_prepare(executor, pool, make_plan(1),
                             Jak2MapLayerExecutorError::FrameIncomplete,
                             "prepare over an incomplete frame rejected");
      id<MTLCommandBuffer> second = [queue commandBuffer];
      Frame frame1 = make_frame();
      encode_frame(second, pipeline, sampler, executor, pool, frame1);
      for (std::size_t i = 0; i < 4; i++) {
        check(executor.layer_publications(i) == 2, "each layer published once per frame");
        check(frame1.sampled[i] != frame0.sampled[i], "frame 1 samples a new MTLTexture");
        objects.insert((__bridge void*)frame1.sampled[i]);
      }
      check(objects.size() == 8, "no MTLTexture shared across layers or frames");
      check(metal_texture_live_count() == initial_live + 4, "handles stay stable across frames");
      // Identity observations are finished; drop the fixture's strong references so only the
      // encoded command buffers retain the frame-0 and frame-1 textures.
      frame0.sampled.fill(nil);
      frame1.sampled.fill(nil);

      [first commit];
      [second commit];
      [second waitUntilCompleted];
      [first waitUntilCompleted];
      check(first.status == MTLCommandBufferStatusCompleted &&
                second.status == MTLCommandBufferStatusCompleted,
            "both frames complete");
      check_frame(frame0, 0);
      check_frame(frame1, 1);

      executor.detach_pool();
      check(!executor.prepare(make_plan(2)) &&
                executor.last_error() == Jak2MapLayerExecutorError::Detached,
            "detached executor rejects prepare");
    }
    check(metal_texture_live_count() == initial_live, "executor releases its four handles");
  }
  std::printf("PASS: jak2 map layer executor four-identity readback\n");
  return 0;
}
