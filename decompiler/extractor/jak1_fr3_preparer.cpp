#include "jak1_fr3_preparer.h"
#include "jak2_fr3_preparer.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "decompiler/Disasm/OpcodeInfo.h"
#include "decompiler/ObjectFile/ObjectFileDB.h"
#include "decompiler/config.h"
#include "decompiler/level_extractor/extract_level.h"

namespace jak1_fr3 {
namespace {

namespace fs = std::filesystem;

struct GameProfile {
  GameVersion game_version;
  std::string_view display_name;
  std::string_view project_name;
  std::string_view config_path;
  std::string_view config_version;
};

constexpr GameProfile kJak1Profile = {
    GameVersion::Jak1,
    "Jak 1",
    "jak1",
    "decompiler/config/jak1/jak1_config.jsonc",
    "ntsc_v1",
};

constexpr GameProfile kJak2Profile = {
    GameVersion::Jak2,
    "Jak II",
    "jak2",
    "decompiler/config/jak2/jak2_config.jsonc",
    "ntsc_v1",
};

class UnsafeOutputError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

Error make_error(ErrorCode code, std::string message) {
  return {code, std::move(message)};
}

std::optional<Error> cancellation_error(const Options& options) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, "FR3 preparation was cancelled.");
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed,
                      "The FR3 preparation cancellation callback failed.");
  }
  return {};
}

std::optional<Error> report(const Options& options,
                            Phase phase,
                            std::uint32_t completed,
                            std::uint32_t total,
                            std::string current_item = {}) {
  if (!options.report_progress) {
    return {};
  }
  try {
    options.report_progress({phase, completed, total, std::move(current_item)});
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The FR3 preparation progress callback failed.");
  }
  return {};
}

bool valid_options(const Options& options) {
  return options.max_archive_bytes > 0 && options.max_expanded_archive_bytes > 0 &&
         options.max_total_archive_bytes > 0 && options.max_total_expanded_archive_bytes > 0 &&
         options.max_output_bytes > 0 &&
         options.max_archives > 0 && options.max_levels > 0;
}

bool supported_revision(const jak1_iso::Revision& revision) {
  const auto& supported = jak1_iso::default_revision();
  return revision.serial == supported.serial && revision.elf_hash == supported.elf_hash &&
         revision.contents_hash == supported.contents_hash &&
         revision.file_count == supported.file_count &&
         revision.decomp_config_version == supported.decomp_config_version &&
         revision.territory == supported.territory && revision.black_label == supported.black_label;
}

bool ends_with(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::uintmax_t directory_size(const fs::path& root, std::uint32_t* file_count = nullptr) {
  std::uintmax_t total = 0;
  for (const auto& entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_symlink()) {
      throw UnsafeOutputError("The preparation output contains a symbolic link.");
    }
    if (!entry.is_regular_file()) {
      if (!entry.is_directory()) {
        throw UnsafeOutputError("The preparation output contains an unsupported file type.");
      }
      continue;
    }
    const auto size = entry.file_size();
    if (size > std::numeric_limits<std::uintmax_t>::max() - total) {
      throw std::overflow_error("The preparation output size overflowed.");
    }
    total += size;
    if (file_count) {
      ++*file_count;
    }
  }
  return total;
}

bool safe_output_basename(std::string_view name, std::string_view suffix) {
  if (name.size() <= suffix.size() || !ends_with(name, suffix) || name.front() == '.' ||
      name.back() == '.') {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

std::set<std::string> fr3_files(const fs::path& root) {
  std::set<std::string> result;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_symlink() || !entry.is_regular_file()) {
      throw UnsafeOutputError("The FR3 output contains an unsupported file type.");
    }
    const auto name = entry.path().filename().string();
    if (!safe_output_basename(name, ".fr3") || !result.insert(name).second) {
      throw UnsafeOutputError("The FR3 output contains an unsafe or duplicate basename.");
    }
  }
  return result;
}

