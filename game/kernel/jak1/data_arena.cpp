#include "data_arena.h"

#include <cstddef>
#include <cstring>
#include <limits>

#include "common/goal_constants.h"
#include "common/symbols.h"

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/memory_layout.h"
#include "game/kernel/jak1/kscheme.h"

namespace jak1 {
namespace {

constexpr std::uint32_t kSymbolTableEntrySize = 8;
constexpr std::uint32_t kSymbolTableOffset = HEAP_START;
constexpr std::uint32_t kSymbolTableEnd = kSymbolTableOffset + SYM_TABLE_MEM_SIZE;
constexpr std::uint32_t kGlobalHeapEnd = GLOBAL_HEAP_END;
constexpr std::uint32_t kS7 =
    kSymbolTableOffset + (GOAL_MAX_SYMBOLS / 2) * kSymbolTableEntrySize + BASIC_OFFSET;
constexpr std::uint32_t kSymbolTable2 = kSymbolTableOffset + BASIC_OFFSET;
constexpr std::uint32_t kLastSymbol = kSymbolTableOffset + SYM_TABLE_END * kSymbolTableEntrySize;
constexpr std::uint32_t kEmptyPair = kS7 + jak1_symbols::FIX_SYM_EMPTY_PAIR;
constexpr std::uint32_t kSymbolInfoOffset = jak1::SYM_INFO_OFFSET;
constexpr std::uint32_t kSymbolHashBucketBits = jak1::bits_for_sym() - 1;
constexpr std::uint32_t kSymbolHashBucketSignBit = 1U << (kSymbolHashBucketBits - 1);
constexpr std::uint32_t kSymbolHashBucketMask = (1U << kSymbolHashBucketBits) - 1;

static_assert(sizeof(kheapinfo) == 4 * sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, base) == 0);
static_assert(offsetof(kheapinfo, top) == sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, current) == 2 * sizeof(std::uint32_t));
static_assert(offsetof(kheapinfo, top_base) == 3 * sizeof(std::uint32_t));
static_assert(sizeof(Symbol) == sizeof(std::uint32_t));
static_assert(offsetof(Symbol, value) == 0);
static_assert(sizeof(SymInfo) == 2 * sizeof(std::uint32_t));
static_assert(offsetof(SymInfo, hash) == 0);
static_assert(offsetof(SymInfo, str) == sizeof(std::uint32_t));
static_assert(offsetof(Type, symbol) == 0);
static_assert(offsetof(Type, allocated_size) == 0x8);
static_assert(offsetof(Type, padded_size) == 0xa);
static_assert(offsetof(Type, num_methods) == 0xe);
static_assert(offsetof(Type, new_method) == 0x10);
static_assert(kGlobalHeapEnd == minimum_data_arena_header_size());
static_assert(kSymbolTableEnd <= kGlobalHeapEnd);
static_assert(kSymbolTableOffset >= GLOBAL_HEAP_INFO_ADDR + sizeof(kheapinfo));
static_assert(kSymbolHashBucketBits == 14);

void write_u32(std::byte* storage, std::size_t offset, std::uint32_t value) {
  std::memcpy(storage + offset, &value, sizeof(value));
}

std::uint32_t read_u32(const std::byte* storage, std::uint32_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, storage + offset, sizeof(value));
  return value;
}

std::uint16_t read_u16(const std::byte* storage, std::uint32_t offset) {
  std::uint16_t value = 0;
  std::memcpy(&value, storage + offset, sizeof(value));
  return value;
}

bool has_bytes(std::size_t storage_size, std::uint32_t offset, std::size_t size) {
  const auto storage_offset = static_cast<std::size_t>(offset);
  return storage_offset <= storage_size && size <= storage_size - storage_offset;
}

bool has_aligned_word(std::size_t storage_size, std::uint32_t offset) {
  return (offset % alignof(std::uint32_t)) == 0 &&
         has_bytes(storage_size, offset, sizeof(std::uint32_t));
}

