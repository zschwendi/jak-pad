/*! GPU regression: ACTUAL translated Jak 1 sky generation under a head
 * rotation H, consumed by the production MetalSkyRenderer with the native sky_source_* state.
 * The legacy test runs first, unchanged, through its renamed main. */
#define main preserved_sky_depth_main
#include "game/graphics/pipelines/metal/metal_jak1_sky_immersive_depth_test.mm"
#undef main

#include "game/graphics/pipelines/metal/jak1_sky_generation_gpu_fixture.h"
extern u8* g_ee_main_mem;

#include "game/graphics/sky_generation.h"

namespace {
using jak1_sky_gpu_fixture::kMemSize;

std::uint64_t g_engine = 41;
std::uint64_t engine_id() {
  return g_engine;
}

void put_qw(u32 addr, u64 lo, u64 hi) {
  std::memcpy(g_ee_main_mem + addr, &lo, 8);
  std::memcpy(g_ee_main_mem + addr + 8, &hi, 8);
}
void put_gif(u32 addr, const std::vector<Qw>& gif) {
  std::memcpy(g_ee_main_mem + addr, gif.data(), gif.size() * 16);
}

struct InPlaceBucket {
  bool ok = false;
  u32 default_regs = 0;
  u32 next_bucket = 0;
};

// Wraps the ACTUAL bytes at 0x9000 in place (no relocation). Layout:
// 0: NEXT->0x8f40 | 0x8f40 CNT setup(4) | 0x8f90 CNT texture(5) | 0x8ff0 CNT geometry+terminator |
// tail = make_sky_chain's: empty NEXT -> CALL default_regs (CNT1 zero, RET) -> NEXT next_bucket(END)
InPlaceBucket wrap_full_bucket(const jak1_sky_gpu_fixture::Generated& g) {
  InPlaceBucket b;
  if (!g.ok || g.begin != 0x9000 || (g.end - g.begin) % 16) return b;
  std::memcpy(g_ee_main_mem + g.end, sky_generation::kCanonicalTerminator,
              sky_generation::kTerminatorBytes);
  const u32 geo_end = g.end + sky_generation::kTerminatorBytes;
  const u16 geo_qwc = static_cast<u16>((geo_end - 0x9000) / 16);
  const auto setup = sky_setup_gif();
  const auto texture = sky_texture_setup_gif();
  if (setup.size() != 4 || texture.size() != 5) return b;
  put_qw(0, dma_tag(DmaTag::Kind::NEXT, 0, 0x8f40), 0);
  put_qw(0x8f40, dma_tag(DmaTag::Kind::CNT, 4, 0), u64(vif_direct(4)) << 32);
  put_gif(0x8f50, setup);
  put_qw(0x8f90, dma_tag(DmaTag::Kind::CNT, 5, 0), u64(vif_direct(5)) << 32);
  put_gif(0x8fa0, texture);
  put_qw(0x8ff0, dma_tag(DmaTag::Kind::CNT, geo_qwc, 0), u64(vif_direct(geo_qwc)) << 32);
  // make_sky_chain order: empty NEXT, CALL, NEXT(next_bucket), default_regs CNT1+zero, RET, END.
  const u32 empty = geo_end, call = empty + 16, next = call + 16;
  b.default_regs = next + 16;
  const u32 ret = b.default_regs + 32;
  b.next_bucket = ret + 16;
  put_qw(empty, dma_tag(DmaTag::Kind::NEXT, 0, call), 0);
  put_qw(call, dma_tag(DmaTag::Kind::CALL, 0, b.default_regs), 0);
  put_qw(next, dma_tag(DmaTag::Kind::NEXT, 0, b.next_bucket), 0);
  put_qw(b.default_regs, dma_tag(DmaTag::Kind::CNT, 1, 0), 0);
  put_qw(b.default_regs + 16, 0, 0);
  put_qw(ret, dma_tag(DmaTag::Kind::RET, 0, 0), 0);
  put_qw(b.next_bucket, dma_tag(DmaTag::Kind::END, 0, 0), 0);
  b.ok = true;
  return b;
}

Frame render_generated(Gpu& gpu,
                       const InPlaceBucket& bucket,
                       const metal_renderer::ViewTransform& world,
                       const metal_renderer::ViewTransform& sky_transform,
                       bool sky_valid,
                       std::uint64_t epoch) {
  Frame frame;
  gpu.stream.reset();
  auto* color_desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:kTargetSize
                                                        height:kTargetSize
                                                     mipmapped:NO];
  color_desc.usage = MTLTextureUsageRenderTarget;
  color_desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> color = [gpu.device newTextureWithDescriptor:color_desc];
  id<MTLBuffer> color_readback = [gpu.device newBufferWithLength:kReadbackRowBytes * kTargetSize
                                                         options:MTLResourceStorageModeShared];
  auto* depth_desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:kTargetSize
                                                        height:kTargetSize
                                                     mipmapped:NO];
  depth_desc.usage = MTLTextureUsageRenderTarget;
  depth_desc.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth = [gpu.device newTextureWithDescriptor:depth_desc];
  id<MTLBuffer> depth_readback = [gpu.device newBufferWithLength:kReadbackRowBytes * kTargetSize
                                                         options:MTLResourceStorageModeShared];
  ASSERT(color && color_readback && depth && depth_readback);

  id<MTLCommandBuffer> commands = [gpu.queue commandBuffer];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(kClear.r / 255.0, kClear.g / 255.0, kClear.b / 255.0, 1.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = kDepthSentinel;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;

  MetalFrameContext ctx;
  ctx.cmds = commands;
  ctx.enc = [commands renderCommandEncoderWithDescriptor:pass];
  ctx.pso_cache = &gpu.pso_cache;
  ctx.sampler_cache = &gpu.sampler_cache;
  ctx.stream = &gpu.stream;
  ctx.color_format = MTLPixelFormatBGRA8Unorm;
  ctx.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  ctx.game_color = color;
  ctx.game_depth = depth;
  ctx.game_viewport = {0.0, 0.0, kTargetSize, kTargetSize, 0.0, 1.0};
  [ctx.enc setCullMode:MTLCullModeNone];
  [ctx.enc setViewport:ctx.game_viewport];

  MetalSharedRenderState state;
  state.version = GameVersion::Jak1;
  state.next_bucket = bucket.next_bucket;
  state.default_regs_buffer = bucket.default_regs;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;
  state.view_transform = world;
  state.immersive_camera_enabled = true;
  // Native candidate field names (API_DRAFT / supervisor brief); verify against metal_bucket_renderer.h.
  state.ee_memory = g_ee_main_mem;
  state.dma_copy_base = g_ee_main_mem;
  state.dma_copy_size = kMemSize;
  state.engine_frame_id = g_engine;
  state.sky_source_view_transform = sky_transform;
  state.sky_source_view_transform_valid = sky_valid;
  state.sky_source_expected = sky_valid;  // explicit test pair: positive true, ordinary false
  state.sky_source_epoch = epoch;

  DmaFollower dma(g_ee_main_mem, 0, kMemSize);
  MetalSkyRenderer sky("synthetic-jak1-generated-sky", 3);
  sky.render(dma, &state, ctx);
  check(state.view_transform.clip_from_game_clip == world.clip_from_game_clip,
        "source sky scope restores World transform exactly");
  check(dma.current_tag_offset() == bucket.next_bucket, "generated bucket returns to next_bucket");
  frame.sky_draw_calls = sky.direct_stats().draw_calls;
  frame.sky_triangles = sky.direct_stats().triangles;
  [ctx.enc endEncoding];

  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:color
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(kTargetSize, kTargetSize, 1)
                      toBuffer:color_readback
             destinationOffset:0
        destinationBytesPerRow:kReadbackRowBytes
      destinationBytesPerImage:kReadbackRowBytes * kTargetSize];
  [blit copyFromTexture:depth
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(kTargetSize, kTargetSize, 1)
                      toBuffer:depth_readback
             destinationOffset:0
        destinationBytesPerRow:kReadbackRowBytes
      destinationBytesPerImage:kReadbackRowBytes * kTargetSize
                       options:MTLBlitOptionDepthFromDepthStencil];
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  if (commands.status != MTLCommandBufferStatusCompleted) return frame;
  frame.bgra.resize(kTargetSize * kTargetSize * 4);
  std::memcpy(frame.bgra.data(), color_readback.contents, frame.bgra.size());
  frame.depth.resize(kTargetSize * kTargetSize);
  std::memcpy(frame.depth.data(), depth_readback.contents, frame.depth.size() * sizeof(float));
  frame.ok = true;
  return frame;
}

