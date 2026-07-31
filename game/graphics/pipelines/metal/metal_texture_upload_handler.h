#pragma once

/*!
 * @file metal_texture_upload_handler.h
 * Metal port of the TextureUploadHandler bucket renderer
 * (game/graphics/opengl_renderer/TextureUploadHandler.cpp).
 *
 * Walks a texture bucket's DMA data, collects the PC-port texture upload
 * packets, and applies them to the TexturePool - pure bookkeeping, no GPU work
 * (the textures themselves are preconverted and uploaded by the loader).
 *
 * Not yet ported from the GL handler (both are Jak 2/3-and-eyes territory,
 * stage 4+): the eye-renderer DMA and the texture-animator PC_PORT packets.
 * Both are counted in Stats so callers can see when they occur; their transfers
 * are consumed by the generic loop without side effects.
 *
 * Plain C++ so it can be exercised by tests without Objective-C++.
 */

#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/texture/TexturePool.h"

class MetalTextureUploadHandler {
 public:
  struct Stats {
    int uploads = 0;
    int skipped_eye_dma = 0;
    int skipped_texture_anim = 0;
  };

  // Processes one texture bucket: dma should be positioned at the bucket start,
  // next_bucket is the tag offset where the bucket ends.
  Stats process(DmaFollower& dma,
                u32 next_bucket,
                TexturePool& pool,
                const u8* ee_memory,
                u32 s7_ptr);

 private:
  struct TextureUpload {
    u64 page;
    s64 mode;
  };
  void flush_uploads(std::vector<TextureUpload>& uploads,
                     TexturePool& pool,
                     const u8* ee_memory,
                     u32 s7_ptr,
                     Stats& stats);
};
