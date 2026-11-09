#include "services/adsb_client.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <ArduinoJson.h>

#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "services/display_settings.h"
#include "services/flight_data_json.h"
#include "services/reusable_http_session.h"
#include "services/tls_slot.h"
#include "services/tls_record_pool.h"

namespace services::adsb {

namespace {

constexpr char kApiBase[] = "https://opendata.adsb.fi/api/v3/lat/";
constexpr float kKmPerNm = 1.852f;
constexpr float kMetresPerFoot = 0.3048f;
constexpr int kConnectAttemptMs = 3000;
constexpr unsigned long kAdsbRequestTimeoutMs = 5000;
constexpr unsigned long kAdsbHandshakeTimeoutSec = 5;
constexpr unsigned long kAdsbFailureBackoffMs = 30000;
constexpr unsigned long kAdsbMaxFailureBackoffMs = 60000;
constexpr size_t kEnrichmentCacheSize = 48;

Aircraft s_aircraft[kMaxAircraft];
size_t s_aircraft_count = 0;
uint32_t s_aircraft_revision = 0;
uint32_t s_aircraft_received_ms = 0;
PollFn s_poll_fn = nullptr;
unsigned long s_last_enrichment_lookup_ms = 0;
unsigned long s_last_enrichment_failure_ms = 0;
unsigned long s_last_adsb_failure_ms = 0;
uint8_t s_adsb_failure_count = 0;
bool s_enrichment_paused = false;
double s_last_center_lat = 0.0;
double s_last_center_lon = 0.0;
services::net::ReusableHttpSession<WiFiClientSecure, HTTPClient> s_position_session;
bool enrichmentSlotAllowed();

class ChunkedTransferStream final : public Stream {
 public:
  explicit ChunkedTransferStream(WiFiClient& client) : client_(client) {}

  int available() override {
    if (!advanceChunk()) {
      return 0;
    }
    const int available_bytes = client_.available();
    if (available_bytes <= 0) {
      return 0;
    }
    return static_cast<size_t>(available_bytes) < chunk_remaining_
               ? available_bytes
               : static_cast<int>(chunk_remaining_);
  }

  int read() override {
    if (!advanceChunk()) {
      return -1;
    }
    const int value = client_.read();
    if (value >= 0) {
      --chunk_remaining_;
      if (chunk_remaining_ == 0U) {
        needs_chunk_terminator_ = true;
      }
    }
    return value;
  }

  int peek() override {
    return advanceChunk() ? client_.peek() : -1;
  }

  void flush() override {}
  size_t write(uint8_t) override { return 0U; }
  size_t write(const uint8_t*, size_t) override { return 0U; }

  void drain() {
    while (!done_ && !failed_) {
      if (!advanceChunk()) {
        break;
      }
      while (chunk_remaining_ > 0U) {
        if (read() < 0) {
          failed_ = true;
          break;
        }
      }
    }
  }

 private:
  bool readLine(char* line, size_t line_size, size_t* length) {
    if (line == nullptr || line_size < 2U || length == nullptr) {
      return false;
    }
    size_t written = 0U;
    while (true) {
      const int value = client_.read();
      if (value < 0) {
        return false;
      }
      if (value == '\n') {
        break;
      }
      if (written + 1U >= line_size) {
        return false;
      }
      line[written] = static_cast<char>(value);
      ++written;
    }
    if (written > 0U && line[written - 1U] == '\r') {
      --written;
    }
    line[written] = '\0';
    *length = written;
    return true;
  }

