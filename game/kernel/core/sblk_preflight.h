#pragma once

#include <cstdint>
#include <span>

namespace sblk_preflight {

enum class Error : std::uint8_t {
  none,
  outer_header,
  file_type,
  chunk_count,
  chunk_range,
  bank_header,
  bank_tag,
  negative_count,
  sound_table,
  grain_table,
  grain_type,
  grain_data,
  sample_data,
  child_sound,
  names,
  user_data,
};

struct Chunk {
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

struct Result {
  Error error = Error::outer_header;
  Chunk bank;
  Chunk samples;

  explicit operator bool() const { return error == Error::none; }
};

// Validates every range and count consumed by SFXBlock::ReadBlock, plus the immediate sound and
// sample references that the loader forms. It does not validate the lifetime of an ADPCM stream or
// every semantic parameter used later during playback because the current Tone model stores no
// sample length.
Result validate(std::span<const std::uint8_t> file) noexcept;
const char* error_name(Error error) noexcept;

}  // namespace sblk_preflight
