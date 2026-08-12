#include "game/graphics/pipelines/metal/metal_jak2_prison_clut_executor.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"
#include "game/graphics/pipelines/metal/metal_texture.h"

namespace metal_renderer {
namespace {

constexpr std::string_view kPrisonClutProvenance = "LDJAKBRN.DGO";

struct PrisonClutSourceSpec {
  std::size_t animated_slot;
  std::size_t plan_tbp_index;
  std::string_view destination;
  std::string_view start_palette;
  std::string_view end_palette;
};

constexpr std::array<PrisonClutSourceSpec, kJak2ClutBlendSlotCount> kSourceSpecs = {{
    {4, 0, "jak-orig-arm-formorph", "jak-orig-arm-formorph-start",
     "jak-orig-arm-formorph-end"},
    {5, 1, "jak-orig-eyebrow-formorph", "jak-orig-eyebrow-formorph-start",
     "jak-orig-eyebrow-formorph-end"},
    {7, 3, "jak-orig-finger-formorph", "jak-orig-finger-formorph-start",
     "jak-orig-finger-formorph-end"},
    {8, 4, "jakb-facelft", "jakb-facelft-norm", "jakb-facelft-dark"},
    {9, 5, "jakb-facert", "jakb-facert-norm", "jakb-facert-dark"},
    {10, 6, "jakb-hairtrans", "jakb-hairtrans-norm", "jakb-hairtrans-dark"},
}};

bool has_provenance(const tfrag3::IndexTexture& texture) {
  return std::find(texture.level_names.begin(), texture.level_names.end(),
                   kPrisonClutProvenance) != texture.level_names.end();
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
    for (std::size_t channel = 0; channel < (*out)[entry].size(); ++channel) {
      (*out)[entry][channel] = source.color_table[entry][channel];
    }
  }
}

bool prepared_output_is_valid(const Jak2PrisonClutExecutor::PreparedOutput& output) {
  if (output.width == 0 || output.height == 0 ||
      output.width > std::numeric_limits<std::size_t>::max() / output.height) {
    return false;
  }
  const std::size_t pixels =
      static_cast<std::size_t>(output.width) * static_cast<std::size_t>(output.height);
  return pixels <= std::numeric_limits<std::size_t>::max() / 4 &&
         output.rgba.size() == pixels * 4;
}

bool valid_registry_only_tbp(u32 tbp) {
  return tbp == kJak2PrisPrisonJakAnimatorMissingTbp ||
         tbp < kJak2PrisPrisonJakAnimatorTbpUpperBound;
}

}  // namespace

Jak2PrisonClutExecutor::Jak2PrisonClutExecutor(id<MTLDevice> device,
                                               id<MTLCommandQueue> queue)
    : m_device(device),
      m_queue(queue),
      m_animated_texture_slots(jak2_animated_texture_slots().size(), 0) {
  m_slot_contract_valid = true;
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    const auto& spec = kSourceSpecs[i];
    if (spec.animated_slot != kJak2PrisonClutAnimatedTextureSlots[i] ||
        spec.plan_tbp_index != kJak2PrisonClutPlanTbpIndices[i] ||
        spec.animated_slot >= jak2_animated_texture_slots().size() ||
        jak2_animated_texture_slots()[spec.animated_slot] != spec.destination) {
      m_slot_contract_valid = false;
      break;
    }
  }
}

Jak2PrisonClutExecutor::~Jak2PrisonClutExecutor() {
  release_textures();
}

bool Jak2PrisonClutExecutor::fail(const char* message) {
  m_error = message;
  return false;
}

