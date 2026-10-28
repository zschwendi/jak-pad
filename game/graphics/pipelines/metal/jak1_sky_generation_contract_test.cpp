/*! CPU regression: actual production translated Jak 1 sky generation + native receipt contract.
 * Standalone binary (no kernel link): defines the link stubs via the fixture.
 * These checks SPECIFY required native behavior; do not weaken them to obtain a pass. */
#define JAK1_SKY_FIXTURE_LINK_STUBS
#include "game/graphics/pipelines/metal/jak1_sky_generation_fixture.h"

#include <cmath>
#include <cstdio>

#include "game/graphics/sky_generation.h"

namespace {
int g_failures = 0;
void check(bool c, const std::string& m) {
  std::printf("[%s] %s\n", c ? "PASS" : "FAIL", m.c_str());
  g_failures += !c;
}

std::uint64_t g_engine_frame = 7;
std::uint64_t engine_frame() {
  return g_engine_frame;
}

constexpr float kPi = 3.14159265358979f;
sky_generation::Mat4 yaw(float deg) {
  const float r = deg * kPi / 180.f, c = std::cos(r), s = std::sin(r);
  return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};  // column-major Y rotation
}
sky_generation::Mat4 pitch(float deg) {
  const float r = deg * kPi / 180.f, c = std::cos(r), s = std::sin(r);
  return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};  // column-major X rotation
}
sky_generation::Mat4 identity() {
  return yaw(0.f);
}

// Appends the canonical close-sky-buffer terminator directly after the emitted geometry, in EE
// memory, so the classified payload is the actual EE bytes at the receipt addresses.
std::uint32_t terminate(const jak1_sky_fixture::Generated& g) {
  std::memcpy(g_ee_main_mem + g.end, sky_generation::kCanonicalTerminator,
              sky_generation::kTerminatorBytes);
  return g.end - g.begin + sky_generation::kTerminatorBytes;
}

// h_requested defaults to ep != 0 (ordinary epoch 0 -> false); pass explicitly to probe.
sky_generation::GeneratedVerdict classify(const jak1_sky_fixture::Generated& g, std::uint64_t ep,
                                          std::uint32_t begin_skew = 0, int h_requested = -1) {
  const std::uint32_t size = terminate(g) - begin_skew;
  const bool requested = h_requested < 0 ? ep != 0 : h_requested != 0;
  return sky_generation::classify_generated_payload(g_ee_main_mem + g.begin + begin_skew, size,
                                                    true /*ee valid*/, g.begin + begin_skew,
                                                    requested, ep)
      .verdict;
}

jak1_sky_fixture::Generated generate() {
  jak1_sky_fixture::init_registers();
  return jak1_sky_fixture::emit_bipyramid();
}

void rotated_case(const std::string& name, const sky_generation::Mat4& h,
                  const jak1_sky_fixture::Generated& ordinary,
                  const std::vector<u8>& camera_before) {
  using sky_generation::GeneratedVerdict;
  sky_generation::FrameIdentity id{};
  check(sky_generation::begin_frame(h, 2, g_engine_frame, &engine_frame, &id) && id.epoch,
        name + " begin_frame");
  const auto rot = generate();
  check(rot.ok && rot.bytes != ordinary.bytes, name + " generation differs from ordinary");
  check(classify(rot, id.epoch) == GeneratedVerdict::AcceptTransformed, name + " receipts accept");
  check(classify(rot, id.epoch + 1) == GeneratedVerdict::Reject, name + " stale epoch rejects");
  check(classify(rot, id.epoch, 16) == GeneratedVerdict::Reject, name + " partial payload rejects");
  check(jak1_sky_fixture::camera_snapshot() == camera_before, name + " camera bytes unchanged");

  // Engine callback advances: old receipts reject; a fresh init+emit under the new engine frame
  // (monotonic rebind) accepts; a further advance without init rejects.
  g_engine_frame++;
  check(classify(rot, id.epoch) == GeneratedVerdict::Reject,
        name + " old receipt rejects after engine frame advance");
  const auto rebound = generate();
  check(rebound.ok && rebound.bytes == rot.bytes, name + " rebound generation same bytes");
  check(classify(rebound, id.epoch) == GeneratedVerdict::AcceptTransformed,
        name + " new init after engine advance accepts");
  g_engine_frame++;
  check(classify(rebound, id.epoch) == GeneratedVerdict::Reject,
        name + " further engine advance without init rejects");
  const auto fresh = generate();
  sky_generation::end_frame();
  check(classify(fresh, id.epoch) == GeneratedVerdict::Reject,
        name + " after end_frame actual transformed source rejects");
  check(classify(fresh, 0, 0, 0) == GeneratedVerdict::Reject,
        name + " after end_frame transformed bytes never classify as ordinary");
}
}  // namespace

