#include "services/display_settings.h"

#include <Preferences.h>

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace services::settings {
namespace {

constexpr char kPrefsNamespace[] = "display";
constexpr char kKeyFooter[] = "footer";
constexpr char kKeyWeather[] = "weather";
constexpr char kKeyFahrenheit[] = "tempF";
constexpr char kKeyAltitudeMetres[] = "altM";
constexpr char kKeyClock24[] = "time24";
constexpr char kKeyTextScale[] = "fontPct";
constexpr char kKeyNightEnabled[] = "nightOn";
constexpr char kKeyNightStart[] = "nightStart";
constexpr char kKeyNightEnd[] = "nightEnd";
constexpr char kKeyUseIata[] = "useIata";
constexpr char kKeyPreferRoute[] = "showRoute";
constexpr char kKeyOtaPassword[] = "otaPass";
constexpr char kKeyColorBackground[] = "colBg";
constexpr char kKeyColorGrid[] = "colGrid";
constexpr char kKeyColorLabel[] = "colLbl";
constexpr char kKeyColorCenter[] = "colCtr";
constexpr char kKeyColorAircraft[] = "colAc";
constexpr char kKeyColorTrack[] = "colTrk";
constexpr char kKeyColorTagType[] = "colTyp";
constexpr char kKeyColorTagAltitude[] = "colAlt";
constexpr char kKeyColorRunway[] = "colRw";
constexpr char kKeyColorRunwayLabel[] = "colRwLbl";
constexpr char kKeyColorFooter[] = "colFoot";
constexpr char kKeyColorRoad[] = "colRoad";
constexpr char kKeyColorCity[] = "colCity";
constexpr char kKeyColorRoadPrimary[] = "colRoad2";
constexpr char kKeyShowGrid[] = "showGrid";
constexpr char kKeyShowCenter[] = "showCtr";
constexpr char kKeyShowLabel[] = "showLbl";
constexpr char kKeyShowAircraft[] = "showAc";
constexpr char kKeyShowTrack[] = "showTrk";
constexpr char kKeyShowTagType[] = "showTyp";
constexpr char kKeyShowTagAltitude[] = "showAlt";
constexpr char kKeyShowRunway[] = "showRw";
constexpr char kKeyShowRunwayLabel[] = "showRwLbl";
constexpr char kKeyShowRoad[] = "showRoad";
constexpr char kKeyShowCity[] = "showCity";
constexpr char kKeyShowRoadPrimary[] = "showRoad2";

char s_ota_password[kOtaPasswordMaxLen + 1] = {};
bool s_footer_enabled = true;
bool s_weather_enabled = true;
bool s_temperature_fahrenheit = false;
bool s_altitude_metres = false;
bool s_use_24_hour_clock = true;
int s_text_scale_percent = kTextScaleDefaultPercent;
bool s_night_enabled = false;
bool s_use_iata = false;
bool s_prefer_route = true;
uint16_t s_night_start = 22 * 60;
uint16_t s_night_end = 7 * 60;

uint32_t s_colors[] = {
    0x040A1Cu, 0x106420u, 0xFFFFFFu, 0xFFFFFFu, 0xFF0000u,
    0xFF00FFu, 0xFFC800u, 0x5AC8FFu, 0x3896AAu, 0x6ED2E6u,
    0x031020u, 0x69737Du, 0xAAAAAAu, 0x3C4650u,
};

constexpr uint32_t kDefaultColors[] = {
    0x040A1Cu, 0x106420u, 0xFFFFFFu, 0xFFFFFFu, 0xFF0000u,
    0xFF00FFu, 0xFFC800u, 0x5AC8FFu, 0x3896AAu, 0x6ED2E6u,
    0x031020u, 0x69737Du, 0xAAAAAAu, 0x3C4650u,
};

bool s_visibility[] = {true, true, true, true, true, true,
                      true, true, true, true, true, true};

bool checkboxChecked(const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  return strcmp(value, "on") == 0 || strcmp(value, "T") == 0 ||
         strcmp(value, "t") == 0 || strcmp(value, "F") == 0 ||
         strcmp(value, "f") == 0;
}

void copyCleanText(const char* value, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (value == nullptr) {
    return;
  }

  size_t written = 0;
  bool previous_space = true;
  for (size_t i = 0; value[i] != '\0' && written + 1 < out_len; ++i) {
    const unsigned char ch = static_cast<unsigned char>(value[i]);
    if (std::isspace(ch)) {
      if (!previous_space) {
        out[written++] = ' ';
        previous_space = true;
      }
      continue;
    }
    if (ch >= 32 && ch <= 126) {
      out[written++] = static_cast<char>(ch);
      previous_space = false;
    }
  }
  while (written > 0 && out[written - 1] == ' ') {
    --written;
  }
  out[written] = '\0';
}

int clampTextScalePercent(int value) {
  if (value < kTextScaleMinPercent) {
    return kTextScaleMinPercent;
  }
  if (value > kTextScaleMaxPercent) {
    return kTextScaleMaxPercent;
  }
  return value;
}

bool parseTextScalePercent(const char* value, int* result) {
  if (value == nullptr || value[0] == '\0' || result == nullptr) {
    return false;
  }

  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value) {
    return false;
  }
  while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (*end != '\0') {
    return false;
  }

  *result = clampTextScalePercent(static_cast<int>(parsed));
  return true;
}

bool parseTimeMinute(const char* value, uint16_t* result) {
  if (value == nullptr || result == nullptr || std::strlen(value) != 5 ||
      value[2] != ':') {
    return false;
  }
  if (!std::isdigit(static_cast<unsigned char>(value[0])) ||
      !std::isdigit(static_cast<unsigned char>(value[1])) ||
      !std::isdigit(static_cast<unsigned char>(value[3])) ||
      !std::isdigit(static_cast<unsigned char>(value[4]))) {
    return false;
  }
  const int hour = (value[0] - '0') * 10 + value[1] - '0';
  const int minute = (value[3] - '0') * 10 + value[4] - '0';
  if (hour > 23 || minute > 59) {
    return false;
  }
  *result = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

bool parseColor(const char* value, uint32_t* result) {
  if (value == nullptr || result == nullptr || value[0] != '#' ||
      std::strlen(value) != 7) {
    return false;
  }

  uint32_t parsed = 0;
  for (size_t i = 1; i < 7; ++i) {
    const char ch = value[i];
    uint8_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<uint8_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<uint8_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<uint8_t>(ch - 'A' + 10);
    } else {
      return false;
    }
    parsed = (parsed << 4) | digit;
  }
  *result = parsed;
  return true;
}

void loadDefaults() {
  copyCleanText(config::kDefaultOtaPassword, s_ota_password,
                sizeof(s_ota_password));
  s_footer_enabled = false;
  s_weather_enabled = false;
  s_temperature_fahrenheit = false;
  s_altitude_metres = false;
  s_use_24_hour_clock = true;
  s_text_scale_percent = kTextScaleDefaultPercent;
  s_night_enabled = false;
  s_use_iata = false;
  s_prefer_route = true;
  s_night_start = 22 * 60;
  s_night_end = 7 * 60;
  std::memcpy(s_colors, kDefaultColors, sizeof(s_colors));
  for (bool& value : s_visibility) {
    value = true;
  }
}

void persist() {
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kKeyFooter, s_footer_enabled);
  prefs.putBool(kKeyWeather, s_weather_enabled);
  prefs.putBool(kKeyFahrenheit, s_temperature_fahrenheit);
  prefs.putBool(kKeyAltitudeMetres, s_altitude_metres);
  prefs.putBool(kKeyClock24, s_use_24_hour_clock);
  prefs.putInt(kKeyTextScale, s_text_scale_percent);
  prefs.putBool(kKeyNightEnabled, s_night_enabled);
  prefs.putUShort(kKeyNightStart, s_night_start);
  prefs.putUShort(kKeyNightEnd, s_night_end);
  prefs.putBool(kKeyUseIata, s_use_iata);
  prefs.putBool(kKeyPreferRoute, s_prefer_route);
  prefs.putString(kKeyOtaPassword, s_ota_password);
  prefs.putULong(kKeyColorBackground, s_colors[0]);
  prefs.putULong(kKeyColorGrid, s_colors[1]);
  prefs.putULong(kKeyColorLabel, s_colors[2]);
  prefs.putULong(kKeyColorCenter, s_colors[3]);
  prefs.putULong(kKeyColorAircraft, s_colors[4]);
  prefs.putULong(kKeyColorTrack, s_colors[5]);
  prefs.putULong(kKeyColorTagType, s_colors[6]);
  prefs.putULong(kKeyColorTagAltitude, s_colors[7]);
  prefs.putULong(kKeyColorRunway, s_colors[8]);
  prefs.putULong(kKeyColorRunwayLabel, s_colors[9]);
  prefs.putULong(kKeyColorFooter, s_colors[10]);
  prefs.putULong(kKeyColorRoad, s_colors[11]);
  prefs.putULong(kKeyColorCity, s_colors[12]);
  prefs.putULong(kKeyColorRoadPrimary, s_colors[13]);
  prefs.putBool(kKeyShowGrid, s_visibility[0]);
  prefs.putBool(kKeyShowCenter, s_visibility[1]);
  prefs.putBool(kKeyShowLabel, s_visibility[2]);
  prefs.putBool(kKeyShowAircraft, s_visibility[3]);
  prefs.putBool(kKeyShowTrack, s_visibility[4]);
  prefs.putBool(kKeyShowTagType, s_visibility[5]);
  prefs.putBool(kKeyShowTagAltitude, s_visibility[6]);
  prefs.putBool(kKeyShowRunway, s_visibility[7]);
  prefs.putBool(kKeyShowRunwayLabel, s_visibility[8]);
  prefs.putBool(kKeyShowRoad, s_visibility[9]);
  prefs.putBool(kKeyShowCity, s_visibility[10]);
  prefs.putBool(kKeyShowRoadPrimary, s_visibility[11]);
  prefs.end();
}

}  // namespace

