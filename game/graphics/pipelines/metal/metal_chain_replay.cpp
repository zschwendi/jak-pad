#include "metal_chain_replay.h"

#include <cstring>
#include <set>

#include "common/dma/dma_chain_read.h"
#include "common/dma/dma_copy.h"
#include "common/goal_constants.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"

#include "game/graphics/opengl_renderer/buckets.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"
#include "third-party/lzokay/lzokay.hpp"
#include "third-party/magic_enum.hpp"

namespace metal_chain_replay {

namespace {

constexpr char kMagic[8] = {'G', 'P', 'D', 'M', 'A', 'C', 'A', 'P'};
constexpr u32 kChunkSize = FixedChunkDmaCopier::chunk_size;

// The lowest address a replayed chain may occupy: the copier refuses anything
// below EE_MAIN_MEM_LOW_PROTECT, which is also never captured.
constexpr u32 kLowestChainBase = 0x100000;

// Version 2 header, read field by field so no struct layout is assumed.
struct CaptureHeader {
  u32 version = 0;
  u32 header_bytes = 0;
  u32 frame = 0;
  u32 chunk_size = 0;
  u32 ee_mem_size = 0;
  u32 ee_base_goal_address = 0;
  u32 s7 = 0;
  u32 sections = 0;
  u64 chain_offset = 0;
  u64 chain_bytes = 0;
  u64 ee_offset = 0;
  u64 ee_bytes = 0;
};

template <typename T>
T read_at(const std::vector<u8>& file, size_t offset) {
  T value;
  memcpy(&value, file.data() + offset, sizeof(T));
  return value;
}

bool parse_chain_section(const u8* data, size_t size, LoadedChain* out, std::string* error) {
  if (size < 12) {
    *error = fmt::format("chain section too small ({} bytes)", size);
    return false;
  }
  Serializer ser(data, size);
  ser.from_ptr(&out->start_offset);
  ser.from_pod_vector(&out->data);
  if (!ser.get_load_finished()) {
    *error = fmt::format("chain section has {} trailing bytes", size - 12 - out->data.size());
    return false;
  }
  if (out->data.empty() || out->data.size() % kChunkSize != 0) {
    *error = fmt::format("capture payload ({} bytes) is not a multiple of the {} byte chunk size",
                         out->data.size(), kChunkSize);
    return false;
  }
  if (out->start_offset >= out->data.size()) {
    *error = fmt::format("start offset {:#x} outside the {} byte payload", out->start_offset,
                         out->data.size());
    return false;
  }
  return true;
}

// Decompresses the EE section into `capture->ee_memory`, which is already sized
// and zeroed - chunks the capture left out were all zero.
bool parse_ee_section(const std::vector<u8>& file,
                      const CaptureHeader& header,
                      LoadedCapture* capture,
                      std::string* error) {
  if (header.ee_offset + header.ee_bytes > file.size() || header.ee_bytes < 16) {
    *error = "EE section is outside the file";
    return false;
  }
  size_t pos = header.ee_offset;
  u32 chunk_count = read_at<u32>(file, pos);
  u32 stored_count = read_at<u32>(file, pos + 4);
  u32 compression = read_at<u32>(file, pos + 8);
  pos += 16;

  if (chunk_count != header.ee_mem_size / header.chunk_size) {
    *error = fmt::format("EE chunk count {} does not match {} bytes of {} byte chunks", chunk_count,
                         header.ee_mem_size, header.chunk_size);
    return false;
  }
  if (compression > 1) {
    *error = fmt::format("unknown EE chunk compression {}", compression);
    return false;
  }
  if (pos + (size_t)stored_count * 8 > file.size()) {
    *error = "EE section directory is outside the file";
    return false;
  }

  std::vector<std::pair<u32, u32>> directory;  // chunk index, stored bytes
  directory.reserve(stored_count);
  for (u32 i = 0; i < stored_count; i++) {
    u32 index = read_at<u32>(file, pos);
    u32 bytes = read_at<u32>(file, pos + 4);
    pos += 8;
    if (index >= chunk_count) {
      *error = fmt::format("EE chunk index {} out of range", index);
      return false;
    }
    directory.emplace_back(index, bytes);
  }

  for (auto& [index, stored_bytes] : directory) {
    if (pos + stored_bytes > file.size()) {
      *error = fmt::format("EE chunk {} runs past the end of the file", index);
      return false;
    }
    u8* dest = capture->ee_memory.data() + (size_t)index * header.chunk_size;
    if (compression == 0) {
      if (stored_bytes != header.chunk_size) {
        *error = fmt::format("uncompressed EE chunk {} is {} bytes", index, stored_bytes);
        return false;
      }
      memcpy(dest, file.data() + pos, stored_bytes);
    } else {
      size_t out_size = 0;
      auto result =
          lzokay::decompress(file.data() + pos, stored_bytes, dest, header.chunk_size, out_size);
      if (result != lzokay::EResult::Success || out_size != header.chunk_size) {
        *error = fmt::format("EE chunk {} failed to decompress (result {}, {} of {} bytes)", index,
                             (int)result, out_size, header.chunk_size);
        return false;
      }
    }
    pos += stored_bytes;
    capture->ee_stored_chunks.push_back(index);
  }

  capture->has_ee_snapshot = true;
  return true;
}

}  // namespace

bool load_capture(const std::string& path, LoadedChain* out, std::string* error) {
  LoadedCapture capture;
  if (!load_capture_file(path, &capture, error)) {
    return false;
  }
  *out = std::move(capture.chain);
  return true;
}

bool load_capture_file(const std::string& path, LoadedCapture* out, std::string* error) {
  if (!fs::exists(path)) {
    *error = fmt::format("capture file does not exist: {}", path);
    return false;
  }
  auto file = file_util::read_binary_file(path);

  if (file.size() < 80 || memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) {
    // version 1: the chain section is the whole file, with no EE snapshot -
    // the chain's pointers into EE memory read as zeros.
    out->version = 1;
    out->ee_memory.assign(EE_MAIN_MEM_SIZE, 0);
    return parse_chain_section(file.data(), file.size(), &out->chain, error);
  }

  CaptureHeader header;
  header.version = read_at<u32>(file, 8);
  header.header_bytes = read_at<u32>(file, 12);
  header.frame = read_at<u32>(file, 16);
  header.chunk_size = read_at<u32>(file, 20);
  header.ee_mem_size = read_at<u32>(file, 24);
  header.ee_base_goal_address = read_at<u32>(file, 28);
  header.s7 = read_at<u32>(file, 32);
  header.sections = read_at<u32>(file, 36);
  header.chain_offset = read_at<u64>(file, 40);
  header.chain_bytes = read_at<u64>(file, 48);
  header.ee_offset = read_at<u64>(file, 56);
  header.ee_bytes = read_at<u64>(file, 64);

  if (header.version != 2) {
    *error = fmt::format("capture version {} is not supported (this build reads 1 and 2)",
                         header.version);
    return false;
  }
  if (header.chunk_size != kChunkSize) {
    *error = fmt::format("capture chunk size {:#x} does not match this build's {:#x}",
                         header.chunk_size, kChunkSize);
    return false;
  }
  if (header.ee_base_goal_address != 0) {
    *error = fmt::format("capture EE image does not start at GOAL address 0 (starts at {:#x})",
                         header.ee_base_goal_address);
    return false;
  }
  if ((header.sections & 1) == 0) {
    *error = "capture has no chain section";
    return false;
  }
  if (header.chain_offset + header.chain_bytes > file.size()) {
    *error = "chain section is outside the file";
    return false;
  }

  out->version = header.version;
  out->frame = header.frame;
  out->s7 = header.s7;
  out->ee_memory.assign(header.ee_mem_size, 0);

  if (!parse_chain_section(file.data() + header.chain_offset, header.chain_bytes, &out->chain,
                           error)) {
    return false;
  }
  if (header.sections & 2) {
    return parse_ee_section(file, header, out, error);
  }
  return true;
}

bool place_chain_in_ee(LoadedCapture* capture, std::string* error) {
  const u32 chunks_needed = (u32)(capture->chain.data.size() / kChunkSize);
  const u32 total_chunks = (u32)(capture->ee_memory.size() / kChunkSize);
  const u32 first_chunk = kLowestChainBase / kChunkSize;
  const std::set<u32> stored(capture->ee_stored_chunks.begin(), capture->ee_stored_chunks.end());

  // Longest run of chunks the snapshot left empty. Those were all zero in the
  // real EE memory, so nothing captured points into them; taking the longest
  // run keeps the chain as far from live data as the frame allows.
  u32 best_start = 0, best_len = 0, run_start = first_chunk, run_len = 0;
  for (u32 chunk = first_chunk; chunk <= total_chunks; chunk++) {
    if (chunk < total_chunks && !stored.count(chunk)) {
      if (run_len == 0) {
        run_start = chunk;
      }
      run_len++;
    } else {
      if (run_len > best_len) {
        best_len = run_len;
        best_start = run_start;
      }
      run_len = 0;
    }
  }
  if (best_len < chunks_needed) {
    *error = fmt::format("no free run of {} chunks in the {} MB EE image (longest is {})",
                         chunks_needed, capture->ee_memory.size() / (1 << 20), best_len);
    return false;
  }

  const u32 base = best_start * kChunkSize;
  if (!rebase_chain(&capture->chain, base, error)) {
    return false;
  }
  memcpy(capture->ee_memory.data() + base, capture->chain.data.data(), capture->chain.data.size());
  capture->chain_base = base;
  return true;
}

bool rebase_chain(LoadedChain* chain, u32 base, std::string* error) {
  if (base % FixedChunkDmaCopier::chunk_size != 0) {
    *error = fmt::format("rebase base {:#x} is not chunk aligned", base);
    return false;
  }
  // pass 1: follow the chain and collect the offset of every tag that carries
  // an address. CALL/RET can bounce through shared buffers, so dedupe.
  std::set<u32> tags_with_addr;
  DmaFollower dma(chain->data.data(), chain->start_offset);
  while (!dma.ended()) {
    u32 tag_offset = dma.current_tag_offset();
    if (tag_offset + 16 > chain->data.size()) {
      *error = fmt::format("chain walked outside the payload at {:#x}", tag_offset);
      return false;
    }
    if (dma.current_tag().addr != 0) {
      tags_with_addr.insert(tag_offset);
    }
    dma.read_and_advance();
  }
  // pass 2: relocate. The address is the low 31 bits of the tag's upper word;
  // the capture's fixups wrote it as a plain u32 (spr is always 0 here).
  for (u32 tag_offset : tags_with_addr) {
    u32 addr;
    memcpy(&addr, chain->data.data() + tag_offset + 4, 4);
    addr += base;
    memcpy(chain->data.data() + tag_offset + 4, &addr, 4);
  }
  chain->start_offset += base;
  return true;
}

std::string jak1_bucket_name(int bucket) {
  auto name = magic_enum::enum_name(jak1::BucketId(bucket));
  if (name.empty()) {
    return fmt::format("bucket-{}", bucket);
  }
  return std::string(name);
}

bool inventory_jak1(const LoadedChain& chain, ChainInventory* out, std::string* error) {
  DmaFollower dma(chain.data.data(), chain.start_offset);

  // initial CALL to the default-registers chain (fog color at data byte 144)
  if (dma.current_tag().kind != DmaTag::Kind::CALL) {
    *error = "chain does not start with the initial CALL";
    return false;
  }
  u32 buckets_base = dma.current_tag_offset() + 16;
  dma.read_and_advance();
  u32 default_regs = dma.current_tag_offset();
  auto default_tag = dma.current_tag();
  if (default_tag.kind != DmaTag::Kind::CNT || default_tag.qwc != 10) {
    *error = "default-registers chain is not a CNT of 10 quadwords";
    return false;
  }
  auto default_data = dma.read_and_advance();
  memcpy(out->fog_color, default_data.data + 144, 4);
  if (dma.current_tag().kind != DmaTag::Kind::RET) {
    *error = "default-registers chain does not end with RET";
    return false;
  }
  dma.read_and_advance();
  if (dma.current_tag_offset() != buckets_base) {
    *error = "initial CALL did not return to the bucket array";
    return false;
  }

  u32 next_bucket = buckets_base + 16;
  for (int bucket = 0; bucket < (int)jak1::BucketId::MAX_BUCKETS; bucket++) {
    BucketInventory inv;
    inv.bucket = bucket;
    while (dma.current_tag_offset() != next_bucket) {
      if (dma.ended()) {
        *error = fmt::format("chain ended inside bucket {}", bucket);
        return false;
      }
      auto transfer = dma.read_and_advance();
      inv.payload_bytes += transfer.size_bytes;
      if (transfer.size_bytes > 0) {
        inv.transfers++;
      }
      if (transfer.size_bytes == 16 && transfer.vifcode0().kind == VifCode::Kind::PC_PORT &&
          transfer.vif1() == 3) {
        inv.pc_port_uploads++;
        u32 page = 0;
        memcpy(&page, transfer.data, 4);  // TextureUpload { u64 page; s64 mode; }
        out->upload_page_addresses.push_back(page);
      }
      if (dma.current_tag_offset() == default_regs) {
        // the bucket-ending bounce through the default-registers chain is
        // structure, not content (same rule as MetalSkipRenderer)
        dma.read_and_advance();  // cnt
        dma.read_and_advance();  // ret
      }
    }
    out->total_payload += inv.payload_bytes;
    out->total_pc_port_uploads += inv.pc_port_uploads;
    out->buckets.push_back(inv);
    next_bucket += 16;
  }
  return true;
}

std::vector<u32> find_texture_pages(const LoadedCapture& capture,
                                    const std::vector<u32>& known_pages,
                                    u32* type_pointer_out) {
  *type_pointer_out = 0;
  const auto& ee = capture.ee_memory;
  if (!capture.has_ee_snapshot || known_pages.empty() || ee.size() < 8) {
    return {};
  }

  // the type pointer of a GOAL basic object is the word before it
  u32 type_pointer = 0;
  for (u32 page : known_pages) {
    if (page < 4 || (size_t)page + sizeof(GoalTexturePage) > ee.size()) {
      continue;
    }
    u32 candidate;
    memcpy(&candidate, ee.data() + page - 4, 4);
    if (candidate == 0 || (size_t)candidate >= ee.size()) {
      continue;
    }
    if (type_pointer == 0) {
      type_pointer = candidate;
    } else if (type_pointer != candidate) {
      // the chain's pages disagree about their type: refuse to guess
      return {};
    }
  }
  if (type_pointer == 0) {
    return {};
  }
  *type_pointer_out = type_pointer;

  // a page is usable if its texture count is plausible and every pointer it
  // would make the pool follow stays inside the image
  auto page_is_valid = [&](u32 page) {
    if (page < 4 || (size_t)page + sizeof(GoalTexturePage) > ee.size()) {
      return false;
    }
    GoalTexturePage tp;
    memcpy(&tp, ee.data() + page, sizeof(tp));
    if (tp.length <= 0 || tp.length > 4096) {
      return false;
    }
    if (tp.name_ptr == 0 || (size_t)tp.name_ptr >= ee.size()) {
      return false;
    }
    const size_t array_end = (size_t)page + sizeof(GoalTexturePage) + 4 * (size_t)tp.length;
    if (array_end > ee.size()) {
      return false;
    }
    for (int i = 0; i < tp.length; i++) {
      u32 tex_ptr;
      memcpy(&tex_ptr, ee.data() + page + sizeof(GoalTexturePage) + 4 * i, 4);
      // s7 (the "not present" marker) is small; anything else must be a real
      // in-range object address
      if (tex_ptr > capture.s7 && (size_t)tex_ptr + sizeof(GoalTexture) > ee.size()) {
        return false;
      }
    }
    return true;
  };

  std::set<u32> pages;
  for (u32 page : known_pages) {
    if (page_is_valid(page)) {
      pages.insert(page);
    }
  }
  for (size_t off = 4; off + 4 <= ee.size(); off += 4) {
    u32 word;
    memcpy(&word, ee.data() + off, 4);
    if (word != type_pointer) {
      continue;
    }
    u32 page = (u32)off + 4;
    if (page_is_valid(page)) {
      pages.insert(page);
    }
  }
  return std::vector<u32>(pages.begin(), pages.end());
}

}  // namespace metal_chain_replay