  bool advanceChunk() {
    if (done_ || failed_) {
      return false;
    }
    if (chunk_remaining_ > 0U) {
      return true;
    }

    if (needs_chunk_terminator_) {
      const int carriage_return = client_.read();
      const int line_feed = client_.read();
      if (carriage_return != '\r' || line_feed != '\n') {
        failed_ = true;
        return false;
      }
      needs_chunk_terminator_ = false;
    }

    char line[64] = {};
    size_t line_length = 0U;
    if (!readLine(line, sizeof(line), &line_length)) {
      failed_ = true;
      return false;
    }
    char* extension = std::strchr(line, ';');
    if (extension != nullptr) {
      *extension = '\0';
    }
    char* end = nullptr;
    const unsigned long chunk_size = std::strtoul(line, &end, 16);
    if (end == line || *end != '\0' || chunk_size > 65536UL) {
      failed_ = true;
      return false;
    }

    if (chunk_size == 0UL) {
      for (size_t trailer_count = 0U; trailer_count < 16U; ++trailer_count) {
        if (!readLine(line, sizeof(line), &line_length)) {
          failed_ = true;
          return false;
        }
        if (line_length == 0U) {
          done_ = true;
          return false;
        }
      }
      failed_ = true;
      return false;
    }

    chunk_remaining_ = static_cast<size_t>(chunk_size);
    return true;
  }

