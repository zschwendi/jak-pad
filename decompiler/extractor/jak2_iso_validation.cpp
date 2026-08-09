#include "jak2_iso_validation.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>
#include <vector>

#include "common/util/PosixFile.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak2_iso {
namespace {

ValidationError make_error(ValidationErrorCode code, std::string message) {
  return {code, std::move(message), std::nullopt, std::nullopt};
}

bool is_ascii_upper(char value) {
  return value >= 'A' && value <= 'Z';
}

bool is_ascii_digit(char value) {
  return value >= '0' && value <= '9';
}

std::optional<std::string> serial_from_executable_name(const std::string& name) {
  if (name.size() != 11 || name[4] != '_' || name[8] != '.') {
    return std::nullopt;
  }
  for (size_t index = 0; index < 4; ++index) {
    if (!is_ascii_upper(name[index])) {
      return std::nullopt;
    }
  }
  for (const size_t index : {size_t(5), size_t(6), size_t(7), size_t(9), size_t(10)}) {
    if (!is_ascii_digit(name[index])) {
      return std::nullopt;
    }
  }
  return name.substr(0, 4) + "-" + name.substr(5, 3) + name.substr(9, 2);
}

struct ExecutableCandidate {
  std::string serial;
  size_t hash_index = 0;
};

struct LayoutScan {
  bool has_dgo_directory = false;
  size_t file_count = 0;
  std::vector<ExecutableCandidate> executables;
};

void scan_entry(const IsoFile::Entry& entry,
                bool root_entry,
                LayoutScan* scan,
                size_t* next_hash_index) {
  if (entry.is_dir) {
    if (root_entry && entry.name == "DGO") {
      scan->has_dgo_directory = true;
    }
    for (const auto& child : entry.children) {
      scan_entry(child, false, scan, next_hash_index);
    }
    return;
  }

  if (root_entry) {
    if (auto serial = serial_from_executable_name(entry.name)) {
      scan->executables.push_back({std::move(*serial), *next_hash_index});
    }
  }
  ++scan->file_count;
  ++*next_hash_index;
}

bool collect_file_identities(const IsoFile::Entry& entry,
                             const std::string& prefix,
                             const IsoFile& layout,
                             std::size_t* next_hash_index,
                             std::set<std::string>* paths,
                             std::vector<checked_file_identity::Identity>* files) {
  const auto basename = iso_file::extracted_output_name(entry.name);
  const auto relative_path = prefix.empty() ? basename : prefix + "/" + basename;
  if (basename.empty() || basename.size() > 128 || basename == "." || basename == ".." ||
      basename.back() == '.' || basename.back() == ' ' || relative_path.size() > 1024 ||
      std::any_of(basename.begin(), basename.end(), [](unsigned char byte) {
        return byte < 0x20 || byte > 0x7e || byte == '/' || byte == '\\' || byte == ':';
      })) {
    return false;
  }
  if (entry.is_dir) {
    for (const auto& child : entry.children) {
      if (!collect_file_identities(child, relative_path, layout, next_hash_index, paths, files)) {
        return false;
      }
    }
    return true;
  }
  auto collision_path = relative_path;
  std::transform(collision_path.begin(), collision_path.end(), collision_path.begin(),
                 [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  if (*next_hash_index >= layout.hashes.size() || relative_path.empty() ||
      !paths->emplace(std::move(collision_path)).second) {
    return false;
  }
  files->push_back(
      {relative_path, static_cast<std::uint64_t>(entry.size), layout.hashes[(*next_hash_index)++]});
  return true;
}

std::optional<std::vector<checked_file_identity::Identity>> file_identities(const IsoFile& layout) {
  std::vector<checked_file_identity::Identity> files;
  files.reserve(layout.hashes.size());
  std::set<std::string> paths;
  std::size_t next_hash_index = 0;
  for (const auto& entry : layout.root.children) {
    if (!collect_file_identities(entry, {}, layout, &next_hash_index, &paths, &files)) {
      return std::nullopt;
    }
  }
  if (next_hash_index != layout.hashes.size() || files.size() != layout.hashes.size()) {
    return std::nullopt;
  }
  return files;
}

ValidationError with_cleanup(ValidationError error,
                             iso_file::OwnedStagingDirectory* staging_directory) {
  if (auto cleanup_error = staging_directory->cleanup()) {
    error.cleanup_error = std::move(*cleanup_error);
  }
  return error;
}

std::string checkpoint_contents(const RevisionMatch& match) {
  std::ostringstream contents;
  contents << "[\n"
           << "  {\n"
           << "    \"elf_hash\": " << match.fingerprint.elf_hash << ",\n"
           << "    \"serial\": \"" << match.fingerprint.serial << "\"\n"
           << "  }\n"
           << "]";
  return contents.str();
}

std::optional<ValidationError> write_checkpoint_file(const RevisionMatch& match,
                                                     const std::filesystem::path& directory) {
  const auto checkpoint = directory / "buildinfo.json";
  const auto temporary = directory / ".buildinfo.json.tmp";
  std::error_code file_error;
  if (std::filesystem::exists(checkpoint, file_error) || file_error) {
    return make_error(ValidationErrorCode::checkpoint_write_failed,
                      "The validated staging directory already contains buildinfo.json.");
  }
  if (std::filesystem::exists(temporary, file_error) || file_error) {
    return make_error(
        ValidationErrorCode::checkpoint_write_failed,
        "The validated staging directory already contains a buildinfo temporary file.");
  }

  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << checkpoint_contents(match);
    output.close();
    if (!output) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return make_error(ValidationErrorCode::checkpoint_write_failed,
                        "Could not write the validated extraction checkpoint.");
    }
  }

  std::filesystem::rename(temporary, checkpoint, file_error);
  if (file_error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return make_error(ValidationErrorCode::checkpoint_write_failed,
                      "Could not atomically install the validated extraction checkpoint: " +
                          file_error.message());
  }
  return std::nullopt;
}

std::optional<ValidationError> write_checkpoint_file_at(
    const RevisionMatch& match,
    iso_file::OwnedStagingDirectory* staging_directory) {
  constexpr std::string_view kCheckpoint = "buildinfo.json";
  constexpr std::string_view kTemporary = ".buildinfo.json.tmp";
  const auto directory = staging_directory->directory_descriptor();
  if (directory < 0) {
    return make_error(ValidationErrorCode::checkpoint_write_failed,
                      "The exact validated staging directory is unavailable.");
  }

  auto temporary =
      posix_file::open_file_at(directory, kTemporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (!temporary || !staging_directory->track_created_file(kTemporary, temporary.get())) {
    return make_error(ValidationErrorCode::checkpoint_write_failed,
                      "Could not exclusively create and retain the extraction checkpoint.");
  }

  const auto contents = checkpoint_contents(match);
  size_t written = 0;
  while (written < contents.size()) {
    const auto result =
        ::write(temporary.get(), contents.data() + written, contents.size() - written);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return make_error(ValidationErrorCode::checkpoint_write_failed,
                        "Could not write the validated extraction checkpoint: " +
                            std::string(std::strerror(errno)));
    }
    written += static_cast<size_t>(result);
  }
  if (!staging_directory->rename_tracked_file(kTemporary, kCheckpoint)) {
    return make_error(ValidationErrorCode::checkpoint_write_failed,
                      "Could not atomically install the exact extraction checkpoint.");
  }
  return std::nullopt;
}

}  // namespace

