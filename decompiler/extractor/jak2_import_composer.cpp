#include "jak2_import_composer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

#include "common/custom_data/Jak2SourceObjectPack.h"

#include "decompiler/extractor/jak2_import_composer_internal.h"
#include "decompiler/extractor/jak2_iso_validation.h"
#include <sys/stat.h>

namespace jak2_import_composer {
namespace {

namespace fs = std::filesystem;
namespace source_pack = jak2_source_object_pack;

constexpr std::string_view kWorkDirectoryName = ".opengoal-import";

Error make_error(ErrorCode code, std::string message) {
  return {code, std::move(message), std::nullopt, std::nullopt};
}

Error make_filesystem_error(ErrorCode code, std::string message, const std::error_code& error) {
  return make_error(code, std::move(message) + ": " + error.message());
}

bool direct_directory(const fs::path& path) {
  std::error_code error;
  return fs::symlink_status(path, error).type() == fs::file_type::directory && !error;
}

bool direct_regular_file(const fs::path& path) {
  std::error_code error;
  return fs::symlink_status(path, error).type() == fs::file_type::regular && !error;
}

bool missing_path(const fs::path& path) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  return (!error || error == std::errc::no_such_file_or_directory) &&
         status.type() == fs::file_type::not_found;
}

class CallbackForwarder {
 public:
  explicit CallbackForwarder(const Options& options) : m_options(options) {}

  bool poll_cancel() {
    if (m_callback_failed) {
      return true;
    }
    if (!m_options.should_cancel) {
      return false;
    }
    try {
      return m_options.should_cancel();
    } catch (...) {
      m_callback_failed = true;
      return true;
    }
  }

  bool report(Progress progress) {
    if (m_callback_failed) {
      return false;
    }
    if (!m_options.on_progress) {
      return true;
    }
    try {
      m_options.on_progress(progress);
      return true;
    } catch (...) {
      m_callback_failed = true;
      return false;
    }
  }

  Error cancellation_or_callback_error(std::string cancellation_message) const {
    return m_callback_failed
               ? make_error(ErrorCode::callback_failed,
                            "The Jak II import callback failed while staging the candidate.")
               : make_error(ErrorCode::cancelled, std::move(cancellation_message));
  }

  bool callback_failed() const { return m_callback_failed; }

 private:
  const Options& m_options;
  bool m_callback_failed = false;
};

struct FileCloser {
  void operator()(FILE* file) const {
    if (file) {
      std::fclose(file);
    }
  }
};

using OwnedFile = std::unique_ptr<FILE, FileCloser>;

Result<OwnedFile> open_iso_file(const fs::path& path) {
  const auto descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO could not be opened safely",
        std::error_code(errno, std::generic_category())));
  }

  struct stat status{};
  if (::fstat(descriptor, &status) != 0) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO could not be inspected", error));
  }
  if (!S_ISREG(status.st_mode)) {
    ::close(descriptor);
    return Result<OwnedFile>::failure(
        make_error(ErrorCode::iso_open_failed, "The selected ISO is not a direct regular file."));
  }

  auto* stream = ::fdopen(descriptor, "rb");
  if (!stream) {
    const std::error_code error(errno, std::generic_category());
    ::close(descriptor);
    return Result<OwnedFile>::failure(make_filesystem_error(
        ErrorCode::iso_open_failed, "The selected ISO stream could not be opened", error));
  }
  return Result<OwnedFile>::success(OwnedFile(stream));
}

bool directory_contains(const fs::path& root,
                        const fs::path& candidate_parent,
                        std::error_code* inspection_error) {
  auto cursor = candidate_parent;
  while (true) {
    std::error_code error;
    if (fs::equivalent(root, cursor, error)) {
      return true;
    }
    if (error) {
      *inspection_error = error;
      return false;
    }
    const auto parent = cursor.parent_path();
    if (parent == cursor) {
      return false;
    }
    cursor = parent;
  }
}

Result<Request> validate_request(const Request& request) {
  const std::array<const fs::path*, 3> paths = {
      &request.iso_path,
      &request.source_object_pack_root,
      &request.candidate_root,
  };
  if (std::any_of(paths.begin(), paths.end(),
                  [](const auto* path) { return !path->is_absolute() || path->empty(); }) ||
      request.candidate_root.filename().empty() || !direct_regular_file(request.iso_path) ||
      !direct_directory(request.source_object_pack_root) ||
      !direct_directory(request.candidate_root.parent_path()) ||
      !missing_path(request.candidate_root)) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The ISO, verified source pack, or fresh candidate path is invalid."));
  }

  Request resolved = request;
  std::error_code error;
  resolved.iso_path = fs::canonical(request.iso_path, error);
  if (error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument, "The ISO path could not be canonicalized."));
  }
  resolved.source_object_pack_root = fs::canonical(request.source_object_pack_root, error);
  if (error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The source-object-pack root could not be canonicalized."));
  }
  const auto candidate_parent = fs::canonical(request.candidate_root.parent_path(), error);
  if (error) {
    return Result<Request>::failure(make_error(
        ErrorCode::invalid_argument, "The import candidate parent could not be canonicalized."));
  }
  resolved.candidate_root = candidate_parent / request.candidate_root.filename();
  if (!missing_path(resolved.candidate_root)) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument, "The canonical import candidate already exists."));
  }

  std::error_code containment_error;
  if (directory_contains(resolved.source_object_pack_root, candidate_parent, &containment_error)) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The import candidate cannot be created inside the source-object pack."));
  }
  if (containment_error) {
    return Result<Request>::failure(
        make_error(ErrorCode::invalid_argument,
                   "The import candidate containment check could not be completed."));
  }
  return Result<Request>::success(std::move(resolved));
}

