#include "game/kernel/core/sblk_preflight.h"

#include <cstddef>
#include <cstring>
#include <limits>

namespace sblk_preflight {
namespace {

constexpr std::uint32_t kSblk = std::uint32_t('S') | (std::uint32_t('B') << 8) |
                                (std::uint32_t('l') << 16) | (std::uint32_t('k') << 24);
constexpr std::size_t kOuterHeaderSize = 24;
constexpr std::size_t kV1HeaderSize = 60;
constexpr std::size_t kV2HeaderSize = 64;
constexpr std::size_t kSoundRecordSize = 12;
constexpr std::size_t kV1GrainSize = 0x28;
constexpr std::size_t kV2GrainSize = 8;
constexpr std::size_t kToneSize = 24;
constexpr std::size_t kLfoSize = 16;
constexpr std::size_t kPlaySoundSize = 32;
constexpr std::size_t kPluginSize = 32;
constexpr std::size_t kNamesHeaderSize = 0x98;
constexpr std::size_t kNameRecordSize = 0x14;
constexpr std::size_t kUserDataSize = 0x10;

constexpr std::uint8_t kTone = 1;
constexpr std::uint8_t kStartChildSound = 5;
constexpr std::uint8_t kStopChildSound = 6;
constexpr std::uint8_t kPluginMessage = 7;
constexpr std::uint8_t kBranch = 8;
constexpr std::uint8_t kTone2 = 9;
constexpr std::uint8_t kMaximumGrainType = 44;
constexpr std::uint16_t kNoiseTone = 8;
constexpr std::uint8_t kLfoCount = 4;
constexpr std::uint32_t kHasNames = 0x100;
constexpr std::uint32_t kHasUserData = 0x200;

bool contains(std::size_t size, std::size_t offset, std::size_t length) {
  return offset <= size && length <= size - offset;
}

bool add(std::size_t left, std::size_t right, std::size_t* out) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *out = left + right;
  return true;
}

bool multiply(std::size_t left, std::size_t right, std::size_t* out) {
  if (left && right > std::numeric_limits<std::size_t>::max() / left) {
    return false;
  }
  *out = left * right;
  return true;
}

template <typename T>
bool read(std::span<const std::uint8_t> data, std::size_t offset, T* out) {
  if (!contains(data.size(), offset, sizeof(T))) {
    return false;
  }
  std::memcpy(out, data.data() + offset, sizeof(T));
  return true;
}

Result failure(Error error, Chunk bank = {}, Chunk samples = {}) {
  return {error, bank, samples};
}

bool validate_tone(std::span<const std::uint8_t> bank,
                   std::span<const std::uint8_t> samples,
                   std::size_t tone_offset) {
  std::uint16_t flags = 0;
  std::uint32_t sample_offset = 0;
  if (!contains(bank.size(), tone_offset, kToneSize) || !read(bank, tone_offset + 14, &flags) ||
      !read(bank, tone_offset + 16, &sample_offset)) {
    return false;
  }
  if (flags & kNoiseTone) {
    return sample_offset <= samples.size();
  }
  return !(sample_offset & 1) && contains(samples.size(), sample_offset, sizeof(std::uint16_t));
}

bool validate_lfo(std::span<const std::uint8_t> bank, std::size_t lfo_offset) {
  std::uint8_t which_lfo = 0;
  return contains(bank.size(), lfo_offset, kLfoSize) && read(bank, lfo_offset, &which_lfo) &&
         which_lfo < kLfoCount;
}

bool child_sound_is_safe(std::span<const std::uint8_t> bank,
                         std::size_t payload_offset,
                         std::int16_t sound_count,
                         std::size_t first_sound,
                         bool branch) {
  std::int32_t child = -1;
  if (!read(bank, payload_offset + 12, &child)) {
    return false;
  }
  if (child < 0) {
    return true;
  }
  if (child >= sound_count) {
    return false;
  }
  if (!branch) {
    return true;
  }
  std::int8_t child_grains = 0;
  const std::size_t child_record = first_sound + std::size_t(child) * kSoundRecordSize;
  return read(bank, child_record + 4, &child_grains) && child_grains > 0;
}

Error validate_names(std::span<const std::uint8_t> bank,
                     std::uint32_t bank_file_offset,
                     std::size_t block_names,
                     std::int16_t sound_count) {
  if (!contains(bank.size(), block_names, kNamesHeaderSize)) {
    return Error::names;
  }

  std::uint32_t table_relative = 0;
  if (!read(bank, block_names + 8, &table_relative)) {
    return Error::names;
  }
  std::size_t table = 0;
  if (!add(block_names, table_relative, &table) || table > bank.size() ||
      ((std::size_t(bank_file_offset) + table) & 3)) {
    return Error::names;
  }

  const std::size_t record_count = (bank.size() - table) / kNameRecordSize;
  for (std::size_t bucket = 0; bucket < 32; bucket++) {
    std::int16_t first = -1;
    if (!read(bank, block_names + 0x18 + bucket * sizeof(first), &first) || first < 0 ||
        std::size_t(first) >= record_count) {
      return Error::names;
    }

    bool terminated = false;
    for (std::size_t index = std::size_t(first); index < record_count; index++) {
      const std::size_t record = table + index * kNameRecordSize;
      std::uint32_t first_name_word = 0;
      if (!read(bank, record, &first_name_word)) {
        return Error::names;
      }
      if (!first_name_word) {
        terminated = true;
        break;
      }
      std::int16_t sound = -1;
      if (!read(bank, record + 0x10, &sound) || sound < 0 || sound >= sound_count) {
        return Error::names;
      }
    }
    if (!terminated) {
      return Error::names;
    }
  }
  return Error::none;
}

}  // namespace

