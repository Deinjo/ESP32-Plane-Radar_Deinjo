#pragma once

#include <cstddef>
#include <cstdint>

namespace services::settings {

constexpr size_t kOtaPasswordMaxLen = 32;
constexpr int kTextScaleMinPercent = 80;
constexpr int kTextScaleMaxPercent = 130;
constexpr int kTextScaleDefaultPercent = 120;

/** Load persistent display and OTA settings from NVS. */
void init();

bool footerEnabled();
bool weatherEnabled();
bool temperatureFahrenheit();
bool altitudeMetres();
bool use24HourClock();
int textScalePercent();
bool nightModeEnabled();
uint16_t nightStartMinute();
uint16_t nightEndMinute();
bool nightModeActive(int local_minute);
bool useIataCodes();
bool preferRoute();
const char* otaPassword();

enum class ColorId : uint8_t {
  kBackground,
  kGrid,
  kLabel,
  kCenter,
  kAircraft,
  kTrackVector,
  kTagType,
  kTagAltitude,
  kRunway,
  kRunwayLabel,
  kFooterBackground,
  kRoad,
  kCity,
  kRoadPrimary,
};

/** Return a configured color as 0xRRGGBB. */
uint32_t color(ColorId id);

enum class VisibilityId : uint8_t {
  kGrid,
  kCenter,
  kLabel,
  kAircraft,
  kTrackVector,
  kTagType,
  kTagAltitude,
  kRunway,
  kRunwayLabel,
  kRoad,
  kCity,
  kRoadPrimary,
};

bool visible(VisibilityId id);

/**
 * Store web-portal values. An empty OTA password keeps the current password so
 * the portal never needs to echo the stored secret into its HTML.
 */
void saveFromPortal(const char* footer_checkbox, const char* weather_checkbox,
                    const char* fahrenheit_checkbox,
                    const char* altitude_metres_checkbox,
                     const char* clock24_checkbox,
                     const char* text_scale_percent_value,
                     const char* ota_password_value,
                     const char* night_enabled_checkbox,
                     const char* night_start_value,
                     const char* night_end_value,
                     const char* use_iata_checkbox,
                     const char* prefer_route_checkbox);

/** Store HTML color input values as persistent 0xRRGGBB colors. */
void saveColorsFromPortal(const char* background, const char* grid,
                          const char* label, const char* center,
                          const char* aircraft, const char* track_vector,
                          const char* tag_type, const char* tag_altitude,
                           const char* runway, const char* runway_label,
                           const char* footer_background, const char* road,
                           const char* city, const char* road_primary);

/** Store visibility checkbox values for drawable radar elements. */
void saveVisibilityFromPortal(const char* grid, const char* center,
                              const char* label, const char* aircraft,
                              const char* track_vector, const char* tag_type,
                               const char* tag_altitude, const char* runway,
                               const char* runway_label, const char* road,
                               const char* city, const char* road_primary);

/** Restore defaults during a full BOOT-button reset. */
void clear();

}  // namespace services::settings
