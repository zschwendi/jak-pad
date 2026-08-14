#pragma once

#include "common/common_types.h"

namespace metal_renderer {

constexpr u32 kJak2MechFlameTextureTbp = 325;
constexpr u16 kJak2MechFlameTexturePage = 12;
constexpr u16 kJak2MechFlameTextureIndex = 144;
constexpr u32 kJak2MechFlameTextureComboId =
    (static_cast<u32>(kJak2MechFlameTexturePage) << 16) | kJak2MechFlameTextureIndex;
constexpr u16 kJak2MissingTextureIdentity = 0xffff;

static_assert(kJak2MechFlameTextureComboId == 786576);

enum class Jak2MechFlameIdentityState : u32 {
  NotObserved = 0,
  Missing = 1,
  Correct = 2,
  Wrong = 3,
};

struct Jak2MechFlameIdentityObservation {
  bool capture = false;
  u32 actual_present = 0;
  u16 actual_page = kJak2MissingTextureIdentity;
  u16 actual_texture = kJak2MissingTextureIdentity;
  u32 actual_combo_id = 0;
  u32 actual_placeholder = 0;
  Jak2MechFlameIdentityState state = Jak2MechFlameIdentityState::NotObserved;

  bool operator==(const Jak2MechFlameIdentityObservation& other) const {
    return capture == other.capture && actual_present == other.actual_present &&
           actual_page == other.actual_page && actual_texture == other.actual_texture &&
           actual_combo_id == other.actual_combo_id &&
           actual_placeholder == other.actual_placeholder && state == other.state;
  }
};

constexpr Jak2MechFlameIdentityObservation observe_jak2_mech_flame_identity(
    bool enabled,
    u32 tbp,
    bool actual_present,
    u16 actual_page = kJak2MissingTextureIdentity,
    u16 actual_texture = kJak2MissingTextureIdentity,
    bool actual_placeholder = false) {
  Jak2MechFlameIdentityObservation result;
  if (!enabled || tbp != kJak2MechFlameTextureTbp) {
    return result;
  }

  result.capture = true;
  if (!actual_present) {
    result.state = Jak2MechFlameIdentityState::Missing;
    return result;
  }

  result.actual_present = 1;
  result.actual_page = actual_page;
  result.actual_texture = actual_texture;
  result.actual_combo_id = (static_cast<u32>(actual_page) << 16) | actual_texture;
  result.actual_placeholder = actual_placeholder ? 1 : 0;
  result.state = result.actual_combo_id == kJak2MechFlameTextureComboId
                     ? Jak2MechFlameIdentityState::Correct
                     : Jak2MechFlameIdentityState::Wrong;
  return result;
}

}  // namespace metal_renderer
