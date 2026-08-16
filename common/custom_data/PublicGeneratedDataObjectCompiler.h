#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

class GameSubtitleBank;
class GameTextBank;

namespace public_generated_data_object_compiler {

std::vector<std::uint8_t> build_directory_tpages(std::span<const std::uint32_t> lengths);
std::vector<std::uint8_t> build_game_text(std::string_view group_name, const GameTextBank& bank);
std::vector<std::uint8_t> build_subtitle_v2(const GameSubtitleBank& bank);

}  // namespace public_generated_data_object_compiler