Matrix4 mul(const Matrix4& a, const Matrix4& b) {  // column-major
  Matrix4 o{};
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      float s = 0.f;
      for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
      o[c * 4 + r] = s;
    }
  return o;
}
Matrix4 translate_x(float x) {
  return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
}
Matrix4 transpose3(const Matrix4& h) {
  Matrix4 o{};
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 3; r++) o[c * 4 + r] = h[r * 4 + c];
  o[15] = 1.f;
  return o;
}
Matrix4 head(float yaw_deg, float pitch_deg) {
  const float y = yaw_deg * 3.14159265f / 180.f, p = pitch_deg * 3.14159265f / 180.f;
  const Matrix4 ry = {std::cos(y), 0, -std::sin(y), 0, 0, 1, 0, 0, std::sin(y), 0, std::cos(y), 0,
                      0, 0, 0, 1};
  const Matrix4 rx = {1, 0, 0, 0, 0, std::cos(p), std::sin(p), 0, 0, -std::sin(p), std::cos(p), 0,
                      0, 0, 0, 1};
  return mul(ry, rx);
}

metal_renderer::ViewTransform derive_with(const Matrix4& eye_from_game) {
  metal_renderer::PhysicalViewCamera view;
  view.enabled = true;
  view.clip_from_eye = reverse_z_eye_projection();
  view.eye_from_game_camera_meters = eye_from_game;
  metal_renderer::ViewTransform out;
  auto error = metal_renderer::ImmersiveCameraError::None;
  const bool ok = metal_renderer::derive_immersive_view_transform(
      jak1_sky_gpu_fixture::packet(), metal_renderer::kJak1MetalScissorAdjust, view, &out, &error);
  check(ok && error == metal_renderer::ImmersiveCameraError::None, "derive ok");
  return out;
}

