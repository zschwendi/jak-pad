#include "read_iso_file.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#endif
#include <fstream>
#include <limits>
#include <set>
#include <unordered_set>
#ifndef _WIN32
#include <unistd.h>

#include "common/util/PosixFile.h"

#include <sys/stat.h>
#endif

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

constexpr uint64_t kSectorSize = 0x800;
constexpr uint32_t kFirstDescriptorSector = 16;
constexpr uint32_t kMaxDescriptorSectors = 64;
constexpr size_t kMaxReadChunkBytes = 8 * 1024 * 1024;
constexpr uint64_t kProgressIntervalBytes = 8ull * 1024 * 1024;

using iso_file::Error;
using iso_file::ErrorCode;

Error make_error(ErrorCode code,
                 uint64_t offset,
                 std::string message,
                 std::error_code system_error = {}) {
  return {code, offset, std::move(message), system_error};
}

Error make_system_error(ErrorCode code,
                        uint64_t offset,
                        std::string message,
                        int error_number) {
  const std::error_code system_error(error_number, std::generic_category());
  if (system_error) {
    message += ": " + system_error.message();
  }
  return make_error(code, offset, std::move(message), system_error);
}

int seek_64(FILE* file, uint64_t offset, int origin) {
  if (offset > uint64_t(LLONG_MAX)) {
    errno = EOVERFLOW;
    return -1;
  }
#ifdef _WIN32
  return _fseeki64(file, static_cast<long long>(offset), origin);
#else
  return fseeko(file, static_cast<off_t>(offset), origin);
#endif
}

int64_t tell_64(FILE* file) {
#ifdef _WIN32
  return _ftelli64(file);
#else
  return ftello(file);
#endif
}

std::optional<Error> image_size(FILE* file, uint64_t* size) {
  errno = 0;
  const auto original = tell_64(file);
  if (original < 0) {
    return make_system_error(ErrorCode::seek_failed, 0,
                             "Could not determine the current ISO image position", errno);
  }
  errno = 0;
  if (seek_64(file, 0, SEEK_END)) {
    return make_system_error(ErrorCode::seek_failed, 0, "Could not seek to the ISO image end",
                             errno);
  }
  errno = 0;
  const auto end = tell_64(file);
  if (end < 0) {
    return make_system_error(ErrorCode::seek_failed, 0, "Could not determine the ISO image size",
                             errno);
  }
  errno = 0;
  if (seek_64(file, static_cast<uint64_t>(original), SEEK_SET)) {
    return make_system_error(ErrorCode::seek_failed, 0,
                             "Could not restore the ISO image position", errno);
  }
  *size = static_cast<uint64_t>(end);
  return std::nullopt;
}

std::optional<Error> read_exact(FILE* file,
                                uint64_t image_bytes,
                                uint64_t offset,
                                void* destination,
                                size_t size) {
  if (offset > image_bytes || size > image_bytes - offset) {
    return make_error(ErrorCode::extent_out_of_bounds, offset,
                      "A read extends beyond the ISO image.");
  }
  errno = 0;
  if (seek_64(file, offset, SEEK_SET)) {
    return make_system_error(ErrorCode::seek_failed, offset,
                             "Could not seek to the requested ISO bytes", errno);
  }
  if (size) {
    errno = 0;
    const auto bytes_read = fread(destination, 1, size, file);
    if (bytes_read != size) {
      const auto failure_offset = offset + bytes_read;
      if (ferror(file)) {
        return make_system_error(ErrorCode::read_failed, failure_offset,
                                 "Could not read the requested ISO bytes", errno);
      }
      return make_error(ErrorCode::read_failed, failure_offset,
                        "The ISO image ended before the requested bytes were read.");
    }
  }
  return std::nullopt;
}

std::optional<Error> read_extent_chunk(FILE* file,
                                       uint64_t image_bytes,
                                       uint64_t offset,
                                       void* destination,
                                       size_t size) {
  if (offset > image_bytes || size > image_bytes - offset) {
    return make_error(ErrorCode::extent_out_of_bounds, offset,
                      "A read extends beyond the ISO image.");
  }
#ifdef _WIN32
  return read_exact(file, image_bytes, offset, destination, size);
#else
  errno = 0;
  const auto descriptor = fileno(file);
  if (descriptor < 0) {
    return make_system_error(ErrorCode::read_failed, offset,
                             "Could not access the ISO file descriptor", errno);
  }
  size_t completed = 0;
  while (completed < size) {
    const auto result = ::pread(descriptor, static_cast<uint8_t*>(destination) + completed,
                                size - completed, static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0) {
      const int read_error = errno;
      return make_system_error(ErrorCode::read_failed, offset + completed,
                               "Could not read the requested ISO bytes", read_error);
    }
    if (result == 0) {
      return make_error(ErrorCode::read_failed, offset + completed,
                        "The ISO image ended before the requested bytes were read.");
    }
    completed += static_cast<size_t>(result);
  }
  return std::nullopt;
#endif
}

uint16_t read_le16(const uint8_t* bytes) {
  return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
}

uint16_t read_be16(const uint8_t* bytes) {
  return (uint16_t(bytes[0]) << 8) | uint16_t(bytes[1]);
}

uint32_t read_le32(const uint8_t* bytes) {
  return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) |
         (uint32_t(bytes[3]) << 24);
}

