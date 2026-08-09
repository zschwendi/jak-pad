/*!
 * @file vag_stream.cpp
 * Streamed VAG audio, answered without an IOP.
 *
 * The spooled dialogue and cutscene audio lives in one file per language, `VAGWAD.<lang>`, and
 * `VAGDIR.AYB` is its directory: 868 entries of an 8-byte name and the sector the stream starts at.
 * Upstream this is the second half of the overlord, spread over three places:
 *
 *   - `game/overlord/jak1/iso_api.cpp` posts VAG commands to the ISO thread's mailbox,
 *   - `game/overlord/jak1/iso.cpp` owns the state machine: the ISO thread's VAG cases, which start
 *     and stop a stream, and `ProcessVAGData`, which fills a 0xC000-byte double buffer in SPU RAM
 *     as the voice plays out of it,
 *   - `game/overlord/jak1/stream.cpp` answers the two EE channels that drive it.
 *
 * This file is the same state machine with the ISO thread taken out. There is no mailbox, so a
 * command runs where it is sent, exactly as `dgo_loader.cpp` and `sound_rpc.cpp` do. There is no
 * background reader, so `frame()` - which the sound RPC calls once per frame, where upstream's
 * vblank handler runs - does what one turn of the ISO thread's loop did: check how far the voice
 * has played, read the next 0x6000-byte buffer when it wants one, and hand it to `process_data`.
 *
 * Everything below the buffer is upstream's and unchanged: `game/sound/sdshim.cpp`'s SPU register
 * shims, the `snd::Voice` ADPCM voice they drive, and `game/overlord/common/ssound.cpp`'s falloff
 * and pan math. The voice is one of the eight raw voices `snd_StartSoundSystem` permanently
 * submits to the synth, which is the same voice upstream's `snd_ExternVoiceAlloc` hands out.
 *
 * A 0x6000-byte buffer is about two seconds of audio, so reading one per frame is not a rate this
 * has to keep up with; it is what upstream reads too, just from a thread that spins instead.
 */

#include "game/kernel/core/vag_stream.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/kernel/core/kernel_core.h"
#include "game/sound/sdshim.h"
#include "game/sound/sndshim.h"

#include "fmt/format.h"

namespace vag_stream {

namespace {

// The sizes upstream reads and plays with: game/overlord/jak1/isocommon.h and the SPU RAM
// game/overlord/jak1/ssound.cpp reserves for the stream.
constexpr u32 kSectorSize = 2048;
constexpr u32 kBufferSize = 0x6000;  // STR_BUFFER_DATA_SIZE
constexpr u32 kStreamSram = 0;       // snd_SRAMMalloc(0xc030) on this platform
constexpr u32 kTrapSram = kStreamSram + 0xC000;
constexpr int kVagDirEntries = 868;
constexpr int kVoice = 0;  // snd_ExternVoiceAlloc's answer, through jak1/ssound.cpp's conversion
constexpr s32 kFakeClockStep = 1024 / 60;  // upstream's 1024 Hz clock, one 60 Hz game frame

// Voice register selectors, addressed the way sdshim.cpp expects: the register, plus the voice.
u32 voice_reg(u32 reg) {
  return reg | (kVoice << 1);
}

struct VagDirectory {
  u32 count = 0;
  VagStreamEntry entry[kVagDirEntries] = {};
};

/*! Upstream's `VagCommand`, minus the ISO message header it no longer needs. */
struct StreamState {
  const VagStreamEntry* vag = nullptr;
  u32 sound_id = 0;
  s32 volume = 0;
  u32 priority = 0;
  bool positioned = false;
  Vec3w trans = {};

  // ProcessVAGData's own state
  u32 buffer_number = 0;
  s32 data_left = 0;
  s32 end_point = -1;
  s32 sample_rate = 0;
  bool started = false;
  bool paused = false;
  bool stop = false;
  bool ready_for_data = false;
  bool moved_loop_to_end = false;

