#include "Jak1OutputRecipeGenerator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_output_recipe_generator {
namespace {

namespace fs = std::filesystem;

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
      return make_error(ErrorCode::cancelled, "Output-recipe generation was cancelled.",
                        archive_index, object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The output-recipe cancellation callback failed.",
                      archive_index, object_index);
  }
  return {};
}

bool valid_options(const Options& options) {
  return options.limits.max_manifest_bytes > 0 && options.limits.max_manifest_entries > 0 &&
         options.limits.max_catalog_entries > 0 && options.limits.max_graph_archives > 0 &&
         options.limits.max_graph_objects > 0 && options.limits.max_path_bytes > 0 &&
         options.limits.max_name_bytes > 0 && !options.iso_target.empty() &&
         !options.source_target.empty() && options.expected_source_object_count > 0 &&
         options.expected_source_object_count <= options.limits.max_manifest_entries;
}

bool has_suffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool valid_name(std::string_view value, uint32_t cap) {
  if (value.empty() || value.size() > cap || value == "." || value == ".." || value.back() == '.') {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

bool safe_relative_path(std::string_view value, uint32_t cap) {
  if (value.empty() || value.size() > cap || value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  size_t component_start = 0;
  for (size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size() && value[index] != '/') {
      const auto byte = static_cast<unsigned char>(value[index]);
      if (byte < 0x21 || byte > 0x7e || value[index] == ':') {
        return false;
      }
      continue;
    }
    const auto component = value.substr(component_start, index - component_start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    component_start = index + 1;
  }
  return true;
}

std::string collision_key(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  return result;
}

bool parse_u32(std::string_view text, uint32_t* value) {
  if (text.empty() || text.front() == '+' || text.front() == '-') {
    return false;
  }
  uint64_t wide = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), wide, 10);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size() ||
      wide > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *value = static_cast<uint32_t>(wide);
  return true;
}

bool parse_u64(std::string_view text, uint64_t* value) {
  if (text.empty() || text.front() == '+' || text.front() == '-') {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value, 10);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool parse_hex64(std::string_view text, uint64_t* value) {
  if (text.size() != 16 || std::any_of(text.begin(), text.end(), [](char byte) {
        return !((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'));
      })) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), *value, 16);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

bool split_row(std::string_view row, std::array<std::string_view, 5>* fields) {
  size_t start = 0;
  for (size_t index = 0; index < fields->size(); ++index) {
    const auto tab = row.find('\t', start);
    if (index + 1 == fields->size()) {
      if (tab != std::string_view::npos) {
        return false;
      }
      fields->at(index) = row.substr(start);
    } else {
      if (tab == std::string_view::npos) {
        return false;
      }
      fields->at(index) = row.substr(start, tab - start);
      start = tab + 1;
    }
  }
  return true;
}

std::string source_tag(std::string_view source) {
  const auto slash = source.find_last_of('/');
  auto basename = source.substr(slash == std::string_view::npos ? 0 : slash + 1);
  const auto dot = basename.find_last_of('.');
  if (dot != std::string_view::npos) {
    basename = basename.substr(0, dot);
  }
  return std::string(basename);
}

bool is_initial_revision(const jak1_output_recipe::RevisionProvenance& revision) {
  const auto& expected = jak1_iso::default_revision();
  return revision.serial == expected.serial && revision.executable_hash == expected.elf_hash &&
         revision.contents_hash == expected.contents_hash &&
         revision.file_count == expected.file_count &&
         revision.config_version == expected.decomp_config_version &&
         revision.territory == expected.territory && revision.black_label == expected.black_label;
}

std::optional<std::string> relative_to_root(const std::string& source,
                                            const std::string& root,
                                            uint32_t cap) {
  if (root.empty()) {
    return {};
  }
  const auto normalized_source = fs::path(source).lexically_normal();
  const auto normalized_root = fs::path(root).lexically_normal();
  auto relative = normalized_source.lexically_relative(normalized_root);
  if (relative.empty() || relative.is_absolute()) {
    return {};
  }
  for (const auto& component : relative) {
    if (component == ".." || component == ".") {
      return {};
    }
  }
  auto result = relative.generic_string();
  if (!safe_relative_path(result, cap)) {
    return {};
  }
  return result;
}

bool same_retail_payload(const RetailCatalogObject& left, const RetailCatalogObject& right) {
  return left.internal_name == right.internal_name && left.object_version == right.object_version &&
         left.size == right.size && left.xxh64 == right.xxh64;
}

std::string basename(std::string_view path) {
  const auto slash = path.find_last_of("/\\");
  return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
}

Result<const RetailCatalogObject*> select_retail_object(
    const GraphArchive& archive,
    const GraphObject& object,
    const std::unordered_map<std::string, std::vector<const RetailCatalogObject*>>& by_unique_name,
    uint32_t archive_index,
    uint32_t object_index) {
  auto prepared_unique_name = object.prepared_basename;
  if (!has_suffix(prepared_unique_name, ".go")) {
    return Result<const RetailCatalogObject*>::failure(make_error(
        ErrorCode::invalid_graph, "A retail graph object does not have a .go prepared basename.",
        archive_index, object_index));
  }
  prepared_unique_name.resize(prepared_unique_name.size() - 3);

  const auto found = by_unique_name.find(prepared_unique_name);
  if (found == by_unique_name.end()) {
    return Result<const RetailCatalogObject*>::failure(make_error(
        ErrorCode::missing_retail_object,
        "No verified retail object matches prepared object " + object.prepared_basename + ".",
        archive_index, object_index));
  }

  std::vector<const RetailCatalogObject*> candidates;
  for (const auto* candidate : found->second) {
    if (candidate->internal_name == object.internal_name) {
      candidates.push_back(candidate);
    }
  }
  if (candidates.empty()) {
    return Result<const RetailCatalogObject*>::failure(
        make_error(ErrorCode::missing_retail_object,
                   "The verified retail object for " + object.prepared_basename +
                       " has a different internal DGO name.",
                   archive_index, object_index));
  }

  const auto destination_key = collision_key(archive.destination_basename);
  std::vector<const RetailCatalogObject*> archive_matches;
  for (const auto* candidate : candidates) {
    if (collision_key(basename(candidate->source_archive_relative_path)) == destination_key) {
      archive_matches.push_back(candidate);
    }
  }
  if (archive_matches.size() == 1) {
    return Result<const RetailCatalogObject*>::success(archive_matches.front());
  }
  if (!archive_matches.empty()) {
    candidates = std::move(archive_matches);
  }

  const auto* first = candidates.front();
  if (!std::all_of(candidates.begin() + 1, candidates.end(), [&](const auto* candidate) {
        return same_retail_payload(*first, *candidate);
      })) {
    return Result<const RetailCatalogObject*>::failure(make_error(
        ErrorCode::ambiguous_retail_object,
        "Multiple divergent verified retail objects match " + object.prepared_basename + ".",
        archive_index, object_index));
  }

  const auto selected = std::min_element(
      candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
        return std::tie(left->source_archive_relative_path, left->archive_object_index) <
               std::tie(right->source_archive_relative_path, right->archive_object_index);
      });
  return Result<const RetailCatalogObject*>::success(*selected);
}

std::optional<jak1_output_recipe::GeneratedDataKind> generated_kind(ObjectProducerKind producer) {
  switch (producer) {
    case ObjectProducerKind::directory_tpages:
      return jak1_output_recipe::GeneratedDataKind::directory_tpages;
    case ObjectProducerKind::game_count:
      return jak1_output_recipe::GeneratedDataKind::game_count;
    case ObjectProducerKind::custom_actor:
      return jak1_output_recipe::GeneratedDataKind::custom_actor;
    case ObjectProducerKind::custom_level:
      return jak1_output_recipe::GeneratedDataKind::custom_level;
    default:
      return {};
  }
}

}  // namespace

Result<SourceObjectPackManifest> parse_source_object_pack_manifest(std::string_view bytes,
                                                                   const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<SourceObjectPackManifest>::failure(make_error(
          ErrorCode::invalid_argument, "The source-object-pack parser options are invalid."));
    }
    if (bytes.empty() || bytes.size() > options.limits.max_manifest_bytes || bytes.back() != '\n' ||
        bytes.find('\r') != std::string_view::npos) {
      return Result<SourceObjectPackManifest>::failure(make_error(
          ErrorCode::invalid_manifest, "The source-object-pack manifest framing is invalid."));
    }
    if (const auto error = check_cancelled(options)) {
      return Result<SourceObjectPackManifest>::failure(*error);
    }

    size_t position = 0;
    const auto next_line = [&]() -> std::optional<std::string_view> {
      if (position >= bytes.size()) {
        return {};
      }
      const auto end = bytes.find('\n', position);
      if (end == std::string_view::npos) {
        return {};
      }
      const auto line = bytes.substr(position, end - position);
      position = end + 1;
      return line;
    };

    const auto format = next_line();
    const auto count_line = next_line();
    const auto aggregate_line = next_line();
    const auto header = next_line();
    if (!format || *format != "FORMAT\tgoalc-source-object-pack-v1" || !count_line ||
        !aggregate_line || !header || *header != "SOURCE\tTAG\tFILE\tBYTES\tXXH64") {
      return Result<SourceObjectPackManifest>::failure(
          make_error(ErrorCode::invalid_manifest, "The source-object-pack header is invalid."));
    }

    constexpr std::string_view kCountPrefix = "COUNT\t";
    constexpr std::string_view kAggregatePrefix = "AGGREGATE_XXH64\t";
    uint32_t declared_count = 0;
    uint64_t declared_aggregate = 0;
    if (!count_line->starts_with(kCountPrefix) ||
        !parse_u32(count_line->substr(kCountPrefix.size()), &declared_count) ||
        declared_count == 0 || declared_count != options.expected_source_object_count ||
        !aggregate_line->starts_with(kAggregatePrefix) ||
        !parse_hex64(aggregate_line->substr(kAggregatePrefix.size()), &declared_aggregate) ||
        declared_aggregate == 0) {
      return Result<SourceObjectPackManifest>::failure(make_error(
          ErrorCode::invalid_manifest, "The source-object-pack count or aggregate is invalid."));
    }

    const auto rows_offset = position;
    const auto computed_aggregate =
        XXH64(bytes.data() + rows_offset, bytes.size() - rows_offset, 0);
    if (computed_aggregate != declared_aggregate) {
      return Result<SourceObjectPackManifest>::failure(
          make_error(ErrorCode::manifest_hash_mismatch,
                     "The source-object-pack row aggregate does not match."));
    }

    SourceObjectPackManifest manifest;
    manifest.identity = {declared_count, declared_aggregate};
    manifest.entries.reserve(declared_count);
    std::unordered_set<std::string> source_files;
    std::unordered_set<std::string> tags;
    std::unordered_set<std::string> bundle_paths;
    while (position < bytes.size()) {
      if (const auto error = check_cancelled(options)) {
        return Result<SourceObjectPackManifest>::failure(*error);
      }
      const auto line = next_line();
      std::array<std::string_view, 5> fields{};
      uint64_t byte_size = 0;
      uint64_t hash = 0;
      if (!line || line->empty() || !split_row(*line, &fields) ||
          !safe_relative_path(fields[0], options.limits.max_path_bytes) ||
          !valid_name(fields[1], options.limits.max_name_bytes) ||
          !valid_name(fields[2], options.limits.max_name_bytes) || !has_suffix(fields[2], ".o") ||
          source_tag(fields[0]) != fields[1] || fields[2] != std::string(fields[1]) + ".o" ||
          !parse_u64(fields[3], &byte_size) || byte_size == 0 || !parse_hex64(fields[4], &hash)) {
        return Result<SourceObjectPackManifest>::failure(make_error(
            ErrorCode::invalid_manifest, "A source-object-pack manifest row is invalid."));
      }
      if (!source_files.emplace(collision_key(fields[0])).second ||
          !tags.emplace(collision_key(fields[1])).second ||
          !bundle_paths.emplace(collision_key(fields[2])).second) {
        return Result<SourceObjectPackManifest>::failure(make_error(
            ErrorCode::invalid_manifest, "The source-object-pack manifest repeats an identity."));
      }
      manifest.entries.push_back({std::string(fields[0]), std::string(fields[1]),
                                  std::string(fields[2]), byte_size, hash});
      if (manifest.entries.size() > declared_count) {
        return Result<SourceObjectPackManifest>::failure(make_error(
            ErrorCode::invalid_manifest, "The source-object-pack has more rows than declared."));
      }
    }
    if (manifest.entries.size() != declared_count) {
      return Result<SourceObjectPackManifest>::failure(make_error(
          ErrorCode::invalid_manifest, "The source-object-pack row count does not match."));
    }
    return Result<SourceObjectPackManifest>::success(std::move(manifest));
  } catch (const std::bad_alloc&) {
    return Result<SourceObjectPackManifest>::failure(
        make_error(ErrorCode::allocation_failed, "Source-object-pack parsing ran out of memory."));
  }
}

