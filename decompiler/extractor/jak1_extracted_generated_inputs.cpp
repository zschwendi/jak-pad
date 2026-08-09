#include "jak1_extracted_generated_inputs.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <span>
#include <string_view>

namespace jak1_extracted_generated_inputs {
namespace {

constexpr std::string_view kGameArchivePath = "CGO/GAME.CGO";
constexpr std::string_view kGameArchiveName = "GAME.CGO";
constexpr std::uint32_t kRetailLanguageCount = 7;
constexpr std::size_t kProgressUnits = 11;
constexpr std::size_t kV2HeaderBytes = 12;
constexpr std::size_t kV4HeaderBytes = 16;
constexpr std::size_t kObjectAlignment = 16;
constexpr std::size_t kLinkAlignment = 64;

Error make_error(ErrorCode code,
                 std::string message,
                 std::string source_path = {},
                 std::optional<std::uint32_t> object_index = {},
                 std::optional<std::uint32_t> language_id = {},
                 std::optional<std::size_t> byte_offset = {}) {
  return {code,        std::move(message), std::move(source_path), object_index, language_id,
          byte_offset, std::nullopt};
}

enum class CallbackState {
  continue_work,
  cancelled,
  failed,
};

CallbackState poll_cancel(const Options& options) {
  if (!options.should_cancel) {
    return CallbackState::continue_work;
  }
  try {
    return options.should_cancel() ? CallbackState::cancelled : CallbackState::continue_work;
  } catch (...) {
    return CallbackState::failed;
  }
}

std::optional<Error> cancellation_error(const Options& options,
                                        const std::string& source_path = {},
                                        std::optional<std::uint32_t> language_id = {}) {
  const auto state = poll_cancel(options);
  if (state == CallbackState::continue_work) {
    return {};
  }
  return make_error(
      state == CallbackState::cancelled ? ErrorCode::cancelled : ErrorCode::callback_failed,
      state == CallbackState::cancelled ? "Generated-input loading was cancelled."
                                        : "The cancellation callback failed.",
      source_path, {}, language_id);
}

std::optional<Error> emit_progress(const Options& options, Progress progress) {
  if (!options.on_progress) {
    return {};
  }
  try {
    options.on_progress(progress);
    return {};
  } catch (...) {
    return make_error(ErrorCode::callback_failed, "The progress callback failed.",
                      progress.source_relative_path, {}, progress.language_id);
  }
}

bool same_revision(const jak1_iso::Revision& left, const jak1_iso::Revision& right) {
  return left.serial == right.serial && left.elf_hash == right.elf_hash &&
         left.canonical_name == right.canonical_name && left.territory == right.territory &&
         left.file_count == right.file_count && left.contents_hash == right.contents_hash &&
         left.decomp_config_version == right.decomp_config_version &&
         left.black_label == right.black_label;
}

bool supported_revision(const jak1_iso::Revision& revision) {
  return same_revision(revision, jak1_iso::default_revision());
}

bool valid_options(const Options& options) {
  const auto& limits = options.limits;
  return limits.max_game_archive_input_bytes > 0 && limits.max_game_archive_compressed_bytes > 0 &&
         limits.max_game_archive_expanded_bytes > 0 && limits.max_archive_object_bytes > 0 &&
         limits.max_archive_total_object_bytes >= limits.max_archive_object_bytes &&
         limits.max_direct_object_bytes > 0 && limits.file_read_chunk_bytes > 0 &&
         limits.file_read_chunk_bytes <=
             static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()) &&
         limits.max_archive_objects > 0 && limits.max_archive_expansion_ratio > 0 &&
         limits.max_pointer_links > 0 && limits.max_named_links > 0 &&
         limits.max_tpage_entries > 0 && limits.max_game_count_entries > 0 &&
         limits.max_text_lines_per_bank > 0 && limits.max_subtitle_scenes_per_bank > 0 &&
         limits.max_subtitle_lines_per_scene > 0 && limits.max_name_bytes > 0 &&
         limits.max_string_bytes > 0 && limits.max_generated_bank_string_bytes > 0 &&
         (options.subtitle_mode == artifacts::SubtitleMode::public_content ||
          options.subtitle_mode == artifacts::SubtitleMode::empty);
}

Result<std::filesystem::path> checked_input_path(const ValidatedTree& tree,
                                                 std::string_view relative_path) {
  const auto fail = [&](ErrorCode code, std::string message) {
    return Result<std::filesystem::path>::failure(
        make_error(code, std::move(message), std::string(relative_path)));
  };
  if (tree.root.empty() || !tree.root.is_absolute() || tree.root != tree.root.lexically_normal()) {
    return fail(ErrorCode::invalid_extracted_tree,
                "The validated extraction root must be an absolute normalized path.");
  }

  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(tree.root, error);
  if (error || root_status.type() != std::filesystem::file_type::directory) {
    return fail(ErrorCode::invalid_extracted_tree,
                "The validated extraction root is not a plain directory.");
  }

  auto current = tree.root;
  const std::filesystem::path relative(relative_path);
  for (auto component = relative.begin(); component != relative.end(); ++component) {
    current /= *component;
    const auto status = std::filesystem::symlink_status(current, error);
    if (error || status.type() == std::filesystem::file_type::not_found) {
      return fail(ErrorCode::missing_input, "A required extracted-disc input is missing.");
    }
    const bool final = std::next(component) == relative.end();
    if ((!final && status.type() != std::filesystem::file_type::directory) ||
        (final && status.type() != std::filesystem::file_type::regular)) {
      return fail(ErrorCode::invalid_extracted_tree,
                  "A required extracted-disc path is not a plain file hierarchy.");
    }
  }
  return Result<std::filesystem::path>::success(std::move(current));
}

Result<std::vector<std::uint8_t>> read_direct_object(const ValidatedTree& tree,
                                                     const std::string& relative_path,
                                                     const Options& options,
                                                     std::uint32_t language_id) {
  auto checked_path = checked_input_path(tree, relative_path);
  if (!checked_path) {
    auto error = checked_path.error();
    error.language_id = language_id;
    return Result<std::vector<std::uint8_t>>::failure(std::move(error));
  }

  std::ifstream input(checked_path.value(), std::ios::binary | std::ios::ate);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_open_failed, "Could not open a retail game-text object.",
                   relative_path, {}, language_id));
  }
  const auto end = input.tellg();
  if (end < 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_read_failed, "Could not determine a game-text object size.",
                   relative_path, {}, language_id));
  }
  const auto input_size = static_cast<std::uintmax_t>(end);
  if (input_size == 0 || input_size > options.limits.max_direct_object_bytes ||
      input_size > std::numeric_limits<std::size_t>::max()) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_too_large,
                   "A retail game-text object is empty or exceeds the configured limit.",
                   relative_path, {}, language_id));
  }
  if (const auto error = cancellation_error(options, relative_path, language_id)) {
    return Result<std::vector<std::uint8_t>>::failure(*error);
  }

  input.seekg(0);
  if (!input) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::input_read_failed, "Could not seek to a game-text object start.",
                   relative_path, {}, language_id));
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(input_size));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (const auto error = cancellation_error(options, relative_path, language_id)) {
      return Result<std::vector<std::uint8_t>>::failure(*error);
    }
    const auto chunk = std::min(options.limits.file_read_chunk_bytes, bytes.size() - offset);
    input.read(reinterpret_cast<char*>(bytes.data() + offset), static_cast<std::streamsize>(chunk));
    if (input.gcount() != static_cast<std::streamsize>(chunk)) {
      return Result<std::vector<std::uint8_t>>::failure(make_error(
          ErrorCode::input_read_failed, "Could not read a complete retail game-text object.",
          relative_path, {}, language_id, offset));
    }
    offset += chunk;
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
         (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}

