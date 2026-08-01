/*!
 * @file goalpad_play.cpp
 * The host that puts the two halves together: the portable Jak 1 kernel runs the game, and the
 * Metal renderer draws what it builds.
 *
 * The structure is upstream's, from `game/runtime.cpp` plus `Gfx::Loop`. The GOAL kernel runs on
 * its own thread and calls `__send-gfx-dma-chain` and `syncv` from there; the main thread owns the
 * window and pumps the renderer, consuming one chain per frame and presenting it. The two meet
 * only at `GfxRendererModule`, through the C seam in `game/kernel/core/gfx_host.h` - the kernel
 * library itself still knows nothing about SDL, Metal or a window, which is what keeps it usable
 * from an iPadOS application.
 *
 * What is different from upstream: there is no IOP thread (the DGO, STR, ramdisk and sound RPCs
 * are answered where they are sent, see `dgo_loader.cpp` and `sound_rpc.cpp`), no DECI2 listener,
 * and no runtime code generation - every GOAL function was compiled ahead of time.
 *
 * The player's own extracted data is read from the directory given by `--data-dir`; no game data
 * ships with this and none is written anywhere but where the caller asks.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/pipelines/metal/metal_pipeline.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/continue_warp.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/pad.h"
#include "game/kernel/core/scripted_walk.h"
#include "game/kernel/core/sound_rpc.h"
#include "game/kernel/jak1/klisten.h"
#include "game/goalpad_audio.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

#include "third-party/SDL/include/SDL3/SDL.h"

namespace {

// ---------------------------------------------------------------------------------------------
// options
// ---------------------------------------------------------------------------------------------

struct Options {
  std::string data_dir;
  std::string saves_dir;  // set from data_dir unless --saves-dir overrides it
  int window_w = 1280;
  int window_h = 960;
  // The internal resolution the game renders at, as a multiple of the PS2's 640x480. Whole
  // multiples keep the 4:3 aspect and the HUD proportions the game computes for 640x480. 2x is
  // the default, matching the iPad app shell.
  int render_scale = 2;
  int game_res_w = 0;  // set from render_scale unless --game-res overrides it
  int game_res_h = 0;
  int exit_after_frames = 0;  // 0 = run until the window is closed
  bool sound = true;
  bool report_state = false;
  bool report_levels = false;
  bool report_pad = false;
  bool present_pacing = true;
  // frame -> file. Written from the offscreen game target, which is exactly what the present pass
  // shows.
  std::map<int, std::string> screenshots;
  // where the capture hotkeys write their .gpdma files. Outside any checkout, and never where the
  // player's own game data lives.
  std::string capture_dir = "/private/tmp/goalpad-dma-capture";
  int capture_burst = 8;             // frames one burst capture writes
  std::vector<int> capture_frames;   // chain indices to capture without a keypress
  // scripted input, so a run can be reproduced without a person at the keyboard. Same syntax as
  // jak1-data-boot-test's --press: buttons joined with '+', at a frame number.
  struct Press {
    int first_frame = 0;   // absolute, or an offset from `after_state` when that is set
    int hold_frames = 6;
    uint32_t buttons = 0;  // 0 for a stick hold
    bool is_stick = false;
    uint8_t stick_x = GOAL_PAD_ANALOG_NEUTRAL;
    uint8_t stick_y = GOAL_PAD_ANALOG_NEUTRAL;
    std::string after_state;
  };
  std::vector<Press> presses;
  // Start the game at one of its own continue points, the way a warp gate does, so a run can be
  // in a level without playing the whole game to it. See kernel/core/continue_warp.h.
  struct Warp {
    std::string name;
    int first_frame = 0;
    std::string after_state;
  };
  std::vector<Warp> warps;
};

const struct {
  const char* name;
  uint32_t bit;
} kPadButtonNames[] = {
    {"select", GOAL_PAD_SELECT},     {"l3", GOAL_PAD_L3},
    {"r3", GOAL_PAD_R3},             {"start", GOAL_PAD_START},
    {"up", GOAL_PAD_UP},             {"right", GOAL_PAD_RIGHT},
    {"down", GOAL_PAD_DOWN},         {"left", GOAL_PAD_LEFT},
    {"l2", GOAL_PAD_L2},             {"r2", GOAL_PAD_R2},
    {"l1", GOAL_PAD_L1},             {"r1", GOAL_PAD_R1},
    {"triangle", GOAL_PAD_TRIANGLE}, {"circle", GOAL_PAD_CIRCLE},
    {"x", GOAL_PAD_X},               {"square", GOAL_PAD_SQUARE},
};

bool parse_buttons(const std::string& names, uint32_t* out) {
  uint32_t bits = 0;
  size_t at = 0;
  while (at <= names.size()) {
    const size_t plus = names.find('+', at);
    const std::string one = names.substr(at, plus == std::string::npos ? plus : plus - at);
    bool found = false;
    for (const auto& entry : kPadButtonNames) {
      if (one == entry.name) {
        bits |= entry.bit;
        found = true;
        break;
      }
    }
    if (!found) {
      lg::error("unknown pad button '{}'", one);
      return false;
    }
    if (plus == std::string::npos) {
      break;
    }
    at = plus + 1;
  }
  *out = bits;
  return true;
}

// ---------------------------------------------------------------------------------------------
// what the two threads share
// ---------------------------------------------------------------------------------------------

struct Shared {
  std::atomic<bool> want_exit{false};
  std::atomic<bool> game_running{false};
  std::atomic<int> game_frames{0};

  // the controller, read on the main thread and consumed by the game thread
  std::mutex pad_mutex;
  goal_pad_state pad;

  // Capture requests: the hotkey is seen on the main thread, but the chain only exists on the
  // game thread, inside send_chain. The counter is how one hands the request to the other.
  std::atomic<int> captures_wanted{0};
};
Shared g_shared;

// ---------------------------------------------------------------------------------------------
// the renderer seam: the C hooks the kernel calls, forwarded to the Metal module
// ---------------------------------------------------------------------------------------------

const GfxRendererModule* g_gfx = nullptr;
std::string g_capture_dir;
int g_chain_index = 0;  // 1-based, counts chains: what a capture file calls its frame
// chain indices asked for on the command line, so a run can be reproduced without a keypress.
// Written before the game thread starts and only read after.
std::set<int> g_capture_frames;

/*!
 * Write the chain this frame built, before it is handed to the renderer, so the file holds exactly
 * what the player is looking at. Called on the game thread; a capture stalls the frame for as long
 * as the write takes, which is the intent - a frame worth keeping is worth waiting for.
 */
