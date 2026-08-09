#include "jak1_checked_dgo_writer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <unistd.h>
#include <unordered_set>

#include "common/util/PosixFile.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_checked_dgo_writer {
namespace {

constexpr std::size_t kHeaderBytes = 64;
constexpr std::size_t kNameFieldBytes = 60;
constexpr std::size_t kObjectAlignment = 16;

struct PreparedArchive {
  WriteSummary summary;
};

Error make_error(ErrorCode code,
                 std::string message,
                 std::optional<std::uint32_t> object_index = {}) {
  return {code, object_index, std::move(message)};
}

std::optional<Error> check_cancelled(const Options& options,
                                     std::optional<std::uint32_t> object_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, "DGO writing was cancelled.", object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The DGO cancellation callback failed.",
                      object_index);
  }
  return {};
}

std::optional<Error> report_progress(const Options& options,
                                     ProgressPhase phase,
                                     std::uint32_t objects_completed,
                                     std::uint32_t object_count,
                                     std::size_t bytes_completed,
                                     std::size_t total_bytes,
                                     std::optional<std::uint32_t> object_index = {}) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress({phase, objects_completed, object_count, bytes_completed, total_bytes});
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The DGO progress callback failed.",
                      object_index);
  }
  return {};
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
  return remainder == 0 ? (*result = value, true)
                        : checked_add(value, alignment - remainder, result);
}

std::optional<Error> validate_options(const Options& options) {
  if (options.max_name_bytes == 0 || options.max_name_bytes >= kNameFieldBytes ||
      options.max_objects == 0 || options.max_object_bytes == 0 ||
      options.max_total_object_bytes == 0 || options.max_output_bytes < kHeaderBytes ||
      options.write_chunk_bytes == 0 ||
      options.write_chunk_bytes > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())) {
    return make_error(ErrorCode::invalid_argument, "The DGO writer options are invalid.");
  }
  return {};
}

std::optional<Error> validate_name(std::string_view name,
                                   const Options& options,
                                   std::optional<std::uint32_t> object_index = {}) {
  if (name.empty() || name.size() > options.max_name_bytes) {
    return make_error(ErrorCode::invalid_name,
                      "A DGO name is empty or exceeds the configured name limit.", object_index);
  }
  for (const auto character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte < 0x20 || byte > 0x7e || character == '/' || character == '\\' || character == ':') {
      return make_error(ErrorCode::invalid_name,
                        "A DGO name contains a path separator, control byte, or non-ASCII byte.",
                        object_index);
    }
  }
  return {};
}

