/*!
 * @file jak2_sound_rpc_test.cpp
 * Behavioral coverage for Jak 2's startup state, checked sound-bank playback and ordinary/chunked
 * STR seams.
 */

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/common/str_rpc_types.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/kernel_game.h"
#include "game/kernel/core/sblk_preflight.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"
#include "game/sound/989snd/ame_handler.h"
#include "game/sound/sndshim.h"

namespace {

constexpr u32 kCommandSize = 0x50;
constexpr u32 kStrRequestSize = 0x40;
constexpr u32 kStrReplySize = 0x20;
constexpr u32 kGuardSize = 16;
int g_failures = 0;

void check(bool condition, const char* what) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    g_failures++;
  }
}

void check_u32(u32 got, u32 expected, const char* what) {
  if (got == expected) {
    std::printf("  ok   %-42s #x%08x\n", what, got);
  } else {
    std::printf("  FAIL %-42s got #x%08x, expected #x%08x\n", what, got, expected);
    g_failures++;
  }
}

void check_s32(s32 got, s32 expected, const char* what) {
  if (got == expected) {
    std::printf("  ok   %-42s %d\n", what, got);
  } else {
    std::printf("  FAIL %-42s got %d, expected %d\n", what, got, expected);
    g_failures++;
  }
}

struct GuardedCommand {
  Ptr<u8> allocation;
  Ptr<jak2::SoundRpcCommand> command;
};

GuardedCommand guarded_command(const char* name) {
  auto allocation = kmalloc(kglobalheap, kCommandSize + 2 * kGuardSize,
                            KMALLOC_MEMSET | KMALLOC_ALIGN_16, name);
  if (!allocation.offset) {
    check(false, "allocate a guarded RPC command");
    return {};
  }
  memset(allocation.c(), 0xa5, kCommandSize + 2 * kGuardSize);
  return {allocation, (allocation + kGuardSize).cast<jak2::SoundRpcCommand>()};
}

void check_guards(GuardedCommand& buffer, const char* what) {
  bool intact = true;
  for (u32 i = 0; i < kGuardSize; i++) {
    intact &= buffer.allocation.c()[i] == 0xa5;
    intact &= buffer.allocation.c()[kGuardSize + kCommandSize + i] == 0xa5;
  }
  check(intact, what);
}

struct GuardedBuffer {
  Ptr<u8> allocation;
  Ptr<u8> data;
  u32 size;
};

GuardedBuffer guarded_buffer(u32 size, const char* name) {
  auto allocation =
      kmalloc(kglobalheap, size + 2 * kGuardSize, KMALLOC_MEMSET | KMALLOC_ALIGN_16, name);
  if (!allocation.offset) {
    check(false, "allocate a guarded buffer");
    return {};
  }
  memset(allocation.c(), 0xa5, size + 2 * kGuardSize);
  return {allocation, allocation + kGuardSize, size};
}

void check_guards(GuardedBuffer& buffer, const char* what) {
  bool intact = true;
  for (u32 i = 0; i < kGuardSize; i++) {
    intact &= buffer.allocation.c()[i] == 0xa5;
    intact &= buffer.allocation.c()[kGuardSize + buffer.size + i] == 0xa5;
  }
  check(intact, what);
}

std::vector<u8> snapshot(GuardedBuffer& buffer) {
  return {buffer.data.c(), buffer.data.c() + buffer.size};
}

struct StrRequest {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
  char basename[32];
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

void reset_str_request(GuardedBuffer& buffer,
                       u32 address,
                       s32 section,
                       u32 maxlen,
                       const std::string& basename) {
  memset(buffer.data.c(), 0, buffer.size);
  auto* request = buffer.data.cast<StrRequest>().c();
  request->rsvd = 0x5aa5;
  request->result = 666;
  request->address = address;
  request->section = section;
  request->maxlen = maxlen;
  for (u32 i = 0; i < 4; i++) {
    request->dummy[i] = 0x11111111 * (i + 1);
  }
  memcpy(request->basename, basename.data(),
         std::min(basename.size(), sizeof(request->basename)));
}

bool write_fixture(const std::filesystem::path& path, const std::array<u8, 96>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return output.good();
}

bool write_fixture(const std::filesystem::path& path, const std::vector<u8>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return output.good();
}

template <typename T>
void write_value(std::vector<u8>* data, size_t offset, T value) {
  memcpy(data->data() + offset, &value, sizeof(value));
}

std::vector<u8> wrap_sfx_bank(const std::vector<u8>& bank, const std::vector<u8>& samples = {}) {
  constexpr u32 kAttributesSize = 24;
  std::vector<u8> result(kAttributesSize + bank.size() + samples.size(), 0);
  write_value(&result, 0, u32(1));
  write_value(&result, 4, u32(2));
  write_value(&result, 8, kAttributesSize);
  write_value(&result, 12, static_cast<u32>(bank.size()));
  write_value(&result, 16, static_cast<u32>(kAttributesSize + bank.size()));
  write_value(&result, 20, static_cast<u32>(samples.size()));
  memcpy(result.data() + kAttributesSize, bank.data(), bank.size());
  if (!samples.empty()) {
    memcpy(result.data() + kAttributesSize + bank.size(), samples.data(), samples.size());
  }
  return result;
}

std::vector<u8> minimal_sfx_bank() {
  std::vector<u8> bank(60, 0);
  write_value(&bank, 0, u32(0x6b6c4253));  // SBlk
  write_value(&bank, 4, u32(1));
  write_value(&bank, 28, u32(bank.size()));
  write_value(&bank, 32, u32(bank.size()));
  return wrap_sfx_bank(bank);
}

std::vector<u8> one_grain_sfx_bank() {
  constexpr size_t kHeaderSize = 60;
  constexpr size_t kDescriptorSize = 12;
  constexpr size_t kGrainSize = 0x28;
  std::vector<u8> bank(kHeaderSize + kDescriptorSize + kGrainSize, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(1));
  write_value(&bank, 22, s16(1));
  write_value(&bank, 24, s16(1));
  write_value(&bank, 28, u32(kHeaderSize));
  write_value(&bank, 32, u32(kHeaderSize + kDescriptorSize));
  write_value(&bank, kHeaderSize + 4, s8(1));
  write_value(&bank, kHeaderSize + 8, u32(0));
  write_value(&bank, kHeaderSize + kDescriptorSize, u32(0));  // NULL_GRAIN
  return wrap_sfx_bank(bank);
}

std::vector<u8> named_sfx_bank() {
  constexpr size_t kHeaderSize = 60;
  constexpr size_t kNamesSize = 0x98;
  constexpr size_t kNameEntrySize = 0x14;
  std::vector<u8> bank(kHeaderSize + kNamesSize + kNameEntrySize, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(1));
  write_value(&bank, 8, u32(0x100));
  write_value(&bank, 28, u32(kHeaderSize));
  write_value(&bank, 32, u32(kHeaderSize));
  write_value(&bank, 52, u32(kHeaderSize));
  write_value(&bank, kHeaderSize + 8, u32(kNamesSize));
  return wrap_sfx_bank(bank);
}

std::vector<u8> unterminated_names_sfx_bank() {
  constexpr size_t kHeaderSize = 60;
  constexpr size_t kDescriptorSize = 12;
  constexpr size_t kNamesSize = 0x98;
  constexpr size_t kNameEntrySize = 0x14;
  const size_t block_names = kHeaderSize + kDescriptorSize;
  const size_t name_table = block_names + kNamesSize;
  std::vector<u8> bank(name_table + kNameEntrySize, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(1));
  write_value(&bank, 8, u32(0x100));
  write_value(&bank, 22, s16(1));
  write_value(&bank, 28, u32(kHeaderSize));
  write_value(&bank, 32, u32(block_names));
  write_value(&bank, 52, static_cast<u32>(block_names));
  write_value(&bank, block_names + 8, u32(kNamesSize));
  write_value(&bank, name_table, u32(1));
  write_value(&bank, name_table + 0x10, s16(0));
  return wrap_sfx_bank(bank);
}

std::vector<u8> userdata_sfx_bank() {
  constexpr size_t kHeaderSize = 60;
  constexpr size_t kDescriptorSize = 12;
  std::vector<u8> bank(kHeaderSize + kDescriptorSize + 16, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(1));
  write_value(&bank, 8, u32(0x200));
  write_value(&bank, 22, s16(1));
  write_value(&bank, 28, u32(kHeaderSize));
  write_value(&bank, 32, u32(kHeaderSize + kDescriptorSize));
  write_value(&bank, 56, u32(kHeaderSize + kDescriptorSize));
  return wrap_sfx_bank(bank);
}

std::vector<u8> version_2_tone_bank() {
  constexpr size_t kHeaderSize = 64;
  constexpr size_t kDescriptorSize = 12;
  constexpr size_t kGrainSize = 8;
  constexpr size_t kToneSize = 24;
  const size_t grain_data = kHeaderSize + kDescriptorSize + kGrainSize;
  std::vector<u8> bank(grain_data + kToneSize, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(2));
  write_value(&bank, 22, s16(1));
  write_value(&bank, 24, s16(1));
  write_value(&bank, 28, u32(kHeaderSize));
  write_value(&bank, 32, u32(kHeaderSize + kDescriptorSize));
  write_value(&bank, 52, static_cast<u32>(grain_data));
  write_value(&bank, kHeaderSize + 4, s8(1));
  write_value(&bank, kHeaderSize + 8, u32(0));
  write_value(&bank, kHeaderSize + kDescriptorSize, u32(1u << 24));  // TONE at GrainData + 0
  write_value(&bank, grain_data + 16, u32(0));
  return wrap_sfx_bank(bank, std::vector<u8>{0, 0});
}

std::vector<u8> playable_named_sfx_bank(
    const char* sound_name = "TEST_TONE",
    const std::array<u32, 4>& user_data = {}) {
  constexpr size_t kHeaderSize = 64;
  constexpr size_t kDescriptorSize = 12;
  constexpr size_t kGrainSize = 8;
  constexpr size_t kToneSize = 24;
  constexpr size_t kNamesSize = 0x98;
  constexpr size_t kNameEntrySize = 0x14;
  constexpr size_t kSampleSize = 16;
  const size_t first_sound = kHeaderSize;
  const size_t first_grain = first_sound + kDescriptorSize;
  const size_t grain_data = first_grain + kGrainSize;
  const size_t block_names = grain_data + kToneSize;
  const size_t name_table = block_names + kNamesSize;
  const bool has_user_data = std::any_of(user_data.begin(), user_data.end(), [](u32 value) {
    return value != 0;
  });
  const size_t user_data_offset = name_table + 2 * kNameEntrySize;
  std::vector<u8> bank(user_data_offset + (has_user_data ? 16 : 0), 0);

  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(2));
  write_value(&bank, 8, u32(0x100 | (has_user_data ? 0x200 : 0)));  // names, optional userdata
  write_value(&bank, 22, s16(1));
  write_value(&bank, 24, s16(1));
  write_value(&bank, 28, static_cast<u32>(first_sound));
  write_value(&bank, 32, static_cast<u32>(first_grain));
  write_value(&bank, 52, static_cast<u32>(grain_data));
  write_value(&bank, 56, static_cast<u32>(block_names));
  if (has_user_data) {
    write_value(&bank, 60, static_cast<u32>(user_data_offset));
  }

