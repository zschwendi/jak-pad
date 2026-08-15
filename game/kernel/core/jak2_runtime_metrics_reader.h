#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "common/goal_constants.h"
#include "common/util/fnv.h"
#include "game/kernel/core/jak2_runtime.h"

namespace jak2_runtime_metrics_reader {

namespace layout {

// Type offsets in decompiler/config/jak2/all-types.gc include a basic object's four-byte type tag.
// Live GOAL basic pointers are BASIC_OFFSET bytes past that tag. Structure fields keep their type
// offsets because structure pointers have no tag bias.
constexpr std::size_t live_basic_offset(std::size_t asserted_offset) {
  return asserted_offset - BASIC_OFFSET;
}

// process and clock.
constexpr std::size_t kProcessState = live_basic_offset(64);
constexpr std::size_t kClockFrameCounter = live_basic_offset(24);

// display and game-info.
constexpr std::size_t kDisplayBaseClock = live_basic_offset(48);
constexpr std::size_t kGameInfoBlackoutTime = live_basic_offset(392);

// setting-control::user-current is an inline structure. user-setting-data's six contiguous
// background floats occupy offsets 76 through 99; movie and spooling are tracked pointer fields.
constexpr std::size_t kSettingUserCurrent = live_basic_offset(16);
constexpr std::size_t kUserMovie = 40;
constexpr std::size_t kUserSpooling = 48;
constexpr std::size_t kUserBackgroundAlpha = 88;
constexpr std::size_t kUserBackgroundAlphaForce = 96;

// scene-player and scene.
constexpr std::size_t kScenePlayerSkeleton = live_basic_offset(140);
constexpr std::size_t kScenePlayerSceneList = live_basic_offset(200);
constexpr std::size_t kScenePlayerScene = live_basic_offset(204);
constexpr std::size_t kScenePlayerSceneIndex = live_basic_offset(208);
constexpr std::size_t kScenePlayerAnimation = live_basic_offset(212);
constexpr std::size_t kScenePlayerNextAnimation = live_basic_offset(216);
constexpr std::size_t kScenePlayerSceneStartTime = live_basic_offset(312);
constexpr std::size_t kSceneEntity = live_basic_offset(36);
constexpr std::size_t kSceneArtGroup = live_basic_offset(40);
constexpr std::size_t kSceneAnimation = live_basic_offset(44);

// Built-in array/string layouts come from common/type_system/TypeSystem.cpp and
// game/kernel/common/kscheme.h. Their live pointers are also past the basic-object type tag.
constexpr std::size_t kArrayLength = live_basic_offset(4);
constexpr std::size_t kArrayData = sizeof(uint32_t);
constexpr std::size_t kStringLength = 0;
constexpr std::size_t kStringData = sizeof(uint32_t);

constexpr uint32_t kSceneActorFields = 4;
constexpr uint32_t kSceneActorFlags = 0;
constexpr uint32_t kSceneActorLevelIndex = 1;
constexpr uint32_t kSceneActorSkeletonStatus = 2;
constexpr uint32_t kSceneActorMercJointCount = 3;

// joint-control is basic; joint-control-channel is a structure. The art-joint-anim artist values
// are the tracked floats at offsets 24 and 28 in all-types.gc.
constexpr std::size_t kJointControlStatus = live_basic_offset(4);
constexpr std::size_t kJointControlActiveChannels = live_basic_offset(7);
constexpr std::size_t kJointControlRootChannel = live_basic_offset(16);
constexpr std::size_t kChannelFrameGroup = 4;
constexpr std::size_t kChannelFrameNumber = 8;
constexpr std::size_t kAnimationArtistBase = live_basic_offset(24);
constexpr std::size_t kAnimationArtistStep = live_basic_offset(28);

static_assert(kProcessState == 60);
static_assert(kClockFrameCounter == 20);
static_assert(kDisplayBaseClock == 44);
static_assert(kGameInfoBlackoutTime == 388);
static_assert(kSettingUserCurrent + kUserBackgroundAlpha == 100);
static_assert(kSettingUserCurrent + kUserBackgroundAlphaForce == 108);
static_assert(kScenePlayerSceneIndex == 204);
static_assert(kJointControlActiveChannels == 3);

}  // namespace layout

struct MemoryView {
  const uint8_t* data = nullptr;
  std::size_t size = 0;
  uint32_t false_object = 0;

