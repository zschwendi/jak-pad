#include "game/graphics/pipelines/metal/metal_jak2_map_layer_executor.h"

#include <span>
#include <string>
#include <utility>

#include "game/graphics/pipelines/metal/metal_jak2_map_layer_convert.h"

namespace metal_renderer {

const char* jak2_map_layer_executor_error_name(Jak2MapLayerExecutorError error) {
  switch (error) {
    case Jak2MapLayerExecutorError::None:
      return "none";
    case Jak2MapLayerExecutorError::Detached:
      return "detached";
    case Jak2MapLayerExecutorError::PlanNotPresent:
      return "plan-not-present";
    case Jak2MapLayerExecutorError::PlanNotMixed:
      return "plan-not-mixed";
    case Jak2MapLayerExecutorError::LayerCount:
      return "layer-count";
    case Jak2MapLayerExecutorError::LayerShape:
      return "layer-shape";
    case Jak2MapLayerExecutorError::OpOrder:
      return "op-order";
    case Jak2MapLayerExecutorError::Decode:
      return "decode";
    case Jak2MapLayerExecutorError::FrameIncomplete:
      return "frame-incomplete";
    case Jak2MapLayerExecutorError::NotPrepared:
      return "not-prepared";
    case Jak2MapLayerExecutorError::PublishOrder:
      return "publish-order";
    case Jak2MapLayerExecutorError::PublishRange:
      return "publish-range";
    case Jak2MapLayerExecutorError::Publication:
      return "publication";
  }
  return "unknown";
}

Jak2MapLayerExecutor::Jak2MapLayerExecutor(id<MTLDevice> device,
                                           id<MTLCommandQueue> queue,
                                           TexturePool* pool)
    : m_pool(pool) {
  // MetalPoolTexture construction does not touch the pool or allocate GPU memory.
  for (std::size_t i = 0; i < m_layers.size(); i++) {
    m_layers[i] = std::make_unique<MetalPoolTexture>(
        device, queue, pool, kJak2MapIndexedLayerWidth, kJak2MapIndexedLayerHeight,
        kJak2MapLayerVramSlot, "jak2-map-layer-" + std::to_string(i));
  }
}

Jak2MapLayerExecutor::~Jak2MapLayerExecutor() {
  detach_pool();
}

void Jak2MapLayerExecutor::detach_pool() {
  for (auto& layer : m_layers) {
    layer->detach_pool();
  }
  m_pool = nullptr;
  m_prepared = false;
  m_next_layer = 0;
}

bool Jak2MapLayerExecutor::reject_prepare(Jak2MapLayerExecutorError error) {
  m_last_error = error;
  m_stats.rejected_prepares++;
  return false;
}

bool Jak2MapLayerExecutor::reject_publish(Jak2MapLayerExecutorError error) {
  m_last_error = error;
  m_stats.rejected_publishes++;
  return false;
}

bool Jak2MapLayerExecutor::prepare(const Jak2MapTextureUploadTypedPlan& plan) {
  if (!m_pool) {
    return reject_prepare(Jak2MapLayerExecutorError::Detached);
  }
  if (m_prepared && !frame_complete()) {
    return reject_prepare(Jak2MapLayerExecutorError::FrameIncomplete);
  }
  if (!plan.present) {
    return reject_prepare(Jak2MapLayerExecutorError::PlanNotPresent);
  }
  if (!plan.mixed) {
    return reject_prepare(Jak2MapLayerExecutorError::PlanNotMixed);
  }
  if (plan.layer_count != kJak2MapIndexedLayerMaximum) {
    return reject_prepare(Jak2MapLayerExecutorError::LayerCount);
  }
  for (const auto& layer : plan.layers) {
    if (layer.width != kJak2MapIndexedLayerWidth || layer.height != kJak2MapIndexedLayerHeight ||
        layer.indices.size() != kJak2MapIndexedLayerIndexBytes) {
      return reject_prepare(Jak2MapLayerExecutorError::LayerShape);
    }
  }

  // Exactly layers 0..3 in DMA order, then ordinary groups 0..ordinary_count-1; ordinary uploads
  // stay with the host.
  if (plan.op_count > plan.ops.size() || plan.ordinary_count > plan.ordinary.size()) {
    return reject_prepare(Jak2MapLayerExecutorError::OpOrder);
  }
  if (plan.op_count != kJak2MapIndexedLayerMaximum + plan.ordinary_count) {
    return reject_prepare(Jak2MapLayerExecutorError::OpOrder);
  }
  for (std::size_t i = 0; i < plan.op_count; i++) {
    const auto& op = plan.ops[i];
    const bool layer_op = i < kJak2MapIndexedLayerMaximum;
    const auto kind =
        layer_op ? Jak2MapUploadOpKind::AnimationLayer : Jak2MapUploadOpKind::OrdinaryUpload;
    const std::size_t index = layer_op ? i : i - kJak2MapIndexedLayerMaximum;
    if (op.kind != kind || op.index != index) {
      return reject_prepare(Jak2MapLayerExecutorError::OpOrder);
    }
  }

  // Decode everything before replacing any held state, so a failure leaves the previous frame.
  std::array<std::vector<u32>, kJak2MapIndexedLayerMaximum> decoded;
  for (std::size_t i = 0; i < decoded.size(); i++) {
    const auto& layer = plan.layers[i];
    auto converted = convert_jak2_map_psmt8_to_rgba(
        std::span<const u8>(layer.indices), std::span<const u32>(layer.clut), layer.width,
        layer.height);
    if (!converted.valid ||
        converted.rgba.size() != static_cast<std::size_t>(layer.width) * layer.height) {
      return reject_prepare(Jak2MapLayerExecutorError::Decode);
    }
    decoded[i] = std::move(converted.rgba);
  }

  m_rgba = std::move(decoded);
  m_prepared = true;
  m_next_layer = 0;
  m_last_error = Jak2MapLayerExecutorError::None;
  m_stats.prepared_frames++;
  return true;
}

bool Jak2MapLayerExecutor::publish_layer(std::size_t layer_index) {
  if (!m_pool) {
    return reject_publish(Jak2MapLayerExecutorError::Detached);
  }
  if (!m_prepared) {
    return reject_publish(Jak2MapLayerExecutorError::NotPrepared);
  }
  if (layer_index >= kJak2MapIndexedLayerMaximum) {
    return reject_publish(Jak2MapLayerExecutorError::PublishRange);
  }
  if (layer_index != m_next_layer) {
    return reject_publish(Jak2MapLayerExecutorError::PublishOrder);
  }
  const auto& rgba = m_rgba[layer_index];
  if (!m_layers[layer_index]->publish_at(rgba.data(), rgba.size(), kJak2MapLayerVramSlot)) {
    return reject_publish(Jak2MapLayerExecutorError::Publication);
  }
  m_next_layer++;
  m_stats.layer_publications++;
  if (m_next_layer == kJak2MapIndexedLayerMaximum) {
    m_stats.completed_frames++;
  }
  m_last_error = Jak2MapLayerExecutorError::None;
  return true;
}

void Jak2MapLayerExecutor::abandon_frame() {
  m_prepared = false;
  m_next_layer = 0;
}

u64 Jak2MapLayerExecutor::layer_handle(std::size_t layer_index) const {
  return layer_index < m_layers.size() ? m_layers[layer_index]->handle() : 0;
}

u64 Jak2MapLayerExecutor::layer_publications(std::size_t layer_index) const {
  return layer_index < m_layers.size() ? m_layers[layer_index]->publications() : 0;
}

const std::vector<u32>& Jak2MapLayerExecutor::prepared_rgba(std::size_t layer_index) const {
  static const std::vector<u32> empty;
  return layer_index < m_rgba.size() ? m_rgba[layer_index] : empty;
}

}  // namespace metal_renderer
