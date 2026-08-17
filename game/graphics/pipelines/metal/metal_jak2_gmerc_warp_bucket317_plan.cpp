#include "game/graphics/pipelines/metal/metal_jak2_gmerc_warp_bucket317_plan.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"

namespace metal_renderer {
namespace {

constexpr std::size_t kMaximumEeSnapshotBytes = 128 * (1 << 20);
constexpr u32 kFragmentHeaderBytes = 7 * 16;
constexpr u32 kAdgifBytes = 5 * 16;
constexpr u64 kOffsetBasis = 14695981039346656037ull;
constexpr u64 kPrime = 1099511628211ull;

void set_rejection(Jak2GmercWarpBucket317RejectReason* out,
                   u32* out_transfer_index,
                   Jak2GmercWarpBucket317RejectReason reason,
                   u32 transfer_index = kJak2GmercWarpNoTransferIndex) {
  if (out) {
    *out = reason;
  }
  if (out_transfer_index) {
    *out_transfer_index = transfer_index;
  }
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

void hash_byte(u64* hash, u8 value) {
  *hash ^= value;
  *hash *= kPrime;
}

template <typename T>
void hash_integer(u64* hash, T value) {
  for (std::size_t i = 0; i < sizeof(value); ++i) {
    hash_byte(hash, static_cast<u8>(value >> (i * 8)));
  }
}

struct CheckedTransfer {
  DmaTag tag{0};
  const u8* payload = nullptr;
  u32 payload_bytes = 0;
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
    const DmaTag tag(read_unaligned<u64>(m_memory + m_offset));
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
    out->payload = m_memory + payload_offset;
    out->payload_bytes = static_cast<u32>(payload_bytes);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = static_cast<u32>(next_offset);
    return true;
  }

 private:
  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2GmercWarpMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_kind(u32 raw, VifCode::Kind kind) {
  return VifCode(raw).kind == kind;
}

bool is_nop_zero(const CheckedTransfer& transfer) {
  return transfer.payload_bytes == 0 && is_kind(transfer.vif0, VifCode::Kind::NOP) &&
         is_kind(transfer.vif1, VifCode::Kind::NOP);
}

bool is_terminator(const CheckedTransfer& transfer) {
  const VifCode direct(transfer.vif1);
  return transfer.payload_bytes == 160 && is_kind(transfer.vif0, VifCode::Kind::FLUSHA) &&
         direct.kind == VifCode::Kind::DIRECT && direct.immediate == 10;
}

bool add_count(u32 amount, u32 maximum, u32* count) {
  if (amount > maximum - *count) {
    return false;
  }
  *count += amount;
  return true;
}

bool read_word(const CheckedTransfer& transfer, u32 offset, u32* value) {
  if (!value || offset > transfer.payload_bytes || transfer.payload_bytes - offset < 4) {
    return false;
  }
  *value = read_unaligned<u32>(transfer.payload + offset);
  return true;
}

void skip_vif_kind(const CheckedTransfer& transfer,
                   u32* offset,
                   VifCode::Kind first,
                   VifCode::Kind second = VifCode::Kind::NOP) {
  u32 word = 0;
  while (read_word(transfer, *offset, &word)) {
    const auto kind = VifCode(word).kind;
    if (kind != first && kind != second) {
      break;
    }
    *offset += 4;
  }
}

struct FragmentParse {
  u32 end_offset = 0;
  bool needs_positions = false;
  u32 vertex_count = 0;
  u32 adgif_count = 0;
};

bool parse_fragment(const CheckedTransfer& transfer,
                    u32 start_offset,
                    u32 header_bytes,
                    bool continued,
                    FragmentParse* parsed) {
  if (!parsed || header_bytes < kFragmentHeaderBytes + kAdgifBytes ||
      (header_bytes - kFragmentHeaderBytes) % kAdgifBytes != 0) {
    return false;
  }
  const u32 aligned_header = (start_offset + 15) & ~15u;
  if (aligned_header > transfer.payload_bytes ||
      header_bytes > transfer.payload_bytes - aligned_header) {
    return false;
  }
  parsed->adgif_count = (header_bytes - kFragmentHeaderBytes) / kAdgifBytes;

  // This deliberately mirrors Generic2's source parser. For a second fragment packed into one
  // transfer, start_offset points at its V4_32 code while the header begins at the next qword.
  u32 offset = start_offset + header_bytes;
  if (offset >= transfer.payload_bytes) {
    return false;
  }
  skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);

  u32 word = 0;
  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode vertex_cycle(word);
  if (vertex_cycle.kind != VifCode::Kind::STCYCL || vertex_cycle.immediate != 0x103) {
    return false;
  }
  offset += 4;

  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode position_unpack(word);
  u32 vertex_count = 0;
  if (continued) {
    if (position_unpack.kind != VifCode::Kind::UNPACK_V4_8) {
      return false;
    }
    vertex_count = position_unpack.num;
    parsed->needs_positions = true;
  } else {
    if (position_unpack.kind != VifCode::Kind::UNPACK_V3_32 || position_unpack.num == 0) {
      return false;
    }
    vertex_count = position_unpack.num;
    offset += 4;
    const u64 position_bytes = static_cast<u64>(vertex_count) * 12;
    if (position_bytes > transfer.payload_bytes - offset) {
      return false;
    }
    offset += static_cast<u32>(position_bytes);
    skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);
  }

  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode color_unpack(word);
  if (color_unpack.kind != VifCode::Kind::UNPACK_V4_8 || color_unpack.num != vertex_count) {
    return false;
  }
  offset += 4;
  const u64 color_bytes = static_cast<u64>(vertex_count) * 4;
  if (color_bytes > transfer.payload_bytes - offset) {
    return false;
  }
  offset += static_cast<u32>(color_bytes);
  skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);

  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode texcoord_unpack(word);
  if (texcoord_unpack.kind != VifCode::Kind::UNPACK_V2_16 ||
      texcoord_unpack.num != vertex_count) {
    return false;
  }
  offset += 4;
  const u64 texcoord_bytes = static_cast<u64>(vertex_count) * 4;
  if (texcoord_bytes > transfer.payload_bytes - offset) {
    return false;
  }
  offset += static_cast<u32>(texcoord_bytes);
  if (offset == transfer.payload_bytes) {
    parsed->end_offset = offset;
    parsed->vertex_count = vertex_count;
    return true;
  }

  skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);
  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode first_tail(word);
  offset += 4;
  if (first_tail.kind == VifCode::Kind::STCYCL) {
    skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);
    if (!read_word(transfer, offset, &word) || VifCode(word).kind != VifCode::Kind::MSCAL) {
      return false;
    }
    offset += 4;
  } else if (first_tail.kind == VifCode::Kind::MSCAL) {
    skip_vif_kind(transfer, &offset, VifCode::Kind::NOP);
    if (!read_word(transfer, offset, &word) || VifCode(word).kind != VifCode::Kind::STCYCL) {
      return false;
    }
    offset += 4;
  } else {
    return false;
  }
  skip_vif_kind(transfer, &offset, VifCode::Kind::NOP, VifCode::Kind::FLUSHE);
  parsed->end_offset = offset;
  parsed->vertex_count = vertex_count;
  return true;
}

bool parse_fragment_transfer(const CheckedTransfer& transfer,
                             Jak2GmercWarpBucket317Plan* plan,
                             bool* needs_positions,
                             u32* continued_vertices) {
  const VifCode header_unpack(transfer.vif1);
  if (!plan || !needs_positions || !continued_vertices ||
      !is_kind(transfer.vif0, VifCode::Kind::STCYCL) ||
      header_unpack.kind != VifCode::Kind::UNPACK_V4_32 || header_unpack.num == 0) {
    return false;
  }
  FragmentParse first;
  if (!parse_fragment(transfer, 0, header_unpack.num * 16, false, &first) ||
      !add_count(1, kJak2GmercWarpMaximumFragments, &plan->fragment_count) ||
      !add_count(first.vertex_count, kJak2GmercWarpMaximumVertices, &plan->vertex_count) ||
      !add_count(first.adgif_count, kJak2GmercWarpMaximumAdgifs, &plan->adgif_count)) {
    return false;
  }
  u32 offset = first.end_offset;
  if (offset == transfer.payload_bytes) {
    return true;
  }

  u32 word = 0;
  if (!read_word(transfer, offset, &word) || VifCode(word).kind != VifCode::Kind::STCYCL) {
    return false;
  }
  offset += 4;
  if (!read_word(transfer, offset, &word)) {
    return false;
  }
  const VifCode second_header(word);
  if (second_header.kind != VifCode::Kind::UNPACK_V4_32 || second_header.num == 0) {
    return false;
  }
  FragmentParse second;
  if (!parse_fragment(transfer, offset, second_header.num * 16, true, &second) ||
      second.end_offset != transfer.payload_bytes || !second.needs_positions ||
      !add_count(1, kJak2GmercWarpMaximumFragments, &plan->fragment_count) ||
      !add_count(second.vertex_count, kJak2GmercWarpMaximumVertices, &plan->vertex_count) ||
      !add_count(second.adgif_count, kJak2GmercWarpMaximumAdgifs, &plan->adgif_count) ||
      !add_count(1, kJak2GmercWarpMaximumFragments, &plan->continued_fragment_count)) {
    return false;
  }
  *needs_positions = true;
  *continued_vertices = second.vertex_count;
  return true;
}

