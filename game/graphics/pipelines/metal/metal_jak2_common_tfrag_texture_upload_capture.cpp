#include "game/graphics/pipelines/metal/metal_jak2_common_tfrag_texture_upload_capture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/goal_constants.h"

namespace metal_renderer {
namespace {

constexpr u16 kStartAnimatorArray = kJak2PrisPrisonJakAnimatorStartOpcode;
constexpr u16 kFinishAnimatorArray = kJak2PrisPrisonJakAnimatorFinishOpcode;
constexpr u16 kDarkJakOpcode = kJak2CommonPrisDarkJakAnimatorOpcode;
constexpr u16 kPrisonJakOpcode = kJak2PrisPrisonJakAnimatorOpcode;
constexpr u16 kSkullGemOpcode = 27;
constexpr u16 kSecurityOpcode = 30;
constexpr u32 kPs2VramTbpUpperBound = 0x40000;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;
constexpr u32 kFlushaVif = static_cast<u32>(VifCode::Kind::FLUSHA) << 24;
constexpr u32 kDirectVif = static_cast<u32>(VifCode::Kind::DIRECT) << 24;

struct GoalTexturePageHeaderLayout {
  struct Segment {
    u32 block_data_ptr;
    u32 size;
    u32 destination;
  };
  u32 file_info_ptr;
  u32 name_ptr;
  u32 id;
  s32 length;
  u32 mip0_size;
  u32 size;
  Segment segments[3];
  u32 pad[16];
};
static_assert(sizeof(GoalTexturePageHeaderLayout) == kJak2Bucket4OrdinaryPageHeaderBytes);

struct CheckedTransfer {
  DmaTag tag{0};
  u32 tag_offset = 0;
  u32 size_bytes = 0;
  u32 vif0 = 0;
  u32 vif1 = 0;
};

template <typename T>
T read_unaligned(const u8* address) {
  T result;
  std::memcpy(&result, address, sizeof(result));
  return result;
}

bool range_is_valid(u64 offset, u64 size, std::size_t memory_size) {
  return offset <= memory_size && size <= memory_size - offset;
}

class CheckedDmaFollower {
 public:
  CheckedDmaFollower(const u8* memory, std::size_t memory_size, u32 start_offset)
      : m_memory(memory),
        m_memory_size(std::min<std::size_t>(memory_size, EE_MAIN_MEM_SIZE)),
        m_offset(start_offset) {}

  u32 offset() const { return m_offset; }

  bool read(CheckedTransfer* out) {
    if (!out || !m_memory || m_visited_count == m_visited.size() ||
        !range_is_valid(m_offset, 16, m_memory_size) || (m_offset & 15) != 0 ||
        already_visited(m_offset)) {
      return false;
    }

    const DmaTag tag(read_unaligned<u64>(m_memory + m_offset));
    if (tag.spr) {
      return false;
    }

    const u64 payload_bytes = static_cast<u64>(tag.qwc) * 16;
    u64 data_offset = 0;
    u64 next_offset = 0;
    switch (tag.kind) {
      case DmaTag::Kind::CNT:
        if (tag.addr != 0) {
          return false;
        }
        data_offset = static_cast<u64>(m_offset) + 16;
        next_offset = data_offset + payload_bytes;
        break;
      case DmaTag::Kind::NEXT:
        if (tag.addr == 0) {
          return false;
        }
        data_offset = static_cast<u64>(m_offset) + 16;
        next_offset = tag.addr;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        if (tag.qwc != 0 && tag.addr == 0) {
          return false;
        }
        data_offset = tag.addr;
        next_offset = static_cast<u64>(m_offset) + 16;
        break;
      default:
        return false;
    }

    if ((data_offset & 15) != 0 || !range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    m_visited[m_visited_count++] = m_offset;
    out->tag = tag;
    out->tag_offset = m_offset;
    out->size_bytes = static_cast<u32>(payload_bytes);
    out->vif0 = read_unaligned<u32>(m_memory + m_offset + 8);
    out->vif1 = read_unaligned<u32>(m_memory + m_offset + 12);
    m_offset = static_cast<u32>(next_offset);
    return true;
  }

 private:
  bool already_visited(u32 offset) const {
    return std::find(m_visited.begin(), m_visited.begin() + m_visited_count, offset) !=
           m_visited.begin() + m_visited_count;
  }

  const u8* m_memory = nullptr;
  std::size_t m_memory_size = 0;
  u32 m_offset = 0;
  std::array<u32, kJak2CommonTfragTextureUploadMaximumTransfers> m_visited = {};
  std::size_t m_visited_count = 0;
};

bool is_inert(const CheckedTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_exact_animator_start(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 0 &&
         transfer.vif0 == (kPcPortVif | kStartAnimatorArray) && transfer.vif1 == 0;
}

bool is_exact_animator_finish(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 0 &&
         transfer.vif0 == (kPcPortVif | kFinishAnimatorArray) &&
         (transfer.vif1 == 0 || transfer.vif1 == kPcPortVif);
}

bool is_ordinary_descriptor(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.size_bytes == 16 &&
         transfer.vif0 == kPcPortVif && transfer.vif1 == 3;
}

bool is_exact_direct_setup(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 10 &&
         transfer.size_bytes == 160 && transfer.vif0 == kFlushaVif &&
         transfer.vif1 == (kDirectVif | 10);
}

bool is_exact_gs_setup(const CheckedTransfer& transfer) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.tag.qwc == 2 &&
         transfer.size_bytes == 32 && transfer.vif0 == 0 &&
         transfer.vif1 == (kDirectVif | 2);
}

bool record_transfer(const CheckedTransfer& transfer,
                     u32 bucket_offset,
                     Jak2CommonTfragTextureUploadCapture* out) {
  if (transfer.tag_offset < bucket_offset) {
    return false;
  }
  const VifCode vif0(transfer.vif0);
  const VifCode vif1(transfer.vif1);
  auto& metadata = out->transfers[out->transfer_count++];
  metadata.relative_tag_offset = transfer.tag_offset - bucket_offset;
  metadata.payload_bytes = transfer.size_bytes;
  metadata.qwc = transfer.tag.qwc;
  metadata.tag_kind = static_cast<u8>(transfer.tag.kind);
  metadata.vif0_kind = static_cast<u8>(vif0.kind);
  metadata.vif0_immediate = vif0.immediate;
  metadata.vif1_kind = static_cast<u8>(vif1.kind);
  metadata.vif1_immediate = vif1.immediate;
  out->total_payload_bytes += transfer.size_bytes;
  return true;
}

Jak2CommonTfragTextureUploadClass classify(
    const Jak2CommonTfragTextureUploadCapture& capture) {
  if (capture.eye_markers != 0 || capture.other_transfers != 0) {
    return Jak2CommonTfragTextureUploadClass::EyeOrOther;
  }
  if (capture.gs_setup_transfers != 0) {
    if (capture.gs_setup_transfers == 1 && capture.direct_setup_transfers == 1 &&
        capture.ordinary_descriptors == 0 && capture.animator_arrays == 0) {
      return Jak2CommonTfragTextureUploadClass::GsSetupOnly;
    }
    return Jak2CommonTfragTextureUploadClass::EyeOrOther;
  }
  if (capture.direct_setup_transfers != 0 && capture.ordinary_descriptors == 0 &&
      capture.animator_arrays == 0) {
    return Jak2CommonTfragTextureUploadClass::EyeOrOther;
  }
  if (capture.ordinary_descriptors != 0 && capture.animator_arrays != 0) {
    return Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator;
  }
  if (capture.ordinary_descriptors != 0) {
    return Jak2CommonTfragTextureUploadClass::OrdinaryOnly;
  }
  if (capture.animator_arrays != 0) {
    return Jak2CommonTfragTextureUploadClass::AnimatorOnly;
  }
  return Jak2CommonTfragTextureUploadClass::Absent;
}

bool is_audited_tfrag_texture_upload_bucket(u32 bucket_id) {
  return bucket_id == kJak2CommonTfragTextureUploadBucket ||
         bucket_id == kJak2CommonPrisTextureUploadBucket ||
         std::find(kJak2NormalTfragTextureUploadBuckets.begin(),
                   kJak2NormalTfragTextureUploadBuckets.end(), bucket_id) !=
             kJak2NormalTfragTextureUploadBuckets.end() ||
         std::find(kJak2NormalShrubTextureUploadBuckets.begin(),
                   kJak2NormalShrubTextureUploadBuckets.end(), bucket_id) !=
             kJak2NormalShrubTextureUploadBuckets.end() ||
         std::find(kJak2AlphaTextureUploadBuckets.begin(),
                   kJak2AlphaTextureUploadBuckets.end(), bucket_id) !=
             kJak2AlphaTextureUploadBuckets.end() ||
         std::find(kJak2PrisTextureUploadBuckets.begin(),
                   kJak2PrisTextureUploadBuckets.end(), bucket_id) !=
             kJak2PrisTextureUploadBuckets.end() ||
         std::find(kJak2Pris2CaptureBuckets.begin(), kJak2Pris2CaptureBuckets.end(), bucket_id) !=
             kJak2Pris2CaptureBuckets.end() ||
         std::find(kJak2WaterTextureUploadBuckets.begin(),
                   kJak2WaterTextureUploadBuckets.end(), bucket_id) !=
             kJak2WaterTextureUploadBuckets.end();
}

bool is_normal_tfrag_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2NormalTfragTextureUploadBuckets.begin(),
                   kJak2NormalTfragTextureUploadBuckets.end(), bucket_id) !=
         kJak2NormalTfragTextureUploadBuckets.end();
}

bool is_normal_shrub_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2NormalShrubTextureUploadBuckets.begin(),
                   kJak2NormalShrubTextureUploadBuckets.end(), bucket_id) !=
         kJak2NormalShrubTextureUploadBuckets.end();
}

bool is_alpha_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2AlphaTextureUploadBuckets.begin(),
                   kJak2AlphaTextureUploadBuckets.end(), bucket_id) !=
         kJak2AlphaTextureUploadBuckets.end();
}

bool is_pris_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2PrisTextureUploadBuckets.begin(),
                   kJak2PrisTextureUploadBuckets.end(), bucket_id) !=
         kJak2PrisTextureUploadBuckets.end();
}

bool is_water_texture_upload_bucket(u32 bucket_id) {
  return std::find(kJak2WaterTextureUploadBuckets.begin(),
                   kJak2WaterTextureUploadBuckets.end(), bucket_id) !=
         kJak2WaterTextureUploadBuckets.end();
}