void init() {
  loadDefaults();

  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, true)) {
    return;
  }

  s_footer_enabled = prefs.getBool(kKeyFooter, false);
  s_weather_enabled = prefs.getBool(kKeyWeather, false);
  s_temperature_fahrenheit = prefs.getBool(kKeyFahrenheit, false);
  s_altitude_metres = prefs.getBool(kKeyAltitudeMetres, false);
  s_use_24_hour_clock = prefs.getBool(kKeyClock24, true);
  s_text_scale_percent = clampTextScalePercent(
      prefs.getInt(kKeyTextScale, kTextScaleDefaultPercent));
  s_night_enabled = prefs.getBool(kKeyNightEnabled, false);
  s_night_start = prefs.getUShort(kKeyNightStart, 22 * 60);
  s_night_end = prefs.getUShort(kKeyNightEnd, 7 * 60);
  s_use_iata = prefs.getBool(kKeyUseIata, false);
  s_prefer_route = prefs.getBool(kKeyPreferRoute, true);
  if (s_night_start >= 24 * 60) s_night_start = 22 * 60;
  if (s_night_end >= 24 * 60) s_night_end = 7 * 60;
  const char* color_keys[] = {
      kKeyColorBackground, kKeyColorGrid, kKeyColorLabel, kKeyColorCenter,
      kKeyColorAircraft, kKeyColorTrack, kKeyColorTagType,
      kKeyColorTagAltitude, kKeyColorRunway, kKeyColorRunwayLabel,
      kKeyColorFooter, kKeyColorRoad, kKeyColorCity, kKeyColorRoadPrimary};
  for (size_t i = 0; i < sizeof(s_colors) / sizeof(s_colors[0]); ++i) {
    s_colors[i] = prefs.getULong(color_keys[i], kDefaultColors[i]);
  }
  const char* visibility_keys[] = {
      kKeyShowGrid, kKeyShowCenter, kKeyShowLabel, kKeyShowAircraft,
      kKeyShowTrack, kKeyShowTagType, kKeyShowTagAltitude, kKeyShowRunway,
      kKeyShowRunwayLabel, kKeyShowRoad, kKeyShowCity, kKeyShowRoadPrimary};
  for (size_t i = 0; i < sizeof(s_visibility) / sizeof(s_visibility[0]); ++i) {
    s_visibility[i] = prefs.getBool(visibility_keys[i], true);
  }

  String value = prefs.getString(kKeyOtaPassword, config::kDefaultOtaPassword);
  copyCleanText(value.c_str(), s_ota_password, sizeof(s_ota_password));
  if (s_ota_password[0] == '\0') {
    copyCleanText(config::kDefaultOtaPassword, s_ota_password,
                  sizeof(s_ota_password));
  }
  prefs.end();
}

