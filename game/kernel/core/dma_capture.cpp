/*!
 * @file dma_capture.cpp
 * `__send-gfx-dma-chain`: measure the DMA chain a frame built, and write chosen frames to a file.
 *
 * This is the seam where a frame's work leaves GOAL. `engine/draw/drawable.gc` builds a DMA chain
 * in EE main memory and hands its address to `__send-gfx-dma-chain`; upstream passes it to the
 * renderer, which walks it with `FixedChunkDmaCopier` (common/dma/dma_copy.h) and draws it.
 *
 * There is no renderer here, so nothing is drawn. What this does instead is run the same copier -
 * which is where the chain is validated, since following it means reading every tag - then walk
 * the copy a second time. Jak 1 follows its renderer's bucket dispatch and records what each bucket
 * received. Later games use a direct bucket array with a different envelope, so until their
 * renderer is present this seam records only game-neutral tag, payload and texture-upload totals;
 * that second walk must agree with the copier before the chain is called well formed.
 *
 * A capture writes one frame to a file so the renderer track has a real frame of Jak 1 DMA to
 * replay without the rest of the runtime. A capture is the player's own game data rendered into
 * DMA commands, so it is written where the caller asks and never into the repository.
 *
 * ============================================================================================
 * Capture file format, version 2 ("GPDMACAP")
 * ============================================================================================
 *
 * Version 1 was the chain and nothing else: the bytes of
 * `FixedChunkDmaCopier::serialize_last_result`, always of the first chain after boot. Two things
 * were missing. The first chain draws nothing, and the chain alone is not enough to replay a
 * frame: the PC-port texture-upload packets in it carry EE addresses of GOAL `texture-page`
 * structures, which live in the level and global heaps, not in the chain. Version 2 adds a frame
 * selector and a snapshot of EE main memory.
 *
 * All integers are little-endian. All addresses are GOAL addresses: byte offsets from EE main
 * memory byte 0, which is what `ee_base_goal_address` below records.
 *
 * Header, 80 bytes:
 *
 *   off  size  field
 *   0    8     magic                 "GPDMACAP"
 *   8    4     version               2
 *   12   4     header_bytes          80 (where the first section may start; sections are located
 *                                    by the offsets below, not by this)
 *   16   4     frame                 1-based index of the __send-gfx-dma-chain call captured
 *   20   4     chunk_size            FixedChunkDmaCopier::chunk_size, 0x20000
 *   24   4     ee_mem_size           EE_MAIN_MEM_SIZE, 0x8000000
 *   28   4     ee_base_goal_address  GOAL address of EE image byte 0. Always 0; present so a
 *                                    reader never has to assume it.
 *   32   4     s7                    GOAL address of the symbol table's s7 register value, which
 *                                    GOAL-pointer arithmetic on the snapshot needs (it is what
 *                                    TexturePool::handle_upload_now calls `s7_ptr`)
 *   36   4     sections              bit 0: chain present. bit 1: EE snapshot present.
 *   40   8     chain_offset          file offset of the chain section
 *   48   8     chain_bytes
 *   56   8     ee_offset             file offset of the EE section, 0 when absent
 *   64   8     ee_bytes              0 when absent
 *   72   8     reserved              0
 *
 * Chain section - byte for byte the version 1 file, so a version 1 reader works unchanged once it
 * seeks to `chain_offset`:
 *
 *   u32   start_offset      GOAL address, within the payload below, of the chain's first tag
 *   u64   payload_bytes     always a multiple of chunk_size
 *   u8    payload[payload_bytes]
 *
 * The payload is the chunks of EE memory the chain touched, packed together, with every address
 * inside a tag rewritten to point into the packed copy. It is not at its original address: this
 * is `FixedChunkDmaCopier`'s output and behaves exactly as it does for the OpenGL renderer.
 *
 * EE section - EE main memory as it stood when the frame's chain was handed over, stored as the
 * same fixed chunks, with the all-zero ones left out:
 *
 *   u32   chunk_count       ee_mem_size / chunk_size
 *   u32   stored_count      chunks present below
 *   u32   compression       0 = raw, 1 = each chunk compressed on its own with LZO (lzokay)
 *   u32   reserved          0
 *   then stored_count directory entries, ascending chunk_index:
 *     u32 chunk_index
 *     u32 stored_bytes      compressed size, or chunk_size when compression is 0
 *   then the stored bytes of each chunk, in the same order, back to back with no padding.
 *
 * To read it: allocate ee_mem_size zeroed bytes, then for each entry decompress its stored bytes
 * into `image + chunk_index * chunk_size`, expecting exactly chunk_size out. Chunks that are not
 * in the directory were all zero. GOAL address A of the snapshot is then `image[A]`; the chain
 * section's addresses are *not* in this space.
 *
 * The first EE_MAIN_MEM_LOW_PROTECT (512 kB, four chunks) is never stored. That is the PS2
 * kernel's memory, which GOAL never touches and which this runtime maps PROT_NONE so a GOAL null
 * dereference crashes instead of corrupting; it is zero, and it cannot even be read to check.
 */

