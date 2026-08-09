#include "game/graphics/opengl_renderer/sprite/Sprite3.h"
#include "game/graphics/sprite_glow_math.h"

/*!
 * Handle glow dma and draw glow sprites using GlowRenderer
 */
void Sprite3::glow_dma_and_draw(DmaFollower& dma,
                                SharedRenderState* render_state,
                                ScopedProfilerNode& prof) {
  auto maybe_consts_setup = dma.read_and_advance();
  if (maybe_consts_setup.size_bytes != sizeof(SpriteGlowConsts)) {
    return;
  }
  SpriteGlowConsts consts;
  memcpy(&consts, maybe_consts_setup.data, sizeof(SpriteGlowConsts));

  auto templ_1 = dma.read_and_advance();
  ASSERT(templ_1.size_bytes == 16 * 0x54);

  auto templ_2 = dma.read_and_advance();
  ASSERT(templ_2.size_bytes == 16 * 0x54);

  auto bo = dma.read_and_advance();
  ASSERT(bo.size_bytes == 0);

  auto flushe = dma.read_and_advance();
  ASSERT(flushe.size_bytes == 0);

  auto control_xfer = dma.read_and_advance();
  while (control_xfer.size_bytes == 0 && control_xfer.vifcode0().kind == VifCode::Kind::NOP &&
         control_xfer.vifcode1().kind == VifCode::Kind::NOP) {
    control_xfer = dma.read_and_advance();
  }
  while (control_xfer.size_bytes == 16) {
    auto vecdata_xfer = dma.read_and_advance();
    auto shader_xfer = dma.read_and_advance();
    auto call = dma.read_and_advance();
    (void)call;

    u32 num_sprites;
    memcpy(&num_sprites, control_xfer.data, 4);
    ASSERT(num_sprites == 1);  // always, for whatever reason.

    ASSERT(vecdata_xfer.size_bytes == 4 * 16);
    ASSERT(shader_xfer.size_bytes == 5 * 16);

    if (m_enable_glow) {
      if (m_glow_renderer.at_max_capacity()) {
        m_glow_renderer.flush(render_state, prof);
      }
      auto* out = m_glow_renderer.alloc_sprite();
      if (!glow_math(&consts, m_glow_renderer.new_mode, vecdata_xfer.data, shader_xfer.data, out)) {
        m_glow_renderer.cancel_sprite();
      }
    }

    control_xfer = dma.read_and_advance();
    while (control_xfer.size_bytes == 0 && control_xfer.vifcode0().kind == VifCode::Kind::NOP &&
           control_xfer.vifcode1().kind == VifCode::Kind::NOP) {
      control_xfer = dma.read_and_advance();
    }
  }

  m_glow_renderer.flush(render_state, prof);
}
