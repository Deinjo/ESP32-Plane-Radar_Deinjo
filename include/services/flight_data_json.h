#pragma once

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

namespace services::adsb::detail {

inline void flightDataFilter(JsonDocument& filter) {
  filter["response"]["flightroute"]["origin"]["iata_code"] = true;
  filter["response"]["flightroute"]["origin"]["icao_code"] = true;
  filter["response"]["flightroute"]["destination"]["iata_code"] = true;
  filter["response"]["flightroute"]["destination"]["icao_code"] = true;
  filter["response"]["aircraft"]["type"] = true;
  filter["response"]["aircraft"]["icao_type"] = true;
}

inline void airportCode(JsonObject airport, char (&out)[5]) {
  for (const char* key : {"iata_code", "icao_code"}) {
    const char* text = airport[key] | "";
    size_t length = std::strlen(text);
    while (length != 0 && text[length - 1] == ' ') {
      --length;
    }
    if (length == 0) {
      continue;
    }
    if (length > sizeof(out) - 1) {
      length = sizeof(out) - 1;
    }
    std::memcpy(out, text, length);
    out[length] = '\0';
    return;
  }
  out[0] = '\0';
}

inline void parseRoute(JsonObject response, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  JsonObject route = response["flightroute"].as<JsonObject>();
  if (route.isNull()) {
    return;
  }
  char origin[5] = {};
  char destination[5] = {};
  airportCode(route["origin"].as<JsonObject>(), origin);
  airportCode(route["destination"].as<JsonObject>(), destination);
  if (origin[0] != '\0' && destination[0] != '\0') {
    std::snprintf(out, out_len, "%s-%s", origin, destination);
  }
}

}  // namespace services::adsb::detail