#include "game/kernel/core/dma_capture.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/kernel_game.h"
#include "game/runtime.h"

#include "third-party/lzokay/lzokay.hpp"

namespace {

constexpr char kMagic[8] = {'G', 'P', 'D', 'M', 'A', 'C', 'A', 'P'};
constexpr u32 kVersion = 2;
constexpr u32 kHeaderBytes = 80;
constexpr u32 kSectionChain = 1;
constexpr u32 kSectionEe = 2;
constexpr u32 kCompressionLzo = 1;

struct BucketSummary {
  int transfers = 0;
  u32 payload_bytes = 0;
  int texture_uploads = 0;
};

struct ChainSummary {
  int tags = 0;
  u32 payload_bytes = 0;
  int texture_uploads = 0;
  std::vector<BucketSummary> buckets;
  std::vector<u32> texture_page_addresses;
  std::string problem;  // empty when the selected game's validation completed
};

/*!
 * Walk a copied chain the way `OpenGLRenderer::dispatch_buckets_jak1` does: an initial CALL to the
 * default-registers chain, then an array of 16-byte bucket entries, each bucket's transfers ending
 * where the next entry begins. Records what each bucket was given instead of drawing it.
 *
 * A chain that does not have that shape is reported rather than guessed at: `problem` is set and
 * whatever was counted before the mismatch is kept.
 */
ChainSummary summarize_jak1_chain(const DmaData& chain) {
  ChainSummary out;
  DmaFollower dma(chain.data.data(), chain.start_offset);

  if (dma.current_tag().kind != DmaTag::Kind::CALL) {
    out.problem = "chain does not start with the initial CALL to the default-registers chain";
    return out;
  }
  const u32 buckets_base = dma.current_tag_offset() + 16;
  dma.read_and_advance();
  const u32 default_regs = dma.current_tag_offset();
  if (dma.current_tag().kind != DmaTag::Kind::CNT || dma.current_tag().qwc != 10) {
    out.problem = "default-registers chain is not a CNT of 10 quadwords";
    return out;
  }
  dma.read_and_advance();
  if (dma.current_tag().kind != DmaTag::Kind::RET) {
    out.problem = "default-registers chain does not end with RET";
    return out;
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() != buckets_base) {
    out.problem = "the initial CALL did not return to the bucket array";
    return out;
  }

  u32 next_bucket = buckets_base + 16;
  while (!dma.ended()) {
    BucketSummary bucket;
    while (!dma.ended() && dma.current_tag_offset() != next_bucket) {
      const auto transfer = dma.read_and_advance();
      out.tags++;
      bucket.payload_bytes += transfer.size_bytes;
      if (transfer.size_bytes > 0) {
        bucket.transfers++;
      }
      // the PC port's texture upload: one quadword holding { u64 page; s64 mode; }, where `page`
      // is the GOAL address of a texture-page structure in EE memory
      if (transfer.size_bytes == 16 && transfer.vifcode0().kind == VifCode::Kind::PC_PORT &&
          transfer.vif1() == 3) {
        bucket.texture_uploads++;
        u64 page = 0;
        std::memcpy(&page, transfer.data, sizeof(page));
        out.texture_page_addresses.push_back((u32)page);
      }
      if (dma.current_tag_offset() == default_regs) {
        // the bucket-ending bounce back through the default-registers chain is structure, not
        // content: a CNT of the same 10 quadwords and its RET
        dma.read_and_advance();
        dma.read_and_advance();
      }
    }
    out.payload_bytes += bucket.payload_bytes;
    out.texture_uploads += bucket.texture_uploads;
    out.buckets.push_back(bucket);
    next_bucket += 16;
  }
  return out;
}

/*!
 * Validate a copied chain without assigning any game's bucket topology to it. This is the bounded
 * Jak 2 frontier: the same copied bytes must be independently walkable to END/REFE and reproduce
 * the copier's tag and payload totals. Drawing and per-bucket interpretation remain renderer work.
 */
ChainSummary summarize_generic_chain(const DmaData& chain) {
  ChainSummary out;
  DmaFollower dma(chain.data.data(), chain.start_offset);
  DmaTag::Kind terminal = DmaTag::Kind::CNT;

  while (!dma.ended()) {
    terminal = dma.current_tag().kind;
    const auto transfer = dma.read_and_advance();
    out.tags++;
    out.payload_bytes += transfer.size_bytes;
    if (transfer.size_bytes == 16 && transfer.vifcode0().kind == VifCode::Kind::PC_PORT &&
        transfer.vif1() == 3) {
      out.texture_uploads++;
      u64 page = 0;
      std::memcpy(&page, transfer.data, sizeof(page));
      out.texture_page_addresses.push_back((u32)page);
    }
  }

  if (terminal != DmaTag::Kind::END && terminal != DmaTag::Kind::REFE) {
    out.problem = "copied chain did not terminate with END or REFE";
  } else if (out.tags != chain.stats.num_tags) {
    out.problem = "second walk did not reproduce the copier's tag count";
  } else if (out.payload_bytes != (u32)chain.stats.num_data_bytes) {
    out.problem = "second walk did not reproduce the copier's payload byte count";
  }
  return out;
}

ChainSummary summarize_chain(const DmaData& chain) {
  if (goal_game_version() == GameVersion::Jak1) {
    return summarize_jak1_chain(chain);
  }
  return summarize_generic_chain(chain);
}

void append_u32(std::vector<u8>& out, u32 value) {
  const u8 bytes[4] = {(u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24)};
  out.insert(out.end(), bytes, bytes + 4);
}

void append_u64(std::vector<u8>& out, u64 value) {
  append_u32(out, (u32)value);
  append_u32(out, (u32)(value >> 32));
}

bool chunk_is_zero(const u8* chunk, u32 size) {
  for (u32 at = 0; at < size; at += 8) {
    u64 word;
    std::memcpy(&word, chunk + at, sizeof(word));
    if (word) {
      return false;
    }
  }
  return true;
}

/*!
 * EE main memory as the chunks the chain copier uses, with the all-zero ones left out and each of
 * the rest compressed on its own. See the format comment at the top of this file.
 */
std::vector<u8> build_ee_section(u32* out_stored_chunks) {
  constexpr u32 chunk_size = FixedChunkDmaCopier::chunk_size;
  const u32 chunk_count = EE_MAIN_MEM_SIZE / chunk_size;
  // the low 512 kB is mapped PROT_NONE: reading it to see whether it is zero would fault
  static_assert(EE_MAIN_MEM_LOW_PROTECT % chunk_size == 0);
  const u32 first_readable_chunk = EE_MAIN_MEM_LOW_PROTECT / chunk_size;
  const u8* memory = (const u8*)g_ee_main_mem;

  std::vector<u32> index;
  std::vector<u32> stored_bytes;
  std::vector<u8> payload;
  std::vector<u8> scratch(lzokay::compress_worst_size(chunk_size));
  lzokay::Dict<> dict;

  for (u32 chunk = first_readable_chunk; chunk < chunk_count; chunk++) {
    const u8* source = memory + (size_t)chunk * chunk_size;
    if (chunk_is_zero(source, chunk_size)) {
      continue;
    }
    std::size_t compressed = 0;
    const auto result =
        lzokay::compress(source, chunk_size, scratch.data(), scratch.size(), compressed, dict);
    if (result != lzokay::EResult::Success) {
      lg::error("[dma-capture] LZO refused chunk {}: giving up on the EE snapshot", chunk);
      *out_stored_chunks = 0;
      return {};
    }
    index.push_back(chunk);
    stored_bytes.push_back((u32)compressed);
    payload.insert(payload.end(), scratch.data(), scratch.data() + compressed);
  }

  std::vector<u8> out;
  append_u32(out, chunk_count);
  append_u32(out, (u32)index.size());
  append_u32(out, kCompressionLzo);
  append_u32(out, 0);
  for (size_t i = 0; i < index.size(); i++) {
    append_u32(out, index[i]);
    append_u32(out, stored_bytes[i]);
  }
  out.insert(out.end(), payload.begin(), payload.end());
  *out_stored_chunks = (u32)index.size();
  return out;
}

/*!
 * Every texture-page address the frame's chain names has to be inside a chunk the snapshot kept,
 * or the capture cannot be replayed. Zero chunks are left out, and a live texture-page structure
 * is not zero, so this should never fire - which is exactly why it is worth saying when it does.
 */
void check_texture_pages_are_in_the_snapshot(const std::vector<u32>& pages) {
  constexpr u32 chunk_size = FixedChunkDmaCopier::chunk_size;
  const u8* memory = (const u8*)g_ee_main_mem;
  for (u32 page : pages) {
    if (page < EE_MAIN_MEM_LOW_PROTECT || page + sizeof(u64) > (u32)EE_MAIN_MEM_SIZE) {
      lg::error(
          "[dma-capture] texture-page address {:#x} is outside the readable part of EE main "
          "memory",
          page);
      continue;
    }
    const u32 chunk = page / chunk_size;
    if (chunk_is_zero(memory + (size_t)chunk * chunk_size, chunk_size)) {
      lg::error("[dma-capture] texture page {:#x} is in an all-zero chunk; the snapshot omits it",
                page);
    }
  }
}

FixedChunkDmaCopier* g_copier = nullptr;
goal_gfx_dma_stats g_stats;
std::vector<goal_gfx_dma_frame_summary> g_frames;
std::map<int, std::vector<goal_gfx_dma_bucket_summary>> g_captured_buckets;
std::map<int, std::string> g_requested_captures;
std::string g_threshold_dir;
u32 g_threshold_payload_bytes = 0;
int g_threshold_remaining = 0;

bool write_capture(const std::string& path, int frame, const ChainSummary& summary) {
  if (goal_game_version() != GameVersion::Jak1) {
    lg::error("[dma-capture] capture files are not defined for this game version");
    return false;
  }
  Serializer chain_serializer;
  g_copier->serialize_last_result(chain_serializer);
  const auto chain = chain_serializer.get_save_result();

  u32 stored_chunks = 0;
  const std::vector<u8> ee = build_ee_section(&stored_chunks);
  check_texture_pages_are_in_the_snapshot(summary.texture_page_addresses);

  std::vector<u8> header;
  header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
  append_u32(header, kVersion);
  append_u32(header, kHeaderBytes);
  append_u32(header, (u32)frame);
  append_u32(header, FixedChunkDmaCopier::chunk_size);
  append_u32(header, (u32)EE_MAIN_MEM_SIZE);
  append_u32(header, 0);  // ee_base_goal_address
  append_u32(header, s7.offset);
  append_u32(header, kSectionChain | (ee.empty() ? 0 : kSectionEe));
  append_u64(header, kHeaderBytes);
  append_u64(header, (u64)chain.second);
  append_u64(header, ee.empty() ? 0 : kHeaderBytes + (u64)chain.second);
  append_u64(header, (u64)ee.size());
  append_u64(header, 0);  // reserved
  ASSERT(header.size() == kHeaderBytes);

  const auto parent = fs::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
      lg::error("[dma-capture] cannot make {}: {}", parent.string(), ec.message());
    }
  }
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    lg::error("[dma-capture] cannot write {}: {}", path, std::strerror(errno));
    return false;
  }
  const bool ok = std::fwrite(header.data(), 1, header.size(), fp) == header.size() &&
                  std::fwrite(chain.first, 1, chain.second, fp) == chain.second &&
                  std::fwrite(ee.data(), 1, ee.size(), fp) == ee.size();
  std::fclose(fp);
  if (!ok) {
    lg::error("[dma-capture] short write to {}: {}", path, std::strerror(errno));
    return false;
  }

  const u64 total = header.size() + chain.second + ee.size();
  g_stats.captures++;
  g_stats.captured_bytes += (u32)total;
  lg::warn(
      "[dma-capture] frame {}: wrote {} bytes to {} ({} byte chain, {} byte EE snapshot of {} "
      "chunks, {} byte payload in {} buckets, {} texture uploads)",
      frame, total, path, chain.second, ee.size(), stored_chunks, summary.payload_bytes,
      summary.buckets.size(), summary.texture_uploads);
  return true;
}

