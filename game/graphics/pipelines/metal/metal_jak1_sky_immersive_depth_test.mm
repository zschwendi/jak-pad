// GPU regression for the black physical-immersive Jak 1 sky.
//
// Synthetic DMA in the two production sky-bucket layouts (sky-tng's dome envelope and the
// screen-gradient fallback) is replayed through the production MetalSkyRenderer and its direct
// renderer, with eye transforms from the production derive_immersive_view_transform. Every sky
// vertex has GS z = 0, as sky-tng emits it. Without the immersive sky depth clamp the eye's
// 5,000 m reverse-Z far plane clips the whole sky and the target keeps its clear colour; with it
// the sky keeps its per-eye direction and writes the reverse-Z far depth 0. Controls cover the
// Comfort identity path, a non-physical transform that must stay clipped, eye translation and
// yaw, and a later out-of-frustum World draw that must stay clipped (the clamp cannot leak).
// All geometry and colours are synthetic.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/util/Assert.h"

#include "game/graphics/pipelines/metal/metal_direct_renderer.h"
#include "game/graphics/pipelines/metal/metal_immersive_camera.h"
#include "game/graphics/pipelines/metal/metal_sky_renderer.h"
#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

using Matrix4 = std::array<float, 16>;
using Vector4 = std::array<float, 4>;

constexpr int kTargetSize = 64;
// 64 BGRA8 or Depth32Float texels: one 256-byte-aligned readback row for both targets.
constexpr int kReadbackRowBytes = kTargetSize * 4;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kGsDepthScale = 16777215.f;
constexpr float kDepthSentinel = 0.25f;
constexpr u32 kJak1Zbp = 448;
constexpr u32 kSkyBaseBlock = 8160;

struct Pixel {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
};

constexpr Pixel kClear = {0, 0, 0};
constexpr Pixel kSkyLeft = {40, 120, 200};
constexpr Pixel kSkyRight = {200, 160, 40};
constexpr Pixel kGradient = {90, 30, 150};
constexpr Pixel kWorldControl = {250, 250, 250};

int g_failures = 0;

void check(bool condition, const std::string& message) {
  if (!condition) {
    g_failures++;
    std::printf("  FAIL: %s\n", message.c_str());
  }
}

bool pixel_near(const Pixel& actual, const Pixel& expected, int tolerance = 2) {
  return std::abs(actual.r - expected.r) <= tolerance &&
         std::abs(actual.g - expected.g) <= tolerance &&
         std::abs(actual.b - expected.b) <= tolerance;
}

std::string describe(const Pixel& pixel) {
  return "(" + std::to_string(pixel.r) + "," + std::to_string(pixel.g) + "," +
         std::to_string(pixel.b) + ")";
}

// ---------------------------------------------------------------------------------------------
// Camera: Jak 1 update-math-camera constants and a host-shaped reverse-Z eye.

float& element(Matrix4& matrix, int column, int row) {
  return matrix[column * 4 + row];
}

metal_renderer::ImmersivePacketCamera make_jak1_packet() {
  constexpr float kNear = 1024.f;
  constexpr float kFar = 40960000.f;
  constexpr float kFogStart = 40960.f;
  constexpr float kFogEnd = 819200.f;
  constexpr float kFogMinimum = 150.f;
  constexpr float kFogMaximum = 255.f;
  constexpr float kDepthMinimum = 100.f;
  constexpr float kDepthMaximum = 16760631.f;
  const float x_ratio = std::tan(32.f * kPi / 180.f);
  const float y_ratio = x_ratio * 0.75f;
  const float fog_slope = kNear * (kFogMinimum - kFogMaximum) / (kFogEnd - kFogStart);
  const float depth_half_range = -0.5f * (kDepthMaximum - kDepthMinimum);
  const float half_depth_slope = depth_half_range / (kNear * (kFar - kNear));

  metal_renderer::ImmersivePacketCamera packet;
  packet.hvdf_off = {2048.f, 2048.f, 0.5f * (kDepthMaximum + kDepthMinimum), 17.f};
  packet.fog = {fog_slope, kFogMinimum, kFogMaximum, 0.f};
  packet.trans = {1234.f, -567.f, 9101.f, 1.f};
  const float c = std::cos(0.4f);
  const float s = std::sin(0.4f);
  packet.rot = {{{c, 0.f, -s, 0.f}, {0.f, 1.f, 0.f, 0.f}, {s, 0.f, c, 0.f}, {0.f, 0.f, 0.f, 1.f}}};
  packet.perspective[0][0] = -256.f / (x_ratio * kNear);
  packet.perspective[1][1] = -112.f / (y_ratio * kNear);
  packet.perspective[2][2] = (kFar + kNear) * half_depth_slope;
  packet.perspective[2][3] = fog_slope / kNear;
  packet.perspective[3][2] = -2.f * half_depth_slope * kFar * kNear;
  return packet;
}