uint32_t read_be32(const uint8_t* bytes) {
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

std::optional<Error> read_both16(const uint8_t* bytes, uint64_t image_offset, uint16_t* value) {
  const auto little = read_le16(bytes);
  const auto big = read_be16(bytes + 2);
  if (little != big) {
    return make_error(ErrorCode::invalid_descriptor, image_offset,
                      "ISO little- and big-endian 16-bit values disagree.");
  }
  *value = little;
  return std::nullopt;
}

std::optional<Error> read_both32(const uint8_t* bytes,
                                 uint64_t image_offset,
                                 uint32_t* value,
                                 ErrorCode code = ErrorCode::invalid_descriptor) {
  const auto little = read_le32(bytes);
  const auto big = read_be32(bytes + 4);
  if (little != big) {
    return make_error(code, image_offset, "ISO little- and big-endian 32-bit values disagree.");
  }
  *value = little;
  return std::nullopt;
}

bool checked_multiply(uint64_t left, uint64_t right, uint64_t* result) {
  if (left && right > std::numeric_limits<uint64_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

std::string output_name(const std::string& name) {
  return name == "WATER_AN.CGO" ? "WATER-AN.CGO" : name;
}

std::filesystem::path standard_path(const fs::path& path) {
#ifdef _WIN32
  return std::filesystem::path(path.wstring());
#else
  return std::filesystem::path(path.string());
#endif
}

std::string collision_key(const std::string& name) {
  auto key = output_name(name);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A'))
                                      : static_cast<char>(byte);
  });
  return key;
}

std::optional<Error> validate_path_component(const std::string& name,
                                             uint64_t image_offset,
                                             const iso_file::Options& options) {
  if (name.empty() || name == "." || name == ".." || name.size() > options.max_name_bytes) {
    return make_error(ErrorCode::unsafe_path, image_offset,
                      "The ISO contains an empty, reserved, or overlong path component.");
  }
  if (name.back() == '.' || name.back() == ' ') {
    return make_error(ErrorCode::unsafe_path, image_offset,
                      "The ISO contains a path component with an unsafe ending.");
  }
  for (const auto byte : name) {
    const auto value = static_cast<unsigned char>(byte);
    if (value < 0x20 || value > 0x7e || byte == '/' || byte == '\\' || byte == ':') {
      return make_error(ErrorCode::unsafe_path, image_offset,
                        "The ISO contains a path separator, control byte, or unsafe character.");
    }
  }
  return std::nullopt;
}

struct ParsedRecord {
  bool is_directory = false;
  bool is_dot = false;
  bool is_dot_dot = false;
  uint32_t extent_sector = 0;
  uint32_t data_size = 0;
  std::string name;
};

std::optional<Error> parse_record(const uint8_t* record,
                                  size_t available,
                                  uint64_t image_offset,
                                  const iso_file::Options& options,
                                  ParsedRecord* parsed) {
  if (available < 34 || record[0] < 34 || record[0] > available) {
    return make_error(ErrorCode::invalid_directory_record, image_offset,
                      "An ISO directory record has an invalid length.");
  }
  const auto record_size = record[0];
  const auto identifier_size = record[32];
  if (identifier_size == 0 || size_t(33) + identifier_size > record_size) {
    return make_error(ErrorCode::invalid_directory_record, image_offset,
                      "An ISO directory record has an invalid identifier length.");
  }
  if (record[1] != 0 || record[26] != 0 || record[27] != 0 || (record[25] & 0x80)) {
    return make_error(ErrorCode::unsupported_format, image_offset,
                      "Extended attributes, interleaving, and multi-extent files are unsupported.");
  }

  uint32_t extent = 0;
  if (auto error =
          read_both32(record + 2, image_offset + 2, &extent, ErrorCode::invalid_directory_record)) {
    return error;
  }
  uint32_t size = 0;
  if (auto error =
          read_both32(record + 10, image_offset + 10, &size, ErrorCode::invalid_directory_record)) {
    return error;
  }
  const auto little_sequence = read_le16(record + 28);
  const auto big_sequence = read_be16(record + 30);
  if (little_sequence != big_sequence || little_sequence != 1) {
    return make_error(ErrorCode::invalid_directory_record, image_offset + 28,
                      "An ISO directory record has an invalid volume sequence.");
  }
  parsed->is_directory = (record[25] & 0x02) != 0;
  parsed->extent_sector = extent;
  parsed->data_size = size;
  if (identifier_size == 1 && record[33] == 0) {
    parsed->is_dot = true;
    return std::nullopt;
  }
  if (identifier_size == 1 && record[33] == 1) {
    parsed->is_dot_dot = true;
    return std::nullopt;
  }

  parsed->name.assign(reinterpret_cast<const char*>(record + 33), identifier_size);
  if (!parsed->is_directory) {
    if (parsed->name.size() < 3 || parsed->name.substr(parsed->name.size() - 2) != ";1") {
      return make_error(ErrorCode::unsupported_format, image_offset + 33,
                        "An ISO file identifier is missing the supported ;1 version suffix.");
    }
    parsed->name.resize(parsed->name.size() - 2);
  }
  return validate_path_component(output_name(parsed->name), image_offset + 33, options);
}

struct ParseState {
  FILE* file = nullptr;
  const iso_file::Options* options = nullptr;
  uint64_t image_bytes = 0;
  uint64_t volume_bytes = 0;
  uint32_t entry_count = 0;
  uint64_t total_file_bytes = 0;
  std::set<std::pair<uint64_t, uint64_t>> visited_directories;
};

std::optional<Error> cancelled(const ParseState& state, uint64_t offset) {
  if (state.options->should_cancel && state.options->should_cancel()) {
    return make_error(ErrorCode::cancelled, offset, "ISO processing was cancelled.");
  }
  return std::nullopt;
}

std::optional<Error> validate_extent(const ParseState& state,
                                     uint32_t sector,
                                     uint64_t size,
                                     uint64_t image_offset,
                                     uint64_t* byte_offset) {
  if (!checked_multiply(sector, kSectorSize, byte_offset) || *byte_offset > state.volume_bytes ||
      size > state.volume_bytes - *byte_offset || *byte_offset > state.image_bytes ||
      size > state.image_bytes - *byte_offset) {
    return make_error(ErrorCode::extent_out_of_bounds, image_offset,
                      "An ISO extent lies outside the declared image volume.");
  }
  return std::nullopt;
}

std::optional<Error> parse_directory(ParseState* state,
                                     uint32_t sector,
                                     uint32_t size,
                                     uint32_t depth,
                                     const std::string& prefix,
                                     IsoFile::Entry* parent) {
  if (depth > state->options->max_depth) {
    return make_error(ErrorCode::depth_limit_exceeded, uint64_t(sector) * kSectorSize,
                      "The ISO directory nesting limit was exceeded.");
  }
  if (size > state->options->max_directory_bytes) {
    return make_error(ErrorCode::file_size_limit_exceeded, uint64_t(sector) * kSectorSize,
                      "An ISO directory exceeds the configured size limit.");
  }

  uint64_t directory_offset = 0;
  if (auto error = validate_extent(*state, sector, size, uint64_t(sector) * kSectorSize,
                                   &directory_offset)) {
    return error;
  }
  if (!state->visited_directories.emplace(directory_offset, size).second) {
    return make_error(ErrorCode::invalid_directory_record, directory_offset,
                      "The ISO contains a repeated or cyclic directory extent.");
  }

  std::unordered_set<std::string> output_names;
  uint64_t position = 0;
  std::array<uint8_t, 255> record{};
  while (position < size) {
    if (auto error = cancelled(*state, directory_offset + position)) {
      return error;
    }
    uint8_t record_size = 0;
    if (auto error = read_exact(state->file, state->image_bytes, directory_offset + position,
                                &record_size, 1)) {
      return error;
    }
    if (record_size == 0) {
      const auto next_sector = ((position / kSectorSize) + 1) * kSectorSize;
      if (next_sector <= position) {
        return make_error(ErrorCode::invalid_directory_record, directory_offset + position,
                          "An ISO directory record did not advance.");
      }
      position = std::min<uint64_t>(next_sector, size);
      continue;
    }
    const auto offset_in_sector = position % kSectorSize;
    if (record_size > size - position || record_size > kSectorSize - offset_in_sector) {
      return make_error(ErrorCode::invalid_directory_record, directory_offset + position,
                        "An ISO directory record crosses its directory or sector boundary.");
    }
    if (auto error = read_exact(state->file, state->image_bytes, directory_offset + position,
                                record.data(), record_size)) {
      return error;
    }

    ParsedRecord parsed;
    if (auto error = parse_record(record.data(), record_size, directory_offset + position,
                                  *state->options, &parsed)) {
      return error;
    }
    if (parsed.is_dot || parsed.is_dot_dot) {
      position += record_size;
      continue;
    }
    if (++state->entry_count > state->options->max_entries) {
      return make_error(ErrorCode::entry_limit_exceeded, directory_offset + position,
                        "The ISO entry-count limit was exceeded.");
    }

    const auto safe_name = output_name(parsed.name);
    const auto relative_path = prefix.empty() ? safe_name : prefix + "/" + safe_name;
    if (relative_path.size() > state->options->max_path_bytes) {
      return make_error(ErrorCode::unsafe_path, directory_offset + position,
                        "The ISO contains an overlong output path.");
    }
    if (!output_names.emplace(collision_key(safe_name)).second) {
      return make_error(ErrorCode::unsafe_path, directory_offset + position,
                        "The ISO contains duplicate output names in one directory.");
    }

    auto& entry = parent->children.emplace_back();
    entry.is_dir = parsed.is_directory;
    entry.name = parsed.name;
    uint64_t entry_offset = 0;
    if (auto error = validate_extent(*state, parsed.extent_sector, parsed.data_size,
                                     directory_offset + position + 2, &entry_offset)) {
      return error;
    }
    if (entry_offset > std::numeric_limits<size_t>::max()) {
      return make_error(ErrorCode::file_size_limit_exceeded, directory_offset + position,
                        "An ISO entry cannot be represented on this platform.");
    }
    entry.offset_in_file = static_cast<size_t>(entry_offset);
    entry.size = static_cast<size_t>(parsed.data_size);

    if (entry.is_dir) {
      if (auto error = parse_directory(state, parsed.extent_sector, parsed.data_size, depth + 1,
                                       relative_path, &entry)) {
        return error;
      }
    } else {
      if (parsed.data_size > state->options->max_file_bytes) {
        return make_error(ErrorCode::file_size_limit_exceeded, entry_offset,
                          "An ISO file exceeds the configured size limit.");
      }
      if (parsed.data_size > state->options->max_total_output_bytes ||
          state->total_file_bytes > state->options->max_total_output_bytes - parsed.data_size) {
        return make_error(ErrorCode::total_size_limit_exceeded, entry_offset,
                          "The ISO output exceeds the configured total-size limit.");
      }
      state->total_file_bytes += parsed.data_size;
    }
    position += record_size;
  }
  return std::nullopt;
}

struct LayoutStats {
  uint32_t file_count = 0;
  uint32_t entry_count = 0;
  uint64_t total_bytes = 0;
};

std::optional<Error> validate_layout_entry(const IsoFile::Entry& entry,
                                           const std::string& prefix,
                                           uint32_t depth,
                                           uint64_t image_bytes,
                                           const iso_file::Options& options,
                                           LayoutStats* stats) {
  if (++stats->entry_count > options.max_entries) {
    return make_error(ErrorCode::entry_limit_exceeded, entry.offset_in_file,
                      "The ISO entry-count limit was exceeded.");
  }
  if (depth > options.max_depth) {
    return make_error(ErrorCode::depth_limit_exceeded, entry.offset_in_file,
                      "The ISO directory nesting limit was exceeded.");
  }
  if (auto error =
          validate_path_component(output_name(entry.name), entry.offset_in_file, options)) {
    return error;
  }
  const auto path =
      prefix.empty() ? output_name(entry.name) : prefix + "/" + output_name(entry.name);
  if (path.size() > options.max_path_bytes) {
    return make_error(ErrorCode::unsafe_path, entry.offset_in_file,
                      "The ISO contains an overlong output path.");
  }

  if (entry.is_dir) {
    std::unordered_set<std::string> names;
    for (const auto& child : entry.children) {
      if (!names.emplace(collision_key(child.name)).second) {
        return make_error(ErrorCode::unsafe_path, child.offset_in_file,
                          "The ISO contains duplicate output names in one directory.");
      }
      if (auto error = validate_layout_entry(child, path, depth + 1, image_bytes, options, stats)) {
        return error;
      }
    }
    return std::nullopt;
  }

  if (entry.size > options.max_file_bytes || entry.offset_in_file > image_bytes ||
      entry.size > image_bytes - entry.offset_in_file) {
    return make_error(entry.size > options.max_file_bytes ? ErrorCode::file_size_limit_exceeded
                                                          : ErrorCode::extent_out_of_bounds,
                      entry.offset_in_file, "An ISO file violates the configured size or bounds.");
  }
  if (entry.size > options.max_total_output_bytes ||
      stats->total_bytes > options.max_total_output_bytes - entry.size) {
    return make_error(ErrorCode::total_size_limit_exceeded, entry.offset_in_file,
                      "The ISO output exceeds the configured total-size limit.");
  }
  stats->total_bytes += entry.size;
  stats->file_count++;
  return std::nullopt;
}

std::optional<Error> validate_options(FILE* file,
                                      const iso_file::Options& options,
                                      uint64_t* bytes) {
  if (!file) {
    return make_error(ErrorCode::invalid_argument, 0, "The ISO file handle is null.");
  }
  if (!options.max_image_bytes || !options.max_directory_bytes || !options.max_file_bytes ||
      !options.max_total_output_bytes || !options.max_entries || !options.max_name_bytes ||
      !options.max_path_bytes || !options.read_chunk_bytes ||
      options.read_chunk_bytes > kMaxReadChunkBytes) {
    return make_error(ErrorCode::invalid_argument, 0, "The ISO reader options are invalid.");
  }
  if (auto error = image_size(file, bytes)) {
    return error;
  }
  if (*bytes > options.max_image_bytes) {
    return make_error(ErrorCode::file_size_limit_exceeded, 0,
                      "The ISO image exceeds the configured image-size limit.");
  }
  if (*bytes < uint64_t(kFirstDescriptorSector + 1) * kSectorSize) {
    return make_error(ErrorCode::invalid_descriptor, 0,
                      "The file is too small to contain an ISO9660 descriptor.");
  }
  return std::nullopt;
}

std::optional<Error> ensure_output_directory(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  if (error) {
    return make_error(ErrorCode::output_create_failed, 0,
                      "Could not create output directory: " + path.string() + ": " +
                          error.message(),
                      error);
  }
  if (!std::filesystem::is_directory(path, error) || error) {
    auto message = "Could not retain output directory: " + path.string();
    if (error) {
      message += ": " + error.message();
    }
    return make_error(ErrorCode::output_create_failed, 0, std::move(message), error);
  }
  return std::nullopt;
}

struct ExtractState {
  FILE* file = nullptr;
  uint64_t image_bytes = 0;
  const iso_file::Options* options = nullptr;
  iso_file::Progress progress;
  uint64_t next_progress_bytes = kProgressIntervalBytes;
};

size_t next_read_amount(const ExtractState& state, size_t buffer_size, uint64_t remaining) {
  auto amount = std::min<uint64_t>(buffer_size, remaining);
  if (state.options->on_progress && state.progress.bytes_completed < state.next_progress_bytes) {
    amount = std::min(amount, state.next_progress_bytes - state.progress.bytes_completed);
  }
  return static_cast<size_t>(amount);
}

void schedule_next_progress(ExtractState* state) {
  state->next_progress_bytes =
      state->progress.bytes_completed <=
              std::numeric_limits<uint64_t>::max() - kProgressIntervalBytes
          ? state->progress.bytes_completed + kProgressIntervalBytes
          : std::numeric_limits<uint64_t>::max();
}

void report_progress(ExtractState* state) {
  if (!state->options->on_progress) {
    return;
  }
  state->options->on_progress(state->progress);
  schedule_next_progress(state);
}

std::optional<Error> extract_entry(ExtractState* state,
                                   IsoFile::Entry* entry,
                                   const std::filesystem::path& destination,
                                   const std::string& relative_path,
                                   IsoFile* layout) {
  if (state->options->should_cancel && state->options->should_cancel()) {
    return make_error(ErrorCode::cancelled, entry->offset_in_file, "ISO extraction was cancelled.");
  }

  const auto name = output_name(entry->name);
  const auto output = destination / name;
  const auto child_path = relative_path.empty() ? name : relative_path + "/" + name;
  if (entry->is_dir) {
    if (auto error = ensure_output_directory(output)) {
      return error;
    }
    for (auto& child : entry->children) {
      if (auto error = extract_entry(state, &child, output, child_path, layout)) {
        return error;
      }
    }
    return std::nullopt;
  }

  errno = 0;
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream) {
    return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                             "Could not create output file: " + output.string(), errno);
  }

  XXH64_state_t hash_state;
  if (state->options->hash_files) {
    XXH64_reset(&hash_state, 0);
  }
  std::vector<uint8_t> buffer(
      std::min<size_t>(state->options->read_chunk_bytes, std::max<size_t>(entry->size, 1)));
  uint64_t copied = 0;
  state->progress.current_path = child_path;
  while (copied < entry->size) {
    if (state->options->should_cancel && state->options->should_cancel()) {
      return make_error(ErrorCode::cancelled, entry->offset_in_file + copied,
                        "ISO extraction was cancelled.");
    }
    const auto amount = next_read_amount(*state, buffer.size(), entry->size - copied);
    if (auto error = read_extent_chunk(state->file, state->image_bytes,
                                       entry->offset_in_file + copied, buffer.data(), amount)) {
      return error;
    }
    errno = 0;
    stream.write(reinterpret_cast<const char*>(buffer.data()),
                 static_cast<std::streamsize>(amount));
    if (!stream) {
      return make_system_error(ErrorCode::output_write_failed, entry->offset_in_file + copied,
                               "Could not write output file: " + output.string(), errno);
    }
    if (state->options->hash_files) {
      XXH64_update(&hash_state, buffer.data(), amount);
    }
    copied += amount;
    state->progress.bytes_completed += amount;
    if (copied < entry->size && state->progress.bytes_completed >= state->next_progress_bytes) {
      report_progress(state);
    }
  }
  errno = 0;
  stream.close();
  if (!stream) {
    return make_system_error(ErrorCode::output_write_failed, entry->offset_in_file + copied,
                             "Could not finish output file: " + output.string(), errno);
  }

  layout->files_extracted++;
  if (state->options->hash_files) {
    layout->hashes.push_back(XXH64_digest(&hash_state));
  }
  state->progress.files_completed++;
  report_progress(state);
  return std::nullopt;
}

}  // namespace

