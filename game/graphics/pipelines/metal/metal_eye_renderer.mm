#include "metal_eye_renderer.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "common/log/log.h"
#include "common/util/fnv.h"

#include "game/graphics/opengl_renderer/AdgifHandler.h"
#include "game/graphics/pipelines/metal/metal_jak2_pris2_bucket228_plan.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

namespace {

// A bucket that does not match is consumed whole and reported, never asserted
// on: this renderer reads the player's own game data.
bool eye_expect(bool condition, const char* what, int* counter, bool* warned) {
  if (condition) {
    return true;
  }
  (*counter)++;
  if (!*warned) {
    *warned = true;
    lg::warn("Metal eyes: expected {}; the bucket is skipped (logged once)", what);
  }
  return false;
}

id<MTLTexture> make_eye_target(id<MTLDevice> device) {
  auto* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                  width:METAL_EYE_TEX_SIZE
                                                                 height:METAL_EYE_TEX_SIZE
                                                              mipmapped:NO];
  desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  desc.storageMode = MTLStorageModePrivate;
  return [device newTextureWithDescriptor:desc];
}

constexpr std::size_t kDiagnosticReadbackAlignment = 256;
constexpr std::size_t kMaximumDiagnosticReadbackBytes = 16 * 1024 * 1024;

struct DiagnosticTextureCopy {
  id<MTLTexture> texture = nil;
  std::size_t offset = 0;
  std::size_t bytes_per_row = 0;
  u32 width = 0;
  u32 height = 0;
};

bool prepare_diagnostic_copy(id<MTLTexture> texture,
                             std::size_t* next_offset,
                             DiagnosticTextureCopy* out) {
  if (!texture || texture.pixelFormat != MTLPixelFormatRGBA8Unorm || texture.width == 0 ||
      texture.height == 0 || texture.width > UINT32_MAX || texture.height > UINT32_MAX) {
    return false;
  }
  const std::size_t unaligned_row_bytes = texture.width * sizeof(u32);
  const std::size_t row_bytes =
      (unaligned_row_bytes + kDiagnosticReadbackAlignment - 1) &
      ~(kDiagnosticReadbackAlignment - 1);
  if (texture.height > kMaximumDiagnosticReadbackBytes / row_bytes) {
    return false;
  }
  const std::size_t byte_count = row_bytes * texture.height;
  if (*next_offset > kMaximumDiagnosticReadbackBytes - byte_count) {
    return false;
  }
  out->texture = texture;
  out->offset = *next_offset;
  out->bytes_per_row = row_bytes;
  out->width = static_cast<u32>(texture.width);
  out->height = static_cast<u32>(texture.height);
  *next_offset += byte_count;
  return true;
}

void encode_diagnostic_copy(id<MTLBlitCommandEncoder> blit,
                            id<MTLBuffer> buffer,
                            const DiagnosticTextureCopy& copy) {
  if (!copy.texture) {
    return;
  }
  [blit copyFromTexture:copy.texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(copy.width, copy.height, 1)
               toBuffer:buffer
      destinationOffset:copy.offset
 destinationBytesPerRow:copy.bytes_per_row
destinationBytesPerImage:copy.bytes_per_row * copy.height
                options:MTLBlitOptionNone];
}

u64 hash_diagnostic_region(const u8* pixels,
                           std::size_t bytes_per_row,
                           u32 x,
                           u32 y,
                           u32 width,
                           u32 height) {
  u64 hash = 0xcbf29ce484222325;
  for (u32 row = 0; row < height; ++row) {
    const u8* source = pixels + (y + row) * bytes_per_row + x * sizeof(u32);
    for (u32 byte = 0; byte < width * sizeof(u32); ++byte) {
      hash = 1099511628211ull * (static_cast<u64>(source[byte]) ^ hash);
    }
  }
  return hash;
}

void diagnostic_corners(const u8* pixels,
                        std::size_t bytes_per_row,
                        u32 width,
                        u32 height,
                        std::array<u32, 4>* out) {
  const std::array<std::pair<u32, u32>, 4> coordinates = {
      std::pair<u32, u32>{0, 0}, {width - 1, 0}, {0, height - 1}, {width - 1, height - 1}};
  for (std::size_t i = 0; i < coordinates.size(); ++i) {
    const auto [x, y] = coordinates[i];
    std::memcpy(&(*out)[i], pixels + y * bytes_per_row + x * sizeof(u32), sizeof(u32));
  }
}

void record_diagnostic_source(const u8* buffer,
                              const DiagnosticTextureCopy& copy,
                              u64* hash,
                              u32* width,
                              u32* height,
                              std::array<u32, 4>* corners) {
  if (!copy.texture) {
    return;
  }
  const u8* pixels = buffer + copy.offset;
  *hash = hash_diagnostic_region(pixels, copy.bytes_per_row, 0, 0, copy.width, copy.height);
  *width = copy.width;
  *height = copy.height;
  diagnostic_corners(pixels, copy.bytes_per_row, copy.width, copy.height, corners);
}

}  // namespace

