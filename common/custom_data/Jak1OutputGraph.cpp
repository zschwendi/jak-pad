#include "Jak1OutputGraph.h"

#include <algorithm>
#include <limits>
#include <new>
#include <string_view>
#include <unordered_set>

#define XXH_PRIVATE_API
#include "third-party/zstd/lib/common/xxhash.h"

namespace jak1_output_graph {
namespace {

constexpr std::size_t kHeaderBytes = kMagic.size() + sizeof(std::uint32_t) + sizeof(std::uint64_t);
constexpr std::size_t kHashBytes = sizeof(std::uint64_t);

Error make_error(ErrorCode code,
                 std::size_t offset,
                 std::string message,
                 std::optional<std::uint32_t> archive_index = {},
                 std::optional<std::uint32_t> object_index = {}) {
  return {code, offset, archive_index, object_index, std::move(message)};
}

std::optional<Error> check_cancelled(const Options& options,
                                     std::size_t offset = 0,
                                     std::optional<std::uint32_t> archive_index = {},
                                     std::optional<std::uint32_t> object_index = {}) {
  if (!options.should_cancel) {
    return {};
  }
  try {
    if (options.should_cancel()) {
      return make_error(ErrorCode::cancelled, offset, "Output-graph processing was cancelled.",
                        archive_index, object_index);
    }
  } catch (...) {
    return make_error(ErrorCode::callback_failed, offset,
                      "The output-graph cancellation callback failed.", archive_index,
                      object_index);
  }
  return {};
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_wire_bytes >= kHeaderBytes + kHashBytes && limits.max_source_files > 0 &&
         limits.max_archives > 0 && limits.max_objects_per_archive > 0 &&
         limits.max_total_objects > 0 && limits.max_flat_file_copies > 0 &&
         limits.max_generated_flat_files > 0 && limits.max_name_bytes > 0 &&
         limits.max_path_bytes > 0;
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
  if (value.empty() || value.size() > cap || value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t component_start = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    if (index != value.size() && value[index] != '/') {
      const auto byte = static_cast<unsigned char>(value[index]);
      if (byte < 0x21 || byte > 0x7e || value[index] == ':') {
        return false;
      }
      continue;
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
  std::string key(value);
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char byte) {
    return static_cast<char>(byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte);
  });
  return key;
}

bool archive_basename(std::string_view value, std::uint32_t cap) {
  return valid_name(value, cap) && (value.ends_with(".DGO") || value.ends_with(".CGO"));
}

bool known_producer(ObjectProducerKind producer) {
  return producer >= ObjectProducerKind::bundled_source &&
         producer <= ObjectProducerKind::custom_level;
}

bool known_generated_flat(jak1_output_recipe::GeneratedFlatFileKind kind) {
  return kind == jak1_output_recipe::GeneratedFlatFileKind::game_text ||
         kind == jak1_output_recipe::GeneratedFlatFileKind::game_subtitle;
}

template <typename Collection>
bool strictly_sorted_destinations(const Collection& values) {
  return std::adjacent_find(values.begin(), values.end(), [](const auto& left, const auto& right) {
           return left.destination_basename >= right.destination_basename;
         }) == values.end();
}

std::optional<Error> validate_graph(const Graph& graph, const Options& options) {
  const auto& limits = options.limits;
  if (graph.ordered_source_files.empty() ||
      graph.ordered_source_files.size() > limits.max_source_files || graph.archives.empty() ||
      graph.archives.size() > limits.max_archives ||
      graph.flat_file_copies.size() > limits.max_flat_file_copies ||
      graph.generated_flat_files.size() > limits.max_generated_flat_files ||
      !strictly_sorted_destinations(graph.archives) ||
      !strictly_sorted_destinations(graph.flat_file_copies) ||
      !strictly_sorted_destinations(graph.generated_flat_files)) {
    return make_error(ErrorCode::invalid_graph, 0,
                      "The output graph counts or canonical destination order are invalid.");
  }

  std::unordered_set<std::string> source_paths;
  for (const auto& source : graph.ordered_source_files) {
    if (const auto error = check_cancelled(options)) {
      return error;
    }
    if (!safe_relative_path(source, limits.max_path_bytes) ||
        !source_paths.emplace(collision_key(source)).second) {
      return make_error(ErrorCode::invalid_graph, 0,
                        "The output graph source-file order contains an unsafe duplicate.");
    }
  }

  std::unordered_set<std::string> destinations;
  std::size_t total_objects = 0;
  for (std::uint32_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
    const auto& archive = graph.archives[archive_index];
    if (!archive_basename(archive.destination_basename, limits.max_name_bytes) ||
        collision_key(archive.destination_basename) == "savegame.ico" ||
        !destinations.emplace(collision_key(archive.destination_basename)).second ||
        archive.objects.empty() || archive.objects.size() > limits.max_objects_per_archive ||
        archive.objects.size() > limits.max_total_objects - total_objects) {
      return make_error(ErrorCode::invalid_graph, 0,
                        "An output graph archive is unsafe, duplicate, empty, or too large.",
                        archive_index);
    }
    total_objects += archive.objects.size();
    for (std::uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
      if (const auto error = check_cancelled(options, 0, archive_index, object_index)) {
        return error;
      }
      const auto& object = archive.objects[object_index];
      if (!valid_name(object.prepared_basename, limits.max_name_bytes) ||
          !(object.prepared_basename.ends_with(".o") ||
            object.prepared_basename.ends_with(".go")) ||
          !valid_name(object.internal_name, limits.max_name_bytes) ||
          !known_producer(object.producer) ||
          (object.producer == ObjectProducerKind::verified_retail &&
           (!safe_relative_path(object.retail_source_archive, limits.max_path_bytes) ||
            !(object.retail_source_archive.ends_with(".DGO") ||
              object.retail_source_archive.ends_with(".CGO")))) ||
          (object.producer != ObjectProducerKind::verified_retail &&
           !object.retail_source_archive.empty())) {
        return make_error(ErrorCode::invalid_graph, 0,
                          "An output graph object has an invalid identity or producer.",
                          archive_index, object_index);
      }
    }
  }

  for (const auto& copy : graph.flat_file_copies) {
    if (const auto error = check_cancelled(options)) {
      return error;
    }
    if (!safe_relative_path(copy.source_path, limits.max_path_bytes) ||
        !valid_name(copy.destination_basename, limits.max_name_bytes) ||
        collision_key(copy.destination_basename) == "savegame.ico" ||
        !destinations.emplace(collision_key(copy.destination_basename)).second) {
      return make_error(ErrorCode::invalid_graph, 0,
                        "An output graph flat-file copy is unsafe or duplicate.");
    }
  }
  for (const auto& generated : graph.generated_flat_files) {
    if (!known_generated_flat(generated.kind) ||
        !valid_name(generated.destination_basename, limits.max_name_bytes) ||
        collision_key(generated.destination_basename) == "savegame.ico" ||
        !destinations.emplace(collision_key(generated.destination_basename)).second) {
      return make_error(ErrorCode::invalid_graph, 0,
                        "An output graph generated flat file is unsafe or duplicate.");
    }
  }
  return {};
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* result) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

void write_u32_at(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>(value >> shift);
  }
}

void write_u64_at(std::vector<std::uint8_t>* bytes, std::size_t offset, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    (*bytes)[offset++] = static_cast<std::uint8_t>(value >> shift);
  }
}