IsoFile::IsoFile() {
  root.is_dir = true;
}

std::string IsoFile::print() const {
  std::string result;
  root.print(&result, "");
  return result;
}

void IsoFile::Entry::print(std::string* result, const std::string& prefix) const {
  if (is_dir) {
    const auto child_prefix = prefix + "/" + name;
    for (const auto& child : children) {
      child.print(result, child_prefix);
    }
  } else {
    result->append(prefix);
    result->push_back('/');
    result->append(name);
    result->push_back('\n');
  }
}

namespace iso_file {

#ifndef _WIN32
struct OwnedStagingDirectory::Impl {
  enum class State {
    extracting,
    verified,
    finalization_required,
    finalizing,
    failed,
  };

  struct Entry {
    std::string name;
    bool is_directory = false;
    posix_file::Identity identity;
    std::vector<Entry> children;
    bool has_expected_contents = false;
    uint64_t expected_size = 0;
    uint64_t expected_hash = 0;
  };

  std::filesystem::path parent_path;
  std::string root_name;
  posix_file::OwnedFd parent;
  posix_file::Identity parent_identity;
  posix_file::OwnedFd root;
  posix_file::Identity root_identity;
  std::vector<Entry> entries;
  bool cleanup_pending = false;
  State state = State::extracting;

