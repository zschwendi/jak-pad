#include "game/graphics/pipelines/metal/metal_jak2_effects_bucket315_plan.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

using Plan = metal_renderer::Jak2EffectsBucket315Plan;
using Transfer = metal_renderer::Jak2EffectsBucket315Transfer;
using Variant = metal_renderer::Jak2EffectsBucket315Variant;
using VifKind = metal_renderer::Jak2EffectsBucket315VifKind;

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

Transfer transfer(u32 payload_bytes, VifKind vif0, VifKind vif1) {
  return {payload_bytes, vif0, vif1};
}

std::vector<Transfer> make_lightning(u32 fragment_count, u32 vertices_per_fragment) {
  std::vector<Transfer> result = {
      transfer(0, VifKind::Mark, VifKind::Nop),
      transfer(32, VifKind::Nop, VifKind::Direct),
      transfer(128, VifKind::Stcycl, VifKind::UnpackV4_32),
      transfer(32, VifKind::Mscalf, VifKind::Stmod),
      transfer(0, VifKind::Nop, VifKind::Nop),
  };
  for (u32 i = 0; i < fragment_count; ++i) {
    result.push_back(transfer(metal_renderer::kJak2EffectsLightningHeaderBytes, VifKind::Nop,
                              VifKind::UnpackV4_32));
    result.push_back(transfer(vertices_per_fragment * metal_renderer::kJak2EffectsLightningVertexBytes,
                              VifKind::Nop, VifKind::UnpackV4_32));
    result.push_back(transfer(0, VifKind::Nop, VifKind::Mscal));
  }
  result.push_back(transfer(0, VifKind::Nop, VifKind::Nop));
  result.push_back(transfer(160, VifKind::Flusha, VifKind::Direct));
  result.push_back(transfer(0, VifKind::Nop, VifKind::Nop));
  return result;
}

std::optional<Plan> plan(const std::vector<Transfer>& transfers,
                         u32 bucket_id = metal_renderer::kJak2EffectsBucket) {
  return metal_renderer::plan_jak2_effects_bucket315(transfers.data(), transfers.size(),
                                                      bucket_id);
}

void test_observed_envelopes() {
  const std::vector<Transfer> absent = {transfer(0, VifKind::Nop, VifKind::Nop)};
  const auto absent_plan = plan(absent);
  check(absent_plan && absent_plan->variant == Variant::Absent && absent_plan->transfer_count == 0 &&
            absent_plan->payload_bytes == 0,
        "the exact unused bucket slot remains a passive no-op");

  const auto baseline = plan(make_lightning(0, 0));
  check(baseline && baseline->variant == Variant::Lightning && baseline->transfer_count == 8 &&
            baseline->fragment_count == 0 && baseline->vertex_count == 0 &&
            baseline->payload_bytes == 352 && baseline->semantic_fingerprint != 0,
        "the observed 352-byte city envelope is accepted without execution");

  const auto active = plan(make_lightning(2, 32));
  check(active && active->transfer_count == 14 && active->fragment_count == 2 &&
            active->vertex_count == 64 && active->payload_bytes == 3808 &&
            active->semantic_fingerprint != baseline->semantic_fingerprint,
        "the observed 3808-byte active envelope records two bounded Lightning fragments");

  const auto larger = plan(make_lightning(10, 40));
  check(larger && larger->payload_bytes == 21472 && larger->fragment_count == 10 &&
            larger->vertex_count == 400,
        "a larger retained envelope is accepted only through the source Lightning grammar");
}

void test_rejections() {
  auto malformed = make_lightning(1, 32);
  malformed[5].payload_bytes--;
  check(!plan(malformed), "a non-source Lightning header size is rejected");

  malformed = make_lightning(1, 32);
  malformed[6].payload_bytes++;
  check(!plan(malformed), "a vertex payload outside the 48-byte record grammar is rejected");

  malformed = make_lightning(1, 32);
  malformed[7].vif1 = VifKind::Nop;
  check(!plan(malformed), "a non-MSCAL fragment terminator is rejected");

  malformed = make_lightning(0, 0);
  malformed.back().vif0 = VifKind::Mark;
  check(!plan(malformed), "a non-NOP bucket terminator is rejected");

  malformed = make_lightning(0, 0);
  malformed.erase(malformed.end() - 3);
  check(!plan(malformed), "the source bucket-link NOP before the trailer is required");

  const auto valid = make_lightning(0, 0);
  check(!plan(valid, metal_renderer::kJak2EffectsBucket + 1),
        "the diagnostic never accepts another bucket ID");
  check(!metal_renderer::plan_jak2_effects_bucket315(nullptr, 1,
                                                      metal_renderer::kJak2EffectsBucket),
        "a nonempty null transfer summary is rejected");
}

}  // namespace

int main() {
  test_observed_envelopes();
  test_rejections();
  std::puts("jak2 effects bucket-315 plan tests passed");
  return 0;
}
