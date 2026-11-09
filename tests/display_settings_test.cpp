#include <Preferences.h>
#include "services/display_settings.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace settings = services::settings;

// Current function: saveFromPortal(). Real module linked; only NVS/Serial and
// the test-only default secret are faked. Reload through init() observes stored
// results. This is supplemental host evidence, not target-NVS acceptance.
static void saveRoutePreference(const char* checked) {
  settings::saveFromPortal("T", "T", "T", "T", "", "105", "", "T",
                            "23:00", "06:00", "T", checked);
}

static void assertOtherSettingsPreserved() {
  assert(settings::footerEnabled() && settings::weatherEnabled());
  assert(settings::temperatureFahrenheit() && settings::altitudeMetres());
  assert(!settings::use24HourClock() && settings::textScalePercent() == 105);
  assert(settings::nightModeEnabled() && settings::nightStartMinute() == 23 * 60);
  assert(settings::nightEndMinute() == 6 * 60 && settings::useIataCodes());
  assert(std::strcmp(settings::otaPassword(), "host-test-existing") == 0);
  assert(settings::color(settings::ColorId::kGrid) == 0x123456);
  assert(!settings::visible(settings::VisibilityId::kTagType));
}

int main() {
  Preferences::numbers.clear(); Preferences::strings.clear(); Preferences::available = true;
  Preferences::numbers = {{"display/footer", 1}, {"display/weather", 1},
      {"display/tempF", 1}, {"display/altM", 1}, {"display/time24", 0},
      {"display/fontPct", 105}, {"display/nightOn", 1}, {"display/nightStart", 1380},
      {"display/nightEnd", 360}, {"display/useIata", 1}, {"display/colGrid", 0x123456},
      {"display/showTyp", 0}};
  Preferences::strings["display/otaPass"] = "host-test-existing";
  const auto previous = Preferences::numbers;
  settings::init();
  assert(settings::preferRoute()); // older NVS without the new key keeps route preference
  assert(Preferences::numbers == previous); // reads/migration must not persist anything
  assertOtherSettingsPreserved();

  saveRoutePreference(""); // an unchecked checkbox is absent/empty in a valid form
  settings::init();
  assert(!settings::preferRoute());
  assert(Preferences::numbers.at("display/showRoute") == 0);
  assertOtherSettingsPreserved();

  saveRoutePreference("T");
  settings::init();
  assert(settings::preferRoute());
  assert(Preferences::numbers.at("display/showRoute") == 1);
  assertOtherSettingsPreserved();
  std::puts("PASS: real settings save/load with fake NVS, route on/off persistence,");
  std::puts("      older-NVS default without writes, blank-password retention and unrelated settings");
}