bool checked_add(std::size_t left, std::size_t right, std::size_t* output) {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *output = left + right;
  return true;
}

bool add_with_limit(std::size_t* total, std::size_t amount, std::size_t limit) {
  if (*total > limit || amount > limit - *total) {
    return false;
  }
  *total += amount;
  return true;
}

enum class NamedLinkKind {
  symbol,
  type,
};

struct NamedLink {
  NamedLinkKind kind = NamedLinkKind::symbol;
  std::string name;
};

struct DecodedObject {
  std::span<const std::uint8_t> code;
  std::map<std::size_t, std::size_t> pointers;
  std::map<std::size_t, NamedLink> named_links;
};

struct ObjectContext {
  std::string source_path;
  std::optional<std::uint32_t> object_index;
  std::optional<std::uint32_t> language_id;
};

Error object_error(ErrorCode code,
                   std::string message,
                   const ObjectContext& context,
                   std::optional<std::size_t> byte_offset = {}) {
  return make_error(code, std::move(message), context.source_path, context.object_index,
                    context.language_id, byte_offset);
}

std::optional<Error> object_cancellation_error(const Options& options,
                                               const ObjectContext& context,
                                               std::size_t byte_offset) {
  const auto state = poll_cancel(options);
  if (state == CallbackState::continue_work) {
    return {};
  }
  return object_error(
      state == CallbackState::cancelled ? ErrorCode::cancelled : ErrorCode::callback_failed,
      state == CallbackState::cancelled ? "Generated-input loading was cancelled."
                                        : "The cancellation callback failed.",
      context, byte_offset);
}

Result<std::size_t> read_run_integer(std::span<const std::uint8_t> data,
                                     std::size_t* cursor,
                                     std::size_t end,
                                     const ObjectContext& context,
                                     const Options& options) {
  std::size_t result = 0;
  std::size_t bytes_read = 0;
  while (true) {
    if ((bytes_read & 0x3ffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, *cursor)) {
        return Result<std::size_t>::failure(*error);
      }
    }
    if (*cursor >= end) {
      return Result<std::size_t>::failure(object_error(
          ErrorCode::invalid_link_data, "A V2 link-table run is truncated.", context, *cursor));
    }
    const auto byte = data[(*cursor)++];
    ++bytes_read;
    if (byte > std::numeric_limits<std::size_t>::max() - result) {
      return Result<std::size_t>::failure(object_error(
          ErrorCode::invalid_link_data, "A V2 link-table run overflows.", context, *cursor - 1));
    }
    result += byte;
    if (byte != std::numeric_limits<std::uint8_t>::max()) {
      return Result<std::size_t>::success(result);
    }
  }
}

Result<std::size_t> read_named_delta(std::span<const std::uint8_t> data,
                                     std::size_t* cursor,
                                     std::size_t end,
                                     const ObjectContext& context,
                                     const Options& options) {
  if (const auto error = object_cancellation_error(options, context, *cursor)) {
    return Result<std::size_t>::failure(*error);
  }
  if (*cursor >= end) {
    return Result<std::size_t>::failure(object_error(
        ErrorCode::invalid_link_data, "A named-link relocation is truncated.", context, *cursor));
  }
  const auto first = data[(*cursor)++];
  const auto encoded_bytes = static_cast<std::size_t>(first & 3u) + 1;
  if (encoded_bytes > end - (*cursor - 1)) {
    return Result<std::size_t>::failure(object_error(ErrorCode::invalid_link_data,
                                                     "A named-link relocation is truncated.",
                                                     context, *cursor - 1));
  }
  std::uint32_t value = first;
  for (std::size_t index = 1; index < encoded_bytes; ++index) {
    value |= std::uint32_t(data[(*cursor)++]) << (index * 8);
  }
  return Result<std::size_t>::success(static_cast<std::size_t>(value & 0xfffffffcu));
}

