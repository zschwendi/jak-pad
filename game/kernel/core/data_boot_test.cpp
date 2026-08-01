/*!
 * @file data_boot_test.cpp
 * Boot Jak 1 the way the game boots it: out of the DGO archives, in DGO order, with the code of
 * every object supplied by the AOT path and the data of every object read off the disc.
 *
 * Two modes, because they need different things:
 *
 *   --synthetic  builds a DGO archive in a temporary directory and loads it. No game data is
 *                needed or read, so this always runs. It proves the archive reader and the
 *                code/data precedence rule, not the game.
 *
 *   (default)    loads the player's real KERNEL.CGO and GAME.CGO and then runs the engine's own
 *                startup - `play`, then the GOAL kernel dispatcher. Needs a data directory, given
 *                by --data-dir or GOALPAD_JAK1_DATA_DIR, and reports that it was skipped when
 *                there is none. No game data is ever written anywhere.
 *
 * Like aot_boot_test, this is a progress probe: it reports how far the boot got and what stopped
 * it. Nothing here may skip a step to get further.
 */

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include "aot_boot_manifest.h"
}

#include "common/dma/dma_chain_read.h"
#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"
#include "common/symbols.h"

#include "game/kernel/common/kboot.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmalloc.h"
#include "game/kernel/common/kprint.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/core/aot_loader.h"
#include "game/kernel/core/dgo_loader.h"
#include "game/kernel/core/dma_capture.h"
#include "game/kernel/core/kernel_core.h"
#include "game/kernel/jak1/klisten.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

#include "third-party/lzokay/lzokay.hpp"

namespace {

void say(const char* format, ...) __attribute__((format(printf, 1, 2)));
void say(const char* format, ...) {
  va_list args;
  va_start(args, format);
  std::vfprintf(stdout, format, args);
  va_end(args);
  std::fflush(stdout);
}

/*!
 * `(method update-vis! level)` in engine/load/decomp.gc checks its own decompressed visibility
 * against the BSP's all-visible list and prints this when a bit names a drawable that does not
 * exist. Nothing stops when it happens - the game only reports it - so the report is counted here
 * and failed on at the end. It is the one signal that says the visibility a frame computed is
 * wrong.
 */
int g_illegal_vis_reports = 0;

/*! Anything GOAL printed since the last call, so a failing top-level's own message is visible. */
void drain_goal_print_buffer() {
  const char* printed = Ptr<char>(PrintBufArea.offset + sizeof(ListenerMessageHeader)).c();
  if (printed[0]) {
    for (const char* at = printed; (at = std::strstr(at, "illegal vis bits set")) != nullptr; at++) {
      g_illegal_vis_reports++;
    }
    say("GOAL said: %s\n", printed);
    clear_print();
  }
}

/*!
 * How close the run came to overflowing a GOAL thread's backup stack. The sizes come from
 * `process-stack-save-size` in kernel/gkernel-h.gc, which converts the game's PS2 measurements for
 * this build's code generator; this says whether that conversion is generous or nearly wrong.
 */
void report_stack_watermark() {
  goal_thread_stack_watermark_report w;
  goal_thread_stack_watermark(&w);
  if (!w.suspends) {
    return;
  }
  say("  backup stacks: %d suspends, deepest %d bytes of %d ('%s), fullest %d of %d (%d%%, '%s)\n",
      w.suspends, w.deepest_used, w.deepest_size, w.deepest_name, w.fullest_used, w.fullest_size,
      w.fullest_size ? 100 * w.fullest_used / w.fullest_size : 0, w.fullest_name);
}


void report_heap(const char* what) {
  goal_kernel_core_state state;
  if (goal_kernel_core_get_state(&state) == GOAL_KERNEL_CORE_OK) {
    say("  %s: global heap at #x%x (%u bytes used), %d symbols\n", what,
        state.global_heap_current_offset, state.global_heap_used_bytes, state.symbol_count);
  }
}

/*! The DGO object name for a GOAL source: its base name without the extension. */
std::string object_name_of(const char* source) {
  std::string path = source;
  const auto slash = path.find_last_of('/');
  if (slash != std::string::npos) {
    path = path.substr(slash + 1);
  }
  const auto dot = path.find_last_of('.');
  if (dot != std::string::npos) {
    path = path.substr(0, dot);
  }
  return path;
}

/*!
 * Tell the DGO loader which native translation unit stands in for each object it will meet.
 */
void register_aot_objects() {
  for (int i = 0; i < goal_aot_boot_file_count; i++) {
    const auto& entry = goal_aot_boot_files[i];
    goal_aot_object_file file = {entry.tag,       entry.statics,          *entry.static_count,
                                 entry.functions, *entry.function_count,  entry.link};
    goal_aot_register_object(object_name_of(entry.source).c_str(), &file);
  }
}

/*!
 * GAME.CGO is the release build's union of ENGINE.CGO, ART.CGO and COMMON.CGO, which do not exist
 * as separate files on the disc. The C kernel records that by putting their names in
 * `*kernel-packages*` after loading GAME.CGO (jak1::InitMachineScheme in
 * game/kernel/jak1/kmachine.cpp), so `alloc-levels!`'s `(load-package "art" global)` finds "art"
 * already loaded and asks for nothing. Without this the level system tries to load ART.CGO and
 * fails, because there is no such file to load.
 */
void record_packages_in_game_cgo() {
  using namespace jak1_symbols;
  for (const char* package : {"engine", "art", "common"}) {
    jak1::kernel_packages->value =
        jak1::new_pair(s7.offset + FIX_SYM_GLOBAL_HEAP, *((s7 + FIX_SYM_PAIR_TYPE).cast<u32>()),
                       jak1::make_string_from_c(package), jak1::kernel_packages->value);
  }
}

std::vector<std::string> split_commas(const std::string& list) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = list.find(',', at);
    const std::string name = list.substr(at, comma == std::string::npos ? comma : comma - at);
    if (!name.empty()) {
      out.push_back(name);
    }
    if (comma == std::string::npos) {
      break;
    }
    at = comma + 1;
  }
  return out;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  FILE* fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    return false;
  }
  const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), fp) == bytes.size();
  std::fclose(fp);
  return ok;
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; i++) {
    out.push_back((uint8_t)(value >> (8 * i)));
  }
}

