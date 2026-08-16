#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

#include "common/dma/dma.h"
#include "common/util/Assert.h"

/*!
 * @file dma_chain_read.h
 * This file contains utilities for reading/following a DMA chain.
 *
 * This allows you to iterate through transfers like this:
 *
 * DmaFollower dma(mem, start_addr);
 * while (!reader.ended) {
 *   DmaTransfer transfer = reader.advance_to_next_transfer();
 *   // do something with the data in transfer.
 * }
 */

/*!
 * Represents a DMA transfer, including 64-bits of VIF tag.
 */
struct DmaTransfer {
  const u8* data = nullptr;
  u32 data_offset = 0;
  u32 size_bytes = 0;
  u64 transferred_tag = 0;

  u32 vif0() const { return transferred_tag & 0xffffffff; }
  u32 vif1() const { return (transferred_tag >> 32) & 0xffffffff; }

  VifCode vifcode0() const { return VifCode(vif0()); }
  VifCode vifcode1() const { return VifCode(vif1()); }

  template <typename T>
  T read_val(u32 offset) const {
    T result;
    memcpy(&result, (const u8*)data + offset, sizeof(T));
    return result;
  }
};

class DmaFollower {
 public:
  DmaFollower() { m_ended = true; }
  DmaFollower(const void* data, u32 start_offset) : m_base(data), m_tag_offset(start_offset) {}
  DmaFollower(const void* data, u32 start_offset, std::size_t data_size)
      : m_base(data), m_tag_offset(start_offset), m_data_size(data_size), m_bounded(true) {}
  template <typename T>
  T read_val(u32 offset) const {
    if (m_bounded) {
      return read_bounded_val<T>(offset);
    }
    T result;
    memcpy(&result, (const u8*)m_base + offset, sizeof(T));
    return result;
  }

  /*!
   * Read the current tag, return its transfer, then advance to the next.
   */
  DmaTransfer read_and_advance() {
    if (m_ended) {
      throw std::logic_error("DmaFollower cannot advance after the chain ended");
    }
    if (m_bounded) {
      validate_tag_target(m_tag_offset, "current DMA tag");
    }
    DmaTag tag(m_bounded ? read_bounded_val<u64>(m_tag_offset) : read_val<u64>(m_tag_offset));
    DmaTransfer result;
    result.transferred_tag =
        m_bounded ? read_bounded_val<u64>(static_cast<u64>(m_tag_offset) + 8)
                  : read_val<u64>(m_tag_offset + 8);
    result.size_bytes = (u32)tag.qwc * 16;
    if (m_bounded && tag.spr) {
      throw std::invalid_argument("DmaFollower does not support scratchpad DMA");
    }
    ASSERT(!tag.spr);

    const u64 inline_offset = static_cast<u64>(m_tag_offset) + 16;
    const u64 payload_size = static_cast<u64>(tag.qwc) * 16;
    if (m_bounded && inline_offset > std::numeric_limits<u64>::max() - payload_size) {
      throw std::overflow_error("DmaFollower inline payload address overflowed");
    }
    const u64 inline_end = inline_offset + payload_size;
    u64 data_offset = 0;
    u64 next_tag_offset = 0;
    bool has_next_tag = false;
    bool ends_chain = false;
    bool pushes_return = false;
    bool pops_return = false;
    switch (tag.kind) {
      case DmaTag::Kind::CNT:
        // data, then next tag. doesn't read address.
        if (m_bounded && tag.addr != 0) {
          throw std::invalid_argument("DmaFollower CNT tag has a nonzero address");
        }
        ASSERT(tag.addr == 0);
        data_offset = inline_offset;
        next_tag_offset = inline_end;
        has_next_tag = true;
        break;
      case DmaTag::Kind::NEXT:
        data_offset = inline_offset;
        next_tag_offset = tag.addr;
        has_next_tag = true;
        break;
      case DmaTag::Kind::REF:
      case DmaTag::Kind::REFS:
        data_offset = tag.addr;
        next_tag_offset = inline_offset;
        has_next_tag = true;
        break;
      case DmaTag::Kind::REFE:
        data_offset = tag.addr;
        next_tag_offset = inline_offset;
        ends_chain = true;
        break;
      case DmaTag::Kind::CALL:
        data_offset = inline_offset;
        if (m_bounded && m_sp >= 2) {
          throw std::overflow_error("DmaFollower CALL stack exceeds two entries");
        }
        ASSERT(m_sp <= 1);
        if (m_bounded && inline_end > std::numeric_limits<u32>::max()) {
          throw std::overflow_error("DmaFollower CALL return offset overflowed");
        }
        next_tag_offset = tag.addr;
        has_next_tag = true;
        pushes_return = true;
        break;
      case DmaTag::Kind::RET:
        if (m_bounded && m_sp <= 0) {
          throw std::underflow_error("DmaFollower RET has no matching CALL");
        }
        ASSERT(m_sp > 0);
        data_offset = inline_offset;
        next_tag_offset = static_cast<u32>(m_stack[m_sp - 1]);
        has_next_tag = true;
        pops_return = true;
        break;
      case DmaTag::Kind::END:
        data_offset = inline_offset;
        next_tag_offset = m_tag_offset;
        ends_chain = true;
        break;

      default:
        if (m_bounded) {
          throw std::invalid_argument("DmaFollower encountered an invalid DMA tag kind");
        }
        ASSERT(false);
    }

    if (m_bounded) {
      validate_aligned(data_offset, "DMA transfer");
      validate_span(data_offset, payload_size, "DMA transfer payload");
      if (data_offset > std::numeric_limits<u32>::max()) {
        throw std::overflow_error("DmaFollower transfer offset overflowed");
      }
      if (has_next_tag) {
        validate_tag_target(next_tag_offset, "DMA control-flow target");
      } else if (next_tag_offset > std::numeric_limits<u32>::max()) {
        throw std::overflow_error("DmaFollower terminal offset overflowed");
      }
    }

    result.data_offset = static_cast<u32>(data_offset);
    m_tag_offset = static_cast<u32>(next_tag_offset);
    if (pushes_return) {
      m_stack[m_sp++] = static_cast<u32>(inline_end);
    } else if (pops_return) {
      m_stack[--m_sp] = -1;
    }
    m_ended = ends_chain;
    result.data = static_cast<const u8*>(m_base) + result.data_offset;
    return result;
  }

