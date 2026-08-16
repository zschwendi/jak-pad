#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace decompiler {

using LevelOutputValidator = std::function<bool(std::string_view)>;
using LevelOutputWriter = std::function<void(std::string_view)>;

namespace internal {

std::optional<std::string> validate_and_write_level_output(
    std::string_view level_name,
    const LevelOutputValidator& validator,
    const LevelOutputWriter& writer);

}  // namespace internal
}  // namespace decompiler