void append_name(std::vector<uint8_t>& out, const std::string& name) {
  for (int i = 0; i < 60; i++) {
    out.push_back(i < (int)name.size() ? (uint8_t)name[i] : 0);
  }
}

/*!
 * A DGO holding the eight objects of the real KERNEL.CGO, by name, with bodies that are not the
 * game's: each is a v3 OpenGOAL header over filler. The loader must recognize them as code, throw
 * the bodies away, and run the native translation instead - so the filler is exactly the point.
 * Nothing here comes from the game.
 */
std::vector<uint8_t> build_synthetic_dgo(const std::vector<std::string>& objects) {
  std::vector<uint8_t> out;
  append_u32(out, (uint32_t)objects.size());
  append_name(out, "SYNTH.CGO");

  for (const auto& name : objects) {
    // an object body just long enough to hold a link header the reader will classify
    std::vector<uint8_t> body;
    append_u32(body, 0x4c414f47);  // 'GOAL', the v3 type tag: not the 0xffffffff of v2/v4 data
    append_u32(body, 0);
    append_u32(body, 3);  // object file version
    while (body.size() < 48) {
      body.push_back(0xcd);
    }

    append_u32(out, (uint32_t)body.size());
    append_name(out, name);
    out.insert(out.end(), body.begin(), body.end());
    while (out.size() % 16) {
      out.push_back(0);
    }
  }
  return out;
}

/*!
 * A capture file read back from nothing but the format documented at the top of dma_capture.cpp -
 * which is the point: the renderer track has to be able to write this reader from that comment.
 */
struct ReadCapture {
  uint32_t version = 0;
  uint32_t header_bytes = 0;
  uint32_t frame = 0;
  uint32_t chunk_size = 0;
  uint32_t ee_mem_size = 0;
  uint32_t ee_base_goal_address = 0;
  uint32_t s7 = 0;
  uint32_t sections = 0;
  uint32_t chain_start_offset = 0;
  uint32_t stored_chunks = 0;
  std::vector<uint32_t> stored_index;
  std::vector<uint8_t> chain;  // the packed chunks the chain lives in
  std::vector<uint8_t> ee;     // EE main memory, zero-filled where the file stored nothing
  std::string error;
};

uint32_t read_u32(const std::vector<uint8_t>& bytes, size_t at) {
  uint32_t value = 0;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t at) {
  uint64_t value = 0;
  std::memcpy(&value, bytes.data() + at, sizeof(value));
  return value;
}

ReadCapture read_capture(const std::string& path) {
  ReadCapture out;
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    out.error = "cannot open " + path;
    return out;
  }
  std::vector<uint8_t> file;
  uint8_t block[64 * 1024];
  while (const size_t got = std::fread(block, 1, sizeof(block), fp)) {
    file.insert(file.end(), block, block + got);
  }
  std::fclose(fp);

  if (file.size() < 80 || std::memcmp(file.data(), "GPDMACAP", 8) != 0) {
    out.error = "not a GPDMACAP file";
    return out;
  }
  out.version = read_u32(file, 8);
  out.header_bytes = read_u32(file, 12);
  out.frame = read_u32(file, 16);
  out.chunk_size = read_u32(file, 20);
  out.ee_mem_size = read_u32(file, 24);
  out.ee_base_goal_address = read_u32(file, 28);
  out.s7 = read_u32(file, 32);
  out.sections = read_u32(file, 36);
  const uint64_t chain_offset = read_u64(file, 40);
  const uint64_t chain_bytes = read_u64(file, 48);
  const uint64_t ee_offset = read_u64(file, 56);
  const uint64_t ee_bytes = read_u64(file, 64);
  if (chain_offset + chain_bytes > file.size() || ee_offset + ee_bytes > file.size()) {
    out.error = "a section runs off the end of the file";
    return out;
  }

  if (chain_bytes < 12) {
    out.error = "chain section is too small";
    return out;
  }
  out.chain_start_offset = read_u32(file, chain_offset);
  const uint64_t payload_bytes = read_u64(file, chain_offset + 4);
  if (12 + payload_bytes != chain_bytes) {
    out.error = "chain payload length does not fill the chain section";
    return out;
  }
  out.chain.assign(file.begin() + chain_offset + 12,
                   file.begin() + chain_offset + 12 + payload_bytes);

  if (ee_bytes) {
    const uint32_t chunk_count = read_u32(file, ee_offset);
    out.stored_chunks = read_u32(file, ee_offset + 4);
    const uint32_t compression = read_u32(file, ee_offset + 8);
    if (chunk_count * (uint64_t)out.chunk_size != out.ee_mem_size) {
      out.error = "EE chunk count does not cover EE main memory";
      return out;
    }
    if (compression != 1) {
      out.error = "unexpected EE compression " + std::to_string(compression);
      return out;
    }
    out.ee.assign(out.ee_mem_size, 0);
    uint64_t payload_at = ee_offset + 16 + (uint64_t)out.stored_chunks * 8;
    for (uint32_t i = 0; i < out.stored_chunks; i++) {
      const uint32_t chunk = read_u32(file, ee_offset + 16 + (uint64_t)i * 8);
      const uint32_t stored = read_u32(file, ee_offset + 16 + (uint64_t)i * 8 + 4);
      std::size_t produced = 0;
      const auto result = lzokay::decompress(file.data() + payload_at, stored,
                                             out.ee.data() + (size_t)chunk * out.chunk_size,
                                             out.chunk_size, produced);
      if (result != lzokay::EResult::Success || produced != out.chunk_size) {
        out.error = "chunk " + std::to_string(chunk) + " did not decompress to a whole chunk";
        return out;
      }
      out.stored_index.push_back(chunk);
      payload_at += stored;
    }
    if (payload_at != ee_offset + ee_bytes) {
      out.error = "EE section has trailing bytes";
      return out;
    }
  }
  return out;
}

