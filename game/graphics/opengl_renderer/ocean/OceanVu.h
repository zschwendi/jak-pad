#pragma once

/*!
 * @file OceanVu.h
 * VU1 emulation for the ocean renderers, with no graphics API in it.
 *
 * Roughly 10k of the ocean renderer's ~14k lines are transcriptions of the VU1
 * microprograms (OceanTexture_PC.cpp, OceanMid_PS2.cpp, OceanNear_PS2.cpp).
 * They touch nothing but VU registers, VU data memory and the CPU-side vertex
 * buffers, so every backend can run the exact same code: the classes below own
 * that state and those functions, and each backend's ocean renderer derives
 * from them and supplies only the drawing (`xgkick`).
 *
 * Nothing here is backend-specific; the split exists so the OpenGL and Metal
 * ocean renderers share one emulator instead of two copies of it.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/common/vu.h"

/*!
 * The ocean-texture VU1 program: it turns the DMA'd ocean vertices into the
 * 128x128 generated ocean texture's geometry (32 strips of 66 vertices).
 * Implemented in OceanTexture_PC.cpp.
 */
class OceanTextureVu {
 public:
  virtual ~OceanTextureVu() = default;

 protected:
  OceanTextureVu() {
    m_dbuf_x = m_dbuf_a;
    m_dbuf_y = m_dbuf_b;
    m_tbuf_x = m_tbuf_a;
    m_tbuf_y = m_tbuf_b;
    init_vertex_geometry();
  }

  // The fixed part of the ocean texture's geometry: 32 triangle strips over a
  // 2048x2048 space, separated by the 0xFFFFFFFF restart index.
  void init_vertex_geometry() {
    int i = 0;
    m_pc.vertex_positions.resize(NUM_VERTS);
    m_pc.vertex_dynamic.resize(NUM_VERTS);
    m_pc.index_buffer.clear();
    for (u32 strip = 0; strip < NUM_STRIPS; strip++) {
      u32 lo = 64 * strip;
      u32 hi = 64 * (strip + 1);
      for (u32 vert_pair = 0; vert_pair < NUM_VERTS_PER_STRIP / 2; vert_pair++) {
        m_pc.index_buffer.push_back(i);
        m_pc.vertex_positions[i++] = math::Vector2f(vert_pair * 64, lo);
        m_pc.index_buffer.push_back(i);
        m_pc.vertex_positions[i++] = math::Vector2f(vert_pair * 64, hi);
      }
      m_pc.index_buffer.push_back(UINT32_MAX);
    }
  }

  void run_L1_PC();
  void run_L2_PC();
  void run_L3_PC();
  void run_L5_PC();
  void xgkick_PC(Vf* src);

  void run_L1_PC_jak2();
  void run_L2_PC_jak2();
  void run_L3_PC_jak2();

  // resets the vertex write cursor for a new frame
  void setup_renderer();

  static constexpr int TEX0_SIZE = 128;
  static constexpr int NUM_MIPS = 8;

  // (deftype ocean-texture-constants (structure)
  struct OceanTextureConstants {
    //  ((giftag    qword    :inline :offset-assert 0) 985
    u8 giftag[16];
    //   (buffers   vector4w :inline :offset-assert 16) 986
    math::Vector<u32, 4> buffers;
    //   (dests     vector4w :inline :offset-assert 32) 987
    math::Vector<u32, 4> dests;
    //   (start     vector   :inline :offset-assert 48) 988
    math::Vector4f start;
    //   (offsets   vector   :inline :offset-assert 64) 989
    math::Vector4f offsets;
    //   (constants vector   :inline :offset-assert 80) 990
    math::Vector4f constants;
    //   (cam-nrm   vector   :inline :offset-assert 96) 991
    math::Vector4f cam_nrm;
    //   )
  } m_texture_constants;
  static_assert(sizeof(OceanTextureConstants) == 112);

  AdGifData m_envmap_adgif;

  Vf m_texture_vertices_a[192];
  Vf m_texture_vertices_b[192];

  static constexpr int DBUF_SIZE = 99;
  Vf m_dbuf_a[DBUF_SIZE];
  Vf m_dbuf_b[DBUF_SIZE];

