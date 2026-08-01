#include "game/graphics/opengl_renderer/ShadowVu.h"

#include "fmt/format.h"

void ShadowVu::xgkick(u16 imm) {
  u32 ind_of_fan_start = UINT32_MAX;
  bool fan_running = false;
  const u8* data = (const u8*)(m_vu_data + imm);

  u8 rgba[4] = {1, 2, 3, 4};

  bool eop = false;

  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;

    // unpack registers.
    // faster to do it once outside of the nloop loop.
    GifTag::RegisterDescriptor reg_desc[16];
    u32 nreg = tag.nreg();
    for (u32 i = 0; i < nreg; i++) {
      reg_desc[i] = tag.reg(i);
    }

    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.pre()) {
        GsPrim prim(tag.prim());
        ASSERT(prim.kind() == GsPrim::Kind::TRI_FAN);
      }
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 reg = 0; reg < nreg; reg++) {
          switch (reg_desc[reg]) {
            case GifTag::RegisterDescriptor::AD: {
              u64 value;
              GsRegisterAddress addr;
              memcpy(&value, data + offset, sizeof(u64));
              memcpy(&addr, data + offset + 8, sizeof(GsRegisterAddress));

              switch (addr) {
                case GsRegisterAddress::TEXFLUSH:
                  break;
                case GsRegisterAddress::RGBAQ: {
                  rgba[0] = data[0 + offset];
                  rgba[1] = data[1 + offset];
                  rgba[2] = data[2 + offset];
                  rgba[3] = data[3 + offset];
                  float Q;
                  memcpy(&Q, data + offset + 4, 4);
                  // fmt::print("rgba: {} {} {} {}: {}\n", rgba[0], rgba[1], rgba[2], rgba[3], Q);
                } break;
                default:
                  ASSERT_MSG(false, fmt::format("Address {} is not supported",
                                                register_address_name(addr)));
              }
            } break;
            case GifTag::RegisterDescriptor::ST: {
              float s, t;
              memcpy(&s, data + offset, 4);
              memcpy(&t, data + offset + 4, 4);
              // fmt::print("st: {} {}\n", s, t);
            } break;
            case GifTag::RegisterDescriptor::RGBAQ:
              for (int i = 0; i < 4; i++) {
                rgba[i] = data[offset + i * 4];
              }
              // fmt::print("rgbaq: {} {} {} {}\n", rgba[0], rgba[1], rgba[2], rgba[3]);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              // handle_xyzf2_packed(data + offset, render_state, prof);
              {
                u32 x, y;
                memcpy(&x, data + offset, 4);
                memcpy(&y, data + offset + 4, 4);

                u64 upper;
                memcpy(&upper, data + offset + 8, 8);
                u32 z = (upper >> 4) & 0xffffff;

                x <<= 16;
                y <<= 16;
                z <<= 8;
                u32 vidx = m_next_vertex++;
                auto& v = m_vertices[vidx];
                ASSERT(m_next_vertex < MAX_VERTICES);
                v.xyz[0] = (float)x / (float)UINT32_MAX;
                v.xyz[1] = (float)y / (float)UINT32_MAX;
                v.xyz[2] = (float)z / (float)UINT32_MAX;

                if (ind_of_fan_start == UINT32_MAX) {
                  ind_of_fan_start = vidx;
                } else {
                  if (fan_running) {
                    // todo, actually use triangle fans in opengl...
                    if (rgba[0] > 0) {
                      // back
                      m_back_indices[m_next_back_index++] = vidx;
                      m_back_indices[m_next_back_index++] = vidx - 1;
                      m_back_indices[m_next_back_index++] = ind_of_fan_start;
                    } else {
                      m_front_indices[m_next_front_index++] = vidx;
                      m_front_indices[m_next_front_index++] = vidx - 1;
                      m_front_indices[m_next_front_index++] = ind_of_fan_start;
                    }
                  } else {
                    fan_running = true;
                  }
                }

                // fmt::print("xyzfadc: {} {} {} {} {}\n", x, y, z, f, adc);
              }
              break;
            default:
              ASSERT_MSG(false, fmt::format("Register {} is not supported in packed mode yet\n",
                                            reg_descriptor_name(reg_desc[reg])));
          }
          offset += 16;  // PACKED = quadwords
        }
      }
    } else {
      ASSERT(false);  // format not packed or reglist.
    }

    eop = tag.eop();
  }
}
