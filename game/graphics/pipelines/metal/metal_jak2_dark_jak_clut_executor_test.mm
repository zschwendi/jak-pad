#include "game/graphics/pipelines/metal/metal_jak2_dark_jak_clut_executor.h"

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
  texture.level_names = {"GAME.DGO"};
  texture.name = name;
  texture.tpage_name = "synthetic-dark-jak-clut";
  for (std::size_t entry = 0; entry < texture.color_table.size(); ++entry) {
    texture.color_table[entry][0] = static_cast<u8>(entry + bias);
    texture.color_table[entry][1] = static_cast<u8>(entry + bias + 1);
    texture.color_table[entry][2] = static_cast<u8>(entry + bias + 2);
    texture.color_table[entry][3] = static_cast<u8>(255 - entry);
  }
  return texture;
}

void add_dark_jak_sources(tfrag3::Level* level) {
  constexpr std::array<std::array<std::string_view, 3>, 4> kNames = {{
      {"jakbsmall-eyebrow", "jakbsmall-eyebrow-norm", "jakbsmall-eyebrow-dark"},
      {"jakbsmall-face", "jakbsmall-face-norm", "jakbsmall-face-dark"},
      {"jakbsmall-finger", "jakbsmall-finger-norm", "jakbsmall-finger-dark"},
      {"jakbsmall-hair", "jakbsmall-hair-norm", "jakbsmall-hair-dark"},
  }};
  for (std::size_t slot = 0; slot < kNames.size(); ++slot) {
    level->index_textures.push_back(make_index_texture(kNames[slot][0], 0));
    level->index_textures.push_back(
        make_index_texture(kNames[slot][1], static_cast<u8>(slot * 8 + 4)));
    level->index_textures.push_back(
        make_index_texture(kNames[slot][2], static_cast<u8>(slot * 8 + 20)));
  }
}

metal_renderer::Jak2CommonPrisDarkJakAnimatorPlan make_plan(float morph) {
  metal_renderer::Jak2CommonPrisDarkJakAnimatorPlan plan;
  plan.morph = morph;
  plan.destination_tbps = {0x1000, 0x1010, 0x1020, 0x1030};
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
    add_dark_jak_sources(&common_level);

    {
      metal_renderer::Jak2DarkJakClutExecutor executor(device, queue);
      metal_renderer::Jak2DarkJakClutExecutor::Prepared prepared;

      auto invalid_plan = make_plan(0.5f);
      invalid_plan.destination_tbps[0] =
          metal_renderer::kJak2PrisPrisonJakAnimatorTbpUpperBound;
      check(!executor.prepare(invalid_plan, common_level, &prepared) &&
                metal_texture_live_count() == initial_live,
            "an invalid destination fails before registry mutation");

      auto duplicate_source = common_level;
      duplicate_source.index_textures.push_back(
          make_index_texture("jakbsmall-face-dark", 99));
      check(!executor.prepare(make_plan(0.5f), duplicate_source, &prepared) &&
                metal_texture_live_count() == initial_live,
            "a conflicting duplicate common source fails before publication");

      check(executor.prepare(make_plan(0.5f), common_level, &prepared) &&
                executor.stats().preparations == 1 &&
                metal_texture_live_count() == initial_live,
            "four Dark Jak outputs prepare without Metal mutation");
      check(executor.publish(prepared) && executor.stats().publications == 1 &&
                metal_texture_live_count() == initial_live + 4,
            "four Dark Jak outputs publish exactly once");

      const auto first_handles = executor.stats().texture_handles;
      bool exact_slots = true;
      for (std::size_t i = 0; i < first_handles.size(); ++i) {
        exact_slots = exact_slots && first_handles[i] != 0 &&
                      executor.animated_texture_slots()[i] == first_handles[i] &&
                      executor.stats().destination_tbps[i] == 0x1000 + i * 0x10;
      }
      check(exact_slots, "outputs publish only stable animated slots 0 through 3");
      check(first_pixel(first_handles[0]) == std::array<u8, 4>{12, 13, 14, 255},
            "the published texture contains the CPU-blended indexed result");

      std::vector<u64> merged(jak2_animated_texture_slots().size(), 0);
      merged[4] = 0xaaaa;
      merged[5] = 0xbbbb;
      merged[7] = 0xcccc;
      merged[8] = 0xdddd;
      merged[9] = 0xeeee;
      merged[10] = 0xffff;
      executor.merge_animated_texture_slots(merged);
      check(merged[0] == first_handles[0] && merged[3] == first_handles[3] &&
                merged[4] == 0xaaaa && merged[5] == 0xbbbb && merged[7] == 0xcccc &&
                merged[8] == 0xdddd && merged[9] == 0xeeee && merged[10] == 0xffff,
            "slot merging fills Merc slots 0 through 3 and preserves prison slots");

      metal_renderer::Jak2DarkJakClutExecutor::Prepared updated;
      check(executor.prepare(make_plan(0.25f), common_level, &updated) &&
                executor.publish(updated) && executor.stats().preparations == 2 &&
                executor.stats().publications == 2 &&
                executor.stats().texture_handles == first_handles &&
                metal_texture_live_count() == initial_live + 4,
            "later frames update all four stable handles in place");
      check(first_pixel(first_handles[0]) == std::array<u8, 4>{8, 9, 10, 255},
            "the stable handle resolves the replacement blend");
    }

    check(metal_texture_live_count() == initial_live,
          "executor destruction releases all four registry textures");
  }

  if (failures) {
    std::printf("FAIL: %d Jak II Dark Jak CLUT executor checks failed\n", failures);
    return 1;
  }
  std::puts("PASS: Jak II Dark Jak CLUT executor");
  return 0;
}
