#include "ui/round_frame_layout.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
  using ui::detail::RoundFrameLayout;
  RoundFrameLayout layout;
  std::vector<bool> visited(RoundFrameLayout::kPixelCount);
  size_t count = 0;
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 240; ++x) {
      const int dx = x - 120, dy = y - 120;
      const bool stored = dx * dx + dy * dy <= 128 * 128;
      const size_t index = layout.index(x, y);
      assert((index < RoundFrameLayout::kPixelCount) == stored);
      // ALL physical-panel pixels, not just the inner radar ring, remain stored.
      if (dx * dx + dy * dy <= 120 * 120) assert(stored);
      if (stored) {
        assert(!visited[index]);
        visited[index] = true;
        ++count;
      }
    }
  }
  assert(count == RoundFrameLayout::kPixelCount);
  for (bool v : visited) assert(v);
  assert(layout.index(-1, 0) == RoundFrameLayout::kPixelCount);
  assert(layout.index(0, -1) == RoundFrameLayout::kPixelCount);
  assert(layout.index(240, 120) == RoundFrameLayout::kPixelCount);
  // Cardinal text padding stays in the preview, beyond the physical circle.
  for (int x = 112; x < 128; ++x) {
    assert(layout.index(x, 0) < RoundFrameLayout::kPixelCount);
    assert(layout.index(0, x) < RoundFrameLayout::kPixelCount);
  }
  std::puts("PASS: exhaustive round-frame mapping, unique/bounded storage,");
  std::puts("      complete physical panel coverage and cardinal padding");
}