void capture_this_chain(const void* ee_base, uint32_t chain_offset) {
  const std::string path =
      fmt::format("{}/dma-frame-{}.gpdma", g_capture_dir, g_chain_index);
  if (goal_gfx_dma_capture_chain_now(ee_base, chain_offset, g_chain_index, path.c_str())) {
    // What the renderer made of the *previous* frame, which is the closest reading of this one
    // available before it is drawn: it says which renderer the character geometry went through.
    const auto chain = metal_renderer::get_chain_stats();
    lg::warn("[capture] wrote {}", path);
    lg::warn("[capture]   last frame's foreground: merc {} models / {} draws / {} tris, "
             "generic {} draws / {} tris",
             chain.merc_models, chain.merc_draws, chain.merc_triangles, chain.generic_draws,
             chain.generic_triangles);
  }
}

void host_send_chain(const void* ee_base, uint32_t chain_offset) {
  g_chain_index++;
  if (!g_capture_dir.empty()) {
    bool wanted = g_capture_frames.count(g_chain_index) != 0;
    if (!wanted && g_shared.captures_wanted.load() > 0) {
      g_shared.captures_wanted--;
      wanted = true;
    }
    if (wanted) {
      capture_this_chain(ee_base, chain_offset);
    }
  }
  g_gfx->send_chain(ee_base, chain_offset);
}
uint32_t host_vsync(void) {
  return g_gfx->vsync();
}
uint32_t host_sync_path(void) {
  return g_gfx->sync_path();
}
void host_texture_upload_now(const uint8_t* tpage, int mode, uint32_t s7_ptr) {
  g_gfx->texture_upload_now(tpage, mode, s7_ptr);
}
void host_texture_relocate(uint32_t dst, uint32_t src, uint32_t format) {
  g_gfx->texture_relocate(dst, src, format);
}
void host_set_levels(const char* const* names, int count) {
  std::vector<std::string> levels;
  levels.reserve(count);
  for (int i = 0; i < count; i++) {
    levels.emplace_back(names[i]);
  }
  g_gfx->set_levels(levels);
}
void host_set_pmode_alp(float alp) {
  g_gfx->set_pmode_alp(alp);
}

// ---------------------------------------------------------------------------------------------
// input: read straight from SDL, pushed into the kernel's pad seam
//
// SDL's keyboard and gamepad *state* is what is read, not its events, because a held key is a
// state and there is no event for "still held". The event queue is drained by the display, which
// is what keeps that state current.
//
// The keyboard layout is the PC port's own default (game/system/hid/input_bindings.cpp), and the
// gamepad is SDL's standard mapping laid onto the PS2 face buttons.
// ---------------------------------------------------------------------------------------------

SDL_Gamepad* g_gamepad = nullptr;
SDL_JoystickID g_gamepad_id = 0;

/*!
 * SDL initializes only what it is asked for: the Metal pipeline starts the video subsystem, so
 * without this the gamepad list is always empty and a connected controller is never found.
 */
bool start_gamepads() {
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    lg::error("could not initialize SDL gamepads: {}", SDL_GetError());
    return false;
  }
  return true;
}

/*!
 * Open the first connected controller, and notice when it comes or goes. The display drains the
 * SDL event queue every frame, which is what keeps SDL's device list current, so polling it here
 * handles hot-plug without this file having to see the events.
 */
void refresh_gamepad() {
  if (g_gamepad && !SDL_GamepadConnected(g_gamepad)) {
    lg::info("[pad] controller disconnected");
    SDL_CloseGamepad(g_gamepad);
    g_gamepad = nullptr;
    g_gamepad_id = 0;
  }
  if (g_gamepad) {
    return;
  }
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids) {
    if (count > 0) {
      g_gamepad = SDL_OpenGamepad(ids[0]);
      if (g_gamepad) {
        g_gamepad_id = ids[0];
        lg::info("[pad] controller connected: {}", SDL_GetGamepadName(g_gamepad));
      }
    }
    SDL_free(ids);
  }
}

/*! One axis of a stick, as the byte the game reads (0 to 255, 127 centered). */
u8 axis_byte(bool negative, bool positive, s16 analog) {
  if (negative || positive) {
    return negative ? 0 : 255;
  }
  // SDL reports -32768..32767; the PS2 pad reported a byte.
  return (u8)std::clamp((analog + 32768) / 257, 0, 255);
}

