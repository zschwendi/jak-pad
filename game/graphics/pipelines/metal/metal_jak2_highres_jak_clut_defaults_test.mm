#include "game/graphics/pipelines/metal/metal_jak2_highres_jak_clut_defaults.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"

#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
  if (!condition) {
    failures++;
  }
}

tfrag3::IndexTexture make_index_texture(std::string_view name,
                                        u8 bias,
                                        std::string_view provenance,
                                        u16 width,
                                        u16 height) {
  tfrag3::IndexTexture texture;
  texture.w = width;
  texture.h = height;
  texture.index_data.resize(static_cast<std::size_t>(width) * height);
  for (std::size_t i = 0; i < texture.index_data.size(); ++i) {
    texture.index_data[i] = static_cast<u8>(i % 4);
  }
  texture.level_names = {std::string(provenance)};
  texture.name = name;
  texture.tpage_name = "synthetic-highres-jak-default";
  for (std::size_t entry = 0; entry < texture.color_table.size(); ++entry) {
    texture.color_table[entry][0] = static_cast<u8>(entry + bias);
    texture.color_table[entry][1] = static_cast<u8>(entry + bias + 1);
    texture.color_table[entry][2] = static_cast<u8>(entry + bias + 2);
    texture.color_table[entry][3] = static_cast<u8>(255 - entry);
  }
  return texture;
}

void add_highres_jak_sources(tfrag3::Level* level,
                             std::string_view provenance,
                             u8 provenance_bias,
                             u16 width,
                             u16 height) {
  constexpr std::array<std::array<std::string_view, 3>, 5> kNames = {{
      {"jakb-eyebrow", "jakb-eyebrow-norm", "jakb-eyebrow-dark"},
      {"jakb-eyelid", "jakb-eyelid-norm", "jakb-eyelid-dark"},
      {"jakb-facelft", "jakb-facelft-norm", "jakb-facelft-dark"},
      {"jakb-facert", "jakb-facert-norm", "jakb-facert-dark"},
      {"jakb-hairtrans", "jakb-hairtrans-norm", "jakb-hairtrans-dark"},
  }};
  for (std::size_t output = 0; output < kNames.size(); ++output) {
    level->index_textures.push_back(
        make_index_texture(kNames[output][0], 0, provenance, width, height));
    level->index_textures.push_back(
        make_index_texture(kNames[output][1], static_cast<u8>(provenance_bias + output * 8 + 4),
                           provenance, width, height));
    level->index_textures.push_back(
        make_index_texture(kNames[output][2], static_cast<u8>(provenance_bias + output * 8 + 20),
                           provenance, width, height));
  }
}

