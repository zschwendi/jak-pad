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
#include "game/graphics/pipelines/metal/metal_shrub.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr char kLevelName[] = "shrub-test";
constexpr char kLeftProto[] = "left-proto";
constexpr char kRightProto[] = "right-proto";
constexpr u32 kShrubBucket = static_cast<u32>(jak2::BucketId::SHRUB_L0_SHRUB);
constexpr std::size_t kLiveMaskBytes = 160;
static_assert(kShrubBucket == 74);
static_assert(sizeof(MetalTfragPcPortData) + kLiveMaskBytes == 560);

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

u32 vif_code(VifCode::Kind kind) {
  return static_cast<u32>(kind) << 24;
}

struct SyntheticShrubChain {
  std::vector<u8> bytes;
  u32 next_bucket = 0;
  std::size_t live_payload_bytes = 0;

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
  pc.camera.itimes[0][0] = 0x00400040;
  pc.camera.itimes[0][1] = 0x00400040;
  return pc;
}

std::vector<u8> hidden_left_mask() {
  std::vector<u8> mask(kLiveMaskBytes, 0);
  std::size_t offset = 0;
  const auto append = [&](const char* name, std::size_t* cursor) {
    const std::size_t size = std::strlen(name) + 1;
    std::memcpy(mask.data() + *cursor, name, size);
    *cursor += size;
  };
  append(kLeftProto, &offset);
  append("unknown-proto", &offset);
  return mask;
}

SyntheticShrubChain make_shrub_chain(const std::vector<u8>& mask) {
  SyntheticShrubChain chain;
  check((mask.size() & 0xf) == 0, "the synthetic shrub mask is qword aligned");
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);

  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.tag(DmaTag::Kind::CNT, mask.size() / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT),
            mask.empty() ? nullptr : mask.data());
  chain.live_payload_bytes = sizeof(pc) + mask.size();

  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_real_layout_shrub_chain(const std::vector<u8>& mask) {
  SyntheticShrubChain chain;
  check((mask.size() & 0xf) == 0, "the real-layout shrub mask is qword aligned");

  constexpr u32 kNextBucketEntry = 16;
  constexpr u32 kPayloadOffset = 32;
  chain.tag(DmaTag::Kind::NEXT, 0, kPayloadOffset, 0, 0);
  chain.next_bucket = kNextBucketEntry;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);

  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.tag(DmaTag::Kind::CNT, mask.size() / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT),
            mask.empty() ? nullptr : mask.data());
  chain.live_payload_bytes = sizeof(pc) + mask.size();
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  return chain;
}

SyntheticShrubChain make_jak1_shrub_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);
  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_truncated_shrub_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);
  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.next_bucket = static_cast<u32>(chain.bytes.size());
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_ref_pc_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);
  chain.tag(DmaTag::Kind::REF, 0, 0, 0, vif_code(VifCode::Kind::PC_PORT));
  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_ref_opening_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::REF, 0, 0, 0, 0);
  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_next_mask_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);
  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);
  chain.next_bucket = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0,
            vif_code(VifCode::Kind::PC_PORT));
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticShrubChain make_call_mask_chain() {
  SyntheticShrubChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 16, 0, 0);
  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, sizeof(pc) / 16, 0, 0, vif_code(VifCode::Kind::PC_PORT), &pc);

  const u32 call_offset = static_cast<u32>(chain.bytes.size());
  const u32 return_offset = call_offset + 16;
  const u32 subroutine_offset = return_offset + 16;
  chain.next_bucket = subroutine_offset + 16;
  chain.tag(DmaTag::Kind::CALL, 0, subroutine_offset, 0,
            vif_code(VifCode::Kind::PC_PORT));
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  chain.tag(DmaTag::Kind::RET, 0, 0, 0, 0);
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

tfrag3::PackedShrubVertices::Vertex shrub_vertex(float x,
                                                  float y,
                                                  u8 r,
                                                  u8 g,
                                                  u8 b,
                                                  float s,
                                                  float t) {
  tfrag3::PackedShrubVertices::Vertex vertex = {};
  vertex.x = x;
  vertex.y = y;
  vertex.z = 10.f;
  vertex.s = s;
  vertex.t = t;
  vertex.rgba[0] = r;
  vertex.rgba[1] = g;
  vertex.rgba[2] = b;
  return vertex;
}