bool metadata_is_inert_next(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::NEXT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_strict_empty(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_ordinary_descriptor(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 1 &&
         transfer.payload_bytes == 16 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 3;
}

bool metadata_is_direct_setup(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 10 &&
         transfer.payload_bytes == 160 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::FLUSHA) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::DIRECT) &&
         transfer.vif1_immediate == 10;
}

bool metadata_is_gs_setup(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 2 &&
         transfer.payload_bytes == 32 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif0_immediate == 0 &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::DIRECT) &&
         transfer.vif1_immediate == 2;
}

bool metadata_is_animator_start(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kStartAnimatorArray &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_opcode27_body(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 31 &&
         transfer.payload_bytes == sizeof(Jak2Opcode27SkullGemPlan) &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kSkullGemOpcode &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_prison_jak_body(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) &&
         transfer.qwc == kJak2PrisPrisonJakAnimatorBodyBytes / 16 &&
         transfer.payload_bytes == kJak2PrisPrisonJakAnimatorBodyBytes &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kPrisonJakOpcode &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_dark_jak_body(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) &&
         transfer.qwc == kJak2CommonPrisDarkJakAnimatorBodyBytes / 16 &&
         transfer.payload_bytes == kJak2CommonPrisDarkJakAnimatorBodyBytes &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kDarkJakOpcode &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_opcode30_security_body(
    const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 52 &&
         transfer.payload_bytes == sizeof(Jak2Opcode30SecurityPlan) &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kSecurityOpcode &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool metadata_is_animator_finish(const Jak2CommonTfragTransferMetadata& transfer) {
  return transfer.tag_kind == static_cast<u8>(DmaTag::Kind::CNT) && transfer.qwc == 0 &&
         transfer.payload_bytes == 0 &&
         transfer.vif0_kind == static_cast<u8>(VifCode::Kind::PC_PORT) &&
         transfer.vif0_immediate == kFinishAnimatorArray &&
         transfer.vif1_kind == static_cast<u8>(VifCode::Kind::NOP) &&
         transfer.vif1_immediate == 0;
}

bool has_exact_opcode27_counts(const Jak2CommonTfragTextureUploadCapture& capture) {
  for (std::size_t i = 0; i < capture.opcode_counts.size(); ++i) {
    const u32 expected =
        i == kStartAnimatorArray || i == kFinishAnimatorArray || i == kSkullGemOpcode;
    if (capture.opcode_counts[i] != expected) {
      return false;
    }
  }
  return true;
}

bool has_exact_security_counts(const Jak2CommonTfragTextureUploadCapture& capture) {
  for (std::size_t i = 0; i < capture.opcode_counts.size(); ++i) {
    const u32 expected =
        i == kStartAnimatorArray || i == kFinishAnimatorArray || i == kSecurityOpcode;
    if (capture.opcode_counts[i] != expected) {
      return false;
    }
  }
  return true;
}

bool has_exact_prison_jak_counts(const Jak2CommonTfragTextureUploadCapture& capture,
                                 bool present) {
  for (std::size_t i = 0; i < capture.opcode_counts.size(); ++i) {
    const u32 expected = present &&
                                 (i == kStartAnimatorArray || i == kFinishAnimatorArray ||
                                  i == kPrisonJakOpcode)
                             ? 1
                             : 0;
    if (capture.opcode_counts[i] != expected) {
      return false;
    }
  }
  return true;
}

bool has_exact_dark_jak_counts(const Jak2CommonTfragTextureUploadCapture& capture) {
  for (std::size_t i = 0; i < capture.opcode_counts.size(); ++i) {
    const u32 expected = i == kStartAnimatorArray || i == kFinishAnimatorArray ||
                                 i == kDarkJakOpcode
                             ? 1
                             : 0;
    if (capture.opcode_counts[i] != expected) {
      return false;
    }
  }
  return true;
}

bool transfer_data_offset(u32 chain_offset,
                          u32 bucket_id,
                          const Jak2CommonTfragTransferMetadata& transfer,
                          std::size_t snapshot_size,
                          u64* out) {
  const u64 tag_offset = static_cast<u64>(chain_offset) + bucket_id * 16 +
                         transfer.relative_tag_offset;
  const u64 data_offset = tag_offset + 16;
  if (!out || !range_is_valid(data_offset, transfer.payload_bytes, snapshot_size)) {
    return false;
  }
  *out = data_offset;
  return true;
}

template <std::size_t Size>
bool copy_finite_floats(const u8* source, std::array<float, Size>* out) {
  std::memcpy(out->data(), source, out->size() * sizeof(float));
  return std::all_of(out->begin(), out->end(), [](float value) {
    return std::isfinite(value);
  });
}

bool parse_layer_values(const u8* source, Jak2Opcode27LayerValues* out) {
  if (!copy_finite_floats(source, &out->color) ||
      !copy_finite_floats(source + 16, &out->scale) ||
      !copy_finite_floats(source + 24, &out->offset) ||
      !copy_finite_floats(source + 32, &out->st_scale) ||
      !copy_finite_floats(source + 40, &out->st_offset) ||
      !copy_finite_floats(source + 48, &out->qs)) {
    return false;
  }
  out->rot = read_unaligned<float>(source + 64);
  out->st_rot = read_unaligned<float>(source + 68);
  std::memcpy(out->source_padding.data(), source + 72, out->source_padding.size());
  return std::isfinite(out->rot) && std::isfinite(out->st_rot);
}

bool parse_opcode27_skull_gem(const u8* source, Jak2Opcode27SkullGemPlan* out) {
  out->time = read_unaligned<float>(source);
  out->destination_tbp = read_unaligned<u32>(source + 4);
  std::memcpy(out->source_header_tail.data(), source + 8, out->source_header_tail.size());
  if (!std::isfinite(out->time) || out->destination_tbp >= kPs2VramTbpUpperBound) {
    return false;
  }

  const u8* layer_source = source + 16;
  for (auto& layer : out->layers) {
    if (!parse_layer_values(layer_source, &layer.start) ||
        !parse_layer_values(layer_source + sizeof(Jak2Opcode27LayerValues), &layer.end)) {
      return false;
    }
    layer_source += sizeof(Jak2Opcode27LayerTransition);
  }
  return true;
}

template <typename Plan>
bool parse_fixed_animation(const u8* source, Plan* out) {
  out->time = read_unaligned<float>(source);
  out->destination_tbp = read_unaligned<u32>(source + 4);
  std::memcpy(out->source_header_tail.data(), source + 8,
              out->source_header_tail.size());
  if (!std::isfinite(out->time) || out->destination_tbp >= kPs2VramTbpUpperBound) {
    return false;
  }

  const u8* layer_source = source + 16;
  for (auto& layer : out->layers) {
    if (!parse_layer_values(layer_source, &layer.start) ||
        !parse_layer_values(layer_source + sizeof(Jak2Opcode27LayerValues),
                            &layer.end)) {
      return false;
    }
    layer_source += sizeof(Jak2Opcode27LayerTransition);
  }
  return true;
}

bool parse_opcode30_security(const u8* source, Jak2Opcode30SecurityPlan* out) {
  return parse_fixed_animation(source, &out->environment) &&
         parse_fixed_animation(source + sizeof(out->environment), &out->dot);
}

constexpr u64 kFnvOffsetBasis = 14695981039346656037ull;
constexpr u64 kFnvPrime = 1099511628211ull;
constexpr u64 kGsSetAdRegisters = 0xeeeeeeeeeeeeeeeeull;

enum class DisplayValidation {
  Valid,
  Tag,
  Values,
};

void set_pris_eye_rejection(
    Jak2PrisEyeTextureUploadRejection* rejection,
    Jak2PrisEyeTextureUploadRejectReason reason,
    u8 chunk_index = kJak2PrisEyeRejectIndexNotApplicable,
    u8 body_index = kJak2PrisEyeRejectIndexNotApplicable) {
  if (rejection) {
    rejection->reason = reason;
    rejection->chunk_index = chunk_index;
    rejection->body_index = body_index;
  }
}

void hash_bytes(u64* hash, const void* bytes, std::size_t size) {
  const auto* input = static_cast<const u8*>(bytes);
  for (std::size_t i = 0; i < size; ++i) {
    *hash ^= input[i];
    *hash *= kFnvPrime;
  }
}

constexpr u64 make_gif_tag_word(u32 nloop, bool pre, u32 prim, u32 nreg) {
  return static_cast<u64>(nloop) | (1ull << 15) | (static_cast<u64>(pre) << 46) |
         (static_cast<u64>(prim) << 47) | (static_cast<u64>(nreg) << 60);
}

constexpr u64 make_scissor(u32 x0, u32 x1, u32 y0, u32 y1) {
  return static_cast<u64>(x0) | (static_cast<u64>(x1) << 16) |
         (static_cast<u64>(y0) << 32) | (static_cast<u64>(y1) << 48);
}

bool get_transfer_tag(const u8* snapshot,
                      std::size_t snapshot_size,
                      u32 chain_offset,
                      u32 bucket_id,
                      const Jak2CommonTfragTransferMetadata& transfer,
                      const u8** out_tag) {
  const u64 bucket_offset = static_cast<u64>(chain_offset) + bucket_id * 16;
  const u64 tag_offset = bucket_offset + transfer.relative_tag_offset;
  const std::size_t checked_size = std::min<std::size_t>(snapshot_size, EE_MAIN_MEM_SIZE);
  if (!snapshot || !out_tag || !range_is_valid(tag_offset, 16 + transfer.payload_bytes,
                                                checked_size)) {
    return false;
  }
  *out_tag = snapshot + tag_offset;
  return true;
}

bool get_plain_cnt_payload(const u8* snapshot,
                           std::size_t snapshot_size,
                           u32 chain_offset,
                           u32 bucket_id,
                           const Jak2CommonTfragTransferMetadata& transfer,
                           u16 qwc,
                           u32 vif0,
                           u32 vif1,
                           const u8** out_payload) {
  const u8* tag = nullptr;
  if (!get_transfer_tag(snapshot, snapshot_size, chain_offset, bucket_id, transfer, &tag) ||
      transfer.tag_kind != static_cast<u8>(DmaTag::Kind::CNT) || transfer.qwc != qwc ||
      transfer.payload_bytes != static_cast<u32>(qwc) * 16 ||
      read_unaligned<u64>(tag) !=
          (static_cast<u64>(qwc) | (static_cast<u64>(DmaTag::Kind::CNT) << 28)) ||
      read_unaligned<u32>(tag + 8) != vif0 || read_unaligned<u32>(tag + 12) != vif1) {
    return false;
  }
  *out_payload = tag + 16;
  return true;
}

bool has_plain_next_tag(const u8* snapshot,
                        std::size_t snapshot_size,
                        u32 chain_offset,
                        u32 bucket_id,
                        const Jak2CommonTfragTransferMetadata& transfer) {
  const u8* tag = nullptr;
  if (!get_transfer_tag(snapshot, snapshot_size, chain_offset, bucket_id, transfer, &tag) ||
      !metadata_is_inert_next(transfer)) {
    return false;
  }
  const u64 raw = read_unaligned<u64>(tag);
  return static_cast<u32>(raw) == (static_cast<u32>(DmaTag::Kind::NEXT) << 28) &&
         (raw >> 32) != 0 && read_unaligned<u32>(tag + 8) == 0 &&
         read_unaligned<u32>(tag + 12) == 0;
}

bool validate_ad_gif_tag(const u8* payload, u32 nloop, u32 nreg, u64 registers) {
  return read_unaligned<u64>(payload) == make_gif_tag_word(nloop, false, 0, nreg) &&
         read_unaligned<u64>(payload + 8) == registers;
}

bool validate_single_ad(const u8* payload, GsRegisterAddress address, u64 value) {
  return validate_ad_gif_tag(payload, 1, 1, kGsSetAdRegisters) &&
         read_unaligned<u64>(payload + 16) == value &&
         read_unaligned<u64>(payload + 24) == static_cast<u64>(address);
}

DisplayValidation validate_display_setup(const u8* payload,
                                         Jak2PrisEyeResolution* resolution,
                                         u32* source_fbp) {
  constexpr std::array<GsRegisterAddress, 7> kAddresses = {
      GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
      GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
      GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
      GsRegisterAddress::TEXFLUSH};
  if (!validate_ad_gif_tag(payload, 1, kAddresses.size(), kGsSetAdRegisters)) {
    return DisplayValidation::Tag;
  }

  std::array<u64, kAddresses.size()> values = {};
  for (std::size_t i = 0; i < kAddresses.size(); ++i) {
    values[i] = read_unaligned<u64>(payload + 16 + i * 16);
    if (read_unaligned<u64>(payload + 24 + i * 16) !=
        static_cast<u64>(kAddresses[i])) {
      return DisplayValidation::Values;
    }
  }

  const GsFrame frame(values[2]);
  const bool eye32 = values[0] == make_scissor(0, 63, 0, 511) &&
                     values[1] == (512ull | (512ull << 32)) && frame.fbw() == 1;
  const bool eye64 = values[0] == make_scissor(0, 127, 0, 255) &&
                     values[1] == (1024ull | (1024ull << 32)) && frame.fbw() == 2;
  if (eye32 == eye64 || frame.psm() != GsFrame::PSM::PSMCT32 || frame.fbmsk() != 0 ||
      values[2] != (static_cast<u64>(frame.fbp()) |
                    (static_cast<u64>(frame.fbw()) << 16)) ||
      values[3] != 0x30000 || values[4] != 0x8000000080ull ||
      values[5] != (0x130ull | (1ull << 24) | (1ull << 32)) || values[6] != 0) {
    return DisplayValidation::Values;
  }

  *resolution = eye32 ? Jak2PrisEyeResolution::Eye32 : Jak2PrisEyeResolution::Eye64;
  *source_fbp = frame.fbp();
  return DisplayValidation::Valid;
}

DisplayValidation validate_display_reset(const u8* payload) {
  constexpr std::array<GsRegisterAddress, 7> kAddresses = {
      GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
      GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
      GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
      GsRegisterAddress::TEXFLUSH};
  constexpr std::array<u64, 7> kValues = {
      make_scissor(0, 511, 0, 415), 0x730000007000ull,
      0x198ull | (8ull << 16),       0x50000ull,
      0x8000000000ull,               0x130ull | (1ull << 24),
      0};
  if (!validate_ad_gif_tag(payload, 1, kAddresses.size(), kGsSetAdRegisters)) {
    return DisplayValidation::Tag;
  }
  for (std::size_t i = 0; i < kAddresses.size(); ++i) {
    if (read_unaligned<u64>(payload + 16 + i * 16) != kValues[i] ||
        read_unaligned<u64>(payload + 24 + i * 16) !=
            static_cast<u64>(kAddresses[i])) {
      return DisplayValidation::Values;
    }
  }
  return DisplayValidation::Valid;
}

bool is_valid_eye_texture_psm(GsTex0::PSM psm) {
  switch (psm) {
    case GsTex0::PSM::PSMCT32:
    case GsTex0::PSM::PSMCT24:
    case GsTex0::PSM::PSMCT16:
    case GsTex0::PSM::PSMCT16S:
    case GsTex0::PSM::PSMT8:
    case GsTex0::PSM::PSMT4:
    case GsTex0::PSM::PSMT8H:
    case GsTex0::PSM::PSMT4HL:
    case GsTex0::PSM::PSMT4HH:
      return true;
    default:
      return false;
  }
}

bool validate_eye_adgif(const u8* payload,
                        Jak2PrisEyeResolution resolution,
                        u64 alpha,
                        u32* uv1_u,
                        u32* uv1_v) {
  constexpr u64 kAdRegisters = static_cast<u64>(GifTag::RegisterDescriptor::AD);
  const u64 max_uv = resolution == Jak2PrisEyeResolution::Eye32 ? 31 : 63;
  const u64 expected_clamp = 1ull | (1ull << 2) | (max_uv << 14) | (max_uv << 34);
  if (!uv1_u || !uv1_v || !validate_ad_gif_tag(payload, 5, 1, kAdRegisters)) {
    return false;
  }
  const auto adgif = read_unaligned<AdGifData>(payload + 16);
  const GsTex0 tex0(adgif.tex0_data);
  const u32 cld = (adgif.tex0_data >> 61) & 7;
  if (static_cast<u8>(adgif.tex0_addr) != static_cast<u8>(GsRegisterAddress::TEX0_1) ||
      static_cast<u8>(adgif.tex1_addr) != static_cast<u8>(GsRegisterAddress::TEX1_1) ||
      static_cast<u8>(adgif.mip_addr) != static_cast<u8>(GsRegisterAddress::MIPTBP1_1) ||
      static_cast<u8>(adgif.clamp_addr) != static_cast<u8>(GsRegisterAddress::CLAMP_1) ||
      static_cast<u8>(adgif.alpha_addr) != static_cast<u8>(GsRegisterAddress::ALPHA_1) ||
      adgif.clamp_data != expected_clamp || adgif.alpha_data != alpha ||
      tex0.tbw() == 0 || tex0.tw() > 11 || tex0.th() > 11 || tex0.tcc() != 1 ||
      cld != 1 || !is_valid_eye_texture_psm(tex0.psm())) {
    return false;
  }
  *uv1_u = 16u << tex0.tw();
  *uv1_v = 16u << tex0.th();
  return true;
}

bool validate_eye_scissor(const u8* payload, u64 expected) {
  return validate_single_ad(payload, GsRegisterAddress::SCISSOR_1, expected);
}

bool validate_eye_sprite(const u8* payload,
                         bool alpha_blend,
                         u32 alpha,
                         u32 uv1_u,
                         u32 uv1_v,
                         u32 sprite_index,
                         u32 eye_width,
                         bool exact_background,
                         u32 background_x0,
                         u32 background_y0,
                         u32 background_x1,
                         u32 background_y1) {
  constexpr u64 kSpriteRegisters =
      static_cast<u64>(GifTag::RegisterDescriptor::RGBAQ) |
      (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 4) |
      (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 8) |
      (static_cast<u64>(GifTag::RegisterDescriptor::UV) << 12) |
      (static_cast<u64>(GifTag::RegisterDescriptor::XYZ2) << 16);
  const u32 prim = static_cast<u32>(GsPrim::Kind::SPRITE) | (1u << 4) |
                   (static_cast<u32>(alpha_blend) << 6) | (1u << 8);
  if (read_unaligned<u64>(payload) != make_gif_tag_word(1, true, prim, 5) ||
      read_unaligned<u64>(payload + 8) != kSpriteRegisters ||
      read_unaligned<u32>(payload + 16) != 128 ||
      read_unaligned<u32>(payload + 20) != 128 ||
      read_unaligned<u32>(payload + 24) != 128 ||
      read_unaligned<u32>(payload + 28) != alpha ||
      read_unaligned<u32>(payload + 40) != 0 ||
      read_unaligned<u32>(payload + 44) != 0 ||
      read_unaligned<u32>(payload + 56) != 0xffffff ||
      read_unaligned<u32>(payload + 60) != 0 ||
      read_unaligned<u32>(payload + 72) != 0 ||
      read_unaligned<u32>(payload + 76) != 0 ||
      read_unaligned<u32>(payload + 88) != 0xffffff ||
      read_unaligned<u32>(payload + 92) != 0) {
    return false;
  }
  const u32 x0 = read_unaligned<u32>(payload + 48);
  const u32 y0 = read_unaligned<u32>(payload + 52);
  const u32 x1 = read_unaligned<u32>(payload + 80);
  const u32 y1 = read_unaligned<u32>(payload + 84);
  if (x0 > 0xffff || y0 > 0xffff || x1 > 0xffff || y1 > 0xffff) {
    return false;
  }
  if (exact_background) {
    return read_unaligned<u64>(payload + 32) == 0 && x0 == background_x0 &&
           y0 == background_y0 && read_unaligned<u64>(payload + 64) == 0 &&
           x1 == background_x1 && y1 == background_y1;
  }

  if (read_unaligned<u64>(payload + 32) != 0 ||
      read_unaligned<u32>(payload + 64) != uv1_u ||
      read_unaligned<u32>(payload + 68) != uv1_v || y0 > y1) {
    return false;
  }
  const bool mirrored_lid = sprite_index == 6;
  if ((!mirrored_lid && x0 > x1) || (mirrored_lid && x0 < x1)) {
    return false;
  }
  if (sprite_index == 5 && (x0 != eye_width * 16 || x1 != eye_width * 2 * 16)) {
    return false;
  }
  if (sprite_index == 6 && (x0 != eye_width * 3 * 16 || x1 != eye_width * 2 * 16)) {
    return false;
  }
  return true;
}

bool parse_pris_eye_chunk(const u8* snapshot,
                          std::size_t snapshot_size,
                          u32 chain_offset,
                          u32 bucket_id,
                          const Jak2CommonTfragTextureUploadCapture& capture,
                          u32 start_transfer_index,
                          u8 chunk_index,
                          Jak2PrisEyeChunkPlan* out,
                          u32* source_fbp,
                          Jak2PrisEyeTextureUploadRejection* rejection) {
  constexpr std::array<u8, 22> kBodyKinds = {
      0, 1, 2, 1, 2, 0, 1, 2, 3, 0, 1,
      2, 0, 1, 2, 3, 0, 1, 2, 0, 1, 2};
  constexpr std::array<u32, 7> kSpriteAlpha = {128, 128, 128, 128, 128, 0, 0};
  constexpr std::array<bool, 7> kSpriteBlend = {false, false, false, true, true, true, true};
  constexpr std::array<u64, 6> kAdgifAlpha = {0x44, 0x44, 0x44, 0x44, 1, 1};

  if (!out || !source_fbp ||
      start_transfer_index + kJak2PrisEyeChunkTransferCount > capture.transfer_count) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::ChunkBounds,
                           chunk_index);
    return false;
  }

  const u8* payload = nullptr;
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                             capture.transfers[start_transfer_index], 8, kFlushaVif,
                             kDirectVif | 8, &payload)) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::SetupTransfer,
                           chunk_index);
    return false;
  }
  const auto setup_validation = validate_display_setup(payload, &out->resolution, source_fbp);
  if (setup_validation != DisplayValidation::Valid) {
    set_pris_eye_rejection(
        rejection,
        setup_validation == DisplayValidation::Tag
            ? Jak2PrisEyeTextureUploadRejectReason::SetupTag
            : Jak2PrisEyeTextureUploadRejectReason::SetupValues,
        chunk_index);
    return false;
  }
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                             capture.transfers[start_transfer_index + 1], 2, 0,
                             kDirectVif | 2, &payload) ||
      !validate_single_ad(payload, GsRegisterAddress::TEST_1, 0x30003)) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::InitialTest,
                           chunk_index);
    return false;
  }

  const u32 eye_width = out->resolution == Jak2PrisEyeResolution::Eye32 ? 32 : 64;
  const u32 full_width = eye_width * 2;
  u32 y0 = 0;
  u32 pair_index = 0;
  u32 adgif_index = 0;
  u32 scissor_index = 0;
  u32 sprite_index = 0;
  u32 test_index = 0;
  u32 current_uv1_u = 0;
  u32 current_uv1_v = 0;
  for (u32 body_index = 0; body_index < kBodyKinds.size(); ++body_index) {
    const u32 transfer_index = start_transfer_index + 2 + body_index;
    switch (kBodyKinds[body_index]) {
      case 0:
        if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                                   capture.transfers[transfer_index], 6, 0, kDirectVif | 6,
                                   &payload) ||
            !validate_eye_adgif(payload, out->resolution, kAdgifAlpha[adgif_index],
                                &current_uv1_u, &current_uv1_v)) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodyAdgif,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        adgif_index++;
        break;
      case 1: {
        if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                                   capture.transfers[transfer_index], 2, 0, kDirectVif | 2,
                                   &payload)) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodyScissor,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        if (scissor_index == 0) {
          const u64 raw_scissor = read_unaligned<u64>(payload + 16);
          const GsScissor scissor(raw_scissor);
          if (scissor.x0() != 0 || scissor.x1() != full_width - 1 ||
              scissor.y1() != scissor.y0() + eye_width - 1 ||
              scissor.y0() % eye_width != 0) {
            set_pris_eye_rejection(rejection,
                                   Jak2PrisEyeTextureUploadRejectReason::BodyScissor,
                                   chunk_index, static_cast<u8>(body_index));
            return false;
          }
          y0 = scissor.y0();
          pair_index = out->resolution == Jak2PrisEyeResolution::Eye32
                           ? y0 / eye_width
                           : (y0 / eye_width) * 4;
          if (pair_index >= 20) {
            set_pris_eye_rejection(rejection,
                                   Jak2PrisEyeTextureUploadRejectReason::BodyScissor,
                                   chunk_index, static_cast<u8>(body_index));
            return false;
          }
        }
        const bool right = scissor_index != 0 && (scissor_index % 2) == 0;
        const u32 expected_x0 = scissor_index == 0 ? 0 : (right ? eye_width : 0);
        const u32 expected_x1 = scissor_index == 0 ? full_width - 1
                                                   : expected_x0 + eye_width - 1;
        if (!validate_eye_scissor(payload,
                                  make_scissor(expected_x0, expected_x1, y0,
                                               y0 + eye_width - 1))) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodyScissor,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        scissor_index++;
      } break;
      case 2: {
        if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                                   capture.transfers[transfer_index], 6, 0, kDirectVif | 6,
                                   &payload)) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodySprite,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        const bool background = sprite_index == 0;
        const u32 group = out->resolution == Jak2PrisEyeResolution::Eye32
                              ? pair_index
                              : pair_index / 4;
        const u32 background_x0 = eye_width * 16;
        const u32 background_y0 = (group * eye_width + eye_width) * 16;
        if (!validate_eye_sprite(payload, kSpriteBlend[sprite_index],
                                 kSpriteAlpha[sprite_index], current_uv1_u, current_uv1_v,
                                 sprite_index, eye_width, background, background_x0,
                                 background_y0, (eye_width + full_width) * 16,
                                 background_y0 + eye_width * 16)) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodySprite,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        sprite_index++;
      } break;
      case 3:
        if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                                   capture.transfers[transfer_index], 2, 0, kDirectVif | 2,
                                   &payload) ||
            !validate_single_ad(payload, GsRegisterAddress::TEST_1,
                                test_index++ == 0 ? 0x33001 : 0x30003)) {
          set_pris_eye_rejection(rejection,
                                 Jak2PrisEyeTextureUploadRejectReason::BodyTest,
                                 chunk_index, static_cast<u8>(body_index));
          return false;
        }
        break;
      default:
        return false;
    }
  }

  if (adgif_index != kAdgifAlpha.size() || scissor_index != 7 ||
      sprite_index != kSpriteAlpha.size() || test_index != 2) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::ChunkBounds,
                           chunk_index);
    return false;
  }
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                             capture.transfers[start_transfer_index + 24], 8, kFlushaVif,
                             kDirectVif | 8, &payload)) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::ResetTransfer,
                           chunk_index);
    return false;
  }
  const auto reset_validation = validate_display_reset(payload);
  if (reset_validation != DisplayValidation::Valid) {
    set_pris_eye_rejection(
        rejection,
        reset_validation == DisplayValidation::Tag
            ? Jak2PrisEyeTextureUploadRejectReason::ResetTag
            : Jak2PrisEyeTextureUploadRejectReason::ResetValues,
        chunk_index);
    return false;
  }
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                             capture.transfers[start_transfer_index + 25], 2, 0,
                             kDirectVif | 2, &payload) ||
      !validate_single_ad(payload, GsRegisterAddress::ALPHA_1, 0x44)) {
    set_pris_eye_rejection(rejection, Jak2PrisEyeTextureUploadRejectReason::Alpha,
                           chunk_index);
    return false;
  }

  u64 fingerprint = kFnvOffsetBasis;
  for (u32 i = 0; i < kJak2PrisEyeChunkTransferCount; ++i) {
    const auto& transfer = capture.transfers[start_transfer_index + i];
    const u8* tag = nullptr;
    if (!get_transfer_tag(snapshot, snapshot_size, chain_offset, bucket_id, transfer, &tag)) {
      set_pris_eye_rejection(rejection,
                             Jak2PrisEyeTextureUploadRejectReason::ChunkFingerprint,
                             chunk_index);
      return false;
    }
    hash_bytes(&fingerprint, tag, 16 + transfer.payload_bytes);
  }

  out->pair_index = pair_index;
  out->start_transfer_index = start_transfer_index;
  out->start_relative_tag_offset = capture.transfers[start_transfer_index].relative_tag_offset;
  out->linker_transfer_index = start_transfer_index + kJak2PrisEyeChunkTransferCount;
  out->linker_relative_tag_offset =
      capture.transfers[out->linker_transfer_index].relative_tag_offset;
  out->transfer_count = kJak2PrisEyeChunkTransferCount;
  out->payload_bytes = kJak2PrisEyeChunkPayloadBytes;
  out->eye_slot_mask = 3ull << (pair_index * 2);
  out->semantic_fingerprint = fingerprint;
  return true;
}

