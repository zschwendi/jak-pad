#include "Jak1OutputMaterializer.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

#include "common/util/PosixFile.h"
#include "common/versions/jak2_iso_revisions.h"
#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"
#include "decompiler/extractor/jak1_retail_object_catalog.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_output_materializer {
namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kDgoHeaderBytes = 64;

using OutputMap = std::unordered_map<std::string, checked_file_identity::Identity>;

Error make_error(ErrorCode code,
                 std::string message,
                 std::optional<std::uint32_t> archive_index = {},
                 std::optional<std::uint32_t> object_index = {}) {
  return {code, std::move(message), archive_index, object_index};
}

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

std::optional<Error> cancelled(const Options& options,
                               std::optional<std::uint32_t> archive_index = {},
                               std::optional<std::uint32_t> object_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(
          ErrorCode::cancelled,
          std::string(options.wire_game == jak1_output_recipe::WireGame::jak1 ? "Jak 1"
                                                                              : "Jak II") +
              " output materialization was cancelled.",
          archive_index, object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The materializer cancellation callback failed.",
                      archive_index, object_index);
  }
  return {};
}

std::optional<Error> report(const Options& options,
                            Phase phase,
                            std::uint32_t completed,
                            std::uint32_t total,
                            std::uint64_t bytes_written,
                            std::string current_item = {}) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress({phase, completed, total, bytes_written, std::move(current_item)});
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The materializer progress callback failed.");
  }
  return {};
}

bool valid_name(std::string_view value, std::uint32_t cap) {
  if (value.empty() || value.size() > cap || value == "." || value == ".." || value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

bool safe_relative_path(std::string_view value, std::uint32_t cap) {
  if (value.empty() || value.size() > cap || value.front() == '/') {
    return false;
  }
  std::size_t component_start = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size()) {
      const auto byte = static_cast<unsigned char>(value[index]);
      if (byte < 0x21 || byte > 0x7e || value[index] == '\\' || value[index] == ':') {
        return false;
      }
      if (value[index] != '/') {
        continue;
      }
    }
    const auto component = value.substr(component_start, index - component_start);
    if (component.empty() || component == "." || component == ".." || component.back() == '.') {
      return false;
    }
    component_start = index + 1;
  }
  return true;
}

std::string collision_key(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
  return result;
}

using IdentityMap =
    std::unordered_map<std::string, const checked_file_identity::Identity*>;

Result<IdentityMap> index_file_identities(
    std::span<const checked_file_identity::Identity> identities,
    std::size_t cap,
    const Options& options) {
  if (identities.size() > cap) {
    return Result<IdentityMap>::failure(make_error(
        ErrorCode::input_identity_mismatch,
        "A validated file-identity manifest exceeds its entry cap."));
  }
  IdentityMap indexed;
  indexed.reserve(identities.size());
  for (const auto& identity : identities) {
    if (!safe_relative_path(identity.relative_path, options.limits.max_path_bytes) ||
        !indexed.emplace(collision_key(identity.relative_path), &identity).second) {
      return Result<IdentityMap>::failure(make_error(
          ErrorCode::input_identity_mismatch,
          "A validated file-identity manifest is unsafe or ambiguous."));
    }
  }
  return Result<IdentityMap>::success(std::move(indexed));
}

Result<const checked_file_identity::Identity*> required_identity(
    const IdentityMap& identities,
    std::string_view relative_path,
    const Options& options) {
  const auto found = identities.find(collision_key(relative_path));
  if (found != identities.end() && found->second->relative_path == relative_path) {
    return Result<const checked_file_identity::Identity*>::success(found->second);
  }
  if (options.require_validated_file_identities || !identities.empty()) {
    return Result<const checked_file_identity::Identity*>::failure(make_error(
        ErrorCode::input_identity_mismatch,
        "A required input has no exact validated file identity."));
  }
  return Result<const checked_file_identity::Identity*>::success(nullptr);
}

std::optional<Error> inspect_parent_identity(const fs::path& path,
                                             const posix_file::Identity& expected) {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      !posix_file::same_identity(status, expected)) {
    return make_error(ErrorCode::stage_install_failed,
                      "The output parent directory changed during materialization.");
  }
  return {};
}

std::optional<Error> remove_direct_entries(int directory) {
  auto enumeration = posix_file::open_directory_at(directory, ".");
  if (!enumeration) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "Could not reopen an owned output directory for cleanup.");
  }
  DIR* stream = ::fdopendir(enumeration.release());
  if (!stream) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "Could not enumerate an owned output directory for cleanup.");
  }
  int read_error = 0;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream);
    if (!entry) {
      read_error = errno;
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status {};
    if (::fstatat(directory, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) != 0 ||
        S_ISDIR(status.st_mode)) {
      ::closedir(stream);
      return make_error(ErrorCode::stage_cleanup_failed,
                        "An owned output directory changed or gained a nested directory.");
    }
    const posix_file::Identity identity{status.st_dev, status.st_ino};
    if (!posix_file::entry_identity(directory, entry->d_name, identity) ||
        ::unlinkat(directory, entry->d_name, 0) != 0) {
      ::closedir(stream);
      return make_error(ErrorCode::stage_cleanup_failed,
                        "An owned staged output changed during cleanup.");
    }
  }
  ::closedir(stream);
  if (read_error != 0) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "Could not completely enumerate an owned output directory.");
  }
  return {};
}

struct OutputStage {
  fs::path parent_path;
  std::string destination_name;
  std::string stage_name;
  posix_file::OwnedFd parent;
  posix_file::OwnedFd root;
  posix_file::OwnedFd iso;
  posix_file::OwnedFd fr3;
  posix_file::Identity parent_identity;
  posix_file::Identity root_identity;
  posix_file::Identity iso_identity;
  posix_file::Identity fr3_identity;
  bool linked = false;
  bool iso_linked = false;
  bool fr3_linked = false;
  bool installed = false;

  std::optional<Error> cleanup() {
    if (!linked || installed) {
      return {};
    }
    if (!root) {
      root = posix_file::open_directory_at(parent.get(), stage_name);
      posix_file::Identity current;
      if (!root || !posix_file::descriptor_identity(root.get(), &current) ||
          current.device != root_identity.device || current.inode != root_identity.inode) {
        return make_error(ErrorCode::stage_cleanup_failed,
                          "Could not retain the exact owned materializer stage for cleanup.");
      }
    }
    if (iso_linked && !iso) {
      iso = posix_file::open_directory_at(root.get(), "iso");
      posix_file::Identity current;
      if (!iso || !posix_file::descriptor_identity(iso.get(), &current) ||
          current.device != iso_identity.device || current.inode != iso_identity.inode) {
        return make_error(ErrorCode::stage_cleanup_failed,
                          "Could not retain the exact owned ISO stage for cleanup.");
      }
    }
    if (fr3_linked && !fr3) {
      fr3 = posix_file::open_directory_at(root.get(), "fr3");
      posix_file::Identity current;
      if (!fr3 || !posix_file::descriptor_identity(fr3.get(), &current) ||
          current.device != fr3_identity.device || current.inode != fr3_identity.inode) {
        return make_error(ErrorCode::stage_cleanup_failed,
                          "Could not retain the exact owned FR3 stage for cleanup.");
      }
    }
    if (iso_linked) {
      if (const auto error = remove_direct_entries(iso.get())) {
        return error;
      }
      if (!posix_file::entry_identity(root.get(), "iso", iso_identity) ||
          ::unlinkat(root.get(), "iso", AT_REMOVEDIR) != 0) {
        return make_error(ErrorCode::stage_cleanup_failed,
                          "Could not remove the exact owned ISO stage.");
      }
      iso_linked = false;
    }
    if (fr3_linked) {
      if (const auto error = remove_direct_entries(fr3.get())) {
        return error;
      }
      if (!posix_file::entry_identity(root.get(), "fr3", fr3_identity) ||
          ::unlinkat(root.get(), "fr3", AT_REMOVEDIR) != 0) {
        return make_error(ErrorCode::stage_cleanup_failed,
                          "Could not remove the exact owned FR3 stage.");
      }
      fr3_linked = false;
    }
    if (!posix_file::entry_identity(parent.get(), stage_name, root_identity) ||
        ::unlinkat(parent.get(), stage_name.c_str(), AT_REMOVEDIR) != 0) {
      return make_error(ErrorCode::stage_cleanup_failed,
                        "Could not remove the exact owned materializer stage.");
    }
    linked = false;
    return {};
  }

