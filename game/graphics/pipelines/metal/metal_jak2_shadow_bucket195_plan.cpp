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

constexpr std::array<u8, 8> kSerializedPlanMagic = {'J', '2', 'S', '1', '9', '5', 'P', 0};
constexpr u16 kSerializedPlanHeaderBytes = 32;
constexpr u64 kSerializedPlanHashOffset = 14695981039346656037ull;
constexpr u64 kSerializedPlanHashPrime = 1099511628211ull;

class BlobWriter {
 public:
  explicit BlobWriter(std::size_t maximum_size) : m_maximum_size(maximum_size) {}

  bool write_u8(u8 value) { return write_bytes(&value, sizeof(value)); }

  bool write_u16(u16 value) {
    const std::array<u8, 2> bytes = {
        static_cast<u8>(value),
        static_cast<u8>(value >> 8),
    };
    return write_bytes(bytes.data(), bytes.size());
  }

  bool write_u32(u32 value) {
    const std::array<u8, 4> bytes = {
        static_cast<u8>(value),
        static_cast<u8>(value >> 8),
        static_cast<u8>(value >> 16),
        static_cast<u8>(value >> 24),
    };
    return write_bytes(bytes.data(), bytes.size());
  }

  bool write_u64(u64 value) {
    std::array<u8, 8> bytes;
    for (u32 i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<u8>(value >> (i * 8));
    }
    return write_bytes(bytes.data(), bytes.size());
  }

  bool write_bytes(const u8* data, std::size_t size) {
    if ((!data && size != 0) || size > m_maximum_size - m_data.size()) {
      return false;
    }
    if (size == 0) {
      return true;
    }
    m_data.insert(m_data.end(), data, data + size);
    return true;
  }

  const std::vector<u8>& data() const { return m_data; }
  std::vector<u8> take() { return std::move(m_data); }

 private:
  std::size_t m_maximum_size = 0;
  std::vector<u8> m_data;
};

class BlobReader {
 public:
  BlobReader(const u8* data, std::size_t size) : m_data(data), m_size(size) {}

  bool read_u8(u8* value) { return read_bytes(value, sizeof(*value)); }

  bool read_u16(u16* value) {
    std::array<u8, 2> bytes;
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    *value = static_cast<u16>(bytes[0]) | (static_cast<u16>(bytes[1]) << 8);
    return true;
  }

  bool read_u32(u32* value) {
    std::array<u8, 4> bytes;
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    *value = static_cast<u32>(bytes[0]) | (static_cast<u32>(bytes[1]) << 8) |
             (static_cast<u32>(bytes[2]) << 16) | (static_cast<u32>(bytes[3]) << 24);
    return true;
  }

  bool read_u64(u64* value) {
    std::array<u8, 8> bytes;
    if (!read_bytes(bytes.data(), bytes.size())) {
      return false;
    }
    *value = 0;
    for (u32 i = 0; i < bytes.size(); ++i) {
      *value |= static_cast<u64>(bytes[i]) << (i * 8);
    }
    return true;
  }

  bool read_bytes(u8* output, std::size_t size) {
    if ((!output && size != 0) || !m_data || size > m_size - m_offset) {
      return false;
    }
    std::memcpy(output, m_data + m_offset, size);
    m_offset += size;
    return true;
  }

  bool empty() const { return m_offset == m_size; }

 private:
  const u8* m_data = nullptr;
  std::size_t m_size = 0;
  std::size_t m_offset = 0;
};

u64 serialized_plan_hash(const u8* data, std::size_t size) {
  u64 hash = kSerializedPlanHashOffset;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= kSerializedPlanHashPrime;
  }
  return hash;
}

template <std::size_t Size>
bool bytes_are_zero(const std::array<u8, Size>& bytes) {
  return std::all_of(bytes.begin(), bytes.end(), [](u8 byte) { return byte == 0; });
}

