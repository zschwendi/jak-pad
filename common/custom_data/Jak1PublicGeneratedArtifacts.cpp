#include "Jak1PublicGeneratedArtifacts.h"

#include <algorithm>
#include <limits>
#include <map>
#include <new>
#include <string_view>

#include "common/custom_data/GoalDataObjectBuilder.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_public_generated_artifacts {
namespace {

namespace graph = jak1_output_graph;
namespace recipe = jak1_output_recipe;
using goal_data_object_builder::Builder;

Error make_error(ErrorCode code,
                 std::string message,
                 std::optional<std::uint32_t> bank_index = {},
                 std::optional<std::uint32_t> item_index = {}) {
  return {code, std::move(message), bank_index, item_index};
}

std::optional<Error> cancellation_error(const Options& options) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, "Generated-artifact building was cancelled.");
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed,
                      "The generated-artifact cancellation callback failed.");
  }
  return {};
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_tpage_entries > 0 && limits.max_game_count_entries > 0 &&
         limits.max_banks > 0 && limits.max_text_lines_per_bank > 0 &&
         limits.max_subtitle_scenes_per_bank > 0 && limits.max_subtitle_lines_per_scene > 0 &&
         limits.max_name_bytes > 0 && limits.max_string_bytes > 0 &&
         limits.max_artifact_bytes > 0 && limits.max_total_bytes >= limits.max_artifact_bytes &&
         (options.subtitle_mode == SubtitleMode::public_content ||
          options.subtitle_mode == SubtitleMode::empty);
}

bool valid_basename(std::string_view value, std::uint32_t cap) {
  if (value.empty() || value.size() > cap || value == "." || value == ".." || value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

bool valid_string(std::string_view value, std::uint32_t cap) {
  return value.size() <= cap && value.size() <= std::numeric_limits<std::uint32_t>::max();
}

bool add_estimate(std::uint64_t* estimate, std::uint64_t amount, std::uint64_t cap) {
  if (*estimate > cap || amount > cap - *estimate) {
    return false;
  }
  *estimate += amount;
  return true;
}

std::string collision_key(std::string_view value) {
  std::string key(value);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  return key;
}

std::string ascii_upper(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte);
  });
  return result;
}

std::optional<recipe::GeneratedDataKind> generated_kind(graph::ObjectProducerKind producer) {
  switch (producer) {
    case graph::ObjectProducerKind::directory_tpages:
      return recipe::GeneratedDataKind::directory_tpages;
    case graph::ObjectProducerKind::game_count:
      return recipe::GeneratedDataKind::game_count;
    case graph::ObjectProducerKind::bundled_source:
    case graph::ObjectProducerKind::verified_retail:
    case graph::ObjectProducerKind::custom_actor:
    case graph::ObjectProducerKind::custom_level:
      return {};
  }
  return {};
}

std::vector<std::uint8_t> build_directory_tpages(const DirectoryTpages& input) {
  Builder builder;
  builder.add_type_tag("texture-page-dir");
  builder.add_word(static_cast<std::uint32_t>(input.lengths.size()));
  for (const auto length : input.lengths) {
    builder.add_word(length);
    builder.add_symbol_link("#f");
    builder.add_symbol_link("#f");
  }
  return builder.generate_v4();
}

std::vector<std::uint8_t> build_game_count(const GameCount& input) {
  Builder builder;
  builder.add_type_tag("game-count-info");
  builder.add_word(static_cast<std::uint32_t>(input.entries.size()));
  for (const auto& entry : input.entries) {
    builder.add_word(entry.money);
    builder.add_word(entry.buzzer);
  }
  builder.add_word(input.unknown_1);
  builder.add_word(input.unknown_2);
  return builder.generate_v4();
}

std::vector<std::uint8_t> build_game_text(const GameTextBank& input) {
  Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(static_cast<std::uint32_t>(input.lines.size()));
  builder.add_word(input.language_id);
  builder.add_ref_to_string(input.group_name);
  for (const auto& line : input.lines) {
    builder.add_word(line.id);
    builder.add_ref_to_string(line.encoded_text);
  }
  return builder.generate_v2();
}

