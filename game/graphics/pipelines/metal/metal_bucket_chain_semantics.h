#pragma once

#include "common/dma/dma.h"
#include "common/versions/versions.h"

namespace metal_renderer {

enum class MetalBucketChainLayout {
  Jak1DefaultRegs,
  Jak2Direct,
  Unsupported,
};

constexpr MetalBucketChainLayout bucket_chain_layout(GameVersion version) {
  switch (version) {
    case GameVersion::Jak1:
      return MetalBucketChainLayout::Jak1DefaultRegs;
    case GameVersion::Jak2:
      return MetalBucketChainLayout::Jak2Direct;
    case GameVersion::Jak3:
    case GameVersion::JakX:
      return MetalBucketChainLayout::Unsupported;
  }
}

inline bool is_strict_empty_bucket_tag(MetalBucketChainLayout layout, const DmaTag& tag) {
  return layout == MetalBucketChainLayout::Jak2Direct && tag.kind == DmaTag::Kind::CNT &&
         tag.qwc == 0;
}

}  // namespace metal_renderer
