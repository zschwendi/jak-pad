/*!
 * @file jak2_apple_audio.mm
 * See jak2_apple_audio.h.
 */

#include "game/kernel/core/jak2_apple_audio.h"

#import <TargetConditionals.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>

#include "game/kernel/core/kernel_game.h"
#import <AVFAudio/AVFAudio.h>
#import <AudioToolbox/AudioToolbox.h>

#if !TARGET_OS_IOS
#error "jak2_apple_audio.mm is only supported on iOS and the iOS simulator"
#endif

namespace {

constexpr int kSampleRate = 48000;
constexpr UInt32 kOutputBus = 0;
constexpr UInt32 kBytesPerFrame = 2 * sizeof(int16_t);

struct AtomicStats {
  std::atomic<uint64_t> start_attempts{0};
  std::atomic<uint64_t> starts{0};
  std::atomic<uint64_t> suspends{0};
  std::atomic<uint64_t> resumes{0};
  std::atomic<uint64_t> render_callbacks{0};
  std::atomic<uint64_t> frames_requested{0};
  std::atomic<uint64_t> frames_rendered{0};
  std::atomic<uint64_t> silent_frames{0};
  std::atomic<uint64_t> underruns{0};
  std::atomic<uint64_t> invalid_buffers{0};
};

std::mutex g_lifecycle_mutex;
AudioUnit g_output_unit = nullptr;
bool g_output_initialized = false;
bool g_output_started = false;
bool g_session_active = false;
AtomicStats g_stats;
std::atomic<int32_t> g_state{GOAL_JAK2_APPLE_AUDIO_CLOSED};
std::atomic<int64_t> g_last_error_code{0};
std::atomic<const char*> g_last_error{""};

void clear_error() {
  g_last_error_code.store(0, std::memory_order_relaxed);
  g_last_error.store("", std::memory_order_release);
}

void set_error(const char* message, int64_t code) {
  g_last_error_code.store(code, std::memory_order_relaxed);
  g_last_error.store(message, std::memory_order_release);
}

int64_t error_code(NSError* error) {
  return error ? static_cast<int64_t>(error.code) : 0;
}

OSStatus audio_render(void* /*context*/,
                      AudioUnitRenderActionFlags* flags,
                      const AudioTimeStamp* /*timestamp*/,
                      UInt32 /*bus*/,
                      UInt32 requested_frames,
                      AudioBufferList* data) {
  g_stats.render_callbacks.fetch_add(1, std::memory_order_relaxed);
  g_stats.frames_requested.fetch_add(requested_frames, std::memory_order_relaxed);

  if (!data || data->mNumberBuffers == 0) {
    g_stats.invalid_buffers.fetch_add(1, std::memory_order_relaxed);
    g_stats.underruns.fetch_add(1, std::memory_order_relaxed);
    g_stats.silent_frames.fetch_add(requested_frames, std::memory_order_relaxed);
    if (flags) {
      *flags |= kAudioUnitRenderAction_OutputIsSilence;
    }
    return noErr;
  }

  for (UInt32 i = 0; i < data->mNumberBuffers; ++i) {
    AudioBuffer& buffer = data->mBuffers[i];
    if (buffer.mData && buffer.mDataByteSize) {
      std::memset(buffer.mData, 0, buffer.mDataByteSize);
    }
  }

  AudioBuffer& output = data->mBuffers[0];
  const UInt32 buffer_capacity = output.mDataByteSize / kBytesPerFrame;
  const UInt32 pull_frames =
      std::min({requested_frames, buffer_capacity, static_cast<UInt32>(INT_MAX)});
  if (!output.mData || output.mNumberChannels != 2 || data->mNumberBuffers != 1 ||
      buffer_capacity < requested_frames) {
    g_stats.invalid_buffers.fetch_add(1, std::memory_order_relaxed);
  }

  int rendered = 0;
  if (output.mData && output.mNumberChannels == 2 && pull_frames > 0) {
    rendered = goal_game_sound_pull_audio(static_cast<int16_t*>(output.mData),
                                          static_cast<int>(pull_frames));
  }
  rendered = std::clamp(rendered, 0, static_cast<int>(pull_frames));
  g_stats.frames_rendered.fetch_add(static_cast<uint64_t>(rendered), std::memory_order_relaxed);

  const uint64_t silent_frames = requested_frames - static_cast<UInt32>(rendered);
  if (silent_frames) {
    g_stats.silent_frames.fetch_add(silent_frames, std::memory_order_relaxed);
    g_stats.underruns.fetch_add(1, std::memory_order_relaxed);
  }
  if (rendered == 0 && flags) {
    *flags |= kAudioUnitRenderAction_OutputIsSilence;
  }
  return noErr;
}

bool deactivate_session(bool record_error) {
  if (!g_session_active) {
    return true;
  }
  NSError* error = nil;
  const BOOL deactivated = [[AVAudioSession sharedInstance]
        setActive:NO
      withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
            error:&error];
  g_session_active = false;
  if (!deactivated && record_error) {
    set_error("Could not deactivate the iOS audio session.", error_code(error));
  }
  return deactivated;
}

bool close_locked(bool record_error) {
  bool succeeded = true;
  int64_t first_code = 0;
  const char* first_error = "";
  const auto note_failure = [&](const char* message, OSStatus status) {
    if (succeeded) {
      first_error = message;
      first_code = status;
    }
    succeeded = false;
  };

  if (g_output_unit && g_output_started) {
    const OSStatus status = AudioOutputUnitStop(g_output_unit);
    if (status != noErr) {
      note_failure("Could not stop the iOS RemoteIO output unit.", status);
    }
    g_output_started = false;
  }
  if (g_output_unit && g_output_initialized) {
    const OSStatus status = AudioUnitUninitialize(g_output_unit);
    if (status != noErr) {
      note_failure("Could not uninitialize the iOS RemoteIO output unit.", status);
    }
    g_output_initialized = false;
  }
  if (g_output_unit) {
    const OSStatus status = AudioComponentInstanceDispose(g_output_unit);
    if (status != noErr) {
      note_failure("Could not dispose the iOS RemoteIO output unit.", status);
    }
    g_output_unit = nullptr;
  }
  if (!deactivate_session(false)) {
    note_failure("Could not deactivate the iOS audio session.", 0);
  }
  g_state.store(GOAL_JAK2_APPLE_AUDIO_CLOSED, std::memory_order_release);
  if (!succeeded && record_error) {
    set_error(first_error, first_code);
  }
  return succeeded;
}

int fail_closed(const char* message, int64_t code) {
  set_error(message, code);
  close_locked(false);
  return 0;
}

bool activate_session() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  if (![session setPreferredSampleRate:kSampleRate error:&error]) {
    set_error("The iOS audio session rejected the 48 kHz preferred rate.", error_code(error));
    return false;
  }
  if (![session setActive:YES error:&error]) {
    set_error("Could not activate the iOS audio session.", error_code(error));
    return false;
  }
  g_session_active = true;
  return true;
}