/*!
 * A DMA chain written by hand into EE main memory, shaped like the one the game builds: an initial
 * CALL to a default-registers chain, then a bucket array. Bucket 0 gets a PC-port texture-upload
 * packet and a block of data, bucket 1 is empty, bucket 2 jumps to the end.
 *
 * `marker` is written into bucket 0's data, so a reader can tell which call built which chain -
 * which is how frame selection is checked rather than assumed.
 */
constexpr uint32_t kSyntheticChain = 0x2000000;

void build_synthetic_dma_chain(uint8_t marker) {
  const uint32_t chain = kSyntheticChain;
  const uint32_t default_regs = chain + 0x100;
  const uint32_t bucket0 = chain + 0x200;
  const uint32_t end_tag = chain + 0x300;
  uint8_t* memory = (uint8_t*)g_ee_main_mem;
  std::memset(memory + chain, 0, 0x400);

  auto put_tag = [&](uint32_t at, DmaTag::Kind kind, uint16_t qwc, uint32_t addr, uint32_t vif0,
                     uint32_t vif1) {
    const uint64_t tag = (uint64_t)qwc | ((uint64_t)kind << 28) | ((uint64_t)addr << 32);
    std::memcpy(memory + at, &tag, sizeof(tag));
    std::memcpy(memory + at + 8, &vif0, sizeof(vif0));
    std::memcpy(memory + at + 12, &vif1, sizeof(vif1));
  };

  put_tag(chain + 0, DmaTag::Kind::CALL, 0, default_regs, 0, 0);
  put_tag(chain + 16, DmaTag::Kind::NEXT, 0, bucket0, 0, 0);  // bucket 0
  put_tag(chain + 32, DmaTag::Kind::CNT, 0, 0, 0, 0);         // bucket 1, empty
  put_tag(chain + 48, DmaTag::Kind::NEXT, 0, end_tag, 0, 0);  // bucket 2

  put_tag(default_regs, DmaTag::Kind::CNT, 10, 0, 0, 0);
  put_tag(default_regs + 176, DmaTag::Kind::RET, 0, 0, 0, 0);

  // the PC port's texture upload: vif0 is a PC_PORT vifcode, vif1 is 3, and the quadword of data
  // is { u64 texture-page address; s64 mode }. The page here points back at this chain, which is
  // memory the snapshot certainly holds.
  const uint32_t pc_port_vif0 = (uint32_t)VifCode::Kind::PC_PORT << 24;
  put_tag(bucket0 + 0, DmaTag::Kind::CNT, 1, 0, pc_port_vif0, 3);
  const uint64_t page = chain;
  const uint64_t mode = 0;
  std::memcpy(memory + bucket0 + 16, &page, sizeof(page));
  std::memcpy(memory + bucket0 + 24, &mode, sizeof(mode));

  put_tag(bucket0 + 32, DmaTag::Kind::CNT, 2, 0, 0, 0);
  std::memset(memory + bucket0 + 48, marker, 32);
  put_tag(bucket0 + 80, DmaTag::Kind::NEXT, 0, chain + 32, 0, 0);

  put_tag(end_tag, DmaTag::Kind::END, 0, 0, 0, 0);
}

/*!
 * Hand three chains to `__send-gfx-dma-chain` and ask for the second one. Checks that the selector
 * picks that frame and no other, that the measurements match the chain that was built, and that
 * both halves of the capture file - the chain and the EE snapshot - read back from the documented
 * format alone.
 *
 * No game data is involved: the chain is built here.
 */
