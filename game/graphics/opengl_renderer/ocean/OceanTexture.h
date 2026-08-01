#pragma once

#include "game/common/vu.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanVu.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"

class OceanTexture : public OceanTextureVu {
 public:
  OceanTexture(bool generate_mipmaps);
  void handle_ocean_texture_jak1(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof);
  void handle_ocean_texture_jak2(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof);
  void init_textures(TexturePool& pool, GameVersion version);
  void draw_debug_window();
  ~OceanTexture();

 private:
  void flush(SharedRenderState* render_state, ScopedProfilerNode& prof);

  void init_pc();
  void destroy_pc();

  void make_texture_with_mipmaps(SharedRenderState* render_state, ScopedProfilerNode& prof);

  bool m_generate_mipmaps;

  FramebufferTexturePair m_result_texture;
  FramebufferTexturePair m_temp_texture;
  GpuTexture* m_tex0_gpu = nullptr;

  struct {
    GLuint vao, static_vertex_buffer, dynamic_vertex_buffer, gl_index_buffer;
  } m_ogl;

  struct MipMap {
    GLuint vao, vtx_buffer;
    struct Vertex {
      float x, y;
      float s, t;
    };
    static_assert(sizeof(Vertex) == 16);
  } m_mipmap;
};