/*!
 * `(__send-gfx-dma-chain bank chain)`. `bank` is the PS2 DMA channel register address, which means
 * nothing without the hardware; `chain` is the GOAL address the chain starts at.
 */
u64 send_gfx_dma_chain(u32 bank, u32 chain) {
  (void)bank;
  if (!g_copier) {
    g_copier = new FixedChunkDmaCopier(EE_MAIN_MEM_SIZE);
  }

  const auto& result = g_copier->run(g_ee_main_mem, chain, false);
  g_stats.chains++;
  const int frame = g_stats.chains;
  g_stats.last_bytes = (u32)result.data.size();
  if (g_stats.last_bytes > g_stats.largest_bytes) {
    g_stats.largest_bytes = g_stats.last_bytes;
  }

  const ChainSummary summary = summarize_chain(result);
  if (!summary.problem.empty()) {
    g_stats.malformed_chains++;
    lg::error("[dma-capture] frame {}: {}", frame, summary.problem);
  } else {
    g_stats.well_formed_chains++;
  }
  g_stats.last_payload_bytes = summary.payload_bytes;
  g_stats.last_texture_uploads = summary.texture_uploads;
  if (summary.payload_bytes > g_stats.largest_payload_bytes) {
    g_stats.largest_payload_bytes = summary.payload_bytes;
    g_stats.largest_payload_frame = frame;
    g_stats.largest_payload_uploads = summary.texture_uploads;
  }

  goal_gfx_dma_frame_summary record;
  record.frame = frame;
  record.copied_bytes = g_stats.last_bytes;
  record.payload_bytes = summary.payload_bytes;
  record.tags = summary.tags;
  record.texture_uploads = summary.texture_uploads;
  record.buckets = summary.problem.empty() ? (int)summary.buckets.size() : 0;
  record.well_formed = summary.problem.empty();
  g_frames.push_back(record);

  std::string path;
  const auto requested = g_requested_captures.find(frame);
  if (requested != g_requested_captures.end()) {
    path = requested->second;
    g_requested_captures.erase(requested);
  } else if (g_threshold_remaining > 0 && summary.payload_bytes >= g_threshold_payload_bytes) {
    path = g_threshold_dir + "/dma-frame-" + std::to_string(frame) + ".gpdma";
    g_threshold_remaining--;
  }
  if (!path.empty() && write_capture(path, frame, summary)) {
    auto& buckets = g_captured_buckets[frame];
    for (size_t i = 0; i < summary.buckets.size(); i++) {
      goal_gfx_dma_bucket_summary entry;
      entry.bucket = (int)i;
      entry.transfers = summary.buckets[i].transfers;
      entry.payload_bytes = summary.buckets[i].payload_bytes;
      entry.texture_uploads = summary.buckets[i].texture_uploads;
      buckets.push_back(entry);
    }
  }
  return 0;
}

}  // namespace

