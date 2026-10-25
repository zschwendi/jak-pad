#include "game/graphics/pipelines/metal/metal_immersive_camera.h"

#include <algorithm>
#include <cmath>

#include "game/graphics/opengl_renderer/buckets.h"

namespace metal_renderer {
namespace {

using Matrix4d = std::array<double, 16>;
using Vector4d = std::array<double, 4>;

double& element(Matrix4d& matrix, int column, int row) {
  return matrix[column * 4 + row];
}

double element(const Matrix4d& matrix, int column, int row) {
  return matrix[column * 4 + row];
}

bool finite(const Matrix4d& matrix) {
  return std::all_of(matrix.begin(), matrix.end(),
                     [](double value) { return std::isfinite(value); });
}

// Only the lanes read by game_camera_meters_from_world and shader_clip are validated. The rest are
// not inputs to any derivation, and some are never written by the producer: add-pc-tfrag3-data
// leaves fog.w as stale DMA-buffer bytes, which can be any bit pattern.
bool finite_camera_pose(const ImmersivePacketCamera& packet) {
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(packet.trans[axis]) || !std::isfinite(packet.rot[axis][0]) ||
        !std::isfinite(packet.rot[axis][1]) || !std::isfinite(packet.rot[axis][2])) {
      return false;
    }
  }
  return true;
}

bool finite_camera_projection(const ImmersivePacketCamera& packet) {
  return finite_camera_pose(packet) && std::isfinite(packet.fog[0]) &&
         std::isfinite(packet.hvdf_off[2]) && std::isfinite(packet.perspective[0][0]) &&
         std::isfinite(packet.perspective[1][1]) && std::isfinite(packet.perspective[2][2]) &&
         std::isfinite(packet.perspective[2][3]) && std::isfinite(packet.perspective[3][2]);
}

Matrix4d from_float(const std::array<float, 16>& matrix) {
  Matrix4d out = {};
  std::copy(matrix.begin(), matrix.end(), out.begin());
  return out;
}

bool to_float(const Matrix4d& matrix, std::array<float, 16>* out) {
  if (!out || !finite(matrix)) {
    return false;
  }
  for (std::size_t i = 0; i < matrix.size(); ++i) {
    const float value = static_cast<float>(matrix[i]);
    if (!std::isfinite(value)) {
      return false;
    }
    out->at(i) = value;
  }
  return true;
}

Matrix4d multiply(const Matrix4d& left, const Matrix4d& right) {
  Matrix4d out = {};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      for (int inner = 0; inner < 4; ++inner) {
        element(out, column, row) += element(left, inner, row) * element(right, column, inner);
      }
    }
  }
  return out;
}

bool inverse(const Matrix4d& matrix, Matrix4d* out) {
  if (!out || !finite(matrix)) {
    return false;
  }
  double augmented[4][8] = {};
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      augmented[row][column] = element(matrix, column, row);
    }
    augmented[row][row + 4] = 1.0;
  }

  for (int pivot = 0; pivot < 4; ++pivot) {
    int best = pivot;
    for (int row = pivot + 1; row < 4; ++row) {
      if (std::abs(augmented[row][pivot]) > std::abs(augmented[best][pivot])) {
        best = row;
      }
    }
    if (!std::isfinite(augmented[best][pivot]) || std::abs(augmented[best][pivot]) < 1e-12) {
      return false;
    }
    if (best != pivot) {
      for (int column = 0; column < 8; ++column) {
        std::swap(augmented[pivot][column], augmented[best][column]);
      }
    }
    const double divisor = augmented[pivot][pivot];
    for (double& value : augmented[pivot]) {
      value /= divisor;
    }
    for (int row = 0; row < 4; ++row) {
      if (row == pivot) {
        continue;
      }
      const double factor = augmented[row][pivot];
      for (int column = 0; column < 8; ++column) {
        augmented[row][column] -= factor * augmented[pivot][column];
      }
    }
  }

  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      element(*out, column, row) = augmented[row][column + 4];
    }
  }
  return finite(*out);
}