int main() {
  using sky_generation::GeneratedVerdict;
  jak1_sky_fixture::setup_memory();
  jak1_sky_fixture::setup_caches();
  jak1_sky_fixture::configure_camera(jak1_sky_fixture::packet());
  const auto camera_before = jak1_sky_fixture::camera_snapshot();

  // 1. Ordinary (no context) generation: baseline bytes.
  const auto ordinary = generate();
  check(ordinary.ok && ordinary.ranges.size() >= 4, "ordinary bipyramid emits actual tri GIF");
  check(classify(ordinary, 0) == GeneratedVerdict::Ordinary, "no context -> Ordinary");
  const auto ordinary_quad = (jak1_sky_fixture::init_registers(), jak1_sky_fixture::emit_quad(jak1_sky_fixture::kOutput + 0x4000));
  check(ordinary_quad.ok && !ordinary_quad.ranges.empty(), "ordinary quad emits actual GIF");

  // 1b. Active H = I: transformed path must be byte-identical to ordinary (tri and quad).
  sky_generation::FrameIdentity id{};
  check(sky_generation::begin_frame(identity(), 1, g_engine_frame, &engine_frame, &id) &&
            id.epoch != 0,
        "begin_frame(H=I)");
  // Live context with NO emission yet: a payload at an unreceipted address must not accept.
  check(classify(ordinary, id.epoch) == GeneratedVerdict::Reject,
        "missing receipt while live rejects");
  const auto ident = generate();
  check(ident.ok && ident.bytes == ordinary.bytes, "H=I generated GIF == ordinary bytes");
  check(classify(ident, id.epoch) == GeneratedVerdict::AcceptTransformed,
        "H=I receipts accept at actual EE addresses");
  const auto ident_quad = (jak1_sky_fixture::init_registers(), jak1_sky_fixture::emit_quad(jak1_sky_fixture::kOutput + 0x4000));
  check(ident_quad.ok && ident_quad.bytes == ordinary_quad.bytes, "H=I quad == ordinary quad");
  check(classify(ident_quad, id.epoch) == GeneratedVerdict::AcceptTransformed,
        "H=I quad receipt accepts");
  check(jak1_sky_fixture::camera_snapshot() == camera_before, "1024 camera bytes unchanged (H=I)");
  // Adversarial: a quad emitted over the receipted tri range (0x9000) must poison -> Reject.
  const auto overlap = (jak1_sky_fixture::init_registers(), jak1_sky_fixture::emit_quad());
  check(overlap.ok && classify(overlap, id.epoch) == GeneratedVerdict::Reject,
        "overlapping emission over receipted range rejects");
  sky_generation::end_frame();

  // 2. Rotated heads.
  rotated_case("yaw90", yaw(90.f), ordinary, camera_before);
  rotated_case("yaw180", yaw(180.f), ordinary, camera_before);
  rotated_case("yaw270", yaw(270.f), ordinary, camera_before);
  rotated_case("pitch+60", pitch(60.f), ordinary, camera_before);
  rotated_case("pitch-60", pitch(-60.f), ordinary, camera_before);

  // 4b. Cancel mid-frame: actual transformed source rejects (not Ordinary).
  check(sky_generation::begin_frame(yaw(90.f), 3, g_engine_frame, &engine_frame, &id), "begin");
  const auto pre_cancel = generate();
  sky_generation::cancel_frame();
  check(classify(pre_cancel, id.epoch) == GeneratedVerdict::Reject,
        "after cancel actual transformed source rejects");
  check(classify(pre_cancel, 0, 0, 0) == GeneratedVerdict::Reject,
        "after cancel transformed bytes never classify as ordinary");
  sky_generation::end_frame();

  // 4c. Ordinary after lifecycle returns to baseline bytes.
  const auto after = generate();
  check(after.ok && after.bytes == ordinary.bytes, "post-lifecycle ordinary bytes == baseline");
  check(classify(after, 0, 0, false) == GeneratedVerdict::Ordinary,
        "fresh ordinary generation after immersive exit is accepted");
  check(jak1_sky_fixture::camera_snapshot() == camera_before, "camera bytes unchanged (final)");

  std::printf("jak1-sky-generation-contract-test: %s (%d)\n", g_failures ? "FAIL" : "PASS",
              g_failures);
  return g_failures ? 1 : 0;
}
