/*!
 * @file scripted_walk.cpp
 * See scripted_walk.h. The level offsets are the types' own from engine/level/level-h.gc and
 * engine/level/load-boundary-h.gc, less the 4 bytes of basic type tag.
 */

#include "game/kernel/core/scripted_walk.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace {

constexpr double kMetre = 4096.0;

double angle_between(double a, double b) {
  double d = a - b;
  while (d > M_PI) {
    d -= 2 * M_PI;
  }
  while (d < -M_PI) {
    d += 2 * M_PI;
  }
  return d;
}

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

uint32_t symbol_value(const char* name) {
  auto symbol = jak1::find_symbol_from_c(name);
  return symbol.offset ? symbol->value : 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// the aim: which way "forward" points in the world, measured rather than read from the camera.
// Two hypotheses are carried, one for each handedness of the stick-to-world mapping, and the one
// whose prediction has been closer wins.
// ---------------------------------------------------------------------------------------------

double WalkScript::StickAim::yaw_a() const {
  return std::atan2(sin_a, cos_a);
}

double WalkScript::StickAim::yaw_b() const {
  return std::atan2(sin_b, cos_b);
}

/*! What the last frame's stick actually did, if the target moved far enough to say. */
void WalkScript::StickAim::observe(double x, double z) {
  const bool had = have_last && aiming;
  const double dx = x - last_x, dz = z - last_z;
  last_x = x;
  last_z = z;
  have_last = true;
  if (!had || std::sqrt(dx * dx + dz * dz) < 0.05 * kMetre) {
    return;
  }
  const double world = std::atan2(dx, dz);
  const double decay = 0.97;
  residual_a = decay * residual_a + std::fabs(angle_between(world, stick_angle + yaw_a()));
  residual_b = decay * residual_b + std::fabs(angle_between(world, yaw_b() - stick_angle));
  sin_a = decay * sin_a + std::sin(world - stick_angle);
  cos_a = decay * cos_a + std::cos(world - stick_angle);
  sin_b = decay * sin_b + std::sin(world + stick_angle);
  cos_b = decay * cos_b + std::cos(world + stick_angle);
}

/*! The stick that should send the target off in `world`, as a unit vector (right, forward). */
void WalkScript::StickAim::aim(double world, double* u, double* v) {
  stick_angle = trust_first_hypothesis() ? world - yaw_a() : yaw_b() - world;
  aiming = true;
  *u = std::sin(stick_angle);
  *v = std::cos(stick_angle);
}

void WalkScript::set_target(bool known, double x, double z, const std::string& state) {
  m_target_known = known;
  m_target_x = x;
  m_target_z = z;
  m_target_state = state;
}

/*!
 * The leg that should be driving this frame, or nullptr. Legs run in the order they were given and
 * a leg that has arrived hands over to the next one, so a route is a list of places.
 */
WalkLeg* WalkScript::active_leg(int frame) {
  for (auto& leg : legs) {
    int first = leg.first_frame;
    if (!leg.after_state.empty()) {
      const int entered = state_first_seen ? state_first_seen(leg.after_state) : 0;
      if (entered <= 0) {
        return nullptr;  // the route has not started yet
      }
      first += entered;
    }
    if (frame < first) {
      return nullptr;
    }
    if (leg.arrived || (leg.began && frame >= leg.began + leg.frames)) {
      continue;
    }
    if (!leg.began) {
      leg.began = frame;
    }
    return &leg;
  }
  return nullptr;
}

bool WalkScript::drive(int frame, goal_pad_state* pad) {
  WalkLeg* leg = active_leg(frame);
  if (!leg || !m_target_known) {
    return false;
  }
  m_aim.observe(m_target_x, m_target_z);
  const double dx = leg->x * kMetre - m_target_x;
  const double dz = leg->z * kMetre - m_target_z;
  const double distance = std::sqrt(dx * dx + dz * dz) / kMetre;
  char line[160];
  if (distance < leg->closest - 0.5) {
    leg->closest = distance;
    leg->closest_frame = frame;
    leg->stuck_since = 0;
    leg->nudge = 0;
  } else if (++leg->stuck_since > 180) {
    // Cornered on the geometry. Try a wider line each time, and jump, the way a player would;
    // once the whole sweep has been tried, take where it is now as the new best and start over.
    leg->stuck_since = 0;
    if (++leg->nudge > 8) {
      leg->nudge = 0;
      leg->closest = distance;
    }
  }
  if (distance <= leg->stop_radius) {
    leg->arrived = true;
    if (log) {
      std::snprintf(line, sizeof(line), "frame %5d: walked to within %.1f m of (%.1f %.1f)", frame,
                    distance, leg->x, leg->z);
      log(line);
    }
    return false;
  }
  if (log && (frame - leg->began) % 300 == 0) {
    std::snprintf(line, sizeof(line), "frame %5d: walking to (%.0f %.0f), at (%.1f %.1f), %.0f m to go",
                  frame, leg->x, leg->z, m_target_x / kMetre, m_target_z / kMetre, distance);
    log(line);
  }
  double heading = std::atan2(dx, dz);
  if (leg->nudge) {
    heading += (leg->nudge % 2 ? 1 : -1) * (M_PI / 6) * ((leg->nudge + 1) / 2);
    if (frame % 90 < 3 && m_target_state.compare(0, 12, "target-swim-") != 0) {
      pad->buttons |= GOAL_PAD_X;
    }
  }
  double u = 0, v = 0;
  m_aim.aim(heading, &u, &v);
  const auto to_byte = [](double value) {
    const int byte = (int)std::lround(127.0 + 127.0 * value);
    return (uint8_t)(byte < 0 ? 0 : byte > 255 ? 255 : byte);
  };
  pad->left_x = to_byte(u);
  pad->left_y = to_byte(-v);
  return true;
}

bool WalkScript::parse_place(const std::string& spec, size_t at, WalkLeg* leg) {
  const size_t comma = spec.find(',');
  if (comma == std::string::npos || at == std::string::npos || comma > at) {
    return false;
  }
  leg->x = std::atof(spec.substr(0, comma).c_str());
  leg->z = std::atof(spec.substr(comma + 1, at - comma - 1).c_str());
  const size_t slash = spec.find('/');
  if (slash != std::string::npos && slash < at) {
    leg->stop_radius = std::max(1.0, std::atof(spec.substr(slash + 1, at - slash - 1).c_str()));
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// the level system, read out of the real heap
// ---------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kLevelGroupBorderOffset = 20 - 4;
constexpr uint32_t kLevelGroupLoadingLevelOffset = 12 - 4;
// An inline basic field's declared offset names its type tag, so the object is 4 bytes on.
constexpr uint32_t kLevelGroupLevel0Offset = 96;
constexpr uint32_t kLevelStride = 2704 - 96;

constexpr uint32_t kLevelNameOffset = 4 - 4;
constexpr uint32_t kLevelStatusOffset = 20 - 4;
constexpr uint32_t kLevelHeapOffset = 32 - 4;  // kheap: base, current, top, top-base
constexpr uint32_t kLevelDisplayOffset = 376 - 4;

constexpr uint32_t kLoadStateWantOffset = 4 - 4;
constexpr uint32_t kLoadStateWantStride = 16;
constexpr uint32_t kLoadStateVisNickOffset = 36 - 4;

}  // namespace

bool LevelState::operator!=(const LevelState& other) const {
  if (want0 != other.want0 || want1 != other.want1 || vis_nick != other.vis_nick ||
      loading != other.loading || border != other.border) {
    return true;
  }
  for (int i = 0; i < kLevelSlots; i++) {
    if (!slot[i].same_as(other.slot[i])) {
      return true;
    }
  }
  return false;
}

LevelState read_level_state() {
  LevelState now;
  const uint32_t load_state = symbol_value("*load-state*");
  if (load_state && load_state != s7.offset) {
    now.want0 = symbol_name(goal_u32(load_state + kLoadStateWantOffset));
    now.want1 = symbol_name(goal_u32(load_state + kLoadStateWantOffset + kLoadStateWantStride));
    now.vis_nick = symbol_name(goal_u32(load_state + kLoadStateVisNickOffset));
  }
  const uint32_t group = symbol_value("*level*");
  if (!group || group == s7.offset) {
    return now;
  }
  now.border = goal_u32(group + kLevelGroupBorderOffset) != s7.offset;
  const uint32_t loading = goal_u32(group + kLevelGroupLoadingLevelOffset);
  if (loading && loading != s7.offset) {
    now.loading = symbol_name(goal_u32(loading + kLevelNameOffset));
  }
  for (int i = 0; i < kLevelSlots; i++) {
    const uint32_t level = group + kLevelGroupLevel0Offset + (uint32_t)i * kLevelStride;
    now.slot[i].name = symbol_name(goal_u32(level + kLevelNameOffset));
    now.slot[i].status = symbol_name(goal_u32(level + kLevelStatusOffset));
    now.slot[i].displayed = goal_u32(level + kLevelDisplayOffset) != s7.offset;
    const uint32_t base = goal_u32(level + kLevelHeapOffset);
    const uint32_t current = goal_u32(level + kLevelHeapOffset + 4);
    now.slot[i].heap_used = current > base ? current - base : 0;
  }
  return now;
}

std::string level_state_text(const LevelState& now) {
  std::string out = "levels want ";
  out += now.want0.empty() ? "#f" : now.want0;
  out += "+";
  out += now.want1.empty() ? "#f" : now.want1;
  out += " vis '" + now.vis_nick;
  if (now.border) {
    out += ", on a border";
  }
  if (!now.loading.empty()) {
    out += ", loading '" + now.loading;
  }
  char piece[96];
  for (int i = 0; i < kLevelSlots; i++) {
    if (now.slot[i].name.empty() || now.slot[i].name == "#f") {
      continue;
    }
    std::snprintf(piece, sizeof(piece), " | %s '%s%s %u kB", now.slot[i].name.c_str(),
                  now.slot[i].status.c_str(), now.slot[i].displayed ? " shown" : "",
                  now.slot[i].heap_used / 1024);
    out += piece;
  }
  return out;
}
