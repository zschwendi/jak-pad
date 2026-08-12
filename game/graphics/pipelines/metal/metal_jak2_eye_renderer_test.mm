#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "common/dma/gs.h"

#include "game/graphics/pipelines/metal/metal_eye_renderer.h"
#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#import <Metal/Metal.h>

extern "C" const unsigned char g_goalpad_metallib[];
extern "C" const unsigned long g_goalpad_metallib_size;

namespace {

constexpr u32 kSourceTbp = 100;
constexpr u64 kEyeHash = 0x123456789abcdef0ull;
constexpr u32 kPrisBucket = 200;
constexpr u32 kPrisBucketOffset = kPrisBucket * 16;
constexpr u32 kPrisOrdinaryOffset = 0x4000;
constexpr u32 kPrisChunkOffset = 0x5000;

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
                     VifCode::Kind vif1,
                     u16 vif0_immediate = 0,
                     u16 vif1_immediate = 0) {
  const u64 tag = payload.size() / 16 |
                  (static_cast<u64>(DmaTag::Kind::CNT) << 28);
  const u64 transferred_tag = vif0_immediate | (static_cast<u64>(vif0) << 24) |
                              (static_cast<u64>(vif1_immediate) << 32) |
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

std::vector<u8> make_scissor(u32 pair) {
  std::vector<u8> data;
  const u64 tag = 1 | (1ull << 15) | (1ull << 60);
  append_qword(&data, tag, static_cast<u64>(GifTag::RegisterDescriptor::AD));
  const u64 y0 = pair * 32;
  const u64 y1 = y0 + 31;
  const u64 scissor = 31ull << 16 | y0 << 32 | y1 << 48;
  append_qword(&data, scissor, static_cast<u64>(GsRegisterAddress::SCISSOR_1));
  return data;
}

std::vector<u8> make_sprite(u64 hash, u32 pair) {
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
  const u64 y0 = static_cast<u64>((pair + 1) * 32 * 16);
  const u64 y1 = y0 + 32 * 16;
  append_qword(&data, 512ull | (y0 << 32), 0xffffffull << 4);
  append_qword(&data, 0, 0);
  append_qword(&data, 1024ull | (y1 << 32), 0xffffffull << 4);
  return data;
}

void append_eye_draw(std::vector<u8>* chain, u64 hash, u32 pair) {
  append_transfer(chain, make_scissor(pair), VifCode::Kind::NOP, VifCode::Kind::DIRECT, 0, 2);
  append_transfer(chain, make_sprite(hash, pair), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
}

std::vector<u8> make_eye_chain(u32 pair = 0) {
  std::vector<u8> chain;
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::FLUSHA,
                  VifCode::Kind::DIRECT, 0, 8);
  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);

  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, kEyeHash + pair, pair);  // pair/hash metadata
  append_eye_draw(&chain, 0, pair);                 // left iris
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right iris

  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // left pupil
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right pupil

  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // left lid
  append_transfer(&chain, make_adgif(kSourceTbp), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 6);
  append_eye_draw(&chain, 0, pair);  // right lid

  // get_draws stops before the fixed GS-state restore transfer.
  append_transfer(&chain, std::vector<u8>(128), VifCode::Kind::FLUSHA,
                  VifCode::Kind::DIRECT, 0, 8);
  append_transfer(&chain, std::vector<u8>(32), VifCode::Kind::NOP,
                  VifCode::Kind::DIRECT, 0, 2);
  return chain;
}

void put_tag(std::vector<u8>* data,
             u32 offset,
             DmaTag::Kind kind,
             u16 qwc,
             u32 address,
             u32 vif0,
             u32 vif1) {
  const u64 raw = qwc | (static_cast<u64>(kind) << 28) |
                  (static_cast<u64>(address) << 32);
  std::memcpy(data->data() + offset, &raw, sizeof(raw));
  std::memcpy(data->data() + offset + 8, &vif0, sizeof(vif0));
  std::memcpy(data->data() + offset + 12, &vif1, sizeof(vif1));
}

struct PrisFixture {
  std::vector<u8> data;
  metal_renderer::Jak2PrisEyeTextureUploadPlan plan;
};

