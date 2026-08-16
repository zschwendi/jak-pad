#include "game/graphics/pipelines/metal/metal_jak2_opcode27_skull_gem_executor.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"
#include "game/graphics/pipelines/metal/metal_pool_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {
namespace {

constexpr std::array<std::string_view, kJak2Opcode27SkullGemLayerCount> kSourceTextureNames = {
    "skull-gem-alpha-00", "skull-gem-alpha-01", "skull-gem-alpha-02"};
constexpr std::array<std::string_view, 2> kBombSourceTextureNames = {
    "bomb-gradient-rim", "bomb-gradient-flames"};
constexpr std::array<std::string_view, 2> kSecurityEnvironmentSourceTextureNames = {
    "security-env-uscroll", "security-env-uscroll"};
constexpr std::array<std::string_view, 3> kSecurityDotSourceTextureNames = {
    "common-white", "security-dot-src", "security-dot-src"};
constexpr std::array<float, 2> kSecurityEnvironmentEndTimes = {4800.f, 4800.f};
constexpr std::array<float, 3> kSecurityDotEndTimes = {4800.f, 600.f, 600.f};
constexpr std::array<float, 2> kBombEndTimes = {300.f, 300.f};

bool has_valid_rgba_shape(const tfrag3::Texture& texture) {
  return texture.w != 0 && texture.h != 0 &&
         texture.data.size() ==
             static_cast<std::size_t>(texture.w) * static_cast<std::size_t>(texture.h);
}

const tfrag3::Texture* find_consistent_texture(const tfrag3::Level& level,
                                              std::string_view name) {
  const tfrag3::Texture* found = nullptr;
  for (const auto& candidate : level.textures) {
    if (candidate.debug_name == name) {
      if (!has_valid_rgba_shape(candidate) ||
          (found && (candidate.w != found->w || candidate.h != found->h ||
                     candidate.data != found->data))) {
        return nullptr;
      }
      if (!found) {
        found = &candidate;
      }
    }
  }
  return found;
}

bool copy_rgba_source(const tfrag3::Texture* source, Jak2Opcode27RgbaSource* out) {
  if (!source || source->w == 0 || source->h == 0 ||
      source->data.size() !=
          static_cast<std::size_t>(source->w) * static_cast<std::size_t>(source->h)) {
    return false;
  }
  out->width = source->w;
  out->height = source->h;
  out->rgba.resize(source->data.size() * sizeof(u32));
  std::memcpy(out->rgba.data(), source->data.data(), out->rgba.size());
  return true;
}

template <std::size_t Size>
bool load_sources(const tfrag3::Level& level,
                  const std::array<std::string_view, Size>& names,
                  std::array<Jak2Opcode27RgbaSource, Size>* out) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!copy_rgba_source(find_consistent_texture(level, names[i]), &(*out)[i])) {
      return false;
    }
  }
  return true;
}

template <std::size_t Size>
bool load_sources(const std::array<const tfrag3::Level*, Size>& levels,
                  const std::array<std::string_view, Size>& names,
                  std::array<Jak2Opcode27RgbaSource, Size>* out) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (!levels[i] ||
        !copy_rgba_source(find_consistent_texture(*levels[i], names[i]), &(*out)[i])) {
      return false;
    }
  }
  return true;
}

template <typename Plan, std::size_t Size>
bool prepare_fixed_output(const Plan& plan,
                          const tfrag3::Level& destination_level,
                          std::string_view destination_name,
                          const std::array<const tfrag3::Level*, Size>& source_levels,
                          const std::array<std::string_view, Size>& source_names,
                          const std::array<float, Size>& end_times,
                          Jak2Opcode27SkullGemExecutor::PreparedFixedOutput* out) {
  const auto* destination = find_consistent_texture(destination_level, destination_name);
  std::array<Jak2Opcode27RgbaSource, Size> sources;
  if (!destination || destination->w == 0 || destination->h == 0 ||
      destination->data.size() !=
          static_cast<std::size_t>(destination->w) * destination->h ||
      !load_sources(source_levels, source_names, &sources)) {
    return false;
  }

  Jak2Opcode27SkullGemExecutor::PreparedFixedOutput prepared;
  prepared.destination_tbp = plan.destination_tbp;
  prepared.width = destination->w;
  prepared.height = destination->h;
  if (!compose_jak2_fixed_animation_cpu(plan.time, end_times, plan.layers, sources,
                                        prepared.width, prepared.height, &prepared.rgba)) {
    return false;
  }
  *out = std::move(prepared);
  return true;
}

}  // namespace

