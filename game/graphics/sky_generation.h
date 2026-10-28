#pragma once

// Jak 1 physical-immersive sky generation context.
//
// The host publishes one immutable head rotation H (gameCameraFromCurrentHead, right/up/back,
// column-major, zero translation) for one reserved stereo frame. init-sky-regs composes its
// private sky rows as camera-rot * K * perspective with K = S * inverse(H) * S (S = diag(-1, 1, -1)
// converts the host basis to Jak's camera basis), so the sky is generated for the head instead of
// the game camera. Each emitted sky GIF range is receipted against the exact init that produced it.
// MetalSkyRenderer replays only fully receipted source geometry with the original packet and E * H.
//
// Ownership: receipts, init serials and H live in thread-local storage owned by the thread that
// called begin_frame (the GOAL dispatch / DMA consumption thread). A process-global atomic epoch
// lets any thread cancel: every check compares the owner's TLS epoch with the global epoch, so a
// cancelled context is dead immediately without touching another thread's TLS. Plain C++17.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace sky_generation {

using Mat4 = std::array<float, 16>;  // column-major: m[col * 4 + row]
using EngineFrameIdFn = std::uint64_t (*)();

struct FrameIdentity {
  std::uint64_t epoch = 0;
  std::uint64_t host_tick_id = 0;
  std::uint64_t engine_frame_id = 0;
};

enum class SourceKind : std::uint8_t { Tri = 1, Quad = 2 };

// Persisted next to sky_regs_vfs. epoch == 0 means the regs hold ordinary (untransformed) rows.
struct InitToken {
  std::uint64_t epoch = 0;
  std::uint64_t init_serial = 0;
  std::uint64_t engine_frame_id = 0;
  std::thread::id thread;
  bool transformed() const { return epoch != 0 && init_serial != 0; }
};

enum class GeneratedVerdict : std::uint8_t { Ordinary, AcceptTransformed, Reject };

struct GeneratedPayload {
  GeneratedVerdict verdict = GeneratedVerdict::Reject;
  bool terminator_valid = false;  // last 16 bytes are the canonical close-sky-buffer qword
  std::uint32_t geometry_bytes = 0;
};