  static std::string system_error(std::string_view message) {
    return std::string(message) + ": " + std::strerror(errno);
  }

  static bool direct_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos && name.find('\\') == std::string_view::npos;
  }

  std::optional<Error> create(const std::filesystem::path& staging_directory) {
    root_name = staging_directory.filename().string();
    parent_path = staging_directory.parent_path();
    if (parent_path.empty()) {
      parent_path = ".";
    }
    if (!direct_name(root_name)) {
      return make_error(ErrorCode::invalid_argument, 0,
                        "The staging directory does not have a safe basename.");
    }
    parent = posix_file::open_directory(parent_path.c_str());
    if (!parent) {
      const int open_error = errno;
      return make_system_error(ErrorCode::output_create_failed, 0,
                               "Could not open the staging parent directory", open_error);
    }
    errno = 0;
    if (!posix_file::descriptor_identity(parent.get(), &parent_identity)) {
      const int identity_error = errno;
      return make_system_error(ErrorCode::output_create_failed, 0,
                               "Could not inspect the staging parent directory", identity_error);
    }
    if (::mkdirat(parent.get(), root_name.c_str(), 0700) != 0) {
      const int create_error = errno;
      const auto message = create_error == EEXIST
                               ? "The staging directory already exists and will not be overwritten"
                               : "Could not create the staging directory";
      return make_system_error(ErrorCode::output_create_failed, 0,
                               std::string(message) + ": " + staging_directory.string(),
                               create_error);
    }
    root = posix_file::open_directory_at(parent.get(), root_name);
    if (!root) {
      const int open_error = errno;
      return make_system_error(ErrorCode::output_create_failed, 0,
                               "Could not open the newly created staging directory", open_error);
    }
    errno = 0;
    if (!posix_file::descriptor_identity(root.get(), &root_identity)) {
      const int identity_error = errno;
      return make_system_error(ErrorCode::output_create_failed, 0,
                               "Could not inspect the newly created staging directory",
                               identity_error);
    }
    errno = 0;
    if (!posix_file::entry_identity(parent.get(), root_name, root_identity)) {
      const int identity_error = errno;
      return make_system_error(ErrorCode::output_create_failed, 0,
                               "Could not retain the exact newly created staging directory",
                               identity_error);
    }
    cleanup_pending = true;
    return {};
  }