Result<DecodedObject> decode_data_object(std::span<const std::uint8_t> data,
                                         const ObjectContext& context,
                                         const Options& options) {
  if (const auto error = object_cancellation_error(options, context, 0)) {
    return Result<DecodedObject>::failure(*error);
  }
  if (data.size() < kV2HeaderBytes || read_u32(data, 0) != 0xffffffffu) {
    return Result<DecodedObject>::failure(object_error(
        ErrorCode::invalid_data_object, "The GOAL data-object header is invalid.", context, 0));
  }

  const auto version = read_u32(data, 8);
  const auto link_length = static_cast<std::size_t>(read_u32(data, 4));
  std::size_t code_offset = 0;
  std::size_t code_size = 0;
  std::size_t link_header_offset = 0;
  std::size_t object_end = 0;
  if (version == 2) {
    if (link_length < kV2HeaderBytes || link_length >= data.size() ||
        link_length % kLinkAlignment != 0 || (data.size() - link_length) % 4 != 0) {
      return Result<DecodedObject>::failure(
          object_error(ErrorCode::invalid_data_object,
                       "The Jak 1 V2 data-object extent is invalid.", context, 4));
    }
    code_offset = link_length;
    code_size = data.size() - code_offset;
    link_header_offset = 0;
  } else if (version == 4) {
    if (data.size() < kV4HeaderBytes || link_length < kV2HeaderBytes ||
        link_length % kLinkAlignment != 0) {
      return Result<DecodedObject>::failure(
          object_error(ErrorCode::invalid_data_object,
                       "The Jak 1 V4 data-object header is invalid.", context, 4));
    }
    code_size = static_cast<std::size_t>(read_u32(data, 12));
    if (code_size == 0 || code_size % kObjectAlignment != 0 ||
        !checked_add(kV4HeaderBytes, code_size, &link_header_offset) ||
        !checked_add(link_header_offset, link_length, &object_end) || object_end != data.size() ||
        data.size() - link_header_offset < kV2HeaderBytes ||
        read_u32(data, link_header_offset) != 0xffffffffu ||
        read_u32(data, link_header_offset + 4) != link_length ||
        read_u32(data, link_header_offset + 8) != 2) {
      return Result<DecodedObject>::failure(object_error(
          ErrorCode::invalid_data_object,
          "The Jak 1 V4 object lacks a matching bounded trailing V2 link header.", context, 12));
    }
    code_offset = kV4HeaderBytes;
  } else {
    return Result<DecodedObject>::failure(
        object_error(ErrorCode::unsupported_data_object_version,
                     "The retail object is not a Jak 1 V2 or V4 data object.", context, 8));
  }

  if (code_size > options.limits.max_archive_object_bytes || code_size % 4 != 0) {
    return Result<DecodedObject>::failure(
        object_error(ErrorCode::limit_exceeded,
                     "The GOAL data section exceeds the configured bound.", context, code_offset));
  }

  const auto link_end = link_header_offset + link_length;
  std::size_t cursor = link_header_offset + kV2HeaderBytes;
  std::size_t word_cursor = 0;
  DecodedObject decoded;
  decoded.code = data.subspan(code_offset, code_size);

  while (true) {
    auto seek = read_run_integer(data, &cursor, link_end, context, options);
    if (!seek) {
      return Result<DecodedObject>::failure(seek.error());
    }
    if (seek.value() == 0) {
      break;
    }
    if (seek.value() > decoded.code.size() / 4 - std::min(word_cursor, decoded.code.size() / 4)) {
      return Result<DecodedObject>::failure(
          object_error(ErrorCode::invalid_link_data,
                       "A pointer-link seek leaves the GOAL data section.", context, cursor));
    }
    word_cursor += seek.value();
    auto count = read_run_integer(data, &cursor, link_end, context, options);
    if (!count || count.value() == 0 || count.value() > decoded.code.size() / 4 - word_cursor) {
      return Result<DecodedObject>::failure(
          count ? object_error(ErrorCode::invalid_link_data,
                               "A pointer-link run is empty or leaves the GOAL data section.",
                               context, cursor)
                : count.error());
    }
    for (std::size_t index = 0; index < count.value(); ++index) {
      if ((index & 0x3ffu) == 0) {
        if (const auto error = object_cancellation_error(options, context, cursor)) {
          return Result<DecodedObject>::failure(*error);
        }
      }
      const auto source_word = word_cursor + index;
      const auto target = static_cast<std::size_t>(read_u32(decoded.code, source_word * 4));
      if (decoded.pointers.size() >= options.limits.max_pointer_links) {
        return Result<DecodedObject>::failure(object_error(
            ErrorCode::limit_exceeded, "The GOAL pointer-link count exceeds its bound.", context,
            source_word * 4));
      }
      if (target >= decoded.code.size() || !decoded.pointers.emplace(source_word, target).second) {
        return Result<DecodedObject>::failure(
            object_error(ErrorCode::invalid_pointer,
                         "A GOAL pointer target is out of bounds or its source is duplicated.",
                         context, source_word * 4));
      }
    }
    word_cursor += count.value();
  }

  if (cursor >= link_end) {
    return Result<DecodedObject>::failure(object_error(
        ErrorCode::invalid_link_data, "The named-link table is missing.", context, cursor));
  }
  while (data[cursor] != 0) {
    if (const auto error = object_cancellation_error(options, context, cursor)) {
      return Result<DecodedObject>::failure(*error);
    }
    const auto marker_offset = cursor;
    const auto marker = data[cursor++];
    NamedLinkKind kind = NamedLinkKind::symbol;
    if (marker & 0x80u) {
      if ((marker & 0x7fu) != 0) {
        return Result<DecodedObject>::failure(object_error(
            ErrorCode::invalid_link_data, "A Jak 1 type link has an unsupported method count.",
            context, marker_offset));
      }
      kind = NamedLinkKind::type;
    } else {
      --cursor;
    }

    const auto name_start = cursor;
    while (cursor < link_end && data[cursor] != 0) {
      if (((cursor - name_start) & 0x3ffu) == 0) {
        if (const auto error = object_cancellation_error(options, context, cursor)) {
          return Result<DecodedObject>::failure(*error);
        }
      }
      const auto byte = data[cursor];
      if (byte < 0x20 || byte > 0x7e || cursor - name_start >= options.limits.max_name_bytes) {
        return Result<DecodedObject>::failure(object_error(ErrorCode::invalid_link_data,
                                                           "A named GOAL link has an invalid name.",
                                                           context, cursor));
      }
      ++cursor;
    }
    if (cursor == name_start || cursor >= link_end) {
      return Result<DecodedObject>::failure(
          object_error(ErrorCode::invalid_link_data,
                       "A named GOAL link has a missing name terminator.", context, name_start));
    }
    std::string name(reinterpret_cast<const char*>(data.data() + name_start), cursor - name_start);
    ++cursor;

    std::size_t location = 0;
    bool first_location = true;
    while (true) {
      auto delta = read_named_delta(data, &cursor, link_end, context, options);
      if (!delta) {
        return Result<DecodedObject>::failure(delta.error());
      }
      if (!first_location && delta.value() == 0) {
        return Result<DecodedObject>::failure(
            object_error(ErrorCode::invalid_link_data,
                         "A named GOAL link repeats a source location.", context, cursor));
      }
      first_location = false;
      if (delta.value() > decoded.code.size() - std::min(location, decoded.code.size())) {
        return Result<DecodedObject>::failure(
            object_error(ErrorCode::invalid_link_data, "A named GOAL link leaves the data section.",
                         context, cursor));
      }
      location += delta.value();
      if (decoded.named_links.size() >= options.limits.max_named_links) {
        return Result<DecodedObject>::failure(
            object_error(ErrorCode::limit_exceeded, "The named GOAL link count exceeds its bound.",
                         context, location));
      }
      if (location % 4 != 0 || location >= decoded.code.size() ||
          read_u32(decoded.code, location) != 0xffffffffu ||
          decoded.pointers.contains(location / 4) ||
          !decoded.named_links.emplace(location / 4, NamedLink{kind, name}).second) {
        return Result<DecodedObject>::failure(object_error(
            ErrorCode::invalid_link_data,
            "A named GOAL link has an invalid or duplicated source word.", context, location));
      }
      if (cursor >= link_end) {
        return Result<DecodedObject>::failure(object_error(
            ErrorCode::invalid_link_data, "A named GOAL link is unterminated.", context, cursor));
      }
      if (data[cursor] == 0) {
        ++cursor;
        break;
      }
    }
  }
  ++cursor;
  for (std::size_t offset = cursor; offset < link_end; ++offset) {
    if (((offset - cursor) & 0xfffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, offset)) {
        return Result<DecodedObject>::failure(*error);
      }
    }
    if (data[offset] != 0) {
      return Result<DecodedObject>::failure(
          object_error(ErrorCode::invalid_link_data, "The V2 link table has nonzero trailing data.",
                       context, offset));
    }
  }
  return Result<DecodedObject>::success(std::move(decoded));
}

bool use_named_link(const DecodedObject& object,
                    std::size_t word,
                    NamedLinkKind kind,
                    std::string_view name,
                    std::set<std::size_t>* used) {
  const auto found = object.named_links.find(word);
  if (found == object.named_links.end() || found->second.kind != kind ||
      found->second.name != name) {
    return false;
  }
  used->insert(word);
  return true;
}

bool mark_words(std::vector<bool>* used, std::size_t begin, std::size_t count) {
  if (begin > used->size() || count > used->size() - begin) {
    return false;
  }
  for (std::size_t index = begin; index < begin + count; ++index) {
    (*used)[index] = true;
  }
  return true;
}

std::optional<Error> validate_unmarked_words(const DecodedObject& object,
                                             const std::vector<bool>& used,
                                             const ObjectContext& context,
                                             const Options& options) {
  for (std::size_t word = 0; word < used.size(); ++word) {
    if ((word & 0xfffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, word * 4)) {
        return error;
      }
    }
    if (!used[word] && read_u32(object.code, word * 4) != 0) {
      return object_error(ErrorCode::invalid_data_object,
                          "The GOAL data object has unexpected nonzero padding.", context,
                          word * 4);
    }
  }
  return {};
}