// close-sky-buffer writes int128 0 then int32 #x8000 at the base: a GIF tag with EOP, NLOOP 0.
inline constexpr std::uint32_t kTerminatorBytes = 16;
inline constexpr std::uint8_t kCanonicalTerminator[kTerminatorBytes] = {
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
inline constexpr std::size_t kMaxPoisonRanges = 256;

namespace detail {

struct Receipt {
  std::uint32_t begin = 0;  // EE offset, half-open [begin, end)
  std::uint32_t end = 0;
  std::uint64_t init_serial = 0;
  std::uint64_t engine_frame_id = 0;
  SourceKind kind = SourceKind::Tri;
};

struct Range {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
};

struct ThreadContext {
  // Set by every begin_frame on this thread (even one that fails validation) and cleared only by
  // end_frame: an H-enabled frame was requested, so generated sky can never fall back to ordinary.
  bool requested = false;
  std::uint64_t epoch = 0;
  std::thread::id owner;
  Mat4 head = {};
  std::uint64_t host_tick_id = 0;
  std::uint64_t engine_frame_id = 0;
  EngineFrameIdFn engine_frame_id_fn = nullptr;
  std::uint64_t init_serial = 0;
  std::uint64_t current_init_serial = 0;
  std::vector<Receipt> receipts;
  // Closed-lifetime taint: EE ranges known to hold head-generated (transformed) bytes that no live
  // receipt owns: retired receipts (end/cancel/begin/engine advance) and stale/mismatched
  // emissions. Survives end/cancel/begin. Only a fresh producer emission of the exact interval
  // (ordinary, or receipted transformed) removes it. Offsets only; no EE pointer is held.
  std::vector<Range> poison;
  bool poison_overflow = false;  // sticky for the thread: every generated payload rejects
};

inline std::atomic<std::uint64_t> g_epoch_source{0};
inline std::atomic<std::uint64_t> g_active_epoch{0};
inline ThreadContext& thread_context() {
  // Function-local TLS coalesces its initializer across the Apple device link's translation units.
  static thread_local ThreadContext context;
  return context;
}

// Owner thread, uncancelled epoch. Engine identity is checked separately: one armed host tick may
// run several engine steps under the same immutable H.
inline bool live(const ThreadContext& ctx) {
  return ctx.epoch != 0 && ctx.owner == std::this_thread::get_id() &&
         g_active_epoch.load(std::memory_order_acquire) == ctx.epoch &&
         ctx.engine_frame_id_fn != nullptr;
}

inline bool engine_current(const ThreadContext& ctx) {
  return ctx.engine_frame_id_fn() == ctx.engine_frame_id;
}

inline void retire_receipts(ThreadContext& ctx);

// Generation side only: follow monotonic engine advancement. A new engine step drops the prior
// step's receipts and current init; regression kills generation for this context.
inline bool sync_engine(ThreadContext& ctx) {
  const std::uint64_t now = ctx.engine_frame_id_fn();
  if (now < ctx.engine_frame_id) {
    return false;
  }
  if (now > ctx.engine_frame_id) {
    ctx.engine_frame_id = now;
    retire_receipts(ctx);
    ctx.current_init_serial = 0;
  }
  return true;
}

inline bool overlaps(std::uint32_t a0, std::uint32_t a1, std::uint32_t b0, std::uint32_t b1);
inline void add_poison(ThreadContext& ctx, std::uint32_t begin, std::uint32_t end);

// Receipted bytes stay head-generated after their context dies.
inline void retire_receipts(ThreadContext& ctx) {
  for (const auto& r : ctx.receipts) {
    add_poison(ctx, r.begin, r.end);
  }
  ctx.receipts.clear();
}

// A fresh emission rewrote exactly [begin, end): those bytes are no longer stale.
inline void subtract_poison(ThreadContext& ctx, std::uint32_t begin, std::uint32_t end) {
  std::vector<Range> kept;
  for (const auto& p : ctx.poison) {
    if (!overlaps(p.begin, p.end, begin, end)) {
      kept.push_back(p);
      continue;
    }
    if (p.begin < begin) {
      kept.push_back(Range{p.begin, begin});
    }
    if (p.end > end) {
      kept.push_back(Range{end, p.end});
    }
  }
  if (kept.size() > kMaxPoisonRanges) {
    ctx.poison_overflow = true;
  }
  ctx.poison.swap(kept);
}

// Drops live receipts overlapping [begin, end); their bytes become taint.
inline bool retire_overlapping(ThreadContext& ctx, std::uint32_t begin, std::uint32_t end) {
  bool any = false;
  std::vector<Receipt> kept;
  for (const auto& r : ctx.receipts) {
    if (overlaps(r.begin, r.end, begin, end)) {
      add_poison(ctx, r.begin, r.end);
      any = true;
    } else {
      kept.push_back(r);
    }
  }
  ctx.receipts.swap(kept);
  return any;
}

inline void clear_context(ThreadContext& ctx) {
  retire_receipts(ctx);
  ctx.epoch = 0;
  ctx.owner = std::thread::id();
  ctx.head = {};
  ctx.host_tick_id = 0;
  ctx.engine_frame_id = 0;
  ctx.engine_frame_id_fn = nullptr;
  ctx.init_serial = 0;
  ctx.current_init_serial = 0;
  ctx.receipts.clear();
}

inline void add_poison(ThreadContext& ctx, std::uint32_t begin, std::uint32_t end) {
  if (end < begin) {
    ctx.poison_overflow = true;
    return;
  }
  if (end == begin) {
    return;
  }
  // coalesce overlapping/adjacent ranges to stay bounded
  for (auto it = ctx.poison.begin(); it != ctx.poison.end();) {
    if (it->begin <= end && begin <= it->end) {
      begin = std::min(begin, it->begin);
      end = std::max(end, it->end);
      it = ctx.poison.erase(it);
    } else {
      ++it;
    }
  }
  if (ctx.poison.size() >= kMaxPoisonRanges) {
    ctx.poison_overflow = true;
    return;
  }
  ctx.poison.push_back(Range{begin, end});
}

inline bool overlaps(std::uint32_t a0, std::uint32_t a1, std::uint32_t b0, std::uint32_t b1) {
  return a0 < b1 && b0 < a1;
}

inline bool valid_head_rotation(const Mat4& h) {
  for (float v : h) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  // zero translation, affine last row
  if (h[12] != 0.f || h[13] != 0.f || h[14] != 0.f || h[3] != 0.f || h[7] != 0.f ||
      h[11] != 0.f || h[15] != 1.f) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      double dot = 0.0;
      for (int k = 0; k < 3; k++) {
        dot += double(h[i * 4 + k]) * double(h[j * 4 + k]);
      }
      if (std::fabs(dot - (i == j ? 1.0 : 0.0)) > 1e-3) {
        return false;
      }
    }
  }
  const double det = double(h[0]) * (double(h[5]) * h[10] - double(h[9]) * h[6]) -
                     double(h[4]) * (double(h[1]) * h[10] - double(h[9]) * h[2]) +
                     double(h[8]) * (double(h[1]) * h[6] - double(h[5]) * h[2]);
  return det > 0.0;
}

}  // namespace detail