  write_value(&bank, first_sound + 0, s8(127));
  write_value(&bank, first_sound + 1, s8(0));
  write_value(&bank, first_sound + 2, s16(0));
  write_value(&bank, first_sound + 4, s8(1));
  write_value(&bank, first_sound + 8, u32(0));

  write_value(&bank, first_grain, u32(1u << 24));  // TONE at GrainData + 0
  write_value(&bank, grain_data + 1, s8(127));
  write_value(&bank, grain_data + 2, s8(60));
  write_value(&bank, grain_data + 7, s8(127));
  write_value(&bank, grain_data + 8, s8(12));
  write_value(&bank, grain_data + 9, s8(12));
  write_value(&bank, grain_data + 10, u16(0x000f));
  write_value(&bank, grain_data + 12, u16(0x1fc0));
  write_value(&bank, grain_data + 16, u32(0));

  memcpy(bank.data() + block_names, "PLAY", 4);
  write_value(&bank, block_names + 8, u32(kNamesSize));
  memcpy(bank.data() + name_table, sound_name, std::min(strlen(sound_name), size_t(16)));
  write_value(&bank, name_table + 16, s16(0));
  if (has_user_data) {
    for (size_t index = 0; index < user_data.size(); index++) {
      write_value(&bank, user_data_offset + index * sizeof(u32), user_data[index]);
    }
  }

  std::vector<u8> samples(kSampleSize, 0x77);
  samples[0] = 0;
  samples[1] = 1;  // end after this synthetic PS-ADPCM block
  return wrap_sfx_bank(bank, samples);
}

std::vector<u8> shared_reference_budget_sfx_bank() {
  constexpr size_t kHeaderSize = 64;
  constexpr size_t kSoundSize = 12;
  constexpr size_t kGrainSize = 8;
  constexpr size_t kSoundCount = 259;
  constexpr size_t kGrainCount = 127;
  constexpr size_t kFirstSound = kHeaderSize;
  constexpr size_t kFirstGrain = kFirstSound + kSoundCount * kSoundSize;
  constexpr size_t kGrainData = kFirstGrain + kGrainCount * kGrainSize;
  std::vector<u8> bank(kGrainData, 0);
  write_value(&bank, 0, u32(0x6b6c4253));
  write_value(&bank, 4, u32(2));
  write_value(&bank, 22, s16(kSoundCount));
  write_value(&bank, 24, s16(kGrainCount));
  write_value(&bank, 28, u32(kFirstSound));
  write_value(&bank, 32, u32(kFirstGrain));
  write_value(&bank, 52, u32(kGrainData));
  for (size_t sound = 0; sound < kSoundCount; sound++) {
    const s8 references = sound + 1 == kSoundCount ? 1 : s8(kGrainCount);
    write_value(&bank, kFirstSound + sound * kSoundSize + 4, references);
    write_value(&bank, kFirstSound + sound * kSoundSize + 8, u32(0));
  }
  for (size_t grain = 0; grain < kGrainCount; grain++) {
    write_value(&bank, kFirstGrain + grain * kGrainSize, u32(24) << 24);
  }
  return wrap_sfx_bank(bank);
}

using GoalEightArgumentFunction =
    u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
using GoalOneArgumentFunction = u64 (*)(u64);

template <typename Function>
Function native_entry(const char* name, u32* object_out = nullptr) {
  u32 object = 0;
  if (goal_kernel_core_lookup(name, nullptr, &object) != GOAL_KERNEL_CORE_OK || !object) {
    check(false, name);
    return nullptr;
  }
  uintptr_t entry = 0;
  memcpy(&entry, Ptr<u8>(object).c(), sizeof(entry));
  if (object_out) {
    *object_out = object;
  }
  check(entry != 0, name);
  return reinterpret_cast<Function>(entry);
}

void reset_command(GuardedCommand& buffer, jak2::Jak2SoundCommand command, u32 ee_addr) {
  memset(buffer.command.c(), 0, kCommandSize);
  buffer.command->rsvd1 = 0x5aa5;
  buffer.command->j2command = command;
  buffer.command->irx_version.major = 0xdeadbeef;
  buffer.command->irx_version.minor = 0xcafef00d;
  buffer.command->irx_version.ee_addr = ee_addr;
  memset(buffer.command->max_size + sizeof(SoundRpcGetIrxVersion), 0x3c,
         sizeof(buffer.command->max_size) - sizeof(SoundRpcGetIrxVersion));
}

std::array<char, 16> bank_name(const char* text) {
  std::array<char, 16> result{};
  memcpy(result.data(), text, std::min(strlen(text), result.size()));
  return result;
}

void reset_bank_command(GuardedCommand& buffer, const std::array<char, 16>& name) {
  memset(buffer.command.c(), 0, kCommandSize);
  buffer.command->rsvd1 = 0x5aa5;
  buffer.command->j2command = jak2::Jak2SoundCommand::load_bank;
  memcpy(buffer.command->load_bank.bank_name, name.data(), name.size());
}

void reset_language_command(GuardedCommand& buffer, u32 language_id) {
  memset(buffer.command.c(), 0, kCommandSize);
  buffer.command->rsvd1 = 0x5aa5;
  buffer.command->j2command = jak2::Jak2SoundCommand::set_language;
  buffer.command->set_language.langauge_id = language_id;
}

jak2::SoundRpcCommand* reset_player_command(GuardedBuffer& buffer,
                                             u32 index,
                                             jak2::Jak2SoundCommand command) {
  auto* result = (buffer.data + index * kCommandSize).cast<jak2::SoundRpcCommand>().c();
  memset(result, 0, kCommandSize);
  result->rsvd1 = 0x5aa5;
  result->j2command = command;
  return result;
}

void set_player_master_volume(GuardedBuffer& buffer, u32 index, u8 groups, s32 volume) {
  auto* command =
      reset_player_command(buffer, index, jak2::Jak2SoundCommand::set_master_volume);
  command->master_volume.group.group = groups;
  command->master_volume.volume = volume;
}

void set_player_midi(GuardedBuffer& buffer,
                     u32 index,
                     s32 reg,
                     s16 value,
                     u16 adjacent_padding = 0) {
  auto* command = reset_player_command(buffer, index, jak2::Jak2SoundCommand::set_midi_reg);
  command->midi_reg.reg = reg;
  memcpy(reinterpret_cast<u8*>(&command->midi_reg) + 4, &value, sizeof(value));
  memcpy(reinterpret_cast<u8*>(&command->midi_reg) + 6, &adjacent_padding,
         sizeof(adjacent_padding));
}

