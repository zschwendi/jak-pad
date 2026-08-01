#pragma once

/*!
 * @file metal_chain_replay.h
 * Loading and preparation of captured DMA chains for offline replay.
 *
 * The runtime track's `__send-gfx-dma-chain` capture (game/kernel/core/
 * dma_capture.cpp on the runtime branch) writes one frame of a real game to a
 * file. Two versions exist and both load here:
 *
 *  - version 1: the chain alone, in `FixedChunkDmaCopier::serialize_last_result`
 *    form (u32 start offset + POD vector of the copied 128 kB chunks, with every
 *    DMA tag address already rewritten to an offset inside that buffer).
 *  - version 2: an 80-byte "GPDMACAP" header, the same chain section byte for
 *    byte, and a snapshot of EE main memory stored as the same fixed chunks with
 *    the all-zero ones left out and each stored chunk LZO-compressed.
 *
 * The EE snapshot is what makes a replay resolve the chain's *pointers*: the
 * PC-port texture-upload packets carry EE addresses of GOAL `texture-page`
 * structs that live in the level and global heaps, not in the chain.
 *
 * This module reads a capture, reconstructs the EE image, places the chain in a
 * region of it the snapshot left empty (the copier refuses addresses below its
 * low-memory protect, and the chain must not land on real data), and produces a
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

// Reads the chain section of a capture file (either version). Returns false
// with a message on malformed input.
bool load_capture(const std::string& path, LoadedChain* out, std::string* error);

struct LoadedCapture {
  u32 version = 1;
  u32 frame = 0;  // 1-based __send-gfx-dma-chain call index, 0 when unknown
  u32 s7 = 0;     // GOAL symbol-table pointer the frame ran with, 0 when absent
  LoadedChain chain;

  // EE main memory as the frame saw it. Always allocated (zeros when the
  // capture carries no snapshot) so callers have one uniform address space.
  std::vector<u8> ee_memory;
  bool has_ee_snapshot = false;
  std::vector<u32> ee_stored_chunks;  // ascending indices of the non-zero chunks

  // Where `chain` was placed inside ee_memory by place_chain_in_ee.
  u32 chain_base = 0;
};

// Reads a capture file: chain, and for version 2 the decompressed EE snapshot.
bool load_capture_file(const std::string& path, LoadedCapture* out, std::string* error);

// Rebases the chain into a chunk-aligned run of EE memory the snapshot left
// empty and copies it there, so replaying it cannot disturb captured data.
// Records the chosen base in `chain_base`.
bool place_chain_in_ee(LoadedCapture* capture, std::string* error);

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
  // GOAL addresses of the texture-page structs the upload packets point at
  std::vector<u32> upload_page_addresses;
};

// Walks the chain the way dispatch_buckets_jak1 does (initial CALL to the
// default-registers chain, then the 70-slot bucket array) and reports what
// each bucket carried. Diagnostic only; renders nothing.
bool inventory_jak1(const LoadedChain& chain, ChainInventory* out, std::string* error);

// Human-readable Jak 1 bucket name for the inventory printout.
std::string jak1_bucket_name(int bucket);

/*!
 * Finds every GOAL `texture-page` in the EE snapshot.
 *
 * A replayed frame only uploads the pages *that frame* touched, but the game's
 * VRAM slots were filled over many earlier frames, so textures the frame draws
 * with (the debug font, for one) have no upload packet and resolve to the
 * placeholder. The snapshot does contain those pages, and they can be found
 * without any symbol-table knowledge: a GOAL basic object stores its type
 * pointer in the word before it, so one page address from the chain gives the
 * `texture-page` type pointer, and every other occurrence of that word marks
 * another page.
 *
 * `known_pages` are page addresses taken from the chain (at least one is
 * required). Results are validated - plausible texture count, in-range name and
 * per-texture pointers - so a stale type word in freed heap memory cannot make
 * a caller read outside the image. Returns page addresses in ascending order,
 * including the known ones, and reports the type pointer it derived.
 */
std::vector<u32> find_texture_pages(const LoadedCapture& capture,
                                    const std::vector<u32>& known_pages,
                                    u32* type_pointer_out);

}  // namespace metal_chain_replay