bool parse_pris_prison_jak_animator(
    const u8* snapshot,
    std::size_t snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const Jak2CommonTfragTextureUploadCapture& capture,
    u32 start_transfer_index,
    Jak2PrisPrisonJakAnimatorPlan* out) {
  if (!out || start_transfer_index + 3 >= capture.transfer_count ||
      !metadata_is_animator_start(capture.transfers[start_transfer_index]) ||
      !metadata_is_prison_jak_body(capture.transfers[start_transfer_index + 1]) ||
      !metadata_is_animator_finish(capture.transfers[start_transfer_index + 2]) ||
      !metadata_is_inert_next(capture.transfers[start_transfer_index + 3]) ||
      !has_plain_next_tag(snapshot, snapshot_size, chain_offset, bucket_id,
                          capture.transfers[start_transfer_index + 3])) {
    return false;
  }

  const u8* payload = nullptr;
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset, bucket_id,
                             capture.transfers[start_transfer_index + 1],
                             kJak2PrisPrisonJakAnimatorBodyBytes / 16,
                             kPcPortVif | kPrisonJakOpcode, 0, &payload)) {
    return false;
  }

  out->morph = read_unaligned<float>(payload);
  if (!std::isfinite(out->morph) || out->morph < 0.f || out->morph > 1.f) {
    return false;
  }
  for (std::size_t i = 0; i < out->destination_tbps.size(); ++i) {
    const u32 tbp = read_unaligned<u32>(payload + 16 + i * sizeof(u32));
    if (tbp != kJak2PrisPrisonJakAnimatorMissingTbp &&
        tbp >= kJak2PrisPrisonJakAnimatorTbpUpperBound) {
      return false;
    }
    out->destination_tbps[i] = tbp;
  }
  // pc-clut-blender writes morph.x and the seven TBPs only. The vector tail
  // and final four bytes are source-owned opaque bytes, not zero padding.
  std::memcpy(out->source_padding.data(), payload + 4, 12);
  std::memcpy(out->source_padding.data() + 12, payload + 44, 4);

  out->start_transfer_index = start_transfer_index;
  out->start_relative_tag_offset =
      capture.transfers[start_transfer_index].relative_tag_offset;
  out->body_transfer_index = start_transfer_index + 1;
  out->body_relative_tag_offset =
      capture.transfers[out->body_transfer_index].relative_tag_offset;
  out->finish_transfer_index = start_transfer_index + 2;
  out->finish_relative_tag_offset =
      capture.transfers[out->finish_transfer_index].relative_tag_offset;
  out->linker_transfer_index = start_transfer_index + 3;
  out->linker_relative_tag_offset =
      capture.transfers[out->linker_transfer_index].relative_tag_offset;

  u64 fingerprint = kFnvOffsetBasis;
  hash_bytes(&fingerprint, payload, kJak2PrisPrisonJakAnimatorBodyBytes);
  out->semantic_fingerprint = fingerprint;
  return true;
}

