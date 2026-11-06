#include "ui/radar_display.h"

#include <lgfx/v1/lgfx_fonts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/radar_location.h"
#include "services/weather_time.h"
#include "ui/radar_range.h"
#include "ui/radar_theme.h"
#include "ui/runway_overlay.h"
#include "ui/motorway_map_30km.h"

namespace lgfx_fonts = lgfx::v1::fonts;

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorCenter = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTrackVector = 0xFFFF;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitude = 0xFFE0;
uint16_t kColorRunway = 0x4D5F;
uint16_t kColorRunwayLabel = 0x7DFF;
uint16_t kColorFooterBackground = 0x0084;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
bool s_cardinal_use_vlw = false;
bool s_scale_use_vlw = false;
float s_cardinal_vlw_size = 0.56f;
float s_scale_vlw_size = 0.50f;
float s_tag_vlw_size = 0.56f;
const lgfx::GFXfont* s_cardinal_gfx = &lgfx_fonts::FreeSansBold12pt7b;
const lgfx::GFXfont* s_scale_gfx = &lgfx_fonts::FreeSansBold9pt7b;
const lgfx::GFXfont* s_tag_gfx = &lgfx_fonts::FreeSansBold12pt7b;

bool s_tag_label_metrics_ready = false;
bool s_tag_use_vlw = false;
bool s_footer_metrics_ready = false;
bool s_footer_use_vlw = false;
float s_footer_vlw_size = 0.36f;

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

lgfx::LovyanGFX* s_draw = &tft;
LGFX_Sprite s_frame(&tft);
bool s_frame_ready = false;
bool s_last_frame_complete = false;
bool s_last_frame_used_sprite = false;
bool s_night_mode_known = false;
bool s_last_night_mode = false;
// Keep the full-screen frame buffer in static DRAM instead of allocating it
// from the live Wi-Fi/TLS heap after the network stack has started.
uint16_t s_frame_buffer[radar::kSize * radar::kSize] = {};

