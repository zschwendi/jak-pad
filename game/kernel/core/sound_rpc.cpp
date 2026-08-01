/*!
 * @file sound_rpc.cpp
 * The sound half of the overlord, answered synchronously, driving the real 989snd.
 *
 * Upstream, GOAL's two sound RPC channels reach IOP threads: `Thread_Loader` and `Thread_Player`
 * in game/overlord/jak1/srpc.cpp, which sit in `sceSifRpcLoop` and call `RPC_Loader` / `RPC_Player`
 * when the EE sends a buffer. Those two functions are what this file re-implements. Everything they
 * call that is not the IOP - the sound table and its volume/pan/falloff math
 * (game/overlord/common/ssound.cpp), the bank table (game/overlord/common/sbank.cpp), and 989snd
 * itself - is compiled in unchanged and used as-is.
 *
 * What is different, and why:
 *
 * - There is no IOP and no RPC thread, so a channel is answered where it is sent, exactly as
 *   dgo_loader.cpp does: `rpc-call` does the work and returns, and `rpc-busy?` is always 0.
 *   GOAL's own double-buffered `rpc-buffer-pair` is untouched. Both handlers already walk a buffer
 *   of `size / 0x50` commands, so the batching GOAL does still works.
 *
 * - There is no `iso_mbx` and no ISO thread, so a bank is not requested from a file server and
 *   waited on. `load_bank_file` below reads the .SBK out of the data directory and hands it
 *   straight to 989snd, which is what `FS_LoadSoundBank` (game/overlord/jak1/fake_iso.cpp) does
 *   once the ISO thread has scheduled it. Same for .MUS.
 *
 * - There is no VBlank interrupt, so `goal_sound_frame` stands in for `VBlank_Handler`: it runs
 *   the music fade and writes the `sound-iop-info` block back to the EE.
 *
 * - Streamed VAG audio - the spooled dialogue and cutscene audio in VAGWAD.<lang> - is answered by
 *   `vag_stream.cpp`, which is the same state machine without the ISO thread. This file is what
 *   drives it: the `spool-` prefix on the player channel, the streamed-audio channel (5), and the
 *   pause/stop/volume commands that address the stream by its sound id rather than a sound slot.
 *   The 'STRV' plugin - the path where a music sequence itself queues a stream - is still absent.
 *
 * 989snd has no output device in this build (GOALPAD_SND_NO_CUBEB). `goal_sound_pull_audio` is the
 * seam instead: it calls the same `snd::Player::Tick` the desktop port's audio callback calls.
 */

#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/BinaryReader.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/common/play_rpc_types.h"
#include "game/common/str_rpc_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/sound_rpc.h"
#include "game/kernel/core/vag_stream.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak1/srpc.h"
#include "game/sound/sndshim.h"

#include "fmt/format.h"

// defined in desktop_seams.cpp, next to the machine-layer stubs it reports through
u64 goal_kernel_core_machine_stub_report(const char* what);

namespace jak1 {
// Declared by game/overlord/jak1/srpc.h and defined by srpc.cpp, which is not in this build. The
// streaming voice's id lives in vag_stream.cpp instead; this exists only to satisfy the header.
s32 gVAG_Id = 0;
}  // namespace jak1

