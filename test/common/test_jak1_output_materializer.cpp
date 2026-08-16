#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <fcntl.h>
#include <membership.h>
#include <sys/acl.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#include "common/custom_data/Jak1OutputMaterializer.h"
#include "common/custom_data/Jak1OutputMaterializerTestHooks.h"

#include "decompiler/extractor/jak1_checked_dgo.h"
#include "decompiler/extractor/jak1_checked_dgo_writer.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace fs = std::filesystem;
using namespace jak1_output_materializer;

#define CHECK(condition)                                                                         \
  do {                                                                                           \
    if (!(condition)) {                                                                          \
      std::cerr << __func__ << ": check failed at line " << __LINE__ << ": " #condition << '\n'; \
      return false;                                                                              \
    }                                                                                            \
  } while (false)

void write_u32(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  (*bytes)[offset] = static_cast<std::uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<std::uint8_t>(value >> 8);
  (*bytes)[offset + 2] = static_cast<std::uint8_t>(value >> 16);
  (*bytes)[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

std::vector<std::uint8_t> make_v4_object(std::uint8_t seed) {
  std::vector<std::uint8_t> bytes(144);
  write_u32(&bytes, 0, 0xffffffff);
  write_u32(&bytes, 4, 64);
  write_u32(&bytes, 8, 4);
  write_u32(&bytes, 12, 64);
  for (std::size_t index = 16; index < 80; ++index) {
    bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  write_u32(&bytes, 80, 0xffffffff);
  write_u32(&bytes, 84, 64);
  write_u32(&bytes, 88, 2);
  return bytes;
}

std::uint64_t hash_of(std::span<const std::uint8_t> bytes) {
  return XXH64(bytes.data(), bytes.size(), 0);
}

bool write_bytes(const fs::path& path, std::span<const std::uint8_t> bytes) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  return static_cast<bool>(output);
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct TempDirectory {
  TempDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
      path = fs::temp_directory_path() / ("jak1-output-materializer-test-" + std::to_string(nonce) +
                                          "-" + std::to_string(attempt));
      std::error_code error;
      if (fs::create_directory(path, error)) {
        return;
      }
    }
    throw std::runtime_error("could not create materializer test directory");
  }

  ~TempDirectory() {
    std::error_code error;
    fs::remove_all(path, error);
  }

  fs::path path;
};

#if defined(__APPLE__)
std::optional<bool> filesystem_supports_file_cloning(const fs::path& root) {
  const auto source = root / "clone-probe-source";
  const auto destination_directory = root / "clone-probe-destination";
  const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
  std::error_code error;
  if (!write_bytes(source, bytes) || !fs::create_directory(destination_directory, error) || error) {
    return std::nullopt;
  }
  const int source_descriptor = ::open(source.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  const int destination_descriptor =
      ::open(destination_directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (source_descriptor < 0 || destination_descriptor < 0) {
    if (source_descriptor >= 0) {
      ::close(source_descriptor);
    }
    if (destination_descriptor >= 0) {
      ::close(destination_descriptor);
    }
    return std::nullopt;
  }
  errno = 0;
  const int result =
      ::fclonefileat(source_descriptor, destination_descriptor, "clone", CLONE_NOOWNERCOPY);
  const int clone_error = errno;
  ::close(destination_descriptor);
  ::close(source_descriptor);
  if (result == 0) {
    return true;
  }
  if (clone_error == ENOTSUP || clone_error == EOPNOTSUPP || clone_error == EXDEV) {
    return false;
  }
  return std::nullopt;
}

bool add_test_extended_acl(const fs::path& path) {
  uuid_t user_uuid;
  if (::mbr_uid_to_uuid(::geteuid(), user_uuid) != 0) {
    return false;
  }
  acl_t acl = ::acl_init(1);
  acl_entry_t entry = nullptr;
  acl_permset_t permissions = nullptr;
  acl_flagset_t flags = nullptr;
  const bool configured =
      acl && ::acl_create_entry(&acl, &entry) == 0 &&
      ::acl_set_tag_type(entry, ACL_EXTENDED_ALLOW) == 0 &&
      ::acl_set_qualifier(entry, user_uuid) == 0 &&
      ::acl_get_permset(entry, &permissions) == 0 && ::acl_clear_perms(permissions) == 0 &&
      ::acl_add_perm(permissions, ACL_READ_DATA) == 0 &&
      ::acl_set_permset(entry, permissions) == 0 && ::acl_get_flagset_np(entry, &flags) == 0 &&
      ::acl_clear_flags_np(flags) == 0 && ::acl_set_flagset_np(entry, flags) == 0 &&
      ::acl_valid(acl) == 0 && ::acl_set_file(path.c_str(), ACL_TYPE_EXTENDED, acl) == 0;
  if (acl) {
    ::acl_free(acl);
  }
  return configured;
}

bool has_no_extended_acl(const fs::path& path) {
  errno = 0;
  acl_t acl = ::acl_get_file(path.c_str(), ACL_TYPE_EXTENDED);
  if (!acl) {
    return errno == ENOENT || errno == ENOATTR;
  }
  acl_entry_t entry = nullptr;
  errno = 0;
  const auto result = ::acl_get_entry(acl, ACL_FIRST_ENTRY, &entry);
  const auto entry_error = errno;
  ::acl_free(acl);
  return result == -1 && entry_error == EINVAL;
}

std::size_t extended_acl_setter_calls = 0;

int reject_extended_acl_set(int, acl_t, acl_type_t) {
  ++extended_acl_setter_calls;
  errno = EPERM;
  return -1;
}

int count_extended_acl_set(int descriptor, acl_t acl, acl_type_t type) {
  ++extended_acl_setter_calls;
  return ::acl_set_fd_np(descriptor, acl, type);
}

struct ScopedExtendedAclSetter {
  explicit ScopedExtendedAclSetter(testing::ExtendedAclSetter setter)
      : previous(testing::replace_extended_acl_setter(setter)) {
    extended_acl_setter_calls = 0;
  }

  ~ScopedExtendedAclSetter() { testing::replace_extended_acl_setter(previous); }

  testing::ExtendedAclSetter previous;
};

struct ScopedTestDescriptor {
  explicit ScopedTestDescriptor(int descriptor) : value(descriptor) {}
  ~ScopedTestDescriptor() {
    if (value >= 0) {
      ::close(value);
    }
  }

  int value;
};
#endif

jak1_output_recipe::RevisionProvenance provenance_of(const jak1_iso::Revision& revision) {
  return {
      std::string(revision.serial),
      revision.elf_hash,
      revision.contents_hash,
      revision.file_count,
      std::string(revision.decomp_config_version),
      revision.territory,
      revision.black_label,
  };
}

struct Fixture {
  TempDirectory temp;
  fs::path source_root = temp.path / "source-pack";
  fs::path iso_root = temp.path / "extracted";
  fs::path generated_root = temp.path / "generated";
  fs::path fr3_root = temp.path / "prepared-fr3";
  fs::path recipe_file = temp.path / "recipe.bin";
  fs::path destination = temp.path / "installed";
  std::vector<std::uint8_t> bundled = {1, 2, 3, 4};
  std::vector<std::uint8_t> retail = make_v4_object(7);
  std::vector<std::uint8_t> generated = {9, 8, 7, 6, 5};
  std::vector<std::uint8_t> flat = {0x10, 0x20, 0x30};
  std::vector<std::uint8_t> generated_flat = {0x40, 0x50};
  std::vector<std::uint8_t> fr3 = {0x60, 0x70, 0x80};
  jak1_output_recipe::SourceObjectPackIdentity source_pack = {1, 0x123456789abcdef0ULL};
  jak1_output_recipe::Recipe recipe;
  std::vector<checked_file_identity::Identity> extracted_identities;
  std::vector<checked_file_identity::Identity> fr3_identities;
  Inputs inputs;
  Options options;

  bool setup() {
    const auto revision = provenance_of(jak1_iso::default_revision());
    recipe.revision = revision;
    recipe.source_object_pack = source_pack;
    recipe.archives = {{
        "OUT.DGO",
        {
            {"bundled", jak1_output_recipe::BundledSourceObject{"objects/bundled.o", bundled.size(),
                                                                hash_of(bundled)}},
            {"retail", jak1_output_recipe::VerifiedRetailObject{"DGO/RETAIL.DGO", 0, 4,
                                                                retail.size(), hash_of(retail)}},
            {"tpage-dir",
             jak1_output_recipe::GeneratedData{
                 jak1_output_recipe::GeneratedDataKind::directory_tpages}},
        },
    }};
    recipe.flat_file_copies = {{"DATA.BIN", "DATA.BIN"}};
    recipe.generated_flat_files = {
        {jak1_output_recipe::GeneratedFlatFileKind::game_text, "0COMMON.TXT"}};
    recipe.expected_fr3_basenames = {"level.fr3"};

    jak1_output_recipe::Options recipe_options;
    recipe_options.expected_revision = revision;
    recipe_options.expected_source_object_pack = source_pack;
    auto encoded = jak1_output_recipe::encode(recipe, recipe_options);
    if (!encoded || !write_bytes(recipe_file, encoded.value()) ||
        !write_bytes(source_root / "objects/bundled.o", bundled) ||
        !write_bytes(iso_root / "DATA.BIN", flat) ||
        !write_bytes(generated_root / "objects/tpage-dir.o", generated) ||
        !write_bytes(generated_root / "flat/0COMMON.TXT", generated_flat) ||
        !write_bytes(fr3_root / "level.fr3", fr3)) {
      return false;
    }

    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> retail_objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", retail},
    };
    auto retail_dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", retail_objects);
    if (!retail_dgo || !write_bytes(iso_root / "DGO/RETAIL.DGO", retail_dgo.value())) {
      return false;
    }

    inputs.recipe_file = recipe_file;
    inputs.source_object_pack_root = source_root;
    inputs.extracted_iso_root = iso_root;
    inputs.generated_artifact_root = generated_root;
    inputs.prepared_fr3_root = fr3_root;
    inputs.generated_objects = {{
        jak1_output_recipe::GeneratedDataKind::directory_tpages,
        "tpage-dir",
        "objects/tpage-dir.o",
        generated.size(),
        hash_of(generated),
    }};
    inputs.generated_flat_files = {{
        jak1_output_recipe::GeneratedFlatFileKind::game_text,
        "0COMMON.TXT",
        "flat/0COMMON.TXT",
        generated_flat.size(),
        hash_of(generated_flat),
    }};
    options.expected_revision = revision;
    options.expected_source_object_pack = source_pack;
    return true;
  }

  bool rewrite_recipe() {
    jak1_output_recipe::Options recipe_options;
    recipe_options.expected_revision = recipe.revision;
    recipe_options.expected_source_object_pack = source_pack;
    auto encoded = jak1_output_recipe::encode(recipe, recipe_options);
    return encoded && write_bytes(recipe_file, encoded.value());
  }

  void bind_validated_identities() {
    const auto retail_archive = read_bytes(iso_root / "DGO/RETAIL.DGO");
    extracted_identities = {
        {"DGO/RETAIL.DGO", retail_archive.size(), hash_of(retail_archive)},
        {"DATA.BIN", flat.size(), hash_of(flat)},
    };
    fr3_identities = {{"level.fr3", fr3.size(), hash_of(fr3)}};
    inputs.validated_extracted_files = extracted_identities;
    inputs.validated_fr3_files = fr3_identities;
    options.require_validated_file_identities = true;
  }

  bool stage_absent() const { return !fs::exists(fs::path(destination.string() + ".stage")); }
};

bool materializes_checked_desktop_layout() {
  Fixture fixture;
  CHECK(fixture.setup());
  CHECK(!fixture.options.compressed_trailing_alignment_bytes);
  CHECK(!fixture.options.expected_recipe_bytes);
  CHECK(!fixture.options.require_validated_file_identities);
  CHECK(!fixture.options.expected_validated_extracted_file_count);
  CHECK(!fixture.options.expected_validated_fr3_file_count);
  CHECK(fixture.inputs.validated_extracted_files.empty());
  CHECK(fixture.inputs.validated_fr3_files.empty());
  std::vector<Progress> progress;
  fixture.options.on_progress = [&](const Progress& update) { progress.push_back(update); };
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(result);
  CHECK(result.value().archives_written == 1);
  CHECK(result.value().objects_written == 3);
  CHECK(result.value().flat_files_written == 2);
  CHECK(result.value().fr3_files_written == 1);
  CHECK(result.value().files_cloned == 0);
  CHECK(result.value().output_bytes > 0);
  CHECK(fixture.stage_absent());
  CHECK(read_bytes(fixture.destination / "iso/DATA.BIN") == fixture.flat);
  CHECK(read_bytes(fixture.destination / "iso/0COMMON.TXT") == fixture.generated_flat);
  CHECK(read_bytes(fixture.destination / "fr3/level.fr3") == fixture.fr3);

  const auto output_dgo = read_bytes(fixture.destination / "iso/OUT.DGO");
  const auto decoded = jak1_checked_dgo::read(output_dgo, "OUT.DGO");
  CHECK(decoded);
  CHECK(decoded.value().objects.size() == 3);
  CHECK(decoded.value().objects[0].internal_name == "bundled");
  CHECK(decoded.value().objects[0].data == fixture.bundled);
  CHECK(decoded.value().objects[1].internal_name == "retail");
  CHECK(decoded.value().objects[1].data == fixture.retail);
  CHECK(decoded.value().objects[2].internal_name == "tpage-dir");
  CHECK(decoded.value().objects[2].data == fixture.generated);
  CHECK(!progress.empty());
  CHECK(progress.front().phase == Phase::validating);
  CHECK(progress.back().phase == Phase::installing);
  return true;
}

bool clones_flat_files_without_mutating_absent_acls() {
#if defined(__APPLE__)
  Fixture fixture;
  CHECK(fixture.setup());
  fixture.bind_validated_identities();
  const auto clone_support = filesystem_supports_file_cloning(fixture.temp.path);
  CHECK(clone_support.has_value());
  if (!*clone_support) {
    std::cout << "clone success test skipped: filesystem does not support file cloning\n";
    return true;
  }
  CHECK(has_no_extended_acl(fixture.iso_root / "DATA.BIN"));
  CHECK(has_no_extended_acl(fixture.generated_root / "flat/0COMMON.TXT"));
  CHECK(has_no_extended_acl(fixture.fr3_root / "level.fr3"));
  ScopedExtendedAclSetter setter(reject_extended_acl_set);
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(result);
  CHECK(result.value().files_cloned == 3);
  CHECK(extended_acl_setter_calls == 0);
  const auto output = fixture.destination / "iso/DATA.BIN";
  CHECK(read_bytes(output) == fixture.flat);
  CHECK(has_no_extended_acl(output));
  const auto original_output = read_bytes(output);
  fixture.flat.front() ^= 1;
  CHECK(write_bytes(fixture.iso_root / "DATA.BIN", fixture.flat));
  CHECK(read_bytes(output) == original_output);
#else
  std::cout << "no-ACL clone test skipped: file cloning is Apple-only\n";
#endif
  return true;
}

bool normalization_clears_present_acl_and_metadata() {
#if defined(__APPLE__)
  TempDirectory temp;
  const auto output = temp.path / "cloned-output";
  const std::array<std::uint8_t, 4> bytes = {1, 2, 3, 4};
  CHECK(write_bytes(output, bytes));
  constexpr char xattr_value[] = "clone-metadata";
  CHECK(::setxattr(output.c_str(), "com.goalpad.clone-test", xattr_value,
                   sizeof(xattr_value), 0, 0) == 0);
  CHECK(add_test_extended_acl(output));
  CHECK(!has_no_extended_acl(output));
  ScopedTestDescriptor descriptor(::open(output.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
  CHECK(descriptor.value >= 0);
  CHECK(::fchmod(descriptor.value, 0400) == 0);
  CHECK(::fchflags(descriptor.value, UF_IMMUTABLE) == 0);
  ScopedExtendedAclSetter setter(count_extended_acl_set);
  CHECK(testing::normalize_cloned_output(descriptor.value));
  CHECK(extended_acl_setter_calls == 1);
  struct stat output_status {};
  CHECK(::fstat(descriptor.value, &output_status) == 0);
  CHECK(S_ISREG(output_status.st_mode));
  CHECK(output_status.st_nlink == 1);
  CHECK(output_status.st_uid == ::geteuid());
  CHECK((output_status.st_mode & 07777) == 0600);
  CHECK(output_status.st_flags == 0);
  errno = 0;
  CHECK(::getxattr(output.c_str(), "com.goalpad.clone-test", nullptr, 0, 0, 0) == -1);
  CHECK(errno == ENOATTR);
  CHECK(has_no_extended_acl(output));
#else
  std::cout << "clone ACL clear test skipped: file cloning is Apple-only\n";
#endif
  return true;
}

bool cloned_copy_rejects_mutation_and_cancellation() {
#if defined(__APPLE__)
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bind_validated_identities();
    const auto clone_support = filesystem_supports_file_cloning(fixture.temp.path);
    CHECK(clone_support.has_value());
    if (!*clone_support) {
      std::cout << "clone mutation test skipped: filesystem does not support file cloning\n";
      return true;
    }
    bool saw_initial = false;
    bool mutated = false;
    bool mutation_write_ok = true;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase != Phase::copying_flat_files || update.current_item != "DATA.BIN") {
        return;
      }
      if (!saw_initial) {
        saw_initial = true;
      } else if (!mutated) {
        fixture.flat.back() ^= 1;
        mutation_write_ok = write_bytes(fixture.iso_root / "DATA.BIN", fixture.flat);
        mutated = true;
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(mutated);
    CHECK(mutation_write_ok);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::input_identity_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bind_validated_identities();
    const auto clone_support = filesystem_supports_file_cloning(fixture.temp.path);
    CHECK(clone_support.has_value());
    if (!*clone_support) {
      std::cout << "clone cancellation test skipped: filesystem does not support file cloning\n";
      return true;
    }
    bool saw_initial = false;
    bool cancel = false;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase != Phase::copying_flat_files || update.current_item != "DATA.BIN") {
        return;
      }
      if (!saw_initial) {
        saw_initial = true;
      } else {
        cancel = true;
      }
    };
    fixture.options.should_cancel = [&] { return cancel; };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::cancelled);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
#else
  std::cout << "clone mutation/cancellation test skipped: file cloning is Apple-only\n";
#endif
  return true;
}

bool forced_stream_copy_reports_throttled_byte_progress() {
  Fixture fixture;
  CHECK(fixture.setup());
  fixture.options.attempt_file_clones = false;
  fixture.options.limits.io_chunk_bytes = 1;
  std::vector<Progress> progress;
  fixture.options.on_progress = [&](const Progress& update) { progress.push_back(update); };
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(result);
  CHECK(result.value().files_cloned == 0);

  std::vector<std::uint64_t> flat_bytes;
  std::vector<std::uint64_t> installation_bytes;
  for (const auto& update : progress) {
    if (update.phase == Phase::copying_flat_files && update.current_item == "DATA.BIN") {
      flat_bytes.push_back(update.bytes_written);
    }
    if (update.phase == Phase::installing) {
      installation_bytes.push_back(update.bytes_written);
    }
  }
  CHECK(flat_bytes.size() == 2);
  CHECK(std::is_sorted(flat_bytes.begin(), flat_bytes.end()));
  CHECK(flat_bytes.back() - flat_bytes.front() == fixture.flat.size());
  CHECK(installation_bytes.size() == 1);
  CHECK(installation_bytes.front() == result.value().output_bytes);
  return true;
}

bool streamed_copy_rejects_progress_mutation() {
  Fixture fixture;
  CHECK(fixture.setup());
  fixture.bind_validated_identities();
  fixture.options.attempt_file_clones = false;
  fixture.options.limits.io_chunk_bytes = 1;
  bool saw_initial = false;
  bool mutated = false;
  bool mutation_write_ok = true;
  fixture.options.on_progress = [&](const Progress& update) {
    if (update.phase != Phase::copying_flat_files || update.current_item != "DATA.BIN") {
      return;
    }
    if (!saw_initial) {
      saw_initial = true;
      return;
    }
    if (!mutated && update.bytes_written > 0) {
      fixture.flat.back() ^= 1;
      mutation_write_ok = write_bytes(fixture.iso_root / "DATA.BIN", fixture.flat);
      mutated = true;
    }
  };
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(mutated);
  CHECK(mutation_write_ok);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::input_identity_mismatch);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool byte_progress_cancellation_is_atomic() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.attempt_file_clones = false;
    fixture.options.limits.io_chunk_bytes = fixture.flat.size();
    bool saw_initial = false;
    bool cancel = false;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase != Phase::copying_flat_files || update.current_item != "DATA.BIN") {
        return;
      }
      if (!saw_initial) {
        saw_initial = true;
      } else {
        cancel = true;
      }
    };
    fixture.options.should_cancel = [&] { return cancel; };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::cancelled);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    bool cancel = false;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase == Phase::installing &&
          update.current_item == fixture.destination.filename().string()) {
        cancel = true;
      }
    };
    fixture.options.should_cancel = [&] { return cancel; };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::cancelled);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_exact_recipe_mismatch_before_staging() {
  Fixture fixture;
  CHECK(fixture.setup());
  auto expected = read_bytes(fixture.recipe_file);
  CHECK(!expected.empty());
  auto& bundled = std::get<jak1_output_recipe::BundledSourceObject>(
      fixture.recipe.archives.front().objects.front().source);
  bundled.bundle_relative_path = "objects/alternate.o";
  CHECK(write_bytes(fixture.source_root / bundled.bundle_relative_path, fixture.bundled));
  CHECK(fixture.rewrite_recipe());
  fixture.options.expected_recipe_bytes = expected;
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::recipe_mismatch);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool validated_input_mutations_fail_without_promotion() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bind_validated_identities();
    bool callback_write_ok = true;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (progress.phase == Phase::writing_archives) {
        auto archive = read_bytes(fixture.iso_root / "DGO/RETAIL.DGO");
        archive.back() ^= 1;
        callback_write_ok = write_bytes(fixture.iso_root / "DGO/RETAIL.DGO", archive);
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(callback_write_ok);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::input_identity_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bind_validated_identities();
    bool callback_write_ok = true;
    bool mutated = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!mutated && progress.phase == Phase::copying_flat_files) {
        fixture.flat.front() ^= 1;
        callback_write_ok = write_bytes(fixture.iso_root / "DATA.BIN", fixture.flat);
        mutated = true;
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(callback_write_ok);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::input_identity_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bind_validated_identities();
    bool callback_write_ok = true;
    bool mutated = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!mutated && progress.phase == Phase::copying_fr3) {
        fixture.fr3.front() ^= 1;
        callback_write_ok = write_bytes(fixture.fr3_root / "level.fr3", fixture.fr3);
        mutated = true;
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(callback_write_ok);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::input_identity_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool validated_archive_hash_is_checked_by_the_single_decode() {
  Fixture fixture;
  CHECK(fixture.setup());
  fixture.bind_validated_identities();
  fixture.extracted_identities.front().xxh64 ^= 1;
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::input_identity_mismatch);
  CHECK(result.error().message.find("The checked DGO reader rejected") != std::string::npos);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool malformed_retail_archive_preserves_catalog_failure() {
  Fixture fixture;
  CHECK(fixture.setup());
  const std::array<std::uint8_t, 64> malformed{};
  CHECK(write_bytes(fixture.iso_root / "DGO/RETAIL.DGO", malformed));
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::retail_catalog_failed);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool descriptor_owned_outputs_reject_terminal_races() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    bool mutated = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!mutated && progress.phase == Phase::installing) {
        auto bytes = read_bytes(fs::path(fixture.destination.string() + ".stage") / "iso/DATA.BIN");
        if (!bytes.empty()) {
          bytes.front() ^= 1;
          mutated = write_bytes(fs::path(fixture.destination.string() + ".stage") /
                                    "iso/DATA.BIN",
                                bytes);
        }
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(mutated);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::output_write_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const auto external = fixture.temp.path / "external-file";
    const std::vector<std::uint8_t> sentinel{0x5a, 0x31, 0x44};
    CHECK(write_bytes(external, sentinel));
    bool injected = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!injected && progress.phase == Phase::installing) {
        std::error_code error;
        fs::rename(external,
                   fs::path(fixture.destination.string() + ".stage") / "iso/injected.bin",
                   error);
        injected = !error;
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(injected);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::stage_cleanup_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(read_bytes(fs::path(fixture.destination.string() + ".stage") /
                     "iso/injected.bin") == sentinel);
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    bool raced = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!raced && progress.phase == Phase::installing) {
        std::error_code error;
        raced = fs::create_directory(fixture.destination, error) && !error;
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(raced);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::stage_install_failed);
    CHECK(fs::is_directory(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const auto outside = fixture.temp.path / "outside";
    CHECK(write_bytes(outside / "sentinel", std::array<std::uint8_t, 1>{0x5a}));
    bool swapped = false;
    fixture.options.on_progress = [&](const Progress& progress) {
      if (!swapped && progress.phase == Phase::copying_flat_files) {
        const auto stage_root = fs::path(fixture.destination.string() + ".stage");
        std::error_code error;
        fs::rename(stage_root / "iso", stage_root / "iso-held", error);
        if (!error) {
          fs::create_directory_symlink(outside, stage_root / "iso", error);
          swapped = !error;
        }
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(swapped);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::stage_cleanup_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(read_bytes(outside / "sentinel") == std::vector<std::uint8_t>{0x5a});
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.on_progress = [](const Progress& progress) {
      if (progress.phase == Phase::installing) {
        throw std::runtime_error("terminal callback failure");
      }
    };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::callback_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_mismatched_checked_inputs_and_cleans_stage() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.bundled[0] ^= 1;
    CHECK(write_bytes(fixture.source_root / "objects/bundled.o", fixture.bundled));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::source_object_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    auto changed = make_v4_object(19);
    const std::array<jak1_checked_dgo_writer::ObjectRecord, 1> objects = {
        jak1_checked_dgo_writer::ObjectRecord{"retail", changed},
    };
    const auto dgo = jak1_checked_dgo_writer::build("RETAIL.DGO", objects);
    CHECK(dgo);
    CHECK(write_bytes(fixture.iso_root / "DGO/RETAIL.DGO", dgo.value()));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::retail_object_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.generated[0] ^= 1;
    CHECK(write_bytes(fixture.generated_root / "objects/tpage-dir.o", fixture.generated));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::generated_artifact_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool preserves_typed_recipe_identity_failures() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.expected_revision =
        provenance_of(*(jak1_iso::supported_revisions().begin() + 1));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::revision_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.expected_source_object_pack.aggregate_xxh64 ^= 1;
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::source_pack_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_inexact_generated_and_fr3_catalogs() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.inputs.generated_objects.push_back({
        jak1_output_recipe::GeneratedDataKind::custom_actor,
        "test-actor",
        "objects/test-actor-ag.go",
        1,
        1,
    });
    fixture.inputs.generated_objects.push_back({
        jak1_output_recipe::GeneratedDataKind::custom_level,
        "test-zone",
        "objects/test-zone.go",
        1,
        1,
    });
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::generated_catalog_invalid);
    CHECK(result.error().message.find("unreferenced") != std::string::npos);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const std::array<std::uint8_t, 1> extra = {1};
    CHECK(write_bytes(fixture.fr3_root / "extra.fr3", extra));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::fr3_set_mismatch);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool preserves_existing_destination_and_output_budget() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    const std::array<std::uint8_t, 2> sentinel = {0xaa, 0xbb};
    CHECK(write_bytes(fixture.destination / "sentinel", sentinel));
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::destination_exists);
    CHECK(read_bytes(fixture.destination / "sentinel") ==
          std::vector<std::uint8_t>(sentinel.begin(), sentinel.end()));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.limits.max_total_output_bytes = 128;
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::output_limit_exceeded);
    CHECK(result.error().message.find("checked DGO writer rejected") != std::string::npos);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool cancellation_and_callback_failure_are_atomic() {
  {
    Fixture fixture;
    CHECK(fixture.setup());
    bool cancel = false;
    fixture.options.on_progress = [&](const Progress& update) {
      if (update.phase == Phase::writing_archives) {
        cancel = true;
      }
    };
    fixture.options.should_cancel = [&] { return cancel; };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::cancelled);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  {
    Fixture fixture;
    CHECK(fixture.setup());
    fixture.options.on_progress = [](const Progress&) { throw std::runtime_error("test"); };
    const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
    CHECK(!result);
    CHECK(result.error().code == ErrorCode::callback_failed);
    CHECK(!fs::exists(fixture.destination));
    CHECK(fixture.stage_absent());
  }
  return true;
}

