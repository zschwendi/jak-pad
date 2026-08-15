#include "game/graphics/pipelines/metal/metal_jak2_highres_jak_clut_defaults.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "common/custom_data/Tfrag3Data.h"
#include "common/texture/texture_slots.h"

#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

namespace metal_renderer {
namespace {

constexpr std::string_view kOracleProvenance = "ORACLE.DGO";
constexpr std::string_view kNestProvenance = "NEB.DGO";

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

bool has_provenance(const tfrag3::IndexTexture& texture, std::string_view provenance) {
  return std::find(texture.level_names.begin(), texture.level_names.end(), provenance) !=
         texture.level_names.end();
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
                                                          std::string_view name,
                                                          std::string_view provenance) {
  const tfrag3::IndexTexture* found = nullptr;
  for (const auto& candidate : level.index_textures) {
    if (candidate.name != name || !has_provenance(candidate, provenance)) {
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

bool prepared_output_is_valid(const Jak2HighresJakClutDefaults::PreparedOutput& output) {
  if (output.width == 0 || output.height == 0 ||
      output.width > std::numeric_limits<std::size_t>::max() / output.height) {
    return false;
  }
  const std::size_t pixels =
      static_cast<std::size_t>(output.width) * static_cast<std::size_t>(output.height);
  return pixels <= std::numeric_limits<std::size_t>::max() / 4 && output.rgba.size() == pixels * 4;
}

bool valid_registry_only_tbp(u32 tbp) {
  return tbp == kJak2PrisPrisonJakAnimatorMissingTbp ||
         tbp < kJak2PrisPrisonJakAnimatorTbpUpperBound;
}

}  // namespace

Jak2HighresJakClutDefaults::Jak2HighresJakClutDefaults(id<MTLDevice> device,
                                                       id<MTLCommandQueue> queue,
                                                       TexturePool* texture_pool)
    : m_device(device),
      m_queue(queue),
      m_texture_pool(texture_pool),
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
  detach_pool();
  release_textures();
}

bool Jak2HighresJakClutDefaults::fail(const char* message) {
  m_error = message;
  return false;
}

Jak2HighresJakClutDefaults::GroupState* Jak2HighresJakClutDefaults::group_for_opcode(u16 opcode) {
  if (opcode == kJak2PrisOracleJakAnimatorOpcode) {
    return &m_oracle_group;
  }
  if (opcode == kJak2PrisNestJakAnimatorOpcode) {
    return &m_nest_group;
  }
  return nullptr;
}

bool Jak2HighresJakClutDefaults::initialize(const tfrag3::Level& common_level) {
  m_error.clear();
  const auto group_has_texture = [](const GroupState& group) {
    return std::any_of(group.texture_handles.begin(), group.texture_handles.end(),
                       [](u64 handle) { return handle != 0; });
  };
  if (!m_device || !m_queue || !m_texture_pool || !m_slot_contract_valid ||
      group_has_texture(m_oracle_group) || group_has_texture(m_nest_group)) {
    return fail("invalid high-resolution Jak CLUT default initialization state");
  }

  Jak2PrisPrisonJakAnimatorPlan plan;
  plan.opcode = kJak2PrisNestJakAnimatorOpcode;
  plan.destination_tbp_count = kJak2HighresJakClutBlendSlotCount;
  plan.morph = 0.f;
  plan.destination_tbps.fill(kJak2PrisPrisonJakAnimatorMissingTbp);
  plan.semantic_fingerprint = 1;
  Prepared prepared;
  return prepare(plan, common_level, &prepared) && publish(prepared, plan.opcode);
}

bool Jak2HighresJakClutDefaults::prepare(const Jak2PrisPrisonJakAnimatorPlan& plan,
                                         const tfrag3::Level& common_level,
                                         Prepared* out) {
  m_error.clear();
  const bool oracle = plan.opcode == kJak2PrisOracleJakAnimatorOpcode;
  const bool nest = plan.opcode == kJak2PrisNestJakAnimatorOpcode;
  if (!out || !m_device || !m_queue || !m_texture_pool || !m_slot_contract_valid ||
      (!oracle && !nest) || plan.destination_tbp_count != kJak2HighresJakClutBlendSlotCount ||
      plan.semantic_fingerprint == 0) {
    return fail("invalid high-resolution Jak CLUT preparation state");
  }
  const std::string_view provenance = oracle ? kOracleProvenance : kNestProvenance;
  std::array<Jak2ClutBlendInput, kJak2HighresJakClutBlendSlotCount> inputs;
  Prepared prepared;
  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    const auto& spec = kSourceSpecs[i];
    const u32 destination_tbp = plan.destination_tbps[i];
    if (!valid_registry_only_tbp(destination_tbp)) {
      return fail("invalid high-resolution Jak CLUT destination TBP contract");
    }
    const auto* destination =
        find_consistent_index_texture(common_level, spec.destination, provenance);
    const auto* normal =
        find_consistent_index_texture(common_level, spec.normal_palette, provenance);
    const auto* dark = find_consistent_index_texture(common_level, spec.dark_palette, provenance);
    if (!destination || !normal || !dark) {
      return fail(
          "required high-resolution Jak CLUT source is unavailable, duplicated, or malformed");
    }

    inputs[i].destination = {destination->w, destination->h, destination->index_data};
    copy_palette(*normal, &inputs[i].start_palette);
    copy_palette(*dark, &inputs[i].end_palette);
    prepared[i].destination_tbp = destination_tbp;
    prepared[i].width = destination->w;
    prepared[i].height = destination->h;
  }

  std::array<std::vector<u8>, kJak2HighresJakClutBlendSlotCount> rgba;
  if (!blend_jak2_highres_jak_clut_group_cpu(plan.morph, inputs, rgba)) {
    return fail("high-resolution Jak CLUT CPU composition rejected its inputs");
  }
  for (std::size_t i = 0; i < prepared.size(); ++i) {
    prepared[i].rgba = std::move(rgba[i]);
  }
  *out = std::move(prepared);
  m_stats.preparations++;
  return true;
}

bool Jak2HighresJakClutDefaults::publish(const Prepared& prepared, u16 opcode) {
  m_error.clear();
  GroupState* group = group_for_opcode(opcode);
  if (!group || !m_texture_pool) {
    return fail("invalid high-resolution Jak CLUT publication opcode");
  }
  for (const auto& output : prepared) {
    if (!prepared_output_is_valid(output) || !valid_registry_only_tbp(output.destination_tbp)) {
      return fail("invalid high-resolution Jak CLUT publication state");
    }
  }
  const bool first_publication =
      std::all_of(group->texture_handles.begin(), group->texture_handles.end(),
                  [](u64 handle) { return handle == 0; });
  const bool complete_publication =
      std::all_of(group->texture_handles.begin(), group->texture_handles.end(),
                  [](u64 handle) { return handle != 0; });
  if (!first_publication && !complete_publication) {
    return fail("high-resolution Jak CLUT registry handle set is incomplete");
  }

  if (first_publication) {
    std::array<u64, kJak2HighresJakClutBlendSlotCount> created = {};
    for (std::size_t i = 0; i < created.size(); ++i) {
      created[i] = metal_upload_texture_rgba8(m_device, m_queue, prepared[i].rgba.data(),
                                              prepared[i].width, prepared[i].height);
      if (!created[i]) {
        for (const u64 handle : created) {
          metal_texture_release(handle);
        }
        return fail("initial high-resolution Jak CLUT Metal publication failed");
      }
    }
    group->texture_handles = created;
  } else {
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      id<MTLTexture> current = metal_texture_lookup(group->texture_handles[i]);
      if (!current || current.width != prepared[i].width || current.height != prepared[i].height ||
          current.pixelFormat != MTLPixelFormatRGBA8Unorm) {
        return fail("existing high-resolution Jak CLUT Metal texture shape changed");
      }
    }
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      if (!metal_update_texture_rgba8(group->texture_handles[i], m_queue, prepared[i].rgba.data(),
                                      prepared[i].width, prepared[i].height)) {
        return fail("high-resolution Jak CLUT Metal update failed");
      }
    }
  }