uint64_t aggregate_contents_hash(std::span<const uint64_t> file_hashes) {
  uint64_t combined_hash = 0;
  for (const auto hash : file_hashes) {
    combined_hash ^= hash;
  }
  return XXH64(&combined_hash, sizeof(uint64_t), 0);
}

ValidationResult<std::vector<checked_file_identity::Identity>> validated_file_identities(
    const IsoFile& layout) {
  if (!layout.shouldHash || layout.files_extracted < 0 ||
      static_cast<std::size_t>(layout.files_extracted) != layout.hashes.size()) {
    return ValidationResult<std::vector<checked_file_identity::Identity>>::failure(
        make_error(ValidationErrorCode::invalid_extraction_result,
                   "The ISO reader did not return one hash for every extracted file."));
  }
  auto files = file_identities(layout);
  if (!files || files->size() != layout.hashes.size()) {
    return ValidationResult<std::vector<checked_file_identity::Identity>>::failure(
        make_error(ValidationErrorCode::invalid_extraction_result,
                   "The extracted-file identity mapping is incomplete, unsafe, or ambiguous."));
  }
  return ValidationResult<std::vector<checked_file_identity::Identity>>::success(std::move(*files));
}

ValidationResult<RevisionMatch> match_supported_revision(const Fingerprint& fingerprint) {
  std::vector<const Revision*> serial_matches;
  std::vector<const Revision*> executable_matches;
  std::vector<const Revision*> count_matches;
  for (const auto& revision : supported_revisions()) {
    if (revision.serial != fingerprint.serial) {
      continue;
    }
    serial_matches.push_back(&revision);
    if (revision.elf_hash != fingerprint.elf_hash) {
      continue;
    }
    executable_matches.push_back(&revision);
    if (revision.file_count != fingerprint.file_count) {
      continue;
    }
    count_matches.push_back(&revision);
    if (revision.contents_hash == fingerprint.contents_hash) {
      return ValidationResult<RevisionMatch>::success({fingerprint, revision});
    }
  }

  if (serial_matches.empty()) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::unsupported_serial,
                   "The disc serial is not a supported Jak II revision."));
  }
  if (executable_matches.empty()) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::unsupported_executable,
                   "The disc executable does not match a supported Jak II revision."));
  }
  if (count_matches.empty()) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::unexpected_file_count,
                   "The disc contains an unexpected number of files."));
  }
  return ValidationResult<RevisionMatch>::failure(
      make_error(ValidationErrorCode::contents_hash_mismatch,
                 "The disc contents do not match a supported Jak II revision."));
}