tfrag3::ShrubDraw shrub_draw(u32 first_index, u16 proto_idx) {
  tfrag3::ShrubDraw draw = {};
  draw.mode.as_int() = 0;
  draw.mode.set_depth_write_enable(true);
  draw.mode.set_zt(true);
  draw.mode.set_depth_test(GsTest::ZTest::GEQUAL);
  draw.mode.set_at(false);
  draw.mode.set_ab(false);
  draw.mode.set_alpha_blend(DrawMode::AlphaBlend::DISABLED);
  draw.mode.set_fog(false);
  draw.mode.set_decal(false);
  draw.mode.set_filt_enable(false);
  draw.mode.set_clamp_s_enable(true);
  draw.mode.set_clamp_t_enable(true);
  draw.tree_tex_id = 0;
  draw.first_index_index = first_index;
  draw.num_indices = 4;
  draw.num_triangles = 2;
  draw.proto_idx = proto_idx;
  return draw;
}

bool write_synthetic_fr3(const std::filesystem::path& path) {
  tfrag3::Level level;
  level.level_name = kLevelName;

  tfrag3::Texture texture = {};
  texture.w = 2;
  texture.h = 2;
  texture.data.assign(4, 0xffffffff);
  texture.debug_name = "synthetic-white";
  texture.debug_tpage_name = "synthetic-shrub";
  level.textures.push_back(std::move(texture));

  std::array<math::Vector4f, 4> identity = {
      math::Vector4f(1.f, 0.f, 0.f, 0.f), math::Vector4f(0.f, 1.f, 0.f, 0.f),
      math::Vector4f(0.f, 0.f, 1.f, 0.f), math::Vector4f(0.f, 0.f, 0.f, 1.f)};

  tfrag3::ShrubTree left_tree = {};
  left_tree.packed_vertices.matrices.push_back(identity);
  left_tree.packed_vertices.vertices = {
      shrub_vertex(5.f, -4.f, 255, 0, 0, 0.f, 4096.f),
      shrub_vertex(1.f, -4.f, 255, 0, 0, 4096.f, 4096.f),
      shrub_vertex(5.f, 4.f, 255, 0, 0, 0.f, 0.f),
      shrub_vertex(1.f, 4.f, 255, 0, 0, 4096.f, 0.f),
  };
  left_tree.packed_vertices.instance_groups.push_back({0, 0, 4, 0});
  left_tree.packed_vertices.total_vertex_count = 4;
  left_tree.indices = {0, 1, 2, 3};
  left_tree.static_draws.push_back(shrub_draw(0, 0));
  left_tree.has_per_proto_visibility_toggle = true;
  left_tree.proto_names = {kLeftProto};

  tfrag3::ShrubTree right_tree = {};
  right_tree.packed_vertices.matrices.push_back(identity);
  right_tree.packed_vertices.vertices = {
      shrub_vertex(-1.f, -4.f, 0, 0, 255, 0.f, 4096.f),
      shrub_vertex(-5.f, -4.f, 0, 0, 255, 4096.f, 4096.f),
      shrub_vertex(-1.f, 4.f, 0, 0, 255, 0.f, 0.f),
      shrub_vertex(-5.f, 4.f, 0, 0, 255, 4096.f, 0.f),
  };
  right_tree.packed_vertices.instance_groups.push_back({0, 0, 4, 0});
  right_tree.packed_vertices.total_vertex_count = 4;
  right_tree.indices = {0, 1, 2, 3};
  right_tree.static_draws.push_back(shrub_draw(0, 0));
  right_tree.has_per_proto_visibility_toggle = true;
  right_tree.proto_names = {kRightProto};

  for (auto* tree : {&left_tree, &right_tree}) {
    tree->time_of_day_colors.color_count = 4;
    tree->time_of_day_colors.data.assign(128, 0);
    for (int color = 0; color < 4; color++) {
      for (int channel = 0; channel < 4; channel++) {
        tree->time_of_day_colors.data[color * 4 + channel] = 64;
      }
    }
  }
  level.shrub_trees.push_back(std::move(left_tree));
  level.shrub_trees.push_back(std::move(right_tree));

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
  MetalShrub::Stats renderer;
  MetalBackgroundState background;
  int draw_calls = 0;
  int triangles = 0;
  bool finished_bucket = false;
  u32 final_dma_offset = 0;
  bool completed = false;
  std::vector<u8> pixels;
};