// Column-major C = A * B.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
  Mat4 out = {};
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++) {
        sum += a[k * 4 + row] * b[col * 4 + k];
      }
      out[col * 4 + row] = sum;
    }
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Host lifecycle (root integration)
// ---------------------------------------------------------------------------------------------

inline bool begin_frame(const Mat4& game_camera_from_current_head,
                        std::uint64_t host_tick_id,
                        std::uint64_t engine_frame_id,
                        EngineFrameIdFn engine_frame_id_fn,
                        FrameIdentity* out_identity = nullptr) {
  auto& ctx = detail::thread_context();
  if (out_identity) {
    *out_identity = {};
  }
  if (detail::live(ctx)) {
    return false;  // previous frame was never ended on this thread
  }
  detail::clear_context(ctx);  // retires any leftover receipts into taint; taint is kept
  ctx.requested = true;
  if (!engine_frame_id_fn || engine_frame_id_fn() != engine_frame_id ||
      !detail::valid_head_rotation(game_camera_from_current_head)) {
    return false;
  }
  const std::uint64_t epoch =
      detail::g_epoch_source.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::uint64_t expected = 0;
  if (!detail::g_active_epoch.compare_exchange_strong(expected, epoch,
                                                      std::memory_order_acq_rel)) {
    return false;  // another thread owns an uncancelled context
  }
  ctx.epoch = epoch;
  ctx.owner = std::this_thread::get_id();
  ctx.head = game_camera_from_current_head;
  ctx.host_tick_id = host_tick_id;
  ctx.engine_frame_id = engine_frame_id;
  ctx.engine_frame_id_fn = engine_frame_id_fn;
  if (out_identity) {
    *out_identity = {epoch, host_tick_id, engine_frame_id};
  }
  return true;
}

inline bool end_frame() {
  auto& ctx = detail::thread_context();
  bool ended = false;
  if (ctx.epoch != 0 && ctx.owner == std::this_thread::get_id()) {
    std::uint64_t expected = ctx.epoch;
    ended = detail::g_active_epoch.compare_exchange_strong(expected, 0,
                                                           std::memory_order_acq_rel);
  }
  detail::clear_context(ctx);
  ctx.requested = false;
  return ended;
}

inline void cancel_frame() {
  detail::g_active_epoch.store(0, std::memory_order_release);
}

// True between begin_frame and end_frame on this thread, whether or not the context is usable.
inline bool frame_requested() {
  return detail::thread_context().requested;
}

inline bool frame_active(FrameIdentity* out_identity = nullptr) {
  const auto& ctx = detail::thread_context();
  if (!detail::live(ctx) || !detail::engine_current(ctx)) {
    return false;
  }
  if (out_identity) {
    *out_identity = {ctx.epoch, ctx.host_tick_id, ctx.engine_frame_id};
  }
  return true;
}

inline bool current_frame_head_rotation(Mat4* out, FrameIdentity* out_identity = nullptr) {
  if (!out || !frame_active(out_identity)) {
    return false;
  }
  *out = detail::thread_context().head;
  return true;
}

// ---------------------------------------------------------------------------------------------
// Generation (sky_tng.cpp)
// ---------------------------------------------------------------------------------------------