bool setup_matches(const std::array<CheckedTransfer, kJak2GmercWarpMaximumTransfers>& transfers,
                   u32 transfer_count,
                   u32* rejection_transfer_index) {
  if (transfer_count < 4) {
    *rejection_transfer_index = transfer_count;
    return false;
  }
  const auto& marker = transfers[0];
  const auto& direct = transfers[1];
  const auto& constants = transfers[2];
  const auto& vu_setup = transfers[3];
  const auto marker_kind = VifCode(marker.vif0).kind;
  const VifCode direct_code(direct.vif1);
  const VifCode constants_unpack(constants.vif1);
  if (marker.payload_bytes != 0 ||
      (marker_kind != VifCode::Kind::MARK && marker_kind != VifCode::Kind::NOP) ||
      !is_kind(marker.vif1, VifCode::Kind::NOP)) {
    *rejection_transfer_index = 0;
    return false;
  }
  if (direct.payload_bytes != 32 || !is_kind(direct.vif0, VifCode::Kind::NOP) ||
      direct_code.kind != VifCode::Kind::DIRECT || direct_code.immediate != 2) {
    *rejection_transfer_index = 1;
    return false;
  }
  if (constants.payload_bytes != 128 ||
      !is_kind(constants.vif0, VifCode::Kind::STCYCL) ||
      constants_unpack.kind != VifCode::Kind::UNPACK_V4_32 || constants_unpack.num != 8) {
    *rejection_transfer_index = 2;
    return false;
  }
  if (vu_setup.payload_bytes != 32 || !is_kind(vu_setup.vif0, VifCode::Kind::MSCALF) ||
      !is_kind(vu_setup.vif1, VifCode::Kind::STMOD)) {
    *rejection_transfer_index = 3;
    return false;
  }
  return true;
}

}  // namespace

