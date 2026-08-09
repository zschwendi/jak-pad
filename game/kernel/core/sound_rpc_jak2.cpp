/*!
 * @file sound_rpc_jak2.cpp
 * Answer Jak 2's initial sound state, loader version handshake, bank loads and unloads, language
 * selection and ordinary and chunked STR requests without an IOP.
 *
 * `check-irx-version` sends one 0x50-byte command on loader channel 1. Upstream's Jak 2 overlord
 * writes version 4.0 into that command, remembers the requested EE info-block address, and returns
 * the command as the RPC reply. Loader command 2 has no receive buffer; it bounded-reads a
 * user-local SBlk once, validates every range the current 989snd parser consumes, then passes those
 * same bytes through 989snd's in-memory bank interface. Loader command 6 releases the retained
 * 989snd handle and makes its fixed bank slot reusable. Loader command 20 selects one of Jak 2's
 * eight bounded language tags without a reply payload. Channel 0 retains master volumes, MIDI
 * registers 3/4/14/16, reverb, FPS and listener transforms. Player command 7 starts or updates
 * ordinary named sounds from those checked SFX banks. Channel 4 reads ordinary files and bounded
 * animation chunks from the configured `iso/` directory into EE memory. Channel 5 retains the
 * four-stream play/stop/queue protocol and publishes the state GOAL's GUI loader consumes. Music,
 * streamed-audio output, later MIDI registers and the rest of the Jak 2 sound protocol remain
 * unimplemented.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/common/str_rpc_types.h"
#include "game/common/play_rpc_types.h"
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
constexpr s32 kPlayChannel = PLAY_RPC_CHANNEL;
constexpr u32 kPlayFunction = 0;
constexpr u32 kPlayAsync = 1;
constexpr s32 kPlayRequestSize = sizeof(RPC_Play_Cmd_Jak2);
constexpr s32 kMaxPlayRequests = 4;
constexpr s32 kMaxPlayBufferSize = kPlayRequestSize * kMaxPlayRequests;
constexpr u32 kStreamBuffered = 1u << 1;
constexpr u32 kStreamQueuedWithoutAudio = 1u << 6;
constexpr u32 kStreamLoadingAudio = 1u << 5;
constexpr u32 kStreamPlaying = 1u << 4;
constexpr u32 kStreamArtLoad = 1u << 10;
constexpr u32 kStreamCurrentMovie = 1u << 24;
constexpr std::array<const char*, 8> kLanguages = {"ENG", "FRE", "GER", "SPA",
                                                    "ITA", "JAP", "KOR", "UKE"};
constexpr size_t kBankStemSize = 8;
constexpr size_t kMaxBankFileSize = 64 * 1024 * 1024;
constexpr s32 kMaximumFalloffCurve = 15;

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
static_assert(sizeof(RPC_Play_Cmd_Jak2) == 0x100);
static_assert(offsetof(RPC_Play_Cmd_Jak2, id) == 0x10);
static_assert(offsetof(RPC_Play_Cmd_Jak2, names) == 0x20);
static_assert(sizeof(jak2::SoundIopInfo) == 0x250);
static_assert(offsetof(jak2::SoundIopInfo, stream_position) == 0x110);
static_assert(offsetof(jak2::SoundIopInfo, stream_status) == 0x120);
static_assert(offsetof(jak2::SoundIopInfo, stream_name) == 0x130);
static_assert(offsetof(jak2::SoundIopInfo, stream_id) == 0x1f0);

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
jak2::SoundIopInfo g_info;
bool g_installed = false;
VolumePair g_pan_table[361];

struct StreamState {
  std::array<char, 48> name = {};
  s32 id = 0;
  s32 position = 0;
  u32 status = 0;
};

struct Jak2VagDirEntry {
  char name[8];
  u32 offset;
  u32 flag;
};
static_assert(sizeof(Jak2VagDirEntry) == 16);

enum class VagDirectoryState { Unread, Missing, Valid, Invalid };
enum class StreamAudioState { None, Buffered, Unknown };

std::array<StreamState, 4> g_streams;
std::vector<Jak2VagDirEntry> g_vag_directory;
VagDirectoryState g_vag_directory_state = VagDirectoryState::Unread;

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

bool falloff_parameters_are_safe(const SoundParams& params) {
  return params.fo_min >= 0 && params.fo_max >= 0 && params.fo_curve >= 0 &&
         params.fo_curve <= kMaximumFalloffCurve;
}

bool apply_falloff_defaults(SoundParams* params, const char* normalized_name) {
  SFXUserData data{};
  const bool found = snd_GetSoundUserData(0, nullptr, -1, const_cast<char*>(normalized_name), &data);
  if ((params->mask & 0x40) == 0) {
    if (found && data.data[0] > static_cast<u32>(std::numeric_limits<s16>::max())) {
      return false;
    }
    params->fo_min = found && data.data[0] ? static_cast<s16>(data.data[0]) : 5;
  }
  if ((params->mask & 0x80) == 0) {
    if (found && data.data[1] > static_cast<u32>(std::numeric_limits<s16>::max())) {
      return false;
    }
    params->fo_max = found && data.data[1] ? static_cast<s16>(data.data[1]) : 30;
  }
  if ((params->mask & 0x100) == 0) {
    if (found && data.data[2] > static_cast<u32>(kMaximumFalloffCurve)) {
      return false;
    }
    params->fo_curve = found && data.data[2] ? static_cast<s8>(data.data[2]) : 2;
  }
  return falloff_parameters_are_safe(*params);
}

bool play_falloff_parameters_are_safe(const SoundRpcPlayCommand& command,
                                      const char* normalized_name) {
  if (!command.sound_id) {
    return true;
  }
  SoundParams params = command.parms;
  return apply_falloff_defaults(&params, normalized_name);
}

const Sound* retained_sound_without_update(u32 sound_id) {
  for (const auto& sound : gSounds) {
    if (static_cast<u32>(sound.id) == sound_id) {
      return &sound;
    }
  }
  return nullptr;
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
    SoundParams params = command.parms;
    const auto existing_name = normalize_sound_name(sound->name);
    if (!apply_falloff_defaults(&params, existing_name.data())) {
      return;
    }
    sound->params = params;
    sound->is_music = 0;
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
  SoundParams params = command.parms;
  if (!apply_falloff_defaults(&params, name.data())) {
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
  sound->params = params;
  sound->is_music = 0;
  sound->bank_entry = nullptr;
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

void load_vag_directory() {
  if (g_vag_directory_state != VagDirectoryState::Unread) {
    return;
  }

  char resolved[1024];
  if (goal_kernel_core_resolve_data_path("iso/VAGDIR.AYB", resolved, sizeof(resolved)) !=
      GOAL_KERNEL_CORE_OK) {
    g_vag_directory_state = VagDirectoryState::Invalid;
    return;
  }
  if (!fs::exists(resolved)) {
    // Upstream zeroes gVagDir before attempting this load. A missing directory therefore means
    // every requested animation has no associated VAG and enters status bit 6.
    g_vag_directory_state = VagDirectoryState::Missing;
    return;
  }

  std::vector<u8> data;
  std::string error;
  if (!read_bounded_file(resolved, &data, &error) || data.size() < sizeof(u32)) {
    lg::error("[jak2-stream-state] rejected VAGDIR.AYB: {}", error);
    g_vag_directory_state = VagDirectoryState::Invalid;
    return;
  }

  u32 count = 0;
  memcpy(&count, data.data(), sizeof(count));
  constexpr u32 kMaximumEntries = 2728;
  const u64 required = sizeof(count) + static_cast<u64>(count) * sizeof(Jak2VagDirEntry);
  if (count > kMaximumEntries || required > data.size()) {
    lg::error("[jak2-stream-state] rejected VAGDIR.AYB: {} entries need {} bytes, file has {}",
              count, required, data.size());
    g_vag_directory_state = VagDirectoryState::Invalid;
    return;
  }

  g_vag_directory.resize(count);
  if (count) {
    memcpy(g_vag_directory.data(), data.data() + sizeof(count),
           count * sizeof(Jak2VagDirEntry));
  }
  g_vag_directory_state = VagDirectoryState::Valid;
}

std::array<char, 8> vag_name_for_stream(const std::array<char, 48>& name) {
  std::array<char, 8> result;
  result.fill(' ');
  const size_t length = strnlen(name.data(), name.size());
  if (length > 8 && name[0] != '$') {
    char iso_name[16] = {};
    file_util::ISONameFromAnimationName(iso_name, name.data());
    memcpy(result.data(), iso_name, result.size());
  } else {
    const size_t first = length && name[0] == '$' ? 1 : 0;
    const size_t copy_length = std::min(result.size(), length - first);
    memcpy(result.data(), name.data() + first, copy_length);
  }
  for (char& value : result) {
    if (value >= 'a' && value <= 'z') {
      value -= 'a' - 'A';
    }
  }
  return result;
}

u32 byte_swap(u32 value) {
  return value >> 24 | (value >> 8 & 0xff00) | (value & 0xff00) << 8 | value << 24;
}

bool vag_stream_is_bounded(const Jak2VagDirEntry& entry) {
  if (!gLanguage) {
    return false;
  }
  char resolved[1024];
  const std::string relative = std::string("iso/VAGWAD.") + gLanguage;
  if (goal_kernel_core_resolve_data_path(relative.c_str(), resolved, sizeof(resolved)) !=
      GOAL_KERNEL_CORE_OK) {
    return false;
  }
  FILE* file = std::fopen(resolved, "rb");
  if (!file) {
    return false;
  }

  std::array<u32, 12> header = {};
  bool valid = std::fseek(file, 0, SEEK_END) == 0;
  const long file_size = valid ? std::ftell(file) : -1;
  valid = valid && file_size >= 0 && entry.offset <= static_cast<u64>(file_size) &&
          sizeof(header) <= static_cast<u64>(file_size) - entry.offset &&
          std::fseek(file, static_cast<long>(entry.offset), SEEK_SET) == 0 &&
          std::fread(header.data(), 1, sizeof(header), file) == sizeof(header);
  const bool close_ok = std::fclose(file) == 0;
  if (!valid || !close_ok) {
    return false;
  }

  constexpr u32 kVagBigEndianMagic = 0x70474156;
  constexpr u32 kVagLittleEndianMagic = 0x56414770;
  if (header[0] != kVagBigEndianMagic && header[0] != kVagLittleEndianMagic) {
    return false;
  }
  const u32 payload_size =
      header[0] == kVagBigEndianMagic ? byte_swap(header[3]) : header[3];
  const u32 sample_rate =
      header[0] == kVagBigEndianMagic ? byte_swap(header[4]) : header[4];
  return sample_rate != 0 && payload_size <= static_cast<u64>(file_size) - entry.offset - 0x30;
}

StreamAudioState stream_audio_state(const std::array<char, 48>& name) {
  load_vag_directory();
  if (g_vag_directory_state == VagDirectoryState::Invalid) {
    return StreamAudioState::Unknown;
  }
  if (g_vag_directory_state == VagDirectoryState::Missing) {
    return StreamAudioState::None;
  }

  const auto vag_name = vag_name_for_stream(name);
  const auto entry = std::find_if(
      g_vag_directory.begin(), g_vag_directory.end(), [&](const Jak2VagDirEntry& candidate) {
        return memcmp(candidate.name, vag_name.data(), vag_name.size()) == 0;
      });
  if (entry == g_vag_directory.end()) {
    return StreamAudioState::None;
  }
  return vag_stream_is_bounded(*entry) ? StreamAudioState::Buffered : StreamAudioState::Unknown;
}

bool copy_stream_name(const SoundStreamName& source, std::array<char, 48>* destination) {
  const char* terminator =
      static_cast<const char*>(memchr(source.chars, '\0', sizeof(source.chars)));
  if (!terminator) {
    return false;
  }
  destination->fill(0);
  memcpy(destination->data(), source.chars, static_cast<size_t>(terminator - source.chars));
  return true;
}

bool same_stream(const StreamState& state, const std::array<char, 48>& name, s32 id) {
  return state.id == id && memcmp(state.name.data(), name.data(), name.size()) == 0;
}

StreamState initial_stream_state(const std::array<char, 48>& name,
                                 s32 id,
                                 StreamAudioState audio_state) {
  StreamState result;
  result.name = name;
  result.id = id;
  switch (audio_state) {
    case StreamAudioState::None:
      result.status = kStreamQueuedWithoutAudio;
      break;
    case StreamAudioState::Buffered:
      // Upstream sets bit 5 while loading, then its second completed SPU DMA sets sb_playing
      // (bit 1). This portable seam validates the complete source range synchronously and retains
      // the equivalent buffered state without claiming audio output or an advancing audio clock.
      result.status = kStreamLoadingAudio | kStreamBuffered;
      break;
    case StreamAudioState::Unknown:
      // Fail closed: a malformed/unreadable directory cannot prove the no-audio condition that
      // upstream represents with status bit 6.
      result.status = kStreamLoadingAudio;
      g_stats.stream_failures++;
      break;
  }
  return result;
}

void apply_queue_command(const RPC_Play_Cmd_Jak2& command,
                         const std::array<StreamAudioState, 4>& audio_states) {
  struct DesiredStream {
    std::array<char, 48> name = {};
    s32 id = 0;
    u32 status_flags = 0;
    StreamAudioState audio_state = StreamAudioState::None;
  };
  std::array<DesiredStream, 4> desired;
  size_t desired_count = 0;

  for (size_t i = 0; i < desired.size(); i++) {
    if (!command.names[i].chars[0] || !command.id[i]) {
      continue;
    }
    std::array<char, 48> name;
    copy_stream_name(command.names[i], &name);
    const u32 flags = ((command.address >> i) & 1 ? kStreamArtLoad : 0) |
                      ((command.address >> (i + 4)) & 1 ? kStreamCurrentMovie : 0);
    auto duplicate = std::find_if(desired.begin(), desired.begin() + desired_count,
                                  [&](const DesiredStream& candidate) {
                                    return candidate.id == static_cast<s32>(command.id[i]) &&
                                           candidate.name == name;
                                  });
    if (duplicate != desired.begin() + desired_count) {
      duplicate->status_flags |= flags;
      continue;
    }
    desired[desired_count].name = name;
    desired[desired_count].id = static_cast<s32>(command.id[i]);
    desired[desired_count].status_flags = flags;
    desired[desired_count].audio_state = audio_states[i];
    desired_count++;
  }

  std::array<StreamState, 4> next;
  std::array<bool, 4> desired_used = {};
  for (size_t slot = 0; slot < g_streams.size(); slot++) {
    if (!g_streams[slot].id) {
      continue;
    }
    for (size_t i = 0; i < desired_count; i++) {
      if (!desired_used[i] && same_stream(g_streams[slot], desired[i].name, desired[i].id)) {
        next[slot] = g_streams[slot];
        next[slot].status &= ~(kStreamArtLoad | kStreamCurrentMovie);
        next[slot].status |= desired[i].status_flags;
        desired_used[i] = true;
        break;
      }
    }
  }

  for (size_t i = 0; i < desired_count; i++) {
    if (desired_used[i]) {
      continue;
    }
    auto slot = std::find_if(next.begin(), next.end(), [](const StreamState& state) {
      return state.id == 0;
    });
    if (slot == next.end()) {
      g_stats.stream_failures++;
      break;
    }
    *slot = initial_stream_state(desired[i].name, desired[i].id, desired[i].audio_state);
    slot->status |= desired[i].status_flags;
  }
  g_streams = next;
}

void apply_play_command(const RPC_Play_Cmd_Jak2& command) {
  for (size_t i = 0; i < g_streams.size(); i++) {
    if (!command.names[i].chars[0] || !command.id[i]) {
      continue;
    }
    std::array<char, 48> name;
    copy_stream_name(command.names[i], &name);
    const auto stream = std::find_if(g_streams.begin(), g_streams.end(), [&](const auto& state) {
      return same_stream(state, name, static_cast<s32>(command.id[i]));
    });
    if (stream != g_streams.end()) {
      stream->status |= kStreamPlaying;
    }
  }
}

void apply_stop_command(const RPC_Play_Cmd_Jak2& command) {
  for (size_t i = 0; i < g_streams.size(); i++) {
    if (!command.names[i].chars[0]) {
      continue;
    }
    std::array<char, 48> name;
    copy_stream_name(command.names[i], &name);
    for (auto& stream : g_streams) {
      if (same_stream(stream, name, static_cast<s32>(command.id[i]))) {
        stream = {};
      }
    }
  }
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

u64 unload_bank(const char source_name[16]) {
  if (!g_installed) {
    return reject("rpc-call (Jak 2 sound, unload-bank while 989snd is stopped)");
  }

  std::array<char, 16> bank_name;
  std::string file_name;
  if (!normalize_bank_name(source_name, &bank_name, &file_name)) {
    return reject("rpc-call (Jak 2 sound, invalid unload-bank name)");
  }

  SoundBank* bank = LookupBank(bank_name.data());
  if (!bank) {
    return 0;
  }

  const snd::BankHandle handle = bank->bank_handle;
  if (handle) {
    snd_UnloadBank(handle);
    snd_ResolveBankXREFS();
  }
  bank->bank_handle = nullptr;
  bank->sound_count = 0;
  bank->unk4 = 0;
  bank->in_use = false;
  return 0;
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

  // Preflight the complete snapshot before changing any state or starting a voice. Duplicate PLAY
  // IDs are rejected because the later command's default source would depend on the earlier PLAY.
  std::array<u32, kMaxPlayerCommands> play_ids = {};
  std::size_t play_id_count = 0;
  for (const auto& command : commands) {
    switch (command.j2command) {
      case jak2::Jak2SoundCommand::play: {
        if (!command.play.sound_id) {
          break;
        }
        const auto command_name = normalize_sound_name(command.play.name);
        if (std::find(play_ids.begin(), play_ids.begin() + play_id_count,
                      command.play.sound_id) != play_ids.begin() + play_id_count) {
          return reject_player("rpc-call (Jak 2 player, duplicate PLAY sound ID)");
        }
        play_ids[play_id_count++] = command.play.sound_id;
        if (const Sound* sound = retained_sound_without_update(command.play.sound_id)) {
          const auto existing_name = normalize_sound_name(sound->name);
          // LookupSound may clear a voice that finishes between preflight and apply, so validate
          // both the retained-name update and the command-name new-sound paths without mutating it.
          if (!play_falloff_parameters_are_safe(command.play, existing_name.data()) ||
              !play_falloff_parameters_are_safe(command.play, command_name.data())) {
            return reject_player("rpc-call (Jak 2 player, unsafe PLAY falloff parameters)");
          }
          break;
        }
        if (!play_falloff_parameters_are_safe(command.play, command_name.data())) {
          return reject_player("rpc-call (Jak 2 player, unsafe PLAY falloff parameters)");
        }
        break;
      }
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
    case jak2::Jak2SoundCommand::unload_bank:
      if (recv_buffer != 0 || recv_size != 0) {
        return reject("rpc-call (Jak 2 sound, unload-bank unexpectedly requested a reply)");
      }
      return unload_bank(command.load_bank.bank_name);
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

bool request_basename(const StrRequest& request, std::string* out) {
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
  for (const char c : *out) {
    if (c == '/' || c == '\\' || c == ':') {
      return false;
    }
  }
  return true;
}

bool fail_str_read(const char** reason, const char* value) {
  *reason = value;
  return false;
}

/*! "TIDINTRO STR" -> "TIDINTRO.STR". */
std::string file_name_of_iso_name(const char* iso_name) {
  std::string name(iso_name, 8);
  while (!name.empty() && name.back() == ' ') {
    name.pop_back();
  }
  std::string extension(iso_name + 8, 3);
  while (!extension.empty() && extension.back() == ' ') {
    extension.pop_back();
  }
  return name + "." + extension;
}

