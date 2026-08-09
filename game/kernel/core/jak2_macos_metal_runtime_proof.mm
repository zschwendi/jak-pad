/*!
 * @file jak2_macos_metal_runtime_proof.mm
 * macOS host for the Jak 2 AOT runtime and its external Metal renderer.
 *
 * This is a development proof, not the desktop OpenGOAL runtime. It uses the portable signed-code
 * path shared with iPadOS. Its bounded mode reports the first incomplete renderer boundary; the
 * opt-in interactive mode keeps the same runtime alive for title/input/audio playability checks.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <thread>

#include "common/util/FileUtil.h"

#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#include "game/goalpad_audio.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/jak2_runtime.h"
#include "game/kernel/core/pad.h"

#include "third-party/SDL/include/SDL3/SDL.h"

#import <QuartzCore/CAMetalLayer.h>

namespace {

struct Options {
  std::string data_dir;
  std::string saves_dir = "/private/tmp/goalpad-jak2-macos-saves";
  int maximum_ticks = 3;
  bool hidden = false;
  bool require_presentation = false;
  bool interactive = false;
  bool report_pad = false;
  bool probe_pad = false;
  bool audio = false;
  bool ticks_explicit = false;
};

int usage(const char* program) {
  std::fprintf(
      stderr,
      "usage: %s --data-dir <prepared-jak2-dir> [--saves-dir <dir>] [--ticks <1-1200>] "
      "[--hidden] [--require-presentation] [--interactive] [--report-pad] [--probe-pad] "
      "[--audio]\n"
      "       --data-dir defaults to $GOALPAD_JAK2_DATA_DIR\n",
      program);
  return 2;
}

bool parse_options(int argc, char** argv, Options* out) {
  if (const char* data_dir = std::getenv("GOALPAD_JAK2_DATA_DIR")) {
    out->data_dir = data_dir;
  }
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--data-dir" && i + 1 < argc) {
      out->data_dir = argv[++i];
    } else if (arg == "--saves-dir" && i + 1 < argc) {
      out->saves_dir = argv[++i];
    } else if (arg == "--ticks" && i + 1 < argc) {
      out->maximum_ticks = std::atoi(argv[++i]);
      out->ticks_explicit = true;
      if (out->maximum_ticks < 1 || out->maximum_ticks > 1200) {
        return false;
      }
    } else if (arg == "--hidden") {
      out->hidden = true;
    } else if (arg == "--require-presentation") {
      out->require_presentation = true;
    } else if (arg == "--interactive") {
      out->interactive = true;
    } else if (arg == "--report-pad") {
      out->report_pad = true;
    } else if (arg == "--probe-pad") {
      out->probe_pad = true;
    } else if (arg == "--audio") {
      out->audio = true;
    } else {
      return false;
    }
  }
  if (out->interactive) {
    out->hidden = false;
    out->require_presentation = true;
    if (!out->ticks_explicit) {
      out->maximum_ticks = 0;
    }
  }
  return !out->data_dir.empty() && !out->saves_dir.empty();
}

struct ProofResources {
  bool sdl_initialized = false;
  bool audio_started = false;
  SDL_Window* window = nullptr;
  SDL_MetalView metal_view = nullptr;
  goal_jak2_metal_host* metal_host = nullptr;
  SDL_Gamepad* gamepad = nullptr;

  ~ProofResources() {
    // The runtime retains copied callbacks into metal_host. It must always release them first.
    if (audio_started) {
      goalpad_audio::stop();
    }
    goal_jak2_runtime_shutdown();
    if (gamepad) {
      SDL_CloseGamepad(gamepad);
    }
    if (metal_host) {
      goal_jak2_metal_host_destroy(metal_host);
    }
    if (metal_view) {
      SDL_Metal_DestroyView(metal_view);
    }
    if (window) {
      SDL_DestroyWindow(window);
    }
    if (sdl_initialized) {
      SDL_Quit();
    }
  }
};

bool requests_quit(const SDL_Event& event) {
  return event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
         (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE);
}

void pump_events(bool* quit_requested) {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (requests_quit(event)) {
      *quit_requested = true;
    }
  }
}

bool window_presentable(SDL_Window* window) {
  const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
  return (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) == 0;
}

goal_jak2_runtime_status start_runtime_while_pumping_events(
    const goal_jak2_runtime_config* config,
    bool* quit_requested) {
  auto starter = std::async(std::launch::async, [config] { return goal_jak2_runtime_start(config); });
  while (starter.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    @autoreleasepool {
      pump_events(quit_requested);
      SDL_Delay(1);
    }
  }
  return starter.get();
}

void refresh_gamepad(ProofResources* resources) {
  if (resources->gamepad && !SDL_GamepadConnected(resources->gamepad)) {
    SDL_CloseGamepad(resources->gamepad);
    resources->gamepad = nullptr;
  }
  if (resources->gamepad) {
    return;
  }

  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids) {
    if (count > 0) {
      resources->gamepad = SDL_OpenGamepad(ids[0]);
    }
    SDL_free(ids);
  }
}

uint8_t axis_byte(bool negative, bool positive, int16_t analog) {
  if (negative || positive) {
    return negative ? 0 : 255;
  }
  return static_cast<uint8_t>(std::clamp((analog + 32768) / 257, 0, 255));
}

goal_pad_state read_pad(ProofResources* resources) {
  refresh_gamepad(resources);
  const bool* keys = SDL_GetKeyboardState(nullptr);

  goal_pad_state pad;
  goal_pad_state_neutral(&pad);
  const auto key = [keys](SDL_Scancode code) { return keys && keys[code]; };
  const auto button = [resources](SDL_GamepadButton code) {
    return resources->gamepad && SDL_GetGamepadButton(resources->gamepad, code);
  };
  const auto axis = [resources](SDL_GamepadAxis code) -> int16_t {
    return resources->gamepad ? SDL_GetGamepadAxis(resources->gamepad, code) : 0;
  };

  struct Bind {
    uint32_t bit;
    SDL_Scancode key;
    SDL_GamepadButton button;
  };
  static constexpr Bind kBinds[] = {
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
    if (key(bind.key) || button(bind.button)) {
      pad.buttons |= bind.bit;
    }
  }
  if (key(SDL_SCANCODE_1) || axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192) {
    pad.buttons |= GOAL_PAD_L2;
  }
  if (key(SDL_SCANCODE_P) || axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) {
    pad.buttons |= GOAL_PAD_R2;
  }

  pad.left_x = axis_byte(key(SDL_SCANCODE_A), key(SDL_SCANCODE_D),
                         axis(SDL_GAMEPAD_AXIS_LEFTX));
  pad.left_y = axis_byte(key(SDL_SCANCODE_W), key(SDL_SCANCODE_S),
                         axis(SDL_GAMEPAD_AXIS_LEFTY));
  pad.right_x = axis_byte(key(SDL_SCANCODE_L), key(SDL_SCANCODE_J),
                          axis(SDL_GAMEPAD_AXIS_RIGHTX));
  pad.right_y = axis_byte(key(SDL_SCANCODE_I), key(SDL_SCANCODE_K),
                          axis(SDL_GAMEPAD_AXIS_RIGHTY));
  return pad;
}

void report_pad_change(const goal_pad_state& pad) {
  static goal_pad_state previous = {};
  static bool have_previous = false;
  const bool changed = !have_previous || pad.buttons != previous.buttons ||
                       pad.left_x != previous.left_x || pad.left_y != previous.left_y ||
                       pad.right_x != previous.right_x || pad.right_y != previous.right_y;
  if (changed) {
    std::printf("pad: buttons=%#06x left=(%u,%u) right=(%u,%u) reads=%d\n", pad.buttons,
                pad.left_x, pad.left_y, pad.right_x, pad.right_y, goal_pad_read_count(0));
    previous = pad;
    have_previous = true;
  }
}

void print_runtime_metrics(const goal_jak2_runtime_metrics& runtime) {
  std::printf(
      "runtime: state=%d ticks=%llu title=%d dispatcher=%llu host-chains=%d sync=%d/%d "
      "uploads=%d relocations=%d\n",
      runtime.state, static_cast<unsigned long long>(runtime.ticks), runtime.title_ready,
      static_cast<unsigned long long>(runtime.last_dispatch_result), runtime.host_chains,
      runtime.host_sync_paths, runtime.host_syncvs, runtime.host_texture_uploads,
      runtime.host_texture_relocations);
}

void print_metal_metrics(const goal_jak2_metal_host_metrics& metal) {
  std::printf(
      "metal: chains=%llu complete=%llu failed=%llu buckets=%llu commit=%llu/%llu errors=%llu "
      "drawable=%llu miss=%llu submit=%llu present=%llu late=%llu drops=%llu order=%llu "
      "unsupported-blends=%llu draws=%llu tris=%llu skipped=%llu\n",
      static_cast<unsigned long long>(metal.chains),
      static_cast<unsigned long long>(metal.completed_chains),
      static_cast<unsigned long long>(metal.failed_chains),
      static_cast<unsigned long long>(metal.last_buckets_dispatched),
      static_cast<unsigned long long>(metal.command_buffers_completed),
      static_cast<unsigned long long>(metal.command_buffers_committed),
      static_cast<unsigned long long>(metal.command_buffer_errors),
      static_cast<unsigned long long>(metal.drawables_acquired),
      static_cast<unsigned long long>(metal.drawable_misses),
      static_cast<unsigned long long>(metal.submissions),
      static_cast<unsigned long long>(metal.presentations),
      static_cast<unsigned long long>(metal.late_present_submissions),
      static_cast<unsigned long long>(metal.presentation_drops),
      static_cast<unsigned long long>(metal.presentation_order_mismatches),
      static_cast<unsigned long long>(metal.unsupported_blends),
      static_cast<unsigned long long>(metal.draws),
      static_cast<unsigned long long>(metal.triangles),
      static_cast<unsigned long long>(metal.skipped_bucket_bytes));
  std::printf(
      "background: tie=%llu/%llu missing-levels=%llu missing-textures=%llu anim-slots=%llu; "
      "merc: models=%llu draws=%llu tris=%llu malformed=%llu missing-models=%llu "
      "bones=(bad-ptr=%llu missing=%llu nonfinite=%llu degenerate=%llu incoherent=%llu)\n",
      static_cast<unsigned long long>(metal.last_tie_draws),
      static_cast<unsigned long long>(metal.last_tie_triangles),
      static_cast<unsigned long long>(metal.last_background_missing_levels),
      static_cast<unsigned long long>(metal.last_background_missing_textures),
      static_cast<unsigned long long>(metal.last_background_anim_slot_draws),
      static_cast<unsigned long long>(metal.last_merc_models),
      static_cast<unsigned long long>(metal.last_merc_draws),
      static_cast<unsigned long long>(metal.last_merc_triangles),
      static_cast<unsigned long long>(metal.last_merc_malformed_dma),
      static_cast<unsigned long long>(metal.last_merc_missing_models),
      static_cast<unsigned long long>(metal.last_merc_bad_bone_pointers),
      static_cast<unsigned long long>(metal.last_merc_missing_bone_slots),
      static_cast<unsigned long long>(metal.last_merc_nonfinite_bone_matrices),
      static_cast<unsigned long long>(metal.last_merc_degenerate_bone_matrices),
      static_cast<unsigned long long>(metal.last_merc_incoherent_bone_sources));
  const auto print_texture_capture = [](const char* family,
                                        const goal_jak2_tfrag_texture_upload_metrics& upload) {
    std::printf(
        "%s-textures[%u]: captures=%llu present=%llu executed=%llu "
        "classes=(malformed=%llu absent=%llu "
        "ordinary=%llu animator=%llu mixed=%llu eye-other=%llu gs-setup=%llu) "
        "transfers=%llu payload=%llu inert=%llu descriptors=%llu direct-tail=%llu "
        "gs-setup-transfers=%llu animator-arrays=%llu/%llu "
        "animator-bytes=%llu "
        "eye=%llu other=%llu malformed-transfers=%llu "
        "last-nonordinary=(bytes=%u qwc=%u tag=%u vif=%u:%u/%u:%u)\n",
        family, upload.bucket_id, static_cast<unsigned long long>(upload.captures),
        static_cast<unsigned long long>(upload.present_captures),
        static_cast<unsigned long long>(upload.executions),
        static_cast<unsigned long long>(upload.classifications[0]),
        static_cast<unsigned long long>(upload.classifications[1]),
        static_cast<unsigned long long>(upload.classifications[2]),
        static_cast<unsigned long long>(upload.classifications[3]),
        static_cast<unsigned long long>(upload.classifications[4]),
        static_cast<unsigned long long>(upload.classifications[5]),
        static_cast<unsigned long long>(upload.classifications[6]),
        static_cast<unsigned long long>(upload.transfers),
        static_cast<unsigned long long>(upload.payload_bytes),
        static_cast<unsigned long long>(upload.inert_transfers),
        static_cast<unsigned long long>(upload.ordinary_descriptors),
        static_cast<unsigned long long>(upload.direct_setup_transfers),
        static_cast<unsigned long long>(upload.gs_setup_transfers),
        static_cast<unsigned long long>(upload.animator_arrays),
        static_cast<unsigned long long>(upload.animator_body_transfers),
        static_cast<unsigned long long>(upload.animator_payload_bytes),
        static_cast<unsigned long long>(upload.eye_markers),
        static_cast<unsigned long long>(upload.other_transfers),
        static_cast<unsigned long long>(upload.malformed_transfers),
        upload.last_nonordinary_payload_bytes, upload.last_nonordinary_qwc,
        upload.last_nonordinary_tag_kind, upload.last_nonordinary_vif0_kind,
        upload.last_nonordinary_vif0_immediate, upload.last_nonordinary_vif1_kind,
        upload.last_nonordinary_vif1_immediate);
    for (std::size_t i = 0; i < std::size(upload.opcode_counts); ++i) {
      if (upload.opcode_counts[i] != 0) {
        std::printf("%s-textures[%u]-opcode[%zu]=%llu\n", family, upload.bucket_id, i,
                    static_cast<unsigned long long>(upload.opcode_counts[i]));
      }
    }
  };
  for (const auto& upload : metal.tfrag_texture_uploads) {
    print_texture_capture("tfrag", upload);
  }
  for (const auto& upload : metal.shrub_texture_uploads) {
    print_texture_capture("shrub", upload);
  }
  print_texture_capture("common-tfrag", metal.common_tfrag_texture_upload);
  std::printf(
      "common-tfrag-skull-gem: ordinary=%llu prepared=%llu published=%llu "
      "texture=%llu tbp=%u anim-slot=%u\n",
      static_cast<unsigned long long>(metal.common_tfrag_ordinary_uploads),
      static_cast<unsigned long long>(metal.common_tfrag_skull_gem_preparations),
      static_cast<unsigned long long>(metal.common_tfrag_skull_gem_publications),
      static_cast<unsigned long long>(metal.common_tfrag_skull_gem_texture),
      metal.common_tfrag_skull_gem_destination_tbp,
      metal.common_tfrag_skull_gem_anim_slot);
  std::printf(
      "sky: draws=%llu tris=%llu valid=%u textured=%u vertices=%u rgb-vertices=%u "
      "tbp=%u tcc=%u decal=%u lookup=%u placeholder=%u write-rgb=%u "
      "blend=%u(%u,%u,%u,%u) alpha-test=%u(%u,%u,%u)\n",
      static_cast<unsigned long long>(metal.last_sky_draw_draws),
      static_cast<unsigned long long>(metal.last_sky_draw_triangles),
      metal.last_sky_draw_batch_valid, metal.last_sky_draw_batch_textured,
      metal.last_sky_draw_batch_vertices, metal.last_sky_draw_batch_nonzero_rgb_vertices,
      metal.last_sky_draw_batch_tex0_tbp, metal.last_sky_draw_batch_tex0_tcc,
      metal.last_sky_draw_batch_tex0_decal, metal.last_sky_draw_batch_texture_lookup_hit,
      metal.last_sky_draw_batch_used_placeholder, metal.last_sky_draw_batch_write_rgb,
      metal.last_sky_draw_batch_blend_enabled, metal.last_sky_draw_batch_blend_a,
      metal.last_sky_draw_batch_blend_b, metal.last_sky_draw_batch_blend_c,
      metal.last_sky_draw_batch_blend_d, metal.last_sky_draw_batch_alpha_test_enabled,
      metal.last_sky_draw_batch_alpha_test_mode, metal.last_sky_draw_batch_alpha_aref,
      metal.last_sky_draw_batch_alpha_afail);
  const auto& bucket4 = metal.last_bucket4_texture_upload;
  std::printf(
      "bucket4-upload: valid=%u present=%u payload=%u transfers=%u/%u "
      "inert=%u(cnt=%u next=%u states=%#x) malformed=%u/%u unsupported=%u/%u\n",
      bucket4.valid, bucket4.present, bucket4.total_payload_bytes, bucket4.payload_transfers,
      bucket4.dma_transfers, bucket4.inert_transfers, bucket4.inert_cnt_transfers,
      bucket4.inert_next_transfers, bucket4.inert_state_mask, bucket4.malformed_bytes,
      bucket4.malformed_transfers, bucket4.unsupported_bytes, bucket4.unsupported_transfers);
  std::printf(
      "bucket4-ordinary: descriptors=%u page=%#llx mode=%lld; "
      "animator: arrays=%u bytes=%u opcodes=12:%u 13:%u 14:%u 15:%u 16:%u 41:%u "
      "finishes=%u cloud-dest=%d; executed: ordinary=%llu mixed=%llu cloud=%llu fog=%llu "
      "handles=(%#llx,%#llx)\n",
      bucket4.ordinary_descriptors, static_cast<unsigned long long>(bucket4.ordinary_page),
      static_cast<long long>(bucket4.ordinary_mode), bucket4.animator_arrays,
      bucket4.animator_bytes, bucket4.opcode_counts[12], bucket4.opcode_counts[13],
      bucket4.opcode_counts[14], bucket4.opcode_counts[15], bucket4.opcode_counts[16],
      bucket4.opcode_counts[41], bucket4.finishes, bucket4.cloud_destination,
      static_cast<unsigned long long>(metal.bucket4_ordinary_uploads),
      static_cast<unsigned long long>(metal.bucket4_mixed_executions),
      static_cast<unsigned long long>(metal.bucket4_cloud_publications),
      static_cast<unsigned long long>(metal.bucket4_fog_publications),
      static_cast<unsigned long long>(metal.bucket4_cloud_texture),
      static_cast<unsigned long long>(metal.bucket4_fog_texture));
  std::printf(
      "bucket4-erase: %ux%u dest=%u test=%#llx alpha=%#llx clamp=%#llx "
      "clear=(%u,%u,%u,%u); "
      "generic: src=%#x %ux%u dest=%u format=%u force=%u; clut: src=%#x dest=%u\n",
      bucket4.erase_width, bucket4.erase_height, bucket4.erase_destination,
      static_cast<unsigned long long>(bucket4.erase_test),
      static_cast<unsigned long long>(bucket4.erase_alpha),
      static_cast<unsigned long long>(bucket4.erase_clamp),
      bucket4.erase_clear[0], bucket4.erase_clear[1], bucket4.erase_clear[2],
      bucket4.erase_clear[3], bucket4.generic_source, bucket4.generic_width,
      bucket4.generic_height, bucket4.generic_destination, bucket4.generic_format,
      bucket4.generic_force_to_gpu, bucket4.clut_source, bucket4.clut_destination);
  const auto& sprite_upload = metal.last_sprite_texture_upload;
  std::printf("sprite-upload: valid=%u present=%u count=%u groups=", sprite_upload.valid,
              sprite_upload.present, sprite_upload.upload_count);
  for (uint32_t i = 0;
       i < sprite_upload.upload_count && i < GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS; ++i) {
    std::printf("%s(%#llx,%lld)", i == 0 ? "" : ",",
                static_cast<unsigned long long>(sprite_upload.pages[i]),
                static_cast<long long>(sprite_upload.modes[i]));
  }
  std::printf(" executed=%llu\n",
              static_cast<unsigned long long>(metal.sprite_texture_uploads));
  std::printf(
      "sprites: 2d=%llu 3d=%llu hud=%llu distort=%llu normal-submitted=%llu "
      "glow-marked=%llu glow=(parsed=%llu accepted=%llu rejected=%llu invalid=%llu "
      "visibility-final-drawn/submitted=%llu/%llu draws=%llu tris=%llu missing=%llu skipped=%llu) "
      "draws=%llu tris=%llu "
      "missing-textures=%llu unsupported-bytes=%llu; direct: sky=%llu/%llu "
      "screen-filter=%llu/%llu debug-no-zbuf2=%llu/%llu\n",
      static_cast<unsigned long long>(metal.last_sprites_2d),
      static_cast<unsigned long long>(metal.last_sprites_3d),
      static_cast<unsigned long long>(metal.last_sprites_hud),
      static_cast<unsigned long long>(metal.last_sprites_distort),
      static_cast<unsigned long long>(metal.last_sprite_normal_submitted),
      static_cast<unsigned long long>(metal.last_sprite_glow_marked),
      static_cast<unsigned long long>(metal.last_sprite_glow_parsed),
      static_cast<unsigned long long>(metal.last_sprite_glow_accepted),
      static_cast<unsigned long long>(metal.last_sprite_glow_rejected),
      static_cast<unsigned long long>(metal.last_sprite_glow_invalid_records),
      static_cast<unsigned long long>(metal.last_sprite_glow_force_visible_drawn),
      static_cast<unsigned long long>(metal.last_sprite_glow_force_visible_submitted),
      static_cast<unsigned long long>(metal.last_sprite_glow_force_visible_draws),
      static_cast<unsigned long long>(metal.last_sprite_glow_force_visible_triangles),
      static_cast<unsigned long long>(metal.last_sprite_glow_force_visible_missing_textures),
      static_cast<unsigned long long>(metal.last_sprite_glow_skipped),
      static_cast<unsigned long long>(metal.last_sprite_draws),
      static_cast<unsigned long long>(metal.last_sprite_triangles),
      static_cast<unsigned long long>(metal.last_sprite_missing_textures),
      static_cast<unsigned long long>(metal.last_sprite_unsupported_bytes),
      static_cast<unsigned long long>(metal.last_sky_draw_draws),
      static_cast<unsigned long long>(metal.last_sky_draw_triangles),
      static_cast<unsigned long long>(metal.last_screen_filter_draws),
      static_cast<unsigned long long>(metal.last_screen_filter_triangles),
      static_cast<unsigned long long>(metal.last_debug_no_zbuf2_draws),
      static_cast<unsigned long long>(metal.last_debug_no_zbuf2_triangles));
}

void print_frame(const goal_jak2_metal_frame_summary& frame) {
  std::printf(
      "frame: %ux%u bytes=%llu hash=%016llx non-black-rgb=%llu nonzero-alpha=%llu "
      "max-alpha=%u\n",
      frame.width, frame.height, static_cast<unsigned long long>(frame.byte_count),
      static_cast<unsigned long long>(frame.hash),
      static_cast<unsigned long long>(frame.non_black_pixels),
      static_cast<unsigned long long>(frame.nonzero_alpha_pixels), frame.max_alpha);
}

bool exact_submission_gate(const goal_jak2_runtime_metrics& runtime,
                           const goal_jak2_metal_host_metrics& metal,
                           int expected_ticks,
                           bool require_presentation) {
  return runtime.ticks == static_cast<uint64_t>(expected_ticks) &&
         metal.chains == static_cast<uint64_t>(expected_ticks) &&
         goal_jak2_metal_host_metrics_pass_frame_gate(&metal, require_presentation);
}

bool wait_for_frame_while_pumping_events(goal_jak2_metal_host* host,
                                         bool require_presentation,
                                         bool* quit_requested) {
  if (!require_presentation) {
    return goal_jak2_metal_host_wait_for_last_frame(host, 5.0, 0) != 0;
  }

  // A presented CAMetalDrawable needs the main Cocoa event loop to keep advancing. Mirror the iOS
  // proof: wait for the retained presentation callback off-main while the app thread pumps SDL.
  auto waiter = std::async(std::launch::async, [host] {
    return goal_jak2_metal_host_wait_for_last_frame(host, 5.0, 1) != 0;
  });
  while (waiter.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    @autoreleasepool {
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (requests_quit(event)) {
          *quit_requested = true;
        }
      }
      SDL_Delay(1);
    }
  }
  return waiter.get();
}

}  // namespace

int main(int argc, char** argv) {
  @autoreleasepool {
    Options options;
    if (!parse_options(argc, argv, &options)) {
      return usage(argv[0]);
    }

    std::error_code saves_error;
    std::filesystem::create_directories(options.saves_dir, saves_error);
    if (saves_error) {
      std::fprintf(stderr, "could not create saves directory %s: %s\n", options.saves_dir.c_str(),
                   saves_error.message().c_str());
      return 1;
    }
    file_util::override_user_config_dir(fs::path(options.saves_dir), true);

    ProofResources resources;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      std::fprintf(stderr, "SDL video initialization failed: %s\n", SDL_GetError());
      return 1;
    }
    resources.sdl_initialized = true;
    if (options.interactive && !SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
      std::fprintf(stderr, "SDL gamepad initialization failed: %s\n", SDL_GetError());
      return 1;
    }

    SDL_WindowFlags flags = SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (options.hidden) {
      flags |= SDL_WINDOW_HIDDEN;
    }
    resources.window = SDL_CreateWindow("OpenGOAL Jak II Metal Runtime Proof", 960, 720, flags);
    if (!resources.window) {
      std::fprintf(stderr, "SDL Metal window creation failed: %s\n", SDL_GetError());
      return 1;
    }
    if (options.interactive) {
      if (!SDL_ShowWindow(resources.window) || !SDL_RaiseWindow(resources.window) ||
          !SDL_SyncWindow(resources.window)) {
        std::fprintf(stderr, "could not make the Jak II window interactive: %s\n", SDL_GetError());
        return 1;
      }
      refresh_gamepad(&resources);
    }
    resources.metal_view = SDL_Metal_CreateView(resources.window);
    if (!resources.metal_view) {
      std::fprintf(stderr, "SDL Metal view creation failed: %s\n", SDL_GetError());
      return 1;
    }
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(resources.metal_view);
    if (!layer) {
      std::fprintf(stderr, "SDL Metal view did not provide a CAMetalLayer\n");
      return 1;
    }

    resources.metal_host = goal_jak2_metal_host_create_presenting(layer);
    goal_gfx_host graphics_host = {};
    if (!resources.metal_host) {
      std::fprintf(stderr, "Jak II Metal host creation failed\n");
      return 1;
    }
    if (options.interactive &&
        !goal_jak2_metal_host_set_present_pacing(resources.metal_host, 1.0 / 60.0)) {
      std::fprintf(stderr, "Jak II Metal presentation pacing setup failed\n");
      return 1;
    }
    const std::string fr3_directory =
        (std::filesystem::path(options.data_dir) / "fr3").string();
    if (!goal_jak2_metal_host_configure_level_art(resources.metal_host,
                                                   fr3_directory.c_str())) {
      std::fprintf(stderr, "Jak II Metal level-art configuration failed: %s\n",
                   goal_jak2_metal_host_last_error(resources.metal_host));
      return 1;
    }
    if (!goal_jak2_metal_host_copy_gfx_host(resources.metal_host, &graphics_host)) {
      std::fprintf(stderr, "Jak II Metal host callback copy failed\n");
      return 1;
    }

    goal_jak2_runtime_config config = {};
    config.data_directory = options.data_dir.c_str();
    config.saves_directory = options.saves_dir.c_str();
    config.graphics = GOAL_JAK2_RUNTIME_GRAPHICS_EXTERNAL_HOST;
    config.external_gfx_host = &graphics_host;
    bool quit_requested = false;
    if (start_runtime_while_pumping_events(&config, &quit_requested) != GOAL_JAK2_RUNTIME_OK) {
      std::fprintf(stderr, "Jak II runtime start failed: %s\n", goal_jak2_runtime_last_error());
      return 1;
    }
    if (quit_requested) {
      return 0;
    }
    if (options.audio) {
      resources.audio_started = goalpad_audio::start();
      if (!resources.audio_started) {
        std::fprintf(stderr, "Jak II CoreAudio output was requested but could not start\n");
        return 1;
      }
    }

    goal_jak2_thread_suspend_probe probe = {};
    if (goal_jak2_runtime_probe_thread_suspend(&probe) != GOAL_JAK2_RUNTIME_OK) {
      std::fprintf(stderr, "Jak II thread-suspend probe failed: %s\n",
                   goal_jak2_runtime_last_error());
      return 1;
    }
    std::printf("thread-suspend: source=%08x/%llx hook=%08x/%llx matches=%d\n",
                probe.function_object, static_cast<unsigned long long>(probe.native_entry),
                probe.hook_function_object,
                static_cast<unsigned long long>(probe.hook_native_entry), probe.matches_expected);

    goal_jak2_runtime_metrics runtime = {};
    goal_jak2_metal_host_metrics metal = {};
    goal_jak2_metal_frame_summary frame = {};
    uint64_t baseline_hash = 0;
    uint64_t baseline_non_black_pixels = 0;
    bool have_baseline = false;
    bool saw_visible_title_frame = false;
    bool saw_attributed_title_sprite_frame = false;
    bool tick_failed = false;
    const bool require_presentation = options.require_presentation;
    bool suspended = false;
    bool need_presentation_confirmation = require_presentation;
    int pad_pushes = 0;
    const int pad_reads_before = goal_pad_read_count(0);
    int probe_press_read = -1;
    int probe_release_read = -1;
    auto pacing_deadline = std::chrono::steady_clock::now();
    auto health_started = pacing_deadline;
    uint64_t health_tick_start = 0;
    const auto frame_duration =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / 60.0));

    for (int tick = 0;
         (options.maximum_ticks == 0 || tick < options.maximum_ticks) && !quit_requested; ++tick) {
      @autoreleasepool {
        pump_events(&quit_requested);
        while (options.interactive && !quit_requested &&
               !window_presentable(resources.window)) {
          if (!suspended) {
            std::printf("lifecycle: suspended while the window is not presentable\n");
            if (resources.audio_started) {
              goalpad_audio::stop();
              resources.audio_started = false;
            }
            suspended = true;
          }
          SDL_Delay(10);
          pump_events(&quit_requested);
        }
        if (suspended && !quit_requested) {
          if (options.audio && !goalpad_audio::start()) {
            std::fprintf(stderr, "Jak II CoreAudio output could not resume\n");
            tick_failed = true;
            break;
          }
          resources.audio_started = options.audio;
          need_presentation_confirmation = require_presentation;
          pacing_deadline = std::chrono::steady_clock::now();
          health_started = pacing_deadline;
          health_tick_start = runtime.ticks;
          suspended = false;
          std::printf("lifecycle: resumed\n");
        }
        if (quit_requested) {
          break;
        }

        const int current_pad_reads = goal_pad_read_count(0);
        if (options.probe_pad) {
          if (probe_press_read < 0 && current_pad_reads > pad_reads_before) {
            probe_press_read = current_pad_reads;
          } else if (probe_press_read >= 0 && probe_release_read < 0 &&
                     current_pad_reads > probe_press_read) {
            probe_release_read = current_pad_reads;
          }
        }
        goal_pad_state pad;
        goal_pad_state_neutral(&pad);
        if (options.interactive) {
          pad = read_pad(&resources);
        } else if (options.probe_pad && probe_press_read < 0) {
          pad.buttons = GOAL_PAD_L3;
        }
        if (options.interactive || options.probe_pad) {
          if (goal_pad_set_state(0, &pad) != GOAL_KERNEL_CORE_OK) {
            std::fprintf(stderr, "could not push Jak II pad state\n");
            tick_failed = true;
            break;
          }
          pad_pushes++;
        }

        const uint64_t previous_chains = metal.chains;
        const goal_jak2_runtime_status tick_result = goal_jak2_runtime_tick();
        if (tick_result != GOAL_JAK2_RUNTIME_OK) {
          std::fprintf(stderr, "Jak II runtime tick %d failed (%d): %s\n", tick + 1, tick_result,
                       goal_jak2_runtime_last_error());
          tick_failed = true;
          break;
        }
        if (goal_jak2_runtime_get_metrics(&runtime) != GOAL_JAK2_RUNTIME_OK ||
            !goal_jak2_metal_host_get_metrics(resources.metal_host, &metal)) {
          std::fprintf(stderr, "could not copy Jak II runtime/Metal metrics\n");
          tick_failed = true;
          break;
        }

        if (metal.chains > previous_chains &&
            (metal.failed_chains != 0 || metal.completed_chains != metal.chains)) {
          std::fprintf(stderr, "Jak II Metal chain failed before frame wait: %s\n",
                       goal_jak2_metal_host_last_error(resources.metal_host));
          print_runtime_metrics(runtime);
          print_metal_metrics(metal);
          tick_failed = true;
          break;
        }
        const bool confirm_presentation =
            require_presentation && (!options.interactive || need_presentation_confirmation);
        if (metal.chains > previous_chains &&
            !wait_for_frame_while_pumping_events(resources.metal_host, confirm_presentation,
                                                 &quit_requested)) {
          std::fprintf(stderr, "Jak II Metal frame wait failed: %s\n",
                       goal_jak2_metal_host_last_error(resources.metal_host));
          if (goal_jak2_metal_host_get_metrics(resources.metal_host, &metal)) {
            print_runtime_metrics(runtime);
            print_metal_metrics(metal);
          }
          if (goal_jak2_metal_host_read_last_frame(resources.metal_host, &frame)) {
            print_frame(frame);
          }
          tick_failed = true;
          break;
        }
        if (confirm_presentation) {
          need_presentation_confirmation = false;
        }
        if (!goal_jak2_metal_host_get_metrics(resources.metal_host, &metal) ||
            !goal_jak2_metal_host_read_last_frame(resources.metal_host, &frame)) {
          std::fprintf(stderr, "could not copy the completed Jak II Metal frame\n");
          tick_failed = true;
          break;
        }

        if (options.interactive && options.report_pad) {
          report_pad_change(pad);
        }
        if (options.interactive) {
          if (tick == 0 || (tick + 1) % 300 == 0) {
            const auto now = std::chrono::steady_clock::now();
            const double health_seconds =
                std::chrono::duration<double>(now - health_started).count();
            const uint64_t health_ticks = runtime.ticks - health_tick_start;
            const double health_fps = health_seconds > 0.0 ? health_ticks / health_seconds : 0.0;
            std::printf(
                "health: ticks=%llu title=%d chains=%llu/%llu failed=%llu present=%llu "
                "draws=%llu tris=%llu pad-reads=%d fps=%.2f frame=%016llx non-black=%llu\n",
                static_cast<unsigned long long>(runtime.ticks), runtime.title_ready,
                static_cast<unsigned long long>(metal.completed_chains),
                static_cast<unsigned long long>(metal.chains),
                static_cast<unsigned long long>(metal.failed_chains),
                static_cast<unsigned long long>(metal.presentations),
                static_cast<unsigned long long>(metal.draws),
                static_cast<unsigned long long>(metal.triangles),
                goal_pad_read_count(0), health_fps,
                static_cast<unsigned long long>(frame.hash),
                static_cast<unsigned long long>(frame.non_black_pixels));
            health_started = now;
            health_tick_start = runtime.ticks;
          }
        } else {
          std::printf("\n-- tick %d --\n", tick + 1);
          print_runtime_metrics(runtime);
          print_metal_metrics(metal);
          print_frame(frame);
        }
        if (!have_baseline) {
          baseline_hash = frame.hash;
          baseline_non_black_pixels = frame.non_black_pixels;
          have_baseline = true;
        } else {
          saw_visible_title_frame |=
              runtime.title_ready != 0 && frame.hash != baseline_hash &&
              frame.non_black_pixels > baseline_non_black_pixels && metal.draws != 0 &&
              metal.triangles != 0;
          const bool exact_sky_frame = metal.last_sky_draw_draws == 1 &&
                                       metal.last_sky_draw_triangles == 2 &&
                                       metal.last_sky_draw_batch_valid != 0;
          bool bounded_sprite_upload =
              metal.last_sprite_texture_upload.valid != 0 &&
              metal.last_sprite_texture_upload.present != 0 &&
              metal.last_sprite_texture_upload.upload_count > 0 &&
              metal.last_sprite_texture_upload.upload_count <=
                  GOAL_JAK2_SPRITE_TEXTURE_UPLOAD_MAX_GROUPS &&
              metal.sprite_texture_uploads >= metal.last_sprite_texture_upload.upload_count;
          for (uint32_t i = 0;
               bounded_sprite_upload && i < metal.last_sprite_texture_upload.upload_count; ++i) {
            bounded_sprite_upload = metal.last_sprite_texture_upload.pages[i] != 0 &&
                                    metal.last_sprite_texture_upload.modes[i] == -1;
          }
          const bool exact_visibility_glow_frame =
              metal.last_sprites_2d == 64 && metal.last_sprites_3d == 0 &&
              metal.last_sprites_hud == 0 && metal.last_sprites_distort == 0 &&
              metal.last_sprite_normal_submitted == 60 && metal.last_sprite_glow_marked == 4 &&
              metal.last_sprite_glow_parsed == 4 && metal.last_sprite_glow_accepted == 4 &&
              metal.last_sprite_glow_rejected == 0 &&
              metal.last_sprite_glow_invalid_records == 0 &&
              metal.last_sprite_glow_force_visible_submitted == 4 &&
              metal.last_sprite_glow_force_visible_drawn == 4 &&
              metal.last_sprite_glow_force_visible_draws == 4 &&
              metal.last_sprite_glow_force_visible_triangles == 8 &&
              metal.last_sprite_glow_force_visible_missing_textures == 0 &&
              metal.last_sprite_glow_skipped == 0 && metal.last_sprite_draws == 12 &&
              metal.last_sprite_triangles == 176 &&
              metal.last_sprite_missing_textures == 0;
          const bool exact_draw_attribution =
              metal.draws == metal.last_sky_draw_draws + metal.last_screen_filter_draws +
                                 metal.last_debug_no_zbuf2_draws + metal.last_sprite_draws &&
              metal.triangles ==
                  metal.last_sky_draw_triangles + metal.last_screen_filter_triangles +
                      metal.last_debug_no_zbuf2_triangles + metal.last_sprite_triangles;
          saw_attributed_title_sprite_frame |=
              exact_sky_frame && bounded_sprite_upload && exact_visibility_glow_frame &&
              exact_draw_attribution && frame.hash != baseline_hash &&
              frame.non_black_pixels > baseline_non_black_pixels;
        }
        if (options.interactive) {
          pacing_deadline += frame_duration;
          const auto now = std::chrono::steady_clock::now();
          if (pacing_deadline > now) {
            std::this_thread::sleep_until(pacing_deadline);
          } else if (now - pacing_deadline > frame_duration * 4) {
            pacing_deadline = now;
          }
        }
      }
    }

    const int pad_reads_after = goal_pad_read_count(0);
    if (options.probe_pad && probe_press_read < 0 && pad_reads_after > pad_reads_before) {
      probe_press_read = pad_reads_after;
    } else if (options.probe_pad && probe_press_read >= 0 && probe_release_read < 0 &&
               pad_reads_after > probe_press_read) {
      probe_release_read = pad_reads_after;
    }
    const bool pad_probe_passed = !options.probe_pad ||
                                  (pad_pushes == options.maximum_ticks &&
                                   probe_press_read > pad_reads_before &&
                                   probe_release_read > probe_press_read);
    if (options.probe_pad) {
      std::printf(
          "pad-probe: pushes=%d reads=%d->%d press-read=%d release-read=%d transitioned=%d\n",
          pad_pushes, pad_reads_before, pad_reads_after, probe_press_read, probe_release_read,
          pad_probe_passed);
    }

    if (options.interactive) {
      const bool healthy = !tick_failed && runtime.title_ready != 0 && metal.failed_chains == 0 &&
                           metal.presentations != 0 && saw_visible_title_frame &&
                           pad_reads_after > pad_reads_before;
      if (healthy) {
        std::printf("STOP: interactive Jak II runtime ended after %llu ticks.\n",
                    static_cast<unsigned long long>(runtime.ticks));
      }
      return healthy ? 0 : 1;
    }

    const bool passed = !quit_requested && !tick_failed && runtime.title_ready != 0 &&
                        exact_submission_gate(runtime, metal, options.maximum_ticks,
                                              require_presentation) &&
                        saw_visible_title_frame && pad_probe_passed;
    if (passed) {
      std::printf("PASS: bounded Jak II AOT runtime produced a changing non-black title frame.\n");
      return 0;
    }

    std::fprintf(
        stderr,
        "INCOMPLETE: exact=%d title=%d visible-title=%d attributed-title-sprites=%d "
        "baseline-non-black=%llu last-non-black=%llu quit=%d tick-failed=%d\n",
        exact_submission_gate(runtime, metal, options.maximum_ticks, require_presentation),
        runtime.title_ready, saw_visible_title_frame,
        saw_attributed_title_sprite_frame,
        static_cast<unsigned long long>(baseline_non_black_pixels),
        static_cast<unsigned long long>(frame.non_black_pixels), quit_requested, tick_failed);
    return 1;
  }
}
