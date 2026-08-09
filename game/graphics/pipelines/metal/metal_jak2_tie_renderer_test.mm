#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
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
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/pipelines/metal/metal_tie.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>
#import <TargetConditionals.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr int kTargetSize = 64;
constexpr char kLevelName[] = "tie-test";
constexpr char kLeftProto[] = "left-proto";
constexpr char kRightProto[] = "right-proto";
constexpr u32 kTieBucket = static_cast<u32>(jak2::BucketId::TIE_L0_TFRAG);
constexpr u32 kEtieBucket = static_cast<u32>(jak2::BucketId::ETIE_L0_TFRAG);
constexpr u32 kAlphaTieBucket = static_cast<u32>(jak2::BucketId::TIE_T_L0_ALPHA);
constexpr u32 kAlphaEtieBucket = static_cast<u32>(jak2::BucketId::ETIE_T_L0_ALPHA);
constexpr u32 kWaterTieBucket = static_cast<u32>(jak2::BucketId::TIE_W_L0_WATER);
constexpr u32 kWaterEtieBucket = static_cast<u32>(jak2::BucketId::ETIE_W_L0_WATER);
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
static_assert(kTieBucket == 9 && kEtieBucket == 10 && kAlphaTieBucket == 129 &&
              kAlphaEtieBucket == 130 && kWaterTieBucket == 256 && kWaterEtieBucket == 257);

int failures = 0;

void check(bool condition, const char* what) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    failures++;
  }
}

u32 vif_code(VifCode::Kind kind,
             u16 immediate = 0,
             u8 num = 0,
             bool interrupt = false) {
  return (interrupt ? 0x80000000u : 0u) | (static_cast<u32>(kind) << 24) |
         (static_cast<u32>(num) << 16) | immediate;
}

struct SyntheticChain {
  std::vector<u8> bytes;
  u32 next_bucket = 0;
  u32 mscalf_offset = 0;
  u32 pc_offset = 0;
  u32 mask_offset = 0;
  u32 tint_offset = 0;

  u32 tag(DmaTag::Kind kind,
          u16 qwc,
          u32 address,
          u32 vif0,
          u32 vif1,
          const void* data = nullptr) {
    const u32 offset = static_cast<u32>(bytes.size());
    bytes.resize(offset + 16 + qwc * 16, 0);
    const u64 dma = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                    (static_cast<u64>(address) << 32);
    std::memcpy(bytes.data() + offset, &dma, sizeof(dma));
    std::memcpy(bytes.data() + offset + 8, &vif0, sizeof(vif0));
    std::memcpy(bytes.data() + offset + 12, &vif1, sizeof(vif1));
    if (data && qwc) {
      std::memcpy(bytes.data() + offset + 16, data, qwc * 16);
    }
    return offset;
  }

  void replace_tag(u32 offset, DmaTag::Kind kind, u16 qwc, u32 address) {
    const u64 dma = static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) |
                    (static_cast<u64>(address) << 32);
    std::memcpy(bytes.data() + offset, &dma, sizeof(dma));
  }

  void replace_vifs(u32 offset, u32 vif0, u32 vif1) {
    std::memcpy(bytes.data() + offset + 8, &vif0, sizeof(vif0));
    std::memcpy(bytes.data() + offset + 12, &vif1, sizeof(vif1));
  }
};

MetalTfragPcPortData make_pc_port_data() {
  MetalTfragPcPortData pc = {};
  std::memcpy(pc.level_name, kLevelName, sizeof(kLevelName));
  pc.camera.itimes[0][0] = 0x00400040;
  pc.camera.itimes[0][1] = 0x00400040;

  constexpr float angle = 0.31f;
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  pc.camera.rot[0] = math::Vector4f(c, s, 0.f, 0.f);
  pc.camera.rot[1] = math::Vector4f(-s, c, 0.f, 0.f);
  pc.camera.rot[2] = math::Vector4f(0.f, 0.f, 1.f, 0.f);
  pc.camera.rot[3] = math::Vector4f(19.f, -11.f, 0.4f, 1.f);
  pc.camera.trans = math::Vector4f(0.f, 0.f, 2.f, 0.f);
  pc.camera.camera[3].w() = 255.f;
  pc.camera.hvdf_off = math::Vector4f(2048.f, 2048.f, 11184810.f, 0.f);
  pc.camera.fog = math::Vector4f(3.25f, 0.f, 255.f, 0.f);
  pc.camera.perspective[0].x() = 1.8f;
  pc.camera.perspective[1].y() = 1.35f;
  pc.camera.perspective[2].z() = 70000.f;
  pc.camera.perspective[2].w() = 1.625f;
  pc.camera.perspective[3].z() = 16000.f;
  return pc;
}

std::vector<u8> proto_mask(std::initializer_list<const char*> names) {
  std::size_t bytes = 0;
  for (const char* name : names) {
    bytes += std::strlen(name) + 1;
  }
  bytes = (bytes + 15) & ~std::size_t(15);
  std::vector<u8> result(bytes, 0);
  std::size_t offset = 0;
  for (const char* name : names) {
    const std::size_t size = std::strlen(name) + 1;
    std::memcpy(result.data() + offset, name, size);
    offset += size;
  }
  return result;
}