metal_renderer::Jak2PrisPrisonJakAnimatorPlan make_runtime_plan(u16 opcode,
                                                                float morph,
                                                                u32 tbp_base) {
  metal_renderer::Jak2PrisPrisonJakAnimatorPlan plan;
  plan.opcode = opcode;
  plan.destination_tbp_count = 5;
  plan.morph = morph;
  plan.destination_tbps.fill(metal_renderer::kJak2PrisPrisonJakAnimatorMissingTbp);
  for (u32 i = 0; i < 5; ++i) {
    plan.destination_tbps[i] = tbp_base + i * 0x10;
  }
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
    add_highres_jak_sources(&common_level, "NEB.DGO", 0, 2, 2);
    add_highres_jak_sources(&common_level, "ORACLE.DGO", 40, 4, 2);

    {
      auto missing_source = common_level;
      std::erase_if(missing_source.index_textures, [](const tfrag3::IndexTexture& texture) {
        return texture.name == "jakb-eyebrow-dark";
      });
      TexturePool pool(GameVersion::Jak2);
      metal_renderer::Jak2HighresJakClutDefaults defaults(device, queue, &pool);
      check(!defaults.initialize(missing_source) &&
                std::all_of(defaults.animated_texture_slots().begin(),
                            defaults.animated_texture_slots().end(),
                            [](u64 handle) { return handle == 0; }) &&
                metal_texture_live_count() == initial_live,
            "a missing Nest source leaves every high-resolution Jak slot unpublished");
    }

    {
      auto wrong_provenance = common_level;
      for (auto& texture : wrong_provenance.index_textures) {
        if (texture.name == "jakb-eyelid-norm") {
          texture.level_names = {"ORACLE.DGO"};
        }
      }
      TexturePool pool(GameVersion::Jak2);
      metal_renderer::Jak2HighresJakClutDefaults defaults(device, queue, &pool);
      check(!defaults.initialize(wrong_provenance) && metal_texture_live_count() == initial_live,
            "an Oracle source cannot replace the final GL Nest default");
    }

    {
      TexturePool pool(GameVersion::Jak2);
      metal_renderer::Jak2HighresJakClutDefaults defaults(device, queue, &pool);
      check(defaults.initialize(common_level) && metal_texture_live_count() == initial_live + 5,
            "five morph-zero Nest defaults publish exactly once");

      bool exact_slots = true;
      for (std::size_t slot = 0; slot < defaults.animated_texture_slots().size(); ++slot) {
        const bool expected =
            std::find(metal_renderer::kJak2HighresJakDefaultAnimatedTextureSlots.begin(),
                      metal_renderer::kJak2HighresJakDefaultAnimatedTextureSlots.end(),
                      slot) != metal_renderer::kJak2HighresJakDefaultAnimatedTextureSlots.end();
        exact_slots = exact_slots && ((defaults.animated_texture_slots()[slot] != 0) == expected);
      }
      check(exact_slots, "defaults publish only animated slots 8, 9, 10, 11, and 13");

      bool normal_palette_pixels = true;
      for (std::size_t output = 0;
           output < metal_renderer::kJak2HighresJakDefaultAnimatedTextureSlots.size(); ++output) {
        const std::size_t slot = metal_renderer::kJak2HighresJakDefaultAnimatedTextureSlots[output];
        const u8 bias = static_cast<u8>(output * 8 + 4);
        normal_palette_pixels =
            normal_palette_pixels &&
            first_pixel(defaults.animated_texture_slots()[slot]) ==
                std::array<u8, 4>{bias, static_cast<u8>(bias + 1), static_cast<u8>(bias + 2), 255};
      }
      check(normal_palette_pixels, "morph-zero defaults use each Nest -norm palette");

      const auto nest_handles = defaults.stats().texture_handles;
      const auto oracle_plan =
          make_runtime_plan(metal_renderer::kJak2PrisOracleJakAnimatorOpcode, 1.f, 0x120);
      metal_renderer::Jak2HighresJakClutDefaults::Prepared oracle_prepared;
      check(defaults.prepare(oracle_plan, common_level, &oracle_prepared) &&
                defaults.publish(oracle_prepared, oracle_plan.opcode) &&
                metal_texture_live_count() == initial_live + 10,
            "Oracle runtime animation publishes a separate five-texture group");
      const auto oracle_handles = defaults.stats().texture_handles;
      bool exact_oracle_publication = true;
      for (std::size_t i = 0; i < oracle_handles.size(); ++i) {
        id<MTLTexture> texture = metal_texture_lookup(oracle_handles[i]);
        exact_oracle_publication =
            exact_oracle_publication && oracle_handles[i] != 0 &&
            oracle_handles[i] != nest_handles[i] && texture && texture.width == 4 &&
            texture.height == 2 &&
            pool.lookup(oracle_plan.destination_tbps[i]).value_or(0) == oracle_handles[i];
      }
      check(exact_oracle_publication &&
                first_pixel(oracle_handles[0]) == std::array<u8, 4>{60, 61, 62, 255},
            "opcode 24 selects ORACLE.DGO pixels and publishes every destination TBP");

      const auto nest_plan =
          make_runtime_plan(metal_renderer::kJak2PrisNestJakAnimatorOpcode, 1.f, 0x220);
      metal_renderer::Jak2HighresJakClutDefaults::Prepared nest_prepared;
      check(defaults.prepare(nest_plan, common_level, &nest_prepared) &&
                defaults.publish(nest_prepared, nest_plan.opcode),
            "Nest runtime animation can reclaim the public high-resolution Jak slots");
      bool stable_nest_publication = true;
      for (std::size_t i = 0; i < nest_handles.size(); ++i) {
        stable_nest_publication =
            stable_nest_publication && defaults.stats().texture_handles[i] == nest_handles[i] &&
            pool.lookup(nest_plan.destination_tbps[i]).value_or(0) == nest_handles[i];
      }
      check(stable_nest_publication && metal_texture_live_count() == initial_live + 10,
            "opcode 25 preserves Nest identities while moving all five destination TBPs");

      std::vector<u64> merged(jak2_animated_texture_slots().size(), 0);
      merged[0] = 0xaaaa;
      merged[4] = 0xbbbb;
      merged[12] = 0xcccc;
      merged[14] = 0xdddd;
      defaults.merge_animated_texture_slots(merged);
      check(merged[8] != 0 && merged[9] != 0 && merged[10] != 0 && merged[11] != 0 &&
                merged[13] != 0 && merged[0] == 0xaaaa && merged[4] == 0xbbbb &&
                merged[12] == 0xcccc && merged[14] == 0xdddd,
            "slot merging preserves Dark Jak, prison, missing finger, and skull-gem owners");

      check(!defaults.initialize(common_level) && metal_texture_live_count() == initial_live + 10,
            "a repeated initialization preserves both live high-resolution Jak groups");
    }

    check(metal_texture_live_count() == initial_live,
          "publisher destruction releases both five-texture registry groups");
  }

  if (failures) {
    std::printf("FAIL: %d Jak II high-resolution Jak CLUT default checks failed\n", failures);
    return 1;
  }
  std::puts("PASS: Jak II high-resolution Jak CLUT defaults");
  return 0;
}