std::optional<Jak2GmercWarpBucket317Plan> plan_jak2_gmerc_warp_bucket317(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2GmercWarpBucket317RejectReason* out_rejection,
    u32* out_rejection_transfer_index) {
  set_rejection(out_rejection, out_rejection_transfer_index,
                Jak2GmercWarpBucket317RejectReason::None);
  if (bucket_id != kJak2GmercWarpBucket || !dma_packet_snapshot ||
      chain_offset > std::numeric_limits<u32>::max() - (kJak2GmercWarpBucket + 1) * 16) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2GmercWarpBucket317RejectReason::InvalidInput);
    return std::nullopt;
  }
  const u32 bucket_offset = chain_offset + kJak2GmercWarpBucket * 16;
  const u32 next_bucket = chain_offset + (kJak2GmercWarpBucket + 1) * 16;
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size, bucket_offset);
  std::array<CheckedTransfer, kJak2GmercWarpMaximumTransfers> transfers = {};
  u32 transfer_count = 0;
  u64 payload_bytes = 0;
  u64 fingerprint = kOffsetBasis;
  while (dma.offset() != next_bucket) {
    if (transfer_count == transfers.size()) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::TransferLimit, transfer_count);
      return std::nullopt;
    }
    if (!dma.read(&transfers[transfer_count])) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::DmaChain, transfer_count);
      return std::nullopt;
    }
    const auto& transfer = transfers[transfer_count++];
    if (transfer.payload_bytes > kJak2GmercWarpMaximumPayloadBytes - payload_bytes) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::PayloadLimit, transfer_count - 1);
      return std::nullopt;
    }
    payload_bytes += transfer.payload_bytes;
    hash_integer(&fingerprint, static_cast<u8>(transfer.tag.kind));
    hash_integer(&fingerprint, transfer.tag.qwc);
    hash_integer(&fingerprint, transfer.vif0);
    hash_integer(&fingerprint, transfer.vif1);
    for (u32 i = 0; i < transfer.payload_bytes; ++i) {
      hash_byte(&fingerprint, transfer.payload[i]);
    }
  }

  Jak2GmercWarpBucket317Plan plan;
  plan.transfer_count = transfer_count;
  plan.payload_bytes = payload_bytes;
  plan.semantic_fingerprint = fingerprint;
  if (transfer_count == 1 && is_nop_zero(transfers[0])) {
    return plan;
  }
  u32 setup_rejection_transfer_index = 0;
  if (!setup_matches(transfers, transfer_count, &setup_rejection_transfer_index)) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2GmercWarpBucket317RejectReason::SetupGrammar,
                  setup_rejection_transfer_index);
    return std::nullopt;
  }
  if (transfer_count == 4) {
    // The source/OpenGL short setup-only form reaches the boundary only with a NOP marker.
    if (!is_kind(transfers[0].vif0, VifCode::Kind::NOP)) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::SetupOnlyMarker, 0);
      return std::nullopt;
    }
    plan.variant = Jak2GmercWarpBucket317Variant::SetupOnly;
    return plan;
  }

  u32 index = 4;
  while (index < transfer_count && is_nop_zero(transfers[index])) {
    ++index;
  }
  while (index < transfer_count && !is_terminator(transfers[index])) {
    if (index + 2 == transfer_count) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::TerminatorGrammar, index);
      return std::nullopt;
    }
    bool needs_positions = false;
    u32 continued_vertices = 0;
    if (!parse_fragment_transfer(transfers[index], &plan, &needs_positions,
                                 &continued_vertices)) {
      set_rejection(out_rejection, out_rejection_transfer_index,
                    Jak2GmercWarpBucket317RejectReason::FragmentGrammar, index);
      return std::nullopt;
    }
    ++index;
    if (needs_positions) {
      if (index >= transfer_count) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2GmercWarpBucket317RejectReason::ContinuedPositionGrammar, index);
        return std::nullopt;
      }
      const VifCode position_unpack(transfers[index].vif1);
      if ((transfers[index].payload_bytes & 15) != 0 ||
          transfers[index].payload_bytes / 12 != continued_vertices ||
          !is_kind(transfers[index].vif0, VifCode::Kind::NOP) ||
          position_unpack.kind != VifCode::Kind::UNPACK_V3_32 ||
          position_unpack.num != continued_vertices) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2GmercWarpBucket317RejectReason::ContinuedPositionGrammar, index);
        return std::nullopt;
      }
      if (index + 1 >= transfer_count || transfers[index + 1].payload_bytes != 0 ||
          !is_kind(transfers[index + 1].vif1, VifCode::Kind::MSCAL)) {
        set_rejection(out_rejection, out_rejection_transfer_index,
                      Jak2GmercWarpBucket317RejectReason::ContinuedMscalGrammar, index + 1);
        return std::nullopt;
      }
      index += 2;
    }
    while (index < transfer_count && is_nop_zero(transfers[index])) {
      ++index;
    }
  }
  if (index >= transfer_count || !is_terminator(transfers[index])) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2GmercWarpBucket317RejectReason::TerminatorGrammar, index);
    return std::nullopt;
  }
  if (index + 1 >= transfer_count || !is_nop_zero(transfers[index + 1])) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2GmercWarpBucket317RejectReason::BoundaryGrammar, index + 1);
    return std::nullopt;
  }
  if (index + 2 != transfer_count) {
    set_rejection(out_rejection, out_rejection_transfer_index,
                  Jak2GmercWarpBucket317RejectReason::TrailingTransfer, index + 2);
    return std::nullopt;
  }
  plan.variant = plan.fragment_count == 0 ? Jak2GmercWarpBucket317Variant::SetupOnly
                                          : Jak2GmercWarpBucket317Variant::Fragments;
  return plan;
}