bool footerEnabled() { return s_footer_enabled; }

bool weatherEnabled() { return s_weather_enabled; }

bool temperatureFahrenheit() { return s_temperature_fahrenheit; }

bool altitudeMetres() { return s_altitude_metres; }

bool use24HourClock() { return s_use_24_hour_clock; }

int textScalePercent() { return s_text_scale_percent; }

bool nightModeEnabled() { return s_night_enabled; }

uint16_t nightStartMinute() { return s_night_start; }

uint16_t nightEndMinute() { return s_night_end; }

bool nightModeActive(int local_minute) {
  if (!s_night_enabled || local_minute < 0 || local_minute >= 24 * 60) {
    return false;
  }
  if (s_night_start == s_night_end) {
    return true;
  }
  if (s_night_start < s_night_end) {
    return local_minute >= s_night_start && local_minute < s_night_end;
  }
  return local_minute >= s_night_start || local_minute < s_night_end;
}

bool useIataCodes() { return s_use_iata; }
bool preferRoute() { return s_prefer_route; }

const char* otaPassword() { return s_ota_password; }

uint32_t color(ColorId id) {
  const size_t index = static_cast<size_t>(id);
  if (index >= sizeof(s_colors) / sizeof(s_colors[0])) {
    return 0;
  }
  return s_colors[index];
}

