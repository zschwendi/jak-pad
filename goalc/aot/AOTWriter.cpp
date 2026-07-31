#include "AOTWriter.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
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

void validate(const NativeExport0& native_export) {
  const bool is_false_func = native_export.goal_name == "false-func" &&
                             native_export.c_symbol == "goalpad_aot_false_func";
  const bool is_true_func = native_export.goal_name == "true-func" &&
                            native_export.c_symbol == "goalpad_aot_true_func";
  if (!is_false_func && !is_true_func) {
    throw std::invalid_argument(
        "AOT native export proof only supports false-func or true-func zero-argument artifacts");
  }
}

void validate(const NativeExport1& native_export) {
  if (native_export.goal_name != "identity" || native_export.c_symbol != "goalpad_aot_identity") {
    throw std::invalid_argument(
        "AOT native export proof only supports identity as goalpad_aot_identity");
  }
}

std::filesystem::path normalized_path(const std::filesystem::path& path) {
  return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

std::filesystem::path make_auxiliary_path(
    const std::string& output_path,
    const std::string& purpose,
    const std::vector<std::filesystem::path>& reserved_paths) {
  for (size_t suffix = 0; suffix < 1000; suffix++) {
    auto candidate = std::filesystem::path(output_path);
    candidate += ".goalc-aot-" + purpose + "-" + std::to_string(suffix) + ".tmp";
    const auto normalized_candidate = normalized_path(candidate);
    const auto collides_with_reserved_path =
        std::any_of(reserved_paths.begin(), reserved_paths.end(), [&](const auto& reserved_path) {
          return normalized_candidate == normalized_path(reserved_path);
        });
    if (!collides_with_reserved_path && !std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error("Unable to reserve a temporary AOT output beside: " + output_path);
}

void write_text_output(const std::filesystem::path& output_path, const std::string& contents) {
  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Unable to open AOT output: " + output_path.string());
  }
  output << contents;
  if (!output) {
    throw std::runtime_error("Unable to write AOT output: " + output_path.string());
  }
}

void validate_output_destination(const std::string& output_path) {
  if (std::filesystem::exists(output_path) && !std::filesystem::is_regular_file(output_path)) {
    throw std::runtime_error("AOT output destination is not a regular file: " + output_path);
  }
}

void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const std::string& assembly,
                                     const std::string& native_export_source) {
  if (normalized_path(assembly_output_path) == normalized_path(export_output_path)) {
    throw std::invalid_argument("AOT assembly and export output paths must differ");
  }
  validate_output_destination(assembly_output_path);
  validate_output_destination(export_output_path);
  std::vector<std::filesystem::path> reserved_paths = {assembly_output_path, export_output_path};
  const auto assembly_staging_path =
      make_auxiliary_path(assembly_output_path, "new", reserved_paths);
  reserved_paths.push_back(assembly_staging_path);
  const auto export_staging_path = make_auxiliary_path(export_output_path, "new", reserved_paths);
  reserved_paths.push_back(export_staging_path);
  const auto assembly_backup_path =
      make_auxiliary_path(assembly_output_path, "backup", reserved_paths);
  reserved_paths.push_back(assembly_backup_path);
  const auto export_backup_path = make_auxiliary_path(export_output_path, "backup", reserved_paths);
  bool assembly_backup_created = false;
  bool export_backup_created = false;
  bool assembly_published = false;
  bool export_published = false;

  try {
    write_text_output(assembly_staging_path, assembly);
    write_text_output(export_staging_path, native_export_source);
    if (std::filesystem::exists(assembly_output_path)) {
      std::filesystem::rename(assembly_output_path, assembly_backup_path);
      assembly_backup_created = true;
    }
    if (std::filesystem::exists(export_output_path)) {
      std::filesystem::rename(export_output_path, export_backup_path);
      export_backup_created = true;
    }
    std::filesystem::rename(assembly_staging_path, assembly_output_path);
    assembly_published = true;
    std::filesystem::rename(export_staging_path, export_output_path);
    export_published = true;
  } catch (...) {
    std::error_code ignored_error;
    if (assembly_published) {
      std::filesystem::remove(assembly_output_path, ignored_error);
    }
    if (export_published) {
      std::filesystem::remove(export_output_path, ignored_error);
    }
    if (assembly_backup_created) {
      std::filesystem::rename(assembly_backup_path, assembly_output_path, ignored_error);
    }
    if (export_backup_created) {
      std::filesystem::rename(export_backup_path, export_output_path, ignored_error);
    }
    std::filesystem::remove(assembly_staging_path, ignored_error);
    std::filesystem::remove(export_staging_path, ignored_error);
    throw;
  }

  std::error_code ignored_error;
  if (assembly_backup_created) {
    std::filesystem::remove(assembly_backup_path, ignored_error);
  }
  if (export_backup_created) {
    std::filesystem::remove(export_backup_path, ignored_error);
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

std::string render_cpp_xmacro_export0(const NativeExport0& native_export) {
  validate(native_export);
  std::ostringstream output;
  output << "#ifndef OPENGOAL_AOT_EXPORT0\n"
         << "#error \"Define OPENGOAL_AOT_EXPORT0 before including this file.\"\n"
         << "#endif\n"
         << "OPENGOAL_AOT_EXPORT0(\"" << native_export.goal_name << "\", " << native_export.c_symbol
         << ")\n";

  return output.str();
}

std::string render_cpp_xmacro_export1(const NativeExport1& native_export) {
  validate(native_export);
  std::ostringstream output;
  output << "#ifndef OPENGOAL_AOT_EXPORT1\n"
         << "#error \"Define OPENGOAL_AOT_EXPORT1 before including this file.\"\n"
         << "#endif\n"
         << "OPENGOAL_AOT_EXPORT1(\"" << native_export.goal_name << "\", "
         << native_export.c_symbol << ")\n";

  return output.str();
}

void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport0& native_export) {
  const auto assembly = render_apple_arm64_assembly(function);
  const auto native_export_source = render_cpp_xmacro_export0(native_export);
  write_apple_arm64_artifact_pair(assembly_output_path, export_output_path, assembly,
                                  native_export_source);
}

void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport1& native_export) {
  const auto assembly = render_apple_arm64_assembly(function);
  const auto native_export_source = render_cpp_xmacro_export1(native_export);
  write_apple_arm64_artifact_pair(assembly_output_path, export_output_path, assembly,
                                  native_export_source);
}

}  // namespace aot
