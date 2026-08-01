// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "ame_handler.h"
#include "handle_allocator.h"
#include "loader.h"
#include "sound_handler.h"

#include "common/common_types.h"

#include "../common/synth.h"
#include "game/sound/989snd/vagvoice.h"

// GOALPAD_SND_NO_CUBEB builds the sequencer and mixer without an output backend, for platforms
// where the host owns the audio device and pulls frames itself (see Tick below). Everything else in
// this class is unchanged and backend-independent.
#ifndef GOALPAD_SND_NO_CUBEB
#include "third-party/cubeb/cubeb/include/cubeb/cubeb.h"
#endif

namespace snd {

class Player {
 public:
  Player();
  ~Player();
  Player(const Player&) = delete;
  Player operator=(const Player&) = delete;

  // player(player&& other) noexcept = default;
  // player& operator=(player&& other) noexcept = default;

  BankHandle LoadBank(std::span<u8> bank);

  u32 PlaySound(BankHandle bank, u32 sound, s32 vol, s32 pan, s32 pm, s32 pb);
  u32 PlaySoundByName(BankHandle bank,
                      char* bank_name,
                      char* sound_name,
                      s32 vol,
                      s32 pan,
                      s32 pm,
                      s32 pb);
  void DebugPrintAllSoundsInBank(BankHandle bank);
  void SetSoundReg(u32 sound_id, u8 reg, u8 value);
  u8 GetSoundGroup(u32 sound_id);
  void SetGlobalExcite(u8 value) { GlobalExcite = value; };
  bool SoundStillActive(u32 sound_id);
  void SetMasterVolume(u32 group, s32 volume);
  void UnloadBank(BankHandle bank_handle);
  void StopSound(u32 sound_handle);
  u32 GetSoundID(u32 sound_handle);
  void SetPanTable(VolPair* pantable);
  void SetPlaybackMode(s32 mode);
  void PauseSound(s32 sound_handle);
  void ContinueSound(s32 sound_handle);
  void PauseAllSoundsInGroup(u8 group);
  void ContinueAllSoundsInGroup(u8 group);
  void SetSoundVolPan(s32 sound_handle, s32 vol, s32 pan);
  void SubmitVoice(std::shared_ptr<Voice>& voice) { mSynth.AddVoice(voice); };
  void SetSoundPmod(s32 sound_handle, s32 mod);
  void InitCubeb();
  void DestroyCubeb();
  s32 GetTick() { return mTick; };

  //! Render `samples` interleaved stereo frames at 48 kHz, advancing the 240 Hz sequencer as it
  //! goes. Called by the output backend's callback, or directly by a host that owns the device.
  void Tick(s16Output* stream, int samples);

  void StopAllSounds();
  s32 GetSoundUserData(BankHandle block_handle,
                       char* block_name,
                       s32 sound_id,
                       char* sound_name,
                       SFXUserData* dst);

 private:
  std::recursive_mutex mTickLock;  // TODO does not need to recursive with some light restructuring
  IdAllocator mHandleAllocator;
  std::map<u32, std::unique_ptr<SoundHandler>> mHandlers;

#ifdef _WIN32
  bool m_coinitialized = false;
#endif

  Loader mLoader;
  Synth mSynth;
  VoiceManager mVmanager;
  s32 mTick{0};

#ifndef GOALPAD_SND_NO_CUBEB
  cubeb* mCtx{nullptr};
  cubeb_stream* mStream{nullptr};

  static long sound_callback(cubeb_stream* stream,
                             void* user,
                             const void* input,
                             void* output_buffer,
                             long len);
  static void state_callback(cubeb_stream* stream, void* user, cubeb_state state);
#endif
};
}  // namespace snd