bool Jak2PrisonClutExecutor::prepare(const Jak2PrisPrisonJakAnimatorPlan& plan,
                                     const tfrag3::Level& common_level,
                                     Prepared* out) {
  m_error.clear();
  if (!out || !m_device || !m_queue || !m_slot_contract_valid ||
      plan.semantic_fingerprint == 0) {
    return fail("invalid prison CLUT preparation state");
  }

  std::array<Jak2ClutBlendInput, kJak2ClutBlendSlotCount> inputs;
  Prepared prepared;
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    const auto& spec = kSourceSpecs[i];
    const u32 destination_tbp = plan.destination_tbps[spec.plan_tbp_index];
    if (!valid_registry_only_tbp(destination_tbp)) {
      return fail("invalid prison CLUT destination TBP contract");
    }
    const auto* destination = find_consistent_index_texture(common_level, spec.destination);
    const auto* start = find_consistent_index_texture(common_level, spec.start_palette);
    const auto* end = find_consistent_index_texture(common_level, spec.end_palette);
    if (!destination || !start || !end) {
      return fail(
          "required LDJAKBRN.DGO prison CLUT index texture is unavailable, duplicated, or malformed");
    }

    inputs[i].destination = {destination->w, destination->h, destination->index_data};
    copy_palette(*start, &inputs[i].start_palette);
    copy_palette(*end, &inputs[i].end_palette);
    prepared[i].destination_tbp = destination_tbp;
    prepared[i].width = destination->w;
    prepared[i].height = destination->h;
  }

  std::array<std::vector<u8>, kJak2ClutBlendSlotCount> rgba;
  if (!blend_jak2_clut_group_cpu(plan.morph, inputs, rgba)) {
    return fail("prison CLUT CPU composition rejected its inputs");
  }
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    prepared[i].rgba = std::move(rgba[i]);
  }
  *out = std::move(prepared);
  m_stats.preparations++;
  return true;
}

bool Jak2PrisonClutExecutor::publish(const Prepared& prepared) {
  m_error.clear();
  for (const auto& output : prepared) {
    if (!prepared_output_is_valid(output) || !valid_registry_only_tbp(output.destination_tbp)) {
      return fail("invalid prison CLUT publication state");
    }
  }

  const bool first_publication =
      std::all_of(m_texture_handles.begin(), m_texture_handles.end(), [](u64 handle) {
        return handle == 0;
      });
  const bool complete_publication =
      std::all_of(m_texture_handles.begin(), m_texture_handles.end(), [](u64 handle) {
        return handle != 0;
      });
  if (!first_publication && !complete_publication) {
    return fail("prison CLUT registry handle set is incomplete");
  }

  if (first_publication) {
    std::array<u64, kJak2ClutBlendSlotCount> created = {};
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      const auto& output = prepared[i];
      created[i] = metal_upload_texture_rgba8(m_device, m_queue, output.rgba.data(), output.width,
                                               output.height);
      if (!created[i]) {
        for (const u64 handle : created) {
          metal_texture_release(handle);
        }
        return fail("initial prison CLUT Metal publication failed");
      }
    }
    m_texture_handles = created;
  } else {
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      const auto& output = prepared[i];
      id<MTLTexture> current = metal_texture_lookup(m_texture_handles[i]);
      if (!current || current.width != output.width || current.height != output.height ||
          current.pixelFormat != MTLPixelFormatRGBA8Unorm) {
        return fail("existing prison CLUT Metal texture shape changed");
      }
    }
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      const auto& output = prepared[i];
      if (!metal_update_texture_rgba8(m_texture_handles[i], m_queue, output.rgba.data(),
                                      output.width, output.height)) {
        return fail("prison CLUT Metal update failed");
      }
    }
  }

  for (std::size_t i = 0; i < prepared.size(); ++i) {
    m_animated_texture_slots[kSourceSpecs[i].animated_slot] = m_texture_handles[i];
    m_stats.destination_tbps[i] = prepared[i].destination_tbp;
    m_stats.texture_handles[i] = m_texture_handles[i];
  }
  m_stats.publications++;
  return true;
}

void Jak2PrisonClutExecutor::merge_animated_texture_slots(std::span<u64> slots) const {
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

void Jak2PrisonClutExecutor::release_textures() {
  for (u64& handle : m_texture_handles) {
    metal_texture_release(handle);
    handle = 0;
  }
  std::fill(m_animated_texture_slots.begin(), m_animated_texture_slots.end(), 0);
}

}  // namespace metal_renderer