Result<PreparedArchive> prepare(std::string_view archive_name,
                                std::span<const ObjectRecord> objects,
                                const Options& options) {
  if (const auto error = validate_options(options)) {
    return Result<PreparedArchive>::failure(*error);
  }
  if (const auto error = check_cancelled(options)) {
    return Result<PreparedArchive>::failure(*error);
  }
  if (const auto error = validate_name(archive_name, options)) {
    return Result<PreparedArchive>::failure(*error);
  }
  if (objects.empty()) {
    return Result<PreparedArchive>::failure(
        make_error(ErrorCode::empty_archive, "A DGO must contain at least one object."));
  }
  if (objects.size() > options.max_objects ||
      objects.size() > std::numeric_limits<std::uint32_t>::max()) {
    return Result<PreparedArchive>::failure(
        make_error(ErrorCode::object_count_limit_exceeded,
                   "The DGO object count exceeds the configured or format limit."));
  }

  const auto object_count = static_cast<std::uint32_t>(objects.size());
  std::unordered_set<std::string_view> names;
  if (options.duplicate_name_policy == DuplicateNamePolicy::reject) {
    try {
      names.reserve(objects.size());
    } catch (const std::bad_alloc&) {
      return Result<PreparedArchive>::failure(make_error(
          ErrorCode::allocation_failed, "Could not allocate the DGO duplicate-name table."));
    }
  }

  std::size_t total_object_bytes = 0;
  std::size_t output_bytes = kHeaderBytes;
  for (std::uint32_t index = 0; index < object_count; ++index) {
    if (const auto error = check_cancelled(options, index)) {
      return Result<PreparedArchive>::failure(*error);
    }
    const auto& object = objects[index];
    if (const auto error = validate_name(object.internal_name, options, index)) {
      return Result<PreparedArchive>::failure(*error);
    }
    if (object.data.empty() || object.data.data() == nullptr ||
        object.data.size() > options.max_object_bytes ||
        object.data.size() > std::numeric_limits<std::uint32_t>::max()) {
      return Result<PreparedArchive>::failure(
          make_error(ErrorCode::object_size_limit_exceeded,
                     "A DGO object is empty or exceeds the configured or format limit.", index));
    }
    if (options.duplicate_name_policy == DuplicateNamePolicy::reject) {
      try {
        if (!names.insert(object.internal_name).second) {
          return Result<PreparedArchive>::failure(
              make_error(ErrorCode::duplicate_object_name,
                         "The DGO contains a duplicate object name.", index));
        }
      } catch (const std::bad_alloc&) {
        return Result<PreparedArchive>::failure(make_error(
            ErrorCode::allocation_failed, "Could not extend the DGO duplicate-name table.", index));
      }
    }

    if (!checked_add(total_object_bytes, object.data.size(), &total_object_bytes) ||
        total_object_bytes > options.max_total_object_bytes) {
      return Result<PreparedArchive>::failure(
          make_error(ErrorCode::total_object_size_limit_exceeded,
                     "The DGO object payloads exceed the configured total-size limit.", index));
    }

    std::size_t padded_size = 0;
    std::size_t record_size = 0;
    if (!align_up(object.data.size(), kObjectAlignment, &padded_size) ||
        !checked_add(kHeaderBytes, padded_size, &record_size) ||
        !checked_add(output_bytes, record_size, &output_bytes) ||
        output_bytes > options.max_output_bytes) {
      return Result<PreparedArchive>::failure(make_error(
          ErrorCode::output_size_limit_exceeded,
          "The encoded DGO exceeds the configured or addressable output-size limit.", index));
    }
    if (const auto error = report_progress(options, ProgressPhase::validating, index + 1,
                                           object_count, total_object_bytes, 0, index)) {
      return Result<PreparedArchive>::failure(*error);
    }
  }

  if (const auto error = report_progress(options, ProgressPhase::validating, object_count,
                                         object_count, total_object_bytes, output_bytes)) {
    return Result<PreparedArchive>::failure(*error);
  }
  return Result<PreparedArchive>::success({{object_count, total_object_bytes, output_bytes}});
}

std::array<std::uint8_t, kHeaderBytes> make_header(std::uint32_t size_or_count,
                                                   std::string_view name) {
  std::array<std::uint8_t, kHeaderBytes> header{};
  header[0] = static_cast<std::uint8_t>(size_or_count);
  header[1] = static_cast<std::uint8_t>(size_or_count >> 8);
  header[2] = static_cast<std::uint8_t>(size_or_count >> 16);
  header[3] = static_cast<std::uint8_t>(size_or_count >> 24);
  std::memcpy(header.data() + sizeof(std::uint32_t), name.data(), name.size());
  return header;
}