class DrawScope {
 public:
  explicit DrawScope(lgfx::LovyanGFX& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  lgfx::LovyanGFX* prev_;
};

float configuredTextScale() {
  return static_cast<float>(services::settings::textScalePercent()) / 100.0f;
}

void applyBitmapTextScale(lgfx::LGFXBase& gfx) {
  gfx.setTextSize(configuredTextScale());
}

int absDiff(int a, int b) { return std::abs(a - b); }

int measureGfxHeight(const lgfx::GFXfont& font) {
  tft.setFont(&font);
  tft.setTextSize(1);
  return tft.fontHeight();
}

int measureVlwHeight(float size) {
  tft.setTextSize(size);
  return tft.fontHeight();
}

float findVlwSizeForHeight(int target_px) {
  float lo = 0.25f;
  float hi = 1.2f;
  for (int i = 0; i < 16; ++i) {
    const float mid = (lo + hi) * 0.5f;
    if (measureVlwHeight(mid) < target_px) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

void applyScaleStyle();

const lgfx::GFXfont* pickGfxFontClosest(
    int target_px, const lgfx::GFXfont* const* candidates, size_t count) {
  const lgfx::GFXfont* best = candidates[0];
  int best_diff = absDiff(measureGfxHeight(*best), target_px);

  for (size_t i = 1; i < count; ++i) {
    const int diff = absDiff(measureGfxHeight(*candidates[i]), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = candidates[i];
    }
  }
  return best;
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  const int cardinal_target = radar::kCardinalLabelHeightPx;

  if (displayFontIsSmooth()) {
    s_cardinal_use_vlw = true;
    s_cardinal_vlw_size = findVlwSizeForHeight(cardinal_target);
    const int cardinal_h = measureVlwHeight(s_cardinal_vlw_size);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    s_scale_use_vlw = true;
    s_scale_vlw_size = findVlwSizeForHeight(scale_target);
  } else {
    const lgfx::GFXfont* cardinal_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_cardinal_gfx =
        pickGfxFontClosest(cardinal_target, cardinal_candidates, 2);
    s_cardinal_use_vlw = false;

    const int cardinal_h = measureGfxHeight(*s_cardinal_gfx);
    const int scale_target = cardinal_h - radar::kScaleBelowCardinalPx;
    const lgfx::GFXfont* scale_candidates[] = {
        &lgfx_fonts::FreeSansBold9pt7b, &lgfx_fonts::FreeSansBold12pt7b};
    s_scale_gfx = pickGfxFontClosest(scale_target, scale_candidates, 2);
    s_scale_use_vlw = false;
  }

  applyScaleStyle();
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangePresetCount; ++i) {
    for (bool miles : {false, true}) {
      radar::formatRing3Label(label, sizeof(label), radar::kRangePresets[i].ring3_km,
                              miles);
      const int w = tft.textWidth(label);
      if (w > s_scale_label_max_w) {
        s_scale_label_max_w = w;
      }
    }
  }

  s_label_metrics_ready = true;
}

void initTagLabelMetrics() {
  if (s_tag_label_metrics_ready) {
    return;
  }

  const int target = radar::kAircraftTagLabelHeightPx;
  if (displayFontIsSmooth()) {
    s_tag_use_vlw = true;
    s_tag_vlw_size = findVlwSizeForHeight(target);
  } else {
    const lgfx::GFXfont* tag_candidates[] = {
        &lgfx_fonts::FreeSansBold12pt7b, &lgfx_fonts::FreeSansBold9pt7b};
    s_tag_gfx = pickGfxFontClosest(target, tag_candidates, 2);
    s_tag_use_vlw = false;
  }

  s_tag_label_metrics_ready = true;
}

void initFooterMetrics() {
  if (s_footer_metrics_ready) {
    return;
  }
  if (displayFontIsSmooth()) {
    s_footer_use_vlw = true;
    s_footer_vlw_size =
        findVlwSizeForHeight(radar::kFooterLabelHeightPx);
  }
  s_footer_metrics_ready = true;
}

uint16_t configuredColor(services::settings::ColorId id) {
  const uint32_t value = services::settings::color(id);
  const uint8_t red = static_cast<uint8_t>((value >> 16) & 0xFFu);
  const uint8_t green = static_cast<uint8_t>((value >> 8) & 0xFFu);
  const uint8_t blue = static_cast<uint8_t>(value & 0xFFu);
  if (config::kDisplayRgbOrder) {
    return tft.color565(blue, green, red);
  }
  return tft.color565(red, green, blue);
}

void initPalette() {
  radar::kColorBackground = configuredColor(services::settings::ColorId::kBackground);
  radar::kColorGrid = configuredColor(services::settings::ColorId::kGrid);
  radar::kColorLabel = configuredColor(services::settings::ColorId::kLabel);
  radar::kColorCenter = configuredColor(services::settings::ColorId::kCenter);
  radar::kColorAircraft = configuredColor(services::settings::ColorId::kAircraft);
  radar::kColorTrackVector = configuredColor(services::settings::ColorId::kTrackVector);
  radar::kColorTagType = configuredColor(services::settings::ColorId::kTagType);
  radar::kColorTagAltitude = configuredColor(services::settings::ColorId::kTagAltitude);
  radar::kColorRunway = configuredColor(services::settings::ColorId::kRunway);
  radar::kColorRunwayLabel = configuredColor(services::settings::ColorId::kRunwayLabel);
  radar::kColorFooterBackground =
      configuredColor(services::settings::ColorId::kFooterBackground);
}

constexpr float kKmPerDeg = 111.0f;
constexpr float kDegToRad = 0.01745329252f;

void offsetKmFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                        float* dist_km) {
  const float center_lat =
      static_cast<float>(services::location::lat());
  const float lon_scale = cosf(center_lat * kDegToRad);

  *dx_km = static_cast<float>(lon - services::location::lon()) *
           kKmPerDeg * lon_scale;
  *dy_km = static_cast<float>(lat - services::location::lat()) *
           kKmPerDeg;
  *dist_km = sqrtf((*dx_km) * (*dx_km) + (*dy_km) * (*dy_km));
}

float innerRingMaxKm() {
  const float outer_km = radar::rangeCurrent().outer_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

/** Flat lat/lon as x/y: 1° ≈ 111 km, north = screen up. */
void latLonToScreen(float lat, float lon, int* out_x, int* out_y) {
  const float outer_km = radar::rangeCurrent().outer_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;

  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);

  *out_x = radar::kCenterX + static_cast<int>(lroundf(dx_km * px_per_km));
  *out_y = radar::kCenterY - static_cast<int>(lroundf(dy_km * px_per_km));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

bool isInsideOuterRing(int x, int y) {
  const int max_r = radar::kGridOuterRadius - radar::kAircraftInsideRingInsetPx;
  return distSqFromCenter(x, y) <= max_r * max_r;
}

struct MapCity {
  const char* label;
  float lat;
  float lon;
};

// First small manually maintained city set for the Ruhr area.
// Coordinates describe approximate city centres.
constexpr MapCity kMapCities[] = {
    {"DO",  51.5142f, 7.4684f},  // Dortmund
    {"WIT", 51.4333f, 7.3333f},  // Witten
    {"HA",  51.3671f, 7.4633f},  // Hagen
    {"BO",  51.4818f, 7.2162f},  // Bochum
    {"E",   51.4556f, 7.0116f},  // Essen

    {"WAL", 51.6219f, 7.3976f},  // Waltrop
    {"HER", 51.5369f, 7.2009f},  // Herne
    {"CAS", 51.5567f, 7.3116f},  // Castrop-Rauxel
    {"UN",  51.5340f, 7.6890f},  // Unna
    {"HAM", 51.6739f, 7.8150f},  // Hamm
    {"RE",  51.6141f, 7.1979f},  // Recklinghausen
    {"IS",  51.3755f, 7.7028f},  // Iserlohn
    {"GE",  51.5177f, 7.0857f},  // Gelsenkirchen
    {"W",   51.2707f, 7.1808f},  // Wuppertal
};

constexpr size_t kMapCityCount = sizeof(kMapCities) / sizeof(kMapCities[0]);

struct ScreenRect {
  int left;
  int top;
  int right;
  int bottom;
};

bool rectsOverlap(const ScreenRect& a, const ScreenRect& b, int margin = 0) {
  return a.left <= b.right + margin &&
         a.right + margin >= b.left &&
         a.top <= b.bottom + margin &&
         a.bottom + margin >= b.top;
}

ScreenRect cityLabelBounds(const MapCity& city, int x, int y) {
  constexpr int kLabelGapPx = 4;

  const int label_w = s_draw->textWidth(city.label);
  const int label_h = s_draw->fontHeight();
  const bool label_to_right = x < radar::kCenterX;

  const int left =
      label_to_right ? x + kLabelGapPx : x - kLabelGapPx - label_w;

  return {
      left,
      y - label_h / 2,
      left + label_w - 1,
      y + (label_h - 1) / 2,
  };
}

bool cityLabelOverlapsFixedRadarUi(const ScreenRect& label) {
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  // Conservative exclusion zones for the centre marker, cardinal labels,
  // and the range label on the east side.
  const ScreenRect protected_areas[] = {
      {cx - 18, cy - 12, cx + 18, cy + 12},  // radar centre
      {cx - 22, 0, cx + 22, 28},             // N
      {cx - 22, edge - 28, cx + 22, edge},   // S
      {0, cy - 18, 30, cy + 18},             // W
      {edge - 44, cy - 22, edge, cy + 22},   // E + range label
  };

  for (const ScreenRect& area : protected_areas) {
    if (rectsOverlap(label, area, 2)) {
      return true;
    }
  }

  return false;
}

bool cityTooCloseToAircraft(int city_x, int city_y) {
  constexpr int kAircraftClearancePx = 16;
  constexpr int kAircraftClearanceSq =
      kAircraftClearancePx * kAircraftClearancePx;

  const size_t aircraft_count = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  for (size_t i = 0; i < aircraft_count; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;

    offsetKmFromCenter(planes[i].lat, planes[i].lon,
                       &dx_km, &dy_km, &dist_km);

    if (!isInsideOuterRingKm(dist_km)) {
      continue;
    }

    int aircraft_x = 0;
    int aircraft_y = 0;
    latLonToScreen(planes[i].lat, planes[i].lon,
                   &aircraft_x, &aircraft_y);

    const int dx = city_x - aircraft_x;
    const int dy = city_y - aircraft_y;

    if (dx * dx + dy * dy <= kAircraftClearanceSq) {
      return true;
    }
  }

  return false;
}

/** Rim dot from true bearing; always on screen edge (even if target is 50+ km away). */
bool beyondRingEdgeDotFromLatLon(float lat, float lon, int* out_x, int* out_y) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  offsetKmFromCenter(lat, lon, &dx_km, &dy_km, &dist_km);
  if (dist_km < 0.01f) {
    return false;
  }
  if (isInsideOuterRingKm(dist_km)) {
    return false;
  }

  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
  const float angle_rad = atan2f(dx_km, dy_km);

  *out_x = cx + static_cast<int>(lroundf(sinf(angle_rad) * rim_r));
  *out_y = cy - static_cast<int>(lroundf(cosf(angle_rad) * rim_r));
  return true;
}

void drawBeyondRingDot(int x, int y) {
  s_draw->fillSmoothCircle(x, y, radar::kBeyondRingDotRadiusPx,
                           radar::kColorAircraft);
}

void clipPointToOuterRing(int x0, int y0, int* x1, int* y1) {
  const int max_r = radar::kGridOuterRadius;
  const int max_r_sq = max_r * max_r;
  if (distSqFromCenter(*x1, *y1) <= max_r_sq) {
    return;
  }

  const int dx = *x1 - x0;
  const int dy = *y1 - y0;
  float t = 1.0f;
  for (int step = 0; step < 20; ++step) {
    const int px = x0 + static_cast<int>(lroundf(dx * t));
    const int py = y0 + static_cast<int>(lroundf(dy * t));
    if (distSqFromCenter(px, py) <= max_r_sq) {
      *x1 = px;
      *y1 = py;
      return;
    }
    t -= 0.05f;
    if (t <= 0.0f) {
      *x1 = x0;
      *y1 = y0;
      return;
    }
  }
}

// Clip a complete line segment against the circular radar area. Clipping
// endpoints independently is not sufficient: a segment can start and end
// outside the circle while crossing through its visible area.
bool clipSegmentToOuterRing(int* x0, int* y0, int* x1, int* y1) {
  const float cx = static_cast<float>(radar::kCenterX);
  const float cy = static_cast<float>(radar::kCenterY);
  const float radius = static_cast<float>(radar::kGridOuterRadius);

  const float start_x = static_cast<float>(*x0) - cx;
  const float start_y = static_cast<float>(*y0) - cy;
  const float delta_x = static_cast<float>(*x1 - *x0);
  const float delta_y = static_cast<float>(*y1 - *y0);
  const float a = delta_x * delta_x + delta_y * delta_y;

  if (a < 0.0001f) {
    return start_x * start_x + start_y * start_y <= radius * radius;
  }

  const float b = 2.0f * (start_x * delta_x + start_y * delta_y);
  const float c = start_x * start_x + start_y * start_y - radius * radius;
  const float discriminant = b * b - 4.0f * a * c;

  float enter = 0.0f;
  float leave = 1.0f;
  if (discriminant < 0.0f) {
    // No boundary crossing. The segment is visible only if its start point
    // is inside; a segment wholly outside has no useful draw operation.
    if (c > 0.0f) {
      return false;
    }
  } else {
    const float root = sqrtf(discriminant);
    const float t0 = (-b - root) / (2.0f * a);
    const float t1 = (-b + root) / (2.0f * a);
    enter = fmaxf(0.0f, fminf(t0, t1));
    leave = fminf(1.0f, fmaxf(t0, t1));
  }

  if (enter > leave) {
    return false;
  }

  *x0 += static_cast<int>(lroundf(delta_x * enter));
  *y0 += static_cast<int>(lroundf(delta_y * enter));
  *x1 = *x0 + static_cast<int>(lroundf(delta_x * (leave - enter)));
  *y1 = *y0 + static_cast<int>(lroundf(delta_y * (leave - enter)));
  return true;
}

int speedLineLengthPx(float gs_knots) {
  if (gs_knots <= 0.0f) {
    return 0;
  }

  // Fixed screen scale: 60 s horizon at gs, not tied to current range zoom.
  constexpr float kKmPerKnotPerHorizon =
      1.852f * radar::kAircraftTrackHorizonSec / 3600.0f;
  const float px =
      gs_knots * kKmPerKnotPerHorizon * radar::kGridOuterRadius /
      radar::kAircraftTrackRefOuterKm * radar::kAircraftTrackLengthScale;

  const int len = static_cast<int>(px + 0.5f);
  if (len < radar::kAircraftSpeedLineMinPx) {
    return radar::kAircraftSpeedLineMinPx;
  }
  return len;
}

void noseTip(int cx, int cy, float heading_deg, int* tip_x, int* tip_y) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  *tip_x = cx + static_cast<int>(lroundf(sinf(rad) * radar::kAircraftNoseLenPx));
  *tip_y = cy - static_cast<int>(lroundf(cosf(rad) * radar::kAircraftNoseLenPx));
}

