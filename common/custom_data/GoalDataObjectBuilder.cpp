#include "GoalDataObjectBuilder.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace goal_data_object_builder {
namespace {

constexpr std::uint32_t align16(std::uint32_t value) {
  return (value + 15u) & ~15u;
}

void append_u32(std::vector<std::uint8_t>* output, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    output->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void append_variable_length_integer(std::vector<std::uint8_t>* output, std::uint32_t value) {
  while (value > std::numeric_limits<std::uint8_t>::max()) {
    output->push_back(std::numeric_limits<std::uint8_t>::max());
    value -= std::numeric_limits<std::uint8_t>::max();
  }
  if (value == std::numeric_limits<std::uint8_t>::max()) {
    output->push_back(std::numeric_limits<std::uint8_t>::max());
    output->push_back(0);
  } else {
    output->push_back(static_cast<std::uint8_t>(value));
  }
}

void append_better_variable_length_integer(std::vector<std::uint8_t>* output, std::uint32_t value) {
  if (value > 0x00ffffff) {
    output->push_back(static_cast<std::uint8_t>((value & 0xff) | 3));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
    output->push_back(static_cast<std::uint8_t>(value >> 16));
    output->push_back(static_cast<std::uint8_t>(value >> 24));
  } else if (value > 0x0000ffff) {
    output->push_back(static_cast<std::uint8_t>((value & 0xff) | 2));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
    output->push_back(static_cast<std::uint8_t>(value >> 16));
  } else if (value > 0x000000ff) {
    output->push_back(static_cast<std::uint8_t>((value & 0xff) | 1));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
  } else {
    output->push_back(static_cast<std::uint8_t>(value));
  }
}

}  // namespace

std::uint32_t Builder::add_word(std::uint32_t word) {
  if (m_words.size() >= std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("GOAL data object has too many words.");
  }
  const auto result = static_cast<std::uint32_t>(m_words.size());
  m_words.push_back(word);
  return result;
}

std::uint32_t Builder::add_word_float(float value) {
  static_assert(sizeof(float) == sizeof(std::uint32_t));
  return add_word(std::bit_cast<std::uint32_t>(value));
}

std::uint32_t Builder::add_ref_to_string(std::string_view value) {
  const auto result = add_word(0);
  m_string_pool[std::string(value)].push_back(result);
  return result;
}

void Builder::add_type_tag(std::string_view value) {
  m_type_links[std::string(value)].push_back(add_word(0));
}

void Builder::add_symbol_link(std::string_view value) {
  m_symbol_links[std::string(value)].push_back(add_word(0));
}

void Builder::link_word_to_word(std::uint32_t source, std::uint32_t target) {
  if (source >= m_words.size() || target > std::numeric_limits<std::uint32_t>::max() / 4) {
    throw std::length_error("GOAL data object pointer link is out of range.");
  }
  m_pointer_links.push_back({source, target * 4});
}

std::uint32_t Builder::word_count() const {
  return static_cast<std::uint32_t>(m_words.size());
}

void Builder::align_words(std::uint32_t alignment) {
  while (m_words.size() % alignment) {
    add_word(0);
  }
}

void Builder::add_strings() {
  for (const auto& [value, sources] : m_string_pool) {
    align_words(4);
    add_type_tag("string");
    const auto target = add_word(static_cast<std::uint32_t>(value.size()));
    for (std::size_t offset = 0; offset <= value.size(); offset += 4) {
      std::uint32_t word = 0;
      for (std::size_t byte = 0; byte < 4 && offset + byte < value.size(); ++byte) {
        word |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(value[offset + byte]))
                << (byte * 8);
      }
      add_word(word);
    }
    for (const auto source : sources) {
      link_word_to_word(source, target);
    }
  }
}

std::vector<std::uint8_t> Builder::generate_link_table() {
  std::vector<std::uint8_t> link;
  std::sort(
      m_pointer_links.begin(), m_pointer_links.end(),
      [](const auto& left, const auto& right) { return left.source_word < right.source_word; });

  std::uint32_t previous_word = 0;
  for (std::size_t index = 0; index < m_pointer_links.size();) {
    const auto& entry = m_pointer_links[index];
    if (entry.source_word < previous_word) {
      throw std::logic_error("GOAL data object pointer links overlap.");
    }
    append_variable_length_integer(&link, entry.source_word - previous_word);
    m_words.at(entry.source_word) = entry.target_byte;
    previous_word = entry.source_word + 1;

    std::uint32_t consecutive = 1;
    while (index + consecutive < m_pointer_links.size() &&
           m_pointer_links[index + consecutive].source_word ==
               m_pointer_links[index + consecutive - 1].source_word + 1) {
      const auto& next = m_pointer_links[index + consecutive];
      m_words.at(next.source_word) = next.target_byte;
      previous_word = next.source_word + 1;
      ++consecutive;
    }
    append_variable_length_integer(&link, consecutive);
    index += consecutive;
  }
  append_variable_length_integer(&link, 0);

  const auto append_named_links = [&](const auto& links, bool type_links) {
    for (const auto& [name, locations] : links) {
      if (type_links) {
        link.push_back(0x80);
      }
      link.insert(link.end(), name.begin(), name.end());
      link.push_back(0);
      auto sorted_locations = locations;
      std::sort(sorted_locations.begin(), sorted_locations.end());
      std::uint32_t previous = 0;
      for (const auto location : sorted_locations) {
        if (location < previous || location > std::numeric_limits<std::uint32_t>::max() / 4) {
          throw std::logic_error("GOAL data object named links are invalid.");
        }
        append_better_variable_length_integer(&link, (location - previous) * 4);
        m_words.at(location) = 0xffffffff;
        previous = location;
      }
      link.push_back(0);
    }
  };
  append_named_links(m_symbol_links, false);
  append_named_links(m_type_links, true);
  append_variable_length_integer(&link, 0);
  while ((link.size() + 12) % 64) {
    link.push_back(0);
  }
  return link;
}

std::vector<std::uint8_t> Builder::generate_v2() {
  add_strings();
  auto link = generate_link_table();
  std::vector<std::uint8_t> output;
  append_u32(&output, 0xffffffff);
  append_u32(&output, static_cast<std::uint32_t>(12 + link.size()));
  append_u32(&output, 2);
  output.insert(output.end(), link.begin(), link.end());
  for (const auto word : m_words) {
    append_u32(&output, word);
  }
  while (output.size() % 16) {
    output.push_back(0);
  }
  return output;
}

std::vector<std::uint8_t> Builder::generate_v4() {
  add_strings();
  auto link = generate_link_table();
  if (m_words.size() > std::numeric_limits<std::uint32_t>::max() / 4) {
    throw std::length_error("GOAL data object is too large.");
  }
  const auto code_size = align16(static_cast<std::uint32_t>(m_words.size() * 4));
  std::vector<std::uint8_t> output;
  append_u32(&output, 0xffffffff);
  append_u32(&output, static_cast<std::uint32_t>(12 + link.size()));
  append_u32(&output, 4);
  append_u32(&output, code_size);
  for (const auto word : m_words) {
    append_u32(&output, word);
  }
  while (output.size() % 16) {
    output.push_back(0);
  }
  append_u32(&output, 0xffffffff);
  append_u32(&output, static_cast<std::uint32_t>(12 + link.size()));
  append_u32(&output, 2);
  output.insert(output.end(), link.begin(), link.end());
  return output;
}

}  // namespace goal_data_object_builder
