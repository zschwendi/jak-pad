#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "common/custom_data/CheckedFileIdentity.h"
#include "common/util/read_iso_file.h"
#include "common/versions/jak2_iso_revisions.h"

namespace jak2_iso {

enum class ValidationErrorCode {
  iso_reader_failed,
  cancelled,
  callback_failed,
  invalid_extraction_result,
  missing_dgo_directory,
  missing_executable,
  ambiguous_executable,
  unsupported_serial,
  unsupported_executable,
  unexpected_file_count,
  contents_hash_mismatch,
  checkpoint_write_failed,
};

struct ValidationError {
  ValidationErrorCode code = ValidationErrorCode::invalid_extraction_result;
  std::string message;
  std::optional<iso_file::Error> reader_error;
  std::optional<std::string> cleanup_error;
};

template <typename T>
class ValidationResult {
 public:
  static ValidationResult success(T value) {
    ValidationResult result;
    result.m_value.emplace(std::move(value));
    return result;
  }

  static ValidationResult failure(ValidationError error) {
    ValidationResult result;
    result.m_error.emplace(std::move(error));
    return result;
  }

  explicit operator bool() const { return m_value.has_value(); }
  const T& value() const { return *m_value; }
  T take_value() { return std::move(*m_value); }
  const ValidationError& error() const { return *m_error; }

 private:
  std::optional<T> m_value;
  std::optional<ValidationError> m_error;
};

struct Fingerprint {
  std::string serial;
  uint64_t elf_hash = 0;
  uint64_t contents_hash = 0;
  uint32_t file_count = 0;
};

struct RevisionMatch {
  Fingerprint fingerprint;
  Revision revision;
};

struct StagedExtraction {
  RevisionMatch match;
  std::filesystem::path staging_directory;
  std::vector<checked_file_identity::Identity> files = {};
};

uint64_t aggregate_contents_hash(std::span<const uint64_t> file_hashes);

ValidationResult<RevisionMatch> match_supported_revision(const Fingerprint& fingerprint);

/// Validate the result returned by iso_file::extract_to_staging. IsoFile::hashes must contain one
/// digest per file in the reader's depth-first, on-disc record order.
ValidationResult<RevisionMatch> validate_extracted_layout(const IsoFile& layout);

/// Bind the reader's depth-first file hashes to the exact normalized relative paths it extracted.
ValidationResult<std::vector<checked_file_identity::Identity>> validated_file_identities(
    const IsoFile& layout);

/// Atomically add the desktop-compatible buildinfo.json checkpoint to a validated staging tree.
ValidationResult<std::filesystem::path> write_buildinfo_checkpoint(
    const RevisionMatch& match,
    const std::filesystem::path& staging_directory);

/// POSIX builds bind staging cleanup and validation to creation-owned descriptors. Windows keeps
/// the portable path-based staging behavior and does not provide POSIX descriptor hardening.
ValidationResult<StagedExtraction> extract_and_validate(
    FILE* image,
    const std::filesystem::path& new_staging_directory,
    iso_file::Options options = {});

const char* validation_error_code_name(ValidationErrorCode code);

}  // namespace jak2_iso
