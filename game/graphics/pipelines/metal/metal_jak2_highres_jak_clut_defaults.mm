#include "game/graphics/pipelines/metal/metal_jak2_highres_jak_clut_defaults.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"

#include "game/graphics/pipelines/metal/metal_texture.h"

namespace metal_renderer {
namespace {

constexpr std::string_view kHighresJakDefaultProvenance = "NEB.DGO";

struct HighresJakDefaultSourceSpec {
  std::size_t animated_slot;
  std::string_view destination;
  std::string_view normal_palette;
  std::string_view dark_palette;
};

// TextureAnimator creates the Nest group last, so these are the GL renderer's startup owners for
// the overlapping high-resolution Jak face and hair slots.
constexpr std::array<HighresJakDefaultSourceSpec, kJak2HighresJakClutBlendSlotCount> kSourceSpecs =
    {{{13, "jakb-eyebrow", "jakb-eyebrow-norm", "jakb-eyebrow-dark"},
      {11, "jakb-eyelid", "jakb-eyelid-norm", "jakb-eyelid-dark"},
      {8, "jakb-facelft", "jakb-facelft-norm", "jakb-facelft-dark"},
      {9, "jakb-facert", "jakb-facert-norm", "jakb-facert-dark"},
      {10, "jakb-hairtrans", "jakb-hairtrans-norm", "jakb-hairtrans-dark"}}};

bool has_provenance(const tfrag3::IndexTexture& texture) {
  return std::find(texture.level_names.begin(), texture.level_names.end(),
                   kHighresJakDefaultProvenance) != texture.level_names.end();
}

bool valid_index_texture(const tfrag3::IndexTexture& texture) {
  if (texture.w == 0 || texture.h == 0 ||
      texture.w > std::numeric_limits<std::size_t>::max() / texture.h) {
    return false;
  }
  return texture.index_data.size() ==
         static_cast<std::size_t>(texture.w) * static_cast<std::size_t>(texture.h);
}

const tfrag3::IndexTexture* find_consistent_index_texture(const tfrag3::Level& level,
                                                          std::string_view name) {
  const tfrag3::IndexTexture* found = nullptr;
  for (const auto& candidate : level.index_textures) {
    if (candidate.name != name || !has_provenance(candidate)) {
      continue;
    }
    if (!valid_index_texture(candidate) ||
        (found && (candidate.w != found->w || candidate.h != found->h ||
                   candidate.index_data != found->index_data ||
                   candidate.color_table != found->color_table))) {
      return nullptr;
    }
    if (!found) {
      found = &candidate;
    }
  }
  return found;
}

void copy_palette(const tfrag3::IndexTexture& source, Jak2ClutBlendPalette* out) {
  for (std::size_t entry = 0; entry < out->size(); ++entry) {
    for (int channel = 0; channel < static_cast<int>((*out)[entry].size()); ++channel) {
      (*out)[entry][channel] = source.color_table[entry][channel];
    }
  }
}

}  // namespace

Jak2HighresJakClutDefaults::Jak2HighresJakClutDefaults(id<MTLDevice> device,
                                                       id<MTLCommandQueue> queue)
    : m_device(device),
      m_queue(queue),
      m_animated_texture_slots(jak2_animated_texture_slots().size(), 0) {
  m_slot_contract_valid = true;
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    const auto& spec = kSourceSpecs[i];
    if (spec.animated_slot != kJak2HighresJakDefaultAnimatedTextureSlots[i] ||
        spec.animated_slot >= jak2_animated_texture_slots().size() ||
        jak2_animated_texture_slots()[spec.animated_slot] != spec.destination) {
      m_slot_contract_valid = false;
      break;
    }
  }
}

Jak2HighresJakClutDefaults::~Jak2HighresJakClutDefaults() {
  release_textures();
}

bool Jak2HighresJakClutDefaults::fail(const char* message) {
  m_error = message;
  return false;
}

bool Jak2HighresJakClutDefaults::initialize(const tfrag3::Level& common_level) {
  m_error.clear();
  if (!m_device || !m_queue || !m_slot_contract_valid ||
      std::any_of(m_texture_handles.begin(), m_texture_handles.end(),
                  [](u64 handle) { return handle != 0; })) {
    return fail("invalid high-resolution Jak CLUT default initialization state");
  }

  std::array<Jak2ClutBlendInput, kJak2HighresJakClutBlendSlotCount> inputs;
  std::array<u16, kJak2HighresJakClutBlendSlotCount> widths = {};
  std::array<u16, kJak2HighresJakClutBlendSlotCount> heights = {};
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    const auto& spec = kSourceSpecs[i];
    const auto* destination = find_consistent_index_texture(common_level, spec.destination);
    const auto* normal = find_consistent_index_texture(common_level, spec.normal_palette);
    const auto* dark = find_consistent_index_texture(common_level, spec.dark_palette);
    if (!destination || !normal || !dark) {
      return fail("required NEB.DGO high-resolution Jak CLUT source is unavailable, duplicated, or "
                  "malformed");
    }

    inputs[i].destination = {destination->w, destination->h, destination->index_data};
    copy_palette(*normal, &inputs[i].start_palette);
    copy_palette(*dark, &inputs[i].end_palette);
    widths[i] = destination->w;
    heights[i] = destination->h;
  }

  std::array<std::vector<u8>, kJak2HighresJakClutBlendSlotCount> rgba;
  if (!blend_jak2_highres_jak_clut_group_cpu(0.f, inputs, rgba)) {
    return fail("high-resolution Jak CLUT default CPU composition rejected its inputs");
  }

  std::array<u64, kJak2HighresJakClutBlendSlotCount> created = {};
  for (std::size_t i = 0; i < created.size(); ++i) {
    created[i] =
        metal_upload_texture_rgba8(m_device, m_queue, rgba[i].data(), widths[i], heights[i]);
    if (!created[i]) {
      for (const u64 handle : created) {
        metal_texture_release(handle);
      }
      return fail("high-resolution Jak CLUT default Metal publication failed");
    }
  }

  m_texture_handles = created;
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    m_animated_texture_slots[kSourceSpecs[i].animated_slot] = m_texture_handles[i];
  }
  return true;
}

void Jak2HighresJakClutDefaults::merge_animated_texture_slots(std::span<u64> slots) const {
  if (slots.size() < m_animated_texture_slots.size()) {
    return;
  }
  for (const auto& spec : kSourceSpecs) {
    const u64 handle = m_animated_texture_slots[spec.animated_slot];
    if (handle) {
      slots[spec.animated_slot] = handle;
    }
  }
}

void Jak2HighresJakClutDefaults::release_textures() {
  for (u64& handle : m_texture_handles) {
    metal_texture_release(handle);
    handle = 0;
  }
  std::fill(m_animated_texture_slots.begin(), m_animated_texture_slots.end(), 0);
}

}  // namespace metal_renderer
