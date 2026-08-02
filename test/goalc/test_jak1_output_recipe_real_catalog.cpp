#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "common/custom_data/Jak1PublicOutputGraph.h"

#include "decompiler/extractor/jak1_retail_object_catalog.h"
#include "goalc/make/Jak1OutputRecipeGenerator.h"

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace {

namespace generator = jak1_output_recipe_generator;
namespace recipe = jak1_output_recipe;
namespace retail_catalog = jak1_retail_object_catalog;
namespace stdfs = std::filesystem;

constexpr std::size_t kExpectedSourceRows = 518;
constexpr std::uintmax_t kMaxManifestBytes = 4 * 1024 * 1024;
constexpr std::uintmax_t kMaxArchiveBytes = 512ull * 1024 * 1024;
constexpr std::uintmax_t kMaxTotalArchiveBytes = 2ull * 1024 * 1024 * 1024;
constexpr std::string_view kGraphIsoRoot = "iso_data/jak1";
constexpr std::string_view kAggregateDomain = "opengoal-jak1-real-catalog-proof-v1";

class ProofFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string message) {
  throw ProofFailure(std::move(message));
}

struct CliOptions {
  stdfs::path extracted_root;
  stdfs::path manifest_path;
  std::size_t expected_catalog_count = 0;
  std::optional<std::uint64_t> expected_aggregate;
};

std::optional<std::uint64_t> parse_unsigned(std::string_view text, int base) {
  if (text.empty() || text.front() == '+' || text.front() == '-') {
    return {};
  }
  std::uint64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value, base);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
    return {};
  }
  return value;
}

CliOptions parse_cli(int argc, char** argv) {
  CliOptions options;
  bool saw_root = false;
  bool saw_manifest = false;
  bool saw_count = false;
  bool saw_aggregate = false;

  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    if (flag == "--help") {
      fail("usage: --validated-extracted-root PATH --object-pack-manifest PATH "
           "--expected-catalog-count COUNT [--expected-aggregate 16_HEX_DIGITS]");
    }
    if (index + 1 >= argc) {
      fail("a command-line option is missing its value");
    }
    const std::string_view value(argv[++index]);
    if (flag == "--validated-extracted-root" && !saw_root) {
      options.extracted_root = stdfs::path(value);
      saw_root = true;
    } else if (flag == "--object-pack-manifest" && !saw_manifest) {
      options.manifest_path = stdfs::path(value);
      saw_manifest = true;
    } else if (flag == "--expected-catalog-count" && !saw_count) {
      const auto parsed = parse_unsigned(value, 10);
      if (!parsed || *parsed == 0 || *parsed > std::numeric_limits<std::uint32_t>::max()) {
        fail("the expected catalog count is invalid");
      }
      options.expected_catalog_count = static_cast<std::size_t>(*parsed);
      saw_count = true;
    } else if (flag == "--expected-aggregate" && !saw_aggregate) {
      const auto parsed = value.size() == 16 ? parse_unsigned(value, 16) : std::nullopt;
      if (!parsed) {
        fail("the expected aggregate must contain exactly 16 hexadecimal digits");
      }
      options.expected_aggregate = *parsed;
      saw_aggregate = true;
    } else {
      fail("an unknown or repeated command-line option was supplied");
    }
  }

  if (!saw_root || !saw_manifest || !saw_count) {
    fail("the validated extracted root, object-pack manifest, and expected catalog count are "
         "required");
  }
  return options;
}

stdfs::path canonical_directory(const stdfs::path& input) {
  if (!input.is_absolute()) {
    fail("the validated extracted root must be an absolute path");
  }
  std::error_code error;
  const auto path = stdfs::canonical(input, error);
  if (error || !stdfs::is_directory(path, error) || error) {
    fail("the validated extracted root is not a readable directory");
  }
  return path;
}

stdfs::path canonical_regular_file(const stdfs::path& input, std::string_view description) {
  if (!input.is_absolute()) {
    fail(std::string(description) + " must be an absolute path");
  }
  std::error_code error;
  const auto path = stdfs::canonical(input, error);
  if (error || !stdfs::is_regular_file(path, error) || error) {
    fail(std::string(description) + " is not a readable regular file");
  }
  return path;
}