// Reverse-Z right/up/back projection with the host's 5,000 m far plane.
Matrix4 reverse_z_eye_projection() {
  constexpr float kNearMeters = 0.1f;
  constexpr float kFarMeters = 5000.f;
  Matrix4 projection = {};
  element(projection, 0, 0) = 1.2f;
  element(projection, 1, 1) = 1.35f;
  element(projection, 2, 2) = kNearMeters / (kFarMeters - kNearMeters);
  element(projection, 2, 3) = -1.f;
  element(projection, 3, 2) = kNearMeters * kFarMeters / (kFarMeters - kNearMeters);
  return projection;
}

// eye_from_game_camera_meters for an eye at `right_meters` along game-camera right, yawed by
// `yaw` radians about game-camera up: inverse(translate * rotate_y).
Matrix4 eye_from_game_camera(float right_meters, float yaw) {
  const float c = std::cos(yaw);
  const float s = std::sin(yaw);
  Matrix4 out = metal_renderer::kIdentityMatrix4;
  // transpose of rotate_y = [c 0 s; 0 1 0; -s 0 c]
  element(out, 0, 0) = c;
  element(out, 2, 0) = -s;
  element(out, 0, 2) = s;
  element(out, 2, 2) = c;
  // -R^T * t, t = (right_meters, 0, 0)
  element(out, 3, 0) = -c * right_meters;
  element(out, 3, 2) = -s * right_meters;
  return out;
}

metal_renderer::ViewTransform derive_eye(const Matrix4& eye_from_game) {
  metal_renderer::PhysicalViewCamera view;
  view.enabled = true;
  view.clip_from_eye = reverse_z_eye_projection();
  view.eye_from_game_camera_meters = eye_from_game;
  metal_renderer::ViewTransform out;
  auto error = metal_renderer::ImmersiveCameraError::None;
  const bool ok = metal_renderer::derive_immersive_view_transform(
      make_jak1_packet(), metal_renderer::kJak1MetalScissorAdjust, view, &out, &error);
  ASSERT(ok && error == metal_renderer::ImmersiveCameraError::None);
  return out;
}

// Direct-shader game clip for game NDC (x, y) at GS depth z.
Vector4 game_clip(float x, float y, float gs_z) {
  return {x, y, gs_z / kGsDepthScale, 1.f};
}

Vector4 eye_clip(const metal_renderer::ViewTransform& transform, const Vector4& clip) {
  return metal_renderer::apply(transform, clip);
}

// Target pixel for the eye-NDC position of a game-clip point.
std::array<int, 2> eye_pixel(const metal_renderer::ViewTransform& transform, const Vector4& clip) {
  const auto eye = eye_clip(transform, clip);
  const float x = eye[0] / eye[3];
  const float y = eye[1] / eye[3];
  return {static_cast<int>(std::floor((x + 1.f) * 0.5f * kTargetSize)),
          static_cast<int>(std::floor((1.f - y) * 0.5f * kTargetSize))};
}

// ---------------------------------------------------------------------------------------------
// Synthetic DMA.

struct Qw {
  u64 lo = 0;
  u64 hi = 0;
};