bool read_str_file(const StrRequest& request,
                   const std::string& basename,
                   u32* length,
                   const char** reason) {
  if (!request.maxlen || !readable_ee_span(request.address, request.maxlen)) {
    return fail_str_read(reason, "invalid ordinary-file destination");
  }

  std::string file_name = basename;
  for (char& c : file_name) {
    if (c >= 'a' && c <= 'z') {
      c -= 'a' - 'A';
    }
  }
  const std::string relative = "iso/" + file_name;
  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    return fail_str_read(reason, "ordinary file open failed");
  }

  const s32 file_size = ee::sceLseek(fd, 0, SCE_SEEK_END);
  if (file_size <= 0 || ee::sceLseek(fd, 0, SCE_SEEK_SET) != 0) {
    ee::sceClose(fd);
    return fail_str_read(reason, "ordinary file size/seek failed");
  }
  const u32 read_size = std::min((u32)file_size, request.maxlen);
  const s32 bytes_read = ee::sceRead(fd, Ptr<u8>(request.address).c(), (s32)read_size);
  ee::sceClose(fd);
  if (bytes_read != (s32)read_size) {
    return fail_str_read(reason, "ordinary file read failed");
  }
  *length = read_size;
  return true;
}

bool valid_chunked_str_header(const StrFileHeaderJ2& header, s32 file_size) {
  if (file_size < (s32)sizeof(header) || file_size % SECTOR_SIZE) {
    return false;
  }

  const u32 end_sector = (u32)file_size / SECTOR_SIZE;
  bool got_zero = false;
  for (s32 i = 0; i < SECTOR_TABLE_SIZE_J2; i++) {
    const u32 sector = header.sectors[i];
    const u32 size = header.sizes[i];
    if (!sector) {
      if (size) {
        return false;
      }
      got_zero = true;
    } else if (got_zero || sector < sizeof(header) / SECTOR_SIZE || sector >= end_sector) {
      return false;
    }
  }

  for (s32 i = 0; i < SECTOR_TABLE_SIZE_J2 && header.sectors[i]; i++) {
    const u32 sector = header.sectors[i];
    const u32 next_sector =
        i + 1 < SECTOR_TABLE_SIZE_J2 && header.sectors[i + 1] ? header.sectors[i + 1]
                                                               : end_sector;
    if (next_sector <= sector || next_sector > end_sector) {
      return false;
    }
    const u64 size = (u64)(next_sector - sector) * SECTOR_SIZE;
    if (size != header.sizes[i]) {
      return false;
    }
  }
  return true;
}

