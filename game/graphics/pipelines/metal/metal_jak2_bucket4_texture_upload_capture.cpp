#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_capture.h"
#include "game/graphics/pipelines/metal/metal_jak2_bucket4_texture_upload_plan.h"

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

constexpr u16 kStartArray = 12;
constexpr u16 kFinishArray = 13;
constexpr u16 kEraseDestination = 14;
constexpr u16 kUploadClut = 15;
constexpr u16 kGenericUpload = 16;
constexpr u16 kCloudsAndFog = 41;
constexpr u32 kMaximumTransfers = 64;
constexpr u32 kGsMemoryUpperBound = 16 * 1024;
constexpr u32 kPcPortVif = static_cast<u32>(VifCode::Kind::PC_PORT) << 24;

struct TextureAnimUpload {
  u32 data;
  u16 width;
  u16 height;
  u32 destination;
  u8 format;
  u8 force_to_gpu;
  u8 pad[2];
};
static_assert(sizeof(TextureAnimUpload) == 16);

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

struct SkyInputLayout {
  float fog_height;
  float cloud_min;
  float cloud_max;
  float times[11];
  float max_times[6];
  float scales[6];
  s32 cloud_destination;
};
static_assert(offsetof(SkyInputLayout, cloud_destination) == 104);
static_assert(sizeof(SkyInputLayout) == kJak2Bucket4SkyInputBytes);

struct ParsedPlanFields {
  SkyInputLayout sky_input = {};
  std::array<u8, sizeof(SkyInputLayout)> sky_input_bytes = {};
  std::array<u64, 9> erase_setup_values = {};
};

struct CheckedTransfer {
  DmaTag tag{0};
  u32 tag_offset = 0;
  const u8* data = nullptr;
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
    if (!out || !m_memory || !range_is_valid(m_offset, 16, m_memory_size) ||
        (m_offset & 15) != 0 || already_visited(m_offset)) {
      return false;
    }
    m_visited[m_visited_count++] = m_offset;

    const u64 raw_tag = read_unaligned<u64>(m_memory + m_offset);
    const DmaTag tag(raw_tag);
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
        data_offset = static_cast<u64>(m_offset) + 16;
        next_offset = tag.addr;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        data_offset = tag.addr;
        next_offset = static_cast<u64>(m_offset) + 16;
        break;
      default:
        return false;
    }
    if (!range_is_valid(data_offset, payload_bytes, m_memory_size) ||
        next_offset > std::numeric_limits<u32>::max() || (next_offset & 15) != 0 ||
        !range_is_valid(next_offset, 0, m_memory_size)) {
      return false;
    }

    out->tag = tag;
    out->tag_offset = m_offset;
    out->data = m_memory + data_offset;
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
  std::array<u32, kMaximumTransfers> m_visited = {};
  u32 m_visited_count = 0;
};

enum class ParseState {
  Texflush,
  OrdinaryDescriptor,
  FirstArrayStart,
  Cloud,
  FirstArrayFinish,
  SecondArrayStart,
  Erase,
  EraseSetup,
  EraseClear,
  GenericUpload,
  ClutUpload,
  SecondArrayFinish,
  Complete,
};

bool is_inert(const CheckedTransfer& transfer) {
  return transfer.size_bytes == 0 && transfer.vif0 == 0 && transfer.vif1 == 0;
}

bool is_pc_port(const CheckedTransfer& transfer, u16 opcode, u32 expected_bytes) {
  return transfer.tag.kind == DmaTag::Kind::CNT && transfer.vif0 == (kPcPortVif | opcode) &&
         transfer.size_bytes == expected_bytes;
}

bool is_direct(const CheckedTransfer& transfer,
               VifCode::Kind expected_vif0,
               u16 expected_qwc) {
  const u32 expected_vif0_value = static_cast<u32>(expected_vif0) << 24;
  const u32 expected_vif1_value =
      (static_cast<u32>(VifCode::Kind::DIRECT) << 24) | expected_qwc;
  return transfer.tag.kind == DmaTag::Kind::CNT &&
         transfer.size_bytes == static_cast<u32>(expected_qwc) * 16 &&
         transfer.vif0 == expected_vif0_value && transfer.vif1 == expected_vif1_value;
}

