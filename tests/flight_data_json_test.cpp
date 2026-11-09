#include "services/flight_data_json.h"

#include <cassert>
#include <string>
#include <cstdio>

namespace {
void expectRoute(const char* json, const char* expected) {
  JsonDocument doc;
  JsonDocument filter;
  services::adsb::detail::flightDataFilter(filter);
  assert(!deserializeJson(doc, json, DeserializationOption::Filter(filter)));
  char route[10] = {};
  services::adsb::detail::parseRoute(doc["response"].as<JsonObject>(), route, sizeof(route));
  assert(std::strcmp(route, expected) == 0);
  char guard[3] = {'x', 'x', '!'};
  services::adsb::detail::parseRoute(doc["response"].as<JsonObject>(), guard, 2);
  assert(guard[1] == '\0' || guard[0] == '\0');
  assert(guard[2] == '!');
}
}

int main() {
  expectRoute(R"({"response":{"flightroute":{"origin":{"iata_code":"DUS","icao_code":"EDDL"},"destination":{"iata_code":"LHR","icao_code":"EGLL"}}}})", "DUS-LHR");
  expectRoute(R"({"response":{"flightroute":{"origin":{"iata_code":null,"icao_code":"EDDL"},"destination":{"iata_code":" ","icao_code":"EGLL"}}}})", "EDDL-EGLL");
  expectRoute(R"({"response":{"flightroute":{"origin":{"iata_code":"DUS  "},"destination":{"iata_code":"LHR "}}}})", "DUS-LHR");
  expectRoute(R"({"response":"unknown aircraft"})", "");
  expectRoute(R"({"response":{"flightroute":{"origin":{"iata_code":"DUS"}}}})", "");
  expectRoute(R"({"response":{}})", "");
  // Unknown/large fields must not be retained in the filtered DOM.
  const std::string json = "{\"unneeded\":\"" + std::string(10000, 'x') + R"(","response":{"aircraft":{"type":"Boeing 737-800","icao_type":"B738","history":")" + std::string(10000, 'y') + "\"}}}";
  JsonDocument doc, filter;
  services::adsb::detail::flightDataFilter(filter);
  assert(!deserializeJson(doc, json, DeserializationOption::Filter(filter)));
  assert(doc["unneeded"].isNull());
  assert(doc["response"]["aircraft"]["history"].isNull());
  assert(std::strcmp(doc["response"]["aircraft"]["icao_type"] | "", "B738") == 0);
  std::string serialized;
  serializeJson(doc, serialized);
  assert(serialized.size() < 150);
  std::puts("PASS: filtered route JSON, IATA/ICAO fallback, whitespace, unknown/missing data,");
  std::puts("      output bounds and exclusion of large unused response fields");
}
