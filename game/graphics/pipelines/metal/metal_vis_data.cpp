#include "game/graphics/pipelines/metal/metal_vis_data.h"

#include <cstring>

namespace metal_renderer {
namespace {

bool is_pc_port_payload(const DmaTransfer& transfer, std::size_t expected_size) {
  return transfer.data && transfer.size_bytes == expected_size &&
         transfer.vifcode1().kind == VifCode::Kind::PC_PORT;
}

bool is_boundary(const DmaTransfer& transfer) {
  return transfer.size_bytes == 0;
}

}  // namespace

void MetalVisibilityFrame::reset() {
  for (auto& level : levels) {
    level.valid = false;
  }
  level_count = 0;
  fog_vif0 = 0;
  has_fallback = false;
}

bool decode_metal_visibility_frame(const DmaTransfer* transfers,
                                   std::size_t transfer_count,
                                   std::size_t level_count,
                                   MetalVisibilityFrame* out) {
  if (!out || level_count > kMetalMaxVisibilityLevels) {
    return false;
  }

  const std::size_t visibility_transfer_count = level_count * 2;
  const bool has_fallback = transfer_count == visibility_transfer_count + 2;
  if ((!transfers && transfer_count) ||
      (transfer_count != visibility_transfer_count && !has_fallback)) {
    return false;
  }

  for (std::size_t level = 0; level < level_count; level++) {
    const auto& payload = transfers[level * 2];
    const bool valid_size = payload.size_bytes == kMetalVisibilityBytes ||
                            payload.size_bytes == kMetalInactiveVisibilityBytes;
    if (!payload.data || !valid_size || payload.vifcode1().kind != VifCode::Kind::PC_PORT ||
        !is_boundary(transfers[level * 2 + 1])) {
      return false;
    }
  }

  if (has_fallback &&
      (!is_pc_port_payload(transfers[visibility_transfer_count],
                           kMetalBackgroundFallbackBytes) ||
       !is_boundary(transfers[visibility_transfer_count + 1]))) {
    return false;
  }

  out->reset();
  out->level_count = level_count;
  for (std::size_t level = 0; level < level_count; level++) {
    const auto& payload = transfers[level * 2];
    out->fog_vif0 = payload.vif0();
    if (payload.size_bytes == kMetalVisibilityBytes) {
      std::memcpy(out->levels[level].data.data(), payload.data, kMetalVisibilityBytes);
      out->levels[level].valid = true;
    }
  }
  if (has_fallback) {
    std::memcpy(out->fallback.data(), transfers[visibility_transfer_count].data,
                kMetalBackgroundFallbackBytes);
    out->has_fallback = true;
  }
  return true;
}

}  // namespace metal_renderer