  std::optional<Error> extract_entry(ExtractState* state,
                                     IsoFile::Entry* entry,
                                     int parent_descriptor,
                                     std::vector<Entry>* owned_entries,
                                     const std::string& relative_path,
                                     IsoFile* layout) {
    if (state->options->should_cancel && state->options->should_cancel()) {
      return make_error(ErrorCode::cancelled, entry->offset_in_file,
                        "ISO extraction was cancelled.");
    }

    const auto name = output_name(entry->name);
    const auto child_path = relative_path.empty() ? name : relative_path + "/" + name;
    if (entry->is_dir) {
      if (::mkdirat(parent_descriptor, name.c_str(), 0700) != 0) {
        const int create_error = errno;
        return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                                 "Could not exclusively create output directory " + child_path,
                                 create_error);
      }
      auto child = posix_file::open_directory_at(parent_descriptor, name);
      if (!child) {
        const int open_error = errno;
        return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                                 "Could not open created output directory " + child_path,
                                 open_error);
      }
      posix_file::Identity identity;
      errno = 0;
      if (!posix_file::descriptor_identity(child.get(), &identity)) {
        const int identity_error = errno;
        return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                                 "Could not inspect created output directory " + child_path,
                                 identity_error);
      }
      errno = 0;
      if (!posix_file::entry_identity(parent_descriptor, name, identity)) {
        const int identity_error = errno;
        return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                                 "Could not retain the exact created output directory " +
                                     child_path,
                                 identity_error);
      }
      auto& owned = owned_entries->emplace_back(Entry{name, true, identity, {}, false, 0, 0});
      for (auto& nested : entry->children) {
        if (auto error =
                extract_entry(state, &nested, child.get(), &owned.children, child_path, layout)) {
          return error;
        }
      }
      return {};
    }

    auto output =
        posix_file::open_file_at(parent_descriptor, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!output) {
      const int open_error = errno;
      return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                               "Could not exclusively create output file " + child_path,
                               open_error);
    }
    posix_file::Identity identity;
    struct stat output_status{};
    errno = 0;
    if (!posix_file::descriptor_identity(output.get(), &identity, &output_status)) {
      const int identity_error = errno;
      return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                               "Could not inspect created output file " + child_path,
                               identity_error);
    }
    if (!S_ISREG(output_status.st_mode)) {
      return make_error(ErrorCode::output_create_failed, entry->offset_in_file,
                        "The created output is not a regular file: " + child_path);
    }
    errno = 0;
    if (!posix_file::entry_identity(parent_descriptor, name, identity)) {
      const int identity_error = errno;
      return make_system_error(ErrorCode::output_create_failed, entry->offset_in_file,
                               "Could not retain the exact created output file " + child_path,
                               identity_error);
    }
    auto& owned = owned_entries->emplace_back(Entry{name, false, identity, {}, false, 0, 0});

    XXH64_state_t hash_state;
    XXH64_reset(&hash_state, 0);
    std::vector<uint8_t> buffer(
        std::min<size_t>(state->options->read_chunk_bytes, std::max<size_t>(entry->size, 1)));
    uint64_t copied = 0;
    state->progress.current_path = child_path;
    while (copied < entry->size) {
      if (state->options->should_cancel && state->options->should_cancel()) {
        return make_error(ErrorCode::cancelled, entry->offset_in_file + copied,
                          "ISO extraction was cancelled.");
      }
      const auto amount = next_read_amount(*state, buffer.size(), entry->size - copied);
      if (auto error = read_extent_chunk(state->file, state->image_bytes,
                                         entry->offset_in_file + copied, buffer.data(), amount)) {
        return error;
      }
      size_t written = 0;
      while (written < amount) {
        const auto result = ::write(output.get(), buffer.data() + written, amount - written);
        if (result < 0 && errno == EINTR) {
          continue;
        }
        if (result < 0) {
          const int write_error = errno;
          return make_system_error(ErrorCode::output_write_failed,
                                   entry->offset_in_file + copied,
                                   "Could not write output file " + child_path, write_error);
        }
        if (result == 0) {
          return make_error(ErrorCode::output_write_failed, entry->offset_in_file + copied,
                            "Writing output file made no progress: " + child_path);
        }
        written += static_cast<size_t>(result);
      }
      XXH64_update(&hash_state, buffer.data(), amount);
      copied += amount;
      state->progress.bytes_completed += amount;
      if (copied < entry->size && state->progress.bytes_completed >= state->next_progress_bytes) {
        report_progress(state);
      }
    }

    const auto hash = XXH64_digest(&hash_state);
    owned.has_expected_contents = true;
    owned.expected_size = entry->size;
    owned.expected_hash = hash;
    layout->files_extracted++;
    if (state->options->hash_files) {
      layout->hashes.push_back(hash);
    }
    state->progress.files_completed++;
    report_progress(state);
    return {};
  }

  static bool verify_entry_contents(int parent_descriptor,
                                    const Entry& entry,
                                    std::vector<uint8_t>* buffer) {
    struct stat path_status{};
    if (!posix_file::entry_identity(parent_descriptor, entry.name, entry.identity, &path_status)) {
      return false;
    }
    if (entry.is_directory) {
      if (!S_ISDIR(path_status.st_mode) || entry.has_expected_contents) {
        return false;
      }
      auto directory = posix_file::open_directory_at(parent_descriptor, entry.name);
      if (!directory ||
          !posix_file::entry_identity(parent_descriptor, entry.name, entry.identity)) {
        return false;
      }
      for (const auto& child : entry.children) {
        if (!verify_entry_contents(directory.get(), child, buffer)) {
          return false;
        }
      }
      return posix_file::entry_identity(parent_descriptor, entry.name, entry.identity);
    }

    if (!entry.has_expected_contents || !S_ISREG(path_status.st_mode) || path_status.st_size < 0 ||
        static_cast<uint64_t>(path_status.st_size) != entry.expected_size) {
      return false;
    }
    auto file = posix_file::open_file_at(parent_descriptor, entry.name, O_RDONLY);
    posix_file::Identity descriptor_identity;
    struct stat descriptor_status{};
    if (!file ||
        !posix_file::descriptor_identity(file.get(), &descriptor_identity, &descriptor_status) ||
        descriptor_identity.device != entry.identity.device ||
        descriptor_identity.inode != entry.identity.inode || !S_ISREG(descriptor_status.st_mode) ||
        descriptor_status.st_size < 0 ||
        static_cast<uint64_t>(descriptor_status.st_size) != entry.expected_size ||
        !posix_file::entry_identity(parent_descriptor, entry.name, entry.identity)) {
      return false;
    }

    XXH64_state_t hash_state;
    XXH64_reset(&hash_state, 0);
    uint64_t read_bytes = 0;
    while (true) {
      const auto remaining = entry.expected_size - read_bytes;
      const auto amount =
          static_cast<size_t>(std::min<uint64_t>(buffer->size(), remaining == 0 ? 1 : remaining));
      const auto result = ::read(file.get(), buffer->data(), amount);
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result < 0) {
        return false;
      }
      if (result == 0) {
        break;
      }
      const auto count = static_cast<size_t>(result);
      if (count > remaining) {
        return false;
      }
      XXH64_update(&hash_state, buffer->data(), count);
      read_bytes += count;
    }

    struct stat final_status{};
    return read_bytes == entry.expected_size && XXH64_digest(&hash_state) == entry.expected_hash &&
           posix_file::descriptor_identity(file.get(), &descriptor_identity, &final_status) &&
           descriptor_identity.device == entry.identity.device &&
           descriptor_identity.inode == entry.identity.inode && S_ISREG(final_status.st_mode) &&
           final_status.st_size >= 0 &&
           static_cast<uint64_t>(final_status.st_size) == entry.expected_size &&
           posix_file::entry_identity(parent_descriptor, entry.name, entry.identity);
  }

  bool verify_recorded_contents() const {
    posix_file::Identity descriptor_identity;
    if (!cleanup_pending || !root ||
        !posix_file::descriptor_identity(root.get(), &descriptor_identity) ||
        descriptor_identity.device != root_identity.device ||
        descriptor_identity.inode != root_identity.inode) {
      return false;
    }
    std::vector<uint8_t> buffer(256 * 1024);
    return std::all_of(entries.begin(), entries.end(), [&](const Entry& entry) {
      return verify_entry_contents(root.get(), entry, &buffer);
    });
  }

  static std::optional<std::string> cleanup_entry(int parent_descriptor, Entry* entry) {
    struct stat status{};
    if (::fstatat(parent_descriptor, entry->name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return {};
      }
      return system_error("Could not inspect an importer-created staging entry");
    }
    if (!posix_file::same_identity(status, entry->identity)) {
      return "An importer-created staging entry was replaced and was preserved: " + entry->name;
    }
    if (!entry->is_directory) {
      if (::unlinkat(parent_descriptor, entry->name.c_str(), 0) != 0) {
        return system_error("Could not remove an exact importer-created staging file");
      }
      return {};
    }

    auto directory = posix_file::open_directory_at(parent_descriptor, entry->name);
    if (!directory ||
        !posix_file::entry_identity(parent_descriptor, entry->name, entry->identity)) {
      return "An importer-created staging directory changed and was preserved: " + entry->name;
    }
    std::optional<std::string> first_error;
    for (auto& child : entry->children) {
      if (auto error = cleanup_entry(directory.get(), &child); error && !first_error) {
        first_error = std::move(error);
      }
    }
    if (!posix_file::entry_identity(parent_descriptor, entry->name, entry->identity)) {
      if (!first_error) {
        first_error =
            "An importer-created staging directory changed during cleanup and was preserved: " +
            entry->name;
      }
      return first_error;
    }
    if (::unlinkat(parent_descriptor, entry->name.c_str(), AT_REMOVEDIR) != 0 && !first_error) {
      first_error = system_error(
          "Could not remove an exact importer-created staging directory; unexpected entries were "
          "preserved");
    }
    return first_error;
  }

  std::optional<std::string> cleanup() {
    if (!cleanup_pending) {
      return {};
    }
    cleanup_pending = false;
    std::optional<std::string> first_error;
    for (auto& entry : entries) {
      if (auto error = cleanup_entry(root.get(), &entry); error && !first_error) {
        first_error = std::move(error);
      }
    }
    if (!posix_file::entry_identity(parent.get(), root_name, root_identity)) {
      if (!first_error) {
        first_error =
            "The importer-created staging directory pathname changed; the current path "
            "was preserved.";
      }
      return first_error;
    }
    if (::unlinkat(parent.get(), root_name.c_str(), AT_REMOVEDIR) != 0 && !first_error) {
      first_error = system_error(
          "Could not remove the exact importer-created staging directory; unexpected entries were "
          "preserved");
    }
    return first_error;
  }

  static bool exact_entries(int directory, const std::vector<Entry>& expected) {
    auto enumeration = posix_file::open_directory_at(directory, ".");
    if (!enumeration) {
      return false;
    }
    DIR* stream = ::fdopendir(enumeration.release());
    if (!stream) {
      return false;
    }
    size_t observed = 0;
    while (true) {
      errno = 0;
      const auto* raw_entry = ::readdir(stream);
      if (!raw_entry) {
        const auto read_error = errno;
        ::closedir(stream);
        return read_error == 0 && observed == expected.size();
      }
      const std::string_view name(raw_entry->d_name);
      if (name == "." || name == "..") {
        continue;
      }
      if (++observed > expected.size()) {
        ::closedir(stream);
        return false;
      }
      const auto found = std::find_if(expected.begin(), expected.end(),
                                      [&](const Entry& entry) { return entry.name == name; });
      struct stat status{};
      if (found == expected.end() ||
          !posix_file::entry_identity(directory, name, found->identity, &status) ||
          (found->is_directory != bool(S_ISDIR(status.st_mode)))) {
        ::closedir(stream);
        return false;
      }
      if (found->is_directory) {
        auto child = posix_file::open_directory_at(directory, name);
        if (!child || !posix_file::entry_identity(directory, name, found->identity) ||
            !exact_entries(child.get(), found->children)) {
          ::closedir(stream);
          return false;
        }
      }
    }
  }

  bool is_linked() const {
    auto current_parent = posix_file::open_directory(parent_path.c_str());
    posix_file::Identity descriptor_parent_identity;
    posix_file::Identity current_parent_identity;
    posix_file::Identity descriptor_identity;
    return cleanup_pending && parent && root && current_parent &&
           posix_file::descriptor_identity(parent.get(), &descriptor_parent_identity) &&
           descriptor_parent_identity.device == parent_identity.device &&
           descriptor_parent_identity.inode == parent_identity.inode &&
           posix_file::descriptor_identity(current_parent.get(), &current_parent_identity) &&
           current_parent_identity.device == parent_identity.device &&
           current_parent_identity.inode == parent_identity.inode &&
           posix_file::descriptor_identity(root.get(), &descriptor_identity) &&
           descriptor_identity.device == root_identity.device &&
           descriptor_identity.inode == root_identity.inode &&
           posix_file::entry_identity(parent.get(), root_name, root_identity) &&
           posix_file::entry_identity(current_parent.get(), root_name, root_identity) &&
           exact_entries(root.get(), entries);
  }

  bool track_created_file(std::string_view name, int descriptor) {
    if (!cleanup_pending || !direct_name(name) || descriptor < 0 ||
        std::any_of(
            entries.begin(), entries.end(),
            [&](const Entry& entry) { return entry.name == name; })) {
      return false;
    }
    posix_file::Identity identity;
    struct stat status{};
    if (!posix_file::descriptor_identity(descriptor, &identity, &status) ||
        !S_ISREG(status.st_mode) || !posix_file::entry_identity(root.get(), name, identity)) {
      return false;
    }
    entries.emplace_back(Entry{std::string(name), false, identity, {}, false, 0, 0});
    return true;
  }

  bool rename_tracked_file(std::string_view old_name, std::string_view new_name) {
    if (!cleanup_pending || !direct_name(old_name) || !direct_name(new_name)) {
      return false;
    }
    auto found = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) {
      return !entry.is_directory && entry.name == old_name;
    });
    if (found == entries.end() ||
        std::any_of(entries.begin(), entries.end(),
                    [&](const Entry& entry) { return entry.name == new_name; }) ||
        !posix_file::entry_identity(root.get(), old_name, found->identity) ||
        posix_file::exclusive_rename_at(root.get(), old_name, root.get(), new_name) != 0 ||
        !posix_file::entry_identity(root.get(), new_name, found->identity)) {
      return false;
    }
    found->name = new_name;
    return true;
  }
};

