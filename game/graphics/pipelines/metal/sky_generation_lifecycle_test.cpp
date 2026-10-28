// Standalone CPU check of game/graphics/sky_generation.h (no OpenGOAL, no Metal).
#include "game/graphics/sky_generation.h"

#include <cmath>
#include <cstdio>
#include <thread>

using namespace sky_generation;

static int g_fail = 0;
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
      g_fail++;                                               \
    }                                                         \
  } while (0)

static std::uint64_t g_engine = 7;
static std::uint64_t engine_id() { return g_engine; }  // root: goalpad::frames_run() + 1

static Mat4 yaw(float a) {  // column-major rotation about +Y (RUB)
  Mat4 m = {};
  m[0] = std::cos(a); m[2] = -std::sin(a);
  m[5] = 1.f;
  m[8] = std::sin(a); m[10] = std::cos(a);
  m[15] = 1.f;
  return m;
}

static std::uint8_t buf[256];
static void payload(std::uint32_t geometry) {
  std::memset(buf, 0x55, geometry);
  std::memcpy(buf + geometry, kCanonicalTerminator, 16);
}
static GeneratedVerdict cls(std::uint32_t size, std::uint32_t ee, bool req, std::uint64_t epoch,
                            bool ee_ok = true) {
  return classify_generated_payload(buf, size, ee_ok, ee, req, epoch).verdict;
}
#define REJECT GeneratedVerdict::Reject
#define ORD GeneratedVerdict::Ordinary
#define ACCEPT GeneratedVerdict::AcceptTransformed

