#include "game/graphics/pipelines/metal/metal_jak2_shadow_bucket195_plan.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "common/dma/dma.h"

namespace metal_renderer {
namespace {

constexpr std::size_t kMaximumEeSnapshotBytes = 128 * (1 << 20);
constexpr u16 kTopVertexAddress = 4;
constexpr u16 kBottomVertexAddress = 174;
constexpr u16 kCapIndexAddress = 344;
constexpr u16 kWallIndexAddress = 600;

constexpr u32 raw_vif(VifCode::Kind kind, u8 num = 0, u16 immediate = 0) {
  return (static_cast<u32>(kind) << 24) | (static_cast<u32>(num) << 16) | immediate;
}

constexpr u64 raw_dma(DmaTag::Kind kind, u16 qwc, u32 address = 0) {
  return static_cast<u64>(qwc) | (static_cast<u64>(kind) << 28) | (static_cast<u64>(address) << 32);
}

template <typename T>
T read_unaligned(const u8* source) {
  T result;
  std::memcpy(&result, source, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

struct CheckedTransfer {
  DmaTag tag{0};
  u64 raw_tag = 0;
  const u8* payload = nullptr;
  u32 payload_bytes = 0;
  u32 next_offset = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
};

class CheckedDmaFollower {
 public:
  CheckedDmaFollower(const u8* memory, std::size_t memory_size, u32 start_offset)
      : m_memory(memory),
        m_memory_size(std::min<std::size_t>(memory_size, kMaximumEeSnapshotBytes)),
        m_offset(start_offset) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() ||
        !range_is_valid(m_offset, 16, m_memory_size) || (m_offset & 15) != 0 ||
        std::find(m_visited.begin(), m_visited.begin() + m_visited_count, m_offset) !=
            m_visited.begin() + m_visited_count) {
      return false;
    }

    const u64 raw_tag = read_unaligned<u64>(m_memory + m_offset);
    const DmaTag tag(raw_tag);
    if (tag.spr) {
      return false;
    }
    const u64 payload_bytes = static_cast<u64>(tag.qwc) * 16;
    u64 payload_offset = 0;
    u64 next_offset = 0;
    switch (tag.kind) {
      case DmaTag::Kind::CNT:
        if (tag.addr != 0) {
          return false;
        }
        payload_offset = static_cast<u64>(m_offset) + 16;
        next_offset = payload_offset + payload_bytes;
        break;
      case DmaTag::Kind::NEXT:
        if (tag.addr == 0) {
          return false;
        }
        payload_offset = static_cast<u64>(m_offset) + 16;
        next_offset = tag.addr;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        if (tag.qwc != 0 && tag.addr == 0) {
          return false;
        }
        payload_offset = tag.addr;
        next_offset = static_cast<u64>(m_offset) + 16;
        break;
      default:
        return false;
    }
    if ((payload_offset & 15) != 0 ||
        !range_is_valid(payload_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    m_visited[m_visited_count++] = m_offset;
    out->tag = tag;
    out->raw_tag = raw_tag;
    out->payload = m_memory + payload_offset;
    out->payload_bytes = static_cast<u32>(payload_bytes);
    out->next_offset = static_cast<u32>(next_offset);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = out->next_offset;
    return true;
  }

 private:
  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2ShadowBucket195PlanMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_source_cnt(const CheckedTransfer& transfer) {
  return transfer.raw_tag == raw_dma(DmaTag::Kind::CNT, transfer.tag.qwc);
}

bool account(const CheckedTransfer& transfer, Jak2ShadowBucket195Plan* plan) {
  if (!plan || plan->transfer_count == kJak2ShadowBucket195PlanMaximumTransfers ||
      transfer.payload_bytes > kJak2ShadowBucket195PlanMaximumPayloadBytes - plan->payload_bytes) {
    return false;
  }
  ++plan->transfer_count;
  plan->payload_bytes += transfer.payload_bytes;
  const auto vif1 = VifCode(transfer.vif1);
  if (vif1.kind == VifCode::Kind::DIRECT || vif1.kind == VifCode::Kind::DIRECTHL) {
    ++plan->direct_transfer_count;
    plan->direct_payload_bytes += transfer.payload_bytes;
  } else if (vif1.kind == VifCode::Kind::UNPACK_V4_32) {
    ++plan->v4_32_transfer_count;
  } else if (vif1.kind == VifCode::Kind::UNPACK_V4_8) {
    ++plan->v4_8_transfer_count;
  }
  return true;
}

bool read_accounted(CheckedDmaFollower* dma,
                    CheckedTransfer* transfer,
                    Jak2ShadowBucket195Plan* plan) {
  return dma && dma->read(transfer) && account(*transfer, plan);
}

bool is_exact_zero_transfer(const CheckedTransfer& transfer) {
  return is_source_cnt(transfer) && transfer.payload_bytes == 0 && transfer.vif0 == 0 &&
         transfer.vif1 == 0;
}

bool is_exact_next(const CheckedTransfer& transfer) {
  return transfer.raw_tag == raw_dma(DmaTag::Kind::NEXT, 0, transfer.next_offset) &&
         transfer.payload_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_direct(const CheckedTransfer& transfer, u16 qwc, VifCode::Kind vif0_kind) {
  return is_source_cnt(transfer) && transfer.tag.qwc == qwc &&
         transfer.payload_bytes == static_cast<u32>(qwc) * 16 &&
         transfer.vif0 == raw_vif(vif0_kind) &&
         transfer.vif1 == raw_vif(VifCode::Kind::DIRECT, 0, qwc);
}

bool fixed_unpack_matches(const CheckedTransfer& transfer, u16 qwc, u16 address, u16 count) {
  return is_source_cnt(transfer) && transfer.tag.qwc == qwc &&
         transfer.payload_bytes == static_cast<u32>(qwc) * 16 &&
         transfer.vif0 == raw_vif(VifCode::Kind::STCYCL, 0, 0x404) &&
         transfer.vif1 == raw_vif(VifCode::Kind::UNPACK_V4_32, count, address);
}

u32 decoded_unpack_count(const VifCode& code) {
  return code.num == 0 ? 256 : code.num;
}

bool add_vertices(const CheckedTransfer& transfer,
                  u16 expected_address,
                  std::vector<Jak2ShadowBucket195Vertex>* vertices,
                  Jak2ShadowBucket195Plan* plan) {
  if (!vertices || !plan || !is_source_cnt(transfer)) {
    return false;
  }
  const VifCode vif1(transfer.vif1);
  if ((transfer.vif0 != raw_vif(VifCode::Kind::FLUSH) && transfer.vif0 != 0) ||
      transfer.vif1 != raw_vif(VifCode::Kind::UNPACK_V4_32, vif1.num, expected_address)) {
    return false;
  }
  const u32 vertex_count = vif1.num;
  if (transfer.payload_bytes != vertex_count * 16 || transfer.tag.qwc != vertex_count ||
      vertex_count > kJak2ShadowBucket195PlanMaximumVertices - plan->vertex_count) {
    return false;
  }
  vertices->resize(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i) {
    std::memcpy((*vertices)[i].bytes.data(), transfer.payload + i * 16, 16);
  }
  plan->vertex_count += vertex_count;
  return true;
}

bool record_is_zero(const Jak2ShadowBucket195Record& record) {
  return std::all_of(record.bytes.begin(), record.bytes.end(), [](u8 byte) { return byte == 0; });
}

bool record_indices_fit(const Jak2ShadowBucket195Record& record,
                        Jak2ShadowBucket195CommandKind kind,
                        u32 top_vertex_count,
                        u32 bottom_vertex_count,
                        bool top_only) {
  const u32 required_vertices =
      top_only ? top_vertex_count : std::min(top_vertex_count, bottom_vertex_count);
  if (required_vertices == 0) {
    return false;
  }
  if (kind == Jak2ShadowBucket195CommandKind::Walls) {
    return record.bytes[0] < required_vertices && record.bytes[1] < required_vertices &&
           record.bytes[2] <= 1;
  }
  return record.bytes[0] < required_vertices && record.bytes[1] < required_vertices &&
         record.bytes[2] < required_vertices;
}

bool add_index_command(const CheckedTransfer& transfer,
                       Jak2ShadowBucket195Batch* batch,
                       Jak2ShadowBucket195Plan* plan) {
  if (!batch || !plan || !is_source_cnt(transfer)) {
    return false;
  }
  const VifCode vif1(transfer.vif1);
  if (transfer.vif0 != 0 || vif1.kind != VifCode::Kind::UNPACK_V4_8) {
    return false;
  }
  const VifCodeUnpack unpack(vif1);
  const u32 unpack_count = decoded_unpack_count(vif1);
  const u8 encoded_unpack_count = unpack_count == 256 ? 0 : static_cast<u8>(unpack_count);
  if (transfer.vif1 != raw_vif(VifCode::Kind::UNPACK_V4_8, encoded_unpack_count,
                               static_cast<u16>(unpack.addr_qw | (1 << 14))) ||
      unpack_count % 4 != 0 || transfer.payload_bytes != unpack_count * 4 + 16 ||
      transfer.tag.qwc != unpack_count / 4 + 1) {
    return false;
  }

  const u32 header = read_unaligned<u32>(transfer.payload);
  const u32 record_count = header & 0xff;
  const u32 expected_unpack_count = (record_count + 1 + 3) & ~3u;
  if (record_count == 0 || expected_unpack_count != unpack_count ||
      record_count > kJak2ShadowBucket195PlanMaximumRecords - plan->record_count) {
    return false;
  }
  for (u32 offset = 4 + record_count * 4; offset < unpack_count * 4; ++offset) {
    if (transfer.payload[offset] != 0) {
      return false;
    }
  }
  if (read_unaligned<u32>(transfer.payload + unpack_count * 4) != 0 ||
      read_unaligned<u32>(transfer.payload + unpack_count * 4 + 4) != 0 ||
      read_unaligned<u32>(transfer.payload + unpack_count * 4 + 8) != 0) {
    return false;
  }
  const VifCode mscalf(read_unaligned<u32>(transfer.payload + unpack_count * 4 + 12));
  if (read_unaligned<u32>(transfer.payload + unpack_count * 4 + 12) !=
      raw_vif(VifCode::Kind::MSCALF, 0, mscalf.immediate)) {
    return false;
  }

  Jak2ShadowBucket195Command command;
  switch (mscalf.immediate) {
    case 2:
      if (unpack.addr_qw != kCapIndexAddress || header >> 8 != 0 ||
          batch->bottom_vertices.empty()) {
        return false;
      }
      command.kind = Jak2ShadowBucket195CommandKind::Caps;
      break;
    case 4:
      if (unpack.addr_qw != kWallIndexAddress || header >> 8 != 0 ||
          batch->bottom_vertices.empty()) {
        return false;
      }
      command.kind = Jak2ShadowBucket195CommandKind::Walls;
      break;
    case 6:
      if (unpack.addr_qw != kCapIndexAddress || (header >> 8 != 0 && header >> 8 != 1)) {
        return false;
      }
      command.kind = Jak2ShadowBucket195CommandKind::FlippableCaps;
      if (batch->bottom_vertices.empty()) {
        if (header >> 8 != 1) {
          return false;
        }
        batch->top_only = true;
      } else if (header >> 8 != 0) {
        return false;
      }
      break;
    default:
      return false;
  }

  command.records.reserve(record_count);
  bool reached_padding = false;
  for (u32 i = 0; i < record_count; ++i) {
    Jak2ShadowBucket195Record record;
    std::memcpy(record.bytes.data(), transfer.payload + 4 + i * 4, 4);
    if (record_is_zero(record)) {
      reached_padding = true;
      continue;
    }
    if (reached_padding ||
        !record_indices_fit(record, command.kind, batch->top_vertices.size(),
                            batch->bottom_vertices.size(), batch->top_only) ||
        (command.kind == Jak2ShadowBucket195CommandKind::Caps && record.bytes[3] != 1) ||
        (command.kind == Jak2ShadowBucket195CommandKind::FlippableCaps && record.bytes[3] > 1)) {
      return false;
    }
    command.records.push_back(record);
  }
  plan->record_count += record_count;
  batch->commands.push_back(std::move(command));
  return true;
}

bool command_order_is_source_emittable(const Jak2ShadowBucket195Batch& batch) {
  if (batch.top_only) {
    return !batch.has_bottom_upload &&
           (batch.commands.empty() ||
            (batch.commands.size() == 1 &&
             batch.commands[0].kind == Jak2ShadowBucket195CommandKind::FlippableCaps));
  }

  constexpr std::array kNormalOrder = {
      Jak2ShadowBucket195CommandKind::Caps,
      Jak2ShadowBucket195CommandKind::Walls,
      Jak2ShadowBucket195CommandKind::FlippableCaps,
      Jak2ShadowBucket195CommandKind::Walls,
  };
  std::size_t next_slot = 0;
  for (const auto& command : batch.commands) {
    const auto match =
        std::find(kNormalOrder.begin() + next_slot, kNormalOrder.end(), command.kind);
    if (match == kNormalOrder.end()) {
      return false;
    }
    next_slot = static_cast<std::size_t>(match - kNormalOrder.begin()) + 1;
  }
  return true;
}

bool finish_batch(Jak2ShadowBucket195Plan* plan, Jak2ShadowBucket195Batch* batch) {
  if (!plan || !batch) {
    return false;
  }
  if (!batch->has_top_upload && !batch->has_bottom_upload) {
    return true;
  }
  if (!batch->has_top_upload || plan->batches.size() == kJak2ShadowBucket195PlanMaximumBatches) {
    return false;
  }
  if (!batch->has_bottom_upload) {
    batch->top_only = true;
  }
  if ((batch->has_bottom_upload && batch->top_vertices.size() != batch->bottom_vertices.size()) ||
      !command_order_is_source_emittable(*batch)) {
    return false;
  }
  if (batch->top_only) {
    plan->disposition = Jak2ShadowBucket195PlanDisposition::AcceptedDeferredNoDraw;
  }
  plan->batches.push_back(std::move(*batch));
  *batch = {};
  return true;
}

bool is_tail_start(const CheckedTransfer& transfer) {
  return is_direct(transfer, 6, VifCode::Kind::FLUSHA);
}

}  // namespace

std::optional<Jak2ShadowBucket195Plan> plan_jak2_shadow_bucket195(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id) {
  if (!dma_packet_snapshot || bucket_id != kJak2ShadowBucket195PlanBucket ||
      chain_offset > std::numeric_limits<u32>::max() - (kJak2ShadowBucket195PlanBucket + 1) * 16) {
    return std::nullopt;
  }
  const u32 bucket_offset = chain_offset + kJak2ShadowBucket195PlanBucket * 16;
  const u32 next_bucket = chain_offset + (kJak2ShadowBucket195PlanBucket + 1) * 16;
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size, bucket_offset);
  Jak2ShadowBucket195Plan plan;
  CheckedTransfer transfer;
  if (!read_accounted(&dma, &transfer, &plan)) {
    return std::nullopt;
  }
  if (dma.offset() == next_bucket && is_exact_zero_transfer(transfer)) {
    return plan;
  }
  if (transfer.tag.kind != DmaTag::Kind::NEXT || transfer.tag.qwc != 0 ||
      transfer.payload_bytes != 0 || transfer.next_offset == next_bucket) {
    return std::nullopt;
  }

  if (!read_accounted(&dma, &transfer, &plan) || !fixed_unpack_matches(transfer, 13, 0x370, 13)) {
    return std::nullopt;
  }
  std::memcpy(plan.constants.data(), transfer.payload, plan.constants.size());
  if (!read_accounted(&dma, &transfer, &plan) || !fixed_unpack_matches(transfer, 4, 0x3ac, 4)) {
    return std::nullopt;
  }
  std::memcpy(plan.vu_data.data(), transfer.payload, plan.vu_data.size());
  if (!read_accounted(&dma, &transfer, &plan) || !fixed_unpack_matches(transfer, 4, 0, 4)) {
    return std::nullopt;
  }
  std::memcpy(plan.perspective_matrix.data(), transfer.payload, plan.perspective_matrix.size());
  if (!read_accounted(&dma, &transfer, &plan) || !is_source_cnt(transfer) ||
      transfer.payload_bytes != 0 || transfer.vif0 != raw_vif(VifCode::Kind::MSCALF, 0, 10) ||
      transfer.vif1 != raw_vif(VifCode::Kind::FLUSHE)) {
    return std::nullopt;
  }
  if (!read_accounted(&dma, &transfer, &plan) || !is_exact_zero_transfer(transfer)) {
    return std::nullopt;
  }

  CheckedDmaFollower peek = dma;
  if (!peek.read(&transfer)) {
    return std::nullopt;
  }
  if (is_direct(transfer, 35, VifCode::Kind::NOP)) {
    if (!read_accounted(&dma, &transfer, &plan)) {
      return std::nullopt;
    }
    plan.has_initial_direct35 = true;
  }

  plan.disposition = Jak2ShadowBucket195PlanDisposition::Ready;
  Jak2ShadowBucket195Batch batch;
  bool reached_tail = false;
  while (dma.offset() != next_bucket) {
    if (!read_accounted(&dma, &transfer, &plan)) {
      return std::nullopt;
    }
    if (is_tail_start(transfer)) {
      reached_tail = true;
      plan.has_direct6_state = true;
      break;
    }

    const VifCode vif1(transfer.vif1);
    if (vif1.kind == VifCode::Kind::UNPACK_V4_32) {
      const u16 address = VifCodeUnpack(vif1).addr_qw;
      if (address == kTopVertexAddress) {
        if ((batch.has_top_upload || batch.has_bottom_upload) && !finish_batch(&plan, &batch)) {
          return std::nullopt;
        }
        if (!add_vertices(transfer, kTopVertexAddress, &batch.top_vertices, &plan)) {
          return std::nullopt;
        }
        batch.has_top_upload = true;
      } else if (address == kBottomVertexAddress) {
        if (!batch.has_top_upload || batch.has_bottom_upload ||
            !add_vertices(transfer, kBottomVertexAddress, &batch.bottom_vertices, &plan)) {
          return std::nullopt;
        }
        batch.has_bottom_upload = true;
      } else {
        return std::nullopt;
      }
    } else if (vif1.kind == VifCode::Kind::UNPACK_V4_8) {
      if (!batch.has_top_upload || !add_index_command(transfer, &batch, &plan)) {
        return std::nullopt;
      }
    } else {
      return std::nullopt;
    }
  }
  if (!reached_tail || !finish_batch(&plan, &batch)) {
    return std::nullopt;
  }

  if (!read_accounted(&dma, &transfer, &plan) || !is_direct(transfer, 35, VifCode::Kind::FLUSHA)) {
    return std::nullopt;
  }
  plan.has_color_direct35 = true;
  std::memcpy(plan.color.data(), transfer.payload + 24, plan.color.size());
  if (!read_accounted(&dma, &transfer, &plan) || !is_direct(transfer, 8, VifCode::Kind::FLUSHA)) {
    return std::nullopt;
  }
  plan.has_reset_display_state = true;
  if (!read_accounted(&dma, &transfer, &plan) || !is_exact_next(transfer) ||
      dma.offset() == next_bucket) {
    return std::nullopt;
  }
  if (!read_accounted(&dma, &transfer, &plan) || !is_direct(transfer, 10, VifCode::Kind::FLUSHA)) {
    return std::nullopt;
  }
  plan.has_default_end_state = true;
  if (!read_accounted(&dma, &transfer, &plan) || !is_exact_next(transfer) ||
      dma.offset() != next_bucket) {
    return std::nullopt;
  }
  return plan;
}

}  // namespace metal_renderer
