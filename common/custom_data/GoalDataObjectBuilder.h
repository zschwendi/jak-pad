#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace goal_data_object_builder {

class Builder {
 public:
  std::uint32_t add_word(std::uint32_t word);
  std::uint32_t add_ref_to_string(std::string_view value);
  void add_type_tag(std::string_view value);
  void add_symbol_link(std::string_view value);
  void link_word_to_word(std::uint32_t source, std::uint32_t target);
  std::uint32_t word_count() const;

  std::vector<std::uint8_t> generate_v2();
  std::vector<std::uint8_t> generate_v4();

 private:
  struct PointerLink {
    std::uint32_t source_word = 0;
    std::uint32_t target_byte = 0;
  };

  void add_strings();
  std::vector<std::uint8_t> generate_link_table();
  void align_words(std::uint32_t alignment);

  std::map<std::string, std::vector<std::uint32_t>> m_string_pool;
  std::vector<std::uint32_t> m_words;
  std::vector<PointerLink> m_pointer_links;
  std::map<std::string, std::vector<std::uint32_t>> m_type_links;
  std::map<std::string, std::vector<std::uint32_t>> m_symbol_links;
};

}  // namespace goal_data_object_builder