  WiFiClient& client_;
  size_t chunk_remaining_ = 0U;
  bool needs_chunk_terminator_ = false;
  bool done_ = false;
  bool failed_ = false;
};

struct EnrichmentCacheEntry {
  char hex[7] = {};
  char callsign[9] = {};
  char route[10] = {};
  char type[18] = {};
  unsigned long refreshed_ms = 0;
  bool route_pending = false;
};

EnrichmentCacheEntry s_enrichment_cache[kEnrichmentCacheSize];

void pollNetwork() {
  if (s_poll_fn != nullptr) {
    s_poll_fn();
  }
}

int performGet(HTTPClient& http) {
  http.setConnectTimeout(kConnectAttemptMs);
  const int code = http.GET();
  if (code == HTTPC_ERROR_CONNECTION_REFUSED ||
      code == HTTPC_ERROR_NOT_CONNECTED) {
    // A failed TLS allocation is not recoverable by immediately reconnecting.
    // Repeated GET() calls only amplify heap fragmentation on the ESP32-C3.
    pollNetwork();
  }
  return code;
}

void printRequestDiagnostics(const char* phase, unsigned long started_ms) {
  Serial.printf("adsb: %s t=%lums heap=%u min_heap=%u\n", phase,
                millis() - started_ms, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()));
}

void printNetworkDiagnostics() {
  Serial.printf("adsb: wifi status=%d local=%s gateway=%s dns=%s\n",
                static_cast<int>(WiFi.status()), WiFi.localIP().toString().c_str(),
                WiFi.gatewayIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str());
}

size_t largestFreeBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

size_t largestInternalFreeBlock() {
  return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

bool pauseEnrichmentAfterTlsAllocationFailure(WiFiClientSecure& client,
                                               const char* service) {
  char error[96] = {};
  if (client.lastError(error, sizeof(error)) != MBEDTLS_ERR_SSL_ALLOC_FAILED) {
    return false;
  }

  s_enrichment_paused = true;
  Serial.printf("flight data: paused after %s TLS allocation failure: %s "
                "heap=%u largest_internal=%u\n",
                service, error, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(largestInternalFreeBlock()));
  return true;
}

unsigned long adsbFailureBackoff() {
  if (s_adsb_failure_count <= 1) {
    return kAdsbFailureBackoffMs;
  }
  const uint8_t shift = s_adsb_failure_count > 4 ? 3 : s_adsb_failure_count - 1;
  unsigned long backoff = kAdsbFailureBackoffMs << shift;
  return backoff > kAdsbMaxFailureBackoffMs ? kAdsbMaxFailureBackoffMs : backoff;
}

float kmToNauticalMiles(float km) { return km / kKmPerNm; }

bool readJsonFloat(const JsonObject& obj, const char* key, float* out) {
  if (obj[key].is<float>() || obj[key].is<double>() || obj[key].is<int>()) {
    *out = obj[key].as<float>();
    return true;
  }
  return false;
}

float pickNoseHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickTrackHeading(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "track", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "true_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "mag_heading", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "dir", &v)) {
    return v;
  }
  return 0.0f;
}

float pickGroundSpeed(const JsonObject& plane) {
  float v = 0.0f;
  if (readJsonFloat(plane, "gs", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "tas", &v)) {
    return v;
  }
  if (readJsonFloat(plane, "ias", &v)) {
    return v;
  }
  return 0.0f;
}

bool isOnGround(const JsonObject& plane) {
  if (!plane["alt_baro"].is<const char*>()) {
    return false;
  }
  return strcmp(plane["alt_baro"].as<const char*>(), "ground") == 0;
}

void copyJsonStringTrimmed(const JsonObject& obj, const char* key, char* out,
                           size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (!obj[key].is<const char*>()) {
    return;
  }
  const char* s = obj[key].as<const char*>();
  size_t n = strnlen(s, out_len - 1);
  while (n > 0 && s[n - 1] == ' ') {
    --n;
  }
  memcpy(out, s, n);
  out[n] = '\0';
}

void formatAltitudeTag(const JsonObject& plane, char* out, size_t out_len) {
  out[0] = '\0';
  if (out_len == 0) {
    return;
  }

  if (plane["alt_baro"].is<const char*>()) {
    const char* s = plane["alt_baro"].as<const char*>();
    if (strcmp(s, "ground") == 0) {
      strncpy(out, "GND", out_len - 1);
      out[out_len - 1] = '\0';
      return;
    }
  }

  float alt = 0.0f;
  
  if (readJsonFloat(plane, "alt_baro", &alt) ||
      readJsonFloat(plane, "alt_geom", &alt)) {
    if (services::settings::altitudeMetres()) {
      const int metres = static_cast<int>(lroundf(alt * kMetresPerFoot));
      snprintf(out, out_len, "%d m", metres);
    } else {
      snprintf(out, out_len, "%d ft", static_cast<int>(lroundf(alt)));
    }
  }
}

void fillTagFields(Aircraft* ac, const JsonObject& plane) {
  copyJsonStringTrimmed(plane, "hex", ac->hex, sizeof(ac->hex));
  copyJsonStringTrimmed(plane, "flight", ac->callsign, sizeof(ac->callsign));
  if (ac->callsign[0] == '\0') {
    copyJsonStringTrimmed(plane, "hex", ac->callsign, sizeof(ac->callsign));
  }

  ac->route[0] = '\0';
  copyJsonStringTrimmed(plane, "t", ac->type, sizeof(ac->type));
  formatAltitudeTag(plane, ac->alt, sizeof(ac->alt));
}

bool sameKey(const EnrichmentCacheEntry& entry, const Aircraft& plane) {
  return entry.refreshed_ms != 0 && strcmp(entry.hex, plane.hex) == 0 &&
         strcmp(entry.callsign, plane.callsign) == 0;
}

bool cacheEntryFresh(const EnrichmentCacheEntry& entry,
                     unsigned long now) {
  if (entry.refreshed_ms == 0) {
    return false;
  }
  // Finding an aircraft type does not turn a missing route into a 6 h hit.
  const unsigned long ttl = entry.route[0] != '\0' ? config::kFlightCacheSuccessMs
                                                : config::kFlightCacheMissMs;
  return now - entry.refreshed_ms < ttl;
}

EnrichmentCacheEntry* findCacheEntry(const Aircraft& plane,
                                     unsigned long now) {
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) && cacheEntryFresh(entry, now)) {
      return &entry;
    }
  }
  return nullptr;
}

bool copyIfDifferent(char* destination, size_t destination_len,
                     const char* source) {
  if (source == nullptr || source[0] == '\0' ||
      strcmp(destination, source) == 0) {
    return false;
  }
  strncpy(destination, source, destination_len - 1);
  destination[destination_len - 1] = '\0';
  return true;
}

bool applyCacheEntry(Aircraft* plane, const EnrichmentCacheEntry& entry) {
  bool changed = false;
  changed |= copyIfDifferent(plane->route, sizeof(plane->route), entry.route);
  changed |= copyIfDifferent(plane->type, sizeof(plane->type), entry.type);
  return changed;
}