void record_malformed(Jak2Bucket4TextureUploadCapture* out, const CheckedTransfer* transfer) {
  out->malformed_transfers++;
  if (transfer) {
    out->malformed_bytes += transfer->size_bytes;
  }
}

void record_unsupported(Jak2Bucket4TextureUploadCapture* out,
                        const CheckedTransfer& transfer) {
  out->unsupported_transfers++;
  out->unsupported_bytes += transfer.size_bytes;
}

bool texture_source_range_is_valid(const u8* live_ee_memory,
                                   u32 source,
                                   u16 width,
                                   u16 height,
                                   u8 format,
                                   std::size_t live_ee_memory_size) {
  const u64 pixels = static_cast<u64>(width) * height;
  u64 bytes = 0;
  switch (format) {
    case 0:   // PSMCT32
      bytes = pixels * 4;
      break;
    case 19:  // PSMT8
    case 20:  // PSMT4, matching TextureAnimator's current CPU staging size
      bytes = pixels;
      break;
    default:
      return false;
  }
  const std::size_t checked_size =
      std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  return live_ee_memory && width != 0 && height != 0 && source != 0 &&
         range_is_valid(source, bytes, checked_size);
}

bool ordinary_page_is_valid(const u8* live_ee_memory,
                            u64 page,
                            std::size_t live_ee_memory_size) {
  const std::size_t checked_size =
      std::min<std::size_t>(live_ee_memory_size, EE_MAIN_MEM_SIZE);
  return live_ee_memory && page != 0 &&
         range_is_valid(page, sizeof(GoalTexturePageHeaderLayout), checked_size);
}

bool cloud_input_is_valid(const SkyInputLayout& input) {
  if (!std::isfinite(input.cloud_min) || !std::isfinite(input.cloud_max)) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(input.times[i + 1]) || !std::isfinite(input.max_times[i]) ||
        input.max_times[i] <= 0.f || !std::isfinite(input.scales[i])) {
      return false;
    }
  }
  return true;
}

