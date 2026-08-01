#include "metal_chain_replay.h"

#include <set>

#include "common/dma/dma_chain_read.h"
#include "common/dma/dma_copy.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"

#include "game/graphics/opengl_renderer/buckets.h"

#include "fmt/format.h"
#include "third-party/magic_enum.hpp"

namespace metal_chain_replay {

bool load_capture(const std::string& path, LoadedChain* out, std::string* error) {
  if (!fs::exists(path)) {
    *error = fmt::format("capture file does not exist: {}", path);
    return false;
  }
  auto file = file_util::read_binary_file(path);
  if (file.size() < 12) {
    *error = fmt::format("capture file too small ({} bytes)", file.size());
    return false;
  }
  Serializer ser(file.data(), file.size());
  ser.from_ptr(&out->start_offset);
  ser.from_pod_vector(&out->data);
  if (!ser.get_load_finished()) {
    *error = fmt::format("capture file has {} trailing bytes", ser.data_size() - 12 - out->data.size());
    return false;
  }
  if (out->data.empty() || out->data.size() % FixedChunkDmaCopier::chunk_size != 0) {
    *error = fmt::format("capture payload ({} bytes) is not a multiple of the {} byte chunk size",
                         out->data.size(), FixedChunkDmaCopier::chunk_size);
    return false;
  }
  if (out->start_offset >= out->data.size()) {
    *error = fmt::format("start offset {:#x} outside the {} byte payload", out->start_offset,
                         out->data.size());
    return false;
  }
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

}  // namespace metal_chain_replay