bool read_chunked_str_file(const StrRequest& request,
                           const std::string& animation_name,
                           u32* length,
                           const char** reason) {
  if (request.section < 0 || request.section >= SECTOR_TABLE_SIZE_J2 ||
      animation_name.size() < 2 || !request.maxlen ||
      !readable_ee_span(request.address, request.maxlen)) {
    return fail_str_read(reason, "invalid chunk request/destination");
  }

  char iso_name[16] = {};
  file_util::ISONameFromAnimationName(iso_name, animation_name.c_str());
  const std::string relative = "iso/" + file_name_of_iso_name(iso_name);
  const s32 fd = ee::sceOpen(relative.c_str(), SCE_RDONLY);
  if (fd < 0) {
    return fail_str_read(reason, "chunk file open failed");
  }

  StrFileHeaderJ2 header;
  const s32 file_size = ee::sceLseek(fd, 0, SCE_SEEK_END);
  const bool got_header = file_size >= 0 && ee::sceLseek(fd, 0, SCE_SEEK_SET) == 0 &&
                          ee::sceRead(fd, &header, sizeof(header)) == (s32)sizeof(header);
  if (!got_header || !valid_chunked_str_header(header, file_size)) {
    ee::sceClose(fd);
    return fail_str_read(reason, "chunk header invalid");
  }

  const u32 chunk_size = header.sizes[request.section];
  const u64 chunk_offset = (u64)header.sectors[request.section] * SECTOR_SIZE;
  if (!chunk_size || chunk_size > request.maxlen ||
      chunk_offset < sizeof(StrFileHeaderJ2) ||
      chunk_offset > (u64)file_size || chunk_size > (u64)file_size - chunk_offset ||
      chunk_offset > (u64)std::numeric_limits<s32>::max() ||
      chunk_size > (u32)std::numeric_limits<s32>::max()) {
    ee::sceClose(fd);
    return fail_str_read(reason,
                         chunk_size > request.maxlen ? "chunk exceeds destination"
                                                     : "chunk range invalid");
  }

  std::vector<u8> chunk;
  try {
    chunk.resize(chunk_size);
  } catch (const std::exception&) {
    ee::sceClose(fd);
    return fail_str_read(reason, "chunk allocation failed");
  }
  const s32 offset = (s32)chunk_offset;
  const bool read = ee::sceLseek(fd, offset, SCE_SEEK_SET) == offset &&
                    ee::sceRead(fd, chunk.data(), (s32)chunk_size) == (s32)chunk_size;
  const bool closed = ee::sceClose(fd) == 0;
  if (!read || !closed) {
    return fail_str_read(reason, "chunk read/close failed");
  }

  memcpy(Ptr<u8>(request.address).c(), chunk.data(), chunk.size());
  *length = chunk_size;
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
  g_stats.str_requests++;
  std::string basename;
  u32 length = 0;
  const bool valid_name = request_basename(request, &basename);
  const char* failure_reason = "invalid basename";
  const bool loaded =
      valid_name &&
      (request.section < 0 ? read_str_file(request, basename, &length, &failure_reason)
                           : read_chunked_str_file(request, basename, &length, &failure_reason));
  if (loaded) {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_DONE, length);
    g_stats.str_reads++;
    g_stats.str_bytes += length;
  } else {
    write_str_reply(request, recv_buffer, STR_RPC_RESULT_ERROR, 0);
    g_stats.str_failures++;
    if (g_stats.str_failures <= 3) {
      lg::warn(
          "[jak2-str] failed {} basename='{}' section={} address=#x{:x} maxlen={} address-mod64={}",
          failure_reason, valid_name ? basename : "<invalid>", request.section, request.address,
          request.maxlen, request.address & 63);
    }
  }
  return 0;
}