bool is_readable_basic_pointer(std::size_t storage_size, std::uint32_t value) {
  if (value < BASIC_OFFSET || (value & OFFSET_MASK) != BASIC_OFFSET) {
    return false;
  }

  const auto raw_offset = value - BASIC_OFFSET;
  return has_aligned_word(storage_size, raw_offset) && has_aligned_word(storage_size, value);
}

bool checked_add(std::uint32_t base, std::uint32_t delta, std::uint32_t* result) {
  if (delta > std::numeric_limits<std::uint32_t>::max() - base) {
    return false;
  }
  *result = base + delta;
  return true;
}

bool checked_add_signed(std::uint32_t base, std::int32_t delta, std::uint32_t* result) {
  if (delta >= 0) {
    return checked_add(base, static_cast<std::uint32_t>(delta), result);
  }

  const auto magnitude = static_cast<std::uint32_t>(-static_cast<std::int64_t>(delta));
  if (magnitude > base) {
    return false;
  }
  *result = base - magnitude;
  return true;
}

bool read_fixed_symbol_value(const std::byte* storage,
                             std::size_t storage_size,
                             const DataArenaHeader& header,
                             std::int32_t displacement,
                             std::uint32_t* value) {
  std::uint32_t symbol_offset = 0;
  if (!checked_add_signed(header.s7, displacement, &symbol_offset) ||
      !has_aligned_word(storage_size, symbol_offset)) {
    return false;
  }
  *value = read_u32(storage, symbol_offset);
  return true;
}

bool is_valid_header(const DataArenaHeader& header, std::size_t storage_size) {
  if (!has_bytes(storage_size, header.global_heap_info, sizeof(kheapinfo)) ||
      !has_bytes(storage_size, header.global_heap_base, 1) ||
      header.global_heap_base > header.global_heap_current ||
      header.global_heap_current > header.global_heap_end ||
      static_cast<std::size_t>(header.global_heap_end) > storage_size) {
    return false;
  }

  if ((header.symbol_table & OFFSET_MASK) != 0 || (header.symbol_table_end & OFFSET_MASK) != 0 ||
      (header.symbol_table2 & OFFSET_MASK) != BASIC_OFFSET ||
      (header.s7 & OFFSET_MASK) != BASIC_OFFSET || (header.last_symbol & OFFSET_MASK) != 0) {
    return false;
  }

  std::uint32_t expected_symbol_table_end = 0;
  std::uint32_t expected_symbol_table2 = 0;
  std::uint32_t expected_s7 = 0;
  std::uint32_t expected_last_symbol = 0;
  std::uint32_t expected_fixed_symbol_end = 0;
  std::uint32_t expected_empty_pair = 0;
  std::uint32_t expected_true_value = 0;
  if (!checked_add(header.symbol_table, SYM_TABLE_MEM_SIZE, &expected_symbol_table_end) ||
      !checked_add(header.symbol_table, BASIC_OFFSET, &expected_symbol_table2) ||
      !checked_add(header.symbol_table,
                   (GOAL_MAX_SYMBOLS / 2) * kSymbolTableEntrySize + BASIC_OFFSET, &expected_s7) ||
      !checked_add(header.symbol_table, SYM_TABLE_END * kSymbolTableEntrySize,
                   &expected_last_symbol) ||
      !checked_add(expected_s7, jak1_symbols::FIX_FIXED_SYM_END_OFFSET,
                   &expected_fixed_symbol_end) ||
      !checked_add_signed(expected_s7, jak1_symbols::FIX_SYM_EMPTY_PAIR, &expected_empty_pair) ||
      !checked_add(expected_s7, jak1_symbols::FIX_SYM_TRUE, &expected_true_value)) {
    return false;
  }

  if (header.symbol_table != header.global_heap_base ||
      header.symbol_table_end != expected_symbol_table_end ||
      header.symbol_table2 != expected_symbol_table2 || header.s7 != expected_s7 ||
      header.last_symbol != expected_last_symbol || header.empty_pair != expected_empty_pair ||
      header.false_value != expected_s7 || header.true_value != expected_true_value ||
      header.global_heap_current < header.symbol_table_end ||
      header.symbol_table_end > header.global_heap_end ||
      !has_bytes(storage_size, header.symbol_table, SYM_TABLE_MEM_SIZE) ||
      !has_aligned_word(storage_size, header.symbol_table2) ||
      !has_aligned_word(storage_size, header.s7) ||
      !has_aligned_word(storage_size, header.last_symbol - sizeof(std::uint32_t)) ||
      expected_fixed_symbol_end > header.last_symbol) {
    return false;
  }

  return true;
}