bool visible(VisibilityId id) {
  const size_t index = static_cast<size_t>(id);
  if (index >= sizeof(s_visibility) / sizeof(s_visibility[0])) {
    return false;
  }
  return s_visibility[index];
}

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
                    const char* prefer_route_checkbox) {
  s_footer_enabled = checkboxChecked(footer_checkbox);
  s_weather_enabled = checkboxChecked(weather_checkbox);
  s_temperature_fahrenheit = checkboxChecked(fahrenheit_checkbox);
  s_altitude_metres = checkboxChecked(altitude_metres_checkbox);
  s_use_24_hour_clock = checkboxChecked(clock24_checkbox);
  int text_scale_percent = s_text_scale_percent;
  if (parseTextScalePercent(text_scale_percent_value, &text_scale_percent)) {
    s_text_scale_percent = text_scale_percent;
  }
  s_night_enabled = checkboxChecked(night_enabled_checkbox);
  s_use_iata = checkboxChecked(use_iata_checkbox);
  s_prefer_route = checkboxChecked(prefer_route_checkbox);
  uint16_t parsed_time = 0;
  if (parseTimeMinute(night_start_value, &parsed_time)) {
    s_night_start = parsed_time;
  }
  if (parseTimeMinute(night_end_value, &parsed_time)) {
    s_night_end = parsed_time;
  }

  char password[kOtaPasswordMaxLen + 1] = {};
  copyCleanText(ota_password_value, password, sizeof(password));
  if (password[0] != '\0') {
    strncpy(s_ota_password, password, sizeof(s_ota_password) - 1);
    s_ota_password[sizeof(s_ota_password) - 1] = '\0';
  }

  persist();
  Serial.printf("Display footer: %s, weather: %s, text: %d%%\n",
                s_footer_enabled ? "on" : "off",
                s_weather_enabled ? "on" : "off", s_text_scale_percent);
}

void saveColorsFromPortal(const char* background, const char* grid,
                          const char* label, const char* center,
                          const char* aircraft, const char* track_vector,
                          const char* tag_type, const char* tag_altitude,
                          const char* runway, const char* runway_label,
                          const char* footer_background, const char* road,
                          const char* city, const char* road_primary) {
  const char* values[] = {background, grid, label, center, aircraft,
                          track_vector, tag_type, tag_altitude, runway,
                          runway_label, footer_background, road, city,
                          road_primary};
  for (size_t i = 0; i < sizeof(s_colors) / sizeof(s_colors[0]); ++i) {
    uint32_t parsed = 0;
    if (parseColor(values[i], &parsed)) {
      s_colors[i] = parsed;
    }
  }
  persist();
}

void saveVisibilityFromPortal(const char* grid, const char* center,
                              const char* label, const char* aircraft,
                              const char* track_vector, const char* tag_type,
                              const char* tag_altitude, const char* runway,
                              const char* runway_label, const char* road,
                              const char* city, const char* road_primary) {
  const char* values[] = {grid, center, label, aircraft, track_vector,
                          tag_type, tag_altitude, runway, runway_label, road,
                          city, road_primary};
  for (size_t i = 0; i < sizeof(s_visibility) / sizeof(s_visibility[0]); ++i) {
    s_visibility[i] = checkboxChecked(values[i]);
  }
  persist();
}

void clear() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  loadDefaults();
}

}  // namespace services::settings