OwnedStagingDirectory::OwnedStagingDirectory() = default;

OwnedStagingDirectory::~OwnedStagingDirectory() {
  (void)cleanup();
}

OwnedStagingDirectory::OwnedStagingDirectory(OwnedStagingDirectory&&) noexcept = default;

OwnedStagingDirectory& OwnedStagingDirectory::operator=(OwnedStagingDirectory&& other) noexcept {
  if (this != &other) {
    (void)cleanup();
    m_impl = std::move(other.m_impl);
  }
  return *this;
}

std::optional<std::string> OwnedStagingDirectory::cleanup() {
  if (!m_impl) {
    return {};
  }
  auto error = m_impl->cleanup();
  m_impl.reset();
  return error;
}

bool OwnedStagingDirectory::keep() {
  if (!m_impl || m_impl->state != Impl::State::verified || !m_impl->is_linked()) {
    return false;
  }
  m_impl->cleanup_pending = false;
  m_impl.reset();
  return true;
}

OwnedStagingFinalizationResult OwnedStagingDirectory::finalize_and_keep(
    const std::function<bool()>& checkpoint) {
  if (!m_impl || m_impl->state != Impl::State::finalization_required || !checkpoint) {
    return OwnedStagingFinalizationResult::unavailable;
  }
  m_impl->state = Impl::State::finalizing;
  bool callback_succeeded = false;
  try {
    callback_succeeded = checkpoint();
  } catch (...) {
    m_impl->state = Impl::State::failed;
    return OwnedStagingFinalizationResult::callback_failed;
  }
  if (!callback_succeeded) {
    m_impl->state = Impl::State::failed;
    return OwnedStagingFinalizationResult::callback_failed;
  }
  if (!m_impl->is_linked()) {
    m_impl->state = Impl::State::failed;
    return OwnedStagingFinalizationResult::staging_changed;
  }

  m_impl->cleanup_pending = false;
  m_impl.reset();
  return OwnedStagingFinalizationResult::success;
}

bool OwnedStagingDirectory::is_linked() const {
  return m_impl && m_impl->is_linked();
}

bool OwnedStagingDirectory::verify_recorded_contents() const {
  return m_impl && m_impl->verify_recorded_contents();
}

int OwnedStagingDirectory::directory_descriptor() const {
  return m_impl ? m_impl->root.get() : -1;
}

bool OwnedStagingDirectory::track_created_file(std::string_view name, int descriptor) {
  return m_impl && m_impl->track_created_file(name, descriptor);
}