Vector4d shader_clip(const ImmersivePacketCamera& packet,
                     float scissor_adjust,
                     const Vector4d& world) {
  const double fog_scale = packet.fog[0];
  const double pxx = packet.perspective[0][0];
  const double pyy = packet.perspective[1][1];
  const double pzz = packet.perspective[2][2];
  const double pzw = packet.perspective[2][3];
  const double pwz = packet.perspective[3][2];
  const std::array<double, 4> scale = {
      fog_scale * pxx / 256.0,
      -fog_scale * pyy / 128.0,
      fog_scale * pzz / 8388608.0 + (packet.hvdf_off[2] / 8388608.0 - 1.0) * pzw,
      0.0,
  };

  double pc_camera[4][4] = {};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      pc_camera[row][column] = packet.rot[row][column] * scale[column];
    }
    pc_camera[row][3] = packet.rot[row][2] * pzw;
  }
  pc_camera[3][2] = fog_scale * pwz / 8388608.0;

  const Vector4d relative = {
      world[0] - packet.trans[0] * world[3],
      world[1] - packet.trans[1] * world[3],
      world[2] - packet.trans[2] * world[3],
      0.0,
  };
  Vector4d out = {};
  for (int lane = 0; lane < 4; ++lane) {
    out[lane] = -pc_camera[3][lane] * world[3];
  }
  out[3] = 0.0;
  for (int column = 0; column < 3; ++column) {
    for (int lane = 0; lane < 4; ++lane) {
      out[lane] -= pc_camera[column][lane] * relative[column];
    }
  }
  out[1] *= scissor_adjust;
  out[2] = (out[2] + out[3]) * 0.5;
  return out;
}

Matrix4d game_clip_from_world(const ImmersivePacketCamera& packet, float scissor_adjust) {
  Matrix4d out = {};
  for (int column = 0; column < 4; ++column) {
    Vector4d basis = {};
    basis[column] = 1.0;
    const auto clip = shader_clip(packet, scissor_adjust, basis);
    for (int row = 0; row < 4; ++row) {
      element(out, column, row) = clip[row];
    }
  }
  return out;
}

Matrix4d game_camera_meters_from_world(const ImmersivePacketCamera& packet) {
  Matrix4d out = {};
  // Jak's math-camera uses the clip-equivalent left/up/forward basis: real packet-local +Z is in
  // front of the camera, and packet-local +X projects toward screen-left. CompositorServices'
  // rightUpBack contract uses the opposite X and Z axes. Keep this conversion in the shared
  // game-camera basis so rendering and binocular culling cannot disagree.
  constexpr std::array<double, 3> kRightUpBackFromJakCamera = {-1.0, 1.0, -1.0};
  for (int axis = 0; axis < 3; ++axis) {
    for (int world_axis = 0; world_axis < 3; ++world_axis) {
      element(out, world_axis, axis) =
          kRightUpBackFromJakCamera[axis] * packet.rot[world_axis][axis] / kJakWorldUnitsPerMeter;
    }
    double translated = 0.0;
    for (int world_axis = 0; world_axis < 3; ++world_axis) {
      translated -= kRightUpBackFromJakCamera[axis] * packet.rot[world_axis][axis] *
                    packet.trans[world_axis] / kJakWorldUnitsPerMeter;
    }
    element(out, 3, axis) = translated;
  }
  element(out, 3, 3) = 1.0;
  return out;
}

void set_error(ImmersiveCameraError value, ImmersiveCameraError* error) {
  if (error) {
    *error = value;
  }
}

bool same_contract(const ImmersiveCameraContract& left, const ImmersiveCameraContract& right) {
  return left.view.enabled == right.view.enabled &&
         left.view.clip_from_eye == right.view.clip_from_eye &&
         left.view.eye_from_game_camera_meters == right.view.eye_from_game_camera_meters &&
         left.binocular_cull.enabled == right.binocular_cull.enabled &&
         left.binocular_cull.clip_from_game_camera_meters ==
             right.binocular_cull.clip_from_game_camera_meters;
}

}  // namespace

