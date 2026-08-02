#include "Jak1OutputRecipeMakeSystem.h"

#include <algorithm>
#include <new>
#include <unordered_map>
#include <unordered_set>

#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "goalc/make/MakeSystem.h"
#include "goalc/make/Tools.h"

namespace jak1_output_recipe_generator {
namespace {

Error make_error(ErrorCode code,
                 std::string message,
                 std::optional<uint32_t> archive_index = {},
                 std::optional<uint32_t> object_index = {}) {
  return {code, std::move(message), archive_index, object_index};
}

std::optional<Error> check_cancelled(const Options& options,
                                     std::optional<uint32_t> archive_index = {},
                                     std::optional<uint32_t> object_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, "MakeSystem inspection was cancelled.", archive_index,
                        object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed,
                      "The MakeSystem inspection cancellation callback failed.", archive_index,
                      object_index);
  }
  return {};
}

bool valid_options(const Options& options) {
  return options.limits.max_graph_archives > 0 && options.limits.max_graph_objects > 0 &&
         !options.iso_target.empty() && !options.source_target.empty();
}

std::string basename(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
}

std::optional<ObjectProducerKind> producer_kind(std::string_view tool) {
  if (tool == "goalc") {
    return ObjectProducerKind::bundled_source;
  }
  if (tool == "copy") {
    return ObjectProducerKind::verified_retail;
  }
  if (tool == "tpage-dir") {
    return ObjectProducerKind::directory_tpages;
  }
  if (tool == "game-cnt") {
    return ObjectProducerKind::game_count;
  }
  if (tool == "build-actor") {
    return ObjectProducerKind::custom_actor;
  }
  if (tool == "build-level") {
    return ObjectProducerKind::custom_level;
  }
  return {};
}

std::string source_key(std::string_view value) {
  std::string key(value);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte);
  });
  return key;
}

std::optional<Error> attach_public_retail_provenance(Graph* graph) {
  const auto all_objects = parse_commented_json(
      file_util::read_text_file(file_util::get_file_path({"goal_src/jak1/build/all_objs.json"})),
      "goal_src/jak1/build/all_objs.json");
  const auto inputs = parse_commented_json(file_util::read_text_file(file_util::get_file_path(
                                               {"decompiler/config/jak1/ntsc_v1/inputs.jsonc"})),
                                           "decompiler/config/jak1/ntsc_v1/inputs.jsonc");

  std::unordered_map<std::string, std::vector<std::string>> sources_by_unique_name;
  for (const auto& row : all_objects) {
    if (!row.is_array() || row.size() < 4 || !row[0].is_string() || !row[3].is_array()) {
      return make_error(ErrorCode::invalid_graph,
                        "The public Jak 1 object provenance table is malformed.");
    }
    auto sources = row[3].get<std::vector<std::string>>();
    if (sources.empty() ||
        !sources_by_unique_name.emplace(row[0].get<std::string>(), std::move(sources)).second) {
      return make_error(ErrorCode::invalid_graph,
                        "The public Jak 1 object provenance table repeats an identity.");
    }
  }

  struct OrderedArchive {
    std::size_t input_index = 0;
    std::string path;
  };
  std::unordered_map<std::string, OrderedArchive> archives_by_stem;
  const auto dgo_names = inputs.at("dgo_names").get<std::vector<std::string>>();
  for (std::size_t index = 0; index < dgo_names.size(); ++index) {
    const auto& path = dgo_names[index];
    const auto slash = path.find_last_of('/');
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot <= (slash == std::string::npos ? 0 : slash + 1)) {
      return make_error(ErrorCode::invalid_graph,
                        "The public Jak 1 input archive order contains an invalid path.");
    }
    const auto stem = path.substr(slash == std::string::npos ? 0 : slash + 1,
                                  dot - (slash == std::string::npos ? 0 : slash + 1));
    if (!archives_by_stem.emplace(source_key(stem), OrderedArchive{index, path}).second) {
      return make_error(ErrorCode::invalid_graph,
                        "The public Jak 1 input archive order repeats an archive stem.");
    }
  }

  for (std::uint32_t archive_index = 0; archive_index < graph->archives.size(); ++archive_index) {
    auto& archive = graph->archives[archive_index];
    for (std::uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      auto& object = archive.objects[object_index];
      if (object.producer != ObjectProducerKind::verified_retail) {
        continue;
      }
      auto unique_name = object.prepared_basename;
      if (!unique_name.ends_with(".go")) {
        return make_error(ErrorCode::invalid_graph,
                          "A retail graph object has no public .go identity.", archive_index,
                          object_index);
      }
      unique_name.resize(unique_name.size() - 3);
      const auto found_sources = sources_by_unique_name.find(unique_name);
      if (found_sources == sources_by_unique_name.end()) {
        return make_error(ErrorCode::invalid_graph,
                          "A retail graph object is absent from the public provenance table.",
                          archive_index, object_index);
      }

      const OrderedArchive* winner = nullptr;
      for (const auto& source : found_sources->second) {
        const auto found_archive = archives_by_stem.find(source_key(source));
        if (found_archive != archives_by_stem.end() &&
            (!winner || found_archive->second.input_index > winner->input_index)) {
          winner = &found_archive->second;
        }
      }
      if (!winner) {
        return make_error(ErrorCode::invalid_graph,
                          "A retail graph object's public sources are absent from the checked "
                          "input archive order.",
                          archive_index, object_index);
      }
      // Desktop raw_obj generation overwrites equal unique names in dgo_names order. Recording the
      // final public source makes that behavior explicit and independent of retail catalog order.
      object.retail_source_archive = winner->path;
    }
  }
  return {};
}

}  // namespace