MetalEyeRenderer::MetalEyeRenderer(const std::string& name,
                                   int my_id,
                                   id<MTLDevice> device,
                                   id<MTLCommandQueue> queue)
    : MetalBucketRenderer(name, my_id), m_device(device), m_queue(queue) {
  const char* diagnostic_vertex_buffer =
      std::getenv("GOALPAD_JAK2_DIAGNOSTIC_EYE_DEDICATED_VERTEX_BUFFER");
  m_use_diagnostic_vertex_buffer =
      diagnostic_vertex_buffer && std::strcmp(diagnostic_vertex_buffer, "1") == 0;
  const char* cancel_shader_y_negation =
      std::getenv("GOALPAD_JAK2_DIAGNOSTIC_EYE_CANCEL_SHADER_Y_NEGATION");
  m_cancel_diagnostic_shader_y_negation =
      cancel_shader_y_negation && std::strcmp(cancel_shader_y_negation, "1") == 0;
  const char* diagnostic_output_readback =
      std::getenv("GOALPAD_JAK2_DIAGNOSTIC_EYE_OUTPUT_READBACK");
  m_use_diagnostic_output_readback =
      diagnostic_output_readback && std::strcmp(diagnostic_output_readback, "1") == 0;
  if (m_use_diagnostic_vertex_buffer) {
    m_diagnostic_vertex_buffer =
        [device newBufferWithLength:VTX_BUFFER_FLOATS * sizeof(float)
                            options:MTLResourceStorageModeShared];
    lg::info("Metal eyes: using the diagnostic dedicated vertex buffer");
  }
  if (m_cancel_diagnostic_shader_y_negation) {
    lg::info("Metal eyes: canceling the eye shader's vertex-Y negation for diagnostics");
  }
  if (m_use_diagnostic_output_readback) {
    lg::info("Metal eyes: reading back the first composed eye and real iris/lid sources");
  }
  // Audited PRIS producers can each replace any of the forty slots once per frame.
  // Reserve every possible retired generation before publication can begin.
  m_retired_handles.reserve(METAL_NUM_EYE_PAIRS * 2 *
                            (metal_renderer::kJak2PrisEyeProducerCount - 1));
  for (auto& tex : m_gpu_eye_textures) {
    tex.texture = make_eye_target(device);
    tex.handle = metal_texture_register(tex.texture);
  }
}

MetalEyeRenderer::~MetalEyeRenderer() {
  detach_pool();
  for (u64 handle : m_retired_handles) {
    metal_texture_release(handle);
  }
  m_retired_handles.clear();
  for (auto& tex : m_gpu_eye_textures) {
    if (tex.handle) {
      metal_texture_release(tex.handle);
      tex.handle = 0;
    }
  }
}

/*!
 * Mirror of EyeRenderer::init_textures: each eye gets a VRAM slot so merc's
 * adgifs and the pool's slot lookups resolve to the composed texture.
 */
bool MetalEyeRenderer::init_textures(TexturePool& texture_pool, GameVersion version) {
  if (m_pool || !m_device || !m_queue ||
      (m_use_diagnostic_vertex_buffer && !m_diagnostic_vertex_buffer) ||
      (version != GameVersion::Jak1 && version != GameVersion::Jak2)) {
    return false;
  }
  for (const auto& tex : m_gpu_eye_textures) {
    if (!tex.texture || !tex.handle) {
      return false;
    }
  }

  std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
  for (int pair_idx = 0; pair_idx < METAL_NUM_EYE_PAIRS; pair_idx++) {
    for (int lr = 0; lr < 2; lr++) {
      u32 tidx = pair_idx * 2 + lr;
      u32 tbp = pair_idx * 2 + lr;
      // Jak 2 deliberately uses Jak 1's base, matching the GL renderer. Its
      // nominal base overlaps ocean state in the original game.
      tbp += METAL_EYE_BASE_BLOCK_JAK1;

      TextureInput in;
      in.gpu_texture = m_gpu_eye_textures[tidx].handle;
      in.w = 32;
      in.h = 32;
      in.debug_page_name = "PC-EYES";
      in.debug_name = fmt::format("{}-eye-gpu-{}", lr ? "left" : "right", pair_idx);
      in.id = texture_pool.allocate_pc_port_texture(version);
      m_gpu_eye_textures[tidx].texture_id = in.id;
      m_gpu_eye_textures[tidx].gpu_tex = texture_pool.give_texture_and_load_to_vram(in, tbp);
      m_gpu_eye_textures[tidx].tbp = tbp;
    }
  }
  m_pool = &texture_pool;
  return true;
}

void MetalEyeRenderer::detach_pool() {
  if (!m_pool) {
    return;
  }
  std::lock_guard<std::mutex> pool_lock(m_pool->mutex());
  for (auto& tex : m_gpu_eye_textures) {
    if (tex.gpu_tex && tex.handle) {
      m_pool->unload_texture(tex.texture_id, tex.handle);
    }
    tex.gpu_tex = nullptr;
  }
  m_pool = nullptr;
}

void MetalEyeRenderer::start_frame() {
  // MetalRenderer calls this only after waiting for the prior stream submission. Earlier Merc
  // encoders can no longer reference these retained generations at this boundary.
  for (u64 handle : m_retired_handles) {
    metal_texture_release(handle);
  }
  m_retired_handles.clear();
  m_stats = Stats();
  m_frame_producer_buckets.fill(0);
  for (auto& tex : m_gpu_eye_textures) {
    tex.composed_this_frame = false;
  }
}

// ---------------------------------------------------------------------------
// DMA decode - the GL logic, with the asserts turned into reports
// ---------------------------------------------------------------------------