  DmaTransfer advance_and_print_dma(DmaFollower& dma) {
    auto data = dma.read_and_advance();
    printf(
        "dma transfer:\n%ssize: %d\nvif0: %s, data: %d\nvif1: %s, data: %d, imm: "
        "%d\n\n",
        dma.current_tag().print().c_str(), data.size_bytes, data.vifcode0().print().c_str(),
        data.vif0(), data.vifcode1().print().c_str(), data.vifcode1().num,
        data.vifcode1().immediate);
    return data;
  }

  DmaTag current_tag() const {
    validate_current_tag_access();
    return DmaTag(m_bounded ? read_bounded_val<u64>(m_tag_offset) : read_val<u64>(m_tag_offset));
  }
  u32 current_tag_vif0() const {
    validate_current_tag_access();
    return m_bounded ? read_bounded_val<u32>(static_cast<u64>(m_tag_offset) + 8)
                     : read_val<u32>(m_tag_offset + 8);
  }
  u32 current_tag_vif1() const {
    validate_current_tag_access();
    return m_bounded ? read_bounded_val<u32>(static_cast<u64>(m_tag_offset) + 12)
                     : read_val<u32>(m_tag_offset + 12);
  }
  VifCode current_tag_vifcode0() const { return VifCode(current_tag_vif0()); }
  VifCode current_tag_vifcode1() const { return VifCode(current_tag_vif1()); }
  u32 current_tag_offset() const { return m_tag_offset; }
  bool ended() const { return m_ended; }

 private:
  template <typename T>
  T read_bounded_val(u64 offset) const {
    validate_span(offset, sizeof(T), "DMA read");
    T result;
    memcpy(&result, static_cast<const u8*>(m_base) + static_cast<std::size_t>(offset), sizeof(T));
    return result;
  }

  void validate_span(u64 offset, u64 size, const char* description) const {
    if (!m_base) {
      throw std::invalid_argument("DmaFollower bounded memory is null");
    }
    if (offset > m_data_size || size > m_data_size - static_cast<std::size_t>(offset)) {
      throw std::out_of_range(std::string(description) + " is outside bounded DMA memory");
    }
  }

  void validate_aligned(u64 offset, const char* description) const {
    if ((offset & 15) != 0) {
      throw std::invalid_argument(std::string(description) + " is not 16-byte aligned");
    }
  }

  void validate_tag_target(u64 offset, const char* description) const {
    validate_aligned(offset, description);
    validate_span(offset, 16, description);
    if (offset > std::numeric_limits<u32>::max()) {
      throw std::overflow_error(std::string(description) + " exceeds the follower offset range");
    }
  }

  void validate_current_tag_access() const {
    if (m_bounded) {
      if (m_ended) {
        throw std::logic_error("DmaFollower cannot inspect a tag after the chain ended");
      }
      validate_tag_target(m_tag_offset, "current DMA tag");
    }
  }

  const void* m_base = nullptr;
  u32 m_tag_offset = 0;
  std::size_t m_data_size = 0;
  bool m_bounded = false;
  s32 m_sp = 0;
  s32 m_stack[2] = {-1, -1};
  bool m_ended = false;
};