int count_gaps(const Frame& f, int* depth_bad) {
  int gaps = 0, bad = 0;
  for (int y = 0; y < kTargetSize; y++)
    for (int x = 0; x < kTargetSize; x++) {
      const Pixel p = f.pixel(x, y);
      gaps += (p.r == kClear.r && p.g == kClear.g && p.b == kClear.b);
      bad += f.depth_at(x, y) != 0.f;
    }
  *depth_bad = bad;
  return gaps;
}

int generated_main(Gpu& gpu) {
  jak1_sky_gpu_fixture::setup_memory();
  // Synthetic giftag choice: original 0x302ec00000008001 has PRIM 0x5D (TRI_FAN|IIP|TME|ABE);
  // clear TME (prim bit 4 = lo bit 51) so the white 128 colors render without a texture pool.
  // Emitted XYZ/STQ bytes remain the actual translated output.
  u64 lo = 0;
  std::memcpy(&lo, g_ee_main_mem + jak1_sky_gpu_fixture::kGiftag, 8);
  lo &= ~(1ull << 51);
  std::memcpy(g_ee_main_mem + jak1_sky_gpu_fixture::kGiftag, &lo, 8);
  jak1_sky_gpu_fixture::setup_caches();
  jak1_sky_gpu_fixture::configure_camera(jak1_sky_gpu_fixture::packet());
  const auto camera_before = jak1_sky_gpu_fixture::camera_snapshot();

  const float cases[6][2] = {{0, 0}, {90, 0}, {180, 0}, {270, 0}, {0, 60}, {0, -60}};
  const float eyes[2] = {-0.032f, 0.032f};
  for (const auto& hc : cases) {
    const Matrix4 h = head(hc[0], hc[1]);
    for (float ex : eyes) {
      const std::string name = "yaw" + std::to_string(int(hc[0])) + " pitch" +
                               std::to_string(int(hc[1])) + " eye" + std::to_string(ex);
      sky_generation::FrameIdentity id{};
      check(sky_generation::begin_frame(h, 1, g_engine, &engine_id, &id), name + " begin_frame");
      jak1_sky_gpu_fixture::init_registers();
      const auto g = jak1_sky_gpu_fixture::emit_bipyramid(0x9000);
      const auto bucket = wrap_full_bucket(g);
      check(bucket.ok, name + " in-place bucket");
      const Matrix4 e_original = mul(translate_x(-ex), transpose3(h));  // inverse(H*T(+ex))
      const Matrix4 e_pair = translate_x(-ex);                         // E_original*H, once
      const Frame f = render_generated(gpu, bucket, derive_with(e_original), derive_with(e_pair),
                                       true, id.epoch);
      int depth_bad = 0;
      const int gaps = f.ok ? count_gaps(f, &depth_bad) : -1;
      check(f.ok && f.sky_triangles > 0, name + " sky triangles drawn");
      if (gaps || depth_bad)
        std::printf("  %s: %d clear pixels, %d depth!=0\n", name.c_str(), gaps, depth_bad);
      check(gaps == 0 && depth_bad == 0, name + " every pixel sky with depth 0");
      sky_generation::end_frame();
    }
  }

  {
    const auto fixed_world = derive_with(mul(translate_x(-0.032f), transpose3(head(90, 0))));
    const Frame off = render(gpu, SkyLayout::ScreenGradient, fixed_world, true);
    sky_generation::FrameIdentity fallback_id{};
    check(sky_generation::begin_frame(head(90, 0), 2, g_engine, &engine_id, &fallback_id),
          "fallback begin_frame");
    const Frame on = render(gpu, SkyLayout::ScreenGradient, fixed_world, true);
    check(off.ok && on.ok && off.bgra == on.bgra && off.depth.size() == on.depth.size() &&
              std::memcmp(off.depth.data(), on.depth.data(), off.depth.size() * sizeof(float)) == 0,
          "fallback RGBA/depth bytes unchanged while H context active");
    sky_generation::end_frame();
  }

  // Negative control: ordinary generation (no context), yaw 90 original eyes, sky state invalid.
  for (float ex : eyes) {
    const Matrix4 h = head(90, 0);
    jak1_sky_gpu_fixture::init_registers();
    const auto g = jak1_sky_gpu_fixture::emit_bipyramid(0x9000);
    const auto bucket = wrap_full_bucket(g);
    const Matrix4 e_original = mul(translate_x(-ex), transpose3(h));
    const Frame f = render_generated(gpu, bucket, derive_with(e_original),
                                     derive_with(e_original), false, 0);
    int depth_bad = 0;
    const int gaps = f.ok ? count_gaps(f, &depth_bad) : -1;
    int clear_sentinels = 0;
    for (int y = 0; f.ok && y < kTargetSize; y++)
      for (int x = 0; x < kTargetSize; x++) {
        const Pixel p = f.pixel(x, y);
        clear_sentinels += p.r == kClear.r && p.g == kClear.g && p.b == kClear.b &&
                           f.depth_at(x, y) == kDepthSentinel;
      }
    check(f.ok && gaps > 0 && clear_sentinels > 0,
          "negative control yaw90 ordinary source shows untouched clear+sentinel gaps");
  }
  check(jak1_sky_gpu_fixture::camera_snapshot() == camera_before, "camera bytes unchanged");
  return g_failures;
}
}  // namespace

int main() {
  const int legacy = preserved_sky_depth_main();
  g_failures = 0;
  Gpu gpu;
  gpu.device = MTLCreateSystemDefaultDevice();
  ASSERT(gpu.device);
  gpu.queue = [gpu.device newCommandQueue];
  ASSERT(gpu.queue);
  dispatch_data_t library_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                      nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  id<MTLLibrary> library = [gpu.device newLibraryWithData:library_data error:&library_error];
  ASSERT(library);
  ASSERT(gpu.pso_cache.init(gpu.device, library));
  gpu.sampler_cache.init(gpu.device);
  gpu.stream.init(gpu.device);
  const int generated = generated_main(gpu);
  std::printf("metal-jak1-sky-generated-immersive-test: legacy=%s generated=%s (%d)\n",
              legacy ? "FAIL" : "PASS", generated ? "FAIL" : "PASS", generated);
  return legacy || generated ? 1 : 0;
}