u32 vif_direct(u16 qwc) {
  return (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | qwc;
}

u64 dma_tag(DmaTag::Kind kind, u16 qwc, u32 addr) {
  return qwc | (static_cast<u64>(kind) << 28) | (static_cast<u64>(addr) << 32);
}

u64 gif_tag(u32 nloop, bool pre, u64 prim, GifTag::Format format, u32 nreg) {
  return nloop | (1ull << 15) | (static_cast<u64>(pre) << 46) | (prim << 47) |
         (static_cast<u64>(format) << 58) | (static_cast<u64>(nreg) << 60);
}

u64 gif_regs(std::initializer_list<GifTag::RegisterDescriptor> regs) {
  u64 out = 0;
  int i = 0;
  for (auto reg : regs) {
    out |= static_cast<u64>(reg) << (4 * i++);
  }
  return out;
}

u64 gs_prim(GsPrim::Kind kind) {
  return static_cast<u64>(kind) | (1u << 3);  // gouraud, untextured, no blend
}

Qw ad(GsRegisterAddress address, u64 value) {
  return {value, static_cast<u64>(address)};
}

// sky-tng / drawable.gc envelope: ZBUF with depth writes, ZTST ALWAYS, ALPHA (Cs - Cd) As + Cd.
std::vector<Qw> sky_setup_gif() {
  return {
      {gif_tag(3, false, 0, GifTag::Format::PACKED, 1), gif_regs({GifTag::RegisterDescriptor::AD})},
      ad(GsRegisterAddress::ZBUF_1, kJak1Zbp | (1ull << 24)),
      ad(GsRegisterAddress::TEST_1, 1 | (1 << 1) | (1 << 16) | (1 << 17)),
      ad(GsRegisterAddress::ALPHA_1, (1 << 2) | (1 << 6)),
  };
}

// sky-tng's 5-qword texture setup for the dome.
std::vector<Qw> sky_texture_setup_gif() {
  const u64 tex0 = kSkyBaseBlock | (1ull << 14) | (5ull << 26) | (5ull << 30);
  return {
      {gif_tag(4, false, 0, GifTag::Format::PACKED, 1), gif_regs({GifTag::RegisterDescriptor::AD})},
      ad(GsRegisterAddress::TEX0_1, tex0),
      ad(GsRegisterAddress::TEX1_1, (1 << 5) | (1 << 6)),
      ad(GsRegisterAddress::CLAMP_1, 0b0101),
      ad(GsRegisterAddress::TEXFLUSH, 0),
  };
}

Qw rgbaq(const Pixel& color) {
  return {color.r | (static_cast<u64>(color.g) << 32), color.b | (0x80ull << 32)};
}

// Packed XYZF2 in GS pixel coordinates (12.4 fixed point) with a 24-bit depth.
Qw xyzf2(float x_pixels, float y_pixels, u32 gs_z) {
  const u64 x = static_cast<u64>(x_pixels * 16.f);
  const u64 y = static_cast<u64>(y_pixels * 16.f);
  return {x | (y << 32), static_cast<u64>(gs_z) << 4};
}

Qw st() {
  const float s = 0.f;
  const float t = 0.f;
  const float q = 1.f;
  Qw out;
  std::memcpy(&out.lo, &s, 4);
  std::memcpy(reinterpret_cast<u8*>(&out.lo) + 4, &t, 4);
  std::memcpy(&out.hi, &q, 4);
  return out;
}

// sky-tng roof-shaped TRI_FAN (ST, RGBAQ, XYZF2) quad in GS pixels.
std::vector<Qw> sky_fan_gif(float x0, float y0, float x1, float y1, const Pixel& color) {
  std::vector<Qw> out = {
      {gif_tag(4, true, gs_prim(GsPrim::Kind::TRI_FAN), GifTag::Format::PACKED, 3),
       gif_regs({GifTag::RegisterDescriptor::ST, GifTag::RegisterDescriptor::RGBAQ,
                 GifTag::RegisterDescriptor::XYZF2})}};
  const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
  for (const auto& corner : corners) {
    out.push_back(st());
    out.push_back(rgbaq(color));
    out.push_back(xyzf2(corner[0], corner[1], 0));
  }
  return out;
}

// draw-quad2d-shaped TRI_STRIP (PRIM, then four RGBAQ/XYZF2 pairs) quad in GS pixels.
std::vector<Qw> quad2d_gif(float x0, float y0, float x1, float y1, u32 gs_z, const Pixel& color) {
  using Reg = GifTag::RegisterDescriptor;
  std::vector<Qw> out = {
      {gif_tag(1, false, 0, GifTag::Format::PACKED, 9),
       gif_regs({Reg::PRIM, Reg::RGBAQ, Reg::XYZF2, Reg::RGBAQ, Reg::XYZF2, Reg::RGBAQ, Reg::XYZF2,
                 Reg::RGBAQ, Reg::XYZF2})},
      {gs_prim(GsPrim::Kind::TRI_STRIP), 0},
  };
  const float corners[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
  for (const auto& corner : corners) {
    out.push_back(rgbaq(color));
    out.push_back(xyzf2(corner[0], corner[1], gs_z));
  }
  return out;
}

// GS pixel bounds of game NDC [-ndc, ndc] (Jak 1: 256 px half-width, 112 px half-height).
float gs_x(float ndc) {
  return 2048.f + 256.f * ndc;
}
float gs_y(float ndc) {
  return 2048.f + 112.f * ndc;
}

enum class SkyLayout { SkyTng, ScreenGradient };

struct SkyChain {
  std::vector<u8> bytes;
  u32 next_bucket = 0;
  u32 default_regs = 0;

  u32 offset() const { return static_cast<u32>(bytes.size()); }

  void tag(u64 value, u32 vif0, u32 vif1) {
    const std::size_t at = bytes.size();
    bytes.resize(at + 16);
    std::memcpy(bytes.data() + at, &value, 8);
    std::memcpy(bytes.data() + at + 8, &vif0, 4);
    std::memcpy(bytes.data() + at + 12, &vif1, 4);
  }

  void patch_tag(u32 at, u64 value) { std::memcpy(bytes.data() + at, &value, 8); }

  void direct(const std::vector<Qw>& gif) {
    tag(dma_tag(DmaTag::Kind::CNT, static_cast<u16>(gif.size()), 0), 0,
        vif_direct(static_cast<u16>(gif.size())));
    for (const auto& qw : gif) {
      const std::size_t at = bytes.size();
      bytes.resize(at + 16);
      std::memcpy(bytes.data() + at, &qw.lo, 8);
      std::memcpy(bytes.data() + at + 8, &qw.hi, 8);
    }
  }
};

// Bucket shape consumed by MetalSkyRenderer::render: opening NEXT, 4-qword GS setup, optional
// sky-tng texture setup, DIRECT draws, empty NEXT, CALL into the default-register block, NEXT
// to the following bucket.
SkyChain make_sky_chain(SkyLayout layout) {
  SkyChain chain;
  chain.tag(dma_tag(DmaTag::Kind::NEXT, 0, 16), 0, 0);
  chain.direct(sky_setup_gif());
  if (layout == SkyLayout::SkyTng) {
    chain.direct(sky_texture_setup_gif());
    // Dome halves span sky-tng's 4x guard band, split at the game camera's forward direction.
    chain.direct(sky_fan_gif(gs_x(-4.f), gs_y(-4.f), gs_x(0.f), gs_y(4.f), kSkyLeft));
    chain.direct(sky_fan_gif(gs_x(0.f), gs_y(-4.f), gs_x(4.f), gs_y(4.f), kSkyRight));
  } else {
    chain.direct(quad2d_gif(gs_x(-1.f), gs_y(-1.f), gs_x(1.f), gs_y(1.f), 0, kGradient));
  }
  const u32 empty = chain.offset();
  chain.tag(0, 0, 0);
  const u32 call = chain.offset();
  chain.tag(0, 0, 0);
  const u32 next = chain.offset();
  chain.tag(0, 0, 0);
  chain.default_regs = chain.offset();
  chain.tag(dma_tag(DmaTag::Kind::CNT, 1, 0), 0, 0);
  chain.bytes.resize(chain.bytes.size() + 16);
  chain.tag(dma_tag(DmaTag::Kind::RET, 0, 0), 0, 0);
  chain.next_bucket = chain.offset();
  chain.tag(dma_tag(DmaTag::Kind::END, 0, 0), 0, 0);
  chain.patch_tag(empty, dma_tag(DmaTag::Kind::NEXT, 0, call));
  chain.patch_tag(call, dma_tag(DmaTag::Kind::CALL, 0, chain.default_regs));
  chain.patch_tag(next, dma_tag(DmaTag::Kind::NEXT, 0, chain.next_bucket));
  return chain;
}

// ---------------------------------------------------------------------------------------------
// Rendering.

struct Gpu {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  MetalPsoCache pso_cache;
  MetalSamplerCache sampler_cache;
  MetalStreamBuffer stream;
};

struct WorldControl {
  bool enabled = false;
  u32 gs_z = 0;
};

struct Frame {
  bool ok = false;
  int sky_draw_calls = 0;
  int sky_triangles = 0;
  int world_draw_calls = 0;
  std::vector<u8> bgra;
  std::vector<float> depth;

  Pixel pixel(int x, int y) const {
    const std::size_t at = static_cast<std::size_t>(y * kTargetSize + x) * 4;
    return {bgra[at + 2], bgra[at + 1], bgra[at + 0]};
  }
  float depth_at(int x, int y) const { return depth[y * kTargetSize + x]; }
};

Frame render(Gpu& gpu,
             SkyLayout layout,
             const metal_renderer::ViewTransform& transform,
             bool immersive_camera_enabled,
             const WorldControl& world = {}) {
  Frame frame;
  gpu.stream.reset();
  const SkyChain chain = make_sky_chain(layout);

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
  // A sentinel instead of the production far clear distinguishes "sky wrote far depth 0" from
  // "sky was clipped".
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
  id<MTLRenderCommandEncoder> encoder = ctx.enc;

  MetalSharedRenderState state;
  state.version = GameVersion::Jak1;
  state.next_bucket = chain.next_bucket;
  state.default_regs_buffer = chain.default_regs;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;
  state.view_transform = transform;
  state.immersive_camera_enabled = immersive_camera_enabled;

  DmaFollower dma(chain.bytes.data(), 0, chain.bytes.size());
  MetalSkyRenderer sky("synthetic-jak1-sky", 3);
  sky.render(dma, &state, ctx);
  ASSERT(dma.current_tag_offset() == chain.next_bucket);
  ASSERT(ctx.enc == encoder);
  frame.sky_draw_calls = sky.direct_stats().draw_calls;
  frame.sky_triangles = sky.direct_stats().triangles;

  if (world.enabled) {
    // A later World-space direct draw on the same encoder, after the sky bucket returned.
    MetalDirectRenderer world_renderer("synthetic-world-control", 5, 100,
                                       metal_renderer::StereoSpace::World);
    world_renderer.reset_state();
    std::vector<Qw> gif = sky_setup_gif();
    world_renderer.render_gif(reinterpret_cast<const u8*>(gif.data()),
                              static_cast<u32>(gif.size() * 16), &state, ctx);
    gif = quad2d_gif(gs_x(-0.5f), gs_y(-0.5f), gs_x(0.5f), gs_y(0.5f), world.gs_z, kWorldControl);
    world_renderer.render_gif(reinterpret_cast<const u8*>(gif.data()),
                              static_cast<u32>(gif.size() * 16), &state, ctx);
    world_renderer.flush_pending(&state, ctx);
    frame.world_draw_calls = world_renderer.stats().draw_calls;
  }
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
  if (commands.status != MTLCommandBufferStatusCompleted) {
    if (commands.error) {
      std::printf("  Metal command-buffer error: %s\n",
                  commands.error.localizedDescription.UTF8String);
    }
    return frame;
  }

  frame.bgra.resize(kTargetSize * kTargetSize * 4);
  std::memcpy(frame.bgra.data(), color_readback.contents, frame.bgra.size());
  frame.depth.resize(kTargetSize * kTargetSize);
  std::memcpy(frame.depth.data(), depth_readback.contents, frame.depth.size() * sizeof(float));
  frame.ok = true;
  return frame;
}

void expect_sky(const Frame& frame, int x, int y, const Pixel& color, const std::string& label) {
  const Pixel actual = frame.pixel(x, y);
  const float depth = frame.depth_at(x, y);
  check(pixel_near(actual, color),
        label + ": pixel (" + std::to_string(x) + "," + std::to_string(y) + ") is " +
            describe(actual) + ", expected sky " + describe(color) +
            (pixel_near(actual, kClear) ? " (sky far-clipped: clear colour shows)" : ""));
  check(depth == 0.f,
        label + ": depth " + std::to_string(depth) + " at sky pixel, expected reverse-Z far 0");
}

void expect_clipped(const Frame& frame, int x, int y, const std::string& label) {
  check(pixel_near(frame.pixel(x, y), kClear),
        label + ": expected clipped sky (clear colour), got " + describe(frame.pixel(x, y)));
  check(frame.depth_at(x, y) == kDepthSentinel,
        label + ": expected untouched depth sentinel, got " + std::to_string(frame.depth_at(x, y)));
}

void begin_case(const char* name) {
  std::printf("case: %s\n", name);
}

}  // namespace

int main() {
  Gpu gpu;
  gpu.device = MTLCreateSystemDefaultDevice();
  ASSERT(gpu.device);
  gpu.queue = [gpu.device newCommandQueue];
  ASSERT(gpu.queue);
  dispatch_data_t library_data = dispatch_data_create(g_goalpad_metallib, g_goalpad_metallib_size,
                                                      nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError* library_error = nil;
  id<MTLLibrary> library = [gpu.device newLibraryWithData:library_data error:&library_error];
  if (!library && library_error) {
    std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
  }
  ASSERT(library);
  ASSERT(gpu.pso_cache.init(gpu.device, library));
  gpu.sampler_cache.init(gpu.device);
  gpu.stream.init(gpu.device);

  const auto centre_eye = derive_eye(metal_renderer::kIdentityMatrix4);
  const metal_renderer::ViewTransform identity;
  constexpr int kMid = kTargetSize / 2;

  // Preconditions from the production derivation: the GS z = 0 sky is beyond the eye far plane,
  // and the eye keeps the game's left/right handedness.
  {
    const auto sky = eye_clip(centre_eye, game_clip(0.f, 0.f, 0.f));
    check(sky[3] > 0.f && sky[2] < 0.f, "precondition: GS z 0 sky must lie beyond the eye far");
    const auto left = eye_pixel(centre_eye, game_clip(-0.5f, 0.f, 0.f));
    const auto right = eye_pixel(centre_eye, game_clip(0.5f, 0.f, 0.f));
    check(left[0] < kMid && right[0] > kMid, "precondition: game left/right map to eye left/right");
  }

  // 1. The failing regression: sky-tng dome envelope in a physical-immersive eye.
  begin_case("sky-tng dome, physical immersive centre eye");
  {
    const auto frame = render(gpu, SkyLayout::SkyTng, centre_eye, true);
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      check(frame.sky_draw_calls >= 1 && frame.sky_triangles == 4,
            "sky geometry was submitted (" + std::to_string(frame.sky_triangles) + " triangles)");
      const auto left = eye_pixel(centre_eye, game_clip(-0.5f, 0.f, 0.f));
      const auto right = eye_pixel(centre_eye, game_clip(0.5f, 0.f, 0.f));
      expect_sky(frame, left[0], left[1], kSkyLeft, "dome left half");
      expect_sky(frame, right[0], right[1], kSkyRight, "dome right half");
      expect_sky(frame, 1, 1, kSkyLeft, "dome top-left corner");
      expect_sky(frame, kTargetSize - 2, kTargetSize - 2, kSkyRight, "dome bottom-right corner");
    }
  }

  // 2. The same failure through the screen-gradient fallback (drawable.gc / draw-quad2d).
  begin_case("screen-gradient fallback, physical immersive centre eye");
  {
    const auto frame = render(gpu, SkyLayout::ScreenGradient, centre_eye, true);
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      check(frame.sky_draw_calls >= 1 && frame.sky_triangles == 2, "gradient was submitted");
      expect_sky(frame, kMid, kMid, kGradient, "gradient centre");
    }
  }

  // 3. Comfort/windowed identity: unchanged sky output and far depth 0.
  begin_case("Comfort identity control");
  {
    const auto frame = render(gpu, SkyLayout::SkyTng, identity, false);
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      expect_sky(frame, kMid / 2, kMid, kSkyLeft, "Comfort left");
      expect_sky(frame, kMid + kMid / 2, kMid, kSkyRight, "Comfort right");
    }
  }

  // 4. Scope: a far-clipping transform without the physical-immersive flag keeps Metal's Clip.
  begin_case("non-physical transform keeps depth clipping");
  {
    const auto frame = render(gpu, SkyLayout::SkyTng, centre_eye, false);
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      expect_clipped(frame, kMid / 2, kMid, "non-physical left");
      expect_clipped(frame, kMid + kMid / 2, kMid, "non-physical right");
    }
  }

  // 5. Eye pose: the sky follows each eye's orientation and ignores small head translation.
  struct PoseCase {
    const char* name;
    float right_meters;
    float yaw;
    bool expect_shift;
  };
  constexpr PoseCase poses[] = {
      {"eye translated 0.065 m", 0.065f, 0.f, false},
      {"eye translated 0.065 m and yawed 10 degrees", 0.065f, 10.f * kPi / 180.f, true},
      {"eye yawed -10 degrees", 0.f, -10.f * kPi / 180.f, true},
  };
  for (const auto& pose : poses) {
    begin_case(pose.name);
    const auto transform = derive_eye(eye_from_game_camera(pose.right_meters, pose.yaw));
    const auto boundary = eye_pixel(transform, game_clip(0.f, 0.f, 0.f));
    if (pose.expect_shift) {
      check(std::abs(boundary[0] - kMid) >= 4,
            "yaw moved the forward direction by " + std::to_string(boundary[0] - kMid) + " px");
    } else {
      check(std::abs(boundary[0] - kMid) <= 1, "translation alone left the sky boundary in place");
    }
    const auto frame = render(gpu, SkyLayout::SkyTng, transform, true);
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      expect_sky(frame, boundary[0] - 4, kMid, kSkyLeft, "left of the forward direction");
      expect_sky(frame, boundary[0] + 4, kMid, kSkyRight, "right of the forward direction");
    }
  }

  // 6. The clamp is restored: a genuinely out-of-frustum World draw after the sky stays clipped,
  //    while the same draw inside the frustum renders.
  constexpr u32 kWorldBeyondFar = 200;   // ~8 km: inside Jak's 10 km, beyond the 5 km eye
  constexpr u32 kWorldInside = 1000000;  // ~4 m
  const auto probe = eye_pixel(centre_eye, game_clip(-0.25f, 0.f, 0.f));
  {
    const auto beyond = eye_clip(centre_eye, game_clip(-0.25f, 0.f, kWorldBeyondFar));
    const auto inside = eye_clip(centre_eye, game_clip(-0.25f, 0.f, kWorldInside));
    check(beyond[3] > 0.f && beyond[2] < 0.f, "precondition: control draw is beyond the eye far");
    check(inside[3] > 0.f && inside[2] > 0.f && inside[2] < inside[3],
          "precondition: inside control draw is within the eye depth range");
  }
  begin_case("out-of-frustum World draw after sky stays clipped");
  {
    const auto frame = render(gpu, SkyLayout::SkyTng, centre_eye, true, {true, kWorldBeyondFar});
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      check(frame.world_draw_calls == 1, "control draw was submitted");
      check(!pixel_near(frame.pixel(probe[0], probe[1]), kWorldControl),
            "depth clamp leaked past the sky bucket: out-of-frustum World draw rendered");
      expect_sky(frame, probe[0], probe[1], kSkyLeft, "sky under the clipped World draw");
    }
  }
  begin_case("in-frustum World draw after sky renders");
  {
    const auto frame = render(gpu, SkyLayout::SkyTng, centre_eye, true, {true, kWorldInside});
    check(frame.ok, "command buffer completed");
    if (frame.ok) {
      check(pixel_near(frame.pixel(probe[0], probe[1]), kWorldControl),
            "in-frustum World control draw is visible, got " +
                describe(frame.pixel(probe[0], probe[1])));
    }
  }

  if (g_failures) {
    std::printf("metal-jak1-sky-immersive-depth-test: FAIL (%d checks)\n", g_failures);
    return 1;
  }
  std::puts("metal-jak1-sky-immersive-depth-test: PASS");
  return 0;
}
