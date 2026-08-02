#include "jak1_checked_dgo.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_set>

#include "third-party/lzokay/lzokay.hpp"

namespace jak1_checked_dgo {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kNameFieldBytes = 60;
constexpr std::size_t kObjectAlignment = 16;
constexpr std::size_t kBlzoBlockBytes = 0x8000;
constexpr std::array<std::uint8_t, 4> kBlzoMagic = {'o', 'Z', 'l', 'B'};
constexpr const char* kJak1ArtGroupPrefix = "/src/next/data/art-group6/";
constexpr const char* kArtGroupSuffix = "-ag.go";

Error make_error(ErrorCode code,
                 std::size_t offset,
                 std::string message,
                 std::optional<std::uint32_t> object_index = {}) {
  return {code, offset, object_index, std::move(message)};
}

bool cancelled(const Options& options) {
  return options.should_cancel && options.should_cancel();
}

std::optional<Error> validate_options(const Options& options) {
  if (options.max_name_bytes == 0 || options.max_name_bytes >= kNameFieldBytes ||
      options.max_objects == 0 || options.max_object_bytes == 0 ||
      options.max_total_object_bytes == 0 || options.max_input_bytes == 0 ||
      options.max_expanded_bytes == 0 || options.file_read_chunk_bytes == 0 ||
      options.file_read_chunk_bytes >
          static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) ||
      options.max_compressed_chunk_bytes == 0 ||
      options.max_compressed_chunk_bytes >= kBlzoBlockBytes) {
    return make_error(ErrorCode::invalid_argument, 0, "The DGO reader options are invalid.");
  }
  return {};
}