  std::optional<Error> install() {
    if (!linked || !iso_linked || !fr3_linked || installed ||
        inspect_parent_identity(parent_path, parent_identity) ||
        !posix_file::entry_identity(parent.get(), stage_name, root_identity) ||
        !posix_file::entry_identity(root.get(), "iso", iso_identity) ||
        !posix_file::entry_identity(root.get(), "fr3", fr3_identity)) {
      return make_error(ErrorCode::stage_install_failed,
                        "The exact owned materializer stage changed before installation.");
    }
    if (posix_file::exclusive_rename_at(parent.get(), stage_name, parent.get(),
                                        destination_name) != 0 ||
        !posix_file::entry_identity(parent.get(), destination_name, root_identity)) {
      return make_error(ErrorCode::stage_install_failed,
                        "Could not exclusively install the exact materializer stage.");
    }
    installed = true;
    linked = false;
    return {};
  }
};

std::optional<Error> create_output_stage(const fs::path& destination, OutputStage* stage) {
  if (!stage || !destination.is_absolute() || destination.filename().empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "The output destination is not a safe absolute path.");
  }
  stage->parent_path = destination.parent_path();
  stage->destination_name = destination.filename().string();
  stage->stage_name = stage->destination_name + ".stage";
  if (!valid_name(stage->destination_name, 255) || !valid_name(stage->stage_name, 255)) {
    return make_error(ErrorCode::invalid_argument,
                      "The output destination basename is unsafe.");
  }
  stage->parent = posix_file::open_directory(stage->parent_path.c_str());
  if (!stage->parent ||
      !posix_file::descriptor_identity(stage->parent.get(), &stage->parent_identity)) {
    return make_error(ErrorCode::destination_inspection_failed,
                      "Could not open the direct output parent directory.");
  }
  struct stat destination_status {};
  if (::fstatat(stage->parent.get(), stage->destination_name.c_str(), &destination_status,
                AT_SYMLINK_NOFOLLOW) == 0) {
    return make_error(ErrorCode::destination_exists, "The output destination already exists.");
  }
  if (errno != ENOENT) {
    return make_error(ErrorCode::destination_inspection_failed,
                      "Could not inspect the output destination.");
  }
  if (::mkdirat(stage->parent.get(), stage->stage_name.c_str(), 0700) != 0) {
    return make_error(errno == EEXIST ? ErrorCode::destination_exists
                                      : ErrorCode::stage_create_failed,
                      "The private output stage already exists or could not be created.");
  }
  stage->linked = true;
  struct stat root_status {};
  if (::fstatat(stage->parent.get(), stage->stage_name.c_str(), &root_status,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(root_status.st_mode)) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not retain the exact output-stage identity.");
  }
  stage->root_identity = {root_status.st_dev, root_status.st_ino};
  stage->root = posix_file::open_directory_at(stage->parent.get(), stage->stage_name);
  posix_file::Identity opened_root;
  if (!stage->root || !posix_file::descriptor_identity(stage->root.get(), &opened_root) ||
      opened_root.device != stage->root_identity.device ||
      opened_root.inode != stage->root_identity.inode) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not open the exact descriptor-owned output stage.");
  }
  if (::mkdirat(stage->root.get(), "iso", 0700) != 0) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not create the descriptor-owned ISO output directory.");
  }
  stage->iso_linked = true;
  struct stat iso_status {};
  if (::fstatat(stage->root.get(), "iso", &iso_status, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(iso_status.st_mode)) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not retain the exact ISO output identity.");
  }
  stage->iso_identity = {iso_status.st_dev, iso_status.st_ino};
  stage->iso = posix_file::open_directory_at(stage->root.get(), "iso");
  posix_file::Identity opened_iso;
  if (!stage->iso || !posix_file::descriptor_identity(stage->iso.get(), &opened_iso) ||
      opened_iso.device != stage->iso_identity.device ||
      opened_iso.inode != stage->iso_identity.inode) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not open the exact descriptor-owned ISO output directory.");
  }
  if (::mkdirat(stage->root.get(), "fr3", 0700) != 0) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not create the descriptor-owned FR3 output directory.");
  }
  stage->fr3_linked = true;
  struct stat fr3_status {};
  if (::fstatat(stage->root.get(), "fr3", &fr3_status, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISDIR(fr3_status.st_mode)) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not retain the exact FR3 output identity.");
  }
  stage->fr3_identity = {fr3_status.st_dev, fr3_status.st_ino};
  stage->fr3 = posix_file::open_directory_at(stage->root.get(), "fr3");
  posix_file::Identity opened_fr3;
  if (!stage->fr3 || !posix_file::descriptor_identity(stage->fr3.get(), &opened_fr3) ||
      opened_fr3.device != stage->fr3_identity.device ||
      opened_fr3.inode != stage->fr3_identity.inode) {
    return make_error(ErrorCode::stage_create_failed,
                      "Could not open the exact descriptor-owned FR3 output directory.");
  }
  return {};
}

bool known_revision(const jak1_output_recipe::RevisionProvenance& revision,
                    jak1_output_recipe::WireGame game) {
  if (game == jak1_output_recipe::WireGame::jak1) {
    const auto revisions = jak1_iso::supported_revisions();
    return std::any_of(revisions.begin(), revisions.end(), [&](const auto& known) {
      return revision.serial == known.serial && revision.executable_hash == known.elf_hash &&
             revision.contents_hash == known.contents_hash &&
             revision.file_count == known.file_count &&
             revision.config_version == known.decomp_config_version &&
             revision.territory == known.territory && revision.black_label == known.black_label;
    });
  }
  if (game != jak1_output_recipe::WireGame::jak2) {
    return false;
  }
  const auto& known = jak2_iso::import_revision();
  return revision.serial == known.serial && revision.executable_hash == known.elf_hash &&
         revision.contents_hash == known.contents_hash && revision.file_count == known.file_count &&
         revision.config_version == known.decomp_config_version &&
         static_cast<int>(revision.territory) == static_cast<int>(known.territory) &&
         !revision.black_label;
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_recipe_bytes > 0 && limits.max_source_object_bytes > 0 &&
         limits.max_generated_object_bytes > 0 && limits.max_retail_archive_bytes > 0 &&
         limits.max_flat_file_bytes > 0 && limits.max_fr3_file_bytes > 0 &&
         limits.max_total_output_bytes > 0 && limits.max_generated_objects > 0 &&
         limits.max_generated_flat_files > 0 && limits.max_path_bytes > 0 &&
         limits.max_name_bytes > 0 && limits.max_validated_extracted_files > 0 &&
         limits.max_validated_fr3_files > 0 && limits.io_chunk_bytes > 0 &&
         (!options.compressed_trailing_alignment_bytes ||
          *options.compressed_trailing_alignment_bytes > 0) &&
         (!options.expected_recipe_bytes ||
          (!options.expected_recipe_bytes->empty() &&
           options.expected_recipe_bytes->size() <= limits.max_recipe_bytes)) &&
         options.expected_source_object_pack.object_count > 0 &&
         options.expected_source_object_pack.aggregate_xxh64 != 0 &&
         known_revision(options.expected_revision, options.wire_game);
}