bool consume_semantic_transfer(const CheckedTransfer& transfer,
                               const u8* live_ee_memory,
                               std::size_t live_ee_memory_size,
                               ParseState* state,
                               Jak2Bucket4TextureUploadCapture* out,
                               ParsedPlanFields* plan_fields) {
  const VifCode vif0(transfer.vif0);
  const bool inside_animator = *state >= ParseState::Cloud && *state <= ParseState::SecondArrayFinish;
  if (inside_animator) {
    out->animator_bytes += transfer.size_bytes;
  }
  if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate < out->opcode_counts.size() &&
      *state >= ParseState::FirstArrayStart) {
    out->opcode_counts[vif0.immediate]++;
  }

  switch (*state) {
    case ParseState::Texflush: {
      if (!is_direct(transfer, VifCode::Kind::NOP, 2)) {
        record_malformed(out, &transfer);
        return false;
      }
      const GifTag gif(transfer.data);
      const bool valid_gif = gif.nloop() == 1 && gif.eop() && !gif.pre() &&
                             gif.flg() == GifTag::Format::PACKED && gif.nreg() == 1 &&
                             gif.reg(0) == GifTag::RegisterDescriptor::AD;
      const u64 register_data = read_unaligned<u64>(transfer.data + 16);
      const u64 register_address = read_unaligned<u64>(transfer.data + 24);
      if (!valid_gif || register_data != 1 ||
          register_address != static_cast<u64>(GsRegisterAddress::TEXFLUSH)) {
        record_malformed(out, &transfer);
        return false;
      }
      *state = ParseState::OrdinaryDescriptor;
      return true;
    }
    case ParseState::OrdinaryDescriptor: {
      if (!is_pc_port(transfer, 0, 16) || transfer.vif1 != 3) {
        record_malformed(out, &transfer);
        return false;
      }
      out->ordinary_page = read_unaligned<u64>(transfer.data);
      out->ordinary_mode = read_unaligned<s64>(transfer.data + 8);
      out->ordinary_descriptors++;
      if (!ordinary_page_is_valid(live_ee_memory, out->ordinary_page,
                                  live_ee_memory_size)) {
        record_malformed(out, &transfer);
        return false;
      }
      *state = ParseState::FirstArrayStart;
      return true;
    }
    case ParseState::FirstArrayStart:
    case ParseState::SecondArrayStart:
      if (!is_pc_port(transfer, kStartArray, 0) || transfer.vif1 != 0) {
        record_unsupported(out, transfer);
        return false;
      }
      out->animator_arrays++;
      *state = *state == ParseState::FirstArrayStart ? ParseState::Cloud : ParseState::Erase;
      return true;
    case ParseState::Cloud: {
      if (!is_pc_port(transfer, kCloudsAndFog, 112)) {
        record_unsupported(out, transfer);
        return false;
      }
      out->cloud_destination =
          read_unaligned<s32>(transfer.data + offsetof(SkyInputLayout, cloud_destination));
      const auto input = read_unaligned<SkyInputLayout>(transfer.data);
      if (transfer.vif1 != 0 || !cloud_input_is_valid(input) || out->cloud_destination < 0 ||
          static_cast<u32>(out->cloud_destination) >= kGsMemoryUpperBound) {
        record_malformed(out, &transfer);
        return false;
      }
      plan_fields->sky_input = input;
      std::memcpy(plan_fields->sky_input_bytes.data(), transfer.data,
                  plan_fields->sky_input_bytes.size());
      *state = ParseState::FirstArrayFinish;
      return true;
    }
    case ParseState::FirstArrayFinish:
    case ParseState::SecondArrayFinish: {
      if (!is_pc_port(transfer, kFinishArray, 0)) {
        record_unsupported(out, transfer);
        return false;
      }
      const bool expected_vif1 =
          *state == ParseState::FirstArrayFinish
              ? transfer.vif1 == 0
              : transfer.vif1 == kPcPortVif;
      if (!expected_vif1) {
        record_malformed(out, &transfer);
        return false;
      }
      out->finishes++;
      *state = *state == ParseState::FirstArrayFinish ? ParseState::SecondArrayStart
                                                      : ParseState::Complete;
      return true;
    }
    case ParseState::Erase:
      if (!is_pc_port(transfer, kEraseDestination, 0) || transfer.vif1 != 0) {
        record_unsupported(out, transfer);
        return false;
      }
      *state = ParseState::EraseSetup;
      return true;
    case ParseState::EraseSetup: {
      if (!is_direct(transfer, VifCode::Kind::FLUSHA, 10)) {
        record_malformed(out, &transfer);
        return false;
      }
      const u8* ad = transfer.data + 16;
      const GifTag gif(transfer.data);
      constexpr std::array<GsRegisterAddress, 9> kExpectedRegisters = {
          GsRegisterAddress::SCISSOR_1, GsRegisterAddress::XYOFFSET_1,
          GsRegisterAddress::FRAME_1,   GsRegisterAddress::TEST_1,
          GsRegisterAddress::ALPHA_1,   GsRegisterAddress::CLAMP_1,
          GsRegisterAddress::TEXA,      GsRegisterAddress::ZBUF_1,
          GsRegisterAddress::TEXFLUSH,
      };
      bool registers_match = gif.nloop() == 1 && gif.eop() && !gif.pre() &&
                             gif.flg() == GifTag::Format::PACKED && gif.nreg() == 9;
      for (u32 i = 0; i < kExpectedRegisters.size(); ++i) {
        plan_fields->erase_setup_values[i] = read_unaligned<u64>(ad + i * 16);
        registers_match = registers_match && gif.reg(i) == GifTag::RegisterDescriptor::AD &&
                          read_unaligned<u64>(ad + i * 16 + 8) ==
                              static_cast<u64>(kExpectedRegisters[i]);
      }
      if (!registers_match) {
        record_malformed(out, &transfer);
        return false;
      }
      const GsScissor scissor(read_unaligned<u64>(ad));
      const GsFrame frame(read_unaligned<u64>(ad + 2 * 16));
      out->erase_test = read_unaligned<u64>(ad + 3 * 16);
      out->erase_alpha = read_unaligned<u64>(ad + 4 * 16);
      out->erase_clamp = read_unaligned<u64>(ad + 5 * 16);
      out->erase_width = scissor.x1() + 1;
      out->erase_height = scissor.y1() + 1;
      out->erase_destination = 32 * frame.fbp();
      constexpr u64 kExpectedXyOffset = 0x8000ull | (0x8000ull << 32);
      constexpr u64 kExpectedTexa = 0x80ull | (0x80ull << 32);
      constexpr u64 kExpectedZbuf = 0x130ull | (1ull << 24) | (1ull << 32);
      if (scissor.x0() != 0 || scissor.y0() != 0 || out->erase_width != 16 ||
          out->erase_height != 16 || frame.psm() != GsFrame::PSM::PSMCT32 ||
          frame.fbw() != (out->erase_width + 63) / 64 || frame.fbmsk() != 0 ||
          (out->erase_clamp & ~0x5ull) != 0 ||
          read_unaligned<u64>(ad + 1 * 16) != kExpectedXyOffset ||
          read_unaligned<u64>(ad + 6 * 16) != kExpectedTexa ||
          read_unaligned<u64>(ad + 7 * 16) != kExpectedZbuf ||
          read_unaligned<u64>(ad + 8 * 16) != 0 ||
          out->erase_destination >= kGsMemoryUpperBound) {
        record_malformed(out, &transfer);
        return false;
      }
      *state = ParseState::EraseClear;
      return true;
    }
    case ParseState::EraseClear:
      if (!is_direct(transfer, VifCode::Kind::NOP, 4)) {
        record_malformed(out, &transfer);
        return false;
      }
      {
        const GifTag gif(transfer.data);
        const u32 x0 = read_unaligned<u32>(transfer.data + 32);
        const u32 y0 = read_unaligned<u32>(transfer.data + 36);
        const u32 z0 = read_unaligned<u32>(transfer.data + 40);
        const u32 w0 = read_unaligned<u32>(transfer.data + 44);
        const u32 x1 = read_unaligned<u32>(transfer.data + 48);
        const u32 y1 = read_unaligned<u32>(transfer.data + 52);
        const u32 z1 = read_unaligned<u32>(transfer.data + 56);
        const u32 w1 = read_unaligned<u32>(transfer.data + 60);
        const bool valid_sprite =
            gif.nloop() == 1 && gif.eop() && gif.pre() &&
            gif.flg() == GifTag::Format::PACKED && gif.nreg() == 3 &&
            gif.prim() == static_cast<u32>(GsPrim::Kind::SPRITE) &&
            gif.reg(0) == GifTag::RegisterDescriptor::RGBAQ &&
            gif.reg(1) == GifTag::RegisterDescriptor::XYZ2 &&
            gif.reg(2) == GifTag::RegisterDescriptor::XYZ2 && x0 == 2048 * 16 &&
            y0 == 2048 * 16 && x1 == x0 + out->erase_width * 16 &&
            y1 == y0 + out->erase_height * 16 && z0 == 0x00ffffff &&
            z1 == 0x00ffffff && w0 == 0 && w1 == 0;
        if (!valid_sprite) {
          record_malformed(out, &transfer);
          return false;
        }
      }
      std::memcpy(out->erase_clear.data(), transfer.data + 16,
                  out->erase_clear.size() * sizeof(u32));
      *state = ParseState::GenericUpload;
      return true;
    case ParseState::GenericUpload: {
      if (!is_pc_port(transfer, kGenericUpload, sizeof(TextureAnimUpload)) || transfer.vif1 != 0) {
        record_unsupported(out, transfer);
        return false;
      }
      const auto upload = read_unaligned<TextureAnimUpload>(transfer.data);
      out->generic_source = upload.data;
      out->generic_width = upload.width;
      out->generic_height = upload.height;
      out->generic_destination = upload.destination;
      out->generic_format = upload.format;
      out->generic_force_to_gpu = upload.force_to_gpu;
      if (upload.width != 256 || upload.height != 1 || upload.format != 19 ||
          upload.force_to_gpu != 1 || upload.destination >= kGsMemoryUpperBound ||
          !texture_source_range_is_valid(live_ee_memory, upload.data, upload.width, upload.height,
                                         upload.format, live_ee_memory_size)) {
        record_malformed(out, &transfer);
        return false;
      }
      *state = ParseState::ClutUpload;
      return true;
    }
    case ParseState::ClutUpload: {
      if (!is_pc_port(transfer, kUploadClut, sizeof(TextureAnimUpload)) || transfer.vif1 != 0) {
        record_unsupported(out, transfer);
        return false;
      }
      const auto upload = read_unaligned<TextureAnimUpload>(transfer.data);
      out->clut_source = upload.data;
      out->clut_destination = upload.destination;
      if (upload.width != 16 || upload.height != 16 || upload.format != 0 ||
          upload.destination != out->erase_destination ||
          upload.destination >= kGsMemoryUpperBound ||
          !texture_source_range_is_valid(live_ee_memory, upload.data, 16, 16, 0,
                                         live_ee_memory_size)) {
        record_malformed(out, &transfer);
        return false;
      }
      *state = ParseState::SecondArrayFinish;
      return true;
    }
    case ParseState::Complete:
      record_unsupported(out, transfer);
      return false;
  }
  return false;
}

