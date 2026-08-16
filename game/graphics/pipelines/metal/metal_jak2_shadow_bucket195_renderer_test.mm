#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow2_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_plan.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_shadow195_frame_capture_metal.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr u32 kBucketOffset = metal_renderer::kJak2ShadowBucket195PlanBucket * 16;
constexpr u32 kNextBucket = kBucketOffset + 16;
constexpr int kTargetSize = 64;

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  failures += !condition;
}

void put_float(std::array<u8, 208>* bytes, std::size_t offset, float value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

void put_float(std::array<u8, 64>* bytes, std::size_t offset, float value) {
  std::memcpy(bytes->data() + offset, &value, sizeof(value));
}

metal_renderer::Jak2ShadowBucket195Vertex vertex(float x, float y, float z) {
  metal_renderer::Jak2ShadowBucket195Vertex result;
  std::memcpy(result.bytes.data(), &x, sizeof(x));
  std::memcpy(result.bytes.data() + 4, &y, sizeof(y));
  std::memcpy(result.bytes.data() + 8, &z, sizeof(z));
  return result;
}

metal_renderer::Jak2ShadowBucket195Plan ready_plan() {
  using namespace metal_renderer;
  Jak2ShadowBucket195Plan plan;
  plan.disposition = Jak2ShadowBucket195PlanDisposition::Ready;
  plan.transfer_count = 1;
  plan.vertex_count = 8;
  plan.record_count = 2;
  put_float(&plan.constants, 64, 2048.f);
  put_float(&plan.constants, 68, 2048.f);
  put_float(&plan.constants, 72, 12582912.f);
  put_float(&plan.constants, 80, 1.f);
  put_float(&plan.perspective_matrix, 0, -4096.f);
  put_float(&plan.perspective_matrix, 20, -6656.f);
  put_float(&plan.perspective_matrix, 48, 2048.f);
  put_float(&plan.perspective_matrix, 52, 3328.f);
  put_float(&plan.perspective_matrix, 60, -1.f);
  plan.color = {64, 192, 128, 128};

  Jak2ShadowBucket195Batch batch;
  batch.has_top_upload = true;
  batch.has_bottom_upload = true;
  batch.top_vertices = {
      vertex(0.4375f, 0.46875f, 0.75f), vertex(0.4375f, 0.53125f, 0.75f),
      vertex(0.5625f, 0.46875f, 0.75f), vertex(0.5625f, 0.53125f, 0.75f)};
  batch.bottom_vertices = {
      vertex(0.5f, 0.46875f, 0.75f), vertex(0.5f, 0.53125f, 0.75f),
      vertex(0.5625f, 0.46875f, 0.75f), vertex(0.5625f, 0.53125f, 0.75f)};
  Jak2ShadowBucket195Command caps;
  caps.kind = Jak2ShadowBucket195CommandKind::Caps;
  caps.records = {{{{0, 1, 2, 1}}}, {{{2, 1, 3, 1}}}};
  batch.commands.push_back(std::move(caps));
  plan.batches.push_back(std::move(batch));
  return plan;
}

metal_renderer::Jak2ShadowBucket195Plan wall_plan() {
  using namespace metal_renderer;
  auto plan = ready_plan();
  plan.record_count = 1;
  auto& batch = plan.batches[0];
  batch.top_vertices = {
      vertex(0.46875f, 0.484375f, 0.75f), vertex(0.53125f, 0.484375f, 0.75f)};
  batch.bottom_vertices = {
      vertex(0.46875f, 0.515625f, 0.75f), vertex(0.53125f, 0.515625f, 0.75f)};
  batch.commands.clear();
  Jak2ShadowBucket195Command walls;
  walls.kind = Jak2ShadowBucket195CommandKind::Walls;
  walls.records = {{{{0, 1, 0, 0xa5}}}};
  batch.commands.push_back(std::move(walls));
  plan.vertex_count = static_cast<u32>(batch.top_vertices.size() + batch.bottom_vertices.size());
  return plan;
}

std::vector<u8> one_transfer_chain() {
  std::vector<u8> chain(kNextBucket + 16, 0);
  const u64 next = static_cast<u64>(DmaTag::Kind::NEXT) << 28 |
                   static_cast<u64>(kNextBucket) << 32;
  const u64 end = static_cast<u64>(DmaTag::Kind::END) << 28;
  std::memcpy(chain.data() + kBucketOffset, &next, sizeof(next));
  std::memcpy(chain.data() + kNextBucket, &end, sizeof(end));
  return chain;
}

std::vector<u8> render_plan(id<MTLDevice> device,
                            id<MTLCommandQueue> queue,
                            id<MTLLibrary> library,
                            const metal_renderer::Jak2ShadowBucket195Plan& plan,
                            metal_renderer::MetalJak2Shadow2Renderer::Stats* out_stats,
                            metal_renderer::Jak2Shadow195FrameCaptureResult* out_capture =
                                nullptr,
                            double entry_depth = 0.0,
                            u32 entry_stencil = 0) {
  MetalPsoCache pso_cache;
  MetalStreamBuffer stream;
  check(pso_cache.init(device, library), "initialized the Shadow2 pipeline cache");
  stream.init(device);
  stream.reset();

  auto* color_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                   width:kTargetSize
                                  height:kTargetSize
                               mipmapped:NO];
  color_desc.usage = MTLTextureUsageRenderTarget;
#if TARGET_OS_OSX
  color_desc.storageMode = MTLStorageModeManaged;
#else
  color_desc.storageMode = MTLStorageModeShared;
#endif
  id<MTLTexture> color = [device newTextureWithDescriptor:color_desc];
  auto* depth_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                   width:kTargetSize
                                  height:kTargetSize
                               mipmapped:NO];
  depth_desc.usage = MTLTextureUsageRenderTarget;
  depth_desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth = [device newTextureWithDescriptor:depth_desc];
  check(color != nil && depth != nil, "created Shadow2 color and depth-stencil targets");

  id<MTLCommandBuffer> commands = [queue commandBuffer];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(100.0 / 255.0, 100.0 / 255.0,
                                                          100.0 / 255.0, 1.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = entry_depth;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = entry_stencil;
  id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
  [encoder setScissorRect:MTLScissorRect{0, 0, kTargetSize, kTargetSize}];

  auto chain = one_transfer_chain();
  DmaFollower dma(chain.data(), kBucketOffset, chain.size());
  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.buckets_base = 0;
  state.next_bucket = kNextBucket;
  state.jak2_shadow_bucket195_plan = &plan;
  state.game_res_w = 1280;
  state.game_res_h = 960;
  std::shared_ptr<metal_renderer::Jak2Shadow195FrameCapture> capture;
  if (out_capture) {
    capture = std::make_shared<metal_renderer::Jak2Shadow195FrameCapture>();
    metal_renderer::Jak2Shadow195FrameCaptureSelector selector;
    selector.match = metal_renderer::Jak2Shadow195MatchHostTick |
                     metal_renderer::Jak2Shadow195MatchEngineFrame |
                     metal_renderer::Jak2Shadow195MatchPlanFingerprint;
    selector.host_tick_id = 195;
    selector.engine_frame_id = 65;
    selector.plan_fingerprint = 0x195195195ull;
    check(capture->arm(selector) && capture->try_select({195, 195, 65, 0x195195195ull}),
          "armed the exact synthetic Shadow195 attachment selector");
    state.jak2_shadow195_frame_capture = capture.get();
    state.render_target_view_id = 77;
    state.render_target_external = true;
  }
  MetalFrameContext context;
  context.enc = encoder;
  context.pso_cache = &pso_cache;
  context.stream = &stream;
  context.color_format = MTLPixelFormatBGRA8Unorm;
  context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  context.cmds = commands;
  context.game_color = color;
  context.game_depth = depth;
  context.game_viewport = {0.0, 0.0, (double)kTargetSize, (double)kTargetSize, 0.0, 1.0};
  context.game_scissor = {0, 0, kTargetSize, kTargetSize};
  context.game_scissor_valid = true;
  context.game_scissor_explicit = true;
  context.color_load_action = static_cast<u32>(pass.colorAttachments[0].loadAction);
  context.color_store_action = static_cast<u32>(pass.colorAttachments[0].storeAction);
  context.depth_load_action = static_cast<u32>(pass.depthAttachment.loadAction);
  context.depth_store_action = static_cast<u32>(pass.depthAttachment.storeAction);
  context.stencil_load_action = static_cast<u32>(pass.stencilAttachment.loadAction);
  context.stencil_store_action = static_cast<u32>(pass.stencilAttachment.storeAction);

  metal_renderer::MetalJak2Shadow2Renderer renderer("shadow2-proof", plan.bucket_id);
  renderer.render(dma, &state, context);
  *out_stats = renderer.stats();
  [context.enc endEncoding];
#if TARGET_OS_OSX
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit synchronizeResource:color];
  [blit endEncoding];