std::uint32_t read_u32_le(const std::uint8_t* bytes) {
  return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
         (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool align_up(std::size_t value, std::size_t alignment, std::size_t* result) {
  const auto remainder = value % alignment;
  if (remainder == 0) {
    *result = value;
    return true;
  }
  return checked_add(value, alignment - remainder, result);
}

Result<std::string> read_name(std::span<const std::uint8_t> field,
                              std::size_t field_offset,
                              const Options& options,
                              std::optional<std::uint32_t> object_index = {}) {
  const auto terminator = std::find(field.begin(), field.end(), std::uint8_t{0});
  if (terminator == field.end()) {
    return Result<std::string>::failure(make_error(
        ErrorCode::invalid_name, field_offset, "A DGO name is not null-terminated.", object_index));
  }

  const auto length = static_cast<std::size_t>(terminator - field.begin());
  if (length == 0 || length > options.max_name_bytes) {
    return Result<std::string>::failure(
        make_error(ErrorCode::invalid_name, field_offset,
                   "A DGO name is empty or exceeds the configured name limit.", object_index));
  }
  if (std::any_of(terminator + 1, field.end(), [](std::uint8_t byte) { return byte != 0; })) {
    return Result<std::string>::failure(
        make_error(ErrorCode::invalid_name, field_offset,
                   "A DGO name has nonzero bytes after its terminator.", object_index));
  }

  std::string name(reinterpret_cast<const char*>(field.data()), length);
  for (const auto byte : name) {
    const auto value = static_cast<unsigned char>(byte);
    if (value < 0x20 || value > 0x7e || byte == '/' || byte == '\\' || byte == ':') {
      return Result<std::string>::failure(make_error(
          ErrorCode::invalid_name, field_offset,
          "A DGO name contains a path separator, control byte, or non-ASCII byte.", object_index));
    }
  }
  return Result<std::string>::success(std::move(name));
}

Result<std::string> derive_unique_name(const std::string& internal_name,
                                       std::span<const std::uint8_t> object_data,
                                       std::size_t object_offset,
                                       std::uint32_t object_index,
                                       const Options& options) {
  if (internal_name.find("-ag") != std::string::npos) {
    return Result<std::string>::failure(
        make_error(ErrorCode::invalid_name, object_offset,
                   "An internal DGO object name contains the reserved -ag suffix.", object_index));
  }

  const std::string prefix(kJak1ArtGroupPrefix);
  const std::string expected_tail = internal_name + kArtGroupSuffix;
  if (object_data.size() < prefix.size()) {
    return Result<std::string>::success(internal_name);
  }
  for (std::size_t marker_offset = 0; marker_offset <= object_data.size() - prefix.size();
       ++marker_offset) {
    if ((marker_offset % options.file_read_chunk_bytes) == 0 && cancelled(options)) {
      return Result<std::string>::failure(
          make_error(ErrorCode::cancelled, object_offset + marker_offset,
                     "DGO art-group name detection was cancelled.", object_index));
    }
    if (std::memcmp(object_data.data() + marker_offset, prefix.data(), prefix.size())) {
      continue;
    }
    std::size_t tail_offset = 0;
    if (!checked_add(marker_offset, prefix.size(), &tail_offset) ||
        tail_offset > object_data.size() ||
        expected_tail.size() + 1 > object_data.size() - tail_offset) {
      return Result<std::string>::failure(
          make_error(ErrorCode::invalid_art_group_marker, object_offset + marker_offset,
                     "A Jak 1 art-group marker is truncated.", object_index));
    }
    if (std::memcmp(object_data.data() + tail_offset, expected_tail.data(), expected_tail.size()) ||
        object_data[tail_offset + expected_tail.size()] != 0) {
      return Result<std::string>::failure(
          make_error(ErrorCode::invalid_art_group_marker, object_offset + marker_offset,
                     "A Jak 1 art-group marker does not match its DGO object name.", object_index));
    }
    return Result<std::string>::success(internal_name + "-ag");
  }
  return Result<std::string>::success(internal_name);
}

Result<std::vector<std::uint8_t>> decompress_blzo(std::span<const std::uint8_t> input,
                                                  const Options& options) {
  if (input.size() > options.max_compressed_bytes) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::compressed_input_too_large, 0,
                   "The compressed DGO exceeds the configured compressed-input limit."));
  }
  if (input.size() < 8) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::truncated_header, 0, "The compressed DGO header is truncated."));
  }

  const auto expanded_size = static_cast<std::size_t>(read_u32_le(input.data() + 4));
  if (expanded_size > options.max_expanded_bytes) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::expanded_input_too_large, 4,
                   "The compressed DGO declares an expanded size above the configured limit."));
  }
  if (options.max_expansion_ratio != 0 && expanded_size != 0) {
    const auto quotient = expanded_size / input.size();
    const auto remainder = expanded_size % input.size();
    if (quotient > options.max_expansion_ratio ||
        (quotient == options.max_expansion_ratio && remainder != 0)) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::expansion_ratio_exceeded, 4,
                     "The compressed DGO exceeds the configured expansion-ratio limit."));
    }
  }

  std::vector<std::uint8_t> output;
  try {
    output.resize(expanded_size);
  } catch (const std::bad_alloc&) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::allocation_failed, 4, "Could not allocate the expanded DGO buffer."));
  }

  std::size_t input_offset = 8;
  std::size_t output_offset = 0;
  std::uint32_t chunks = 0;
  while (output_offset < output.size()) {
    if (cancelled(options)) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::cancelled, input_offset, "DGO decompression was cancelled."));
    }
    if (++chunks > options.max_compressed_chunks) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::compressed_chunk_limit_exceeded, input_offset,
                     "The compressed DGO exceeds the configured chunk-count limit."));
    }

    std::size_t padding_bytes = 0;
    std::uint32_t chunk_size = 0;
    do {
      if (input_offset > input.size() || input.size() - input_offset < sizeof(std::uint32_t)) {
        return Result<std::vector<std::uint8_t>>::failure(
            make_error(ErrorCode::invalid_compressed_chunk, input_offset,
                       "The compressed DGO ends before its next chunk header."));
      }
      chunk_size = read_u32_le(input.data() + input_offset);
      input_offset += sizeof(std::uint32_t);
      if (chunk_size == 0) {
        padding_bytes += sizeof(std::uint32_t);
        if (padding_bytes > options.max_compressed_padding_bytes) {
          return Result<std::vector<std::uint8_t>>::failure(
              make_error(ErrorCode::compressed_padding_limit_exceeded, input_offset,
                         "The compressed DGO has excessive zero padding between chunks."));
        }
      }
    } while (chunk_size == 0);

    const auto output_remaining = output.size() - output_offset;
    if (chunk_size < kBlzoBlockBytes) {
      if (chunk_size > options.max_compressed_chunk_bytes ||
          chunk_size > input.size() - input_offset) {
        return Result<std::vector<std::uint8_t>>::failure(make_error(
            ErrorCode::invalid_compressed_chunk, input_offset,
            "A compressed DGO chunk is truncated or exceeds the configured chunk limit."));
      }
      std::size_t bytes_written = 0;
      const auto output_capacity = std::min(kBlzoBlockBytes, output_remaining);
      const auto status =
          lzokay::decompress(input.data() + input_offset, chunk_size, output.data() + output_offset,
                             output_capacity, bytes_written);
      if (status != lzokay::EResult::Success || bytes_written == 0 ||
          bytes_written > output_capacity) {
        return Result<std::vector<std::uint8_t>>::failure(make_error(
            ErrorCode::decompression_failed, input_offset,
            "An LZO-compressed DGO chunk could not be decoded within its output bound."));
      }
      input_offset += chunk_size;
      output_offset += bytes_written;
    } else {
      if (output_remaining < kBlzoBlockBytes || input.size() - input_offset < kBlzoBlockBytes) {
        return Result<std::vector<std::uint8_t>>::failure(
            make_error(ErrorCode::invalid_compressed_chunk, input_offset,
                       "A raw compressed-DGO chunk is truncated or exceeds the declared output."));
      }
      std::memcpy(output.data() + output_offset, input.data() + input_offset, kBlzoBlockBytes);
      input_offset += kBlzoBlockBytes;
      output_offset += kBlzoBlockBytes;
    }

    const auto remainder = input_offset % sizeof(std::uint32_t);
    if (remainder != 0) {
      const auto alignment_bytes = sizeof(std::uint32_t) - remainder;
      if (alignment_bytes > input.size() - input_offset) {
        return Result<std::vector<std::uint8_t>>::failure(
            make_error(ErrorCode::invalid_compressed_chunk, input_offset,
                       "A compressed DGO chunk is missing alignment bytes."));
      }
      if (alignment_bytes > options.max_compressed_padding_bytes ||
          padding_bytes > options.max_compressed_padding_bytes - alignment_bytes ||
          std::any_of(input.begin() + input_offset, input.begin() + input_offset + alignment_bytes,
                      [](std::uint8_t byte) { return byte != 0; })) {
        return Result<std::vector<std::uint8_t>>::failure(
            make_error(ErrorCode::compressed_padding_limit_exceeded, input_offset,
                       "A compressed DGO chunk has invalid or excessive alignment padding."));
      }
      input_offset += alignment_bytes;
    }
  }

  const auto trailing_bytes = input.size() - input_offset;
  if (trailing_bytes > options.max_compressed_padding_bytes) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::compressed_padding_limit_exceeded, input_offset,
                   "The compressed DGO has excessive trailing padding."));
  }
  if (std::any_of(input.begin() + input_offset, input.end(),
                  [](std::uint8_t byte) { return byte != 0; })) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::trailing_data, input_offset,
                   "The compressed DGO has nonzero data after its final chunk."));
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(output));
}

