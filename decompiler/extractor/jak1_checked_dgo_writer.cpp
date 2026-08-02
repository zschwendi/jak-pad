#include "jak1_checked_dgo_writer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <string>
#include <system_error>
#include <unistd.h>
#include <unordered_set>

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

struct OwnedStage {
  int descriptor = -1;
  std::filesystem::path path;
  bool exists = false;

  OwnedStage() = default;
  OwnedStage(const OwnedStage&) = delete;
  OwnedStage& operator=(const OwnedStage&) = delete;
  OwnedStage(OwnedStage&& other) noexcept
      : descriptor(std::exchange(other.descriptor, -1)),
        path(std::move(other.path)),
        exists(std::exchange(other.exists, false)) {}
  OwnedStage& operator=(OwnedStage&&) = delete;

  ~OwnedStage() {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    if (exists) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
    }
  }

  std::optional<Error> close_checked() {
    if (descriptor < 0) {
      return {};
    }
    const int current = descriptor;
    descriptor = -1;
    if (::close(current) != 0) {
      return make_error(ErrorCode::stage_close_failed,
                        system_error_message("Could not close the owned DGO stage file", errno));
    }
    return {};
  }

  std::optional<Error> remove_checked() {
    if (!exists) {
      return {};
    }
    std::error_code error;
    const bool removed = std::filesystem::remove(path, error);
    if (error || !removed) {
      return make_error(ErrorCode::stage_cleanup_failed,
                        "Could not remove the owned DGO stage file: " +
                            (error ? error.message() : std::string("the file was not present")));
    }
    exists = false;
    return {};
  }
};

Result<OwnedStage> create_stage(const std::filesystem::path& destination) {
  const auto filename = destination.filename().string();
  if (filename.empty()) {
    return Result<OwnedStage>::failure(
        make_error(ErrorCode::invalid_argument, "The DGO destination has no file name."));
  }
  auto directory = destination.parent_path();
  if (directory.empty()) {
    directory = ".";
  }
  const auto template_path = directory / ("." + filename + ".opengoal-stage-XXXXXX");
  auto template_string = template_path.string();
  if (template_string.find('\0') != std::string::npos) {
    return Result<OwnedStage>::failure(
        make_error(ErrorCode::invalid_argument, "The DGO destination contains a null byte."));
  }

  std::vector<char> writable_template;
  try {
    writable_template.assign(template_string.begin(), template_string.end());
    writable_template.push_back('\0');
  } catch (const std::bad_alloc&) {
    return Result<OwnedStage>::failure(make_error(
        ErrorCode::allocation_failed, "Could not allocate the owned DGO stage-file path."));
  }

  const int descriptor = ::mkstemp(writable_template.data());
  if (descriptor < 0) {
    return Result<OwnedStage>::failure(
        make_error(ErrorCode::stage_create_failed,
                   system_error_message("Could not create an owned DGO stage file", errno)));
  }
  OwnedStage stage;
  stage.descriptor = descriptor;
  try {
    stage.path = std::filesystem::path(writable_template.data());
  } catch (const std::bad_alloc&) {
    ::close(descriptor);
    ::unlink(writable_template.data());
    stage.descriptor = -1;
    return Result<OwnedStage>::failure(make_error(
        ErrorCode::allocation_failed, "Could not retain the owned DGO stage-file path."));
  }
  stage.exists = true;
  return Result<OwnedStage>::success(std::move(stage));
}

std::optional<Error> destination_error(const std::filesystem::path& destination) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(destination, error);
  if (error && error != std::errc::no_such_file_or_directory) {
    return make_error(ErrorCode::destination_inspection_failed,
                      "Could not inspect the DGO destination: " + error.message());
  }
  if (status.type() != std::filesystem::file_type::not_found) {
    return make_error(ErrorCode::destination_exists, "The DGO destination already exists.");
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
  if (destination.empty()) {
    return Result<WriteSummary>::failure(
        make_error(ErrorCode::invalid_argument, "The DGO destination is empty."));
  }
  auto prepared = prepare(archive_name, objects, options);
  if (!prepared) {
    return Result<WriteSummary>::failure(prepared.error());
  }
  if (const auto error = destination_error(destination)) {
    return Result<WriteSummary>::failure(*error);
  }

  auto stage_result = create_stage(destination);
  if (!stage_result) {
    return Result<WriteSummary>::failure(stage_result.error());
  }
  auto stage = stage_result.take_value();

  const auto sink = [&](std::span<const std::uint8_t> bytes) -> std::optional<Error> {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written = ::write(stage.descriptor, bytes.data() + offset, bytes.size() - offset);
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
      offset += static_cast<std::size_t>(written);
    }
    return {};
  };
  if (const auto error = emit(archive_name, objects, prepared.value(), options, sink)) {
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : *error);
  }
  if (::fsync(stage.descriptor) != 0) {
    const auto error =
        make_error(ErrorCode::stage_sync_failed,
                   system_error_message("Could not synchronize the owned DGO stage file", errno));
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : error);
  }
  if (const auto error = stage.close_checked()) {
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : *error);
  }
  if (const auto error = check_cancelled(options)) {
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : *error);
  }
  if (const auto error = report_progress(
          options, ProgressPhase::installing, prepared.value().summary.object_count,
          prepared.value().summary.object_count, prepared.value().summary.output_bytes,
          prepared.value().summary.output_bytes)) {
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : *error);
  }

  std::error_code install_error;
  std::filesystem::create_hard_link(stage.path, destination, install_error);
  if (install_error) {
    auto error = make_error(ErrorCode::atomic_install_failed,
                            "Could not atomically install the DGO: " + install_error.message());
    std::error_code inspect_error;
    const auto status = std::filesystem::symlink_status(destination, inspect_error);
    if (!inspect_error && status.type() != std::filesystem::file_type::not_found) {
      error = make_error(ErrorCode::destination_exists,
                         "The DGO destination was created before atomic installation.");
    }
    const auto cleanup = stage.remove_checked();
    return Result<WriteSummary>::failure(cleanup ? *cleanup : error);
  }
  if (const auto cleanup = stage.remove_checked()) {
    return Result<WriteSummary>::failure(*cleanup);
  }
  return Result<WriteSummary>::success(prepared.value().summary);
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
