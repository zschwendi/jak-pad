#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aot {

struct AppleArm64Function {
  std::string c_symbol;
  std::vector<std::uint8_t> code;
};

struct AppleArm64LoadStateValueFunction {
  std::string c_symbol;
};

struct NativeExport0 {
  std::string goal_name;
  std::string c_symbol;
};

struct NativeExport1 {
  std::string goal_name;
  std::string c_symbol;
};

struct NativeExport2 {
  std::string goal_name;
  std::string c_symbol;
};

struct NativeExport3 {
  std::string goal_name;
  std::string c_symbol;
};

struct SymbolValueOffset32 {
  std::string game_name;
  std::string goal_name;
  std::string value_type;
  std::string c_symbol;
};

std::string render_apple_arm64_assembly(const AppleArm64Function& function);
std::string render_apple_arm64_load_state_value_assembly(
    const AppleArm64LoadStateValueFunction& function);
void write_apple_arm64_assembly(const std::string& output_path, const AppleArm64Function& function);
std::string render_cpp_xmacro_export0(const NativeExport0& native_export);
std::string render_cpp_xmacro_export1(const NativeExport1& native_export);
std::string render_cpp_xmacro_export2(const NativeExport2& native_export);
std::string render_cpp_xmacro_export3(const NativeExport3& native_export);
std::string render_cpp_xmacro_symbol_value_offset32(const SymbolValueOffset32& offset);
void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport0& native_export);
void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport1& native_export);
void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport2& native_export);
void write_apple_arm64_artifact_pair(const std::string& assembly_output_path,
                                     const std::string& export_output_path,
                                     const AppleArm64Function& function,
                                     const NativeExport3& native_export);
void write_apple_arm64_load_state_value_artifact(
    const std::string& assembly_output_path,
    const std::string& export_output_path,
    const std::string& data_output_path,
    const AppleArm64LoadStateValueFunction& function,
    const NativeExport0& native_export,
    const SymbolValueOffset32& offset);

}  // namespace aot