std::vector<std::uint8_t> build_subtitles(const SubtitleBank& input) {
  Builder builder;
  builder.add_type_tag("subtitle-text-info");
  builder.add_word(static_cast<std::uint32_t>(input.scenes.size()));
  builder.add_word(input.language_id);
  builder.add_word(0);

  std::vector<std::uint32_t> line_array_links;
  line_array_links.reserve(input.scenes.size());
  for (const auto& scene : input.scenes) {
    builder.add_word((scene.is_cutscene ? 0u : 2u) |
                     (static_cast<std::uint32_t>(scene.lines.size()) << 16));
    line_array_links.push_back(builder.add_word(0));
    builder.add_ref_to_string(scene.name);
    builder.add_word(scene.hint_id);
  }
  for (std::size_t scene_index = 0; scene_index < input.scenes.size(); ++scene_index) {
    builder.link_word_to_word(line_array_links[scene_index], builder.word_count());
    for (const auto& line : input.scenes[scene_index].lines) {
      builder.add_word(line.frame_start);
      builder.add_ref_to_string(line.encoded_text);
      builder.add_ref_to_string(line.encoded_speaker);
      builder.add_word(line.offscreen ? 1 : 0);
    }
  }
  return builder.generate_v2();
}

struct ObjectRequirement {
  recipe::GeneratedDataKind kind = recipe::GeneratedDataKind::directory_tpages;
  std::string prepared_basename;
  std::string internal_name;
};

Result<std::vector<ObjectRequirement>> object_requirements(const graph::Graph& public_graph) {
  std::map<std::string, ObjectRequirement> unique;
  for (const auto& archive : public_graph.archives) {
    for (const auto& object : archive.objects) {
      const auto kind = generated_kind(object.producer);
      if (!kind) {
        continue;
      }
      const auto key =
          std::to_string(static_cast<unsigned>(*kind)) + "\n" + collision_key(object.internal_name);
      const ObjectRequirement requirement{*kind, object.prepared_basename, object.internal_name};
      const auto [found, inserted] = unique.emplace(key, requirement);
      if (!inserted && (found->second.prepared_basename != requirement.prepared_basename ||
                        found->second.internal_name != requirement.internal_name)) {
        return Result<std::vector<ObjectRequirement>>::failure(make_error(
            ErrorCode::unsupported_graph_output,
            "The public graph maps one generated-object identity to conflicting outputs."));
      }
    }
  }
  std::vector<ObjectRequirement> result;
  for (auto& [key, requirement] : unique) {
    result.push_back(std::move(requirement));
  }
  std::sort(result.begin(), result.end(),
            [](const auto& left, const auto& right) { return left.kind < right.kind; });
  return Result<std::vector<ObjectRequirement>>::success(std::move(result));
}

std::optional<Error> add_artifact(Build* output, Artifact artifact, const Options& options) {
  if (artifact.bytes.empty() || artifact.bytes.size() > options.limits.max_artifact_bytes ||
      artifact.bytes.size() > options.limits.max_total_bytes - output->total_bytes) {
    return make_error(ErrorCode::limit_exceeded,
                      "A generated artifact exceeds the configured output limits.");
  }
  artifact.xxh64 = XXH64(artifact.bytes.data(), artifact.bytes.size(), 0);
  output->total_bytes += artifact.bytes.size();
  output->artifacts.push_back(std::move(artifact));
  return {};
}

std::optional<Error> validate_text_bank(const GameTextBank& bank,
                                        std::uint32_t bank_index,
                                        const Options& options) {
  const auto expected_destination =
      std::to_string(bank.language_id) + ascii_upper(bank.group_name) + ".TXT";
  if (!valid_basename(bank.destination_basename, options.limits.max_name_bytes) ||
      !valid_string(bank.group_name, options.limits.max_name_bytes) || bank.group_name.empty() ||
      bank.destination_basename != expected_destination || bank.lines.empty() ||
      bank.lines.size() > options.limits.max_text_lines_per_bank) {
    return make_error(ErrorCode::invalid_input, "A game-text bank is invalid.", bank_index);
  }
  std::optional<std::uint32_t> previous_id;
  std::uint64_t estimated_bytes = 4096;
  for (std::uint32_t line_index = 0; line_index < bank.lines.size(); ++line_index) {
    const auto& line = bank.lines[line_index];
    if ((previous_id && line.id <= *previous_id) ||
        !valid_string(line.encoded_text, options.limits.max_string_bytes) ||
        !add_estimate(&estimated_bytes, 64 + line.encoded_text.size(),
                      options.limits.max_artifact_bytes)) {
      return make_error(ErrorCode::invalid_input,
                        "A game-text bank is not canonically ordered or contains an oversized "
                        "string.",
                        bank_index, line_index);
    }
    previous_id = line.id;
  }
  return {};
}