bool rejects_symlinked_input() {
  Fixture fixture;
  CHECK(fixture.setup());
  std::error_code error;
  fs::remove(fixture.source_root / "objects/bundled.o", error);
  CHECK(!error);
  fs::create_symlink(fixture.iso_root / "DATA.BIN", fixture.source_root / "objects/bundled.o",
                     error);
  if (error) {
    std::cout << "symlink test skipped: " << error.message() << '\n';
    return true;
  }
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::unsafe_path);
  CHECK(!fs::exists(fixture.destination));
  CHECK(fixture.stage_absent());
  return true;
}

bool rejects_dangling_destination_symlink() {
  Fixture fixture;
  CHECK(fixture.setup());
  std::error_code error;
  fs::create_symlink(fixture.temp.path / "missing", fixture.destination, error);
  if (error) {
    std::cout << "destination symlink test skipped: " << error.message() << '\n';
    return true;
  }
  const auto result = materialize(fixture.inputs, fixture.destination, fixture.options);
  CHECK(!result);
  CHECK(result.error().code == ErrorCode::destination_exists);
  CHECK(fs::symlink_status(fixture.destination).type() == fs::file_type::symlink);
  CHECK(fixture.stage_absent());
  return true;
}

}  // namespace

int main() {
  const std::array tests = {
      materializes_checked_desktop_layout,
      clones_flat_files_without_mutating_absent_acls,
      normalization_clears_present_acl_and_metadata,
      cloned_copy_rejects_mutation_and_cancellation,
      forced_stream_copy_reports_throttled_byte_progress,
      streamed_copy_rejects_progress_mutation,
      byte_progress_cancellation_is_atomic,
      rejects_exact_recipe_mismatch_before_staging,
      validated_input_mutations_fail_without_promotion,
      validated_archive_hash_is_checked_by_the_single_decode,
      malformed_retail_archive_preserves_catalog_failure,
      descriptor_owned_outputs_reject_terminal_races,
      rejects_mismatched_checked_inputs_and_cleans_stage,
      preserves_typed_recipe_identity_failures,
      rejects_inexact_generated_and_fr3_catalogs,
      preserves_existing_destination_and_output_budget,
      cancellation_and_callback_failure_are_atomic,
      rejects_symlinked_input,
      rejects_dangling_destination_symlink,
  };
  for (const auto test : tests) {
    if (!test()) {
      return 1;
    }
  }
  std::cout << "Jak 1 output materializer tests passed\n";
  return 0;
}