Result<Archive> parse_expanded(std::span<const std::uint8_t> input,
                               const std::optional<std::string>& expected_archive_name,
                               const Options& options) {
  if (input.size() > options.max_expanded_bytes) {
    return Result<Archive>::failure(
        make_error(ErrorCode::expanded_input_too_large, 0,
                   "The DGO exceeds the configured expanded-input limit."));
  }
  if (input.size() < kHeaderBytes) {
    return Result<Archive>::failure(
        make_error(ErrorCode::truncated_header, 0, "The DGO archive header is truncated."));
  }
  if (cancelled(options)) {
    return Result<Archive>::failure(
        make_error(ErrorCode::cancelled, 0, "DGO parsing was cancelled."));
  }

  const auto object_count = read_u32_le(input.data());
  if (object_count > options.max_objects) {
    return Result<Archive>::failure(
        make_error(ErrorCode::object_count_limit_exceeded, 0,
                   "The DGO object count exceeds the configured limit."));
  }

  auto archive_name = read_name(input.subspan(4, kNameFieldBytes), 4, options);
  if (!archive_name) {
    return Result<Archive>::failure(archive_name.error());
  }
  if (expected_archive_name && archive_name.value() != *expected_archive_name) {
    return Result<Archive>::failure(
        make_error(ErrorCode::unexpected_archive_name, 4,
                   "The DGO internal name does not match the expected file name."));
  }

  Archive archive;
  archive.internal_name = archive_name.take_value();
  archive.expanded_size = input.size();
  try {
    archive.objects.reserve(object_count);
  } catch (const std::bad_alloc&) {
    return Result<Archive>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Could not allocate the DGO object table."));
  }

  std::unordered_set<std::string> unique_names;
  std::size_t total_object_bytes = 0;
  std::size_t offset = kHeaderBytes;
  for (std::uint32_t index = 0; index < object_count; ++index) {
    if (cancelled(options)) {
      return Result<Archive>::failure(
          make_error(ErrorCode::cancelled, offset, "DGO parsing was cancelled.", index));
    }
    if (offset % kObjectAlignment != 0) {
      return Result<Archive>::failure(make_error(ErrorCode::invalid_alignment, offset,
                                                 "A DGO object header is not 16-byte aligned.",
                                                 index));
    }
    if (offset > input.size() || input.size() - offset < kHeaderBytes) {
      return Result<Archive>::failure(make_error(ErrorCode::truncated_header, offset,
                                                 "A DGO object header is truncated.", index));
    }

    const auto object_size = static_cast<std::size_t>(read_u32_le(input.data() + offset));
    if (object_size == 0 || object_size > options.max_object_bytes) {
      return Result<Archive>::failure(
          make_error(ErrorCode::object_size_limit_exceeded, offset,
                     "A DGO object is empty or exceeds the configured per-object limit.", index));
    }
    auto object_name =
        read_name(input.subspan(offset + 4, kNameFieldBytes), offset + 4, options, index);
    if (!object_name) {
      return Result<Archive>::failure(object_name.error());
    }

    std::size_t data_offset = 0;
    std::size_t data_end = 0;
    if (!checked_add(offset, kHeaderBytes, &data_offset) ||
        !checked_add(data_offset, object_size, &data_end) || data_end > input.size()) {
      return Result<Archive>::failure(
          make_error(ErrorCode::truncated_object, data_offset,
                     "A DGO object payload extends beyond the available input.", index));
    }
    if (data_offset % kObjectAlignment != 0) {
      return Result<Archive>::failure(make_error(ErrorCode::invalid_alignment, data_offset,
                                                 "A DGO object payload is not 16-byte aligned.",
                                                 index));
    }

    std::size_t new_total = 0;
    if (!checked_add(total_object_bytes, object_size, &new_total) ||
        new_total > options.max_total_object_bytes) {
      return Result<Archive>::failure(
          make_error(ErrorCode::total_object_size_limit_exceeded, data_offset,
                     "The DGO object payloads exceed the configured aggregate limit.", index));
    }
    total_object_bytes = new_total;

    auto unique_name = derive_unique_name(
        object_name.value(), input.subspan(data_offset, object_size), data_offset, index, options);
    if (!unique_name) {
      return Result<Archive>::failure(unique_name.error());
    }
    if (unique_names.contains(unique_name.value())) {
      unique_name =
          Result<std::string>::success(unique_name.value() + "-" + std::to_string(object_size));
    }
    if (!unique_names.insert(unique_name.value()).second) {
      return Result<Archive>::failure(make_error(
          ErrorCode::duplicate_object_name, offset,
          "The DGO contains object names that remain ambiguous after size disambiguation.", index));
    }

    Object object;
    object.internal_name = object_name.take_value();
    object.unique_name = unique_name.take_value();
    try {
      object.data.resize(object_size);
      std::size_t copied = 0;
      while (copied < object_size) {
        if (cancelled(options)) {
          return Result<Archive>::failure(make_error(ErrorCode::cancelled, data_offset + copied,
                                                     "Copying a DGO object was cancelled.", index));
        }
        const auto chunk = std::min(options.file_read_chunk_bytes, object_size - copied);
        std::memcpy(object.data.data() + copied, input.data() + data_offset + copied, chunk);
        copied += chunk;
      }
      archive.objects.push_back(std::move(object));
    } catch (const std::bad_alloc&) {
      return Result<Archive>::failure(make_error(ErrorCode::allocation_failed, data_offset,
                                                 "Could not allocate a DGO object payload.",
                                                 index));
    }

    if (!align_up(data_end, kObjectAlignment, &offset) || offset > input.size()) {
      return Result<Archive>::failure(
          make_error(ErrorCode::invalid_alignment, data_end,
                     "A DGO object is missing required 16-byte alignment padding.", index));
    }
  }

  if (offset != input.size()) {
    return Result<Archive>::failure(
        make_error(ErrorCode::trailing_data, offset,
                   "The DGO contains data after the declared object sequence."));
  }
  return Result<Archive>::success(std::move(archive));
}

}  // namespace

