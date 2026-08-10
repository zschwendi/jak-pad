/*! Regression coverage for Jak 1's persistent translated sky vector registers. */
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/mips2c/mips2c_private.h"

u8* g_ee_main_mem = nullptr;
extern "C" u64 g_goal_current_process = 0;

[[noreturn]] void private_assert_failed(const char*, const char*, int, const char*, const char*) {
  std::abort();
}

namespace jak1 {
struct Symbol;
Ptr<Symbol> intern_from_c(const char*) {
  return {};
}
}  // namespace jak1

namespace Mips2C {
LinkedFunctionTable gLinkedFunctionTable;
void LinkedFunctionTable::reg(const std::string&, u64 (*)(void*), u32) {}
}  // namespace Mips2C

namespace Mips2C::jak1 {
extern ExecutionContext sky_regs_vfs;
namespace set_sky_vf27 {
u64 execute(void*);
}
namespace set_tex_offset {
u64 execute(void*);
}
}  // namespace Mips2C::jak1

namespace {
constexpr u32 kGiftagAddress = 0x1000;
constexpr u32 kStackTop = 0x3000;
constexpr std::array<u64, 2> kRoofGiftag = {0x302ec00000008001ull, 0x0000000000000412ull};

bool near(float actual, float expected) {
  return std::isfinite(actual) && std::fabs(actual - expected) <= 1e-6f;
}
}  // namespace

int main() {
  std::vector<u8> memory(0x4000);
  g_ee_main_mem = memory.data();
  std::memcpy(g_ee_main_mem + kGiftagAddress, kRoofGiftag.data(), sizeof(kRoofGiftag));

  Mips2C::jak1::sky_regs_vfs = {};

  Mips2C::ExecutionContext giftag_context{};
  giftag_context.gprs[Mips2C::a0].du64[0] = kGiftagAddress;
  Mips2C::jak1::set_sky_vf27::execute(&giftag_context);

  Mips2C::ExecutionContext texture_context{};
  texture_context.gprs[Mips2C::a0].du64[0] = 0x4000;
  texture_context.gprs[Mips2C::a1].du64[0] = static_cast<u64>(static_cast<s64>(-0x2000));
  texture_context.gprs[Mips2C::sp].du64[0] = kStackTop;
  Mips2C::jak1::set_tex_offset::execute(&texture_context);

  const auto& sky_registers = Mips2C::jak1::sky_regs_vfs;
  const bool giftag_preserved = sky_registers.vfs[Mips2C::vf27].du64[0] == kRoofGiftag[0] &&
                                 sky_registers.vfs[Mips2C::vf27].du64[1] == kRoofGiftag[1];
  const bool texture_offset_updated = near(sky_registers.vfs[Mips2C::vf24].f[0], 0.25f) &&
                                      near(sky_registers.vfs[Mips2C::vf24].f[1], -0.125f) &&
                                      near(sky_registers.vfs[Mips2C::vf24].f[2], 0.f) &&
                                      near(sky_registers.vfs[Mips2C::vf24].f[3], 0.f);

  std::printf("[%s] set-tex-offset preserves vf27\n", giftag_preserved ? "PASS" : "FAIL");
  std::printf("[%s] set-tex-offset updates vf24\n", texture_offset_updated ? "PASS" : "FAIL");
  return giftag_preserved && texture_offset_updated ? 0 : 1;
}