  for (std::size_t i = 0; i < kSourceSpecs.size(); ++i) {
    m_animated_texture_slots[kSourceSpecs[i].animated_slot] = group->texture_handles[i];
    m_stats.destination_tbps[i] = prepared[i].destination_tbp;
    m_stats.texture_handles[i] = group->texture_handles[i];
  }

  {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    for (std::size_t i = 0; i < prepared.size(); ++i) {
      const u32 tbp = prepared[i].destination_tbp;
      if (tbp == kJak2PrisPrisonJakAnimatorMissingTbp) {
        continue;
      }
      if (!group->pool_textures[i]) {
        TextureInput input;
        input.debug_page_name = "PC-ANIM";
        input.debug_name =
            opcode == kJak2PrisOracleJakAnimatorOpcode ? "oracle-jak-clut" : "nest-jak-clut";
        input.id = m_texture_pool->allocate_pc_port_texture(GameVersion::Jak2);
        input.gpu_texture = group->texture_handles[i];
        input.w = prepared[i].width;
        input.h = prepared[i].height;
        group->pool_texture_ids[i] = input.id;
        group->pool_textures[i] = m_texture_pool->give_texture_and_load_to_vram(input, tbp);
      } else {
        m_texture_pool->move_existing_to_vram(group->pool_textures[i], tbp);
      }
    }
  }
  m_stats.last_opcode = opcode;
  m_stats.publications++;
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

void Jak2HighresJakClutDefaults::detach_pool() {
  if (!m_texture_pool) {
    return;
  }
  std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
  for (GroupState* group : {&m_oracle_group, &m_nest_group}) {
    for (std::size_t i = 0; i < group->pool_textures.size(); ++i) {
      if (group->pool_textures[i] && group->texture_handles[i]) {
        m_texture_pool->unload_texture(group->pool_texture_ids[i], group->texture_handles[i]);
      }
      group->pool_textures[i] = nullptr;
    }
  }
  m_texture_pool = nullptr;
}

void Jak2HighresJakClutDefaults::release_textures() {
  for (GroupState* group : {&m_oracle_group, &m_nest_group}) {
    for (u64& handle : group->texture_handles) {
      metal_texture_release(handle);
      handle = 0;
    }
  }
  std::fill(m_animated_texture_slots.begin(), m_animated_texture_slots.end(), 0);
}

}  // namespace metal_renderer