bool parse_common_pris_dark_jak_animator(
    const u8* snapshot,
    std::size_t snapshot_size,
    u32 chain_offset,
    const Jak2CommonTfragTextureUploadCapture& capture,
    u32 start_transfer_index,
    Jak2CommonPrisDarkJakAnimatorPlan* out) {
  if (!out || start_transfer_index + 3 >= capture.transfer_count ||
      !metadata_is_animator_start(capture.transfers[start_transfer_index]) ||
      !metadata_is_dark_jak_body(capture.transfers[start_transfer_index + 1]) ||
      !metadata_is_animator_finish(capture.transfers[start_transfer_index + 2]) ||
      !metadata_is_inert_next(capture.transfers[start_transfer_index + 3]) ||
      !has_plain_next_tag(snapshot, snapshot_size, chain_offset,
                          kJak2CommonPrisTextureUploadBucket,
                          capture.transfers[start_transfer_index + 3])) {
    return false;
  }

  const u8* payload = nullptr;
  if (!get_plain_cnt_payload(snapshot, snapshot_size, chain_offset,
                             kJak2CommonPrisTextureUploadBucket,
                             capture.transfers[start_transfer_index + 1],
                             kJak2CommonPrisDarkJakAnimatorBodyBytes / 16,
                             kPcPortVif | kDarkJakOpcode, 0, &payload)) {
    return false;
  }

  out->morph = read_unaligned<float>(payload);
  if (!std::isfinite(out->morph) || out->morph < 0.f || out->morph > 1.f) {
    return false;
  }
  std::memcpy(out->source_padding.data(), payload + 4, out->source_padding.size());
  for (std::size_t i = 0; i < out->destination_tbps.size(); ++i) {
    const u32 tbp = read_unaligned<u32>(payload + 16 + i * sizeof(u32));
    if (tbp != kJak2PrisPrisonJakAnimatorMissingTbp &&
        tbp >= kJak2PrisPrisonJakAnimatorTbpUpperBound) {
      return false;
    }
    out->destination_tbps[i] = tbp;
  }

  out->start_transfer_index = start_transfer_index;
  out->start_relative_tag_offset = capture.transfers[start_transfer_index].relative_tag_offset;
  out->body_transfer_index = start_transfer_index + 1;
  out->body_relative_tag_offset = capture.transfers[out->body_transfer_index].relative_tag_offset;
  out->finish_transfer_index = start_transfer_index + 2;
  out->finish_relative_tag_offset =
      capture.transfers[out->finish_transfer_index].relative_tag_offset;
  out->linker_transfer_index = start_transfer_index + 3;
  out->linker_relative_tag_offset =
      capture.transfers[out->linker_transfer_index].relative_tag_offset;

  u64 fingerprint = kFnvOffsetBasis;
  hash_bytes(&fingerprint, payload, kJak2CommonPrisDarkJakAnimatorBodyBytes);
  out->semantic_fingerprint = fingerprint;
  return true;
}