void append_control(SyntheticChain* chain,
                    const std::vector<u8>& mask,
                    const std::array<float, 4>& tint,
                    u32 tail_address,
                    bool record_offsets) {
  const auto pc = make_pc_port_data();
  const u32 pc_offset = chain->tag(DmaTag::Kind::CNT, 25, 0, 0, kPcPortVif, &pc);
  const u32 mask_offset =
      chain->tag(DmaTag::Kind::CNT, mask.size() / 16, 0, 0, kPcPortVif,
                 mask.empty() ? nullptr : mask.data());
  const u32 tint_offset = chain->tag(DmaTag::Kind::CNT, 1, 0, 0, kPcPortVif, tint.data());
  chain->tag(DmaTag::Kind::NEXT, 0, tail_address, 0, 0);
  if (record_offsets) {
    chain->pc_offset = pc_offset;
    chain->mask_offset = mask_offset;
    chain->tint_offset = tint_offset;
  }
}

SyntheticChain make_parent_chain(const std::vector<u8>& first_mask,
                                 const std::array<float, 4>& first_tint,
                                 const std::optional<std::vector<u8>>& second_mask = std::nullopt) {
  SyntheticChain chain;
  constexpr u32 kBucketEntry = 16;
  constexpr u32 kSetupOffset = 32;
  chain.tag(DmaTag::Kind::NEXT, 0, kSetupOffset, 0, 0);
  chain.next_bucket = kBucketEntry;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);

  std::array<u8, 160> constants = {};
  chain.tag(DmaTag::Kind::CNT, 10, 0, vif_code(VifCode::Kind::STMOD),
            vif_code(VifCode::Kind::UNPACK_V4_32, 0x3c6, 10), constants.data());
  chain.mscalf_offset =
      chain.tag(DmaTag::Kind::CNT, 0, 0, vif_code(VifCode::Kind::MSCALF, 8),
                vif_code(VifCode::Kind::FLUSHA));
  std::array<u8, 32> row = {};
  chain.tag(DmaTag::Kind::CNT, 2, 0, 0, vif_code(VifCode::Kind::STROW), row.data());
  std::array<u8, 32> direct = {};
  chain.tag(DmaTag::Kind::CNT, 2, 0, 0, vif_code(VifCode::Kind::DIRECT, 2), direct.data());
  const u32 first_control = static_cast<u32>(chain.bytes.size() + 16);
  chain.tag(DmaTag::Kind::NEXT, 0, first_control, 0, 0);

  if (second_mask) {
    const u32 first_pc = static_cast<u32>(chain.bytes.size());
    const u32 first_bytes =
        16 + 25 * 16 + 16 + static_cast<u32>(first_mask.size()) + 32 + 16;
    const u32 second_control = first_pc + first_bytes;
    append_control(&chain, first_mask, first_tint, second_control, true);
    const std::array<float, 4> second_tint = {128.f, 128.f, 128.f, 128.f};
    append_control(&chain, *second_mask, second_tint, chain.next_bucket, false);
  } else {
    append_control(&chain, first_mask, first_tint, chain.next_bucket, true);
  }
  return chain;
}

SyntheticChain make_empty_bucket() {
  SyntheticChain chain;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  chain.next_bucket = 16;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  return chain;
}

SyntheticChain make_nonempty_child() {
  SyntheticChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 32, 0, 0);
  chain.next_bucket = 16;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  std::array<u8, 16> payload = {};
  chain.tag(DmaTag::Kind::CNT, 1, 0, 0, kPcPortVif, payload.data());
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  return chain;
}

SyntheticChain make_jak1_parent_chain() {
  SyntheticChain chain;
  chain.tag(DmaTag::Kind::NEXT, 0, 32, 0, 0);
  chain.next_bucket = 16;
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  std::array<u8, 160> gs = {};
  chain.tag(DmaTag::Kind::CNT, 10, 0, 0, 0, gs.data());
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  std::array<u8, 32> row = {};
  chain.tag(DmaTag::Kind::CNT, 2, 0, 0, 0, row.data());
  chain.tag(DmaTag::Kind::CNT, 0, 0, 0, 0);
  const auto pc = make_pc_port_data();
  chain.tag(DmaTag::Kind::CNT, 25, 0, 0, kPcPortVif, &pc);
  std::array<u8, 84 * 16> wind = {};
  chain.tag(DmaTag::Kind::CNT, 84, 0, 0, 0, wind.data());
  const std::array<float, 4> tint = {64.f, 64.f, 64.f, 128.f};
  chain.tag(DmaTag::Kind::CNT, 1, 0, 0, kPcPortVif, tint.data());
  chain.tag(DmaTag::Kind::NEXT, 0, chain.next_bucket, 0, 0);
  return chain;
}

tfrag3::PackedTieVertices::Vertex tie_vertex(float x,
                                              float y,
                                              u8 r,
                                              u8 g,
                                              u8 b) {
  tfrag3::PackedTieVertices::Vertex vertex = {};
  vertex.x = x;
  vertex.y = y;
  vertex.z = 1.f;
  vertex.s = 0.5f;
  vertex.t = 0.5f;
  vertex.nz = 127;
  vertex.r = r;
  vertex.g = g;
  vertex.b = b;
  vertex.a = 255;
  return vertex;
}