ErrorCode map_recipe_error(jak1_output_recipe::ErrorCode code) {
  switch (code) {
    case jak1_output_recipe::ErrorCode::cancelled:
      return ErrorCode::cancelled;
    case jak1_output_recipe::ErrorCode::callback_failed:
      return ErrorCode::callback_failed;
    case jak1_output_recipe::ErrorCode::unsupported_revision:
      return ErrorCode::revision_mismatch;
    case jak1_output_recipe::ErrorCode::wrong_source_pack:
      return ErrorCode::source_pack_mismatch;
    default:
      return ErrorCode::recipe_invalid;
  }
}

ErrorCode map_writer_error(jak1_checked_dgo_writer::ErrorCode code) {
  switch (code) {
    case jak1_checked_dgo_writer::ErrorCode::cancelled:
      return ErrorCode::cancelled;
    case jak1_checked_dgo_writer::ErrorCode::callback_failed:
      return ErrorCode::callback_failed;
    case jak1_checked_dgo_writer::ErrorCode::output_size_limit_exceeded:
      return ErrorCode::output_limit_exceeded;
    default:
      return ErrorCode::dgo_write_failed;
  }
}

std::size_t size_cap(std::uint64_t value) {
  return value > std::numeric_limits<std::size_t>::max() ? std::numeric_limits<std::size_t>::max()
                                                         : static_cast<std::size_t>(value);
}

Result<fs::path> resolve_regular_file(const fs::path& root,
                                      std::string_view relative,
                                      const Options& options) {
  if (!root.is_absolute() || !safe_relative_path(relative, options.limits.max_path_bytes)) {
    return Result<fs::path>::failure(
        make_error(ErrorCode::unsafe_path, "An input path is not a safe absolute-root path."));
  }
  std::error_code error;
  auto current = root;
  const auto root_status = fs::symlink_status(current, error);
  if (error || root_status.type() != fs::file_type::directory) {
    return Result<fs::path>::failure(make_error(
        error ? ErrorCode::input_missing : ErrorCode::input_not_regular,
        "An input root is missing, a symbolic link, or not a directory: " + root.string()));
  }
  for (const auto& component : fs::path(relative)) {
    current /= component;
    const auto status = fs::symlink_status(current, error);
    if (error || status.type() == fs::file_type::not_found) {
      return Result<fs::path>::failure(
          make_error(ErrorCode::input_missing, "A required input is missing: " + current.string()));
    }
    if (status.type() == fs::file_type::symlink) {
      return Result<fs::path>::failure(
          make_error(ErrorCode::unsafe_path,
                     "A required input path contains a symbolic link: " + current.string()));
    }
  }
  const auto final_status = fs::symlink_status(current, error);
  if (error || final_status.type() != fs::file_type::regular) {
    return Result<fs::path>::failure(
        make_error(ErrorCode::input_not_regular,
                   "A required input is not a regular file: " + current.string()));
  }
  return Result<fs::path>::success(std::move(current));
}

Result<std::vector<std::uint8_t>> read_file(const fs::path& path,
                                            std::uint64_t cap,
                                            const Options& options) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error || status.type() == fs::file_type::not_found) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_missing, "A required input is missing: " + path.string()));
  }
  if (status.type() != fs::file_type::regular) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(status.type() == fs::file_type::symlink ? ErrorCode::unsafe_path
                                                           : ErrorCode::input_not_regular,
                   "A required input is not a regular file: " + path.string()));
  }
  const auto size = fs::file_size(path, error);
  if (error) {
    return Result<std::vector<std::uint8_t>>::failure(make_error(
        ErrorCode::input_read_failed, "Could not inspect an input file: " + path.string()));
  }
  if (size == 0 || size > cap || size > std::numeric_limits<std::size_t>::max()) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_too_large,
                   "An input file is empty or exceeds its configured cap: " + path.string()));
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_read_failed, "Could not open an input file: " + path.string()));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (const auto error_result = cancelled(options)) {
      return Result<std::vector<std::uint8_t>>::failure(*error_result);
    }
    const auto chunk = std::min(options.limits.io_chunk_bytes, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::input_read_failed,
                     "Could not completely read an input file: " + path.string()));
    }
    offset += chunk;
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<std::vector<std::uint8_t>> read_verified_file(const fs::path& root,
                                                     std::string_view relative,
                                                     std::uint64_t expected_size,
                                                     std::uint64_t expected_hash,
                                                     std::uint64_t cap,
                                                     ErrorCode mismatch_code,
                                                     const Options& options) {
  auto resolved = resolve_regular_file(root, relative, options);
  if (!resolved) {
    return Result<std::vector<std::uint8_t>>::failure(resolved.error());
  }
  auto bytes = read_file(resolved.value(), cap, options);
  if (!bytes) {
    return bytes;
  }
  const auto hash = XXH64(bytes.value().data(), bytes.value().size(), 0);
  if (bytes.value().size() != expected_size || hash != expected_hash) {
    return Result<std::vector<std::uint8_t>>::failure(make_error(
        mismatch_code,
        "An input artifact does not match its checked size and hash: " + std::string(relative)));
  }
  return bytes;
}

std::optional<Error> reserve_output(std::uint64_t size,
                                    std::uint64_t* total,
                                    const Options& options) {
  std::uint64_t next = 0;
  if (!checked_add(*total, size, &next) || next > options.limits.max_total_output_bytes) {
    return make_error(ErrorCode::output_limit_exceeded,
                      "The materialized output exceeds its configured size cap.");
  }
  *total = next;
  return {};
}

std::optional<Error> hash_descriptor(int descriptor,
                                     std::uint64_t size,
                                     std::uint64_t* result) {
  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);
  std::vector<std::uint8_t> buffer(64 * 1024);
  std::uint64_t offset = 0;
  while (offset < size) {
    const auto chunk = static_cast<std::size_t>(
        std::min<std::uint64_t>(buffer.size(), size - offset));
    const auto count = ::pread(descriptor, buffer.data(), chunk, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count != static_cast<ssize_t>(chunk)) {
      return make_error(ErrorCode::output_write_failed,
                        "Could not re-read an exact staged output.");
    }
    XXH64_update(&hash_state, buffer.data(), chunk);
    offset += chunk;
  }
  *result = XXH64_digest(&hash_state);
  return {};
}