  bool is_object(uint32_t address) const { return address != 0 && address != false_object; }

  bool span_fits(uint64_t address, std::size_t bytes) const {
    return data && address <= size && bytes <= size - static_cast<std::size_t>(address);
  }

  template <typename T>
  bool read(uint32_t object, std::size_t offset, T* out) const {
    if (!out || !is_object(object)) {
      return false;
    }
    const uint64_t address = static_cast<uint64_t>(object) + offset;
    if (!span_fits(address, sizeof(T))) {
      return false;
    }
    std::memcpy(out, data + address, sizeof(T));
    return true;
  }

  bool copy_string(uint32_t string, char* out, std::size_t out_size) const {
    if (!out || out_size == 0) {
      return false;
    }
    out[0] = '\0';
    uint32_t length = 0;
    if (!read(string, layout::kStringLength, &length)) {
      return false;
    }
    const uint64_t data_address = static_cast<uint64_t>(string) + layout::kStringData;
    if (!span_fits(data_address, length)) {
      return false;
    }
    const std::size_t copied = std::min<std::size_t>(length, out_size - 1);
    std::memcpy(out, data + data_address, copied);
    out[copied] = '\0';
    return true;
  }

  bool hash_string(uint32_t string, uint64_t* out) const {
    if (!out) {
      return false;
    }
    *out = 0;
    uint32_t length = 0;
    if (!read(string, layout::kStringLength, &length)) {
      return false;
    }
    const uint64_t data_address = static_cast<uint64_t>(string) + layout::kStringData;
    if (!span_fits(data_address, length)) {
      return false;
    }
    *out = fnv64(data + data_address, length);
    return true;
  }

  bool array_has_length(uint32_t array, uint32_t required) const {
    uint32_t length = 0;
    return read(array, layout::kArrayLength, &length) && length >= required;
  }

  template <typename T>
  bool read_array(uint32_t array, uint32_t index, T* out) const {
    uint32_t length = 0;
    if (!read(array, layout::kArrayLength, &length) || index >= length) {
      return false;
    }
    return read(array, layout::kArrayData + static_cast<std::size_t>(index) * sizeof(T), out);
  }
};

struct Inputs {
  uint32_t display = 0;
  uint32_t game_info = 0;
  uint32_t setting_control = 0;
  uint32_t scene_player = 0;
  uint32_t scene_actor_sequence = 0;
  uint32_t scene_actor_scene_name = 0;
  uint32_t scene_actor_count = 0;
  uint32_t scene_actor_total_count = 0;
  uint32_t scene_actor_data = 0;
};

struct SceneActorDiagnostics {
  bool valid = false;
  uint32_t sequence = 0;
  uint64_t scene_name_hash = 0;
  int32_t count = 0;
  int32_t total_count = 0;
  bool overflow = false;
  std::array<goal_jak2_scene_actor_diagnostic, GOAL_JAK2_SCENE_ACTOR_DIAGNOSTIC_MAX> actors = {};
};

struct Snapshot {
  bool display_timing_valid = false;
  int64_t display_base_frame_counter = 0;
  int64_t blackout_time = 0;
  int64_t blackout_remaining = 0;

  bool settings_valid = false;
  float background_alpha = 0.f;
  float background_alpha_force = 0.f;
  uint32_t movie_process = 0;
  uint32_t spooling_process = 0;

  bool scene_valid = false;
  bool scene_identity_valid = false;
  uint32_t scene_list = 0;
  int32_t scene_list_length = 0;
  uint32_t scene = 0;
  int32_t scene_index = 0;
  uint32_t animation = 0;
  uint32_t next_animation = 0;
  int64_t scene_start_time = 0;
  int64_t scene_elapsed = 0;
  std::array<char, 48> scene_entity = {};
  std::array<char, 48> scene_art_group = {};
  std::array<char, 64> scene_animation = {};

  bool skeleton_valid = false;
  uint16_t skeleton_status = 0;
  uint8_t active_channels = 0;

  bool animation_valid = false;
  uint32_t animation_frame_group = 0;
  float animation_frame = 0.f;
  float animation_aframe = 0.f;

