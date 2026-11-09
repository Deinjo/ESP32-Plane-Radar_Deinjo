#include "ui/radar_motion.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ui::radar::motion;

static void near(float actual, float expected, float tolerance = 0.001f) {
  assert(std::fabs(actual - expected) < tolerance);
}

int main() {
  Track flight;
  flight.update({100, 100}, {2, -1}, 1000, 0, true);
  near(flight.position(1500).x, 101);
  near(flight.position(1500).y, 99.5f);
  near(flight.position(11000).x, 120);
  near(flight.position(90000).x, 120);  // freeze after the prediction limit

  Track aged;
  aged.update({10, 20}, {1, 0}, 100, 8000, true);
  near(aged.position(100).x, 18);
  near(aged.position(2100).x, 20);
  near(aged.position(30000).x, 20);
  aged.update({10, 20}, {1, 0}, 30000, 30000, true);
  near(aged.position(31000).x, 20);

  Track reprojected;
  // A zoom/context reset reprojects the last sample but keeps its receipt
  // timestamp. It must not restart prediction of an already stale sample.
  reprojected.update({40, 50}, {2, 0}, 1000, 2000, true);
  near(reprojected.position(30000).x, 60);
  near(reprojected.position(35000).x, 60);

  Track invalid;
  invalid.update({50, 40}, {100, 100}, 1, 2000, false);
  near(invalid.position(9000).x, 50);
  near(invalid.position(9000).y, 40);

  Track wrapping;
  wrapping.update({0, 0}, {1, 0}, UINT32_MAX - 499, 0, true);
  near(wrapping.position(500).x, 1);

  const Point before = flight.position(4000);
  flight.update({107, 96}, {2, -1}, 4000, 0, true);
  near(flight.position(4000).x, before.x);  // no measurement-update jump
  near(flight.position(4000).y, before.y);
  near(flight.position(5000).x, 109);
  flight.update({220, 20}, {2, -1}, 5000, 0, true);
  near(flight.position(5000).x, 220);  // large jumps are not fictitious flights

  Track repeated;
  repeated.update({0, 0}, {1, 0}, 0, 0, true);
  for (uint32_t t = 1000; t <= 12000; t += 1000) {
    const Point previous = repeated.position(t);
    repeated.update({0, 0}, {1, 0}, t, t, true);
    near(repeated.position(t).x, previous.x);
  }
  near(repeated.position(30000).x, 10);

  near(approachAngle(359, 1, 1), 0);
  near(approachAngle(1, 359, 1), 0);

  const Rect north = trailingLabelBounds(100, 100, 0, 41, 21, 3, 1);
  assert(north.top == 104 && north.left == 80);
  const Rect east = trailingLabelBounds(100, 100, 90, 41, 21, 3, 1);
  assert(east.right == 96 && east.top == 90);
  const Rect south = trailingLabelBounds(100, 100, 180, 41, 21, 3, 1);
  assert(south.bottom == 96);
  const Rect west = trailingLabelBounds(100, 100, 270, 41, 21, 3, 1);
  assert(west.left == 104);
  // The nearest text edge stays at the tail gap when route/row sizes change.
  assert(trailingLabelBounds(100, 100, 90, 81, 39, 3, 1).right == 96);
  assert(trailingLabelBounds(100, 100, 0, 81, 39, 3, 1).top == 104);
  for (int heading = 0; heading < 360; heading += 5) {
    const Rect initial = trailingLabelBounds(100, 100, heading, 60, 30, 3, 1);
    for (int frame = 0; frame < 600; ++frame) {
      const Rect next = trailingLabelBounds(100 + frame, 100 - frame,
                                            heading, 60, 30, 3, 1);
      assert(next.left == initial.left + frame && next.top == initial.top - frame);
    }
  }
  assert(std::strcmp(displayIdentity("FRA-DUS", "DLH123", true), "FRA-DUS") == 0);
  assert(std::strcmp(displayIdentity("FRA-DUS", "DLH123", false), "DLH123") == 0);
  assert(std::strcmp(displayIdentity("", "DLH123", true), "DLH123") == 0);
  assert(std::strcmp(displayIdentity("", "", true), "") == 0);
  // Enumerate actual clip pixels and compare against the circle for positions
  // across all edges/corners. Bands must neither overlap nor erase background.
  for (int x = -40; x <= 240; x += 20) {
    for (int y = -40; y <= 240; y += 20) {
      const Rect box = {x, y, x + 79, y + 39};
      std::vector<int> pixels(80 * 40, 0);
      forEachCircleClip(box, 120, 120, 106, [&](const Rect& clip) {
        assert(clip.left >= box.left && clip.right <= box.right);
        assert(clip.top >= box.top && clip.bottom <= box.bottom);
        for (int py = clip.top; py <= clip.bottom; ++py)
          for (int px = clip.left; px <= clip.right; ++px)
            ++pixels[(py - y) * 80 + px - x];
      });
      for (int py = y; py < y + 40; ++py) {
        for (int px = x; px < x + 80; ++px) {
          const bool inside = (px - 120) * (px - 120) + (py - 120) * (py - 120) <= 106 * 106;
          assert(pixels[(py - y) * 80 + px - x] == (inside ? 1 : 0));
        }
      }
    }
  }
  // Outbound aircraft centre is outside, but its tail still crosses the ring.
  const Rect exiting = trailingLabelBounds(230, 120, 90, 61, 21, 3, 1);
  assert(rectIntersectsCircle(exiting, 120, 120, 106));
  assert(!rectInsideCircle(exiting, 120, 120, 106));
  const Rect gone = trailingLabelBounds(310, 120, 90, 61, 21, 3, 1);
  assert(!rectIntersectsCircle(gone, 120, 120, 106));

  std::puts("PASS: motion, age/freeze, invalid velocity, rollover, correction, repeated samples,");
  std::puts("      angle wrap, trailing attachment, resizing, partial circle clipping, route selection");
}