namespace {

constexpr int kSampleRate = 48000;
constexpr int kCommandSize = (int)sizeof(jak1::SoundRpcCommand);  // 0x50

// english, french, german, spanish, italian, japanese, uk - the VAGWAD extensions, from
// game/overlord/jak1/srpc.cpp.
const char* kLanguages[] = {"ENG", "FRE", "GER", "SPA", "ITA", "JAP", "UKE"};

bool g_installed = false;
bool g_verbose = false;
std::string g_error;
goal_sound_rpc_stats g_stats;

// The EE address check-irx-version asked the info block be written to, and the block itself.
u32 g_info_ee = 0;
jak1::SoundIopInfo g_info;

s16 g_flava = 0;

// Everything this build could not play, named once each, so a run reports rather than drops.
std::set<std::string> g_unhandled;
std::string g_unhandled_report;

void set_error(const std::string& message) {
  g_error = message;
  lg::error("[sound-rpc] {}", message);
}

void note_unhandled(const std::string& what) {
  g_unhandled.insert(what);
}

/*! Resolve "COMMON.SBK" against `<data directory>/iso/`. Empty when there is no data directory. */
std::string iso_path(const std::string& file_name) {
  char resolved[1024];
  if (goal_kernel_core_resolve_data_path(("iso/" + file_name).c_str(), resolved,
                                         sizeof(resolved)) != GOAL_KERNEL_CORE_OK) {
    return {};
  }
  return resolved;
}

/*! GOAL sends bank and music names lowercase and unterminated; the files on disc are uppercase. */
std::string iso_file_name(const char* name, size_t max_stem, const char* extension) {
  std::string stem;
  for (size_t i = 0; i < max_stem && name[i]; i++) {
    const char c = name[i];
    stem.push_back(c >= 'a' && c <= 'z' ? (char)(c - 0x20) : c);
  }
  return stem + extension;
}

// ================================================================================================
// Loading a bank
//
// A .SBK is two things in one file: a directory of 16-byte sound names with their falloff
// parameters, which the overlord keeps and searches by name, and - some way in - the 989snd bank
// the names index into. `AllocateBank` picks the slot and, as upstream's comment says, leaves the
// sector count in `sound_count` for the loader to read: 0x3fe for the common bank, which puts the
// 989snd bank 10 sectors in, and 0x65 for a level bank, which puts it 1 sector in.
// ================================================================================================

/*! The name directory at the top of a .SBK. Same read as `parseSoundBank` in
 *  game/overlord/jak1/fake_iso.cpp. */
bool read_bank_names(const std::vector<u8>& data, SoundBank* bank) {
  BinaryReader reader(data);
  bank->name = reader.read<std::array<char, 16>>();
  reader.read<u32>();
  bank->bank_handle = nullptr;
  bank->sound_count = reader.read<u32>();
  if (bank->sound_count > 0x3fe) {
    set_error(fmt::format("a sound bank claims {} sounds", bank->sound_count));
    return false;
  }
  bank->sound.resize(bank->sound_count);
  for (auto& sound : bank->sound) {
    sound.name = reader.read<std::array<char, 16>>();
    sound.fallof_params = reader.read<u32>();
  }
  return true;
}

bool load_bank_file(const char* bank_name, SoundBank* bank) {
  const int data_offset = bank->sound_count == 0x65 ? 1 * 2048 : 10 * 2048;
  const std::string path = iso_path(iso_file_name(bank_name, 8, ".SBK"));
  if (path.empty() || !fs::exists(path)) {
    set_error(fmt::format("no sound bank '{}' in the data directory", bank_name));
    g_stats.bank_failures++;
    note_unhandled(fmt::format("sound bank '{}' (no file)", bank_name));
    return false;
  }

  const auto data = file_util::read_binary_file(path);
  if (!read_bank_names(data, bank)) {
    g_stats.bank_failures++;
    return false;
  }

  const snd::BankHandle handle = snd_BankLoadEx(path.c_str(), data_offset, 0, 0);
  if (!handle) {
    set_error(fmt::format("989snd rejected the sound bank '{}'", bank_name));
    g_stats.bank_failures++;
    note_unhandled(fmt::format("sound bank '{}' (989snd rejected it)", bank_name));
    return false;
  }
  snd_ResolveBankXREFS();
  bank->bank_handle = handle;
  bank->in_use = true;
  g_stats.banks_loaded++;
  lg::info("[sound-rpc] loaded sound bank {} ({} sounds)", bank_name, bank->sound_count);
  return true;
}

/*! `LoadMusicTweaks` in game/overlord/common/fake_iso.cpp: TWEAKVAL.MUS is the struct on disc. */
void load_music_tweaks() {
  const std::string path = iso_path("TWEAKVAL.MUS");
  if (path.empty() || !fs::exists(path)) {
    gMusicTweakInfo.TweakCount = 0;
    return;
  }
  const auto data = file_util::read_binary_file(path);
  const size_t n = std::min(data.size(), sizeof(gMusicTweakInfo));
  memcpy(&gMusicTweakInfo, data.data(), n);
  if (gMusicTweakInfo.TweakCount > MUSIC_TWEAK_COUNT) {
    gMusicTweakInfo.TweakCount = 0;
  }
}

/*! `LoadMusic` in game/overlord/jak1/iso_api.cpp, without the ISO thread round trip. `name` is a
 *  16-byte sound name and may not be terminated inside it. */
bool load_music_file(const char* name) {
  const std::string music_name(name, strnlen(name, 16));
  const std::string path = iso_path(iso_file_name(music_name.c_str(), 8, ".MUS"));
  if (path.empty() || !fs::exists(path)) {
    set_error(fmt::format("no music '{}' in the data directory", music_name));
    g_stats.music_failures++;
    note_unhandled(fmt::format("music '{}' (no file)", music_name));
    return false;
  }

  gMusic = snd_BankLoadEx(path.c_str(), 0, 0, 0);
  if (!gMusic) {
    set_error(fmt::format("989snd rejected the music bank '{}'", music_name));
    g_stats.music_failures++;
    note_unhandled(fmt::format("music '{}' (989snd rejected it)", music_name));
    return false;
  }
  snd_ResolveBankXREFS();

  gMusicTweak = 0x80;
  for (u32 i = 0; i < gMusicTweakInfo.TweakCount; i++) {
    if (music_name == gMusicTweakInfo.MusicTweak[i].MusicName) {
      gMusicTweak = gMusicTweakInfo.MusicTweak[i].VolumeAdjust;
      break;
    }
  }
  g_stats.music_loaded++;
  lg::info("[sound-rpc] loaded music {} (tweak {})", music_name, gMusicTweak);
  return true;
}

/*!
 * Upstream sets the fade direction to -1 and blocks on the vblank handler until the music has faded
 * all the way out before it unloads the bank. Nothing can block here - the RPC is answered on the
 * caller's own thread - so the bank goes immediately and a music change is abrupt rather than
 * faded. The fade back in is upstream's, because the player channel does it.
 */
void unload_music() {
  if (gMusic) {
    snd_UnloadBank(gMusic);
    snd_ResolveBankXREFS();
    gMusic = nullptr;
  }
  gMusicFade = 0;
  gMusicFadeDir = 0;
}

// ================================================================================================
// The parts of game/overlord/jak1/ssound.cpp that are not the IOP
//
// That file is not compiled in: its `InitSound_Overlord` creates an IOP semaphore and allocates SPU
// voices through the streaming path, and its `SetEarTrans` ends in `SetVAGVol`. The rest of it -
// the falloff curves, the pan table, the music volume and the per-sound position update - is what
// follows, kept the same shape so it stays comparable to upstream.
// ================================================================================================

VolumePair g_pan_table[361];

void build_pan_table() {
  for (int i = 0; i < 91; i++) {
    const s16 opposing_front = static_cast<s16>(((i * 0x33ff) / 0x5a) + 0xc00);
    const s16 rear_right = static_cast<s16>(((i * -0x2800) / 0x5a) + 0x3400);
    const s16 rear_left = static_cast<s16>(((i * -0xbff) / 0x5a) + 0x3fff);

    g_pan_table[90 - i].left = 0x3FFF;
    g_pan_table[180 - i].left = opposing_front;
    g_pan_table[270 - i].left = rear_right;
    g_pan_table[360 - i].left = rear_left;

    g_pan_table[i].right = opposing_front;
    g_pan_table[90 + i].right = 0x3FFF;
    g_pan_table[180 + i].right = rear_left;
    g_pan_table[270 + i].right = rear_right;
  }
}

s32 g_music_vol = 0x400;

void set_music_vol() {
  const s32 volume = (g_music_vol * gMusicFade >> 0x10) * gMusicTweak >> 7;
  g_stats.music_group_volume = volume;
  snd_SetMasterVolume(1, volume);
  snd_SetMasterVolume(2, volume);
}

/*! `UpdateLocation` in game/overlord/jak1/ssound.cpp. */
void update_location(Sound* sound) {
  if (sound->id == 0 || (sound->bank_entry->fallof_params >> 28) == 0) {
    return;
  }
  const s32 id = snd_SoundIsStillPlaying(sound->sound_handle);
  if (id == 0) {
    sound->id = 0;
    return;
  }
  const s32 volume = GetVolume(sound);
  if (volume == 0) {
    snd_StopSound(sound->sound_handle);
  } else {
    snd_SetSoundVolPan(id, volume, GetPan(sound));
  }
}

/*! `SetEarTrans` in game/overlord/jak1/ssound.cpp, without the `SetVAGVol` at the end. */
void set_ear_trans(const Vec3w* ear_trans, const Vec3w* cam_trans, s32 cam_angle) {
  const s32 tick = snd_GetTick();
  const u32 delta = tick - sLastTick;
  sLastTick = tick;

  gEarTrans[0] = *ear_trans;
  gEarTrans[1] = *ear_trans;
  gCamTrans = *cam_trans;
  gCamAngle = cam_angle;

  for (auto& s : gSounds) {
    if (s.id != 0 && s.is_music == 0) {
      if (s.auto_time != 0) {
        UpdateAutoVol(&s, delta);
      }
      update_location(&s);
    }
  }

  // `SetEarTrans` ends in `SetVAGVol`: a positioned stream is aimed from the listener too.
  vag_stream::update_volume();
}

// ================================================================================================
// RPC_Player, from game/overlord/jak1/srpc.cpp
// ================================================================================================

/*! A VAGDIR name is 8 bytes, space-padded and uppercase. `name` is a sound name's tail, which may
 *  run to the end of the 16 bytes without a terminator. */
void vag_name_from(char* out8, const char* name, size_t max) {
  memset(out8, ' ', 8);
  for (size_t i = 0; i < 8 && i < max && name[i]; i++) {
    const char c = name[i];
    out8[i] = c >= 'a' && c <= 'z' ? (char)(c - 0x20) : c;
  }
}

/*! The `spool-` prefix means streamed VAG out of VAGWAD.<lang>. This is upstream's PLAY case for
 *  it (game/overlord/jak1/srpc.cpp), which reads the trans out of the command when the pitch
 *  modifier is set. `name` is what is left of a 16-byte sound name after the prefix. */
void play_spool_request(const jak1::SoundRpcCommand* cmd) {
  g_stats.spool_requests++;
  char stem[9] = {};
  vag_name_from(stem, cmd->play.name + 6, 10);

  const VagStreamEntry* vag = vag_stream::find(stem);
  if (!vag) {
    note_unhandled(fmt::format("streamed audio '{}' (no stream by that name)", stem));
    return;
  }
  const Vec3w* trans = cmd->play.parms.pitch_mod ? &cmd->play.parms.trans : nullptr;
  vag_stream::play(vag, cmd->play.sound_id, cmd->play.parms.volume, 0, trans);
}

void play_sound(const jak1::SoundRpcCommand* cmd) {
  if (cmd->play.sound_id == 0) {
    return;
  }
  if (!memcmp(cmd->play.name, "spool-", 6)) {
    play_spool_request(cmd);
    return;
  }

  SoundBank* bank = nullptr;
  const s32 index = LookupSoundIndex(cmd->play.name, &bank);
  if (index < 0) {
    g_stats.sounds_missing++;
    char stem[17] = {};
    memcpy(stem, cmd->play.name, 16);
    note_unhandled(fmt::format("sound '{}' (no loaded bank has it)", stem));
    return;
  }

  // Already playing under this id: re-aim it rather than starting a second voice.
  if (Sound* existing = LookupSound(cmd->play.sound_id)) {
    memcpy(&existing->params, &cmd->play.parms, sizeof(existing->params));
    existing->bank_entry = &bank->sound[index];
    existing->is_music = 0;
    if ((existing->params.mask & 0x40) == 0) {
      existing->params.fo_min = bank->sound[index].fallof_params & 0x3fff;
    }
    if ((existing->params.mask & 0x80) == 0) {
      existing->params.fo_max = (bank->sound[index].fallof_params >> 14) & 0x3fff;
    }
    if ((existing->params.mask & 0x100) == 0) {
      existing->params.fo_curve = bank->sound[index].fallof_params >> 28;
    }
    UpdateVolume(existing);
    snd_SetSoundPitchModifier(existing->sound_handle, cmd->play.parms.pitch_mod);
    snd_SetSoundPitchBend(existing->sound_handle, cmd->play.parms.bend);
    return;
  }

  Sound* sound = AllocateSound(false);
  if (!sound) {
    note_unhandled("a sound was dropped: all 64 sound slots are in use");
    return;
  }
  memcpy(&sound->params, &cmd->play.parms, sizeof(sound->params));
  sound->bank_entry = &bank->sound[index];
  sound->is_music = 0;
  sound->auto_time = 0;
  if ((sound->params.mask & 0x40) == 0) {
    sound->params.fo_min = bank->sound[index].fallof_params & 0x3fff;
  }
  if ((sound->params.mask & 0x80) == 0) {
    sound->params.fo_max = (bank->sound[index].fallof_params >> 14) & 0x3fff;
  }
  if ((sound->params.mask & 0x100) == 0) {
    sound->params.fo_curve = bank->sound[index].fallof_params >> 28;
  }

  sound->sound_handle = snd_PlaySoundVolPanPMPB(bank->bank_handle, index, GetVolume(sound),
                                                GetPan(sound), sound->params.pitch_mod,
                                                sound->params.bend);
  if (sound->sound_handle) {
    sound->id = cmd->play.sound_id;
    g_stats.sounds_started++;
  } else {
    g_stats.sounds_missing++;
    char stem[17] = {};
    memcpy(stem, cmd->play.name, 16);
    note_unhandled(fmt::format("sound '{}' (989snd gave it no voice)", stem));
  }
}

void set_param(const jak1::SoundRpcCommand* cmd) {
  Sound* sound = LookupSound(cmd->sound_id.sound_id);
  if (!sound) {
    if ((s32)cmd->sound_id.sound_id == vag_stream::stream_id()) {
      vag_stream::set_stream_volume(cmd->param.parms.volume);
    }
    return;
  }
  const u32 mask = cmd->param.parms.mask;
  if (mask & 1) {
    if (mask & 0x10) {
      sound->auto_time = cmd->param.auto_time;
      sound->new_volume = cmd->param.parms.volume;
    } else {
      sound->params.volume = cmd->param.parms.volume;
    }
  }
  if (mask & 0x20) {
    sound->params.trans = cmd->param.parms.trans;
  }
  if (mask & 0x21) {
    UpdateVolume(sound);
  }
  if (mask & 2) {
    sound->params.pitch_mod = cmd->param.parms.pitch_mod;
    if (mask & 0x10) {
      snd_AutoPitch(sound->sound_handle, sound->params.pitch_mod, cmd->param.auto_time,
                    cmd->param.auto_from);
    } else {
      snd_SetSoundPitchModifier(sound->sound_handle, cmd->param.parms.pitch_mod);
    }
  }
  if (mask & 4) {
    sound->params.bend = cmd->param.parms.bend;
    if (mask & 0x10) {
      snd_AutoPitchBend(sound->sound_handle, sound->params.bend, cmd->param.auto_time,
                        cmd->param.auto_from);
    } else {
      snd_SetSoundPitchBend(sound->sound_handle, cmd->param.parms.bend);
    }
  }
}

/*! The head of upstream's `RPC_Player`: the loaded music bank plays on sound id 666, restarted
 *  whenever it is not already running, and re-faded in from silence each time. */
void keep_music_playing() {
  if (gMusic && !gMusicPause && !LookupSound(666)) {
    if (Sound* music = AllocateSound(false)) {
      gMusicFade = 0;
      gMusicFadeDir = 1;
      set_music_vol();
      music->sound_handle = snd_PlaySoundVolPanPMPB(gMusic, 0, 0x400, -1, 0, 0);
      music->id = 666;
      music->is_music = 1;
      if (music->sound_handle) {
        g_stats.music_starts++;
        lg::info("[sound-rpc] music started; game music volume {} of 0x400, tweak {} of 0x80",
                 g_music_vol, gMusicTweak);
      } else {
        music->id = 0;
        note_unhandled("the music bank loaded but 989snd gave sound 0 no voice");
      }
    }
  }

  set_music_vol();
  if (Sound* music = LookupSound(666)) {
    snd_SetSoundVolPan(music->sound_handle, 0x7FFFFFFF, 0);
    snd_SetMIDIRegister(music->sound_handle, 0, g_flava);
  }
}

void rpc_player(u32 send_buffer, int send_size) {
  keep_music_playing();

  const int count = send_size / kCommandSize;
  const auto* cmd = (const jak1::SoundRpcCommand*)Ptr<u8>(send_buffer).c();
  for (int i = 0; i < count; i++, cmd++) {
    g_stats.player_commands++;
    if (g_verbose) {
      lg::info("[sound-rpc] player command {}", (int)cmd->j1command);
    }
    switch (cmd->j1command) {
      case jak1::Jak1SoundCommand::PLAY:
        play_sound(cmd);
        break;
      case jak1::Jak1SoundCommand::PAUSE_SOUND:
        if (Sound* sound = LookupSound(cmd->sound_id.sound_id)) {
          snd_PauseSound(sound->sound_handle);
        } else if ((s32)cmd->sound_id.sound_id == vag_stream::stream_id()) {
          vag_stream::pause();
        }
        break;
      case jak1::Jak1SoundCommand::STOP_SOUND:
        if (Sound* sound = LookupSound(cmd->sound_id.sound_id)) {
          snd_StopSound(sound->sound_handle);
        } else if ((s32)cmd->sound_id.sound_id == vag_stream::stream_id()) {
          vag_stream::stop(nullptr, 0);
        }
        break;
      case jak1::Jak1SoundCommand::CONTINUE_SOUND:
        if (Sound* sound = LookupSound(cmd->sound_id.sound_id)) {
          snd_ContinueSound(sound->sound_handle);
        } else if ((s32)cmd->sound_id.sound_id == vag_stream::stream_id()) {
          vag_stream::unpause();
        }
        break;
      case jak1::Jak1SoundCommand::SET_PARAM:
        set_param(cmd);
        break;
      case jak1::Jak1SoundCommand::SET_MASTER_VOLUME: {
        const u32 group = cmd->master_volume.group.group;
        for (int bit = 0; bit < 32; bit++) {
          if ((group >> bit) & 1) {
            if (bit == 1) {
              g_music_vol = cmd->master_volume.volume;
            } else if (bit == 2) {
              // Group 2 is the dialog volume: upstream routes it to the VAG stream, not to 989snd.
              vag_stream::set_dialog_volume(cmd->master_volume.volume);
            } else {
              snd_SetMasterVolume(bit, cmd->master_volume.volume);
            }
          }
        }
      } break;
      case jak1::Jak1SoundCommand::PAUSE_GROUP:
        snd_PauseAllSoundsInGroup(cmd->group.group);
        if (cmd->group.group & 4) {
          vag_stream::pause();
        }
        if (cmd->group.group & 2) {
          gMusicPause = 1;
        }
        break;
      case jak1::Jak1SoundCommand::STOP_GROUP:
        KillSoundsInGroup(cmd->group.group);
        if (cmd->group.group & 4) {
          vag_stream::stop(nullptr, 0);
        }
        break;
      case jak1::Jak1SoundCommand::CONTINUE_GROUP:
        snd_ContinueAllSoundsInGroup(cmd->group.group);
        if (cmd->group.group & 4) {
          vag_stream::unpause();
        }
        if (cmd->group.group & 2) {
          gMusicPause = 0;
        }
        break;
      case jak1::Jak1SoundCommand::SET_FALLOFF_CURVE:
        SetCurve(cmd->fallof_curve.curve, cmd->fallof_curve.falloff, cmd->fallof_curve.ease);
        break;
      case jak1::Jak1SoundCommand::SET_SOUND_FALLOFF: {
        SoundBank* bank = nullptr;
        const s32 index = LookupSoundIndex(cmd->fallof.name, &bank);
        if (index >= 0) {
          bank->sound[index].fallof_params =
              (cmd->fallof.curve << 28) | (cmd->fallof.max << 14) | cmd->fallof.min;
        }
      } break;
      case jak1::Jak1SoundCommand::SET_FLAVA:
        g_flava = cmd->flava.flava;
        break;
      case jak1::Jak1SoundCommand::SET_EAR_TRANS:
        set_ear_trans(&cmd->ear_trans.ear_trans, &cmd->ear_trans.cam_trans,
                      cmd->ear_trans.cam_angle);
        break;
      case jak1::Jak1SoundCommand::SHUTDOWN:
        gSoundEnable = 0;
        snd_StopSoundSystem();
        break;
      default:
        g_stats.unknown_commands++;
        note_unhandled(fmt::format("player RPC command {}", (int)cmd->j1command));
        break;
    }
  }
}

// ================================================================================================
// RPC_Loader, from game/overlord/jak1/srpc.cpp
// ================================================================================================

void rpc_loader(u32 send_buffer, int send_size, u32 recv_buffer) {
  const int count = send_size / kCommandSize;
  auto* cmd = (jak1::SoundRpcCommand*)Ptr<u8>(send_buffer).c();
  for (int i = 0; i < count; i++, cmd++) {
    g_stats.loader_commands++;
    if (g_verbose) {
      lg::info("[sound-rpc] loader command {}", (int)cmd->j1command);
    }
    switch (cmd->j1command) {
      case jak1::Jak1SoundCommand::LOAD_BANK: {
        if (LookupBank(cmd->load_bank.bank_name)) {
          break;
        }
        if (SoundBank* bank = AllocateBank()) {
          // The bank's name comes out of the file, not out of the command: that is the name
          // `LookupBank` matches GOAL's against. On failure the slot goes back to being free.
          if (!load_bank_file(cmd->load_bank.bank_name, bank)) {
            bank->bank_handle = 0;
            bank->sound.clear();
            bank->sound_count = 0;
            strcpy(bank->name.data(), "<unused>");
          }
        } else {
          note_unhandled("a sound bank was dropped: all 6 bank slots are in use");
        }
      } break;
      case jak1::Jak1SoundCommand::UNLOAD_BANK: {
        if (SoundBank* bank = LookupBank(cmd->load_bank.bank_name)) {
          const snd::BankHandle handle = bank->bank_handle;
          bank->bank_handle = 0;
          bank->sound.clear();
          bank->sound_count = 0;
          strcpy(bank->name.data(), "<unused>");
          snd_UnloadBank(handle);
          snd_ResolveBankXREFS();
        }
      } break;
      case jak1::Jak1SoundCommand::GET_IRX_VERSION:
        // The one sound command that replies. `check-irx-version` crashes the game on any answer
        // but 2.0, and hands over the EE address the info block goes to every frame.
        cmd->irx_version.major = IRX_VERSION_MAJOR;
        cmd->irx_version.minor = IRX_VERSION_MINOR;
        g_info_ee = cmd->irx_version.ee_addr;
        if (recv_buffer) {
          memcpy(Ptr<u8>(recv_buffer).c(), cmd, kCommandSize);
        }
        break;
      case jak1::Jak1SoundCommand::RELOAD_INFO:
        // Upstream re-reads every loaded bank's name table off the disc. The names are already in
        // memory here and nothing changes them, so there is nothing to re-read.
        break;
      case jak1::Jak1SoundCommand::SET_LANGUAGE:
        if (cmd->set_language.langauge_id < sizeof(kLanguages) / sizeof(kLanguages[0])) {
          gLanguage = kLanguages[cmd->set_language.langauge_id];
          vag_stream::set_language(gLanguage);
          lg::info("[sound-rpc] language {}", gLanguage);
        }
        break;
      case jak1::Jak1SoundCommand::LOAD_MUSIC:
        unload_music();
        load_music_file(cmd->load_bank.bank_name);
        break;
      case jak1::Jak1SoundCommand::UNLOAD_MUSIC:
        unload_music();
        break;
      case jak1::Jak1SoundCommand::LIST_SOUNDS:
        PrintActiveSounds();
        break;
      case jak1::Jak1SoundCommand::MIRROR_MODE:
        gMirrorMode = cmd->mirror.value;
        break;
      default:
        g_stats.unknown_commands++;
        note_unhandled(fmt::format("loader RPC command {}", (int)cmd->j1command));
        break;
    }
  }
}

// ================================================================================================
// RPC_PLAY, from game/overlord/jak1/stream.cpp
//
// Channel 5 is how the game starts, stops and queues a spooled stream by name. Upstream's handler
// also re-finds the VAGWAD file record every sixteenth call to keep the drive's head near it; there
// is no drive here, so only the name matters.
// ================================================================================================

void rpc_play(u32 send_buffer, int send_size) {
  constexpr int kPlayMsgSize = (int)sizeof(RPC_Play_Cmd_Jak1);  // 0x40
  const int count = send_size / kPlayMsgSize;
  const auto* cmd = (const RPC_Play_Cmd_Jak1*)Ptr<u8>(send_buffer).c();

  for (int i = 0; i < count; i++, cmd++) {
    char name[9] = {};
    if (cmd->name[0] == '$') {
      vag_name_from(name, cmd->name + 1, sizeof(cmd->name) - 1);
    } else {
      // An animation name, which has its own spelling on disc.
      char iso_name[128];
      file_util::ISONameFromAnimationName(iso_name, cmd->name);
      memcpy(name, iso_name, 8);
    }

    const VagStreamEntry* vag = vag_stream::find(name);
    if (!vag) {
      note_unhandled(fmt::format("streamed audio '{}' (no stream by that name)", name));
      continue;
    }
    if (cmd->result == 0) {
      vag_stream::play(vag, cmd->address, 0x400, 1, nullptr);
    } else if (cmd->result == 1) {
      vag_stream::stop(vag, 1);
    } else {
      vag_stream::queue(vag, 0, 1);
    }
  }
}

}  // namespace