stdfs::path selected_archive_path(const stdfs::path& root, const std::string& relative_path) {
  std::error_code error;
  const auto selected = stdfs::canonical(root / relative_path, error);
  if (error || !stdfs::is_regular_file(selected, error) || error) {
    fail("a selected retail archive is missing: " + relative_path);
  }
  const auto actual_relative = selected.lexically_relative(root).generic_string();
  if (actual_relative != relative_path) {
    fail("a selected retail archive escapes or aliases the extracted root: " + relative_path);
  }
  return selected;
}

std::vector<std::uint8_t> read_bounded_file(const stdfs::path& path,
                                            std::uintmax_t byte_limit,
                                            std::string_view description) {
  std::error_code error;
  const auto file_size = stdfs::file_size(path, error);
  if (error || file_size == 0 || file_size > byte_limit ||
      file_size > std::numeric_limits<std::size_t>::max() ||
      file_size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    fail(std::string(description) + " is empty, unreadable, or exceeds its byte limit");
  }

  std::ifstream input(path, std::ios::binary);
  if (!input) {
    fail(std::string(description) + " could not be opened");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_size));
  input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || !input) {
    fail(std::string(description) + " could not be read exactly");
  }
  char extra = 0;
  if (input.get(extra)) {
    fail(std::string(description) + " changed while it was being read");
  }
  return bytes;
}

generator::Graph load_public_graph() {
  const auto result = jak1_public_output_graph::decode();
  if (!result) {
    fail("the embedded public GROUP:iso graph could not be decoded: " + result.error().message);
  }
  if (result.value().ordered_source_files.size() != kExpectedSourceRows ||
      result.value().archives.empty()) {
    fail("the public GROUP:iso graph does not match the checked Jak 1 proof shape");
  }
  return result.value();
}

std::string collision_key(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  return value;
}

bool safe_archive_basename(std::string_view basename, std::string_view extension) {
  if (basename.empty() || basename.size() > 128 || !basename.ends_with(extension)) {
    return false;
  }
  return std::all_of(basename.begin(), basename.end(), [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
  });
}