int run_capture_round_trip(const std::string& dir) {
  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    say("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
      failures++;
    }
  };

  auto send = jak1::find_symbol_from_c("__send-gfx-dma-chain");
  if (!send.offset || !send->value) {
    say("FAIL: __send-gfx-dma-chain holds nothing\n");
    return 1;
  }

  const std::string path = dir + "/dma-frame-2.gpdma";
  remove(path.c_str());
  const int wanted = 2;
  goal_gfx_dma_capture_frames_to_dir(dir.c_str(), &wanted, 1);
  for (uint8_t frame = 1; frame <= 3; frame++) {
    build_synthetic_dma_chain(frame);
    goal_aot_call(send->value, 0, kSyntheticChain, 0);
  }

  goal_gfx_dma_stats stats;
  goal_gfx_dma_get_stats(&stats);
  expect(stats.chains == 3, "all three chains were followed");
  expect(stats.captures == 1, "exactly the one requested frame was captured");

  goal_gfx_dma_frame_summary summary;
  expect(goal_gfx_dma_get_frame(2, &summary) && summary.payload_bytes == 48 && summary.tags == 7 &&
             summary.texture_uploads == 1 && summary.buckets == 3,
         "the bucket walk measured the chain that was built");
  if (goal_gfx_dma_get_frame(2, &summary)) {
    say("  frame 2: %u bytes of payload, %d tags, %d texture uploads, %d buckets\n",
        summary.payload_bytes, summary.tags, summary.texture_uploads, summary.buckets);
  }
  goal_gfx_dma_bucket_summary bucket;
  expect(goal_gfx_dma_get_bucket(2, 0, &bucket) && bucket.payload_bytes == 48 &&
             bucket.transfers == 2 && bucket.texture_uploads == 1,
         "bucket 0 kept the texture upload and the data block");
  expect(goal_gfx_dma_get_bucket(2, 1, &bucket) && bucket.payload_bytes == 0, "bucket 1 was empty");
  expect(!goal_gfx_dma_get_bucket(1, 0, &bucket),
         "frames that were not captured keep no bucket detail");

  const ReadCapture capture = read_capture(path);
  if (!capture.error.empty()) {
    say("FAIL: %s\n", capture.error.c_str());
    return failures + 1;
  }
  goal_kernel_core_state state;
  goal_kernel_core_get_state(&state);
  expect(capture.version == 2 && capture.header_bytes == 80, "the file says version 2");
  expect(capture.frame == 2, "the file names the frame that was asked for");
  expect(capture.chunk_size == 0x20000 && capture.ee_mem_size == (uint32_t)EE_MAIN_MEM_SIZE,
         "the file records the chunk size and EE memory size");
  expect(capture.ee_base_goal_address == 0 && capture.s7 == state.s7_offset,
         "the file records s7 and the EE base so GOAL pointers can be resolved");
  expect(capture.sections == 3, "both the chain and the EE snapshot are present");

  expect(!capture.chain.empty() && capture.chain.size() % capture.chunk_size == 0,
         "the chain payload is a whole number of chunks");
  // the whole chain, including the initial CALL to the 10-quadword default-registers chain that
  // the bucket walk treats as structure rather than as a bucket's content
  uint32_t payload_bytes = 0;
  int tags = 0;
  {
    DmaFollower dma(capture.chain.data(), capture.chain_start_offset);
    while (!dma.ended()) {
      payload_bytes += dma.read_and_advance().size_bytes;
      tags++;
    }
  }
  expect(payload_bytes == 48 + 160 && tags == 7 + 3,
         "the chain read back out of the file is the same chain");

  expect(capture.ee.size() == (size_t)EE_MAIN_MEM_SIZE, "the EE snapshot covers EE main memory");
  expect(std::memcmp(capture.ee.data() + kSyntheticChain, (uint8_t*)g_ee_main_mem + kSyntheticChain,
                     0x400) != 0,
         "the EE snapshot is of frame 2, not of the memory left behind by frame 3");
  expect(capture.ee[kSyntheticChain + 0x200 + 48] == 2, "the EE snapshot holds frame 2's own data");
  expect(capture.ee[state.s7_offset] != 0 || capture.ee[state.s7_offset + 4] != 0,
         "the EE snapshot holds the symbol table s7 points into");

  // a chunk the file left out has to be zero in the snapshot and zero in the memory it came from,
  // or leaving it out lost something
  const uint32_t chunks = capture.ee_mem_size / capture.chunk_size;
  uint32_t absent = 0;
  for (uint32_t chunk = EE_MAIN_MEM_LOW_PROTECT / 0x20000; chunk < chunks && !absent; chunk++) {
    if (std::find(capture.stored_index.begin(), capture.stored_index.end(), chunk) ==
        capture.stored_index.end()) {
      absent = chunk;
    }
  }
  bool absent_is_zero = absent != 0;
  for (uint32_t at = 0; at < capture.chunk_size && absent_is_zero; at++) {
    const size_t address = (size_t)absent * capture.chunk_size + at;
    absent_is_zero = capture.ee[address] == 0 && ((uint8_t*)g_ee_main_mem)[address] == 0;
  }
  expect(absent_is_zero, "a chunk the file left out is zero, in the file and in EE memory");
  say("  %u of %u chunks stored; chunk %u is one of the ones left out\n", capture.stored_chunks,
      chunks, absent);

  remove(path.c_str());
  return failures;
}