RenderResult render_chain(id<MTLDevice> device,
                          id<MTLCommandQueue> queue,
                          MetalPsoCache* pso_cache,
                          MetalSamplerCache* sampler_cache,
                          TexturePool* texture_pool,
                          MetalShrub* renderer,
                          const SyntheticShrubChain& chain,
                          GameVersion version = GameVersion::Jak2) {
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
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
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
  MetalSharedRenderState state;
  state.next_bucket = chain.next_bucket;
  state.texture_pool = texture_pool;
  state.background = &background;
  state.version = version;
  state.fog_intensity = 0.f;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;

  DmaFollower dma(chain.bytes.data(), 0);
  renderer->render(dma, &state, context);
  result.final_dma_offset = dma.current_tag_offset();
  result.finished_bucket = dma.current_tag_offset() == state.next_bucket;
  result.renderer = renderer->stats();
  result.background = background;
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;

  [encoder endEncoding];
#if TARGET_OS_OSX
  id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
  [blit synchronizeResource:color];
  [blit endEncoding];
#endif
  [commands commit];
  [commands waitUntilCompleted];
  result.completed = commands.status == MTLCommandBufferStatusCompleted;
  result.pixels.resize(kTargetSize * kTargetSize * 4);
  if (result.completed) {
    [color getBytes:result.pixels.data()
        bytesPerRow:kTargetSize * 4
         fromRegion:MTLRegionMake2D(0, 0, kTargetSize, kTargetSize)
        mipmapLevel:0];
  }
  return result;
}

bool rgba_is(const std::vector<u8>& pixels, int x, int y, u8 r, u8 g, u8 b, u8 a) {
  const std::size_t offset = static_cast<std::size_t>(y * kTargetSize + x) * 4;
  return pixels[offset] == r && pixels[offset + 1] == g && pixels[offset + 2] == b &&
         pixels[offset + 3] == a;
}

