#include "game/graphics/pipelines/metal/metal_jak2_mech_flame_identity.h"

#include <cstdio>
#include <cstdlib>

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

}  // namespace

int main() {
  using metal_renderer::Jak2MechFlameIdentityState;
  using metal_renderer::kJak2MechFlameTextureComboId;
  using metal_renderer::kJak2MechFlameTextureIndex;
  using metal_renderer::kJak2MechFlameTexturePage;
  using metal_renderer::kJak2MechFlameTextureTbp;
  using metal_renderer::kJak2MissingTextureIdentity;
  using metal_renderer::observe_jak2_mech_flame_identity;

  const auto disabled = observe_jak2_mech_flame_identity(
      false, kJak2MechFlameTextureTbp, true, kJak2MechFlameTexturePage, kJak2MechFlameTextureIndex);
  check(!disabled.capture && disabled.state == Jak2MechFlameIdentityState::NotObserved,
        "the unset diagnostic does no identity work");

  const auto other_tbp =
      observe_jak2_mech_flame_identity(true, kJak2MechFlameTextureTbp + 1, true,
                                       kJak2MechFlameTexturePage, kJak2MechFlameTextureIndex);
  check(!other_tbp.capture && other_tbp.state == Jak2MechFlameIdentityState::NotObserved,
        "the diagnostic ignores every non-target TBP");

  const auto missing = observe_jak2_mech_flame_identity(true, kJak2MechFlameTextureTbp, false);
  check(missing.capture && missing.actual_present == 0 &&
            missing.actual_page == kJak2MissingTextureIdentity &&
            missing.actual_texture == kJak2MissingTextureIdentity &&
            missing.state == Jak2MechFlameIdentityState::Missing,
        "a missing TBP 325 publication is explicit");

  const auto correct = observe_jak2_mech_flame_identity(
      true, kJak2MechFlameTextureTbp, true, kJak2MechFlameTexturePage, kJak2MechFlameTextureIndex);
  check(correct.capture && correct.actual_present == 1 &&
            correct.actual_combo_id == kJak2MechFlameTextureComboId &&
            correct.actual_placeholder == 0 && correct.state == Jak2MechFlameIdentityState::Correct,
        "the authored mech-flame identity is recognized");

  const auto placeholder =
      observe_jak2_mech_flame_identity(true, kJak2MechFlameTextureTbp, true,
                                       kJak2MechFlameTexturePage, kJak2MechFlameTextureIndex, true);
  check(placeholder.state == Jak2MechFlameIdentityState::Correct &&
            placeholder.actual_placeholder == 1 && !(placeholder == correct),
        "an authored identity backed by the pool placeholder remains distinguishable");

  const auto wrong =
      observe_jak2_mech_flame_identity(true, kJak2MechFlameTextureTbp, true,
                                       kJak2MechFlameTexturePage + 1, kJak2MechFlameTextureIndex);
  check(wrong.capture && wrong.actual_present == 1 &&
            wrong.actual_combo_id != kJak2MechFlameTextureComboId &&
            wrong.state == Jak2MechFlameIdentityState::Wrong && !(wrong == correct) &&
            !(wrong == missing),
        "a wrong live TexturePool identity is distinct from correct and missing");

  std::puts("jak2-mech-flame-identity-test: PASS");
  return 0;
}
