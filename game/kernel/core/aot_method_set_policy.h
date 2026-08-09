#pragma once

#include "common/common_types.h"

#include "game/kernel/common/klink.h"

struct AotMethodSetLinkPolicy {
  bool enable_method_set = false;
  bool force_fast_link = false;
};

inline AotMethodSetLinkPolicy aot_method_set_link_policy(u32 link_flags,
                                                         bool master_debug,
                                                         bool disk_boot) {
  const bool force_fast_link = link_flags & LINK_FLAG_FORCE_FAST_LINK;
  const bool keep_debug = (link_flags & LINK_FLAG_FORCE_DEBUG) && master_debug && !disk_boot;

  // KERNEL/GAME are loaded without FORCE_FAST_LINK and need the propagation that their native
  // boot callers wrap around the whole DGO. Level objects use FORCE_FAST_LINK and match klink's
  // m_keep_debug exception.
  return {!force_fast_link || keep_debug, force_fast_link};
}
