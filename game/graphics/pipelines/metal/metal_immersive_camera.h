#pragma once

#include <array>
#include <cstdint>

#include "game/graphics/pipelines/metal/metal_view_transform.h"

namespace metal_renderer {

inline constexpr float kJakWorldUnitsPerMeter = 4096.f;
inline constexpr float kJak1MetalScissorAdjust = 512.f / 448.f;

// Renderer-independent copy of the current background packet fields that determine vertex
// position. Keeping this plain C++ makes the physical-camera contract testable without Metal.
struct ImmersivePacketCamera {
  std::array<float, 4> hvdf_off = {};
  std::array<float, 4> fog = {};
  std::array<float, 4> trans = {};
  std::array<std::array<float, 4>, 4> rot = {};
  std::array<std::array<float, 4>, 4> perspective = {};
};

// Four normalized side planes in the layout consumed by metal_sphere_in_view_ref:
// planes[0..2][lane] * xyz - planes[3][lane] > -radius.
using ImmersiveCullPlanes = std::array<std::array<float, 4>, 4>;

enum class ImmersiveCameraError {
  None,
  DisabledContract,
  PacketUnavailable,
  PacketInconsistent,
  DmaChainInvalid,
  WorldPacketUnavailableThisFrame,
  UnsupportedGameVersion,
  ExternalStereoStateInvalid,
  StereoContractInvalid,
  NonFiniteInput,
  SingularGameCamera,
  SingularGameClip,
  DegenerateCullPlane,
  NonFiniteResult,
};

const char* immersive_camera_error_name(ImmersiveCameraError error);

// True when nonempty content in this Jak 1 bucket can contain a World-space draw. Unknown and
// mixed buckets are conservative; only texture, empty, deferred, and explicit Screen buckets are
// exempt.
bool jak1_bucket_content_requires_world_camera(int bucket);

struct ImmersiveStereoFrameCamera {
  ViewTransform left;
  ViewTransform right;
  ImmersiveCullPlanes binocular_cull = {};
  std::uint64_t packet_fingerprint = 0;
  int packet_count = 0;
  // Source sky-tng geometry generated for the head (sky_generation.h) replays with the same
  // original packet and eye_from_game_camera_meters = E * H. World draws keep left/right.
  ViewTransform left_sky;
  ViewTransform right_sky;
  bool sky_valid = false;
  std::uint64_t sky_epoch = 0;
  // An H-enabled frame was requested when this camera was prepared, even if no usable pair
  // could be derived. Forbids ordinary replay of generated sky for every user of this camera.
  bool sky_requested = false;
};

// A camera may be decoded in one DMA chain and consumed by a later World-only chain. The key is
// deliberately strict: no previous host tick, engine frame, or changed physical contract can be
// reused as a fallback.
class ImmersiveStereoFrameCameraCache {
 public:
  void clear();
  void store(std::uint64_t host_tick_id,
             std::uint64_t engine_frame_id,
             const ImmersiveCameraContract& left,
             const ImmersiveCameraContract& right,
             const ImmersiveStereoFrameCamera& camera);
  const ImmersiveStereoFrameCamera* find(std::uint64_t host_tick_id,
                                         std::uint64_t engine_frame_id,
                                         const ImmersiveCameraContract& left,
                                         const ImmersiveCameraContract& right) const;

 private:
  bool m_valid = false;
  std::uint64_t m_host_tick_id = 0;
  std::uint64_t m_engine_frame_id = 0;
  ImmersiveCameraContract m_left;
  ImmersiveCameraContract m_right;
  ImmersiveStereoFrameCamera m_camera;
};

// Reconstructs the packet's projection after converting Jak's left/up/forward camera coordinates
// to the host's right/up/back game-camera meter basis. This is primarily useful for deterministic
// equivalence tests; production supplies clip_from_eye from the current compositor drawable.
bool packet_clip_from_game_camera_meters(const ImmersivePacketCamera& packet,
                                         float scissor_adjust,
                                         std::array<float, 16>* out,
                                         ImmersiveCameraError* error = nullptr);

// Derives one shader post-transform from the physical view and this exact DMA packet camera.
bool derive_immersive_view_transform(const ImmersivePacketCamera& packet,
                                     float scissor_adjust,
                                     const PhysicalViewCamera& view,
                                     ViewTransform* out,
                                     ImmersiveCameraError* error = nullptr);

// Derives one conservative, shared, normalized binocular frustum in Jak world units. The result
// is for CPU visibility only and must not be used for rendering output.
bool derive_immersive_cull_planes(const ImmersivePacketCamera& packet,
                                  const PhysicalBinocularCull& cull,
                                  ImmersiveCullPlanes* out,
                                  ImmersiveCameraError* error = nullptr);

}  // namespace metal_renderer