void drawHeadingTriangle(int cx, int cy, float heading_deg, uint16_t color) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  const int base_x =
      cx - static_cast<int>(lroundf(sin_h * static_cast<float>(radar::kAircraftTailLenPx)));
  const int base_y =
      cy + static_cast<int>(lroundf(cos_h * static_cast<float>(radar::kAircraftTailLenPx)));

  const int wing_x = static_cast<int>(lroundf(cos_h * radar::kAircraftTailHalfPx));
  const int wing_y = static_cast<int>(lroundf(sin_h * radar::kAircraftTailHalfPx));

  s_draw->fillTriangle(tip_x, tip_y, base_x + wing_x, base_y + wing_y,
                       base_x - wing_x, base_y - wing_y, color);
}

void drawSpeedVector(int cx, int cy, float heading_deg, float track_deg,
                     float gs_knots, uint16_t color) {
  const int len = speedLineLengthPx(gs_knots);
  if (len <= 0) {
    return;
  }

  int tip_x = 0;
  int tip_y = 0;
  noseTip(cx, cy, heading_deg, &tip_x, &tip_y);

  constexpr float kDegToRad = 0.01745329252f;
  const float rad = track_deg * kDegToRad;
  int ex = tip_x + static_cast<int>(lroundf(sinf(rad) * len));
  int ey = tip_y - static_cast<int>(lroundf(cosf(rad) * len));
  clipPointToOuterRing(tip_x, tip_y, &ex, &ey);
  if (ex == tip_x && ey == tip_y) {
    return;
  }
  s_draw->drawWideLine(tip_x, tip_y, ex, ey, radar::kAircraftTrackLineHalfWidth,
                       color);
}