bool output_format_matches(const AudioStreamBasicDescription& format) {
  return format.mSampleRate == kSampleRate && format.mFormatID == kAudioFormatLinearPCM &&
         (format.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0 &&
         (format.mFormatFlags & kAudioFormatFlagIsPacked) != 0 && format.mChannelsPerFrame == 2 &&
         format.mBitsPerChannel == 16 && format.mFramesPerPacket == 1 &&
         format.mBytesPerFrame == kBytesPerFrame && format.mBytesPerPacket == kBytesPerFrame;
}

int resume_locked() {
  if (g_state.load(std::memory_order_acquire) == GOAL_JAK2_APPLE_AUDIO_RUNNING) {
    return 1;
  }
  if (g_state.load(std::memory_order_acquire) != GOAL_JAK2_APPLE_AUDIO_SUSPENDED ||
      !g_output_unit || !g_output_initialized || g_output_started) {
    return fail_closed("The iOS audio output is not suspended.", 0);
  }
  clear_error();
  if (!activate_session()) {
    const char* message = g_last_error.load(std::memory_order_acquire);
    const int64_t code = g_last_error_code.load(std::memory_order_relaxed);
    return fail_closed(message, code);
  }
  const OSStatus status = AudioOutputUnitStart(g_output_unit);
  if (status != noErr) {
    return fail_closed("Could not resume the iOS RemoteIO output unit.", status);
  }
  g_output_started = true;
  g_state.store(GOAL_JAK2_APPLE_AUDIO_RUNNING, std::memory_order_release);
  g_stats.resumes.fetch_add(1, std::memory_order_relaxed);
  return 1;
}

}  // namespace