ValidationResult<RevisionMatch> validate_extracted_layout(const IsoFile& layout) {
  LayoutScan scan;
  size_t next_hash_index = 0;
  for (const auto& entry : layout.root.children) {
    scan_entry(entry, true, &scan, &next_hash_index);
  }

  if (!scan.has_dgo_directory) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::missing_dgo_directory,
                   "The extracted disc does not contain a root DGO directory."));
  }
  if (scan.executables.empty()) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::missing_executable,
                   "The extracted disc does not contain a root game executable."));
  }
  if (scan.executables.size() != 1) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::ambiguous_executable,
                   "The extracted disc contains more than one root game executable."));
  }
  if (!layout.shouldHash || layout.files_extracted < 0 ||
      static_cast<size_t>(layout.files_extracted) != scan.file_count ||
      layout.hashes.size() != scan.file_count ||
      scan.executables.front().hash_index >= layout.hashes.size()) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::invalid_extraction_result,
                   "The ISO reader did not return one hash for every extracted file."));
  }
  if (scan.file_count > UINT32_MAX) {
    return ValidationResult<RevisionMatch>::failure(
        make_error(ValidationErrorCode::invalid_extraction_result,
                   "The extracted file count cannot be represented by the revision fingerprint."));
  }

  const auto& executable = scan.executables.front();
  Fingerprint fingerprint;
  fingerprint.serial = executable.serial;
  fingerprint.elf_hash = layout.hashes[executable.hash_index];
  fingerprint.contents_hash = aggregate_contents_hash(layout.hashes);
  fingerprint.file_count = static_cast<uint32_t>(scan.file_count);
  return match_supported_revision(fingerprint);
}

ValidationResult<std::filesystem::path> write_buildinfo_checkpoint(
    const RevisionMatch& match,
    const std::filesystem::path& staging_directory) {
  if (auto error = write_checkpoint_file(match, staging_directory)) {
    return ValidationResult<std::filesystem::path>::failure(std::move(*error));
  }
  return ValidationResult<std::filesystem::path>::success(staging_directory / "buildinfo.json");
}

