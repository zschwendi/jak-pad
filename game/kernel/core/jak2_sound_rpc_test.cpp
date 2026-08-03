/*!
 * @file jak2_sound_rpc_test.cpp
 * Behavioral coverage for the narrow Jak 2 sound-loader and ordinary-file STR seams.
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

#include "game/kernel/common/kmalloc.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/core/sound_rpc_jak2.h"
#include "game/overlord/jak2/srpc.h"
#include "game/runtime.h"

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

  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_NOT_INITIALIZED,
        "installation rejects an uninitialized kernel");
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
  check(goal_jak2_sound_rpc_install() == GOAL_KERNEL_CORE_OK, "Jak 2 sound handshake installs");

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

  std::printf("\n== no-reply sound-bank command remains explicit and unimplemented ==\n");
  reset_command(send, jak2::Jak2SoundCommand::load_bank, 0x3456789a);
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto bank_send = snapshot(send);
  const auto bank_recv = snapshot(recv);
  check_u32((u32)rpc_call(1, 0, 1, send.command.offset, kCommandSize, 0, 0, 0), 0,
            "a command 2 request uses no reply buffer");
  check(snapshot(send) == bank_send && snapshot(recv) == bank_recv,
        "unimplemented bank loading mutates neither EE buffer");
  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.bank_load_requests, 1, "the well-framed bank request is counted");
  check_u32(stats.bank_load_unimplemented, 1, "the bank request is reported as unimplemented");
  check_guards(send, "no-reply command send canaries stay intact");
  check_guards(recv, "no-reply command receive canaries stay intact");

  std::printf("\n== synchronous ordinary-file STR loads ==\n");
  const auto fixture_root =
      std::filesystem::temp_directory_path() / "goalpad-jak2-sound-rpc-test";
  std::error_code fixture_error;
  std::filesystem::remove_all(fixture_root, fixture_error);
  std::filesystem::create_directories(fixture_root / "iso", fixture_error);
  std::array<u8, 96> fixture_bytes;
  for (u32 i = 0; i < fixture_bytes.size(); i++) {
    fixture_bytes[i] = (u8)(i ^ 0x5a);
  }
  constexpr const char* kFullWidthName = "ABCDEFGHIJKLMNOPQRSTUVWXYZ123456";
  check(!fixture_error && write_fixture(fixture_root / "iso" / "MIXED.TXT", fixture_bytes) &&
            write_fixture(fixture_root / "iso" / kFullWidthName, fixture_bytes),
        "create synthetic STR fixtures");
  goal_kernel_core_set_data_directory(fixture_root.string().c_str());

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

  std::printf("\n== unsupported and malformed requests remain unimplemented ==\n");
  reset_command(send, jak2::Jak2SoundCommand::load_bank, 0x3456789a);
  memset(recv.command.c(), 0xcc, kCommandSize);
  const auto unsupported_send = snapshot(send);
  const auto unsupported_recv = snapshot(recv);
  rpc_call(1, 0, 1, send.command.offset, kCommandSize, recv.command.offset, kCommandSize, 0);
  check(snapshot(send) == unsupported_send && snapshot(recv) == unsupported_recv,
        "a no-reply command with a reply buffer mutates neither buffer");
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
  rpc_call(4, 0, 1, str_send.data.offset, kStrRequestSize, str_recv.data.offset, kStrReplySize, 0);
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
        "chunked and malformed STR requests do not mutate the reply");
  check_u32((u32)rpc_busy(3), 0, "an unsupported busy query still returns not-busy");
  check_guards(send, "rejected-call send canaries stay intact");
  check_guards(recv, "rejected-call receive canaries stay intact");

  goal_jak2_sound_rpc_stats_get(&stats);
  check_u32(stats.version_requests, 2, "rejected calls do not count as handshakes");
  check_u32(stats.bank_load_requests, 1, "malformed bank framing is not counted as a request");
  check_u32(stats.bank_load_unimplemented, 1, "no bank request is acknowledged as loaded");
  check_u32(stats.str_requests, 9, "rejected STR calls do not count as file requests");
  check_u32(stats.rejected_calls, 25, "every unsupported request is reported");

  goal_kernel_core_set_data_directory(nullptr);
  std::filesystem::remove_all(fixture_root, fixture_error);
  goal_kernel_core_shutdown();
  std::printf("\n%s: Jak 2 sound-RPC version and ordinary-file STR seams\n",
              g_failures ? "FAIL" : "PASS");
  return g_failures ? 1 : 0;
}