Result<jak1_output_recipe::Recipe> generate_from_graph(const Graph& graph,
                                                       std::string_view source_object_pack_manifest,
                                                       const VerifiedInputs& verified_inputs,
                                                       const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<jak1_output_recipe::Recipe>::failure(make_error(
          ErrorCode::invalid_argument, "The output-recipe generator options are invalid."));
    }
    if (!is_initial_revision(verified_inputs.revision)) {
      return Result<jak1_output_recipe::Recipe>::failure(make_error(
          ErrorCode::unsupported_revision,
          "Jak 1 output recipes currently support only SCUS-97124 ntsc_v1 black label."));
    }
    if (graph.archives.empty() || graph.archives.size() > options.limits.max_graph_archives ||
        graph.ordered_source_files.empty() ||
        verified_inputs.retail_catalog.size() > options.limits.max_catalog_entries) {
      return Result<jak1_output_recipe::Recipe>::failure(
          make_error(ErrorCode::invalid_graph, "The projected Jak 1 build graph is invalid."));
    }

    auto parsed_manifest = parse_source_object_pack_manifest(source_object_pack_manifest, options);
    if (!parsed_manifest) {
      return Result<jak1_output_recipe::Recipe>::failure(parsed_manifest.error());
    }
    auto manifest = parsed_manifest.take_value();
    if (manifest.entries.size() != graph.ordered_source_files.size()) {
      return Result<jak1_output_recipe::Recipe>::failure(make_error(
          ErrorCode::manifest_graph_mismatch,
          "The source-object-pack count does not match the exact GROUP:all-code graph."));
    }
    for (size_t index = 0; index < manifest.entries.size(); ++index) {
      if (manifest.entries[index].source_file != graph.ordered_source_files[index]) {
        return Result<jak1_output_recipe::Recipe>::failure(make_error(
            ErrorCode::manifest_graph_mismatch,
            "The source-object-pack order does not match the exact GROUP:all-code graph."));
      }
    }

    std::unordered_map<std::string, const SourceObjectPackEntry*> source_objects;
    for (const auto& entry : manifest.entries) {
      source_objects.emplace(entry.bundle_relative_path, &entry);
    }

    std::unordered_map<std::string, std::vector<const RetailCatalogObject*>> retail_by_unique_name;
    std::unordered_set<std::string> catalog_identities;
    for (const auto& object : verified_inputs.retail_catalog) {
      if (const auto error = check_cancelled(options)) {
        return Result<jak1_output_recipe::Recipe>::failure(*error);
      }
      if (!safe_relative_path(object.source_archive_relative_path, options.limits.max_path_bytes) ||
          !(has_suffix(object.source_archive_relative_path, ".DGO") ||
            has_suffix(object.source_archive_relative_path, ".CGO")) ||
          !valid_name(object.internal_name, options.limits.max_name_bytes) ||
          !valid_name(object.unique_name, options.limits.max_name_bytes) ||
          (object.object_version != 2 && object.object_version != 4) || object.size == 0 ||
          !catalog_identities
               .emplace(collision_key(object.source_archive_relative_path) + "\n" +
                        std::to_string(object.archive_object_index))
               .second) {
        return Result<jak1_output_recipe::Recipe>::failure(make_error(
            ErrorCode::invalid_retail_catalog, "The verified retail-object catalog is invalid."));
      }
      retail_by_unique_name[object.unique_name].push_back(&object);
    }

    std::unordered_set<std::string> verified_flat_paths;
    for (const auto& path : verified_inputs.verified_extracted_iso_relative_paths) {
      if (!safe_relative_path(path, options.limits.max_path_bytes) ||
          !verified_flat_paths.emplace(collision_key(path)).second) {
        return Result<jak1_output_recipe::Recipe>::failure(make_error(
            ErrorCode::invalid_argument, "The verified extracted-ISO path catalog is invalid."));
      }
    }

    jak1_output_recipe::Recipe recipe;
    recipe.revision = verified_inputs.revision;
    recipe.source_object_pack = manifest.identity;
    size_t total_graph_objects = 0;
    recipe.archives.reserve(graph.archives.size());
    for (uint32_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
      if (const auto error = check_cancelled(options, archive_index)) {
        return Result<jak1_output_recipe::Recipe>::failure(*error);
      }
      const auto& graph_archive = graph.archives[archive_index];
      if (graph_archive.objects.empty() ||
          graph_archive.objects.size() > options.limits.max_graph_objects - total_graph_objects) {
        return Result<jak1_output_recipe::Recipe>::failure(
            make_error(ErrorCode::invalid_graph,
                       "An output archive exceeds the graph object limit.", archive_index));
      }
      total_graph_objects += graph_archive.objects.size();
      jak1_output_recipe::ArchiveRecord archive;
      archive.destination_basename = graph_archive.destination_basename;
      archive.objects.reserve(graph_archive.objects.size());
      for (uint32_t object_index = 0; object_index < graph_archive.objects.size(); ++object_index) {
        if (const auto error = check_cancelled(options, archive_index, object_index)) {
          return Result<jak1_output_recipe::Recipe>::failure(*error);
        }
        const auto& graph_object = graph_archive.objects[object_index];
        jak1_output_recipe::ObjectEntry object;
        object.internal_name = graph_object.internal_name;
        if (graph_object.producer == ObjectProducerKind::bundled_source) {
          const auto found = source_objects.find(graph_object.prepared_basename);
          if (found == source_objects.end() || found->second->tag != graph_object.internal_name) {
            return Result<jak1_output_recipe::Recipe>::failure(
                make_error(ErrorCode::missing_source_object,
                           "A graph object is missing from the exact source-object pack.",
                           archive_index, object_index));
          }
          object.source = jak1_output_recipe::BundledSourceObject{
              found->second->bundle_relative_path, found->second->size, found->second->xxh64};
        } else if (graph_object.producer == ObjectProducerKind::verified_retail) {
          auto selected = select_retail_object(graph_archive, graph_object, retail_by_unique_name,
                                               archive_index, object_index);
          if (!selected) {
            return Result<jak1_output_recipe::Recipe>::failure(selected.error());
          }
          const auto* retail = selected.value();
          object.source = jak1_output_recipe::VerifiedRetailObject{
              retail->source_archive_relative_path, retail->archive_object_index,
              retail->object_version, retail->size, retail->xxh64};
        } else {
          const auto kind = generated_kind(graph_object.producer);
          if (!kind) {
            return Result<jak1_output_recipe::Recipe>::failure(make_error(
                ErrorCode::invalid_graph, "An archive object has an unknown producer kind.",
                archive_index, object_index));
          }
          object.source = jak1_output_recipe::GeneratedData{*kind};
        }
        archive.objects.push_back(std::move(object));
      }
      recipe.archives.push_back(std::move(archive));
    }

    recipe.flat_file_copies.reserve(graph.flat_file_copies.size());
    for (const auto& copy : graph.flat_file_copies) {
      if (const auto error = check_cancelled(options)) {
        return Result<jak1_output_recipe::Recipe>::failure(*error);
      }
      const auto relative = relative_to_root(copy.source_path, verified_inputs.extracted_iso_root,
                                             options.limits.max_path_bytes);
      if (!relative || !verified_flat_paths.contains(collision_key(*relative))) {
        return Result<jak1_output_recipe::Recipe>::failure(make_error(
            ErrorCode::unverified_flat_source,
            "A GROUP:iso flat-file source is not in the verified extracted-ISO catalog."));
      }
      recipe.flat_file_copies.push_back({*relative, copy.destination_basename});
    }
    for (const auto& generated : graph.generated_flat_files) {
      recipe.generated_flat_files.push_back({generated.kind, generated.destination_basename});
    }
    recipe.expected_fr3_basenames = verified_inputs.expected_fr3_basenames;

    const auto by_destination = [](const auto& left, const auto& right) {
      return left.destination_basename < right.destination_basename;
    };
    std::sort(recipe.archives.begin(), recipe.archives.end(), by_destination);
    std::sort(recipe.flat_file_copies.begin(), recipe.flat_file_copies.end(), by_destination);
    std::sort(recipe.generated_flat_files.begin(), recipe.generated_flat_files.end(),
              by_destination);
    std::sort(recipe.expected_fr3_basenames.begin(), recipe.expected_fr3_basenames.end());

    jak1_output_recipe::Options schema_options;
    schema_options.limits = options.recipe_limits;
    schema_options.expected_source_object_pack = manifest.identity;
    schema_options.should_cancel = options.should_cancel;
    const auto encoded = jak1_output_recipe::encode(recipe, schema_options);
    if (!encoded) {
      return Result<jak1_output_recipe::Recipe>::failure(
          make_error(encoded.error().code == jak1_output_recipe::ErrorCode::cancelled
                         ? ErrorCode::cancelled
                         : ErrorCode::recipe_validation_failed,
                     std::string("The generated output recipe failed schema validation: ") +
                         encoded.error().message));
    }
    return Result<jak1_output_recipe::Recipe>::success(std::move(recipe));
  } catch (const std::bad_alloc&) {
    return Result<jak1_output_recipe::Recipe>::failure(
        make_error(ErrorCode::allocation_failed, "Output-recipe generation ran out of memory."));
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
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::invalid_manifest:
      return "invalid_manifest";
    case ErrorCode::manifest_hash_mismatch:
      return "manifest_hash_mismatch";
    case ErrorCode::manifest_graph_mismatch:
      return "manifest_graph_mismatch";
    case ErrorCode::invalid_graph:
      return "invalid_graph";
    case ErrorCode::unsupported_step:
      return "unsupported_step";
    case ErrorCode::missing_source_object:
      return "missing_source_object";
    case ErrorCode::invalid_retail_catalog:
      return "invalid_retail_catalog";
    case ErrorCode::missing_retail_object:
      return "missing_retail_object";
    case ErrorCode::ambiguous_retail_object:
      return "ambiguous_retail_object";
    case ErrorCode::unverified_flat_source:
      return "unverified_flat_source";
    case ErrorCode::recipe_validation_failed:
      return "recipe_validation_failed";
  }
  return "unknown";
}

}  // namespace jak1_output_recipe_generator