class Writer {
 public:
  Writer(std::vector<std::uint8_t>* bytes, std::size_t cap) : m_bytes(bytes), m_cap(cap) {}

  bool u8(std::uint8_t value) { return append(std::span<const std::uint8_t>(&value, 1)); }

  bool u32(std::uint32_t value) {
    std::array<std::uint8_t, 4> bytes{};
    for (unsigned shift = 0; shift < 32; shift += 8) {
      bytes[shift / 8] = static_cast<std::uint8_t>(value >> shift);
    }
    return append(bytes);
  }

  bool string(std::string_view value) {
    return value.size() <= std::numeric_limits<std::uint32_t>::max() &&
           u32(static_cast<std::uint32_t>(value.size())) &&
           append(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.data()),
                                                value.size()));
  }

 private:
  bool append(std::span<const std::uint8_t> value) {
    std::size_t next = 0;
    if (!checked_add(m_bytes->size(), value.size(), &next) || next > m_cap) {
      return false;
    }
    m_bytes->insert(m_bytes->end(), value.begin(), value.end());
    return true;
  }

  std::vector<std::uint8_t>* m_bytes;
  std::size_t m_cap;
};

class Reader {
 public:
  Reader(std::span<const std::uint8_t> bytes, const Options& options)
      : m_bytes(bytes), m_options(options) {}

