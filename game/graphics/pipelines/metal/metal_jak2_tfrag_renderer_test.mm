#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_tfrag.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr char kLevelName[] = "tfrag-test";
constexpr u32 kTfragBucket = static_cast<u32>(jak2::BucketId::TFRAG_L0_TFRAG);
constexpr u32 kAlphaTfragBucket = static_cast<u32>(jak2::BucketId::TFRAG_T_L0_ALPHA);
constexpr u32 kWaterTfragBucket = static_cast<u32>(jak2::BucketId::TFRAG_W_L0_WATER);
static_assert(kTfragBucket == 8 && kAlphaTfragBucket == 128 && kWaterTfragBucket == 255);

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

u32 vif_code(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

u32 vif_stcycl(u16 cl, u16 wl) {
  return vif_code(VifCode::Kind::STCYCL, cl | (wl << 8));
}

u32 vif_unpack_v4_32(u8 qwc, u16 address) {
  return vif_code(VifCode::Kind::UNPACK_V4_32, address, qwc);
}

struct SyntheticTfragChain {
  std::vector<u8> bytes;
  u32 next_bucket = 0;

  void tag(DmaTag::Kind kind,
           u16 qwc,
           u32 address,
           u32 vif0,
           u32 vif1,
           const void* data = nullptr) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 16 + qwc * 16, 0);
    const u64 dma = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                    (static_cast<u64>(address) << 32);
    std::memcpy(bytes.data() + offset, &dma, sizeof(dma));
    std::memcpy(bytes.data() + offset + 8, &vif0, sizeof(vif0));
    std::memcpy(bytes.data() + offset + 12, &vif1, sizeof(vif1));
    if (data && qwc) {
      std::memcpy(bytes.data() + offset + 16, data, qwc * 16);
    }
  }
};

MetalTfragPcPortData make_pc_port_data() {
  MetalTfragPcPortData pc = {};
  std::memcpy(pc.level_name, kLevelName, sizeof(kLevelName));

  pc.camera.rot[0] = math::Vector4f(1.f, 0.f, 0.f, 0.f);
  pc.camera.rot[1] = math::Vector4f(0.f, 1.f, 0.f, 0.f);
  pc.camera.rot[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
  pc.camera.rot[3] = math::Vector4f(0.f, 0.f, 0.f, 1.f);
  pc.camera.perspective[0].x() = 256.f;
  pc.camera.perspective[1].y() = 208.f;
  pc.camera.perspective[2].w() = -1.f;
  pc.camera.hvdf_off.z() = 8388608.f;
  pc.camera.fog = math::Vector4f(1.f, 0.f, 255.f, 0.f);

  constexpr float kPackedPositionScale = (4096.f * 40.f) / 65535.f;
  pc.camera.trans = math::Vector4f(kPackedPositionScale * 2.f,
                                   kPackedPositionScale * 2.f, 0.f, 0.f);

  // One palette at weight 64 reproduces its packed values after the >> 6 interpolation.
  pc.camera.itimes[0][0] = 0x00400040;
  pc.camera.itimes[0][1] = 0x00400040;
  return pc;
}

SyntheticTfragChain make_tfrag_chain(bool malformed_setup) {
  SyntheticTfragChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);

  std::array<u8, 32> gs_setup = {};
  const u16 setup_qwc = malformed_setup ? 1 : 2;
  chain.tag(DmaTag::Kind::CNT, setup_qwc, 0, 0,
            vif_code(VifCode::Kind::DIRECT, 2), gs_setup.data());

  std::array<u8, 64> matrix = {};
  chain.tag(DmaTag::Kind::CNT, 4, 0, vif_stcycl(4, 4), vif_unpack_v4_32(4, 5),
            matrix.data());
  chain.tag(DmaTag::Kind::CNT, 4, 0, vif_stcycl(4, 4), vif_unpack_v4_32(4, 333),
            matrix.data());

  std::array<u8, 16> frame_data = {};
  chain.tag(DmaTag::Kind::CNT, 1, 0, 0, 0, frame_data.data());
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, vif_code(VifCode::Kind::MSCAL, 0));

  const auto pc = make_pc_port_data();
  static_assert((sizeof(pc) & 0xf) == 0);
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0,
            vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.tag(DmaTag::Kind::CNT, 0, 0, vif_code(VifCode::Kind::BASE, 0),
            vif_code(VifCode::Kind::OFFSET, 328));

  // The PC renderer consumes, but intentionally does not interpret, the VU geometry transfers.
  std::array<u8, 16> vu_geometry = {};
  chain.tag(DmaTag::Kind::CNT, 1, 0, vif_stcycl(4, 4), 0, vu_geometry.data());

  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

