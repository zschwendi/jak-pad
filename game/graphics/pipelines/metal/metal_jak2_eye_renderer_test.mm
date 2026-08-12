#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/dma/gs.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr u32 kSourceTbp = 100;
constexpr u64 kEyeHash = 0x123456789abcdef0ull;

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

void append_qword(std::vector<u8>* data, u64 lo, u64 hi) {
  const std::size_t offset = data->size();
  data->resize(offset + 16);
  std::memcpy(data->data() + offset, &lo, sizeof(lo));
  std::memcpy(data->data() + offset + 8, &hi, sizeof(hi));
}

void append_transfer(std::vector<u8>* chain,
                     const std::vector<u8>& payload,
                     VifCode::Kind vif0,
                     VifCode::Kind vif1) {
  const u64 tag = payload.size() / 16 |
                  (static_cast<u64>(DmaTag::Kind::CNT) << 28);
  const u64 transferred_tag = static_cast<u64>(vif0) << 24 |
                              (static_cast<u64>(vif1) << 56);
  append_qword(chain, tag, transferred_tag);
  chain->insert(chain->end(), payload.begin(), payload.end());
}

std::vector<u8> make_adgif(u32 tbp) {
  std::vector<u8> data;
  const u64 tag = 5 | (1ull << 15) | (1ull << 60);
  append_qword(&data, tag, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  AdGifData adgif = {};
  adgif.tex0_data = tbp;
  adgif.tex0_addr = static_cast<u64>(GsRegisterAddress::TEX0_1);
  adgif.tex1_addr = static_cast<u64>(GsRegisterAddress::TEX1_1);
  adgif.mip_addr = static_cast<u64>(GsRegisterAddress::MIPTBP1_1);
  adgif.clamp_addr = static_cast<u64>(GsRegisterAddress::CLAMP_1);
  adgif.alpha_addr = static_cast<u64>(GsRegisterAddress::ALPHA_1);
  const std::size_t offset = data.size();
  data.resize(offset + sizeof(adgif));
  std::memcpy(data.data() + offset, &adgif, sizeof(adgif));
  return data;
}

std::vector<u8> make_scissor() {
  std::vector<u8> data;
  const u64 tag = 1 | (1ull << 15) | (1ull << 60);
  append_qword(&data, tag, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  const u64 scissor = 31ull << 16 | 31ull << 48;
  append_qword(&data, scissor, static_cast<u64>(GsRegisterAddress::SCISSOR_1));
  return data;
}

std::vector<u8> make_sprite(u64 hash) {
  std::vector<u8> data;
  const u64 tag = 1 | (1ull << 15) | (1ull << 46) | (5ull << 60);
  const u64 regs = static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 4) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 8) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 12) |
                   (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 16);
  append_qword(&data, tag, regs);

  std::array<u8, 16> color = {};
  color[0] = 128;
  color[4] = 128;
  color[8] = 128;
  color[12] = 128;
  data.insert(data.end(), color.begin(), color.end());
  append_qword(&data, hash, 0);
  append_qword(&data, 512ull | (512ull << 32), 0xffffffull << 4);
  append_qword(&data, 0, 0);
  append_qword(&data, 1024ull | (1024ull << 32), 0xffffffull << 4);
  return data;
}

void append_eye_draw(std::vector<u8>* chain, u64 hash) {
  append_transfer(chain, make_scissor(), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_transfer(chain, make_sprite(hash), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
}

std::vector<u8> make_eye_chain() {
  std::vector<u8> chain;
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::FLUSHA,
                  VifCode::Kind::DIRECT);
  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP, VifCode::Kind::DIRECT);

  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_eye_draw(&chain, kEyeHash);  // pair/hash metadata
  append_eye_draw(&chain, 0);         // left iris
  append_eye_draw(&chain, 0);         // right iris

  append_transfer(&chain, std::vector<u8>(16), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_eye_draw(&chain, 0);  // left pupil
  append_eye_draw(&chain, 0);  // right pupil

  append_transfer(&chain, std::vector<u8>(16), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  append_eye_draw(&chain, 0);  // left lid
  append_eye_draw(&chain, 0);  // right lid

  // get_draws stops before the fixed GS-state restore transfer.
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::NOP, VifCode::Kind::DIRECT);
  return chain;
}

}  // namespace