std::uint32_t stateless_goal_crc32(std::string_view name) {
  std::uint32_t crc = 0;
  for (const auto character : name) {
    std::uint32_t table_entry = (crc >> 24) << 24;
    for (int bit = 0; bit < 8; ++bit) {
      table_entry =
          (table_entry & 0x80000000U) ? (table_entry << 1) ^ CRC_POLY : (table_entry << 1);
    }
    crc = table_entry ^ ((crc << 8) | static_cast<unsigned char>(character));
  }
  return ~crc;
}

std::int32_t symbol_hash_displacement(std::uint32_t hash) {
  const auto low_bucket = hash & kSymbolHashBucketMask;
  const auto bucket = (low_bucket & kSymbolHashBucketSignBit)
                          ? static_cast<std::int32_t>(low_bucket) -
                                static_cast<std::int32_t>(1U << kSymbolHashBucketBits)
                          : static_cast<std::int32_t>(low_bucket);
  return bucket * static_cast<std::int32_t>(kSymbolTableEntrySize);
}

enum class ProbeResult {
  Continue,
  Vacant,
  Exhausted,
  Found,
  MalformedSymbol,
  MalformedString,
};

ProbeResult inspect_symbol(const std::byte* storage,
                           std::size_t storage_size,
                           std::uint32_t symbol_offset,
                           std::uint32_t requested_hash,
                           std::string_view requested_name) {
  std::uint32_t symbol_info_offset = 0;
  if (!has_aligned_word(storage_size, symbol_offset) ||
      !checked_add(symbol_offset, kSymbolInfoOffset, &symbol_info_offset) ||
      !has_bytes(storage_size, symbol_info_offset, sizeof(SymInfo))) {
    return ProbeResult::MalformedSymbol;
  }

  const auto stored_hash = read_u32(storage, symbol_info_offset);
  if (stored_hash == 0) {
    return ProbeResult::Vacant;
  }
  if (stored_hash != requested_hash) {
    return ProbeResult::Continue;
  }

  const auto string_offset = read_u32(storage, symbol_info_offset + sizeof(std::uint32_t));
  if (string_offset < BASIC_OFFSET || (string_offset & OFFSET_MASK) != BASIC_OFFSET ||
      !has_aligned_word(storage_size, string_offset)) {
    return ProbeResult::MalformedString;
  }

  const auto raw_string_offset = string_offset - BASIC_OFFSET;
  std::uint32_t string_data_offset = 0;
  if (!has_aligned_word(storage_size, raw_string_offset) ||
      !checked_add(string_offset, sizeof(std::uint32_t), &string_data_offset) ||
      !has_bytes(storage_size, string_data_offset, requested_name.size() + 1)) {
    return ProbeResult::MalformedString;
  }

  if (!requested_name.empty() && std::memcmp(storage + string_data_offset, requested_name.data(),
                                             requested_name.size()) != 0) {
    return ProbeResult::Continue;
  }
  if (storage[string_data_offset + requested_name.size()] != std::byte{0}) {
    return ProbeResult::Continue;
  }
  return ProbeResult::Found;
}

ProbeResult probe_symbol_area(const std::byte* storage,
                              std::size_t storage_size,
                              std::uint32_t start,
                              std::uint32_t end,
                              std::uint32_t requested_hash,
                              std::string_view requested_name,
                              std::uint32_t* found_symbol_offset) {
  for (auto symbol_offset = start; symbol_offset < end;) {
    const auto result =
        inspect_symbol(storage, storage_size, symbol_offset, requested_hash, requested_name);
    if (result != ProbeResult::Continue) {
      if (result == ProbeResult::Found) {
        *found_symbol_offset = symbol_offset;
      }
      return result;
    }
    if (!checked_add(symbol_offset, kSymbolTableEntrySize, &symbol_offset)) {
      return ProbeResult::MalformedSymbol;
    }
  }
  return ProbeResult::Exhausted;
}