std::optional<Error> remove_owned_file(int directory,
                                       std::string_view name,
                                       const posix_file::Identity& identity) {
  if (!posix_file::entry_identity(directory, name, identity)) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "A staged output changed before cleanup.");
  }
  const std::string owned_name(name);
  if (::unlinkat(directory, owned_name.c_str(), 0) != 0) {
    return make_error(ErrorCode::stage_cleanup_failed,
                      "Could not remove an exact staged output.");
  }
  return {};
}

std::optional<Error> copy_file_at(
    const fs::path& source,
    int destination_directory,
    std::string_view destination_name,
    std::uint64_t cap,
    const std::optional<std::pair<std::uint64_t, std::uint64_t>>& expected,
    ErrorCode mismatch_code,
    std::uint64_t* total_output,
    const Options& options,
    checked_file_identity::Identity* produced) {
  std::error_code error;
  const auto size = fs::file_size(source, error);
  if (error || size == 0 || size > cap) {
    return make_error(error ? ErrorCode::input_read_failed : ErrorCode::input_too_large,
                      "A copied input is unreadable, empty, or too large: " + source.string());
  }
  if (expected && expected->first != size) {
    return make_error(mismatch_code, "A copied artifact has the wrong size: " + source.string());
  }
  auto next_total = *total_output;
  if (const auto budget = reserve_output(size, &next_total, options)) {
    return budget;
  }
  const int input_descriptor = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  posix_file::OwnedFd input(input_descriptor);
  auto output = posix_file::open_file_at(
      destination_directory, destination_name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (!input || !output) {
    return make_error(ErrorCode::output_write_failed,
                      "Could not open a materializer copy input or output.");
  }
  posix_file::Identity output_identity;
  struct stat output_status {};
  if (!posix_file::descriptor_identity(output.get(), &output_identity, &output_status) ||
      !S_ISREG(output_status.st_mode) || output_status.st_nlink != 1) {
    return make_error(ErrorCode::output_write_failed,
                      "The copied output is not a private regular file.");
  }
  XXH64_state_t hash_state;
  XXH64_reset(&hash_state, 0);
  std::vector<char> buffer(options.limits.io_chunk_bytes);
  std::uint64_t copied = 0;
  while (copied < size) {
    if (const auto error_result = cancelled(options)) {
      return error_result;
    }
    const auto chunk =
        static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - copied));
    const auto read_count = ::read(input.get(), buffer.data(), chunk);
    if (read_count < 0 && errno == EINTR) {
      continue;
    }
    if (read_count != static_cast<ssize_t>(chunk)) {
      return make_error(ErrorCode::input_read_failed,
                        "Could not completely read a copied input: " + source.string());
    }
    std::size_t written = 0;
    while (written < chunk) {
      const auto write_count =
          ::write(output.get(), buffer.data() + written, chunk - written);
      if (write_count < 0 && errno == EINTR) {
        continue;
      }
      if (write_count <= 0) {
        return make_error(ErrorCode::output_write_failed,
                          "Could not write a staged output.");
      }
      written += static_cast<std::size_t>(write_count);
    }
    XXH64_update(&hash_state, buffer.data(), chunk);
    copied += chunk;
  }
  if (::fsync(output.get()) != 0) {
    return make_error(ErrorCode::output_write_failed,
                      "Could not synchronize a staged output.");
  }
  const auto copied_hash = XXH64_digest(&hash_state);
  if (expected && copied_hash != expected->second) {
    return make_error(mismatch_code, "A copied artifact has the wrong hash: " + source.string());
  }
  struct stat final_status {};
  std::uint64_t installed_hash = 0;
  if (!posix_file::descriptor_identity(output.get(), nullptr, &final_status) ||
      !posix_file::entry_identity(destination_directory, destination_name, output_identity) ||
      !S_ISREG(final_status.st_mode) || final_status.st_nlink != 1 ||
      final_status.st_size != static_cast<off_t>(size) ||
      hash_descriptor(output.get(), size, &installed_hash) || installed_hash != copied_hash) {
    const auto cleanup = remove_owned_file(destination_directory, destination_name, output_identity);
    return cleanup ? cleanup
                   : std::optional<Error>(make_error(
                         ErrorCode::output_write_failed,
                         "A staged output changed while it was descriptor-validated."));
  }
  *total_output = next_total;
  if (produced) {
    *produced = {std::string(destination_name), size, copied_hash};
  }
  return {};
}

std::optional<Error> validate_output_directory(int directory,
                                               const OutputMap& expected,
                                               std::uint64_t file_cap,
                                               std::uint32_t name_cap) {
  auto enumeration = posix_file::open_directory_at(directory, ".");
  if (!enumeration) {
    return make_error(ErrorCode::output_write_failed,
                      "Could not reopen a staged output directory.");
  }
  DIR* stream = ::fdopendir(enumeration.release());
  if (!stream) {
    return make_error(ErrorCode::output_write_failed,
                      "Could not enumerate a staged output directory.");
  }
  std::unordered_set<std::string> found;
  int read_error = 0;
  while (true) {
    errno = 0;
    const auto* entry = ::readdir(stream);
    if (!entry) {
      read_error = errno;
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!valid_name(name, name_cap) || !found.emplace(collision_key(name)).second) {
      ::closedir(stream);
      return make_error(ErrorCode::output_write_failed,
                        "A staged output directory contains an unsafe entry.");
    }
    const auto expected_entry = expected.find(collision_key(name));
    if (expected_entry == expected.end() || expected_entry->second.relative_path != name ||
        expected_entry->second.size == 0 || expected_entry->second.size > file_cap) {
      ::closedir(stream);
      return make_error(ErrorCode::output_write_failed,
                        "A staged output directory differs from its exact manifest.");
    }
    auto file = posix_file::open_file_at(directory, name, O_RDONLY);
    posix_file::Identity identity;
    struct stat before {};
    if (!file || !posix_file::descriptor_identity(file.get(), &identity, &before) ||
        !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        before.st_size != static_cast<off_t>(expected_entry->second.size)) {
      ::closedir(stream);
      return make_error(ErrorCode::output_write_failed,
                        "A staged output is not the expected private regular file.");
    }
    std::uint64_t hash = 0;
    struct stat after {};
    if (hash_descriptor(file.get(), expected_entry->second.size, &hash) ||
        hash != expected_entry->second.xxh64 ||
        !posix_file::descriptor_identity(file.get(), nullptr, &after) ||
        !posix_file::same_identity(after, identity) || before.st_size != after.st_size ||
        !posix_file::entry_identity(directory, name, identity)) {
      ::closedir(stream);
      return make_error(ErrorCode::output_write_failed,
                        "A staged output changed during final descriptor validation.");
    }
  }
  ::closedir(stream);
  if (read_error != 0 || found.size() != expected.size()) {
    return make_error(ErrorCode::output_write_failed,
                      "A staged output directory is incomplete or changed during enumeration.");
  }
  return {};
}

std::optional<Error> validate_stage(const OutputStage& stage,
                                    const OutputMap& iso_outputs,
                                    const OutputMap& fr3_outputs,
                                    const Options& options) {
  if (inspect_parent_identity(stage.parent_path, stage.parent_identity) ||
      !posix_file::entry_identity(stage.parent.get(), stage.stage_name, stage.root_identity) ||
      !posix_file::entry_identity(stage.root.get(), "iso", stage.iso_identity) ||
      !posix_file::entry_identity(stage.root.get(), "fr3", stage.fr3_identity)) {
    return make_error(ErrorCode::output_write_failed,
                      "The descriptor-owned materializer stage changed before validation.");
  }
  if (const auto error = validate_output_directory(
          stage.iso.get(), iso_outputs, options.limits.max_total_output_bytes,
          options.limits.max_name_bytes)) {
    return error;
  }
  return validate_output_directory(stage.fr3.get(), fr3_outputs,
                                   options.limits.max_fr3_file_bytes,
                                   options.limits.max_name_bytes);
}