void add_quad(tfrag3::TieTree* tree,
              float min_x,
              float max_x,
              float min_y,
              float max_y,
              u16 color_index,
              u8 r,
              u8 g,
              u8 b,
              std::vector<u32>* indices) {
  const u32 first = static_cast<u32>(tree->packed_vertices.vertices.size());
  tree->packed_vertices.vertices.push_back(tie_vertex(min_x, min_y, r, g, b));
  tree->packed_vertices.vertices.push_back(tie_vertex(max_x, min_y, r, g, b));
  tree->packed_vertices.vertices.push_back(tie_vertex(min_x, max_y, r, g, b));
  tree->packed_vertices.vertices.push_back(tie_vertex(max_x, max_y, r, g, b));
  tree->packed_vertices.color_indices.insert(tree->packed_vertices.color_indices.end(), 4,
                                              color_index);
  *indices = {first, first + 1, first + 2, first + 2, first + 1, first + 3};
}

enum class SyntheticWaterPass { NONE, BASE, ENVMAP_SECOND };

tfrag3::StripDraw tie_draw(
    const std::vector<u32>& indices,
    std::vector<tfrag3::StripDraw::VisGroup> groups,
    bool envmap_second,
    bool translucent = false,
    SyntheticWaterPass water_pass = SyntheticWaterPass::NONE) {
  tfrag3::StripDraw draw = {};
  const bool water = water_pass != SyntheticWaterPass::NONE;
  draw.mode.as_int() = 0;
  draw.mode.set_depth_write_enable(water || (!envmap_second && !translucent));
  draw.mode.set_zt(true);
  draw.mode.set_depth_test(GsTest::ZTest::GEQUAL);
  draw.mode.set_ab(water || envmap_second || translucent);
  draw.mode.set_alpha_blend(
      water_pass == SyntheticWaterPass::ENVMAP_SECOND
          ? DrawMode::AlphaBlend::SRC_0_DST_DST
          : water ? DrawMode::AlphaBlend::SRC_DST_SRC_DST
                  : envmap_second ? DrawMode::AlphaBlend::SRC_0_FIX_DST
                                  : translucent ? DrawMode::AlphaBlend::SRC_DST_FIX_DST
                                                : DrawMode::AlphaBlend::DISABLED);
  draw.mode.set_at(water);
  draw.mode.set_alpha_test(water ? DrawMode::AlphaTest::NEVER
                                 : DrawMode::AlphaTest::GEQUAL);
  draw.mode.set_aref(0);
  draw.mode.set_alpha_fail(water ? GsTest::AlphaFail::FB_ONLY : GsTest::AlphaFail::KEEP);
  draw.mode.set_fog(false);
  draw.mode.set_decal(false);
  draw.mode.set_filt_enable(water_pass == SyntheticWaterPass::ENVMAP_SECOND);
  draw.mode.set_clamp_s_enable(true);
  draw.mode.set_clamp_t_enable(true);
  draw.tree_tex_id = 0;
  draw.plain_indices = indices;
  draw.vis_groups = std::move(groups);
  for (const auto& group : draw.vis_groups) {
    draw.num_triangles += group.num_tris;
  }
  return draw;
}

void set_tod_color(tfrag3::PackedTimeOfDay* colors,
                   int color,
                   u8 r,
                   u8 g,
                   u8 b,
                   u8 a) {
  for (int palette = 0; palette < 8; ++palette) {
    colors->read(color, palette, 0) = r;
    colors->read(color, palette, 1) = g;
    colors->read(color, palette, 2) = b;
    colors->read(color, palette, 3) = a;
  }
}

tfrag3::TieTree make_named_tree() {
  tfrag3::TieTree tree = {};
  tree.use_strips = false;
  tree.has_per_proto_visibility_toggle = true;
  tree.proto_names = {kLeftProto, kRightProto};

  std::vector<u32> left;
  std::vector<u32> right;
  std::vector<u32> env;
  std::vector<u32> trans;
  std::vector<u32> trans_env;
  std::vector<u32> water;
  std::vector<u32> water_env;
  add_quad(&tree, -64.f, -8.f, -32.f, 32.f, 0, 255, 255, 255, &left);
  add_quad(&tree, 8.f, 64.f, -32.f, 32.f, 1, 255, 255, 255, &right);
  add_quad(&tree, -16.f, 16.f, -16.f, 16.f, 2, 0, 255, 0, &env);
  add_quad(&tree, -6.f, 6.f, -28.f, 28.f, 3, 255, 255, 255, &trans);
  add_quad(&tree, -5.f, 5.f, -14.f, 14.f, 4, 255, 0, 0, &trans_env);
  add_quad(&tree, -6.f, 6.f, -28.f, 28.f, 5, 255, 255, 255, &water);
  add_quad(&tree, -5.f, 5.f, -14.f, 14.f, 6, 0, 0, 255, &water_env);

  std::vector<u32> normal = left;
  normal.insert(normal.end(), right.begin(), right.end());
  tree.static_draws.push_back(
      tie_draw(normal, {{6, 2, UINT16_MAX, 0}, {6, 2, UINT16_MAX, 1}}, false));
  tree.static_draws.push_back(tie_draw(trans, {{6, 2, UINT16_MAX, 1}}, false, true));
  tree.static_draws.push_back(tie_draw(water, {{6, 2, UINT16_MAX, 1}}, false, true,
                                       SyntheticWaterPass::BASE));
  tree.static_draws.push_back(tie_draw(env, {{6, 2, UINT16_MAX, 1}}, false));
  tree.static_draws.push_back(tie_draw(trans_env, {{6, 2, UINT16_MAX, 1}}, false, true));
  tree.static_draws.push_back(tie_draw(water_env, {{6, 2, UINT16_MAX, 1}}, false, true,
                                       SyntheticWaterPass::BASE));
  tree.static_draws.push_back(tie_draw(env, {{6, 2, UINT16_MAX, 1}}, true));
  tree.static_draws.push_back(tie_draw(trans_env, {{6, 2, UINT16_MAX, 1}}, true));
  tree.static_draws.push_back(tie_draw(water_env, {{6, 2, UINT16_MAX, 1}}, true, false,
                                       SyntheticWaterPass::ENVMAP_SECOND));
  tree.category_draw_indices = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  tree.packed_vertices.matrix_groups.push_back(
      {-1, 0, static_cast<u32>(tree.packed_vertices.vertices.size()), true});
  tree.colors.color_count = 8;
  tree.colors.data.assign(tree.colors.color_count * 8 * 4, 0);
  set_tod_color(&tree.colors, 0, 128, 0, 0, 64);
  set_tod_color(&tree.colors, 1, 0, 0, 128, 64);
  set_tod_color(&tree.colors, 2, 0, 0, 0, 64);
  set_tod_color(&tree.colors, 3, 0, 128, 0, 64);
  set_tod_color(&tree.colors, 4, 0, 0, 0, 64);
  set_tod_color(&tree.colors, 5, 0, 0, 128, 64);
  set_tod_color(&tree.colors, 6, 0, 0, 128, 64);
  return tree;
}