namespace {

bool decode_scissor(const DmaTransfer& dma,
                    MetalEyeRenderer::ScissorInfo* out,
                    int* counter,
                    bool* warned) {
  if (!eye_expect(dma.vif0() == 0 && dma.vifcode1().kind == VifCode::Kind::DIRECT &&
                      dma.size_bytes == 32,
                  "a 32-byte DIRECT scissor transfer", counter, warned)) {
    return false;
  }
  GifTag gif_tag(dma.data);
  if (!eye_expect(gif_tag.nloop() == 1 && gif_tag.eop() && !gif_tag.pre() &&
                      gif_tag.flg() == GifTag::Format::PACKED && gif_tag.nreg() == 1,
                  "a 1-register PACKED scissor GIF tag", counter, warned)) {
    return false;
  }
  u8 reg_addr;
  memcpy(&reg_addr, dma.data + 24, 1);
  if (!eye_expect((GsRegisterAddress)reg_addr == GsRegisterAddress::SCISSOR_1,
                  "the scissor register", counter, warned)) {
    return false;
  }
  u64 val;
  memcpy(&val, dma.data + 16, 8);
  GsScissor reg(val);
  out->x0 = reg.x0();
  out->x1 = reg.x1();
  out->y0 = reg.y0();
  out->y1 = reg.y1();
  return true;
}

bool decode_sprite(const DmaTransfer& dma,
                   MetalEyeRenderer::SpriteInfo* out,
                   int* counter,
                   bool* warned) {
  if (!eye_expect(dma.vif0() == 0 && dma.vifcode1().kind == VifCode::Kind::DIRECT &&
                      dma.size_bytes == 6 * 16,
                  "a 6-quadword DIRECT sprite transfer", counter, warned)) {
    return false;
  }
  GifTag gif_tag(dma.data);
  if (!eye_expect(gif_tag.nloop() == 1 && gif_tag.eop() && gif_tag.pre() &&
                      gif_tag.flg() == GifTag::Format::PACKED && gif_tag.nreg() == 5,
                  "a 5-register PACKED sprite GIF tag", counter, warned)) {
    return false;
  }
  if (!eye_expect(dma.data[16] == 128 && dma.data[16 + 4] == 128 && dma.data[16 + 8] == 128,
                  "an unmodulated sprite color", counter, warned)) {
    return false;
  }
  memcpy(&out->a, dma.data + 16 + 12, 1);
  memcpy(&out->uv0, &dma.data[32], 8);
  memcpy(&out->xyz0[0], &dma.data[48], 12);
  out->xyz0[2] >>= 4;
  memcpy(&out->uv1[0], &dma.data[64], 8);
  memcpy(&out->xyz1[0], &dma.data[80], 12);
  out->xyz1[2] >>= 4;
  return true;
}

bool read_eye_draw(DmaFollower& dma,
                   MetalEyeRenderer::EyeDraw* out,
                   int* counter,
                   bool* warned) {
  if (!decode_scissor(dma.read_and_advance(), &out->scissor, counter, warned)) {
    return false;
  }
  return decode_sprite(dma.read_and_advance(), &out->sprite, counter, warned);
}

}  // namespace

/*!
 * Resolves an adgif's TEX0 to a pool handle. The GL renderer dereferences
 * lookup() unconditionally; here a slot with nothing in it is counted and the
 * draw is dropped, which is what the GL renderer's pupil path already does.
 */
enum class EyeSourceComponent : u32 {
  Iris = 1,
  Pupil = 2,
  Lid = 3,
};

static u64 lookup_eye_source(TexturePool* pool,
                             u32 tbp,
                             bool* has_data,
                             MetalEyeRenderer::Stats* stats,
                             EyeSourceComponent component) {
  auto handle = pool->lookup(tbp);
  auto* gpu_tex = pool->lookup_gpu_texture(tbp);
  *has_data = gpu_tex && gpu_tex->get_data_ptr();
  if (!handle) {
    stats->missing_textures++;
    return 0;
  }
  if (gpu_tex && gpu_tex->is_placeholder) {
    stats->placeholder_textures++;
    switch (component) {
      case EyeSourceComponent::Iris:
        stats->placeholder_iris_textures++;
        break;
      case EyeSourceComponent::Pupil:
        stats->placeholder_pupil_textures++;
        break;
      case EyeSourceComponent::Lid:
        stats->placeholder_lid_textures++;
        break;
    }
    if (!stats->first_placeholder_handle) {
      stats->first_placeholder_component = static_cast<u32>(component);
      stats->first_placeholder_tbp = tbp;
      stats->first_placeholder_handle = *handle;
      if (gpu_tex) {
        stats->first_placeholder_width = gpu_tex->w;
        stats->first_placeholder_height = gpu_tex->h;
        stats->first_placeholder_texture_id =
            (static_cast<u32>(gpu_tex->tex_id.page) << 16) | gpu_tex->tex_id.tex;
      }
    }
  }
  return *handle;
}

