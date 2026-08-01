#include "metal_texture_upload_handler.h"

#include "common/log/log.h"
#include "common/util/Assert.h"

/*!
 * Mirror of TextureUploadHandler::render. The upload packets are 16 bytes of
 * data under a PC_PORT vifcode with vif1 == 3, produced by the PC-port GOAL
 * changes. Eye-renderer DMA (qwc == 8) and texture-animator packets (PC_PORT
 * immediate 12) flush pending uploads at the same points the GL handler does;
 * the eye data then goes to eye_dma_handler (the animator is Jak 2/3 territory
 * and is skipped).
 */
MetalTextureUploadHandler::Stats MetalTextureUploadHandler::process(
    DmaFollower& dma,
    u32 next_bucket,
    TexturePool& pool,
    const u8* ee_memory,
    u32 s7_ptr,
    const std::function<void(DmaFollower&)>& eye_dma_handler) {
  Stats stats;
  std::vector<TextureUpload> uploads;

  while (dma.current_tag_offset() != next_bucket) {
    auto dma_tag = dma.current_tag();
    auto vif0 = dma.current_tag_vifcode0();

    if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == 12) {
      // texture animator (Jak 2/3). the GL handler flushes uploads, then lets
      // the animator consume its DMA; here the animator does not exist yet, so
      // its data flows through the generic path below with no effect.
      dma.read_and_advance();
      flush_uploads(uploads, pool, ee_memory, s7_ptr, stats);
      uploads.clear();
      stats.skipped_texture_anim++;
      static bool warned_anim = false;
      if (!warned_anim) {
        lg::warn("Metal TextureUploadHandler: texture animator DMA not ported yet, skipping");
        warned_anim = true;
      }
      continue;
    }

    if (dma_tag.qwc == (128 / 16)) {
      // eye renderer data. flush first (its uploads may contain eye textures),
      // like the GL handler, then let the eye renderer consume its chunk.
      flush_uploads(uploads, pool, ee_memory, s7_ptr, stats);
      uploads.clear();
      if (eye_dma_handler) {
        stats.eye_dma++;
        eye_dma_handler(dma);
      } else {
        stats.skipped_eye_dma++;
        static bool warned_eye = false;
        if (!warned_eye) {
          lg::warn("Metal TextureUploadHandler: no eye renderer attached, skipping eye DMA");
          warned_eye = true;
        }
      }
    }

    auto data = dma.read_and_advance();
    if (data.size_bytes == 0 && data.vif0() == 0 && data.vif1() == 0) {
      continue;
    }

    if (data.size_bytes == 16 && data.vifcode0().kind == VifCode::Kind::PC_PORT &&
        data.vif1() == 3) {
      TextureUpload upload_data;
      memcpy(&upload_data, data.data, sizeof(upload_data));
      uploads.push_back(upload_data);
      continue;
    }

    if (dma_tag.kind == DmaTag::Kind::CALL) {
      dma.read_and_advance();  // call
      dma.read_and_advance();  // cnt
      dma.read_and_advance();  // ret
      ASSERT(dma.current_tag_offset() == next_bucket);
    }
  }

  flush_uploads(uploads, pool, ee_memory, s7_ptr, stats);
  return stats;
}

void MetalTextureUploadHandler::flush_uploads(std::vector<TextureUpload>& uploads,
                                              TexturePool& pool,
                                              const u8* ee_memory,
                                              u32 s7_ptr,
                                              Stats& stats) {
  stats.uploads += (int)uploads.size();
  for (auto& upload : uploads) {
    pool.handle_upload_now(ee_memory + upload.page, (int)upload.mode, ee_memory, s7_ptr, false);
  }
}