struct LoadedArchive {
  std::string relative_path;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::string> select_archive_paths(const stdfs::path& extracted_root,
                                              const generator::Graph& graph) {
  const auto dgo_directory = extracted_root / "DGO";
  std::error_code error;
  const auto dgo_status = stdfs::symlink_status(dgo_directory, error);
  if (error || stdfs::is_symlink(dgo_status) || !stdfs::is_directory(dgo_status)) {
    fail("the validated extracted root does not contain a direct regular DGO directory");
  }

  const auto cgo_directory = extracted_root / "CGO";
  const auto cgo_status = stdfs::symlink_status(cgo_directory, error);
  if (error || stdfs::is_symlink(cgo_status) || !stdfs::is_directory(cgo_status)) {
    fail("the validated extracted root does not contain a direct regular CGO directory");
  }
  std::set<std::string> exact_paths;
  std::set<std::string> collision_keys;
  for (const auto& archive : graph.archives) {
    for (const auto& object : archive.objects) {
      if (object.producer != generator::ObjectProducerKind::verified_retail) {
        continue;
      }
      const auto& relative_path = object.retail_source_archive;
      const auto separator = relative_path.find('/');
      if (separator == std::string::npos ||
          relative_path.find('/', separator + 1) != std::string::npos) {
        fail("the embedded graph contains an unsafe retail archive path");
      }
      const auto directory = std::string_view(relative_path).substr(0, separator);
      const auto basename = std::string_view(relative_path).substr(separator + 1);
      const bool safe =
          (directory == "DGO" && safe_archive_basename(basename, ".DGO")) ||
          (directory == "CGO" && safe_archive_basename(basename, ".CGO"));
      if (!safe) {
        fail("the embedded graph contains an unsafe retail archive path");
      }
      if (!exact_paths.emplace(relative_path).second) {
        continue;
      }
      if (!collision_keys.emplace(collision_key(relative_path)).second) {
        fail("the embedded graph contains case-colliding retail archive paths");
      }
    }
  }
  if (exact_paths.empty()) {
    fail("the embedded graph does not select any retail archives");
  }
  return {exact_paths.begin(), exact_paths.end()};
}

std::vector<LoadedArchive> load_selected_archives(const stdfs::path& extracted_root,
                                                  const generator::Graph& graph) {
  std::vector<LoadedArchive> loaded;
  const auto selected_paths = select_archive_paths(extracted_root, graph);
  loaded.reserve(selected_paths.size());
  std::uintmax_t total_bytes = 0;
  for (const auto& relative_path : selected_paths) {
    const auto input_path = selected_archive_path(extracted_root, relative_path);
    std::error_code error;
    const auto input_size = stdfs::file_size(input_path, error);
    if (error || input_size > kMaxTotalArchiveBytes - total_bytes) {
      fail("the graph-selected archives exceed the aggregate input limit");
    }
    total_bytes += input_size;
    loaded.push_back({relative_path,
                      read_bounded_file(input_path, kMaxArchiveBytes, "a selected retail archive")});
  }
  return loaded;
}

std::vector<retail_catalog::ArchiveSource> make_catalog_sources(
    const std::vector<LoadedArchive>& loaded) {
  std::vector<retail_catalog::ArchiveSource> sources;
  sources.reserve(loaded.size());
  for (const auto& archive : loaded) {
    sources.push_back({archive.relative_path, archive.bytes});
  }
  return sources;
}

retail_catalog::Catalog build_catalog(
    std::span<const retail_catalog::ArchiveSource> sources) {
  auto result = retail_catalog::build(sources);
  if (!result) {
    auto message = std::string("the checked retail catalog failed: ") +
                   retail_catalog::error_code_name(result.error().code);
    if (!result.error().source_archive_relative_path.empty()) {
      message += " at " + result.error().source_archive_relative_path;
    }
    if (result.error().archive_object_index) {
      message += " object " + std::to_string(*result.error().archive_object_index);
    }
    fail(std::move(message));
  }
  return result.take_value();
}

recipe::RevisionProvenance initial_revision() {
  const auto& revision = jak1_iso::default_revision();
  return {std::string(revision.serial),
          revision.elf_hash,
          revision.contents_hash,
          revision.file_count,
          std::string(revision.decomp_config_version),
          revision.territory,
          revision.black_label};
}

std::vector<generator::RetailCatalogObject> adapt_catalog(
    const retail_catalog::Catalog& catalog) {
  std::vector<generator::RetailCatalogObject> result;
  result.reserve(catalog.entries().size());
  for (const auto& entry : catalog.entries()) {
    const auto& provenance = entry.provenance;
    result.push_back({provenance.source_archive_relative_path,
                      provenance.archive_object_index,
                      provenance.internal_name,
                      provenance.unique_name,
                      static_cast<std::uint32_t>(provenance.object_version),
                      static_cast<std::uint64_t>(provenance.byte_size),
                      provenance.xxh64});
  }
  return result;
}

std::vector<std::string> adapt_graph_to_root(generator::Graph* graph,
                                             const stdfs::path& extracted_root) {
  std::set<std::string> verified_paths;
  for (auto& copy : graph->flat_file_copies) {
    const auto relative = stdfs::path(copy.source_path)
                              .lexically_relative(stdfs::path(kGraphIsoRoot))
                              .generic_string();
    if (relative.empty() || relative.starts_with("../") || relative == "..") {
      fail("the public graph contains a flat-file source outside its ISO root");
    }
    verified_paths.emplace(relative);
    copy.source_path = (extracted_root / relative).string();
  }
  return {verified_paths.begin(), verified_paths.end()};
}

generator::VerifiedInputs make_verified_inputs(
    const stdfs::path& extracted_root,
    std::vector<std::string> verified_flat_paths,
    const retail_catalog::Catalog& catalog) {
  generator::VerifiedInputs inputs;
  inputs.revision = initial_revision();
  inputs.extracted_iso_root = extracted_root.string();
  inputs.verified_extracted_iso_relative_paths = std::move(verified_flat_paths);
  inputs.retail_catalog = adapt_catalog(catalog);
  return inputs;
}

struct ResolvedOccurrence {
  std::string destination_basename;
  std::uint32_t output_object_index = 0;
  std::string internal_name;
  recipe::VerifiedRetailObject source;