// camera_rot_rows / perspective_rows are the GOAL matrices as stored (4 row qwords, row-vector
// convention, camera-temp = camera-rot * perspective, cam-update.gc). On a transformed token,
// out_rows = camera-rot * Krow * perspective where Krow = (S * H^-1 * S)^T = S * H * S for a rigid
// H: v_row * Krow == (K * v_col)^T, so the generated camera is Rgen = S * H^-1 * S * Rgame and the
// camera-rot translation row is carried through the same K (Ggen = H^-1 * Gorig).
// Returns an ordinary token (and leaves out_rows untouched) if no live context or any result is
// non-finite. Every call invalidates the previous init token.
inline InitToken consume_init(const float camera_rot_rows[16],
                              const float perspective_rows[16],
                              float out_rows[16]) {
  auto& ctx = detail::thread_context();
  ctx.current_init_serial = 0;
  if (!detail::live(ctx) || !detail::sync_engine(ctx)) {
    return {};
  }
  float k_row[16];  // row-major [r * 4 + c]
  const float s[4] = {-1.f, 1.f, -1.f, 1.f};
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      k_row[r * 4 + c] = s[r] * s[c] * ctx.head[c * 4 + r];
    }
  }
  float rk[16];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++) {
        sum += camera_rot_rows[r * 4 + k] * k_row[k * 4 + c];
      }
      rk[r * 4 + c] = sum;
    }
  }
  float result[16];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float sum = 0.f;
      for (int k = 0; k < 4; k++) {
        sum += rk[r * 4 + k] * perspective_rows[k * 4 + c];
      }
      if (!std::isfinite(sum)) {
        return {};
      }
      result[r * 4 + c] = sum;
    }
  }
  std::memcpy(out_rows, result, sizeof(result));
  ctx.current_init_serial = ++ctx.init_serial;
  return InitToken{ctx.epoch, ctx.current_init_serial, ctx.engine_frame_id,
                   std::this_thread::get_id()};
}

// Something other than init-sky-regs rewrote the persisted sky rows (init-boundary-regs).
inline void invalidate_init() {
  detail::thread_context().current_init_serial = 0;
}

// Producer hook: [ee_begin, ee_end) is the dma-buffer base before/after draw-large-polygon.
// Returns true when the range is empty or was receipted/ordinary; false when it was poisoned.
inline bool record_emit(const InitToken& token,
                        SourceKind kind,
                        std::uint32_t ee_begin,
                        std::uint32_t ee_end) {
  auto& ctx = detail::thread_context();
  if (ee_end == ee_begin) {
    return true;  // fully clipped polygon
  }
  if (ee_end < ee_begin) {
    ctx.poison_overflow = true;
    return false;
  }
  if (!token.transformed()) {
    // Fresh ordinary bytes own exactly [ee_begin, ee_end). Any overlapping transformed receipt
    // is invalidated (its untouched remainder stays tainted). A live consumer still rejects:
    // these bytes have no receipt.
    const bool rewrote_receipt = detail::retire_overlapping(ctx, ee_begin, ee_end);
    detail::subtract_poison(ctx, ee_begin, ee_end);
    return !rewrote_receipt;
  }
  const bool matches = detail::live(ctx) && detail::sync_engine(ctx) &&
                       token.epoch == ctx.epoch && token.engine_frame_id == ctx.engine_frame_id &&
                       token.thread == std::this_thread::get_id() &&
                       token.init_serial == ctx.current_init_serial &&
                       (kind == SourceKind::Tri || kind == SourceKind::Quad);
  if (matches) {
    bool clear = true;
    for (const auto& r : ctx.receipts) {
      clear = clear && !detail::overlaps(r.begin, r.end, ee_begin, ee_end);
    }
    if (clear) {
      detail::subtract_poison(ctx, ee_begin, ee_end);
      ctx.receipts.push_back(
          detail::Receipt{ee_begin, ee_end, token.init_serial, token.engine_frame_id, kind});
      return true;
    }
  }
  detail::retire_overlapping(ctx, ee_begin, ee_end);
  detail::add_poison(ctx, ee_begin, ee_end);
  return false;
}

// ---------------------------------------------------------------------------------------------
// Consumption (metal_sky_renderer.mm)
// ---------------------------------------------------------------------------------------------

inline bool canonical_terminator(const std::uint8_t* tail16) {
  return std::memcmp(tail16, kCanonicalTerminator, kTerminatorBytes) == 0;
}

// EE offset of a DMA payload, only when the renderer is walking the live EE image itself:
// chain_base must equal ee_memory and [data, data + size) must lie inside [ee, ee + chain_size).
// A copied chain (chain_base != ee_memory) or any pointer outside that span yields false.
inline bool live_ee_offset(const std::uint8_t* data,
                           std::uint32_t size,
                           const std::uint8_t* ee_memory,
                           const std::uint8_t* chain_base,
                           std::size_t chain_size,
                           std::uint32_t* out_offset) {
  if (!data || !ee_memory || chain_base != ee_memory || !out_offset) {
    return false;
  }
  const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(data);
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(ee_memory);
  if (p < base) {
    return false;
  }
  const std::uintptr_t offset = p - base;
  if (offset > chain_size || std::uint64_t(size) > std::uint64_t(chain_size - offset) ||
      std::uint64_t(offset) + size > UINT32_MAX) {
    return false;
  }
  *out_offset = static_cast<std::uint32_t>(offset);
  return true;
}