extern "C" {

goal_kernel_core_status goal_sound_install(void) {
  if (g_installed) {
    return GOAL_KERNEL_CORE_ALREADY_INITIALIZED;
  }
  if (!goal_kernel_core_is_initialized()) {
    set_error("goal_sound_install: the kernel is not initialized");
    return GOAL_KERNEL_CORE_NOT_INITIALIZED;
  }
  if (!goal_kernel_core_data_directory()[0]) {
    set_error("goal_sound_install: no data directory is set, so no sound bank can be read");
    return GOAL_KERNEL_CORE_NOT_FOUND;
  }

  g_stats = goal_sound_rpc_stats();
  g_unhandled.clear();
  g_error.clear();
  g_info_ee = 0;
  g_info = {};
  g_info.strpos = -1;
  g_flava = 0;
  g_music_vol = 0x400;

  srpc_init_globals();
  ssound_init_globals();
  sbank_init_globals();
  InitBanks();
  gLanguage = kLanguages[(int)Language::English];

  // The Jak 1 half of `InitSound_Overlord` (game/overlord/jak1/ssound.cpp).
  SetCurve(1, 0, 0);
  SetCurve(2, 4096, 0);
  SetCurve(3, 0, 4096);
  SetCurve(4, 2048, 0);
  SetCurve(5, 2048, 2048);
  SetCurve(6, -4096, 0);
  SetCurve(7, -2048, 0);

  snd_StartSoundSystem();
  snd_SetMixerMode(0, 0);
  for (int i = 0; i < 8; i++) {
    snd_SetGroupVoiceRange(i, 0x10, 0x2f);
  }
  snd_SetGroupVoiceRange(1, 0, 0xf);
  snd_SetGroupVoiceRange(2, 0, 0xf);
  snd_SetReverbDepth(SND_CORE_0 | SND_CORE_1, 0, 0);
  snd_SetReverbType(SND_CORE_0, SD_REV_MODE_OFF);
  snd_SetReverbType(SND_CORE_1, SD_REV_MODE_OFF);
  build_pan_table();
  snd_SetPanTable((s16*)g_pan_table);
  snd_SetPlayBackMode(2);

  load_music_tweaks();

  // Streamed VAG audio. It needs the pan table above for a positioned stream, and it is optional:
  // without VAGDIR.AYB the sequenced sound still plays and a spool request reports itself.
  if (!vag_stream::install(g_pan_table)) {
    note_unhandled("streamed audio: there is no VAGDIR.AYB in the data directory");
  }
  vag_stream::set_language(gLanguage);

  g_installed = true;
  lg::info("[sound-rpc] 989snd is running at {} Hz; the sound RPC channels are answered here",
           kSampleRate);
  return GOAL_KERNEL_CORE_OK;
}

void goal_sound_shutdown(void) {
  if (!g_installed) {
    return;
  }
  vag_stream::shutdown();
  unload_music();
  snd_StopSoundSystem();
  g_installed = false;
}

int goal_sound_is_installed(void) {
  return g_installed ? 1 : 0;
}

void goal_sound_frame(void) {
  if (!g_installed || !gSoundEnable) {
    return;
  }
  g_stats.frames++;

  // `VBlank_Handler` in game/overlord/jak1/srpc.cpp: the music fade in over 64 frames and out over
  // 128.
  if (gMusicFadeDir > 0) {
    gMusicFade += 0x10000 / 64;
    if (gMusicFade > 0x10000 || (gMusicFadeHack & 1)) {
      gMusicFade = 0x10000;
      gMusicFadeDir = 0;
    }
  } else if (gMusicFadeDir < 0) {
    gMusicFade -= 0x10000 / 128;
    if (gMusicFade < 0 || (gMusicFadeHack & 2)) {
      gMusicFade = 0;
      gMusicFadeDir = 0;
    }
  }

  // One turn of the ISO thread's VAG work: advance the stream and refill the voice's buffer.
  vag_stream::frame();

  if (!g_info_ee) {
    return;
  }
  gFrameNum++;
  g_info.frame = gFrameNum;
  // `str-is-playing?` reads strpos: -1 when nothing is streaming, so the game does not wait on a
  // stream that will never advance.
  g_info.strpos = vag_stream::stream_pos();
  g_info.std_id = (u32)vag_stream::stream_id();
  g_info.freemem = 0;
  g_info.freemem2 = 0;
  g_info.nocd = 0;
  g_info.dirtycd = 0;
  for (int i = 0; i < 48; i++) {
    g_info.chinfo[i] = snd_GetVoiceStatus(i) == 1 ? 0xff : 0;
  }
  // Upstream DMAs 0x110 bytes to the EE; here the EE's memory is addressable directly.
  memcpy(Ptr<u8>(g_info_ee).c(), &g_info, 0x110);
}

int goal_sound_sample_rate(void) {
  return kSampleRate;
}

int goal_sound_pull_audio(int16_t* out, int frames) {
  if (!g_installed) {
    return 0;
  }
  const int got = snd_PullAudio(out, frames);
  g_stats.audio_frames += got;
  return got;
}

void goal_sound_rpc_stats_get(goal_sound_rpc_stats* out) {
  *out = g_stats;
  const vag_stream::stats vag = vag_stream::get_stats();
  out->streams_started = vag.streams_started;
  out->streams_played_out = vag.streams_finished;
  out->stream_buffers = vag.buffers_read;
  out->streams_missing = vag.streams_missing;
}

const char* goal_sound_unhandled_report(void) {
  g_unhandled_report.clear();
  for (const auto& entry : g_unhandled) {
    g_unhandled_report += "  " + entry + "\n";
  }
  return g_unhandled_report.c_str();
}

void goal_sound_set_verbose(int on) {
  g_verbose = on != 0;
}

const char* goal_sound_last_error(void) {
  return g_error.c_str();
}

uint64_t goal_sound_rpc_call(int32_t channel, const uint64_t* args) {
  if (!g_installed) {
    return goal_kernel_core_machine_stub_report(channel == PLAY_RPC_CHANNEL
                                                    ? "rpc-call (streamed audio, no sound system)"
                                                    : "rpc-call (sound, no sound system)");
  }
  if (!gSoundEnable) {
    return 0;
  }

  const u32 send_buffer = (u32)args[3];
  const int send_size = (int)(s32)args[4];
  const u32 recv_buffer = (u32)args[5];

  if (channel == PLAY_RPC_CHANNEL) {
    g_stats.play_rpc_calls++;
    rpc_play(send_buffer, send_size);
    return 0;
  }
  if (channel == 1) {
    rpc_loader(send_buffer, send_size, recv_buffer);
    return 0;
  }
  rpc_player(send_buffer, send_size);
  return 0;
}

}  // extern "C"