void applyTagStyle() {
  if (s_tag_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_tag_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_tag_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

int measureTagBlockWidth(const services::adsb::Aircraft& plane) {
  applyTagStyle();
  int max_w = 0;
  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    const int w = s_draw->textWidth(identity);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.type[0] != '\0') {
    const int w = s_draw->textWidth(plane.type);
    if (w > max_w) {
      max_w = w;
    }
  }
  if (plane.alt[0] != '\0') {
    const int w = s_draw->textWidth(plane.alt);
    if (w > max_w) {
      max_w = w;
    }
  }
  return max_w;
}

void drawAircraftTag(int x, int y, const services::adsb::Aircraft& plane) {
  if (!services::settings::visible(
          services::settings::VisibilityId::kLabel)) {
    return;
  }
  initTagLabelMetrics();
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  const int block_w = measureTagBlockWidth(plane);
  const int block_h = line_h * 3;
  int ly = y - block_h / 2;

  const int symbol_half =
      radar::kAircraftNoseLenPx + radar::kAircraftTailHalfPx;
  // West (left): tag toward center on the right; east (right): tag on the left.
  const bool tag_on_right = x < radar::kCenterX;
  int anchor_x = 0;
  if (tag_on_right) {
    anchor_x = x + symbol_half + radar::kAircraftLabelGapPx;
    anchor_x = std::min(anchor_x, radar::kSize - block_w - 1);
    s_draw->setTextDatum(textdatum_t::top_left);
  } else {
    anchor_x = x - symbol_half - radar::kAircraftLabelGapPx;
    anchor_x = std::max(anchor_x, block_w + 1);
    s_draw->setTextDatum(textdatum_t::top_right);
  }
  ly = std::max(1, std::min(ly, radar::kSize - block_h - 1));

  const char* identity =
      plane.route[0] != '\0' ? plane.route : plane.callsign;
  if (identity[0] != '\0') {
    s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
    s_draw->drawString(identity, anchor_x, ly);
  }
  ly += line_h;

  if (plane.type[0] != '\0' &&
      services::settings::visible(
          services::settings::VisibilityId::kTagType)) {
    s_draw->setTextColor(radar::kColorTagType, radar::kColorBackground);
    s_draw->drawString(plane.type, anchor_x, ly);
  }
  ly += line_h;

  if (plane.alt[0] != '\0' &&
      services::settings::visible(
          services::settings::VisibilityId::kTagAltitude)) {
    s_draw->setTextColor(radar::kColorTagAltitude, radar::kColorBackground);
    s_draw->drawString(plane.alt, anchor_x, ly);
  }
}

void applyFooterStyle() {
  initFooterMetrics();
  if (s_footer_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_footer_vlw_size * configuredTextScale());
  } else {
    s_draw->setFont(&lgfx_fonts::Font0);
    applyBitmapTextScale(*s_draw);
  }
}

