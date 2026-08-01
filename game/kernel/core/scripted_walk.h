#pragma once

/*!
 * @file scripted_walk.h
 * The pieces a scripted run of the real game shares between the headless test and the windowed
 * player: an autopilot that walks the target to a coordinate, and a reader for the level system.
 *
 * The autopilot exists because `--stick` holds a camera-relative direction while the camera turns
 * behind the player, so a fixed stick curves. Nothing here reads the camera: the angle between the
 * stick that was pushed and the direction the target actually moved is what the camera
 * contributes, and a decaying circular mean of it is enough to aim.
 */

#include <cstdint>
#include <string>
#include <vector>

#include "game/kernel/core/pad.h"

struct WalkLeg {
  double x = 0, z = 0;  //! where to walk to, in metres
  int first_frame = 1;
  std::string after_state;
  int frames = 1 << 30;
  double stop_radius = 10.0;

  int began = 0;         //! the frame it actually started on, once known
  bool arrived = false;  //! set when the target got inside stop_radius, so it stops there
  double closest = 1e30;
  int closest_frame = 0;
  int stuck_since = 0;
  int nudge = 0;
};

struct WalkScript {
  std::vector<WalkLeg> legs;
  //! the frame a target state was first entered, <= 0 when it has not been; the harness's own
  int (*state_first_seen)(const std::string& state) = nullptr;
  void (*log)(const char* line) = nullptr;

  /*! Where the target is standing this frame, fed by the harness before drive(). */
  void set_target(bool known, double x, double z, const std::string& state);

  /*! Point the stick at the active leg's destination. Returns false if nothing is walking. */
  bool drive(int frame, goal_pad_state* pad);

  /*! Parse "<x>,<z>[/<radius>]" out of a `--walk-to <x>,<z>[/<radius>]@...` spec, up to `at`. */
  static bool parse_place(const std::string& spec, size_t at, WalkLeg* leg);

 private:
  WalkLeg* active_leg(int frame);

  struct StickAim {
    double sin_a = 0, cos_a = 0;  // circular mean of world-minus-stick
    double sin_b = 0, cos_b = 0;  // ... and of world-plus-stick
    double residual_a = 0, residual_b = 0;
    double stick_angle = 0;
    bool aiming = false;
    bool have_last = false;
    double last_x = 0, last_z = 0;

    void observe(double x, double z);
    void aim(double world, double* u, double* v);

   private:
    bool trust_first_hypothesis() const { return residual_a <= residual_b; }
    double yaw_a() const;
    double yaw_b() const;
  };

  StickAim m_aim;
  bool m_target_known = false;
  double m_target_x = 0, m_target_z = 0;
  std::string m_target_state;
};

// ---------------------------------------------------------------------------------------------
// Which levels the game has. A transition between two Jak 1 levels is the level system moving one
// of its two slots from one level to another, so what a transition *is* can be read out of
// `*level*` and `*load-state*`: what the load state wants, what each of the two level slots holds,
// what status it is in, and which one is displayed. Reported when it changes, which is what says a
// boundary was crossed.
// ---------------------------------------------------------------------------------------------

constexpr int kLevelSlots = 3;  // level0, level1 and level-default

struct LevelSlot {
  std::string name;
  std::string status;
  bool displayed = false;
  uint32_t heap_used = 0;

  bool same_as(const LevelSlot& other) const {
    return name == other.name && status == other.status && displayed == other.displayed;
  }
};

struct LevelState {
  std::string want0, want1, vis_nick, loading;
  bool border = false;
  LevelSlot slot[kLevelSlots];

  bool operator!=(const LevelState& other) const;
};

LevelState read_level_state();

/*! One line of telemetry: "levels want a+b vis 'x, on a border | a 'active shown 11000 kB ...". */
std::string level_state_text(const LevelState& now);