bool serialized_plan_is_valid(const Jak2ShadowBucket195Plan& plan) {
  if (plan.bucket_id != kJak2ShadowBucket195PlanBucket ||
      static_cast<u8>(plan.disposition) >
          static_cast<u8>(Jak2ShadowBucket195PlanDisposition::AcceptedDeferredNoDraw) ||
      plan.transfer_count > kJak2ShadowBucket195PlanMaximumTransfers ||
      plan.direct_transfer_count > plan.transfer_count ||
      plan.v4_32_transfer_count > plan.transfer_count ||
      plan.v4_8_transfer_count > plan.transfer_count ||
      plan.vertex_count > kJak2ShadowBucket195PlanMaximumVertices ||
      plan.record_count > kJak2ShadowBucket195PlanMaximumRecords ||
      plan.payload_bytes > kJak2ShadowBucket195PlanMaximumPayloadBytes ||
      plan.direct_payload_bytes > plan.payload_bytes ||
      plan.batches.size() > kJak2ShadowBucket195PlanMaximumBatches) {
    return false;
  }

  if (plan.disposition == Jak2ShadowBucket195PlanDisposition::Absent) {
    return plan.transfer_count == 1 && plan.direct_transfer_count == 0 &&
           plan.direct_payload_bytes == 0 && plan.v4_32_transfer_count == 0 &&
           plan.v4_8_transfer_count == 0 && plan.vertex_count == 0 && plan.record_count == 0 &&
           plan.payload_bytes == 0 && plan.batches.empty() && !plan.has_initial_direct35 &&
           !plan.has_direct6_state && !plan.has_color_direct35 && !plan.has_reset_display_state &&
           !plan.has_default_end_state && bytes_are_zero(plan.constants) &&
           bytes_are_zero(plan.vu_data) && bytes_are_zero(plan.perspective_matrix) &&
           bytes_are_zero(plan.color);
  }

  if (!plan.has_direct6_state || !plan.has_color_direct35 || !plan.has_reset_display_state ||
      !plan.has_default_end_state ||
      plan.direct_transfer_count != (plan.has_initial_direct35 ? 5 : 4) ||
      plan.direct_payload_bytes != (plan.has_initial_direct35 ? 1504 : 944)) {
    return false;
  }

  std::size_t total_vertices = 0;
  std::size_t total_commands = 0;
  std::size_t executable_records = 0;
  std::size_t upload_count = 0;
  bool has_top_only = false;
  for (const auto& batch : plan.batches) {
    if (!batch.has_top_upload || batch.top_only == batch.has_bottom_upload ||
        (!batch.has_bottom_upload && !batch.bottom_vertices.empty()) ||
        (batch.has_bottom_upload && batch.top_vertices.size() != batch.bottom_vertices.size()) ||
        !command_order_is_source_emittable(batch)) {
      return false;
    }
    has_top_only |= batch.top_only;
    upload_count += 1 + (batch.has_bottom_upload ? 1 : 0);
    if (batch.top_vertices.size() > kJak2ShadowBucket195PlanMaximumVertices - total_vertices) {
      return false;
    }
    total_vertices += batch.top_vertices.size();
    if (batch.bottom_vertices.size() > kJak2ShadowBucket195PlanMaximumVertices - total_vertices) {
      return false;
    }
    total_vertices += batch.bottom_vertices.size();

    if (batch.commands.size() > kJak2ShadowBucket195PlanMaximumTransfers - total_commands) {
      return false;
    }
    total_commands += batch.commands.size();
    for (const auto& command : batch.commands) {
      if (static_cast<u8>(command.kind) >
              static_cast<u8>(Jak2ShadowBucket195CommandKind::FlippableCaps) ||
          command.records.size() > kJak2ShadowBucket195PlanMaximumRecords - executable_records) {
        return false;
      }
      executable_records += command.records.size();
      for (const auto& record : command.records) {
        if (record_is_zero(record) ||
            !record_indices_fit(record, command.kind, batch.top_vertices.size(),
                                batch.bottom_vertices.size(), batch.top_only) ||
            (command.kind == Jak2ShadowBucket195CommandKind::Caps && record.bytes[3] != 1) ||
            (command.kind == Jak2ShadowBucket195CommandKind::FlippableCaps &&
             record.bytes[3] > 1)) {
          return false;
        }
      }
    }
  }

  const bool accepted_deferred =
      plan.disposition == Jak2ShadowBucket195PlanDisposition::AcceptedDeferredNoDraw;
  if (accepted_deferred != has_top_only || total_vertices != plan.vertex_count ||
      total_commands != plan.v4_8_transfer_count || executable_records > plan.record_count ||
      total_commands > plan.record_count || plan.v4_32_transfer_count != 3 + upload_count ||
      plan.transfer_count !=
          12 + (plan.has_initial_direct35 ? 1 : 0) + upload_count + total_commands) {
    return false;
  }

  const u64 minimum_payload_bytes =
      336 + plan.direct_payload_bytes + static_cast<u64>(plan.vertex_count) * 16 +
      static_cast<u64>(plan.record_count) * 4 + static_cast<u64>(total_commands) * 20;
  return plan.payload_bytes >= minimum_payload_bytes;
}