DataArenaSymbolValueLookupResult make_found_result(const DataArenaHeader& header,
                                                   std::uint32_t symbol_offset) {
  const auto displacement =
      static_cast<std::int64_t>(symbol_offset) - static_cast<std::int64_t>(header.s7);
  if (displacement < std::numeric_limits<std::int32_t>::min() ||
      displacement > std::numeric_limits<std::int32_t>::max()) {
    return {.error = DataArenaSymbolValueLookupError::MalformedSymbol};
  }
  return {
      .cell =
          {
              .goal_offset = symbol_offset,
              .s7_relative_displacement = static_cast<std::int32_t>(displacement),
          },
      .error = DataArenaSymbolValueLookupError::None,
  };
}

DataArenaSymbolValueLookupResult lookup_fixed_symbols(const std::byte* storage,
                                                      std::size_t storage_size,
                                                      const DataArenaHeader& header,
                                                      std::uint32_t requested_hash,
                                                      std::string_view requested_name) {
  std::uint32_t fixed_symbol_end = 0;
  if (!checked_add(header.s7, jak1_symbols::FIX_FIXED_SYM_END_OFFSET, &fixed_symbol_end)) {
    return {.error = DataArenaSymbolValueLookupError::InvalidHeader};
  }

  for (auto symbol_offset = header.s7; symbol_offset < fixed_symbol_end;) {
    const auto result =
        inspect_symbol(storage, storage_size, symbol_offset, requested_hash, requested_name);
    switch (result) {
      case ProbeResult::Found:
        return make_found_result(header, symbol_offset);
      case ProbeResult::MalformedSymbol:
        return {.error = DataArenaSymbolValueLookupError::MalformedSymbol};
      case ProbeResult::MalformedString:
        return {.error = DataArenaSymbolValueLookupError::MalformedString};
      case ProbeResult::Continue:
      case ProbeResult::Vacant:
      case ProbeResult::Exhausted:
        break;
    }
    if (!checked_add(symbol_offset, kSymbolTableEntrySize, &symbol_offset)) {
      return {.error = DataArenaSymbolValueLookupError::MalformedSymbol};
    }
  }
  return {.error = DataArenaSymbolValueLookupError::NotFound};
}

DataArenaSymbolValueLookupResult finish_symbol_probe(const std::byte* storage,
                                                     std::size_t storage_size,
                                                     const DataArenaHeader& header,
                                                     ProbeResult result,
                                                     std::uint32_t found_symbol_offset,
                                                     std::uint32_t requested_hash,
                                                     std::string_view requested_name) {
  switch (result) {
    case ProbeResult::Found:
      return make_found_result(header, found_symbol_offset);
    case ProbeResult::MalformedSymbol:
      return {.error = DataArenaSymbolValueLookupError::MalformedSymbol};
    case ProbeResult::MalformedString:
      return {.error = DataArenaSymbolValueLookupError::MalformedString};
    case ProbeResult::Vacant:
      return lookup_fixed_symbols(storage, storage_size, header, requested_hash, requested_name);
    case ProbeResult::Continue:
    case ProbeResult::Exhausted:
      return {.error = DataArenaSymbolValueLookupError::NotFound};
  }
  return {.error = DataArenaSymbolValueLookupError::NotFound};
}

}  // namespace