std::optional<Error> validate_all_words_used(const std::vector<bool>& used,
                                             const ObjectContext& context,
                                             const Options& options) {
  for (std::size_t word = 0; word < used.size(); ++word) {
    if ((word & 0xfffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, word * 4)) {
        return error;
      }
    }
    if (!used[word]) {
      return object_error(ErrorCode::invalid_data_object,
                          "The GOAL data object contains a noncanonical gap.", context, word * 4);
    }
  }
  return {};
}

Result<std::string> read_goal_string(const DecodedObject& object,
                                     std::size_t pointer_word,
                                     std::vector<bool>* used_words,
                                     std::set<std::size_t>* used_pointers,
                                     std::set<std::size_t>* used_named,
                                     std::map<std::size_t, std::string>* strings,
                                     const ObjectContext& context,
                                     const Options& options) {
  const auto pointer = object.pointers.find(pointer_word);
  if (pointer == object.pointers.end() || pointer->second % 4 != 0 || pointer->second < 4) {
    return Result<std::string>::failure(
        object_error(ErrorCode::invalid_pointer, "A GOAL string pointer is missing or unaligned.",
                     context, pointer_word * 4));
  }
  used_pointers->insert(pointer_word);
  const auto target_word = pointer->second / 4;
  if ((target_word - 1) % 4 != 0) {
    return Result<std::string>::failure(object_error(ErrorCode::invalid_pointer,
                                                     "A GOAL string object is not 16-byte aligned.",
                                                     context, pointer->second));
  }
  if (!use_named_link(object, target_word - 1, NamedLinkKind::type, "string", used_named)) {
    return Result<std::string>::failure(object_error(
        ErrorCode::unexpected_type, "A GOAL string pointer does not target a string object.",
        context, pointer->second));
  }
  const auto existing = strings->find(target_word);
  if (existing != strings->end()) {
    return Result<std::string>::success(existing->second);
  }

  const auto length = static_cast<std::size_t>(read_u32(object.code, pointer->second));
  if (length > options.limits.max_string_bytes || pointer->second + 4 > object.code.size() ||
      length >= object.code.size() - (pointer->second + 4)) {
    return Result<std::string>::failure(object_error(
        ErrorCode::limit_exceeded, "A GOAL string exceeds its object or configured bound.", context,
        pointer->second));
  }
  const auto string_start = pointer->second + 4;
  if (object.code[string_start + length] != 0) {
    return Result<std::string>::failure(object_error(
        ErrorCode::invalid_value, "A GOAL string lacks a terminator at its declared length.",
        context, string_start + length));
  }
  std::size_t unaligned_object_bytes = 0;
  if (!checked_add(length, 9, &unaligned_object_bytes) ||
      unaligned_object_bytes > std::numeric_limits<std::size_t>::max() - 15) {
    return Result<std::string>::failure(object_error(ErrorCode::limit_exceeded,
                                                     "A GOAL string object extent overflows.",
                                                     context, pointer->second));
  }
  const auto string_object_words = ((unaligned_object_bytes + 15) / 16) * 4;
  const auto object_begin = target_word - 1;
  if (object_begin > used_words->size() ||
      string_object_words > used_words->size() - object_begin) {
    return Result<std::string>::failure(
        object_error(ErrorCode::invalid_pointer, "A GOAL string object leaves the data section.",
                     context, pointer->second));
  }
  for (std::size_t word = object_begin; word < object_begin + string_object_words; ++word) {
    if (((word - object_begin) & 0x3ffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, word * 4)) {
        return Result<std::string>::failure(*error);
      }
    }
    if ((*used_words)[word]) {
      return Result<std::string>::failure(
          object_error(ErrorCode::invalid_pointer,
                       "A GOAL string object overlaps another data object.", context, word * 4));
    }
  }
  const auto object_end = (object_begin + string_object_words) * 4;
  for (std::size_t offset = string_start + length + 1; offset < object_end; ++offset) {
    if (((offset - string_start) & 0xfffu) == 0) {
      if (const auto error = object_cancellation_error(options, context, offset)) {
        return Result<std::string>::failure(*error);
      }
    }
    if (object.code[offset] != 0) {
      return Result<std::string>::failure(
          object_error(ErrorCode::invalid_data_object,
                       "A GOAL string has nonzero alignment padding.", context, offset));
    }
  }
  mark_words(used_words, object_begin, string_object_words);
  std::string value(reinterpret_cast<const char*>(object.code.data() + string_start), length);
  strings->emplace(target_word, value);
  return Result<std::string>::success(std::move(value));
}

Result<artifacts::DirectoryTpages> parse_directory_tpages(
    std::span<const std::uint8_t> data,
    const ObjectContext& context,
    const Options& options,
    bool allow_nonzero_canonical_tail = false) {
  auto decoded = decode_data_object(data, context, options);
  if (!decoded) {
    return Result<artifacts::DirectoryTpages>::failure(decoded.error());
  }
  const auto words = decoded.value().code.size() / 4;
  if (words < 2) {
    return Result<artifacts::DirectoryTpages>::failure(object_error(
        ErrorCode::invalid_data_object, "The directory-tpage object is truncated.", context));
  }
  std::set<std::size_t> named;
  if (!use_named_link(decoded.value(), 0, NamedLinkKind::type, "texture-page-dir", &named)) {
    return Result<artifacts::DirectoryTpages>::failure(object_error(
        ErrorCode::unexpected_type, "The retail object is not texture-page-dir.", context));
  }
  const auto count = static_cast<std::size_t>(read_u32(decoded.value().code, 4));
  if (count == 0 || count > options.limits.max_tpage_entries || count > (words - 2) / 3) {
    return Result<artifacts::DirectoryTpages>::failure(
        object_error(ErrorCode::limit_exceeded,
                     "The directory-tpage count is invalid or exceeds its bound.", context, 4));
  }
  std::vector<bool> used(words);
  mark_words(&used, 0, 2);
  artifacts::DirectoryTpages output;
  output.lengths.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto entry_word = 2 + index * 3;
    const auto entry = read_u32(decoded.value().code, entry_word * 4);
    if ((entry & 0xffff7000u) != 0 ||
        !use_named_link(decoded.value(), entry_word + 1, NamedLinkKind::symbol, "#f", &named) ||
        !use_named_link(decoded.value(), entry_word + 2, NamedLinkKind::symbol, "#f", &named)) {
      return Result<artifacts::DirectoryTpages>::failure(
          object_error(ErrorCode::invalid_value, "A directory-tpage entry is malformed.", context,
                       entry_word * 4));
    }
    mark_words(&used, entry_word, 3);
    output.lengths.push_back(entry & 0xffffu);
  }
  const auto meaningful_words = 2 + count * 3;
  const auto canonical_words = (meaningful_words + 3) / 4 * 4;
  if (!decoded.value().pointers.empty() || named.size() != decoded.value().named_links.size() ||
      canonical_words != words) {
    return Result<artifacts::DirectoryTpages>::failure(
        object_error(ErrorCode::invalid_data_object,
                     "The directory-tpage object has unexpected linked data.", context));
  }
  if (allow_nonzero_canonical_tail) {
    mark_words(&used, meaningful_words, canonical_words - meaningful_words);
  }
  if (const auto error = validate_unmarked_words(decoded.value(), used, context, options)) {
    return Result<artifacts::DirectoryTpages>::failure(*error);
  }
  return Result<artifacts::DirectoryTpages>::success(std::move(output));
}