bool OwnedStagingDirectory::rename_tracked_file(std::string_view old_name,
                                                std::string_view new_name) {
  return m_impl && m_impl->rename_tracked_file(old_name, new_name);
}
#endif

std::string extracted_output_name(std::string_view entry_name) {
  return output_name(std::string(entry_name));
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::seek_failed:
      return "seek_failed";
    case ErrorCode::read_failed:
      return "read_failed";
    case ErrorCode::invalid_descriptor:
      return "invalid_descriptor";
    case ErrorCode::unsupported_format:
      return "unsupported_format";
    case ErrorCode::invalid_directory_record:
      return "invalid_directory_record";
    case ErrorCode::unsafe_path:
      return "unsafe_path";
    case ErrorCode::extent_out_of_bounds:
      return "extent_out_of_bounds";
    case ErrorCode::depth_limit_exceeded:
      return "depth_limit_exceeded";
    case ErrorCode::entry_limit_exceeded:
      return "entry_limit_exceeded";
    case ErrorCode::file_size_limit_exceeded:
      return "file_size_limit_exceeded";
    case ErrorCode::total_size_limit_exceeded:
      return "total_size_limit_exceeded";
    case ErrorCode::output_create_failed:
      return "output_create_failed";
    case ErrorCode::output_write_failed:
      return "output_write_failed";
  }
  return "unknown";
}

Exception::Exception(Error error)
    : std::runtime_error(std::string(error_code_name(error.code)) + ": " + error.message),
      m_error(std::move(error)) {}

Result<IsoFile> inspect(FILE* file, const Options& options) {
  uint64_t bytes = 0;
  if (auto error = validate_options(file, options, &bytes)) {
    return Result<IsoFile>::failure(std::move(*error));
  }
  if (options.should_cancel && options.should_cancel()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::cancelled, 0, "ISO inspection was cancelled."));
  }

  std::array<uint8_t, kSectorSize> descriptor{};
  bool found_primary = false;
  bool found_terminator = false;
  uint64_t volume_bytes = 0;
  ParsedRecord root_record;
  for (uint32_t index = 0; index < kMaxDescriptorSectors; ++index) {
    const auto sector = kFirstDescriptorSector + index;
    const auto offset = uint64_t(sector) * kSectorSize;
    if (options.should_cancel && options.should_cancel()) {
      return Result<IsoFile>::failure(
          make_error(ErrorCode::cancelled, offset, "ISO inspection was cancelled."));
    }
    if (auto error = read_exact(file, bytes, offset, descriptor.data(), descriptor.size())) {
      return Result<IsoFile>::failure(std::move(*error));
    }
    if (memcmp(descriptor.data() + 1, "CD001", 5) || descriptor[6] != 1) {
      return Result<IsoFile>::failure(make_error(ErrorCode::invalid_descriptor, offset,
                                                 "An ISO9660 volume descriptor is invalid."));
    }
    if (descriptor[0] == 255) {
      found_terminator = true;
      break;
    }
    if (descriptor[0] != 1 || found_primary) {
      continue;
    }

    uint16_t logical_block_size = 0;
    if (auto error = read_both16(descriptor.data() + 128, offset + 128, &logical_block_size)) {
      return Result<IsoFile>::failure(std::move(*error));
    }
    if (logical_block_size != kSectorSize) {
      return Result<IsoFile>::failure(
          make_error(ErrorCode::unsupported_format, offset + 128,
                     "Only ISO9660 images with 2048-byte logical blocks are supported."));
    }
    uint32_t volume_sectors = 0;
    if (auto error = read_both32(descriptor.data() + 80, offset + 80, &volume_sectors)) {
      return Result<IsoFile>::failure(std::move(*error));
    }
    if (!volume_sectors || !checked_multiply(volume_sectors, logical_block_size, &volume_bytes) ||
        volume_bytes > bytes || volume_bytes > options.max_image_bytes) {
      return Result<IsoFile>::failure(
          make_error(ErrorCode::extent_out_of_bounds, offset + 80,
                     "The ISO9660 volume size lies outside the image or configured limit."));
    }
    if (auto error = parse_record(descriptor.data() + 156, descriptor.size() - 156, offset + 156,
                                  options, &root_record)) {
      return Result<IsoFile>::failure(std::move(*error));
    }
    if (!root_record.is_directory || !root_record.is_dot) {
      return Result<IsoFile>::failure(
          make_error(ErrorCode::invalid_descriptor, offset + 156,
                     "The ISO9660 primary descriptor has an invalid root directory record."));
    }
    found_primary = true;
  }
  if (!found_primary || !found_terminator) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::invalid_descriptor, uint64_t(kFirstDescriptorSector) * kSectorSize,
                   "The ISO9660 primary descriptor or descriptor terminator is missing."));
  }

  IsoFile layout;
  ParseState state{file, &options, bytes, volume_bytes, 0, 0, {}};
  if (auto error = parse_directory(&state, root_record.extent_sector, root_record.data_size, 0, "",
                                   &layout.root)) {
    return Result<IsoFile>::failure(std::move(*error));
  }
  return Result<IsoFile>::success(std::move(layout));
}

Result<IsoFile> extract_layout(FILE* file,
                               IsoFile layout,
                               const std::filesystem::path& destination,
                               const Options& options) {
  uint64_t bytes = 0;
  if (auto error = validate_options(file, options, &bytes)) {
    return Result<IsoFile>::failure(std::move(*error));
  }
  if (destination.empty()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The extraction destination is empty."));
  }

  LayoutStats stats;
  std::unordered_set<std::string> root_names;
  for (const auto& entry : layout.root.children) {
    if (!root_names.emplace(collision_key(entry.name)).second) {
      return Result<IsoFile>::failure(
          make_error(ErrorCode::unsafe_path, entry.offset_in_file,
                     "The ISO contains duplicate output names in its root directory."));
    }
    if (auto error = validate_layout_entry(entry, "", 0, bytes, options, &stats)) {
      return Result<IsoFile>::failure(std::move(*error));
    }
  }
  if (auto error = ensure_output_directory(destination)) {
    return Result<IsoFile>::failure(std::move(*error));
  }

  layout.files_extracted = 0;
  layout.shouldHash = options.hash_files;
  layout.hashes.clear();
  ExtractState state{file, bytes, &options, {}, kProgressIntervalBytes};
  state.progress.bytes_total = stats.total_bytes;
  state.progress.files_total = stats.file_count;
  if (options.on_progress) {
    options.on_progress(state.progress);
  }
  for (auto& entry : layout.root.children) {
    if (auto error = extract_entry(&state, &entry, destination, "", &layout)) {
      return Result<IsoFile>::failure(std::move(*error));
    }
  }
  if (options.should_cancel && options.should_cancel()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::cancelled, bytes, "ISO extraction was cancelled."));
  }
  return Result<IsoFile>::success(std::move(layout));
}

