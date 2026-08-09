#pragma once

#include <cstddef>
#include <exception>
#include <limits>

#include "common/dma/dma_chain_read.h"
#include "common/dma/dma_chain_validation.h"

#include "game/graphics/pipelines/metal/metal_jak2_bucket_table.h"

namespace metal_renderer {

enum class Jak2MetalChainValidationError {
  None,
  DmaChain,
  BucketArrayOverflow,
  EarlyTermination,
  BoundedFollower,
};

struct Jak2MetalChainValidationResult {
  Jak2MetalChainValidationError error = Jak2MetalChainValidationError::None;
  DmaChainValidationResult dma;
  std::size_t failed_bucket = 0;

  explicit operator bool() const { return error == Jak2MetalChainValidationError::None; }
};

inline const char* jak2_metal_chain_validation_error_message(
    Jak2MetalChainValidationError error) {
  switch (error) {
    case Jak2MetalChainValidationError::None:
      return "valid";
    case Jak2MetalChainValidationError::DmaChain:
      return "the DMA chain is structurally invalid";
    case Jak2MetalChainValidationError::BucketArrayOverflow:
      return "the 327-bucket array overflows the DMA follower offset";
    case Jak2MetalChainValidationError::EarlyTermination:
      return "the DMA chain terminates before all 327 bucket boundaries";
    case Jak2MetalChainValidationError::BoundedFollower:
      return "the bounded DMA follower rejected the certified chain";
  }
  return "unknown Jak 2 Metal chain validation error";
}

inline Jak2MetalChainValidationResult validate_jak2_metal_dma_chain(const void* chain_data,
                                                                     std::size_t chain_size,
                                                                     u32 chain_offset) noexcept {
  Jak2MetalChainValidationResult result;
  result.dma = validate_dma_chain(chain_data, chain_size, chain_offset);
  if (!result.dma) {
    result.error = Jak2MetalChainValidationError::DmaChain;
    return result;
  }

  try {
    DmaFollower dma(chain_data, chain_offset, chain_size);
    for (std::size_t bucket = 0; bucket < kJak2MetalBucketCount; ++bucket) {
      const u64 next_offset =
          static_cast<u64>(chain_offset) + static_cast<u64>(bucket + 1) * 16;
      if (next_offset > std::numeric_limits<u32>::max()) {
        result.error = Jak2MetalChainValidationError::BucketArrayOverflow;
        result.failed_bucket = bucket;
        return result;
      }
      const u32 next_bucket = static_cast<u32>(next_offset);
      while (!dma.ended() && dma.current_tag_offset() != next_bucket) {
        dma.read_and_advance();
      }
      if (dma.ended()) {
        result.error = Jak2MetalChainValidationError::EarlyTermination;
        result.failed_bucket = bucket;
        return result;
      }
    }
  } catch (const std::exception&) {
    result.error = Jak2MetalChainValidationError::BoundedFollower;
  } catch (...) {
    result.error = Jak2MetalChainValidationError::BoundedFollower;
  }
  return result;
}

}  // namespace metal_renderer