Jak2Bucket4OrdinaryUploadPlan make_ordinary_plan(
    const Jak2Bucket4TextureUploadCapture& capture,
    const u8* live_ee_memory) {
  Jak2Bucket4OrdinaryUploadPlan ordinary;
  ordinary.page_offset = capture.ordinary_page;
  ordinary.mode = capture.ordinary_mode;
  std::memcpy(ordinary.page_header.data(),
              live_ee_memory + static_cast<std::size_t>(capture.ordinary_page),
              ordinary.page_header.size());
  return ordinary;
}

Jak2Bucket4SkyInputPlan make_sky_plan(const ParsedPlanFields& fields) {
  Jak2Bucket4SkyInputPlan sky;
  sky.bytes = fields.sky_input_bytes;
  sky.fog_height = fields.sky_input.fog_height;
  sky.cloud_min = fields.sky_input.cloud_min;
  sky.cloud_max = fields.sky_input.cloud_max;
  for (std::size_t i = 0; i < sky.times.size(); ++i) {
    sky.times[i] = fields.sky_input.times[i];
  }
  for (std::size_t i = 0; i < sky.max_times.size(); ++i) {
    sky.max_times[i] = fields.sky_input.max_times[i];
    sky.scales[i] = fields.sky_input.scales[i];
  }
  sky.cloud_destination = fields.sky_input.cloud_destination;
  return sky;
}