#endif
  [commands commit];
  [commands waitUntilCompleted];
  check(commands.status == MTLCommandBufferStatusCompleted,
        "completed the deterministic Shadow2 GPU command buffer");
  if (out_capture) {
    check(capture->wait_for_terminal(1.0),
          "completed the same-command-buffer Shadow195 attachment reduction");
    *out_capture = capture->result();
  }

  std::vector<u8> pixels(kTargetSize * kTargetSize * 4);
  [color getBytes:pixels.data()
      bytesPerRow:kTargetSize * 4
       fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
      mipmapLevel:0];
  return pixels;
}

void check_no_draw_disposition(metal_renderer::Jak2ShadowBucket195PlanDisposition disposition,
                               const char* what) {
  using namespace metal_renderer;
  auto plan = ready_plan();
  plan.disposition = disposition;
  if (disposition == Jak2ShadowBucket195PlanDisposition::Absent) {
    plan.batches.clear();
    plan.vertex_count = 0;
    plan.record_count = 0;
  } else {
    plan.batches[0].bottom_vertices.clear();
    plan.batches[0].has_bottom_upload = false;
    plan.batches[0].top_only = true;
  }
  auto chain = one_transfer_chain();
  DmaFollower dma(chain.data(), kBucketOffset, chain.size());
  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.next_bucket = kNextBucket;
  state.jak2_shadow_bucket195_plan = &plan;
  MetalFrameContext context;
  MetalJak2Shadow2Renderer renderer("shadow2-no-draw", plan.bucket_id);
  renderer.render(dma, &state, context);
  const auto& stats = renderer.stats();
  check(stats.reached_boundary && stats.draw_calls == 0 && stats.output_vertices == 0 &&
            (disposition == Jak2ShadowBucket195PlanDisposition::Absent
                 ? stats.absent == 1
                 : stats.accepted_deferred_no_draw == 1),
        what);
}