int run_synthetic() {
  const char* tmp = std::getenv("TMPDIR");
  const std::string dir = std::string(tmp && tmp[0] ? tmp : "/tmp") + "/goalpad-synthetic-dgo";
  const std::string iso = dir + "/iso";
  mkdir(dir.c_str(), 0755);
  if (mkdir(iso.c_str(), 0755) != 0 && errno != EEXIST) {
    say("FAIL: could not make %s\n", iso.c_str());
    return 1;
  }

  const std::vector<std::string> objects = {"gcommon",  "gstring-h", "gkernel-h", "gkernel",
                                            "pskernel", "gstring",   "dgo-h",     "gstate"};
  if (!write_file(iso + "/SYNTH.CGO", build_synthetic_dgo(objects))) {
    say("FAIL: could not write %s/SYNTH.CGO\n", iso.c_str());
    return 1;
  }

  goal_kernel_core_set_data_directory(dir.c_str());
  goal_kernel_core_stub_machine_layer(0);

  goal_dgo_load_stats stats;
  say("loading the synthetic archive from %s\n", iso.c_str());
  if (goal_dgo_load("SYNTH", LINK_FLAG_EXECUTE, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAIL: %s\n", goal_dgo_last_error());
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code (from the AOT path), %d data, %d already loaded\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.reused_code);
  report_heap("after");

  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    say("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
      failures++;
    }
  };
  expect(stats.objects == (int)objects.size(), "every object in the archive was processed");
  expect(stats.code_objects == (int)objects.size(), "all of them were classified as code");
  expect(stats.data_objects == 0, "none of them were linked as data");

  // If the top-levels really ran, the GOAL kernel is there: these come from gkernel.gc and
  // gstate.gc, not from the C kernel.
  uint32_t value = 0;
  expect(goal_kernel_core_lookup("*kernel-version*", nullptr, &value) == GOAL_KERNEL_CORE_OK &&
             value != 0,
         "*kernel-version* was set by the GOAL kernel's own top-level");
  say("  *kernel-version* = %u.%u\n", value >> 0x13, (value >> 3) & 0xffff);
  expect(goal_kernel_core_lookup("kernel-dispatcher", nullptr, &value) == GOAL_KERNEL_CORE_OK &&
             value != 0,
         "kernel-dispatcher holds a function");

  // A code object with no native translation must fail loudly rather than be skipped.
  if (!write_file(iso + "/NOSUCH.CGO", build_synthetic_dgo({"an-object-that-does-not-exist"}))) {
    say("FAIL: could not write the second fixture\n");
    return 1;
  }
  const auto status = goal_dgo_load("NOSUCH", LINK_FLAG_EXECUTE, 0x400000, nullptr);
  expect(status != GOAL_KERNEL_CORE_OK, "an untranslated code object fails the load");
  say("  reported: %s\n", goal_dgo_last_error());

  say("\n=== DMA capture, frame selection and the capture file format\n");
  goal_gfx_dma_install();
  failures += run_capture_round_trip(dir);

  remove((iso + "/SYNTH.CGO").c_str());
  remove((iso + "/NOSUCH.CGO").c_str());
  rmdir(iso.c_str());
  rmdir(dir.c_str());
  return failures ? 1 : 0;
}

/*!
 * The boot sequence itself, in the order jak1::InitHeapAndSymbol and jak1::InitMachineScheme run
 * it. Every step says what it is standing in for.
 */
/*! What the caller asked to be captured, and where. See dma_capture.h for the file format. */
struct DmaCaptureRequest {
  std::string path;               // --capture-dma: one frame to exactly this file
  int frame = 1;                  // --capture-dma-frame
  std::string dir;                // --capture-dma-dir
  std::vector<int> dir_frames;    // --capture-dma-frames
  uint32_t min_payload = 0;       // --capture-dma-min-payload
  int over_count = 0;             // --capture-dma-count
  bool per_frame_report = false;  // --dma-frame-report

  int count() const {
    return (path.empty() ? 0 : 1) + (dir.empty() ? 0 : (int)dir_frames.size() + over_count);
  }

  void install() const {
    if (!path.empty()) {
      goal_gfx_dma_capture_frame_to_file(path.c_str(), frame);
    }
    if (!dir.empty() && !dir_frames.empty()) {
      goal_gfx_dma_capture_frames_to_dir(dir.c_str(), dir_frames.data(), (int)dir_frames.size());
    }
    if (!dir.empty() && over_count > 0) {
      goal_gfx_dma_capture_frames_over(dir.c_str(), min_payload, over_count);
    }
  }
};

/*!
 * Bucket names for the Jak 1 chain, indexed the same way `jak1::BucketId` is. Only the ones a
 * report needs to be readable are named; the rest print as their number. Kept here rather than
 * pulled from game/graphics so this library does not depend on the renderer.
 */
const char* jak1_bucket_name(int bucket) {
  switch (bucket) {
    case 3:
      return "sky-draw";
    case 4:
      return "ocean-mid-and-far";
    case 5:
      return "tfrag-tex-l0";
    case 6:
      return "tfrag-l0";
    case 7:
      return "tfrag-near-l0";
    case 8:
      return "tie-near-l0";
    case 9:
      return "tie-l0";
    case 10:
      return "merc-tfrag-tex-l0";
    case 12:
      return "tfrag-tex-l1";
    case 13:
      return "tfrag-l1";
    case 16:
      return "tie-l1";
    case 17:
      return "merc-tfrag-tex-l1";
    case 19:
      return "shrub-tex-l0";
    case 20:
      return "shrub-normal-l0";
    case 25:
      return "shrub-tex-l1";
    case 26:
      return "shrub-normal-l1";
    case 31:
      return "alpha-tex-l0";
    case 32:
      return "tfrag-trans0-and-sky-blend-l0";
    case 38:
      return "alpha-tex-l1";
    case 39:
      return "tfrag-trans1-and-sky-blend-l1";
    case 45:
      return "merc-after-alpha";
    case 47:
      return "shadow";
    case 48:
      return "pris-tex-l0";
    case 49:
      return "merc-pris-l0";
    case 51:
      return "pris-tex-l1";
    case 52:
      return "merc-pris-l1";
    case 54:
      return "merc-eyes-after-pris";
    case 55:
      return "merc-after-pris";
    case 63:
      return "ocean-near";
    case 64:
      return "depth-cue";
    case 65:
      return "pre-sprite-tex";
    case 66:
      return "sprite";
    case 67:
      return "debug";
    case 68:
      return "debug-no-zbuf";
    case 69:
      return "subtitle";
    // Jak 1 has 70 buckets; the walk keeps going to the end of the chain, so what follows is the
    // chain's ending data, which the renderer's bucket dispatch also walks past
    default:
      return bucket >= 70 ? "(chain ending data)" : "";
  }
}

