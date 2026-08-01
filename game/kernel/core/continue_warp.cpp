/*!
 * @file continue_warp.cpp
 * See continue_warp.h. The lookup is `(method get-continue-by-name game-info)` walking
 * `*level-load-list*`, done here in C so no GOAL string has to be built to ask for it.
 *
 * The field offsets are `level-load-info`'s and `continue-point`'s own, from
 * `engine/level/level-h.gc` and `engine/game/game-info-h.gc`, less the 4 bytes of basic type tag
 * that a GOAL pointer to a basic object is past.
 */

#include "game/kernel/core/continue_warp.h"

#include <cstring>
#include <string>

#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

constexpr uint32_t kLevelLoadInfoContinues = 56 - 4;
constexpr uint32_t kContinueName = 4 - 4;
constexpr uint32_t kContinueLevel = 8 - 4;
constexpr uint32_t kContinueVisNick = 104 - 4;
constexpr uint32_t kContinueLev0 = 108 - 4;
constexpr uint32_t kContinueLev1 = 116 - 4;
constexpr uint32_t kStringData = 4;  //! `string` holds an allocated length, then the characters

uint32_t read_u32(uint32_t address) {
  uint32_t value = 0;
  if (address && address + 4 <= (uint32_t)EE_MAIN_MEM_SIZE) {
    std::memcpy(&value, (uint8_t*)g_ee_main_mem + address, sizeof(value));
  }
  return value;
}

const char* name_of_symbol(uint32_t symbol) {
  static uint32_t symbol_type = 0;
  if (!symbol_type) {
    auto type = jak1::find_symbol_from_c("symbol");
    symbol_type = type.offset ? type->value : 0;
  }
  if (!symbol || symbol < 4 || read_u32(symbol - 4) != symbol_type) {
    return "";
  }
  return jak1::info(Ptr<jak1::Symbol>(symbol))->str->data();
}

uint32_t g_point = 0;

/*!
 * `(start 'play <point>)`, as a GOAL function of no arguments.
 *
 * `start` spawns the target, and a process spawn stores its catch-frame's address in a 32-bit
 * field, so it has to run on a stack inside GOAL memory (docs/aot-stack-model.md). The way onto
 * that stack, `call_goal_on_stack`, passes no arguments - so this stands between: it is reached
 * with none, on GOAL's own stack, and calls `start` from there with both.
 */
u64 start_on_goal_stack() {
  auto start = jak1::find_symbol_from_c("start");
  return call_goal(Ptr<Function>(start->value), jak1::intern_from_c("play").offset, g_point, 0,
                   s7.offset, g_ee_main_mem);
}

}  // namespace

unsigned int goal_find_continue_point(const char* name) {
  if (!name) {
    return 0;
  }
  auto list = jak1::find_symbol_from_c("*level-load-list*");
  if (!list.offset) {
    return 0;
  }
  // `'()` is the empty pair, which lives in the symbol table and is not `#f`; walking off the end
  // of a list into it and reading its cdr would go round forever.
  const uint32_t empty = jak1::find_symbol_from_c("_empty_").offset;
  uint32_t levels = list->value;
  while (levels && levels != s7.offset && levels != empty) {
    const uint32_t info = read_u32(read_u32(levels - 2));  // (-> (the-as symbol (car list)) value)
    uint32_t continues = info ? read_u32(info + kLevelLoadInfoContinues) : 0;
    while (continues && continues != s7.offset && continues != empty) {
      const uint32_t point = read_u32(continues - 2);
      const uint32_t text = read_u32(point + kContinueName);
      if (text && text != s7.offset &&
          !std::strcmp(name, (const char*)((uint8_t*)g_ee_main_mem + text + kStringData))) {
        return point;
      }
      continues = read_u32(continues + 2);
    }
    levels = read_u32(levels + 2);
  }
  return 0;
}

int goal_continue_point_describe(const char* name, goal_continue_point_info* out) {
  const uint32_t point = goal_find_continue_point(name);
  if (!point || !out) {
    return 0;
  }
  out->level = name_of_symbol(read_u32(point + kContinueLevel));
  out->want0 = name_of_symbol(read_u32(point + kContinueLev0));
  out->want1 = name_of_symbol(read_u32(point + kContinueLev1));
  out->vis_nick = name_of_symbol(read_u32(point + kContinueVisNick));
  return 1;
}

int goal_warp_to_continue(const char* name) {
  g_point = goal_find_continue_point(name);
  if (!g_point) {
    return 0;
  }
  auto start = jak1::find_symbol_from_c("start");
  if (!start.offset || !start->value) {
    return 0;
  }
  call_goal_on_stack(jak1::make_function_from_native((void*)start_on_goal_stack),
                     goal_kernel_stack_top(), s7.offset, g_ee_main_mem);
  return 1;
}
