#include "game/kernel/core/jak2_runtime_metrics_reader.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {

using namespace jak2_runtime_metrics_reader;

int g_failures = 0;

void expect(bool condition, const char* description) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", description);
  if (!condition) {
    g_failures++;
  }
}

template <typename T, std::size_t Size>
void write(std::array<uint8_t, Size>& memory, uint32_t address, T value) {
  std::memcpy(memory.data() + address, &value, sizeof(value));
}

template <std::size_t Size>
void write_string(std::array<uint8_t, Size>& memory, uint32_t address, const char* value) {
  const uint32_t length = static_cast<uint32_t>(std::strlen(value));
  write(memory, address, length);
  std::memcpy(memory.data() + address + sizeof(length), value, length);
}

void reads_asserted_jak2_scene_layout() {
  std::array<uint8_t, 0x4000> bytes = {};
  constexpr uint32_t kFalse = 4;
  constexpr uint32_t kDisplay = 0x100;
  constexpr uint32_t kBaseClock = 0x200;
  constexpr uint32_t kGameInfo = 0x300;
  constexpr uint32_t kSettings = 0x600;
  constexpr uint32_t kScenePlayer = 0x1200;
  constexpr uint32_t kSceneList = 0x1700;
  constexpr uint32_t kScene = 0x1800;
  constexpr uint32_t kSkeleton = 0x1a00;
  constexpr uint32_t kChannel = 0x1b00;
  constexpr uint32_t kFrameGroup = 0x1c00;
  constexpr uint32_t kEntity = 0x2000;
  constexpr uint32_t kArtGroup = 0x2100;
  constexpr uint32_t kSceneAnimation = 0x2200;

  write(bytes, kDisplay + layout::kDisplayBaseClock, kBaseClock);
  write<int64_t>(bytes, kBaseClock + layout::kClockFrameCounter, 1510);
  write<int64_t>(bytes, kGameInfo + layout::kGameInfoBlackoutTime, 1600);

  const std::size_t user = layout::kSettingUserCurrent;
  write(bytes, kSettings + user + layout::kUserMovie, uint32_t{0x2500});
  write(bytes, kSettings + user + layout::kUserSpooling, uint32_t{0x2600});
  write(bytes, kSettings + user + layout::kUserBackgroundAlpha, 0.25f);
  write(bytes, kSettings + user + layout::kUserBackgroundAlphaForce, 0.75f);

  write(bytes, kScenePlayer + layout::kScenePlayerSceneList, kSceneList);
  write(bytes, kScenePlayer + layout::kScenePlayerScene, kScene);
  write<int32_t>(bytes, kScenePlayer + layout::kScenePlayerSceneIndex, 2);
  write(bytes, kScenePlayer + layout::kScenePlayerAnimation, uint32_t{0x2700});
  write(bytes, kScenePlayer + layout::kScenePlayerNextAnimation, uint32_t{0x2800});
  write<int64_t>(bytes, kScenePlayer + layout::kScenePlayerSceneStartTime, 1400);
  write<int32_t>(bytes, kSceneList + layout::kArrayLength, 7);
  write(bytes, kScene + layout::kSceneEntity, kEntity);
  write(bytes, kScene + layout::kSceneArtGroup, kArtGroup);
  write(bytes, kScene + layout::kSceneAnimation, kSceneAnimation);
  write_string(bytes, kEntity, "title-actor");
  write_string(bytes, kArtGroup, "title-ag");
  write_string(bytes, kSceneAnimation, "title-disk-intro");

  write(bytes, kScenePlayer + layout::kScenePlayerSkeleton, kSkeleton);
  write<uint16_t>(bytes, kSkeleton + layout::kJointControlStatus, 0x23);
  write<uint8_t>(bytes, kSkeleton + layout::kJointControlActiveChannels, 2);
  write(bytes, kSkeleton + layout::kJointControlRootChannel, kChannel);
  write(bytes, kChannel + layout::kChannelFrameGroup, kFrameGroup);
  write(bytes, kChannel + layout::kChannelFrameNumber, 12.5f);
  write(bytes, kFrameGroup + layout::kAnimationArtistBase, 100.f);
  write(bytes, kFrameGroup + layout::kAnimationArtistStep, 2.f);

  const Snapshot snapshot =
      read({bytes.data(), bytes.size(), kFalse}, {kDisplay, kGameInfo, kSettings, kScenePlayer});
  expect(snapshot.display_timing_valid && snapshot.display_base_frame_counter == 1510 &&
             snapshot.blackout_time == 1600 && snapshot.blackout_remaining == 90,
         "display and blackout timing use their asserted Jak II layouts");
  expect(snapshot.settings_valid && snapshot.background_alpha == 0.25f &&
             snapshot.background_alpha_force == 0.75f && snapshot.movie_process == 0x2500 &&
             snapshot.spooling_process == 0x2600,
         "movie, spooling and background settings use user-current");
  expect(snapshot.scene_valid && snapshot.scene_list == kSceneList &&
             snapshot.scene_list_length == 7 && snapshot.scene == kScene &&
             snapshot.scene_index == 2 && snapshot.animation == 0x2700 &&
             snapshot.next_animation == 0x2800 && snapshot.scene_start_time == 1400 &&
             snapshot.scene_elapsed == 110,
         "scene-player progress preserves index, list, animation and elapsed time");
  expect(snapshot.scene_identity_valid &&
             std::strcmp(snapshot.scene_entity.data(), "title-actor") == 0 &&
             std::strcmp(snapshot.scene_art_group.data(), "title-ag") == 0 &&
             std::strcmp(snapshot.scene_animation.data(), "title-disk-intro") == 0,
         "current scene identity copies bounded GOAL strings");
  expect(snapshot.skeleton_valid && snapshot.skeleton_status == 0x23 &&
             snapshot.active_channels == 2 && snapshot.animation_valid &&
             snapshot.animation_frame_group == kFrameGroup && snapshot.animation_frame == 12.5f &&
             snapshot.animation_aframe == 125.f,
         "root animation reports skeleton state, frame and artist frame");

  write<int64_t>(bytes, kGameInfo + layout::kGameInfoBlackoutTime, 1500);
  const Snapshot expired =
      read({bytes.data(), bytes.size(), kFalse}, {kDisplay, kGameInfo, kSettings, kScenePlayer});
  expect(expired.display_timing_valid && expired.blackout_remaining == 0,
         "expired blackout time has no remaining duration");
}