std::optional<Error> output_budget_error(const fs::path& work_root, const Options& options) {
  if (directory_size(work_root) > options.max_output_bytes) {
    return make_error(ErrorCode::output_limit_exceeded,
                      "FR3 preparation output exceeds its configured cap.");
  }
  return {};
}

class OwnedWorkRoot {
 public:
  explicit OwnedWorkRoot(fs::path root) : m_root(std::move(root)) {}
  ~OwnedWorkRoot() {
    if (!m_keep) {
      std::error_code ignored;
      fs::remove_all(m_root, ignored);
    }
  }
  void keep() { m_keep = true; }

 private:
  fs::path m_root;
  bool m_keep = false;
};

}  // namespace

static Result<Summary> prepare_for_profile(const fs::path& project_root,
                                           const fs::path& extracted_iso_root,
                                           const fs::path& work_root,
                                           const GameProfile& profile,
                                           const Options& options) {
  if (!valid_options(options) || project_root.empty() || extracted_iso_root.empty() ||
      work_root.empty()) {
    return Result<Summary>::failure(
        make_error(ErrorCode::invalid_argument, "The FR3 preparation arguments are invalid."));
  }

  try {
    if (const auto error = cancellation_error(options)) {
      return Result<Summary>::failure(*error);
    }
    if (fs::exists(work_root)) {
      return Result<Summary>::failure(
          make_error(ErrorCode::output_exists, "The FR3 work directory already exists."));
    }
    if (!fs::is_directory(project_root) || !fs::is_directory(extracted_iso_root) ||
        !fs::is_directory(work_root.parent_path())) {
      return Result<Summary>::failure(make_error(
          ErrorCode::input_missing,
          "The project, extracted ISO, or FR3 work-directory parent is missing."));
    }
    if (fs::space(work_root.parent_path()).available < options.max_output_bytes) {
      return Result<Summary>::failure(make_error(
          ErrorCode::output_limit_exceeded,
          "The FR3 work-directory volume has less free space than the configured output cap."));
    }

    decompiler::init_opcode_info();

    std::error_code create_error;
    if (!fs::create_directory(work_root, create_error)) {
      return Result<Summary>::failure(make_error(
          create_error ? ErrorCode::output_creation_failed : ErrorCode::output_exists,
          create_error ? "Could not create the owned FR3 work directory: " +
                             create_error.message()
                       : "The FR3 work directory already exists."));
    }
    OwnedWorkRoot owned_work(work_root);
    const auto intermediates = work_root / "intermediates";
    const auto raw_objects = intermediates / "raw_obj";
    const auto assets = intermediates / "assets";
    const auto textures = intermediates / "textures";
    const auto fr3 = work_root / "fr3";
    const auto entities = intermediates / "entities";
    for (const auto& directory : {raw_objects, assets, textures, fr3, entities}) {
      fs::create_directories(directory);
    }

    if (const auto error = report(options, Phase::loading_configuration, 0, 1)) {
      return Result<Summary>::failure(*error);
    }
    const ghc::filesystem::path project_path(project_root.string());
    if (!file_util::setup_project_path(project_path, true)) {
      return Result<Summary>::failure(make_error(
          ErrorCode::project_setup_failed, "Could not select the bundled OpenGOAL project data."));
    }
    if (!fs::equivalent(project_root,
                        fs::path(file_util::get_jak_project_dir().string()))) {
      return Result<Summary>::failure(make_error(
          ErrorCode::project_setup_failed,
          "The process already selected a different OpenGOAL project-data root."));
    }
    const auto config_path = project_path / std::string(profile.config_path);
    auto config = decompiler::read_config_file(config_path, std::string(profile.config_version));
    if (config.game_version != profile.game_version) {
      return Result<Summary>::failure(make_error(
          ErrorCode::configuration_failed,
          std::string(profile.display_name) + " decompiler configuration selected another game."));
    }
    config.rip_levels = false;
    config.save_texture_pngs = false;
    config.rip_collision = false;
    config.rip_streamed_audio = false;
    config.write_patches = false;
    if (const auto error = report(options, Phase::loading_configuration, 1, 1)) {
      return Result<Summary>::failure(*error);
    }

    std::vector<fs::path> archive_paths;
    for (const auto& name : config.dgo_names) {
      if (ends_with(name, ".DGO") || ends_with(name, "GAME.CGO")) {
        archive_paths.push_back(extracted_iso_root / name);
      }
    }
    if (archive_paths.empty() ||
        archive_paths.size() + config.str_texture_file_names.size() > options.max_archives) {
      return Result<Summary>::failure(make_error(
          ErrorCode::archive_limit_exceeded,
          "The " + std::string(profile.display_name) + " archive list is empty or exceeds its cap."));
    }
    if (config.levels_to_extract.empty() || config.levels_to_extract.size() > options.max_levels) {
      return Result<Summary>::failure(make_error(
          ErrorCode::archive_limit_exceeded,
          "The " + std::string(profile.display_name) + " level list is empty or exceeds its cap."));
    }

    std::vector<ghc::filesystem::path> text_objects;
    for (const auto& name : config.object_file_names) {
      const auto path = extracted_iso_root / name;
      if (!fs::is_regular_file(path)) {
        return Result<Summary>::failure(make_error(
            ErrorCode::input_missing,
            "A required " + std::string(profile.display_name) + " text object is missing: " +
                name));
      }
      text_objects.emplace_back(path.string());
    }

    std::uintmax_t total_archive_bytes = 0;
    std::vector<ghc::filesystem::path> streamed_texture_objects;
    for (const auto& name : config.str_texture_file_names) {
      const auto path = extracted_iso_root / name;
      if (!fs::is_regular_file(path)) {
        return Result<Summary>::failure(make_error(
            ErrorCode::input_missing,
            "A required " + std::string(profile.display_name) +
                " streamed texture archive is missing: " + name));
      }
      const auto input_size = fs::file_size(path);
      if (input_size > options.max_archive_bytes ||
          input_size > options.max_total_archive_bytes - total_archive_bytes) {
        return Result<Summary>::failure(make_error(
            ErrorCode::archive_limit_exceeded,
            "The " + std::string(profile.display_name) +
                " archives exceed their configured cap."));
      }
      total_archive_bytes += input_size;
      streamed_texture_objects.emplace_back(path.string());
    }

    const auto total_input_files = archive_paths.size() + streamed_texture_objects.size();
    for (std::size_t index = 0; index < streamed_texture_objects.size(); ++index) {
      if (const auto error = cancellation_error(options)) {
        return Result<Summary>::failure(*error);
      }
      if (const auto error = report(options, Phase::reading_archives,
                                    static_cast<std::uint32_t>(index),
                                    static_cast<std::uint32_t>(total_input_files),
                                    fs::path(streamed_texture_objects[index].string())
                                        .filename()
                                        .string())) {
        return Result<Summary>::failure(*error);
      }
    }
    decompiler::ObjectFileDB database({}, ghc::filesystem::path(config.obj_file_name_map_file), {},
                                      {}, streamed_texture_objects, {}, config, true);
    std::uintmax_t total_expanded_archive_bytes = 0;
    for (std::size_t index = 0; index < archive_paths.size(); ++index) {
      if (const auto error = cancellation_error(options)) {
        return Result<Summary>::failure(*error);
      }
      const auto& path = archive_paths[index];
      if (!fs::is_regular_file(path)) {
        return Result<Summary>::failure(make_error(
            ErrorCode::input_missing, "A required " + std::string(profile.display_name) +
                                          " archive is missing: " + path.string()));
      }
      const auto input_size = fs::file_size(path);
      if (input_size > options.max_archive_bytes ||
          input_size > options.max_total_archive_bytes - total_archive_bytes) {
        return Result<Summary>::failure(make_error(
            ErrorCode::archive_limit_exceeded,
            "The " + std::string(profile.display_name) +
                " archives exceed their configured cap."));
      }
      total_archive_bytes += input_size;
      if (const auto error = report(options, Phase::reading_archives,
                                    static_cast<std::uint32_t>(streamed_texture_objects.size() +
                                                               index),
                                    static_cast<std::uint32_t>(total_input_files),
                                    path.filename().string())) {
        return Result<Summary>::failure(*error);
      }

      jak1_checked_dgo::Options read_options;
      read_options.game_version = profile.game_version;
      read_options.max_input_bytes = static_cast<std::size_t>(options.max_archive_bytes);
      read_options.max_compressed_bytes = static_cast<std::size_t>(options.max_archive_bytes);
      read_options.max_expanded_bytes =
          static_cast<std::size_t>(options.max_expanded_archive_bytes);
      read_options.max_total_object_bytes =
          static_cast<std::size_t>(options.max_expanded_archive_bytes);
      read_options.compressed_trailing_alignment_bytes =
          options.compressed_trailing_alignment_bytes;
      bool read_callback_failed = false;
      read_options.should_cancel = [&] {
        const auto error = cancellation_error(options);
        read_callback_failed = error && error->code == ErrorCode::callback_failed;
        return error.has_value();
      };
      auto archive = jak1_checked_dgo::read_file(
          path, path.filename().string(), read_options);
      if (!archive) {
        if (read_callback_failed) {
          return Result<Summary>::failure(make_error(
              ErrorCode::callback_failed, "The FR3 preparation cancellation callback failed."));
        }
        return Result<Summary>::failure(make_error(
            archive.error().code == jak1_checked_dgo::ErrorCode::cancelled
                ? ErrorCode::cancelled
                : ErrorCode::archive_failed,
            "Could not read " + path.filename().string() + ": " + archive.error().message));
      }
      if (archive.value().expanded_size >
          options.max_total_expanded_archive_bytes - total_expanded_archive_bytes) {
        return Result<Summary>::failure(make_error(
            ErrorCode::archive_limit_exceeded,
            "The expanded " + std::string(profile.display_name) +
                " archives exceed their configured aggregate cap."));
      }
      total_expanded_archive_bytes += archive.value().expanded_size;
      database.add_checked_dgo(archive.value(), config);
    }
    for (const auto& object_file : text_objects) {
      database.add_plain_object_file(object_file, config);
    }
    if (const auto error = report(options, Phase::reading_archives,
                                  static_cast<std::uint32_t>(total_input_files),
                                  static_cast<std::uint32_t>(total_input_files))) {
      return Result<Summary>::failure(*error);
    }

    if (const auto error = cancellation_error(options)) {
      return Result<Summary>::failure(*error);
    }
    if (const auto error = report(options, Phase::linking_objects, 0, 1)) {
      return Result<Summary>::failure(*error);
    }
    database.process_link_data(config);
    database.find_code(config);
    database.process_labels();
    if (config.find_functions) {
      database.ir2_top_level_pass(config);
    }
    if (config.process_art_groups) {
      database.extract_art_info();
    } else if (!config.art_group_info_dump.empty() || !config.jg_info_dump.empty()) {
      database.dts.art_group_info = config.art_group_info_dump;
      database.dts.jg_info = config.jg_info_dump;
    } else {
      return Result<Summary>::failure(make_error(
          ErrorCode::configuration_failed,
          "The " + std::string(profile.display_name) + " art-group metadata is unavailable."));
    }
    if (config.process_part_group_table && !config.part_group_table.empty()) {
      database.dts.part_group_table = config.part_group_table;
    }
    if (config.process_tpages && !config.texture_info_dump.empty()) {
      database.dts.textures = config.texture_info_dump;
    }
    if (const auto error = report(options, Phase::linking_objects, 1, 1)) {
      return Result<Summary>::failure(*error);
    }

    if (const auto error = cancellation_error(options)) {
      return Result<Summary>::failure(*error);
    }
    if (const auto error = report(options, Phase::extracting_intermediates, 0, 4,
                                  "raw objects")) {
      return Result<Summary>::failure(*error);
    }
    database.dump_raw_objects(raw_objects.string());
    if (const auto error = output_budget_error(work_root, options)) {
      return Result<Summary>::failure(*error);
    }
    auto game_text = database.process_game_text_files(config);
    if (game_text.empty()) {
      return Result<Summary>::failure(
          make_error(ErrorCode::extraction_failed,
                     std::string(profile.display_name) + " game text extraction was empty."));
    }
    file_util::write_text_file((assets / "game_text.txt").string(), game_text);
    if (const auto error = report(options, Phase::extracting_intermediates, 1, 4, "game text")) {
      return Result<Summary>::failure(*error);
    }

    decompiler::TextureDB texture_database;
    auto tpage_directory = database.process_tpages(texture_database, textures.string(), config,
                                                   (intermediates / "import").string());
    if (tpage_directory.empty()) {
      return Result<Summary>::failure(
          make_error(ErrorCode::extraction_failed,
                     std::string(profile.display_name) + " texture extraction was empty."));
    }
    file_util::write_text_file((textures / "tpage-dir.txt").string(), tpage_directory);
    file_util::write_text_file((textures / "tex-remap.txt").string(),
                               texture_database.generate_texture_dest_adjustment_table());
    const auto texture_merges = project_root / "game/assets" / std::string(profile.project_name) /
                                "texture_merges";
    if (fs::exists(texture_merges)) {
      texture_database.merge_textures(ghc::filesystem::path(texture_merges.string()));
    }
    const auto texture_replacements = project_root / "custom_assets" /
                                      std::string(profile.project_name) / "texture_replacements";
    if (fs::exists(texture_replacements)) {
      texture_database.replace_textures(ghc::filesystem::path(texture_replacements.string()));
    }
    if (const auto error = output_budget_error(work_root, options)) {
      return Result<Summary>::failure(*error);
    }
    if (const auto error = report(options, Phase::extracting_intermediates, 2, 4, "textures")) {
      return Result<Summary>::failure(*error);
    }

    auto game_count = database.process_game_count_file();
    if (game_count.empty()) {
      return Result<Summary>::failure(
          make_error(ErrorCode::extraction_failed,
                     std::string(profile.display_name) + " game-count extraction was empty."));
    }
    file_util::write_text_file((assets / "game_count.txt").string(), game_count);
    if (const auto error = report(options, Phase::extracting_intermediates, 3, 4, "game count")) {
      return Result<Summary>::failure(*error);
    }

    auto expected_fr3 = fr3_files(fr3);
    decompiler::extract_common(database, texture_database, "GAME.CGO", fr3.string(), config);
    const auto common_outputs = fr3_files(fr3);
    if (common_outputs.size() != expected_fr3.size() + 1 ||
        !common_outputs.contains("GAME.fr3")) {
      return Result<Summary>::failure(make_error(
          ErrorCode::output_incomplete, "Common extraction did not create exactly GAME.fr3."));
    }
    expected_fr3 = common_outputs;
    if (const auto error = output_budget_error(work_root, options)) {
      return Result<Summary>::failure(*error);
    }
    if (const auto error = report(options, Phase::extracting_intermediates, 4, 4, "GAME.fr3")) {
      return Result<Summary>::failure(*error);
    }

    for (std::size_t index = 0; index < config.levels_to_extract.size(); ++index) {
      if (const auto error = cancellation_error(options)) {
        return Result<Summary>::failure(*error);
      }
      const auto& level = config.levels_to_extract[index];
      if (const auto error = report(options, Phase::extracting_levels,
                                    static_cast<std::uint32_t>(index),
                                    static_cast<std::uint32_t>(config.levels_to_extract.size()),
                                    level)) {
        return Result<Summary>::failure(*error);
      }
      decompiler::extract_from_level(database, texture_database, level, config, fr3.string(),
                                     entities.string());
      const auto current_fr3 = fr3_files(fr3);
      if (current_fr3.size() != expected_fr3.size() + 1 ||
          !std::includes(current_fr3.begin(), current_fr3.end(), expected_fr3.begin(),
                         expected_fr3.end())) {
        return Result<Summary>::failure(make_error(
            ErrorCode::output_incomplete,
            "A level extraction did not create exactly one safe FR3 output."));
      }
      expected_fr3 = current_fr3;
      if (const auto error = output_budget_error(work_root, options)) {
        return Result<Summary>::failure(*error);
      }
    }
    if (const auto error = report(options, Phase::extracting_levels,
                                  static_cast<std::uint32_t>(config.levels_to_extract.size()),
                                  static_cast<std::uint32_t>(config.levels_to_extract.size()))) {
      return Result<Summary>::failure(*error);
    }

    Summary summary;
    summary.archives_read = static_cast<std::uint32_t>(total_input_files);
    summary.levels_written = 0;
    summary.output_bytes = directory_size(work_root);
    directory_size(fr3, &summary.levels_written);
    directory_size(raw_objects, &summary.raw_objects_written);
    const auto expected_levels = static_cast<std::uint32_t>(config.levels_to_extract.size() + 1);
    if (summary.levels_written != expected_levels || expected_fr3.size() != expected_levels) {
      return Result<Summary>::failure(make_error(
          ErrorCode::output_incomplete, "FR3 preparation did not produce every expected level."));
    }
    if (const auto error = output_budget_error(work_root, options)) {
      return Result<Summary>::failure(*error);
    }

    owned_work.keep();
    return Result<Summary>::success(std::move(summary));
  } catch (const UnsafeOutputError& error) {
    return Result<Summary>::failure(make_error(ErrorCode::unsafe_output, error.what()));
  } catch (const std::exception& error) {
    return Result<Summary>::failure(make_error(ErrorCode::extraction_failed, error.what()));
  }
}

