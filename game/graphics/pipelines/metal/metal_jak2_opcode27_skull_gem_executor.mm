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
      jak2_animated_texture_slots()[kJak2SkullGemAnimatedTextureSlot] == "skull-gem-dest";
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
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const tfrag3::Texture* source = nullptr;
    for (const auto& candidate : common_level.textures) {
      if (candidate.debug_name == kSourceTextureNames[i]) {
        if (source) {
          return fail("required skull-gem source texture is duplicated");
        }
        source = &candidate;
      }
    }
    if (!source || source->w == 0 || source->h == 0 ||
        source->data.size() !=
            static_cast<std::size_t>(source->w) * static_cast<std::size_t>(source->h)) {
      return fail("required skull-gem source texture is unavailable or malformed");
    }
    sources[i].width = source->w;
    sources[i].height = source->h;
    sources[i].rgba.resize(source->data.size() * sizeof(u32));
    std::memcpy(sources[i].rgba.data(), source->data.data(), sources[i].rgba.size());
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

void Jak2Opcode27SkullGemExecutor::detach_pool() {
  std::fill(m_animated_texture_slots.begin(), m_animated_texture_slots.end(), 0);
  if (m_publication) {
    m_publication->detach_pool();
    m_publication.reset();
  }
  m_pool = nullptr;
}

}  // namespace metal_renderer