  // where the next buffer comes from
  std::FILE* fp = nullptr;
  u32 sector = 0;
};

bool g_installed = false;
const VolumePair* g_pan_table = nullptr;
VagDirectory g_dir;
std::string g_wad_path;

StreamState g_stream;
bool g_active = false;
bool g_stream_paused = false;  // the ISO thread's `vag_paused`
bool g_resume_on_continue = false;
s32 g_vag_id = 0;

s32 g_dialog_volume = 0;
u32 g_play_pos = 48;
bool g_playing = false;
bool g_last_half = false;
s32 g_sample_rate = 0;
s32 g_real_clock = 0;
s32 g_real_clock_samples = 0;
bool g_real_clock_running = false;
s32 g_fake_clock = 0;
bool g_fake_clock_running = false;
bool g_fake_clock_paused = false;
const VagStreamEntry* g_fake_vag = nullptr;
u32 g_fake_priority = 0;

std::vector<u8> g_buffer;
stats g_stats;

u32 bswap(u32 in) {
  return ((in >> 0x18) & 0xff) | ((in >> 8) & 0xff00) | ((in & 0xff00) << 8) | (in << 0x18);
}

/*! `DMA_SendToSPUAndSync` (game/overlord/jak1/dma.cpp), which is one `sceSdVoiceTrans`. */
void send_to_spu(const void* data, u32 size, u32 spu_addr) {
  sceSdVoiceTrans(0, 0, data, spu_addr, size);
}

/*! `CalculateVAGVolumes` in game/overlord/jak1/iso.cpp. */
void calculate_volumes(s32 volume, bool positioned, Vec3w* trans, s32* left, s32* right) {
  if (positioned && g_pan_table) {
    volume = CalculateFalloffVolume(trans, (volume * g_dialog_volume) >> 10, 1, 10, 50);
    const VolumePair* pan = &g_pan_table[(630 - CalculateAngle(trans)) % 360];
    *left = (pan->left * volume) >> 10;
    *right = (pan->right * volume) >> 10;
    if (*left >= 0x4000) {
      *left = 0x3FFF;
    }
    if (*right >= 0x4000) {
      *right = 0x3FFF;
    }
  } else {
    volume = (volume * g_dialog_volume) >> 6;
    if (volume >= 0x4000) {
      volume = 0x3FFF;
    }
    *left = volume;
    *right = volume;
  }
}

/*! `PauseVAG`. */
void pause_voice() {
  g_fake_clock_paused = true;
  if (g_stream.paused) {
    return;
  }
  g_stream.paused = true;
  if (g_stream.started) {
    sceSdSetParam(voice_reg(SD_VP_VOLL), 0);
    sceSdSetParam(voice_reg(SD_VP_VOLR), 0);
    sceSdSetParam(voice_reg(SD_VP_PITCH), 0);
  }
}

/*! `UnpauseVAG`. */
void unpause_voice() {
  g_fake_clock_paused = false;
  if (!g_stream.paused) {
    return;
  }
  if (g_stream.started) {
    s32 left = 0, right = 0;
    calculate_volumes(g_stream.volume, g_stream.positioned, &g_stream.trans, &left, &right);
    sceSdSetParam(voice_reg(SD_VP_VOLL), left);
    sceSdSetParam(voice_reg(SD_VP_VOLR), right);
    sceSdSetParam(voice_reg(SD_VP_PITCH), (g_stream.sample_rate << 12) / 48000);
  }
  g_stream.paused = false;
}

/*! `StopVAG`, plus closing the file the ISO thread would have closed with the message. */
void stop_voice() {
  g_playing = false;
  pause_voice();
  snd_keyOffVoiceRaw(kVoice & 1, kVoice >> 1);
  g_fake_clock_running = false;
  g_real_clock_running = false;
  g_vag_id = 1;
  if (g_stream.fp) {
    std::fclose(g_stream.fp);
  }
  g_stream = StreamState();
  g_active = false;
}

void stop_fake_clock() {
  g_fake_clock_running = false;
  g_fake_clock_paused = false;
  g_fake_vag = nullptr;
  g_fake_priority = 0;
  g_vag_id = 1;
}

void start_fake_clock(const VagStreamEntry* vag, u32 sound_id, u32 priority) {
  if (g_active) {
    stop_voice();
  }
  g_real_clock_running = false;
  g_fake_clock = 0;
  g_fake_clock_running = true;
  g_fake_clock_paused = g_stream_paused;
  g_fake_vag = vag;
  g_fake_priority = priority;
  g_vag_id = (s32)sound_id;
}

/*! `VAG_MarkLoopEnd`: set the loop flag in the last ADPCM block's header. */
void mark_loop_end(u8* data, u32 size) {
  data[size - 15] = 3;
}

/*! `GetPlayPos` and `UpdatePlayPos`. */
void update_play_pos() {
  if (!g_playing) {
    return;
  }
  const u32 nax = sceSdGetAddr(voice_reg(SD_VA_NAX));
  u32 pos = nax > 0xBFFF ? 0xffffffff : nax;
  if (pos == 0xffffffff) {
    pos = g_last_half ? 0xC000 : 0x6000;
  } else {
    g_last_half = pos >= 0x6000;
  }

  if (pos >= g_play_pos) {
    g_real_clock_samples += pos - g_play_pos;
  } else {
    g_real_clock_samples += pos + 0xC000 - g_play_pos;
  }
  if (g_sample_rate) {
    g_real_clock = 4 * (0x1C00 * (g_real_clock_samples / 16) / g_sample_rate);
  }
  g_play_pos = pos;
}

/*! `ProcessVAGData`, given one 0x6000-byte buffer. */
void process_data(u8* data, u32 data_size) {
  StreamState& vag = g_stream;
  if (vag.stop) {
    return;
  }

  if (vag.buffer_number == 0) {
    const u32* words = (const u32*)data;
    if (words[0] != 0x70474156 /* 'VAGp' */ && words[0] != 0x56414770 /* 'pGAV' */) {
      vag.stop = true;
      return;
    }
    vag.sample_rate = words[4];
    vag.data_left = words[3];
    if (words[0] == 0x70474156) {
      vag.sample_rate = bswap(vag.sample_rate);
      vag.data_left = bswap(vag.data_left);
    }
    g_sample_rate = vag.sample_rate;
    g_last_half = false;
    vag.data_left += 48;
    if ((s32)data_size >= vag.data_left) {
      vag.end_point = vag.data_left - 16;
    }

    send_to_spu(data, data_size, kStreamSram);

    sceSdSetParam(voice_reg(SD_VP_VOLL), 0);
    sceSdSetParam(voice_reg(SD_VP_VOLR), 0);
    sceSdSetParam(voice_reg(SD_VP_PITCH), 0);
    sceSdSetAddr(voice_reg(SD_VA_SSA), kStreamSram + 0x30);
    sceSdSetParam(voice_reg(SD_VP_ADSR1), 0xf);
    sceSdSetParam(voice_reg(SD_VP_ADSR2), 0x1fc0);
    if (vag.end_point == -1) {
      sceSdSetAddr(voice_reg(SD_VA_LSAX), kTrapSram);
    }
    snd_keyOnVoiceRaw(kVoice & 1, kVoice >> 1);
    vag.started = true;
    vag.data_left -= data_size;
    vag.buffer_number++;
    return;
  }

  const bool second_half = (vag.buffer_number & 1) != 0;
  if ((s32)data_size < vag.data_left) {
    mark_loop_end(data, data_size);
  } else {
    vag.end_point = second_half ? vag.data_left + 0x5FF0 : vag.data_left - 16;
  }
  send_to_spu(data, data_size, second_half ? kStreamSram + 0x6000 : kStreamSram);

  if (vag.buffer_number == 1) {
    if (!vag.paused) {
      vag.paused = true;
      unpause_voice();
    }
    g_play_pos = 48;
    g_playing = true;
  } else {
    sceSdSetAddr(voice_reg(SD_VA_LSAX), second_half ? kStreamSram + 0x6000 : kStreamSram);
  }

  vag.ready_for_data = false;
  vag.data_left -= data_size;
  vag.buffer_number++;
}

/*! `CheckVAGStreamProgress`. False means the stream has played out. */
bool check_progress() {
  StreamState& vag = g_stream;
  if (vag.stop) {
    return false;
  }
  if (!vag.started) {
    return true;
  }

  if (vag.end_point != -1) {
    if ((s32)(g_play_pos & 0xFFFFFFF0) == vag.end_point) {
      return false;
    }
    if ((g_play_pos < 0x6000 && vag.end_point < 0x6000) ||
        (g_play_pos > 0x5fff && vag.end_point > 0x5fff)) {
      if (!vag.moved_loop_to_end && g_play_pos < (u32)vag.end_point) {
        sceSdSetAddr(voice_reg(SD_VA_LSAX), kStreamSram + vag.end_point);
        vag.moved_loop_to_end = true;
      }
      return true;
    }
  }

  // The voice is playing out of one half, so the other one can be refilled.
  const bool voice_in_first_half = g_play_pos < 0x6000;
  if (voice_in_first_half == ((vag.buffer_number & 1) != 0)) {
    vag.ready_for_data = true;
    sceSdSetAddr(voice_reg(SD_VA_LSAX), kTrapSram);
  }
  return true;
}

/*! One buffer, where the ISO thread's `begin_read` + `sync_read` were: reading past the end of the
 *  file leaves the tail of the buffer as it was, which is what `fs_read` does too. */
bool read_next_buffer() {
  if (!g_stream.fp) {
    return false;
  }
  if (std::fseek(g_stream.fp, (long)(g_stream.sector * kSectorSize), SEEK_SET) != 0) {
    return false;
  }
  const size_t got = std::fread(g_buffer.data(), 1, kBufferSize, g_stream.fp);
  if (got == 0) {
    return false;
  }
  if (got < kBufferSize) {
    memset(g_buffer.data() + got, 0, kBufferSize - got);
  }
  g_stream.sector += kBufferSize / kSectorSize;
  g_stats.buffers_read++;
  return true;
}

bool open_stream(const VagStreamEntry* vag) {
  g_stream = StreamState();
  if (g_wad_path.empty()) {
    return false;
  }
  g_stream.vag = vag;
  g_stream.sector = vag->offset;
  g_stream.fp = file_util::open_file(g_wad_path, "rb");
  if (!g_stream.fp) {
    lg::error("[vag-stream] could not open '{}'", g_wad_path);
    return false;
  }
  g_play_pos = 48;
  return true;
}

/*! The head of the ISO thread's PLAY_VAG_STREAM / QUEUE_VAG_STREAM cases: take over the voice. */
void begin(const VagStreamEntry* vag, u32 sound_id, s32 volume, u32 priority, const Vec3w* trans) {
  if (g_active) {
    stop_voice();
  }
  if (!open_stream(vag)) {
    g_active = false;
    return;
  }
  g_stream.sound_id = sound_id;
  g_stream.volume = volume;
  g_stream.priority = priority;
  g_stream.paused = g_stream_paused;
  if (trans) {
    g_stream.trans = *trans;
    g_stream.positioned = true;
  }
  g_stream.ready_for_data = true;
  g_active = true;
  g_stats.streams_started++;
  lg::info("[vag-stream] '{:.8s}' at sector {}", vag->name, vag->offset);
}

}  // namespace

bool install(const VolumePair* pan_table) {
  shutdown();
  g_pan_table = pan_table;
  g_stats = stats();

  char resolved[1024];
  if (goal_kernel_core_resolve_data_path("iso/VAGDIR.AYB", resolved, sizeof(resolved)) !=
      GOAL_KERNEL_CORE_OK) {
    return false;
  }
  if (!fs::exists(resolved)) {
    lg::warn("[vag-stream] no VAGDIR.AYB in the data directory; streamed audio is unavailable");
    return false;
  }
  const auto data = file_util::read_binary_file(std::string(resolved));
  if (data.size() < sizeof(u32)) {
    lg::error("[vag-stream] VAGDIR.AYB is too small to hold a directory");
    return false;
  }
  const size_t want = std::min(data.size(), sizeof(VagDirectory));
  memcpy(&g_dir, data.data(), want);
  if (g_dir.count > kVagDirEntries) {
    lg::error("[vag-stream] VAGDIR.AYB claims {} streams; the directory holds {}", g_dir.count,
              kVagDirEntries);
    g_dir.count = 0;
    return false;
  }

  g_buffer.assign(kBufferSize, 0);
  g_installed = true;
  lg::info("[vag-stream] {} streams in VAGDIR.AYB", g_dir.count);
  return true;
}

void shutdown() {
  if (g_active) {
    stop_voice();
  }
  g_installed = false;
  g_dir = VagDirectory();
  g_wad_path.clear();
  g_buffer.clear();
  g_pan_table = nullptr;
  g_dialog_volume = 0;
  g_vag_id = 0;
  g_playing = false;
  g_real_clock = 0;
  g_real_clock_samples = 0;
  g_real_clock_running = false;
  g_fake_clock = 0;
  g_fake_clock_running = false;
  g_fake_clock_paused = false;
  g_fake_vag = nullptr;
  g_fake_priority = 0;
  g_stream_paused = false;
  g_resume_on_continue = false;
}

bool installed() {
  return g_installed;
}

void set_language(const char* extension) {
  char resolved[1024];
  const std::string name = std::string("iso/VAGWAD.") + extension;
  if (goal_kernel_core_resolve_data_path(name.c_str(), resolved, sizeof(resolved)) !=
      GOAL_KERNEL_CORE_OK) {
    g_wad_path.clear();
    return;
  }
  if (!fs::exists(resolved)) {
    lg::warn("[vag-stream] no {} in the data directory; streamed audio has nothing to read", name);
    g_wad_path.clear();
    return;
  }
  g_wad_path = resolved;
}

const VagStreamEntry* find(const char* name) {
  if (!g_installed) {
    return nullptr;
  }
  for (u32 i = 0; i < g_dir.count; i++) {
    if (memcmp(g_dir.entry[i].name, name, 8) == 0) {
      return &g_dir.entry[i];
    }
  }
  return nullptr;
}

void play(const VagStreamEntry* vag, u32 sound_id, s32 volume, u32 priority, const Vec3w* trans) {
  if (!g_installed || !vag) {
    g_stats.streams_missing++;
    if ((g_active && !g_stream.paused && (s32)priority < (s32)g_stream.priority) ||
        (g_fake_clock_running && !g_fake_clock_paused &&
         (s32)priority < (s32)g_fake_priority)) {
      return;
    }
    start_fake_clock(vag, sound_id, priority);
    return;
  }

  // Already playing this one: re-aim it rather than restarting.
  if (g_active && g_stream.vag == vag) {
    g_stream.volume = volume;
    g_stream.sound_id = sound_id;
    if (g_stream.paused) {
      if (g_stream_paused) {
        g_resume_on_continue = true;
      } else {
        unpause_voice();
      }
    }
  } else {
    // A louder-priority stream keeps the voice.
    if ((g_active && !g_stream.paused && (s32)priority < (s32)g_stream.priority) ||
        (g_fake_clock_running && !g_fake_clock_paused &&
         (s32)priority < (s32)g_fake_priority)) {
      return;
    }
    stop_fake_clock();
    begin(vag, sound_id, volume, priority, trans);
    if (g_stream_paused) {
      g_resume_on_continue = true;
    }
  }

  if (!g_active) {
    start_fake_clock(vag, sound_id, priority);
    return;
  }
  g_fake_clock_running = false;
  g_fake_clock_paused = false;
  g_fake_vag = nullptr;
  g_fake_priority = 0;
  g_real_clock = 0;
  g_real_clock_samples = 0;
  g_real_clock_running = true;
  g_vag_id = (s32)g_stream.sound_id;
}

void queue(const VagStreamEntry* vag, u32 sound_id, u32 priority) {
  if (!g_installed || !vag) {
    return;
  }
  if ((g_active && (g_stream.vag == vag || !g_stream.paused)) ||
      (g_fake_clock_running && !g_fake_clock_paused)) {
    return;
  }
  stop_fake_clock();
  begin(vag, sound_id, 0x400, priority, nullptr);
  g_stream.paused = true;
}

void stop(const VagStreamEntry* vag, u32 priority) {
  if (g_active && (!vag || g_stream.vag == vag) && (s32)priority >= (s32)g_stream.priority) {
    stop_voice();
  }
  if (g_fake_clock_running && (!vag || g_fake_vag == vag) &&
      (s32)priority >= (s32)g_fake_priority) {
    stop_fake_clock();
  }
  g_stream_paused = false;
  g_resume_on_continue = false;
}

void pause() {
  g_fake_clock_paused = true;
  if (g_stream_paused) {
    return;
  }
  if (g_active && !g_stream.paused) {
    pause_voice();
    g_resume_on_continue = true;
  } else {
    g_resume_on_continue = false;
  }
  g_stream_paused = true;
}

void unpause() {
  g_fake_clock_paused = false;
  if (!g_stream_paused) {
    return;
  }
  if (g_resume_on_continue && g_active) {
    unpause_voice();
  }
  g_stream_paused = false;
  g_resume_on_continue = false;
}

void set_stream_volume(s32 volume) {
  if (g_active) {
    g_stream.volume = volume;
    update_volume();
  }
}

void set_dialog_volume(s32 volume) {
  g_dialog_volume = volume;
  if (g_active) {
    update_volume();
  }
}

void update_volume() {
  if (g_active && g_stream.started && !g_stream.paused) {
    s32 left = 0, right = 0;
    calculate_volumes(g_stream.volume, g_stream.positioned, &g_stream.trans, &left, &right);
    sceSdSetParam(voice_reg(SD_VP_VOLL), left);
    sceSdSetParam(voice_reg(SD_VP_VOLR), right);
  }
}

s32 stream_pos() {
  update_play_pos();
  if (g_fake_clock_running) {
    return g_fake_clock;
  }
  if (g_real_clock_running) {
    return g_real_clock;
  }
  return -1;
}

s32 stream_id() {
  return g_vag_id;
}

void frame() {
  if (g_fake_clock_running && !g_fake_clock_paused) {
    g_fake_clock += kFakeClockStep;
  }
  if (!g_installed || !g_active) {
    return;
  }
  // Where upstream's ISO thread reads a `gPlayPos` the vblank handler keeps up to date. Both live
  // here, so the position is read first: `check_progress` decides off it.
  update_play_pos();
  if (!check_progress()) {
    stop_voice();
    g_stats.streams_finished++;
    return;
  }
  // The first two buffers fill the double buffer back to back; after that one is read whenever the
  // voice has moved into the other half.
  const bool wants_data = g_stream.buffer_number < 2 || g_stream.ready_for_data;
  if (wants_data) {
    if (!read_next_buffer()) {
      stop_voice();
      g_stats.streams_finished++;
      return;
    }
    process_data(g_buffer.data(), kBufferSize);
  }
}

stats get_stats() {
  return g_stats;
}

}  // namespace vag_stream