void fitFooterText(const char* source, char* out, size_t out_len,
                   int max_width) {
  if (out_len == 0) {
    return;
  }
  strncpy(out, source != nullptr ? source : "", out_len - 1);
  out[out_len - 1] = '\0';
  if (s_draw->textWidth(out) <= max_width) {
    return;
  }

  size_t length = strlen(out);
  while (length > 3) {
    length -= 1;
    out[length] = '\0';
    if (length >= 3) {
      out[length - 3] = '.';
      out[length - 2] = '.';
      out[length - 1] = '.';
    }
    if (s_draw->textWidth(out) <= max_width) {
      return;
    }
  }
}

void drawFooterLine(const char* text, int y, int max_width, uint16_t color) {
  if (text == nullptr || text[0] == '\0') {
    return;
  }
  applyFooterStyle();
  char fitted[32] = {};
  fitFooterText(text, fitted, sizeof(fitted), max_width);
  s_draw->setTextDatum(textdatum_t::top_center);
  s_draw->setTextColor(color, radar::kColorFooterBackground);
  s_draw->drawString(fitted, radar::kCenterX, y);
}

void drawFooter() {
  if (!services::settings::footerEnabled()) {
    return;
  }

  // The trapezoid follows the narrowing bottom edge of the round panel.
  s_draw->fillTriangle(28, radar::kFooterTopY, 212, radar::kFooterTopY, 168,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->fillTriangle(28, radar::kFooterTopY, 168, radar::kFooterBottomY, 72,
                       radar::kFooterBottomY,
                       radar::kColorFooterBackground);
  s_draw->drawFastHLine(44, radar::kFooterTopY, 152, radar::kColorGrid);

  if (services::settings::weatherEnabled()) {
    char weather[32] = {};
    services::weather::formatWeatherLine(weather, sizeof(weather));
    drawFooterLine(weather, radar::kFooterWeatherY, 176,
                   radar::kColorTagType);
  }

  char date_time[20] = {};
  services::weather::formatDateTimeLine(date_time, sizeof(date_time));
  const int time_y = services::settings::weatherEnabled()
                         ? radar::kFooterTimeY
                         : radar::kFooterTimeOnlyY;
  drawFooterLine(date_time, time_y, 128,
                 radar::kColorTagAltitude);
}

struct AircraftDrawItem {
  size_t index = 0;
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

struct BeyondDotDrawItem {
  int x = 0;
  int y = 0;
  int dist_sq = 0;
};

void sortDrawItemsFarFirst(AircraftDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const AircraftDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void sortBeyondDotsFarFirst(BeyondDotDrawItem* items, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const BeyondDotDrawItem key = items[i];
    size_t j = i;
    while (j > 0 && items[j - 1].dist_sq < key.dist_sq) {
      items[j] = items[j - 1];
      --j;
    }
    items[j] = key;
  }
}

void drawAircraft() {
  initLabelMetrics();

  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  AircraftDrawItem items[services::adsb::kMaxAircraft];
  BeyondDotDrawItem dots[services::adsb::kMaxAircraft];
  size_t draw_count = 0;
  size_t dot_count = 0;

  for (size_t i = 0; i < n; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    offsetKmFromCenter(planes[i].lat, planes[i].lon, &dx_km, &dy_km, &dist_km);

    if (isInsideOuterRingKm(dist_km)) {
      int x = 0;
      int y = 0;
      latLonToScreen(planes[i].lat, planes[i].lon, &x, &y);
      items[draw_count].index = i;
      items[draw_count].x = x;
      items[draw_count].y = y;
      items[draw_count].dist_sq = distSqFromCenter(x, y);
      ++draw_count;
      continue;
    }

    int dot_x = 0;
    int dot_y = 0;
    if (!beyondRingEdgeDotFromLatLon(planes[i].lat, planes[i].lon, &dot_x,
                                     &dot_y)) {
      continue;
    }
    dots[dot_count].x = dot_x;
    dots[dot_count].y = dot_y;
    dots[dot_count].dist_sq = distSqFromCenter(dot_x, dot_y);
    ++dot_count;
  }

  sortBeyondDotsFarFirst(dots, dot_count);
  for (size_t d = 0; d < dot_count; ++d) {
    if (services::settings::visible(
            services::settings::VisibilityId::kAircraft)) {
      drawBeyondRingDot(dots[d].x, dots[d].y);
    }
  }

  sortDrawItemsFarFirst(items, draw_count);
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    const int x = items[d].x;
    const int y = items[d].y;
    if (services::settings::visible(
            services::settings::VisibilityId::kTrackVector)) {
      drawSpeedVector(x, y, planes[i].nose_deg, planes[i].track_deg,
                      planes[i].gs_knots, radar::kColorTrackVector);
    }
    if (services::settings::visible(
            services::settings::VisibilityId::kAircraft)) {
      drawHeadingTriangle(x, y, planes[i].nose_deg, radar::kColorAircraft);
    }
  }
  for (size_t d = 0; d < draw_count; ++d) {
    const size_t i = items[d].index;
    drawAircraftTag(items[d].x, items[d].y, planes[i]);
  }
}

void applyCardinalStyle() {
  if (s_cardinal_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_cardinal_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_cardinal_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void applyScaleStyle() {
  if (s_scale_use_vlw) {
    displayFontSetSmoothSize(*s_draw,
                             s_scale_vlw_size * configuredTextScale());
  } else {
    displayFontSetBitmap(*s_draw, s_scale_gfx);
    applyBitmapTextScale(*s_draw);
  }
}

void drawCardinalLabel(const char* text, int x, int y, textdatum_t datum) {
  applyCardinalStyle();
  s_draw->setTextDatum(datum);
  s_draw->setTextColor(radar::kColorLabel, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  applyScaleStyle();
  s_draw->setTextDatum(textdatum_t::middle_right);

  const int tw = s_draw->textWidth(text);
  const int th = s_draw->fontHeight();
  constexpr int kPadX = 3;
  constexpr int kPadY = 2;

  const int left = x - tw - kPadX;
  const int top = y - th / 2 - kPadY;

  s_draw->fillRect(left, top, tw + kPadX * 2, th + kPadY * 2,
                   radar::kColorBackground);
  s_draw->setTextColor(radar::kColorGrid, radar::kColorBackground);
  s_draw->drawString(text, x, y);
}

struct RoadDrawStyle {
  uint16_t color;
  uint8_t half_width;
};

RoadDrawStyle roadDrawStyle(MapRoadKind kind) {
  switch (kind) {
    case MapRoadKind::kMotorway:
      // Visible but still behind cities, runways and aircraft.
      return {configuredColor(services::settings::ColorId::kRoad), 0};

    case MapRoadKind::kPrimary:
      // More subtle than a motorway.
      return {configuredColor(services::settings::ColorId::kRoadPrimary), 0};
  }

  // Defensive fallback; should not be reached with the current enum.
  return {configuredColor(services::settings::ColorId::kRoad), 0};
}

void drawRoadOverlay() {
  // Subtle dark-grey motorway layer. It should remain behind cities,
  // runways and aircraft.
  for (size_t road_index = 0; road_index < kMapRoadCount; ++road_index) {
    const MapRoad& road = kMapRoads[road_index];
    const services::settings::VisibilityId visibility_id =
        road.kind == MapRoadKind::kPrimary
            ? services::settings::VisibilityId::kRoadPrimary
            : services::settings::VisibilityId::kRoad;
    if (!services::settings::visible(visibility_id)) {
      continue;
    }
    const RoadDrawStyle style = roadDrawStyle(road.kind);

    if (road.point_count < 2) {
      continue;
    }

    for (size_t point_index = 1;
         point_index < road.point_count;
         ++point_index) {
      int x0 = 0;
      int y0 = 0;
      int x1 = 0;
      int y1 = 0;

      latLonToScreen(road.points[point_index - 1].lat,
                     road.points[point_index - 1].lon,
                     &x0, &y0);

      latLonToScreen(road.points[point_index].lat,
                     road.points[point_index].lon,
                     &x1, &y1);

      if (!clipSegmentToOuterRing(&x0, &y0, &x1, &y1)) {
        continue;
      }

      if (style.half_width == 0) {
        s_draw->drawLine(x0, y0, x1, y1, style.color);
      } else {
        s_draw->drawWideLine(x0, y0, x1, y1, style.half_width, style.color);
      }
    }
  }
}

void drawCityOverlay() {
  // Cities are orientation aids. Aircraft, runway labels and radar UI
  // always have priority over city labels.
  if (!services::settings::visible(services::settings::VisibilityId::kCity)) {
    return;
  }

  const uint16_t city_color =
      configuredColor(services::settings::ColorId::kCity);

  s_draw->setFont(&lgfx_fonts::Font0);
  s_draw->setTextSize(1);
  s_draw->setTextColor(city_color, radar::kColorBackground);

  ScreenRect accepted_labels[kMapCityCount];
  size_t accepted_label_count = 0;

  for (size_t i = 0; i < kMapCityCount; ++i) {
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;

    offsetKmFromCenter(kMapCities[i].lat, kMapCities[i].lon,
                       &dx_km, &dy_km, &dist_km);

    if (!isInsideOuterRingKm(dist_km)) {
      continue;
    }

    int x = 0;
    int y = 0;
    latLonToScreen(kMapCities[i].lat, kMapCities[i].lon, &x, &y);

    const ScreenRect label = cityLabelBounds(kMapCities[i], x, y);

    bool hidden = cityLabelOverlapsFixedRadarUi(label) ||
                  cityTooCloseToAircraft(x, y);

    // Earlier entries in kMapCities have higher display priority.
    for (size_t accepted = 0;
         !hidden && accepted < accepted_label_count;
         ++accepted) {
      if (rectsOverlap(label, accepted_labels[accepted], 2)) {
        hidden = true;
      }
    }

    if (hidden) {
      continue;
    }

    s_draw->fillCircle(x, y, 2, city_color);

    const bool label_to_right = x < radar::kCenterX;
    s_draw->setTextDatum(label_to_right ? textdatum_t::middle_left
                                        : textdatum_t::middle_right);

    const int label_x = label_to_right ? x + 4 : x - 4;
    s_draw->drawString(kMapCities[i].label, label_x, y);

    accepted_labels[accepted_label_count] = label;
    ++accepted_label_count;
  }
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }

  // Dash and gap lengths are measured in display pixels, not degrees.
  // This keeps the visual dash size consistent for all ring diameters.
  constexpr float kDashLengthPx = 8.0f;
  constexpr float kGapLengthPx = 8.0f;
  constexpr float kDashPeriodPx = kDashLengthPx + kGapLengthPx;

  constexpr float kTwoPi = 6.28318530718f;
  constexpr float kSampleSpacingPx = 0.5f;

  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));

  // Use a small angular step which corresponds to about half a pixel
  // on this ring. Larger rings therefore get more samples.
  const float angle_step = kSampleSpacingPx / static_cast<float>(r);

  for (float angle = 0.0f; angle < kTwoPi; angle += angle_step) {
    // Arc length from the 3-o'clock position, measured in pixels.
    const float arc_px = angle * static_cast<float>(r);
    const float dash_phase = fmodf(arc_px, kDashPeriodPx);

    // Skip the gap portion of each dash period.
    if (dash_phase >= kDashLengthPx) {
      continue;
    }

    const float cos_angle = cosf(angle);
    const float sin_angle = sinf(angle);

    for (int i = 0; i < thickness && r - i > 0; ++i) {
      const int rr = r - i;
      const int x = cx + static_cast<int>(lroundf(cos_angle * rr));
      const int y = cy + static_cast<int>(lroundf(sin_angle * rr));

      s_draw->drawPixel(x, y, color);
    }
  }
}

void drawRings(int cx, int cy, int outer_radius) {
  if (!services::settings::visible(services::settings::VisibilityId::kGrid)) {
    return;
  }
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawCrosshairs(int cx, int cy, int radius, uint16_t color) {
  if (!services::settings::visible(services::settings::VisibilityId::kGrid)) {
    return;
  }
  s_draw->drawWideLine(cx, cy - radius, cx, cy + radius,
                       radar::kGridStrokeHalfWidth, color);
  s_draw->drawWideLine(cx - radius, cy, cx + radius, cy,
                       radar::kGridStrokeHalfWidth, color);
}

void drawCenterDot(int cx, int cy) {
  if (!services::settings::visible(services::settings::VisibilityId::kCenter)) {
    return;
  }
  s_draw->fillSmoothCircle(cx, cy, radar::kCenterDotRadius, radar::kColorCenter);
}

void drawCardinalLabels() {
  if (!services::settings::visible(services::settings::VisibilityId::kLabel)) {
    return;
  }
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int edge = radar::kSize - 1;

  drawCardinalLabel("N", cx, radar::kCardinalNorthOffsetY, textdatum_t::top_center);
  drawCardinalLabel("S", cx, edge + radar::kCardinalSouthOffsetY,
                    textdatum_t::bottom_center);
  drawCardinalLabel("W", 0, cy, textdatum_t::middle_left);
  drawCardinalLabel("E", edge, cy, textdatum_t::middle_right);
}

int scaleLabelAnchorX(int cx, int outer_radius) {
  return cx + outer_radius - radar::kScaleGapFromOuterRing;
}

void drawScaleLabel(int cx, int cy, int outer_radius) {
  if (!services::settings::visible(services::settings::VisibilityId::kLabel)) {
    return;
  }
  char scale_label[12];
  radar::formatCurrentRing3Label(scale_label, sizeof(scale_label));
  drawScaleLabelWithBackground(scale_label,
                               scaleLabelAnchorX(cx, outer_radius), cy);
}

template <typename Gfx>
void drawStaticGrid(Gfx& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  displayFontEnsureLoaded(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);

  initPalette();
  drawRings(cx, cy, grid_r);
  drawCrosshairs(cx, cy, grid_r, radar::kColorGrid);
  drawRoadOverlay();
  drawCityOverlay();
  runway::drawLargeAirportRunways(gfx);

  drawCenterDot(cx, cy);
  drawCardinalLabels();
  drawScaleLabel(cx, cy, grid_r);
  gfx.setTextDatum(textdatum_t::top_left);
}

bool ensureFrameSprite() {
  if (s_frame_ready) {
    return true;
  }
  s_frame.setColorDepth(16);
  s_frame.setBuffer(s_frame_buffer, radar::kSize, radar::kSize, 16);
  if (s_frame.getBuffer() == nullptr) {
    Serial.println("radar: frame sprite alloc failed");
    return false;
  }
  s_frame_ready = true;
  return true;
}

// Double-buffered frame: composite the grid AND aircraft into the off-screen
// sprite, then blit it to the panel in a single pushSprite. Because the panel
// is updated in one pass, labels never show an erase/redraw gap — no flicker.
void renderFrame() {
  drawStaticGrid(s_frame);  // opens its own DrawScope(s_frame)
  {
    const DrawScope scope(s_frame);
    drawAircraft(); 
    drawFooter();
  }
  s_frame.pushSprite(0, 0);
  tft.setTextDatum(textdatum_t::top_left);
  s_last_frame_complete = true;
  s_last_frame_used_sprite = true;
}

}  // namespace

bool nightModeActive() {
  return services::settings::nightModeActive(
      services::weather::localMinuteOfDay());
}

void radarDisplayDraw() {
  const bool night = nightModeActive();
  s_night_mode_known = true;
  s_last_night_mode = night;
  if (night) {
    tft.fillScreen(0x0000);
    s_last_frame_complete = true;
    s_last_frame_used_sprite = false;
    return;
  }

  initPalette();
  initLabelMetrics();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  // Fallback when the sprite can't be allocated: draw straight to the panel.
  const DrawScope scope(tft);
  drawStaticGrid(tft);
  drawAircraft(); 
  drawFooter();
  tft.setTextDatum(textdatum_t::top_left);
  s_last_frame_complete = true;
  s_last_frame_used_sprite = false;
}

void radarDisplayRefreshAircraft() {
  if (nightModeActive()) {
    return;
  }
  initPalette();

  if (ensureFrameSprite()) {
    renderFrame();
    return;
  }

  radarDisplayDraw();
}

void radarDisplayTick() {
  const bool night = nightModeActive();
  if (!s_night_mode_known) {
    s_night_mode_known = true;
    s_last_night_mode = night;
    return;
  }
  if (night != s_last_night_mode) {
    radarDisplayDraw();
  }
}

void radarDisplayWriteBmp(Print& output) {
  constexpr int kBmpWidth = radar::kSize;
  constexpr int kBmpHeight = radar::kSize;
  constexpr uint32_t kHeaderSize = 54;
  constexpr uint32_t kRowSize = kBmpWidth * 3;
  constexpr uint32_t kImageSize = kRowSize * kBmpHeight;
  constexpr uint32_t kFileSize = kHeaderSize + kImageSize;

  uint8_t header[kHeaderSize] = {};
  header[0] = 'B';
  header[1] = 'M';
  auto put32 = [&header](size_t offset, uint32_t value) {
    header[offset] = static_cast<uint8_t>(value);
    header[offset + 1] = static_cast<uint8_t>(value >> 8);
    header[offset + 2] = static_cast<uint8_t>(value >> 16);
    header[offset + 3] = static_cast<uint8_t>(value >> 24);
  };
  put32(2, kFileSize);
  put32(10, kHeaderSize);
  put32(14, 40);
  put32(18, kBmpWidth);
  put32(22, kBmpHeight);
  header[26] = 1;
  header[28] = 24;
  put32(34, kImageSize);
  output.write(header, sizeof(header));

  const bool black = nightModeActive();
  uint8_t row[kRowSize];
  for (int y = kBmpHeight - 1; y >= 0; --y) {
    for (int x = 0; x < kBmpWidth; ++x) {
      uint16_t pixel = 0;
      if (!black && s_last_frame_used_sprite && s_frame_ready) {
        pixel = s_frame.readPixel(x, y);
      } else if (!black && s_last_frame_complete) {
        pixel = tft.readPixel(x, y);
      }
      uint8_t red = static_cast<uint8_t>((pixel >> 11) & 0x1F);
      const uint8_t green = static_cast<uint8_t>((pixel >> 5) & 0x3F);
      uint8_t blue = static_cast<uint8_t>(pixel & 0x1F);
      if (config::kDisplayRgbOrder) {
        // The sprite stores the panel's BGR-adjusted RGB565 values. Convert
        // them back to logical RGB before writing the browser image.
        const uint8_t channel = red;
        red = blue;
        blue = channel;
      }
      row[x * 3] = static_cast<uint8_t>((blue << 3) | (blue >> 2));
      row[x * 3 + 1] = static_cast<uint8_t>((green << 2) | (green >> 4));
      row[x * 3 + 2] = static_cast<uint8_t>((red << 3) | (red >> 2));
    }
    output.write(row, sizeof(row));
  }
}

bool radarDisplayFrameReady() { return s_last_frame_complete; }

}  // namespace ui