void set_player_fps(GuardedBuffer& buffer, u32 index, u8 fps) {
  auto* command = reset_player_command(buffer, index, jak2::Jak2SoundCommand::set_fps);
  command->fps.fps = fps;
}

void set_player_play(GuardedBuffer& buffer,
                     u32 index,
                     u32 sound_id,
                     const std::array<char, 16>& name,
                     s32 volume = 1024) {
  auto* command = reset_player_command(buffer, index, jak2::Jak2SoundCommand::play);
  command->play.sound_id = sound_id;
  memcpy(command->play.name, name.data(), name.size());
  command->play.parms.volume = volume;
}

void set_player_ear(GuardedBuffer& buffer,
                    u32 index,
                    const Vec3w& ear1,
                    const Vec3w& ear0,
                    const Vec3w& camera,
                    s32 angle) {
  auto* command = reset_player_command(buffer, index, jak2::Jak2SoundCommand::set_ear_trans);
  command->ear_trans_j2.ear_trans1 = ear1;
  command->ear_trans_j2.ear_trans0 = ear0;
  command->ear_trans_j2.cam_trans = camera;
  command->ear_trans_j2.cam_angle = angle;
}

goal_jak2_sound_player_state player_state() {
  goal_jak2_sound_player_state result{};
  goal_jak2_sound_player_state_get(&result);
  return result;
}

bool same_player_state(const goal_jak2_sound_player_state& left,
                       const goal_jak2_sound_player_state& right) {
  return memcmp(&left, &right, sizeof(left)) == 0;
}

std::array<u8, kCommandSize> snapshot(GuardedCommand& buffer) {
  std::array<u8, kCommandSize> result;
  memcpy(result.data(), buffer.command.c(), result.size());
  return result;
}

void check_version_reply(GuardedCommand& buffer, u32 ee_addr, const char* prefix) {
  check_u32((u32)buffer.command->j2command, 16, prefix);
  check_u32(buffer.command->irx_version.major, 4, "reply major version");
  check_u32(buffer.command->irx_version.minor, 0, "reply minor version");
  check_u32(buffer.command->irx_version.ee_addr, ee_addr, "reply preserves the EE info address");
  check_u32(buffer.command->rsvd1, 0x5aa5, "reply preserves the reserved header");
}

}  // namespace