Result<Archive> read(std::span<const std::uint8_t> input,
                     const std::optional<std::string>& expected_archive_name,
                     const Options& options) {
  try {
    if (auto error = validate_options(options)) {
      return Result<Archive>::failure(std::move(*error));
    }
    if (input.size() > options.max_input_bytes) {
      return Result<Archive>::failure(make_error(
          ErrorCode::input_too_large, 0, "The DGO input exceeds the configured input limit."));
    }
    if (cancelled(options)) {
      return Result<Archive>::failure(
          make_error(ErrorCode::cancelled, 0, "DGO processing was cancelled."));
    }

    const bool compressed = input.size() >= kBlzoMagic.size() &&
                            std::equal(kBlzoMagic.begin(), kBlzoMagic.end(), input.begin());
    if (compressed) {
      auto expanded = decompress_blzo(input, options);
      if (!expanded) {
        return Result<Archive>::failure(expanded.error());
      }
      auto parsed = parse_expanded(expanded.value(), expected_archive_name, options);
      if (!parsed) {
        return parsed;
      }
      auto archive = parsed.take_value();
      archive.was_compressed = true;
      archive.input_size = input.size();
      return Result<Archive>::success(std::move(archive));
    }

    auto parsed = parse_expanded(input, expected_archive_name, options);
    if (!parsed) {
      return parsed;
    }
    auto archive = parsed.take_value();
    archive.input_size = input.size();
    return Result<Archive>::success(std::move(archive));
  } catch (const std::bad_alloc&) {
    return Result<Archive>::failure(make_error(ErrorCode::allocation_failed, 0,
                                               "Could not allocate memory while reading a DGO."));
  } catch (const std::length_error&) {
    return Result<Archive>::failure(
        make_error(ErrorCode::allocation_failed, 0,
                   "A DGO allocation exceeded the platform container limit."));
  }
}