u64 play_rpc(u32 function,
             u32 async,
             u32 send_buffer,
             s32 send_size,
             u32 recv_buffer,
             s32 recv_size) {
  if (function != kPlayFunction || async != kPlayAsync || recv_buffer != 0 || recv_size != 0 ||
      send_size <= 0 || send_size > kMaxPlayBufferSize || send_size % kPlayRequestSize != 0 ||
      (send_buffer & 0xf) || !readable_ee_span(send_buffer, static_cast<u32>(send_size))) {
    return reject("rpc-call (Jak 2 PLAY, malformed request)");
  }
  if (!g_installed || !gSoundEnable) {
    return reject("rpc-call (Jak 2 PLAY, sound system is stopped)");
  }

  std::vector<RPC_Play_Cmd_Jak2> commands;
  try {
    commands.resize(static_cast<size_t>(send_size / kPlayRequestSize));
  } catch (const std::exception&) {
    return reject("rpc-call (Jak 2 PLAY, could not snapshot request)");
  }
  memcpy(commands.data(), Ptr<u8>(send_buffer).c(), static_cast<size_t>(send_size));
  std::vector<std::array<StreamAudioState, 4>> audio_states(commands.size());

  // Snapshot and validate the complete batch before changing stream state. Upstream treats each
  // name as a bounded 48-byte C string and only defines command results 0, 1 and 2.
  for (const auto& command : commands) {
    if (command.result > 2) {
      return reject("rpc-call (Jak 2 PLAY, invalid command result)");
    }
    for (const auto& name : command.names) {
      std::array<char, 48> ignored;
      if (!copy_stream_name(name, &ignored)) {
        return reject("rpc-call (Jak 2 PLAY, unterminated stream name)");
      }
    }
    if (command.result == 2) {
      const size_t command_index = static_cast<size_t>(&command - commands.data());
      for (size_t i = 0; i < g_streams.size(); i++) {
        if (!command.names[i].chars[0] || !command.id[i]) {
          continue;
        }
        std::array<char, 48> name;
        copy_stream_name(command.names[i], &name);
        audio_states[command_index][i] = stream_audio_state(name);
        if (audio_states[command_index][i] == StreamAudioState::Unknown) {
          g_stats.stream_failures++;
          return reject("rpc-call (Jak 2 PLAY, invalid VAG source range)");
        }
      }
    }
  }

  for (size_t command_index = 0; command_index < commands.size(); command_index++) {
    const auto& command = commands[command_index];
    switch (command.result) {
      case 0:
        apply_play_command(command);
        g_stats.stream_play_requests++;
        break;
      case 1:
        apply_stop_command(command);
        g_stats.stream_stop_requests++;
        break;
      case 2:
        apply_queue_command(command, audio_states[command_index]);
        g_stats.stream_queue_requests++;
        break;
    }
  }
  g_stats.stream_batches++;
  g_stats.stream_commands += static_cast<u32>(commands.size());
  return 0;
}