void read_pad(const Options& opts) {
  refresh_gamepad();
  const bool* keys = SDL_GetKeyboardState(nullptr);

  goal_pad_state pad;
  goal_pad_state_neutral(&pad);
  auto key = [&](SDL_Scancode code) { return keys && keys[code]; };
  auto button = [&](SDL_GamepadButton b) {
    return g_gamepad && SDL_GetGamepadButton(g_gamepad, b);
  };
  auto axis = [&](SDL_GamepadAxis a) -> s16 {
    return g_gamepad ? SDL_GetGamepadAxis(g_gamepad, a) : 0;
  };

  struct Bind {
    uint32_t bit;
    SDL_Scancode key;
    SDL_GamepadButton pad_button;
  };
  static const Bind kBinds[] = {
      {GOAL_PAD_START, SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START},
      {GOAL_PAD_SELECT, SDL_SCANCODE_APOSTROPHE, SDL_GAMEPAD_BUTTON_BACK},
      {GOAL_PAD_X, SDL_SCANCODE_SPACE, SDL_GAMEPAD_BUTTON_SOUTH},
      {GOAL_PAD_CIRCLE, SDL_SCANCODE_E, SDL_GAMEPAD_BUTTON_EAST},
      {GOAL_PAD_SQUARE, SDL_SCANCODE_F, SDL_GAMEPAD_BUTTON_WEST},
      {GOAL_PAD_TRIANGLE, SDL_SCANCODE_R, SDL_GAMEPAD_BUTTON_NORTH},
      {GOAL_PAD_L1, SDL_SCANCODE_Q, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
      {GOAL_PAD_R1, SDL_SCANCODE_O, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
      {GOAL_PAD_L3, SDL_SCANCODE_COMMA, SDL_GAMEPAD_BUTTON_LEFT_STICK},
      {GOAL_PAD_R3, SDL_SCANCODE_PERIOD, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
      {GOAL_PAD_UP, SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
      {GOAL_PAD_DOWN, SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
      {GOAL_PAD_LEFT, SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
      {GOAL_PAD_RIGHT, SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
  };
  for (const auto& bind : kBinds) {
    if (key(bind.key) || button(bind.pad_button)) {
      pad.buttons |= bind.bit;
    }
  }
  // the triggers are analog on a gamepad and keys on a keyboard
  if (key(SDL_SCANCODE_1) || axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192) {
    pad.buttons |= GOAL_PAD_L2;
  }
  if (key(SDL_SCANCODE_P) || axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) {
    pad.buttons |= GOAL_PAD_R2;
  }

  pad.left_x = axis_byte(key(SDL_SCANCODE_A), key(SDL_SCANCODE_D), axis(SDL_GAMEPAD_AXIS_LEFTX));
  pad.left_y = axis_byte(key(SDL_SCANCODE_W), key(SDL_SCANCODE_S), axis(SDL_GAMEPAD_AXIS_LEFTY));
  pad.right_x = axis_byte(key(SDL_SCANCODE_L), key(SDL_SCANCODE_J), axis(SDL_GAMEPAD_AXIS_RIGHTX));
  pad.right_y = axis_byte(key(SDL_SCANCODE_I), key(SDL_SCANCODE_K), axis(SDL_GAMEPAD_AXIS_RIGHTY));

  // Capture hotkeys. F9 asks for this frame, F10 for a burst - a thing that goes wrong *while*
  // the character turns needs several frames to show the whole of it, not one. Both are on the
  // key's edge, so holding the key does not queue hundreds of files.
  {
    static bool f9_was_down = false;
    static bool f10_was_down = false;
    const bool f9 = key(SDL_SCANCODE_F9);
    const bool f10 = key(SDL_SCANCODE_F10);
    if (f9 && !f9_was_down) {
      g_shared.captures_wanted += 1;
      lg::warn("[capture] F9: capturing the next frame into {}", g_capture_dir);
    }
    if (f10 && !f10_was_down) {
      g_shared.captures_wanted += opts.capture_burst;
      lg::warn("[capture] F10: capturing the next {} frames into {}", opts.capture_burst,
               g_capture_dir);
    }
    f9_was_down = f9;
    f10_was_down = f10;
  }

  if (opts.report_pad) {
    static goal_pad_state last;
    static bool have_last = false;
    if (!have_last || pad.buttons != last.buttons || pad.left_x != last.left_x ||
        pad.left_y != last.left_y || pad.right_x != last.right_x || pad.right_y != last.right_y) {
      std::string names;
      for (const auto& entry : kPadButtonNames) {
        if (pad.buttons & entry.bit) {
          names += " ";
          names += entry.name;
        }
      }
      lg::info("[pad] buttons{} left ({},{}) right ({},{})", names.empty() ? " -" : names,
               pad.left_x, pad.left_y, pad.right_x, pad.right_y);
      last = pad;
      have_last = true;
    }
  }

  {
    std::lock_guard<std::mutex> lock(g_shared.pad_mutex);
    g_shared.pad = pad;
  }

  // What GOAL asked the motors to do, back out to the controller. Same argument order as
  // game/sce/libpad.cpp's scePadSetActDirect: byte 0 is the on/off large motor, byte 1 the
  // analog small one.
  if (g_gamepad) {
    uint8_t large = 0;
    uint8_t small = 0;
    goal_pad_get_rumble(0, &large, &small);
    static uint8_t last_large = 0;
    static uint8_t last_small = 0;
    if (large != last_large || small != last_small) {
      // SDL takes two 16-bit intensities. The PS2's large motor was on/off and its small one
      // analog, which is the pair game/sce/libpad.cpp hands the PC port.
      SDL_RumbleGamepad(g_gamepad, (u16)(small * 257), large ? 0xFFFF : 0, 1000);
      if (opts.report_pad && (large || small)) {
        lg::info("[pad] rumble: large {} small {}", large, small);
      }
      last_large = large;
      last_small = small;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// what the game is doing, read out of the real heap
//
// The same reader jak1-data-boot-test uses: the state `*target*` is running names what the game
// is doing, so an input that lands changes it. A scripted press can be scheduled off a state
// rather than off a frame number, which makes a run reproducible even though the level loads run
// off the wall clock.
// ---------------------------------------------------------------------------------------------

uint32_t goal_u32(uint32_t address) {
  uint32_t value = 0;
  if (address && address + 4 <= (uint32_t)EE_MAIN_MEM_SIZE) {
    std::memcpy(&value, (uint8_t*)g_ee_main_mem + address, sizeof(value));
  }
  return value;
}

const char* symbol_name(uint32_t symbol) {
  static uint32_t symbol_type = 0;
  if (!symbol_type) {
    auto type = jak1::find_symbol_from_c("symbol");
    symbol_type = type.offset ? type->value : 0;
  }
  if (!symbol || symbol < 4 || goal_u32(symbol - 4) != symbol_type) {
    return "";
  }
  return jak1::info(Ptr<jak1::Symbol>(symbol))->str->data();
}

/*! The state a process is running, by name. `state` is at offset 52 and its name at offset 0. */
const char* process_state_name(uint32_t process) {
  if (!process || process == s7.offset) {
    return "";
  }
  return symbol_name(goal_u32(goal_u32(process + 52)));
}

uint32_t symbol_value(const char* name) {
  auto symbol = jak1::find_symbol_from_c(name);
  return symbol.offset ? symbol->value : 0;
}

/*! Where the target is standing, in metres. See jak1-data-boot-test for the two offsets. */
constexpr uint32_t kProcessDrawableRootOffset = 112 - 4;
constexpr uint32_t kTrsTransOffset = 16 - 4;

struct GameState {
  std::string master_mode;
  std::string target_state;
  bool position_known = false;
  float x = 0, y = 0, z = 0;
};

GameState read_game_state() {
  GameState out;
  out.master_mode = symbol_name(symbol_value("*master-mode*"));
  const uint32_t target = symbol_value("*target*");
  if (target && target != s7.offset) {
    out.target_state = process_state_name(target);
    const uint32_t root = goal_u32(target + kProcessDrawableRootOffset);
    if (root && root != s7.offset) {
      const uint32_t trans = root + kTrsTransOffset;
      float xyz[3];
      std::memcpy(xyz, (uint8_t*)g_ee_main_mem + trans, sizeof(xyz));
      out.position_known = true;
      out.x = xyz[0] / 4096.f;
      out.y = xyz[1] / 4096.f;
      out.z = xyz[2] / 4096.f;
    }
  }
  return out;
}

// frame each target state was first entered, so `@state+offset` can be resolved
std::vector<std::pair<std::string, int>> g_state_first_frame;

int frame_state_was_first_seen(const std::string& state) {
  for (const auto& entry : g_state_first_frame) {
    if (entry.first == state) {
      return entry.second;
    }
  }
  return -1;
}

// the walk autopilot the headless test drives routes with, fed from the same state reader
WalkScript g_walk;

void install_walk_callbacks() {
  g_walk.state_first_seen = frame_state_was_first_seen;
  g_walk.log = [](const char* line) { lg::info("[game] {}", line); };
}

// ---------------------------------------------------------------------------------------------
// the game thread: boot, then the GOAL kernel's own frame loop
// ---------------------------------------------------------------------------------------------

std::string object_name_of(const char* source) {
  std::string path = source;
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    path = path.substr(slash + 1);
  }
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    path = path.substr(0, dot);
  }
  return path;
}

/*! Tell the DGO loader which native translation unit stands in for each object it will meet. */
void register_aot_objects() {
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,      entry.statics,         *entry.static_count,
                                 entry.functions, *entry.function_count, entry.link};
    goal_aot_register_object(object_name_of(entry.source).c_str(), &file);
  }
}

/*!
 * GAME.CGO is the release build's union of ENGINE.CGO, ART.CGO and COMMON.CGO, which do not exist
 * as separate files on the disc. The C kernel records that by putting their names in
 * `*kernel-packages*` after loading GAME.CGO (jak1::InitMachineScheme in
 * game/kernel/jak1/kmachine.cpp), so `alloc-levels!` finds them already loaded.
 */
void record_packages_in_game_cgo() {
  using namespace jak1_symbols;
  for (const char* package : {"engine", "art", "common"}) {
    jak1::kernel_packages->value =
        jak1::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP, *((s7 + FIX_SYM_PAIR_TYPE).cast<u32>()),
                       jak1::make_string_from_c(package), jak1::kernel_packages->value);
  }
}

void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    std::printf("GOAL: %s", printed);
    std::fflush(stdout);
    clear_print();
  }
}

/*!
 * The whole game, on its own thread. This is `jak1::goal_main` with the parts that do not exist
 * here left out: InitHeapAndSymbol's two DGO loads, InitMachineScheme's `play`, and then
 * KernelCheckAndDispatch's loop body without the listener half.
 */
void game_thread(const Options& opts) {
  const u32 boot_flags = LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;
  goal_dgo_load_stats stats;

  lg::info("[game] loading KERNEL.CGO");
  if (goal_dgo_load("KERNEL", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    lg::error("[game] KERNEL.CGO failed: {}", goal_dgo_last_error());
    drain_goal_print_buffer();
    g_shared.want_exit = true;
    MasterExit = RuntimeExitStatus::EXIT;
    return;
  }
  drain_goal_print_buffer();

  jak1::InitListener();
  goal_kernel_core_stub_machine_layer(0);
  // Before GAME.CGO: pad.gc's top level builds `*cpad-list*`, which calls `cpad-open`.
  goal_pad_install();
  goal_dgo_install_goal_loader();
  // The renderer, in place of the machine layer's graphics stubs.
  goal_gfx_host host = {};
  host.send_chain = host_send_chain;
  host.vsync = host_vsync;
  host.sync_path = host_sync_path;
  host.texture_upload_now = host_texture_upload_now;
  host.texture_relocate = host_texture_relocate;
  host.set_levels = host_set_levels;
  host.set_pmode_alp = host_set_pmode_alp;
  goal_gfx_host_install(&host);
  if (opts.sound) {
    if (goal_sound_install() != GOAL_KERNEL_CORE_OK) {
      lg::error("[game] sound: {}", goal_sound_last_error());
    }
  }
  jak1::intern_from_c("*kernel-boot-message*")->value =
      jak1::intern_from_c(DebugBootMessage).offset;
  jak1::intern_from_c("*kernel-boot-mode*")->value = jak1::intern_from_c("boot").offset;
  jak1::intern_from_c("*kernel-boot-level*")->value = jak1::intern_from_c(DebugBootLevel).offset;

  lg::info("[game] loading GAME.CGO");
  if (goal_dgo_load("GAME", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    lg::error("[game] GAME.CGO failed after {} objects: {}", stats.objects, goal_dgo_last_error());
    drain_goal_print_buffer();
    g_shared.want_exit = true;
    MasterExit = RuntimeExitStatus::EXIT;
    return;
  }
  drain_goal_print_buffer();
  record_packages_in_game_cgo();

  lg::info("[game] (play)");
  jak1::call_goal_function_by_name("play");
  drain_goal_print_buffer();

  auto dispatcher = jak1::find_symbol_from_c("kernel-dispatcher");
  if (!dispatcher.offset || !dispatcher->value) {
    lg::error("[game] kernel-dispatcher holds nothing");
    g_shared.want_exit = true;
    MasterExit = RuntimeExitStatus::EXIT;
    return;
  }

  lg::info("[game] running");
  g_shared.game_running = true;
  int frame = 0;
  while (!g_shared.want_exit && MasterExit == RuntimeExitStatus::RUNNING) {
    frame++;
    {
      goal_pad_state pad;
      {
        std::lock_guard<std::mutex> lock(g_shared.pad_mutex);
        pad = g_shared.pad;
      }
      g_walk.drive(frame, &pad);
      for (const auto& press : opts.presses) {
        int first = press.first_frame;
        if (!press.after_state.empty()) {
          const int seen = frame_state_was_first_seen(press.after_state);
          if (seen < 0) {
            continue;
          }
          first = seen + press.first_frame;
        }
        if (frame < first || frame >= first + press.hold_frames) {
          continue;
        }
        if (press.is_stick) {
          pad.left_x = press.stick_x;
          pad.left_y = press.stick_y;
        } else {
          pad.buttons |= press.buttons;
        }
      }
      goal_pad_set_state(0, &pad);
    }
    for (const auto& warp : opts.warps) {
      int when = warp.first_frame;
      if (!warp.after_state.empty()) {
        const int seen = frame_state_was_first_seen(warp.after_state);
        if (seen < 0) {
          continue;
        }
        when = seen + warp.first_frame;
      }
      if (frame == when) {
        goal_continue_point_info point;
        if (!goal_continue_point_describe(warp.name.c_str(), &point) ||
            !goal_warp_to_continue(warp.name.c_str())) {
          lg::error("[game] no continue point is named \"{}\"", warp.name);
        } else {
          lg::info("[game] warping to \"{}\" in '{}, wanting '{} and '{}, vis '{}", warp.name,
                   point.level, point.want0, point.want1, point.vis_nick);
          drain_goal_print_buffer();
        }
      }
    }
    call_goal_on_stack(Ptr<Function>(dispatcher->value), goal_kernel_stack_top(), s7.offset,
                       g_ee_main_mem);
    drain_goal_print_buffer();
    // The overlord's vblank work. The audio itself is pulled by the device's own thread.
    goal_sound_frame();
    g_shared.game_frames = frame;

    const GameState now = read_game_state();
    g_walk.set_target(now.position_known, now.x * 4096.0, now.z * 4096.0, now.target_state);
    if (opts.report_levels) {
      static LevelState last_levels;
      const LevelState levels = read_level_state();
      if (levels != last_levels) {
        lg::info("[game] frame {}: {}", frame, level_state_text(levels));
        last_levels = levels;
      }
    }
    if (!now.target_state.empty() && frame_state_was_first_seen(now.target_state) < 0) {
      g_state_first_frame.emplace_back(now.target_state, frame);
      if (opts.report_state) {
        if (now.position_known) {
          lg::info("[game] frame {}: master-mode '{}, target in '{} at ({:.1f} {:.1f} {:.1f})m",
                   frame, now.master_mode, now.target_state, now.x, now.y, now.z);
        } else {
          lg::info("[game] frame {}: master-mode '{}, target in '{}", frame, now.master_mode,
                   now.target_state);
        }
      }
    }
    if (opts.exit_after_frames > 0 && frame >= opts.exit_after_frames) {
      break;
    }
  }
  lg::info("[game] stopped after {} frames", frame);
  g_shared.game_running = false;
  g_shared.want_exit = true;
  MasterExit = RuntimeExitStatus::EXIT;
}

// ---------------------------------------------------------------------------------------------

void write_screenshot(const std::string& path) {
  metal_renderer::FramePixels frame;
  if (!metal_renderer::read_last_frame(&frame)) {
    lg::error("screenshot {}: no frame has been rendered", path);
    return;
  }
  // The game target's alpha is the PS2's, which is not an opacity; the present pass ignores it and
  // so does the file.
  std::vector<u8> pixels = frame.rgba;
  for (size_t i = 3; i < pixels.size(); i += 4) {
    pixels[i] = 255;
  }
  try {
    file_util::write_rgba_png(fs::path(path), pixels.data(), frame.width, frame.height);
    lg::info("wrote {} ({}x{})", path, frame.width, frame.height);
  } catch (const std::exception& e) {
    lg::error("could not write {}: {}", path, e.what());
  }
}

int usage() {
  std::printf(
      "goalpad-play - Jak 1 on the portable kernel, drawn by the Metal renderer\n"
      "\n"
      "  --data-dir <dir>        the player's prepared data (holds iso/ and fr3/).\n"
      "                          Defaults to $GOALPAD_JAK1_DATA_DIR.\n"
      "  --saves-dir <dir>       where the game's save files live\n"
      "                          (default <data-dir>/saves; created on the first save)\n"
      "  --window <w> <h>        window size (default 1280 960)\n"
      "  --scale <1-4>           internal render resolution, in multiples of 640x480 (default 2)\n"
      "  --game-res <w> <h>      internal render resolution, exactly\n"
      "  --frames <n>            stop after n game frames (default: until the window closes)\n"
      "  --screenshot <n> <file> write the frame rendered at game frame n to file (repeatable)\n"
      "  --press <buttons>@<when>[:<frames>]    hold buttons, for scripted runs. <when> is a\n"
      "                          frame number or a target state plus an offset, e.g.\n"
      "                          start@target-title-wait+60\n"
      "  --stick <x>,<y>@<when>[:<frames>]     hold the left stick; a byte per axis, 127 centred\n"
      "  --warp <continue>@<when>  start at one of the game's own continue points, the way a\n"
      "                          warp gate does, e.g. beach-start@target-stance+300\n"
      "  --walk-to <x>,<z>@<when>[:<frames>]   walk the target to a place, in metres; legs\n"
      "                          run in order, each until it arrives or the limit runs out\n"
      "  --report-levels         print the level system's state whenever it changes\n"
      "  --report-state          print each state the target enters, and where it is standing\n"
      "  --no-sound              boot without 989snd\n"
      "  --capture-dma-dir <dir> where F9/F10 write .gpdma captures\n"
      "                          (default /private/tmp/goalpad-dma-capture)\n"
      "  --capture-burst <n>     frames one F10 press captures (default 8)\n"
      "  --capture-dma-frames <a,b,c>  capture those chain indices without a keypress\n"
      "\n"
      "Keyboard: arrows/WASD move, Return = Start, Space = X, E = Circle, F = Square,\n"
      "R = Triangle, Q/O = L1/R1, 1/P = L2/R2. A gamepad works too.\n"
      "\n"
      "F9  captures the next frame's DMA chain + EE snapshot to a .gpdma file.\n"
      "F10 captures the next n frames (see --capture-burst): hold a turn and press it, so the\n"
      "    whole of something that goes wrong mid-rotation is on disk, not one frame of it.\n"
      "Each file's path is printed. Replay one with:\n"
      "    metal-proof --replay <file.gpdma> --replay-png <out.png> \\\n"
      "        --replay-fr3 <level>.fr3 --replay-common-fr3 GAME.fr3\n");
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  lg::initialize();
  Options opts;
  if (const char* env = std::getenv("GOALPAD_JAK1_DATA_DIR")) {
    opts.data_dir = env;
  }
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--data-dir" && i + 1 < argc) {
      opts.data_dir = argv[++i];
    } else if (arg == "--saves-dir" && i + 1 < argc) {
      opts.saves_dir = argv[++i];
    } else if (arg == "--window" && i + 2 < argc) {
      opts.window_w = std::atoi(argv[++i]);
      opts.window_h = std::atoi(argv[++i]);
    } else if (arg == "--game-res" && i + 2 < argc) {
      opts.game_res_w = std::atoi(argv[++i]);
      opts.game_res_h = std::atoi(argv[++i]);
    } else if (arg == "--scale" && i + 1 < argc) {
      opts.render_scale = std::clamp(std::atoi(argv[++i]), 1, 4);
    } else if (arg == "--no-present-pacing") {
      opts.present_pacing = false;
    } else if (arg == "--report-pad") {
      opts.report_pad = true;
    } else if (arg == "--frames" && i + 1 < argc) {
      opts.exit_after_frames = std::atoi(argv[++i]);
    } else if (arg == "--screenshot" && i + 2 < argc) {
      const int frame = std::atoi(argv[++i]);
      opts.screenshots[frame] = argv[++i];
    } else if (arg == "--capture-dma-dir" && i + 1 < argc) {
      opts.capture_dir = argv[++i];
    } else if (arg == "--capture-burst" && i + 1 < argc) {
      opts.capture_burst = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--capture-dma-frames" && i + 1 < argc) {
      const std::string list = argv[++i];
      size_t at = 0;
      while (at <= list.size()) {
        const size_t comma = list.find(',', at);
        const int frame = std::atoi(list.substr(at, comma == std::string::npos ? comma : comma - at)
                                        .c_str());
        if (frame > 0) {
          opts.capture_frames.push_back(frame);
        }
        if (comma == std::string::npos) {
          break;
        }
        at = comma + 1;
      }
    } else if ((arg == "--press" || arg == "--stick") && i + 1 < argc) {
      const std::string spec = argv[++i];
      const size_t at = spec.find('@');
      if (at == std::string::npos) {
        return usage();
      }
      Options::Press press;
      const std::string what = spec.substr(0, at);
      if (arg == "--stick") {
        const size_t comma = what.find(',');
        if (comma == std::string::npos) {
          return usage();
        }
        press.is_stick = true;
        press.stick_x = (uint8_t)std::clamp(std::atoi(what.substr(0, comma).c_str()), 0, 255);
        press.stick_y = (uint8_t)std::clamp(std::atoi(what.substr(comma + 1).c_str()), 0, 255);
      } else if (!parse_buttons(what, &press.buttons)) {
        return usage();
      }
      std::string when = spec.substr(at + 1);
      const size_t colon = when.find(':');
      if (colon != std::string::npos) {
        press.hold_frames = std::max(1, std::atoi(when.substr(colon + 1).c_str()));
        when = when.substr(0, colon);
      }
      // either a frame number, or "<state>+<offset>"
      const size_t plus = when.find('+');
      if (plus != std::string::npos || !std::isdigit((unsigned char)when[0])) {
        press.after_state = when.substr(0, plus);
        press.first_frame = plus == std::string::npos ? 0 : std::atoi(when.substr(plus + 1).c_str());
      } else {
        press.first_frame = std::atoi(when.c_str());
      }
      opts.presses.push_back(press);
    } else if (arg == "--warp" && i + 1 < argc) {
      const std::string spec = argv[++i];
      const size_t at = spec.find('@');
      if (at == std::string::npos) {
        return usage();
      }
      Options::Warp warp;
      warp.name = spec.substr(0, at);
      const std::string when = spec.substr(at + 1);
      const size_t plus = when.find('+');
      if (plus != std::string::npos || !std::isdigit((unsigned char)when[0])) {
        warp.after_state = when.substr(0, plus);
        warp.first_frame = plus == std::string::npos ? 0 : std::atoi(when.substr(plus + 1).c_str());
      } else {
        warp.first_frame = std::atoi(when.c_str());
      }
      opts.warps.push_back(warp);
    } else if (arg == "--walk-to" && i + 1 < argc) {
      // --walk-to <x>,<z>@<frame|state[+delay]>[:<frames>], metres; legs run in order, each
      // until it arrives or its frame limit runs out
      const std::string spec = argv[++i];
      WalkLeg leg;
      const size_t at = spec.find('@');
      if (!WalkScript::parse_place(spec, at, &leg)) {
        return usage();
      }
      std::string when = spec.substr(at + 1);
      const size_t colon = when.find(':');
      if (colon != std::string::npos) {
        leg.frames = std::max(1, std::atoi(when.substr(colon + 1).c_str()));
        when = when.substr(0, colon);
      }
      const size_t plus = when.find('+');
      if (plus != std::string::npos || !std::isdigit((unsigned char)when[0])) {
        leg.after_state = when.substr(0, plus);
        leg.first_frame = plus == std::string::npos ? 0 : std::atoi(when.substr(plus + 1).c_str());
      } else {
        leg.first_frame = std::atoi(when.c_str());
      }
      g_walk.legs.push_back(leg);
    } else if (arg == "--report-levels") {
      opts.report_levels = true;
    } else if (arg == "--report-state") {
      opts.report_state = true;
    } else if (arg == "--no-sound") {
      opts.sound = false;
    } else {
      return usage();
    }
  }
  if (opts.game_res_w <= 0 || opts.game_res_h <= 0) {
    opts.game_res_w = 640 * opts.render_scale;
    opts.game_res_h = 480 * opts.render_scale;
  }
  if (opts.data_dir.empty()) {
    std::printf("goalpad-play needs the player's own extracted Jak 1 data.\n\n");
    return usage();
  }

  g_main_thread_id = std::this_thread::get_id();
  g_game_version = GameVersion::Jak1;
  install_walk_callbacks();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    lg::error("kernel init failed: {}", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_set_data_directory(opts.data_dir.c_str()) != GOAL_KERNEL_CORE_OK) {
    lg::error("data directory: {}", goal_kernel_core_last_error());
    return 1;
  }
  if (opts.saves_dir.empty()) {
    opts.saves_dir = opts.data_dir + "/saves";
  }
  goal_kernel_core_set_saves_directory(opts.saves_dir.c_str());
  lg::info("saves: {}", opts.saves_dir);
  register_aot_objects();

  // Graphics first, exactly as exec_runtime does it: the game uploads textures while it boots and
  // the renderer has to be there to catch them.
  // The module directly, rather than through Gfx::GetRenderer: that function names the OpenGL
  // pipeline too, and this build deliberately does not link the desktop renderer.
  g_gfx = &gRendererMetal;
  Gfx::g_global_settings.renderer = g_gfx;
  Gfx::g_global_settings.game_res_w = opts.game_res_w;
  Gfx::g_global_settings.game_res_h = opts.game_res_h;
  if (g_gfx->init(Gfx::g_global_settings)) {
    lg::error("Metal init failed");
    return 1;
  }
  auto display = g_gfx->make_display(opts.window_w, opts.window_h, "GOALPad - Jak and Daxter",
                                     Gfx::g_global_settings, GameVersion::Jak1, true);
  if (!display) {
    lg::error("could not create the window");
    return 1;
  }
  metal_renderer::set_level_art_directory(opts.data_dir + "/fr3");
  // Before the game exists. The boot relocates textures out of the shared level while GAME.CGO
  // links (`setup-font-texture!`), so it cannot be appearing underneath a running game.
  if (!metal_renderer::load_common_level_art()) {
    lg::error("could not load the shared level art ({}/fr3/GAME.fr3); textures will be missing",
              opts.data_dir);
  }

  // Frame pacing. There are two clocks available and running both is worse than running either:
  //
  //  - the display. Asking Metal to hold each drawable for one game frame
  //    (presentDrawable:afterMinimumDuration:) makes the game's rate an exact division of the
  //    refresh rate, which is as steady as the hardware gets - but only when the refresh rate
  //    really is a multiple of the game's 60.
  //  - the software frame limiter, which sleeps to a wall-clock target. It works on any display,
  //    but its period is the target plus its own overhead: measured 16.69 ms against the 16.67 ms
  //    the game wants. On a 120 Hz display that 0.02 ms is a slow drift against the vsync grid,
  //    and every ~13 seconds a frame misses its slot and is held an extra refresh. The average
  //    frame rate stays 59.9; the picture hitches.
  //
  // So: when the display's refresh is a whole multiple of the target rate, the display is the
  // clock and the limiter is switched off. Otherwise the limiter is the only clock there is.
  {
    const float target = Gfx::g_global_settings.target_fps;
    float refresh = 0;
    if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay())) {
      refresh = mode->refresh_rate;
    }
    const double multiple = refresh > 0 ? refresh / target : 0;
    const bool display_can_pace =
        opts.present_pacing && multiple >= 1 && std::abs(multiple - std::round(multiple)) < 0.02;
    metal_renderer::set_present_pacing(display_can_pace ? 1.0 / target : 0.0);
    // Both clocks stay on, at the same rate. The limiter has to: presentation pacing stops
    // applying the moment the window is not being composited - occluded, or on another space -
    // and measured, the loop then runs away to the display's full 120 fps. Giving the limiter
    // headroom so the display alone decides was tried and measured worse (interval 16.8 ms
    // +/- 1.3, spikes to 26 ms, against +/- 0.01 with both at the target rate): with headroom
    // each frame is free to land on either of two vsyncs, and it takes both.
    Gfx::g_global_settings.framelimiter = true;
    lg::info("[gfx] display is {:.3f} Hz, target {:.0f} fps: paced by {}", refresh, target,
             display_can_pace ? "the display, with the frame limiter as a floor"
                              : "the software frame limiter");
  }

  if (opts.sound && !goalpad_audio::start()) {
    lg::warn("running without audio");
  }
  start_gamepads();
  refresh_gamepad();
  if (!g_gamepad) {
    lg::info("[pad] no controller found; the keyboard is the controller");
  }

  g_capture_dir = opts.capture_dir;
  g_capture_frames.insert(opts.capture_frames.begin(), opts.capture_frames.end());
  lg::info("[capture] F9 captures the next frame, F10 the next {}, into {}", opts.capture_burst,
           g_capture_dir);

  std::thread game(game_thread, std::cref(opts));

  // The main thread is the graphics thread: it owns the window, the SDL event queue and the
  // renderer, and it presents one frame per pass. Mirror of Gfx::Loop.
  Timer clock;
  double last_report = 0;
  int frames_at_report = 0;
  auto screenshots_left = opts.screenshots;
  while (!g_shared.want_exit) {
    display->render();
    read_pad(opts);

    if (!screenshots_left.empty()) {
      const int frame = g_shared.game_frames;
      auto it = screenshots_left.begin();
      if (frame >= it->first) {
        write_screenshot(it->second);
        screenshots_left.erase(it);
      }
    }

    const double now = clock.getSeconds();
    if (now - last_report >= 2.0) {
      const int frames = g_shared.game_frames;
      const auto art = metal_renderer::get_level_art_stats();
      const auto timing = metal_renderer::take_present_timing();
      lg::info(
          "[gfx] {:.1f} game fps, {} frames, frame interval {:.2f} ms +/- {:.2f} "
          "(min {:.2f}, max {:.2f}, {} late), levels wanted '{}' loaded '{}'",
          (frames - frames_at_report) / (now - last_report), frames, timing.mean_ms,
          timing.stddev_ms, timing.min_ms, timing.max_ms, timing.late_frames,
          art.wanted.empty() ? "-" : art.wanted, art.loaded.empty() ? "-" : art.loaded);
      last_report = now;
      frames_at_report = frames;
    }
    if (opts.exit_after_frames > 0 && !g_shared.game_running && g_shared.game_frames > 0) {
      break;
    }
  }

  // Let the game thread out of syncv, whichever side asked to stop.
  g_shared.want_exit = true;
  MasterExit = RuntimeExitStatus::EXIT;
  // The renderer's vsync wakes on MasterExit, but a game thread already inside a frame still needs
  // frames to come back from, so keep presenting until it is done.
  for (int i = 0; i < 240 && g_shared.game_running; i++) {
    display->render();
  }
  game.join();

  for (const auto& [frame, path] : screenshots_left) {
    lg::warn("screenshot for frame {} ({}) was never taken: the run ended at frame {}", frame, path,
             (int)g_shared.game_frames);
  }

  goal_gfx_host_stats stats;
  goal_gfx_host_stats_get(&stats);
  const auto art = metal_renderer::get_level_art_stats();
  const auto chain = metal_renderer::get_chain_stats();
  lg::info(
      "{} game frames, {} chains sent, {} presented, {} syncv, {} texture uploads; "
      "levels: {} requests, {} loaded ({}), {} failures",
      (int)g_shared.game_frames, stats.chains, (int)chain.chains_rendered, stats.vsyncs,
      stats.texture_uploads, art.requests, art.levels_loaded, art.loaded, art.load_failures);

  if (opts.report_state && !g_state_first_frame.empty()) {
    std::string states;
    for (const auto& entry : g_state_first_frame) {
      states += fmt::format(" '{}@{}", entry.first, entry.second);
    }
    lg::info("the target's states, in the order they were first entered:{}", states);
  }

  goalpad_audio::stop();
  goal_sound_shutdown();
  display.reset();
  g_gfx->exit();
  goal_kernel_core_shutdown();
  return 0;
}
