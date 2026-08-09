#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fmt/format.h"

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/dma.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/pipelines/metal/metal_merc.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr char kLevelName[] = "jak2-merc-gpu-test";
constexpr char kNormalModelName[] = "jak2-merc-normal-model";
constexpr char kFilteredModelName[] = "jak2-merc-filtered-model";
constexpr char kAlphaModelName[] = "jak2-merc-alpha-model";
constexpr char kWaterModelName[] = "jak2-merc-water-model";
constexpr u32 kMercBucket = static_cast<u32>(jak2::BucketId::MERC_L0_TFRAG);
constexpr u32 kMercAlphaBucket = static_cast<u32>(jak2::BucketId::MERC_L0_ALPHA);
constexpr u32 kMercWaterBucket = static_cast<u32>(jak2::BucketId::MERC_L0_WATER);
constexpr u32 kMercCommonWaterBucket = static_cast<u32>(jak2::BucketId::MERC_LCOM_WATER);
constexpr u32 kOpening = 0x100;
constexpr u32 kBoundary = 0x200;
constexpr u32 kSetup = 0x400;
constexpr u32 kGsSetup = kSetup + 16 + 10 * 16;
constexpr u32 kSetupPatch = kGsSetup + 16 + 3 * 16;
constexpr u32 kModel = 0x800;
constexpr u32 kTerminal = 0xc00;
constexpr u32 kBone0 = 0x3000;
constexpr u32 kBone1 = 0x3080;
constexpr std::size_t kMemorySize = 0x4000;
constexpr float kMercZ = 8388608.f;

static_assert(kMercBucket == 14);

int failures = 0;

constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a) {
  return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
         (static_cast<u32>(a) << 24);
}

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

