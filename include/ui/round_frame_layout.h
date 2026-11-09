#pragma once

#include <cstddef>
#include <cstdint>

namespace ui::detail {

// Physical panel radius 120 plus 8 px of preview/cardinal-label padding.
// Pixels in the four remaining square corners are outside the round panel.
class RoundFrameLayout {
 public:
  static constexpr int kSize = 240;
  static constexpr int kRadius = 128;
  static constexpr size_t kPixelCount = 49551;
  struct Row {
    uint16_t offset;
    uint8_t left;
    uint8_t width;
  };

  RoundFrameLayout() {
    size_t offset = 0;
    for (int y = 0; y < kSize; ++y) {
      int left = 0;
      int right = kSize - 1;
      while (!contains(left, y)) ++left;
      while (!contains(right, y)) --right;
      rows_[y] = {static_cast<uint16_t>(offset), static_cast<uint8_t>(left),
                  static_cast<uint8_t>(right - left + 1)};
      offset += right - left + 1;
    }
  }

  static bool contains(int x, int y) {
    const int dx = x - kSize / 2;
    const int dy = y - kSize / 2;
    return x >= 0 && y >= 0 && x < kSize && y < kSize &&
           dx * dx + dy * dy <= kRadius * kRadius;
  }

  size_t index(int x, int y) const {
    if (x < 0 || y < 0 || x >= kSize || y >= kSize) return kPixelCount;
    const Row& r = rows_[y];
    if (x < r.left || x >= r.left + r.width) return kPixelCount;
    return r.offset + x - r.left;
  }

  const Row& row(int y) const { return rows_[y]; }

 private:
  Row rows_[kSize];
};

}  // namespace ui::detail