EnrichmentCacheEntry* cacheSlotFor(const Aircraft& plane,
                                   unsigned long now) {
  EnrichmentCacheEntry* oldest = &s_enrichment_cache[0];
  unsigned long oldest_age = 0;
  for (auto& entry : s_enrichment_cache) {
    if (sameKey(entry, plane) || entry.refreshed_ms == 0) {
      return &entry;
    }
    const unsigned long age = now - entry.refreshed_ms;
    if (age >= oldest_age) {
      oldest = &entry;
      oldest_age = age;
    }
  }
  return oldest;
}

void copySafeIdentifier(const char* source, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  size_t written = 0;
  if (source != nullptr) {
    for (size_t i = 0; source[i] != '\0' && written + 1 < out_len; ++i) {
      const unsigned char ch = static_cast<unsigned char>(source[i]);
      if (std::isalnum(ch)) {
        out[written++] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
    }
  }
  out[written] = '\0';
}

bool startsWith(const char* value, const char* prefix) {
  return value != nullptr && prefix != nullptr &&
         strncmp(value, prefix, strlen(prefix)) == 0;
}

void compactAircraftType(const char* detailed, const char* icao, char* out,
                         size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';

  const char* value = detailed;
  char prefixed[40] = {};
  if (startsWith(value, "Boeing ")) {
    snprintf(prefixed, sizeof(prefixed), "B%s", value + 7);
    value = prefixed;
  } else if (startsWith(value, "Airbus ")) {
    value += 7;
  } else if (startsWith(value, "Bombardier ")) {
    value += 11;
  } else if (startsWith(value, "Embraer ")) {
    value += 8;
  } else if (startsWith(value, "De Havilland Canada ")) {
    value += 20;
  }

  if (value == nullptr || value[0] == '\0') {
    value = icao;
  }
  if (value == nullptr) {
    return;
  }

  size_t written = 0;
  bool previous_space = false;
  for (size_t i = 0; value[i] != '\0' && written + 1 < out_len; ++i) {
    const char ch = value[i];
    if (ch == ' ') {
      if (!previous_space) {
        out[written++] = ' ';
        previous_space = true;
      }
      continue;
    }
    out[written++] = ch;
    previous_space = false;
  }
  while (written > 0 && out[written - 1] == ' ') {
    --written;
  }
  out[written] = '\0';
}

void parseDetailedType(const JsonObject& response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject aircraft = response["aircraft"].as<JsonObject>();
  if (aircraft.isNull()) {
    return;
  }
  compactAircraftType(aircraft["type"] | nullptr,
                      aircraft["icao_type"] | nullptr, out, out_len);
}

String enrichmentUrl(const Aircraft& plane) {
  char hex[sizeof(plane.hex)] = {};
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.hex, hex, sizeof(hex));
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));

  String url = config::kFlightDataApiBase;
  if (hex[0] != '\0') {
    url += "aircraft/";
    url += hex;
    if (callsign[0] != '\0' && strcmp(hex, callsign) != 0) {
      url += "?callsign=";
      url += callsign;
    }
  } else {
    url += "callsign/";
    url += callsign;
  }
  return url;
}

String callsignRouteUrl(const Aircraft& plane) {
  char callsign[sizeof(plane.callsign)] = {};
  copySafeIdentifier(plane.callsign, callsign, sizeof(callsign));
  if (callsign[0] == '\0') {
    return {};
  }

  String url = config::kFlightDataApiBase;
  url += "callsign/";
  url += callsign;
  return url;
}