/*! Every frame's chain, one line each: what a caller uses to find a frame worth capturing. */
void report_every_frame() {
  const int count = goal_gfx_dma_frame_count();
  say("\n=== every frame's chain\n");
  say("  %6s %12s %12s %8s %8s %8s\n", "frame", "payload", "copied", "tags", "uploads", "buckets");
  for (int frame = 1; frame <= count; frame++) {
    goal_gfx_dma_frame_summary f;
    if (goal_gfx_dma_get_frame(frame, &f)) {
      say("  %6d %12u %12u %8d %8d %8d\n", f.frame, f.payload_bytes, f.copied_bytes, f.tags,
          f.texture_uploads, f.buckets);
    }
  }
}

/*! The buckets of one captured frame, largest payload first. Empty buckets are left out. */
void report_captured_frame_buckets(int frame) {
  goal_gfx_dma_frame_summary f;
  if (!goal_gfx_dma_get_frame(frame, &f)) {
    return;
  }
  std::vector<goal_gfx_dma_bucket_summary> buckets;
  for (int bucket = 0;; bucket++) {
    goal_gfx_dma_bucket_summary b;
    if (!goal_gfx_dma_get_bucket(frame, bucket, &b)) {
      break;
    }
    if (b.payload_bytes || b.texture_uploads) {
      buckets.push_back(b);
    }
  }
  if (buckets.empty()) {
    return;
  }
  say("\n=== frame %d: %u bytes of payload in %d buckets, %d texture uploads\n", frame,
      f.payload_bytes, f.buckets, f.texture_uploads);
  std::sort(buckets.begin(), buckets.end(),
            [](const goal_gfx_dma_bucket_summary& a, const goal_gfx_dma_bucket_summary& b) {
              return a.payload_bytes > b.payload_bytes;
            });
  for (const auto& b : buckets) {
    say("  bucket %3d %-32s %10u bytes, %5d transfers, %3d texture uploads\n", b.bucket,
        jak1_bucket_name(b.bucket), b.payload_bytes, b.transfers, b.texture_uploads);
  }
}

