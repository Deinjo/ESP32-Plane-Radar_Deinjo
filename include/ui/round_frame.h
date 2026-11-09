#pragma once

#include <LovyanGFX.hpp>
#include <lgfx/v1/panel/Panel_FlexibleFrameBuffer.hpp>
#include <algorithm>

#include "ui/round_frame_layout.h"

namespace ui::detail {

// Keep the normal 240x240 drawing coordinates, RGB565 and alpha blending.
// Only storage is packed; LovyanGFX supplies clipping and pixel operations.
class RoundFramePanel final : public lgfx::Panel_FlexibleFrameBuffer {
 public:
  RoundFramePanel() {
    auto cfg = config();
    cfg.memory_width = cfg.panel_width = RoundFrameLayout::kSize;
    cfg.memory_height = cfg.panel_height = RoundFrameLayout::kSize;
    config(cfg);
  }

  bool init(bool) override {
    setRotation(0);
    return true;
  }

  void setBackground(uint16_t rgb565) {
    background_ = lgfx::color_convert<lgfx::swap565_t, lgfx::rgb565_t>(rgb565);
  }

  void pushTo(lgfx::LovyanGFX& target, int x, int y) const {
    target.startWrite();
    for (int row = 0; row < RoundFrameLayout::kSize; ++row) {
      const auto& r = layout_.row(row);
      target.pushImage(x + r.left, y + row, r.width, 1,
          reinterpret_cast<const lgfx::swap565_t*>(pixels_ + r.offset));
    }
    target.endWrite();
  }

 protected:
  uint32_t _read_pixel_inner(uint_fast16_t x, uint_fast16_t y) override {
    const size_t index = layout_.index(x, y);
    return index < RoundFrameLayout::kPixelCount ? pixels_[index] : background_;
  }

  void _draw_pixel_inner(uint_fast16_t x, uint_fast16_t y, uint32_t color) override {
    const size_t index = layout_.index(x, y);
    if (index < RoundFrameLayout::kPixelCount) pixels_[index] = color;
  }

  void _fill_rect_inner(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w,
                        uint_fast16_t h, uint32_t color) override {
    for (int row = y; row < y + h && row < RoundFrameLayout::kSize; ++row) {
      const auto& r = layout_.row(row);
      const int left = std::max<int>(x, r.left);
      const int right = std::min<int>(x + w, r.left + r.width);
      if (left < right) {
        std::fill(pixels_ + r.offset + left - r.left,
                  pixels_ + r.offset + right - r.left, static_cast<uint16_t>(color));
      }
    }
  }

 private:
  RoundFrameLayout layout_;
  uint16_t pixels_[RoundFrameLayout::kPixelCount] = {};
  uint16_t background_ = 0;
};

class RoundFrame final : public lgfx::LGFX_Device {
 public:
  explicit RoundFrame(lgfx::LovyanGFX* target) : target_(target) { setPanel(&panel_); }
  void setBackground(uint16_t color) { panel_.setBackground(color); }
  void pushSprite(int x, int y) { panel_.pushTo(*target_, x, y); }

 private:
  RoundFramePanel panel_;
  lgfx::LovyanGFX* target_;
};

}  // namespace ui::detail