  Vf* m_dbuf_x;
  Vf* m_dbuf_y;

  static constexpr int TBUF_SIZE = 199;
  Vf m_tbuf_a[TBUF_SIZE];
  Vf m_tbuf_b[TBUF_SIZE];

  Vf* m_tbuf_x;
  Vf* m_tbuf_y;

  Vf* m_texture_vertices_loading = nullptr;
  Vf* m_texture_vertices_drawing = nullptr;

  Vf* swap_vu_upload_buffers() {
    std::swap(m_texture_vertices_drawing, m_texture_vertices_loading);
    return m_texture_vertices_drawing;
  }

  void swap_dbuf() { std::swap(m_dbuf_x, m_dbuf_y); }

  void swap_tbuf() { std::swap(m_tbuf_x, m_tbuf_y); }

  Vf* get_dbuf() { return m_dbuf_x; }

  Vf* get_dbuf_other() { return m_dbuf_y; }

  Vf* get_tbuf() { return m_tbuf_x; }

  struct {
    Vf startx;  //           vf14
    // Vf base_pos;          vf15
    // Vf nrm0;              vf24
    Vf* dbuf_read_a;      // vi03
    Vf* dbuf_read_b;      // vi04
    Vf* in_ptr;           // vi05
    Vf* dbuf_write;       // vi06
    Vf* dbuf_write_base;  // vi07
    Vf* tptr;             // vi08
    Vf* tbase;            // vi09
  } vu;

  static constexpr u32 NUM_STRIPS = 32;
  static constexpr u32 NUM_VERTS_PER_STRIP = 66;
  static constexpr u32 NUM_VERTS = NUM_STRIPS * NUM_VERTS_PER_STRIP;

  // note: if we used u16's for s/t, we could make this 8 bytes, but I'm afraid that some GPUs
  // will be unhappy with that format.
  struct Vertex {
    float s, t;
    math::Vector<u8, 4> rgba;
    u32 pad;
  };
  static_assert(sizeof(Vertex) == 16);

  // CPU-side geometry: the positions and index buffer are fixed and built once,
  // the dynamic vertices are what the VU program produces each frame.
  struct {
    std::vector<math::Vector2f> vertex_positions;
    std::vector<Vertex> vertex_dynamic;
    std::vector<u32> index_buffer;
    u32 vtx_idx = 0;
  } m_pc;

  enum TexVu1Data {
    BUF0 = 384,
    BUF1 = 583,
    DEST0 = 782,
    DEST1 = 881,
    CONSTANTS = 985,
  };

  enum TexVu1Prog { START = 0, REST = 2, DONE = 4 };

  static constexpr int NUM_FRAG_LOOPS = 9;
};

/*!
 * The ocean-mid VU1 program. Implemented in OceanMid_PS2.cpp. The derived class
 * supplies xgkick, which hands the produced GIF packet to the backend's ocean
 * renderer.
 */
class OceanMidVu {
 public:
  virtual ~OceanMidVu() = default;

 protected:
  OceanMidVu() {
    for (auto& x : m_vu_data) {
      x.fill(999.);
    }
    vu.vf25 = Vf(1, 1, 1, 1);
  }

  virtual void xgkick(u16 addr) = 0;

  void run_call0_vu2c();
  void run_call41_vu2c();
  void run_call43_vu2c();
  void run_call46_vu2c();
  void run_call73_vu2c();
  void run_call73_vu2c_jak2();
  void run_call107_vu2c();
  void run_call107_vu2c_jak2();
  void run_call275_vu2c();
  void run_call275_vu2c_jak2();

  void run_L26_vu2c();
  void run_L32_vu2c();
  void run_L32_vu2c_jak2();
  void run_L38_vu2c();
  void run_L38_vu2c_jak2();
  void run_L43_vu2c();
  void run_L45_vu2c();

  bool m_buffer_toggle = false;
  static constexpr int VU1_INPUT_BUFFER_BASE = 0;
  static constexpr int VU1_INPUT_BUFFER_OFFSET = 0x76;

  u16 xtop() {
    m_buffer_toggle = !m_buffer_toggle;
    return get_vu_buffer();
  }