Result<Archive> read_file(const std::filesystem::path& input_path,
                          const std::optional<std::string>& expected_archive_name,
                          const Options& options) {
  try {
    if (auto error = validate_options(options)) {
      return Result<Archive>::failure(std::move(*error));
    }
    std::ifstream input(input_path, std::ios::binary | std::ios::ate);
    if (!input) {
      return Result<Archive>::failure(
          make_error(ErrorCode::input_open_failed, 0, "Could not open the DGO input file."));
    }
    const auto end = input.tellg();
    if (end < 0) {
      return Result<Archive>::failure(
          make_error(ErrorCode::input_read_failed, 0, "Could not determine the DGO input size."));
    }
    const auto input_size = static_cast<std::uintmax_t>(end);
    if (input_size > options.max_input_bytes ||
        input_size > std::numeric_limits<std::size_t>::max()) {
      return Result<Archive>::failure(make_error(
          ErrorCode::input_too_large, 0, "The DGO input file exceeds the configured input limit."));
    }

    if (input_size >= kBlzoMagic.size()) {
      std::array<std::uint8_t, kBlzoMagic.size()> prefix{};
      input.seekg(0);
      input.read(reinterpret_cast<char*>(prefix.data()), prefix.size());
      if (input.gcount() != static_cast<std::streamsize>(prefix.size())) {
        return Result<Archive>::failure(
            make_error(ErrorCode::input_read_failed, 0, "Could not read the DGO file header."));
      }
      if (prefix == kBlzoMagic && input_size > options.max_compressed_bytes) {
        return Result<Archive>::failure(
            make_error(ErrorCode::compressed_input_too_large, 0,
                       "The compressed DGO file exceeds the configured compressed-input limit."));
      }
    }
    input.clear();
    input.seekg(0);
    if (!input) {
      return Result<Archive>::failure(
          make_error(ErrorCode::input_read_failed, 0, "Could not seek to the DGO file start."));
    }

    std::vector<std::uint8_t> bytes;
    bytes.resize(static_cast<std::size_t>(input_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      if (cancelled(options)) {
        return Result<Archive>::failure(
            make_error(ErrorCode::cancelled, offset, "Reading the DGO input file was cancelled."));
      }
      const auto chunk = std::min(options.file_read_chunk_bytes, bytes.size() - offset);
      input.read(reinterpret_cast<char*>(bytes.data() + offset),
                 static_cast<std::streamsize>(chunk));
      if (input.gcount() != static_cast<std::streamsize>(chunk)) {
        return Result<Archive>::failure(make_error(ErrorCode::input_read_failed, offset,
                                                   "Could not read the complete DGO file."));
      }
      offset += chunk;
    }
    return read(bytes, expected_archive_name, options);
  } catch (const std::bad_alloc&) {
    return Result<Archive>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Could not allocate the DGO input buffer."));
  } catch (const std::length_error&) {
    return Result<Archive>::failure(
        make_error(ErrorCode::allocation_failed, 0,
                   "The DGO input buffer exceeds the platform container limit."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::input_open_failed:
      return "input_open_failed";
    case ErrorCode::input_read_failed:
      return "input_read_failed";
    case ErrorCode::input_too_large:
      return "input_too_large";
    case ErrorCode::compressed_input_too_large:
      return "compressed_input_too_large";
    case ErrorCode::expanded_input_too_large:
      return "expanded_input_too_large";
    case ErrorCode::expansion_ratio_exceeded:
      return "expansion_ratio_exceeded";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::truncated_header:
      return "truncated_header";
    case ErrorCode::invalid_name:
      return "invalid_name";
    case ErrorCode::unexpected_archive_name:
      return "unexpected_archive_name";
    case ErrorCode::object_count_limit_exceeded:
      return "object_count_limit_exceeded";
    case ErrorCode::object_size_limit_exceeded:
      return "object_size_limit_exceeded";
    case ErrorCode::total_object_size_limit_exceeded:
      return "total_object_size_limit_exceeded";
    case ErrorCode::truncated_object:
      return "truncated_object";
    case ErrorCode::invalid_alignment:
      return "invalid_alignment";
    case ErrorCode::trailing_data:
      return "trailing_data";
    case ErrorCode::compressed_padding_limit_exceeded:
      return "compressed_padding_limit_exceeded";
    case ErrorCode::compressed_chunk_limit_exceeded:
      return "compressed_chunk_limit_exceeded";
    case ErrorCode::invalid_compressed_chunk:
      return "invalid_compressed_chunk";
    case ErrorCode::decompression_failed:
      return "decompression_failed";
    case ErrorCode::invalid_art_group_marker:
      return "invalid_art_group_marker";
    case ErrorCode::duplicate_object_name:
      return "duplicate_object_name";
  }
  return "unknown";
}

}  // namespace jak1_checked_dgo
