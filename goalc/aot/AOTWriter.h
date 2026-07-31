#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aot {

struct AppleArm64Function {
  std::string c_symbol;
  std::vector<std::uint8_t> code;
};

std::string render_apple_arm64_assembly(const AppleArm64Function& function);
void write_apple_arm64_assembly(const std::string& output_path,
                                const AppleArm64Function& function);

}  // namespace aot