Result<Graph> inspect_make_system(const MakeSystem& make_system, const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<Graph>::failure(make_error(ErrorCode::invalid_argument,
                                               "The MakeSystem inspection options are invalid."));
    }
    if (const auto error = check_cancelled(options)) {
      return Result<Graph>::failure(*error);
    }

    Graph graph;
    for (const auto& output : make_system.get_dependencies(options.source_target)) {
      if (const auto error = check_cancelled(options)) {
        return Result<Graph>::failure(*error);
      }
      const auto* step = make_system.find_step(output);
      if (step && step->tool == "goalc") {
        if (step->input.size() != 1) {
          return Result<Graph>::failure(make_error(
              ErrorCode::invalid_graph, "A GROUP:all-code compiler step has invalid inputs."));
        }
        graph.ordered_source_files.push_back(step->input.front());
      }
    }
    if (graph.ordered_source_files.empty()) {
      return Result<Graph>::failure(make_error(
          ErrorCode::invalid_graph, "GROUP:all-code contains no compiler source steps."));
    }

    const auto* iso_group = make_system.find_step(options.iso_target);
    if (!iso_group || iso_group->tool != "group" || iso_group->deps.empty()) {
      return Result<Graph>::failure(
          make_error(ErrorCode::invalid_graph, "GROUP:iso is missing or is not a group step."));
    }

    std::unordered_set<const MakeStep*> processed_multi_output_steps;
    goos::Reader dgo_reader;
    size_t total_objects = 0;
    for (const auto& dependency : iso_group->deps) {
      if (const auto error = check_cancelled(options)) {
        return Result<Graph>::failure(*error);
      }
      const auto* step = make_system.find_step(dependency);
      if (!step) {
        return Result<Graph>::failure(make_error(
            ErrorCode::invalid_graph, "A direct GROUP:iso dependency has no producing step."));
      }

      if (step->tool == "dgo") {
        if (step->outputs.size() != 1 || step->outputs.front() != dependency) {
          return Result<Graph>::failure(
              make_error(ErrorCode::invalid_graph, "A GROUP:iso DGO step has unexpected outputs."));
        }
        if (graph.archives.size() >= options.limits.max_graph_archives) {
          return Result<Graph>::failure(make_error(
              ErrorCode::invalid_graph, "GROUP:iso exceeds the archive inspection limit."));
        }
        GraphArchive archive;
        archive.destination_basename = basename(dependency);
        if (step->input.size() != 1) {
          return Result<Graph>::failure(
              make_error(ErrorCode::invalid_graph, "A GROUP:iso DGO step has invalid inputs."));
        }
        const auto description = parse_dgo_description_file(step->input.front(), dgo_reader);
        if (description.dgo_name != archive.destination_basename || description.entries.empty() ||
            description.entries.size() > options.limits.max_graph_objects - total_objects) {
          return Result<Graph>::failure(
              make_error(ErrorCode::invalid_graph,
                         "A GROUP:iso archive description has the wrong name or object count."));
        }
        archive.objects.reserve(description.entries.size());
        for (uint32_t object_index = 0; object_index < description.entries.size(); ++object_index) {
          if (const auto error = check_cancelled(
                  options, static_cast<uint32_t>(graph.archives.size()), object_index)) {
            return Result<Graph>::failure(*error);
          }
          const auto& description_entry = description.entries[object_index];
          const auto object_dependency =
              "out/" + make_system.compiler_output_prefix() + "obj/" + description_entry.file_name;
          const auto* producer = make_system.find_step(object_dependency);
          const auto kind = producer ? producer_kind(producer->tool) : std::nullopt;
          if (!producer || !kind || description_entry.name_in_dgo.empty()) {
            return Result<Graph>::failure(make_error(
                ErrorCode::unsupported_step,
                "An archive object is produced by an unsupported or malformed MakeSystem step.",
                static_cast<uint32_t>(graph.archives.size()), object_index));
          }
          archive.objects.push_back(
              {description_entry.file_name, description_entry.name_in_dgo, *kind, {}});
        }
        total_objects += archive.objects.size();
        graph.archives.push_back(std::move(archive));
        continue;
      }

      if (step->tool == "copy") {
        if (step->input.size() != 1 || step->outputs.empty()) {
          return Result<Graph>::failure(make_error(
              ErrorCode::invalid_graph, "A GROUP:iso copy step has invalid inputs or outputs."));
        }
        if (!processed_multi_output_steps.emplace(step).second) {
          continue;
        }
        for (const auto& output : step->outputs) {
          graph.flat_file_copies.push_back({step->input.front(), basename(output)});
        }
        continue;
      }

      std::optional<jak1_output_recipe::GeneratedFlatFileKind> generated_kind;
      if (step->tool == "text") {
        generated_kind = jak1_output_recipe::GeneratedFlatFileKind::game_text;
      } else if (step->tool == "subtitle") {
        generated_kind = jak1_output_recipe::GeneratedFlatFileKind::game_subtitle;
      }
      if (generated_kind) {
        if (step->outputs.empty()) {
          return Result<Graph>::failure(make_error(
              ErrorCode::invalid_graph, "A generated GROUP:iso flat-file step has no outputs."));
        }
        if (!processed_multi_output_steps.emplace(step).second) {
          continue;
        }
        for (const auto& output : step->outputs) {
          graph.generated_flat_files.push_back({*generated_kind, basename(output)});
        }
        continue;
      }

      return Result<Graph>::failure(
          make_error(ErrorCode::unsupported_step,
                     "GROUP:iso contains an unsupported direct step: " + step->tool));
    }

    const auto by_destination = [](const auto& left, const auto& right) {
      return left.destination_basename < right.destination_basename;
    };
    std::sort(graph.archives.begin(), graph.archives.end(), by_destination);
    std::sort(graph.flat_file_copies.begin(), graph.flat_file_copies.end(), by_destination);
    std::sort(graph.generated_flat_files.begin(), graph.generated_flat_files.end(), by_destination);
    if (const auto error = attach_public_retail_provenance(&graph)) {
      return Result<Graph>::failure(*error);
    }
    return Result<Graph>::success(std::move(graph));
  } catch (const std::bad_alloc&) {
    return Result<Graph>::failure(
        make_error(ErrorCode::allocation_failed, "MakeSystem inspection ran out of memory."));
  } catch (const std::exception& error) {
    return Result<Graph>::failure(make_error(
        ErrorCode::invalid_graph, std::string("MakeSystem inspection failed: ") + error.what()));
  }
}

Result<jak1_output_recipe::Recipe> generate(const MakeSystem& make_system,
                                            std::string_view source_object_pack_manifest,
                                            const VerifiedInputs& verified_inputs,
                                            const Options& options) {
  auto graph = inspect_make_system(make_system, options);
  if (!graph) {
    return Result<jak1_output_recipe::Recipe>::failure(graph.error());
  }
  return generate_from_graph(graph.value(), source_object_pack_manifest, verified_inputs, options);
}

}  // namespace jak1_output_recipe_generator