Jak2Bucket4ErasePlan make_erase_plan(const Jak2Bucket4TextureUploadCapture& capture,
                                     const ParsedPlanFields& fields) {
  Jak2Bucket4ErasePlan erase;
  erase.setup_values = fields.erase_setup_values;
  erase.width = capture.erase_width;
  erase.height = capture.erase_height;
  erase.destination = capture.erase_destination;
  erase.test = capture.erase_test;
  erase.alpha = capture.erase_alpha;
  erase.clamp = capture.erase_clamp;
  erase.clear = capture.erase_clear;
  return erase;
}

Jak2Bucket4FogUploadPlan make_fog_plan(const Jak2Bucket4TextureUploadCapture& capture,
                                       const u8* live_ee_memory) {
  Jak2Bucket4FogUploadPlan fog;
  fog.width = capture.generic_width;
  fog.height = capture.generic_height;
  fog.destination = capture.generic_destination;
  fog.format = capture.generic_format;
  fog.force_to_gpu = capture.generic_force_to_gpu;
  fog.clut_destination = capture.clut_destination;
  std::memcpy(fog.indices.data(), live_ee_memory + capture.generic_source, fog.indices.size());
  std::memcpy(fog.clut.data(), live_ee_memory + capture.clut_source, fog.clut.size());
  return fog;
}

