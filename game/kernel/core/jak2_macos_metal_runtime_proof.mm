/*!
 * @file jak2_macos_metal_runtime_proof.mm
 * Bounded macOS host for the Jak 2 AOT runtime and its external Metal renderer.
 *
 * This is a development proof, not the desktop OpenGOAL runtime. It uses the portable signed-code
 * path shared with iPadOS, presents a bounded number of real DMA chains, and reports the first renderer
 * boundary that is still incomplete.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>

#include "common/util/FileUtil.h"

#include "game/graphics/pipelines/metal/metal_jak2_host_bridge.h"
#include "game/kernel/core/gfx_host.h"
#include "game/kernel/core/jak2_runtime.h"

#include "third-party/SDL/include/SDL3/SDL.h"

#import <QuartzCore/CAMetalLayer.h>

namespace {

struct Options {
  std::string data_dir;
  std::string saves_dir = "/private/tmp/goalpad-jak2-macos-saves";
  int maximum_ticks = 3;
  bool hidden = false;
  bool require_presentation = false;
};

int usage(const char* program) {
  std::fprintf(
      stderr,
      "usage: %s --data-dir <prepared-jak2-dir> [--saves-dir <dir>] [--ticks <1-1200>] "
      "[--hidden] [--require-presentation]\n"
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
      if (out->maximum_ticks < 1 || out->maximum_ticks > 1200) {
        return false;
      }
    } else if (arg == "--hidden") {
      out->hidden = true;
    } else if (arg == "--require-presentation") {
      out->require_presentation = true;
    } else {
      return false;
    }
  }
  return !out->data_dir.empty() && !out->saves_dir.empty();
}

struct ProofResources {
  bool sdl_initialized = false;
  SDL_Window* window = nullptr;
  SDL_MetalView metal_view = nullptr;
  goal_jak2_metal_host* metal_host = nullptr;

  ~ProofResources() {
    // The runtime retains copied callbacks into metal_host. It must always release them first.
    goal_jak2_runtime_shutdown();
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
      "force-visible-drawn/submitted=%llu/%llu draws=%llu tris=%llu missing=%llu skipped=%llu) "
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
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
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
    if (goal_jak2_runtime_start(&config) != GOAL_JAK2_RUNTIME_OK) {
      std::fprintf(stderr, "Jak II runtime start failed: %s\n", goal_jak2_runtime_last_error());
      return 1;
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
    bool saw_attributed_title_sprite_frame = false;
    bool quit_requested = false;
    bool tick_failed = false;
    const bool require_presentation = options.require_presentation;

    for (int tick = 0; tick < options.maximum_ticks && !quit_requested; ++tick) {
      @autoreleasepool {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
          if (event.type == SDL_EVENT_QUIT ||
              event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            quit_requested = true;
          }
        }
        if (quit_requested) {
          break;
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
        if (metal.chains > previous_chains &&
            !wait_for_frame_while_pumping_events(resources.metal_host, require_presentation,
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
        if (!goal_jak2_metal_host_get_metrics(resources.metal_host, &metal) ||
            !goal_jak2_metal_host_read_last_frame(resources.metal_host, &frame)) {
          std::fprintf(stderr, "could not copy the completed Jak II Metal frame\n");
          tick_failed = true;
          break;
        }

        std::printf("\n-- tick %d --\n", tick + 1);
        print_runtime_metrics(runtime);
        print_metal_metrics(metal);
        print_frame(frame);
        if (!have_baseline) {
          baseline_hash = frame.hash;
          baseline_non_black_pixels = frame.non_black_pixels;
          have_baseline = true;
        } else {
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
          const bool exact_force_visible_glow_frame =
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
              metal.last_sprite_glow_skipped == 0 && metal.last_sprite_draws == 6 &&
              metal.last_sprite_triangles == 128 &&
              metal.last_sprite_missing_textures == 0;
          const bool exact_draw_attribution =
              metal.draws == metal.last_sky_draw_draws + metal.last_screen_filter_draws +
                                 metal.last_debug_no_zbuf2_draws + metal.last_sprite_draws &&
              metal.triangles ==
                  metal.last_sky_draw_triangles + metal.last_screen_filter_triangles +
                      metal.last_debug_no_zbuf2_triangles + metal.last_sprite_triangles;
          saw_attributed_title_sprite_frame |=
              exact_sky_frame && bounded_sprite_upload && exact_force_visible_glow_frame &&
              exact_draw_attribution && frame.hash != baseline_hash &&
              frame.non_black_pixels > baseline_non_black_pixels;
        }
      }
    }

    const bool passed = !quit_requested && !tick_failed && runtime.title_ready != 0 &&
                        exact_submission_gate(runtime, metal, options.maximum_ticks,
                                              require_presentation) &&
                        saw_attributed_title_sprite_frame;
    if (passed) {
      std::printf("PASS: bounded Jak II AOT runtime produced an attributed non-black Metal frame.\n");
      return 0;
    }

    std::fprintf(
        stderr,
        "INCOMPLETE: exact=%d title=%d attributed-title-sprites=%d baseline-non-black=%llu "
        "last-non-black=%llu quit=%d tick-failed=%d\n",
        exact_submission_gate(runtime, metal, options.maximum_ticks, require_presentation),
        runtime.title_ready,
        saw_attributed_title_sprite_frame,
        static_cast<unsigned long long>(baseline_non_black_pixels),
        static_cast<unsigned long long>(frame.non_black_pixels), quit_requested, tick_failed);
    return 1;
  }
}