  bool operator==(const ResolvedOccurrence&) const = default;
};

std::vector<ResolvedOccurrence> collect_resolved_occurrences(
    const generator::Graph& graph,
    const recipe::Recipe& output) {
  std::vector<ResolvedOccurrence> occurrences;
  for (const auto& graph_archive : graph.archives) {
    const auto output_archive =
        std::find_if(output.archives.begin(), output.archives.end(), [&](const auto& archive) {
          return archive.destination_basename == graph_archive.destination_basename;
        });
    if (output_archive == output.archives.end() ||
        output_archive->objects.size() != graph_archive.objects.size()) {
      fail("the generated recipe does not retain the public archive graph");
    }
    for (std::size_t index = 0; index < graph_archive.objects.size(); ++index) {
      const auto& graph_object = graph_archive.objects[index];
      const auto& output_object = output_archive->objects[index];
      if (output_object.internal_name != graph_object.internal_name) {
        fail("the generated recipe changes a public graph object identity");
      }
      const auto* retail = std::get_if<recipe::VerifiedRetailObject>(&output_object.source);
      const bool expected_retail =
          graph_object.producer == generator::ObjectProducerKind::verified_retail;
      if (expected_retail != (retail != nullptr)) {
        fail("the generated recipe changes a public graph producer kind");
      }
      if (retail) {
        if (index > std::numeric_limits<std::uint32_t>::max()) {
          fail("a resolved output object index exceeds the canonical aggregate format");
        }
        occurrences.push_back({graph_archive.destination_basename,
                               static_cast<std::uint32_t>(index), output_object.internal_name,
                               *retail});
      }
    }
  }
  std::sort(occurrences.begin(), occurrences.end(), [](const auto& left, const auto& right) {
    return std::tie(left.destination_basename, left.output_object_index, left.internal_name,
                    left.source.source_archive_relative_path, left.source.archive_object_index,
                    left.source.object_version, left.source.size, left.source.xxh64) <
           std::tie(right.destination_basename, right.output_object_index, right.internal_name,
                    right.source.source_archive_relative_path, right.source.archive_object_index,
                    right.source.object_version, right.source.size, right.source.xxh64);
  });
  return occurrences;
}

class CanonicalBytes {
 public:
  void add_u8(std::uint8_t value) { m_bytes.push_back(value); }

  void add_u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void add_u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      m_bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void add_string(std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      fail("a string exceeds the canonical aggregate format");
    }
    add_u32(static_cast<std::uint32_t>(value.size()));
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
  }

  std::uint64_t digest() const { return XXH64(m_bytes.data(), m_bytes.size(), 0); }

 private:
  std::vector<std::uint8_t> m_bytes;
};

std::uint32_t canonical_count(std::size_t value, std::string_view description) {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    fail(std::string(description) + " exceeds the canonical aggregate format");
  }
  return static_cast<std::uint32_t>(value);
}

