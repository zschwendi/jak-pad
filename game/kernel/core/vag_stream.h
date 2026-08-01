#pragma once

/*!
 * @file vag_stream.h
 * Streamed VAG audio - the spooled dialogue and cutscene audio in VAGWAD.<lang> - answered without
 * an IOP. See vag_stream.cpp.
 */

#include "common/common_types.h"

#include "game/overlord/common/ssound.h"

struct VagStreamEntry {
  char name[8];
  u32 offset;  // in 2048-byte sectors, into VAGWAD.<lang>
};

namespace vag_stream {

/*! Read VAGDIR.AYB out of the data directory and take the raw SPU voice the stream plays on.
 *  `pan_table` is the 361-entry table the sound RPC built; the positioned volume math uses it.
 *  Returns false and leaves the subsystem unavailable when there is no VAGDIR.AYB. */
bool install(const VolumePair* pan_table);
void shutdown();
bool installed();

/*! Which `VAGWAD.<lang>` the streams are read out of. The loader channel's SET_LANGUAGE command
 *  chooses it, the same way it chooses which language the EE asks for. */
void set_language(const char* extension);

/*! Find a stream by its 8-byte, space-padded, uppercased name. Null when the directory has none. */
const VagStreamEntry* find(const char* name);

void play(const VagStreamEntry* vag, u32 sound_id, s32 volume, u32 priority, const Vec3w* trans);
void queue(const VagStreamEntry* vag, u32 sound_id, u32 priority);
void stop(const VagStreamEntry* vag, u32 priority);
void pause();
void unpause();
void set_stream_volume(s32 volume);
void set_dialog_volume(s32 volume);

/*! `SetVAGVol`: re-apply the current volume, which the listener may have moved. */
void update_volume();

/*! `GetVAGStreamPos`: what the sound-iop-info block's `strpos` reports, and what
 *  `str-is-playing?` reads. -1 when nothing is streaming. */
s32 stream_pos();

/*! The sound id the stream is playing under, which the player channel matches pause/stop/param
 *  commands against when no `Sound` slot owns the id. */
s32 stream_id();

/*! One iteration of the ISO thread's VAG work: advance the state machine, read the next buffer
 *  when the voice needs one, and stop the stream when it has run out. Called once per frame. */
void frame();

struct stats {
  int streams_started;
  int streams_missing;  // named in a play request, but not in VAGDIR.AYB
  int buffers_read;
  int streams_finished;
};
stats get_stats();

}  // namespace vag_stream
