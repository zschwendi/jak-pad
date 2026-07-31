#include "AOTWriter.h"

#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace aot {
namespace {

bool is_valid_c_symbol(const std::string& symbol) {
  if (symbol.empty() ||
      !(std::isalpha(static_cast<unsigned char>(symbol.front())) || symbol.front() == '_')) {
    return false;
  }

  for (const auto character : symbol) {
    if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_')) {
      return false;
    }
  }

  return true;
}

std::uint32_t read_little_endian_word(const std::vector<std::uint8_t>& code, size_t offset) {
  return static_cast<std::uint32_t>(code.at(offset)) |
         (static_cast<std::uint32_t>(code.at(offset + 1)) << 8) |
         (static_cast<std::uint32_t>(code.at(offset + 2)) << 16) |
         (static_cast<std::uint32_t>(code.at(offset + 3)) << 24);
}

void validate(const AppleArm64Function& function) {
  if (!is_valid_c_symbol(function.c_symbol)) {
    throw std::invalid_argument("AOT output requires a valid C symbol name");
  }
  if (function.code.empty() || function.code.size() % sizeof(std::uint32_t) != 0) {
    throw std::invalid_argument("AOT ARM64 code must contain whole, non-empty instruction words");
  }
}

}  // namespace

std::string render_apple_arm64_assembly(const AppleArm64Function& function) {
  validate(function);

  std::ostringstream output;
  output << ".section __TEXT,__text,regular,pure_instructions\n"
         << ".p2align 2\n"
         << ".globl _" << function.c_symbol << "\n"
         << "_" << function.c_symbol << ":\n";

  for (size_t offset = 0; offset < function.code.size(); offset += sizeof(std::uint32_t)) {
    output << "  .long 0x" << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
           << read_little_endian_word(function.code, offset) << "\n";
  }

  output << ".subsections_via_symbols\n";
  return output.str();
}

void write_apple_arm64_assembly(const std::string& output_path,
                                const AppleArm64Function& function) {
  const auto assembly = render_apple_arm64_assembly(function);
  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open AOT assembly output: " + output_path);
  }

  output << assembly;
  if (!output) {
    throw std::runtime_error("Unable to write AOT assembly output: " + output_path);
  }
}

}  // namespace aot