std::vector<MetalEyeRenderer::SingleEyeDraws> MetalEyeRenderer::get_draws(
    DmaFollower& dma,
    MetalSharedRenderState* render_state) {
  std::vector<SingleEyeDraws> draws;
  int* counter = &m_stats.unexpected_dma;
  bool* warned = &m_warned_dma;
  auto* pool = render_state->texture_pool;

  // The end condition is the 8-quadword transfer that restores GS state. The
  // eye DMA lives outside the bucket's own address range (the bucket chains
  // to it), so the loop is bounded by the number of eye pairs the renderer
  // has textures for rather than by the bucket's end offset.
  while (dma.current_tag().qwc != 8) {
    if ((int)draws.size() >= METAL_NUM_EYE_PAIRS * 2) {
      eye_expect(false, "no more eyes than the renderer has textures for", counter, warned);
      draws.clear();
      return draws;
    }
    draws.emplace_back();
    draws.emplace_back();

    auto& l_draw = draws[draws.size() - 2];
    auto& r_draw = draws[draws.size() - 1];
    l_draw.lr = 0;
    r_draw.lr = 1;

    auto adgif0_dma = dma.read_and_advance();
    if (!eye_expect(adgif0_dma.size_bytes == 96 && adgif0_dma.vif0() == 0 &&
                        adgif0_dma.vifcode1().kind == VifCode::Kind::DIRECT,
                    "the eye background adgif", counter, warned)) {
      draws.clear();
      return draws;
    }
    AdgifHelper adgif0(adgif0_dma.data + 16);
    bool tex0_has_data = false;
    const u64 tex0 = lookup_eye_source(pool, adgif0.tex0().tbp0(), &tex0_has_data, &m_stats,
                                       EyeSourceComponent::Iris);

    // first draw: the background. It reads 0,0 of the texture and uses that
    // color everywhere. The eye index falls out of its coordinates.
    bool using_64 = false;
    {
      EyeDraw draw0;
      if (!read_eye_draw(dma, &draw0, counter, warned)) {
        draws.clear();
        return draws;
      }
      l_draw.fnv_name_hash = draw0.sprite.uv0;
      r_draw.fnv_name_hash = draw0.sprite.uv0;
      if (draw0.scissor.y1 - draw0.scissor.y0 == 63) {
        using_64 = true;
        l_draw.using_64 = true;
        r_draw.using_64 = true;
      }
      u32 y0 = (draw0.sprite.xyz0[1] - 512) >> 4;
      if (using_64) {
        y0 = (draw0.sprite.xyz0[1] - 1024) >> 5;
        y0 *= 4;
      }
      u32 pair_idx = y0 / METAL_SINGLE_EYE_SIZE;
      if (!eye_expect(pair_idx < (u32)METAL_NUM_EYE_PAIRS, "an in-range eye pair index", counter,
                      warned)) {
        draws.clear();
        return draws;
      }
      l_draw.pair = (int)pair_idx;
      r_draw.pair = (int)pair_idx;
    }

    // the iris
    {
      if (!read_eye_draw(dma, &l_draw.iris, counter, warned)) {
        draws.clear();
        return draws;
      }
      l_draw.has_iris = tex0 != 0;
      l_draw.iris_tex_handle = tex0;

      if (dma.current_tag().qwc == 6) {
        // the right eye changes adgif
        auto r_iris_adgif = dma.read_and_advance();
        if (!eye_expect(r_iris_adgif.size_bytes == 96 && r_iris_adgif.vif0() == 0 &&
                            r_iris_adgif.vifcode1().kind == VifCode::Kind::DIRECT,
                        "the right iris adgif", counter, warned)) {
          draws.clear();
          return draws;
        }
        AdgifHelper r_iris_helper(r_iris_adgif.data + 16);
        bool has_data = false;
        u64 handle = lookup_eye_source(pool, r_iris_helper.tex0().tbp0(), &has_data, &m_stats,
                                       EyeSourceComponent::Iris);
        if (!read_eye_draw(dma, &r_draw.iris, counter, warned)) {
          draws.clear();
          return draws;
        }
        r_draw.has_iris = handle != 0;
        r_draw.iris_tex_handle = handle;
      } else {
        if (!read_eye_draw(dma, &r_draw.iris, counter, warned)) {
          draws.clear();
          return draws;
        }
        r_draw.has_iris = l_draw.has_iris;
        r_draw.iris_tex_handle = l_draw.iris_tex_handle;
      }
    }

    // the pupil, drawn on top
    dma.read_and_advance();  // test register
    auto adgif1_dma = dma.read_and_advance();
    if (!eye_expect(adgif1_dma.size_bytes == 96 && adgif1_dma.vif0() == 0 &&
                        adgif1_dma.vifcode1().kind == VifCode::Kind::DIRECT,
                    "the pupil adgif", counter, warned)) {
      draws.clear();
      return draws;
    }
    AdgifHelper adgif1(adgif1_dma.data + 16);
    bool tex1_has_data = false;
    const u64 tex1 = lookup_eye_source(pool, adgif1.tex0().tbp0(), &tex1_has_data, &m_stats,
                                       EyeSourceComponent::Pupil);

    if (tex1_has_data && tex1) {
      if (!read_eye_draw(dma, &l_draw.pupil, counter, warned)) {
        draws.clear();
        return draws;
      }
      l_draw.has_pupil = true;
      l_draw.pupil_tex_handle = tex1;
    }

    if (dma.current_tag().qwc == 6) {
      auto r_pupil_adgif = dma.read_and_advance();
      if (!eye_expect(r_pupil_adgif.size_bytes == 96 && r_pupil_adgif.vif0() == 0 &&
                          r_pupil_adgif.vifcode1().kind == VifCode::Kind::DIRECT,
                      "the right pupil adgif", counter, warned)) {
        draws.clear();
        return draws;
      }
      AdgifHelper r_pupil_helper(r_pupil_adgif.data + 16);
      bool has_data = false;
      u64 handle = lookup_eye_source(pool, r_pupil_helper.tex0().tbp0(), &has_data, &m_stats,
                                     EyeSourceComponent::Pupil);
      if (!read_eye_draw(dma, &r_draw.pupil, counter, warned)) {
        draws.clear();
        return draws;
      }
      r_draw.has_pupil = handle != 0;
      r_draw.pupil_tex_handle = handle;
    } else if (tex1_has_data && tex1) {
      if (!read_eye_draw(dma, &r_draw.pupil, counter, warned)) {
        draws.clear();
        return draws;
      }
      r_draw.has_pupil = true;
      r_draw.pupil_tex_handle = tex1;
    }

    // and finally the eyelid
    dma.read_and_advance();  // test register
    auto adgif2_dma = dma.read_and_advance();
    if (!eye_expect(adgif2_dma.size_bytes == 96 && adgif2_dma.vif0() == 0 &&
                        adgif2_dma.vifcode1().kind == VifCode::Kind::DIRECT,
                    "the eyelid adgif", counter, warned)) {
      draws.clear();
      return draws;
    }
    AdgifHelper adgif2(adgif2_dma.data + 16);
    bool tex2_has_data = false;
    const u64 tex2 = lookup_eye_source(pool, adgif2.tex0().tbp0(), &tex2_has_data, &m_stats,
                                       EyeSourceComponent::Lid);

    if (!read_eye_draw(dma, &l_draw.lid, counter, warned)) {
      draws.clear();
      return draws;
    }
    l_draw.has_lid = tex2 != 0;
    l_draw.lid_tex_handle = tex2;

    if (dma.current_tag().qwc == 6) {
      auto r_lid_adgif = dma.read_and_advance();
      if (!eye_expect(r_lid_adgif.size_bytes == 96 && r_lid_adgif.vif0() == 0 &&
                          r_lid_adgif.vifcode1().kind == VifCode::Kind::DIRECT,
                      "the right eyelid adgif", counter, warned)) {
        draws.clear();
        return draws;
      }
      AdgifHelper r_lid_helper(r_lid_adgif.data + 16);
      bool has_data = false;
      u64 handle = lookup_eye_source(pool, r_lid_helper.tex0().tbp0(), &has_data, &m_stats,
                                     EyeSourceComponent::Lid);
      if (!read_eye_draw(dma, &r_draw.lid, counter, warned)) {
        draws.clear();
        return draws;
      }
      r_draw.has_lid = handle != 0;
      r_draw.lid_tex_handle = handle;
    } else {
      if (!read_eye_draw(dma, &r_draw.lid, counter, warned)) {
        draws.clear();
        return draws;
      }
      r_draw.has_lid = l_draw.has_lid;
      r_draw.lid_tex_handle = l_draw.lid_tex_handle;
    }

    if (render_state->version == GameVersion::Jak1) {
      auto end = dma.read_and_advance();
      if (!eye_expect(end.size_bytes == 0 && end.vif0() == 0 && end.vif1() == 0,
                      "the per-eye terminator", counter, warned)) {
        draws.clear();
        return draws;
      }
    }
  }
  return draws;
}