  u16 get_upload_buffer() {
    if (m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  u16 get_vu_buffer() {
    if (!m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  // (deftype ocean-mid-constants (structure)
  struct Constants {
    //  ((hmge-scale     vector       :inline :offset-assert 0)
    math::Vector4f hmge_scale;
    //   (inv-hmge-scale vector       :inline :offset-assert 16)
    math::Vector4f inv_hmge_scale;
    //   (hvdf-offset    vector       :inline :offset-assert 32)
    math::Vector4f hvdf_offset;
    //   (fog            vector       :inline :offset-assert 48)
    math::Vector4f fog;
    //   (constants      vector       :inline :offset-assert 64)
    math::Vector4f constants;
    //   (constants2     vector       :inline :offset-assert 80)
    math::Vector4f constants2;
    //   (drw-fan        gs-gif-tag        :inline :offset-assert 96) ;; was qword
    u8 drw_fan[16];
    //   (env-fan        gs-gif-tag        :inline :offset-assert 112) ;; was qword
    u8 env_fan[16];
    //   (drw-adgif      gs-gif-tag        :inline :offset-assert 128);; was qword
    AdGifData drw_adgif;
    //   (drw-texture    adgif-shader :inline :offset-assert 144)
    u8 drw_texture[16];
    //   (drw-strip-0    gs-gif-tag        :inline :offset-assert 224) ;; was qword
    u8 drw_strip_0[16];
    //   (drw-strip-1    gs-gif-tag        :inline :offset-assert 240) ;; was qword
    u8 drw_strip_1[16];
    //   (env-adgif      gs-gif-tag        :inline :offset-assert 256) ;; was qword
    u8 env_adgif[16];
    //   (env-texture    adgif-shader :inline :offset-assert 272)
    AdGifData env_texture;
    //   (env-strip      gs-gif-tag        :inline :offset-assert 352) ;; was qword
    u8 env_strip[16];
    //   (env-color      vector       :inline :offset-assert 368)
    math::Vector4f env_color;
    //   (index-table    vector4w      8 :inline      :offset-assert 384)
    math::Vector<u32, 4> index_table[8];
    //   (pos0           vector       :inline :offset-assert 512)
    math::Vector4f pos0;
    //   (pos1           vector       :inline :offset-assert 528)
    math::Vector4f pos1;
    //   (pos2           vector       :inline :offset-assert 544)
    math::Vector4f pos2;
    //   (pos3           vector       :inline :offset-assert 560)
    math::Vector4f pos3;
    //   )
    //  :method-count-assert 9
    //  :size-assert         #x240
    //  :flag-assert         #x900000240
    //  )
  } m_constants;
  static_assert(sizeof(Constants) == 0x240);

  enum Vu1Data {
    IN_BUFFER_0 = VU1_INPUT_BUFFER_BASE,    // 0
    IN_BUFFER_1 = VU1_INPUT_BUFFER_OFFSET,  // 0x76

    CONSTANTS = 0x2dd,
  };

  Vf m_vu_data[1024];

  void sq_buffer(Mask mask, const Vf& val, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        m_vu_data[addr].data[i] = val[i];
      }
    }
  }

  void ilw_buffer(Mask mask, u16& dest, u16 addr) {
    ASSERT(addr < 1024);
    switch (mask) {
      case Mask::x:
        dest = m_vu_data[addr].x_as_u16();
        break;
      case Mask::y:
        dest = m_vu_data[addr].y_as_u16();
        break;
      case Mask::z:
        dest = m_vu_data[addr].z_as_u16();
        break;
      case Mask::w:
        dest = m_vu_data[addr].w_as_u16();
        break;
      default:
        ASSERT(false);
    }
  }

  void isw_buffer(Mask mask, u16 src, u16 addr) {
    ASSERT(addr < 1024);
    u32 val32 = src;
    switch (mask) {
      case Mask::x:
        memcpy(&m_vu_data[addr].data[0], &val32, 4);
        break;
      case Mask::y:
        memcpy(&m_vu_data[addr].data[1], &val32, 4);
        break;
      case Mask::z:
        memcpy(&m_vu_data[addr].data[2], &val32, 4);
        break;
      case Mask::w:
        memcpy(&m_vu_data[addr].data[3], &val32, 4);
        break;
      default:
        ASSERT(false);
    }
  }

  void lq_buffer(Mask mask, Vf& dest, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        dest[i] = m_vu_data[addr].data[i];
      }
    }
  }

