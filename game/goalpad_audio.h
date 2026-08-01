#pragma once

/*!
 * @file goalpad_audio.h
 * The host's audio device: a CoreAudio default output unit whose render callback pulls
 * `goal_sound_pull_audio`.
 *
 * It is a translation unit of its own because Apple's MacTypes.h - which AudioToolbox drags in -
 * defines a `Ptr` typedef that collides with the GOAL kernel's `Ptr` template. The same collision
 * is why metal_kernel_bridge.cpp exists.
 */

namespace goalpad_audio {

// Opens the device and starts pulling. False (with the reason logged) if there is no device.
bool start();
void stop();

}  // namespace goalpad_audio
