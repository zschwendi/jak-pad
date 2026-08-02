#include <algorithm>
#include <new>
#include <unordered_set>

#include "Jak1OutputRecipeGenerator.h"
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
              {description_entry.file_name, description_entry.name_in_dgo, *kind});
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
