#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/pipelines/metal/metal_jak2_map_texture_upload_plan.h"
#include "game/graphics/pipelines/metal/metal_pool_texture.h"

#import <Metal/Metal.h>

class TexturePool;

namespace metal_renderer {

// bigmap.gc uploads every indexed layer to the same TBP; each layer is sampled by the sprite draw
// that immediately follows it.
constexpr u32 kJak2MapLayerVramSlot = 8;

enum class Jak2MapLayerExecutorError : u8 {
  None,
  Detached,
  PlanNotPresent,
  PlanNotMixed,
  LayerCount,
  LayerShape,
  OpOrder,
  Decode,
  FrameIncomplete,
  NotPrepared,
  PublishOrder,
  PublishRange,
  Publication,
};

const char* jak2_map_layer_executor_error_name(Jak2MapLayerExecutorError error);

struct Jak2MapLayerExecutorStats {
  u64 prepared_frames = 0;
  u64 completed_frames = 0;
  u64 rejected_prepares = 0;
  u64 rejected_publishes = 0;
  u64 layer_publications = 0;
};

/*!
 * Four persistent MetalPoolTexture identities, one per indexed layer, all published at TBP 8.
 *
 * prepare() decodes all four layers to owned RGBA on the CPU and touches neither the pool nor the
 * GPU. publish_layer(i) publishes layer i, in strict 0..3 order, once per prepared frame; between
 * publications the host replays the sprite draw that samples TBP 8. Ordinary uploads in the same
 * plan remain with the host caller.
 *
 * GPU lifetime: each publication replaces the registry entry for that layer's stable handle with a
 * new MTLTexture (metal_update_texture_rgba8). Draws already encoded keep the previous object only
 * if their command buffer retains references ([queue commandBuffer] default). Callers using
 * commandBufferWithUnretainedReferences must keep prior frames alive themselves.
 *
 * Frame boundary: a new prepare() requires the previous prepared frame to have published all four
 * layers, or an explicit abandon_frame().
 *
 * The TexturePool must outlive this executor, or detach_pool() must be called while it is alive.
 */
class Jak2MapLayerExecutor {
 public:
  Jak2MapLayerExecutor(id<MTLDevice> device, id<MTLCommandQueue> queue, TexturePool* pool);
  ~Jak2MapLayerExecutor();

  Jak2MapLayerExecutor(const Jak2MapLayerExecutor&) = delete;
  Jak2MapLayerExecutor& operator=(const Jak2MapLayerExecutor&) = delete;

  bool prepare(const Jak2MapTextureUploadTypedPlan& plan);
  bool publish_layer(std::size_t layer_index);
  void abandon_frame();
  void detach_pool();

  bool prepared() const { return m_prepared; }
  bool frame_complete() const { return m_prepared && m_next_layer == kJak2MapIndexedLayerMaximum; }
  std::size_t next_layer() const { return m_next_layer; }
  u64 layer_handle(std::size_t layer_index) const;
  u64 layer_publications(std::size_t layer_index) const;
  const std::vector<u32>& prepared_rgba(std::size_t layer_index) const;
  const Jak2MapLayerExecutorStats& stats() const { return m_stats; }
  Jak2MapLayerExecutorError last_error() const { return m_last_error; }

 private:
  bool reject_prepare(Jak2MapLayerExecutorError error);
  bool reject_publish(Jak2MapLayerExecutorError error);

  TexturePool* m_pool = nullptr;
  std::array<std::unique_ptr<MetalPoolTexture>, kJak2MapIndexedLayerMaximum> m_layers;
  std::array<std::vector<u32>, kJak2MapIndexedLayerMaximum> m_rgba;
  bool m_prepared = false;
  std::size_t m_next_layer = 0;
  Jak2MapLayerExecutorStats m_stats;
  Jak2MapLayerExecutorError m_last_error = Jak2MapLayerExecutorError::None;
};

}  // namespace metal_renderer