std::string generated_object_key(jak1_output_recipe::GeneratedDataKind kind,
                                 std::string_view name) {
  return std::to_string(static_cast<unsigned>(kind)) + ":" + collision_key(name);
}

std::string generated_flat_key(jak1_output_recipe::GeneratedFlatFileKind kind,
                               std::string_view name) {
  return std::to_string(static_cast<unsigned>(kind)) + ":" + collision_key(name);
}

bool known_generated_kind(jak1_output_recipe::GeneratedDataKind kind) {
  return kind >= jak1_output_recipe::GeneratedDataKind::directory_tpages &&
         kind <= jak1_output_recipe::GeneratedDataKind::custom_level;
}

bool known_generated_flat_kind(jak1_output_recipe::GeneratedFlatFileKind kind) {
  return kind >= jak1_output_recipe::GeneratedFlatFileKind::game_text &&
         kind <= jak1_output_recipe::GeneratedFlatFileKind::game_subtitle;
}

std::optional<Error> cleanup_failure(Error error, OutputStage& stage) {
  if (const auto cleanup = stage.cleanup()) {
    auto cleanup_error = *cleanup;
    cleanup_error.message = error.message + " Cleanup also failed: " + cleanup_error.message;
    return cleanup_error;
  }
  return error;
}

struct LoadedRetailArchive {
  std::vector<std::uint8_t> raw;
  jak1_checked_dgo::Archive archive;
  jak1_retail_object_catalog::Catalog catalog;
};

Result<LoadedRetailArchive> load_retail_archive(const Inputs& inputs,
                                                std::string_view relative,
                                                const checked_file_identity::Identity* identity,
                                                const Options& options) {
  auto resolved = resolve_regular_file(inputs.extracted_iso_root, relative, options);
  if (!resolved) {
    return Result<LoadedRetailArchive>::failure(resolved.error());
  }
  auto raw = read_file(resolved.value(), options.limits.max_retail_archive_bytes, options);
  if (!raw) {
    return Result<LoadedRetailArchive>::failure(raw.error());
  }
  if (identity &&
      (raw.value().size() != identity->size ||
       XXH64(raw.value().data(), raw.value().size(), 0) != identity->xxh64)) {
    return Result<LoadedRetailArchive>::failure(make_error(
        ErrorCode::input_identity_mismatch,
        "A retail archive does not match its validated size and hash."));
  }

  const std::string source_path(relative);
  const jak1_retail_object_catalog::ArchiveSource catalog_source{source_path, raw.value()};
  jak1_retail_object_catalog::Options catalog_options;
  catalog_options.game_version = options.wire_game == jak1_output_recipe::WireGame::jak1
                                     ? GameVersion::Jak1
                                     : GameVersion::Jak2;
  catalog_options.max_archive_input_bytes = options.limits.max_retail_archive_bytes;
  catalog_options.max_archive_compressed_bytes = options.limits.max_retail_archive_bytes;
  catalog_options.compressed_trailing_alignment_bytes =
      options.compressed_trailing_alignment_bytes;
  catalog_options.should_cancel = options.should_cancel;
  auto catalog = jak1_retail_object_catalog::build(
      std::span<const jak1_retail_object_catalog::ArchiveSource>(&catalog_source, 1),
      catalog_options);
  if (!catalog) {
    return Result<LoadedRetailArchive>::failure(make_error(
        catalog.error().code == jak1_retail_object_catalog::ErrorCode::cancelled
            ? ErrorCode::cancelled
        : catalog.error().code == jak1_retail_object_catalog::ErrorCode::callback_failed
            ? ErrorCode::callback_failed
            : ErrorCode::retail_catalog_failed,
        "The checked retail catalog rejected " + source_path + ": " + catalog.error().message));
  }

  jak1_checked_dgo::Options dgo_options;
  dgo_options.game_version = options.wire_game == jak1_output_recipe::WireGame::jak1
                                 ? GameVersion::Jak1
                                 : GameVersion::Jak2;
  dgo_options.max_input_bytes = options.limits.max_retail_archive_bytes;
  dgo_options.max_compressed_bytes = options.limits.max_retail_archive_bytes;
  dgo_options.compressed_trailing_alignment_bytes =
      options.compressed_trailing_alignment_bytes;
  if (identity) {
    dgo_options.expected_input = *identity;
  }
  dgo_options.should_cancel = options.should_cancel;
  const auto archive_name = fs::path(relative).filename().string();
  auto archive = jak1_checked_dgo::read(raw.value(), archive_name, dgo_options);
  if (!archive) {
    return Result<LoadedRetailArchive>::failure(make_error(
        archive.error().code == jak1_checked_dgo::ErrorCode::cancelled
            ? ErrorCode::cancelled
        : archive.error().code == jak1_checked_dgo::ErrorCode::callback_failed
            ? ErrorCode::callback_failed
        : archive.error().code == jak1_checked_dgo::ErrorCode::input_identity_mismatch
            ? ErrorCode::input_identity_mismatch
            : ErrorCode::retail_archive_failed,
        "The checked DGO reader rejected " + source_path + ": " + archive.error().message));
  }
  return Result<LoadedRetailArchive>::success(
      {raw.take_value(), archive.take_value(), catalog.take_value()});
}

}  // namespace