bool fetchFlightDataJson(const String& url, const char* callsign,
                         JsonDocument* doc, bool* not_found) {
  *not_found = false;
  const unsigned long started_ms = millis();
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(3);

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("flight data: http.begin failed");
    return false;
  }
  // Prefer a Content-Length response so truncated HTTPS bodies can be
  // detected before they reach ArduinoJson.
  http.useHTTP10(true);
  http.setTimeout(config::kFlightLookupTimeoutMs);
  const int code = performGet(http);
  Serial.printf("flight data: after-get HTTP=%d heap=%u min_heap=%u largest=%u t=%lums\n",
                code, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()),
                static_cast<unsigned>(largestInternalFreeBlock()), millis() - started_ms);

  DeserializationError error;
  if (code == HTTP_CODE_OK) {
    WiFiClient* stream = http.getStreamPtr();
    if (stream == nullptr) {
      Serial.printf("flight data: response stream unavailable for %s t=%lums heap=%u min_heap=%u\n",
                    callsign, millis() - started_ms,
                    static_cast<unsigned>(ESP.getFreeHeap()),
                    static_cast<unsigned>(ESP.getMinFreeHeap()));
      http.end();
      return false;
    }
    JsonDocument filter;
    detail::flightDataFilter(filter);
    error = deserializeJson(*doc, *stream, DeserializationOption::Filter(filter));
  }
  http.end();

  if (code == HTTP_CODE_NOT_FOUND) {
    *not_found = true;
    return true;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("flight data: HTTP %d for %s t=%lums heap=%u min_heap=%u\n",
                  code, callsign, millis() - started_ms,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    pauseEnrichmentAfterTlsAllocationFailure(client, "route");
    printNetworkDiagnostics();
    return false;
  }

  if (error) {
    Serial.printf("flight data: JSON parse error: %s for %s t=%lums heap=%u min_heap=%u\n",
                  error.c_str(), callsign, millis() - started_ms,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    return false;
  }
  return true;
}

bool fetchEnrichment(const Aircraft& plane, EnrichmentCacheEntry* entry) {
  const bool route_only = entry->route_pending;
  entry->route[0] = '\0';
  if (!route_only) {
    entry->type[0] = '\0';
  }
  const String url = route_only ? callsignRouteUrl(plane) : enrichmentUrl(plane);

  {
    JsonDocument doc;
    bool not_found = false;
    if (!fetchFlightDataJson(url, plane.callsign, &doc,
                             &not_found)) {
      return false;
    }

    if (!not_found) {
      JsonObject response = doc["response"].as<JsonObject>();
      if (!response.isNull()) {
        detail::parseRoute(response, entry->route, sizeof(entry->route));
        if (!route_only) {
          parseDetailedType(response, entry->type, sizeof(entry->type));
        }
      }
    }
  }

  // The combined endpoint can miss a known callsign. Defer its fallback to
  // another rate-limited slot: never run two TLS handshakes in one loop turn.
  const String route_url = callsignRouteUrl(plane);
  entry->route_pending = !route_only && entry->route[0] == '\0' &&
                        route_url.length() > 0 && route_url != url;

  if (entry->route[0] == '\0' && entry->type[0] == '\0') {
    Serial.printf("flight data: no match for %s\n", plane.callsign);
  }
  return true;
}

void applyCachedEnrichment(Aircraft* plane) {
  EnrichmentCacheEntry* entry = findCacheEntry(*plane, millis());
  if (entry != nullptr) {
    applyCacheEntry(plane, *entry);
  }
}

}  // namespace

void setPollFn(PollFn fn) { s_poll_fn = fn; }

size_t aircraftCount() { return s_aircraft_count; }

const Aircraft* aircraftList() { return s_aircraft; }

uint32_t aircraftRevision() { return s_aircraft_revision; }

uint32_t aircraftReceivedMs() { return s_aircraft_received_ms; }