u64 rpc_call(const u64* args) {
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
  if (channel == kPlayChannel) {
    return play_rpc(function, async, send_buffer, send_size, recv_buffer, recv_size);
  }
  return reject("rpc-call (Jak 2 sound, unimplemented channel)");
}

u64 rpc_busy(u64 channel) {
  if ((s32)channel != kPlayerChannel && (s32)channel != kLoaderChannel &&
      (s32)channel != kStrChannel && (s32)channel != kPlayChannel) {
    return reject("rpc-busy? (Jak 2 sound, unimplemented channel)");
  }
  return 0;
}

template <u64 (*Function)(const u64*)>
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
  g_info = {};
  g_info.strpos = -1;
  g_streams = {};
  g_vag_directory.clear();
  g_vag_directory_state = VagDirectoryState::Unread;
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

void goal_jak2_sound_frame(void) {
  if (!g_installed || !gSoundEnable || !g_stats.info_ee ||
      !readable_ee_span(g_stats.info_ee, sizeof(g_info))) {
    return;
  }

  g_info.frame++;
  g_info.iop_ticks++;
  g_info.strpos = -1;
  g_info.std_id = 0;
  g_info.freemem = 0;
  g_info.freemem2 = 0;
  g_info.nocd = 0;
  g_info.dirtycd = 0;
  for (size_t i = 0; i < g_streams.size(); i++) {
    g_info.stream_position[i] = g_streams[i].position;
    g_info.stream_status[i] = static_cast<s32>(g_streams[i].status);
    memcpy(g_info.stream_name[i].dat, g_streams[i].name.data(), g_streams[i].name.size());
    g_info.stream_id[i] = g_streams[i].id;
  }
  for (int i = 0; i < 48; i++) {
    g_info.chinfo[i] = snd_GetVoiceStatus(i) == 1 ? 0xff : 0;
  }
  memcpy(Ptr<u8>(g_stats.info_ee).c(), &g_info, sizeof(g_info));
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

uint64_t goal_jak2_sound_rpc_call(const uint64_t* args) {
  return rpc_call(args);
}

uint64_t goal_jak2_sound_rpc_busy(int32_t channel) {
  return rpc_busy((u64)(u32)channel);
}

}  // extern "C"