Result<Summary> prepare(const fs::path& project_root,
                        const fs::path& extracted_iso_root,
                        const fs::path& work_root,
                        const jak1_iso::Revision& revision,
                        const Options& options) {
  if (!valid_options(options) || project_root.empty() || extracted_iso_root.empty() ||
      work_root.empty()) {
    return Result<Summary>::failure(
        make_error(ErrorCode::invalid_argument, "The FR3 preparation arguments are invalid."));
  }
  if (!supported_revision(revision)) {
    return Result<Summary>::failure(
        make_error(ErrorCode::unsupported_revision,
                   "This FR3 preparer currently supports only the verified NTSC-U v1 revision."));
  }
  return prepare_for_profile(project_root, extracted_iso_root, work_root, kJak1Profile, options);
}

static Result<Summary> prepare_jak2(const fs::path& project_root,
                                    const fs::path& extracted_iso_root,
                                    const fs::path& work_root,
                                    const Options& options) {
  return prepare_for_profile(project_root, extracted_iso_root, work_root, kJak2Profile, options);
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::output_exists:
      return "output_exists";
    case ErrorCode::output_creation_failed:
      return "output_creation_failed";
    case ErrorCode::project_setup_failed:
      return "project_setup_failed";
    case ErrorCode::configuration_failed:
      return "configuration_failed";
    case ErrorCode::input_missing:
      return "input_missing";
    case ErrorCode::archive_failed:
      return "archive_failed";
    case ErrorCode::archive_limit_exceeded:
      return "archive_limit_exceeded";
    case ErrorCode::extraction_failed:
      return "extraction_failed";
    case ErrorCode::output_limit_exceeded:
      return "output_limit_exceeded";
    case ErrorCode::output_incomplete:
      return "output_incomplete";
    case ErrorCode::unsafe_output:
      return "unsafe_output";
  }
  return "unknown";
}

}  // namespace jak1_fr3

namespace jak2_fr3 {

Result<Summary> prepare(const std::filesystem::path& project_root,
                        const std::filesystem::path& extracted_iso_root,
                        const std::filesystem::path& work_root,
                        const Options& options) {
  return jak1_fr3::prepare_jak2(project_root, extracted_iso_root, work_root, options);
}

}  // namespace jak2_fr3
