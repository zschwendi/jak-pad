/*!
 * @file sound_rpc_jak2.cpp
 * Answer Jak 2's initial sound state, loader version handshake, bank loads, language selection and
 * ordinary-file STR requests without an IOP.
 *
 * `check-irx-version` sends one 0x50-byte command on loader channel 1. Upstream's Jak 2 overlord
 * writes version 4.0 into that command, remembers the requested EE info-block address, and returns
 * the command as the RPC reply. Loader command 2 has no receive buffer; it bounded-reads a
 * user-local SBlk once, validates every range the current 989snd parser consumes, then passes those
 * same bytes through 989snd's in-memory bank interface. Loader command 20 selects one of Jak 2's
 * eight bounded language tags without a reply payload. Channel 0 retains master volumes, MIDI
 * registers 3/4/14/16, reverb, FPS and listener transforms. Player command 7 starts or updates
 * ordinary named sounds from those checked SFX banks. Channel 4 reads an ordinary file from the
 * configured `iso/` directory into EE memory. Music, streaming, later MIDI registers, chunked STR
 * files and the rest of the Jak 2 sound protocol remain unimplemented.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/common/str_rpc_types.h"
#include "game/kernel/core/sblk_preflight.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/kernel/jak2/kscheme.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"
#include "game/sce/sif_ee.h"
#include "game/sound/989snd/sfxgrain.h"
#include "game/sound/sndshim.h"

// Defined beside the machine stubs in desktop_seams.cpp.
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace {

constexpr s32 kPlayerChannel = 0;
constexpr u32 kPlayerFunction = 0;
constexpr u32 kPlayerAsync = 1;
constexpr s32 kMaxPlayerCommands = 128;
constexpr s32 kLoaderChannel = 1;
constexpr s32 kCommandSize = 0x50;
constexpr s32 kMaxPlayerBufferSize = kCommandSize * kMaxPlayerCommands;
constexpr s32 kStrChannel = 4;
constexpr u32 kStrFunction = 0;
constexpr s32 kStrRequestSize = 0x40;
constexpr s32 kStrReplySize = 0x20;
constexpr u32 kIrxMajor = 4;
constexpr u32 kIrxMinor = 0;
constexpr std::array<const char*, 8> kLanguages = {"ENG", "FRE", "GER", "SPA",
                                                    "ITA", "JAP", "KOR", "UKE"};
constexpr size_t kBankStemSize = 8;
constexpr size_t kMaxBankFileSize = 64 * 1024 * 1024;

static_assert(sizeof(snd::Grain) == 48,
              "Review the SBlk decoded-grain budget when the Grain layout changes");

static_assert(sizeof(jak2::SoundRpcCommand) == kCommandSize);
static_assert(offsetof(jak2::SoundRpcCommand, j2command) == 2);
static_assert(offsetof(jak2::SoundRpcCommand, set_language) == 4);
static_assert(sizeof(SoundRpcSetLanguageCommand) == 4);
static_assert(offsetof(SoundRpcSetLanguageCommand, langauge_id) == 0);
static_assert(offsetof(jak2::SoundRpcCommand, play) == 4);
static_assert(sizeof(SoundRpcPlayCommand) == 60);
static_assert(offsetof(SoundRpcPlayCommand, sound_id) == 0);
static_assert(offsetof(SoundRpcPlayCommand, pad) == 4);
static_assert(offsetof(SoundRpcPlayCommand, name) == 12);
static_assert(offsetof(SoundRpcPlayCommand, parms) == 28);
static_assert(sizeof(SoundParams) == 32);
static_assert(offsetof(SoundParams, mask) == 0);
static_assert(offsetof(SoundParams, pitch_mod) == 2);
static_assert(offsetof(SoundParams, bend) == 4);
static_assert(offsetof(SoundParams, fo_min) == 6);
static_assert(offsetof(SoundParams, fo_max) == 8);
static_assert(offsetof(SoundParams, fo_curve) == 10);
static_assert(offsetof(SoundParams, priority) == 11);
static_assert(offsetof(SoundParams, volume) == 12);
static_assert(offsetof(SoundParams, trans) == 16);
static_assert(offsetof(SoundParams, group) == 28);
static_assert(offsetof(SoundParams, reg) == 29);
static_assert(offsetof(jak2::SoundRpcCommand, master_volume) == 4);
static_assert(sizeof(SoundRpcMasterVolCommand) == 8);
static_assert(offsetof(SoundRpcMasterVolCommand, group) == 0);
static_assert(offsetof(SoundRpcMasterVolCommand, volume) == 4);
static_assert(offsetof(jak2::SoundRpcCommand, midi_reg) == 4);
static_assert(sizeof(SoundRpcSetMidiReg) == 8);
static_assert(offsetof(SoundRpcSetMidiReg, reg) == 0);
static_assert(offsetof(SoundRpcSetMidiReg, value) == 4);
static_assert(sizeof(SoundRpcSetMidiReg::value) == 2);
static_assert(offsetof(jak2::SoundRpcCommand, reverb) == 4);
static_assert(sizeof(SoundRpcSetReverb) == 16);
static_assert(offsetof(SoundRpcSetReverb, core) == 0);
static_assert(offsetof(SoundRpcSetReverb, reverb) == 4);
static_assert(offsetof(SoundRpcSetReverb, left) == 8);
static_assert(offsetof(SoundRpcSetReverb, right) == 12);
static_assert(offsetof(jak2::SoundRpcCommand, fps) == 4);
static_assert(sizeof(SoundRpcSetFPSCommand) == 1);
static_assert(offsetof(SoundRpcSetFPSCommand, fps) == 0);
static_assert(offsetof(jak2::SoundRpcCommand, ear_trans_j2) == 4);
static_assert(sizeof(SoundRpc2SetEarTrans) == 40);
static_assert(offsetof(SoundRpc2SetEarTrans, ear_trans1) == 0);
static_assert(offsetof(SoundRpc2SetEarTrans, ear_trans0) == 12);
static_assert(offsetof(SoundRpc2SetEarTrans, cam_trans) == 24);
static_assert(offsetof(SoundRpc2SetEarTrans, cam_angle) == 36);
static_assert(offsetof(jak2::SoundRpcCommand, irx_version) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, major) == 0);
static_assert(offsetof(SoundRpcGetIrxVersion, minor) == 4);
static_assert(offsetof(SoundRpcGetIrxVersion, ee_addr) == 8);
static_assert(sizeof(RPC_Str_Cmd_Jak2) == 0x50);
static_assert(offsetof(RPC_Str_Cmd_Jak2, basename) == kStrReplySize);

struct StrRequest {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
  char basename[kStrRequestSize - kStrReplySize];
};
static_assert(sizeof(StrRequest) == kStrRequestSize);

struct StrReply {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
};
static_assert(sizeof(StrReply) == kStrReplySize);

goal_jak2_sound_rpc_stats g_stats;
goal_jak2_sound_player_state g_player_state;
bool g_installed = false;
VolumePair g_pan_table[361];

bool readable_ee_span(u32 address, u32 size) {
  return g_ee_main_mem && address >= (u32)EE_MAIN_MEM_LOW_PROTECT &&
         address <= (u32)EE_MAIN_MEM_SIZE && size <= (u32)EE_MAIN_MEM_SIZE - address;
}

u64 reject(const char* what) {
  g_stats.rejected_calls++;
  return goal_kernel_core_machine_stub_report(what);
}

u64 reject_player(const char* what) {
  g_stats.player_failures++;
  return reject(what);
}

void reset_player_state() {
  g_player_state = {};
  std::fill_n(g_player_state.master_volumes, 32, 0x400);
  g_player_state.fps = 60;
  gFPS = 60;
}

void reset_spatial_sound_state() {
  ssound_init_globals();
  for (auto& sound : gSounds) {
    sound = {};
  }
  gEarTrans[0] = {};
  gEarTrans[1] = {};
  gCamTrans = {};
  gCamAngle = 0;
  gMirrorMode = 0;
  sLastTick = 0;
  for (auto& curve : gCurves) {
    curve = {};
  }
}

void build_pan_table() {
  for (int i = 0; i < 91; i++) {
    const s16 opposing_front = static_cast<s16>(((i * 0x33ff) / 90) + 0xc00);
    const s16 rear_right = static_cast<s16>(((i * -0x2800) / 90) + 0x3400);
    const s16 rear_left = static_cast<s16>(((i * -0xbff) / 90) + 0x3fff);

    g_pan_table[90 - i].left = 0x3fff;
    g_pan_table[180 - i].left = opposing_front;
    g_pan_table[270 - i].left = rear_right;
    g_pan_table[360 - i].left = rear_left;

    g_pan_table[i].right = opposing_front;
    g_pan_table[90 + i].right = 0x3fff;
    g_pan_table[180 + i].right = rear_left;
    g_pan_table[270 + i].right = rear_right;
  }
}

std::array<char, 17> normalize_sound_name(const char source[16]) {
  std::array<char, 17> result{};
  bool ended = false;
  for (size_t i = 0; i < 16; i++) {
    char value = source[i];
    if (ended || value == '\0') {
      ended = true;
      continue;
    }
    if (value >= 'a' && value <= 'z') {
      value -= 'a' - 'A';
    } else if (value == '-') {
      value = '_';
    }
    result[i] = value;
  }
  return result;
}

void apply_falloff_defaults(SoundParams* params, const char* normalized_name) {
  SFXUserData data{};
  const bool found = snd_GetSoundUserData(0, nullptr, -1, const_cast<char*>(normalized_name), &data);
  if ((params->mask & 0x40) == 0) {
    params->fo_min = found && data.data[0] ? static_cast<s16>(data.data[0]) : 5;
  }
  if ((params->mask & 0x80) == 0) {
    params->fo_max = found && data.data[1] ? static_cast<s16>(data.data[1]) : 30;
  }
  if ((params->mask & 0x100) == 0) {
    params->fo_curve = found && data.data[2] ? static_cast<s8>(data.data[2]) : 2;
  }
}

void apply_sound_registers(const Sound& sound) {
  if (sound.params.mask & 0x800) {
    snd_SetSoundReg(sound.sound_handle, 0, sound.params.reg[0]);
  }
  if (sound.params.mask & 0x1000) {
    snd_SetSoundReg(sound.sound_handle, 1, sound.params.reg[1]);
  }
  if (sound.params.mask & 0x2000) {
    snd_SetSoundReg(sound.sound_handle, 2, sound.params.reg[2]);
  }
}

void update_location(Sound* sound) {
  if (!sound->id) {
    return;
  }
  const s32 handle = snd_SoundIsStillPlaying(sound->sound_handle);
  sound->sound_handle = handle;
  if (!handle) {
    sound->id = 0;
    return;
  }

  const s32 volume = GetVolume(sound);
  if (!volume) {
    snd_StopSound(handle);
    return;
  }
  const s32 pan = sound->params.fo_curve == 1 || sound->params.fo_curve == 10 ? 0 : GetPan(sound);
  snd_SetSoundVolPan(handle, volume, pan);
}

void apply_ear_transform(const SoundRpc2SetEarTrans& transform) {
  const s32 tick = snd_GetTick();
  const u32 delta = tick - sLastTick;
  sLastTick = tick;
  gEarTrans[0] = transform.ear_trans0;
  gEarTrans[1] = transform.ear_trans1;
  gCamTrans = transform.cam_trans;
  gCamAngle = transform.cam_angle;

  for (auto& sound : gSounds) {
    if (sound.id && !sound.is_music) {
      if (sound.auto_time) {
        UpdateAutoVol(&sound, delta);
      }
      update_location(&sound);
    }
  }
}

void play_sound(const SoundRpcPlayCommand& command) {
  g_stats.play_requests++;
  if (!command.sound_id) {
    return;
  }

  if (Sound* sound = LookupSound(command.sound_id)) {
    sound->params = command.parms;
    sound->is_music = 0;
    const auto existing_name = normalize_sound_name(sound->name);
    apply_falloff_defaults(&sound->params, existing_name.data());
    UpdateVolume(sound);
    snd_SetSoundPitchModifier(sound->sound_handle, sound->params.pitch_mod);
    if (sound->params.mask & 0x4) {
      snd_SetSoundPitchBend(sound->sound_handle, sound->params.bend);
    }
    apply_sound_registers(*sound);
    g_stats.sound_updates++;
    return;
  }

  const auto name = normalize_sound_name(command.name);
  SFXUserData data{};
  if (!snd_GetSoundUserData(0, nullptr, -1, const_cast<char*>(name.data()), &data)) {
    g_stats.sounds_missing++;
    return;
  }

  Sound* sound = AllocateSound(true);
  if (!sound) {
    g_stats.sounds_missing++;
    return;
  }
  const s64 add_index = sound->add_index;
  *sound = {};
  sound->add_index = add_index;
  memcpy(sound->name, name.data(), sizeof(sound->name));
  sound->params = command.parms;
  sound->is_music = 0;
  sound->bank_entry = nullptr;
  apply_falloff_defaults(&sound->params, name.data());
  sound->sound_handle = snd_PlaySoundByNameVolPanPMPB(
      0, nullptr, const_cast<char*>(name.data()), GetVolume(sound), GetPan(sound),
      sound->params.pitch_mod, sound->params.bend);
  if (!sound->sound_handle) {
    g_stats.sounds_missing++;
    return;
  }
  sound->id = command.sound_id;
  apply_sound_registers(*sound);
  g_stats.sounds_started++;
}

bool normalize_bank_name(const char source[16],
                         std::array<char, 16>* normalized,
                         std::string* file_name) {
  normalized->fill(0);
  std::string stem;
  for (size_t i = 0; i < 16 && source[i]; i++) {
    const unsigned char raw = source[i];
    const bool safe = (raw >= 'A' && raw <= 'Z') || (raw >= 'a' && raw <= 'z') ||
                      (raw >= '0' && raw <= '9') || raw == '_' || raw == '-';
    if (!safe) {
      return false;
    }

    char lower = source[i];
    if (lower >= 'A' && lower <= 'Z') {
      lower += 'a' - 'A';
    }
    (*normalized)[i] = lower;

    if (i < kBankStemSize) {
      char upper = source[i];
      if (upper >= 'a' && upper <= 'z') {
        upper -= 'a' - 'A';
      }
      stem.push_back(upper);
    }
  }
  if (stem.empty()) {
    return false;
  }
  *file_name = stem + ".SBK";
  return true;
}

bool read_bounded_file(const char* path, std::vector<u8>* data, std::string* error) {
  FILE* file = std::fopen(path, "rb");
  if (!file) {
    *error = "file is missing or unreadable";
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    *error = "file size could not be read";
    return false;
  }
  const long length = std::ftell(file);
  if (length <= 0 || static_cast<unsigned long>(length) > kMaxBankFileSize ||
      std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    *error = "file is empty or exceeds the bank-size limit";
    return false;
  }

  data->resize(static_cast<size_t>(length));
  const size_t read = std::fread(data->data(), 1, data->size(), file);
  const bool close_ok = std::fclose(file) == 0;
  if (read != data->size() || !close_ok) {
    data->clear();
    *error = "file changed or could not be read completely";
    return false;
  }
  return true;
}

bool load_bank(const char source_name[16]) {
  g_stats.bank_requests++;
  if (!g_installed) {
    g_stats.bank_failures++;
    lg::error("[jak2-sound-rpc] rejected a bank request while 989snd is stopped");
    return false;
  }

  std::array<char, 16> bank_name;
  std::string file_name;
  if (!normalize_bank_name(source_name, &bank_name, &file_name)) {
    g_stats.bank_failures++;
    lg::error("[jak2-sound-rpc] rejected an invalid fixed-width bank name");
    return false;
  }
  if (LookupBank(bank_name.data())) {
    g_stats.bank_reuses++;
    return true;
  }

  char resolved[1024];
  const std::string relative = "iso/" + file_name;
  if (goal_kernel_core_resolve_data_path(relative.c_str(), resolved, sizeof(resolved)) !=
      GOAL_KERNEL_CORE_OK) {
    g_stats.bank_failures++;
    return false;
  }

  try {
    std::vector<u8> data;
    std::string error;
    if (!read_bounded_file(resolved, &data, &error)) {
      g_stats.bank_failures++;
      lg::error("[jak2-sound-rpc] rejected sound bank {}: {}", file_name, error);
      return false;
    }
    const auto preflight = sblk_preflight::validate(data);
    if (!preflight) {
      g_stats.bank_failures++;
      lg::error("[jak2-sound-rpc] rejected sound bank {}: {}", file_name,
                sblk_preflight::error_name(preflight.error));
      return false;
    }

    SoundBank* bank = AllocateBankName(bank_name.data());
    if (!bank) {
      g_stats.bank_failures++;
      lg::error("[jak2-sound-rpc] no bank slot is available for {}", file_name);
      return false;
    }

    // The validated vector is the only file snapshot used. The partial-load API copies these same
    // bytes into 989snd before parsing, avoiding a validate-path/reopen gap.
    snd_BankLoadFromIOPPartialEx_Start();
    snd_BankLoadFromIOPPartialEx(data.data(), static_cast<u32>(data.size()), bank->spu_loc,
                                 bank->spu_size);
    const snd::BankHandle handle = snd_BankLoadFromIOPPartialEx_Completion();
    if (!handle) {
      g_stats.bank_failures++;
      lg::error("[jak2-sound-rpc] 989snd rejected sound bank {}", file_name);
      return false;
    }

    bank->name = bank_name;
    bank->bank_handle = handle;
    bank->in_use = true;
    bank->unk4 = 0;
    snd_ResolveBankXREFS();
    g_stats.banks_loaded++;
    return true;
  } catch (const std::exception& exception) {
    g_stats.bank_failures++;
    lg::error("[jak2-sound-rpc] failed to load sound bank {}: {}", file_name, exception.what());
    return false;
  }
}

bool set_language(u32 language_id) {
  g_stats.language_requests++;
  if (!g_installed) {
    g_stats.language_failures++;
    lg::error("[jak2-sound-rpc] rejected a language request while 989snd is stopped");
    return false;
  }
  if (language_id >= kLanguages.size()) {
    g_stats.language_failures++;
    lg::error("[jak2-sound-rpc] rejected invalid language id {}", language_id);
    return false;
  }

  gLanguage = kLanguages[language_id];
  g_stats.language_id = language_id;
  return true;
}

bool retained_midi_register(s32 reg) {
  return reg == 3 || reg == 4 || reg == 14 || reg == 16;
}

void retain_vec3(const Vec3w& source, int32_t destination[3]) {
  destination[0] = source.x;
  destination[1] = source.y;
  destination[2] = source.z;
}

void apply_player_command(const jak2::SoundRpcCommand& command) {
  switch (command.j2command) {
    case jak2::Jak2SoundCommand::play:
      play_sound(command.play);
      break;
    case jak2::Jak2SoundCommand::set_master_volume: {
      const u32 groups = command.master_volume.group.group;
      for (u32 group = 0; group < 32; group++) {
        if ((groups >> group) & 1) {
          g_player_state.master_volumes[group] = command.master_volume.volume;
          if (group != 1 && group != 2) {
            snd_SetMasterVolume(group, command.master_volume.volume);
          }
        }
      }
    } break;
    case jak2::Jak2SoundCommand::set_midi_reg: {
      const s32 reg = command.midi_reg.reg;
      const s32 value = static_cast<s32>(command.midi_reg.value);
      g_player_state.midi_registers[reg] = value;
      g_player_state.midi_register_mask |= 1u << reg;
      if (reg == 16) {
        snd_SetGlobalExcite(static_cast<u8>(value));
      }
    } break;
    case jak2::Jak2SoundCommand::set_reverb:
      g_player_state.reverb_seen = 1;
      g_player_state.reverb_core = command.reverb.core;
      g_player_state.reverb_type = command.reverb.reverb;
      g_player_state.reverb_left = command.reverb.left;
      g_player_state.reverb_right = command.reverb.right;
      lg::warn("[jak2-sound-rpc] retained reverb state; reverb application is not implemented");
      break;
    case jak2::Jak2SoundCommand::set_ear_trans:
      g_player_state.ear_transform_seen = 1;
      retain_vec3(command.ear_trans_j2.ear_trans1, g_player_state.ear_trans1);
      retain_vec3(command.ear_trans_j2.ear_trans0, g_player_state.ear_trans0);
      retain_vec3(command.ear_trans_j2.cam_trans, g_player_state.camera_trans);
      g_player_state.camera_angle = command.ear_trans_j2.cam_angle;
      apply_ear_transform(command.ear_trans_j2);
      break;
    case jak2::Jak2SoundCommand::set_fps:
      g_player_state.fps = command.fps.fps;
      gFPS = command.fps.fps;
      break;
    default:
      break;
  }
}

u64 player_rpc(u32 function,
               u32 async,
               u32 send_buffer,
               s32 send_size,
               u32 recv_buffer,
               s32 recv_size) {
  if (function != kPlayerFunction || async != kPlayerAsync || recv_buffer != 0 || recv_size != 0 ||
      send_size <= 0 || send_size > kMaxPlayerBufferSize || send_size % kCommandSize != 0 ||
      (send_buffer & 0xf) || !readable_ee_span(send_buffer, static_cast<u32>(send_size))) {
    return reject_player("rpc-call (Jak 2 player, malformed state batch)");
  }
  if (!g_installed || !gSoundEnable) {
    return reject_player("rpc-call (Jak 2 player, sound system is stopped)");
  }

  std::vector<jak2::SoundRpcCommand> commands;
  try {
    commands.resize(static_cast<size_t>(send_size / kCommandSize));
  } catch (const std::exception&) {
    return reject_player("rpc-call (Jak 2 player, could not snapshot state batch)");
  }
  memcpy(commands.data(), Ptr<u8>(send_buffer).c(), static_cast<size_t>(send_size));

  // Preflight the complete snapshot before changing any state or starting a voice. Any unsupported
  // command rejects the whole batch instead of applying the supported entries before it.
  for (const auto& command : commands) {
    switch (command.j2command) {
      case jak2::Jak2SoundCommand::play:
      case jak2::Jak2SoundCommand::set_master_volume:
      case jak2::Jak2SoundCommand::set_reverb:
      case jak2::Jak2SoundCommand::set_ear_trans:
        break;
      case jak2::Jak2SoundCommand::set_midi_reg:
        if (!retained_midi_register(command.midi_reg.reg)) {
          return reject_player("rpc-call (Jak 2 player, unsupported MIDI register)");
        }
        break;
      case jak2::Jak2SoundCommand::set_fps:
        if (command.fps.fps == 0) {
          return reject_player("rpc-call (Jak 2 player, zero FPS)");
        }
        break;
      default:
        return reject_player("rpc-call (Jak 2 player, command is unsupported)");
    }
  }

  for (const auto& command : commands) {
    apply_player_command(command);
  }
  g_stats.player_batches++;
  g_stats.player_commands += static_cast<u32>(commands.size());
  return 0;
}

u64 loader_rpc(u32 send_buffer, s32 send_size, u32 recv_buffer, s32 recv_size) {
  if (send_size != kCommandSize || (send_buffer & 0xf) ||
      !readable_ee_span(send_buffer, kCommandSize)) {
    return reject("rpc-call (Jak 2 sound, malformed loader command)");
  }

  jak2::SoundRpcCommand command;
  memcpy(&command, Ptr<u8>(send_buffer).c(), sizeof(command));
  switch (command.j2command) {
    case jak2::Jak2SoundCommand::get_irx_version:
      if (recv_size != kCommandSize || (recv_buffer & 0xf) ||
          !readable_ee_span(recv_buffer, kCommandSize)) {
        return reject("rpc-call (Jak 2 sound, malformed version reply)");
      }

      command.irx_version.major = kIrxMajor;
      command.irx_version.minor = kIrxMinor;
      g_stats.version_requests++;
      g_stats.info_ee = command.irx_version.ee_addr;

      // Upstream mutates its IOP-side loader buffer, then SIF copies the returned 0x50 bytes to the
      // EE receive buffer. The game's check-irx-version aliases send and receive, but separate EE
      // send buffers must remain untouched.
      memcpy(Ptr<u8>(recv_buffer).c(), &command, sizeof(command));
      return 0;
    case jak2::Jak2SoundCommand::load_bank:
      if (recv_size != 0) {
        return reject("rpc-call (Jak 2 sound, load-bank unexpectedly requested a reply)");
      }
      load_bank(command.load_bank.bank_name);
      return 0;
    case jak2::Jak2SoundCommand::set_language:
      if (recv_buffer != 0 || recv_size != 0) {
        return reject("rpc-call (Jak 2 sound, set-language unexpectedly requested a reply)");
      }
      set_language(command.set_language.langauge_id);
      return 0;
    default:
      return reject("rpc-call (Jak 2 sound, unimplemented loader command)");
  }
}

void write_str_reply(const StrRequest& request, u32 recv_buffer, u16 result, u32 length) {
  StrReply reply;
  memcpy(&reply, &request, sizeof(reply));
  reply.result = result;
  reply.maxlen = length;
  memcpy(Ptr<u8>(recv_buffer).c(), &reply, sizeof(reply));
}

bool uppercase_basename(const StrRequest& request, std::string* out) {
  char basename[sizeof(request.basename) + 1];
  memcpy(basename, request.basename, sizeof(request.basename));
  basename[sizeof(request.basename)] = '\0';

  size_t length = 0;
  while (basename[length]) {
    length++;
  }
  if (!length) {
    return false;
  }

  out->assign(basename, length);
  if (*out == "." || *out == "..") {
    return false;
  }
  for (char& c : *out) {
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  return true;
}

bool read_str_file(const StrRequest& request, const std::string& basename, u32* length) {
  if (!request.maxlen || !readable_ee_span(request.address, request.maxlen)) {
    return false;
  }

  const std::string relative = "iso/" + basename;
  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    return false;
  }

  const s32 file_size = ee::sceLseek(fd, 0, SCE_SEEK_END);
  if (file_size <= 0 || ee::sceLseek(fd, 0, SCE_SEEK_SET) != 0) {
    ee::sceClose(fd);
    return false;
  }
  const u32 read_size = std::min((u32)file_size, request.maxlen);
  const s32 bytes_read = ee::sceRead(fd, Ptr<u8>(request.address).c(), (s32)read_size);
  ee::sceClose(fd);
  if (bytes_read != (s32)read_size) {
    return false;
  }
  *length = read_size;
  return true;
}

u64 str_rpc(u32 function,
            u32 send_buffer,
            s32 send_size,
            u32 recv_buffer,
            s32 recv_size) {
  if (function != kStrFunction) {
    return reject("rpc-call (Jak 2 STR, unimplemented function)");
  }
  if (send_size != kStrRequestSize || recv_size != kStrReplySize ||
      !readable_ee_span(send_buffer, kStrRequestSize) ||
      !readable_ee_span(recv_buffer, kStrReplySize)) {
    return reject("rpc-call (Jak 2 STR, malformed buffers)");
  }

  StrRequest request;
  memcpy(&request, Ptr<u8>(send_buffer).c(), sizeof(request));
  if (request.section >= 0) {
    return reject("rpc-call (Jak 2 STR, chunked files unimplemented)");
  }

  g_stats.str_requests++;
  std::string basename;
  u32 length = 0;
  if (uppercase_basename(request, &basename) && read_str_file(request, basename, &length)) {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_DONE, length);
    g_stats.str_reads++;
    g_stats.str_bytes += length;
  } else {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_ERROR, 0);
    g_stats.str_failures++;
  }
  return 0;
}

u64 rpc_call(u64* args) {
  if (!args) {
    return reject("rpc-call (Jak 2 sound, missing arguments)");
  }

  const s32 channel = (s32)args[0];
  const u32 function = (u32)args[1];
  const u32 async = (u32)args[2];
  const u32 send_buffer = (u32)args[3];
  const s32 send_size = (s32)args[4];
  const u32 recv_buffer = (u32)args[5];
  const s32 recv_size = (s32)args[6];

  if (channel == kPlayerChannel) {
    return player_rpc(function, async, send_buffer, send_size, recv_buffer, recv_size);
  }
  if (channel == kLoaderChannel) {
    return loader_rpc(send_buffer, send_size, recv_buffer, recv_size);
  }
  if (channel == kStrChannel) {
    return str_rpc(function, send_buffer, send_size, recv_buffer, recv_size);
  }
  return reject("rpc-call (Jak 2 sound, unimplemented channel)");
}

u64 rpc_busy(u64 channel) {
  if ((s32)channel != kPlayerChannel && (s32)channel != kLoaderChannel &&
      (s32)channel != kStrChannel) {
    return reject("rpc-busy? (Jak 2 sound, unimplemented channel)");
  }
  return 0;
}

template <u64 (*Function)(u64*)>
u64 stack_arg_shim(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  return Function(args);
}

}  // namespace

extern "C" {

goal_kernel_core_status goal_jak2_sound_rpc_install(void) {
  if (g_installed) {
    return GOAL_KERNEL_CORE_ALREADY_INITIALIZED;
  }
  if (!goal_kernel_core_is_initialized()) {
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }

  g_stats = {};
  srpc_init_globals();
  reset_player_state();
  reset_spatial_sound_state();
  gLanguage = kLanguages[0];
  sbank_init_globals();
  InitBanks();
  try {
    snd_StartSoundSystem();
    snd_SetGlobalExcite(0);
    SetCurve(2, 0, 0);
    SetCurve(9, 0, 0);
    SetCurve(11, 0, 0);
    SetCurve(10, 0, 0);
    SetCurve(3, 4096, 0);
    SetCurve(4, 0, 4096);
    SetCurve(5, 2048, 0);
    SetCurve(6, 2048, 2048);
    SetCurve(7, -4096, 0);
    SetCurve(8, -2048, 0);
    build_pan_table();
    snd_SetPanTable(reinterpret_cast<s16*>(g_pan_table));
    snd_SetPlayBackMode(2);
  } catch (const std::exception& exception) {
    snd_StopSoundSystem();
    lg::error("[jak2-sound-rpc] could not start 989snd: {}", exception.what());
    return GOAL_KERNEL_CORE_OUT_OF_MEMORY;
  }
  g_installed = true;
  jak2::make_stack_arg_function_symbol_from_c("rpc-call", (void*)stack_arg_shim<rpc_call>);
  jak2::make_function_symbol_from_c("rpc-busy?", (void*)rpc_busy);
  return GOAL_KERNEL_CORE_OK;
}

void goal_jak2_sound_rpc_shutdown(void) {
  if (!g_installed) {
    return;
  }
  snd_StopSoundSystem();
  reset_spatial_sound_state();
  sbank_init_globals();
  InitBanks();
  g_installed = false;
}

int goal_jak2_sound_rpc_is_installed(void) {
  return g_installed ? 1 : 0;
}

void goal_jak2_sound_rpc_stats_get(goal_jak2_sound_rpc_stats* out) {
  if (out) {
    *out = g_stats;
  }
}

void goal_jak2_sound_player_state_get(goal_jak2_sound_player_state* out) {
  if (out) {
    *out = g_player_state;
  }
}

}  // extern "C"