Jak2Opcode27SkullGemExecutor::Jak2Opcode27SkullGemExecutor(id<MTLDevice> device,
                                                           id<MTLCommandQueue> queue,
                                                           TexturePool* pool)
    : m_device(device),
      m_queue(queue),
      m_pool(pool),
      m_animated_texture_slots(jak2_animated_texture_slots().size(), 0) {
  m_slot_contract_valid =
      kJak2SkullGemAnimatedTextureSlot < jak2_animated_texture_slots().size() &&
      jak2_animated_texture_slots()[kJak2SkullGemAnimatedTextureSlot] == "skull-gem-dest" &&
      kJak2BombAnimatedTextureSlot < jak2_animated_texture_slots().size() &&
      jak2_animated_texture_slots()[kJak2BombAnimatedTextureSlot] == "bomb-gradient" &&
      kJak2SecurityEnvironmentAnimatedTextureSlot < jak2_animated_texture_slots().size() &&
      jak2_animated_texture_slots()[kJak2SecurityEnvironmentAnimatedTextureSlot] ==
          "security-env-dest" &&
      kJak2SecurityDotAnimatedTextureSlot < jak2_animated_texture_slots().size() &&
      jak2_animated_texture_slots()[kJak2SecurityDotAnimatedTextureSlot] ==
          "security-dot-dest";
}

Jak2Opcode27SkullGemExecutor::~Jak2Opcode27SkullGemExecutor() = default;

bool Jak2Opcode27SkullGemExecutor::fail(const char* message) {
  m_error = message;
  return false;
}

