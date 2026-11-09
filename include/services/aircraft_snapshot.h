#pragma once

#include <cmath>
#include <cstdio>

#include "services/adsb_client.h"

namespace services::adsb {

inline double snapshotDistanceKm(double lat, double lon, double center_lat, double center_lon) {
  constexpr double radians = 0.017453292519943295;
  const double dlat = (lat - center_lat) * radians;
  const double dlon = (lon - center_lon) * radians;
  const double a = std::sin(dlat * .5) * std::sin(dlat * .5) +
      std::cos(lat * radians) * std::cos(center_lat * radians) *
      std::sin(dlon * .5) * std::sin(dlon * .5);
  return 12742.0 * std::asin(std::sqrt(std::fmax(0.0, std::fmin(1.0, a))));
}

template <typename Writer>
void snapshotNumber(Writer& out, double value, int decimals = 1) {
  char text[48];
  const int length = std::snprintf(text, sizeof(text), "%.*f", decimals, value);
  out.append(std::isfinite(value) && length > 0 &&
      static_cast<size_t>(length) < sizeof(text) ? text : "null");
}

template <typename Writer>
void snapshotUnsigned(Writer& out, uint32_t value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%lu", static_cast<unsigned long>(value));
  out.append(text);
}

// Read-only, bounded streaming view of the accepted feed list. The caller runs
// on the same application task as fetchUpdate(), so the list stays coherent
// while yielding to the network stack. No copy, JSON tree or API call is needed.
template <typename Writer>
void writeAircraftSnapshot(Writer& out, const Aircraft* planes, size_t count,
    uint32_t revision, uint32_t received_ms, uint32_t now_ms,
    double center_lat, double center_lon, bool miles) {
  const bool has_sample = revision != 0;
  const uint32_t elapsed_ms = now_ms - received_ms;
  out.append("{\"has_sample\":");
  out.append(has_sample ? "true" : "false");
  out.append(",\"revision\":");
  snapshotUnsigned(out, revision);
  out.append(",\"sample_age_ms\":");
  if (has_sample) snapshotUnsigned(out, elapsed_ms);
  else out.append("null");
  out.append(",\"distance_unit\":");
  out.jsonString(miles ? "mi" : "km");
  out.append(",\"aircraft\":[");
  if (count > kMaxAircraft) count = kMaxAircraft;
  for (size_t i = 0; has_sample && planes != nullptr && i < count; ++i) {
    const Aircraft& plane = planes[i];
    if (i != 0) out.append(",");
    out.append("{\"hex\":"); out.jsonString(plane.hex);
    out.append(",\"callsign\":"); out.jsonString(plane.callsign);
    out.append(",\"route\":"); out.jsonString(plane.route);
    out.append(",\"type\":"); out.jsonString(plane.type);
    out.append(",\"altitude\":"); out.jsonString(plane.alt);
    out.append(",\"speed_knots\":");
    snapshotNumber(out, plane.gs_knots >= 0 ? plane.gs_knots : NAN);
    out.append(",\"track_deg\":");
    snapshotNumber(out, plane.track_deg >= 0 && plane.track_deg < 360 ? plane.track_deg : NAN);
    out.append(",\"distance\":");
    const bool valid_position = std::isfinite(plane.lat) && std::isfinite(plane.lon) &&
        plane.lat >= -90 && plane.lat <= 90 && plane.lon >= -180 && plane.lon <= 180;
    snapshotNumber(out, valid_position ?
        snapshotDistanceKm(plane.lat, plane.lon, center_lat, center_lon) /
            (miles ? 1.609344 : 1.0) : NAN);
    out.append(",\"position_age_s\":");
    snapshotNumber(out, plane.position_age_s >= 0 ?
        plane.position_age_s + static_cast<double>(elapsed_ms) * .001 : NAN);
    out.append("}");
  }
  out.append("]}");
}

}  // namespace services::adsb