DataArenaHeaderResult initialize_data_arena_header(std::byte* storage, std::size_t storage_size) {
  if (!storage) {
    return {.error = DataArenaHeaderError::NullStorage};
  }
  if (storage_size < minimum_data_arena_header_size()) {
    return {.error = DataArenaHeaderError::StorageTooSmall};
  }
  if (storage_size > std::numeric_limits<std::uint32_t>::max()) {
    return {.error = DataArenaHeaderError::StorageTooLarge};
  }

  std::memset(storage, 0, kGlobalHeapEnd);

  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, base), HEAP_START);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top), kGlobalHeapEnd);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, current), kSymbolTableEnd);
  write_u32(storage, GLOBAL_HEAP_INFO_ADDR + offsetof(kheapinfo, top_base), kGlobalHeapEnd);

  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_EMPTY_CAR, kEmptyPair);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_EMPTY_CDR, kEmptyPair);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_FALSE, kS7);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_TRUE, kS7 + jak1_symbols::FIX_SYM_TRUE);
  write_u32(storage, kS7 + jak1_symbols::FIX_SYM_GLOBAL_HEAP, GLOBAL_HEAP_INFO_ADDR);

  return {
      .header =
          {
              .global_heap_info = GLOBAL_HEAP_INFO_ADDR,
              .global_heap_base = HEAP_START,
              .global_heap_current = kSymbolTableEnd,
              .global_heap_end = kGlobalHeapEnd,
              .symbol_table = kSymbolTableOffset,
              .symbol_table_end = kSymbolTableEnd,
              .symbol_table2 = kSymbolTable2,
              .last_symbol = kLastSymbol,
              .s7 = kS7,
              .empty_pair = kEmptyPair,
              .false_value = kS7,
              .true_value = kS7 + jak1_symbols::FIX_SYM_TRUE,
          },
      .error = DataArenaHeaderError::None,
  };
}

DataArenaSymbolValueLookupResult find_data_arena_symbol_value_cell(const std::byte* storage,
                                                                   std::size_t storage_size,
                                                                   const DataArenaHeader& header,
                                                                   std::string_view name) {
  if (!storage) {
    return {.error = DataArenaSymbolValueLookupError::NullStorage};
  }
  if (storage_size > std::numeric_limits<std::uint32_t>::max()) {
    return {.error = DataArenaSymbolValueLookupError::StorageTooLarge};
  }
  if (!is_valid_header(header, storage_size)) {
    return {.error = DataArenaSymbolValueLookupError::InvalidHeader};
  }
  if (name.size() > std::numeric_limits<std::int32_t>::max()) {
    return {.error = DataArenaSymbolValueLookupError::QueryTooLong};
  }
  if (name.find('\0') != std::string_view::npos) {
    return {.error = DataArenaSymbolValueLookupError::QueryContainsNull};
  }
  if (name == "_empty_") {
    return {.error = DataArenaSymbolValueLookupError::UnsupportedPseudoSymbol};
  }

  const auto hash = stateless_goal_crc32(name);
  if (hash == 0) {
    return {.error = DataArenaSymbolValueLookupError::UnrepresentableHash};
  }
  const auto displacement = symbol_hash_displacement(hash);
  std::uint32_t primary_start = 0;
  if (!checked_add_signed(header.s7, displacement, &primary_start)) {
    return {.error = DataArenaSymbolValueLookupError::InvalidHeader};
  }

  std::uint32_t primary_end = 0;
  std::uint32_t overflow_start = 0;
  std::uint32_t overflow_end = 0;
  if (displacement > 0) {
    primary_end = header.last_symbol;
    overflow_start = header.symbol_table2;
    if (!checked_add_signed(header.s7, -0x10, &overflow_end)) {
      return {.error = DataArenaSymbolValueLookupError::InvalidHeader};
    }
  } else {
    if (!checked_add_signed(header.s7, -0x10, &primary_end) ||
        !checked_add(header.s7, jak1_symbols::FIX_FIXED_SYM_END_OFFSET, &overflow_start)) {
      return {.error = DataArenaSymbolValueLookupError::InvalidHeader};
    }
    overflow_end = header.last_symbol;
  }

  std::uint32_t found_symbol_offset = 0;
  auto result = probe_symbol_area(storage, storage_size, primary_start, primary_end, hash, name,
                                  &found_symbol_offset);
  if (result != ProbeResult::Exhausted) {
    return finish_symbol_probe(storage, storage_size, header, result, found_symbol_offset, hash,
                               name);
  }

  result = probe_symbol_area(storage, storage_size, overflow_start, overflow_end, hash, name,
                             &found_symbol_offset);
  if (result != ProbeResult::Exhausted) {
    return finish_symbol_probe(storage, storage_size, header, result, found_symbol_offset, hash,
                               name);
  }

  return lookup_fixed_symbols(storage, storage_size, header, hash, name);
}