PrisFixture make_pris_fixture(std::size_t chunk_count) {
  constexpr u32 kPcPort = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
  constexpr u32 kFlusha = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
  constexpr u32 kDirect = static_cast<u32>(VifCode::Kind::DIRECT) << 24;
  constexpr u64 kPageOffset = 0x200000;
  constexpr s64 kMode = -1;
  PrisFixture fixture;
  fixture.data.resize(0x8000);
  fixture.plan.bucket_id = kPrisBucket;
  fixture.plan.present = true;
  fixture.plan.ordinary.page_offset = kPageOffset;
  fixture.plan.ordinary.mode = kMode;
  fixture.plan.chunk_count = chunk_count;

  put_tag(&fixture.data, kPrisBucketOffset, DmaTag::Kind::NEXT, 0,
          kPrisOrdinaryOffset, 0, 0);
  put_tag(&fixture.data, kPrisOrdinaryOffset, DmaTag::Kind::CNT, 1, 0,
          kPcPort, 3);
  std::memcpy(fixture.data.data() + kPrisOrdinaryOffset + 16, &kPageOffset,
              sizeof(kPageOffset));
  std::memcpy(fixture.data.data() + kPrisOrdinaryOffset + 24, &kMode, sizeof(kMode));

  u32 chunk_offset = kPrisChunkOffset;
  put_tag(&fixture.data, kPrisOrdinaryOffset + 32, DmaTag::Kind::NEXT, 0,
          chunk_offset, 0, 0);
  for (std::size_t i = 0; i < chunk_count; ++i) {
    const auto chunk_data = make_eye_chain(static_cast<u32>(i));
    std::memcpy(fixture.data.data() + chunk_offset, chunk_data.data(), chunk_data.size());
    const u32 linker_offset = chunk_offset + static_cast<u32>(chunk_data.size());
    put_tag(&fixture.data, linker_offset, DmaTag::Kind::NEXT, 0, linker_offset + 16, 0, 0);

    auto& chunk = fixture.plan.chunks[i];
    chunk.resolution = metal_renderer::Jak2PrisEyeResolution::Eye32;
    chunk.pair_index = static_cast<u32>(i);
    chunk.start_transfer_index = 3 + static_cast<u32>(i) * 27;
    chunk.start_relative_tag_offset = chunk_offset - kPrisBucketOffset;
    chunk.linker_transfer_index = chunk.start_transfer_index +
                                  metal_renderer::kJak2PrisEyeChunkTransferCount;
    chunk.linker_relative_tag_offset = linker_offset - kPrisBucketOffset;
    chunk.transfer_count = metal_renderer::kJak2PrisEyeChunkTransferCount;
    chunk.payload_bytes = metal_renderer::kJak2PrisEyeChunkPayloadBytes;
    chunk.eye_slot_mask = 3ull << (i * 2);
    fixture.plan.eye_slot_mask |= chunk.eye_slot_mask;
    chunk_offset = linker_offset + 16;
  }

  fixture.plan.direct_reset_transfer_index = 3 + static_cast<u32>(chunk_count) * 27;
  fixture.plan.direct_reset_relative_tag_offset = chunk_offset - kPrisBucketOffset;
  put_tag(&fixture.data, chunk_offset, DmaTag::Kind::CNT, 10, 0, kFlusha,
          kDirect | 10);
  const u32 terminal_offset = chunk_offset + 176;
  fixture.plan.terminal_transfer_index = fixture.plan.direct_reset_transfer_index + 1;
  fixture.plan.terminal_relative_tag_offset = terminal_offset - kPrisBucketOffset;
  put_tag(&fixture.data, terminal_offset, DmaTag::Kind::NEXT, 0,
          kPrisBucketOffset + 16, 0, 0);
  return fixture;
}

struct HostBucketCounter {
  u32 calls = 0;
  u32 bucket_id = 0;
};

void count_host_bucket(void* opaque, u32 bucket_id) {
  auto* counter = static_cast<HostBucketCounter*>(opaque);
  counter->calls++;
  counter->bucket_id = bucket_id;
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

      MetalJak2PrisEyeBucketRenderer pris_renderer("jak2-pris-eye-200", kPrisBucket);
      state.eye_renderer = &renderer;
      state.buckets_base = 0;
      state.next_bucket = kPrisBucketOffset + 16;
      state.host_bucket_callback = count_host_bucket;

      auto one_chunk = make_pris_fixture(1);
      HostBucketCounter host_counter;
      state.host_bucket_context = &host_counter;
      state.jak2_pris_eye_plans = &one_chunk.plan;
      state.jak2_pris_eye_plan_count = 1;
      renderer.start_frame();
      DmaFollower one_chunk_dma(one_chunk.data.data(), kPrisBucketOffset,
                                one_chunk.data.size());
      pris_renderer.render(one_chunk_dma, &state, context);
      const auto one_chunk_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPrisBucket &&
                one_chunk_dma.current_tag_offset() == state.next_bucket &&
                one_chunk_stats.eyes == 2 && one_chunk_stats.draw_calls == 8 &&
                one_chunk_stats.triangles == 16 && one_chunk_stats.missing_textures == 0 &&
                one_chunk_stats.unexpected_dma == 0 &&
                one_chunk_stats.duplicate_slot_writes == 0 &&
                one_chunk_stats.command_buffers_committed == 1 &&
                one_chunk_stats.command_buffers_completed == 1 &&
                one_chunk_stats.command_buffer_errors == 0,
            "the PRIS bucket callback runs once before one exact detached eye chunk completes");

      auto two_chunks = make_pris_fixture(2);
      host_counter = {};
      state.jak2_pris_eye_plans = &two_chunks.plan;
      renderer.start_frame();
      DmaFollower two_chunk_dma(two_chunks.data.data(), kPrisBucketOffset,
                                two_chunks.data.size());
      pris_renderer.render(two_chunk_dma, &state, context);
      const auto two_chunk_stats = renderer.stats();
      check(host_counter.calls == 1 && host_counter.bucket_id == kPrisBucket &&
                two_chunk_dma.current_tag_offset() == state.next_bucket &&
                two_chunk_stats.eyes == 4 && two_chunk_stats.draw_calls == 16 &&
                two_chunk_stats.triangles == 32 && two_chunk_stats.missing_textures == 0 &&
                two_chunk_stats.unexpected_dma == 0 &&
                two_chunk_stats.duplicate_slot_writes == 0 &&
                two_chunk_stats.command_buffers_committed == 2 &&
                two_chunk_stats.command_buffers_completed == 2 &&
                two_chunk_stats.command_buffer_errors == 0,
            "the PRIS renderer follows both planned chunks and consumes every terminal shape");
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