int main() {
  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  auto valid_bank = minimal_sfx_bank();
  auto truncated_bank = valid_bank;
  truncated_bank.resize(23);
  auto overflow_bank = valid_bank;
  write_value(&overflow_bank, 12, u32(0xffffffff));
  auto negative_count_bank = one_grain_sfx_bank();
  write_value(&negative_count_bank, 24 + 22, s16(-1));
  auto negative_sound_grain_count_bank = one_grain_sfx_bank();
  write_value(&negative_sound_grain_count_bank, 24 + 60 + 4, s8(-1));
  auto bad_grain_offset_bank = one_grain_sfx_bank();
  write_value(&bad_grain_offset_bank, 24 + 60 + 8, u32(0xfffffff8));
  auto bad_grain_type_bank = one_grain_sfx_bank();
  write_value(&bad_grain_type_bank, 24 + 60 + 12, u32(45));
  auto valid_names_bank = named_sfx_bank();
  auto bad_name_hash_bank = valid_names_bank;
  write_value(&bad_name_hash_bank, 24 + 60 + 0x18, s16(-1));
  auto bad_name_terminator_bank = unterminated_names_sfx_bank();
  auto valid_userdata_bank = userdata_sfx_bank();
  auto bad_userdata_bank = valid_userdata_bank;
  write_value(&bad_userdata_bank, 24 + 56, u32(0xffffffff));
  auto valid_v2_bank = version_2_tone_bank();
  auto playable_bank = playable_named_sfx_bank("TEST_TONE", {5, 30, 0xff, 0});
  auto shared_reference_budget_bank = shared_reference_budget_sfx_bank();
  auto bad_v2_offset_bank = valid_v2_bank;
  write_value(&bad_v2_offset_bank, 24 + 64 + 12, u32(0x01ffffff));
  auto bad_sample_offset_bank = valid_v2_bank;
  write_value(&bad_sample_offset_bank, 24 + 64 + 12 + 8 + 16, u32(1));

  std::printf("\n== non-aborting SBlk boundary ==\n");
  check(bool(sblk_preflight::validate(valid_bank)), "minimal original SBlk fixture validates");
  check(bool(sblk_preflight::validate(one_grain_sfx_bank())),
        "one-sound version-1 SBlk fixture validates");
  check(bool(sblk_preflight::validate(valid_names_bank)), "bounded names-table fixture validates");
  check(bool(sblk_preflight::validate(valid_userdata_bank)), "bounded userdata fixture validates");
  check(bool(sblk_preflight::validate(valid_v2_bank)), "version-2 grain-data fixture validates");
  check(bool(sblk_preflight::validate(playable_bank)),
        "named playable fixture with hostile falloff userdata validates structurally");
  check(bool(sblk_preflight::validate(shared_reference_budget_bank)),
        "exact decoded-grain budget fixture validates");
  check(!sblk_preflight::validate(truncated_bank), "truncated outer attributes are rejected");
  check(!sblk_preflight::validate(overflow_bank), "overflowing outer chunk is rejected");
  check(!sblk_preflight::validate(negative_count_bank), "negative sound count is rejected");
  check(!sblk_preflight::validate(negative_sound_grain_count_bank),
        "negative per-sound grain count is rejected");
  check(!sblk_preflight::validate(bad_grain_offset_bank),
        "nested per-sound grain offset is rejected");
  check(!sblk_preflight::validate(bad_grain_type_bank),
        "out-of-range grain dispatch type is rejected");
  check(!sblk_preflight::validate(bad_name_hash_bank), "negative name hash offset is rejected");
  check(!sblk_preflight::validate(bad_name_terminator_bank),
        "unterminated name hash chain is rejected");
  check(!sblk_preflight::validate(bad_userdata_bank), "out-of-range userdata is rejected");
  check(!sblk_preflight::validate(bad_v2_offset_bank),
        "out-of-range version-2 GrainData payload is rejected");
  check(!sblk_preflight::validate(bad_sample_offset_bank),
        "out-of-range tone sample offset is rejected");

  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_NOT_INITIALIZED,
        "installation rejects an uninitialized kernel");
  check_s32(goal_game_sound_sample_rate(), 48000,
            "the game-neutral audio seam reports the Jak 2 mixer rate");
  std::array<s16, 8> stopped_audio;
  stopped_audio.fill(0x1234);
  check_s32(goal_game_sound_pull_audio(stopped_audio.data(), stopped_audio.size() / 2), 0,
            "the game-neutral audio seam does not pull before sound installation");
  check(std::all_of(stopped_audio.begin(), stopped_audio.end(),
                    [](s16 sample) { return sample == 0x1234; }),
        "an uninstalled audio pull leaves the host buffer untouched");
  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    std::printf("FAIL kernel initialization: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  check(goal_kernel_core_stub_machine_layer(0) == GOAL_KERNEL_CORE_OK,
        "reporting-mode machine stubs install");

  u32 stub_call = 0;
  u32 stub_busy = 0;
  goal_kernel_core_lookup("rpc-call", nullptr, &stub_call);
  goal_kernel_core_lookup("rpc-busy?", nullptr, &stub_busy);
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_OK,
        "Jak 2 sound loader and 989snd install");
  check(goal_jak2_sound_rpc_is_installed(), "the Jak 2 sound owner reports installed");
  check(gLanguage && strcmp(gLanguage, "ENG") == 0,
        "sound installation starts from the English language default");
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_ALREADY_INITIALIZED,
        "a duplicate install preserves the owned 989snd instance");

  u32 installed_call = 0;
  u32 installed_busy = 0;
  auto rpc_call = native_entry<GoalEightArgumentFunction>("rpc-call", &installed_call);
  auto rpc_busy = native_entry<GoalOneArgumentFunction>("rpc-busy?", &installed_busy);
  check(installed_call != stub_call, "rpc-call replaces its reporting stub");
  check(installed_busy != stub_busy, "rpc-busy? replaces its reporting stub");

  auto send = guarded_command("jak2-sound-rpc-send");
  auto recv = guarded_command("jak2-sound-rpc-recv");
  if (!send.command.offset || !recv.command.offset || !rpc_call || !rpc_busy) {
    goal_kernel_core_shutdown();
    return 1;
  }

  std::printf("\n== separate send and receive buffers ==\n");
  reset_command(send, jak2::Jak2SoundCommand::get_irx_version, 0x12345678);
  const auto original = snapshot(send);
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset,
                         kCommandSize, 0),
            0, "rpc-call returns synchronously");
  check(snapshot(send) == original, "the EE send buffer remains untouched");
  check_version_reply(recv, 0x12345678, "reply preserves command 16");
  check(memcmp(original.data() + 16, recv.command.cast<u8>().c() + 16, kCommandSize - 16) == 0,
        "bytes outside the version fields are preserved");
  check_guards(send, "send-buffer canaries stay intact");
  check_guards(recv, "receive-buffer canaries stay intact");

  std::printf("\n== in-place reply, as check-irx-version uses it ==\n");
  reset_command(send, jak2::Jak2SoundCommand::get_irx_version, 0x23456789);
  check_u32((u32)rpc_call(1, 99, 0, send.command.offset, kCommandSize, send.command.offset,
                         kCommandSize, 0),
            0, "loader fno and async mode do not change the handler");
  check_version_reply(send, 0x23456789, "in-place reply preserves command 16");
  check_u32((u32)rpc_busy(1), 0, "the synchronous loader channel is never busy");

  goal_jak2_sound_rpc_stats stats;
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.version_requests, 2, "two version requests were handled");
  check_u32(stats.info_ee, 0x23456789, "the latest EE info address is retained");

  std::printf("\n== exact-buffer no-reply language selection ==\n");
  constexpr std::array<const char*, 8> kExpectedLanguages = {"ENG", "FRE", "GER", "SPA",
                                                              "ITA", "JAP", "KOR", "UKE"};
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto language_recv = snapshot(recv);
  for (u32 language_id = 0; language_id < static_cast<u32>(kExpectedLanguages.size());
       language_id++) {
    reset_language_command(send, language_id);
    const auto language_send = snapshot(send);
    check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
              "a valid language selection remains synchronous");
    check(snapshot(send) == language_send && snapshot(recv) == language_recv,
          "a valid language selection mutates neither EE buffer");
    check(gLanguage && strcmp(gLanguage, kExpectedLanguages[language_id]) == 0,
          "each language id selects its wire tag");
  }

  reset_language_command(send, static_cast<u32>(kExpectedLanguages.size()));
  const auto invalid_language_send = snapshot(send);
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "an invalid language id fails synchronously");
  check(snapshot(send) == invalid_language_send && snapshot(recv) == language_recv,
        "an invalid language leaves both EE buffers untouched");
  check(gLanguage && strcmp(gLanguage, "UKE") == 0,
        "an invalid language id preserves the last valid language");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.language_requests, 9, "every well-framed language request is counted");
  check_u32(stats.language_failures, 1, "the invalid language id is counted as a failure");
  check_u32(stats.language_id, 7, "language stats retain the last valid id");
  check_guards(send, "no-reply language send canaries stay intact");
  check_guards(recv, "no-reply language receive canaries stay intact");

  std::printf("\n== atomic startup-state player batch ==\n");
  const auto initial_player_state = player_state();
  bool default_master_volumes = true;
  for (s32 volume : initial_player_state.master_volumes) {
    default_master_volumes &= volume == 0x400;
  }
  check(default_master_volumes, "installation initializes all 32 retained master volumes");
  check_u32(initial_player_state.fps, 60, "installation initializes the player FPS");
  check_u32(initial_player_state.midi_register_mask, 0,
            "installation has no retained MIDI writes");
  check_u32(initial_player_state.reverb_seen, 0, "installation has no retained reverb command");
  check_u32(initial_player_state.ear_transform_seen, 0,
            "installation has no retained listener transform");

  constexpr u32 kStartupPlayerCommands = 10;
  auto player = guarded_buffer(kStartupPlayerCommands * kCommandSize, "jak2-player-startup");
  if (!player.data.offset) {
    goal_kernel_core_shutdown();
    return 1;
  }
  set_player_master_volume(player, 0, 0x01, 1024);
  set_player_master_volume(player, 1, 0x02, 768);
  set_player_master_volume(player, 2, 0x24, 1024);
  set_player_master_volume(player, 3, 0x10, 1024);
  set_player_midi(player, 4, 14, 0);
  auto* reverb = reset_player_command(player, 5, jak2::Jak2SoundCommand::set_reverb);
  reverb->reverb.core = 3;
  reverb->reverb.reverb = 4;
  reverb->reverb.left = 0;
  reverb->reverb.right = 0;
  set_player_midi(player, 6, 3, 0);
  set_player_midi(player, 7, 4, 0);
  set_player_fps(player, 8, 60);
  auto* ear = reset_player_command(player, 9, jak2::Jak2SoundCommand::set_ear_trans);
  ear->ear_trans_j2.ear_trans1 = {11, 12, 13};
  ear->ear_trans_j2.ear_trans0 = {21, 22, 23};
  ear->ear_trans_j2.cam_trans = {31, 32, 33};
  ear->ear_trans_j2.cam_angle = 270;
  const auto startup_player_bytes = snapshot(player);

  check_u32((u32)rpc_busy(0), 0, "the synchronous player channel is never busy");
  check_u32((u32)rpc_call(0, 0, 1, player.data.offset, player.size, 0, 0, 0), 0,
            "the exact startup player batch completes synchronously");
  check(snapshot(player) == startup_player_bytes, "the player batch remains read-only");
  check_guards(player, "startup player batch canaries stay intact");

  auto state = player_state();
  check_s32(state.master_volumes[0], 1024, "SFX master volume is retained");
  check_s32(state.master_volumes[1], 768, "music master volume is retained without playback");
  check_s32(state.master_volumes[2], 1024, "dialog master volume is retained");
  check_s32(state.master_volumes[4], 1024, "ambient master volume is retained");
  check_s32(state.master_volumes[5], 1024, "dialog2 master volume is retained");
  check_s32(state.master_volumes[31], 0x400, "unaddressable master groups keep their default");
  check_u32(state.midi_register_mask, (1u << 3) | (1u << 4) | (1u << 14),
            "only the startup MIDI registers are retained");
  check_s32(state.midi_registers[3], 0, "mode MIDI register is retained as a no-op");
  check_s32(state.midi_registers[4], 0, "tune MIDI register is retained as a no-op");
  check_s32(state.midi_registers[14], 0, "flava MIDI register is retained as a no-op");
  check_u32(state.reverb_seen, 1, "the explicitly unsupported reverb state is retained");
  check_u32(state.reverb_core, 3, "reverb core is retained");
  check_s32(state.reverb_type, 4, "reverb type is retained");
  check_u32(state.fps, 60, "player FPS is retained");
  check_u32(state.ear_transform_seen, 1, "listener transform state is retained");
  check_s32(state.ear_trans1[0], 11, "ear1 wire order is retained");
  check_s32(state.ear_trans0[0], 21, "ear0 wire order is retained");
  check_s32(state.camera_trans[0], 31, "camera transform is retained");
  check_s32(state.camera_angle, 270, "camera angle is retained");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.player_batches, 1, "one startup player batch is applied");
  check_u32(stats.player_commands, kStartupPlayerCommands,
            "every startup state command is applied");
  check_u32(stats.player_failures, 0, "the exact startup player batch has no failure");

  std::printf("\n== player order and atomic rejection ==\n");
  constexpr u32 kOrderedPlayerCommands = 7;
  auto ordered = guarded_buffer(kOrderedPlayerCommands * kCommandSize, "jak2-player-order");
  set_player_master_volume(ordered, 0, 0x04, 111);
  set_player_master_volume(ordered, 1, 0x04, 222);
  set_player_midi(ordered, 2, 3, 5);
  set_player_midi(ordered, 3, 3, -1234, 0x7abc);
  set_player_midi(ordered, 4, 16, 77);
  set_player_fps(ordered, 5, 50);
  set_player_fps(ordered, 6, 60);
  const auto ordered_bytes = snapshot(ordered);
  rpc_call(0, 0, 1, ordered.data.offset, ordered.size, 0, 0, 0);
  state = player_state();
  check_s32(state.master_volumes[2], 222, "master-volume commands apply in wire order");
  check_s32(state.midi_registers[3], -1234,
            "signed 16-bit MIDI value ignores adjacent padding");
  check_s32(state.midi_registers[16], 77, "global-excite MIDI state is retained");
  check_u32(snd::GlobalExcite, 77, "global-excite MIDI value reaches 989snd");
  check_u32(state.fps, 60, "FPS commands apply in wire order");
  check(snapshot(ordered) == ordered_bytes, "ordered state batch remains read-only");
  check_guards(ordered, "ordered player batch canaries stay intact");

  auto atomic = guarded_buffer(3 * kCommandSize, "jak2-player-atomic");
  const auto state_before_rejection = player_state();
  set_player_master_volume(atomic, 0, 0x02, 333);
  set_player_play(atomic, 1, 0x7001, bank_name("test-tone"));
  reset_player_command(atomic, 2, jak2::Jak2SoundCommand::pause_sound);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "an unsupported command rejects state plus PLAY before mutation");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.play_requests, 0, "atomic rejection starts no earlier PLAY command");

  set_player_master_volume(atomic, 0, 0x02, 444);
  reset_player_command(atomic, 1, static_cast<jak2::Jak2SoundCommand>(0xffff));
  set_player_fps(atomic, 2, 50);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "an unknown command rejects the complete batch atomically");

  set_player_master_volume(atomic, 0, 0x02, 555);
  set_player_fps(atomic, 1, 0);
  set_player_fps(atomic, 2, 50);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "zero FPS rejects the complete batch and preserves FPS");

  set_player_master_volume(atomic, 0, 0x02, 666);
  set_player_midi(atomic, 1, 2, 1);
  set_player_fps(atomic, 2, 50);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "an unsupported MIDI register rejects the complete batch atomically");

  set_player_master_volume(atomic, 0, 0x02, 777);
  auto* unsafe_curve =
      reset_player_command(atomic, 1, jak2::Jak2SoundCommand::play);
  unsafe_curve->play.sound_id = 0x7004;
  unsafe_curve->play.parms.mask = 0x100;
  unsafe_curve->play.parms.fo_curve = -1;
  set_player_fps(atomic, 2, 50);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "an out-of-range explicit falloff curve rejects the complete batch");

  set_player_master_volume(atomic, 0, 0x02, 888);
  auto* unsafe_distance =
      reset_player_command(atomic, 1, jak2::Jak2SoundCommand::play);
  unsafe_distance->play.sound_id = 0x7005;
  unsafe_distance->play.parms.mask = 0x40;
  unsafe_distance->play.parms.fo_min = -1;
  set_player_fps(atomic, 2, 50);
  rpc_call(0, 0, 1, atomic.data.offset, atomic.size, 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "a negative explicit falloff distance rejects the complete batch");
  check_guards(atomic, "atomically rejected player batch canaries stay intact");

  std::printf("\n== strict player framing ==\n");
  rpc_call(0, 1, 1, player.data.offset, kCommandSize, 0, 0, 0);
  rpc_call(0, 0, 0, player.data.offset, kCommandSize, 0, 0, 0);
  rpc_call(0, 0, 1, player.data.offset, kCommandSize, player.data.offset, 0, 0);
  rpc_call(0, 0, 1, player.data.offset, kCommandSize, 0, 1, 0);
  rpc_call(0, 0, 1, player.data.offset, 0, 0, 0, 0);
  rpc_call(0, 0, 1, player.data.offset, kCommandSize - 1, 0, 0, 0);
  rpc_call(0, 0, 1, player.data.offset, 129 * kCommandSize, 0, 0, 0);
  rpc_call(0, 0, 1, player.data.offset + 1, kCommandSize, 0, 0, 0);
  rpc_call(0, 0, 1, EE_MAIN_MEM_SIZE - kCommandSize + 1, kCommandSize, 0, 0, 0);
  rpc_call(0, 0, 1, player.data.offset,
           static_cast<u64>(static_cast<s64>(-static_cast<s32>(kCommandSize))), 0, 0, 0);
  check(same_player_state(player_state(), state_before_rejection),
        "malformed player framing never mutates retained state");
  check(snapshot(player) == startup_player_bytes, "malformed framing leaves the EE batch intact");
  check_guards(player, "malformed player framing preserves canaries");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.player_batches, 2, "rejected batches do not count as applied batches");
  check_u32(stats.player_commands, kStartupPlayerCommands + kOrderedPlayerCommands,
            "rejected batches do not count commands as applied");
  check_u32(stats.player_failures, 16, "every semantic or framing rejection is counted");
  u32 player_rejected_calls = stats.rejected_calls;

  const auto fixture_root =
      std::filesystem::temp_directory_path() / "goalpad-jak2-sound-rpc-test";
  std::error_code fixture_error;
  std::filesystem::remove_all(fixture_root, fixture_error);
  std::filesystem::create_directories(fixture_root / "iso", fixture_error);
  std::array<u8, 96> fixture_bytes;
  for (u32 i = 0; i < fixture_bytes.size(); i++) {
    fixture_bytes[i] = (u8)(i ^ 0x5a);
  }
  std::vector<u8> chunked_str(4 * SECTOR_SIZE, 0);
  auto* chunked_header = reinterpret_cast<StrFileHeaderJ2*>(chunked_str.data());
  chunked_header->sectors[0] = 2;
  chunked_header->sizes[0] = SECTOR_SIZE;
  chunked_header->sectors[1] = 3;
  chunked_header->sizes[1] = SECTOR_SIZE;
  for (u32 i = 0; i < SECTOR_SIZE; i++) {
    chunked_str[2 * SECTOR_SIZE + i] = (u8)(i ^ 0x96);
    chunked_str[3 * SECTOR_SIZE + i] = (u8)(i ^ 0x69);
  }

  auto unaligned_chunked_str = chunked_str;
  unaligned_chunked_str.pop_back();
  std::vector<u8> nonzero_after_zero_str(4 * SECTOR_SIZE, 0);
  auto* nonzero_after_zero_header =
      reinterpret_cast<StrFileHeaderJ2*>(nonzero_after_zero_str.data());
  nonzero_after_zero_header->sectors[0] = 2;
  nonzero_after_zero_header->sizes[0] = 2 * SECTOR_SIZE;
  nonzero_after_zero_header->sectors[2] = 3;
  nonzero_after_zero_header->sizes[2] = SECTOR_SIZE;
  auto descending_chunked_str = chunked_str;
  auto* descending_header = reinterpret_cast<StrFileHeaderJ2*>(descending_chunked_str.data());
  descending_header->sectors[0] = 3;
  descending_header->sectors[1] = 2;
  auto inside_header_str = chunked_str;
  reinterpret_cast<StrFileHeaderJ2*>(inside_header_str.data())->sectors[0] = 1;
  auto mismatched_size_str = chunked_str;
  reinterpret_cast<StrFileHeaderJ2*>(mismatched_size_str.data())->sizes[0] = SECTOR_SIZE - 1;
  auto size_without_sector_str = chunked_str;
  auto* size_without_sector_header =
      reinterpret_cast<StrFileHeaderJ2*>(size_without_sector_str.data());
  size_without_sector_header->sectors[1] = 0;
  size_without_sector_header->sizes[1] = SECTOR_SIZE;
  auto sector_beyond_file_str = chunked_str;
  reinterpret_cast<StrFileHeaderJ2*>(sector_beyond_file_str.data())->sectors[1] = UINT32_MAX;

  constexpr const char* kFullWidthName = "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
  check(!fixture_error && write_fixture(fixture_root / "iso" / "MIXED.TXT", fixture_bytes) &&
            write_fixture(fixture_root / "iso" / kFullWidthName, fixture_bytes) &&
            write_fixture(fixture_root / "iso" / "TIDINTRO.STR", chunked_str) &&
            write_fixture(fixture_root / "iso" / "AA.STR", unaligned_chunked_str) &&
            write_fixture(fixture_root / "iso" / "AB.STR", nonzero_after_zero_str) &&
            write_fixture(fixture_root / "iso" / "AC.STR", descending_chunked_str) &&
            write_fixture(fixture_root / "iso" / "AD.STR", inside_header_str) &&
            write_fixture(fixture_root / "iso" / "AE.STR", mismatched_size_str) &&
            write_fixture(fixture_root / "iso" / "AF.STR", size_without_sector_str) &&
            write_fixture(fixture_root / "iso" / "AG.STR", sector_beyond_file_str) &&
            write_fixture(fixture_root / "iso" / "VALID.SBK", valid_bank) &&
            write_fixture(fixture_root / "iso" / "BUDGET.SBK", shared_reference_budget_bank) &&
            write_fixture(fixture_root / "iso" / "PLAY.SBK", playable_bank) &&
            write_fixture(fixture_root / "iso" / "TRUNC.SBK", truncated_bank) &&
            write_fixture(fixture_root / "iso" / "OVERFLOW.SBK", overflow_bank) &&
            write_fixture(fixture_root / "iso" / "NEGCOUNT.SBK", negative_count_bank) &&
            write_fixture(fixture_root / "iso" / "SNDCNT.SBK", negative_sound_grain_count_bank) &&
            write_fixture(fixture_root / "iso" / "GRAINOFF.SBK", bad_grain_offset_bank) &&
            write_fixture(fixture_root / "iso" / "BADTYPE.SBK", bad_grain_type_bank) &&
            write_fixture(fixture_root / "iso" / "NAMEHASH.SBK", bad_name_hash_bank) &&
            write_fixture(fixture_root / "iso" / "NAMETERM.SBK", bad_name_terminator_bank) &&
            write_fixture(fixture_root / "iso" / "USERDATA.SBK", bad_userdata_bank) &&
            write_fixture(fixture_root / "iso" / "V2OFFSET.SBK", bad_v2_offset_bank) &&
            write_fixture(fixture_root / "iso" / "SAMPLEOF.SBK", bad_sample_offset_bank),
        "create synthetic STR and sound-bank fixtures");
  goal_kernel_core_set_data_directory(fixture_root.string().c_str());

  std::printf("\n== exact-buffer no-reply sound-bank loads ==\n");
  reset_bank_command(send, bank_name("valid"));
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto bank_send = snapshot(send);
  const auto bank_recv = snapshot(recv);
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "a validated command 2 load uses no reply buffer");
  check(snapshot(send) == bank_send && snapshot(recv) == bank_recv,
        "successful bank loading mutates neither EE buffer");
  check(LookupBank(bank_name("valid").data()) != nullptr,
        "the minimal fixture has a real retained bank handle");
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "a repeated bank request remains synchronous");
  reset_bank_command(send, bank_name("budget"));
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "an exact-budget bank load uses no reply buffer");
  check(LookupBank(bank_name("budget").data()) != nullptr,
        "the exact-budget fixture decodes through 989snd");
  reset_bank_command(send, bank_name("play"));
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "the named tone bank load remains synchronous");
  check(LookupBank(bank_name("play").data()) != nullptr,
        "the named tone fixture decodes through 989snd");
  const std::array<const char*, 11> invalid_bank_names = {
      "trunc",    "overflow", "negcount", "sndcnt",  "grainoff", "badtype",
      "namehash", "nameterm", "userdata", "v2offset", "sampleof"};
  bool invalid_buffers_untouched = true;
  bool invalid_banks_absent = true;
  for (const char* name : invalid_bank_names) {
    reset_bank_command(send, bank_name(name));
    const auto invalid_send = snapshot(send);
    const auto invalid_recv = snapshot(recv);
    rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0);
    invalid_buffers_untouched &= snapshot(send) == invalid_send && snapshot(recv) == invalid_recv;
    invalid_banks_absent &= LookupBank(bank_name(name).data()) == nullptr;
  }
  reset_bank_command(send, bank_name("missing"));
  const auto missing_send = snapshot(send);
  const auto missing_recv = snapshot(recv);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0);
  invalid_buffers_untouched &= snapshot(send) == missing_send && snapshot(recv) == missing_recv;
  invalid_banks_absent &= LookupBank(bank_name("missing").data()) == nullptr;
  reset_bank_command(send, bank_name("../unsafe"));
  const auto unsafe_send = snapshot(send);
  const auto unsafe_recv = snapshot(recv);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0);
  invalid_buffers_untouched &= snapshot(send) == unsafe_send && snapshot(recv) == unsafe_recv;
  invalid_banks_absent &= LookupBank(bank_name("../unsafe").data()) == nullptr;
  check(invalid_buffers_untouched, "failed bank requests mutate neither EE buffer");
  check(invalid_banks_absent, "failed bank requests never enter loaded state");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.bank_requests, 17, "every well-framed bank request is counted");
  check_u32(stats.banks_loaded, 3, "only structurally valid bank fixtures are loaded");
  check_u32(stats.bank_reuses, 1, "a repeated request reuses the retained bank");
  check_u32(stats.bank_failures, 13, "every invalid, missing, or unsafe bank fails closed");
  check_guards(send, "no-reply command send canaries stay intact");
  check_guards(recv, "no-reply command receive canaries stay intact");

  auto hostile_falloff = guarded_buffer(kCommandSize, "jak2-player-hostile-falloff");
  set_player_play(hostile_falloff, 0, 0x7006, bank_name("test-tone"));
  const auto hostile_falloff_bytes = snapshot(hostile_falloff);
  rpc_call(0, 0, 1, hostile_falloff.data.offset, hostile_falloff.size, 0, 0, 0);
  check(snapshot(hostile_falloff) == hostile_falloff_bytes,
        "hostile checked userdata leaves the PLAY buffer untouched");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.play_requests, 0, "hostile checked userdata starts no PLAY request");
  check_u32(stats.player_failures, 17, "hostile checked userdata rejects the player batch");
  check_guards(hostile_falloff, "hostile checked-userdata command canaries stay intact");

  auto duplicate_play = guarded_buffer(2 * kCommandSize, "jak2-player-duplicate-play");
  set_player_play(duplicate_play, 0, 0x7007, bank_name("test-tone"));
  auto* safe_first_play = duplicate_play.data.cast<jak2::SoundRpcCommand>().c();
  safe_first_play->play.parms.mask = 0x1c0;
  safe_first_play->play.parms.fo_min = 5;
  safe_first_play->play.parms.fo_max = 30;
  safe_first_play->play.parms.fo_curve = 2;
  set_player_play(duplicate_play, 1, 0x7007, bank_name("not-there"));
  const auto duplicate_play_bytes = snapshot(duplicate_play);
  rpc_call(0, 0, 1, duplicate_play.data.offset, duplicate_play.size, 0, 0, 0);
  check(snapshot(duplicate_play) == duplicate_play_bytes,
        "duplicate-ID PLAY rejection leaves the complete batch untouched");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.play_requests, 0, "duplicate-ID rejection starts no earlier PLAY request");
  check_u32(stats.player_failures, 18, "duplicate PLAY IDs reject the complete batch");
  player_rejected_calls = stats.rejected_calls;
  check_guards(duplicate_play, "duplicate-ID PLAY command canaries stay intact");

  std::printf("\n== named SFX PLAY, update and semantic miss ==\n");
  constexpr u32 kPlaybackCommands = 3;
  auto playback = guarded_buffer(kPlaybackCommands * kCommandSize, "jak2-player-playback");
  set_player_master_volume(playback, 0, 0x01, 900);
  set_player_play(playback, 1, 0x7002, bank_name("test-tone"));
  auto* playback_command = (playback.data + kCommandSize).cast<jak2::SoundRpcCommand>().c();
  playback_command->play.parms.mask = 0x100;
  playback_command->play.parms.fo_curve = 2;
  set_player_ear(playback, 2, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, 0);
  const auto playback_bytes = snapshot(playback);
  check_u32((u32)rpc_call(0, 0, 1, playback.data.offset, playback.size, 0, 0, 0), 0,
            "mixed state, PLAY and ear commands complete synchronously");
  check(snapshot(playback) == playback_bytes, "mixed playback batch remains read-only");
  check_guards(playback, "mixed playback batch canaries stay intact");
  state = player_state();
  check_s32(state.master_volumes[0], 900, "state before PLAY applies in wire order");
  check_u32(state.ear_transform_seen, 1, "ear state after PLAY applies in wire order");

  auto same_id = guarded_buffer(kCommandSize, "jak2-player-same-id");
  set_player_play(same_id, 0, 0x7002, bank_name("not-there"), 800);
  auto* same_id_command = same_id.data.cast<jak2::SoundRpcCommand>().c();
  same_id_command->play.parms.mask = 0x100;
  same_id_command->play.parms.fo_curve = 2;
  const auto same_id_bytes = snapshot(same_id);
  rpc_call(0, 0, 1, same_id.data.offset, same_id.size, 0, 0, 0);
  check(snapshot(same_id) == same_id_bytes, "same-ID update remains read-only");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.play_requests, 2, "new and same-ID PLAY commands are counted");
  check_u32(stats.sounds_started, 1, "only the first PLAY starts a 989snd handler");
  check_u32(stats.sound_updates, 1, "same-ID PLAY updates the existing handler");
  check_u32(stats.sounds_missing, 0, "same-ID update ignores its replacement name");

  std::array<s16, 1024> audio{};
  check_s32(goal_game_sound_pull_audio(audio.data(), audio.size() / 2), audio.size() / 2,
            "the game-neutral seam renders requested synthetic stereo frames");
  check(std::any_of(audio.begin(), audio.end(), [](s16 sample) { return sample != 0; }),
        "PLAY produces nonzero samples through the game-neutral audio seam");

  auto missing_play = guarded_buffer(kCommandSize, "jak2-player-missing");
  set_player_play(missing_play, 0, 0x7003, bank_name("not-there"));
  rpc_call(0, 0, 1, missing_play.data.offset, missing_play.size, 0, 0, 0);
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.play_requests, 3, "a semantic PLAY miss is counted as a request");
  check_u32(stats.sounds_started, 1, "a semantic miss starts no handler");
  check_u32(stats.sound_updates, 1, "a semantic miss updates no handler");
  check_u32(stats.sounds_missing, 1, "a missing loaded-bank name is surfaced in stats");
  check_guards(same_id, "same-ID player command canaries stay intact");
  check_guards(missing_play, "semantic-miss player command canaries stay intact");

  std::printf("\n== synchronous ordinary-file STR loads ==\n");
  auto str_send = guarded_buffer(kStrRequestSize, "jak2-str-send");
  auto str_recv = guarded_buffer(kStrReplySize, "jak2-str-recv");
  auto str_destination = guarded_buffer(128, "jak2-str-destination");
  if (!str_send.data.offset || !str_recv.data.offset || !str_destination.data.offset) {
    goal_kernel_core_shutdown();
    return 1;
  }

  reset_str_request(str_send, str_destination.data.offset, -1, 17, "mixed.txt");
  memset(str_recv.data.c(), 0xcc, str_recv.size);
  memset(str_destination.data.c(), 0xdd, str_destination.size);
  const auto str_original = snapshot(str_send);
  check_u32((u32)rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset,
                         kStrReplySize, 0),
            0, "ordinary-file rpc-call returns synchronously");
  auto* str_reply = str_recv.data.cast<StrReply>().c();
  check(snapshot(str_send) == str_original, "STR send buffer remains untouched");
  check_u32(str_reply->result, 0, "STR success result is done");
  check_u32(str_reply->maxlen, 17, "STR reply reports the bounded byte count");
  check_u32(str_reply->address, str_destination.data.offset, "STR reply preserves the address");
  check_u32((u32)str_reply->section, (u32)-1, "STR reply preserves the section");
  check(memcmp(str_destination.data.c(), fixture_bytes.data(), 17) == 0,
        "STR copies the requested prefix into EE memory");
  bool destination_tail_intact = true;
  for (u32 i = 17; i < str_destination.size; i++) {
    destination_tail_intact &= str_destination.data.c()[i] == 0xdd;
  }
  check(destination_tail_intact, "STR does not write past maxlen");

  reset_str_request(str_send, str_destination.data.offset, -1, str_destination.size,
                    kFullWidthName);
  memset(str_destination.data.c(), 0xdd, str_destination.size);
  check_u32((u32)rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset,
                         kStrReplySize, 0),
            0, "a transmitted 32-byte basename is accepted");
  check_u32(str_reply->result, 0, "full-width basename load succeeds");
  check_u32(str_reply->maxlen, fixture_bytes.size(), "full file length is returned");
  check(memcmp(str_destination.data.c(), fixture_bytes.data(), fixture_bytes.size()) == 0,
        "only the transmitted basename is needed to find the file");

  std::printf("\n== handled STR failures return an error reply ==\n");
  const std::array<std::string, 5> failed_names = {
      "missing.txt", "", "../MIXED.TXT", "sub/MIXED.TXT", "sub\\MIXED.TXT"};
  for (const auto& name : failed_names) {
    reset_str_request(str_send, str_destination.data.offset, -1, str_destination.size, name);
    memset(str_recv.data.c(), 0xcc, str_recv.size);
    memset(str_destination.data.c(), 0xdd, str_destination.size);
    rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
    check_u32(str_recv.data.cast<StrReply>().c()->result, 1,
              "missing or path-like basename returns an error");
    check_u32(str_recv.data.cast<StrReply>().c()->maxlen, 0,
              "failed STR load returns zero length");
    bool destination_unchanged = true;
    for (u32 i = 0; i < str_destination.size; i++) {
      destination_unchanged &= str_destination.data.c()[i] == 0xdd;
    }
    check(destination_unchanged, "failed STR load leaves the destination untouched");
  }

  reset_str_request(str_send, EE_MAIN_MEM_SIZE - 8, -1, 16, "mixed.txt");
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
  check_u32(str_recv.data.cast<StrReply>().c()->result, 1,
            "an end-crossing destination returns an error");
  check_u32(str_recv.data.cast<StrReply>().c()->maxlen, 0,
            "an invalid destination returns zero length");
  reset_str_request(str_send, str_destination.data.offset, -1, 0, "mixed.txt");
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
  check_u32(str_recv.data.cast<StrReply>().c()->result, 1, "a zero maxlen returns an error");
  check_u32((u32)rpc_busy(4), 0, "the synchronous STR channel is never busy");

  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.str_requests, 9, "all ordinary-file STR requests were counted");
  check_u32(stats.str_reads, 2, "two synthetic STR files were read");
  check_u32(stats.str_failures, 7, "handled STR failures were counted");
  check_u32(stats.str_bytes, 17 + fixture_bytes.size(), "STR byte count is exact");
  check_guards(str_send, "STR send-buffer canaries stay intact");
  check_guards(str_recv, "STR receive-buffer canaries stay intact");
  check_guards(str_destination, "STR destination canaries stay intact");

  std::printf("\n== mapped chunked STR loads and fail-closed table validation ==\n");
  auto chunk_destination = guarded_buffer(SECTOR_SIZE, "jak2-chunked-str-destination");
  if (!chunk_destination.data.offset) {
    goal_kernel_core_shutdown();
    return 1;
  }
  for (s32 section = 0; section < 2; section++) {
    reset_str_request(str_send, chunk_destination.data.offset, section, chunk_destination.size,
                      "title-disk-intro");
    memset(str_recv.data.c(), 0xcc, str_recv.size);
    memset(chunk_destination.data.c(), 0xdd, chunk_destination.size);
    const auto chunk_request = snapshot(str_send);
    check_u32((u32)rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize,
                           str_recv.data.offset, kStrReplySize, 0),
              0, "chunked STR rpc-call returns synchronously");
    str_reply = str_recv.data.cast<StrReply>().c();
    check(snapshot(str_send) == chunk_request, "chunked STR request remains untouched");
    check_u32(str_reply->result, 0, "chunked STR success result is done");
    check_u32(str_reply->maxlen, SECTOR_SIZE, "chunked STR reports its sector span");
    check_u32(str_reply->address, chunk_destination.data.offset,
              "chunked STR reply preserves the address");
    check_s32(str_reply->section, section, "chunked STR reply preserves the section");
    check(memcmp(chunk_destination.data.c(),
                 chunked_str.data() + (section + 2) * SECTOR_SIZE, SECTOR_SIZE) == 0,
          "mapped animation chunk reaches EE memory exactly");
  }

  struct ChunkFailure {
    const char* animation;
    s32 section;
    u32 address;
    u32 maxlen;
  };
  const std::array<ChunkFailure, 12> chunk_failures = {{
      {"aa", 0, chunk_destination.data.offset, chunk_destination.size},
      {"ab", 0, chunk_destination.data.offset, chunk_destination.size},
      {"ac", 0, chunk_destination.data.offset, chunk_destination.size},
      {"ad", 0, chunk_destination.data.offset, chunk_destination.size},
      {"ae", 0, chunk_destination.data.offset, chunk_destination.size},
      {"af", 0, chunk_destination.data.offset, chunk_destination.size},
      {"ag", 0, chunk_destination.data.offset, chunk_destination.size},
      {"zz", 0, chunk_destination.data.offset, chunk_destination.size},
      {"title-disk-intro", 2, chunk_destination.data.offset, chunk_destination.size},
      {"title-disk-intro", SECTOR_TABLE_SIZE_J2, chunk_destination.data.offset,
       chunk_destination.size},
      {"title-disk-intro", 0, chunk_destination.data.offset, SECTOR_SIZE - 1},
      {"title-disk-intro", 0, EE_MAIN_MEM_SIZE - 8, 16},
  }};
  bool chunk_failures_transactional = true;
  for (const auto& failure : chunk_failures) {
    reset_str_request(str_send, failure.address, failure.section, failure.maxlen,
                      failure.animation);
    memset(str_recv.data.c(), 0xcc, str_recv.size);
    memset(chunk_destination.data.c(), 0xdd, chunk_destination.size);
    const auto failed_request = snapshot(str_send);
    rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset,
             kStrReplySize, 0);
    const auto* failed_reply = str_recv.data.cast<StrReply>().c();
    chunk_failures_transactional &= snapshot(str_send) == failed_request;
    chunk_failures_transactional &= failed_reply->result == 1 && failed_reply->maxlen == 0;
    chunk_failures_transactional &=
        std::all_of(chunk_destination.data.c(),
                    chunk_destination.data.c() + chunk_destination.size,
                    [](u8 byte) { return byte == 0xdd; });
  }
  check(chunk_failures_transactional,
        "malformed tables and invalid chunk requests return error without mutation");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.str_requests, 23, "ordinary and chunked STR requests are counted");
  check_u32(stats.str_reads, 4, "two ordinary files and two mapped chunks were read");
  check_u32(stats.str_failures, 19, "all semantic STR failures were counted");
  check_u32(stats.str_bytes, 17 + fixture_bytes.size() + 2 * SECTOR_SIZE,
            "ordinary and chunked STR byte counts are exact");
  check_guards(chunk_destination, "chunked STR destination canaries stay intact");

  std::printf("\n== unsupported and malformed requests remain unimplemented ==\n");
  reset_command(send, jak2::Jak2SoundCommand::load_bank, 0x3456789a);
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto unsupported_send = snapshot(send);
  const auto unsupported_recv = snapshot(recv);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  check(snapshot(send) == unsupported_send && snapshot(recv) == unsupported_recv,
        "a no-reply command with a reply buffer mutates neither buffer");
  reset_language_command(send, 0);
  const auto malformed_language_send = snapshot(send);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, 0, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, kCommandSize, 0);
  check(snapshot(send) == malformed_language_send && snapshot(recv) == unsupported_recv,
        "set-language rejects any receive pointer or receive size without mutating buffers");
  reset_command(send, jak2::Jak2SoundCommand::get_irx_version, 0x3456789a);
  const auto malformed_send = snapshot(send);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0);
  rpc_call(1, 0, 1, send.command.offset + 1, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset + 1, kCommandSize, 0);
  rpc_call(0, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(5, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize - 1, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize - 1, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, kCommandSize, 0);
  rpc_call(1, 0, 1, 0, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, EE_MAIN_MEM_SIZE + 16, kCommandSize, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, EE_MAIN_MEM_SIZE - kCommandSize + 1, kCommandSize, recv.command.offset,
           kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize,
           EE_MAIN_MEM_SIZE - kCommandSize + 1, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, (u64)-1, recv.command.offset, kCommandSize, 0);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, (u64)-1, 0);
  check(snapshot(send) == malformed_send && snapshot(recv) == unsupported_recv,
        "rejected channels and malformed buffers do not mutate memory");

  reset_str_request(str_send, str_destination.data.offset, 0, str_destination.size, "mixed.txt");
  memset(str_recv.data.c(), 0xcc, str_recv.size);
  const auto unsupported_str_recv = snapshot(str_recv);
  rpc_call(4, 1, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize - 1, str_recv.data.offset,
           kStrReplySize, 0);
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset,
           kStrReplySize - 1, 0);
  rpc_call(4, 0, 1, 0, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, 0, kStrReplySize, 0);
  rpc_call(4, 0, 1, EE_MAIN_MEM_SIZE - kStrRequestSize + 1, kStrRequestSize,
           str_recv.data.offset, kStrReplySize, 0);
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize,
           EE_MAIN_MEM_SIZE - kStrReplySize + 1, kStrReplySize, 0);
  check(snapshot(str_recv) == unsupported_str_recv,
        "malformed STR framing does not mutate the reply");
  check_u32((u32)rpc_busy(3), 0, "an unsupported busy query still returns not-busy");
  check_guards(send, "rejected-call send canaries stay intact");
  check_guards(recv, "rejected-call receive canaries stay intact");

  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.version_requests, 2, "rejected calls do not count as handshakes");
  check_u32(stats.bank_requests, 17, "malformed bank framing is not counted as a request");
  check_u32(stats.banks_loaded, 3, "malformed calls do not claim another bank load");
  check_u32(stats.bank_failures, 13, "framing rejection is distinct from a bank failure");
  check_u32(stats.str_requests, 23, "rejected STR calls do not count as file requests");
  check_u32(stats.language_requests, 9, "malformed framing is not a language request");
  check_u32(stats.language_failures, 1, "framing rejection is distinct from language failure");
  check_u32(stats.language_id, 7, "rejected calls preserve the current language");
  check_u32(stats.rejected_calls, player_rejected_calls + 25,
            "every unsupported request is reported");

  std::printf("\n== shutdown and reinitialization ownership ==\n");
  gSoundEnable = 0;
  goal_kernel_core_shutdown();
  check(!goal_jak2_sound_rpc_is_installed(), "kernel shutdown stops its owned 989snd instance");
  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK,
        "the kernel reinitializes after sound teardown");
  check(goal_kernel_core_stub_machine_layer(0) == GOAL_KERNEL_CORE_OK,
        "machine stubs reinstall after shutdown");
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_OK,
        "the Jak 2 sound owner reinstalls after shutdown");
  check(gLanguage && strcmp(gLanguage, "ENG") == 0,
        "reinstall restores the English language default");
  check(gSoundEnable == 1, "reinstall restores the common sound-enabled default");
  const auto reinstalled_player_state = player_state();
  bool reset_master_volumes = true;
  for (s32 volume : reinstalled_player_state.master_volumes) {
    reset_master_volumes &= volume == 0x400;
  }
  check(reset_master_volumes, "reinstall resets every retained master volume");
  check_u32(reinstalled_player_state.midi_register_mask, 0,
            "reinstall clears retained MIDI state");
  check_u32(snd::GlobalExcite, 0, "reinstall resets actual 989snd global excitement");
  check_u32(reinstalled_player_state.reverb_seen, 0, "reinstall clears retained reverb state");
  check_u32(reinstalled_player_state.fps, 60, "reinstall restores the 60 FPS default");
  check_u32(reinstalled_player_state.ear_transform_seen, 0,
            "reinstall clears retained listener state");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.player_batches, 0, "reinstall resets player batch statistics");
  check_u32(stats.player_commands, 0, "reinstall resets player command statistics");
  check_u32(stats.player_failures, 0, "reinstall resets player failure statistics");
  auto reinit_send = guarded_command("jak2-sound-rpc-reinit-send");
  auto reinit_player = guarded_buffer(kCommandSize, "jak2-sound-rpc-reinit-player");
  rpc_call = native_entry<GoalEightArgumentFunction>("rpc-call");
  if (reinit_player.data.offset && rpc_call) {
    set_player_play(reinit_player, 0, 0x7002, bank_name("test-tone"));
    rpc_call(0, 0, 1, reinit_player.data.offset, kCommandSize, 0, 0, 0);
    goal_jak2_sound_rpc_stats_get(&stats);
    check_u32(stats.play_requests, 1, "PLAY remains callable after full reinitialization");
    check_u32(stats.sounds_started, 0, "reinstall retains no prior loaded-bank voice");
    check_u32(stats.sound_updates, 0, "reinstall retains no prior same-ID mapping");
    check_u32(stats.sounds_missing, 1, "reinstall reports the now-unloaded sound name");
  }
  if (reinit_send.command.offset && rpc_call) {
    reset_bank_command(reinit_send, bank_name("valid"));
    rpc_call(1, 0, 1, reinit_send.command.offset, kCommandSize, 0, 0, 0);
    goal_jak2_sound_rpc_stats_get(&stats);
    check_u32(stats.banks_loaded, 1, "a bank loads after full shutdown and reinitialization");
  }
  goal_jak2_sound_rpc_shutdown();
  check(!goal_jak2_sound_rpc_is_installed(), "explicit sound shutdown releases ownership");
  stopped_audio.fill(0x2345);
  check_s32(goal_game_sound_pull_audio(stopped_audio.data(), stopped_audio.size() / 2), 0,
            "the game-neutral audio seam stops pulling after sound shutdown");
  check(std::all_of(stopped_audio.begin(), stopped_audio.end(),
                    [](s16 sample) { return sample == 0x2345; }),
        "a stopped audio pull leaves the host buffer untouched");
  if (reinit_send.command.offset && rpc_call) {
    reset_bank_command(reinit_send, bank_name("valid"));
    rpc_call(1, 0, 1, reinit_send.command.offset, kCommandSize, 0, 0, 0);
    goal_jak2_sound_rpc_stats_get(&stats);
    check_u32(stats.bank_failures, 1, "a stale loader call fails safely after sound shutdown");
    check(LookupBank(bank_name("valid").data()) == nullptr,
          "shutdown clears the retained bank state before rejecting a stale call");
  }
  if (reinit_send.command.offset && rpc_call) {
    gLanguage = "UKE";
    reset_language_command(reinit_send, 0);
    rpc_call(1, 0, 1, reinit_send.command.offset, kCommandSize, 0, 0, 0);
    goal_jak2_sound_rpc_stats_get(&stats);
    check_u32(stats.language_failures, 1,
              "a stale language call fails safely after sound shutdown");
    check(gLanguage && strcmp(gLanguage, "UKE") == 0,
          "a stale language call cannot mutate stopped sound state");
  }
  if (reinit_player.data.offset && rpc_call) {
    set_player_fps(reinit_player, 0, 50);
    const auto stopped_player_state = player_state();
    const auto stopped_player_bytes = snapshot(reinit_player);
    rpc_call(0, 0, 1, reinit_player.data.offset, kCommandSize, 0, 0, 0);
    goal_jak2_sound_rpc_stats_get(&stats);
    check_u32(stats.player_failures, 1,
              "a stale player call fails safely after sound shutdown");
    check(same_player_state(player_state(), stopped_player_state),
          "a stale player call cannot mutate stopped sound state");
    check(snapshot(reinit_player) == stopped_player_bytes,
          "a stale player call leaves its EE buffer untouched");
    check_guards(reinit_player, "stale player call preserves canaries");
  }
  goal_jak2_sound_rpc_shutdown();
  check(!goal_jak2_sound_rpc_is_installed(), "sound shutdown is idempotent");

  goal_kernel_core_set_data_directory(nullptr);
  std::filesystem::remove_all(fixture_root, fixture_error);
  goal_kernel_core_shutdown();
  std::printf("\n%s: Jak 2 startup state, named SFX, checked bank, lifecycle and STR seams\n",
              g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