ValidationResult<StagedExtraction> extract_and_validate(
    FILE* image,
    const std::filesystem::path& new_staging_directory,
    iso_file::Options options) {
  const auto caller_should_cancel = std::move(options.should_cancel);
  const auto caller_on_progress = std::move(options.on_progress);
  bool callback_failed = false;
  options.should_cancel = [&] {
    if (callback_failed) {
      return true;
    }
    if (!caller_should_cancel) {
      return false;
    }
    try {
      return caller_should_cancel();
    } catch (...) {
      callback_failed = true;
      return true;
    }
  };
  options.on_progress = [&](const iso_file::Progress& progress) {
    if (callback_failed || !caller_on_progress) {
      return;
    }
    try {
      caller_on_progress(progress);
    } catch (...) {
      callback_failed = true;
    }
  };
  options.hash_files = true;
  iso_file::OwnedStagingDirectory owned_staging;
  auto extracted =
      iso_file::extract_to_owned_staging(image, new_staging_directory, &owned_staging, options);
  if (!extracted) {
    const auto code = callback_failed ? ValidationErrorCode::callback_failed
                      : extracted.error().code == iso_file::ErrorCode::cancelled
                          ? ValidationErrorCode::cancelled
                          : ValidationErrorCode::iso_reader_failed;
    ValidationError error =
        make_error(code, "The ISO could not be extracted into a private staging directory.");
    error.reader_error = extracted.error();
    return ValidationResult<StagedExtraction>::failure(std::move(error));
  }

  if (options.should_cancel()) {
    const auto code =
        callback_failed ? ValidationErrorCode::callback_failed : ValidationErrorCode::cancelled;
    return ValidationResult<StagedExtraction>::failure(
        with_cleanup(make_error(code, callback_failed ? "A disc-validation callback failed."
                                                      : "Disc validation was cancelled."),
                     &owned_staging));
  }

  auto matched = validate_extracted_layout(extracted.value());
  if (!matched) {
    return ValidationResult<StagedExtraction>::failure(
        with_cleanup(matched.error(), &owned_staging));
  }
  auto match = matched.take_value();
  auto files = validated_file_identities(extracted.value());
  if (!files || files.value().size() != match.fingerprint.file_count) {
    return ValidationResult<StagedExtraction>::failure(with_cleanup(
        make_error(ValidationErrorCode::invalid_extraction_result,
                   "The ISO reader did not return an exact extracted-file identity manifest."),
        &owned_staging));
  }

  if (options.should_cancel()) {
    const auto code =
        callback_failed ? ValidationErrorCode::callback_failed : ValidationErrorCode::cancelled;
    return ValidationResult<StagedExtraction>::failure(
        with_cleanup(make_error(code, callback_failed ? "A disc-validation callback failed."
                                                      : "Disc validation was cancelled."),
                     &owned_staging));
  }
  if (auto checkpoint_error = write_checkpoint_file_at(match, &owned_staging)) {
    return ValidationResult<StagedExtraction>::failure(
        with_cleanup(std::move(*checkpoint_error), &owned_staging));
  }
  if (!owned_staging.is_linked()) {
    return ValidationResult<StagedExtraction>::failure(
        with_cleanup(make_error(ValidationErrorCode::invalid_extraction_result,
                                "The exact validated staging directory changed before completion."),
                     &owned_staging));
  }

  owned_staging.keep();
  return ValidationResult<StagedExtraction>::success(
      {std::move(match), new_staging_directory, files.take_value()});
}

const char* validation_error_code_name(ValidationErrorCode code) {
  switch (code) {
    case ValidationErrorCode::iso_reader_failed:
      return "iso_reader_failed";
    case ValidationErrorCode::cancelled:
      return "cancelled";
    case ValidationErrorCode::callback_failed:
      return "callback_failed";
    case ValidationErrorCode::invalid_extraction_result:
      return "invalid_extraction_result";
    case ValidationErrorCode::missing_dgo_directory:
      return "missing_dgo_directory";
    case ValidationErrorCode::missing_executable:
      return "missing_executable";
    case ValidationErrorCode::ambiguous_executable:
      return "ambiguous_executable";
    case ValidationErrorCode::unsupported_serial:
      return "unsupported_serial";
    case ValidationErrorCode::unsupported_executable:
      return "unsupported_executable";
    case ValidationErrorCode::unexpected_file_count:
      return "unexpected_file_count";
    case ValidationErrorCode::contents_hash_mismatch:
      return "contents_hash_mismatch";
    case ValidationErrorCode::checkpoint_write_failed:
      return "checkpoint_write_failed";
  }
  return "unknown";
}

}  // namespace jak2_iso