std::optional<Error> validate_subtitle_bank(const SubtitleBank& bank,
                                            std::uint32_t bank_index,
                                            const Options& options) {
  const auto expected_destination = std::to_string(bank.language_id) + "SUBTIT.TXT";
  if (!valid_basename(bank.destination_basename, options.limits.max_name_bytes) ||
      bank.destination_basename != expected_destination ||
      (options.subtitle_mode == SubtitleMode::public_content && bank.scenes.empty()) ||
      (options.subtitle_mode == SubtitleMode::empty && !bank.scenes.empty()) ||
      bank.scenes.size() > options.limits.max_subtitle_scenes_per_bank) {
    return make_error(ErrorCode::invalid_input, "A subtitle bank is invalid.", bank_index);
  }
  std::string previous_scene;
  std::uint64_t estimated_bytes = 4096;
  for (std::uint32_t scene_index = 0; scene_index < bank.scenes.size(); ++scene_index) {
    const auto& scene = bank.scenes[scene_index];
    if (!valid_string(scene.name, options.limits.max_name_bytes) || scene.name.empty() ||
        (!previous_scene.empty() && scene.name <= previous_scene) ||
        scene.lines.size() > options.limits.max_subtitle_lines_per_scene ||
        !add_estimate(&estimated_bytes, 64 + scene.name.size(),
                      options.limits.max_artifact_bytes)) {
      return make_error(ErrorCode::invalid_input,
                        "A subtitle scene is invalid or not canonically ordered.", bank_index,
                        scene_index);
    }
    previous_scene = scene.name;
    for (const auto& line : scene.lines) {
      if (!valid_string(line.encoded_text, options.limits.max_string_bytes) ||
          !valid_string(line.encoded_speaker, options.limits.max_string_bytes) ||
          !add_estimate(&estimated_bytes,
                        64 + line.encoded_text.size() + line.encoded_speaker.size(),
                        options.limits.max_artifact_bytes)) {
        return make_error(ErrorCode::invalid_input,
                          "A subtitle line contains an oversized encoded string.", bank_index,
                          scene_index);
      }
    }
  }
  return {};
}

}  // namespace