extern "C" {

int goal_jak2_apple_audio_start(void) {
  std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
  const int32_t state = g_state.load(std::memory_order_acquire);
  if (state == GOAL_JAK2_APPLE_AUDIO_RUNNING) {
    return 1;
  }
  if (state == GOAL_JAK2_APPLE_AUDIO_SUSPENDED) {
    return resume_locked();
  }

  clear_error();
  g_stats.start_attempts.fetch_add(1, std::memory_order_relaxed);
  if (goal_game_sound_sample_rate() != kSampleRate) {
    return fail_closed("The game audio seam is not configured for 48 kHz.", 0);
  }
  if (!activate_session()) {
    const char* message = g_last_error.load(std::memory_order_acquire);
    const int64_t code = g_last_error_code.load(std::memory_order_relaxed);
    return fail_closed(message, code);
  }

  AudioComponentDescription description = {};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_RemoteIO;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (!component) {
    return fail_closed("No iOS RemoteIO output component is available.", 0);
  }

  OSStatus status = AudioComponentInstanceNew(component, &g_output_unit);
  if (status != noErr) {
    return fail_closed("Could not create the iOS RemoteIO output unit.", status);
  }

  AudioStreamBasicDescription format = {};
  format.mSampleRate = kSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  format.mBytesPerPacket = kBytesPerFrame;
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = kBytesPerFrame;
  format.mChannelsPerFrame = 2;
  format.mBitsPerChannel = 16;
  status = AudioUnitSetProperty(g_output_unit, kAudioUnitProperty_StreamFormat,
                                kAudioUnitScope_Input, kOutputBus, &format, sizeof(format));
  if (status != noErr) {
    return fail_closed("The iOS RemoteIO unit rejected 48 kHz stereo signed 16-bit audio.", status);
  }

  AudioStreamBasicDescription accepted_format = {};
  UInt32 accepted_format_size = sizeof(accepted_format);
  status =
      AudioUnitGetProperty(g_output_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input,
                           kOutputBus, &accepted_format, &accepted_format_size);
  if (status != noErr || !output_format_matches(accepted_format)) {
    return fail_closed("The iOS RemoteIO unit did not retain the required mixer format.", status);
  }

  AURenderCallbackStruct callback = {};
  callback.inputProc = audio_render;
  status = AudioUnitSetProperty(g_output_unit, kAudioUnitProperty_SetRenderCallback,
                                kAudioUnitScope_Input, kOutputBus, &callback, sizeof(callback));
  if (status != noErr) {
    return fail_closed("Could not install the iOS audio render callback.", status);
  }

  status = AudioUnitInitialize(g_output_unit);
  if (status != noErr) {
    return fail_closed("Could not initialize the iOS RemoteIO output unit.", status);
  }
  g_output_initialized = true;
  status = AudioOutputUnitStart(g_output_unit);
  if (status != noErr) {
    return fail_closed("Could not start the iOS RemoteIO output unit.", status);
  }
  g_output_started = true;
  g_state.store(GOAL_JAK2_APPLE_AUDIO_RUNNING, std::memory_order_release);
  g_stats.starts.fetch_add(1, std::memory_order_relaxed);
  return 1;
}

int goal_jak2_apple_audio_suspend(void) {
  std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
  const int32_t state = g_state.load(std::memory_order_acquire);
  if (state == GOAL_JAK2_APPLE_AUDIO_CLOSED || state == GOAL_JAK2_APPLE_AUDIO_SUSPENDED) {
    return 1;
  }

  clear_error();
  const OSStatus status = AudioOutputUnitStop(g_output_unit);
  if (status != noErr) {
    return fail_closed("Could not suspend the iOS RemoteIO output unit.", status);
  }
  g_output_started = false;
  if (!deactivate_session(true)) {
    const char* message = g_last_error.load(std::memory_order_acquire);
    const int64_t code = g_last_error_code.load(std::memory_order_relaxed);
    return fail_closed(message, code);
  }
  g_state.store(GOAL_JAK2_APPLE_AUDIO_SUSPENDED, std::memory_order_release);
  g_stats.suspends.fetch_add(1, std::memory_order_relaxed);
  return 1;
}

int goal_jak2_apple_audio_resume(void) {
  std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
  return resume_locked();
}

int goal_jak2_apple_audio_close(void) {
  std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
  if (g_state.load(std::memory_order_acquire) == GOAL_JAK2_APPLE_AUDIO_CLOSED && !g_output_unit &&
      !g_session_active) {
    return 1;
  }
  clear_error();
  return close_locked(true) ? 1 : 0;
}

int goal_jak2_apple_audio_get_stats(goal_jak2_apple_audio_stats* out) {
  if (!out) {
    return 0;
  }
  goal_jak2_apple_audio_stats stats = {};
  stats.start_attempts = g_stats.start_attempts.load(std::memory_order_relaxed);
  stats.starts = g_stats.starts.load(std::memory_order_relaxed);
  stats.suspends = g_stats.suspends.load(std::memory_order_relaxed);
  stats.resumes = g_stats.resumes.load(std::memory_order_relaxed);
  stats.render_callbacks = g_stats.render_callbacks.load(std::memory_order_relaxed);
  stats.frames_requested = g_stats.frames_requested.load(std::memory_order_relaxed);
  stats.frames_rendered = g_stats.frames_rendered.load(std::memory_order_relaxed);
  stats.silent_frames = g_stats.silent_frames.load(std::memory_order_relaxed);
  stats.underruns = g_stats.underruns.load(std::memory_order_relaxed);
  stats.invalid_buffers = g_stats.invalid_buffers.load(std::memory_order_relaxed);
  stats.last_error_code = g_last_error_code.load(std::memory_order_relaxed);
  stats.sample_rate = kSampleRate;
  stats.state = g_state.load(std::memory_order_acquire);
  *out = stats;
  return 1;
}

const char* goal_jak2_apple_audio_last_error(void) {
  return g_last_error.load(std::memory_order_acquire);
}

}  // extern "C"
