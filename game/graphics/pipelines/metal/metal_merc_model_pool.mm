#include "metal_merc_model_pool.h"

#include <algorithm>

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/util/compress.h"

#include "game/graphics/pipelines/metal/metal_texture.h"
#include "game/graphics/texture/TexturePool.h"

#include "fmt/format.h"

MetalMercModelPool& metal_merc_models() {
  static MetalMercModelPool pool;
  return pool;
}

void MetalMercModelPool::init(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* pool) {
  m_device = device;
  m_queue = queue;
  m_texture_pool = pool;
}

bool MetalMercModelPool::load_fr3(const std::string& path,
                                  bool is_common,
                                  LoadResult* out,
                                  std::string* error) {
  if (!ready() || !m_texture_pool) {
    *error = "merc model pool has no device yet (make_display must run first)";
    return false;
  }
  if (!fs::exists(path)) {
    *error = fmt::format("{} does not exist", path);
    return false;
  }

  auto compressed = file_util::read_binary_file(path);
  auto decomp = compression::decompress_zstd(compressed.data(), compressed.size());
  u16 version = 0;
  memcpy(&version, decomp.data(), 2);
  if (version != tfrag3::TFRAG3_VERSION) {
    *error = fmt::format("fr3 version {} does not match this build's {}", version,
                         tfrag3::TFRAG3_VERSION);
    return false;
  }

  auto level = std::make_unique<tfrag3::Level>();
  Serializer ser(decomp.data(), decomp.size());
  level->serialize(ser);
  return add_level(std::move(level), is_common, out, error);
}

bool MetalMercModelPool::add_level(std::unique_ptr<tfrag3::Level> level,
                                   bool is_common,
                                   LoadResult* out,
                                   std::string* error) {
  if (!ready() || !m_texture_pool) {
    *error = "merc model pool has no device yet (make_display must run first)";
    return false;
  }

  auto entry = std::make_unique<MetalMercLevel>();
  entry->level = std::move(level);
  entry->name = entry->level->level_name;

  // textures, the way the GL loader's TextureLoaderStage / load_common do
  metal_add_textures(m_device, m_queue, *m_texture_pool, entry->level->textures, is_common,
                     &entry->textures);

  // merc geometry: the GL MercLoaderStage's two buffers
  const auto& merc = entry->level->merc_data;
  if (!merc.vertices.empty()) {
    entry->vertices = [m_device newBufferWithBytes:merc.vertices.data()
                                            length:merc.vertices.size() * sizeof(tfrag3::MercVertex)
                                           options:MTLResourceStorageModeShared];
  }
  if (!merc.indices.empty()) {
    entry->indices = [m_device newBufferWithBytes:merc.indices.data()
                                           length:merc.indices.size() * sizeof(u32)
                                          options:MTLResourceStorageModeShared];
  }

  // Palette requirements are immutable level metadata. Computing them here
  // keeps the per-model packet path to a few fixed-mask operations and makes
  // their lifetime follow the level across unload/reload.
  entry->required_bone_slots_by_model.resize(merc.models.size());
  for (size_t model_idx = 0; model_idx < merc.models.size(); model_idx++) {
    const auto& model = merc.models[model_idx];
    auto& effect_masks = entry->required_bone_slots_by_model[model_idx];
    effect_masks.resize(model.effects.size());
    for (size_t effect_idx = 0; effect_idx < model.effects.size(); effect_idx++) {
      auto& required_slots = effect_masks[effect_idx];
      for (const auto& draw : model.effects[effect_idx].all_draws) {
        if ((u64)draw.first_index + draw.index_count > merc.indices.size()) {
          continue;
        }
        for (u32 index_offset = 0; index_offset < draw.index_count; index_offset++) {
          const u32 vertex_index = merc.indices[draw.first_index + index_offset];
          if (vertex_index == UINT32_MAX || vertex_index >= merc.vertices.size()) {
            continue;
          }
          const auto& vertex = merc.vertices[vertex_index];
          for (int matrix = 0; matrix < 3; matrix++) {
            if (vertex.weights[matrix] > 0.f) {
              const u8 slot = vertex.mats[matrix];
              required_slots[slot / 64] |= 1ull << (slot % 64);
            }
          }
        }
      }
    }
  }

  out->level_name = entry->name;
  out->textures = (int)entry->textures.size();
  out->models = (int)merc.models.size();
  out->vertices = (u32)merc.vertices.size();
  out->indices = (u32)merc.indices.size();

  const MetalMercLevel* lev = entry.get();
  for (size_t model_idx = 0; model_idx < merc.models.size(); model_idx++) {
    const auto& model = merc.models[model_idx];
    const auto& effect_masks = entry->required_bone_slots_by_model[model_idx];
    m_by_name[model.name].push_back(Ref{&model, lev, &effect_masks});
  }
  m_levels.push_back(std::move(entry));
  return true;
}

bool MetalMercModelPool::remove_level(const std::string& name) {
  auto it = std::find_if(m_levels.begin(), m_levels.end(),
                         [&](const auto& lev) { return lev->name == name; });
  if (it == m_levels.end()) {
    return false;
  }
  MetalMercLevel* lev = it->get();
  for (const auto& model : lev->level->merc_data.models) {
    auto refs = m_by_name.find(model.name);
    if (refs == m_by_name.end()) {
      continue;
    }
    auto& list = refs->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const Ref& ref) { return ref.level == lev; }),
               list.end());
    if (list.empty()) {
      m_by_name.erase(refs);
    }
  }
  {
    std::lock_guard<std::mutex> pool_lock(m_texture_pool->mutex());
    for (size_t i = 0; i < lev->level->textures.size() && i < lev->textures.size(); i++) {
      const auto& tex = lev->level->textures[i];
      if (tex.load_to_pool && lev->textures[i]) {
        m_texture_pool->unload_texture(PcTextureId::from_combo_id(tex.combo_id), lev->textures[i]);
      }
    }
  }
  for (u64 handle : lev->textures) {
    if (handle) {
      metal_texture_release(handle);
    }
  }
  m_levels.erase(it);
  return true;
}

std::optional<MetalMercModelPool::Ref> MetalMercModelPool::get_merc_model(const char* name) const {
  auto it = m_by_name.find(name);
  if (it == m_by_name.end() || it->second.empty()) {
    return std::nullopt;
  }
  return it->second.front();
}