int run_real_boot(const std::string& data_dir,
                  int dispatch_frames,
                  bool run_play,
                  const DmaCaptureRequest& capture,
                  const std::vector<std::string>& level_cycle,
                  int level_cycle_frames) {
  goal_kernel_core_set_data_directory(data_dir.c_str());
  say("data directory: %s\n", data_dir.c_str());

  goal_dgo_load_stats stats;
  const u32 boot_flags = LINK_FLAG_OUTPUT_LOAD | LINK_FLAG_EXECUTE | LINK_FLAG_PRINT_LOGIN;

  // InitHeapAndSymbol: the GOAL kernel.
  say("\n=== KERNEL.CGO\n");
  if (goal_dgo_load("KERNEL", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAILED: %s\n", goal_dgo_last_error());
    drain_goal_print_buffer();
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code, %d data; heap use %u -> %u bytes\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.heap_used_before, stats.heap_used_after);
  report_heap("after KERNEL.CGO");

  uint32_t kernel_version = 0;
  goal_kernel_core_lookup("*kernel-version*", nullptr, &kernel_version);
  if (!kernel_version) {
    say("FAILED: the GOAL kernel did not set *kernel-version*\n");
    return 1;
  }
  say("  GOAL kernel version %u.%u\n", kernel_version >> 0x13, (kernel_version >> 3) & 0xffff);

  // InitListener, then InitMachineScheme. The machine layer is not in this library; the stubs
  // stand in for it and name themselves the first time GOAL calls one. The DGO RPC and the two
  // linker entry points GOAL's own level loader drives are real, and replace the stubs.
  jak1::InitListener();
  goal_kernel_core_stub_machine_layer(0);
  goal_dgo_install_goal_loader();
  goal_gfx_dma_install();
  capture.install();
  jak1::intern_from_c("*kernel-boot-message*")->value =
      jak1::intern_from_c(DebugBootMessage).offset;
  jak1::intern_from_c("*kernel-boot-mode*")->value = jak1::intern_from_c("boot").offset;
  jak1::intern_from_c("*kernel-boot-level*")->value =
      jak1::intern_from_c(DebugBootLevel).offset;

  // InitMachineScheme's DiskBoot path: the engine and the game.
  say("\n=== GAME.CGO\n");
  if (goal_dgo_load("GAME", boot_flags, 0x400000, &stats) != GOAL_KERNEL_CORE_OK) {
    say("FAILED: %s\n", goal_dgo_last_error());
    drain_goal_print_buffer();
    say("  got through %d of GAME.CGO's objects (%d code, %d data)\n", stats.objects,
        stats.code_objects, stats.data_objects);
    return 1;
  }
  drain_goal_print_buffer();
  say("  %d objects: %d code, %d data; heap use %u -> %u bytes\n", stats.objects,
      stats.code_objects, stats.data_objects, stats.heap_used_before, stats.heap_used_after);
  report_heap("after GAME.CGO");
  record_packages_in_game_cgo();

  // The last thing InitMachineScheme does. `play` allocates the level heaps and drives the whole
  // title-level load itself: with no display process running yet its `while` loop calls
  // `load-continue` until the level reaches 'active, which is what needs the DGO RPC and
  // `link-begin` above.
  if (run_play) {
    say("\n=== (play)\n");
    const u64 play_result = jak1::call_goal_function_by_name("play");
    drain_goal_print_buffer();
    say("  play returned #x%" PRIx64 "\n", play_result);
    report_heap("after play");
  } else {
    say("\n=== (play) skipped; pass --play to run it.\n");
  }

  // KernelCheckAndDispatch's loop body, without the listener half: this is the GOAL kernel's own
  // frame, running processes and states.
  auto dispatcher = jak1::find_symbol_from_c("kernel-dispatcher");
  if (!dispatcher.offset || !dispatcher->value) {
    say("FAILED: kernel-dispatcher holds nothing\n");
    return 1;
  }
  say("\n=== kernel-dispatcher: %d frames\n", dispatch_frames);
  auto run_frames = [&](int count) {
    for (int frame = 0; frame < count; frame++) {
      call_goal_on_stack(Ptr<Function>(dispatcher->value), goal_kernel_stack_top(), s7.offset,
                         g_ee_main_mem);
      drain_goal_print_buffer();
    }
  };
  run_frames(dispatch_frames);
  report_heap("after the dispatcher");

  // Ask the level system for one level at a time. The point is the global heap: a level's object
  // files are linked into the level's own heap, which `(method unload! level)` resets, so cycling
  // through levels must not make the global heap grow. It did before that routing was fixed - each
  // level's code went into the global heap and stayed there.
  u32 heap_before_cycle = 0;
  u32 heap_after_cycle = 0;
  if (!level_cycle.empty()) {
    say("\n=== level cycle: %d levels, %d frames each\n", (int)level_cycle.size(),
        level_cycle_frames);
    goal_kernel_core_state state;
    goal_kernel_core_get_state(&state);
    heap_before_cycle = state.global_heap_used_bytes;
    for (const auto& name : level_cycle) {
      auto want = jak1::find_symbol_from_c("load-state-want-levels");
      if (!want.offset || !want->value) {
        say("FAILED: load-state-want-levels holds nothing\n");
        return 1;
      }
      // One step of the cycle names the levels the load state should want, "a" or "a+b".
      // "none" is #f, which unloads whatever is loaded.
      auto as_symbol = [](const std::string& n) {
        return n.empty() || n == "none" ? s7.offset : jak1::intern_from_c(n.c_str()).offset;
      };
      const size_t plus = name.find('+');
      goal_aot_call(want->value, as_symbol(name.substr(0, plus)),
                    plus == std::string::npos ? s7.offset : as_symbol(name.substr(plus + 1)), 0);
      run_frames(level_cycle_frames);
      goal_kernel_core_get_state(&state);
      goal_dgo_rpc_stats step;
      goal_dgo_goal_loader_stats(&step);
      say("  wanted %-10s -> global heap %u bytes (%+d since the cycle began),"
          " %u bytes of level code linked so far\n",
          name.c_str(), state.global_heap_used_bytes,
          (int)state.global_heap_used_bytes - (int)heap_before_cycle, step.level_code_bytes);
    }
    heap_after_cycle = state.global_heap_used_bytes;
  }
  report_stack_watermark();

  goal_gfx_dma_stats dma;
  goal_gfx_dma_get_stats(&dma);
  say("  DMA: %d chains built. Payload - what the tags actually transfer - largest %u bytes"
      " (frame %d, %d texture uploads), last %u bytes. Chunks touched: largest %u, last %u.\n"
      "  Nothing was drawn: there is no renderer here, so the chains are followed, measured,"
      " sometimes written to a file, and dropped.\n",
      dma.chains, dma.largest_payload_bytes, dma.largest_payload_frame, dma.largest_payload_uploads,
      dma.last_payload_bytes, dma.largest_bytes, dma.last_bytes);
  if (dma.captures) {
    say("  %d capture file(s) written, %u bytes in total.\n", dma.captures, dma.captured_bytes);
  }
  if (capture.per_frame_report) {
    report_every_frame();
  }
  for (int frame = 1; frame <= goal_gfx_dma_frame_count(); frame++) {
    report_captured_frame_buckets(frame);
  }

  goal_dgo_rpc_stats rpc;
  goal_dgo_goal_loader_stats(&rpc);
  say("  GOAL's own loader: %d DGO loads, %d objects (%d code from the AOT path, %d data linked),"
      " %d STR reads, %d STR misses\n",
      rpc.dgo_archives, rpc.dgo_objects, rpc.linked_code_objects, rpc.linked_data_objects,
      rpc.str_reads, rpc.str_failures);
  say("  visibility: %d .VIS files in the ramdisk, %d vis strings read, %d misses,"
      " %d illegal-vis reports\n",
      rpc.ramdisk_files, rpc.ramdisk_reads, rpc.ramdisk_misses, g_illegal_vis_reports);
  say("  level code: %u bytes linked into level heaps, and none into the global heap\n",
      rpc.level_code_bytes);

  int failures = 0;
  auto expect = [&](bool ok, const char* what) {
    say("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
      failures++;
    }
  };
  if (run_play) {
    // `play` cannot finish without GOAL's own loader having read a level out of a DGO through the
    // RPC and linked both halves of it, so these are the shape of what it did, not a restatement
    // of "it did not crash".
    expect(rpc.dgo_archives > 0, "GOAL started a level DGO load through the RPC");
    expect(rpc.dgo_objects >= rpc.linked_code_objects + rpc.linked_data_objects,
           "every object GOAL linked came from the RPC");
    expect(rpc.linked_code_objects > 0, "GOAL linked level code through the AOT path");
    expect(rpc.linked_data_objects > 0, "GOAL linked level data through the real linker");
  }
  if (dispatch_frames > 0) {
    goal_thread_stack_watermark_report w;
    goal_thread_stack_watermark(&w);
    expect(w.suspends > 0, "processes suspended and resumed across the frames");
    expect(w.fullest_used <= w.fullest_size, "no backup stack was overrun");
    if (run_play) {
      // A level's own visibility comes out of its .VIS file through the ramdisk RPC, and
      // `update-vis!` checks its own decompressed output. Both halves have to hold: no read may
      // fail, and no swap may produce a bit for a drawable the BSP does not have.
      expect(rpc.ramdisk_reads > 0 && rpc.ramdisk_misses == 0,
             "every vis string a level asked for was read");
      expect(g_illegal_vis_reports == 0, "every visibility swap decompressed to legal bits");
    }
    if (run_play) {
      // Following a chain means reading every tag in it, so a chain that came back with a size is
      // a chain that was well-formed.
      expect(dma.chains > 0 && dma.largest_bytes > 0, "the frames built real DMA chains");
    }
    if (capture.count() > 0) {
      // A frame that was asked for and never arrived is a silent miss otherwise: the run would
      // end with no file and nothing said about it.
      expect(dma.captures == capture.count(), "every requested frame was captured");
    }
  }
  if (!level_cycle.empty()) {
    // The whole point of linking level code into the level's own heap: unloading a level has to
    // give the memory back. Each pass through the cycle relinks the level's object files, so the
    // global heap standing still is the measurement.
    expect(heap_after_cycle == heap_before_cycle,
           "the global heap did not grow across the level cycle");
  }

  say("\nBOOT: KERNEL.CGO and GAME.CGO are loaded and the GOAL kernel dispatcher ran %d frames.\n",
      dispatch_frames);
  return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  bool synthetic = false;
  bool run_play = false;
  std::string data_dir;
  DmaCaptureRequest capture;
  std::vector<std::string> level_cycle;
  int level_cycle_frames = 600;
  int dispatch_frames = 0;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "--synthetic") {
      synthetic = true;
    } else if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg == "--play") {
      run_play = true;
    } else if (arg == "--verbose") {
      goal_dgo_set_verbose(1);
    } else if (arg == "--frames" && i + 1 < argc) {
      dispatch_frames = std::atoi(argv[++i]);
    } else if (arg == "--capture-dma" && i + 1 < argc) {
      capture.path = argv[++i];
    } else if (arg == "--capture-dma-frame" && i + 1 < argc) {
      capture.frame = std::atoi(argv[++i]);
    } else if (arg == "--capture-dma-dir" && i + 1 < argc) {
      capture.dir = argv[++i];
    } else if (arg == "--capture-dma-frames" && i + 1 < argc) {
      for (const auto& name : split_commas(argv[++i])) {
        capture.dir_frames.push_back(std::atoi(name.c_str()));
      }
    } else if (arg == "--capture-dma-min-payload" && i + 1 < argc) {
      capture.min_payload = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
    } else if (arg == "--capture-dma-count" && i + 1 < argc) {
      capture.over_count = std::atoi(argv[++i]);
    } else if (arg == "--dma-frame-report") {
      capture.per_frame_report = true;
    } else if (arg == "--levels" && i + 1 < argc) {
      level_cycle = split_commas(argv[++i]);
    } else if (arg == "--level-frames" && i + 1 < argc) {
      level_cycle_frames = std::atoi(argv[++i]);
    } else {
      say("unknown argument %s\n", arg.c_str());
      return 2;
    }
  }
  if (!capture.dir.empty() && capture.dir_frames.empty() && capture.over_count == 0) {
    say("--capture-dma-dir needs --capture-dma-frames or --capture-dma-count\n");
    return 2;
  }
  if (capture.over_count > 0 && capture.dir.empty()) {
    say("--capture-dma-count needs --capture-dma-dir\n");
    return 2;
  }
  if (data_dir.empty()) {
    const char* env = std::getenv("GOALPAD_JAK1_DATA_DIR");
    data_dir = env ? env : "";
  }

  if (!synthetic && data_dir.empty()) {
    say("SKIPPED: no Jak 1 data directory.\n"
        "This test loads the player's own extracted game data, which is not part of the\n"
        "repository. Set GOALPAD_JAK1_DATA_DIR (or pass --data-dir) to the directory that\n"
        "holds iso/ and fr3/ - what goal_src/jak1/game.gp calls $OUT - to run it.\n");
    return 0;
  }

  lg::set_stdout_level(lg::level::warn);
  lg::set_flush_level(lg::level::warn);
  lg::initialize();

  if (goal_kernel_core_initialize() != GOAL_KERNEL_CORE_OK) {
    say("FAIL: %s\n", goal_kernel_core_last_error());
    return 1;
  }
  clear_print();
  register_aot_objects();
  say("%d AOT translation units registered by object name\n", goal_aot_boot_file_count);

  const int result = synthetic ? run_synthetic()
                               : run_real_boot(data_dir, dispatch_frames, run_play, capture,
                                               level_cycle, level_cycle_frames);

  goal_aot_reset();
  goal_kernel_core_shutdown();
  return result;
}