tfrag3::TieTree make_overlay_tree() {
  tfrag3::TieTree tree = {};
  tree.use_strips = false;
  std::vector<u32> env;
  add_quad(&tree, -16.f, 16.f, -16.f, 16.f, 0, 255, 0, 0, &env);
  tree.static_draws.push_back(tie_draw(env, {{6, 2, UINT16_MAX, 0}}, false));
  tree.static_draws.push_back(tie_draw(env, {{6, 2, UINT16_MAX, 0}}, true));
  tree.category_draw_indices = {0, 0, 0, 0, 1, 1, 1, 2, 2, 2};
  tree.packed_vertices.matrix_groups.push_back(
      {-1, 0, static_cast<u32>(tree.packed_vertices.vertices.size()), true});
  tree.colors.color_count = 4;
  tree.colors.data.assign(128, 0);
  set_tod_color(&tree.colors, 0, 0, 0, 0, 64);
  return tree;
}

bool write_synthetic_fr3(const std::filesystem::path& path) {
  tfrag3::Level level;
  level.level_name = kLevelName;
  tfrag3::Texture texture = {};
  texture.w = 2;
  texture.h = 2;
  texture.data.assign(4, 0xffffffff);
  texture.debug_name = "synthetic-white";
  texture.debug_tpage_name = "synthetic-tie";
  level.textures.push_back(std::move(texture));
  level.tie_trees[0].push_back(make_named_tree());
  level.tie_trees[0].push_back(make_overlay_tree());

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
  MetalTie3::Stats renderer;
  MetalBackgroundState background;
  int draw_calls = 0;
  int triangles = 0;
  u32 parent_final = 0;
  u32 child_final = 0;
  bool parent_finished = true;
  bool child_finished = true;
  bool completed = false;
  std::vector<u8> pixels;
  std::vector<float> depths;
};