void malformed_memory_fails_closed() {
  std::array<uint8_t, 512> bytes = {};
  constexpr uint32_t kFalse = 4;
  constexpr uint32_t kDisplay = 0x100;
  constexpr uint32_t kScenePlayer = 0x20;
  constexpr uint32_t kSceneList = 0x1d0;
  write(bytes, kDisplay + layout::kDisplayBaseClock, uint32_t{0x1ff});

  Snapshot snapshot =
      read({bytes.data(), bytes.size(), kFalse}, {kDisplay, kFalse, kFalse, kFalse});
  expect(!snapshot.display_timing_valid && !snapshot.settings_valid && !snapshot.scene_valid &&
             !snapshot.skeleton_valid && !snapshot.animation_valid,
         "false and truncated pointers leave every diagnostic group invalid");

  std::array<char, 8> copied = {};
  write<uint32_t>(bytes, 0x1f8, std::numeric_limits<uint32_t>::max());
  expect(!MemoryView{bytes.data(), bytes.size(), kFalse}.copy_string(0x1f8, copied.data(),
                                                                     copied.size()) &&
             copied[0] == '\0',
         "a GOAL string length cannot escape the supplied memory view");

  write(bytes, kScenePlayer + layout::kScenePlayerSceneList, kSceneList);
  write(bytes, kScenePlayer + layout::kScenePlayerScene, kFalse);
  write<int32_t>(bytes, kScenePlayer + layout::kScenePlayerSceneIndex, 0);
  write(bytes, kScenePlayer + layout::kScenePlayerAnimation, kFalse);
  write(bytes, kScenePlayer + layout::kScenePlayerNextAnimation, kFalse);
  write<int64_t>(bytes, kScenePlayer + layout::kScenePlayerSceneStartTime, 20);
  write<int32_t>(bytes, kSceneList + layout::kArrayLength, 1);
  snapshot = read({bytes.data(), bytes.size(), kFalse}, {kFalse, kFalse, kFalse, kScenePlayer});
  expect(snapshot.scene_valid && snapshot.scene_elapsed == 0,
         "scene elapsed stays unavailable when display timing is invalid");
}

void remaining_time_saturates() {
  expect(saturating_subtract(std::numeric_limits<int64_t>::min(), 1) ==
                 std::numeric_limits<int64_t>::min() &&
             saturating_subtract(std::numeric_limits<int64_t>::max(), -1) ==
                 std::numeric_limits<int64_t>::max(),
         "diagnostic time subtraction is defined at both integer limits");
}

}  // namespace

int main() {
  reads_asserted_jak2_scene_layout();
  malformed_memory_fails_closed();
  remaining_time_saturates();
  std::printf("%s: jak2 runtime metrics reader\n", g_failures ? "FAILED" : "PASSED");
  return g_failures ? 1 : 0;
}
