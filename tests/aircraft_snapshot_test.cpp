#include "services/aircraft_snapshot.h"
#include "services/portal_html.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

using namespace services::adsb;

int main() {
  std::string text;
  auto sink = [&](const char* bytes, size_t size) {
    assert(size <= 512);
    text.append(bytes, size);
    return true;
  };
  services::portal::HtmlWriter<decltype(sink)> out(sink);
  writeAircraftSnapshot(out, nullptr, 0, 0, 0, 100, 0, 0, false);
  assert(out.flush());
  assert(text == "{\"has_sample\":false,\"revision\":0,\"sample_age_ms\":null,\"distance_unit\":\"km\",\"aircraft\":[]}");
  text.clear();
  Aircraft plane{};
  std::strcpy(plane.hex, "abc123");
  std::strcpy(plane.callsign, "TEST123");
  std::strcpy(plane.route, "A\"B\\C");
  std::strcpy(plane.type, "A320");
  std::strcpy(plane.alt, "1200 m");
  plane.gs_knots = 123.4f; plane.track_deg = 90; plane.position_age_s = 2;
  plane.lon = .01f;
  writeAircraftSnapshot(out, &plane, 1, 1, UINT32_MAX - 499, 500, 0, 0, false);
  assert(out.flush());
  assert(text.find("\"sample_age_ms\":1000") != std::string::npos);
  assert(text.find("\"route\":\"A\\\"B\\\\C\"") != std::string::npos);
  assert(text.find("\"position_age_s\":3.0") != std::string::npos);
  assert(text.find("\"distance\":1.1") != std::string::npos);
  assert(text.find("\"speed_knots\":123.4") != std::string::npos);
  assert(std::fabs(snapshotDistanceKm(0, 179.99, 0, -179.99) - 2.2239) < .01);

  text.clear();
  plane.lat = NAN; plane.gs_knots = INFINITY; plane.track_deg = -1;
  plane.position_age_s = -1;
  writeAircraftSnapshot(out, &plane, 1, 2, 100, 200, 0, 0, true);
  assert(out.flush());
  assert(text.find("\"distance_unit\":\"mi\"") != std::string::npos);
  for (const char* field : {"distance", "speed_knots", "track_deg", "position_age_s"})
    assert(text.find(std::string("\"") + field + "\":null") != std::string::npos);

  text.clear();
  Aircraft planes[kMaxAircraft + 1]{};
  for (auto& item : planes) {
    std::strcpy(item.hex, "abc123");
    std::strcpy(item.callsign, "TEST1234");
    std::strcpy(item.route, "AAA-BBB");
    std::strcpy(item.type, "A321-200");
    std::strcpy(item.alt, "12345 ft");
  }
  writeAircraftSnapshot(out, planes, kMaxAircraft + 1, 3, 100, 100, 0, 0, false);
  assert(out.flush());
  size_t count = 0, position = 0;
  while ((position = text.find("\"hex\":", position)) != std::string::npos) { ++count; ++position; }
  assert(count == kMaxAircraft);
  assert(text.size() > 9000 && text.substr(text.size() - 2) == "]}");
  std::puts("PASS: streamed snapshots, 64-row limit, JSON escaping, empty feed, units,");
  std::puts("      invalid/missing numbers, distance/dateline, source age and timer rollover");
}
