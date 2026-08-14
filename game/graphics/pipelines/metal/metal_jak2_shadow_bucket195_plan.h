#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2ShadowBucket195PlanBucket = 195;
constexpr u32 kJak2ShadowBucket195PlanMaximumTransfers = 256;
constexpr u64 kJak2ShadowBucket195PlanMaximumPayloadBytes = 1 << 20;
constexpr u32 kJak2ShadowBucket195PlanMaximumBatches = 128;
constexpr u32 kJak2ShadowBucket195PlanMaximumVertices = 32 << 10;
constexpr u32 kJak2ShadowBucket195PlanMaximumRecords = 32 << 10;

enum class Jak2ShadowBucket195PlanDisposition : u8 {
  Absent,
  Ready,
  AcceptedDeferredNoDraw,
};

enum class Jak2ShadowBucket195CommandKind : u8 {
  Caps,
  Walls,
  FlippableCaps,
};

struct Jak2ShadowBucket195Vertex {
  std::array<u8, 16> bytes = {};
};

struct Jak2ShadowBucket195Record {
  std::array<u8, 4> bytes = {};
};

struct Jak2ShadowBucket195Command {
  Jak2ShadowBucket195CommandKind kind = Jak2ShadowBucket195CommandKind::Caps;
  std::vector<Jak2ShadowBucket195Record> records;
};

struct Jak2ShadowBucket195Batch {
  std::vector<Jak2ShadowBucket195Vertex> top_vertices;
  std::vector<Jak2ShadowBucket195Vertex> bottom_vertices;
  std::vector<Jak2ShadowBucket195Command> commands;
  bool has_top_upload = false;
  bool has_bottom_upload = false;
  bool top_only = false;
};

/*
 * An owned, renderer-independent execution plan for the packet grammar emitted by Jak II's
 * shadow-vu1/shadow-cpu source. The parser deliberately does not apply renderer-only floating
 * point, fog, projection, or visibility predicates. A source-emittable top-only MSCALF 6 batch is
 * retained but classifies the complete plan as AcceptedDeferredNoDraw, so an executor must not
 * dereference a missing bottom buffer or draw a partial frame.
 */
struct Jak2ShadowBucket195Plan {
  u32 bucket_id = kJak2ShadowBucket195PlanBucket;
  Jak2ShadowBucket195PlanDisposition disposition = Jak2ShadowBucket195PlanDisposition::Absent;
  std::array<u8, 208> constants = {};
  std::array<u8, 64> vu_data = {};
  std::array<u8, 64> perspective_matrix = {};
  std::array<u8, 4> color = {};
  std::vector<Jak2ShadowBucket195Batch> batches;
  u32 transfer_count = 0;
  u32 direct_transfer_count = 0;
  u64 direct_payload_bytes = 0;
  u32 v4_32_transfer_count = 0;
  u32 v4_8_transfer_count = 0;
  u32 vertex_count = 0;
  u32 record_count = 0;
  u64 payload_bytes = 0;
  bool has_initial_direct35 = false;
  bool has_direct6_state = false;
  bool has_color_direct35 = false;
  bool has_reset_display_state = false;
  bool has_default_end_state = false;
};

std::optional<Jak2ShadowBucket195Plan> plan_jak2_shadow_bucket195(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id);

}  // namespace metal_renderer
