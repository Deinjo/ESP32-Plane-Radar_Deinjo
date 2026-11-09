#pragma once

#include <cstddef>
#include <cstdint>

namespace services::adsb {

struct Aircraft {
  float lat;
  float lon;
  float nose_deg;
  float track_deg;
  float gs_knots;
  float position_age_s;
  bool motion_valid;
  char hex[7];
  char callsign[9];
  /** IATA/ICAO origin-destination pair, for example "BOS-IND". */
  char route[10];
  /** Compact detailed model, for example "B737-800". */
  char type[18];
  char alt[12];
};

constexpr size_t kMaxAircraft = 64;

size_t aircraftCount();
const Aircraft* aircraftList();
/** Incremented only when a successful position response replaces the list. */
uint32_t aircraftRevision();
/** millis() when the current position list was accepted, not last rendered. */
uint32_t aircraftReceivedMs();

/** Hook invoked during long HTTP I/O (e.g. wifiLoop). Optional. */
using PollFn = void (*)();
void setPollFn(PollFn fn);

/** Fetch aircraft within fetch_radius_km of center_lat/lon from adsb.fi. */
bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km);

/**
 * Look up one uncached aircraft through ADSBDB. Results are rate-limited and
 * cached. Releases position TLS before admission; at most one HTTP request per
 * 30-second slot, including deferred callsign fallback. Stale/failing position
 * data takes priority. Returns true when a visible route or type changed.
 */
bool enrichOnePending();

}  // namespace services::adsb
