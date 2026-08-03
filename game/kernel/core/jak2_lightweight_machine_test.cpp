/*!
 * @file jak2_lightweight_machine_test.cpp
 * Check the small portable Jak II machine functions that do not belong to a host subsystem.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "common/global_profiler/GlobalProfiler.h"
#include "common/util/FileUtil.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/jak2/kmachine.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/sce/sif_ee.h"

namespace {

int g_failures = 0;

void expect(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) {
    g_failures++;
  }
}

bool lookup_function(const char* name, uint32_t* out) {
  return goal_kernel_core_lookup(name, nullptr, out) == GOAL_KERNEL_CORE_OK && *out != 0;
}

}  // namespace

int main() {
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: initialize: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  if (goal_kernel_core_stub_machine_layer(1) != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL: machine seams: %s\n", goal_kernel_core_last_error());
    goal_kernel_core_shutdown();
    return 1;
  }

  uint32_t pc_prof = 0;
  uint32_t flush_cache = 0;
  uint32_t mouse_get_data = 0;
  expect(lookup_function("pc-prof", &pc_prof), "pc-prof holds a portable implementation");
  expect(lookup_function("flush-cache", &flush_cache),
         "flush-cache holds its signed-AOT no-op implementation");
  expect(lookup_function("mouse-get-data", &mouse_get_data),
         "mouse-get-data holds its inactive-pointer implementation");

  const auto expected_user_dir = file_util::get_user_config_dir().string();
  const auto expected_settings_dir = file_util::get_user_settings_dir(GameVersion::Jak2).string();
  const auto user_dir = jak2::intern_from_c("*pc-user-dir-base-path*")->value();
  const auto settings_dir = jak2::intern_from_c("*pc-settings-folder*")->value();
  const auto settings_sha = jak2::intern_from_c("*pc-settings-built-sha*")->value();
  expect(user_dir && !std::strcmp(Ptr<String>(user_dir)->data(), expected_user_dir.c_str()),
         "Jak 2 PC user directory uses the sandbox Application Support path");
  expect(settings_dir &&
             !std::strcmp(Ptr<String>(settings_dir)->data(), expected_settings_dir.c_str()),
         "Jak 2 settings directory uses the sandbox Application Support path");
  expect(settings_sha && !Ptr<String>(settings_sha)->data()[0],
         "Jak 2 settings build identity is an initialized GOAL string");

  auto& profiler = prof();
  profiler.clear();
  profiler.set_enable(true);
  const uint32_t name = (uint32_t)jak2::make_string_from_c("goalpad-jak2-profiler");
  const uint32_t empty = (uint32_t)jak2::make_string_from_c("");
  goal_aot_call(pc_prof, name, ProfNode::BEGIN, 0);
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  goal_aot_call(pc_prof, empty, ProfNode::END, 0);
  profiler.set_enable(false);
  expect(profiler.get_next_idx() == 3,
         "pc-prof forwards begin, instant and end events to the portable profiler");
  goal_aot_call(pc_prof, name, ProfNode::INSTANT, 0);
  expect(profiler.get_next_idx() == 3, "pc-prof preserves the profiler's disabled behavior");

  expect(goal_aot_call(flush_cache, 0, 0, 0) == 0 &&
             goal_aot_call(flush_cache, 2, 0, 0) == 0 &&
             goal_aot_call(flush_cache, UINT32_MAX, 0, 0) == 0,
         "flush-cache accepts every upstream mode without executable-memory work");

  char temp_template[] = "/tmp/goalpad-jak2-file-stream.XXXXXX";
  char* temp_root = mkdtemp(temp_template);
  expect(temp_root != nullptr, "created a synthetic file-stream root");
  if (temp_root) {
    uint32_t file_open = 0;
    uint32_t file_close = 0;
    uint32_t file_length = 0;
    uint32_t file_seek = 0;
    uint32_t file_read = 0;
    uint32_t file_write = 0;
    uint32_t file_exists = 0;
    uint32_t make_parent = 0;
    const bool file_functions = lookup_function("file-stream-open", &file_open) &&
                                lookup_function("file-stream-close", &file_close) &&
                                lookup_function("file-stream-length", &file_length) &&
                                lookup_function("file-stream-seek", &file_seek) &&
                                lookup_function("file-stream-read", &file_read) &&
                                lookup_function("file-stream-write", &file_write) &&
                                lookup_function("pc-filepath-exists?", &file_exists) &&
                                lookup_function("pc-mkdir-file-path", &make_parent);
    expect(file_functions, "file-stream symbols hold portable implementations");

    const std::string path_string = std::string(temp_root) + "/settings/probe.bin";
    const uint32_t path = (uint32_t)jak2::make_string_from_c(path_string.c_str());
    const uint32_t read_mode = jak2::intern_from_c("read").offset;
    const uint32_t write_mode = jak2::intern_from_c("write").offset;
    auto stream_mem = kmalloc(kglobalheap, sizeof(FileStream), KMALLOC_MEMSET, "file-stream-test");
    auto buffer_mem = kmalloc(kglobalheap, 16, KMALLOC_MEMSET, "file-stream-buffer-test");
    expect(stream_mem.offset && buffer_mem.offset, "allocated synthetic stream and buffer");

    if (file_functions && stream_mem.offset && buffer_mem.offset) {
      expect(goal_aot_call(file_exists, path, 0, 0) == goal_game_false_offset(),
             "missing sandbox path reports false");
      expect(goal_aot_call(make_parent, path, 0, 0) == goal_game_true_offset(),
             "sandbox path parent directory is created");
      expect(goal_aot_call(file_open, stream_mem.offset, path, write_mode) == stream_mem.offset,
             "write stream returns its GOAL object");

      constexpr char payload[] = "goalpad";
      std::memcpy(buffer_mem.c(), payload, sizeof(payload));
      expect(goal_aot_call(file_write, stream_mem.offset, buffer_mem.offset, sizeof(payload)) ==
                 sizeof(payload),
             "file-stream-write stores every byte");
      expect(goal_aot_call(file_close, stream_mem.offset, 0, 0) == stream_mem.offset,
             "file-stream-close returns its GOAL object");
      expect(goal_aot_call(file_exists, path, 0, 0) == goal_game_true_offset(),
             "written sandbox path reports true");

      std::memset(buffer_mem.c(), 0, sizeof(payload));
      expect(goal_aot_call(file_open, stream_mem.offset, path, read_mode) == stream_mem.offset,
             "read stream returns its GOAL object");
      expect(goal_aot_call(file_seek, stream_mem.offset, 2, SCE_SEEK_SET) == 2,
             "file-stream-seek moves away from the start");
      expect(goal_aot_call(file_length, stream_mem.offset, 0, 0) == sizeof(payload),
             "file-stream-length resets the stream to the start");
      expect(goal_aot_call(file_seek, stream_mem.offset, 0, SCE_SEEK_CUR) == 0,
             "file-stream-length restored the start position");
      expect(goal_aot_call(file_read, stream_mem.offset, buffer_mem.offset, sizeof(payload)) ==
                 sizeof(payload) &&
                 std::memcmp(buffer_mem.c(), payload, sizeof(payload)) == 0,
             "file-stream-read restores every byte");
      goal_aot_call(file_close, stream_mem.offset, 0, 0);

      const std::string missing_path_string = std::string(temp_root) + "/missing/probe.bin";
      const uint32_t missing_path =
          (uint32_t)jak2::make_string_from_c(missing_path_string.c_str());
      expect(goal_aot_call(file_open, stream_mem.offset, missing_path, read_mode) ==
                     stream_mem.offset &&
                 Ptr<FileStream>(stream_mem.offset)->file == -1,
             "failed read open preserves the stream object and signed file error");
      expect(goal_aot_call(file_length, stream_mem.offset, 0, 0) == 0 &&
                 goal_aot_call(file_seek, stream_mem.offset, 0, SCE_SEEK_SET) == UINT64_MAX &&
                 goal_aot_call(file_read, stream_mem.offset, buffer_mem.offset, 1) == UINT64_MAX &&
                 goal_aot_call(file_write, stream_mem.offset, buffer_mem.offset, 1) == UINT64_MAX,
             "failed stream operations return sign-extended GOAL integers");
      goal_aot_call(file_close, stream_mem.offset, 0, 0);
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(temp_root, cleanup_error);
    expect(!cleanup_error, "removed the synthetic file-stream root");
  }

  uint32_t pc_get_os = 0;
  uint32_t pc_get_display_mode = 0;
  uint32_t pc_get_display_size = 0;
  uint32_t pc_get_window_size = 0;
  uint32_t pc_get_refresh_rate = 0;
  uint32_t pc_is_supported_resolution = 0;
  const bool settings_queries = lookup_function("pc-get-os", &pc_get_os) &&
                                lookup_function("pc-get-display-mode", &pc_get_display_mode) &&
                                lookup_function("pc-get-active-display-size", &pc_get_display_size) &&
                                lookup_function("pc-get-window-size", &pc_get_window_size) &&
                                lookup_function("pc-get-active-display-refresh-rate",
                                                &pc_get_refresh_rate) &&
                                lookup_function("pc-is-supported-resolution?",
                                                &pc_is_supported_resolution);
  expect(settings_queries, "PC settings queries hold portable implementations");
  auto display_size = kmalloc(kglobalheap, sizeof(s64) * 2, KMALLOC_MEMSET, "display-size-test");
  expect(display_size.offset != 0, "allocated synthetic display-size outputs");
  if (settings_queries && display_size.offset) {
    const auto width = display_size.offset;
    const auto height = display_size.offset + sizeof(s64);
    goal_aot_call(pc_get_display_size, width, height, 0);
    expect(goal_aot_call(pc_get_os, 0, 0, 0) == jak2::intern_from_c("darwin").offset &&
               goal_aot_call(pc_get_display_mode, 0, 0, 0) ==
                   jak2::intern_from_c("windowed").offset,
           "PC settings report the portable Apple window mode");
    expect(*Ptr<s64>(width).c() == 640 && *Ptr<s64>(height).c() == 480 &&
               goal_aot_call(pc_get_refresh_rate, 0, 0, 0) == 60,
           "PC settings expose a deterministic fallback display");
    *Ptr<s64>(width).c() = 0;
    *Ptr<s64>(height).c() = 0;
    goal_aot_call(pc_get_window_size, width, height, 0);
    expect(*Ptr<s64>(width).c() == 640 && *Ptr<s64>(height).c() == 480,
           "PC settings expose a deterministic fallback window");
    expect(goal_aot_call(pc_is_supported_resolution, 640, 480, 0) ==
                   goal_game_true_offset() &&
               goal_aot_call(pc_is_supported_resolution, 0, 480, 0) ==
                   goal_game_false_offset(),
           "PC settings reject only empty fallback resolutions");
  }

  auto mouse_mem =
      kmalloc(kglobalheap, sizeof(jak2::MouseInfo), KMALLOC_MEMSET, "inactive-mouse-test");
  expect(mouse_mem.offset != 0, "allocated synthetic mouse-info");
  if (mouse_mem.offset) {
    auto* mouse = reinterpret_cast<jak2::MouseInfo*>(mouse_mem.c());
    mouse->active = 0xffffffff;
    mouse->valid = 0xffffffff;
    mouse->cursor = 0xffffffff;
    mouse->status = 0xffff;
    mouse->button0 = 0xffff;
    mouse->deltax = 7;
    mouse->deltay = -7;
    mouse->wheel = 9;
    mouse->posx = 123.f;
    mouse->posy = -123.f;

    expect(goal_aot_call(mouse_get_data, mouse_mem.offset, 0, 0) == mouse_mem.offset,
           "mouse-get-data returns its mouse-info");
    expect(mouse->active == s7.offset && mouse->valid == s7.offset &&
               mouse->cursor == s7.offset,
           "mouse-get-data reports no pointer device");
    expect(mouse->status == 0 && mouse->button0 == 0 && mouse->deltax == 0 &&
               mouse->deltay == 0 && mouse->wheel == 0 && mouse->posx == 0.f &&
               mouse->posy == 0.f,
           "mouse-get-data clears stale desktop input state");
  }

  goal_kernel_core_shutdown();

  pc_prof = 0;
  flush_cache = 0;
  mouse_get_data = 0;
  const bool initialized = goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK;
  const bool reinitialized =
      initialized && goal_kernel_core_stub_machine_layer(1) == GOAL_KERNEL_CORE_OK;
  expect(reinitialized && lookup_function("pc-prof", &pc_prof) &&
             lookup_function("flush-cache", &flush_cache) &&
             lookup_function("mouse-get-data", &mouse_get_data),
         "all lightweight implementations survive kernel reinitialization");
  if (reinitialized) {
    goal_aot_call(flush_cache, 0, 0, 0);
  }
  if (initialized) {
    goal_kernel_core_shutdown();
  }

  std::printf("\n%s (%d failures)\n",
              g_failures ? "JAK 2 LIGHTWEIGHT MACHINE TEST FAILED"
                         : "JAK 2 LIGHTWEIGHT MACHINE TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