std::uint64_t canonical_aggregate(const recipe::RevisionProvenance& revision,
                                  const recipe::SourceObjectPackIdentity& source_pack,
                                  const retail_catalog::Catalog& catalog,
                                  const std::vector<ResolvedOccurrence>& occurrences) {
  CanonicalBytes bytes;
  bytes.add_string(kAggregateDomain);

  bytes.add_string("revision");
  bytes.add_string(revision.serial);
  bytes.add_u64(revision.executable_hash);
  bytes.add_u64(revision.contents_hash);
  bytes.add_u32(revision.file_count);
  bytes.add_string(revision.config_version);
  bytes.add_u8(static_cast<std::underlying_type_t<recipe::Territory>>(revision.territory));
  bytes.add_u8(revision.black_label ? 1 : 0);

  bytes.add_string("source-object-pack");
  bytes.add_u32(source_pack.object_count);
  bytes.add_u64(source_pack.aggregate_xxh64);

  std::vector<const retail_catalog::Entry*> catalog_entries;
  catalog_entries.reserve(catalog.entries().size());
  for (const auto& entry : catalog.entries()) {
    catalog_entries.push_back(&entry);
  }
  std::sort(catalog_entries.begin(), catalog_entries.end(), [](const auto* left, const auto* right) {
    const auto& lhs = left->provenance;
    const auto& rhs = right->provenance;
    return std::tie(lhs.source_archive_relative_path, lhs.archive_object_index, lhs.internal_name,
                    lhs.unique_name, lhs.object_version, lhs.byte_size, lhs.xxh64) <
           std::tie(rhs.source_archive_relative_path, rhs.archive_object_index, rhs.internal_name,
                    rhs.unique_name, rhs.object_version, rhs.byte_size, rhs.xxh64);
  });
  bytes.add_string("retail-catalog");
  bytes.add_u32(canonical_count(catalog_entries.size(), "the retail catalog"));
  bytes.add_u32(canonical_count(catalog.skipped_code_object_count(), "the skipped-code count"));
  for (const auto* entry : catalog_entries) {
    const auto& provenance = entry->provenance;
    bytes.add_string(provenance.source_archive_relative_path);
    bytes.add_u32(provenance.archive_object_index);
    bytes.add_string(provenance.internal_name);
    bytes.add_string(provenance.unique_name);
    bytes.add_u32(static_cast<std::uint32_t>(provenance.object_version));
    bytes.add_u64(static_cast<std::uint64_t>(provenance.byte_size));
    bytes.add_u64(provenance.xxh64);
  }

  bytes.add_string("resolved-retail-occurrences");
  bytes.add_u32(canonical_count(occurrences.size(), "the resolved retail occurrence count"));
  for (const auto& occurrence : occurrences) {
    bytes.add_string(occurrence.destination_basename);
    bytes.add_u32(occurrence.output_object_index);
    bytes.add_string(occurrence.internal_name);
    bytes.add_string(occurrence.source.source_archive_relative_path);
    bytes.add_u32(occurrence.source.archive_object_index);
    bytes.add_u32(occurrence.source.object_version);
    bytes.add_u64(occurrence.source.size);
    bytes.add_u64(occurrence.source.xxh64);
  }
  return bytes.digest();
}

struct ProofResult {
  std::size_t source_rows = 0;
  std::size_t archive_count = 0;
  std::size_t catalog_count = 0;
  std::size_t skipped_code_count = 0;
  std::size_t occurrence_count = 0;
  std::uint64_t aggregate = 0;
};