Result<artifacts::GameCount> parse_game_count(std::span<const std::uint8_t> data,
                                              const ObjectContext& context,
                                              const Options& options) {
  auto decoded = decode_data_object(data, context, options);
  if (!decoded) {
    return Result<artifacts::GameCount>::failure(decoded.error());
  }
  const auto words = decoded.value().code.size() / 4;
  if (words < 4) {
    return Result<artifacts::GameCount>::failure(object_error(
        ErrorCode::invalid_data_object, "The game-count object is truncated.", context));
  }
  std::set<std::size_t> named;
  if (!use_named_link(decoded.value(), 0, NamedLinkKind::type, "game-count-info", &named)) {
    return Result<artifacts::GameCount>::failure(object_error(
        ErrorCode::unexpected_type, "The retail object is not game-count-info.", context));
  }
  const auto count = static_cast<std::size_t>(read_u32(decoded.value().code, 4));
  if (count == 0 || count > options.limits.max_game_count_entries || count > (words - 4) / 2) {
    return Result<artifacts::GameCount>::failure(
        object_error(ErrorCode::limit_exceeded,
                     "The game-count entry count is invalid or exceeds its bound.", context, 4));
  }
  const auto meaningful_words = 4 + count * 2;
  if (meaningful_words != words) {
    return Result<artifacts::GameCount>::failure(object_error(
        ErrorCode::invalid_data_object,
        "The game-count data extent does not match the desktop object layout.", context));
  }
  std::vector<bool> used(words);
  mark_words(&used, 0, meaningful_words);
  artifacts::GameCount output;
  output.entries.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    output.entries.push_back({read_u32(decoded.value().code, (2 + index * 2) * 4),
                              read_u32(decoded.value().code, (3 + index * 2) * 4)});
  }
  output.unknown_1 = read_u32(decoded.value().code, (2 + count * 2) * 4);
  output.unknown_2 = read_u32(decoded.value().code, (3 + count * 2) * 4);
  if (!decoded.value().pointers.empty() || named.size() != decoded.value().named_links.size()) {
    return Result<artifacts::GameCount>::failure(
        object_error(ErrorCode::invalid_data_object,
                     "The game-count object has unexpected linked data.", context));
  }
  if (const auto error = validate_unmarked_words(decoded.value(), used, context, options)) {
    return Result<artifacts::GameCount>::failure(*error);
  }
  return Result<artifacts::GameCount>::success(std::move(output));
}

Result<artifacts::GameTextBank> parse_game_text(std::span<const std::uint8_t> data,
                                                std::uint32_t expected_language,
                                                const std::string& destination,
                                                const ObjectContext& context,
                                                const Options& options) {
  auto decoded = decode_data_object(data, context, options);
  if (!decoded) {
    return Result<artifacts::GameTextBank>::failure(decoded.error());
  }
  const auto words = decoded.value().code.size() / 4;
  if (words < 4) {
    return Result<artifacts::GameTextBank>::failure(object_error(
        ErrorCode::invalid_data_object, "The game-text object is truncated.", context));
  }
  std::set<std::size_t> named;
  if (!use_named_link(decoded.value(), 0, NamedLinkKind::type, "game-text-info", &named)) {
    return Result<artifacts::GameTextBank>::failure(object_error(
        ErrorCode::unexpected_type, "The retail object is not game-text-info.", context));
  }
  const auto count = static_cast<std::size_t>(read_u32(decoded.value().code, 4));
  const auto language = read_u32(decoded.value().code, 8);
  if (count == 0 || count > options.limits.max_text_lines_per_bank || count > (words - 4) / 2) {
    return Result<artifacts::GameTextBank>::failure(
        object_error(ErrorCode::limit_exceeded,
                     "The game-text line count is invalid or exceeds its bound.", context, 4));
  }
  if (language != expected_language) {
    return Result<artifacts::GameTextBank>::failure(
        object_error(ErrorCode::invalid_value,
                     "The game-text language does not match its retail path.", context, 8));
  }

  std::vector<bool> used(words);
  const auto main_words = 4 + count * 2;
  const auto aligned_main_words = (main_words + 3) / 4 * 4;
  if (aligned_main_words > words) {
    return Result<artifacts::GameTextBank>::failure(
        object_error(ErrorCode::invalid_data_object,
                     "The game-text main table has an invalid extent.", context));
  }
  mark_words(&used, 0, main_words);
  for (std::size_t word = main_words; word < aligned_main_words; ++word) {
    if (read_u32(decoded.value().code, word * 4) != 0) {
      return Result<artifacts::GameTextBank>::failure(
          object_error(ErrorCode::invalid_data_object,
                       "The game-text main table has nonzero padding.", context, word * 4));
    }
  }
  mark_words(&used, main_words, aligned_main_words - main_words);
  std::set<std::size_t> pointers;
  std::map<std::size_t, std::string> strings;
  auto group =
      read_goal_string(decoded.value(), 3, &used, &pointers, &named, &strings, context, options);
  if (!group) {
    return Result<artifacts::GameTextBank>::failure(group.error());
  }
  if (group.value() != "common") {
    return Result<artifacts::GameTextBank>::failure(object_error(
        ErrorCode::invalid_value, "The Jak 1 retail text group is not common.", context));
  }

  std::map<std::uint32_t, std::string> ordered_lines;
  for (std::size_t index = 0; index < count; ++index) {
    if ((index & 0x3ffu) == 0) {
      if (const auto error = cancellation_error(options, context.source_path, expected_language)) {
        return Result<artifacts::GameTextBank>::failure(*error);
      }
    }
    const auto id_word = 4 + index * 2;
    const auto id = read_u32(decoded.value().code, id_word * 4);
    auto text = read_goal_string(decoded.value(), id_word + 1, &used, &pointers, &named, &strings,
                                 context, options);
    if (!text) {
      return Result<artifacts::GameTextBank>::failure(text.error());
    }
    if (!ordered_lines.emplace(id, text.take_value()).second) {
      return Result<artifacts::GameTextBank>::failure(
          object_error(ErrorCode::duplicate_text_id, "A retail game-text ID is duplicated.",
                       context, id_word * 4));
    }
  }
  if (pointers.size() != decoded.value().pointers.size() ||
      named.size() != decoded.value().named_links.size()) {
    return Result<artifacts::GameTextBank>::failure(
        object_error(ErrorCode::invalid_data_object,
                     "The game-text object has unexpected linked data.", context));
  }
  if (const auto error = validate_all_words_used(used, context, options)) {
    return Result<artifacts::GameTextBank>::failure(*error);
  }

  artifacts::GameTextBank output;
  output.destination_basename = destination;
  output.language_id = language;
  output.group_name = group.take_value();
  output.lines.reserve(ordered_lines.size());
  for (auto& [id, text] : ordered_lines) {
    output.lines.push_back({id, std::move(text)});
  }
  return Result<artifacts::GameTextBank>::success(std::move(output));
}