bool MetalEyeRenderer::handle_eye_dma2(DmaFollower& dma, MetalSharedRenderState* render_state) {
  int* counter = &m_stats.unexpected_dma;
  bool* warned = &m_warned_dma;

  // the GS setup for render-to-texture
  auto offset_setup = dma.read_and_advance();
  if (!eye_expect(offset_setup.size_bytes == 128 &&
                      offset_setup.vifcode0().kind == VifCode::Kind::FLUSHA &&
                      offset_setup.vifcode1().kind == VifCode::Kind::DIRECT,
                  "the render-to-texture GS setup", counter, warned)) {
    return false;
  }

  auto alpha_setup = dma.read_and_advance();
  if (!eye_expect(alpha_setup.size_bytes == 32 &&
                      alpha_setup.vifcode0().kind == VifCode::Kind::NOP &&
                      alpha_setup.vifcode1().kind == VifCode::Kind::DIRECT,
                  "the alpha setup", counter, warned)) {
    return false;
  }

  if (render_state->version == GameVersion::Jak1) {
    if (!eye_expect(dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
                        dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0,
                    "the add-to-bucket tag", counter, warned)) {
      return false;
    }
    dma.read_and_advance();
  }
  return true;
}

void MetalEyeRenderer::render_from_texture_bucket(DmaFollower& dma,
                                                  MetalSharedRenderState* render_state,
                                                  MetalFrameContext& ctx,
                                                  u32 producer_bucket) {
  if (handle_eye_dma2(dma, render_state)) {
    auto draws = get_draws(dma, render_state);
    run_gpu(draws, render_state, ctx, producer_bucket);
  }
}