template <typename Sink>
std::optional<Error> emit(std::string_view archive_name,
                          std::span<const ObjectRecord> objects,
                          const PreparedArchive& prepared,
                          const Options& options,
                          Sink&& sink) {
  const auto object_count = prepared.summary.object_count;
  std::size_t written = 0;
  const auto archive_header = make_header(object_count, archive_name);
  if (const auto error = sink(archive_header)) {
    return error;
  }
  written += archive_header.size();
  if (const auto error = report_progress(options, ProgressPhase::writing, 0, object_count, written,
                                         prepared.summary.output_bytes)) {
    return error;
  }

  constexpr std::array<std::uint8_t, kObjectAlignment> zeros{};
  for (std::uint32_t index = 0; index < object_count; ++index) {
    if (const auto error = check_cancelled(options, index)) {
      return error;
    }
    const auto& object = objects[index];
    const auto object_header =
        make_header(static_cast<std::uint32_t>(object.data.size()), object.internal_name);
    if (const auto error = sink(object_header)) {
      return error;
    }
    written += object_header.size();

    std::size_t offset = 0;
    while (offset < object.data.size()) {
      if (const auto error = check_cancelled(options, index)) {
        return error;
      }
      const auto chunk_size = std::min(options.write_chunk_bytes, object.data.size() - offset);
      if (const auto error = sink(object.data.subspan(offset, chunk_size))) {
        return error;
      }
      offset += chunk_size;
      written += chunk_size;
      if (const auto error = report_progress(options, ProgressPhase::writing, index, object_count,
                                             written, prepared.summary.output_bytes, index)) {
        return error;
      }
    }

    const auto padding =
        (kObjectAlignment - (object.data.size() % kObjectAlignment)) % kObjectAlignment;
    if (padding != 0) {
      if (const auto error = sink(std::span<const std::uint8_t>(zeros).first(padding))) {
        return error;
      }
      written += padding;
    }
    if (const auto error = report_progress(options, ProgressPhase::writing, index + 1, object_count,
                                           written, prepared.summary.output_bytes, index)) {
      return error;
    }
  }

  if (written != prepared.summary.output_bytes) {
    return make_error(ErrorCode::stage_write_failed,
                      "The DGO writer produced an unexpected byte count.");
  }
  return {};
}

std::string system_error_message(const char* action, int error_number) {
  return std::string(action) + ": " +
         std::error_code(error_number, std::generic_category()).message();
}

bool safe_destination_basename(std::string_view name, std::size_t cap) {
  return !name.empty() && name.size() <= cap && name != "." && name != ".." &&
         name.back() != '.' &&
         std::all_of(name.begin(), name.end(), [](unsigned char byte) {
           return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
         });
}

std::optional<Error> remove_owned_output(int directory,
                                         std::string_view name,
                                         const posix_file::Identity& identity) {
  if (!posix_file::entry_identity(directory, name, identity)) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "The owned DGO output changed before cleanup.");
  }
  const std::string owned_name(name);
  if (::unlinkat(directory, owned_name.c_str(), 0) != 0) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      system_error_message("Could not remove the owned DGO output", errno));
  }
  return {};
}

struct OwnedTemporary {
  posix_file::OwnedFd descriptor;
  std::string name;
  posix_file::Identity identity;
};

Result<OwnedTemporary> create_temporary(int directory) {
  static std::atomic<std::uint64_t> next_id{0};
  for (std::size_t attempt = 0; attempt < 64; ++attempt) {
    const auto id = next_id.fetch_add(1, std::memory_order_relaxed);
    auto name = ".opengoal-dgo-" + std::to_string(static_cast<unsigned long long>(::getpid())) +
                "-" + std::to_string(static_cast<unsigned long long>(id)) + ".tmp";
    auto descriptor =
        posix_file::open_file_at(directory, name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (!descriptor) {
      if (errno == EEXIST) {
        continue;
      }
      return Result<OwnedTemporary>::failure(make_error(
          ErrorCode::stage_create_failed,
          system_error_message("Could not exclusively create a DGO temporary file", errno)));
    }
    posix_file::Identity identity;
    struct stat status {};
    if (!posix_file::descriptor_identity(descriptor.get(), &identity, &status) ||
        !S_ISREG(status.st_mode) || status.st_nlink != 1) {
      return Result<OwnedTemporary>::failure(make_error(
          ErrorCode::stage_create_failed,
          "The exclusively created DGO temporary is not a private regular file."));
    }
    return Result<OwnedTemporary>::success(
        {std::move(descriptor), std::move(name), identity});
  }
  return Result<OwnedTemporary>::failure(make_error(
      ErrorCode::stage_create_failed, "Could not allocate a unique DGO temporary basename."));
}

std::optional<Error> verify_descriptor_hash(int descriptor,
                                            std::size_t size,
                                            std::uint64_t expected_hash) {
  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);
  std::vector<std::uint8_t> buffer(64 * 1024);
  std::size_t offset = 0;
  while (offset < size) {
    const auto chunk = std::min(buffer.size(), size - offset);
    const auto count = ::pread(descriptor, buffer.data(), chunk, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count != static_cast<ssize_t>(chunk)) {
      return make_error(ErrorCode::stage_write_failed,
                        "Could not re-read the exact owned DGO output.");
    }
    XXH64_update(&hash_state, buffer.data(), chunk);
    offset += chunk;
  }
  if (XXH64_digest(&hash_state) != expected_hash) {
    return make_error(ErrorCode::atomic_install_failed,
                      "The owned DGO output changed after it was written.");
  }
  return {};
}

}  // namespace