enum class ExactShape { Absent, OrdinaryOnly, Mixed };

Jak2Bucket4TextureUploadPlan make_execution_plan(
    ExactShape shape,
    const Jak2Bucket4TextureUploadCapture& capture,
    const ParsedPlanFields& fields,
    const u8* live_ee_memory) {
  switch (shape) {
    case ExactShape::Absent:
      return Jak2Bucket4AbsentPlan{};
    case ExactShape::OrdinaryOnly:
      return Jak2Bucket4OrdinaryOnlyPlan{make_ordinary_plan(capture, live_ee_memory)};
    case ExactShape::Mixed:
      return Jak2Bucket4MixedPlan{make_ordinary_plan(capture, live_ee_memory),
                                  make_sky_plan(fields), make_erase_plan(capture, fields),
                                  make_fog_plan(capture, live_ee_memory)};
  }
}

struct ParseResult {
  Jak2Bucket4TextureUploadCapture capture;
  std::optional<Jak2Bucket4TextureUploadPlan> plan;
};

ParseResult parse_jak2_bucket4_texture_upload(const u8* dma_packet_snapshot,
                                              std::size_t dma_packet_snapshot_size,
                                              u32 chain_offset,
                                              const u8* live_ee_memory,
                                              std::size_t live_ee_memory_size,
                                              bool materialize_plan) {
  ParseResult result;
  auto& out = result.capture;
  ParsedPlanFields plan_fields;
  const u64 bucket_offset64 = static_cast<u64>(chain_offset) + kJak2TextureUploadBucket * 16;
  const u64 end_offset64 = bucket_offset64 + 16;
  if (!dma_packet_snapshot || bucket_offset64 > std::numeric_limits<u32>::max() ||
      end_offset64 > std::numeric_limits<u32>::max() ||
      !range_is_valid(bucket_offset64, 16,
                      std::min<std::size_t>(dma_packet_snapshot_size, EE_MAIN_MEM_SIZE))) {
    record_malformed(&out, nullptr);
    return result;
  }

  const u32 bucket_offset = static_cast<u32>(bucket_offset64);
  const u32 end_offset = static_cast<u32>(end_offset64);
  CheckedDmaFollower dma(dma_packet_snapshot, dma_packet_snapshot_size, bucket_offset);
  ParseState state = ParseState::Texflush;
  bool saw_non_inert = false;
  while (dma.offset() != end_offset) {
    if (out.dma_transfers == kMaximumTransfers) {
      record_malformed(&out, nullptr);
      return result;
    }
    CheckedTransfer transfer;
    if (!dma.read(&transfer)) {
      record_malformed(&out, nullptr);
      return result;
    }
    out.dma_transfers++;
    out.total_payload_bytes += transfer.size_bytes;
    if (transfer.size_bytes) {
      out.payload_transfers++;
    }
    if (is_inert(transfer)) {
      if (transfer.tag.kind != DmaTag::Kind::CNT && transfer.tag.kind != DmaTag::Kind::NEXT) {
        record_malformed(&out, &transfer);
        return result;
      }
      out.inert_transfers++;
      out.inert_cnt_transfers += transfer.tag.kind == DmaTag::Kind::CNT;
      out.inert_next_transfers += transfer.tag.kind == DmaTag::Kind::NEXT;
      out.inert_state_mask |= 1u << static_cast<u32>(state);
      continue;
    }
    saw_non_inert = true;
    out.present = true;
    if (!consume_semantic_transfer(transfer, live_ee_memory, live_ee_memory_size, &state, &out,
                                   &plan_fields)) {
      return result;
    }
  }

  if (!saw_non_inert) {
    out.valid = true;
    if (materialize_plan) {
      result.plan = make_execution_plan(ExactShape::Absent, out, plan_fields, live_ee_memory);
    }
    return result;
  }
  constexpr u32 kMixedInertStateMask =
      (1u << static_cast<u32>(ParseState::Texflush)) |
      (1u << static_cast<u32>(ParseState::FirstArrayStart)) |
      (1u << static_cast<u32>(ParseState::SecondArrayStart)) |
      (1u << static_cast<u32>(ParseState::Complete));
  const bool exact_mixed_animator =
      state == ParseState::Complete && out.total_payload_bytes == 416 &&
      out.dma_transfers == 16 && out.payload_transfers == 7 && out.inert_transfers == 4 &&
      out.inert_cnt_transfers == 0 && out.inert_next_transfers == 4 &&
      out.inert_state_mask == kMixedInertStateMask && out.ordinary_descriptors == 1 &&
      out.ordinary_mode == -1 && out.animator_arrays == 2 && out.animator_bytes == 368 &&
      out.finishes == 2 && out.opcode_counts[kStartArray] == 2 &&
      out.opcode_counts[kFinishArray] == 2 && out.opcode_counts[kEraseDestination] == 1 &&
      out.opcode_counts[kUploadClut] == 1 && out.opcode_counts[kGenericUpload] == 1 &&
      out.opcode_counts[kCloudsAndFog] == 1;
  constexpr u32 kOrdinaryOnlyInertStateMask =
      (1u << static_cast<u32>(ParseState::Texflush)) |
      (1u << static_cast<u32>(ParseState::FirstArrayStart));
  const bool no_animator_opcodes =
      std::all_of(out.opcode_counts.begin(), out.opcode_counts.end(), [](u32 count) {
        return count == 0;
      });
  const bool exact_ordinary_only =
      state == ParseState::FirstArrayStart && out.total_payload_bytes == 48 &&
      out.dma_transfers == 4 && out.payload_transfers == 2 && out.inert_transfers == 2 &&
      out.inert_cnt_transfers == 0 && out.inert_next_transfers == 2 &&
      out.inert_state_mask == kOrdinaryOnlyInertStateMask && out.ordinary_descriptors == 1 &&
      out.ordinary_mode == -1 && out.animator_arrays == 0 && out.animator_bytes == 0 &&
      out.finishes == 0 && no_animator_opcodes;
  if (!exact_mixed_animator && !exact_ordinary_only) {
    record_malformed(&out, nullptr);
    return result;
  }
  out.valid = true;
  if (materialize_plan) {
    const ExactShape shape =
        exact_mixed_animator ? ExactShape::Mixed : ExactShape::OrdinaryOnly;
    result.plan = make_execution_plan(shape, out, plan_fields, live_ee_memory);
  }
  return result;
}

}  // namespace

Jak2Bucket4TextureUploadCapture capture_jak2_bucket4_texture_upload(
    const u8* ee_memory,
    std::size_t ee_memory_size,
    u32 chain_offset) {
  return parse_jak2_bucket4_texture_upload(ee_memory, ee_memory_size, chain_offset, ee_memory,
                                           ee_memory_size, false)
      .capture;
}

std::optional<Jak2Bucket4TextureUploadPlan> plan_jak2_bucket4_texture_upload(
    const u8* dma_packet_snapshot,
    std::size_t dma_packet_snapshot_size,
    u32 chain_offset,
    const u8* live_ee_memory,
    std::size_t live_ee_memory_size) {
  return parse_jak2_bucket4_texture_upload(dma_packet_snapshot, dma_packet_snapshot_size,
                                           chain_offset, live_ee_memory, live_ee_memory_size, true)
      .plan;
}

}  // namespace metal_renderer