const char* immersive_camera_error_name(ImmersiveCameraError error) {
  switch (error) {
    case ImmersiveCameraError::None:
      return "none";
    case ImmersiveCameraError::DisabledContract:
      return "disabled-contract";
    case ImmersiveCameraError::PacketUnavailable:
      return "packet-unavailable";
    case ImmersiveCameraError::PacketInconsistent:
      return "current-chain-camera-packets-inconsistent";
    case ImmersiveCameraError::DmaChainInvalid:
      return "current-chain-invalid";
    case ImmersiveCameraError::WorldPacketUnavailableThisFrame:
      return "world-camera-packet-unavailable-this-frame";
    case ImmersiveCameraError::UnsupportedGameVersion:
      return "physical-camera-unsupported-game-version";
    case ImmersiveCameraError::ExternalStereoStateInvalid:
      return "external-stereo-state-invalid";
    case ImmersiveCameraError::StereoContractInvalid:
      return "physical-stereo-contract-invalid";
    case ImmersiveCameraError::NonFiniteInput:
      return "nonfinite-input";
    case ImmersiveCameraError::SingularGameCamera:
      return "singular-game-camera";
    case ImmersiveCameraError::SingularGameClip:
      return "singular-game-clip";
    case ImmersiveCameraError::DegenerateCullPlane:
      return "degenerate-cull-plane";
    case ImmersiveCameraError::NonFiniteResult:
      return "nonfinite-result";
  }
  return "unknown";
}

bool jak1_bucket_content_requires_world_camera(int bucket) {
  using BucketId = jak1::BucketId;
  switch (bucket) {
    case static_cast<int>(BucketId::BUCKET0):
    case static_cast<int>(BucketId::BUCKET1):
    case 2:
    case static_cast<int>(BucketId::TFRAG_TEX_LEVEL0):
    case static_cast<int>(BucketId::TFRAG_TEX_LEVEL1):
    case static_cast<int>(BucketId::SHRUB_TEX_LEVEL0):
    case 21:
    case static_cast<int>(BucketId::SHRUB_TEX_LEVEL1):
    case 27:
    case static_cast<int>(BucketId::ALPHA_TEX_LEVEL0):
    case 33:
    case 35:
    case 37:
    case static_cast<int>(BucketId::ALPHA_TEX_LEVEL1):
    case 40:
    case 42:
    case 44:
    case static_cast<int>(BucketId::PRIS_TEX_LEVEL0):
    case static_cast<int>(BucketId::PRIS_TEX_LEVEL1):
    case static_cast<int>(BucketId::MERC_EYES_AFTER_PRIS):
    case static_cast<int>(BucketId::WATER_TEX_LEVEL0):
    case static_cast<int>(BucketId::WATER_TEX_LEVEL1):
    case static_cast<int>(BucketId::DEPTH_CUE):
    case static_cast<int>(BucketId::PRE_SPRITE_TEX):
    case static_cast<int>(BucketId::DEBUG):
    case static_cast<int>(BucketId::DEBUG_NO_ZBUF):
    case static_cast<int>(BucketId::SUBTITLE):
      return false;
    default:
      return true;
  }
}

void ImmersiveStereoFrameCameraCache::clear() {
  m_valid = false;
}

void ImmersiveStereoFrameCameraCache::store(std::uint64_t host_tick_id,
                                            std::uint64_t engine_frame_id,
                                            const ImmersiveCameraContract& left,
                                            const ImmersiveCameraContract& right,
                                            const ImmersiveStereoFrameCamera& camera) {
  m_valid = true;
  m_host_tick_id = host_tick_id;
  m_engine_frame_id = engine_frame_id;
  m_left = left;
  m_right = right;
  m_camera = camera;
}

const ImmersiveStereoFrameCamera* ImmersiveStereoFrameCameraCache::find(
    std::uint64_t host_tick_id,
    std::uint64_t engine_frame_id,
    const ImmersiveCameraContract& left,
    const ImmersiveCameraContract& right) const {
  if (!m_valid || host_tick_id != m_host_tick_id || engine_frame_id != m_engine_frame_id ||
      !same_contract(left, m_left) || !same_contract(right, m_right)) {
    return nullptr;
  }
  return &m_camera;
}

bool packet_clip_from_game_camera_meters(const ImmersivePacketCamera& packet,
                                         float scissor_adjust,
                                         std::array<float, 16>* out,
                                         ImmersiveCameraError* error) {
  set_error(ImmersiveCameraError::None, error);
  if (!out || !finite_camera_projection(packet) || !std::isfinite(scissor_adjust)) {
    set_error(ImmersiveCameraError::NonFiniteInput, error);
    return false;
  }
  const Matrix4d game_camera = game_camera_meters_from_world(packet);
  Matrix4d world_from_game_camera = {};
  if (!inverse(game_camera, &world_from_game_camera)) {
    set_error(ImmersiveCameraError::SingularGameCamera, error);
    return false;
  }
  const Matrix4d projection =
      multiply(game_clip_from_world(packet, scissor_adjust), world_from_game_camera);
  if (!to_float(projection, out)) {
    set_error(ImmersiveCameraError::NonFiniteResult, error);
    return false;
  }
  return true;
}

