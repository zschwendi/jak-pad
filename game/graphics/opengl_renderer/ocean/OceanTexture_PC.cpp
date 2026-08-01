#include "OceanTexture.h"

void OceanTexture::flush(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  ASSERT(m_pc.vtx_idx == 2112);
  glBindVertexArray(m_ogl.vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.dynamic_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * NUM_VERTS, m_pc.vertex_dynamic.data(),
               GL_DYNAMIC_DRAW);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.gl_index_buffer);

  render_state->shaders[ShaderId::OCEAN_TEXTURE].activate();

  GsTex0 tex0(m_envmap_adgif.tex0_data);
  auto lookup = render_state->texture_pool->lookup(tex0.tbp0());
  if (!lookup) {
    lookup = render_state->texture_pool->get_placeholder_texture();
  }
  // no decal
  // yes tcc
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, *lookup);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glUniform1i(glGetUniformLocation(render_state->shaders[ShaderId::OCEAN_TEXTURE].id(), "tex_T0"),
              0);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  // glDrawArrays(GL_TRIANGLE_STRIP, 0, NUM_VERTS);
  glEnable(GL_PRIMITIVE_RESTART);
  glPrimitiveRestartIndex(UINT32_MAX);
  glDrawElements(GL_TRIANGLE_STRIP, m_pc.index_buffer.size(), GL_UNSIGNED_INT, (void*)0);
  prof.add_draw_call();
  prof.add_tri(NUM_STRIPS * NUM_STRIPS * 2);

  glBindVertexArray(0);
}

void OceanTexture::init_pc() {
  // the CPU-side positions and index buffer are built by OceanTextureVu
  glGenVertexArrays(1, &m_ogl.vao);
  glBindVertexArray(m_ogl.vao);

  glGenBuffers(1, &m_ogl.gl_index_buffer);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_ogl.gl_index_buffer);
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(u32) * m_pc.index_buffer.size(),
               m_pc.index_buffer.data(), GL_STATIC_DRAW);

  glGenBuffers(1, &m_ogl.static_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.static_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(math::Vector2f) * NUM_VERTS, m_pc.vertex_positions.data(),
               GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);

  glVertexAttribPointer(0,         // location 0 in the shader
                        2,         // 3 floats per vert
                        GL_FLOAT,  // floats
                        GL_TRUE,   // normalized, ignored,
                        0,         // tightly packed
                        0

  );

  glGenBuffers(1, &m_ogl.dynamic_vertex_buffer);
  glBindBuffer(GL_ARRAY_BUFFER, m_ogl.dynamic_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * NUM_VERTS, nullptr, GL_DYNAMIC_DRAW);
  glVertexAttribPointer(1,                             // location 0 in the shader
                        4,                             // 4 color components
                        GL_UNSIGNED_BYTE,              // floats
                        GL_TRUE,                       // normalized, ignored,
                        sizeof(Vertex),                //
                        (void*)offsetof(Vertex, rgba)  // offset in array (why is this a pointer...)
  );
  glVertexAttribPointer(2,                          // location 0 in the shader
                        2,                          // 2 floats per vert
                        GL_FLOAT,                   // floats
                        GL_FALSE,                   // normalized, ignored,
                        sizeof(Vertex),             //
                        (void*)offsetof(Vertex, s)  // offset in array (why is this a pointer...)
  );
}

void OceanTexture::destroy_pc() {}