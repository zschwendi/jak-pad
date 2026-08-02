/*! Regression coverage for Jak 1's translated parented joint transform. */
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

namespace Mips2C::jak1::cspace_parented_transformq_joint {
u64 execute(void*);
}

namespace {
constexpr uint32_t kChildCspace = 0x1000;
constexpr uint32_t kParentCspace = 0x1100;
constexpr uint32_t kChildBone = 0x1200;
constexpr uint32_t kParentBone = 0x1300;
constexpr uint32_t kTransformq = 0x1400;

using Vec = std::array<float, 4>;
using Matrix = std::array<Vec, 4>;

void store_u32(uint32_t address, uint32_t value) {
  std::memcpy(g_ee_main_mem + address, &value, sizeof(value));
}

void store_vec(uint32_t address, const Vec& value) {
  std::memcpy(g_ee_main_mem + address, value.data(), sizeof(value));
}

Vec load_vec(uint32_t address) {
  Vec value{};
  std::memcpy(value.data(), g_ee_main_mem + address, sizeof(value));
  return value;
}

bool near(float actual, float expected) {
  return std::isfinite(actual) && std::fabs(actual - expected) <= 2e-5f;
}

bool run_case(const char* name,
              const Matrix& parent,
              const Vec& parent_scale,
              const Vec& translation,
              const Vec& child_scale,
              const Matrix& expected) {
  std::memset(g_ee_main_mem, 0, 0x2000);
  store_u32(kChildCspace, kParentCspace);
  store_u32(kChildCspace + 16, kChildBone);
  store_u32(kParentCspace + 16, kParentBone);
  for (int column = 0; column < 4; column++) {
    store_vec(kParentBone + 16 * column, parent[column]);
  }
  store_vec(kParentBone + 64, parent_scale);
  store_vec(kTransformq, translation);
  store_vec(kTransformq + 16, {0, 0, 0, 1});
  store_vec(kTransformq + 32, child_scale);

  Mips2C::ExecutionContext context{};
  context.gprs[Mips2C::a0].du64[0] = kChildCspace;
  context.gprs[Mips2C::a1].du64[0] = kTransformq;
  Mips2C::jak1::cspace_parented_transformq_joint::execute(&context);

  bool passed = true;
  for (int column = 0; column < 4; column++) {
    const auto actual = load_vec(kChildBone + 16 * column);
    for (int lane = 0; lane < 4; lane++) {
      passed &= near(actual[lane], expected[column][lane]);
    }
  }
  std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", name);
  return passed;
}
}  // namespace

int main() {
  std::vector<u8> memory(0x2000);
  g_ee_main_mem = memory.data();

  bool passed = true;
  passed &= run_case(
      "negative parent scale does not poison the homogeneous lane",
      {{{2, 0, 0, 0}, {0, 0.5f, 0, 0}, {0, 0, -4, 0}, {10, 20, 30, 1}}},
      {2, 0.5f, -4, 1}, {1, 6, 3, 1}, {1.25f, 0.75f, 1.5f, 1},
      {{{1.25f, 0, 0, 0}, {0, 0.75f, 0, 0}, {0, 0, 1.5f, 0}, {12, 23, 18, 1}}});

  passed &= run_case(
      "positive attack-scale range remains unmodified",
      {{{0.900391f, 0, 0, 0}, {0, 3.86328f, 0, 0}, {0, 0, 5.81348f, 0}, {0, 0, 0, 1}}},
      {0.900391f, 3.86328f, 5.81348f, 1}, {0, 0, 0, 1},
      {0.591797f, 3.86328f, 5.81348f, 1},
      {{{0.591797f, 0, 0, 0}, {0, 3.86328f, 0, 0}, {0, 0, 5.81348f, 0}, {0, 0, 0, 1}}});

  return passed ? 0 : 1;
}
