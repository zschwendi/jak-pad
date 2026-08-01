#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/ShadowVu.h"

class ShadowRenderer : public BucketRenderer, public ShadowVu {
 public:
  ShadowRenderer(const std::string& name, int my_id);
  ~ShadowRenderer();
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void draw_debug_window() override;

 private:
  void draw(SharedRenderState* render_state, ScopedProfilerNode& prof);

  math::Vector4f m_color;

  struct {
    // index is front, back
    GLuint vertex_buffer, index_buffer[2], vao;
  } m_ogl;

  bool m_debug_draw_volume = false;
};