bool page_header_is_valid(const u8* live_ee_memory,
                          std::size_t live_ee_memory_size,
                          u64 page_offset) {
  const std::size_t checked_size =
      std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  if (!live_ee_memory || page_offset == 0 ||
      !range_is_valid(page_offset, sizeof(GoalTexturePageHeaderLayout), checked_size)) {
    return false;
  }
  const auto header =
      read_unaligned<GoalTexturePageHeaderLayout>(live_ee_memory + page_offset);
  if (header.length < 0) {
    return false;
  }
  const u64 texture_pointer_bytes = static_cast<u64>(header.length) * sizeof(u32);
  return range_is_valid(page_offset + sizeof(header), texture_pointer_bytes, checked_size);
}

}  // namespace

const char* jak2_pris_eye_texture_upload_reject_reason_name(
    Jak2PrisEyeTextureUploadRejectReason reason) {
  switch (reason) {
    case Jak2PrisEyeTextureUploadRejectReason::None:
      return "none";
    case Jak2PrisEyeTextureUploadRejectReason::UnsupportedBucket:
      return "unsupported-bucket";
    case Jak2PrisEyeTextureUploadRejectReason::CaptureInvalid:
      return "capture-invalid";
    case Jak2PrisEyeTextureUploadRejectReason::AbsentEnvelope:
      return "absent-envelope";
    case Jak2PrisEyeTextureUploadRejectReason::Counts:
      return "counts";
    case Jak2PrisEyeTextureUploadRejectReason::Opening:
      return "opening";
    case Jak2PrisEyeTextureUploadRejectReason::Descriptor:
      return "descriptor";
    case Jak2PrisEyeTextureUploadRejectReason::Page:
      return "page";
    case Jak2PrisEyeTextureUploadRejectReason::ChunkBounds:
      return "chunk-bounds";
    case Jak2PrisEyeTextureUploadRejectReason::SetupTransfer:
      return "setup-transfer";
    case Jak2PrisEyeTextureUploadRejectReason::SetupTag:
      return "setup-tag";
    case Jak2PrisEyeTextureUploadRejectReason::SetupValues:
      return "setup-values";
    case Jak2PrisEyeTextureUploadRejectReason::InitialTest:
      return "initial-test";
    case Jak2PrisEyeTextureUploadRejectReason::BodyAdgif:
      return "body-adgif";
    case Jak2PrisEyeTextureUploadRejectReason::BodyScissor:
      return "body-scissor";
    case Jak2PrisEyeTextureUploadRejectReason::BodySprite:
      return "body-sprite";
    case Jak2PrisEyeTextureUploadRejectReason::BodyTest:
      return "body-test";
    case Jak2PrisEyeTextureUploadRejectReason::ResetTransfer:
      return "reset-transfer";
    case Jak2PrisEyeTextureUploadRejectReason::ResetTag:
      return "reset-tag";
    case Jak2PrisEyeTextureUploadRejectReason::ResetValues:
      return "reset-values";
    case Jak2PrisEyeTextureUploadRejectReason::Alpha:
      return "alpha";
    case Jak2PrisEyeTextureUploadRejectReason::Linker:
      return "linker";
    case Jak2PrisEyeTextureUploadRejectReason::SourceFramebuffer:
      return "source-framebuffer";
    case Jak2PrisEyeTextureUploadRejectReason::DuplicateEyeSlots:
      return "duplicate-eye-slots";
    case Jak2PrisEyeTextureUploadRejectReason::DefaultReset:
      return "default-reset";
    case Jak2PrisEyeTextureUploadRejectReason::Terminal:
      return "terminal";
    case Jak2PrisEyeTextureUploadRejectReason::ChunkFingerprint:
      return "chunk-fingerprint";
  }
  return "unknown";
}

Jak2CommonTfragTextureUploadCapture capture_jak2_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id) {
  Jak2CommonTfragTextureUploadCapture out;
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + bucket_id * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  const std::size_t checked_packet_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!dma_packet_snapshot || !is_audited_tfrag_texture_upload_bucket(bucket_id) ||
      (chain_offset & 15) != 0 ||
      bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16, checked_packet_size)) {
    out.malformed_transfers = 1;
    return out;
  }

  const u32 bucket_offset = static_cast<u32>(bucket_offset64);
  const u32 end_offset = static_cast<u32>(end_offset64);
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size,
                         bucket_offset);
  bool inside_animator = false;
  while (dma.offset() != end_offset) {
    if (out.transfer_count == out.transfers.size()) {
      out.malformed_transfers++;
      return out;
    }

    CheckedTransfer transfer;
    if (!dma.read(&transfer)) {
      out.malformed_transfers++;
      return out;
    }
    if (!record_transfer(transfer, bucket_offset, &out)) {
      out.malformed_transfers++;
      return out;
    }

    const VifCode vif0(transfer.vif0);

    if (inside_animator) {
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate < out.opcode_counts.size()) {
        out.opcode_counts[vif0.immediate]++;
      }
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate == kStartAnimatorArray) {
        out.malformed_transfers++;
        return out;
      }
      if (vif0.kind == VifCode::Kind::PC_PORT &&
          vif0.immediate == kFinishAnimatorArray) {
        if (!is_exact_animator_finish(transfer)) {
          out.malformed_transfers++;
          return out;
        }
        inside_animator = false;
        continue;
      }
      out.animator_body_transfers++;
      out.animator_payload_bytes += transfer.size_bytes;
      continue;
    }

    if (vif0.kind == VifCode::Kind::PC_PORT &&
        vif0.immediate == kStartAnimatorArray) {
      if (!is_exact_animator_start(transfer)) {
        out.malformed_transfers++;
        return out;
      }
      out.present = true;
      out.animator_arrays++;
      out.opcode_counts[kStartAnimatorArray]++;
      inside_animator = true;
      continue;
    }

    if (transfer.tag.qwc == 8) {
      out.present = true;
      out.eye_markers++;
      continue;
    }
    if (is_exact_direct_setup(transfer)) {
      out.present = true;
      out.direct_setup_transfers++;
      continue;
    }
    if (is_exact_gs_setup(transfer)) {
      out.present = true;
      out.gs_setup_transfers++;
      continue;
    }
    if (is_inert(transfer)) {
      if (transfer.tag.kind == DmaTag::Kind::CNT || transfer.tag.kind == DmaTag::Kind::NEXT) {
        out.inert_transfers++;
      } else {
        out.present = true;
        out.other_transfers++;
      }
      continue;
    }
    out.present = true;
    if (is_ordinary_descriptor(transfer)) {
      out.ordinary_descriptors++;
    } else {
      out.other_transfers++;
    }
  }

  if (inside_animator) {
    out.malformed_transfers++;
    return out;
  }

  out.valid = true;
  out.classification = classify(out);
  return out;
}