u32 vif(VifCode::Kind kind, u16 immediate = 0, u8 num = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

void put_u32(std::vector<u8>* memory, std::size_t offset, u32 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_u64(std::vector<u8>* memory, std::size_t offset, u64 value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_float(std::vector<u8>* memory, std::size_t offset, float value) {
  std::memcpy(memory->data() + offset, &value, sizeof(value));
}

void put_tag(std::vector<u8>* memory,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 tag = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  put_u64(memory, offset, tag);
  put_u32(memory, offset + 8, vif0);
  put_u32(memory, offset + 12, vif1);
}

void append_u32(std::vector<u8>* data, u32 value) {
  const auto offset = data->size();
  data->resize(offset + sizeof(value));
  put_u32(data, offset, value);
}

void append_u64(std::vector<u8>* data, u64 value) {
  const auto offset = data->size();
  data->resize(offset + sizeof(value));
  put_u64(data, offset, value);
}

std::vector<u8> make_setup() {
  std::vector<u8> data(10 * 16, 0);
  put_u32(&data, 0, vif(VifCode::Kind::BASE, 442));
  put_u32(&data, 4, vif(VifCode::Kind::OFFSET, static_cast<u16>(-442)));
  put_u32(&data, 12, vif(VifCode::Kind::UNPACK_V4_32, 0, 8));

  constexpr std::size_t kLowMemory = 16;
  constexpr std::size_t kPerspective = kLowMemory + 48;
  for (int column = 0; column < 4; column++) {
    for (int row = 0; row < 4; row++) {
      put_float(&data, kPerspective + (column * 4 + row) * sizeof(float),
                column == row ? 1.f : 0.f);
    }
  }
  constexpr std::size_t kFog = kLowMemory + 112;
  put_float(&data, kFog, 1.f);
  put_float(&data, kFog + 8, 255.f);

  put_u32(&data, 9 * 16, vif(VifCode::Kind::FLUSHE));
  put_u32(&data, 9 * 16 + 12, vif(VifCode::Kind::MSCAL));
  return data;
}

std::vector<u8> make_model_packet(const char* model_name) {
  std::vector<u8> data(128, 0);
  const auto model_name_size = std::strlen(model_name) + 1;
  ASSERT(model_name_size <= data.size());
  std::memcpy(data.data(), model_name, model_name_size);
  data.resize(data.size() + 7 * 16, 0);
  constexpr std::size_t kAmbient = 128 + 6 * 16;
  for (int lane = 0; lane < 4; lane++) {
    put_float(&data, kAmbient + lane * sizeof(float), 1.f);
  }

  const auto matrix_slots = data.size();
  data.resize(data.size() + 128, 0xff);
  data[matrix_slots] = 0;
  data[matrix_slots + 1] = 1;

  append_u32(&data, kBone0);
  data.resize(data.size() + 12, 0);
  append_u32(&data, kBone1);
  data.resize(data.size() + 12, 0);

  append_u64(&data, 1);
  append_u64(&data, 0);
  data.push_back(1);
  data.push_back(0);
  data.resize(data.size() + 14, 0);
  data.resize(data.size() + 16, 0);
  data.resize(data.size() + 16, 0);
  return data;
}

void write_bone(std::vector<u8>* memory, u32 address, float x, float y) {
  float matrix[7 * 4] = {};
  matrix[0] = -1.f;
  matrix[5] = -1.f;
  matrix[10] = -1.f;
  matrix[12] = -x;
  matrix[13] = -y;
  matrix[15] = -1.f;
  matrix[16] = 1.f;
  matrix[21] = 1.f;
  matrix[26] = 1.f;
  std::memcpy(memory->data() + address, matrix, sizeof(matrix));
}

std::vector<u8> make_source_chain(const char* model_name) {
  std::vector<u8> memory(kMemorySize, 0);
  put_tag(&memory, kOpening, DmaTag::Kind::NEXT, 0, kSetup, 0, 0);

  const auto setup = make_setup();
  put_tag(&memory, kSetup, DmaTag::Kind::CNT, 10, 0, vif(VifCode::Kind::STCYCL, 0x404),
          vif(VifCode::Kind::STMOD));
  std::memcpy(memory.data() + kSetup + 16, setup.data(), setup.size());
  put_tag(&memory, kGsSetup, DmaTag::Kind::CNT, 3, 0, 0,
          static_cast<u32>(VifCode::Kind::DIRECT) << 24 | 3);
  put_tag(&memory, kSetupPatch, DmaTag::Kind::NEXT, 0, kModel, 0, 0);

  const auto packet = make_model_packet(model_name);
  put_tag(&memory, kModel, DmaTag::Kind::CNT, static_cast<u16>(packet.size() / 16), 0, 0,
          static_cast<u32>(VifCode::Kind::PC_PORT) << 24);
  std::memcpy(memory.data() + kModel + 16, packet.data(), packet.size());
  const u32 model_patch = kModel + 16 + static_cast<u32>(packet.size());
  put_tag(&memory, model_patch, DmaTag::Kind::NEXT, 0, kTerminal, 0, 0);
  put_tag(&memory, kTerminal, DmaTag::Kind::NEXT, 0, kBoundary, 0, 0);

  write_bone(&memory, kBone0, 2048.f, 2048.f);
  write_bone(&memory, kBone1, 2048.f, 2048.f);
  return memory;
}

std::unique_ptr<tfrag3::Level> make_level() {
  auto level = std::make_unique<tfrag3::Level>();
  level->level_name = kLevelName;

  tfrag3::Texture texture;
  texture.w = 16;
  texture.h = 16;
  texture.debug_name = "jak2-merc-filter-alpha";
  texture.debug_tpage_name = "jak2-merc-test";
  texture.load_to_pool = false;
  texture.data.resize(16 * 16);
  for (int y = 0; y < 16; y++) {
    for (int x = 0; x < 16; x++) {
      const u8 r = x < 8 ? 96 : 16;
      const u8 g = y < 8 ? 16 : 96;
      const u8 b = (x < 8) == (y < 8) ? 16 : 96;
      texture.data[y * 16 + x] = rgba(r, g, b, 32);
    }
  }
  level->textures.push_back(std::move(texture));

  auto& merc = level->merc_data;
  merc.vertices.resize(4);
  const float xs[4] = {-64.f, 64.f, -64.f, 64.f};
  const float ys[4] = {-64.f, -64.f, 64.f, 64.f};
  for (int i = 0; i < 4; i++) {
    auto& vertex = merc.vertices[i];
    std::memset(&vertex, 0, sizeof(vertex));
    vertex.pos[0] = xs[i];
    vertex.pos[1] = ys[i];
    vertex.pos[2] = kMercZ;
    vertex.normal[2] = 1.f;
    vertex.weights[0] = 1.f;
    vertex.st[0] = 0.5f;
    vertex.st[1] = 0.5f;
    vertex.rgba[0] = 255;
    vertex.rgba[1] = 255;
    vertex.rgba[2] = 255;
    vertex.rgba[3] = 255;
  }
  merc.indices = {0, 1, 2, 3};

  const auto add_model =
      [&merc](const char* name, bool alpha_blend, bool depth_write, bool filtered) {
    tfrag3::MercDraw draw;
    draw.mode.set_depth_write_enable(depth_write);
    draw.mode.set_zt(true);
    draw.mode.set_depth_test(GsTest::ZTest::GEQUAL);
    draw.mode.set_ab(alpha_blend);
    draw.mode.set_alpha_blend(alpha_blend ? DrawMode::AlphaBlend::SRC_DST_SRC_DST
                                         : DrawMode::AlphaBlend::DISABLED);
    draw.mode.set_at(false);
    draw.mode.set_fog(false);
    draw.mode.set_decal(false);
    draw.mode.set_filt_enable(filtered);
    draw.mode.set_clamp_s_enable(true);
    draw.mode.set_clamp_t_enable(true);
    draw.tree_tex_id = 0;
    draw.first_index = 0;
    draw.index_count = 4;
    draw.num_triangles = 2;

    tfrag3::MercEffect effect;
    effect.all_draws.push_back(draw);
    effect.envmap_texture = 0;
    tfrag3::MercModel model;
    model.name = name;
    model.effects.push_back(std::move(effect));
    model.max_draws = 1;
    model.max_bones = 1;
    model.st_vif_add = 0;
    model.xyz_scale = 1.f;
    model.st_magic = 0.f;
    merc.models.push_back(std::move(model));
  };
  // extract_merc.cpp maps category 3 to alpha blend, category 4 to alpha blend without depth
  // writes, and preserves each shader's TEX1 MMAG filter choice in the extracted DrawMode.
  add_model(kNormalModelName, false, true, false);
  add_model(kFilteredModelName, false, true, true);
  add_model(kAlphaModelName, true, true, true);
  add_model(kWaterModelName, true, false, true);
  return level;
}

struct RenderResult {
  MetalMerc2::Stats stats;
  int draw_calls = 0;
  int triangles = 0;
  u32 final_offset = 0;
  bool completed = false;
  std::vector<u8> pixels;
  std::vector<float> depths;
};

RenderResult render(id<MTLDevice> device,
                    id<MTLCommandQueue> queue,
                    MetalPsoCache* pso_cache,
                    MetalSamplerCache* sampler_cache,
                    TexturePool* texture_pool,
                    MetalMercBucketRenderer* renderer,
                    std::vector<u8>* memory,
                    u64 frame_id) {
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
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 0;
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

  MetalSharedRenderState state;
  state.version = GameVersion::Jak2;
  state.texture_pool = texture_pool;
  state.dma_copy_base = memory->data();
  state.dma_copy_size = memory->size();
  state.ee_memory = memory->data();
  state.next_bucket = kBoundary;
  state.engine_frame_id = frame_id;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;

  DmaFollower dma(memory->data(), kOpening, memory->size());
  renderer->render(dma, &state, context);
  result.stats = renderer->stats();
  result.draw_calls = context.draw_calls;
  result.triangles = context.triangles;
  result.final_offset = dma.current_tag_offset();

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

int count_non_black(const std::vector<u8>& pixels) {
  int count = 0;
  for (std::size_t offset = 0; offset + 2 < pixels.size(); offset += 4) {
    count += pixels[offset] || pixels[offset + 1] || pixels[offset + 2];
  }
  return count;
}

struct Pixel {
  u8 r = 0;
  u8 g = 0;
  u8 b = 0;
  u8 a = 0;

  bool operator==(const Pixel& other) const {
    return r == other.r && g == other.g && b == other.b && a == other.a;
  }

  bool operator!=(const Pixel& other) const { return !(*this == other); }
};

Pixel center_pixel(const std::vector<u8>& pixels) {
  const std::size_t offset = (kTargetSize / 2 * kTargetSize + kTargetSize / 2) * 4;
  return {pixels.at(offset), pixels.at(offset + 1), pixels.at(offset + 2),
          pixels.at(offset + 3)};
}

int count_written_depth(const std::vector<float>& depths) {
  int count = 0;
  for (float depth : depths) {
    count += depth != 0.f;
  }
  return count;
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
    check(metal_setup_placeholder(device, queue, texture_pool),
          "published the Metal placeholder texture");
    const u64 placeholder = texture_pool.get_placeholder_texture();

    auto shared = std::make_shared<MetalMerc2>(device, queue, &texture_pool);
    MetalMercBucketRenderer normal_renderer("merc-l0-tfrag", static_cast<int>(kMercBucket),
                                            shared);
    MetalMercBucketRenderer alpha_renderer("merc-l0-alpha", static_cast<int>(kMercAlphaBucket),
                                           shared);
    MetalMercBucketRenderer water_renderer("merc-l0-water", static_cast<int>(kMercWaterBucket),
                                           shared);
    MetalMercBucketRenderer common_water_renderer(
        "merc-lcom-water", static_cast<int>(kMercCommonWaterBucket), shared);
    MetalMercModelPool::LoadResult load;
    std::string load_error;
    check(metal_merc_models().add_level(make_level(), false, &load, &load_error) &&
              load.level_name == kLevelName && load.models == 4 && load.vertices == 4 &&
              load.indices == 4,
          "registered normal, filtered, alpha, and water synthetic Merc models");
    if (failures) {
      if (!load_error.empty()) {
        std::printf("Merc load error: %s\n", load_error.c_str());
      }
      if (placeholder) {
        metal_texture_release(placeholder);
      }
      return 1;
    }

    const auto source_mode = [](const char* model_name) -> const DrawMode* {
      const auto model = metal_merc_models().get_merc_model(model_name);
      if (!model || model->model->effects.size() != 1 ||
          model->model->effects.front().all_draws.size() != 1) {
        return nullptr;
      }
      return &model->model->effects.front().all_draws.front().mode;
    };
    const DrawMode* normal_mode = source_mode(kNormalModelName);
    const DrawMode* filtered_mode = source_mode(kFilteredModelName);
    const DrawMode* alpha_mode = source_mode(kAlphaModelName);
    const DrawMode* water_mode = source_mode(kWaterModelName);
    check(normal_mode && !normal_mode->get_ab_enable() && normal_mode->get_depth_write_enable() &&
              !normal_mode->get_filt_enable() && filtered_mode &&
              !filtered_mode->get_ab_enable() && filtered_mode->get_depth_write_enable() &&
              filtered_mode->get_filt_enable(),
          "synthetic source draws preserve independent nearest and filtered opaque modes");
    check(alpha_mode && alpha_mode->get_ab_enable() && alpha_mode->get_depth_write_enable() &&
              alpha_mode->get_filt_enable() &&
              alpha_mode->get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST &&
              water_mode && water_mode->get_ab_enable() &&
              !water_mode->get_depth_write_enable() && water_mode->get_filt_enable() &&
              water_mode->get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST,
          "alpha and water source categories differ only in their depth-write contract");

    auto positive_memory = make_source_chain(kNormalModelName);
    const auto positive = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                 &normal_renderer, &positive_memory, 1);
    check(positive.completed && positive.final_offset == kBoundary &&
              positive.stats.models == 1 && positive.stats.draws == 1 &&
              positive.stats.triangles == 2 && positive.draw_calls == 1 &&
              positive.triangles == 2 && positive.stats.malformed_dma == 0 &&
              positive.stats.missing_models == 0 && positive.stats.bad_bone_pointers == 0 &&
              positive.stats.missing_bone_slots == 0 &&
              positive.stats.nonfinite_bone_matrices == 0 &&
              positive.stats.degenerate_bone_matrices == 0 &&
              positive.stats.incoherent_bone_sources == 0,
          "one source-shaped Jak 2 Merc packet draws one two-triangle model without gaps");
    check(count_non_black(positive.pixels) > 0,
          "the source-shaped Jak 2 Merc model produces non-black GPU pixels");

    auto filtered_memory = make_source_chain(kFilteredModelName);
    const auto filtered = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                 &normal_renderer, &filtered_memory, 2);

    struct VariantResult {
      const char* name;
      RenderResult result;
    };
    auto alpha_memory = make_source_chain(kAlphaModelName);
    auto water_memory = make_source_chain(kWaterModelName);
    auto common_water_memory = make_source_chain(kWaterModelName);
    std::array<VariantResult, 3> variants = {{
        {"alpha", render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                         &alpha_renderer, &alpha_memory, 3)},
        {"per-level water", render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                   &water_renderer, &water_memory, 4)},
        {"common water", render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                &common_water_renderer, &common_water_memory, 5)},
    }};
    for (const auto& [name, result] : variants) {
      const bool rendered = result.completed && result.final_offset == kBoundary &&
                            result.stats.models == 1 && result.stats.draws == 1 &&
                            result.stats.triangles == 2 && result.draw_calls == 1 &&
                            result.triangles == 2 && result.stats.malformed_dma == 0 &&
                            count_non_black(result.pixels) > 0;
      check(rendered,
            fmt::format("the {} Merc bucket renders its source category draw mode", name)
                .c_str());
    }

    const Pixel nearest_pixel = center_pixel(positive.pixels);
    const Pixel filtered_pixel = center_pixel(filtered.pixels);
    const Pixel alpha_pixel = center_pixel(variants[0].result.pixels);
    const Pixel water_pixel = center_pixel(variants[1].result.pixels);
    const Pixel common_water_pixel = center_pixel(variants[2].result.pixels);
    check(filtered.completed && filtered.final_offset == kBoundary &&
              filtered.stats.models == 1 && filtered.stats.draws == 1 &&
              filtered.draw_calls == 1 && nearest_pixel != filtered_pixel,
          "a patterned source texture visibly distinguishes nearest and filtered Merc draws");
    check(alpha_pixel.r > 0 && alpha_pixel.g > 0 && alpha_pixel.b > 0 &&
              alpha_pixel.r < filtered_pixel.r && alpha_pixel.g < filtered_pixel.g &&
              alpha_pixel.b < filtered_pixel.b,
          "partial source texture alpha visibly blends alpha Merc over the cleared target");
    check(alpha_pixel == water_pixel && water_pixel == common_water_pixel,
          "alpha and water variants retain identical filtered color output");

    const int filtered_depth = count_written_depth(filtered.depths);
    const int alpha_depth = count_written_depth(variants[0].result.depths);
    const int water_depth = count_written_depth(variants[1].result.depths);
    const int common_water_depth = count_written_depth(variants[2].result.depths);
    check(filtered_depth > 0 && alpha_depth == filtered_depth,
          "alpha Merc writes depth across the same covered pixels as opaque Merc");
    check(water_depth == 0 && common_water_depth == 0,
          "per-level and common water Merc preserve the cleared depth attachment");

    auto malformed_memory = make_source_chain(kAlphaModelName);
    put_tag(&malformed_memory, kModel, DmaTag::Kind::CNT, 0xffff, 0, 0,
            static_cast<u32>(VifCode::Kind::PC_PORT) << 24);
    const auto malformed = render(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                  &alpha_renderer, &malformed_memory, 6);
    check(malformed.completed && malformed.final_offset == kBoundary &&
              malformed.stats.malformed_dma == 1 && malformed.stats.models == 0 &&
              malformed.stats.draws == 0 && malformed.stats.triangles == 0 &&
              malformed.draw_calls == 0 && malformed.triangles == 0,
          "malformed Jak 2 Merc DMA recovers directly without publishing models or draws");
    check(count_non_black(malformed.pixels) == 0,
          "malformed Jak 2 Merc DMA leaves the cleared GPU target untouched");

    check(metal_merc_models().remove_level(kLevelName),
          "released the synthetic Merc level after GPU completion");
    if (placeholder) {
      metal_texture_release(placeholder);
    }
    if (failures) {
      std::printf("FAIL: %d Jak 2 Merc category renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak 2 normal, alpha, and water Merc buckets rendered asset-free fixtures\n");
    return 0;
  }
}
