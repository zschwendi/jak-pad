#pragma once

#include "common/common_types.h"

namespace metal_renderer {

/*
 * Shared normal-path transfer capacity for bucket 195 capture and plan. Derived from the existing
 * 128-batch limit and the strict grammar: 12 fixed transfers (prefix 6 + tail 6), one optional
 * setup transfer, and per batch 2 vertex uploads plus at most 4 single-tag commands
 * (shadow.cpp add_verts + four emitters; plan.cpp max-4 order). 12 + 1 + 128 * 6 = 781.
 */
constexpr u32 kJak2ShadowBucket195SharedMaximumBatches = 128;
constexpr u32 kJak2ShadowBucket195SharedFixedTransfers = 12;
constexpr u32 kJak2ShadowBucket195SharedOptionalSetupTransfers = 1;
constexpr u32 kJak2ShadowBucket195SharedVertexUploadsPerBatch = 2;
constexpr u32 kJak2ShadowBucket195SharedMaximumCommandsPerBatch = 4;
constexpr u32 kJak2ShadowBucket195SharedMaximumTransfers =
    kJak2ShadowBucket195SharedFixedTransfers + kJak2ShadowBucket195SharedOptionalSetupTransfers +
    kJak2ShadowBucket195SharedMaximumBatches *
        (kJak2ShadowBucket195SharedVertexUploadsPerBatch +
         kJak2ShadowBucket195SharedMaximumCommandsPerBatch);
static_assert(kJak2ShadowBucket195SharedMaximumTransfers == 781);

}  // namespace metal_renderer