int main() {
  float rot[16] = {}, persp[16] = {}, rows[16];
  for (int i = 0; i < 16; i++) {
    rot[i] = float((i * 7) % 5) - 2.f;
    persp[i] = float((i * 3) % 7) - 3.f;
  }
  float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  // --- live_ee_offset: only the live EE image, bounded by the chain span.
  {
    static std::uint8_t ee[4096], copy[4096];
    std::uint32_t off = 0;
    CHECK(live_ee_offset(ee + 1024, 80, ee, ee, sizeof(ee), &off) && off == 1024);
    CHECK(!live_ee_offset(copy + 1024, 80, ee, copy, sizeof(copy), &off));  // copied chain
    CHECK(!live_ee_offset(copy + 16, 80, ee, ee, sizeof(ee), &off));        // unrelated pointer
    CHECK(!live_ee_offset(ee + 4090, 80, ee, ee, sizeof(ee), &off));        // past span
    CHECK(!live_ee_offset(ee, 16, nullptr, nullptr, 0, &off));
  }

  // --- H disabled (no begin): ordinary source behaviour preserved.
  CHECK(!consume_init(rot, persp, rows).transformed());
  payload(64);
  CHECK(cls(80, 1000, false, 0) == ORD);
  CHECK(cls(80, 1000, false, 0, false) == ORD);  // copied chain, H disabled: ordinary
  // ...but a renderer-side request or a leftover pair epoch forbids ordinary.
  CHECK(cls(80, 1000, true, 0) == REJECT);
  CHECK(cls(80, 1000, false, 99) == REJECT);

  // --- Begin failures still count as requested (root aborts the frame; fail closed regardless).
  Mat4 bad = yaw(0.3f);
  bad[12] = 1.f;
  CHECK(!begin_frame(bad, 1, 7, &engine_id));
  CHECK(frame_requested() && !frame_active());
  CHECK(cls(80, 1000, false, 0) == REJECT);
  CHECK(!begin_frame(yaw(0.3f), 1, 8, &engine_id));  // engine mismatch at begin
  CHECK(!begin_frame(yaw(0.3f), 1, 7, nullptr));
  end_frame();
  CHECK(!frame_requested() && cls(80, 1000, false, 0) == ORD);

  // --- Identity H reproduces camera-temp = camera-rot * perspective.
  Mat4 id = {};
  id[0] = id[5] = id[10] = id[15] = 1.f;
  FrameIdentity fid;
  CHECK(begin_frame(id, 1, 7, &engine_id, &fid) && fid.epoch != 0);
  CHECK(!begin_frame(id, 1, 7, &engine_id));  // already owned
  auto t0 = consume_init(rot, persp, rows);
  CHECK(t0.transformed() && t0.engine_frame_id == 7);
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 4; c++) {
      float s = 0.f;
      for (int k = 0; k < 4; k++) s += rot[r * 4 + k] * persp[k * 4 + c];
      CHECK(std::fabs(s - rows[r * 4 + c]) < 1e-4f);
    }
  CHECK(end_frame());

  // --- Yaw H: v_row * Krow == (S H^-1 S v_col)^T.
  const Mat4 h = yaw(0.7f);
  CHECK(begin_frame(h, 2, 7, &engine_id, &fid));
  Mat4 hout;
  FrameIdentity hid;
  CHECK(current_frame_head_rotation(&hout, &hid) && hout == h && hid.epoch == fid.epoch);
  auto t1 = consume_init(ident, ident, rows);
  CHECK(t1.transformed());
  const float v[4] = {0.2f, -0.5f, 0.8f, 0.f}, s[4] = {-1, 1, -1, 1};
  for (int c = 0; c < 4; c++) {
    float row = 0.f, col = 0.f;
    for (int r = 0; r < 4; r++) row += v[r] * rows[r * 4 + c];
    for (int k = 0; k < 4; k++) col += s[c] * h[c * 4 + k] * s[k] * v[k];  // (H^T)[c][k]
    CHECK(std::fabs(row - col) < 1e-5f);
  }

  // --- Receipts: tri + quad contiguous, overlap poisons.
  CHECK(record_emit(t1, SourceKind::Tri, 1000, 1032));
  CHECK(record_emit(t1, SourceKind::Quad, 1032, 1064));
  CHECK(record_emit(t1, SourceKind::Tri, 1064, 1064));    // fully clipped
  CHECK(!record_emit(t1, SourceKind::Tri, 1040, 1050));   // overlap -> poison
  payload(64);
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
  CHECK(end_frame());

  CHECK(begin_frame(h, 3, 7, &engine_id, &fid));  // taint kept; fresh receipts below clear it
  auto t2 = consume_init(ident, ident, rows);
  CHECK(record_emit(t2, SourceKind::Tri, 1000, 1032));
  // multi-init in one engine step (sky-tng.gc:724 then :791): both inits' ranges are valid
  auto t2b = consume_init(ident, ident, rows);
  CHECK(!record_emit(t2, SourceKind::Tri, 1100, 1132));  // superseded token cannot emit
  CHECK(record_emit(t2b, SourceKind::Quad, 1032, 1064));
  auto ok = classify_generated_payload(buf, 80, true, 1000, true, fid.epoch);
  CHECK(ok.verdict == ACCEPT && ok.geometry_bytes == 64 && ok.terminator_valid);
  CHECK(cls(80, 1000, true, fid.epoch) == ACCEPT);         // second eye
  CHECK(cls(80, 1000, false, fid.epoch) == REJECT);        // view not expecting H
  CHECK(cls(80, 1000, true, fid.epoch + 1) == REJECT);     // pair from another epoch
  CHECK(cls(80, 1000, true, 0) == REJECT);                 // pair invalid
  CHECK(cls(80, 1000, true, fid.epoch, false) == REJECT);  // copied chain
  payload(96);
  CHECK(cls(112, 1000, true, fid.epoch) == REJECT);  // 32 unreceipted bytes
  payload(64);
  buf[64 + 15] = 1;  // terminator checked in full 16 bytes, not just first word
  auto bt = classify_generated_payload(buf, 80, true, 1000, true, fid.epoch);
  CHECK(bt.verdict == REJECT && !bt.terminator_valid);
  payload(0);
  CHECK(cls(16, 4000, true, fid.epoch) == ACCEPT);  // terminator-only
  CHECK(cls(16, 1032, true, fid.epoch) == REJECT);  // terminator over a receipt
  // ordinary init in live context: no receipt -> reject
  auto t_ord = InitToken{};
  CHECK(record_emit(t_ord, SourceKind::Tri, 5000, 5032));
  payload(32);
  CHECK(cls(48, 5000, true, fid.epoch) == REJECT);
  // init-boundary-regs invalidation: current token can no longer emit
  auto t3 = consume_init(ident, ident, rows);
  invalidate_init();
  CHECK(!record_emit(t3, SourceKind::Tri, 3100, 3132));
  // ordinary rewrite of receipted bytes in same engine step -> reject
  CHECK(!record_emit(t_ord, SourceKind::Tri, 1000, 1032));
  payload(64);
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
  CHECK(end_frame());

  // --- Wrong thread sees no live context and cannot emit/end.
  CHECK(begin_frame(h, 4, 7, &engine_id, &fid));
  auto t4 = consume_init(ident, ident, rows);
  CHECK(record_emit(t4, SourceKind::Tri, 1000, 1064));
  std::thread other([&] {
    CHECK(!frame_active() && !frame_requested());
    Mat4 out;
    CHECK(!current_frame_head_rotation(&out));
    CHECK(!consume_init(ident, ident, rows).transformed());
    CHECK(!record_emit(t4, SourceKind::Tri, 2000, 2032));
    CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
    CHECK(!end_frame());
  });
  other.join();
  CHECK(frame_active());
  CHECK(cls(80, 1000, true, fid.epoch) == ACCEPT);

  // --- Engine rebind within one host tick.
  g_engine = 8;
  CHECK(!frame_active());
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);         // new engine, no new init
  CHECK(!record_emit(t4, SourceKind::Tri, 7000, 7032));    // stale token after advance
  auto t8 = consume_init(ident, ident, rows);
  CHECK(t8.transformed() && t8.engine_frame_id == 8 && frame_active());
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);         // engine-7 receipts dropped
  CHECK(record_emit(t8, SourceKind::Tri, 1000, 1064));
  CHECK(cls(80, 1000, true, fid.epoch) == ACCEPT);
  g_engine = 7;                                            // backward callback
  CHECK(!consume_init(ident, ident, rows).transformed());
  CHECK(!record_emit(t8, SourceKind::Tri, 8000, 8032));
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
  g_engine = 8;

  // --- Cross-thread cancel: dead immediately, still requested -> never ordinary.
  std::thread canceller([] { cancel_frame(); });
  canceller.join();
  CHECK(!frame_active() && frame_requested());
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
  CHECK(cls(80, 9000, false, 0) == REJECT);                // missing receipt while requested
  CHECK(!record_emit(t8, SourceKind::Tri, 6000, 6032));    // stale transformed -> poison
  CHECK(!end_frame());                                     // epoch already cancelled
  CHECK(!frame_requested());
  // After end: expected epoch still positive -> reject; compat 5-arg form likewise.
  CHECK(cls(80, 1000, true, fid.epoch) == REJECT);
  CHECK(cls(80, 1000, false, fid.epoch) == REJECT);
  CHECK(classify_generated_payload(buf, 80, true, 1000, fid.epoch).verdict == REJECT);
  // Closed-lifetime taint: retired receipts never become ordinary in windowed (epoch 0) mode.
  payload(64);
  CHECK(cls(80, 1000, false, 0) == REJECT);
  CHECK(classify_generated_payload(buf, 80, true, 1000, 0).verdict == REJECT);
  payload(16);
  CHECK(cls(32, 6000, false, 0) == REJECT);                // stale emission taint
  CHECK(cls(32, 9000, false, 0) == ORD);                   // H disabled, unrelated bytes
  // Taint survives begin/end.
  CHECK(begin_frame(h, 5, 8, &engine_id, &fid));
  CHECK(end_frame());
  CHECK(cls(32, 6000, false, 0) == REJECT);
  // Fresh ordinary regeneration of the exact interval (windowed: no context) clears only it.
  CHECK(!consume_init(ident, ident, rows).transformed());
  CHECK(record_emit(InitToken{}, SourceKind::Tri, 6000, 6016));
  CHECK(cls(32, 6000, false, 0) == ORD);
  payload(64);
  CHECK(record_emit(InitToken{}, SourceKind::Tri, 1000, 1032));
  CHECK(cls(80, 1000, false, 0) == REJECT);                // [1032,1064) still head-generated
  CHECK(record_emit(InitToken{}, SourceKind::Quad, 1032, 1064));
  CHECK(cls(80, 1000, false, 0) == ORD);

  // Ordinary rewrite of a live receipt invalidates the whole receipt; remainder stays tainted.
  CHECK(begin_frame(h, 6, 8, &engine_id, &fid));
  auto t9 = consume_init(ident, ident, rows);
  CHECK(record_emit(t9, SourceKind::Tri, 2000, 2064));
  CHECK(!record_emit(InitToken{}, SourceKind::Tri, 2000, 2032));
  CHECK(end_frame());
  payload(32);
  CHECK(cls(48, 2000, false, 0) == ORD);                   // freshly ordinary bytes
  payload(64);
  CHECK(cls(80, 2000, false, 0) == REJECT);                // [2032,2064) remainder tainted
  // Terminator over a stale range is judged by content, not taint.
  payload(0);
  CHECK(cls(16, 2032, false, 0) == ORD);

  std::printf(g_fail ? "SKY GENERATION LIFECYCLE FAIL %d\n" : "SKY GENERATION LIFECYCLE OK\n",
              g_fail);
  return g_fail ? 1 : 0;
}