std::optional<Jak2NormalTfragTextureUploadPlan> plan_jak2_normal_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }

  if (!is_normal_tfrag_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2NormalTfragTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool exact_counts =
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 176 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.animator_arrays == 0 &&
      capture.eye_markers == 0 && capture.other_transfers == 0 &&
      capture.malformed_transfers == 0;
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_inert_next(capture.transfers[4])) {
    return std::nullopt;
  }

  const bool descriptor_first = metadata_is_ordinary_descriptor(capture.transfers[1]) &&
                                metadata_is_direct_setup(capture.transfers[3]);
  if (!descriptor_first) {
    return std::nullopt;
  }
  const auto& descriptor = capture.transfers[1];
  const u64 descriptor_tag_offset = static_cast<u64>(chain_offset) + bucket_id * 16 +
                                    descriptor.relative_tag_offset;
  const u64 descriptor_data_offset = descriptor_tag_offset + 16;
  const std::size_t checked_snapshot_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!range_is_valid(descriptor_data_offset, 16, checked_snapshot_size)) {
    return std::nullopt;
  }
  const u64 page_offset =
      read_unaligned<u64>(dma_packet_snapshot + descriptor_data_offset);
  const s64 mode =
      read_unaligned<s64>(dma_packet_snapshot + descriptor_data_offset + sizeof(u64));
  if (mode != -1 || !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return std::nullopt;
  }

  plan.present = true;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());
  return plan;
}

std::optional<Jak2PrisEyeTextureUploadPlan> plan_jak2_pris_eye_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture,
    Jak2PrisEyeTextureUploadRejection* out_rejection) {
  if (out_rejection) {
    *out_rejection = {};
  }
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }
  if (!is_pris_texture_upload_bucket(bucket_id)) {
    set_pris_eye_rejection(out_rejection,
                           Jak2PrisEyeTextureUploadRejectReason::UnsupportedBucket);
    return std::nullopt;
  }
  if (!capture.valid) {
    set_pris_eye_rejection(out_rejection,
                           Jak2PrisEyeTextureUploadRejectReason::CaptureInvalid);
    return std::nullopt;
  }

  Jak2PrisEyeTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      set_pris_eye_rejection(out_rejection,
                             Jak2PrisEyeTextureUploadRejectReason::AbsentEnvelope);
      return std::nullopt;
    }
    return plan;
  }

  const bool has_prison_jak_animator = capture.transfer_count == 9 ||
                                       capture.transfer_count == 36 ||
                                       capture.transfer_count == 63;
  const std::size_t chunk_count =
      capture.transfer_count == (has_prison_jak_animator ? 36 : 32)
          ? 1
          : capture.transfer_count == (has_prison_jak_animator ? 63 : 59) ? 2 : 0;
  const bool ordinary_only = capture.transfer_count == (has_prison_jak_animator ? 9 : 5);
  const u32 expected_base_transfer_count = chunk_count == 0 ? 5 : chunk_count == 1 ? 32 : 59;
  const u32 expected_transfer_count =
      expected_base_transfer_count + (has_prison_jak_animator ? 4 : 0);
  const u64 expected_payload_bytes =
      (chunk_count == 0 ? 176 : chunk_count == 1 ? 2032 : 3888) +
      (has_prison_jak_animator ? kJak2PrisPrisonJakAnimatorBodyBytes : 0);
  const u32 expected_inert =
      (chunk_count == 0 ? 3 : chunk_count == 1 ? 4 : 5) +
      (has_prison_jak_animator ? 1 : 0);
  const u32 expected_eye_markers = static_cast<u32>(chunk_count) * 2;
  const u32 expected_gs = static_cast<u32>(chunk_count) * 11;
  const u32 expected_other = static_cast<u32>(chunk_count) * 13;
  const bool exact_counts =
      capture.transfer_count == expected_transfer_count && (ordinary_only || chunk_count != 0) &&
      capture.classification ==
          (chunk_count != 0
               ? Jak2CommonTfragTextureUploadClass::EyeOrOther
               : has_prison_jak_animator ? Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator
                                          : Jak2CommonTfragTextureUploadClass::OrdinaryOnly) &&
      capture.total_payload_bytes == expected_payload_bytes &&
      capture.inert_transfers == expected_inert && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == expected_gs &&
      capture.animator_arrays == (has_prison_jak_animator ? 1 : 0) &&
      capture.animator_body_transfers == (has_prison_jak_animator ? 1 : 0) &&
      capture.animator_payload_bytes ==
          (has_prison_jak_animator ? kJak2PrisPrisonJakAnimatorBodyBytes : 0) &&
      has_exact_prison_jak_counts(capture, has_prison_jak_animator) &&
      capture.eye_markers == expected_eye_markers &&
      capture.other_transfers == expected_other && capture.malformed_transfers == 0;
  if (!exact_counts) {
    set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Counts);
    return std::nullopt;
  }
  if (!has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                          bucket_id, capture.transfers[0]) ||
      !has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                          bucket_id, capture.transfers[2])) {
    set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Opening);
    return std::nullopt;
  }

  const u8* descriptor_payload = nullptr;
  if (!get_plain_cnt_payload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                             bucket_id, capture.transfers[1], 1, kPcPortVif, 3,
                             &descriptor_payload)) {
    set_pris_eye_rejection(out_rejection,
                           Jak2PrisEyeTextureUploadRejectReason::Descriptor);
    return std::nullopt;
  }
  const u64 page_offset = read_unaligned<u64>(descriptor_payload);
  const s64 mode = read_unaligned<s64>(descriptor_payload + sizeof(u64));
  if (mode != -1) {
    set_pris_eye_rejection(out_rejection,
                           Jak2PrisEyeTextureUploadRejectReason::Descriptor);
    return std::nullopt;
  }
  if (!page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Page);
    return std::nullopt;
  }

  plan.has_prison_jak_animator = has_prison_jak_animator;
  u32 start_transfer_index = 3;
  if (has_prison_jak_animator) {
    if (!parse_pris_prison_jak_animator(
            dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id, capture,
            start_transfer_index, &plan.prison_jak_animator)) {
      set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Counts);
      return std::nullopt;
    }
    start_transfer_index = plan.prison_jak_animator.linker_transfer_index + 1;
  }
  plan.chunk_count = chunk_count;
  u32 common_source_fbp = 0;
  for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
    u32 source_fbp = 0;
    auto& chunk = plan.chunks[chunk_index];
    if (!parse_pris_eye_chunk(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                              bucket_id, capture, start_transfer_index,
                              static_cast<u8>(chunk_index), &chunk, &source_fbp,
                              out_rejection)) {
      return std::nullopt;
    }
    if (!has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                            bucket_id, capture.transfers[chunk.linker_transfer_index])) {
      set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Linker,
                             static_cast<u8>(chunk_index));
      return std::nullopt;
    }
    if (chunk_index != 0 && source_fbp != common_source_fbp) {
      set_pris_eye_rejection(
          out_rejection, Jak2PrisEyeTextureUploadRejectReason::SourceFramebuffer,
          static_cast<u8>(chunk_index));
      return std::nullopt;
    }
    if ((plan.eye_slot_mask & chunk.eye_slot_mask) != 0) {
      set_pris_eye_rejection(
          out_rejection, Jak2PrisEyeTextureUploadRejectReason::DuplicateEyeSlots,
          static_cast<u8>(chunk_index));
      return std::nullopt;
    }
    common_source_fbp = source_fbp;
    plan.eye_slot_mask |= chunk.eye_slot_mask;
    start_transfer_index = chunk.linker_transfer_index + 1;
  }

  plan.direct_reset_transfer_index = start_transfer_index;
  plan.terminal_transfer_index = start_transfer_index + 1;
  const u8* ignored_payload = nullptr;
  if (plan.terminal_transfer_index >= capture.transfer_count ||
      !get_plain_cnt_payload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                             bucket_id, capture.transfers[plan.direct_reset_transfer_index], 10,
                             kFlushaVif, kDirectVif | 10, &ignored_payload)) {
    set_pris_eye_rejection(out_rejection,
                           Jak2PrisEyeTextureUploadRejectReason::DefaultReset);
    return std::nullopt;
  }
  if (!has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                          bucket_id, capture.transfers[plan.terminal_transfer_index])) {
    set_pris_eye_rejection(out_rejection, Jak2PrisEyeTextureUploadRejectReason::Terminal);
    return std::nullopt;
  }
  plan.direct_reset_relative_tag_offset =
      capture.transfers[plan.direct_reset_transfer_index].relative_tag_offset;
  plan.terminal_relative_tag_offset =
      capture.transfers[plan.terminal_transfer_index].relative_tag_offset;

  plan.present = true;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());

  u64 fingerprint = kFnvOffsetBasis;
  hash_bytes(&fingerprint, &plan.bucket_id, sizeof(plan.bucket_id));
  hash_bytes(&fingerprint, &plan.ordinary.page_offset, sizeof(plan.ordinary.page_offset));
  hash_bytes(&fingerprint, &plan.ordinary.mode, sizeof(plan.ordinary.mode));
  hash_bytes(&fingerprint, plan.ordinary.page_header.data(),
             plan.ordinary.page_header.size());
  const u8 has_animator = plan.has_prison_jak_animator ? 1 : 0;
  hash_bytes(&fingerprint, &has_animator, sizeof(has_animator));
  if (plan.has_prison_jak_animator) {
    hash_bytes(&fingerprint, &plan.prison_jak_animator.semantic_fingerprint,
               sizeof(plan.prison_jak_animator.semantic_fingerprint));
  }
  hash_bytes(&fingerprint, &plan.chunk_count, sizeof(plan.chunk_count));
  for (std::size_t i = 0; i < plan.chunk_count; ++i) {
    const auto& chunk = plan.chunks[i];
    hash_bytes(&fingerprint, &chunk.resolution, sizeof(chunk.resolution));
    hash_bytes(&fingerprint, &chunk.pair_index, sizeof(chunk.pair_index));
    hash_bytes(&fingerprint, &chunk.semantic_fingerprint,
               sizeof(chunk.semantic_fingerprint));
  }
  plan.semantic_fingerprint = fingerprint;
  return plan;
}