Result<std::vector<std::uint8_t>> build(std::string_view archive_name,
                                        std::span<const ObjectRecord> objects,
                                        const Options& options) {
  auto prepared = prepare(archive_name, objects, options);
  if (!prepared) {
    return Result<std::vector<std::uint8_t>>::failure(prepared.error());
  }

  std::vector<std::uint8_t> output;
  try {
    output.resize(prepared.value().summary.output_bytes);
  } catch (const std::bad_alloc&) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::allocation_failed, "Could not allocate the encoded DGO buffer."));
  }

  std::size_t offset = 0;
  const auto sink = [&](std::span<const std::uint8_t> bytes) -> std::optional<Error> {
    if (bytes.size() > output.size() - offset) {
      return make_error(ErrorCode::stage_write_failed,
                        "The DGO writer exceeded its validated output size.");
    }
    std::memcpy(output.data() + offset, bytes.data(), bytes.size());
    offset += bytes.size();
    return {};
  };
  if (const auto error = emit(archive_name, objects, prepared.value(), options, sink)) {
    return Result<std::vector<std::uint8_t>>::failure(*error);
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(output));
}

Result<WriteSummary> write_file(const std::filesystem::path& destination,
                                std::string_view archive_name,
                                std::span<const ObjectRecord> objects,
                                const Options& options) {
  if (destination.empty() || destination.filename().empty()) {
    return Result<WriteSummary>::failure(
        make_error(ErrorCode::invalid_argument, "The DGO destination is empty."));
  }
  auto parent = destination.parent_path();
  if (parent.empty()) {
    parent = ".";
  }
  auto directory = posix_file::open_directory(parent.c_str());
  if (!directory) {
    return Result<WriteSummary>::failure(make_error(
        ErrorCode::stage_create_failed,
        system_error_message("Could not open the DGO destination directory", errno)));
  }
  return write_file_at(directory.get(), destination.filename().string(), archive_name, objects,
                       options);
}