  SceneActorDiagnostics scene_actors;
};

constexpr uint32_t kMercPrisLevel0Bucket = 197;
constexpr uint32_t kMercPrisLevelStride = 4;
constexpr uint32_t kMercPrisLevelCount = 7;

constexpr uint32_t merc_pris_bucket(uint32_t level_index) {
  if (level_index >= kMercPrisLevelCount) {
    return std::numeric_limits<uint32_t>::max();
  }
  return kMercPrisLevel0Bucket + kMercPrisLevelStride * level_index;
}

static_assert(kMercPrisLevelCount == jak2::LEVEL_TOTAL);
static_assert(merc_pris_bucket(0) == 197);
static_assert(merc_pris_bucket(1) == 201);
static_assert(merc_pris_bucket(6) == 221);
static_assert(merc_pris_bucket(7) == std::numeric_limits<uint32_t>::max());

inline void retain_scene_actor_diagnostics(SceneActorDiagnostics* retained,
                                           const SceneActorDiagnostics& sample) {
  if (!retained || sample.sequence == 0) {
    return;
  }
  if (sample.sequence != retained->sequence) {
    *retained = {};
    retained->sequence = sample.sequence;
  }
  if (sample.valid) {
    *retained = sample;
  }
}

inline int64_t saturating_subtract(int64_t left, int64_t right) {
  if (right > 0 && left < std::numeric_limits<int64_t>::min() + right) {
    return std::numeric_limits<int64_t>::min();
  }
  if (right < 0 && left > std::numeric_limits<int64_t>::max() + right) {
    return std::numeric_limits<int64_t>::max();
  }
  return left - right;
}

inline Snapshot read(const MemoryView& memory, const Inputs& inputs) {
  Snapshot out;

  uint32_t base_clock = 0;
  if (memory.read(inputs.display, layout::kDisplayBaseClock, &base_clock) &&
      memory.read(base_clock, layout::kClockFrameCounter, &out.display_base_frame_counter) &&
      memory.read(inputs.game_info, layout::kGameInfoBlackoutTime, &out.blackout_time)) {
    out.display_timing_valid = true;
    // Jak II's game paths remain blacked out while base-clock is less than blackout-time.
    out.blackout_remaining = std::max<int64_t>(
        0, saturating_subtract(out.blackout_time, out.display_base_frame_counter));
  }

  const std::size_t user = layout::kSettingUserCurrent;
  if (memory.read(inputs.setting_control, user + layout::kUserMovie, &out.movie_process) &&
      memory.read(inputs.setting_control, user + layout::kUserSpooling, &out.spooling_process) &&
      memory.read(inputs.setting_control, user + layout::kUserBackgroundAlpha,
                  &out.background_alpha) &&
      memory.read(inputs.setting_control, user + layout::kUserBackgroundAlphaForce,
                  &out.background_alpha_force)) {
    out.settings_valid = true;
  }

  if (memory.read(inputs.scene_player, layout::kScenePlayerSceneList, &out.scene_list) &&
      memory.read(inputs.scene_player, layout::kScenePlayerScene, &out.scene) &&
      memory.read(inputs.scene_player, layout::kScenePlayerSceneIndex, &out.scene_index) &&
      memory.read(inputs.scene_player, layout::kScenePlayerAnimation, &out.animation) &&
      memory.read(inputs.scene_player, layout::kScenePlayerNextAnimation, &out.next_animation) &&
      memory.read(inputs.scene_player, layout::kScenePlayerSceneStartTime, &out.scene_start_time) &&
      memory.read(out.scene_list, layout::kArrayLength, &out.scene_list_length)) {
    out.scene_valid = true;
    if (out.display_timing_valid) {
      out.scene_elapsed = saturating_subtract(out.display_base_frame_counter, out.scene_start_time);
    }

    uint32_t entity = 0;
    uint32_t art_group = 0;
    uint32_t animation = 0;
    out.scene_identity_valid =
        memory.read(out.scene, layout::kSceneEntity, &entity) &&
        memory.read(out.scene, layout::kSceneArtGroup, &art_group) &&
        memory.read(out.scene, layout::kSceneAnimation, &animation) &&
        memory.copy_string(entity, out.scene_entity.data(), out.scene_entity.size()) &&
        memory.copy_string(art_group, out.scene_art_group.data(), out.scene_art_group.size()) &&
        memory.copy_string(animation, out.scene_animation.data(), out.scene_animation.size());
  }

  uint32_t skeleton = 0;
  if (memory.read(inputs.scene_player, layout::kScenePlayerSkeleton, &skeleton) &&
      memory.read(skeleton, layout::kJointControlStatus, &out.skeleton_status) &&
      memory.read(skeleton, layout::kJointControlActiveChannels, &out.active_channels)) {
    out.skeleton_valid = true;

    uint32_t root_channel = 0;
    if (out.active_channels > 0 &&
        memory.read(skeleton, layout::kJointControlRootChannel, &root_channel) &&
        memory.read(root_channel, layout::kChannelFrameGroup, &out.animation_frame_group) &&
        memory.read(root_channel, layout::kChannelFrameNumber, &out.animation_frame) &&
        memory.is_object(out.animation_frame_group)) {
      float artist_base = 0.f;
      float artist_step = 0.f;
      if (memory.read(out.animation_frame_group, layout::kAnimationArtistBase, &artist_base) &&
          memory.read(out.animation_frame_group, layout::kAnimationArtistStep, &artist_step)) {
        const float aframe = out.animation_frame * artist_step + artist_base;
        if (std::isfinite(out.animation_frame) && std::isfinite(artist_base) &&
            std::isfinite(artist_step) && std::isfinite(aframe)) {
          out.animation_valid = true;
          out.animation_aframe = aframe;
        }
      }
    }
  }

  out.scene_actors.sequence = inputs.scene_actor_sequence;
  constexpr uint32_t kActorDataCount =
      GOAL_JAK2_SCENE_ACTOR_DIAGNOSTIC_MAX * layout::kSceneActorFields;
  const bool actor_array_valid = memory.array_has_length(inputs.scene_actor_data, kActorDataCount);
  const bool actor_counts_valid =
      inputs.scene_actor_sequence != 0 &&
      inputs.scene_actor_count <= GOAL_JAK2_SCENE_ACTOR_DIAGNOSTIC_MAX &&
      inputs.scene_actor_total_count >= inputs.scene_actor_count;
  uint64_t scene_name_hash = 0;
  if (actor_array_valid && actor_counts_valid &&
      memory.hash_string(inputs.scene_actor_scene_name, &scene_name_hash)) {
    SceneActorDiagnostics diagnostics;
    diagnostics.sequence = inputs.scene_actor_sequence;
    diagnostics.scene_name_hash = scene_name_hash;
    diagnostics.count = static_cast<int32_t>(inputs.scene_actor_count);
    diagnostics.total_count = static_cast<int32_t>(inputs.scene_actor_total_count);
    diagnostics.overflow = inputs.scene_actor_total_count > inputs.scene_actor_count;
    bool actors_valid = true;
    for (uint32_t index = 0; index < inputs.scene_actor_count; ++index) {
      auto& actor = diagnostics.actors[index];
      const uint32_t data = index * layout::kSceneActorFields;
      actors_valid =
          actors_valid &&
          memory.read_array(inputs.scene_actor_data, data + layout::kSceneActorFlags,
                            &actor.flags) &&
          memory.read_array(inputs.scene_actor_data, data + layout::kSceneActorLevelIndex,
                            &actor.level_index) &&
          memory.read_array(inputs.scene_actor_data, data + layout::kSceneActorSkeletonStatus,
                            &actor.skeleton_status) &&
          memory.read_array(inputs.scene_actor_data, data + layout::kSceneActorMercJointCount,
                            &actor.merc_joint_count);
      const uint32_t bucket_flags = GOAL_JAK2_SCENE_ACTOR_DRAW_CONTROL |
                                    GOAL_JAK2_SCENE_ACTOR_MERC_GEOMETRY;
      actor.merc_pris_bucket =
          (actor.flags & bucket_flags) == bucket_flags
              ? merc_pris_bucket(actor.level_index)
              : std::numeric_limits<uint32_t>::max();
    }
    if (actors_valid) {
      diagnostics.valid = true;
      out.scene_actors = diagnostics;
    }
  }

  return out;
}

}  // namespace jak2_runtime_metrics_reader