  std::size_t offset() const { return m_offset; }
  bool done() const { return m_offset == m_bytes.size(); }

  bool u8(std::uint8_t* value) {
    if (m_offset == m_bytes.size()) {
      return false;
    }
    *value = m_bytes[m_offset++];
    return true;
  }

  bool u32(std::uint32_t* value) {
    if (m_bytes.size() - m_offset < sizeof(std::uint32_t)) {
      return false;
    }
    *value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      *value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
    }
    return true;
  }

  bool count(std::uint32_t cap, std::uint32_t* value) { return u32(value) && *value <= cap; }

  bool string(std::uint32_t cap, std::string* value) {
    std::uint32_t size = 0;
    if (!u32(&size) || size == 0 || size > cap || size > m_bytes.size() - m_offset) {
      return false;
    }
    value->assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), size);
    m_offset += size;
    return true;
  }

  std::optional<Error> cancelled(std::optional<std::uint32_t> archive_index = {},
                                 std::optional<std::uint32_t> object_index = {}) const {
    return check_cancelled(m_options, kHeaderBytes + m_offset, archive_index, object_index);
  }

 private:
  std::span<const std::uint8_t> m_bytes;
  const Options& m_options;
  std::size_t m_offset = 0;
};

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
  }
  return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value = 0;
  for (unsigned shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
  }
  return value;
}

}  // namespace