Result<WriteSummary> write_file_at(int directory_fd,
                                   std::string_view destination_basename,
                                   std::string_view archive_name,
                                   std::span<const ObjectRecord> objects,
                                   const Options& options) {
  if (directory_fd < 0 ||
      !safe_destination_basename(destination_basename, options.max_name_bytes)) {
    return Result<WriteSummary>::failure(
        make_error(ErrorCode::invalid_argument, "The DGO destination basename is unsafe."));
  }
  auto prepared = prepare(archive_name, objects, options);
  if (!prepared) {
    return Result<WriteSummary>::failure(prepared.error());
  }

  auto temporary = create_temporary(directory_fd);
  if (!temporary) {
    return Result<WriteSummary>::failure(temporary.error());
  }
  auto owned = temporary.take_value();
  const auto fail_owned = [&](Error error) {
    const auto cleanup = remove_owned_output(directory_fd, owned.name, owned.identity);
    return Result<WriteSummary>::failure(cleanup ? *cleanup : std::move(error));
  };

  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);

  const auto sink = [&](std::span<const std::uint8_t> bytes) -> std::optional<Error> {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written =
          ::write(owned.descriptor.get(), bytes.data() + offset, bytes.size() - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return make_error(ErrorCode::stage_write_failed,
                          system_error_message("Could not write the owned DGO stage file", errno));
      }
      if (written == 0) {
        return make_error(ErrorCode::stage_write_failed,
                          "Writing the owned DGO stage file made no progress.");
      }
      XXH64_update(&hash_state, bytes.data() + offset, static_cast<std::size_t>(written));
      offset += static_cast<std::size_t>(written);
    }
    return {};
  };
  if (const auto error = emit(archive_name, objects, prepared.value(), options, sink)) {
    return fail_owned(*error);
  }
  if (::fsync(owned.descriptor.get()) != 0) {
    const auto error =
        make_error(ErrorCode::stage_sync_failed,
                   system_error_message("Could not synchronize the owned DGO output", errno));
    return fail_owned(error);
  }
  if (const auto error = check_cancelled(options)) {
    return fail_owned(*error);
  }
  if (const auto error = report_progress(
          options, ProgressPhase::installing, prepared.value().summary.object_count,
          prepared.value().summary.object_count, prepared.value().summary.output_bytes,
          prepared.value().summary.output_bytes)) {
    return fail_owned(*error);
  }
  struct stat final_descriptor_status {};
  struct stat final_entry_status {};
  if (!posix_file::descriptor_identity(owned.descriptor.get(), nullptr,
                                       &final_descriptor_status) ||
      !posix_file::entry_identity(directory_fd, owned.name, owned.identity,
                                  &final_entry_status) ||
      !S_ISREG(final_entry_status.st_mode) || final_entry_status.st_nlink != 1 ||
      final_descriptor_status.st_size !=
          static_cast<off_t>(prepared.value().summary.output_bytes) ||
      final_entry_status.st_size != final_descriptor_status.st_size) {
    const auto error = make_error(
        ErrorCode::atomic_install_failed,
        "The owned DGO output changed before descriptor-relative installation completed.");
    return fail_owned(error);
  }
  auto summary = prepared.value().summary;
  summary.output_xxh64 = XXH64_digest(&hash_state);
  if (const auto error =
          verify_descriptor_hash(owned.descriptor.get(), summary.output_bytes,
                                 summary.output_xxh64)) {
    return fail_owned(*error);
  }
  if (posix_file::exclusive_rename_at(directory_fd, owned.name, directory_fd,
                                      destination_basename) != 0) {
    const auto error = make_error(
        errno == EEXIST ? ErrorCode::destination_exists : ErrorCode::atomic_install_failed,
        system_error_message("Could not exclusively install the checked DGO", errno));
    return fail_owned(error);
  }
  if (!posix_file::entry_identity(directory_fd, destination_basename, owned.identity)) {
    return Result<WriteSummary>::failure(make_error(
        ErrorCode::atomic_install_failed,
        "The installed DGO identity changed before installation completed."));
  }
  return Result<WriteSummary>::success(std::move(summary));
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::invalid_name:
      return "invalid_name";
    case ErrorCode::empty_archive:
      return "empty_archive";
    case ErrorCode::object_count_limit_exceeded:
      return "object_count_limit_exceeded";
    case ErrorCode::object_size_limit_exceeded:
      return "object_size_limit_exceeded";
    case ErrorCode::total_object_size_limit_exceeded:
      return "total_object_size_limit_exceeded";
    case ErrorCode::output_size_limit_exceeded:
      return "output_size_limit_exceeded";
    case ErrorCode::duplicate_object_name:
      return "duplicate_object_name";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::destination_inspection_failed:
      return "destination_inspection_failed";
    case ErrorCode::destination_exists:
      return "destination_exists";
    case ErrorCode::stage_create_failed:
      return "stage_create_failed";
    case ErrorCode::stage_write_failed:
      return "stage_write_failed";
    case ErrorCode::stage_sync_failed:
      return "stage_sync_failed";
    case ErrorCode::stage_close_failed:
      return "stage_close_failed";
    case ErrorCode::atomic_install_failed:
      return "atomic_install_failed";
    case ErrorCode::stage_cleanup_failed:
      return "stage_cleanup_failed";
  }
  return "unknown";
}

}  // namespace jak1_checked_dgo_writer
