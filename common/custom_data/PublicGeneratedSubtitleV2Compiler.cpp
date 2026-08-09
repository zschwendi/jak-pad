#include <limits>
#include <stdexcept>

#include "PublicGeneratedDataObjectCompiler.h"

#include "common/custom_data/GoalDataObjectBuilder.h"
#include "common/serialization/subtitles/subtitles_v2.h"
#include "common/util/font/font_utils.h"

namespace public_generated_data_object_compiler {
namespace {

std::uint32_t checked_size(std::size_t size, std::string_view description) {
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error(std::string(description) + " exceeds the GOAL object limit.");
  }
  return static_cast<std::uint32_t>(size);
}

}  // namespace

std::vector<std::uint8_t> build_subtitle_v2(const GameSubtitleBank& bank) {
  const auto font = get_font_bank(bank.m_text_version);
  goal_data_object_builder::Builder builder;
  builder.add_type_tag(get_text_version_name(bank.m_text_version) == "jak3"
                           ? "subtitle3-text-info"
                           : "subtitle2-text-info");
  builder.add_word((checked_size(bank.m_scenes.size(), "Subtitle scene list") & 0xffffu) |
                   (1u << 16));
  builder.add_word((static_cast<std::uint32_t>(bank.m_lang_id) & 0xffffu) |
                   ((checked_size(bank.m_speakers.size(), "Subtitle speaker list") + 1u) << 16));
  const auto speaker_array_link = builder.add_word(0);

  std::vector<std::uint32_t> line_array_links;
  line_array_links.reserve(bank.m_scenes.size());
  for (const auto& [name, scene] : bank.m_scenes) {
    builder.add_ref_to_string(name);
    builder.add_word(checked_size(scene.m_lines.size(), "Subtitle scene line list"));
    line_array_links.push_back(builder.add_word(0));
  }

  std::size_t scene_index = 0;
  for (const auto& [name, scene] : bank.m_scenes) {
    (void)name;
    builder.link_word_to_word(line_array_links.at(scene_index++), builder.word_count());
    for (const auto& line : scene.m_lines) {
      builder.add_word_float(static_cast<float>(line.metadata.frame_start));
      builder.add_word_float(static_cast<float>(line.metadata.frame_end));
      if (line.metadata.merge) {
        builder.add_symbol_link("#f");
      } else if (font->is_language_id_korean(bank.m_lang_id)) {
        builder.add_ref_to_string(font->convert_utf8_to_game_korean(line.text));
      } else {
        builder.add_ref_to_string(font->convert_utf8_to_game(line.text));
      }
      const auto speaker = bank.speaker_enum_value_from_name(line.metadata.speaker);
      std::uint16_t flags = 0;
      flags |= static_cast<std::uint16_t>(line.metadata.offscreen) << 0;
      flags |= static_cast<std::uint16_t>(line.metadata.merge) << 1;
      builder.add_word(static_cast<std::uint32_t>(speaker) |
                       (static_cast<std::uint32_t>(flags) << 16));
    }
  }

  builder.link_word_to_word(speaker_array_link, builder.word_count());
  for (const auto& localized : bank.speaker_names_ordered_by_enum_value()) {
    if (font->is_language_id_korean(bank.m_lang_id)) {
      builder.add_ref_to_string(font->convert_utf8_to_game_korean(localized));
    } else {
      builder.add_ref_to_string(font->convert_utf8_to_game(localized));
    }
  }
  return builder.generate_v2();
}

}  // namespace public_generated_data_object_compiler