std::optional<Error> remove_partial_residue(const fs::path& work_root) {
  if (!direct_directory(work_root)) {
    return {};
  }
  std::error_code error;
  fs::recursive_directory_iterator iterator(work_root, error);
  const fs::recursive_directory_iterator end;
  if (error) {
    return make_error(ErrorCode::candidate_cleanup_failed,
                      "Could not inspect the preserved candidate for partial files.");
  }
  for (; iterator != end; iterator.increment(error)) {
    if (error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "The preserved candidate changed while partial files were removed.");
    }
    if (!iterator->path().filename().string().ends_with(".partial")) {
      continue;
    }
    const auto status = iterator->symlink_status(error);
    if (error || status.type() != fs::file_type::regular || !fs::remove(iterator->path(), error) ||
        error) {
      return make_error(ErrorCode::candidate_cleanup_failed,
                        "Could not remove an import partial file from the preserved candidate.");
    }
  }
  return {};
}

Error preserve_error(Error error, const internal::WorkPaths& paths) {
  error.preserved_candidate_root = paths.candidate_root;
  if (const auto cleanup = remove_partial_residue(paths.work_root)) {
    error.cleanup_error = cleanup->message;
  }
  return error;
}

Error map_source_pack_failure(const source_pack::Error& error, CallbackForwarder& callbacks) {
  if (error.code == source_pack::ErrorCode::callback_failed || callbacks.callback_failed()) {
    return callbacks.cancellation_or_callback_error({});
  }
  if (error.code == source_pack::ErrorCode::cancelled) {
    return make_error(ErrorCode::cancelled, "Jak II source-pack validation was cancelled.");
  }
  return make_error(ErrorCode::source_pack_failed,
                    "The checked Jak II source-object pack was rejected: " + error.message);
}

}  // namespace

namespace internal {

Result<Summary> compose_in_fresh_candidate(const fs::path& candidate_root,
                                           const Options& options,
                                           std::span<const StageAction> stages) {
  WorkPaths paths{candidate_root, candidate_root / kWorkDirectoryName};
  bool candidate_created = false;
  try {
    if (!candidate_root.is_absolute() || candidate_root.filename().empty() ||
        !direct_directory(candidate_root.parent_path()) || !missing_path(candidate_root) ||
        stages.empty()) {
      return Result<Summary>::failure(
          make_error(ErrorCode::invalid_argument,
                     "The composer requires a fresh absolute candidate under a direct directory."));
    }
    for (const auto& stage : stages) {
      if (!stage.run) {
        return Result<Summary>::failure(
            make_error(ErrorCode::invalid_argument, "The composer stage list is incomplete."));
      }
    }

    std::error_code error;
    if (!fs::create_directory(candidate_root, error) || error) {
      return Result<Summary>::failure(
          make_filesystem_error(ErrorCode::candidate_create_failed,
                                "Could not exclusively create the import candidate", error));
    }
    candidate_created = true;
    if (!fs::create_directory(paths.work_root, error) || error) {
      return Result<Summary>::failure(preserve_error(
          make_filesystem_error(ErrorCode::candidate_create_failed,
                                "Could not create the hidden import work directory", error),
          paths));
    }

    CallbackForwarder callbacks(options);
    for (const auto& stage : stages) {
      if (callbacks.poll_cancel()) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error(
                               "Jak II import was cancelled before the next staging phase."),
                           paths));
      }
      if (!callbacks.report({stage.phase, 0, 1, 0, {}})) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error({}), paths));
      }
      if (const auto stage_error = stage.run(paths)) {
        return Result<Summary>::failure(preserve_error(*stage_error, paths));
      }
      if (!callbacks.report({stage.phase, 1, 1, 0, {}})) {
        return Result<Summary>::failure(
            preserve_error(callbacks.cancellation_or_callback_error({}), paths));
      }
    }

    return Result<Summary>::failure(preserve_error(
        make_error(ErrorCode::prepared_output_unavailable,
                   "Jak II ISO and source-pack validation completed, but generated data, FR3, "
                   "checked materialization, and final candidate validation are not implemented."),
        paths));
  } catch (const std::bad_alloc&) {
    auto error =
        make_error(ErrorCode::allocation_failed, "Jak II import staging ran out of memory.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (const std::exception& exception) {
    auto error =
        make_error(ErrorCode::unexpected_failure,
                   "Jak II import staging failed unexpectedly: " + std::string(exception.what()));
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  } catch (...) {
    auto error =
        make_error(ErrorCode::unexpected_failure, "Jak II import staging failed unexpectedly.");
    return Result<Summary>::failure(candidate_created ? preserve_error(std::move(error), paths)
                                                      : std::move(error));
  }
}

}  // namespace internal

