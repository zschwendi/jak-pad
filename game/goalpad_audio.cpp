/*!
 * @file goalpad_audio.cpp
 * See goalpad_audio.h.
 */

#include "game/goalpad_audio.h"

#include <cstring>

#include <AudioToolbox/AudioToolbox.h>

#include "common/log/log.h"

#include "game/kernel/core/kernel_game.h"

namespace goalpad_audio {
namespace {

// audio: CoreAudio's default output unit, pulling from the same seam the iPad's AVAudioEngine
// node will.
//
// Not SDL: the vendored SDL3 is configured with SDL_AUDIO OFF (third-party/cmake/modules/
// SDLOptions.cmake), because the desktop port uses cubeb, which this build leaves out with
// GOALPAD_SND_NO_CUBEB. A default output unit is a dozen lines, needs no third-party build change,
// and is the same shape as the render callback the iPad app needs.


AudioUnit g_output_unit = nullptr;

OSStatus audio_render(void* /*user*/,
                      AudioUnitRenderActionFlags* /*flags*/,
                      const AudioTimeStamp* /*time*/,
                      UInt32 /*bus*/,
                      UInt32 frames,
                      AudioBufferList* data) {
  // One interleaved stereo 16-bit buffer, which is the format asked for below and the format
  // goal_game_sound_pull_audio produces. Before the sound system is up (and after it is torn down)
  // this leaves silence rather than whatever was in the buffer.
  for (UInt32 i = 0; i < data->mNumberBuffers; i++) {
    auto& buffer = data->mBuffers[i];
    std::memset(buffer.mData, 0, buffer.mDataByteSize);
    if (i == 0) {
      goal_game_sound_pull_audio((int16_t*)buffer.mData, (int)frames);
    }
  }
  return noErr;
}

}  // namespace

bool start() {
  AudioComponentDescription desc = {};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_DefaultOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  if (!component) {
    lg::error("no CoreAudio default output unit");
    return false;
  }
  if (AudioComponentInstanceNew(component, &g_output_unit) != noErr) {
    lg::error("could not create the CoreAudio output unit");
    return false;
  }

  // 989snd's sequencer tick, note pitch and envelope timing are all derived from its output rate,
  // so the rate is not a preference. The unit converts to whatever the device is running at.
  AudioStreamBasicDescription format = {};
  format.mSampleRate = goal_game_sound_sample_rate();
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  format.mChannelsPerFrame = 2;
  format.mBitsPerChannel = 16;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = 4;
  format.mBytesPerPacket = 4;
  if (AudioUnitSetProperty(g_output_unit, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 0, &format, sizeof(format)) != noErr) {
    lg::error("the audio device would not take 48 kHz stereo 16-bit");
    return false;
  }

  AURenderCallbackStruct callback = {};
  callback.inputProc = audio_render;
  if (AudioUnitSetProperty(g_output_unit, kAudioUnitProperty_SetRenderCallback,
                           kAudioUnitScope_Input, 0, &callback, sizeof(callback)) != noErr ||
      AudioUnitInitialize(g_output_unit) != noErr || AudioOutputUnitStart(g_output_unit) != noErr) {
    lg::error("could not start the audio device");
    return false;
  }
  lg::info("[sound] CoreAudio output running at {} Hz", goal_game_sound_sample_rate());
  return true;
}

void stop() {
  if (g_output_unit) {
    AudioOutputUnitStop(g_output_unit);
    AudioUnitUninitialize(g_output_unit);
    AudioComponentInstanceDispose(g_output_unit);
    g_output_unit = nullptr;
  }
}

}  // namespace goalpad_audio
