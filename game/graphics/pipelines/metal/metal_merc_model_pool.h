#pragma once

/*!
 * @file metal_merc_model_pool.h
 * Merc model data for the Metal backend. Objective-C++ only.
 *
 * Merc's per-frame DMA carries only control data: the setup packet, lights, the
 * flags quadword and EE pointers to bone matrices. The geometry is not in the
 * chain at all - the GL renderer takes it from
 * `render_state->loader->get_merc_model(name)` (a `tfrag3::MercModel` out of the
 * level's .fr3) plus the level's `merc_vertices` / `merc_indices` GPU buffers,
 * which the GL loader's MercLoaderStage fills
 * (game/graphics/opengl_renderer/loader/LoaderStages.cpp).
 *
 * The Metal path does not run the streaming Loader yet (that Loader is GL-only:
 * its LevelData holds GLuints and it includes glad). This is the merc-scoped
 * equivalent: load an extracted level, upload its textures the way the GL
 * loader's add_texture does, put `merc_data` into two MTLBuffers, and answer
 * model lookups by name. General level-geometry loading is a separate, larger
 * piece of work; the duplication is deliberate and will be unified later.
 */

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

#import <Metal/Metal.h>

class TexturePool;

/*!
 * One extracted level's merc data. The merc-scoped analog of the GL loader's
 * LevelData: the CPU-side level is kept alive because MercModel pointers point
 * into it, the two buffers are what MercLoaderStage puts in merc_vertices /
 * merc_indices, and `textures` is the per-level texture handle array that
 * MercDraw::tree_tex_id indexes.
 */
struct MetalMercLevel {
  std::unique_ptr<tfrag3::Level> level;
  id<MTLBuffer> vertices = nil;
  id<MTLBuffer> indices = nil;
  std::vector<u64> textures;
  std::string name;
};

class MetalMercModelPool {
 public:
  // Must be called before load_fr3. The device/queue upload textures and hold
  // the merc buffers; the pool receives the textures flagged for it.
  void init(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* pool);
  bool ready() const { return m_device != nil; }

  struct LoadResult {
    std::string level_name;
    int textures = 0;
    int models = 0;
    u32 vertices = 0;
    u32 indices = 0;
  };

  // Reads an extracted level (.fr3), uploads its textures and merc geometry.
  // `is_common` marks the shared level (GAME.fr3) for the texture pool, exactly
  // as the GL loader's load_common does.
  bool load_fr3(const std::string& path, bool is_common, LoadResult* out, std::string* error);

  // Same, for a level that is already in memory (the proof builds a synthetic
  // one so the merc path can be checked without any game data).
  bool add_level(std::unique_ptr<tfrag3::Level> level,
                 bool is_common,
                 LoadResult* out,
                 std::string* error);

  struct Ref {
    const tfrag3::MercModel* model = nullptr;
    const MetalMercLevel* level = nullptr;
  };

  // Mirror of Loader::get_merc_model: first match wins.
  std::optional<Ref> get_merc_model(const char* name) const;

  // Releases one level: its model refs leave the lookup, its pool textures are
  // unloaded, and its buffers go with the MetalMercLevel. Must be called on the
  // thread that loads and draws, between frames. Returns false when no such
  // level is loaded.
  bool remove_level(const std::string& name);

  size_t level_count() const { return m_levels.size(); }
  size_t model_count() const { return m_by_name.size(); }

 private:
  id<MTLDevice> m_device = nil;
  id<MTLCommandQueue> m_queue = nil;
  TexturePool* m_texture_pool = nullptr;
  std::vector<std::unique_ptr<MetalMercLevel>> m_levels;
  std::unordered_map<std::string, std::vector<Ref>> m_by_name;
};

// The process-wide merc model pool, the way the texture registry in
// metal_texture.h is process-wide: levels are loaded once and every merc bucket
// renderer reads from the same place.
MetalMercModelPool& metal_merc_models();