Result<IsoFile> extract_to_staging(FILE* file,
                                   const std::filesystem::path& staging_directory,
                                   const Options& options) {
  if (staging_directory.empty()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The staging directory is empty."));
  }
  std::error_code fs_error;
  const auto staging_exists = std::filesystem::exists(staging_directory, fs_error);
  if (fs_error) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::output_create_failed, 0,
                   "Could not inspect staging directory: " + staging_directory.string() + ": " +
                       fs_error.message(),
                   fs_error));
  }
  if (staging_exists) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::output_create_failed, 0,
                   "The staging directory already exists and will not be overwritten: " +
                       staging_directory.string()));
  }

  auto inspected = inspect(file, options);
  if (!inspected) {
    return Result<IsoFile>::failure(inspected.error());
  }
  if (!std::filesystem::create_directory(staging_directory, fs_error) || fs_error) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::output_create_failed, 0,
                   "Could not create staging directory: " + staging_directory.string() +
                       (fs_error ? ": " + fs_error.message() : std::string{}),
                   fs_error));
  }

  auto extracted = extract_layout(file, inspected.take_value(), staging_directory, options);
  if (!extracted) {
    auto extraction_error = extracted.error();
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging_directory, cleanup_error);
    if (cleanup_error) {
      extraction_error.message +=
          " The staging directory could not be removed: " + cleanup_error.message();
    }
    return Result<IsoFile>::failure(std::move(extraction_error));
  }
  return extracted;
}

#ifndef _WIN32
struct OwnedStagingDirectoryAccess {
  static Result<IsoFile> extract(FILE* file,
                                 const std::filesystem::path& staging_directory,
                                 OwnedStagingDirectory* owned_staging,
                                 const Options& options,
                                 bool finalization_required);
};

Result<IsoFile> OwnedStagingDirectoryAccess::extract(FILE* file,
                                                     const std::filesystem::path& staging_directory,
                                                     OwnedStagingDirectory* owned_staging,
                                                     const Options& options,
                                                     bool finalization_required) {
  if (staging_directory.empty()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The staging directory is empty."));
  }
  if (!owned_staging) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::invalid_argument, 0, "The owned staging handle is null."));
  }

  if (auto prior_cleanup = owned_staging->cleanup()) {
    return Result<IsoFile>::failure(
        make_error(ErrorCode::output_create_failed, 0,
                   "Could not release the prior owned staging directory: " + *prior_cleanup));
  }
  owned_staging->m_impl.reset(new OwnedStagingDirectory::Impl());
  if (auto create_error = owned_staging->m_impl->create(staging_directory)) {
    owned_staging->m_impl.reset();
    return Result<IsoFile>::failure(std::move(*create_error));
  }
  auto inspected = inspect(file, options);
  if (!inspected) {
    auto inspection_error = inspected.error();
    if (auto cleanup_error = owned_staging->cleanup()) {
      inspection_error.message +=
          " The staging directory could not be removed safely: " + *cleanup_error;
    }
    return Result<IsoFile>::failure(std::move(inspection_error));
  }

  uint64_t bytes = 0;
  if (auto error = validate_options(file, options, &bytes)) {
    auto cleanup_error = owned_staging->cleanup();
    if (cleanup_error) {
      error->message += " The staging directory could not be removed safely: " + *cleanup_error;
    }
    return Result<IsoFile>::failure(std::move(*error));
  }
  auto layout = inspected.take_value();
  LayoutStats stats;
  std::unordered_set<std::string> root_names;
  for (const auto& entry : layout.root.children) {
    if (!root_names.emplace(collision_key(entry.name)).second) {
      auto error = make_error(ErrorCode::unsafe_path, entry.offset_in_file,
                              "The ISO contains duplicate output names in its root directory.");
      if (auto cleanup_error = owned_staging->cleanup()) {
        error.message += " The staging directory could not be removed safely: " + *cleanup_error;
      }
      return Result<IsoFile>::failure(std::move(error));
    }
    if (auto error = validate_layout_entry(entry, "", 0, bytes, options, &stats)) {
      if (auto cleanup_error = owned_staging->cleanup()) {
        error->message += " The staging directory could not be removed safely: " + *cleanup_error;
      }
      return Result<IsoFile>::failure(std::move(*error));
    }
  }

  layout.files_extracted = 0;
  layout.shouldHash = options.hash_files;
  layout.hashes.clear();
  ExtractState state{file, bytes, &options, {}, kProgressIntervalBytes};
  state.progress.bytes_total = stats.total_bytes;
  state.progress.files_total = stats.file_count;
  if (options.on_progress) {
    options.on_progress(state.progress);
  }
  for (auto& entry : layout.root.children) {
    if (auto error =
            owned_staging->m_impl->extract_entry(&state, &entry, owned_staging->m_impl->root.get(),
                                                 &owned_staging->m_impl->entries, "", &layout)) {
      if (auto cleanup_error = owned_staging->cleanup()) {
        error->message += " The staging directory could not be removed safely: " + *cleanup_error;
      }
      return Result<IsoFile>::failure(std::move(*error));
    }
  }
  if (owned_staging->m_impl->is_linked() && options.should_cancel && options.should_cancel()) {
    auto error = make_error(ErrorCode::cancelled, bytes, "ISO extraction was cancelled.");
    if (auto cleanup_error = owned_staging->cleanup()) {
      error.message += " The staging directory could not be removed safely: " + *cleanup_error;
    }
    return Result<IsoFile>::failure(std::move(error));
  }
  if (finalization_required) {
    owned_staging->m_impl->state = OwnedStagingDirectory::Impl::State::finalization_required;
    return Result<IsoFile>::success(std::move(layout));
  }
  if (!owned_staging->m_impl->verify_recorded_contents()) {
    auto error =
        make_error(ErrorCode::output_write_failed, 0,
                   "A reader-created staging file changed after the final extraction callback.");
    if (auto cleanup_error = owned_staging->cleanup()) {
      error.message += " The staging directory could not be removed safely: " + *cleanup_error;
    }
    return Result<IsoFile>::failure(std::move(error));
  }
  owned_staging->m_impl->state = OwnedStagingDirectory::Impl::State::verified;
  return Result<IsoFile>::success(std::move(layout));
}

Result<IsoFile> extract_to_owned_staging(FILE* file,
                                         const std::filesystem::path& staging_directory,
                                         OwnedStagingDirectory* owned_staging,
                                         const Options& options) {
  return OwnedStagingDirectoryAccess::extract(file, staging_directory, owned_staging, options,
                                              false);
}

Result<IsoFile> extract_to_owned_staging_for_finalization(
    FILE* file,
    const std::filesystem::path& staging_directory,
    OwnedStagingDirectory* owned_staging,
    const Options& options) {
  return OwnedStagingDirectoryAccess::extract(file, staging_directory, owned_staging, options,
                                              true);
}
#endif

}  // namespace iso_file

IsoFile find_files_in_iso(FILE* fp) {
  auto result = iso_file::inspect(fp);
  if (!result) {
    throw iso_file::Exception(result.error());
  }
  return result.take_value();
}

void unpack_iso_files(FILE* fp, IsoFile& layout, const fs::path& dest) {
  unpack_iso_files(fp, layout, dest, false);
}

void unpack_iso_files(FILE* fp, IsoFile& layout, const fs::path& dest, bool print_progress) {
  iso_file::Options options;
  options.hash_files = layout.shouldHash;
  std::string last_path;
  if (print_progress) {
    options.on_progress = [&last_path](const iso_file::Progress& progress) {
      if (!progress.current_path.empty() && progress.current_path != last_path) {
        fprintf(stdout, "Extracting %s...\n", progress.current_path.c_str());
        last_path = progress.current_path;
      }
    };
  }
  auto result = iso_file::extract_layout(fp, layout, standard_path(dest), options);
  if (!result) {
    throw iso_file::Exception(result.error());
  }
  layout = result.take_value();
}

IsoFile unpack_iso_files(FILE* fp,
                         const fs::path& dest,
                         bool print_progress,
                         const bool hashFiles) {
  auto layout = find_files_in_iso(fp);
  layout.shouldHash = hashFiles;
  unpack_iso_files(fp, layout, dest, print_progress);
  return layout;
}
