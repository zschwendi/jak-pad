/*!
 * @file pad.cpp
 * The controller seam: the host pushes pad state in, GOAL reads it out through the two machine
 * functions that read a pad.
 *
 * There are two halves here.
 *
 * The first is the host-facing state, and the EE pad library written against it. Upstream's
 * `game/sce/libpad.cpp` is the same shim over the PC input layer: it answers the PS2 pad library
 * calls out of whatever the SDL input manager last saw. This library has no SDL and no window, so
 * the same functions are answered out of `g_pads` instead, which the host sets with
 * `goal_pad_set_state`. Everything else about them - the DualShock 2 pretence, the mode tables,
 * the always-succeeding async requests - is upstream's, because the game's pad state machine
 * walks through all of it before it will read a button.
 *
 * The second is `CPadOpen` and `CPadGetData` themselves, copied from
 * `game/kernel/common/kmachine.cpp`. They are copied rather than compiled because the file they
 * live in is the machine layer: it includes the display, the renderer, Discord and sqlite, none of
 * which exist here. The two functions themselves have no such dependency - they are the PS2 pad
 * state machine and a structure fill - so the copy is verbatim apart from `pad_dma_buf`, which is
 * a kmachine.cpp global and is local here. Jak 2's `cpad-info` has the same 132-byte prefix these
 * functions access, followed by eight old-axis bytes they do not touch. `desktop_seams.cpp` copies
 * `CacheFlush` for the same reason.
 */

#include "game/kernel/core/pad.h"

#include <cstring>

#include "common/log/log.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/core/kernel_game.h"
#include "game/sce/libpad.h"