Result<std::vector<std::uint8_t>> encode(const Graph& graph, const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<std::vector<std::uint8_t>>::failure(make_error(
          ErrorCode::invalid_argument, 0, "The output-graph encoder options are invalid."));
    }
    if (const auto error = validate_graph(graph, options)) {
      return Result<std::vector<std::uint8_t>>::failure(*error);
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(std::min<std::size_t>(options.limits.max_wire_bytes, 256 * 1024));
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    bytes.resize(kHeaderBytes);
    write_u32_at(&bytes, kMagic.size(), kSchemaVersion);
    Writer writer(&bytes, options.limits.max_wire_bytes - kHashBytes);
    if (!writer.u32(static_cast<std::uint32_t>(graph.ordered_source_files.size()))) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its cap."));
    }
    for (const auto& source : graph.ordered_source_files) {
      if (const auto error = check_cancelled(options, bytes.size())) {
        return Result<std::vector<std::uint8_t>>::failure(*error);
      }
      if (!writer.string(source)) {
        return Result<std::vector<std::uint8_t>>::failure(make_error(
            ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its wire cap."));
      }
    }
    if (!writer.u32(static_cast<std::uint32_t>(graph.archives.size()))) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its cap."));
    }
    for (std::uint32_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
      const auto& archive = graph.archives[archive_index];
      if (!writer.string(archive.destination_basename) ||
          !writer.u32(static_cast<std::uint32_t>(archive.objects.size()))) {
        return Result<std::vector<std::uint8_t>>::failure(
            make_error(ErrorCode::limit_exceeded, bytes.size(),
                       "The output graph exceeds its wire cap.", archive_index));
      }
      for (std::uint32_t object_index = 0; object_index < archive.objects.size(); ++object_index) {
        if (const auto error =
                check_cancelled(options, bytes.size(), archive_index, object_index)) {
          return Result<std::vector<std::uint8_t>>::failure(*error);
        }
        const auto& object = archive.objects[object_index];
        if (!writer.string(object.prepared_basename) || !writer.string(object.internal_name) ||
            !writer.u8(static_cast<std::uint8_t>(object.producer)) ||
            !writer.string(object.producer == ObjectProducerKind::verified_retail
                               ? std::string_view(object.retail_source_archive)
                               : std::string_view("-"))) {
          return Result<std::vector<std::uint8_t>>::failure(
              make_error(ErrorCode::limit_exceeded, bytes.size(),
                         "The output graph exceeds its wire cap.", archive_index, object_index));
        }
      }
    }
    if (!writer.u32(static_cast<std::uint32_t>(graph.flat_file_copies.size()))) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its cap."));
    }
    for (const auto& copy : graph.flat_file_copies) {
      if (!writer.string(copy.source_path) || !writer.string(copy.destination_basename)) {
        return Result<std::vector<std::uint8_t>>::failure(make_error(
            ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its wire cap."));
      }
    }
    if (!writer.u32(static_cast<std::uint32_t>(graph.generated_flat_files.size()))) {
      return Result<std::vector<std::uint8_t>>::failure(
          make_error(ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its cap."));
    }
    for (const auto& generated : graph.generated_flat_files) {
      if (!writer.u8(static_cast<std::uint8_t>(generated.kind)) ||
          !writer.string(generated.destination_basename)) {
        return Result<std::vector<std::uint8_t>>::failure(make_error(
            ErrorCode::limit_exceeded, bytes.size(), "The output graph exceeds its wire cap."));
      }
    }

    write_u64_at(&bytes, kMagic.size() + sizeof(std::uint32_t), bytes.size() - kHeaderBytes);
    const auto hash = XXH64(bytes.data(), bytes.size(), 0);
    const auto hash_offset = bytes.size();
    bytes.resize(hash_offset + kHashBytes);
    write_u64_at(&bytes, hash_offset, hash);
    return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
  } catch (const std::bad_alloc&) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Output-graph encoding ran out of memory."));
  }
}

Result<Graph> decode(std::span<const std::uint8_t> bytes, const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<Graph>::failure(make_error(ErrorCode::invalid_argument, 0,
                                               "The output-graph decoder options are invalid."));
    }
    if (bytes.size() < kHeaderBytes + kHashBytes) {
      return Result<Graph>::failure(make_error(ErrorCode::truncated, bytes.size(),
                                               "The output-graph wire data is truncated."));
    }
    if (bytes.size() > options.limits.max_wire_bytes) {
      return Result<Graph>::failure(
          make_error(ErrorCode::limit_exceeded, 0, "The output graph exceeds its wire cap."));
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
      return Result<Graph>::failure(
          make_error(ErrorCode::wrong_magic, 0, "The output graph has the wrong magic."));
    }
    if (read_u32(bytes, kMagic.size()) != kSchemaVersion) {
      return Result<Graph>::failure(make_error(ErrorCode::unsupported_schema, kMagic.size(),
                                               "The output graph schema is unsupported."));
    }
    const auto payload_size = read_u64(bytes, kMagic.size() + sizeof(std::uint32_t));
    if (payload_size != bytes.size() - kHeaderBytes - kHashBytes) {
      return Result<Graph>::failure(make_error(
          payload_size > bytes.size() - kHeaderBytes - kHashBytes ? ErrorCode::truncated
                                                                  : ErrorCode::trailing_data,
          kHeaderBytes, "The output graph payload extent does not match its wire framing."));
    }
    const auto expected_hash = read_u64(bytes, bytes.size() - kHashBytes);
    if (XXH64(bytes.data(), bytes.size() - kHashBytes, 0) != expected_hash) {
      return Result<Graph>::failure(make_error(ErrorCode::corrupt_hash, bytes.size() - kHashBytes,
                                               "The output graph hash is invalid."));
    }

    Reader reader(bytes.subspan(kHeaderBytes, static_cast<std::size_t>(payload_size)), options);
    Graph graph;
    std::uint32_t count = 0;
    if (!reader.count(options.limits.max_source_files, &count) || count == 0) {
      return Result<Graph>::failure(make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                                               "The source-file count is invalid."));
    }
    graph.ordered_source_files.resize(count);
    for (auto& source : graph.ordered_source_files) {
      if (const auto error = reader.cancelled()) {
        return Result<Graph>::failure(*error);
      }
      if (!reader.string(options.limits.max_path_bytes, &source)) {
        return Result<Graph>::failure(make_error(ErrorCode::truncated,
                                                 kHeaderBytes + reader.offset(),
                                                 "A source-file path is truncated or too large."));
      }
    }

    if (!reader.count(options.limits.max_archives, &count) || count == 0) {
      return Result<Graph>::failure(make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                                               "The archive count is invalid."));
    }
    graph.archives.resize(count);
    std::size_t total_objects = 0;
    for (std::uint32_t archive_index = 0; archive_index < graph.archives.size(); ++archive_index) {
      auto& archive = graph.archives[archive_index];
      std::uint32_t object_count = 0;
      if (!reader.string(options.limits.max_name_bytes, &archive.destination_basename) ||
          !reader.count(options.limits.max_objects_per_archive, &object_count) ||
          object_count == 0 || object_count > options.limits.max_total_objects - total_objects) {
        return Result<Graph>::failure(
            make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                       "An archive record is truncated or exceeds its object cap.", archive_index));
      }
      total_objects += object_count;
      archive.objects.resize(object_count);
      for (std::uint32_t object_index = 0; object_index < object_count; ++object_index) {
        if (const auto error = reader.cancelled(archive_index, object_index)) {
          return Result<Graph>::failure(*error);
        }
        auto& object = archive.objects[object_index];
        std::uint8_t producer = 0;
        if (!reader.string(options.limits.max_name_bytes, &object.prepared_basename) ||
            !reader.string(options.limits.max_name_bytes, &object.internal_name) ||
            !reader.u8(&producer)) {
          return Result<Graph>::failure(make_error(
              ErrorCode::truncated, kHeaderBytes + reader.offset(),
              "An archive object is truncated or too large.", archive_index, object_index));
        }
        object.producer = static_cast<ObjectProducerKind>(producer);
        std::string retail_source;
        if (!reader.string(options.limits.max_path_bytes, &retail_source)) {
          return Result<Graph>::failure(
              make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                         "An archive object's retail provenance is truncated or too large.",
                         archive_index, object_index));
        }
        if (object.producer == ObjectProducerKind::verified_retail) {
          object.retail_source_archive = std::move(retail_source);
        } else if (retail_source != "-") {
          return Result<Graph>::failure(
              make_error(ErrorCode::invalid_graph, kHeaderBytes + reader.offset(),
                         "A non-retail graph object contains retail provenance.", archive_index,
                         object_index));
        }
      }
    }

    if (!reader.count(options.limits.max_flat_file_copies, &count)) {
      return Result<Graph>::failure(make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                                               "The flat-copy count is invalid."));
    }
    graph.flat_file_copies.resize(count);
    for (auto& copy : graph.flat_file_copies) {
      if (!reader.string(options.limits.max_path_bytes, &copy.source_path) ||
          !reader.string(options.limits.max_name_bytes, &copy.destination_basename)) {
        return Result<Graph>::failure(make_error(
            ErrorCode::truncated, kHeaderBytes + reader.offset(), "A flat copy is truncated."));
      }
    }
    if (!reader.count(options.limits.max_generated_flat_files, &count)) {
      return Result<Graph>::failure(make_error(ErrorCode::truncated, kHeaderBytes + reader.offset(),
                                               "The generated-file count is invalid."));
    }
    graph.generated_flat_files.resize(count);
    for (auto& generated : graph.generated_flat_files) {
      std::uint8_t kind = 0;
      if (!reader.u8(&kind) ||
          !reader.string(options.limits.max_name_bytes, &generated.destination_basename)) {
        return Result<Graph>::failure(make_error(ErrorCode::truncated,
                                                 kHeaderBytes + reader.offset(),
                                                 "A generated file is truncated."));
      }
      generated.kind = static_cast<jak1_output_recipe::GeneratedFlatFileKind>(kind);
    }
    if (!reader.done()) {
      return Result<Graph>::failure(make_error(ErrorCode::trailing_data,
                                               kHeaderBytes + reader.offset(),
                                               "The output graph has trailing data."));
    }
    if (const auto error = validate_graph(graph, options)) {
      return Result<Graph>::failure(*error);
    }
    return Result<Graph>::success(std::move(graph));
  } catch (const std::bad_alloc&) {
    return Result<Graph>::failure(
        make_error(ErrorCode::allocation_failed, 0, "Output-graph decoding ran out of memory."));
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
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::invalid_graph:
      return "invalid_graph";
    case ErrorCode::wrong_magic:
      return "wrong_magic";
    case ErrorCode::unsupported_schema:
      return "unsupported_schema";
    case ErrorCode::truncated:
      return "truncated";
    case ErrorCode::corrupt_hash:
      return "corrupt_hash";
    case ErrorCode::trailing_data:
      return "trailing_data";
  }
  return "unknown";
}

}  // namespace jak1_output_graph
