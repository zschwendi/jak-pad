#include <cstdio>
#include <initializer_list>

#include "common/dma/gs.h"

int main() {
  for (bool tcc : {false, true}) {
    for (bool filt : {false, true}) {
      DrawMode mode;
      mode.as_int() = 0;
      mode.set_tcc(tcc);
      mode.set_filt_enable(filt);

      if (mode.get_tcc_enable() != tcc || mode.get_filt_enable() != filt ||
          (mode.as_int() & (0b11 << 6)) !=
              ((static_cast<u32>(tcc) << 7) | (static_cast<u32>(filt) << 6))) {
        std::fprintf(stderr, "DrawMode TCC/filter mismatch for TCC=%d filter=%d\n", tcc, filt);
        return 1;
      }
    }
  }

  std::puts("draw-mode-test: PASS");
  return 0;
}