tfrag3::PackedTfragVertices::Vertex packed_vertex(u16 x, u16 y, s16 s, s16 t) {
  tfrag3::PackedTfragVertices::Vertex vertex = {};
  vertex.xoff = x;
  vertex.yoff = y;
  vertex.zoff = 4;
  vertex.cluster_idx = 0;
  vertex.s = s;
  vertex.t = t;
  vertex.color_index = 0;
  return vertex;
}

bool write_synthetic_fr3(const std::filesystem::path& path) {
  tfrag3::Level level;
  level.level_name = kLevelName;

  tfrag3::Texture texture = {};
  texture.w = 2;
  texture.h = 2;
  texture.data.assign(4, 0xff00ff00);
  texture.debug_name = "synthetic-green";
  texture.debug_tpage_name = "synthetic-tfrag";
  level.textures.push_back(std::move(texture));
  texture = {};
  texture.w = 2;
  texture.h = 2;
  texture.data = {0xff0000ff, 0x400000ff, 0xff0000ff, 0x400000ff};
  texture.debug_name = "synthetic-red";
  texture.debug_tpage_name = "synthetic-alpha";
  level.textures.push_back(std::move(texture));
  texture = {};
  texture.w = 2;
  texture.h = 2;
  texture.data.assign(4, 0xffff0000);
  texture.debug_name = "synthetic-blue";
  texture.debug_tpage_name = "synthetic-water";
  level.textures.push_back(std::move(texture));

  const auto make_tree = [](tfrag3::TFragmentTreeKind kind,
                            int texture_id,
                            DrawMode::AlphaBlend blend) {
    const bool translucent = blend != DrawMode::AlphaBlend::DISABLED;
    const bool water = kind == tfrag3::TFragmentTreeKind::WATER;
    tfrag3::TfragTree tree = {};
    tree.kind = kind;
    tree.use_strips = false;
    tree.packed_vertices.cluster_origins.emplace_back(300, 300, 300);
    tree.packed_vertices.vertices = {
        packed_vertex(0, 0, 0, 0),
        packed_vertex(4, 0, 1024, 0),
        packed_vertex(0, 4, 0, 1024),
        packed_vertex(4, 4, 1024, 1024),
    };

    tfrag3::StripDraw draw = {};
    draw.mode.as_int() = 0;
    draw.mode.set_depth_write_enable(true);
    draw.mode.set_zt(true);
    draw.mode.set_depth_test(GsTest::ZTest::GEQUAL);
    draw.mode.set_at(translucent);
    draw.mode.set_alpha_test(water ? DrawMode::AlphaTest::NEVER
                                   : DrawMode::AlphaTest::GEQUAL);
    draw.mode.set_aref(translucent && !water ? 0x7e : 0);
    draw.mode.set_alpha_fail(translucent ? GsTest::AlphaFail::FB_ONLY
                                         : GsTest::AlphaFail::KEEP);
    draw.mode.set_ab(translucent);
    draw.mode.set_alpha_blend(blend);
    draw.mode.set_fog(false);
    draw.mode.set_decal(false);
    draw.mode.set_filt_enable(false);
    draw.mode.set_clamp_s_enable(true);
    draw.mode.set_clamp_t_enable(true);
    draw.tree_tex_id = texture_id;
    draw.plain_indices = {0, 1, 2, 2, 1, 3};
    draw.vis_groups.push_back({6, 2, UINT16_MAX, 0});
    draw.num_triangles = 2;
    tree.draws.push_back(std::move(draw));

    tree.colors.color_count = 4;
    tree.colors.data.assign(128, 0);
    for (int color = 0; color < 4; color++) {
      tree.colors.data[color * 4 + 0] = 128;
      tree.colors.data[color * 4 + 1] = 128;
      tree.colors.data[color * 4 + 2] = 128;
      tree.colors.data[color * 4 + 3] = 64;
    }
    return tree;
  };
  level.tfrag_trees[0].push_back(
      make_tree(tfrag3::TFragmentTreeKind::NORMAL, 0, DrawMode::AlphaBlend::DISABLED));
  level.tfrag_trees[0].push_back(
      make_tree(tfrag3::TFragmentTreeKind::TRANS, 1, DrawMode::AlphaBlend::SRC_DST_SRC_DST));
  auto water_tree =
      make_tree(tfrag3::TFragmentTreeKind::WATER, 2, DrawMode::AlphaBlend::SRC_DST_SRC_DST);
  const auto& water_mode = water_tree.draws.front().mode;
  if (!water_mode.get_depth_write_enable() || !water_mode.get_zt_enable() ||
      water_mode.get_depth_test() != GsTest::ZTest::GEQUAL || !water_mode.get_at_enable() ||
      water_mode.get_alpha_test() != DrawMode::AlphaTest::NEVER ||
      water_mode.get_alpha_fail() != GsTest::AlphaFail::FB_ONLY ||
      !water_mode.get_ab_enable() ||
      water_mode.get_alpha_blend() != DrawMode::AlphaBlend::SRC_DST_SRC_DST) {
    return false;
  }
  level.tfrag_trees[0].push_back(std::move(water_tree));

  Serializer serializer;
  level.serialize(serializer);
  const auto serialized = serializer.get_save_result();
  const auto compressed = compression::compress_zstd(serialized.first, serialized.second);
  if (compressed.empty()) {
    return false;
  }
  file_util::write_binary_file(path, compressed.data(), compressed.size());
  return std::filesystem::exists(path);
}

