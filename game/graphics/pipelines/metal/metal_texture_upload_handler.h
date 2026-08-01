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
 * Eye-renderer DMA rides in these buckets (the GL handler forwards it to
 * EyeRenderer::handle_eye_dma2). The bucket adapter passes eye_dma_handler for
 * that; without one the eye data is consumed, counted and logged instead.
 * The texture-animator PC_PORT packets are Jak 2/3 territory and stay
 * unported: counted in Stats, consumed without side effects.
 *
 * Plain C++ so it can be exercised by tests without Objective-C++.
 */

#include <functional>
#include <vector>

#include "common/dma/dma_chain_read.h"

#include "game/graphics/texture/TexturePool.h"

class MetalTextureUploadHandler {
 public:
  struct Stats {
    int uploads = 0;
    int eye_dma = 0;  // eye-renderer chunks forwarded to eye_dma_handler
    int skipped_eye_dma = 0;
    int skipped_texture_anim = 0;
  };

  // Processes one texture bucket: dma should be positioned at the bucket start,
  // next_bucket is the tag offset where the bucket ends. eye_dma_handler, when
  // set, consumes an eye-renderer chunk (dma positioned at its first transfer).
  Stats process(DmaFollower& dma,
                u32 next_bucket,
                TexturePool& pool,
                const u8* ee_memory,
                u32 s7_ptr,
                const std::function<void(DmaFollower&)>& eye_dma_handler = {});

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