u8 serialized_plan_flags(const Jak2ShadowBucket195Plan& plan) {
  return static_cast<u8>(
      (plan.has_initial_direct35 ? 1 << 0 : 0) | (plan.has_direct6_state ? 1 << 1 : 0) |
      (plan.has_color_direct35 ? 1 << 2 : 0) | (plan.has_reset_display_state ? 1 << 3 : 0) |
      (plan.has_default_end_state ? 1 << 4 : 0));
}

u8 serialized_batch_flags(const Jak2ShadowBucket195Batch& batch) {
  return static_cast<u8>((batch.has_top_upload ? 1 << 0 : 0) |
                         (batch.has_bottom_upload ? 1 << 1 : 0) | (batch.top_only ? 1 << 2 : 0));
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

bool jak2_shadow_bucket195_plans_match(const Jak2ShadowBucket195Plan& live,
                                       const Jak2ShadowBucket195Plan& copied) {
  return live == copied;
}

std::optional<std::vector<u8>> serialize_jak2_shadow_bucket195_plan(
    const Jak2ShadowBucket195Plan& plan) {
  if (!serialized_plan_is_valid(plan)) {
    return std::nullopt;
  }

  BlobWriter payload(kJak2ShadowBucket195PlanMaximumSerializedBytes - kSerializedPlanHeaderBytes);
  if (!payload.write_u32(plan.bucket_id) || !payload.write_u8(static_cast<u8>(plan.disposition)) ||
      !payload.write_u8(serialized_plan_flags(plan)) || !payload.write_u16(0) ||
      !payload.write_u32(plan.transfer_count) || !payload.write_u32(plan.direct_transfer_count) ||
      !payload.write_u64(plan.direct_payload_bytes) ||
      !payload.write_u32(plan.v4_32_transfer_count) ||
      !payload.write_u32(plan.v4_8_transfer_count) || !payload.write_u32(plan.vertex_count) ||
      !payload.write_u32(plan.record_count) || !payload.write_u64(plan.payload_bytes) ||
      !payload.write_u32(static_cast<u32>(plan.batches.size())) ||
      !payload.write_bytes(plan.constants.data(), plan.constants.size()) ||
      !payload.write_bytes(plan.vu_data.data(), plan.vu_data.size()) ||
      !payload.write_bytes(plan.perspective_matrix.data(), plan.perspective_matrix.size()) ||
      !payload.write_bytes(plan.color.data(), plan.color.size())) {
    return std::nullopt;
  }

  for (const auto& batch : plan.batches) {
    if (!payload.write_u32(static_cast<u32>(batch.top_vertices.size())) ||
        !payload.write_u32(static_cast<u32>(batch.bottom_vertices.size())) ||
        !payload.write_u32(static_cast<u32>(batch.commands.size())) ||
        !payload.write_u8(serialized_batch_flags(batch)) || !payload.write_u8(0) ||
        !payload.write_u16(0)) {
      return std::nullopt;
    }
    for (const auto& vertex : batch.top_vertices) {
      if (!payload.write_bytes(vertex.bytes.data(), vertex.bytes.size())) {
        return std::nullopt;
      }
    }
    for (const auto& vertex : batch.bottom_vertices) {
      if (!payload.write_bytes(vertex.bytes.data(), vertex.bytes.size())) {
        return std::nullopt;
      }
    }
    for (const auto& command : batch.commands) {
      if (!payload.write_u8(static_cast<u8>(command.kind)) || !payload.write_u8(0) ||
          !payload.write_u16(0) || !payload.write_u32(static_cast<u32>(command.records.size()))) {
        return std::nullopt;
      }
      for (const auto& record : command.records) {
        if (!payload.write_bytes(record.bytes.data(), record.bytes.size())) {
          return std::nullopt;
        }
      }
    }
  }

  const auto& payload_bytes = payload.data();
  const std::size_t total_size = kSerializedPlanHeaderBytes + payload_bytes.size();
  if (total_size > kJak2ShadowBucket195PlanMaximumSerializedBytes ||
      total_size > std::numeric_limits<u32>::max()) {
    return std::nullopt;
  }
  BlobWriter output(kJak2ShadowBucket195PlanMaximumSerializedBytes);
  if (!output.write_bytes(kSerializedPlanMagic.data(), kSerializedPlanMagic.size()) ||
      !output.write_u16(kJak2ShadowBucket195PlanSerializationVersion) ||
      !output.write_u16(kSerializedPlanHeaderBytes) ||
      !output.write_u32(static_cast<u32>(total_size)) ||
      !output.write_u32(static_cast<u32>(payload_bytes.size())) || !output.write_u32(0) ||
      !output.write_u64(serialized_plan_hash(payload_bytes.data(), payload_bytes.size())) ||
      !output.write_bytes(payload_bytes.data(), payload_bytes.size())) {
    return std::nullopt;
  }
  return output.take();
}

std::optional<Jak2ShadowBucket195Plan> deserialize_jak2_shadow_bucket195_plan(const u8* data,
                                                                              std::size_t size) {
  if (!data || size < kSerializedPlanHeaderBytes ||
      size > kJak2ShadowBucket195PlanMaximumSerializedBytes) {
    return std::nullopt;
  }

  BlobReader header(data, kSerializedPlanHeaderBytes);
  std::array<u8, 8> magic;
  u16 version = 0;
  u16 header_bytes = 0;
  u32 total_bytes = 0;
  u32 payload_bytes = 0;
  u32 reserved = 0;
  u64 expected_hash = 0;
  if (!header.read_bytes(magic.data(), magic.size()) || !header.read_u16(&version) ||
      !header.read_u16(&header_bytes) || !header.read_u32(&total_bytes) ||
      !header.read_u32(&payload_bytes) || !header.read_u32(&reserved) ||
      !header.read_u64(&expected_hash) || !header.empty() || magic != kSerializedPlanMagic ||
      version != kJak2ShadowBucket195PlanSerializationVersion ||
      header_bytes != kSerializedPlanHeaderBytes || total_bytes != size ||
      payload_bytes != size - kSerializedPlanHeaderBytes || reserved != 0 ||
      expected_hash != serialized_plan_hash(data + kSerializedPlanHeaderBytes, payload_bytes)) {
    return std::nullopt;
  }

  BlobReader input(data + kSerializedPlanHeaderBytes, payload_bytes);
  Jak2ShadowBucket195Plan plan;
  u8 disposition = 0;
  u8 flags = 0;
  u16 payload_reserved = 0;
  u32 batch_count = 0;
  if (!input.read_u32(&plan.bucket_id) || !input.read_u8(&disposition) || !input.read_u8(&flags) ||
      !input.read_u16(&payload_reserved) || !input.read_u32(&plan.transfer_count) ||
      !input.read_u32(&plan.direct_transfer_count) || !input.read_u64(&plan.direct_payload_bytes) ||
      !input.read_u32(&plan.v4_32_transfer_count) || !input.read_u32(&plan.v4_8_transfer_count) ||
      !input.read_u32(&plan.vertex_count) || !input.read_u32(&plan.record_count) ||
      !input.read_u64(&plan.payload_bytes) || !input.read_u32(&batch_count) ||
      !input.read_bytes(plan.constants.data(), plan.constants.size()) ||
      !input.read_bytes(plan.vu_data.data(), plan.vu_data.size()) ||
      !input.read_bytes(plan.perspective_matrix.data(), plan.perspective_matrix.size()) ||
      !input.read_bytes(plan.color.data(), plan.color.size()) || payload_reserved != 0 ||
      (flags & ~0x1f) != 0 || batch_count > kJak2ShadowBucket195PlanMaximumBatches) {
    return std::nullopt;
  }
  plan.disposition = static_cast<Jak2ShadowBucket195PlanDisposition>(disposition);
  plan.has_initial_direct35 = flags & (1 << 0);
  plan.has_direct6_state = flags & (1 << 1);
  plan.has_color_direct35 = flags & (1 << 2);
  plan.has_reset_display_state = flags & (1 << 3);
  plan.has_default_end_state = flags & (1 << 4);

  std::size_t decoded_vertices = 0;
  std::size_t decoded_commands = 0;
  std::size_t decoded_records = 0;
  plan.batches.resize(batch_count);
  for (auto& batch : plan.batches) {
    u32 top_count = 0;
    u32 bottom_count = 0;
    u32 command_count = 0;
    u8 batch_flags = 0;
    u8 batch_reserved8 = 0;
    u16 batch_reserved16 = 0;
    if (!input.read_u32(&top_count) || !input.read_u32(&bottom_count) ||
        !input.read_u32(&command_count) || !input.read_u8(&batch_flags) ||
        !input.read_u8(&batch_reserved8) || !input.read_u16(&batch_reserved16) ||
        (batch_flags & ~0x07) != 0 || batch_reserved8 != 0 || batch_reserved16 != 0 ||
        top_count > kJak2ShadowBucket195PlanMaximumVertices - decoded_vertices) {
      return std::nullopt;
    }
    decoded_vertices += top_count;
    if (bottom_count > kJak2ShadowBucket195PlanMaximumVertices - decoded_vertices ||
        command_count > kJak2ShadowBucket195PlanMaximumTransfers - decoded_commands) {
      return std::nullopt;
    }
    decoded_vertices += bottom_count;
    decoded_commands += command_count;
    batch.has_top_upload = batch_flags & (1 << 0);
    batch.has_bottom_upload = batch_flags & (1 << 1);
    batch.top_only = batch_flags & (1 << 2);
    batch.top_vertices.resize(top_count);
    batch.bottom_vertices.resize(bottom_count);
    batch.commands.resize(command_count);
    for (auto& vertex : batch.top_vertices) {
      if (!input.read_bytes(vertex.bytes.data(), vertex.bytes.size())) {
        return std::nullopt;
      }
    }
    for (auto& vertex : batch.bottom_vertices) {
      if (!input.read_bytes(vertex.bytes.data(), vertex.bytes.size())) {
        return std::nullopt;
      }
    }
    for (auto& command : batch.commands) {
      u8 kind = 0;
      u8 command_reserved8 = 0;
      u16 command_reserved16 = 0;
      u32 record_count = 0;
      if (!input.read_u8(&kind) || !input.read_u8(&command_reserved8) ||
          !input.read_u16(&command_reserved16) || !input.read_u32(&record_count) ||
          command_reserved8 != 0 || command_reserved16 != 0 ||
          record_count > kJak2ShadowBucket195PlanMaximumRecords - decoded_records) {
        return std::nullopt;
      }
      decoded_records += record_count;
      command.kind = static_cast<Jak2ShadowBucket195CommandKind>(kind);
      command.records.resize(record_count);
      for (auto& record : command.records) {
        if (!input.read_bytes(record.bytes.data(), record.bytes.size())) {
          return std::nullopt;
        }
      }
    }
  }
  if (!input.empty() || !serialized_plan_is_valid(plan)) {
    return std::nullopt;
  }
  return plan;
}

}  // namespace metal_renderer