struct RenderResult {
  MetalTFragment::Stats renderer;
  MetalBackgroundState background;
  int draw_calls = 0;
  int triangles = 0;
  bool finished_bucket = false;
  bool completed = false;
  std::vector<u8> pixels;
  std::vector<float> depths;
};

RenderResult render_chain(id<MTLDevice> device,
                          id<MTLCommandQueue> queue,
                          MetalPsoCache* pso_cache,
                          MetalSamplerCache* sampler_cache,
                          TexturePool* texture_pool,
                          MetalTFragment* renderer,
                          const SyntheticTfragChain& chain) {
  RenderResult result;

  auto* color_desc = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
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

  id<MTLCommandBuffer> commands = [queue commandBuffer];
  auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = color;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0.0;
  pass.stencilAttachment.texture = depth;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
  pass.stencilAttachment.clearStencil = 0;

  id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
  [encoder setCullMode:MTLCullModeNone];

  MetalStreamBuffer stream;
  stream.init(device);
  stream.reset();

  MetalFrameContext context;
  context.enc = encoder;
  context.pso_cache = pso_cache;
  context.sampler_cache = sampler_cache;
  context.stream = &stream;
  context.color_format = MTLPixelFormatRGBA8Unorm;
  context.depth_format = MTLPixelFormatDepth32Float_Stencil8;
  context.cmds = commands;
  context.game_color = color;
  context.game_depth = depth;

  MetalBackgroundState background;
  background.reset_frame();
  background.debug_all_visible = true;

  MetalSharedRenderState state;
  state.next_bucket = chain.next_bucket;
  state.texture_pool = texture_pool;
  state.background = &background;
  state.version = GameVersion::Jak2;
  state.fog_intensity = 0.f;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;

  DmaFollower dma(chain.bytes.data(), 0);
  renderer->render(dma, &state, context);
  result.finished_bucket = dma.current_tag_offset() == state.next_bucket;
  result.renderer = renderer->stats();
  result.background = background;
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;

  [encoder endEncoding];
  constexpr std::size_t kDepthBytesPerRow = kTargetSize * sizeof(float);
  id<MTLBuffer> depth_readback =
      [device newBufferWithLength:kDepthBytesPerRow * kTargetSize
                          options:MTLResourceStorageModeShared];
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit copyFromTexture:depth
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(kTargetSize, kTargetSize, 1)
               toBuffer:depth_readback
      destinationOffset:0
 destinationBytesPerRow:kDepthBytesPerRow
destinationBytesPerImage:kDepthBytesPerRow * kTargetSize
                options:MTLBlitOptionDepthFromDepthStencil];
#if TARGET_OS_OSX
  [blit synchronizeResource:color];
#endif
  [blit endEncoding];
  [commands commit];
  [commands waitUntilCompleted];
  result.completed = commands.status == MTLCommandBufferStatusCompleted;

  result.pixels.resize(kTargetSize * kTargetSize * 4);
  if (result.completed) {
    [color getBytes:result.pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
    result.depths.resize(kTargetSize * kTargetSize);
    std::memcpy(result.depths.data(), depth_readback.contents,
                result.depths.size() * sizeof(float));
  }
  return result;
}