Result<Build> build(const graph::Graph& public_graph,
                    const Inputs& inputs,
                    const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<Build>::failure(
          make_error(ErrorCode::invalid_argument, "Generated-artifact options are invalid."));
    }
    if (!graph::encode(public_graph)) {
      return Result<Build>::failure(make_error(ErrorCode::invalid_public_graph,
                                               "The checked public output graph is invalid."));
    }
    if (inputs.directory_tpages.lengths.empty() ||
        inputs.directory_tpages.lengths.size() > options.limits.max_tpage_entries ||
        inputs.game_count.entries.empty() ||
        inputs.game_count.entries.size() > options.limits.max_game_count_entries ||
        inputs.game_text.size() > options.limits.max_banks ||
        inputs.subtitles.size() > options.limits.max_banks) {
      return Result<Build>::failure(
          make_error(ErrorCode::limit_exceeded, "Generated-artifact input counts are invalid."));
    }
    if (std::any_of(inputs.directory_tpages.lengths.begin(), inputs.directory_tpages.lengths.end(),
                    [](std::uint32_t length) { return (length & 0xffff7000u) != 0; })) {
      return Result<Build>::failure(make_error(
          ErrorCode::invalid_input, "A directory-tpage length has unsupported flag bits."));
    }
    if (const auto error = cancellation_error(options)) {
      return Result<Build>::failure(*error);
    }

    auto required_objects = object_requirements(public_graph);
    if (!required_objects) {
      return Result<Build>::failure(required_objects.error());
    }
    if (required_objects.value().size() != 2 ||
        required_objects.value()[0].kind != recipe::GeneratedDataKind::directory_tpages ||
        required_objects.value()[1].kind != recipe::GeneratedDataKind::game_count) {
      return Result<Build>::failure(
          make_error(ErrorCode::unsupported_graph_output,
                     "The public graph does not contain the supported generated-object set."));
    }

    std::map<std::string, recipe::GeneratedFlatFileKind> required_flat_files;
    for (const auto& flat : public_graph.generated_flat_files) {
      if (!required_flat_files.emplace(collision_key(flat.destination_basename), flat.kind)
               .second) {
        return Result<Build>::failure(make_error(
            ErrorCode::invalid_public_graph, "The public graph repeats a generated flat output."));
      }
    }

    std::map<std::string, const GameTextBank*> text_by_destination;
    for (std::uint32_t bank_index = 0; bank_index < inputs.game_text.size(); ++bank_index) {
      const auto& bank = inputs.game_text[bank_index];
      if (const auto error = validate_text_bank(bank, bank_index, options)) {
        return Result<Build>::failure(*error);
      }
      const auto key = collision_key(bank.destination_basename);
      const auto required = required_flat_files.find(key);
      if (required == required_flat_files.end() ||
          required->second != recipe::GeneratedFlatFileKind::game_text) {
        return Result<Build>::failure(make_error(ErrorCode::unexpected_input,
                                                 "A game-text bank is not in the public graph.",
                                                 bank_index));
      }
      if (!text_by_destination.emplace(key, &bank).second) {
        return Result<Build>::failure(make_error(
            ErrorCode::duplicate_input, "A game-text destination is duplicated.", bank_index));
      }
    }

    std::map<std::string, const SubtitleBank*> subtitles_by_destination;
    for (std::uint32_t bank_index = 0; bank_index < inputs.subtitles.size(); ++bank_index) {
      const auto& bank = inputs.subtitles[bank_index];
      if (const auto error = validate_subtitle_bank(bank, bank_index, options)) {
        return Result<Build>::failure(*error);
      }
      const auto key = collision_key(bank.destination_basename);
      const auto required = required_flat_files.find(key);
      if (required == required_flat_files.end() ||
          required->second != recipe::GeneratedFlatFileKind::game_subtitle) {
        return Result<Build>::failure(make_error(ErrorCode::unexpected_input,
                                                 "A subtitle bank is not in the public graph.",
                                                 bank_index));
      }
      if (!subtitles_by_destination.emplace(key, &bank).second) {
        return Result<Build>::failure(make_error(
            ErrorCode::duplicate_input, "A subtitle destination is duplicated.", bank_index));
      }
    }
    if (text_by_destination.size() + subtitles_by_destination.size() !=
        required_flat_files.size()) {
      return Result<Build>::failure(make_error(
          ErrorCode::missing_input, "One or more public generated flat outputs are missing."));
    }

    Build output;
    for (const auto& requirement : required_objects.value()) {
      if (const auto error = cancellation_error(options)) {
        return Result<Build>::failure(*error);
      }
      Artifact artifact;
      artifact.storage = ArtifactStorage::object;
      artifact.object_kind = requirement.kind;
      artifact.internal_name = requirement.internal_name;
      artifact.destination_basename = requirement.prepared_basename;
      artifact.relative_path = "obj/" + requirement.prepared_basename;
      artifact.bytes = requirement.kind == recipe::GeneratedDataKind::directory_tpages
                           ? build_directory_tpages(inputs.directory_tpages)
                           : build_game_count(inputs.game_count);
      if (const auto error = add_artifact(&output, std::move(artifact), options)) {
        return Result<Build>::failure(*error);
      }
    }

    for (const auto& flat : public_graph.generated_flat_files) {
      if (const auto error = cancellation_error(options)) {
        return Result<Build>::failure(*error);
      }
      Artifact artifact;
      artifact.storage = ArtifactStorage::flat_file;
      artifact.flat_file_kind = flat.kind;
      artifact.destination_basename = flat.destination_basename;
      artifact.relative_path = "iso/" + flat.destination_basename;
      const auto key = collision_key(flat.destination_basename);
      if (flat.kind == recipe::GeneratedFlatFileKind::game_text) {
        const auto found = text_by_destination.find(key);
        if (found == text_by_destination.end()) {
          return Result<Build>::failure(
              make_error(ErrorCode::missing_input, "A required game-text bank is missing."));
        }
        artifact.bytes = build_game_text(*found->second);
      } else if (flat.kind == recipe::GeneratedFlatFileKind::game_subtitle) {
        const auto found = subtitles_by_destination.find(key);
        if (found == subtitles_by_destination.end()) {
          return Result<Build>::failure(
              make_error(ErrorCode::missing_input, "A required subtitle bank is missing."));
        }
        artifact.bytes = build_subtitles(*found->second);
      } else {
        return Result<Build>::failure(
            make_error(ErrorCode::unsupported_graph_output,
                       "The public graph contains an unknown flat kind."));
      }
      if (const auto error = add_artifact(&output, std::move(artifact), options)) {
        return Result<Build>::failure(*error);
      }
    }
    return Result<Build>::success(std::move(output));
  } catch (const std::bad_alloc&) {
    return Result<Build>::failure(
        make_error(ErrorCode::allocation_failed, "Generated-artifact building ran out of memory."));
  } catch (const std::exception&) {
    return Result<Build>::failure(
        make_error(ErrorCode::generation_failed, "GOAL data-object generation failed."));
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
    case ErrorCode::invalid_public_graph:
      return "invalid_public_graph";
    case ErrorCode::unsupported_graph_output:
      return "unsupported_graph_output";
    case ErrorCode::missing_input:
      return "missing_input";
    case ErrorCode::duplicate_input:
      return "duplicate_input";
    case ErrorCode::unexpected_input:
      return "unexpected_input";
    case ErrorCode::invalid_input:
      return "invalid_input";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::generation_failed:
      return "generation_failed";
  }
  return "unknown";
}

}  // namespace jak1_public_generated_artifacts