RenderResult render_sequence(id<MTLDevice> device,
                             id<MTLCommandQueue> queue,
                             MetalPsoCache* pso_cache,
                             MetalSamplerCache* sampler_cache,
                             TexturePool* texture_pool,
                             MetalTie3* parent,
                             MetalTieCategory* child,
                             const SyntheticChain* parent_chain,
                             const SyntheticChain* child_chain,
                             u64 engine_frame_id,
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
  MetalSharedRenderState state;
  state.texture_pool = texture_pool;
  state.background = &background;
  state.version = version;
  state.engine_frame_id = engine_frame_id;
  state.fog_intensity = 0.f;
  state.game_res_w = kTargetSize;
  state.game_res_h = kTargetSize;

  if (parent_chain) {
    state.next_bucket = parent_chain->next_bucket;
    DmaFollower dma(parent_chain->bytes.data(), 0);
    parent->render(dma, &state, context);
    result.parent_final = dma.current_tag_offset();
    result.parent_finished = result.parent_final == state.next_bucket;
  }
  if (child_chain) {
    state.next_bucket = child_chain->next_bucket;
    DmaFollower dma(child_chain->bytes.data(), 0);
    child->render(dma, &state, context);
    result.child_final = dma.current_tag_offset();
    result.child_finished = result.child_final == state.next_bucket;
  }

  result.renderer = parent->stats();
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

int count_rgba(const std::vector<u8>& pixels, u8 r, u8 g, u8 b, u8 a) {
  int count = 0;
  for (std::size_t offset = 0; offset + 3 < pixels.size(); offset += 4) {
    count += pixels[offset] == r && pixels[offset + 1] == g && pixels[offset + 2] == b &&
             pixels[offset + 3] == a;
  }
  return count;
}

int count_non_black_rgb(const std::vector<u8>& pixels) {
  int count = 0;
  for (std::size_t offset = 0; offset + 2 < pixels.size(); offset += 4) {
    count += pixels[offset] != 0 || pixels[offset + 1] != 0 || pixels[offset + 2] != 0;
  }
  return count;
}

int count_clear_depth(const std::vector<float>& depths) {
  int count = 0;
  for (float depth : depths) {
    count += depth == 0.f;
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
    const u64 placeholder_handle = texture_pool.get_placeholder_texture();
    check(placeholder_handle != 0 && metal_texture_lookup(placeholder_handle) != nil,
          "the counted fallback resolves to a live placeholder texture");
    const auto source_contract_tree = make_named_tree();
    check(source_contract_tree.colors.color_count == 8 &&
              source_contract_tree.colors.data.size() ==
                  source_contract_tree.colors.color_count * 8 * 4,
          "the synthetic TIE palette follows the extractor's four-color padding contract");
    const GsAlpha water_alpha_override(68);
    check(water_alpha_override.a_mode() == GsAlpha::BlendMode::SOURCE &&
              water_alpha_override.b_mode() == GsAlpha::BlendMode::DEST &&
              water_alpha_override.c_mode() == GsAlpha::BlendMode::SOURCE &&
              water_alpha_override.d_mode() == GsAlpha::BlendMode::DEST,
          "Jak II water alpha override 68 maps exactly to SRC_DST_SRC_DST");
    const auto& water_mode = source_contract_tree.static_draws.at(2).mode;
    const auto& water_env_base_mode = source_contract_tree.static_draws.at(5).mode;
    const auto& water_env_second_mode = source_contract_tree.static_draws.at(8).mode;
    const auto water_test_mode_matches = [](const DrawMode& mode) {
      return mode.get_zt_enable() && mode.get_depth_test() == GsTest::ZTest::GEQUAL &&
             mode.get_depth_write_enable() && mode.get_at_enable() &&
             mode.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
             mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY && mode.get_ab_enable();
    };
    check(water_test_mode_matches(water_mode) && water_test_mode_matches(water_env_base_mode) &&
              water_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST &&
              water_env_base_mode.get_alpha_blend() ==
                  DrawMode::AlphaBlend::SRC_DST_SRC_DST,
          "water TIE and ETIE base draws retain raw depth-write with the extracted test and "
          "source-over mode");
    check(water_test_mode_matches(water_env_second_mode) &&
              water_env_second_mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_0_DST_DST &&
              water_env_second_mode.get_filt_enable(),
          "the water ETIE second draw retains raw depth-write with its extracted "
          "SRC_0_DST_DST filtered mode");
    const auto fixture_path =
        std::filesystem::temp_directory_path() / "goalpad-jak2-normal-tie-test.fr3";
    std::error_code remove_error;
    std::filesystem::remove(fixture_path, remove_error);
    check(write_synthetic_fr3(fixture_path), "created an asset-free synthetic normal TIE FR3");
    std::string load_error;
    auto* level = metal_level_data::load_fr3(device, queue, texture_pool, fixture_path.string(),
                                             false, &load_error);
    std::filesystem::remove(fixture_path, remove_error);
    check(level != nullptr, "loaded the synthetic TIE through the production FR3 path");
    if (!level) {
      std::printf("FR3 load error: %s\n", load_error.c_str());
      if (placeholder_handle) {
        metal_texture_release(placeholder_handle);
      }
      return 1;
    }

    MetalTie3 parent("tie-l0-tfrag", static_cast<int>(kTieBucket), 0);
    MetalTieCategory child("etie-l0-tfrag", static_cast<int>(kEtieBucket), &parent,
                           tfrag3::TieCategory::NORMAL_ENVMAP);
    MetalTieCategory alpha_child("tie-t-l0-alpha", static_cast<int>(kAlphaTieBucket), &parent,
                                 tfrag3::TieCategory::TRANS);
    MetalTieCategory alpha_env_child("etie-t-l0-alpha",
                                     static_cast<int>(kAlphaEtieBucket), &parent,
                                     tfrag3::TieCategory::TRANS_ENVMAP);
    MetalTieCategory water_child("tie-w-l0-water", static_cast<int>(kWaterTieBucket), &parent,
                                 tfrag3::TieCategory::WATER);
    MetalTieCategory water_env_child("etie-w-l0-water",
                                     static_cast<int>(kWaterEtieBucket), &parent,
                                     tfrag3::TieCategory::WATER_ENVMAP);
    const std::array<float, 4> half_red_tint = {64.f, 64.f, 64.f, 128.f};
    const auto empty_child = make_empty_bucket();
    const auto hidden_parent = make_parent_chain(proto_mask({kLeftProto}), half_red_tint);
    check(hidden_parent.next_bucket == 16 && hidden_parent.pc_offset > hidden_parent.next_bucket,
          "the populated parent uses the real out-of-line bucket-table layout");

    const auto hidden = render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &parent, &child, &hidden_parent, nullptr, 10);
    const int hidden_red = count_rgba(hidden.pixels, 255, 0, 0, 255);
    const int hidden_blue = count_rgba(hidden.pixels, 0, 0, 255, 255);
    check(hidden.completed && hidden.parent_finished && hidden.parent_final == 16 &&
              hidden.renderer.trees_rendered == 2 && hidden.renderer.draws == 1 &&
              hidden.renderer.runs == 1 && hidden.renderer.triangles == 2 &&
              hidden.draw_calls == 1 && hidden.triangles == 2,
          "a hidden named proto leaves exactly one normal draw, one run, and two triangles");
    check(hidden_red == 0 && hidden_blue > 100,
          "the parent mask suppresses the left red proto while preserving the right blue proto");
    check(hidden.background.tie_draws == 1 && hidden.background.tie_tris == 2 &&
              hidden.background.tie_envmap_second_draws == 0 &&
              hidden.background.unexpected_dma == 0 && hidden.background.missing_levels == 0 &&
              hidden.background.missing_textures == 0 && hidden.background.anim_slot_draws == 0 &&
              hidden.background.camera_trace.packet_count() == 1 &&
              hidden.background.render_camera_trace.packet_count() == 1,
          "the normal parent reports exact positive stats with zero missing-resource gaps");

    const auto all_parent = make_parent_chain({}, half_red_tint);
    const auto all = render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                     &parent, &child, &all_parent, nullptr, 11);
    check(all.completed && all.parent_finished && all.renderer.draws == 1 &&
              all.renderer.runs == 1 && all.renderer.triangles == 4 && all.draw_calls == 1 &&
              all.triangles == 4 && count_rgba(all.pixels, 255, 0, 0, 255) > 100 &&
              count_rgba(all.pixels, 0, 0, 255, 255) > 100,
          "a zero-qword mask renders both named normal regions in one exact draw");

    const auto paired = render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &parent, &child, &hidden_parent, &empty_child, 12);
    const int paired_half_red = count_rgba(paired.pixels, 128, 0, 0, 255);
    const int paired_half_yellow = count_rgba(paired.pixels, 128, 128, 0, 255);
    check(paired.completed && paired.parent_finished && paired.child_finished &&
              paired.parent_final == 16 && paired.child_final == 16 &&
              paired.renderer.trees_rendered == 2 && paired.renderer.draws == 5 &&
              paired.renderer.runs == 5 && paired.renderer.triangles == 10 &&
              paired.renderer.envmap_second_draws == 2 &&
              paired.renderer.envmap_second_triangles == 4 && paired.draw_calls == 5 &&
              paired.triangles == 10,
          "the empty ETIE child adds adjacent base/second passes for both trees exactly once");
    check(paired_half_red > 20 && paired_half_yellow == 0,
          "ETIE tree adjacency leaves only the last red shine and Jak 2 /128 tint stays half red");
    check(paired.background.tie_draws == 5 && paired.background.tie_tris == 10 &&
              paired.background.tie_envmap_second_draws == 2 &&
              paired.background.tie_envmap_second_tris == 4 &&
              paired.background.unexpected_dma == 0 && paired.background.missing_levels == 0 &&
              paired.background.missing_textures == 0 && paired.background.anim_slot_draws == 0,
          "the paired render reports exact normal/envmap stats and zero missing-resource gaps");

    const auto alpha_after_normal = render_sequence(
        device, queue, &pso_cache, &sampler_cache, &texture_pool, &parent, &alpha_child, nullptr,
        &empty_child, 12);
    check(alpha_after_normal.completed && alpha_after_normal.child_finished &&
              alpha_after_normal.draw_calls == 1 && alpha_after_normal.triangles == 2 &&
              alpha_after_normal.background.tie_draws == 1 &&
              alpha_after_normal.background.tie_tris == 2 &&
              alpha_after_normal.background.unexpected_dma == 0,
          "the later empty alpha TIE child reuses the same-frame normal parent state once");
    check(count_rgba(alpha_after_normal.pixels, 0, 128, 0, 255) > 20,
          "the alpha TIE child applies its half-blend FR3 draw mode on readback");

    const auto alpha_env_after_normal =
        render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool, &parent,
                        &alpha_env_child, nullptr, &empty_child, 12);
    check(alpha_env_after_normal.completed && alpha_env_after_normal.child_finished &&
              alpha_env_after_normal.draw_calls == 2 &&
              alpha_env_after_normal.triangles == 4 &&
              alpha_env_after_normal.background.tie_draws == 2 &&
              alpha_env_after_normal.background.tie_tris == 4 &&
              alpha_env_after_normal.background.tie_envmap_second_draws == 1 &&
              alpha_env_after_normal.background.tie_envmap_second_tris == 2 &&
              alpha_env_after_normal.background.unexpected_dma == 0,
          "the alpha ETIE child emits its adjacent TRANS envmap base and second draws");
    check(count_non_black_rgb(alpha_env_after_normal.pixels) > 20,
          "the alpha ETIE base/shine pair produces deterministic non-black readback");

    const auto water_after_normal = render_sequence(
        device, queue, &pso_cache, &sampler_cache, &texture_pool, &parent, &water_child, nullptr,
        &empty_child, 12);
    check(water_after_normal.completed && water_after_normal.child_finished &&
              water_after_normal.draw_calls == 1 && water_after_normal.triangles == 2 &&
              water_after_normal.background.tie_draws == 1 &&
              water_after_normal.background.tie_tris == 2 &&
              water_after_normal.background.unexpected_dma == 0,
          "the later empty water TIE child reuses the same-frame normal parent state once");
    const int water_blue = count_rgba(water_after_normal.pixels, 0, 0, 255, 255);
    check(water_blue > 20 && water_blue == count_non_black_rgb(water_after_normal.pixels),
          "the water TIE child produces only its source-derived full-blue color");
    check(count_clear_depth(water_after_normal.depths) == kTargetSize * kTargetSize,
          "the water TIE AT NEVER/FB_ONLY mode preserves cleared depth exactly");

    const auto water_env_after_normal =
        render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool, &parent,
                        &water_env_child, nullptr, &empty_child, 12);
    check(water_env_after_normal.completed && water_env_after_normal.child_finished &&
              water_env_after_normal.draw_calls == 2 &&
              water_env_after_normal.triangles == 4 &&
              water_env_after_normal.background.tie_draws == 2 &&
              water_env_after_normal.background.tie_tris == 4 &&
              water_env_after_normal.background.tie_envmap_second_draws == 1 &&
              water_env_after_normal.background.tie_envmap_second_tris == 2 &&
              water_env_after_normal.background.unexpected_dma == 0,
          "the water ETIE child emits its adjacent WATER envmap base and second draws");
    const int water_env_blue = count_rgba(water_env_after_normal.pixels, 0, 0, 255, 255);
    check(water_env_blue > 20 &&
              water_env_blue == count_non_black_rgb(water_env_after_normal.pixels),
          "the water ETIE base/shine pair preserves the source-derived full-blue result");
    check(count_clear_depth(water_env_after_normal.depths) == kTargetSize * kTargetSize,
          "both water ETIE passes preserve cleared depth exactly");

    const auto linked_parent =
        make_parent_chain(proto_mask({kLeftProto}), half_red_tint, proto_mask({kRightProto}));
    const auto linked = render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                        &parent, &child, &linked_parent, nullptr, 13);
    check(linked.completed && linked.parent_finished && linked.parent_final == 16 &&
              linked.renderer.draws == 1 && linked.renderer.triangles == 2 &&
              count_rgba(linked.pixels, 255, 0, 0, 255) == 0 &&
              count_rgba(linked.pixels, 0, 0, 255, 255) > 100 &&
              linked.background.unexpected_dma == 0,
          "a legal linked parent drains later controls while the first mask alone drives rendering");

    const auto legal_empty = make_empty_bucket();
    const auto empty_pair = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                            &texture_pool, &parent, &child, &legal_empty,
                                            &empty_child, 14);
    check(empty_pair.completed && empty_pair.parent_finished && empty_pair.child_finished &&
              empty_pair.draw_calls == 0 && empty_pair.triangles == 0 &&
              empty_pair.background.tie_draws == 0 &&
              empty_pair.background.unexpected_dma == 0 &&
              empty_pair.background.camera_trace.packet_count() == 0,
          "source-legal empty parent and child buckets consume exactly and draw nothing");

    auto unterminated = make_parent_chain(std::vector<u8>(16, 'x'), half_red_tint);
    const auto bad_names = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                           &texture_pool, &parent, &child, &unterminated,
                                           &empty_child, 15);
    check(bad_names.completed && bad_names.parent_finished && bad_names.child_finished &&
              bad_names.draw_calls == 0 && bad_names.triangles == 0 &&
              bad_names.background.tie_draws == 0 &&
              bad_names.background.unexpected_dma == 1 &&
              bad_names.background.camera_trace.packet_count() == 0,
          "an unterminated mask invalidates the parent and its following child fails closed");

    auto irq_setup = make_parent_chain({}, half_red_tint);
    irq_setup.replace_vifs(irq_setup.mscalf_offset,
                           vif_code(VifCode::Kind::MSCALF, 8, 0, true),
                           vif_code(VifCode::Kind::FLUSHA, 0, 0, true));
    const auto bad_irq = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                         &texture_pool, &parent, &child, &irq_setup, &empty_child,
                                         16);
    check(bad_irq.completed && bad_irq.parent_finished && bad_irq.child_finished &&
              bad_irq.draw_calls == 0 && bad_irq.background.unexpected_dma == 1,
          "IRQ-marked TIE setup VIF codes remain distinct from source :msk tags");

    for (int bad_field = 0; bad_field < 3; bad_field++) {
      auto wrong = make_parent_chain({}, half_red_tint);
      const u32 offset = bad_field == 0 ? wrong.pc_offset
                         : bad_field == 1 ? wrong.mask_offset
                                          : wrong.tint_offset;
      wrong.replace_tag(offset, DmaTag::Kind::NEXT, 0, wrong.next_bucket);
      const auto rejected = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                            &texture_pool, &parent, &child, &wrong, &empty_child,
                                            20 + bad_field);
      check(rejected.completed && rejected.parent_finished && rejected.child_finished &&
                rejected.draw_calls == 0 && rejected.background.unexpected_dma == 1,
            bad_field == 0 ? "a wrong PC tag kind rejects the parent before state commit"
            : bad_field == 1 ? "a wrong mask tag kind rejects the parent before state commit"
                             : "a wrong tint tag kind rejects the parent before state commit");
    }

    auto& normal_groups = level->level->tie_trees.at(0).at(0).static_draws.at(0).vis_groups;
    normal_groups.at(1).tie_proto_idx = 2;
    const auto bad_proto = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                           &texture_pool, &parent, &child, &all_parent,
                                           &empty_child, 30);
    normal_groups.at(1).tie_proto_idx = 1;
    check(bad_proto.completed && bad_proto.parent_finished && bad_proto.child_finished &&
              bad_proto.renderer.trees_rendered == 0 && bad_proto.draw_calls == 0 &&
              bad_proto.triangles == 0 && bad_proto.background.tie_draws == 0 &&
              bad_proto.background.unexpected_dma == 1,
          "an out-of-range proto index preflights before any normal or ETIE draw");

    const auto nonempty_child = make_nonempty_child();
    const auto rejected_child = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                                &texture_pool, &parent, &child, &hidden_parent,
                                                &nonempty_child, 31);
    check(rejected_child.completed && rejected_child.parent_finished &&
              rejected_child.child_finished && rejected_child.draw_calls == 1 &&
              rejected_child.triangles == 2 &&
              rejected_child.renderer.envmap_second_draws == 0 &&
              rejected_child.background.unexpected_dma == 1,
          "a nonempty ETIE child preserves the valid normal draw but emits no envmap draw");

    const auto absent_child = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                              &texture_pool, &parent, &child, nullptr,
                                              &empty_child, 32);
    check(absent_child.completed && absent_child.child_finished && absent_child.draw_calls == 0 &&
              absent_child.triangles == 0 && absent_child.background.tie_draws == 0 &&
              absent_child.background.unexpected_dma == 0,
          "an empty ETIE child without a parent state draws nothing");

    const auto leave_state = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                             &texture_pool, &parent, &child, &hidden_parent,
                                             nullptr, 40);
    const auto stale_child = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                             &texture_pool, &parent, &child, nullptr,
                                             &empty_child, 41);
    check(leave_state.draw_calls == 1 && stale_child.completed && stale_child.child_finished &&
              stale_child.draw_calls == 0 && stale_child.triangles == 0 &&
              stale_child.background.tie_draws == 0 &&
              stale_child.background.unexpected_dma == 0,
          "a legal child cannot reuse an otherwise valid parent state from a stale frame");

    const auto leave_same_frame = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                                  &texture_pool, &parent, &child, &hidden_parent,
                                                  nullptr, 50);
    auto malformed_same_frame = make_parent_chain({}, half_red_tint);
    malformed_same_frame.replace_tag(malformed_same_frame.mask_offset, DmaTag::Kind::NEXT, 0,
                                     malformed_same_frame.next_bucket);
    const auto after_malformed = render_sequence(device, queue, &pso_cache, &sampler_cache,
                                                 &texture_pool, &parent, &child,
                                                 &malformed_same_frame, &empty_child, 50);
    check(leave_same_frame.draw_calls == 1 && after_malformed.completed &&
              after_malformed.parent_finished && after_malformed.child_finished &&
              after_malformed.draw_calls == 0 && after_malformed.triangles == 0 &&
              after_malformed.background.tie_draws == 0 &&
              after_malformed.background.unexpected_dma == 1,
          "a malformed parent invalidates stale same-frame state before the child dispatch");

    const auto jak1_chain = make_jak1_parent_chain();
    const auto jak1 = render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool,
                                      &parent, &child, &jak1_chain, nullptr, 60,
                                      GameVersion::Jak1);
    check(jak1.completed && jak1.parent_finished && jak1.draw_calls == 5 &&
              jak1.renderer.draws == 5 && jak1.renderer.envmap_second_draws == 2 &&
              jak1.background.unexpected_dma == 0,
          "the independent Jak 1 parent path retains normal and paired envmap rendering");

    auto& named_normal_draw = level->level->tie_trees.at(0).at(0).static_draws.at(0);
    const s32 static_texture_id = named_normal_draw.tree_tex_id;
    named_normal_draw.tree_tex_id = -1;
    const auto animated_fallback =
        render_sequence(device, queue, &pso_cache, &sampler_cache, &texture_pool, &parent, &child,
                        &all_parent, nullptr, 61);
    named_normal_draw.tree_tex_id = static_texture_id;
    check(animated_fallback.completed && animated_fallback.parent_finished &&
              animated_fallback.renderer.trees_rendered == 2 &&
              animated_fallback.renderer.draws == 1 && animated_fallback.renderer.runs == 1 &&
              animated_fallback.renderer.triangles == 4 && animated_fallback.draw_calls == 1 &&
              animated_fallback.triangles == 4 &&
              animated_fallback.background.tie_draws == 1 &&
              animated_fallback.background.tie_tris == 4 &&
              animated_fallback.background.anim_slot_draws == 1 &&
              animated_fallback.background.missing_textures == 1 &&
              animated_fallback.background.missing_levels == 0 &&
              animated_fallback.background.unexpected_dma == 0,
          "one negative texture-animator slot draws once and reports the exact fallback counters");
    check(count_non_black_rgb(animated_fallback.pixels) > 100,
          "the unsupported animated slot visibly degrades through the placeholder texture");

    check(metal_level_data::unload(texture_pool, kLevelName),
          "released the synthetic TIE level after GPU completion");
    if (placeholder_handle) {
      metal_texture_release(placeholder_handle);
    }
    if (failures) {
      std::printf("FAIL: %d Jak II normal TIE/ETIE renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II normal TIE/ETIE rendered exact asset-free paired fixtures\n");
    return 0;
  }
}