  struct Vu {
    const Vf vf00;
    Accumulator acc;
    Vf vf01, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf10, vf11, vf12, vf13, vf14, vf15,
        vf16, vf17, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30,
        vf31;
    u16 vi01, vi02, vi03, vi04, vi05, vi06, vi07, vi08, vi09, vi10, vi11, vi12, vi13, vi14, vi15;
    float Q, P;
    Vu() : vf00(0, 0, 0, 1) {}
  } vu;
};

/*!
 * The ocean-near VU1 program. Implemented in OceanNear_PS2.cpp.
 */
class OceanNearVu {
 public:
  virtual ~OceanNearVu() = default;

 protected:
  OceanNearVu() {
    for (auto& a : m_vu_data) {
      a.fill(0);
    }
  }

  virtual void xgkick(u16 addr) = 0;

  void run_call0_vu2c();
  void run_call0_vu2c_jak2();
  void run_call39_vu2c();
  void run_call39_vu2c_jak2();
  void run_L15_vu2c();
  void run_L15_vu2c_jak2();
  void run_L21_vu2c();
  void run_L21_vu2c_jak2();
  void run_L23_vu2c();
  void run_L25_vu2c();
  void run_L25_vu2c_jak2();
  void run_L30_vu2c();
  void run_L32_vu2c();

  bool m_buffer_toggle = false;
  static constexpr int VU1_INPUT_BUFFER_BASE = 0;
  static constexpr int VU1_INPUT_BUFFER_OFFSET = 0x10;

  u16 xtop() {
    m_buffer_toggle = !m_buffer_toggle;
    return get_vu_buffer();
  }

  u16 get_upload_buffer() {
    if (m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  u16 get_vu_buffer() {
    if (!m_buffer_toggle) {
      return VU1_INPUT_BUFFER_OFFSET;
    } else {
      return VU1_INPUT_BUFFER_BASE;
    }
  }

  void lq_buffer(Mask mask, Vf& dest, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        dest[i] = m_vu_data[addr].data[i];
      }
    }
  }

  void sq_buffer(Mask mask, const Vf& val, u16 addr) {
    ASSERT(addr < 1024);
    for (int i = 0; i < 4; i++) {
      if ((u64)mask & (1 << i)) {
        m_vu_data[addr].data[i] = val[i];
      }
    }
  }

  void ilw_buffer(Mask mask, u16& dest, u16 addr) {
    ASSERT(addr < 1024);
    switch (mask) {
      case Mask::x:
        dest = m_vu_data[addr].x_as_u16();
        break;
      case Mask::y:
        dest = m_vu_data[addr].y_as_u16();
        break;
      case Mask::z:
        dest = m_vu_data[addr].z_as_u16();
        break;
      case Mask::w:
        dest = m_vu_data[addr].w_as_u16();
        break;
      default:
        ASSERT(false);
    }
  }

  void isw_buffer(Mask mask, u16 src, u16 addr) {
    ASSERT(addr < 1024);
    u32 val32 = src;
    switch (mask) {
      case Mask::x:
        memcpy(&m_vu_data[addr].data[0], &val32, 4);
        break;
      case Mask::y:
        memcpy(&m_vu_data[addr].data[1], &val32, 4);
        break;
      case Mask::z:
        memcpy(&m_vu_data[addr].data[2], &val32, 4);
        break;
      case Mask::w:
        memcpy(&m_vu_data[addr].data[3], &val32, 4);
        break;
      default:
        ASSERT(false);
    }
  }

  Vf m_vu_data[1024];

  struct Vu {
    const Vf vf00;

    Accumulator acc;
    Vf vf01, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf10, vf11, vf12, vf13, vf14, vf15,
        vf16, vf17, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30,
        vf31;
    u16 vi01, vi02, vi03, vi04, vi05, vi06, vi07, vi08, vi09, vi10, vi11, vi12, vi13, vi14;

    float P, Q;
    Vu() : vf00(0, 0, 0, 1) {}

  } vu;
};