// Classifies one sky-tng CNT/DIRECT payload.
// h_requested: the renderer's per-view expected/requested H mode (MetalSharedRenderState::
//   sky_source_expected), carried independently of whether a usable pair exists.
// sky_transform_epoch: epoch the paired sky transform was derived for (0 when none). Any nonzero
//   value, h_requested, or this thread's frame_requested() forbids Ordinary.
// ee_offset_valid: live_ee_offset() result; a copied chain cannot be matched to receipts.
inline GeneratedPayload classify_generated_payload(const std::uint8_t* data,
                                                   std::uint32_t size,
                                                   bool ee_offset_valid,
                                                   std::uint32_t ee_begin,
                                                   bool h_requested,
                                                   std::uint64_t sky_transform_epoch) {
  const auto& ctx = detail::thread_context();
  GeneratedPayload out;
  out.terminator_valid = data && size >= kTerminatorBytes && size % 16 == 0 &&
                         canonical_terminator(data + size - kTerminatorBytes);
  const bool in_range = ee_offset_valid && std::uint64_t(ee_begin) + size <= UINT32_MAX;
  const std::uint32_t ee_end = in_range ? ee_begin + size : 0;

  // A validated canonical terminator is checked by content; taint applies to the rest.
  bool tainted = ctx.poison_overflow;
  if (in_range) {
    const std::uint32_t checked_end = out.terminator_valid ? ee_end - kTerminatorBytes : ee_end;
    for (const auto& p : ctx.poison) {
      tainted = tainted || detail::overlaps(p.begin, p.end, ee_begin, checked_end);
    }
  }
  if (!detail::live(ctx)) {
    bool receipted = false;
    if (in_range) {
      for (const auto& r : ctx.receipts) {
        receipted = receipted || detail::overlaps(r.begin, r.end, ee_begin, ee_end);
      }
    }
    // No live context: the ordinary source path only when no H-enabled frame was requested on
    // this thread and these bytes were not generated with transformed rows.
    const bool expected = h_requested || ctx.requested || sky_transform_epoch != 0;
    out.verdict = expected || tainted || receipted ? GeneratedVerdict::Reject
                                                   : GeneratedVerdict::Ordinary;
    return out;
  }
  if (!h_requested || tainted || !in_range || !out.terminator_valid ||
      sky_transform_epoch != ctx.epoch || !detail::engine_current(ctx)) {
    out.verdict = GeneratedVerdict::Reject;
    return out;
  }
  const std::uint32_t geometry_end = ee_end - kTerminatorBytes;
  std::vector<const detail::Receipt*> hits;
  for (const auto& r : ctx.receipts) {
    if (detail::overlaps(r.begin, r.end, ee_begin, ee_end)) {
      hits.push_back(&r);
    }
  }
  std::sort(hits.begin(), hits.end(),
            [](const detail::Receipt* a, const detail::Receipt* b) { return a->begin < b->begin; });
  std::uint32_t cursor = ee_begin;
  for (const auto* r : hits) {
    if (r->begin != cursor || r->end > geometry_end ||
        r->engine_frame_id != ctx.engine_frame_id) {
      out.verdict = GeneratedVerdict::Reject;  // gap, or receipt reaching into the terminator
      return out;
    }
    cursor = r->end;
  }
  if (cursor != geometry_end) {
    out.verdict = GeneratedVerdict::Reject;  // unreceipted geometry bytes
    return out;
  }
  out.geometry_bytes = geometry_end - ee_begin;
  out.verdict = GeneratedVerdict::AcceptTransformed;  // includes terminator-only payloads
  return out;
}

// Compatibility form (no explicit renderer request): a positive pair epoch is itself a request.
inline GeneratedPayload classify_generated_payload(const std::uint8_t* data,
                                                   std::uint32_t size,
                                                   bool ee_offset_valid,
                                                   std::uint32_t ee_begin,
                                                   std::uint64_t sky_transform_epoch) {
  return classify_generated_payload(data, size, ee_offset_valid, ee_begin,
                                    sky_transform_epoch != 0, sky_transform_epoch);
}

}  // namespace sky_generation