void MetalEyeRenderer::render(DmaFollower& dma,
                              MetalSharedRenderState* render_state,
                              MetalFrameContext& ctx) {
  auto data0 = dma.read_and_advance();
  if (!eye_expect(data0.vif1() == 0 && data0.vif0() == 0 && data0.size_bytes == 0,
                  "the empty bucket-entry transfer", &m_stats.unexpected_dma, &m_warned_dma)) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  // an empty bucket: the renderer did not run this frame
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    for (int i = 0; i < 4; i++) {
      dma.read_and_advance();
    }
    return;
  }

  if (handle_eye_dma2(dma, render_state)) {
    auto draws = get_draws(dma, render_state);
    run_gpu(draws, render_state, ctx, 0);
  }

  while (dma.current_tag_offset() != render_state->next_bucket) {
    dma.read_and_advance();
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

int add_draw_to_buffer_32(int idx,
                          const MetalEyeRenderer::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr) {
  int x_off = lr * METAL_SINGLE_EYE_SIZE * 16;
  int y_off = pair * METAL_SINGLE_EYE_SIZE * 16;

  data[idx++] = draw.sprite.xyz0[0] - x_off;
  data[idx++] = draw.sprite.xyz0[1] - y_off;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = draw.sprite.xyz1[0] - x_off;
  data[idx++] = draw.sprite.xyz0[1] - y_off;
  data[idx++] = 1;
  data[idx++] = 0;

  data[idx++] = draw.sprite.xyz0[0] - x_off;
  data[idx++] = draw.sprite.xyz1[1] - y_off;
  data[idx++] = 0;
  data[idx++] = 1;

  data[idx++] = draw.sprite.xyz1[0] - x_off;
  data[idx++] = draw.sprite.xyz1[1] - y_off;
  data[idx++] = 1;
  data[idx++] = 1;
  return idx;
}

int add_draw_to_buffer_64(int idx,
                          const MetalEyeRenderer::EyeDraw& draw,
                          float* data,
                          int pair,
                          int lr) {
  int x_off = lr * METAL_SINGLE_EYE_SIZE * 32;
  int y_off = (pair / 4) * METAL_SINGLE_EYE_SIZE * 32;

  data[idx++] = (draw.sprite.xyz0[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz0[1] - y_off) / 2;
  data[idx++] = 0;
  data[idx++] = 0;

  data[idx++] = (draw.sprite.xyz1[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz0[1] - y_off) / 2;
  data[idx++] = 1;
  data[idx++] = 0;

  data[idx++] = (draw.sprite.xyz0[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz1[1] - y_off) / 2;
  data[idx++] = 0;
  data[idx++] = 1;

  data[idx++] = (draw.sprite.xyz1[0] - x_off) / 2;
  data[idx++] = (draw.sprite.xyz1[1] - y_off) / 2;
  data[idx++] = 1;
  data[idx++] = 1;
  return idx;
}

// The whole eye texture is cleared with the 0,0 texel of the iris texture.
int add_clear_draw_to_buffer(int idx, float* data) {
  const float center = 768;
  const float upper = center + 256;
  const float lower = center - 256;
  const float xy[4][2] = {{lower, lower}, {upper, lower}, {lower, upper}, {upper, upper}};
  for (const auto& p : xy) {
    data[idx++] = p[0];
    data[idx++] = p[1];
    data[idx++] = 0;
    data[idx++] = 0;
  }
  return idx;
}

}  // namespace

/*!
 * The GL renderer composes each eye into an FBO with immediate-mode state
 * changes. Here each eye is one render pass (its clear replaces
 * glClearBufferfv) with four sub-draws, and the whole set runs on its own
 * command buffer because the frame's encoder is open - the same structure the
 * generated ocean texture uses.
 */
bool MetalEyeRenderer::run_gpu(const std::vector<SingleEyeDraws>& draws,
                               MetalSharedRenderState* render_state,
                               MetalFrameContext& ctx,
                               u32 producer_bucket) {
  if (draws.empty()) {
    return false;
  }

  std::array<bool, METAL_NUM_EYE_PAIRS * 2> pending_slots = {};
  std::array<bool, METAL_NUM_EYE_PAIRS * 2> versioned_slots = {};
  int duplicate_slots = 0;
  for (const auto& draw : draws) {
    const int slot = draw.tex_slot();
    if (slot < 0 || slot >= METAL_NUM_EYE_PAIRS * 2) {
      eye_expect(false, "an in-range eye texture slot", &m_stats.unexpected_dma, &m_warned_dma);
      return false;
    }
    if (pending_slots[slot]) {
      duplicate_slots++;
    } else if (m_gpu_eye_textures[slot].composed_this_frame) {
      if (metal_renderer::jak2_pris_eye_producer_precedes(
              m_frame_producer_buckets[slot], producer_bucket)) {
        versioned_slots[slot] = true;
      } else {
        duplicate_slots++;
      }
    }
    pending_slots[slot] = true;
  }
  if (duplicate_slots) {
    m_stats.duplicate_slot_writes += duplicate_slots;
    if (!m_warned_duplicate_slot) {
      m_warned_duplicate_slot = true;
      lg::warn("Metal eyes: rejected {} duplicate eye slot write(s) (logged once)",
               duplicate_slots);
    }
    return false;
  }

  int buffer_idx = 0;
  for (const auto& draw : draws) {
    buffer_idx = add_clear_draw_to_buffer(buffer_idx, m_cpu_vertex_buffer);
    if (draw.using_64) {
      buffer_idx = add_draw_to_buffer_64(buffer_idx, draw.iris, m_cpu_vertex_buffer, draw.pair,
                                         draw.lr);
      buffer_idx = add_draw_to_buffer_64(buffer_idx, draw.pupil, m_cpu_vertex_buffer, draw.pair,
                                         draw.lr);
      buffer_idx =
          add_draw_to_buffer_64(buffer_idx, draw.lid, m_cpu_vertex_buffer, draw.pair, draw.lr);
    } else {
      buffer_idx = add_draw_to_buffer_32(buffer_idx, draw.iris, m_cpu_vertex_buffer, draw.pair,
                                         draw.lr);
      buffer_idx = add_draw_to_buffer_32(buffer_idx, draw.pupil, m_cpu_vertex_buffer, draw.pair,
                                         draw.lr);
      buffer_idx =
          add_draw_to_buffer_32(buffer_idx, draw.lid, m_cpu_vertex_buffer, draw.pair, draw.lr);
    }
    if (buffer_idx > VTX_BUFFER_FLOATS) {
      eye_expect(false, "no more eyes than the vertex buffer holds", &m_stats.unexpected_dma,
                 &m_warned_dma);
      return false;
    }
  }
  id<MTLBuffer> vertex_buffer = nil;
  u32 vertex_buffer_offset = 0;
  const u32 vertex_buffer_size = (u32)buffer_idx * sizeof(float);
  m_stats.last_source_vertex_fingerprint = fnv64(m_cpu_vertex_buffer, vertex_buffer_size);
  if (m_cancel_diagnostic_shader_y_negation) {
    for (int vertex = 0; vertex < buffer_idx / 4; ++vertex) {
      float& y = m_cpu_vertex_buffer[vertex * 4 + 1];
      y = 1536.f - y;
    }
  }
  void* vertex_data = nullptr;
  if (m_use_diagnostic_vertex_buffer) {
    vertex_buffer = m_diagnostic_vertex_buffer;
    vertex_data = vertex_buffer.contents;
  } else {
    vertex_data = ctx.stream->alloc(vertex_buffer_size, &vertex_buffer, &vertex_buffer_offset);
  }
  memcpy(vertex_data, m_cpu_vertex_buffer, vertex_buffer_size);
  m_stats.vertex_stream_uploads++;
  m_stats.vertex_bytes += vertex_buffer_size;
  m_stats.last_vertex_buffer_offset = vertex_buffer_offset;
  m_stats.last_vertex_fingerprint = fnv64(m_cpu_vertex_buffer, vertex_buffer_size);

  MetalPsoKey opaque_key;
  opaque_key.shader = MetalShaderId::EYE;
  opaque_key.color_format = MTLPixelFormatRGBA8Unorm;
  id<MTLRenderPipelineState> opaque_pso = ctx.pso_cache->get_pipeline(opaque_key);

  MetalPsoKey blend_key = opaque_key;
  blend_key.blend_enable = true;
  blend_key.blend_src_rgb = MTLBlendFactorSourceAlpha;
  blend_key.blend_dst_rgb = MTLBlendFactorOneMinusSourceAlpha;
  blend_key.blend_src_alpha = MTLBlendFactorSourceAlpha;
  blend_key.blend_dst_alpha = MTLBlendFactorOneMinusSourceAlpha;
  id<MTLRenderPipelineState> blend_pso = ctx.pso_cache->get_pipeline(blend_key);

  MetalSamplerKey sampler_key;
  sampler_key.min_filter = MTLSamplerMinMagFilterLinear;
  sampler_key.mag_filter = MTLSamplerMinMagFilterLinear;
  id<MTLSamplerState> sampler = ctx.sampler_cache->get(sampler_key);
  if (!opaque_pso || !blend_pso || !sampler || !m_queue || !m_pool ||
      render_state->texture_pool != m_pool) {
    m_stats.command_buffer_errors++;
    return false;
  }

  struct PendingGeneration {
    id<MTLTexture> texture = nil;
    u64 handle = 0;
  };
  std::array<PendingGeneration, METAL_NUM_EYE_PAIRS * 2> pending_generations = {};
  const auto release_pending_generations = [&pending_generations]() {
    for (auto& generation : pending_generations) {
      if (generation.handle) {
        metal_texture_release(generation.handle);
        generation.handle = 0;
      }
      generation.texture = nil;
    }
  };
  for (std::size_t slot = 0; slot < versioned_slots.size(); ++slot) {
    if (!versioned_slots[slot]) {
      continue;
    }
    auto& generation = pending_generations[slot];
    generation.texture = make_eye_target(m_device);
    generation.handle = metal_texture_register(generation.texture);
    if (!generation.handle) {
      release_pending_generations();
      m_stats.command_buffer_errors++;
      return false;
    }
  }

  DiagnosticTextureCopy output_copy;
  DiagnosticTextureCopy iris_copy;
  DiagnosticTextureCopy lid_copy;
  id<MTLBuffer> diagnostic_readback = nil;
  const bool capture_diagnostic_output =
      m_use_diagnostic_output_readback && render_state->version == GameVersion::Jak2 &&
      m_stats.diagnostic_readbacks == 0;
  if (capture_diagnostic_output) {
    const auto& first_draw = draws.front();
    const int slot = first_draw.tex_slot();
    id<MTLTexture> output = versioned_slots[slot] ? pending_generations[slot].texture
                                                  : m_gpu_eye_textures[slot].texture;
    id<MTLTexture> iris = first_draw.has_iris ? metal_texture_lookup(first_draw.iris_tex_handle)
                                              : nil;
    id<MTLTexture> lid =
        first_draw.has_lid ? metal_texture_lookup(first_draw.lid_tex_handle) : nil;
    std::size_t readback_bytes = 0;
    const bool output_ready = prepare_diagnostic_copy(output, &readback_bytes, &output_copy);
    const bool iris_ready =
        !first_draw.has_iris || prepare_diagnostic_copy(iris, &readback_bytes, &iris_copy);
    const bool lid_ready =
        !first_draw.has_lid || prepare_diagnostic_copy(lid, &readback_bytes, &lid_copy);
    if (!iris_ready || !lid_ready) {
      m_stats.diagnostic_readback_errors++;
    }
    if (output_ready) {
      diagnostic_readback =
          [m_device newBufferWithLength:readback_bytes options:MTLResourceStorageModeShared];
    }
    if (!diagnostic_readback) {
      m_stats.diagnostic_readback_errors++;
    }
  }

  id<MTLCommandBuffer> cmds = [m_queue commandBuffer];
  if (!cmds) {
    release_pending_generations();
    m_stats.command_buffer_errors++;
    return false;
  }
  int encoded_draw_calls = 0;
  int encoded_triangles = 0;
  buffer_idx = 0;
  for (const auto& draw : draws) {
    const int slot = draw.tex_slot();
    auto& out_tex = m_gpu_eye_textures[slot];
    id<MTLTexture> target = versioned_slots[slot]
                                ? pending_generations[slot].texture
                                : out_tex.texture;

    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // the GL renderer's debugging clear: red where nothing draws
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 0.0);
    id<MTLRenderCommandEncoder> enc = [cmds renderCommandEncoderWithDescriptor:pass];
    if (!enc) {
      release_pending_generations();
      m_stats.command_buffer_errors++;
      return false;
    }
    [enc setCullMode:MTLCullModeNone];
    [enc setVertexBuffer:vertex_buffer offset:vertex_buffer_offset atIndex:0];
    [enc setFragmentSamplerState:sampler atIndex:0];

    auto quad = [&](id<MTLRenderPipelineState> pso, u64 handle) {
      id<MTLTexture> tex = metal_texture_lookup(handle);
      if (pso && tex) {
        [enc setRenderPipelineState:pso];
        [enc setFragmentTexture:tex atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:(NSUInteger)(buffer_idx / 4)
                vertexCount:4];
        encoded_draw_calls++;
        encoded_triangles += 2;
      }
      buffer_idx += 4 * 4;
    };

    // background (the whole texture, from the iris texture's 0,0 texel), then
    // the iris, the alpha-blended pupil, and the eyelid
    quad(draw.has_iris ? opaque_pso : nil, draw.iris_tex_handle);
    quad(draw.has_iris ? opaque_pso : nil, draw.iris_tex_handle);
    quad(draw.has_pupil ? blend_pso : nil, draw.pupil_tex_handle);
    quad(draw.has_lid ? opaque_pso : nil, draw.lid_tex_handle);

    [enc endEncoding];
  }

  if (diagnostic_readback) {
    id<MTLBlitCommandEncoder> blit = [cmds blitCommandEncoder];
    if (blit) {
      encode_diagnostic_copy(blit, diagnostic_readback, output_copy);
      encode_diagnostic_copy(blit, diagnostic_readback, iris_copy);
      encode_diagnostic_copy(blit, diagnostic_readback, lid_copy);
      [blit endEncoding];
    } else {
      diagnostic_readback = nil;
      m_stats.diagnostic_readback_errors++;
    }
  }

  [cmds commit];
  m_stats.command_buffers_committed++;
  const bool wait_for_composition =
      !ctx.auxiliary_submissions_share_frame_queue || render_state->version == GameVersion::Jak2;
  if (wait_for_composition) {
    // Borrowed frame command buffers may be submitted to another queue. Preserve immediate
    // ordering there. Jak II also publishes the composed generations synchronously because its
    // downstream PRIS buckets consume and validate them in the same engine frame.
    [cmds waitUntilCompleted];
    m_stats.last_command_buffer_status = static_cast<int>(cmds.status);
    if (cmds.status != MTLCommandBufferStatusCompleted) {
      release_pending_generations();
      m_stats.command_buffer_errors++;
      lg::error("Metal eyes: composition command buffer failed with status {}",
                m_stats.last_command_buffer_status);
      return false;
    }
    m_stats.command_buffers_completed++;
    if (diagnostic_readback) {
      const u8* buffer = static_cast<const u8*>(diagnostic_readback.contents);
      const u8* output = buffer + output_copy.offset;
      m_stats.diagnostic_readbacks++;
      m_stats.diagnostic_producer_bucket = producer_bucket;
      m_stats.diagnostic_eye_slot = static_cast<u32>(draws.front().tex_slot());
      m_stats.diagnostic_output_hash = hash_diagnostic_region(
          output, output_copy.bytes_per_row, 0, 0, output_copy.width, output_copy.height);
      const u32 half_width = output_copy.width / 2;
      const u32 half_height = output_copy.height / 2;
      m_stats.diagnostic_output_quadrant_hashes = {
          hash_diagnostic_region(output, output_copy.bytes_per_row, 0, 0, half_width, half_height),
          hash_diagnostic_region(output, output_copy.bytes_per_row, half_width, 0,
                                 output_copy.width - half_width, half_height),
          hash_diagnostic_region(output, output_copy.bytes_per_row, 0, half_height, half_width,
                                 output_copy.height - half_height),
          hash_diagnostic_region(output, output_copy.bytes_per_row, half_width, half_height,
                                 output_copy.width - half_width,
                                 output_copy.height - half_height)};
      diagnostic_corners(output, output_copy.bytes_per_row, output_copy.width, output_copy.height,
                         &m_stats.diagnostic_output_corners);
      record_diagnostic_source(buffer, iris_copy, &m_stats.diagnostic_iris_source_hash,
                               &m_stats.diagnostic_iris_source_width,
                               &m_stats.diagnostic_iris_source_height,
                               &m_stats.diagnostic_iris_source_corners);
      record_diagnostic_source(buffer, lid_copy, &m_stats.diagnostic_lid_source_hash,
                               &m_stats.diagnostic_lid_source_width,
                               &m_stats.diagnostic_lid_source_height,
                               &m_stats.diagnostic_lid_source_corners);
    }
  }

  {
    std::lock_guard<std::mutex> pool_lock(m_pool->mutex());
    for (const auto& draw : draws) {
      const int slot = draw.tex_slot();
      auto& out_tex = m_gpu_eye_textures[slot];
      if (versioned_slots[slot]) {
        auto& generation = pending_generations[slot];
        const u64 retired_handle = out_tex.handle;
        m_pool->update_gl_texture(out_tex.gpu_tex, 32, 32, generation.handle);
        out_tex.texture = generation.texture;
        out_tex.handle = generation.handle;
        generation.handle = 0;
        generation.texture = nil;
        m_retired_handles.push_back(retired_handle);
        m_stats.versioned_slot_writes++;
      }
      m_pool->move_existing_to_vram(out_tex.gpu_tex, out_tex.tbp);
    }
  }
  for (const auto& draw : draws) {
    auto& out_tex = m_gpu_eye_textures[draw.tex_slot()];
    out_tex.fnv_name_hash = draw.fnv_name_hash;
    out_tex.lr = draw.lr;
    out_tex.composed_once = true;
    out_tex.composed_this_frame = true;
    m_frame_producer_buckets[draw.tex_slot()] = producer_bucket;
    if (!m_stats.first_texture) {
      m_stats.first_texture = out_tex.handle;
    }
  }
  m_stats.eyes += static_cast<int>(draws.size());
  m_stats.draw_calls += encoded_draw_calls;
  m_stats.triangles += encoded_triangles;
  return true;
}

std::optional<u64> MetalEyeRenderer::lookup_eye_texture(u8 eye_id) {
  eye_id = (eye_id % 40);
  if ((int)eye_id >= METAL_NUM_EYE_PAIRS * 2) {
    return {};
  }
  const auto& slot = m_gpu_eye_textures[eye_id];
  if (!slot.composed_once) {
    return {};
  }
  const u64 handle = slot.handle;
  return handle ? std::optional<u64>(handle) : std::optional<u64>();
}

std::optional<u64> MetalEyeRenderer::lookup_eye_texture_hash(u64 hash, bool lr) {
  for (auto& slot : m_gpu_eye_textures) {
    if (slot.composed_once && slot.fnv_name_hash == hash && slot.lr == lr) {
      return slot.handle ? std::optional<u64>(slot.handle) : std::optional<u64>();
    }
  }
  return {};
}