bool valid_name(std::string_view value, std::uint32_t limit) {
  if (value.empty() || value.size() > limit) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7e && byte != '/' && byte != '\\' && byte != ':';
  });
}

std::optional<Error> canonicalize_public_data(const PublicAdditions& additions,
                                              std::vector<artifacts::GameTextBank>* retail_text,
                                              std::vector<artifacts::SubtitleBank>* subtitles,
                                              const Options& options) {
  std::map<std::uint32_t, artifacts::GameTextBank*> retail_by_language;
  for (auto& bank : *retail_text) {
    retail_by_language.emplace(bank.language_id, &bank);
  }
  std::set<std::uint32_t> addition_languages;
  for (const auto& addition : additions.game_text) {
    if (const auto error =
            cancellation_error(options, addition.destination_basename, addition.language_id)) {
      return error;
    }
    const auto retail = retail_by_language.find(addition.language_id);
    const auto expected = std::to_string(addition.language_id) + "COMMON.TXT";
    if (retail == retail_by_language.end() ||
        !addition_languages.insert(addition.language_id).second ||
        addition.destination_basename != expected || addition.group_name != "common" ||
        addition.lines.size() > options.limits.max_text_lines_per_bank) {
      return make_error(ErrorCode::invalid_public_data,
                        "A public game-text addition has an invalid or duplicate bank.",
                        addition.destination_basename, {}, addition.language_id);
    }
    std::map<std::uint32_t, std::string> merged;
    std::size_t bank_string_bytes = retail->second->group_name.size();
    for (std::size_t index = 0; index < retail->second->lines.size(); ++index) {
      if ((index & 0x3ffu) == 0) {
        if (const auto error =
                cancellation_error(options, addition.destination_basename, addition.language_id)) {
          return error;
        }
      }
      auto& line = retail->second->lines[index];
      if (!add_with_limit(&bank_string_bytes, line.encoded_text.size(),
                          options.limits.max_generated_bank_string_bytes)) {
        return make_error(ErrorCode::limit_exceeded,
                          "A generated game-text bank exceeds the aggregate string bound.",
                          addition.destination_basename, {}, addition.language_id);
      }
      merged.emplace(line.id, std::move(line.encoded_text));
    }
    std::set<std::uint32_t> addition_ids;
    for (std::size_t index = 0; index < addition.lines.size(); ++index) {
      if ((index & 0x3ffu) == 0) {
        if (const auto error =
                cancellation_error(options, addition.destination_basename, addition.language_id)) {
          return error;
        }
      }
      const auto& line = addition.lines[index];
      if (line.encoded_text.size() > options.limits.max_string_bytes ||
          !addition_ids.insert(line.id).second) {
        return make_error(ErrorCode::invalid_public_data,
                          "A public game-text addition has a duplicate ID or oversized string.",
                          addition.destination_basename, {}, addition.language_id);
      }
      const auto existing = merged.find(line.id);
      if (existing == merged.end()) {
        if (merged.size() >= options.limits.max_text_lines_per_bank) {
          return make_error(ErrorCode::limit_exceeded,
                            "Merged retail and public game text exceeds the configured line bound.",
                            addition.destination_basename, {}, addition.language_id);
        }
      } else {
        bank_string_bytes -= existing->second.size();
      }
      if (!add_with_limit(&bank_string_bytes, line.encoded_text.size(),
                          options.limits.max_generated_bank_string_bytes)) {
        return make_error(ErrorCode::limit_exceeded,
                          "A generated game-text bank exceeds the aggregate string bound.",
                          addition.destination_basename, {}, addition.language_id);
      }
      if (existing == merged.end()) {
        merged.emplace(line.id, line.encoded_text);
      } else {
        existing->second = line.encoded_text;
      }
    }
    if (merged.size() > options.limits.max_text_lines_per_bank) {
      return make_error(ErrorCode::limit_exceeded,
                        "Merged retail and public game text exceeds the configured line bound.",
                        addition.destination_basename, {}, addition.language_id);
    }
    retail->second->lines.clear();
    retail->second->lines.reserve(merged.size());
    std::size_t merged_index = 0;
    for (auto& [id, text] : merged) {
      if ((merged_index++ & 0x3ffu) == 0) {
        if (const auto error =
                cancellation_error(options, addition.destination_basename, addition.language_id)) {
          return error;
        }
      }
      retail->second->lines.push_back({id, std::move(text)});
    }
  }
  for (const auto& bank : *retail_text) {
    std::size_t bank_string_bytes = bank.group_name.size();
    for (const auto& line : bank.lines) {
      if (!add_with_limit(&bank_string_bytes, line.encoded_text.size(),
                          options.limits.max_generated_bank_string_bytes)) {
        return make_error(ErrorCode::limit_exceeded,
                          "A generated game-text bank exceeds the aggregate string bound.",
                          bank.destination_basename, {}, bank.language_id);
      }
    }
  }

  if (options.subtitle_mode == artifacts::SubtitleMode::empty) {
    if (!additions.subtitles.empty()) {
      return make_error(ErrorCode::invalid_public_data,
                        "The empty-subtitle mode does not accept public subtitle content.");
    }
    subtitles->clear();
    subtitles->reserve(kRetailLanguageCount);
    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      subtitles->push_back({std::to_string(language) + "SUBTIT.TXT", language, {}});
    }
    return {};
  }

  if (additions.subtitles.size() != kRetailLanguageCount) {
    return make_error(ErrorCode::invalid_public_data,
                      "The public data must provide one subtitle bank for each retail language.");
  }
  std::array<const artifacts::SubtitleBank*, kRetailLanguageCount> banks{};
  for (const auto& bank : additions.subtitles) {
    if (bank.language_id >= kRetailLanguageCount || banks[bank.language_id]) {
      return make_error(ErrorCode::invalid_public_data,
                        "A public subtitle bank has an invalid or duplicate language.",
                        bank.destination_basename, {}, bank.language_id);
    }
    banks[bank.language_id] = &bank;
  }
  std::vector<artifacts::SubtitleBank> canonical_subtitles;
  canonical_subtitles.reserve(kRetailLanguageCount);
  for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
    const auto& bank = *banks[language];
    const auto expected = std::to_string(language) + "SUBTIT.TXT";
    if (bank.language_id != language || bank.destination_basename != expected ||
        bank.scenes.empty() || bank.scenes.size() > options.limits.max_subtitle_scenes_per_bank) {
      return make_error(ErrorCode::invalid_public_data,
                        "A public subtitle bank is missing, duplicated, or invalid.",
                        bank.destination_basename, {}, bank.language_id);
    }
    std::set<std::string> scene_names;
    std::size_t bank_string_bytes = 0;
    for (std::size_t scene_index = 0; scene_index < bank.scenes.size(); ++scene_index) {
      const auto& scene = bank.scenes[scene_index];
      if (!valid_name(scene.name, options.limits.max_name_bytes) ||
          !scene_names.insert(scene.name).second ||
          scene.lines.size() > options.limits.max_subtitle_lines_per_scene ||
          scene.lines.size() > std::numeric_limits<std::uint16_t>::max()) {
        return make_error(ErrorCode::invalid_public_data,
                          "A public subtitle scene is invalid or duplicated.",
                          bank.destination_basename, {}, bank.language_id);
      }
      if (!add_with_limit(&bank_string_bytes, scene.name.size(),
                          options.limits.max_generated_bank_string_bytes)) {
        return make_error(ErrorCode::limit_exceeded,
                          "A public subtitle bank exceeds the aggregate string bound.",
                          bank.destination_basename, {}, bank.language_id);
      }
      for (std::size_t line_index = 0; line_index < scene.lines.size(); ++line_index) {
        if ((line_index & 0x3ffu) == 0) {
          if (const auto error =
                  cancellation_error(options, bank.destination_basename, bank.language_id)) {
            return error;
          }
        }
        const auto& line = scene.lines[line_index];
        if (line.encoded_text.size() > options.limits.max_string_bytes ||
            line.encoded_speaker.size() > options.limits.max_string_bytes ||
            !add_with_limit(&bank_string_bytes, line.encoded_text.size(),
                            options.limits.max_generated_bank_string_bytes) ||
            !add_with_limit(&bank_string_bytes, line.encoded_speaker.size(),
                            options.limits.max_generated_bank_string_bytes)) {
          return make_error(ErrorCode::limit_exceeded,
                            "A public subtitle bank exceeds a configured string bound.",
                            bank.destination_basename, {}, bank.language_id);
        }
      }
      if ((scene_index & 0xffu) == 0) {
        if (const auto error =
                cancellation_error(options, bank.destination_basename, bank.language_id)) {
          return error;
        }
      }
    }
    auto canonical_bank = bank;
    std::sort(canonical_bank.scenes.begin(), canonical_bank.scenes.end(),
              [](const auto& left, const auto& right) { return left.name < right.name; });
    for (auto& scene : canonical_bank.scenes) {
      std::stable_sort(
          scene.lines.begin(), scene.lines.end(),
          [](const auto& left, const auto& right) { return left.frame_start < right.frame_start; });
    }
    canonical_subtitles.push_back(std::move(canonical_bank));
  }
  *subtitles = std::move(canonical_subtitles);
  return {};
}

}  // namespace

