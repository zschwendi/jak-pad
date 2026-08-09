#include "PublicGeneratedDataObjectCompiler.h"

#include <limits>
#include <stdexcept>

#include "common/custom_data/GoalDataObjectBuilder.h"
#include "common/serialization/text/text_ser.h"

namespace public_generated_data_object_compiler {
namespace {

using goal_data_object_builder::Builder;

std::uint32_t checked_size(std::size_t size, std::string_view description) {
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error(std::string(description) + " exceeds the GOAL object limit.");
  }
  return static_cast<std::uint32_t>(size);
}

}  // namespace

std::vector<std::uint8_t> build_directory_tpages(std::span<const std::uint32_t> lengths) {
  Builder builder;
  builder.add_type_tag("texture-page-dir");
  builder.add_word(checked_size(lengths.size(), "Texture-page directory"));
  for (const auto length : lengths) {
    builder.add_word(length);
    builder.add_symbol_link("#f");
    builder.add_symbol_link("#f");
  }
  return builder.generate_v4();
}

std::vector<std::uint8_t> build_game_text(std::string_view group_name, const GameTextBank& bank) {
  Builder builder;
  builder.add_type_tag("game-text-info");
  builder.add_word(checked_size(bank.lines().size(), "Game-text bank"));
  builder.add_word(static_cast<std::uint32_t>(bank.lang()));
  builder.add_ref_to_string(group_name);
  for (const auto& [id, line] : bank.lines()) {
    builder.add_word(static_cast<std::uint32_t>(id));
    builder.add_ref_to_string(line);
  }
  return builder.generate_v2();
}

}  // namespace public_generated_data_object_compiler
