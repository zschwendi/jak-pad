#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_executor.h"

#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

tfrag3::IndexTexture make_index_texture(std::string_view name, u8 bias) {
  tfrag3::IndexTexture texture;
  texture.w = 2;
  texture.h = 2;
  texture.index_data = {0, 1, 2, 3};
  texture.level_names = {"LDJAKBRN.DGO"};
  texture.name = name;
  texture.tpage_name = "synthetic-prison-clut";
  for (std::size_t entry = 0; entry < texture.color_table.size(); ++entry) {
    texture.color_table[entry][0] = static_cast<u8>(entry + bias);
    texture.color_table[entry][1] = static_cast<u8>(entry + bias + 1);
    texture.color_table[entry][2] = static_cast<u8>(entry + bias + 2);
    texture.color_table[entry][3] = static_cast<u8>(255 - entry);
  }
  return texture;
}

void add_prison_sources(tfrag3::Level* level) {
  constexpr std::array<std::array<std::string_view, 3>, 6> kNames = {{
      {"jak-orig-arm-formorph", "jak-orig-arm-formorph-start",
       "jak-orig-arm-formorph-end"},
      {"jak-orig-eyebrow-formorph", "jak-orig-eyebrow-formorph-start",
       "jak-orig-eyebrow-formorph-end"},
      {"jak-orig-finger-formorph", "jak-orig-finger-formorph-start",
       "jak-orig-finger-formorph-end"},
      {"jakb-facelft", "jakb-facelft-norm", "jakb-facelft-dark"},
      {"jakb-facert", "jakb-facert-norm", "jakb-facert-dark"},
      {"jakb-hairtrans", "jakb-hairtrans-norm", "jakb-hairtrans-dark"},
  }};
  for (std::size_t slot = 0; slot < kNames.size(); ++slot) {
    level->index_textures.push_back(make_index_texture(kNames[slot][0], 0));
    level->index_textures.push_back(make_index_texture(kNames[slot][1],
                                                        static_cast<u8>(slot * 8 + 4)));
    level->index_textures.push_back(make_index_texture(kNames[slot][2],
                                                        static_cast<u8>(slot * 8 + 20)));
  }
}

metal_renderer::Jak2PrisPrisonJakAnimatorPlan make_plan(float morph) {
  metal_renderer::Jak2PrisPrisonJakAnimatorPlan plan;
  plan.morph = morph;
  plan.destination_tbps = {0x1000, 0x1010, 0x1020, 0x1030,
                           0x1040, 0x1050, 0x1060};
  plan.semantic_fingerprint = 1;
  return plan;
}

std::array<u8, 4> first_pixel(u64 handle) {
  std::array<u8, 4> pixel = {};
  id<MTLTexture> texture = metal_texture_lookup(handle);
  if (texture) {
    [texture getBytes:pixel.data()
          bytesPerRow:4
           fromRegion:MTLRegionMake2D(0, 0, 1, 1)
          mipmapLevel:0];
  }
  return pixel;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(device && queue, "created the Metal device and queue");
    if (!device || !queue) {
      return 1;
    }

    const std::size_t initial_live = metal_texture_live_count();
    tfrag3::Level common_level;
    common_level.level_name = "synthetic-common";
    add_prison_sources(&common_level);

    {
      metal_renderer::Jak2PrisonClutExecutor executor(device, queue);
      metal_renderer::Jak2PrisonClutExecutor::Prepared prepared;
      auto invalid_plan = make_plan(0.5f);
      invalid_plan.destination_tbps[3] =
          metal_renderer::kJak2PrisPrisonJakAnimatorTbpUpperBound;
      check(!executor.prepare(invalid_plan, common_level, &prepared) &&
                metal_texture_live_count() == initial_live,
            "an out-of-range selected output TBP fails before registry mutation");

      auto wrong_provenance = common_level;
      for (auto& texture : wrong_provenance.index_textures) {
        if (texture.name == "jakb-facelft-dark") {
          texture.level_names = {"OTHER.DGO"};
        }
      }
      check(!executor.prepare(make_plan(0.5f), wrong_provenance, &prepared) &&
                metal_texture_live_count() == initial_live,
            "a source without exact LDJAKBRN.DGO provenance fails before publication");

      auto registry_only_plan = make_plan(0.5f);
      for (const std::size_t index : metal_renderer::kJak2PrisonClutPlanTbpIndices) {
        registry_only_plan.destination_tbps[index] =
            metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp;
      }
      check(executor.prepare(registry_only_plan, common_level, &prepared) &&
                executor.stats().preparations == 1 &&
                metal_texture_live_count() == initial_live,
            "missing output TBPs still prepare all six registry-owned outputs before mutation");
      check(executor.publish(prepared) && executor.stats().publications == 1 &&
                metal_texture_live_count() == initial_live + 6,
            "missing output TBPs still publish exactly six registry-only textures");

      const auto first_handles = executor.stats().texture_handles;
      bool exact_slots = true;
      for (std::size_t i = 0; i < first_handles.size(); ++i) {
        const std::size_t slot = metal_renderer::kJak2PrisonClutAnimatedTextureSlots[i];
        exact_slots = exact_slots && first_handles[i] != 0 &&
                      executor.animated_texture_slots()[slot] == first_handles[i] &&
                      executor.stats().destination_tbps[i] ==
                          metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp;
      }
      check(exact_slots, "outputs publish only animated slots 4, 5, 7, 8, 9, and 10");
      check(first_pixel(first_handles[0]) == std::array<u8, 4>{12, 13, 14, 255},
            "the registry texture contains the CPU-blended destination indices");

      std::vector<u64> merged(jak2_animated_texture_slots().size(), 0);
      merged[14] = 0xdead;
      merged[20] = 0xbeef;
      merged[21] = 0xcafe;
      executor.merge_animated_texture_slots(merged);
      check(merged[4] == first_handles[0] && merged[10] == first_handles[5] &&
                merged[14] == 0xdead && merged[20] == 0xbeef && merged[21] == 0xcafe,
            "slot merging preserves skull-gem and both security publications");

      metal_renderer::Jak2PrisonClutExecutor::Prepared with_tbps;
      check(executor.prepare(make_plan(0.5f), common_level, &with_tbps) &&
                executor.publish(with_tbps) && executor.stats().preparations == 2 &&
                executor.stats().publications == 2 &&
                executor.stats().texture_handles == first_handles,
            "a carried non-output eyelid TBP and six present TBPs retain the stable handles");

      metal_renderer::Jak2PrisonClutExecutor::Prepared updated;
      check(executor.prepare(make_plan(0.25f), common_level, &updated) &&
                executor.publish(updated) && executor.stats().preparations == 3 &&
                executor.stats().publications == 3 &&
                executor.stats().texture_handles == first_handles &&
                metal_texture_live_count() == initial_live + 6,
            "later morphs update all six stable registry handles in place");
      check(first_pixel(first_handles[0]) == std::array<u8, 4>{8, 9, 10, 255},
            "the stable handle resolves the replacement CLUT result");
    }

    check(metal_texture_live_count() == initial_live,
          "executor destruction releases all six registry textures");
  }

  if (failures) {
    std::printf("FAIL: %d Jak II prison CLUT executor checks failed\n", failures);
    return 1;
  }
  std::puts("PASS: Jak II prison CLUT executor");
  return 0;
}