namespace {

Result<std::vector<std::uint8_t>> checked_object_extent(std::span<const std::uint8_t> bytes,
                                                        const std::string& source_relative_path,
                                                        const Options& options,
                                                        CheckedObjectProfile profile) {
  if (profile == CheckedObjectProfile::jak1_exact) {
    return Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  }
  if (bytes.size() < kV2HeaderBytes || read_u32(bytes, 0) != 0xffffffffu) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::invalid_data_object, "The Jak II GOAL data-object header is invalid.",
                   source_relative_path, {}, {}, 0));
  }
  if (read_u32(bytes, 8) == 4) {
    return Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
  }
  if (read_u32(bytes, 8) != 2) {
    return Result<std::vector<std::uint8_t>>::failure(
        make_error(ErrorCode::unsupported_data_object_version,
                   "The Jak II retail object is not a V2 or V4 GOAL data object.",
                   source_relative_path, {}, {}, 8));
  }
  const auto link_length = static_cast<std::size_t>(read_u32(bytes, 4));
  if (link_length < kV2HeaderBytes || link_length >= bytes.size() ||
      link_length % kLinkAlignment != 0) {
    return Result<std::vector<std::uint8_t>>::failure(make_error(
        ErrorCode::invalid_data_object, "The Jak II V2 data-object link extent is invalid.",
        source_relative_path, {}, {}, 4));
  }
  const auto physical_tail = (bytes.size() - link_length) % 4;
  const auto physical_padding = physical_tail == 0 ? 0 : 4 - physical_tail;
  const auto word_aligned_code_size = bytes.size() + physical_padding - link_length;
  const auto semantic_tail = word_aligned_code_size % kObjectAlignment;
  const auto semantic_padding = semantic_tail == 0 ? 0 : kObjectAlignment - semantic_tail;
  const auto padding = physical_padding + semantic_padding;
  if (bytes.size() > options.limits.max_archive_object_bytes ||
      padding > options.limits.max_archive_object_bytes - bytes.size()) {
    return Result<std::vector<std::uint8_t>>::failure(make_error(
        ErrorCode::limit_exceeded, "The padded Jak II V2 object exceeds the configured bound.",
        source_relative_path));
  }
  std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
  padded.resize(padded.size() + padding, 0);
  return Result<std::vector<std::uint8_t>>::success(std::move(padded));
}

}  // namespace

Result<artifacts::DirectoryTpages> parse_checked_directory_tpages(
    std::span<const std::uint8_t> bytes,
    std::string source_relative_path,
    const Options& options,
    CheckedObjectProfile profile) {
  auto owned = checked_object_extent(bytes, source_relative_path, options, profile);
  if (!owned) {
    return Result<artifacts::DirectoryTpages>::failure(owned.error());
  }
  return parse_directory_tpages(owned.value(), {std::move(source_relative_path), {}, {}}, options,
                                profile == CheckedObjectProfile::jak2_retail_extent);
}

Result<artifacts::GameTextBank> parse_checked_game_text(std::span<const std::uint8_t> bytes,
                                                        std::uint32_t language_id,
                                                        std::string destination_basename,
                                                        std::string source_relative_path,
                                                        const Options& options,
                                                        CheckedObjectProfile profile) {
  auto owned = checked_object_extent(bytes, source_relative_path, options, profile);
  if (!owned) {
    auto error = owned.error();
    error.language_id = language_id;
    return Result<artifacts::GameTextBank>::failure(std::move(error));
  }
  return parse_game_text(owned.value(), language_id, destination_basename,
                         {std::move(source_relative_path), {}, language_id}, options);
}

