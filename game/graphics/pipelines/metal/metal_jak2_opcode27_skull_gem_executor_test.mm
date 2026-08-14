#include <cstdio>
#include <cstdlib>
#include <limits>

#include "common/custom_data/Tfrag3Data.h"
#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_executor.h"
#include "game/graphics/pipelines/metal/metal_level_data.h"
#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace {

void check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

metal_renderer::Jak2Opcode27LayerValues identity_values() {
  metal_renderer::Jak2Opcode27LayerValues values;
  values.color = {1.f, 1.f, 1.f, 1.f};
  values.scale = {1.f, 1.f};
  values.offset = {0.5f, 0.5f};
  values.st_scale = {1.f, 1.f};
  values.st_offset = {0.5f, 0.5f};
  values.qs = {1.f, 1.f, 1.f, 1.f};
  return values;
}

tfrag3::Texture source_texture(const char* name, u32 seed) {
  tfrag3::Texture texture;
  texture.w = 2;
  texture.h = 2;
  texture.debug_name = name;
  texture.debug_tpage_name = "level-default-tfrag";
  texture.load_to_pool = false;
  texture.data = {seed, seed + 0x00010101, seed + 0x00020202, seed + 0x00030303};
  return texture;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    check(device != nil, "Metal device is available");
    id<MTLCommandQueue> queue = [device newCommandQueue];
    check(queue != nil, "Metal command queue is available");

    const std::size_t initial_live = metal_texture_live_count();
    TexturePool pool(GameVersion::Jak2);
    const auto& placeholder_pixels = pool.placeholder_data();
    const u64 placeholder = metal_upload_texture_rgba8(
        device, queue, reinterpret_cast<const u8*>(placeholder_pixels.data()), 16, 16);
    check(placeholder != 0, "placeholder texture uploads");
    pool.set_placeholder(placeholder);

    tfrag3::Level common_level;
    common_level.textures.push_back(source_texture("skull-gem-alpha-00", 0xff0000ff));
    common_level.textures.push_back(source_texture("skull-gem-alpha-01", 0x80402010));
    common_level.textures.push_back(source_texture("skull-gem-alpha-02", 0x40201008));
    common_level.textures.push_back(source_texture("common-white", 0xffffffff));
    tfrag3::Level ctywide_level;
    ctywide_level.textures.push_back(source_texture("security-env-dest", 0xff000000));
    ctywide_level.textures.push_back(source_texture("security-env-uscroll", 0xff102030));
    ctywide_level.textures.push_back(source_texture("security-dot-dest", 0xff000000));
    ctywide_level.textures.push_back(source_texture("security-dot-src", 0xff403020));
    tfrag3::Level game_level = common_level;
    game_level.textures.insert(game_level.textures.end(), ctywide_level.textures.begin(),
                               ctywide_level.textures.end());
    game_level.textures.push_back(source_texture("bomb-gradient", 0xff000000));
    game_level.textures.push_back(source_texture("bomb-gradient-rim", 0xff204060));
    game_level.textures.push_back(source_texture("bomb-gradient-flames", 0xff604020));

    metal_renderer::Jak2Opcode27SkullGemPlan plan;
    plan.time = 0.f;
    plan.destination_tbp = 128;
    for (auto& layer : plan.layers) {
      layer.start = identity_values();
      layer.end = identity_values();
    }

    metal_renderer::Jak2Opcode27SkullGemExecutor executor(device, queue, &pool);
    metal_renderer::Jak2Opcode27SkullGemExecutor::Prepared prepared;
    tfrag3::Level missing_level;
    check(!executor.prepare(plan, missing_level, &prepared),
          "missing named skull-gem sources fail before publication");
    tfrag3::Level duplicate_level = common_level;
    duplicate_level.textures.push_back(source_texture("skull-gem-alpha-00", 0xff101010));
    check(!executor.prepare(plan, duplicate_level, &prepared),
          "conflicting duplicate skull-gem sources fail before publication");
    tfrag3::Level malformed_level = common_level;
    malformed_level.textures[1].data.pop_back();
    check(!executor.prepare(plan, malformed_level, &prepared),
          "malformed named skull-gem source dimensions fail before publication");
    check(executor.prepare(plan, common_level, &prepared) && prepared.destination_tbp == 128,
          "owned skull-gem preparation copies three named non-pool common-level sources");
    check(executor.publish(prepared), "prepared skull-gem publishes to Metal and TexturePool");
    const u64 stable_handle = executor.stats().texture_handle;
    check(stable_handle != 0 && executor.stats().preparations == 1 &&
              executor.stats().publications == 1 && pool.lookup(128).value_or(0) == stable_handle &&
              executor.animated_texture_slots().at(
                  metal_renderer::kJak2SkullGemAnimatedTextureSlot) == stable_handle,
          "first publication owns the packet TBP and Jak II animated slot 14");

    prepared.destination_tbp = 129;
    check(executor.publish(prepared) && executor.stats().texture_handle == stable_handle &&
              executor.stats().publications == 2 && pool.lookup(129).value_or(0) == stable_handle,
          "a packet-owned destination change preserves the stable published texture handle");

    metal_renderer::Jak2Opcode30SecurityPlan security_plan;
    security_plan.environment.time = 0.f;
    security_plan.environment.destination_tbp = 130;
    for (auto& layer : security_plan.environment.layers) {
      layer.start = identity_values();
      layer.end = identity_values();
    }
    security_plan.dot.time = 0.f;
    security_plan.dot.destination_tbp = 131;
    for (auto& layer : security_plan.dot.layers) {
      layer.start = identity_values();
      layer.end = identity_values();
    }
    metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedSecurity security_prepared;
    check(!executor.prepare_security(security_plan, missing_level, missing_level,
                                     &security_prepared),
          "missing named security sources and destinations fail before publication");
    check(!executor.prepare_security(security_plan, ctywide_level, common_level,
                                     &security_prepared),
          "security textures in the wrong level owners fail before publication");
    tfrag3::Level conflicting_ctywide_level = ctywide_level;
    conflicting_ctywide_level.textures.push_back(
        source_texture("security-env-dest", 0xff101010));
    check(!executor.prepare_security(security_plan, common_level, conflicting_ctywide_level,
                                     &security_prepared),
          "conflicting duplicate ctywide security destinations fail before publication");
    tfrag3::Level malformed_ctywide_level = ctywide_level;
    malformed_ctywide_level.textures[1].data.pop_back();
    check(!executor.prepare_security(security_plan, common_level, malformed_ctywide_level,
                                     &security_prepared),
          "malformed ctywide security sources fail before publication");
    auto environment_plan = security_plan.environment;
    environment_plan.destination_tbp = 132;
    metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedSecurityOutput
        environment_prepared;
    check(!executor.prepare_security_environment(environment_plan, missing_level,
                                                 &environment_prepared),
          "missing environment-only sources fail before publication");
    check(!executor.prepare_security_environment(environment_plan,
                                                 conflicting_ctywide_level,
                                                 &environment_prepared),
          "conflicting environment-only destinations fail before publication");
    metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedSecurityOutput
        common_environment_prepared;
    check(executor.prepare_security_environment(environment_plan, game_level,
                                                &common_environment_prepared) &&
              common_environment_prepared.width == 2 &&
              common_environment_prepared.height == 2 &&
              common_environment_prepared.destination_tbp == 132,
          "the common GAME level owns the source-equivalent security environment textures");
    check(executor.prepare_security_environment(environment_plan, ctywide_level,
                                                &environment_prepared) &&
              environment_prepared.width == 2 && environment_prepared.height == 2 &&
              environment_prepared.destination_tbp == 132 &&
              environment_prepared.rgba == common_environment_prepared.rgba,
          "common GAME and ctywide contain source-equivalent environment inputs");
    check(executor.publish_security_environment(common_environment_prepared),
          "the prepared environment-only output publishes to Metal and TexturePool");
    const u64 environment_only_handle = executor.animated_texture_slots().at(
        metal_renderer::kJak2SecurityEnvironmentAnimatedTextureSlot);
    check(environment_only_handle != 0 &&
              pool.lookup(132).value_or(0) == environment_only_handle &&
              executor.animated_texture_slots().at(
                  metal_renderer::kJak2SecurityDotAnimatedTextureSlot) == 0 &&
              executor.stats().security_preparations == 2 &&
              executor.stats().security_publications == 1,
          "environment-only publication owns slot 20 without manufacturing slot 21");
    tfrag3::Level repeated_common_level = common_level;
    repeated_common_level.textures.push_back(source_texture("common-white", 0xffffffff));
    tfrag3::Level repeated_ctywide_level = ctywide_level;
    for (int i = 0; i < 2; ++i) {
      repeated_ctywide_level.textures.push_back(
          source_texture("security-env-dest", 0xff000000));
      repeated_ctywide_level.textures.push_back(
          source_texture("security-dot-dest", 0xff000000));
    }
    check(executor.prepare_security(security_plan, repeated_common_level,
                                    repeated_ctywide_level,
                                    &security_prepared) &&
              security_prepared.environment.width == 2 &&
              security_prepared.environment.height == 2 &&
              security_prepared.environment.destination_tbp == 130 &&
              security_prepared.dot.destination_tbp == 131,
          "identical repeated common and ctywide textures compose in GOAL order");
    check(executor.publish_security(security_prepared),
          "prepared security outputs publish to Metal and TexturePool");
    const u64 security_environment_handle = executor.animated_texture_slots().at(
        metal_renderer::kJak2SecurityEnvironmentAnimatedTextureSlot);
    const u64 security_dot_handle = executor.animated_texture_slots().at(
        metal_renderer::kJak2SecurityDotAnimatedTextureSlot);
    check(security_environment_handle == environment_only_handle && security_dot_handle != 0 &&
              security_environment_handle != security_dot_handle &&
              pool.lookup(130).value_or(0) == security_environment_handle &&
              pool.lookup(131).value_or(0) == security_dot_handle &&
              executor.stats().security_preparations == 3 &&
              executor.stats().security_publications == 2,
          "full security publication reuses slot 20 and adds packet-owned slot 21");

    metal_renderer::Jak2Opcode28BombPlan bomb_plan;
    bomb_plan.time = 0.f;
    bomb_plan.destination_tbp = 133;
    metal_renderer::Jak2Opcode27SkullGemExecutor::PreparedBomb bomb_prepared;
    const bool bomb_clear_prepared = executor.prepare_bomb(bomb_plan, game_level,
                                                           &bomb_prepared);
    bool bomb_clear_alpha_is_opaque = bomb_prepared.rgba.size() == 16;
    for (std::size_t i = 3; i < bomb_prepared.rgba.size(); i += 4) {
      bomb_clear_alpha_is_opaque &= bomb_prepared.rgba[i] == 255;
    }
    check(bomb_clear_prepared && bomb_clear_alpha_is_opaque,
          "bomb clear alpha 0x80 becomes RGBA8 255 through tex_anim.frag division by 128");
    for (auto& layer : bomb_plan.layers) {
      layer.start = identity_values();
      layer.end = identity_values();
    }
    check(!executor.prepare_bomb(bomb_plan, common_level, &bomb_prepared),
          "bomb textures outside their GAME owner fail before publication");
    tfrag3::Level conflicting_game_level = game_level;
    conflicting_game_level.textures.push_back(
        source_texture("bomb-gradient-rim", 0xff101010));
    check(!executor.prepare_bomb(bomb_plan, conflicting_game_level, &bomb_prepared),
          "conflicting GAME bomb sources fail before publication");
    tfrag3::Level malformed_game_level = game_level;
    malformed_game_level.textures.back().data.pop_back();
    check(!executor.prepare_bomb(bomb_plan, malformed_game_level, &bomb_prepared),
          "malformed GAME bomb sources fail before publication");
    check(executor.prepare_bomb(bomb_plan, game_level, &bomb_prepared) &&
              bomb_prepared.width == 2 && bomb_prepared.height == 2 &&
              bomb_prepared.destination_tbp == 133,
          "opcode-28 preparation uses the GAME-owned bomb destination and two sources");
    check(executor.publish_bomb(bomb_prepared),
          "prepared opcode-28 bomb output publishes to Metal and TexturePool");
    const u64 bomb_handle = executor.animated_texture_slots().at(
        metal_renderer::kJak2BombAnimatedTextureSlot);
    check(bomb_handle != 0 && bomb_handle != security_environment_handle &&
              pool.lookup(133).value_or(0) == bomb_handle &&
              executor.stats().bomb_preparations == 2 &&
              executor.stats().bomb_publications == 1,
          "bomb publication owns packet TBP 133 and animated slot 15");

    MetalLevelData level;
    MetalSharedRenderState render_state;
    render_state.texture_pool = &pool;
    render_state.animated_texture_slots = executor.animated_texture_slots().data();
    render_state.animated_texture_slot_count = executor.animated_texture_slots().size();
    MetalBackgroundState background;
    id<MTLTexture> resolved = metal_background_texture(level, -15, &render_state, &background);
    check(resolved == metal_texture_lookup(stable_handle) && background.anim_slot_draws == 1 &&
              background.missing_textures == 0,
          "negative tree texture -15 resolves through animated slot 14 without a placeholder");
    resolved = metal_background_texture(level, -16, &render_state, &background);
    check(resolved == metal_texture_lookup(bomb_handle) && background.anim_slot_draws == 2 &&
              background.missing_textures == 0,
          "negative tree texture -16 resolves through bomb animated slot 15");
    resolved = metal_background_texture(level, -1, &render_state, &background);
    check(resolved == metal_texture_lookup(placeholder) && background.anim_slot_draws == 3 &&
              background.missing_textures == 1,
          "an unpublished animated slot remains a counted placeholder fallback");
    resolved = metal_background_texture(level, -21, &render_state, &background);
    check(resolved == metal_texture_lookup(security_environment_handle) &&
              background.missing_textures == 1,
          "negative tree texture -21 resolves through security environment slot 20");
    resolved = metal_background_texture(level, -22, &render_state, &background);
    check(resolved == metal_texture_lookup(security_dot_handle) &&
              background.missing_textures == 1,
          "negative tree texture -22 resolves through security dot slot 21");
    resolved = metal_background_texture(level, std::numeric_limits<s32>::min(), &render_state,
                                        &background);
    check(resolved == metal_texture_lookup(placeholder) && background.anim_slot_draws == 6 &&
              background.missing_textures == 2,
          "the minimum signed texture ID is range-checked without overflow");

    executor.detach_pool();
    pool.set_placeholder(0);
    metal_texture_release(placeholder);
    check(metal_texture_live_count() == initial_live,
          "executor teardown releases every registry texture and detaches the live pool");
    std::puts("PASS: Jak II opcode-27 skull-gem executor");
  }
  return 0;
}