Result<Summary> materialize(const Inputs& inputs,
                            const fs::path& destination_root,
                            const Options& options) {
  OutputStage stage;
  try {
    if (!valid_options(options) || !destination_root.is_absolute() ||
        destination_root.filename().empty() ||
        inputs.generated_objects.size() > options.limits.max_generated_objects ||
        inputs.generated_flat_files.size() > options.limits.max_generated_flat_files) {
      return Result<Summary>::failure(make_error(
          ErrorCode::invalid_argument, "The materializer inputs or options are invalid."));
    }
    if (const auto error = cancelled(options)) {
      return Result<Summary>::failure(*error);
    }
    if (const auto error = report(options, Phase::validating, 0, 1, 0)) {
      return Result<Summary>::failure(*error);
    }

    if ((options.expected_validated_extracted_file_count &&
         inputs.validated_extracted_files.size() !=
             *options.expected_validated_extracted_file_count) ||
        (options.expected_validated_fr3_file_count &&
         inputs.validated_fr3_files.size() != *options.expected_validated_fr3_file_count)) {
      return Result<Summary>::failure(make_error(
          ErrorCode::input_identity_mismatch,
          "A validated file-identity manifest has the wrong exact entry count."));
    }
    auto extracted_identities =
        index_file_identities(inputs.validated_extracted_files,
                              options.limits.max_validated_extracted_files, options);
    auto fr3_identities = index_file_identities(inputs.validated_fr3_files,
                                                options.limits.max_validated_fr3_files, options);
    if (!extracted_identities || !fr3_identities ||
        (options.require_validated_file_identities &&
         (extracted_identities.value().empty() || fr3_identities.value().empty()))) {
      return Result<Summary>::failure(
          !extracted_identities ? extracted_identities.error()
          : !fr3_identities    ? fr3_identities.error()
                               : make_error(ErrorCode::input_identity_mismatch,
                                            "Required validated file identities are missing."));
    }

    auto recipe_bytes = read_file(inputs.recipe_file, options.limits.max_recipe_bytes, options);
    if (!recipe_bytes) {
      return Result<Summary>::failure(recipe_bytes.error());
    }
    if (options.expected_recipe_bytes &&
        !std::equal(recipe_bytes.value().begin(), recipe_bytes.value().end(),
                    options.expected_recipe_bytes->begin(),
                    options.expected_recipe_bytes->end())) {
      return Result<Summary>::failure(make_error(
          ErrorCode::recipe_mismatch,
          "The safely read output recipe does not match the caller's exact expected bytes."));
    }
    jak1_output_recipe::Options recipe_options;
    recipe_options.limits = options.recipe_limits;
    recipe_options.expected_revision = options.expected_revision;
    recipe_options.expected_source_object_pack = options.expected_source_object_pack;
    recipe_options.wire_game = options.wire_game;
    recipe_options.should_cancel = options.should_cancel;
    auto decoded = jak1_output_recipe::decode(recipe_bytes.value(), recipe_options);
    if (!decoded) {
      return Result<Summary>::failure(
          make_error(map_recipe_error(decoded.error().code),
                     "The checked output recipe was rejected: " + decoded.error().message,
                     decoded.error().archive_index, decoded.error().object_index));
    }
    const auto& recipe = decoded.value();
    if (!(recipe.revision == options.expected_revision)) {
      return Result<Summary>::failure(make_error(
          ErrorCode::revision_mismatch, "The output recipe does not match the validated disc."));
    }
    if (!(recipe.source_object_pack == options.expected_source_object_pack)) {
      return Result<Summary>::failure(
          make_error(ErrorCode::source_pack_mismatch,
                     "The output recipe names the wrong source-object pack."));
    }

    std::unordered_map<std::string, const GeneratedObjectArtifact*> generated_objects;
    std::unordered_set<std::string> generated_paths;
    for (const auto& artifact : inputs.generated_objects) {
      const auto key = generated_object_key(artifact.kind, artifact.internal_name);
      if (!known_generated_kind(artifact.kind) ||
          !valid_name(artifact.internal_name, options.limits.max_name_bytes) ||
          !safe_relative_path(artifact.relative_path, options.limits.max_path_bytes) ||
          artifact.size == 0 || artifact.xxh64 == 0 ||
          !generated_objects.emplace(key, &artifact).second ||
          !generated_paths.emplace(collision_key(artifact.relative_path)).second) {
        return Result<Summary>::failure(make_error(ErrorCode::generated_catalog_invalid,
                                                   "The generated-object catalog is invalid."));
      }
    }
    std::unordered_map<std::string, const GeneratedFlatArtifact*> generated_flats;
    for (const auto& artifact : inputs.generated_flat_files) {
      const auto key = generated_flat_key(artifact.kind, artifact.destination_basename);
      if (!known_generated_flat_kind(artifact.kind) ||
          !valid_name(artifact.destination_basename, options.limits.max_name_bytes) ||
          !safe_relative_path(artifact.relative_path, options.limits.max_path_bytes) ||
          artifact.size == 0 || artifact.xxh64 == 0 ||
          !generated_flats.emplace(key, &artifact).second ||
          !generated_paths.emplace(collision_key(artifact.relative_path)).second) {
        return Result<Summary>::failure(make_error(ErrorCode::generated_catalog_invalid,
                                                   "The generated-flat catalog is invalid."));
      }
    }

    if (const auto error = create_output_stage(destination_root, &stage)) {
      return Result<Summary>::failure(stage.linked ? *cleanup_failure(*error, stage) : *error);
    }

    Summary summary;
    OutputMap iso_outputs;
    OutputMap fr3_outputs;
    std::unordered_set<std::string> used_generated_objects;
    for (std::uint32_t archive_index = 0; archive_index < recipe.archives.size(); ++archive_index) {
      const auto& archive_record = recipe.archives[archive_index];
      if (const auto error = cancelled(options, archive_index)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      if (const auto error = report(options, Phase::writing_archives, archive_index,
                                    static_cast<std::uint32_t>(recipe.archives.size()),
                                    summary.output_bytes, archive_record.destination_basename)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }

      struct MaterializedObject {
        std::string name;
        std::vector<std::uint8_t> bytes;
      };
      std::vector<MaterializedObject> objects;
      objects.reserve(archive_record.objects.size());
      std::unordered_map<std::string, LoadedRetailArchive> retail_archives;
      for (std::uint32_t object_index = 0; object_index < archive_record.objects.size();
           ++object_index) {
        const auto& object = archive_record.objects[object_index];
        if (const auto error = cancelled(options, archive_index, object_index)) {
          return Result<Summary>::failure(*cleanup_failure(*error, stage));
        }
        std::vector<std::uint8_t> bytes;
        if (std::holds_alternative<jak1_output_recipe::BundledSourceObject>(object.source)) {
          const auto& source = std::get<jak1_output_recipe::BundledSourceObject>(object.source);
          auto loaded =
              read_verified_file(inputs.source_object_pack_root, source.bundle_relative_path,
                                 source.size, source.xxh64, options.limits.max_source_object_bytes,
                                 ErrorCode::source_object_mismatch, options);
          if (!loaded) {
            auto error = loaded.error();
            error.archive_index = archive_index;
            error.object_index = object_index;
            return Result<Summary>::failure(*cleanup_failure(std::move(error), stage));
          }
          bytes = loaded.take_value();
        } else if (std::holds_alternative<jak1_output_recipe::VerifiedRetailObject>(
                       object.source)) {
          const auto& source = std::get<jak1_output_recipe::VerifiedRetailObject>(object.source);
          auto found = retail_archives.find(source.source_archive_relative_path);
          if (found == retail_archives.end()) {
            auto identity = required_identity(extracted_identities.value(),
                                              source.source_archive_relative_path, options);
            if (!identity) {
              auto error = identity.error();
              error.archive_index = archive_index;
              error.object_index = object_index;
              return Result<Summary>::failure(*cleanup_failure(std::move(error), stage));
            }
            auto loaded = load_retail_archive(inputs, source.source_archive_relative_path,
                                              identity.value(), options);
            if (!loaded) {
              auto error = loaded.error();
              error.archive_index = archive_index;
              error.object_index = object_index;
              return Result<Summary>::failure(*cleanup_failure(std::move(error), stage));
            }
            found =
                retail_archives.emplace(source.source_archive_relative_path, loaded.take_value())
                    .first;
          }
          auto& loaded = found->second;
          if (source.archive_object_index >= loaded.archive.objects.size()) {
            return Result<Summary>::failure(
                *cleanup_failure(make_error(ErrorCode::retail_object_mismatch,
                                            "A retail object index is outside its checked archive.",
                                            archive_index, object_index),
                                 stage));
          }
          const auto& retail_object = loaded.archive.objects[source.archive_object_index];
          jak1_retail_object_catalog::Provenance expected{
              source.source_archive_relative_path,
              source.archive_object_index,
              object.internal_name,
              retail_object.unique_name,
              static_cast<std::size_t>(source.size),
              source.xxh64,
              static_cast<jak1_retail_object_catalog::ObjectVersion>(source.object_version),
          };
          const auto catalog_entry = loaded.catalog.lookup(expected);
          const auto hash = XXH64(retail_object.data.data(), retail_object.data.size(), 0);
          if (!catalog_entry || retail_object.internal_name != object.internal_name ||
              retail_object.data.size() != source.size || hash != source.xxh64) {
            return Result<Summary>::failure(*cleanup_failure(
                make_error(ErrorCode::retail_object_mismatch,
                           "A retail object does not match its checked recipe identity.",
                           archive_index, object_index),
                stage));
          }
          bytes = retail_object.data;
        } else {
          const auto kind = std::get<jak1_output_recipe::GeneratedData>(object.source).kind;
          const auto key = generated_object_key(kind, object.internal_name);
          const auto found = generated_objects.find(key);
          if (found == generated_objects.end()) {
            return Result<Summary>::failure(*cleanup_failure(
                make_error(ErrorCode::generated_artifact_mismatch,
                           "A required generated object is missing from its checked catalog.",
                           archive_index, object_index),
                stage));
          }
          const auto& artifact = *found->second;
          auto loaded = read_verified_file(inputs.generated_artifact_root, artifact.relative_path,
                                           artifact.size, artifact.xxh64,
                                           options.limits.max_generated_object_bytes,
                                           ErrorCode::generated_artifact_mismatch, options);
          if (!loaded) {
            auto error = loaded.error();
            error.archive_index = archive_index;
            error.object_index = object_index;
            return Result<Summary>::failure(*cleanup_failure(std::move(error), stage));
          }
          used_generated_objects.emplace(key);
          bytes = loaded.take_value();
        }
        objects.push_back({object.internal_name, std::move(bytes)});
      }

      std::vector<jak1_checked_dgo_writer::ObjectRecord> write_objects;
      write_objects.reserve(objects.size());
      for (const auto& object : objects) {
        write_objects.push_back({object.name, object.bytes});
      }
      const auto remaining_output = options.limits.max_total_output_bytes - summary.output_bytes;
      if (remaining_output < kDgoHeaderBytes) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::output_limit_exceeded,
                       "The remaining output budget cannot contain another DGO header.",
                       archive_index),
            stage));
      }
      jak1_checked_dgo_writer::Options writer_options;
      writer_options.max_name_bytes = std::min(
          {writer_options.max_name_bytes, static_cast<std::size_t>(options.limits.max_name_bytes),
           static_cast<std::size_t>(options.recipe_limits.max_name_bytes)});
      writer_options.max_objects =
          std::min(writer_options.max_objects, options.recipe_limits.max_objects_per_archive);
      writer_options.max_object_bytes = std::min(writer_options.max_object_bytes,
                                                 size_cap(options.recipe_limits.max_object_bytes));
      writer_options.max_total_object_bytes =
          std::min(writer_options.max_total_object_bytes,
                   size_cap(options.recipe_limits.max_total_object_bytes));
      writer_options.max_output_bytes =
          std::min(writer_options.max_output_bytes, size_cap(remaining_output));
      writer_options.write_chunk_bytes =
          std::min(writer_options.write_chunk_bytes, options.limits.io_chunk_bytes);
      writer_options.duplicate_name_policy = jak1_checked_dgo_writer::DuplicateNamePolicy::allow;
      writer_options.should_cancel = options.should_cancel;
      auto written = jak1_checked_dgo_writer::write_file_at(
          stage.iso.get(), archive_record.destination_basename,
          archive_record.destination_basename, write_objects, writer_options);
      if (!written) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(
                map_writer_error(written.error().code),
                "The checked DGO writer rejected a staged archive: " + written.error().message,
                archive_index, written.error().object_index),
            stage));
      }
      if (const auto budget =
              reserve_output(written.value().output_bytes, &summary.output_bytes, options)) {
        return Result<Summary>::failure(*cleanup_failure(*budget, stage));
      }
      ++summary.archives_written;
      summary.objects_written += written.value().object_count;
      if (!iso_outputs
               .emplace(collision_key(archive_record.destination_basename),
                        checked_file_identity::Identity{archive_record.destination_basename,
                                                        written.value().output_bytes,
                                                        written.value().output_xxh64})
               .second) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::output_write_failed,
                       "A staged archive destination collides with another output."),
            stage));
      }
    }
    if (used_generated_objects.size() != generated_objects.size()) {
      return Result<Summary>::failure(*cleanup_failure(
          make_error(ErrorCode::generated_catalog_invalid,
                     "The generated-object catalog contains an unreferenced artifact."),
          stage));
    }

    std::uint32_t flat_index = 0;
    const auto flat_total = static_cast<std::uint32_t>(recipe.flat_file_copies.size() +
                                                       recipe.generated_flat_files.size());
    for (const auto& copy : recipe.flat_file_copies) {
      auto identity = required_identity(extracted_identities.value(),
                                        copy.extracted_iso_relative_path, options);
      if (!identity) {
        return Result<Summary>::failure(*cleanup_failure(identity.error(), stage));
      }
      auto source = resolve_regular_file(inputs.extracted_iso_root,
                                         copy.extracted_iso_relative_path, options);
      if (!source) {
        return Result<Summary>::failure(*cleanup_failure(source.error(), stage));
      }
      if (const auto error = report(options, Phase::copying_flat_files, flat_index++, flat_total,
                                    summary.output_bytes, copy.destination_basename)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      const auto expected = identity.value()
                                ? std::optional<std::pair<std::uint64_t, std::uint64_t>>(
                                      std::pair{identity.value()->size, identity.value()->xxh64})
                                : std::nullopt;
      checked_file_identity::Identity produced;
      if (const auto error = copy_file_at(
              source.value(), stage.iso.get(), copy.destination_basename,
              options.limits.max_flat_file_bytes, expected, ErrorCode::input_identity_mismatch,
              &summary.output_bytes, options, &produced)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      if (!iso_outputs.emplace(collision_key(copy.destination_basename), std::move(produced)).second) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::output_write_failed,
                       "A staged flat destination collides with another output."),
            stage));
      }
      ++summary.flat_files_written;
    }
    std::unordered_set<std::string> used_generated_flats;
    for (const auto& generated : recipe.generated_flat_files) {
      const auto key = generated_flat_key(generated.kind, generated.destination_basename);
      const auto found = generated_flats.find(key);
      if (found == generated_flats.end()) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::generated_artifact_mismatch,
                       "A required generated flat file is missing from its checked catalog."),
            stage));
      }
      const auto& artifact = *found->second;
      auto source =
          resolve_regular_file(inputs.generated_artifact_root, artifact.relative_path, options);
      if (!source) {
        return Result<Summary>::failure(*cleanup_failure(source.error(), stage));
      }
      if (const auto error = report(options, Phase::copying_flat_files, flat_index++, flat_total,
                                    summary.output_bytes, generated.destination_basename)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      checked_file_identity::Identity produced;
      if (const auto error = copy_file_at(
              source.value(), stage.iso.get(), generated.destination_basename,
              options.limits.max_flat_file_bytes,
              std::pair<std::uint64_t, std::uint64_t>{artifact.size, artifact.xxh64},
              ErrorCode::generated_artifact_mismatch, &summary.output_bytes, options,
              &produced)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      if (!iso_outputs
               .emplace(collision_key(generated.destination_basename), std::move(produced))
               .second) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::output_write_failed,
                       "A generated flat destination collides with another output."),
            stage));
      }
      used_generated_flats.emplace(key);
      ++summary.flat_files_written;
    }
    if (used_generated_flats.size() != generated_flats.size()) {
      return Result<Summary>::failure(*cleanup_failure(
          make_error(ErrorCode::generated_catalog_invalid,
                     "The generated-flat catalog contains an unreferenced artifact."),
          stage));
    }

    std::error_code fs_error;
    std::unordered_set<std::string> actual_fr3;
    const auto fr3_status = fs::symlink_status(inputs.prepared_fr3_root, fs_error);
    if (fs_error || fr3_status.type() != fs::file_type::directory) {
      return Result<Summary>::failure(*cleanup_failure(
          make_error(ErrorCode::fr3_set_mismatch,
                     "The prepared FR3 root is missing, a symbolic link, or not a directory."),
          stage));
    }
    for (const auto& entry : fs::directory_iterator(inputs.prepared_fr3_root)) {
      const auto status = entry.symlink_status(fs_error);
      const auto name = entry.path().filename().string();
      if (fs_error || status.type() != fs::file_type::regular ||
          !valid_name(name, options.limits.max_name_bytes) ||
          !actual_fr3.emplace(collision_key(name)).second) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::fr3_set_mismatch,
                       "The prepared FR3 directory contains an unsafe or duplicate entry."),
            stage));
      }
    }
    std::unordered_set<std::string> expected_fr3;
    for (const auto& name : recipe.expected_fr3_basenames) {
      expected_fr3.emplace(collision_key(name));
    }
    if (actual_fr3 != expected_fr3) {
      return Result<Summary>::failure(*cleanup_failure(
          make_error(ErrorCode::fr3_set_mismatch,
                     "The prepared FR3 directory does not exactly match the output recipe."),
          stage));
    }
    if (options.require_validated_file_identities &&
        fr3_identities.value().size() != actual_fr3.size()) {
      return Result<Summary>::failure(*cleanup_failure(
          make_error(ErrorCode::input_identity_mismatch,
                     "The prepared FR3 identity manifest does not match the exact FR3 set."),
          stage));
    }
    for (std::uint32_t index = 0; index < recipe.expected_fr3_basenames.size(); ++index) {
      const auto& name = recipe.expected_fr3_basenames[index];
      auto identity = required_identity(fr3_identities.value(), name, options);
      if (!identity) {
        return Result<Summary>::failure(*cleanup_failure(identity.error(), stage));
      }
      auto source = resolve_regular_file(inputs.prepared_fr3_root, name, options);
      if (!source) {
        return Result<Summary>::failure(*cleanup_failure(source.error(), stage));
      }
      if (const auto error =
              report(options, Phase::copying_fr3, index,
                     static_cast<std::uint32_t>(recipe.expected_fr3_basenames.size()),
                     summary.output_bytes, name)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      checked_file_identity::Identity produced;
      if (const auto error = copy_file_at(
              source.value(), stage.fr3.get(), name, options.limits.max_fr3_file_bytes,
              identity.value()
                  ? std::optional<std::pair<std::uint64_t, std::uint64_t>>(
                        std::pair{identity.value()->size, identity.value()->xxh64})
                  : std::nullopt,
              ErrorCode::input_identity_mismatch, &summary.output_bytes, options, &produced)) {
        return Result<Summary>::failure(*cleanup_failure(*error, stage));
      }
      if (!fr3_outputs.emplace(collision_key(name), std::move(produced)).second) {
        return Result<Summary>::failure(*cleanup_failure(
            make_error(ErrorCode::output_write_failed,
                       "A staged FR3 destination collides with another output."),
            stage));
      }
      ++summary.fr3_files_written;
    }

    if (const auto error = report(options, Phase::installing, 0, 1, summary.output_bytes,
                                  destination_root.filename().string())) {
      return Result<Summary>::failure(*cleanup_failure(*error, stage));
    }
    if (const auto validation = validate_stage(stage, iso_outputs, fr3_outputs, options)) {
      return Result<Summary>::failure(*cleanup_failure(*validation, stage));
    }
    if (const auto error = stage.install()) {
      return Result<Summary>::failure(*cleanup_failure(*error, stage));
    }
    return Result<Summary>::success(std::move(summary));
  } catch (const std::bad_alloc&) {
    auto error = make_error(
        ErrorCode::allocation_failed,
        std::string(options.wire_game == jak1_output_recipe::WireGame::jak1 ? "Jak 1" : "Jak II") +
            " output materialization ran out of memory.");
    return Result<Summary>::failure(stage.linked ? *cleanup_failure(error, stage) : error);
  } catch (const std::exception& exception) {
    auto error = make_error(
        ErrorCode::output_write_failed,
        std::string(options.wire_game == jak1_output_recipe::WireGame::jak1 ? "Jak 1" : "Jak II") +
            " output materialization failed: " + exception.what());
    return Result<Summary>::failure(stage.linked ? *cleanup_failure(error, stage) : error);
  } catch (...) {
    auto error = make_error(
        ErrorCode::output_write_failed,
        std::string(options.wire_game == jak1_output_recipe::WireGame::jak1 ? "Jak 1" : "Jak II") +
            " output materialization failed unexpectedly.");
    return Result<Summary>::failure(stage.linked ? *cleanup_failure(error, stage) : error);
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
    case ErrorCode::allocation_failed:
      return "allocation_failed";
    case ErrorCode::unsafe_path:
      return "unsafe_path";
    case ErrorCode::input_missing:
      return "input_missing";
    case ErrorCode::input_not_regular:
      return "input_not_regular";
    case ErrorCode::input_too_large:
      return "input_too_large";
    case ErrorCode::input_read_failed:
      return "input_read_failed";
    case ErrorCode::recipe_invalid:
      return "recipe_invalid";
    case ErrorCode::recipe_mismatch:
      return "recipe_mismatch";
    case ErrorCode::input_identity_mismatch:
      return "input_identity_mismatch";
    case ErrorCode::revision_mismatch:
      return "revision_mismatch";
    case ErrorCode::source_pack_mismatch:
      return "source_pack_mismatch";
    case ErrorCode::source_object_mismatch:
      return "source_object_mismatch";
    case ErrorCode::retail_archive_failed:
      return "retail_archive_failed";
    case ErrorCode::retail_catalog_failed:
      return "retail_catalog_failed";
    case ErrorCode::retail_object_mismatch:
      return "retail_object_mismatch";
    case ErrorCode::generated_catalog_invalid:
      return "generated_catalog_invalid";
    case ErrorCode::generated_artifact_mismatch:
      return "generated_artifact_mismatch";
    case ErrorCode::fr3_set_mismatch:
      return "fr3_set_mismatch";
    case ErrorCode::destination_inspection_failed:
      return "destination_inspection_failed";
    case ErrorCode::destination_exists:
      return "destination_exists";
    case ErrorCode::stage_create_failed:
      return "stage_create_failed";
    case ErrorCode::output_limit_exceeded:
      return "output_limit_exceeded";
    case ErrorCode::output_write_failed:
      return "output_write_failed";
    case ErrorCode::dgo_write_failed:
      return "dgo_write_failed";
    case ErrorCode::stage_install_failed:
      return "stage_install_failed";
    case ErrorCode::stage_cleanup_failed:
      return "stage_cleanup_failed";
  }
  return "unknown";
}

}  // namespace jak1_output_materializer