bool rgba_is(const std::vector<u8>& pixels, int x, int y, u8 r, u8 g, u8 b, u8 a) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == r && pixels[offset + 1] == g && pixels[offset + 2] == b &&
         pixels[offset + 3] == a;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "a default Metal device is available");
    if (!device) {
      return 1;
    }

    id<MTLCommandQueue> queue = [device newCommandQueue];
    dispatch_data_t library_data = dispatch_data_create(
        g_goalpad_metallib, g_goalpad_metallib_size, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* library_error = nil;
    id<MTLLibrary> library = [device newLibraryWithData:library_data error:&library_error];
    check(queue != nil && library != nil, "initialized Metal and the embedded shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    check(pso_cache.init(device, library), "initialized the Metal pipeline cache");
    sampler_cache.init(device);

    TexturePool texture_pool(GameVersion::Jak2);
    const auto fixture_path =
        std::filesystem::temp_directory_path() / "goalpad-jak2-normal-tfrag-test.fr3";
    std::error_code remove_error;
    std::filesystem::remove(fixture_path, remove_error);
    check(write_synthetic_fr3(fixture_path),
          "created an asset-free FR3 with the extracted WATER draw-mode contract");

    std::string load_error;
    auto* level = metal_level_data::load_fr3(device, queue, texture_pool, fixture_path.string(),
                                             false, &load_error);
    std::filesystem::remove(fixture_path, remove_error);
    check(level != nullptr, "loaded the synthetic TFRAG through the production FR3 path");
    if (!level) {
      std::printf("FR3 load error: %s\n", load_error.c_str());
      return 1;
    }

    MetalTFragment renderer("tfrag-l0-tfrag", static_cast<int>(kTfragBucket),
                            {tfrag3::TFragmentTreeKind::NORMAL}, 0, false);

    const auto good = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                   &renderer, make_tfrag_chain(false));
    check(good.completed && good.finished_bucket,
          "the source-shaped normal TFRAG packet completed and consumed its bucket exactly");
    check(good.renderer.trees_rendered == 1 && good.renderer.draws == 1 &&
              good.renderer.runs == 1 && good.renderer.triangles == 2 &&
              good.draw_calls == 1 && good.triangles == 2,
          "one normal tree emits exactly one draw, one run, and two triangles");
    check(good.background.tfrag_draws == 1 && good.background.tfrag_tris == 2 &&
              good.background.unexpected_dma == 0 && good.background.missing_levels == 0 &&
              good.background.missing_textures == 0 && good.background.anim_slot_draws == 0,
          "normal TFRAG reports exact positive stats without DMA, level, texture, or animator "
          "gaps");

    int green_pixels = 0;
    int clear_pixels = 0;
    int unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (rgba_is(good.pixels, x, y, 0, 255, 0, 255)) {
          green_pixels++;
        } else if (rgba_is(good.pixels, x, y, 0, 0, 0, 0)) {
          clear_pixels++;
        } else {
          unexpected_pixels++;
        }
      }
    }
    if (green_pixels != 1024 || clear_pixels != 3072 || unexpected_pixels != 0) {
      const auto print_pixel = [&](int x, int y) {
        const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
        std::printf("pixel[%d,%d]=(%u,%u,%u,%u)\n", x, y, good.pixels[offset],
                    good.pixels[offset + 1], good.pixels[offset + 2], good.pixels[offset + 3]);
      };
      std::printf("readback counts: green=%d clear=%d unexpected=%d\n", green_pixels,
                  clear_pixels, unexpected_pixels);
      print_pixel(15, 15);
      print_pixel(16, 16);
      print_pixel(31, 31);
      print_pixel(32, 32);
      print_pixel(47, 47);
      print_pixel(48, 48);
    }
    check(rgba_is(good.pixels, 32, 32, 0, 255, 0, 255) &&
              rgba_is(good.pixels, 2, 2, 0, 0, 0, 0) && green_pixels == 1024 &&
              clear_pixels == 3072 && unexpected_pixels == 0,
          "readback is the exact centered 32x32 green mask over transparent black");
    check(good.depths[32 * kTargetSize + 32] > 0.f && good.depths[2 * kTargetSize + 2] == 0.f,
          "normal TFRAG writes depth only under its exact centered mask");

    MetalTFragment alpha_renderer("tfrag-t-l0-alpha", static_cast<int>(kAlphaTfragBucket),
                                  {tfrag3::TFragmentTreeKind::TRANS}, 0, false);
    const auto alpha = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                    &alpha_renderer, make_tfrag_chain(false));
    int opaque_red_pixels = 0;
    int low_alpha_red_pixels = 0;
    int alpha_clear_pixels = 0;
    int alpha_unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (rgba_is(alpha.pixels, x, y, 255, 0, 0, 255)) {
          opaque_red_pixels++;
        } else if (rgba_is(alpha.pixels, x, y, 64, 0, 0, 64)) {
          low_alpha_red_pixels++;
        } else if (rgba_is(alpha.pixels, x, y, 0, 0, 0, 0)) {
          alpha_clear_pixels++;
        } else {
          alpha_unexpected_pixels++;
        }
      }
    }
    check(alpha.completed && alpha.finished_bucket && alpha.renderer.trees_rendered == 1 &&
              alpha.renderer.draws == 2 && alpha.renderer.runs == 2 &&
              alpha.renderer.triangles == 2 && alpha.draw_calls == 2 && alpha.triangles == 2 &&
              alpha.background.tfrag_draws == 2 && alpha.background.tfrag_tris == 2 &&
              alpha.background.unexpected_dma == 0 &&
              alpha.background.missing_levels == 0 && alpha.background.missing_textures == 0,
          "the extracted TRANS mode encodes its alpha-pass and FB_ONLY draws exactly once");
    check(opaque_red_pixels == 512 && low_alpha_red_pixels == 512 &&
              alpha_clear_pixels == 3072 && alpha_unexpected_pixels == 0,
          "TRANS readback covers pixels above and below AREF across the exact centered mask");
    check(alpha.depths[32 * kTargetSize + 24] == 0.f &&
              alpha.depths[32 * kTargetSize + 40] > 0.f,
          "TRANS writes depth above AREF and preserves cleared depth for its FB_ONLY draw");

    MetalTFragment water_renderer("tfrag-w-l0-water", static_cast<int>(kWaterTfragBucket),
                                  {tfrag3::TFragmentTreeKind::WATER}, 0, false);
    const auto water = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                    &water_renderer, make_tfrag_chain(false));
    int blue_pixels = 0;
    int water_clear_pixels = 0;
    int water_unexpected_pixels = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        if (rgba_is(water.pixels, x, y, 0, 0, 255, 255)) {
          blue_pixels++;
        } else if (rgba_is(water.pixels, x, y, 0, 0, 0, 0)) {
          water_clear_pixels++;
        } else {
          water_unexpected_pixels++;
        }
      }
    }
    check(water.completed && water.finished_bucket && water.renderer.trees_rendered == 1 &&
              water.renderer.draws == 1 && water.renderer.runs == 1 &&
              water.renderer.triangles == 2 && water.draw_calls == 1 && water.triangles == 2 &&
              water.background.unexpected_dma == 0 && water.background.missing_levels == 0 &&
              water.background.missing_textures == 0,
          "the source-shaped water TFRAG packet selects one WATER tree exactly");
    check(blue_pixels == 1024 && water_clear_pixels == 3072 &&
              water_unexpected_pixels == 0,
          "WATER readback applies its extracted source-over mode to the exact mask");
    check(water.depths[32 * kTargetSize + 32] == 0.f,
          "the water FR3 draw mode preserves cleared depth under its visible mask");

    const auto malformed = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, make_tfrag_chain(true));
    int malformed_clear = 0;
    for (int y = 0; y < kTargetSize; y++) {
      for (int x = 0; x < kTargetSize; x++) {
        malformed_clear += rgba_is(malformed.pixels, x, y, 0, 0, 0, 0);
      }
    }
    check(malformed.completed && malformed.finished_bucket && malformed.draw_calls == 0 &&
              malformed.triangles == 0 && malformed.renderer.draws == 0 &&
              malformed.renderer.triangles == 0 && malformed.background.unexpected_dma == 1,
          "a malformed setup fails closed, consumes the bucket, and records no draw");
    check(malformed_clear == kTargetSize * kTargetSize,
          "the malformed packet leaves the exact all-clear pixel mask");

    check(metal_level_data::unload(texture_pool, kLevelName),
          "released the synthetic level after GPU completion");

    if (failures) {
      std::printf("FAIL: %d Jak II normal TFRAG renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II normal TFRAG rendered an exact asset-free Metal fixture\n");
    return 0;
  }
}