bool derive_immersive_view_transform(const ImmersivePacketCamera& packet,
                                     float scissor_adjust,
                                     const PhysicalViewCamera& view,
                                     ViewTransform* out,
                                     ImmersiveCameraError* error) {
  set_error(ImmersiveCameraError::None, error);
  if (!view.enabled) {
    set_error(ImmersiveCameraError::DisabledContract, error);
    return false;
  }
  if (!out || !finite_camera_projection(packet) || !std::isfinite(scissor_adjust) ||
      !is_finite_physical_view(view)) {
    set_error(ImmersiveCameraError::NonFiniteInput, error);
    return false;
  }

  const Matrix4d source = game_clip_from_world(packet, scissor_adjust);
  Matrix4d world_from_game_clip = {};
  if (!inverse(source, &world_from_game_clip)) {
    set_error(ImmersiveCameraError::SingularGameClip, error);
    return false;
  }
  const Matrix4d target = multiply(
      multiply(from_float(view.clip_from_eye), from_float(view.eye_from_game_camera_meters)),
      game_camera_meters_from_world(packet));
  const Matrix4d post_clip = multiply(target, world_from_game_clip);
  if (!to_float(post_clip, &out->clip_from_game_clip) || !is_finite(*out)) {
    set_error(ImmersiveCameraError::NonFiniteResult, error);
    return false;
  }
  return true;
}

bool derive_immersive_cull_planes(const ImmersivePacketCamera& packet,
                                  const PhysicalBinocularCull& cull,
                                  ImmersiveCullPlanes* out,
                                  ImmersiveCameraError* error) {
  set_error(ImmersiveCameraError::None, error);
  if (!cull.enabled) {
    set_error(ImmersiveCameraError::DisabledContract, error);
    return false;
  }
  if (!out || !finite_camera_pose(packet) || !is_finite_binocular_cull(cull)) {
    set_error(ImmersiveCameraError::NonFiniteInput, error);
    return false;
  }

  const Matrix4d clip_from_world = multiply(from_float(cull.clip_from_game_camera_meters),
                                            game_camera_meters_from_world(packet));
  if (!finite(clip_from_world)) {
    set_error(ImmersiveCameraError::NonFiniteResult, error);
    return false;
  }

  const std::array<std::array<double, 2>, 4> combinations = {{
      {{1.0, 1.0}},   // left: w + x
      {{1.0, -1.0}},  // right: w - x
      {{1.0, 1.0}},   // bottom: w + y
      {{1.0, -1.0}},  // top: w - y
  }};
  for (int plane = 0; plane < 4; ++plane) {
    const int axis = plane < 2 ? 0 : 1;
    const double w_factor = combinations[plane][0];
    const double axis_factor = combinations[plane][1];
    std::array<double, 4> equation = {};
    for (int column = 0; column < 4; ++column) {
      equation[column] = w_factor * element(clip_from_world, column, 3) +
                         axis_factor * element(clip_from_world, column, axis);
    }
    const double normal_length = std::sqrt(equation[0] * equation[0] + equation[1] * equation[1] +
                                           equation[2] * equation[2]);
    if (!std::isfinite(normal_length) || normal_length < 1e-12) {
      set_error(ImmersiveCameraError::DegenerateCullPlane, error);
      return false;
    }
    for (int coordinate = 0; coordinate < 3; ++coordinate) {
      out->at(coordinate).at(plane) = static_cast<float>(equation[coordinate] / normal_length);
    }
    out->at(3).at(plane) = static_cast<float>(-equation[3] / normal_length);
  }

  for (const auto& coordinate : *out) {
    if (!std::all_of(coordinate.begin(), coordinate.end(),
                     [](float value) { return std::isfinite(value); })) {
      set_error(ImmersiveCameraError::NonFiniteResult, error);
      return false;
    }
  }
  return true;
}

}  // namespace metal_renderer