bool fetchUpdate(double center_lat, double center_lon, float fetch_radius_km) {
  const unsigned long now = millis();
  const unsigned long failure_backoff_ms = adsbFailureBackoff();
  if (s_last_adsb_failure_ms != 0 &&
      now - s_last_adsb_failure_ms < failure_backoff_ms) {
    return false;
  }

  const auto requestFailed = []() {
    s_last_adsb_failure_ms = millis();
    if (s_adsb_failure_count < 10) {
      ++s_adsb_failure_count;
    }
    Serial.printf("adsb: failure backoff=%lus count=%u\n",
                  adsbFailureBackoff() / 1000UL,
                  static_cast<unsigned>(s_adsb_failure_count));
    return false;
  };
  // Keep healthy sessions; replace BOTH objects after peer close or failure.
  // Reusing stopped TLS internals indefinitely is not a recovery strategy.
  auto& session = s_position_session;
  const bool fresh_session = session.prepare();
  WiFiClientSecure& client = session.client();
  HTTPClient& http = session.http();
  if (fresh_session) {
    client.setInsecure();
    client.setHandshakeTimeout(kAdsbHandshakeTimeoutSec);
    static const char* response_headers[] = {"Transfer-Encoding", "Connection"};
    http.collectHeaders(response_headers, 2U);
  }
  Serial.println(fresh_session ? "adsb: fresh HTTPS session" : "adsb: reusing HTTPS connection");

  const unsigned long started_ms = millis();
  printRequestDiagnostics("request-start", started_ms);

  s_last_center_lat = center_lat;
  s_last_center_lon = center_lon;
  const float dist_nm = kmToNauticalMiles(fetch_radius_km);

  String url = kApiBase;
  url += String(center_lat, 6);
  url += "/lon/";
  url += String(center_lon, 6);
  url += "/dist/";
  url += String(dist_nm, 1);

  if (!http.begin(client, url)) {
    Serial.println("adsb: http.begin failed");
    session.discard();
    return requestFailed();
  }

  http.setReuse(true);
  http.useHTTP10(false);
  http.setTimeout(kAdsbRequestTimeoutMs);
  const int code = performGet(http);
  const int response_size = http.getSize();
  String transfer_encoding = http.header("Transfer-Encoding");
  transfer_encoding.toLowerCase();
  const bool chunked_response = transfer_encoding.indexOf("chunked") >= 0;
  static bool response_framing_logged = false;
  if (!response_framing_logged) {
    response_framing_logged = true;
    Serial.printf("adsb: response framing size=%d transfer=%s\n", response_size,
                  transfer_encoding.length() > 0 ? transfer_encoding.c_str()
                                                 : "none");
  }
  printRequestDiagnostics("after-get", started_ms);
  if (code != HTTP_CODE_OK) {
    Serial.printf("adsb: HTTP %d t=%lums heap=%u min_heap=%u largest=%u largest_internal=%u\n", code,
                  millis() - started_ms,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(
                      MALLOC_CAP_8BIT)),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    pauseEnrichmentAfterTlsAllocationFailure(client, "ADS-B");
    char tls_error[96] = {};
    const int tls_code = client.lastError(tls_error, sizeof(tls_error));
    Serial.printf("adsb: transport error=%d detail=%s fresh=%d revision=%lu age=%lus\n",
                  tls_code, tls_error, fresh_session ? 1 : 0,
                  static_cast<unsigned long>(s_aircraft_revision),
                  s_aircraft_revision == 0 ? 0UL : (millis() - s_aircraft_received_ms) / 1000UL);
    printNetworkDiagnostics();
    session.discard();
    return requestFailed();
  }

  WiFiClient* stream = http.getStreamPtr();
  if (stream == nullptr) {
    Serial.println("adsb: response stream unavailable");
    session.discard();
    return requestFailed();
  }

  JsonDocument doc;
  JsonDocument filter;
  JsonArray aircraft_filter_array = filter["ac"].to<JsonArray>();
  JsonObject aircraft_filter = aircraft_filter_array.add<JsonObject>();
  aircraft_filter["hex"] = true;
  aircraft_filter["flight"] = true;
  aircraft_filter["t"] = true;
  aircraft_filter["lat"] = true;
  aircraft_filter["lon"] = true;
  aircraft_filter["alt_baro"] = true;
  aircraft_filter["alt_geom"] = true;
  aircraft_filter["true_heading"] = true;
  aircraft_filter["mag_heading"] = true;
  aircraft_filter["track"] = true;
  aircraft_filter["dir"] = true;
  aircraft_filter["gs"] = true;
  aircraft_filter["tas"] = true;
  aircraft_filter["ias"] = true;
  aircraft_filter["seen_pos"] = true;
  DeserializationError err;
  if (chunked_response) {
    ChunkedTransferStream decoded_stream(*stream);
    err = deserializeJson(doc, decoded_stream,
                          DeserializationOption::Filter(filter));
    decoded_stream.drain();
  } else {
    err = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
  }
  printRequestDiagnostics("after-json", started_ms);
  if (http.header("Connection").indexOf("close") >= 0) {
    Serial.println("adsb: server closes HTTPS session");
  }
  http.end();
  if (err) {
    Serial.printf("adsb: JSON parse error: %s t=%lums heap=%u min_heap=%u\n",
                  err.c_str(), millis() - started_ms,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    session.discard();
    return requestFailed();
  }

  JsonArray ac = doc["ac"].as<JsonArray>();
  if (ac.isNull()) {
    s_aircraft_count = 0;
    s_aircraft_received_ms = millis();
    ++s_aircraft_revision;
    s_last_adsb_failure_ms = 0;
    s_adsb_failure_count = 0;
    Serial.printf("adsb: valid JSON response has no ac array, size=%d "
                  "transfer=%s\n",
                  response_size,
                  transfer_encoding.length() > 0 ? transfer_encoding.c_str()
                                                 : "none");
    return true;
  }

  size_t n = 0;
  for (JsonObject plane : ac) {
    if (n >= kMaxAircraft) {
      break;
    }
    if (!plane["lat"].is<float>() || !plane["lon"].is<float>()) {
      continue;
    }
    if (isOnGround(plane) && !config::kAdsbShowGroundAircraft) {
      continue;
    }

    s_aircraft[n].lat = plane["lat"].as<float>();
    s_aircraft[n].lon = plane["lon"].as<float>();
    s_aircraft[n].nose_deg = pickNoseHeading(plane);
    s_aircraft[n].track_deg = pickTrackHeading(plane);
    s_aircraft[n].gs_knots = pickGroundSpeed(plane);
    const float age_s = plane["seen_pos"] | 0.0f;
    s_aircraft[n].position_age_s =
        std::isfinite(age_s) && age_s >= 0.0f ? age_s : 10.0f;
    const float ground_speed = plane["gs"] | -1.0f;
    const float ground_track = plane["track"] | -1.0f;
    s_aircraft[n].motion_valid =
        std::isfinite(ground_speed) && ground_speed >= 0.0f &&
        ground_speed <= 1500.0f && std::isfinite(ground_track) &&
        ground_track >= 0.0f && ground_track < 360.0f;
    fillTagFields(&s_aircraft[n], plane);
    applyCachedEnrichment(&s_aircraft[n]);
    ++n;
  }

  s_aircraft_count = n;
  s_aircraft_received_ms = millis();
  ++s_aircraft_revision;
  s_last_adsb_failure_ms = 0;
  s_adsb_failure_count = 0;
  printRequestDiagnostics("request-ok", started_ms);
  Serial.printf("adsb: %u aircraft\n", static_cast<unsigned>(n));
  return true;
}

bool enrichOnePending() {
  const unsigned long now = millis();
  if (s_aircraft_count == 0 ||
      (s_last_enrichment_failure_ms != 0 &&
       now - s_last_enrichment_failure_ms <
           config::kFlightLookupFailureBackoffMs) ||
      (s_last_enrichment_lookup_ms != 0 &&
       now - s_last_enrichment_lookup_ms <
           config::kFlightLookupMinIntervalMs)) {
    return false;
  }

  size_t candidate_index = kMaxAircraft;
  float candidate_dist_sq = FLT_MAX;
  for (size_t index = 0; index < s_aircraft_count; ++index) {
    Aircraft& plane = s_aircraft[index];
    if (plane.hex[0] == '\0' && plane.callsign[0] == '\0') {
      continue;
    }

    EnrichmentCacheEntry* cached = findCacheEntry(plane, now);
    if (cached != nullptr) {
      applyCacheEntry(&plane, *cached);
      if (!cached->route_pending) {
        continue;
      }
    }

    const float dlat = plane.lat - static_cast<float>(s_last_center_lat);
    const float dlon = plane.lon - static_cast<float>(s_last_center_lon);
    const float dist_sq = dlat * dlat + dlon * dlon;
    if (dist_sq < candidate_dist_sq) {
      candidate_index = index;
      candidate_dist_sq = dist_sq;
    }
  }

  if (candidate_index == kMaxAircraft) {
    return false;
  }

  Aircraft& plane = s_aircraft[candidate_index];
  s_last_enrichment_lookup_ms = now;
  // Optional lookups must not compete with a stale/failing position feed.
  if (s_adsb_failure_count != 0 || now - s_aircraft_received_ms > 6000UL) {
    return false;
  }
  s_position_session.discard();
  Serial.printf("flight data: TLS slot heap=%u largest=%u\n",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(largestInternalFreeBlock()));
  if (!services::net::tlsRecordPoolIdle() || !enrichmentSlotAllowed()) {
    return false;
  }
#ifdef PLANE_RADAR_ROUTE_MEMORY_PROBE
  // Test-build only: exercise a known public route without altering the feed.
  static bool probe_done = false;
  if (!probe_done && now >= 30000UL) {
    probe_done = true;
    Aircraft probe = {};
    strncpy(probe.callsign, "DLH400", sizeof(probe.callsign) - 1);
    EnrichmentCacheEntry result = {};
    const bool ok = fetchEnrichment(probe, &result);
    Serial.printf("flight data: diagnostic probe ok=%d route=%s heap=%u min_heap=%u\n",
                  ok ? 1 : 0, result.route,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMinFreeHeap()));
    return false;
  }
#endif
  EnrichmentCacheEntry* entry = cacheSlotFor(plane, now);
  if (!sameKey(*entry, plane) || !entry->route_pending) {
    *entry = {};
    strncpy(entry->hex, plane.hex, sizeof(entry->hex) - 1);
    strncpy(entry->callsign, plane.callsign, sizeof(entry->callsign) - 1);
  }

  if (!fetchEnrichment(plane, entry)) {
    // Network failures are retried after a short global backoff.
    *entry = {};
    s_last_enrichment_failure_ms = millis();
    return false;
  }

  s_last_enrichment_failure_ms = 0;
  entry->refreshed_ms = millis();
  const bool changed = applyCacheEntry(&plane, *entry);
  Serial.printf("flight data: %s %s %s\n", plane.callsign,
                entry->route[0] != '\0' ? entry->route : "(route unknown)",
                entry->type[0] != '\0' ? entry->type : "(type unchanged)");
  return changed;
}

namespace {

bool enrichmentSlotAllowed() {
  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block = largestFreeBlock();
  const size_t largest_internal_block = largestInternalFreeBlock();
  const bool pause_critical = !services::net::tlsSlotHasReserve(
      free_heap, largest_block, largest_internal_block,
      config::kEnrichmentSlotFreeHeap, config::kEnrichmentSlotLargestBlock);

  if (pause_critical) {
    if (!s_enrichment_paused) {
      Serial.printf("flight data: paused, heap=%u largest=%u\n",
                    static_cast<unsigned>(free_heap),
                    static_cast<unsigned>(largest_block));
    }
    s_enrichment_paused = true;
    return false;
  }

  if (!s_enrichment_paused) {
    return true;
  }

  s_enrichment_paused = false;
  Serial.printf("flight data: resumed, heap=%u largest=%u\n",
                static_cast<unsigned>(free_heap),
                static_cast<unsigned>(largest_block));
  return true;
}

}  // namespace

}  // namespace services::adsb
