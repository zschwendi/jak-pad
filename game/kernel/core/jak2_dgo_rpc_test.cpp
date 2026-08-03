/*!
 * @file jak2_dgo_rpc_test.cpp
 * Synthetic coverage for Jak 2's composed sound/DGO RPC router and incremental link boundary.
 */

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/goal_constants.h"
#include "common/link_types.h"

#include "game/common/dgo_rpc_types.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"

namespace {

constexpr u32 kGuardSize = 16;
constexpr u32 kDgoCommandSize = 32;
constexpr u32 kSoundCommandSize = sizeof(jak2::SoundRpcCommand);
int g_failures = 0;
int g_top_levels = 0;

void check(bool condition, const char* what) {
  std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
  if (!condition) {
    g_failures++;
  }
}

void check_u32(u32 got, u32 expected, const char* what) {
  if (got == expected) {
    std::printf("  ok   %-48s #x%08x\n", what, got);
  } else {
    std::printf("  FAIL %-48s got #x%08x, expected #x%08x\n", what, got, expected);
    g_failures++;
  }
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
    check(false, "allocate guarded EE buffer");
    return {};
  }
  memset(allocation.c(), 0xa5, size + 2 * kGuardSize);
  return {allocation, allocation + kGuardSize, size};
}

void check_guards(GuardedBuffer buffer, const char* what) {
  bool intact = true;
  for (u32 i = 0; i < kGuardSize; i++) {
    intact &= buffer.allocation.c()[i] == 0xa5;
    intact &= buffer.allocation.c()[kGuardSize + buffer.size + i] == 0xa5;
  }
  check(intact, what);
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

struct DgoCommand {
  u16 rsvd;
  u16 result;
  u32 buffer1;
  u32 buffer2;
  u32 heap_top;
  char name[16];
};
static_assert(sizeof(DgoCommand) == kDgoCommandSize);

struct StrRequest {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
  char basename[32];
};
static_assert(sizeof(StrRequest) == 64);

struct StrReply {
  u16 rsvd;
  u16 result;
  u32 address;
  s32 section;
  u32 maxlen;
  u32 dummy[4];
};
static_assert(sizeof(StrReply) == 32);

void append_u32(std::vector<u8>* out, u32 value) {
  for (int i = 0; i < 4; i++) {
    out->push_back((u8)(value >> (i * 8)));
  }
}

void append_name(std::vector<u8>* out, const std::string& name) {
  for (int i = 0; i < 60; i++) {
    out->push_back(i < (int)name.size() ? (u8)name[i] : 0);
  }
}

std::vector<u8> synthetic_dgo() {
  std::vector<u8> out;
  append_u32(&out, 3);
  append_name(&out, "SYNTH.DGO");
  for (const char* name : {"FIRST", "SECOND", "LAST"}) {
    std::vector<u8> body;
    append_u32(&body, 0x4c414f47);
    append_u32(&body, 0);
    append_u32(&body, 3);
    body.resize(48, 0xcd);
    append_u32(&out, (u32)body.size());
    append_name(&out, name);
    out.insert(out.end(), body.begin(), body.end());
    while (out.size() & 0xf) {
      out.push_back(0);
    }
  }
  return out;
}

bool write_file(const std::filesystem::path& path, const std::vector<u8>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
  return out.good();
}

void reset_dgo_command(GuardedBuffer* buffer,
                       u32 b1,
                       u32 b2,
                       u32 heap_top,
                       const char* name = nullptr) {
  memset(buffer->data.c(), 0, buffer->size);
  auto* command = buffer->data.cast<DgoCommand>().c();
  command->rsvd = 0x5aa5;
  command->result = DGO_RPC_RESULT_INIT;
  command->buffer1 = b1;
  command->buffer2 = b2;
  command->heap_top = heap_top;
  if (name) {
    strncpy(command->name, name, sizeof(command->name));
  }
}

u64 synthetic_top_level() {
  g_top_levels++;
  return 0;
}

int test_link_boundary(GoalEightArgumentFunction link_begin,
                       GoalOneArgumentFunction link_resume) {
  const void* const functions[] = {(const void*)&synthetic_top_level};
  const goal_aot_object_file aot = {"rpc-code", nullptr, 0, functions, 1, nullptr};
  check(goal_aot_register_object("rpc-code", &aot) == GOAL_KERNEL_CORE_OK,
        "register a synthetic AOT code object");

  auto name = kmalloc(kglobalheap, 32, KMALLOC_MEMSET | KMALLOC_ALIGN_16, "dgo-link-name");
  auto code = kmalloc(kglobalheap, 48, KMALLOC_MEMSET | KMALLOC_ALIGN_16, "dgo-link-code");
  if (!name.offset || !code.offset) {
    return 1;
  }
  strcpy(name.cast<char>().c(), "rpc-code");
  auto* code_header = code.cast<u32>().c();
  code_header[0] = 0x4c414f47;
  code_header[2] = 3;
  check_u32((u32)link_begin(code.offset, name.offset, 48, kglobalheap.offset, LINK_FLAG_EXECUTE, 0,
                            0, 0),
            1, "AOT code link finishes in one call");
  check_u32((u32)g_top_levels, 1, "AOT code top-level executes once");

  constexpr u32 kDataBytes = 0x80010;
  constexpr u32 kLinkBytes = 16;
  const u32 saved_top = kglobalheap->top.offset;
  auto data = kmalloc(kglobalheap, kLinkBytes + kDataBytes,
                      KMALLOC_TOP | KMALLOC_MEMSET | KMALLOC_ALIGN_16, "dgo-link-data");
  if (!data.offset) {
    check(false, "allocate incremental synthetic data object");
    return 1;
  }
  auto* data_header = data.cast<LinkHeaderV2>().c();
  data_header->type_tag = 0xffffffff;
  data_header->length = kLinkBytes;
  data_header->version = 2;
  data.c()[12] = 0;
  data.c()[13] = 0;
  strcpy(name.cast<char>().c(), "rpc-data");

  check_u32((u32)link_begin(data.offset, name.offset, kLinkBytes + kDataBytes,
                            kglobalheap.offset, 0, 0, 0, 0),
            0, "large DGO data link begins incrementally");
  check_u32((u32)link_resume(0), 0, "first data-link resume retains work");
  check_u32((u32)link_resume(0), 1, "second data-link resume completes");
  kglobalheap->top.offset = saved_top;

  goal_dgo_rpc_stats stats = {};
  goal_dgo_goal_loader_stats(&stats);
  check_u32((u32)stats.linked_code_objects, 1, "one code object took the AOT path");
  check_u32((u32)stats.linked_data_objects, 1, "one data object took the real linker");
  return 0;
}

}  // namespace