extern "C" {

void goal_gfx_dma_install(void) {
  g_stats = goal_gfx_dma_stats();
  g_frames.clear();
  g_captured_buckets.clear();
  g_requested_captures.clear();
  g_threshold_dir.clear();
  g_threshold_remaining = 0;
  goal_game_make_function_symbol("__send-gfx-dma-chain", (void*)send_gfx_dma_chain);
}

int goal_gfx_dma_capture_chain_now(const void* ee_base,
                                   uint32_t chain_offset,
                                   int frame,
                                   const char* path) {
  if (goal_game_version() != GameVersion::Jak1) {
    lg::error("[dma-capture] capture files are currently Jak 1 only");
    return 0;
  }
  if (!ee_base || !path || !path[0]) {
    return 0;
  }
  if (!g_copier) {
    g_copier = new FixedChunkDmaCopier(EE_MAIN_MEM_SIZE);
  }
  // The same walk and the same serialized result the measuring seam writes, so a file from here
  // is byte-identical in structure to one from --capture-dma and replays the same way.
  const auto& result = g_copier->run((const u8*)ee_base, chain_offset, false);
  const ChainSummary summary = summarize_chain(result);
  if (!summary.problem.empty()) {
    lg::error("[dma-capture] frame {}: {}", frame, summary.problem);
  }
  if (!write_capture(path, frame, summary)) {
    return 0;
  }
  auto& buckets = g_captured_buckets[frame];
  buckets.clear();
  for (size_t i = 0; i < summary.buckets.size(); i++) {
    goal_gfx_dma_bucket_summary entry;
    entry.bucket = (int)i;
    entry.transfers = summary.buckets[i].transfers;
    entry.payload_bytes = summary.buckets[i].payload_bytes;
    entry.texture_uploads = summary.buckets[i].texture_uploads;
    buckets.push_back(entry);
  }
  return 1;
}

void goal_gfx_dma_capture_frame_to_file(const char* path, int frame) {
  if (goal_game_version() != GameVersion::Jak1) {
    lg::error("[dma-capture] capture files are currently Jak 1 only");
    return;
  }
  if (!path || !path[0]) {
    return;
  }
  g_requested_captures[frame < 1 ? 1 : frame] = path;
}

void goal_gfx_dma_capture_frames_to_dir(const char* dir, const int* frames, int count) {
  if (goal_game_version() != GameVersion::Jak1) {
    lg::error("[dma-capture] capture files are currently Jak 1 only");
    return;
  }
  if (!dir || !dir[0] || !frames) {
    return;
  }
  for (int i = 0; i < count; i++) {
    const int frame = frames[i] < 1 ? 1 : frames[i];
    g_requested_captures[frame] =
        std::string(dir) + "/dma-frame-" + std::to_string(frame) + ".gpdma";
  }
}

void goal_gfx_dma_capture_frames_over(const char* dir, uint32_t min_payload_bytes, int count) {
  if (goal_game_version() != GameVersion::Jak1) {
    lg::error("[dma-capture] capture files are currently Jak 1 only");
    return;
  }
  if (!dir || !dir[0] || count < 1) {
    return;
  }
  g_threshold_dir = dir;
  g_threshold_payload_bytes = min_payload_bytes;
  g_threshold_remaining = count;
}

int goal_gfx_dma_frame_count(void) {
  return (int)g_frames.size();
}

int goal_gfx_dma_get_frame(int frame, goal_gfx_dma_frame_summary* out) {
  if (frame < 1 || frame > (int)g_frames.size() || !out) {
    return 0;
  }
  *out = g_frames[frame - 1];
  return 1;
}

int goal_gfx_dma_get_bucket(int frame, int bucket, goal_gfx_dma_bucket_summary* out) {
  const auto found = g_captured_buckets.find(frame);
  if (found == g_captured_buckets.end() || bucket < 0 || bucket >= (int)found->second.size() ||
      !out) {
    return 0;
  }
  *out = found->second[bucket];
  return 1;
}

void goal_gfx_dma_get_stats(goal_gfx_dma_stats* out) {
  *out = g_stats;
}

}  // extern "C"