const char* jak2_gmerc_warp_bucket317_reject_reason_name(
    Jak2GmercWarpBucket317RejectReason reason) {
  switch (reason) {
    case Jak2GmercWarpBucket317RejectReason::None:
      return "none";
    case Jak2GmercWarpBucket317RejectReason::InvalidInput:
      return "invalid-input";
    case Jak2GmercWarpBucket317RejectReason::DmaChain:
      return "dma-chain";
    case Jak2GmercWarpBucket317RejectReason::TransferLimit:
      return "transfer-limit";
    case Jak2GmercWarpBucket317RejectReason::PayloadLimit:
      return "payload-limit";
    case Jak2GmercWarpBucket317RejectReason::SetupGrammar:
      return "setup-grammar";
    case Jak2GmercWarpBucket317RejectReason::SetupOnlyMarker:
      return "setup-only-marker";
    case Jak2GmercWarpBucket317RejectReason::FragmentGrammar:
      return "fragment-grammar";
    case Jak2GmercWarpBucket317RejectReason::ContinuedPositionGrammar:
      return "continued-position-grammar";
    case Jak2GmercWarpBucket317RejectReason::ContinuedMscalGrammar:
      return "continued-mscal-grammar";
    case Jak2GmercWarpBucket317RejectReason::TerminatorGrammar:
      return "terminator-grammar";
    case Jak2GmercWarpBucket317RejectReason::BoundaryGrammar:
      return "boundary-grammar";
    case Jak2GmercWarpBucket317RejectReason::TrailingTransfer:
      return "trailing-transfer";
  }
  return "unknown";
}

bool jak2_gmerc_warp_bucket317_plans_match(const Jak2GmercWarpBucket317Plan& live,
                                           const Jak2GmercWarpBucket317Plan& copied) {
  return live.bucket_id == kJak2GmercWarpBucket && copied.bucket_id == kJak2GmercWarpBucket &&
         live.variant == copied.variant && live.transfer_count == copied.transfer_count &&
         live.fragment_count == copied.fragment_count &&
         live.continued_fragment_count == copied.continued_fragment_count &&
         live.vertex_count == copied.vertex_count && live.adgif_count == copied.adgif_count &&
         live.payload_bytes == copied.payload_bytes &&
         live.semantic_fingerprint == copied.semantic_fingerprint;
}

}  // namespace metal_renderer
