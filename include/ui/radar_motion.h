#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui::radar::motion {

constexpr uint32_t kPredictionLimitMs = 10000;
constexpr uint32_t kCorrectionDurationMs = 1000;
constexpr float kMaxCorrectionPx = 24.0f;

struct Rect {
  int left;
  int top;
  int right;
  int bottom;
};

struct Point {
  float x;
  float y;
  constexpr Point(float px = 0.0f, float py = 0.0f) : x(px), y(py) {}
};

inline float distance(Point a, Point b) {
  const float dx = a.x - b.x;
  const float dy = a.y - b.y;
  return std::sqrt(dx * dx + dy * dy);
}

inline float approachAngle(float current, float target, float max_step) {
  const float delta = std::fmod(target - current + 540.0f, 360.0f) - 180.0f;
  const float step = std::max(-max_step, std::min(delta, max_step));
  return std::fmod(current + step + 360.0f, 360.0f);
}

// A single deterministic attachment behind the displayed triangle. Text stays
// horizontal. No collision placement, inward clamping or layout state exists.
inline Rect trailingLabelBounds(int x, int y, float heading_deg, int width,
                                int height, int tail_length, int gap) {
  const float radians = heading_deg * 0.01745329252f;
  const float dx = -std::sin(radians);
  const float dy = std::cos(radians);
  const float half_width = static_cast<float>(width - 1) * 0.5f;
  const float half_height = static_cast<float>(height - 1) * 0.5f;
  const float distance = tail_length + gap + std::fabs(dx) * half_width +
                         std::fabs(dy) * half_height;
  // Round the relative offset, not the absolute position: half-pixel ties
  // must not change when an aircraft crosses screen coordinate zero.
  const int left = x + static_cast<int>(std::lround(dx * distance - half_width));
  const int top = y + static_cast<int>(std::lround(dy * distance - half_height));
  return {left, top, left + width - 1, top + height - 1};
}

inline const char* displayIdentity(const char* route, const char* callsign,
                                    bool prefer_route) {
  return prefer_route && route[0] != '\0' ? route : callsign;
}

inline bool rectIntersectsCircle(const Rect& bounds, int cx, int cy, int radius) {
  if (bounds.left > bounds.right || bounds.top > bounds.bottom) return false;
  const int dx = std::max(bounds.left, std::min(cx, bounds.right)) - cx;
  const int dy = std::max(bounds.top, std::min(cy, bounds.bottom)) - cy;
  return dx * dx + dy * dy <= radius * radius;
}

inline bool rectInsideCircle(const Rect& bounds, int cx, int cy, int radius) {
  const int dx = std::max(std::abs(bounds.left - cx), std::abs(bounds.right - cx));
  const int dy = std::max(std::abs(bounds.top - cy), std::abs(bounds.bottom - cy));
  return dx * dx + dy * dy <= radius * radius;
}

// Produce nonoverlapping rectangular clip bands. Interior objects draw once;
// only edge objects need narrow bands. No extra framebuffer or heap allocation.
template <typename Draw>
void forEachCircleClip(const Rect& bounds, int cx, int cy, int radius, Draw draw) {
  if (!rectIntersectsCircle(bounds, cx, cy, radius)) return;
  if (rectInsideCircle(bounds, cx, cy, radius)) {
    draw(bounds);
    return;
  }
  Rect band = {1, 1, 0, 0};
  for (int y = std::max(bounds.top, cy - radius);
       y <= std::min(bounds.bottom, cy + radius); ++y) {
    const int dy = y - cy;
    const int half = static_cast<int>(std::sqrt(static_cast<float>(radius * radius - dy * dy)));
    const int left = std::max(bounds.left, cx - half);
    const int right = std::min(bounds.right, cx + half);
    if (left > right) continue;
    if (band.left == left && band.right == right && band.bottom == y - 1) {
      band.bottom = y;
    } else {
      if (band.left <= band.right) draw(band);
      band = {left, y, right, y};
    }
  }
  if (band.left <= band.right) draw(band);
}

// Display-only dead reckoning. Coordinates and velocity use screen pixels.
// Unsigned elapsed times deliberately support the millis() rollover.
struct Track {
  Point observed;
  Point velocity;
  Point correction;
  uint32_t received_ms = 0;
  uint32_t source_age_ms = 0;
  bool ready = false;

  Point position(uint32_t now_ms) const {
    const uint32_t elapsed = now_ms - received_ms;
    const uint32_t remaining = kPredictionLimitMs - source_age_ms;
    const uint32_t extrapolated = std::min(elapsed, remaining);
    const float seconds = static_cast<float>(source_age_ms + extrapolated) *
                          0.001f;
    const float blend = elapsed >= kCorrectionDurationMs
                            ? 0.0f
                            : 1.0f - static_cast<float>(elapsed) /
                                         kCorrectionDurationMs;
    return {observed.x + velocity.x * seconds + correction.x * blend,
            observed.y + velocity.y * seconds + correction.y * blend};
  }

  void update(Point measured, Point pixels_per_second, uint32_t now_ms,
              uint32_t age_ms, bool valid_motion) {
    const Point previous = position(now_ms);
    const bool was_ready = ready;
    observed = measured;
    velocity = valid_motion ? pixels_per_second : Point{};
    received_ms = now_ms;
    source_age_ms = std::min(age_ms, kPredictionLimitMs);
    correction = {};
    const Point next = position(now_ms);
    // Small measurement corrections are blended; teleports/re-acquisition
    // must not draw a fictitious flight across the radar.
    if (was_ready && distance(previous, next) <= kMaxCorrectionPx) {
      correction = {previous.x - next.x, previous.y - next.y};
    }
    ready = true;
  }
};

}  // namespace ui::radar::motion