bool rejected_before_camera(const RenderResult& result) {
  return result.completed && result.finished_bucket && result.renderer.trees_rendered == 0 &&
         result.renderer.draws == 0 && result.renderer.triangles == 0 &&
         result.draw_calls == 0 && result.triangles == 0 &&
         result.background.unexpected_dma == 1 &&
         result.background.camera_trace.packet_count() == 0 &&
         result.background.render_camera_trace.packet_count() == 0;
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
        std::filesystem::temp_directory_path() / "goalpad-jak2-normal-shrub-test.fr3";
    std::error_code remove_error;
    std::filesystem::remove(fixture_path, remove_error);
    check(write_synthetic_fr3(fixture_path), "created an asset-free synthetic normal SHRUB FR3");

    std::string load_error;
    auto* level = metal_level_data::load_fr3(device, queue, texture_pool, fixture_path.string(),
                                             false, &load_error);
    std::filesystem::remove(fixture_path, remove_error);
    check(level != nullptr, "loaded the synthetic SHRUB through the production FR3 path");
    if (!level) {
      std::printf("FR3 load error: %s\n", load_error.c_str());
      return 1;
    }

    MetalShrub renderer("shrub-l0-shrub", static_cast<int>(kShrubBucket));

    const auto hidden_chain = make_real_layout_shrub_chain(hidden_left_mask());
    check(hidden_chain.next_bucket == 16 && hidden_chain.live_payload_bytes == 560,
          "the real-layout packet keeps bucket entry 1 at 16 and carries 400 plus 160 bytes later");
    const auto hidden = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                     &renderer, hidden_chain);
    check(hidden.completed && hidden.finished_bucket && hidden.final_dma_offset == 16 &&
              hidden.renderer.trees_rendered == 2 &&
              hidden.renderer.draws == 1 && hidden.renderer.triangles == 2 &&
              hidden.draw_calls == 1 && hidden.triangles == 2,
          "the 160-byte mask hides one tree-local proto and draws the other tree exactly once");
    check(hidden.background.shrub_draws == 1 && hidden.background.shrub_tris == 2 &&
              hidden.background.unexpected_dma == 0 && hidden.background.missing_levels == 0 &&
              hidden.background.missing_textures == 0 &&
              hidden.background.camera_trace.packet_count() == 1 &&
              hidden.background.render_camera_trace.packet_count() == 1,
          "the live-shaped hidden-proto packet reports exact positive SHRUB stats");
    check(rgba_is(hidden.pixels, 20, 32, 0, 0, 0, 0) &&
              rgba_is(hidden.pixels, 44, 32, 0, 0, 255, 255),
          "the hidden left proto is clear while the unknown name leaves the right proto blue");

    const auto zero_chain = make_real_layout_shrub_chain({});
    check(zero_chain.next_bucket == 16 &&
              zero_chain.live_payload_bytes == sizeof(MetalTfragPcPortData),
          "the real-layout zero-qword mask remains a PC_PORT transfer after the bucket table");
    const auto zero = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                   &renderer, zero_chain);
    check(zero.completed && zero.finished_bucket && zero.final_dma_offset == 16 &&
              zero.renderer.trees_rendered == 2 &&
              zero.renderer.draws == 2 && zero.renderer.triangles == 4 && zero.draw_calls == 2 &&
              zero.triangles == 4 && zero.background.unexpected_dma == 0,
          "a zero-qword mask draws both tree-local named proto regions");
    check(rgba_is(zero.pixels, 20, 32, 255, 0, 0, 255) &&
              rgba_is(zero.pixels, 44, 32, 0, 0, 255, 255),
          "the zero mask renders the left red and right blue proto regions");

    const auto malformed_chain = make_shrub_chain(std::vector<u8>(kLiveMaskBytes, 'x'));
    const auto malformed = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, malformed_chain);
    check(malformed.completed && malformed.finished_bucket && malformed.renderer.draws == 0 &&
              malformed.renderer.triangles == 0 && malformed.draw_calls == 0 &&
              malformed.triangles == 0 && malformed.background.unexpected_dma == 1 &&
              malformed.background.camera_trace.packet_count() == 0 &&
              malformed.background.render_camera_trace.packet_count() == 0,
          "an unterminated nonzero mask consumes the bucket, draws nothing, and records one gap");
    check(rgba_is(malformed.pixels, 20, 32, 0, 0, 0, 0) &&
              rgba_is(malformed.pixels, 44, 32, 0, 0, 0, 0),
          "the malformed name transfer leaves both proto regions clear");

    const auto truncated = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, make_truncated_shrub_chain());
    check(rejected_before_camera(truncated),
          "a valid PC block truncated at the next-bucket boundary rejects once without advancing");

    const auto ref_opening = render_chain(device, queue, &pso_cache, &sampler_cache,
                                          &texture_pool, &renderer, make_ref_opening_chain());
    check(rejected_before_camera(ref_opening),
          "a REF-shaped opening is rejected before changing expected traversal or camera state");
    const auto ref_pc = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                     &renderer, make_ref_pc_chain());
    check(rejected_before_camera(ref_pc),
          "a REF-shaped PC block is rejected before changing expected traversal or camera state");
    const auto next_mask = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, make_next_mask_chain());
    check(rejected_before_camera(next_mask),
          "a NEXT-shaped mask is rejected at the bounded pre-read tag check");
    const auto call_mask = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, make_call_mask_chain());
    check(rejected_before_camera(call_mask),
          "a CALL-shaped mask is rejected while recovery still lands on the bucket boundary");

    auto& later_draw = level->level->shrub_trees.at(1).static_draws.at(0);
    later_draw.proto_idx = 1;
    const auto bad_index = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &renderer, zero_chain);
    later_draw.proto_idx = 0;
    check(bad_index.completed && bad_index.finished_bucket &&
              bad_index.renderer.trees_rendered == 0 && bad_index.draw_calls == 0 &&
              bad_index.triangles == 0 && bad_index.background.shrub_draws == 0 &&
              bad_index.background.unexpected_dma == 1,
          "a later-tree out-of-range proto_idx preflights before any earlier-tree draw");

    const auto jak1 = render_chain(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                   &renderer, make_jak1_shrub_chain(), GameVersion::Jak1);
    check(jak1.completed && jak1.finished_bucket && jak1.renderer.trees_rendered == 2 &&
              jak1.renderer.draws == 2 && jak1.renderer.triangles == 4 &&
              jak1.background.unexpected_dma == 0 &&
              jak1.background.camera_trace.packet_count() == 1 &&
              jak1.background.render_camera_trace.packet_count() == 1,
          "the separate Jak 1 path retains its bounded no-mask chain and draws both trees");

    check(metal_level_data::unload(texture_pool, kLevelName),
          "released the synthetic level after GPU completion");

    if (failures) {
      std::printf("FAIL: %d Jak II normal SHRUB renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II normal SHRUB rendered exact asset-free named-proto fixtures\n");
    return 0;
  }
}