int main() {
  const auto temp = std::filesystem::temp_directory_path() / "goalpad-jak2-dgo-rpc-test";
  const auto iso = temp / "iso";
  std::filesystem::remove_all(temp);
  std::filesystem::create_directories(iso);
  check(write_file(iso / "SYNTH.DGO", synthetic_dgo()), "write synthetic three-object DGO");
  check(write_file(iso / "DELEGATE.TXT", {'d', 'e', 'l', 'e', 'g', 'a', 't', 'e'}),
        "write synthetic STR delegation fixture");

  check(goal_kernel_core_initialize() == GOAL_KERNEL_CORE_OK, "initialize the Jak 2 kernel core");
  goal_kernel_core_set_data_directory(temp.string().c_str());
  check(goal_kernel_core_stub_machine_layer(0) == GOAL_KERNEL_CORE_OK,
        "install reporting machine stubs");
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_OK,
        "install the sound owner before the DGO router");

  u32 sound_call_object = 0;
  native_entry<GoalEightArgumentFunction>("rpc-call", &sound_call_object);
  goal_dgo_install_goal_loader();
  u32 router_call_object = 0;
  auto rpc_call = native_entry<GoalEightArgumentFunction>("rpc-call", &router_call_object);
  auto rpc_busy = native_entry<GoalOneArgumentFunction>("rpc-busy?");
  auto link_begin = native_entry<GoalEightArgumentFunction>("link-begin");
  auto link_resume = native_entry<GoalOneArgumentFunction>("link-resume");
  check(router_call_object != sound_call_object, "DGO installation replaces sound's top router");
  if (!rpc_call || !rpc_busy || !link_begin || !link_resume) {
    goal_kernel_core_shutdown();
    return 1;
  }

  std::printf("\n== preserved sound-channel delegation ==\n");
  auto sound_send = guarded_buffer(kSoundCommandSize, "dgo-sound-send");
  auto sound_recv = guarded_buffer(kSoundCommandSize, "dgo-sound-recv");
  auto* sound = sound_send.data.cast<jak2::SoundRpcCommand>().c();
  memset(sound, 0, sizeof(*sound));
  sound->j2command = jak2::Jak2SoundCommand::get_irx_version;
  sound->irx_version.ee_addr = 0x12345678;
  check_u32((u32)rpc_call(1, 0, 1, sound_send.data.offset, kSoundCommandSize,
                          sound_recv.data.offset, kSoundCommandSize, 0),
            0, "channel 1 remains synchronous through the router");
  auto* version = sound_recv.data.cast<jak2::SoundRpcCommand>().c();
  check_u32(version->irx_version.major, 4, "channel 1 keeps the IRX major reply");
  check_u32(version->irx_version.minor, 0, "channel 1 keeps the IRX minor reply");

  memset(sound, 0, sizeof(*sound));
  sound->j2command = jak2::Jak2SoundCommand::set_master_volume;
  sound->master_volume.group.group = 1;
  sound->master_volume.volume = 321;
  rpc_call(0, 0, 1, sound_send.data.offset, kSoundCommandSize, 0, 0, 0);
  goal_jak2_sound_player_state player = {};
  goal_jak2_sound_player_state_get(&player);
  check_u32((u32)player.master_volumes[0], 321,
            "channel 0 still reaches the sound player responder");

  constexpr u32 kRoutedPlayerCommands = 3;
  auto hostile_play_batch =
      guarded_buffer(kRoutedPlayerCommands * kSoundCommandSize, "dgo-hostile-play-batch");
  memset(hostile_play_batch.data.c(), 0, hostile_play_batch.size);
  auto* routed_commands = hostile_play_batch.data.cast<jak2::SoundRpcCommand>().c();
  routed_commands[0].rsvd1 = 0x5aa5;
  routed_commands[0].j2command = jak2::Jak2SoundCommand::set_master_volume;
  routed_commands[0].master_volume.group.group = 1;
  routed_commands[0].master_volume.volume = 999;
  routed_commands[1].rsvd1 = 0x5aa5;
  routed_commands[1].j2command = jak2::Jak2SoundCommand::play;
  routed_commands[1].play.sound_id = 0x7001;
  memcpy(routed_commands[1].play.name, "HOSTILE", 7);
  routed_commands[1].play.parms.mask = 0x100;
  routed_commands[1].play.parms.fo_curve = -1;
  routed_commands[2].rsvd1 = 0x5aa5;
  routed_commands[2].j2command = jak2::Jak2SoundCommand::set_fps;
  routed_commands[2].fps.fps = 50;

  std::array<u8, kRoutedPlayerCommands * kSoundCommandSize> hostile_play_bytes;
  memcpy(hostile_play_bytes.data(), hostile_play_batch.data.c(), hostile_play_bytes.size());
  goal_jak2_sound_player_state player_before_rejection = {};
  goal_jak2_sound_player_state_get(&player_before_rejection);
  goal_jak2_sound_rpc_stats stats_before_rejection = {};
  goal_jak2_sound_rpc_stats_get(&stats_before_rejection);

  check_u32((u32)rpc_call(0, 0, 1, hostile_play_batch.data.offset, hostile_play_batch.size, 0, 0,
                          0),
            0, "routed hostile PLAY batch rejects synchronously");
  goal_jak2_sound_player_state player_after_rejection = {};
  goal_jak2_sound_player_state_get(&player_after_rejection);
  check(memcmp(&player_after_rejection, &player_before_rejection,
               sizeof(player_before_rejection)) == 0,
        "routed hostile PLAY rejects the full batch before state mutation");
  check(memcmp(hostile_play_bytes.data(), hostile_play_batch.data.c(),
               hostile_play_bytes.size()) == 0,
        "routed hostile PLAY leaves the complete EE batch untouched");

  goal_jak2_sound_rpc_stats expected_rejection_stats = stats_before_rejection;
  expected_rejection_stats.player_failures++;
  expected_rejection_stats.rejected_calls++;
  goal_jak2_sound_rpc_stats stats_after_rejection = {};
  goal_jak2_sound_rpc_stats_get(&stats_after_rejection);
  check(memcmp(&stats_after_rejection, &expected_rejection_stats,
               sizeof(expected_rejection_stats)) == 0,
        "routed hostile PLAY preserves all stats except rejection counters");
  check_guards(hostile_play_batch, "routed hostile PLAY batch canaries stay intact");

  auto str_send = guarded_buffer(sizeof(StrRequest), "dgo-str-send");
  auto str_recv = guarded_buffer(sizeof(StrReply), "dgo-str-recv");
  auto str_data = guarded_buffer(16, "dgo-str-data");
  auto* str = str_send.data.cast<StrRequest>().c();
  memset(str, 0, sizeof(*str));
  str->result = DGO_RPC_RESULT_INIT;
  str->address = str_data.data.offset;
  str->section = -1;
  str->maxlen = str_data.size;
  strcpy(str->basename, "delegate.txt");
  rpc_call(4, 0, 1, str_send.data.offset, sizeof(StrRequest), str_recv.data.offset,
           sizeof(StrReply), 0);
  check_u32(str_recv.data.cast<StrReply>().c()->result, DGO_RPC_RESULT_DONE,
            "channel 4 keeps its ordinary-file reply");
  check_u32(str_recv.data.cast<StrReply>().c()->maxlen, 8,
            "channel 4 keeps the exact byte count");
  check(memcmp(str_data.data.c(), "delegate", 8) == 0,
        "channel 4 still copies through the sound responder");
  for (u32 channel : {0, 1, 3, 4}) {
    check_u32((u32)rpc_busy(channel), 0, "each synchronous implemented channel is not busy");
  }

  std::printf("\n== exact 32-byte DGO channel ==\n");
  auto send = guarded_buffer(kDgoCommandSize, "dgo-rpc-send");
  auto recv = guarded_buffer(kDgoCommandSize, "dgo-rpc-recv");
  std::array<GuardedBuffer, 6> object_buffers = {
      guarded_buffer(128, "dgo-b1-a"), guarded_buffer(128, "dgo-b2-a"),
      guarded_buffer(128, "dgo-b1-b"), guarded_buffer(128, "dgo-b2-b"),
      guarded_buffer(128, "dgo-b1-c"), guarded_buffer(128, "dgo-b2-c")};
  auto heap_a = guarded_buffer(128, "dgo-heap-a");
  auto heap_b = guarded_buffer(128, "dgo-heap-b");
  auto heap_c = guarded_buffer(128, "dgo-heap-c");

  reset_dgo_command(&send, object_buffers[0].data.offset, object_buffers[1].data.offset,
                    heap_a.data.offset, "synth.dgo");
  const std::array<u8, kDgoCommandSize> original = [&]() {
    std::array<u8, kDgoCommandSize> copy;
    memcpy(copy.data(), send.data.c(), copy.size());
    return copy;
  }();
  rpc_call(3, DGO_RPC_LOAD_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  auto* reply = recv.data.cast<DgoCommand>().c();
  check_u32(reply->result, DGO_RPC_RESULT_MORE, "function 0 reports MORE");
  check_u32(reply->buffer1, object_buffers[0].data.offset, "first object lands in initial b1");
  check(memcmp(original.data(), send.data.c(), original.size()) == 0,
        "a separate 32-byte send buffer remains untouched");

  reset_dgo_command(&send, object_buffers[2].data.offset, object_buffers[3].data.offset,
                    heap_b.data.offset);
  rpc_call(3, DGO_RPC_LOAD_NEXT_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  check_u32(reply->result, DGO_RPC_RESULT_MORE, "function 1 reports MORE");
  check_u32(reply->buffer1, object_buffers[3].data.offset,
            "Jak 2 NEXT refreshes both buffers before selecting b2");

  reset_dgo_command(&send, object_buffers[4].data.offset, object_buffers[5].data.offset,
                    heap_c.data.offset);
  rpc_call(3, DGO_RPC_LOAD_NEXT_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  check_u32(reply->result, DGO_RPC_RESULT_DONE, "the last object reports DONE");
  check_u32(reply->buffer1, heap_c.data.offset, "the last object lands at refreshed heap top");

  rpc_call(3, DGO_RPC_LOAD_NEXT_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  check_u32(reply->result, DGO_RPC_RESULT_ERROR, "NEXT after DONE reports ERROR");

  reset_dgo_command(&send, object_buffers[0].data.offset, object_buffers[1].data.offset,
                    heap_a.data.offset, "synth.dgo");
  rpc_call(3, DGO_RPC_LOAD_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  rpc_call(3, DGO_RPC_CANCEL_FNO, 1, send.data.offset, kDgoCommandSize, recv.data.offset,
           kDgoCommandSize, 0);
  check_u32(reply->result, DGO_RPC_RESULT_ABORTED, "function 2 reports ABORTED");

  memset(recv.data.c(), 0xcc, recv.size);
  std::array<u8, kDgoCommandSize> malformed_reply;
  memcpy(malformed_reply.data(), recv.data.c(), malformed_reply.size());
  rpc_call(3, DGO_RPC_LOAD_FNO, 1, send.data.offset, kDgoCommandSize - 1, recv.data.offset,
           kDgoCommandSize, 0);
  check(memcmp(malformed_reply.data(), recv.data.c(), malformed_reply.size()) == 0,
        "non-32-byte framing is rejected without mutating the reply");

  goal_dgo_rpc_stats rpc_stats = {};
  goal_dgo_goal_loader_stats(&rpc_stats);
  check_u32((u32)rpc_stats.dgo_archives, 2, "two well-framed DGO loads are counted");
  check_u32((u32)rpc_stats.dgo_objects, 4, "three completed and one cancelled objects are counted");
  check_guards(send, "DGO send canaries stay intact");
  check_guards(recv, "DGO receive canaries stay intact");
  for (const auto& buffer : object_buffers) {
    check_guards(buffer, "DGO object-buffer canaries stay intact");
  }
  check_guards(heap_a, "DGO heap-a canaries stay intact");
  check_guards(heap_b, "DGO heap-b canaries stay intact");
  check_guards(heap_c, "DGO heap-c canaries stay intact");

  std::printf("\n== AOT-code / DGO-data link rule ==\n");
  test_link_boundary(link_begin, link_resume);

  check_guards(sound_send, "sound send canaries stay intact through the router");
  check_guards(sound_recv, "sound receive canaries stay intact through the router");
  check_guards(str_send, "STR send canaries stay intact through the router");
  check_guards(str_recv, "STR receive canaries stay intact through the router");
  check_guards(str_data, "STR destination canaries stay intact through the router");

  goal_kernel_core_shutdown();
  std::filesystem::remove_all(temp);
  std::printf("\n%s (%d failures)\n", g_failures ? "JAK 2 DGO RPC TEST FAILED"
                                                 : "JAK 2 DGO RPC TEST PASSED",
              g_failures);
  return g_failures ? 1 : 0;
}