namespace {

struct HostPad {
  goal_pad_state state;
  uint8_t rumble_large = 0;
  uint8_t rumble_small = 0;
  int reads = 0;
};

HostPad g_pads[GOAL_PAD_PORTS];

bool valid_port(int port) {
  return port >= 0 && port < GOAL_PAD_PORTS;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// game/sce/libpad.cpp, against the pushed state instead of against an input library
// ---------------------------------------------------------------------------------------------

namespace ee {

int scePadPortOpen(int port, int slot, void*) {
  // a non-zero file descriptor is expected. The port number plus one is what upstream returns, and
  // nothing here ever closes or reopens a pad.
  if (slot != 0 || !valid_port(port)) {
    return 0;
  }
  return port + 1;
}

int scePadGetState(int port, int /*slot*/) {
  if (!valid_port(port) || !g_pads[port].state.connected) {
    return scePadStateDiscon;
  }
  return scePadStateStable;
}

// The game tries hard to put the controller into DualShock 2 mode, and every controller a host can
// give us behaves like one, so this reports that it is already there.
const int kDualShock2ModeIds[2] = {
    (int)PadMode::Controller,  // no vibration, no pressure-sensitive buttons
    (int)PadMode::DualShock2   // vibration and pressure-sensitive buttons
};

int scePadInfoMode(int /*port*/, int /*slot*/, int term, int offs) {
  switch (term) {
    case InfoModeCurExID:
      return kDualShock2ModeIds[1];
    case InfoModeCurID:
      return kDualShock2ModeIds[1];
    case InfoModeCurExOffs:
      return 1;
    case InfoModeIdTable:
      return offs == -1 ? 2 : kDualShock2ModeIds[offs];
    default:
      return 0;
  }
}

/*!
 * Write the 32 bytes of PS2 pad data the game expects: the status byte that says what kind of
 * controller this is, the digital button word, the two sticks, and the pressure bytes.
 */
int scePadRead(int port, int /*slot*/, u8* rdata) {
  auto cpad = (CPadInfo*)(rdata);

  cpad->valid = 0;                                   // success
  cpad->status = 0x70 /* dualshock 2 */ | (20 / 2);  // dualshock 2 data size

  if (!valid_port(port)) {
    return 0;
  }
  const goal_pad_state& pad = g_pads[port].state;
  cpad->leftx = pad.left_x;
  cpad->lefty = pad.left_y;
  cpad->rightx = pad.right_x;
  cpad->righty = pad.right_y;
  cpad->button0 = (u16)pad.buttons;
  std::memcpy(cpad->abutton, pad.pressure, GOAL_PAD_PRESSURE_BYTES);
  g_pads[port].reads++;
  return 32;
}

/*!
 * The game sets the two vibration motors here every frame. Byte 0 is the on/off motor, which GOAL
 * pulses to fake an intensity, and byte 1 is the analog one. There is no hardware to hand them to,
 * so they are kept for the host to read.
 */
int scePadSetActDirect(int port, int /*slot*/, const u8* data) {
  if (!valid_port(port)) {
    return 0;
  }
  g_pads[port].rumble_large = data[0];
  g_pads[port].rumble_small = data[1];
  return 1;
}

int scePadSetActAlign(int /*port*/, int /*slot*/, const u8* /*data*/) {
  return 1;
}

int scePadSetMainMode(int /*port*/, int /*slot*/, int /*offs*/, int /*lock*/) {
  return 1;
}

// nothing here is asynchronous, so a request is always already finished
int scePadGetReqState(int /*port*/, int /*slot*/) {
  return scePadReqStateComplete;
}

int scePadInfoAct(int /*port*/, int /*slot*/, int actno, int term) {
  if (actno == -1) {
    return 2;  // two actuators, like a DualShock 2
  }
  if (actno < 2) {
    switch (term) {
      case InfoActSub:
      case InfoActFunc:
      case InfoActCurr:
        return 1;
      case InfoActSize:
        return 0;
      default:
        return 0;
    }
  }
  return 0;
}

int scePadInfoPressMode(int /*port*/, int /*slot*/) {
  return 0;  // pressure-sensitive buttons are not reported, as upstream does not report them
}

int scePadEnterPressMode(int /*port*/, int /*slot*/) {
  return 1;
}

}  // namespace ee

// ---------------------------------------------------------------------------------------------
// game/kernel/common/kmachine.cpp - CPadOpen and CPadGetData, copied. See the note at the top.
// ---------------------------------------------------------------------------------------------

namespace {

u8 pad_dma_buf[GOAL_PAD_PORTS * SCE_PAD_DMA_BUFFER_SIZE];

/*!
 * Open a new controller pad.
 * Set the new_pad flag to 1 and state to 0.
 * Prints an error if it fails to open.
 */
u64 CPadOpen(u64 cpad_info, s32 pad_number) {
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  if (cpad->cpad_file == 0) {
    // not open, so we will open it
    cpad->cpad_file =
        ee::scePadPortOpen(pad_number, 0, pad_dma_buf + pad_number * SCE_PAD_DMA_BUFFER_SIZE);
    if (cpad->cpad_file < 1) {
      MsgErr("dkernel: !open cpad #%d (%d)\n", pad_number, cpad->cpad_file);
    }
    cpad->new_pad = 1;
    cpad->state = 0;
  }
  return cpad_info;
}

u64 CPadGetData(u64 cpad_info) {
  using namespace ee;
  auto cpad = Ptr<CPadInfo>(cpad_info).c();
  auto pad_state = scePadGetState(cpad->number, 0);
  if (pad_state == scePadStateDiscon) {
    cpad->state = 0;
  }
  cpad->valid = pad_state | 0x80;
  switch (cpad->state) {
    // case 99: // functional
    default:  // controller is functioning as normal
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        scePadRead(cpad->number, 0, (u8*)cpad);
        // ps2 controllers would send an enabled bit if the button was NOT pressed, but we don't do
        // that here. removed code that flipped the bits.

        if (cpad->change_time) {
          scePadSetActDirect(cpad->number, 0, cpad->direct);
        }
        cpad->valid = pad_state;
      }
      break;
    case 0:  // unavailable
      if (pad_state == scePadStateStable || pad_state == scePadStateFindCTP1) {
        auto pad_mode = scePadInfoMode(cpad->number, 0, InfoModeCurID, 0);
        if (pad_mode != 0) {
          auto vibration_mode = scePadInfoMode(cpad->number, 0, InfoModeCurExID, 0);
          if (vibration_mode > 0) {
            // vibration supported
            pad_mode = vibration_mode;
          }
          if (pad_mode == 4) {
            // controller mode
            cpad->state = 40;
          } else if (pad_mode == 7) {
            // dualshock mode
            cpad->state = 70;
          } else {
            // who knows mode
            cpad->state = 90;
          }
        }
      }
      break;
    case 40:  // controller mode - check for extra modes
      cpad->change_time = 0;
      if (scePadInfoMode(cpad->number, 0, InfoModeIdTable, -1) == 0) {
        // no controller modes
        cpad->state = 90;
        return cpad_info;
      }
      cpad->state = 41;
      [[fallthrough]];
    case 41:  // controller mode - change to dualshock mode!
      // try to enter the 2nd controller mode (dualshock for ds2's)
      if (scePadSetMainMode(cpad->number, 0, 1, 3) == 1) {
        cpad->state = 42;
      }
      break;
    case 42:  // controller mode change check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        // failed to change to DS2
        cpad->state = 41;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        // change successful. go back to the beginning.
        cpad->state = 0;
      }
      break;
    case 70:  // dualshock mode - check vibration
      // get number of actuators (2 for DS2)
      if (scePadInfoAct(cpad->number, 0, -1, 0) < 1) {
        // no actuators means no vibration. skip to end!
        cpad->change_time = 0;
        cpad->state = 99;
      } else {
        // we have actuators to use.
        cpad->change_time = 1;  // remember to update pad times.
        cpad->state = 75;
      }
      break;
    case 75:  // set actuator vib param info
      if (scePadSetActAlign(cpad->number, 0, cpad->align) != 0) {
        if (scePadInfoPressMode(cpad->number, 0) == 1) {
          // pressure buttons supported
          cpad->state = 76;
        } else {
          // no pressure buttons, done with controller setup
          cpad->state = 99;
        }
      }
      break;
    case 76:  // enter pressure mode
      if (scePadEnterPressMode(cpad->number, 0) == 1) {
        cpad->state = 78;
      }
      break;
    case 78:  // pressure mode request check
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateFailed) {
        cpad->state = 76;
      }
      if (scePadGetReqState(cpad->number, 0) == scePadReqStateComplete) {
        cpad->state = 99;
      }
      break;
    case 90:
      break;  // unsupported controller. too bad!
  }
  return cpad_info;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// the host-facing seam
// ---------------------------------------------------------------------------------------------

void goal_pad_state_neutral(goal_pad_state* out) {
  if (!out) {
    return;
  }
  std::memset(out, 0, sizeof(*out));
  out->connected = 1;
  out->left_x = GOAL_PAD_ANALOG_NEUTRAL;
  out->left_y = GOAL_PAD_ANALOG_NEUTRAL;
  out->right_x = GOAL_PAD_ANALOG_NEUTRAL;
  out->right_y = GOAL_PAD_ANALOG_NEUTRAL;
}

goal_kernel_core_status goal_pad_install(void) {
  if (!goal_kernel_core_is_initialized()) {
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  goal_game_make_function_symbol("cpad-open", (void*)CPadOpen);
  goal_game_make_function_symbol("cpad-get-data", (void*)CPadGetData);
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_pad_set_state(int port, const goal_pad_state* state) {
  if (!valid_port(port) || !state) {
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  g_pads[port].state = *state;
  return GOAL_KERNEL_CORE_OK;
}

goal_kernel_core_status goal_pad_get_rumble(int port, uint8_t* out_large, uint8_t* out_small) {
  if (!valid_port(port)) {
    return GOAL_KERNEL_CORE_INVALID_ARGUMENT;
  }
  if (out_large) {
    *out_large = g_pads[port].rumble_large;
  }
  if (out_small) {
    *out_small = g_pads[port].rumble_small;
  }
  return GOAL_KERNEL_CORE_OK;
}

int goal_pad_read_count(int port) {
  return valid_port(port) ? g_pads[port].reads : 0;
}