DataArenaBasicMethodValueLookupResult read_data_arena_basic_method_value(
    const std::byte* storage,
    std::size_t storage_size,
    const DataArenaHeader& header,
    std::uint32_t object,
    std::uint32_t method_id) {
  if (!storage) {
    return {.error = DataArenaBasicMethodValueLookupError::NullStorage};
  }
  if (storage_size > std::numeric_limits<std::uint32_t>::max()) {
    return {.error = DataArenaBasicMethodValueLookupError::StorageTooLarge};
  }
  if (!is_valid_header(header, storage_size)) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidHeader};
  }
  if (!is_readable_basic_pointer(storage_size, object)) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidObject};
  }

  const auto object_raw_offset = object - BASIC_OFFSET;
  const auto object_type = read_u32(storage, object_raw_offset);
  if (!is_readable_basic_pointer(storage_size, object_type)) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidObjectType};
  }

  std::uint32_t type_type = 0;
  if (!read_fixed_symbol_value(storage, storage_size, header, jak1_symbols::FIX_SYM_TYPE_TYPE,
                               &type_type) ||
      !is_readable_basic_pointer(storage_size, type_type) ||
      read_u32(storage, type_type - BASIC_OFFSET) != type_type) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidTypeType};
  }
  if (read_u32(storage, object_type - BASIC_OFFSET) != type_type) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidTypeTag};
  }

  std::uint32_t allocated_size_offset = 0;
  std::uint32_t padded_size_offset = 0;
  std::uint32_t method_count_offset = 0;
  if (!checked_add(object_type, offsetof(Type, allocated_size), &allocated_size_offset) ||
      !checked_add(object_type, offsetof(Type, padded_size), &padded_size_offset) ||
      !checked_add(object_type, offsetof(Type, num_methods), &method_count_offset) ||
      !has_bytes(storage_size, allocated_size_offset, sizeof(std::uint16_t)) ||
      !has_bytes(storage_size, padded_size_offset, sizeof(std::uint16_t)) ||
      !has_bytes(storage_size, method_count_offset, sizeof(std::uint16_t))) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidObjectType};
  }

  const auto allocated_size = read_u16(storage, allocated_size_offset);
  const auto padded_size = read_u16(storage, padded_size_offset);
  const auto expected_padded_size =
      (static_cast<std::uint32_t>(allocated_size) + 0xfU) & ~std::uint32_t{0xfU};
  if (allocated_size < BASIC_OFFSET || allocated_size > 0xfff0 ||
      static_cast<std::uint32_t>(padded_size) != expected_padded_size ||
      !has_bytes(storage_size, object_raw_offset, padded_size)) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidObjectSize};
  }

  const auto num_methods = read_u16(storage, method_count_offset);
  if (method_id >= num_methods) {
    return {.error = DataArenaBasicMethodValueLookupError::MethodOutOfRange};
  }

  std::uint32_t method_table_offset = 0;
  std::uint32_t method_offset = 0;
  if (method_id > std::numeric_limits<std::uint32_t>::max() / sizeof(std::uint32_t) ||
      !checked_add(object_type, offsetof(Type, new_method), &method_table_offset) ||
      !checked_add(method_table_offset, method_id * sizeof(std::uint32_t), &method_offset) ||
      !has_aligned_word(storage_size, method_offset)) {
    return {.error = DataArenaBasicMethodValueLookupError::InvalidMethodSlot};
  }

  return {
      .value =
          {
              .object_type = object_type,
              .allocated_size = allocated_size,
              .padded_size = padded_size,
              .method_value = read_u32(storage, method_offset),
          },
      .error = DataArenaBasicMethodValueLookupError::None,
  };
}

}  // namespace jak1