Result<artifacts::Inputs> build(const ValidatedTree& tree,
                                const PublicAdditions& public_additions,
                                const Options& options) {
  try {
    if (!valid_options(options)) {
      return Result<artifacts::Inputs>::failure(
          make_error(ErrorCode::invalid_argument, "The generated-input options are invalid."));
    }
    if (!supported_revision(tree.revision)) {
      return Result<artifacts::Inputs>::failure(
          make_error(ErrorCode::unsupported_revision,
                     "The extracted tree is not the initial SCUS-97124 Jak 1 revision."));
    }
    if (const auto error = cancellation_error(options)) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    if (const auto error = emit_progress(options, {ProgressStage::opening_game_archive,
                                                   0,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    auto game_path = checked_input_path(tree, kGameArchivePath);
    if (!game_path) {
      return Result<artifacts::Inputs>::failure(game_path.error());
    }
    if (const auto error = emit_progress(options, {ProgressStage::reading_game_archive,
                                                   0,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }

    bool cancellation_callback_failed = false;
    jak1_checked_dgo::Options dgo_options;
    dgo_options.max_input_bytes = options.limits.max_game_archive_input_bytes;
    dgo_options.max_compressed_bytes = options.limits.max_game_archive_compressed_bytes;
    dgo_options.max_expanded_bytes = options.limits.max_game_archive_expanded_bytes;
    dgo_options.max_object_bytes = options.limits.max_archive_object_bytes;
    dgo_options.max_total_object_bytes = options.limits.max_archive_total_object_bytes;
    dgo_options.max_objects = options.limits.max_archive_objects;
    dgo_options.max_expansion_ratio = options.limits.max_archive_expansion_ratio;
    dgo_options.file_read_chunk_bytes = options.limits.file_read_chunk_bytes;
    dgo_options.should_cancel = [&]() {
      const auto state = poll_cancel(options);
      cancellation_callback_failed = state == CallbackState::failed;
      return state != CallbackState::continue_work;
    };
    auto archive =
        jak1_checked_dgo::read_file(game_path.value(), std::string(kGameArchiveName), dgo_options);
    if (!archive) {
      if (cancellation_callback_failed) {
        return Result<artifacts::Inputs>::failure(
            make_error(ErrorCode::callback_failed, "The cancellation callback failed.",
                       std::string(kGameArchivePath), archive.error().object_index));
      }
      if (archive.error().code == jak1_checked_dgo::ErrorCode::cancelled) {
        return Result<artifacts::Inputs>::failure(
            make_error(ErrorCode::cancelled, "Retail GAME.CGO loading was cancelled.",
                       std::string(kGameArchivePath), archive.error().object_index));
      }
      auto error = make_error(ErrorCode::checked_dgo_failed,
                              "The checked DGO reader rejected retail GAME.CGO.",
                              std::string(kGameArchivePath), archive.error().object_index);
      error.checked_dgo_error = archive.error();
      return Result<artifacts::Inputs>::failure(std::move(error));
    }

    const jak1_checked_dgo::Object* directory_object = nullptr;
    const jak1_checked_dgo::Object* count_object = nullptr;
    std::optional<std::uint32_t> directory_index;
    std::optional<std::uint32_t> count_index;
    for (std::size_t index = 0; index < archive.value().objects.size(); ++index) {
      const auto& object = archive.value().objects[index];
      if (object.internal_name == "dir-tpages") {
        if (directory_object) {
          return Result<artifacts::Inputs>::failure(
              make_error(ErrorCode::duplicate_retail_object, "GAME.CGO repeats dir-tpages.",
                         std::string(kGameArchivePath), static_cast<std::uint32_t>(index)));
        }
        directory_object = &object;
        directory_index = static_cast<std::uint32_t>(index);
      } else if (object.internal_name == "game-cnt") {
        if (count_object) {
          return Result<artifacts::Inputs>::failure(
              make_error(ErrorCode::duplicate_retail_object, "GAME.CGO repeats game-cnt.",
                         std::string(kGameArchivePath), static_cast<std::uint32_t>(index)));
        }
        count_object = &object;
        count_index = static_cast<std::uint32_t>(index);
      }
    }
    if (!directory_object || !count_object) {
      return Result<artifacts::Inputs>::failure(
          make_error(ErrorCode::missing_retail_object,
                     "GAME.CGO does not contain both dir-tpages and game-cnt.",
                     std::string(kGameArchivePath)));
    }

    artifacts::Inputs output;
    if (const auto error = emit_progress(options, {ProgressStage::parsing_directory_tpages,
                                                   1,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    auto directory = parse_directory_tpages(
        directory_object->data, {std::string(kGameArchivePath), directory_index, std::nullopt},
        options);
    if (!directory) {
      return Result<artifacts::Inputs>::failure(directory.error());
    }
    output.directory_tpages = directory.take_value();

    if (const auto error = emit_progress(options, {ProgressStage::parsing_game_count,
                                                   2,
                                                   kProgressUnits,
                                                   std::string(kGameArchivePath),
                                                   {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    auto count = parse_game_count(
        count_object->data, {std::string(kGameArchivePath), count_index, std::nullopt}, options);
    if (!count) {
      return Result<artifacts::Inputs>::failure(count.error());
    }
    output.game_count = count.take_value();

    output.game_text.reserve(kRetailLanguageCount);
    for (std::uint32_t language = 0; language < kRetailLanguageCount; ++language) {
      const auto destination = std::to_string(language) + "COMMON.TXT";
      const auto relative_path = "TEXT/" + destination;
      if (const auto error = emit_progress(options, {ProgressStage::reading_game_text, 3 + language,
                                                     kProgressUnits, relative_path, language})) {
        return Result<artifacts::Inputs>::failure(*error);
      }
      auto bytes = read_direct_object(tree, relative_path, options, language);
      if (!bytes) {
        return Result<artifacts::Inputs>::failure(bytes.error());
      }
      auto text = parse_game_text(bytes.value(), language, destination,
                                  {relative_path, std::nullopt, language}, options);
      if (!text) {
        return Result<artifacts::Inputs>::failure(text.error());
      }
      output.game_text.push_back(text.take_value());
    }

    if (const auto error = emit_progress(
            options, {ProgressStage::merging_public_data, 10, kProgressUnits, {}, {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    if (const auto error = canonicalize_public_data(public_additions, &output.game_text,
                                                    &output.subtitles, options)) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    if (const auto error = emit_progress(
            options, {ProgressStage::complete, kProgressUnits, kProgressUnits, {}, {}})) {
      return Result<artifacts::Inputs>::failure(*error);
    }
    return Result<artifacts::Inputs>::success(std::move(output));
  } catch (const std::bad_alloc&) {
    return Result<artifacts::Inputs>::failure(make_error(
        ErrorCode::allocation_failed, "Could not allocate the generated-input working set."));
  } catch (const std::length_error&) {
    return Result<artifacts::Inputs>::failure(make_error(
        ErrorCode::allocation_failed, "A generated-input container exceeded its platform limit."));
  }
}

const char* error_code_name(ErrorCode code) {
  switch (code) {
    case ErrorCode::invalid_argument:
      return "invalid_argument";
    case ErrorCode::unsupported_revision:
      return "unsupported_revision";
    case ErrorCode::invalid_extracted_tree:
      return "invalid_extracted_tree";
    case ErrorCode::missing_input:
      return "missing_input";
    case ErrorCode::input_open_failed:
      return "input_open_failed";
    case ErrorCode::input_read_failed:
      return "input_read_failed";
    case ErrorCode::input_too_large:
      return "input_too_large";
    case ErrorCode::checked_dgo_failed:
      return "checked_dgo_failed";
    case ErrorCode::missing_retail_object:
      return "missing_retail_object";
    case ErrorCode::duplicate_retail_object:
      return "duplicate_retail_object";
    case ErrorCode::invalid_data_object:
      return "invalid_data_object";
    case ErrorCode::unsupported_data_object_version:
      return "unsupported_data_object_version";
    case ErrorCode::invalid_link_data:
      return "invalid_link_data";
    case ErrorCode::invalid_pointer:
      return "invalid_pointer";
    case ErrorCode::unexpected_type:
      return "unexpected_type";
    case ErrorCode::invalid_value:
      return "invalid_value";
    case ErrorCode::duplicate_text_id:
      return "duplicate_text_id";
    case ErrorCode::invalid_public_data:
      return "invalid_public_data";
    case ErrorCode::limit_exceeded:
      return "limit_exceeded";
    case ErrorCode::cancelled:
      return "cancelled";
    case ErrorCode::callback_failed:
      return "callback_failed";
    case ErrorCode::allocation_failed:
      return "allocation_failed";
  }
  return "unknown";
}

}  // namespace jak1_extracted_generated_inputs