class UnsupportedCaptureRenderer final : public MetalBucketRenderer {
 public:
  UnsupportedCaptureRenderer() : MetalBucketRenderer("unsupported-capture-route", 195) {}
  void render(DmaFollower&, MetalSharedRenderState*, MetalFrameContext&) override {}
};

void check_capture_dispatch_routes() {
  using namespace metal_renderer;
  MetalSkipRenderer deferred("deferred-shadow195", 195);
  MetalJak2Shadow2Renderer private_renderer("private-shadow195", 195);
  MetalJak2Shadow2Renderer production_renderer("production-shadow195", 195);
  UnsupportedCaptureRenderer unsupported;
  MetalBucketRenderer* selected = nullptr;

  const auto deferred_route =
      route_jak2_shadow195_frame_capture(&deferred, &private_renderer, &selected);
  const bool deferred_ok = deferred_route == Jak2Shadow195CaptureDispatchRoute::DeferredPrivate &&
                           selected == &private_renderer;
  const auto production_route =
      route_jak2_shadow195_frame_capture(&production_renderer, &private_renderer, &selected);
  const bool production_ok =
      production_route == Jak2Shadow195CaptureDispatchRoute::ProductionDirect &&
      selected == &production_renderer;
  const auto unsupported_route =
      route_jak2_shadow195_frame_capture(&unsupported, &private_renderer, &selected);
  const bool unsupported_ok =
      unsupported_route == Jak2Shadow195CaptureDispatchRoute::Unsupported &&
      selected == &unsupported;
  const auto missing_private_route =
      route_jak2_shadow195_frame_capture(&deferred, nullptr, &selected);
  const bool missing_private_ok =
      missing_private_route == Jak2Shadow195CaptureDispatchRoute::Unsupported &&
      selected == &deferred;
  check(deferred_ok && production_ok && unsupported_ok && missing_private_ok,
        "Shadow195 capture routes deferred privately, production directly, and others safely");
}

}  // namespace