Result validate(std::span<const std::uint8_t> file) noexcept {
  if (file.size() < kOuterHeaderSize) {
    return failure(Error::outer_header);
  }

  std::uint32_t file_type = 0;
  std::uint32_t chunk_count = 0;
  Chunk bank;
  Chunk samples;
  if (!read(file, 0, &file_type) || !read(file, 4, &chunk_count) ||
      !read(file, 8, &bank.offset) || !read(file, 12, &bank.size) ||
      !read(file, 16, &samples.offset) || !read(file, 20, &samples.size)) {
    return failure(Error::outer_header);
  }
  if (file_type != 1 && file_type != 3) {
    return failure(Error::file_type, bank, samples);
  }
  if (chunk_count != 2) {
    return failure(Error::chunk_count, bank, samples);
  }
  if (!contains(file.size(), bank.offset, bank.size) ||
      !contains(file.size(), samples.offset, samples.size)) {
    return failure(Error::chunk_range, bank, samples);
  }

  const auto bank_data = file.subspan(bank.offset, bank.size);
  const auto sample_data = file.subspan(samples.offset, samples.size);
  std::uint32_t tag = 0;
  std::uint32_t version = 0;
  if (!read(bank_data, 0, &tag) || !read(bank_data, 4, &version)) {
    return failure(Error::bank_header, bank, samples);
  }
  if (tag != kSblk) {
    return failure(Error::bank_tag, bank, samples);
  }
  const std::size_t header_size = version < 2 ? kV1HeaderSize : kV2HeaderSize;
  if (bank_data.size() < header_size) {
    return failure(Error::bank_header, bank, samples);
  }

  std::uint32_t flags = 0;
  std::int16_t sound_count = -1;
  std::int16_t grain_count = -1;
  std::int16_t vag_count = -1;
  std::uint32_t first_sound_field = 0;
  std::uint32_t first_grain_field = 0;
  std::uint32_t grain_data_field = 0;
  std::uint32_t block_names_field = 0;
  std::uint32_t user_data_field = 0;
  if (!read(bank_data, 8, &flags) || !read(bank_data, 22, &sound_count) ||
      !read(bank_data, 24, &grain_count) || !read(bank_data, 26, &vag_count) ||
      !read(bank_data, 28, &first_sound_field) || !read(bank_data, 32, &first_grain_field)) {
    return failure(Error::bank_header, bank, samples);
  }
  if (sound_count < 0 || grain_count < 0 || vag_count < 0) {
    return failure(Error::negative_count, bank, samples);
  }
  if (version >= 2) {
    if (!read(bank_data, 52, &grain_data_field) || !read(bank_data, 56, &block_names_field) ||
        !read(bank_data, 60, &user_data_field)) {
      return failure(Error::bank_header, bank, samples);
    }
  } else if (!read(bank_data, 52, &block_names_field) ||
             !read(bank_data, 56, &user_data_field)) {
    return failure(Error::bank_header, bank, samples);
  }

  const std::size_t first_sound = first_sound_field;
  const std::size_t first_grain = first_grain_field;
  const std::size_t grain_data = grain_data_field;
  const std::size_t grain_size = version < 2 ? kV1GrainSize : kV2GrainSize;
  std::size_t sound_bytes = 0;
  std::size_t grain_bytes = 0;
  if (!multiply(std::size_t(sound_count), kSoundRecordSize, &sound_bytes) ||
      !contains(bank_data.size(), first_sound, sound_bytes)) {
    return failure(Error::sound_table, bank, samples);
  }
  if (!multiply(std::size_t(grain_count), grain_size, &grain_bytes) ||
      (grain_count && !contains(bank_data.size(), first_grain, grain_bytes)) ||
      (version >= 2 && grain_count && grain_data > bank_data.size())) {
    return failure(Error::grain_table, bank, samples);
  }

  for (std::size_t sound = 0; sound < std::size_t(sound_count); sound++) {
    const std::size_t sound_record = first_sound + sound * kSoundRecordSize;
    std::int8_t sound_grain_count = -1;
    std::uint32_t first_sound_grain_field = 0;
    if (!read(bank_data, sound_record + 4, &sound_grain_count) ||
        !read(bank_data, sound_record + 8, &first_sound_grain_field) || sound_grain_count < 0) {
      return failure(Error::negative_count, bank, samples);
    }
    if (!sound_grain_count) {
      continue;
    }

    const std::size_t first_sound_grain = first_sound_grain_field;
    std::size_t sound_grain_bytes = 0;
    if (!multiply(std::size_t(sound_grain_count), grain_size, &sound_grain_bytes) ||
        !contains(grain_bytes, first_sound_grain, sound_grain_bytes)) {
      return failure(Error::grain_table, bank, samples);
    }
    std::size_t grain_record = 0;
    if (!add(first_grain, first_sound_grain, &grain_record)) {
      return failure(Error::grain_table, bank, samples);
    }

    for (std::size_t grain = 0; grain < std::size_t(sound_grain_count); grain++) {
      const std::size_t record = grain_record + grain * grain_size;
      std::uint8_t type = 0;
      std::size_t payload = record + 8;
      if (version < 2) {
        std::uint32_t type32 = 0;
        if (!read(bank_data, record, &type32) || type32 > kMaximumGrainType) {
          return failure(Error::grain_type, bank, samples);
        }
        type = std::uint8_t(type32);
      } else {
        std::uint32_t opcode = 0;
        if (!read(bank_data, record, &opcode)) {
          return failure(Error::grain_data, bank, samples);
        }
        type = std::uint8_t(opcode >> 24);
        if (type > kMaximumGrainType) {
          return failure(Error::grain_type, bank, samples);
        }
        if (!add(grain_data, opcode & 0x00ffffff, &payload)) {
          return failure(Error::grain_data, bank, samples);
        }
      }

      switch (type) {
        case kTone:
        case kTone2:
          if (!validate_tone(bank_data, sample_data, payload)) {
            return failure(Error::sample_data, bank, samples);
          }
          break;
        case kStartChildSound:
        case kStopChildSound:
        case kBranch:
          if (!contains(bank_data.size(), payload, kPlaySoundSize)) {
            return failure(Error::grain_data, bank, samples);
          }
          if (!child_sound_is_safe(bank_data, payload, sound_count, first_sound,
                                   type == kBranch)) {
            return failure(Error::child_sound, bank, samples);
          }
          break;
        case kPluginMessage:
          if (!contains(bank_data.size(), payload, kPluginSize)) {
            return failure(Error::grain_data, bank, samples);
          }
          break;
        case 4:
          if (!validate_lfo(bank_data, payload)) {
            return failure(Error::grain_data, bank, samples);
          }
          break;
        default:
          break;
      }
    }
  }

  if (flags & kHasNames) {
    const Error names_error =
        validate_names(bank_data, bank.offset, block_names_field, sound_count);
    if (names_error != Error::none) {
      return failure(names_error, bank, samples);
    }
  }
  if (flags & kHasUserData) {
    std::size_t user_bytes = 0;
    if (!multiply(std::size_t(sound_count), kUserDataSize, &user_bytes) ||
        !contains(bank_data.size(), user_data_field, user_bytes)) {
      return failure(Error::user_data, bank, samples);
    }
  }

  return {Error::none, bank, samples};
}

const char* error_name(Error error) noexcept {
  switch (error) {
    case Error::none:
      return "none";
    case Error::outer_header:
      return "outer-header";
    case Error::file_type:
      return "file-type";
    case Error::chunk_count:
      return "chunk-count";
    case Error::chunk_range:
      return "chunk-range";
    case Error::bank_header:
      return "bank-header";
    case Error::bank_tag:
      return "bank-tag";
    case Error::negative_count:
      return "negative-count";
    case Error::sound_table:
      return "sound-table";
    case Error::grain_table:
      return "grain-table";
    case Error::grain_type:
      return "grain-type";
    case Error::grain_data:
      return "grain-data";
    case Error::sample_data:
      return "sample-data";
    case Error::child_sound:
      return "child-sound";
    case Error::names:
      return "names";
    case Error::user_data:
      return "user-data";
  }
  return "unknown";
}

}  // namespace sblk_preflight