Result<Summary> compose(const Request& request, const Options& options) {
  try {
    auto validated_request = validate_request(request);
    if (!validated_request) {
      return Result<Summary>::failure(validated_request.error());
    }
    const auto resolved_request = validated_request.take_value();
    auto iso = open_iso_file(resolved_request.iso_path);
    if (!iso) {
      return Result<Summary>::failure(iso.error());
    }

    CallbackForwarder callbacks(options);
    if (callbacks.poll_cancel()) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error(
          "Jak II import was cancelled before source-pack validation."));
    }
    source_pack::Options source_options;
    source_options.should_cancel = [&] { return callbacks.poll_cancel(); };
    source_options.on_progress = [&](const source_pack::Progress& progress) {
      callbacks.report({Phase::validating_source_pack, progress.completed, progress.total,
                        progress.bytes_hashed, progress.current_file});
    };
    auto verified_pack =
        source_pack::validate_recorded(resolved_request.source_object_pack_root, source_options);
    if (!verified_pack) {
      return Result<Summary>::failure(map_source_pack_failure(verified_pack.error(), callbacks));
    }
    if (callbacks.callback_failed()) {
      return Result<Summary>::failure(callbacks.cancellation_or_callback_error({}));
    }

    OwnedFile iso_file = iso.take_value();
    const std::array<internal::StageAction, 1> stages = {{
        {Phase::extracting_iso,
         [&](const internal::WorkPaths& paths) -> std::optional<Error> {
           CallbackForwarder extraction_callbacks(options);
           iso_file::Options iso_options;
           iso_options.should_cancel = [&] { return extraction_callbacks.poll_cancel(); };
           iso_options.on_progress = [&](const iso_file::Progress& progress) {
             extraction_callbacks.report({Phase::extracting_iso, progress.files_completed,
                                          progress.files_total, progress.bytes_completed,
                                          progress.current_path});
           };
           auto extracted = jak2_iso::extract_and_validate(
               iso_file.get(), paths.work_root / "extracted-iso", iso_options);
           if (!extracted) {
             if (extracted.error().code == jak2_iso::ValidationErrorCode::cancelled ||
                 extraction_callbacks.callback_failed()) {
               return extraction_callbacks.cancellation_or_callback_error(
                   "Jak II ISO extraction was cancelled.");
             }
             return make_error(
                 ErrorCode::iso_validation_failed,
                 "The selected Jak II ISO was rejected: " + extracted.error().message);
           }
           if (extraction_callbacks.callback_failed()) {
             return extraction_callbacks.cancellation_or_callback_error({});
           }
           return {};
         }},
    }};
    return internal::compose_in_fresh_candidate(resolved_request.candidate_root, options, stages);
  } catch (const std::bad_alloc&) {
    return Result<Summary>::failure(
        make_error(ErrorCode::allocation_failed, "Jak II import staging ran out of memory."));
  } catch (const std::exception& exception) {
    return Result<Summary>::failure(
        make_error(ErrorCode::unexpected_failure,
                   "Jak II import staging failed unexpectedly: " + std::string(exception.what())));
  } catch (...) {
    return Result<Summary>::failure(
        make_error(ErrorCode::unexpected_failure, "Jak II import staging failed unexpectedly."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::source_pack_failed:
      return "source_pack_failed";
    case ErrorCode::iso_open_failed:
      return "iso_open_failed";
    case ErrorCode::iso_validation_failed:
      return "iso_validation_failed";
    case ErrorCode::candidate_create_failed:
      return "candidate_create_failed";
    case ErrorCode::candidate_cleanup_failed:
      return "candidate_cleanup_failed";
    case ErrorCode::prepared_output_unavailable:
      return "prepared_output_unavailable";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::unexpected_failure:
      return "unexpected_failure";
  }
  return "unknown";
}

const char* phase_name(Phase phase) {
  switch (phase) {
    case Phase::validating_source_pack:
      return "validating_source_pack";
    case Phase::extracting_iso:
      return "extracting_iso";
  }
  return "unknown";
}

}  // namespace jak2_import_composer