int main() {
  @autoreleasepool {
    check_capture_dispatch_routes();
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    dispatch_data_t library_data = dispatch_data_create(
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(device != nil && queue != nil && library != nil,
          "loaded the product Metal library on a real GPU device");
    if (!device || !queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    metal_renderer::MetalJak2Shadow2Renderer::Stats first_stats;
    metal_renderer::MetalJak2Shadow2Renderer::Stats second_stats;
    metal_renderer::Jak2Shadow195FrameCaptureResult attachment_capture;
    const auto first_plan = ready_plan();
    const auto first =
        render_plan(device, queue, library, first_plan, &first_stats, &attachment_capture);
    const auto second = render_plan(device, queue, library, first_plan, &second_stats);
    const std::array<u8, 4> changed = {100, 132, 68, 255};
    std::size_t exact_changed = 0;
    std::size_t background = 0;
    for (std::size_t offset = 0; offset < first.size(); offset += 4) {
      exact_changed += std::equal(changed.begin(), changed.end(), first.begin() + offset);
      background += first[offset] == 100 && first[offset + 1] == 100 &&
                    first[offset + 2] == 100 && first[offset + 3] == 255;
    }
    check(first == second && exact_changed > 0 && background > 0,
          "Shadow2 stencil coverage and darken/lighten output are exact and deterministic");
    check(first_stats.executions == 1 && first_stats.ready == 1 &&
              first_stats.input_batches == 1 && first_stats.input_vertices == 8 &&
              first_stats.input_records == 2 && first_stats.output_vertices == 12 &&
              first_stats.front_triangles == 2 && first_stats.back_triangles == 2 &&
              first_stats.draw_calls == 4 && first_stats.triangles == 8 &&
              first_stats.darken_draws == 1 && first_stats.lighten_draws == 1 &&
              first_stats.reached_boundary && first_stats.unexpected_dma == 0 &&
              first_stats.invalid_plan == 0 && first_stats.nonfinite_projection == 0 &&
              first_stats.overflow == 0 && first_stats.pipeline_failures == 0,
          "Shadow2 renderer reports exact owned input, geometry, draws, and boundary stats");
    check(second_stats.output_vertices == first_stats.output_vertices &&
              second_stats.front_triangles == first_stats.front_triangles &&
              second_stats.back_triangles == first_stats.back_triangles &&
              second_stats.draw_calls == first_stats.draw_calls &&
              second_stats.triangles == first_stats.triangles &&
              second_stats.darken_draws == first_stats.darken_draws &&
              second_stats.lighten_draws == first_stats.lighten_draws &&
              second_stats.reached_boundary == first_stats.reached_boundary,
          "repeated Shadow2 GPU executions report identical telemetry");
    check(attachment_capture.status ==
                  metal_renderer::Jak2Shadow195FrameCaptureStatus::Complete &&
              attachment_capture.selected.host_tick_id == 195 &&
              attachment_capture.selected.engine_frame_id == 65 &&
              attachment_capture.target.view_id == 77 &&
              attachment_capture.target.external_target == 1 &&
              attachment_capture.target.width == kTargetSize &&
              attachment_capture.target.height == kTargetSize &&
              attachment_capture.target.pixel_format == MTLPixelFormatBGRA8Unorm &&
              attachment_capture.target.depth_pixel_format ==
                  MTLPixelFormatDepth32Float_Stencil8 &&
              attachment_capture.target.stencil_pixel_format ==
                  MTLPixelFormatDepth32Float_Stencil8 &&
              attachment_capture.target.color_load_action == MTLLoadActionClear &&
              attachment_capture.target.depth_load_action == MTLLoadActionClear &&
              attachment_capture.target.stencil_load_action == MTLLoadActionClear &&
              attachment_capture.target.color_store_action == MTLStoreActionStore &&
              attachment_capture.target.depth_store_action == MTLStoreActionStore &&
              attachment_capture.target.stencil_store_action == MTLStoreActionStore &&
              attachment_capture.target.scissor_width == kTargetSize &&
              attachment_capture.target.scissor_height == kTargetSize &&
              attachment_capture.target.scissor_explicit == 1 &&
              attachment_capture.target.render_scale_x == 2.0 &&
              attachment_capture.target.render_scale_y == 2.0 &&
              attachment_capture.before.hash != attachment_capture.after.hash &&
              attachment_capture.before.nonzero_pixels == kTargetSize * kTargetSize &&
              attachment_capture.after.nonzero_pixels == kTargetSize * kTargetSize &&
              attachment_capture.difference.changed_pixels > 0 &&
              attachment_capture.difference.changed_bounds.valid &&
              attachment_capture.before_depth.hash == attachment_capture.after_depth.hash &&
              attachment_capture.before_depth.finite_pixels ==
                  kTargetSize * kTargetSize &&
              attachment_capture.before_depth.nonfinite_pixels == 0 &&
              attachment_capture.before_depth.min_finite == 0.f &&
              attachment_capture.before_depth.max_finite == 0.f &&
              attachment_capture.depth_difference.changed_pixels == 0 &&
              attachment_capture.before_stencil.nonzero_pixels == 0 &&
              attachment_capture.volume_stencil.nonzero_pixels > 0 &&
              attachment_capture.volume_stencil_difference.changed_pixels > 0 &&
              attachment_capture.volume_stencil_difference.changed_bounds.valid &&
              attachment_capture.after_stencil.hash ==
                  attachment_capture.volume_stencil.hash &&
              attachment_capture.final_stencil_difference.changed_pixels == 0 &&
              attachment_capture.renderer.draw_calls == first_stats.draw_calls &&
              attachment_capture.renderer.triangles == first_stats.triangles &&
              attachment_capture.renderer.reached_boundary == 1,
          "same-frame capture retains pass metadata, color/depth/stencil checkpoints, and stats");

    metal_renderer::MetalJak2Shadow2Renderer::Stats blocked_stats;
    metal_renderer::Jak2Shadow195FrameCaptureResult blocked_capture;
    const auto blocked =
        render_plan(device, queue, library, first_plan, &blocked_stats, &blocked_capture, 1.0, 0);
    check(blocked_capture.status ==
                  metal_renderer::Jak2Shadow195FrameCaptureStatus::Complete &&
              blocked_capture.before_depth.min_finite == 1.f &&
              blocked_capture.before_depth.max_finite == 1.f &&
              blocked_capture.before_depth.hash == blocked_capture.after_depth.hash &&
              blocked_capture.depth_difference.changed_pixels == 0 &&
              blocked_capture.before_stencil.nonzero_pixels == 0 &&
              blocked_capture.volume_stencil.nonzero_pixels == 0 &&
              blocked_capture.volume_stencil_difference.changed_pixels == 0 &&
              blocked_capture.before.hash == blocked_capture.after.hash && blocked != first,
          "entry depth deterministically rejects the volume without changing depth or stencil");

    metal_renderer::MetalJak2Shadow2Renderer::Stats seeded_stats;
    metal_renderer::Jak2Shadow195FrameCaptureResult seeded_capture;
    const auto seeded =
        render_plan(device, queue, library, first_plan, &seeded_stats, &seeded_capture, 1.0, 7);
    check(seeded_capture.status ==
                  metal_renderer::Jak2Shadow195FrameCaptureStatus::Complete &&
              seeded_capture.before_stencil.nonzero_pixels == kTargetSize * kTargetSize &&
              seeded_capture.before_stencil.hash == seeded_capture.volume_stencil.hash &&
              seeded_capture.volume_stencil_difference.changed_pixels == 0 &&
              seeded_capture.volume_stencil.hash == seeded_capture.after_stencil.hash &&
              seeded_capture.final_stencil_difference.changed_pixels == 0 &&
              seeded_capture.before.hash != seeded_capture.after.hash && seeded != blocked,
          "entry stencil deterministically shades without a volume-generated stencil delta");
    metal_renderer::MetalJak2Shadow2Renderer::Stats wall_stats;
    const auto wall = render_plan(device, queue, library, wall_plan(), &wall_stats);
    std::size_t wall_changed = 0;
    std::size_t wall_background = 0;
    for (std::size_t offset = 0; offset < wall.size(); offset += 4) {
      const bool is_background = wall[offset] == 100 && wall[offset + 1] == 100 &&
                                 wall[offset + 2] == 100 && wall[offset + 3] == 255;
      wall_changed += !is_background;
      wall_background += is_background;
    }
    check(wall_changed > 0 && wall_background > 0 && wall_stats.input_records == 1 &&
              wall_stats.output_vertices == 6 &&
              wall_stats.front_triangles == 2 && wall_stats.back_triangles == 0 &&
              wall_stats.draw_calls == 3 && wall_stats.triangles == 6 &&
              wall_stats.darken_draws == 1 && wall_stats.lighten_draws == 1 &&
              wall_stats.invalid_plan == 0 && wall_stats.reached_boundary,
          "a wall quad uses one desktop-equivalent stencil-side classification for both triangles");
    check_no_draw_disposition(
        metal_renderer::Jak2ShadowBucket195PlanDisposition::Absent,
        "an Absent plan consumes exactly to the follower boundary without drawing");
    check_no_draw_disposition(
        metal_renderer::Jak2ShadowBucket195PlanDisposition::AcceptedDeferredNoDraw,
        "a top-only plan consumes exactly without dereferencing a missing bottom upload");
  }

  if (failures) {
    std::printf("FAIL: %d Jak II Shadow2 renderer checks failed\n", failures);
    return 1;
  }
  std::puts("Jak II Shadow2 source-exact Metal renderer proof passed");
  return 0;
}
