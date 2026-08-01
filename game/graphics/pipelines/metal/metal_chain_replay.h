#pragma once

/*!
 * @file metal_chain_replay.h
 * Loading and preparation of captured DMA chains for offline replay.
 *
 * The runtime track's `__send-gfx-dma-chain` capture (game/kernel/core/
 * dma_capture.cpp on the runtime branch) serializes one frame's chain in
 * `FixedChunkDmaCopier::serialize_last_result` form: a u32 start offset
 * followed by a POD vector of the copied 128 kB chunks, with every DMA tag
 * address already rewritten to an offset inside that buffer. This module reads
 * that file, relocates the chain to a chosen base inside a fake EE memory (the
 * copier refuses addresses below its low-memory protect), and produces a
 * per-bucket payload inventory so a replay reports exactly what the frame
 * carried. Plain C++ so it can be built and tested without Metal.
 */

#include <string>
#include <vector>

#include "common/common_types.h"

namespace metal_chain_replay {

struct LoadedChain {
  u32 start_offset = 0;
  std::vector<u8> data;
};

// Reads a capture file. Returns false with a message on malformed input.
bool load_capture(const std::string& path, LoadedChain* out, std::string* error);

// Adds `base` to every DMA tag address (and the start offset) so the chain can
// be placed at `base` inside a larger memory. `base` must be a multiple of the
// FixedChunkDmaCopier chunk size so re-copying preserves the layout.
bool rebase_chain(LoadedChain* chain, u32 base, std::string* error);

struct BucketInventory {
  int bucket = 0;
  int transfers = 0;        // transfers that carried data or vif codes
  u64 payload_bytes = 0;    // DMA data bytes, excluding the empty-bucket structure
  int pc_port_uploads = 0;  // 16-byte PC_PORT texture upload packets
};

struct ChainInventory {
  u8 fog_color[4] = {0, 0, 0, 0};
  std::vector<BucketInventory> buckets;  // all 70 Jak 1 buckets, in order
  u64 total_payload = 0;
  int total_pc_port_uploads = 0;
};

// Walks the chain the way dispatch_buckets_jak1 does (initial CALL to the
// default-registers chain, then the 70-slot bucket array) and reports what
// each bucket carried. Diagnostic only; renders nothing.
bool inventory_jak1(const LoadedChain& chain, ChainInventory* out, std::string* error);

// Human-readable Jak 1 bucket name for the inventory printout.
std::string jak1_bucket_name(int bucket);

}  // namespace metal_chain_replay