bool jak2_pris_eye_texture_upload_plans_match(
    const Jak2PrisEyeTextureUploadPlan& live,
    const Jak2PrisEyeTextureUploadPlan& copied) {
  if (live.bucket_id != copied.bucket_id || live.present != copied.present ||
      live.chunk_count != copied.chunk_count || live.eye_slot_mask != copied.eye_slot_mask ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  if (!live.present) {
    return true;
  }
  if (live.ordinary.page_offset != copied.ordinary.page_offset ||
      live.ordinary.mode != copied.ordinary.mode ||
      live.ordinary.page_header != copied.ordinary.page_header ||
      live.direct_reset_transfer_index != copied.direct_reset_transfer_index ||
      live.terminal_transfer_index != copied.terminal_transfer_index) {
    return false;
  }
  if (live.has_prison_jak_animator != copied.has_prison_jak_animator) {
    return false;
  }
  if (live.has_prison_jak_animator) {
    const auto& a = live.prison_jak_animator;
    const auto& b = copied.prison_jak_animator;
    if (std::memcmp(&a.morph, &b.morph, sizeof(a.morph)) != 0 ||
        a.destination_tbps != b.destination_tbps || a.source_padding != b.source_padding ||
        a.start_transfer_index != b.start_transfer_index ||
        a.body_transfer_index != b.body_transfer_index ||
        a.finish_transfer_index != b.finish_transfer_index ||
        a.linker_transfer_index != b.linker_transfer_index ||
        a.semantic_fingerprint != b.semantic_fingerprint) {
      return false;
    }
  }
  for (std::size_t i = 0; i < live.chunk_count; ++i) {
    const auto& a = live.chunks[i];
    const auto& b = copied.chunks[i];
    if (a.resolution != b.resolution || a.pair_index != b.pair_index ||
        a.start_transfer_index != b.start_transfer_index ||
        a.linker_transfer_index != b.linker_transfer_index ||
        a.transfer_count != b.transfer_count || a.payload_bytes != b.payload_bytes ||
        a.eye_slot_mask != b.eye_slot_mask ||
        a.semantic_fingerprint != b.semantic_fingerprint) {
      return false;
    }
  }
  return true;
}

std::optional<Jak2WaterTextureUploadPlan> plan_jak2_water_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }

  if (!is_water_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2WaterTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool descriptor_only =
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryOnly &&
      capture.transfer_count == 3 && capture.total_payload_bytes == 16 &&
      capture.inert_transfers == 2 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 0 && capture.animator_arrays == 0 &&
      capture.eye_markers == 0 && capture.other_transfers == 0 &&
      capture.malformed_transfers == 0 && metadata_is_inert_next(capture.transfers[0]) &&
      metadata_is_ordinary_descriptor(capture.transfers[1]) &&
      metadata_is_inert_next(capture.transfers[2]);
  const bool descriptor_and_standard_reset =
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 176 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 0 &&
      capture.animator_arrays == 0 && capture.animator_body_transfers == 0 &&
      capture.animator_payload_bytes == 0 && capture.eye_markers == 0 &&
      capture.other_transfers == 0 && capture.malformed_transfers == 0 &&
      metadata_is_inert_next(capture.transfers[0]) &&
      metadata_is_ordinary_descriptor(capture.transfers[1]) &&
      metadata_is_inert_next(capture.transfers[2]) &&
      metadata_is_direct_setup(capture.transfers[3]) &&
      metadata_is_inert_next(capture.transfers[4]);
  const bool security_composite =
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator &&
      capture.transfer_count == 9 && capture.total_payload_bytes == 1008 &&
      capture.inert_transfers == 4 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 0 &&
      capture.animator_arrays == 1 && capture.animator_body_transfers == 1 &&
      capture.animator_payload_bytes == sizeof(Jak2Opcode30SecurityPlan) &&
      capture.eye_markers == 0 && capture.other_transfers == 0 &&
      capture.malformed_transfers == 0 && has_exact_security_counts(capture) &&
      metadata_is_inert_next(capture.transfers[0]) &&
      metadata_is_ordinary_descriptor(capture.transfers[1]) &&
      metadata_is_inert_next(capture.transfers[2]) &&
      metadata_is_animator_start(capture.transfers[3]) &&
      metadata_is_opcode30_security_body(capture.transfers[4]) &&
      metadata_is_animator_finish(capture.transfers[5]) &&
      metadata_is_inert_next(capture.transfers[6]) &&
      metadata_is_direct_setup(capture.transfers[7]) &&
      metadata_is_inert_next(capture.transfers[8]);
  if (!descriptor_only && !descriptor_and_standard_reset && !security_composite) {
    return std::nullopt;
  }

  const auto& descriptor = capture.transfers[1];
  const u64 descriptor_tag_offset = static_cast<u64>(chain_offset) + bucket_id * 16 +
                                    descriptor.relative_tag_offset;
  const u64 descriptor_data_offset = descriptor_tag_offset + 16;
  const std::size_t checked_snapshot_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  if (!range_is_valid(descriptor_data_offset, 16, checked_snapshot_size)) {
    return std::nullopt;
  }
  const u64 page_offset = read_unaligned<u64>(dma_packet_snapshot + descriptor_data_offset);
  const s64 mode =
      read_unaligned<s64>(dma_packet_snapshot + descriptor_data_offset + sizeof(u64));
  if (mode != -1 || !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return std::nullopt;
  }

  plan.present = true;
  plan.variant = descriptor_and_standard_reset
                     ? Jak2WaterTextureUploadVariant::DescriptorAndStandardReset
                     : Jak2WaterTextureUploadVariant::DescriptorOnly;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());
  if (security_composite) {
    plan.variant = Jak2WaterTextureUploadVariant::DescriptorSecurityAndStandardReset;
    u64 animator_data_offset = 0;
    if (!transfer_data_offset(chain_offset, bucket_id, capture.transfers[4],
                              checked_snapshot_size, &animator_data_offset)) {
      return std::nullopt;
    }
    constexpr u64 kExactOpcode30BodyTag =
        52ull | (static_cast<u64>(DmaTag::Kind::CNT) << 28);
    const u64 animator_tag_offset = animator_data_offset - 16;
    if (read_unaligned<u64>(dma_packet_snapshot + animator_tag_offset) !=
            kExactOpcode30BodyTag ||
        read_unaligned<u32>(dma_packet_snapshot + animator_tag_offset + 8) !=
            (kPcPortVif | kSecurityOpcode) ||
        read_unaligned<u32>(dma_packet_snapshot + animator_tag_offset + 12) != 0 ||
        !parse_opcode30_security(dma_packet_snapshot + animator_data_offset,
                                 &plan.security)) {
      return std::nullopt;
    }
    plan.has_security_animator = true;
  }
  return plan;
}

std::optional<Jak2CommonPrisTextureUploadPlan> plan_jak2_common_pris_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
      kJak2CommonPrisTextureUploadBucket);
  if (out_capture) {
    *out_capture = capture;
  }
  if (!capture.valid) {
    return std::nullopt;
  }

  Jak2CommonPrisTextureUploadPlan plan;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool one_eye = capture.transfer_count == 36;
  const bool two_eye = capture.transfer_count == 63;
  const bool eyes = one_eye || two_eye;
  const std::size_t chunk_count = one_eye ? 1 : two_eye ? 2 : 0;
  const bool exact_counts =
      (capture.transfer_count == 9 || eyes) &&
      capture.classification == (eyes ? Jak2CommonTfragTextureUploadClass::EyeOrOther
                                      : Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator) &&
      capture.total_payload_bytes == 208 + chunk_count * 1856 &&
      capture.inert_transfers == 4 + chunk_count &&
      capture.ordinary_descriptors == 1 && capture.direct_setup_transfers == 1 &&
      capture.gs_setup_transfers == chunk_count * 11 &&
      capture.animator_arrays == 1 && capture.animator_body_transfers == 1 &&
      capture.animator_payload_bytes == kJak2CommonPrisDarkJakAnimatorBodyBytes &&
      has_exact_dark_jak_counts(capture) && capture.eye_markers == chunk_count * 2 &&
      capture.other_transfers == chunk_count * 13 && capture.malformed_transfers == 0;
  if (!exact_counts || !has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size,
                                            chain_offset, kJak2CommonPrisTextureUploadBucket,
                                            capture.transfers[0]) ||
      !has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                          kJak2CommonPrisTextureUploadBucket, capture.transfers[2])) {
    return std::nullopt;
  }

  const u8* descriptor_payload = nullptr;
  if (!get_plain_cnt_payload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                             kJak2CommonPrisTextureUploadBucket, capture.transfers[1], 1,
                             kPcPortVif, 3, &descriptor_payload)) {
    return std::nullopt;
  }
  const u64 page_offset = read_unaligned<u64>(descriptor_payload);
  const s64 mode = read_unaligned<s64>(descriptor_payload + sizeof(u64));
  if (mode != -1 || !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return std::nullopt;
  }

  if (!parse_common_pris_dark_jak_animator(
          dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, capture, 3,
          &plan.dark_jak_animator)) {
    return std::nullopt;
  }

  plan.chunk_count = chunk_count;
  u32 start_transfer_index = plan.dark_jak_animator.linker_transfer_index + 1;
  u32 common_source_fbp = 0;
  for (std::size_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
    u32 source_fbp = 0;
    auto& chunk = plan.chunks[chunk_index];
    if (!parse_pris_eye_chunk(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                              kJak2CommonPrisTextureUploadBucket, capture,
                              start_transfer_index, static_cast<u8>(chunk_index), &chunk,
                              &source_fbp, nullptr) ||
        !has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                            kJak2CommonPrisTextureUploadBucket,
                            capture.transfers[chunk.linker_transfer_index]) ||
        (chunk_index != 0 && source_fbp != common_source_fbp) ||
        (plan.eye_slot_mask & chunk.eye_slot_mask) != 0) {
      return std::nullopt;
    }
    common_source_fbp = source_fbp;
    plan.eye_slot_mask |= chunk.eye_slot_mask;
    start_transfer_index = chunk.linker_transfer_index + 1;
  }

  plan.direct_reset_transfer_index = start_transfer_index;
  plan.terminal_transfer_index = start_transfer_index + 1;
  const u8* ignored_payload = nullptr;
  if (plan.terminal_transfer_index >= capture.transfer_count ||
      !get_plain_cnt_payload(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                             kJak2CommonPrisTextureUploadBucket,
                             capture.transfers[plan.direct_reset_transfer_index], 10,
                             kFlushaVif, kDirectVif | 10, &ignored_payload) ||
      !has_plain_next_tag(dma_packet_snapshot, dma_packet_snapshot_size, chain_offset,
                          kJak2CommonPrisTextureUploadBucket,
                          capture.transfers[plan.terminal_transfer_index])) {
    return std::nullopt;
  }
  plan.direct_reset_relative_tag_offset =
      capture.transfers[plan.direct_reset_transfer_index].relative_tag_offset;
  u64 reset_fingerprint = kFnvOffsetBasis;
  hash_bytes(&reset_fingerprint, ignored_payload, 160);
  plan.direct_reset_semantic_fingerprint = reset_fingerprint;
  plan.terminal_relative_tag_offset =
      capture.transfers[plan.terminal_transfer_index].relative_tag_offset;

  plan.present = true;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());

  u64 fingerprint = kFnvOffsetBasis;
  hash_bytes(&fingerprint, &plan.bucket_id, sizeof(plan.bucket_id));
  hash_bytes(&fingerprint, &plan.ordinary.page_offset, sizeof(plan.ordinary.page_offset));
  hash_bytes(&fingerprint, &plan.ordinary.mode, sizeof(plan.ordinary.mode));
  hash_bytes(&fingerprint, plan.ordinary.page_header.data(), plan.ordinary.page_header.size());
  hash_bytes(&fingerprint, &plan.dark_jak_animator.semantic_fingerprint,
             sizeof(plan.dark_jak_animator.semantic_fingerprint));
  hash_bytes(&fingerprint, &plan.chunk_count, sizeof(plan.chunk_count));
  for (std::size_t i = 0; i < plan.chunk_count; ++i) {
    const auto& chunk = plan.chunks[i];
    hash_bytes(&fingerprint, &chunk.resolution, sizeof(chunk.resolution));
    hash_bytes(&fingerprint, &chunk.pair_index, sizeof(chunk.pair_index));
    hash_bytes(&fingerprint, &chunk.semantic_fingerprint, sizeof(chunk.semantic_fingerprint));
  }
  hash_bytes(&fingerprint, &plan.direct_reset_semantic_fingerprint,
             sizeof(plan.direct_reset_semantic_fingerprint));
  plan.semantic_fingerprint = fingerprint;
  return plan;
}