bool Jak2Opcode27SkullGemExecutor::prepare(const Jak2Opcode27SkullGemPlan& plan,
                                           const tfrag3::Level& common_level,
                                           Prepared* out) {
  m_error.clear();
  if (!m_pool || !out || !m_slot_contract_valid ||
      plan.destination_tbp >= static_cast<u32>(m_pool->all_textures().size())) {
    return fail("invalid skull-gem preparation destination");
  }

  std::array<Jak2Opcode27RgbaSource, kJak2Opcode27SkullGemLayerCount> sources;
  if (!load_sources(common_level, kSourceTextureNames, &sources)) {
    return fail("required skull-gem source texture is unavailable, duplicated, or malformed");
  }

  Prepared prepared;
  prepared.destination_tbp = plan.destination_tbp;
  if (!compose_jak2_opcode27_skull_gem_cpu(plan, sources, &prepared.rgba)) {
    return fail("skull-gem CPU composition rejected its inputs");
  }
  *out = std::move(prepared);
  m_stats.preparations++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::publish(const Prepared& prepared) {
  m_error.clear();
  if (!m_pool || kJak2SkullGemAnimatedTextureSlot >= m_animated_texture_slots.size() ||
      prepared.destination_tbp >= static_cast<u32>(m_pool->all_textures().size())) {
    return fail("invalid skull-gem publication state");
  }

  std::array<u32, kJak2Opcode27SkullGemRgbaBytes / sizeof(u32)> words;
  std::memcpy(words.data(), prepared.rgba.data(), prepared.rgba.size());
  if (!m_publication) {
    auto publication = std::make_unique<MetalPoolTexture>(
        m_device, m_queue, m_pool, kJak2Opcode27SkullGemSize, kJak2Opcode27SkullGemSize,
        prepared.destination_tbp, "jak2-opcode27-skull-gem");
    if (!publication->publish(words.data(), words.size())) {
      return fail("initial skull-gem Metal publication failed");
    }
    m_publication = std::move(publication);
  } else if (!m_publication->publish_at(words.data(), words.size(), prepared.destination_tbp)) {
    return fail("skull-gem Metal update failed");
  }

  m_animated_texture_slots[kJak2SkullGemAnimatedTextureSlot] = m_publication->handle();
  m_stats.publications = m_publication->publications();
  m_stats.destination_tbp = prepared.destination_tbp;
  m_stats.texture_handle = m_publication->handle();
  return true;
}

bool Jak2Opcode27SkullGemExecutor::prepare_security(
    const Jak2Opcode30SecurityPlan& plan,
    const tfrag3::Level& common_level,
    const tfrag3::Level& ctywide_level,
    PreparedSecurity* out) {
  m_error.clear();
  if (!m_pool || !out || !m_slot_contract_valid ||
      plan.environment.destination_tbp >= static_cast<u32>(m_pool->all_textures().size()) ||
      plan.dot.destination_tbp >= static_cast<u32>(m_pool->all_textures().size())) {
    return fail("invalid security preparation destination");
  }

  PreparedSecurity prepared;
  const std::array<const tfrag3::Level*, 2> environment_source_levels = {
      &ctywide_level, &ctywide_level};
  const std::array<const tfrag3::Level*, 3> dot_source_levels = {
      &common_level, &ctywide_level, &ctywide_level};
  if (!prepare_fixed_output(plan.environment, ctywide_level, "security-env-dest",
                            environment_source_levels, kSecurityEnvironmentSourceTextureNames,
                            kSecurityEnvironmentEndTimes, &prepared.environment) ||
      !prepare_fixed_output(plan.dot, ctywide_level, "security-dot-dest", dot_source_levels,
                            kSecurityDotSourceTextureNames, kSecurityDotEndTimes, &prepared.dot)) {
    return fail("required security textures are unavailable, duplicated, or malformed");
  }
  *out = std::move(prepared);
  m_stats.security_preparations++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::prepare_security_environment(
    const Jak2Opcode30SecurityEnvironmentPlan& plan,
    const tfrag3::Level& ctywide_level,
    PreparedSecurityOutput* out) {
  m_error.clear();
  if (!m_pool || !out || !m_slot_contract_valid ||
      plan.destination_tbp >= static_cast<u32>(m_pool->all_textures().size())) {
    return fail("invalid security-environment preparation destination");
  }

  const std::array<const tfrag3::Level*, 2> source_levels = {
      &ctywide_level, &ctywide_level};
  PreparedSecurityOutput prepared;
  if (!prepare_fixed_output(plan, ctywide_level, "security-env-dest", source_levels,
                            kSecurityEnvironmentSourceTextureNames,
                            kSecurityEnvironmentEndTimes, &prepared)) {
    return fail(
        "required security-environment textures are unavailable, duplicated, or malformed");
  }
  *out = std::move(prepared);
  m_stats.security_preparations++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::prepare_bomb(const Jak2Opcode28BombPlan& plan,
                                                const tfrag3::Level& game_level,
                                                PreparedBomb* out) {
  m_error.clear();
  if (!m_pool || !out || !m_slot_contract_valid ||
      plan.destination_tbp >= static_cast<u32>(m_pool->all_textures().size())) {
    return fail("invalid bomb preparation destination");
  }

  const std::array<const tfrag3::Level*, 2> source_levels = {&game_level, &game_level};
  PreparedBomb prepared;
  if (!prepare_fixed_output(plan, game_level, "bomb-gradient", source_levels,
                            kBombSourceTextureNames, kBombEndTimes, &prepared)) {
    return fail("required GAME bomb textures are unavailable, duplicated, or malformed");
  }
  *out = std::move(prepared);
  m_stats.bomb_preparations++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::publish_fixed_output(
    const PreparedFixedOutput& prepared,
    std::size_t slot,
    const char* label,
    std::unique_ptr<MetalPoolTexture>* publication) {
  if (!m_pool || !publication || slot >= m_animated_texture_slots.size() ||
      prepared.width == 0 || prepared.height == 0 ||
      prepared.destination_tbp >= static_cast<u32>(m_pool->all_textures().size()) ||
      prepared.rgba.size() !=
          static_cast<std::size_t>(prepared.width) * prepared.height * sizeof(u32)) {
    return false;
  }
  std::vector<u32> words(prepared.rgba.size() / sizeof(u32));
  std::memcpy(words.data(), prepared.rgba.data(), prepared.rgba.size());
  if (!*publication) {
    auto created = std::make_unique<MetalPoolTexture>(
        m_device, m_queue, m_pool, prepared.width, prepared.height,
        prepared.destination_tbp, label);
    if (!created->publish(words.data(), words.size())) {
      return false;
    }
    *publication = std::move(created);
  } else if (!(*publication)->publish_at(words.data(), words.size(),
                                         prepared.destination_tbp)) {
    return false;
  }
  m_animated_texture_slots[slot] = (*publication)->handle();
  return true;
}

bool Jak2Opcode27SkullGemExecutor::publish_security(
    const PreparedSecurity& prepared) {
  m_error.clear();
  if (!publish_fixed_output(prepared.environment,
                            kJak2SecurityEnvironmentAnimatedTextureSlot,
                            "jak2-opcode30-security-environment",
                            &m_security_environment_publication) ||
      !publish_fixed_output(prepared.dot, kJak2SecurityDotAnimatedTextureSlot,
                            "jak2-opcode30-security-dot",
                            &m_security_dot_publication)) {
    return fail("security Metal publication failed");
  }
  m_stats.security_publications++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::publish_security_environment(
    const PreparedSecurityOutput& prepared) {
  m_error.clear();
  if (!publish_fixed_output(prepared, kJak2SecurityEnvironmentAnimatedTextureSlot,
                            "jak2-opcode30-security-environment",
                            &m_security_environment_publication)) {
    return fail("security-environment Metal publication failed");
  }
  m_stats.security_publications++;
  return true;
}

bool Jak2Opcode27SkullGemExecutor::publish_bomb(const PreparedBomb& prepared) {
  m_error.clear();
  if (!publish_fixed_output(prepared, kJak2BombAnimatedTextureSlot,
                            "jak2-opcode28-bomb", &m_bomb_publication)) {
    return fail("bomb Metal publication failed");
  }
  m_stats.bomb_publications++;
  return true;
}

void Jak2Opcode27SkullGemExecutor::detach_pool() {
  std::fill(m_animated_texture_slots.begin(), m_animated_texture_slots.end(), 0);
  if (m_publication) {
    m_publication->detach_pool();
    m_publication.reset();
  }
  if (m_bomb_publication) {
    m_bomb_publication->detach_pool();
    m_bomb_publication.reset();
  }
  if (m_security_environment_publication) {
    m_security_environment_publication->detach_pool();
    m_security_environment_publication.reset();
  }
  if (m_security_dot_publication) {
    m_security_dot_publication->detach_pool();
    m_security_dot_publication.reset();
  }
  m_pool = nullptr;
}

}  // namespace metal_renderer