ProofResult run_proof(const CliOptions& cli) {
  // Trust boundary: the caller supplies a root already matched to the supported revision. This
  // proof rechecks path shape and every selected archive, but does not revalidate the entire ISO.
  const auto root = canonical_directory(cli.extracted_root);
  const auto manifest_path = canonical_regular_file(cli.manifest_path, "the object-pack manifest");
  const auto manifest_bytes = read_bounded_file(manifest_path, kMaxManifestBytes,
                                                "the object-pack manifest");
  const std::string_view manifest(reinterpret_cast<const char*>(manifest_bytes.data()),
                                  manifest_bytes.size());
  const auto parsed_manifest = generator::parse_source_object_pack_manifest(manifest);
  if (!parsed_manifest || parsed_manifest.value().entries.size() != kExpectedSourceRows) {
    fail("the checked 518-row object-pack manifest was rejected");
  }

  auto graph = load_public_graph();
  for (std::size_t index = 0; index < graph.ordered_source_files.size(); ++index) {
    if (parsed_manifest.value().entries[index].source_file != graph.ordered_source_files[index]) {
      fail("the checked object-pack manifest does not match the public source graph");
    }
  }

  auto loaded = load_selected_archives(root, graph);
  auto forward_sources = make_catalog_sources(loaded);
  auto reversed_sources = forward_sources;
  std::reverse(reversed_sources.begin(), reversed_sources.end());
  const auto forward_catalog = build_catalog(forward_sources);
  const auto reversed_catalog = build_catalog(reversed_sources);
  const bool identical_entries =
      forward_catalog.entries().size() == reversed_catalog.entries().size() &&
      std::equal(forward_catalog.entries().begin(), forward_catalog.entries().end(),
                 reversed_catalog.entries().begin(), [](const auto& left, const auto& right) {
                   return left.provenance == right.provenance;
                 });
  if (!identical_entries ||
      forward_catalog.skipped_code_object_count() !=
          reversed_catalog.skipped_code_object_count()) {
    fail("reversing the archive input order changed the checked retail catalog");
  }
  if (forward_catalog.entries().size() != cli.expected_catalog_count) {
    fail("catalog_entries=" + std::to_string(forward_catalog.entries().size()) +
         " expected_catalog_entries=" + std::to_string(cli.expected_catalog_count) +
         " error=count_mismatch");
  }

  const auto verified_flat_paths = adapt_graph_to_root(&graph, root);
  const auto forward_inputs = make_verified_inputs(root, verified_flat_paths, forward_catalog);
  const auto reverse_inputs = make_verified_inputs(root, verified_flat_paths, reversed_catalog);
  const auto forward_recipe = generator::generate_from_graph(graph, manifest, forward_inputs);
  const auto reverse_recipe = generator::generate_from_graph(graph, manifest, reverse_inputs);
  if (!forward_recipe || !reverse_recipe) {
    const auto& error = !forward_recipe ? forward_recipe.error() : reverse_recipe.error();
    auto message = std::string("real recipe generation failed: ") +
                   generator::error_code_name(error.code);
    if (error.archive_index) {
      message += " archive=" + std::to_string(*error.archive_index);
    }
    if (error.object_index) {
      message += " object=" + std::to_string(*error.object_index);
    }
    if (!error.message.empty()) {
      message += ": " + error.message;
    }
    if (error.archive_index && error.object_index && *error.archive_index < graph.archives.size() &&
        *error.object_index < graph.archives[*error.archive_index].objects.size()) {
      const auto& object = graph.archives[*error.archive_index].objects[*error.object_index];
      message += " expected_source=" + object.retail_source_archive;
      message += " internal_name=" + object.internal_name;
    }
    fail(std::move(message));
  }
  if (forward_recipe.value() != reverse_recipe.value()) {
    fail("reversing the archive input order changed the generated recipe");
  }

  recipe::Options recipe_options;
  recipe_options.expected_revision = forward_inputs.revision;
  recipe_options.expected_source_object_pack = parsed_manifest.value().identity;
  const auto forward_wire = recipe::encode(forward_recipe.value(), recipe_options);
  const auto reverse_wire = recipe::encode(reverse_recipe.value(), recipe_options);
  if (!forward_wire || !reverse_wire || forward_wire.value() != reverse_wire.value()) {
    fail("the forward and reversed inputs did not produce identical canonical recipe output");
  }

  const auto forward_occurrences = collect_resolved_occurrences(graph, forward_recipe.value());
  const auto reverse_occurrences = collect_resolved_occurrences(graph, reverse_recipe.value());
  if (forward_occurrences.empty() || forward_occurrences != reverse_occurrences) {
    fail("the forward and reversed inputs did not produce identical retail resolution");
  }

  const auto aggregate = canonical_aggregate(forward_inputs.revision,
                                             parsed_manifest.value().identity, forward_catalog,
                                             forward_occurrences);
  if (cli.expected_aggregate && aggregate != *cli.expected_aggregate) {
    fail("the canonical aggregate does not match --expected-aggregate");
  }
  return {parsed_manifest.value().entries.size(), loaded.size(), forward_catalog.entries().size(),
          forward_catalog.skipped_code_object_count(), forward_occurrences.size(), aggregate};
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto result = run_proof(parse_cli(argc, argv));
    std::printf("source_rows=%zu archives=%zu catalog_entries=%zu skipped_code=%zu "
                "retail_occurrences=%zu aggregate_xxh64=%016llx\n",
                result.source_rows, result.archive_count, result.catalog_count,
                result.skipped_code_count, result.occurrence_count,
                static_cast<unsigned long long>(result.aggregate));
    return 0;
  } catch (const ProofFailure& error) {
    std::fprintf(stderr, "error: %s\n", error.what());
  } catch (const std::bad_alloc&) {
    std::fputs("error: allocation failed while running the real-catalog proof\n", stderr);
  } catch (...) {
    std::fputs("error: the real-catalog proof failed unexpectedly\n", stderr);
  }
  return 1;
}