int main() {
  @autoreleasepool {
    const std::size_t initial_live_textures = metal_texture_live_count();
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
    check(queue != nil && library != nil, "initialized Metal and the eye shader library");
    if (!queue || !library) {
      if (library_error) {
        std::printf("Metal library error: %s\n", library_error.localizedDescription.UTF8String);
      }
      return 1;
    }

    MetalPsoCache pso_cache;
    MetalSamplerCache sampler_cache;
    check(pso_cache.init(device, library), "initialized the eye pipeline cache");
    sampler_cache.init(device);

    TexturePool texture_pool(GameVersion::Jak2);
    const std::array<u32, 4> source_pixels = {
        0xffffffff, 0xff2020ff, 0xff20ff20, 0xffff2020};
    const u64 source_handle = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(source_pixels.data()), 2, 2);
    PcTextureId source_id;
    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      TextureInput input;
      input.debug_page_name = "PC-EYE-TEST";
      input.debug_name = "synthetic-eye-source";
      input.id = texture_pool.allocate_pc_port_texture(GameVersion::Jak2);
      input.gpu_texture = source_handle;
      input.src_data = reinterpret_cast<const u8*>(source_pixels.data());
      input.w = 2;
      input.h = 2;
      source_id = input.id;
      texture_pool.give_texture_and_load_to_vram(input, kSourceTbp);
    }
    check(source_handle != 0, "published the synthetic eye source texture");

    {
      MetalEyeRenderer renderer("jak2-eyes", 0, device, queue);
      check(renderer.init_textures(texture_pool, GameVersion::Jak2),
            "initialized Jak II eye slots at the GL-compatible base");
      check(!renderer.lookup_eye_texture(0) &&
                !renderer.lookup_eye_texture_hash(kEyeHash, false),
            "uncomposed eye targets are not lookup-visible");

      MetalSharedRenderState state;
      state.version = GameVersion::Jak2;
      state.texture_pool = &texture_pool;
      MetalFrameContext context;
      context.pso_cache = &pso_cache;
      context.sampler_cache = &sampler_cache;
      const auto chain = make_eye_chain();

      DmaFollower first_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(first_dma, &state, context);
      const auto first = renderer.stats();
      check(first.eyes == 2 && first.draw_calls == 8 && first.triangles == 16 &&
                first.duplicate_slot_writes == 0 && first.command_buffers_committed == 1 &&
                first.command_buffers_completed == 1 && first.command_buffer_errors == 0 &&
                first.last_command_buffer_status == MTLCommandBufferStatusCompleted,
            "one synthetic eye pair publishes only after its command buffer completes");
      check(renderer.lookup_eye_texture(0) && renderer.lookup_eye_texture(1) &&
                renderer.lookup_eye_texture_hash(kEyeHash, false) &&
                renderer.lookup_eye_texture_hash(kEyeHash, true),
            "both successfully composed eye targets are lookup-visible");

      DmaFollower duplicate_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(duplicate_dma, &state, context);
      const auto duplicate = renderer.stats();
      check(duplicate.eyes == 2 && duplicate.duplicate_slot_writes == 2 &&
                duplicate.command_buffers_committed == 1 &&
                duplicate.command_buffers_completed == 1,
            "duplicate same-frame eye slot writes fail before another command buffer");

      renderer.start_frame();
      check(renderer.lookup_eye_texture(0) && renderer.lookup_eye_texture(1),
            "a completed eye composition remains visible across frame reset");
      DmaFollower next_frame_dma(chain.data(), 0, chain.size());
      renderer.render_from_texture_bucket(next_frame_dma, &state, context);
      const auto next_frame = renderer.stats();
      check(next_frame.eyes == 2 && next_frame.duplicate_slot_writes == 0 &&
                next_frame.command_buffers_completed == 1,
            "the same slots can be composed once in the next frame");
    }

    bool detached_all_eye_slots = true;
    for (u32 slot = METAL_EYE_BASE_BLOCK_JAK1;
         slot < METAL_EYE_BASE_BLOCK_JAK1 + METAL_NUM_EYE_PAIRS * 2; slot++) {
      const auto handle = texture_pool.lookup(slot);
      detached_all_eye_slots &= handle.has_value() && *handle == 0;
    }
    check(detached_all_eye_slots,
          "eye teardown unloads every pool publication while the pool is live");
    check(metal_texture_live_count() == initial_live_textures + 1,
          "eye teardown releases all forty registered render targets");

    {
      std::lock_guard<std::mutex> pool_lock(texture_pool.mutex());
      texture_pool.unload_texture(source_id, source_handle);
    }
    metal_texture_release(source_handle);
    check(metal_texture_live_count() == initial_live_textures,
          "the synthetic source cleanup restores the texture registry baseline");

    if (failures) {
      std::printf("FAIL: %d Jak II Metal eye renderer checks failed\n", failures);
      return 1;
    }
    std::printf("PASS: Jak II Metal eye publication and lifecycle checks passed\n");
    return 0;
  }
}