bool jak2_common_pris_texture_upload_plans_match(
    const Jak2CommonPrisTextureUploadPlan& live,
    const Jak2CommonPrisTextureUploadPlan& copied) {
  if (live.bucket_id != copied.bucket_id || live.present != copied.present ||
      live.chunk_count != copied.chunk_count || live.eye_slot_mask != copied.eye_slot_mask ||
      live.semantic_fingerprint != copied.semantic_fingerprint) {
    return false;
  }
  if (!live.present) {
    return true;
  }
  if (live.ordinary.page_offset != copied.ordinary.page_offset ||
      live.ordinary.mode != copied.ordinary.mode ||
      live.ordinary.page_header != copied.ordinary.page_header ||
      live.direct_reset_transfer_index != copied.direct_reset_transfer_index ||
      live.direct_reset_semantic_fingerprint != copied.direct_reset_semantic_fingerprint ||
      live.terminal_transfer_index != copied.terminal_transfer_index) {
    return false;
  }
  const auto& a = live.dark_jak_animator;
  const auto& b = copied.dark_jak_animator;
  if (std::memcmp(&a.morph, &b.morph, sizeof(a.morph)) != 0 ||
      a.source_padding != b.source_padding || a.destination_tbps != b.destination_tbps ||
      a.start_transfer_index != b.start_transfer_index ||
      a.body_transfer_index != b.body_transfer_index ||
      a.finish_transfer_index != b.finish_transfer_index ||
      a.linker_transfer_index != b.linker_transfer_index ||
      a.semantic_fingerprint != b.semantic_fingerprint) {
    return false;
  }
  for (std::size_t i = 0; i < live.chunk_count; ++i) {
    const auto& live_chunk = live.chunks[i];
    const auto& copied_chunk = copied.chunks[i];
    if (live_chunk.resolution != copied_chunk.resolution ||
        live_chunk.pair_index != copied_chunk.pair_index ||
        live_chunk.start_transfer_index != copied_chunk.start_transfer_index ||
        live_chunk.linker_transfer_index != copied_chunk.linker_transfer_index ||
        live_chunk.transfer_count != copied_chunk.transfer_count ||
        live_chunk.payload_bytes != copied_chunk.payload_bytes ||
        live_chunk.eye_slot_mask != copied_chunk.eye_slot_mask ||
        live_chunk.semantic_fingerprint != copied_chunk.semantic_fingerprint) {
      return false;
    }
  }
  return true;
}

std::optional<Jak2NormalShrubTextureUploadPlan> plan_jak2_normal_shrub_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }

  if (!is_normal_shrub_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2NormalShrubTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool exact_counts =
      capture.classification == Jak2CommonTfragTextureUploadClass::GsSetupOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 192 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 0 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 1 &&
      capture.animator_arrays == 0 && capture.eye_markers == 0 &&
      capture.other_transfers == 0 && capture.malformed_transfers == 0;
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_gs_setup(capture.transfers[1]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_direct_setup(capture.transfers[3]) ||
      !metadata_is_inert_next(capture.transfers[4])) {
    return std::nullopt;
  }

  plan.present = true;
  return plan;
}

std::optional<Jak2AlphaTextureUploadPlan> plan_jak2_alpha_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    u32 bucket_id,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset, bucket_id);
  if (out_capture) {
    *out_capture = capture;
  }

  if (!is_alpha_texture_upload_bucket(bucket_id) || !capture.valid) {
    return std::nullopt;
  }

  Jak2AlphaTextureUploadPlan plan;
  plan.bucket_id = bucket_id;
  if (!capture.present) {
    if (capture.classification != Jak2CommonTfragTextureUploadClass::Absent ||
        capture.transfer_count != 1 || capture.total_payload_bytes != 0 ||
        capture.inert_transfers != 1 || !metadata_is_strict_empty(capture.transfers[0])) {
      return std::nullopt;
    }
    return plan;
  }

  const bool exact_counts =
      capture.classification == Jak2CommonTfragTextureUploadClass::GsSetupOnly &&
      capture.transfer_count == 5 && capture.total_payload_bytes == 192 &&
      capture.inert_transfers == 3 && capture.ordinary_descriptors == 0 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 1 &&
      capture.animator_arrays == 0 && capture.eye_markers == 0 &&
      capture.other_transfers == 0 && capture.malformed_transfers == 0;
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_gs_setup(capture.transfers[1]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_direct_setup(capture.transfers[3]) ||
      !metadata_is_inert_next(capture.transfers[4])) {
    return std::nullopt;
  }

  plan.present = true;
  return plan;
}

std::optional<Jak2CommonTfragTextureUploadPlan> plan_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size,
    Jak2CommonTfragTextureUploadCapture* out_capture) {
  const auto capture = capture_jak2_common_tfrag_texture_upload(
      dma_packet_snapshot, dma_packet_snapshot_size, chain_offset);
  if (out_capture) {
    *out_capture = capture;
  }

  if (capture.valid && !capture.present &&
      capture.classification == Jak2CommonTfragTextureUploadClass::Absent &&
      capture.transfer_count == 1 && capture.total_payload_bytes == 0 &&
      capture.inert_transfers == 1 && metadata_is_strict_empty(capture.transfers[0])) {
    return Jak2CommonTfragTextureUploadPlan{};
  }

  const bool exact_counts =
      capture.valid && capture.present &&
      capture.classification == Jak2CommonTfragTextureUploadClass::OrdinaryAndAnimator &&
      capture.transfer_count == 9 && capture.total_payload_bytes == 672 &&
      capture.inert_transfers == 4 && capture.ordinary_descriptors == 1 &&
      capture.direct_setup_transfers == 1 && capture.gs_setup_transfers == 0 &&
      capture.animator_arrays == 1 && capture.animator_body_transfers == 1 &&
      capture.animator_payload_bytes == sizeof(Jak2Opcode27SkullGemPlan) &&
      capture.eye_markers == 0 && capture.other_transfers == 0 &&
      capture.malformed_transfers == 0 && has_exact_opcode27_counts(capture);
  if (!exact_counts || !metadata_is_inert_next(capture.transfers[0]) ||
      !metadata_is_ordinary_descriptor(capture.transfers[1]) ||
      !metadata_is_inert_next(capture.transfers[2]) ||
      !metadata_is_animator_start(capture.transfers[3]) ||
      !metadata_is_opcode27_body(capture.transfers[4]) ||
      !metadata_is_animator_finish(capture.transfers[5]) ||
      !metadata_is_inert_next(capture.transfers[6]) ||
      !metadata_is_direct_setup(capture.transfers[7]) ||
      !metadata_is_inert_next(capture.transfers[8])) {
    return std::nullopt;
  }

  const std::size_t checked_snapshot_size =
      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE);
  u64 descriptor_data_offset = 0;
  u64 animator_data_offset = 0;
  if (!transfer_data_offset(chain_offset, kJak2CommonTfragTextureUploadBucket,
                            capture.transfers[1], checked_snapshot_size,
                            &descriptor_data_offset) ||
      !transfer_data_offset(chain_offset, kJak2CommonTfragTextureUploadBucket,
                            capture.transfers[4], checked_snapshot_size,
                            &animator_data_offset)) {
    return std::nullopt;
  }

  constexpr u64 kExactOpcode27BodyTag =
      31ull | (static_cast<u64>(DmaTag::Kind::CNT) << 28);
  const u64 animator_tag_offset = animator_data_offset - 16;
  if (read_unaligned<u64>(dma_packet_snapshot + animator_tag_offset) !=
          kExactOpcode27BodyTag ||
      read_unaligned<u32>(dma_packet_snapshot + animator_tag_offset + 8) !=
          (kPcPortVif | kSkullGemOpcode) ||
      read_unaligned<u32>(dma_packet_snapshot + animator_tag_offset + 12) != 0) {
    return std::nullopt;
  }

  const u64 page_offset = read_unaligned<u64>(dma_packet_snapshot + descriptor_data_offset);
  const s64 mode = read_unaligned<s64>(dma_packet_snapshot + descriptor_data_offset + 8);
  if (mode != -1 || !page_header_is_valid(live_ee_memory, live_ee_memory_size, page_offset)) {
    return std::nullopt;
  }

  Jak2CommonTfragTextureUploadPlan plan;
  plan.present = true;
  plan.ordinary.page_offset = page_offset;
  plan.ordinary.mode = mode;
  std::memcpy(plan.ordinary.page_header.data(), live_ee_memory + page_offset,
              plan.ordinary.page_header.size());
  if (!parse_opcode27_skull_gem(dma_packet_snapshot + animator_data_offset,
                                &plan.skull_gem)) {
    return std::nullopt;
  }
  return plan;
}

Jak2CommonTfragTextureUploadCapture capture_jak2_common_tfrag_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset) {
  return capture_jak2_tfrag_texture_upload(dma_packet_snapshot, dma_packet_snapshot_size,
                                           chain_offset, kJak2CommonTfragTextureUploadBucket);
}

}  // namespace metal_renderer
