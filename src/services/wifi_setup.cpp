#include "services/wifi_setup.h"

#include <WiFi.h>
#include <WiFiManager.h>

#include <ArduinoJson.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <Preferences.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>

#ifdef WM_MDNS
#include <ESPmDNS.h>
#endif

#include "config.h"
#include "data/large_airports.h"
#include "hardware/display.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/adsb_client.h"
#include "services/aircraft_snapshot.h"
#include "services/portal_html.h"
#include "services/radar_location.h"
#include "ui/radar_range.h"
#include "ui/radar_display.h"
#include "ui/status_screens.h"
#include "web/display_page.h"

portMUX_TYPE s_boot_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_boot_tap_pending = false;
volatile bool s_boot_is_down = false;
volatile unsigned long s_boot_down_ms = 0;
bool s_long_press_handled = false;
bool s_boot_interrupt_attached = false;

void IRAM_ATTR onBootButtonIsr() {
  const bool down = digitalRead(config::kBootPin) == LOW;
  const unsigned long now = millis();
  portENTER_CRITICAL_ISR(&s_boot_mux);
  if (down) {
    s_boot_is_down = true;
    s_boot_down_ms = now;
  } else if (s_boot_is_down) {
    const unsigned long held = now - s_boot_down_ms;
    if (held >= config::kBootTapMinMs && held < config::kBootResetHoldMs) {
      s_boot_tap_pending = true;
    }
    s_boot_is_down = false;
  }
  portEXIT_CRITICAL_ISR(&s_boot_mux);
}

void initBootButton() {
  pinMode(config::kBootPin, INPUT_PULLUP);
  if (s_boot_interrupt_attached) {
    return;
  }
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kBootPin)),
                  onBootButtonIsr, CHANGE);
  s_boot_interrupt_attached = true;
}

namespace {

// Keep WiFiManager's page-sized Strings small. Shared assets are served from
// flash in bounded writes, not copied into every native portal page.
constexpr char kPortalHead[] =
    "<link rel='stylesheet' href='/portal.css?v=" __DATE__ "-" __TIME__ "'>"
    "<script defer src='/portal.js?v=" __DATE__ "-" __TIME__ "'></script>";
constexpr char kPortalCss[] PROGMEM =
    "*{box-sizing:border-box}"
    "body{margin:0;padding:24px;background:#0d151e;color:#d7e0e9;"
    "font-family:Segoe UI,Arial,sans-serif;font-size:15px;line-height:1.45}"
    "body>div,body>form,form{max-width:760px;margin:0 auto}"
    ".wrap{display:block!important;width:100%!important;min-width:0!important;"
    "max-width:760px!important;text-align:left!important}"
    ".wrap form{display:block!important;width:100%!important}"
    ".wrap form>div{float:none!important;clear:both!important;width:100%!important;"
    "display:block!important}"
    "form{padding:24px;background:#141f2a;border:1px solid #2a3a49;"
    "border-radius:12px;box-shadow:0 12px 32px #0005}"
    ".c{float:none!important;clear:both!important;width:100%!important;"
    "box-sizing:border-box!important}"
    "h1,h2,h3{color:#edf3f8;font-weight:600;letter-spacing:.01em}"
    "h1{font-size:1.35rem;margin:0 0 1.2rem}"
    "h2{font-size:1.05rem;margin:1.5rem 0 .6rem}"
    "h3{font-size:1rem;margin:1.5rem 0 .7rem!important;padding:10px 12px;"
    "background:#1a2937;border-left:3px solid #62899d;border-radius:6px}"
    "label{color:#b8c6d3}"
    "input:not([type]),input[type=text],input[type=password],input[type=number],input[type=time],select{"
    "width:100%;padding:9px 10px;background:#0f1923;color:#e4edf4;"
    "border:1px solid #35495b;border-radius:6px;outline:none}"
    "input[type=color]{width:52px;height:32px;padding:3px;background:#0f1923;"
    "border:1px solid #526577;border-radius:5px}"
    "input[type=range]{accent-color:#7098aa}input[type=checkbox]{accent-color:#7098aa}"
    ".common-setup input{width:100%;margin:5px 0;font:inherit}"
    ".common-setup input[type=checkbox]{width:auto}"
    "dt{font-weight:600}dd{margin:0 0 .6rem;overflow-wrap:anywhere}"
    "button,input[type=submit]{padding:8px 13px;background:#38596b;color:#eef5f8;"
    "border:1px solid #5e8191;border-radius:6px;font:inherit;cursor:pointer}"
    "button:hover,input[type=submit]:hover{background:#486f80;border-color:#83a8b7}"
    "a{color:#8eb5c5}small{color:#9aaabd}"
    ".home-link{display:inline-block;margin:0 0 16px;padding:8px 13px;"
    "background:#38596b;color:#eef5f8;border:1px solid #5e8191;"
    "border-radius:6px;text-decoration:none;font-weight:600}"
    ".home-link:hover{background:#486f80;border-color:#83a8b7}"
    ".portal-menu-link{display:block;width:calc(100% - 32px);margin:16px;"
    "padding:12px;text-align:center;background:#38596b;color:#eef5f8;"
    "border:1px solid #5e8191;border-radius:6px;text-decoration:none;"
    "font-size:1.05rem;font-weight:600}"
     ".portal-menu-link:hover{background:#486f80;border-color:#83a8b7}"
     ".portal-divider{border:0;border-top:2px solid #71808a;margin:24px 0 16px}"
      "@media(max-width:520px){body{padding:12px}form{padding:16px}}";
constexpr char kPortalJs[] PROGMEM =
     "document.addEventListener('DOMContentLoaded',function(){"
    "var w=document.querySelector('.wrap');if(!w)return;"
     "if(location.pathname==='/' ){"
     "var r=document.createElement('div');r.className='c';"
     "var d=document.createElement('a');d.href='/display';d.textContent='Display';"
     "d.className='portal-menu-link';r.appendChild(d);"
     "var hr=document.createElement('hr');hr.className='portal-divider';"
     "var h=w.querySelector('h1');if(h&&h.nextSibling){w.insertBefore(r,h.nextSibling);"
     "w.insertBefore(hr,r.nextSibling);}else{w.prepend(hr);w.prepend(r);}return;}"
     "var a=document.createElement('a');a.href='/';a.textContent='Home';"
     "a.className='home-link';w.prepend(a);"
     "if(location.pathname==='/param'){var c=document.createElement('a');"
     "c.href='/component';c.textContent='Component Setup';"
     "c.className='portal-menu-link';var cr=document.createElement('div');"
     "cr.className='c';cr.appendChild(c);var ch=document.createElement('hr');"
     "ch.className='portal-divider';var s=w.querySelector('.msg');"
     "if(s){w.insertBefore(ch,s);w.insertBefore(cr,s);}else{w.appendChild(ch);w.appendChild(cr);}}"
      "});";

/** Separate from planeradar prefs (rangeInit) to avoid NVS handle conflicts. */
constexpr char kWifiPrefsNamespace[] = "wifi";
constexpr char kPrefsForcePortalKey[] = "portal";

bool s_force_config_portal = false;
// Reuse native fragments, not the library's unbounded whole-page builders.
// Keep this adapter local; Wi-Fi scan/save/connect remain native.
class PortalWiFiManager : public WiFiManager {
 public:
  String infoItem(const char* name) { return getInfoData(name); }
  String menuItems() { return getMenuOut(); }
  String statusItem() { String item; reportStatus(item); return item; }
  String scanItems(bool refresh) { WiFi_scanNetworks(refresh, false); return getScanItemOut(); }
  String staticFields() { return getStaticOut(); }
  bool redirectCaptivePortal() { return captivePortal(); }
  String wifiFields() {
    String fields = FPSTR(HTTP_FORM_WIFI);
    fields.replace(FPSTR(T_v), htmlEntities(WiFi_SSID()));
    fields.replace(FPSTR(T_p), WiFi_psk().length() != 0 ? S_passph : "");
    return fields;
  }
  void notePortalRequest() { handleRequest(); }
};
PortalWiFiManager s_wm;
bool s_wm_configured = false;

void ensureWifiManager();
void startLanWebPortal();
void stopLanWebPortal();
bool wifiLinkUp();
void attachSettingsRoutes();

void handleDisplayPage();
void handleDisplayBmp();

const data::large_airports::Airport* findAirportByCode(const String& code) {
  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    const auto& airport = data::large_airports::kAirports[i];
    if (code.equalsIgnoreCase(airport.ident) ||
        (airport.iata[0] != '\0' && code.equalsIgnoreCase(airport.iata))) {
      return &airport;
    }
  }
  return nullptr;
}

void handleAirportSearch() {
  if (!s_wm.server) return;
  String query = s_wm.server->arg("q");
  query.toUpperCase();
  JsonDocument document;
  JsonArray results = document.to<JsonArray>();
  size_t count = 0;
  for (size_t i = 0; i < data::large_airports::kAirportCount && count < 50; ++i) {
    const auto& airport = data::large_airports::kAirports[i];
    String ident = airport.ident;
    String iata = airport.iata;
    ident.toUpperCase();
    iata.toUpperCase();
    if (query.length() > 0) {
      const bool ident_match = ident.indexOf(query) >= 0;
      const bool iata_match = airport.iata[0] != '\0' && iata.indexOf(query) >= 0;
      if (!ident_match && !iata_match) continue;
    }
    JsonObject result = results.add<JsonObject>();
    result["icao"] = airport.ident;
    result["iata"] = airport.iata;
    ++count;
  }
  String body;
  serializeJson(document, body);
  s_wm.server->send(200, "application/json", body);
}

void handleAirportLookup() {
  if (!s_wm.server) return;
  const auto* airport = findAirportByCode(s_wm.server->arg("code"));
  if (!airport) {
    s_wm.server->send(404, "application/json", "{\"success\":false}");
    return;
  }
  JsonDocument document;
  document["success"] = true;
  document["icao"] = airport->ident;
  document["iata"] = airport->iata;
  document["lat"] = static_cast<double>(airport->lat_e7) / 1e7;
  document["lon"] = static_cast<double>(airport->lon_e7) / 1e7;
  String body;
  serializeJson(document, body);
  s_wm.server->send(200, "application/json", body);
}

void handleLocationDefault() {
  if (!s_wm.server) return;
  services::location::clear();
  s_wm.server->send(200, "application/json",
                    "{\"success\":true,\"lat\":" +
                        String(config::kDefaultRadarLat, 7) +
                        ",\"lon\":" + String(config::kDefaultRadarLon, 7) + "}");
}

void handleVisibility() {
  if (!s_wm.server) return;
  const auto value = [](services::settings::VisibilityId id) {
    return services::settings::visible(id) ? "true" : "false";
  };
  String body = "{\"show_grid\":" + String(value(services::settings::VisibilityId::kGrid)) +
                ",\"show_center\":" + String(value(services::settings::VisibilityId::kCenter)) +
                ",\"show_label\":" + String(value(services::settings::VisibilityId::kLabel)) +
                ",\"show_aircraft\":" + String(value(services::settings::VisibilityId::kAircraft)) +
                ",\"show_track\":" + String(value(services::settings::VisibilityId::kTrackVector)) +
                ",\"show_tag_type\":" + String(value(services::settings::VisibilityId::kTagType)) +
                ",\"show_tag_alt\":" + String(value(services::settings::VisibilityId::kTagAltitude)) +
                ",\"show_runway\":" + String(value(services::settings::VisibilityId::kRunway)) +
                ",\"show_runway_label\":" + String(value(services::settings::VisibilityId::kRunwayLabel)) +
                ",\"show_road\":" + String(value(services::settings::VisibilityId::kRoad)) +
                ",\"show_city\":" + String(value(services::settings::VisibilityId::kCity)) +
                ",\"show_road_primary\":" + String(value(services::settings::VisibilityId::kRoadPrimary)) + "}";
  s_wm.server->send(200, "application/json", body);
}

String componentColor(uint32_t color) {
  char value[8];
  snprintf(value, sizeof(value), "#%06lX",
           static_cast<unsigned long>(color & 0xFFFFFFUL));
  return String(value);
}

// Bounded plain-HTTP output without page-sized Strings or tiny chunk-framing
// allocations. TLS clients and native WiFiManager save handlers are untouched.
struct PortalChunkSink {
  WebServer& web;
  unsigned long transfer_started;
  bool preserve_heap;
  explicit PortalChunkSink(WebServer& server, bool reserve_heap = false)
      : web(server), transfer_started(millis()), preserve_heap(reserve_heap) {}

  bool writeAll(const char* bytes, size_t length) {
    WiFiClient client = web.client();
    const int socket = client.fd();
    const unsigned long started = millis();
    while (length != 0 && socket >= 0 && client.connected()) {
      if (millis() - transfer_started >= 8000UL) break;
      // WiFiClient::write can retry for many seconds under heap/TCP pressure.
      // Bound this plain-HTTP socket only; do not touch the ADS-B TLS client.
      // Large previews must let queued TCP/Wi-Fi buffers drain before they
      // consume the RAM needed by the still-live ADS-B TLS connection.
      const bool wait_for_heap = preserve_heap &&
          (ESP.getFreeHeap() < 16000U || ESP.getMaxAllocHeap() < 4096U);
      const int written = wait_for_heap ? 0 : ::send(socket, bytes, length, MSG_DONTWAIT);
      if (written > 0) {
        bytes += written;
        length -= static_cast<size_t>(written);
      } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;
      }
      if (length == 0) {
        return true;
      }
      if (millis() - started >= 2000UL) {
        break;
      }
      delay(1);
    }
    // Abort rather than emitting a corrupt/truncated form with a success tail.
    if (socket >= 0) {
      ::shutdown(socket, SHUT_RDWR);
    }
    Serial.println("Setup: response aborted after disconnect or write timeout");
    return false;
  }

  bool operator()(const char* bytes, size_t length) {
    const bool sent = writeAll(bytes, length);
    delay(5);
    return sent;
  }
};
using PortalHtml = services::portal::HtmlWriter<PortalChunkSink>;

void servePortalAsset(const char* content_type, const char* bytes, size_t length) {
  if (!s_wm.server) {
    return;
  }
  WebServer& web = *s_wm.server;
  web.client().setNoDelay(true);
  web.sendHeader("Cache-Control", "public, max-age=86400, immutable");
  web.setContentLength(length);
  web.send(200, content_type, "");
  PortalChunkSink sink{web};
  while (length != 0) {
    const size_t count = length > 512 ? 512 : length;
    if (!sink(bytes, count)) {
      break;
    }
    bytes += count;
    length -= count;
  }
}

void startPortalPage(WebServer& web, const char* content_type = "text/html; charset=utf-8") {
  s_wm.notePortalRequest();
  Serial.printf("portal: stream %s heap=%u\n", web.uri().c_str(),
                static_cast<unsigned>(ESP.getFreeHeap()));
  web.client().setNoDelay(true);
  // Close-delimited HTTP/1.0 is valid for HTTP/1.0 and HTTP/1.1 clients and
  // avoids WebServer's heap-allocated HTTP chunk headers. handleClient() closes
  // its connection as soon as this handler returns; do not set _chunked state.
  char header[192];
  snprintf(header, sizeof(header),
      "HTTP/1.0 200 OK\r\nContent-Type: %s\r\n"
      "Cache-Control: no-store\r\nConnection: close\r\n\r\n", content_type);
  PortalChunkSink sink{web};
  sink.writeAll(header, strlen(header));
}

void finishPortalPage(PortalHtml& html) {
  html.flush();
}

void handleDisplayPage() {
  if (!s_wm.server) return;
  WebServer& web = *s_wm.server;
  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  page.append(web::display::kPage);
  finishPortalPage(page);
}

void handleAircraftSnapshot() {
  if (!s_wm.server) return;
  WebServer& web = *s_wm.server;
  startPortalPage(web, "application/json");
  PortalChunkSink sink{web};
  PortalHtml json(sink);
  services::adsb::writeAircraftSnapshot(json, services::adsb::aircraftList(),
      services::adsb::aircraftCount(), services::adsb::aircraftRevision(),
      services::adsb::aircraftReceivedMs(), millis(), services::location::lat(),
      services::location::lon(), ui::radar::useMiles());
  finishPortalPage(json);
}

class PortalBmpOutput : public Print {
 public:
  explicit PortalBmpOutput(WebServer& web) : sink_{web, true} {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* bytes, size_t length) override {
    size_t sent = 0;
    while (sent < length) {
      const size_t count = std::min<size_t>(1440, length - sent);
      if (!sink_.writeAll(reinterpret_cast<const char*>(bytes + sent), count)) break;
      sent += count;
      delay(1);
    }
    return sent;
  }
 private:
  PortalChunkSink sink_;
};

void handleDisplayBmp() {
  if (!s_wm.server) return;
  if (!ui::radarDisplayFrameReady()) {
    s_wm.server->send(503, "text/plain", "Display frame not ready");
    return;
  }
  constexpr size_t kBmpSize = 54 + 240 * 240 * 3;
  const unsigned long started = millis();
  Serial.println("portal: BMP begin");
  // Let TCP coalesce the 720-byte rows instead of allocating one tiny packet
  // per writer fragment. This applies only to the preview's plain HTTP socket.
  s_wm.server->client().setNoDelay(false);
  s_wm.server->sendHeader("Connection", "close");
  s_wm.server->setContentLength(kBmpSize);
  s_wm.server->send(200, "image/bmp", "");
  PortalBmpOutput output(*s_wm.server);
  ui::radarDisplayWriteBmp(output);
  Serial.printf("portal: BMP end t=%lums\n", millis() - started);
}

void appendNativePortalHead(PortalHtml& page, const char* title) {
  page.append("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  page.escaped(title);
  page.append("</title>");
  page.append(HTTP_SCRIPT);
  page.append(HTTP_STYLE);
  page.append(kPortalHead);
  page.append("</head><body><div class='wrap'>");
}

void handleHomePage() {
  if (!s_wm.server || s_wm.redirectCaptivePortal()) {
    return;
  }
  WebServer& web = *s_wm.server;
  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  appendNativePortalHead(page, "Plane Radar");
  page.append("<h1>Plane Radar</h1><h3>");
  page.escaped(s_wm.getDefaultAPName().c_str());
  page.append(" - ");
  page.escaped((s_wm.getConfigPortalActive() ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
  page.append("</h3>");
  page.append(s_wm.menuItems().c_str());
  page.append(s_wm.statusItem().c_str());
  page.append("</div></body></html>");
  finishPortalPage(page);
}

void handleWifiPage(bool scan) {
  if (!s_wm.server) {
    return;
  }
  WebServer& web = *s_wm.server;
  // The first native scan can take longer than the bounded HTTP transfer.
  // Complete it before starting the response deadline, not between chunks.
  String scan_items;
  if (scan) {
    scan_items = s_wm.scanItems(web.hasArg("refresh"));
  }
  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  appendNativePortalHead(page, "Configure WiFi");
  if (scan) {
    // The native scan list is the only variable-sized fragment; no large page
    // String coexists with it. Saving credentials still uses /wifisave unchanged.
    page.append(scan_items.c_str());
    scan_items = String();
  }
  page.append("<form method='POST' action='/wifisave'>");
  page.append(s_wm.wifiFields().c_str());
  page.append(s_wm.staticFields().c_str());
  page.append(HTTP_FORM_WIFI_END);
  page.append(HTTP_FORM_END);
  page.append(HTTP_SCAN_LINK);
  page.append(s_wm.statusItem().c_str());
  page.append("</div></body></html>");
  finishPortalPage(page);
}

void rejectIncompleteForm(WebServer& web) {
  web.send(400, "text/plain",
           "Incomplete setup form. Nothing saved. Reload the setup page.");
  Serial.println("Setup: rejected incomplete form; settings unchanged");
}

void handleInfoPage() {
  if (!s_wm.server) {
    return;
  }
  WebServer& web = *s_wm.server;
  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  page.append("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Device information</title>");
  page.append(kPortalHead);
  page.append("</head><body><div class='wrap'><h1>Device information</h1>");
  const char* items[] = {"esphead", "uptime", "chipid", "chiprev", "idesize",
      "flashsize", "cpufreq", "freeheap", "memsketch", "memsmeter", "temp",
      "wifihead", "conx", "stassid", "staip", "stagw", "stasub", "dnss",
      "host", "stamac"};
  for (const char* name : items) {
    page.append(s_wm.infoItem(name).c_str());
  }
  // The C3 has one core. Native WiFiManager's lastreset item assumes two.
  char reset_reason[12];
  snprintf(reset_reason, sizeof(reset_reason), "%d", static_cast<int>(esp_reset_reason()));
  page.append("<dt>Last reset reason (ESP-IDF)</dt><dd>");
  page.escaped(reset_reason);
  page.append("</dd>");
  // LAN mode has no AP netif; querying its hostname produces a framework error.
  if ((WiFi.getMode() & WIFI_AP) != 0) {
    page.append("<h3>WiFi access point</h3>");
    const char* ap_items[] = {"apip", "apmac", "aphost", "apbssid"};
    for (const char* name : ap_items) {
      page.append(s_wm.infoItem(name).c_str());
    }
  }
  page.append("</dl><h3>About</h3><dl>");
  const char* about[] = {"aboutver", "aboutarduinover", "aboutsdkver", "aboutdate"};
  for (const char* name : about) {
    page.append(s_wm.infoItem(name).c_str());
  }
  page.append("</dl><p><a href='/firmware'>Firmware update</a></p>"
      "<form action='/erase' method='get'><button class='D'>Erase WiFi config</button></form>"
      "</div></body></html>");
  finishPortalPage(page);
}

void appendComponentRow(PortalHtml& page, const char* id, const char* label,
                        services::settings::ColorId color_id,
                        services::settings::VisibilityId visibility_id,
                        bool include_visibility,
                        const char* default_color) {
  page.append("<section class='component'><div class='component-title'>");
  page.escaped(label);
  page.append("</div><div class='component-controls'><input id='");
  page.escaped(id);
  page.append("' name='");
  page.escaped(id);
  page.append("' type='color' value='");
  page.escaped(componentColor(services::settings::color(color_id)).c_str());
  page.append("' data-default='");
  page.escaped(default_color);
  page.append("'><button type='button' data-reset='");
  page.escaped(id);
  page.append("'>Back to default</button>");
  if (include_visibility) {
    page.append("<label><input type='checkbox' name='");
    page.escaped(id);
    page.append("_visible' value='T'");
    if (services::settings::visible(visibility_id)) page.append(" checked");
    page.append("> On</label>");
  }
  page.append("</div></section>");
}

void handleComponentPage() {
  if (!s_wm.server) return;
  WebServer& web = *s_wm.server;
  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  page.append("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ComponentSetup</title><style>"
         "*{box-sizing:border-box}body{margin:0;padding:24px;background:#0d151e;color:#d7e0e9;"
         "font:15px Segoe UI,Arial,sans-serif}main{max-width:760px;margin:auto;background:#141f2a;"
         "padding:24px;border:1px solid #2a3a49;border-radius:12px}h1{font-size:1.35rem}"
         ".component{padding:12px 0;border-top:1px solid #2a3a49}.component-title{font-weight:600;"
         "margin-bottom:8px}.component-controls{display:flex;align-items:center;gap:10px;flex-wrap:wrap}"
         "input[type=color]{width:54px;height:34px;background:#0f1923;border:1px solid #526577;"
         "border-radius:5px}button,input[type=submit]{padding:8px 13px;background:#38596b;color:#eef5f8;"
         "border:1px solid #5e8191;border-radius:6px;font:inherit}a{color:#8eb5c5}"
         "@media(max-width:520px){body{padding:12px}main{padding:16px}}"
          "</style></head><body><main><a href='/param'>CommonSetup</a> | "
          "<a href='/'>Home</a><h1>ComponentSetup</h1>"
          "<form method='POST' action='/component-save'>");
  appendComponentRow(page, "color_bg", "Background", services::settings::ColorId::kBackground,
                     services::settings::VisibilityId::kGrid, false, "#040a1c");
  appendComponentRow(page, "color_grid", "Grid", services::settings::ColorId::kGrid,
                     services::settings::VisibilityId::kGrid, true, "#106420");
  appendComponentRow(page, "color_center", "Center", services::settings::ColorId::kCenter,
                     services::settings::VisibilityId::kCenter, true, "#ffffff");
  appendComponentRow(page, "color_label", "Labels", services::settings::ColorId::kLabel,
                     services::settings::VisibilityId::kLabel, true, "#ffffff");
  appendComponentRow(page, "color_aircraft", "Aircraft", services::settings::ColorId::kAircraft,
                     services::settings::VisibilityId::kAircraft, true, "#ff0000");
  appendComponentRow(page, "color_track", "Track vector", services::settings::ColorId::kTrackVector,
                     services::settings::VisibilityId::kTrackVector, true, "#ff00ff");
  appendComponentRow(page, "color_tag_type", "Aircraft type", services::settings::ColorId::kTagType,
                     services::settings::VisibilityId::kTagType, true, "#ffc800");
  appendComponentRow(page, "color_tag_alt", "Altitude", services::settings::ColorId::kTagAltitude,
                     services::settings::VisibilityId::kTagAltitude, true, "#5ac8ff");
  appendComponentRow(page, "color_runway", "Runways", services::settings::ColorId::kRunway,
                     services::settings::VisibilityId::kRunway, true, "#3896aa");
  appendComponentRow(page, "color_runway_label", "Runway labels", services::settings::ColorId::kRunwayLabel,
                     services::settings::VisibilityId::kRunwayLabel, true, "#6ed2e6");
  appendComponentRow(page, "color_road", "Motorways", services::settings::ColorId::kRoad,
                     services::settings::VisibilityId::kRoad, true, "#69737d");
  appendComponentRow(page, "color_road_primary", "Primary roads", services::settings::ColorId::kRoadPrimary,
                     services::settings::VisibilityId::kRoadPrimary, true, "#3c4650");
  appendComponentRow(page, "color_city", "Cities", services::settings::ColorId::kCity,
                     services::settings::VisibilityId::kCity, true, "#aaaaaa");
  appendComponentRow(page, "color_footer", "Footer background", services::settings::ColorId::kFooterBackground,
                     services::settings::VisibilityId::kGrid, false, "#031020");
  page.append("<input type='hidden' name='_form_complete' value='component-v1'>"
          "<p><input type='submit' value='Save'></p></form><script>"
          "document.querySelectorAll('[data-reset]').forEach(function(b){b.onclick=function(){"
          "document.getElementById(b.dataset.reset).value=document.getElementById(b.dataset.reset).dataset.default;};});"
          "</script></main></body></html>");
  finishPortalPage(page);
}

void handleComponentSave() {
  if (!s_wm.server) return;
  WebServer& web = *s_wm.server;
  const String marker = web.arg(services::portal::kFormCompleteField);
  if (!services::portal::completeComponentForm(marker.c_str(), [&web](const char* name) {
        return web.hasArg(name) && web.arg(name).length() != 0;
      })) {
    rejectIncompleteForm(web);
    return;
  }
  const String color_background = web.arg("color_bg");
  const String color_grid = web.arg("color_grid");
  const String color_label = web.arg("color_label");
  const String color_center = web.arg("color_center");
  const String color_aircraft = web.arg("color_aircraft");
  const String color_track = web.arg("color_track");
  const String color_tag_type = web.arg("color_tag_type");
  const String color_tag_alt = web.arg("color_tag_alt");
  const String color_runway = web.arg("color_runway");
  const String color_runway_label = web.arg("color_runway_label");
  const String color_footer = web.arg("color_footer");
  const String color_road = web.arg("color_road");
  const String color_city = web.arg("color_city");
  const String color_road_primary = web.arg("color_road_primary");
  services::settings::saveColorsFromPortal(
      color_background.c_str(), color_grid.c_str(), color_label.c_str(),
      color_center.c_str(), color_aircraft.c_str(), color_track.c_str(),
      color_tag_type.c_str(), color_tag_alt.c_str(), color_runway.c_str(),
      color_runway_label.c_str(), color_footer.c_str(), color_road.c_str(),
      color_city.c_str(), color_road_primary.c_str());
  const String show_grid = web.arg("color_grid_visible");
  const String show_center = web.arg("color_center_visible");
  const String show_label = web.arg("color_label_visible");
  const String show_aircraft = web.arg("color_aircraft_visible");
  const String show_track = web.arg("color_track_visible");
  const String show_tag_type = web.arg("color_tag_type_visible");
  const String show_tag_alt = web.arg("color_tag_alt_visible");
  const String show_runway = web.arg("color_runway_visible");
  const String show_runway_label = web.arg("color_runway_label_visible");
  const String show_road = web.arg("color_road_visible");
  const String show_city = web.arg("color_city_visible");
  const String show_road_primary = web.arg("color_road_primary_visible");
  services::settings::saveVisibilityFromPortal(
      show_grid.c_str(), show_center.c_str(), show_label.c_str(),
      show_aircraft.c_str(), show_track.c_str(), show_tag_type.c_str(),
      show_tag_alt.c_str(), show_runway.c_str(), show_runway_label.c_str(),
      show_road.c_str(), show_city.c_str(), show_road_primary.c_str());
  web.sendHeader("Location", "/component");
  web.send(303, "text/plain", "Saved");
}

constexpr int kCoordParamLen = 20;
constexpr char kLatitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-90\" max=\"90\"";
constexpr char kLongitudeInputAttrs[] =
    "type=\"number\" step=\"0.000001\" min=\"-180\" max=\"180\"";
constexpr int kOtaPasswordParamLen =
    static_cast<int>(services::settings::kOtaPasswordMaxLen);
constexpr int kTextScaleParamLen = 4;

WiFiManagerParameter s_param_lat("radar_lat", "Latitude (deg)", "0",
                                kCoordParamLen, kLatitudeInputAttrs);
WiFiManagerParameter s_param_lon("radar_lon", "Longitude (deg)", "0",
                                 kCoordParamLen, kLongitudeInputAttrs);
WiFiManagerParameter s_param_location_selector(
    "<div class=\"location-tools\"><label>Airport location</label>"
    "<input id=\"airport_code\" list=\"airport_list\" placeholder=\"ICAO or IATA code\">"
    "<datalist id=\"airport_list\"></datalist>"
    "<button type=\"button\" id=\"airport_default\">Location back to default</button>"
    "</div><script>(function(){"
    "var i=document.getElementById('airport_code'),l=document.getElementById('airport_list'),"
    "d=document.getElementById('airport_default'),lat=document.getElementById('radar_lat'),"
    "lon=document.getElementById('radar_lon');if(!i||!l||!lat||!lon)return;"
    "var search=function(){if(i.value.length<1)return;fetch('/airports?q='+encodeURIComponent(i.value))"
    ".then(function(r){return r.json()}).then(function(a){l.innerHTML='';a.forEach(function(x){"
    "var o=document.createElement('option');o.value=x.icao;o.label=x.iata?x.iata+' - '+x.icao:x.icao;l.appendChild(o);});});};"
    "i.addEventListener('input',search);i.addEventListener('change',function(){"
    "fetch('/airport?code='+encodeURIComponent(i.value)).then(function(r){return r.json()})"
    ".then(function(x){if(x.success){lat.value=x.lat;lon.value=x.lon;}});});"
    "d.onclick=function(){fetch('/location-default',{method:'POST'}).then(function(r){return r.json()})"
    ".then(function(x){if(x.success){i.value='';lat.value=x.lat;lon.value=x.lon;}});};"
    "})();</script>");
char s_miles_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_miles("use_miles", "Display distances in miles", "T", 2,
                                   s_miles_checkbox_attrs, WFM_LABEL_AFTER);
char s_iata_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_use_iata(
    "use_iata", "Show IATA airport codes", "T", 2,
    s_iata_checkbox_attrs, WFM_LABEL_AFTER);
char s_route_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_prefer_route(
    "prefer_route", "Show route instead of callsign when available", "T", 2,
    s_route_checkbox_attrs, WFM_LABEL_AFTER);

char s_runways_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_runways("show_runways", "Show airport runways", "T", 2,
                                     s_runways_checkbox_attrs, WFM_LABEL_AFTER);
constexpr char kRangeInputAttrs[] =
    "type=\"range\" min=\"0\" max=\"3\" step=\"1\" "
    "title=\"5 km / 10 km / 15 km / 25 km\" "
    "oninput=\"document.getElementById('range_value').value="
    "['5 km','10 km','15 km','25 km'][this.value]\"";
WiFiManagerParameter s_param_range(
    "range_index", "Radar range (5 / 10 / 15 / 25 km)", "1", 2,
    kRangeInputAttrs);
WiFiManagerParameter s_param_range_break("<br/>");
WiFiManagerParameter s_param_range_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"range_value\">10 km</output></div>"
    "<script>(function(){var s=document.getElementById('range_index'),"
    "o=document.getElementById('range_value'),v=['5 km','10 km','15 km','25 km'];"
    "if(s&&o)o.value=v[s.value]||'10 km';})();</script>");

char s_footer_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_footer("show_footer", "Show weather and clock", "T",
                                    2, s_footer_checkbox_attrs,
                                    WFM_LABEL_AFTER);

char s_weather_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_weather(
    "show_weather", "Show current weather", "T", 2,
    s_weather_checkbox_attrs, WFM_LABEL_AFTER);

char s_fahrenheit_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_fahrenheit(
    "temp_f", "Temperature in Fahrenheit", "T", 2,
    s_fahrenheit_checkbox_attrs, WFM_LABEL_AFTER);

char s_altitude_metres_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_altitude_metres(
    "alt_m", "Display altitude in metres", "T", 2,
    s_altitude_metres_checkbox_attrs, WFM_LABEL_AFTER);

char s_clock24_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_clock24("clock_24", "Use 24-hour clock", "T", 2,
                                     s_clock24_checkbox_attrs,
                                     WFM_LABEL_AFTER);

WiFiManagerParameter s_param_after_clock_break("<br/>");

constexpr char kTextScaleAttrs[] =
    "type=\"range\" min=\"80\" max=\"130\" step=\"5\" "
    "oninput=\"document.getElementById('text_scale_value').value="
    "this.value+'%'\"";
WiFiManagerParameter s_param_text_scale(
    "text_scale", "Radar text size", "120", kTextScaleParamLen,
    kTextScaleAttrs);
WiFiManagerParameter s_param_text_scale_output(
    "<div style=\"text-align:center;margin-top:-5px\">"
    "<output id=\"text_scale_value\" for=\"text_scale\"></output></div>"
    "<script>(function(){var s=document.getElementById('text_scale'),"
    "o=document.getElementById('text_scale_value');"
      "if(s&&o)o.value=s.value+'%';})();</script>");
char s_night_checkbox_attrs[32] = "type=\"checkbox\"";
WiFiManagerParameter s_param_night_enabled(
    "night_enabled", "Enable night mode", "T", 2,
    s_night_checkbox_attrs, WFM_LABEL_AFTER);
WiFiManagerParameter s_param_night_break("<br/>");
constexpr char kNightTimeAttrs[] = "type=\"time\"";
WiFiManagerParameter s_param_night_start(
    "night_start", "Night mode start", "22:00", 6, kNightTimeAttrs);
WiFiManagerParameter s_param_night_end(
    "night_end", "Night mode end", "07:00", 6, kNightTimeAttrs);

constexpr char kOtaPasswordAttrs[] =
    "type=\"password\" autocomplete=\"new-password\" "
    "placeholder=\"leave blank to keep current\"";
WiFiManagerParameter s_param_ota_password(
    "ota_password", "OTA password (user: admin)", "", kOtaPasswordParamLen,
    kOtaPasswordAttrs);

void refreshCheckboxAttrs(char* attrs, size_t attrs_len, bool checked) {
  snprintf(attrs, attrs_len, "type=\"checkbox\"%s",
           checked ? " checked" : "");
}

void refreshPortalParamDefaults() {
  char lat_buf[kCoordParamLen + 1];
  char lon_buf[kCoordParamLen + 1];
  snprintf(lat_buf, sizeof(lat_buf), "%.6f", services::location::lat());
  snprintf(lon_buf, sizeof(lon_buf), "%.6f", services::location::lon());
  s_param_lat.setValue(lat_buf, kCoordParamLen);
  s_param_lon.setValue(lon_buf, kCoordParamLen);
  refreshCheckboxAttrs(s_miles_checkbox_attrs,
                       sizeof(s_miles_checkbox_attrs),
                       ui::radar::useMiles());
  s_param_miles.setValue("T", 2);
  refreshCheckboxAttrs(s_iata_checkbox_attrs, sizeof(s_iata_checkbox_attrs),
                       services::settings::useIataCodes());
  s_param_use_iata.setValue("T", 2);
  refreshCheckboxAttrs(s_route_checkbox_attrs, sizeof(s_route_checkbox_attrs),
                        services::settings::preferRoute());
  s_param_prefer_route.setValue("T", 2);
  refreshCheckboxAttrs(s_runways_checkbox_attrs,
                       sizeof(s_runways_checkbox_attrs),
                       ui::radar::showRunways());
  s_param_runways.setValue("T", 2);
  char range_buf[3];
  snprintf(range_buf, sizeof(range_buf), "%u",
           static_cast<unsigned>(ui::radar::rangeIndex()));
  s_param_range.setValue(range_buf, 2);
  refreshCheckboxAttrs(s_footer_checkbox_attrs,
                       sizeof(s_footer_checkbox_attrs),
                       services::settings::footerEnabled());
  s_param_footer.setValue("T", 2);
  refreshCheckboxAttrs(s_weather_checkbox_attrs,
                       sizeof(s_weather_checkbox_attrs),
                       services::settings::weatherEnabled());
  s_param_weather.setValue("T", 2);
  refreshCheckboxAttrs(s_fahrenheit_checkbox_attrs,
                       sizeof(s_fahrenheit_checkbox_attrs),
                       services::settings::temperatureFahrenheit());
  s_param_fahrenheit.setValue("T", 2);
  refreshCheckboxAttrs(s_altitude_metres_checkbox_attrs,
                     sizeof(s_altitude_metres_checkbox_attrs),
                     services::settings::altitudeMetres());
  s_param_altitude_metres.setValue("T", 2);
  refreshCheckboxAttrs(s_clock24_checkbox_attrs,
                       sizeof(s_clock24_checkbox_attrs),
                       services::settings::use24HourClock());
  s_param_clock24.setValue("T", 2);
  char text_scale_buf[kTextScaleParamLen + 1];
  snprintf(text_scale_buf, sizeof(text_scale_buf), "%d",
           services::settings::textScalePercent());
  s_param_text_scale.setValue(text_scale_buf, kTextScaleParamLen);
  refreshCheckboxAttrs(s_night_checkbox_attrs, sizeof(s_night_checkbox_attrs),
                       services::settings::nightModeEnabled());
  s_param_night_enabled.setValue("T", 2);
  char night_start_buf[6];
  char night_end_buf[6];
  snprintf(night_start_buf, sizeof(night_start_buf), "%02u:%02u",
           services::settings::nightStartMinute() / 60,
           services::settings::nightStartMinute() % 60);
  snprintf(night_end_buf, sizeof(night_end_buf), "%02u:%02u",
           services::settings::nightEndMinute() / 60,
           services::settings::nightEndMinute() % 60);
  s_param_night_start.setValue(night_start_buf, 6);
  s_param_night_end.setValue(night_end_buf, 6);
  s_param_ota_password.setValue("", kOtaPasswordParamLen);

}

void appendPortalParameter(PortalHtml& page, const WiFiManagerParameter& parameter) {
  const char* id = parameter.getID();
  if (id == nullptr) {
    // Custom blocks and attributes are trusted firmware constants, not input.
    page.append(parameter.getCustomHTML());
    return;
  }
  const auto label = [&page, &parameter, id]() {
    page.append("<label for='");
    page.escaped(id);
    page.append("'>");
    page.escaped(parameter.getLabel());
    page.append("</label>");
  };
  if (parameter.getLabelPlacement() == WFM_LABEL_BEFORE) {
    label();
  }
  page.append("<br><input id='");
  page.escaped(id);
  page.append("' name='");
  page.escaped(id);
  char length[12];
  snprintf(length, sizeof(length), "%d", parameter.getValueLength());
  page.append("' maxlength='");
  page.escaped(length);
  page.append("' value='");
  page.escaped(parameter.getValue());
  page.append("' ");
  page.append(parameter.getCustomHTML());
  page.append(">\n");
  if (parameter.getLabelPlacement() == WFM_LABEL_AFTER) {
    label();
  }
}

void handleCommonSetupPage() {
  if (!s_wm.server) {
    return;
  }
  WebServer& web = *s_wm.server;
  refreshPortalParamDefaults();
  WiFiManagerParameter** parameters = s_wm.getParameters();
  const int count = s_wm.getParametersCount();
  if (parameters == nullptr || count == 0) {
    web.send(503, "text/plain", "Setup parameters unavailable. Nothing saved.");
    return;
  }
  for (int i = 0; i < count; ++i) {
    if (parameters[i] == nullptr) {
      web.send(503, "text/plain", "Setup parameters unavailable. Nothing saved.");
      return;
    }
  }

  startPortalPage(web);
  PortalChunkSink sink{web};
  PortalHtml page(sink);
  page.append("<!doctype html><html lang='en'><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Common Setup</title>");
  page.append(kPortalHead);
  page.append("</head><body class='common-setup'><div class='wrap'><h1>Common Setup</h1>"
      "<form method='POST' action='/paramsave'>");
  for (int i = 0; i < count; ++i) {
    appendPortalParameter(page, *parameters[i]);
  }
  // Last control: a browser cannot submit a normally truncated form as valid.
  page.append("<input type='hidden' name='_form_complete' value='common-v2'>"
      "<br><br><button type='submit'>Save</button></form></div></body></html>");
  finishPortalPage(page);
}

void onPortalParamsSaved() {
  if (!services::location::saveFromStrings(s_param_lat.getValue(),
                                           s_param_lon.getValue())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(s_param_miles.getValue());
  ui::radar::saveRunwaysFromPortal(s_param_runways.getValue());
  ui::radar::saveRangeFromPortal(s_param_range.getValue());
  services::settings::saveFromPortal(
    s_param_footer.getValue(), s_param_weather.getValue(),
    s_param_fahrenheit.getValue(),
    s_param_altitude_metres.getValue(),
    s_param_clock24.getValue(),
    s_param_text_scale.getValue(),
    s_param_ota_password.getValue(),
    s_param_night_enabled.getValue(),
    s_param_night_start.getValue(), s_param_night_end.getValue(),
    s_param_use_iata.getValue(), s_param_prefer_route.getValue());
}

void savePortalParamsFromRequest(WebServer& web) {
  const String latitude = web.arg("radar_lat");
  const String longitude = web.arg("radar_lon");
  const String miles = web.arg("use_miles");
  const String runways = web.arg("show_runways");
  const String range_index = web.arg("range_index");
  const String footer = web.arg("show_footer");
  const String weather = web.arg("show_weather");
  const String fahrenheit = web.arg("temp_f");
  const String altitude_metres = web.arg("alt_m");
  const String clock24 = web.arg("clock_24");
  const String text_scale = web.arg("text_scale");
  const String ota_password = web.arg("ota_password");
  const String night_enabled = web.arg("night_enabled");
  const String night_start = web.arg("night_start");
  const String night_end = web.arg("night_end");
  const String use_iata = web.arg("use_iata");
  const String prefer_route = web.arg("prefer_route");
  if (!services::location::saveFromStrings(latitude.c_str(),
                                           longitude.c_str())) {
    Serial.println("Invalid lat/lon in portal — keeping previous location");
  }
  ui::radar::saveMilesFromPortal(miles.c_str());
  ui::radar::saveRunwaysFromPortal(runways.c_str());
  ui::radar::saveRangeFromPortal(range_index.c_str());
  services::settings::saveFromPortal(
    footer.c_str(), weather.c_str(), fahrenheit.c_str(),
    altitude_metres.c_str(), clock24.c_str(),
    text_scale.c_str(), ota_password.c_str(),
    night_enabled.c_str(), night_start.c_str(), night_end.c_str(),
    use_iata.c_str(), prefer_route.c_str());
  refreshPortalParamDefaults();
}

void handleSettingsSaved() {
  if (!s_wm.server) {
    return;
  }

  WebServer& web = *s_wm.server;
  if (web.method() != HTTP_POST) {
    web.sendHeader("Allow", "POST");
    web.send(405, "text/plain", "Use POST to save settings.");
    return;
  }
  const String marker = web.arg(services::portal::kFormCompleteField);
  if (!services::portal::completeCommonForm(marker.c_str(), [&web](const char* name) {
        // A blank OTA password is valid and means keep the stored secret.
        return web.hasArg(name) &&
            (strcmp(name, "ota_password") == 0 || web.arg(name).length() != 0);
      })) {
    rejectIncompleteForm(web);
    return;
  }
  savePortalParamsFromRequest(web);
  web.send(
      200, "text/html",
      "<!doctype html><html lang='en'><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<meta http-equiv='refresh' content='3;url=/param'>"
      "<title>Setup saved</title>"
       "<style>body{font-family:Segoe UI,Arial,sans-serif;text-align:center;"
       "background:#0d151e;color:#d7e0e9;margin:0;padding:3rem}"
       ".msg{display:inline-block;min-width:16rem;text-align:left;padding:1.5rem;"
       "background:#141f2a;border:1px solid #2a3a49;border-left:4px solid #62899d;"
       "border-radius:12px;box-shadow:0 12px 32px #0005}a{color:#8eb5c5}"
       ".home-link{display:inline-block;margin:0 0 16px;padding:8px 13px;"
       "background:#38596b;color:#eef5f8;border:1px solid #5e8191;"
       "border-radius:6px;text-decoration:none;font-weight:600}</style></head><body>"
       "<div class='msg'><strong>Saved</strong><br>"
       "<small>Returning to Setup in 3 seconds...</small><br><br>"
       "<a class='home-link' href='/'>Home</a></div></body></html>");
}

void attachSettingsRoutes() {
  if (!s_wm.server) {
    return;
  }
  // Browsers request this automatically when opening the portal. Returning
  // an empty response avoids a misleading "handler not found" log entry.
  s_wm.server->on("/favicon.ico", HTTP_GET, []() {
    s_wm.server->send(204, "text/plain", "");
  });
  s_wm.server->on("/portal.css", HTTP_GET, []() {
    servePortalAsset("text/css; charset=utf-8", kPortalCss, sizeof(kPortalCss) - 1);
  });
  s_wm.server->on("/portal.js", HTTP_GET, []() {
    servePortalAsset("application/javascript; charset=utf-8", kPortalJs,
                     sizeof(kPortalJs) - 1);
  });
  s_wm.server->on("/display", HTTP_GET, handleDisplayPage);
  s_wm.server->on("/display.js", HTTP_GET, []() {
    servePortalAsset("application/javascript; charset=utf-8", web::display::kScript,
                     sizeof(web::display::kScript) - 1);
  });
  s_wm.server->on("/aircraft.json", HTTP_GET, handleAircraftSnapshot);
  s_wm.server->on("/display.bmp", HTTP_GET, handleDisplayBmp);
  s_wm.server->on("/airports", HTTP_GET, handleAirportSearch);
  s_wm.server->on("/airport", HTTP_GET, handleAirportLookup);
  s_wm.server->on("/location-default", HTTP_POST, handleLocationDefault);
  s_wm.server->on("/visibility", HTTP_GET, handleVisibility);
  s_wm.server->on("/component", HTTP_GET, handleComponentPage);
  s_wm.server->on("/component-save", HTTP_POST, handleComponentSave);
  s_wm.server->on("/", HTTP_GET, handleHomePage);
  s_wm.server->on("/wifi", HTTP_ANY, []() { handleWifiPage(true); });
  s_wm.server->on("/0wifi", HTTP_GET, []() { handleWifiPage(false); });
  s_wm.server->on("/info", HTTP_GET, handleInfoPage);
  // Claim the large common form before WiFiManager's String-based renderer.
  s_wm.server->on("/param", HTTP_GET, handleCommonSetupPage);
  // Register before WiFiManager's built-in /paramsave handler so the custom
  // confirmation can redirect back to Setup.
  s_wm.server->on("/paramsave", HTTP_ANY, handleSettingsSaved);
}

void attachPortalParams(WiFiManager& wm) {
  refreshPortalParamDefaults();
  wm.addParameter(&s_param_lat);
  wm.addParameter(&s_param_lon);
  wm.addParameter(&s_param_location_selector);
  wm.addParameter(&s_param_miles);
  wm.addParameter(&s_param_use_iata);
  wm.addParameter(&s_param_prefer_route);
  wm.addParameter(&s_param_runways);
  wm.addParameter(&s_param_range_break);
  wm.addParameter(&s_param_range);
  wm.addParameter(&s_param_range_output);
  wm.addParameter(&s_param_footer);
  wm.addParameter(&s_param_weather);
  wm.addParameter(&s_param_fahrenheit);
  wm.addParameter(&s_param_altitude_metres);
  wm.addParameter(&s_param_clock24);
  wm.addParameter(&s_param_after_clock_break);
  wm.addParameter(&s_param_text_scale);
  wm.addParameter(&s_param_text_scale_output);
  wm.addParameter(&s_param_night_enabled);
  wm.addParameter(&s_param_night_break);
  wm.addParameter(&s_param_night_start);
  wm.addParameter(&s_param_night_end);
  wm.addParameter(&s_param_ota_password);
  Serial.printf("Setup parameters registered: %d\n", wm.getParametersCount());
  wm.setSaveParamsCallback(onPortalParamsSaved);
}

void markForceConfigPortal() {
  s_force_config_portal = true;
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, false)) {
    return;
  }
  prefs.putBool(kPrefsForcePortalKey, true);
  prefs.end();
}

bool consumeForceConfigPortal() {
  if (s_force_config_portal) {
    s_force_config_portal = false;
    Preferences prefs;
    if (prefs.begin(kWifiPrefsNamespace, false)) {
      prefs.remove(kPrefsForcePortalKey);
      prefs.end();
    }
    return true;
  }

  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  if (!pending) {
    return false;
  }

  if (prefs.begin(kWifiPrefsNamespace, false)) {
    prefs.remove(kPrefsForcePortalKey);
    prefs.end();
  }
  return true;
}

bool storedWifiCredentials() {
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
    WiFi.mode(WIFI_STA);
    delay(50);
  }

  wifi_config_t conf = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK) {
    return false;
  }
  return conf.sta.ssid[0] != '\0';
}

void eraseWifiCredentials() {
  stopLanWebPortal();
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(100);

  ensureWifiManager();
  WiFi.persistent(true);
  s_wm.resetSettings();
  s_wm.erase();
  WiFi.disconnect(true, true);
  WiFi.persistent(false);

  WiFi.mode(WIFI_OFF);
  delay(100);
}

void resetWifiCredentials() {
  markForceConfigPortal();
  eraseWifiCredentials();
  services::location::clear();
  ui::radar::unitsReset();
  services::settings::clear();
  Serial.println("WiFi credentials, location, units, and display settings cleared");
}

void onConfigPortalApStarted(WiFiManager*) {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  statusScreenPortal();
#ifdef WM_MDNS
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("Setup portal: http://%s.local (or http://%s)\n",
                  config::kPortalHostname, config::kPortalIp);
  } else {
    Serial.printf("Setup portal: http://%s (mDNS unavailable)\n", config::kPortalIp);
  }
#else
  Serial.printf("Setup portal: http://%s\n", config::kPortalIp);
#endif
}

bool wifiLinkUp() {
  return WiFi.status() == WL_CONNECTED &&
         WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

void ensureWifiManager() {
  if (s_wm_configured) {
    return;
  }
  s_wm.setConfigPortalTimeout(config::kWifiPortalTimeoutSec);
  s_wm.setAPStaticIPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0));
  s_wm.setHostname(config::kPortalHostname);
  s_wm.setCustomHeadElement(kPortalHead);
  s_wm.setParamsPage(true); // Custom settings live on the streamed /param page.
  s_wm.setTitle("Plane Radar");
  s_wm.setAPCallback(onConfigPortalApStarted);
  attachPortalParams(s_wm);
  services::ota::configure(s_wm, attachSettingsRoutes);
  s_wm_configured = true;
}

void startLanWebPortal() {
  if (!wifiLinkUp() || s_wm.getWebPortalActive() ||
      s_wm.getConfigPortalActive()) {
    return;
  }
  refreshPortalParamDefaults();
  WiFi.mode(WIFI_STA);
  s_wm.setConfigPortalBlocking(false);
#ifdef WM_MDNS
  MDNS.end();
  if (MDNS.begin(config::kPortalHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
#endif
  s_wm.startWebPortal();
  Serial.printf("LAN config: http://%s.local or http://%s\n",
                config::kPortalHostname, WiFi.localIP().toString().c_str());
}

void stopLanWebPortal() {
  if (!s_wm.getWebPortalActive()) {
    return;
  }
  s_wm.stopWebPortal();
#ifdef WM_MDNS
  MDNS.end();
#endif
}

void prepareSta() {
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(WIFI_PS_NONE);
  WiFi.setAutoReconnect(true);
}

void startStaConnect(const String& ssid, const String& pass) {
  prepareSta();
  if (ssid.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin();
  }
}

bool waitForLinkWithUi(const char* ssid_for_ui, unsigned long attempt_ms) {
  const unsigned long deadline = millis() + attempt_ms;
  while (millis() < deadline) {
    if (wifiLinkUp()) {
      return true;
    }
    bootButtonPollLongPress();
    statusScreenConnectingTick();
    delay(config::kWifiConnectingFrameMs);
  }
  return wifiLinkUp();
}

bool tryConnectWithUi(const String& ssid, const String& pass, bool show_ui) {
  if (wifiLinkUp()) {
    return true;
  }

  const char* ui_ssid = ssid.length() > 0 ? ssid.c_str() : "network";
  if (show_ui) {
    statusScreenConnectingBegin(ui_ssid);
  }

  for (uint8_t attempt = 1; attempt <= config::kWifiConnectAttempts; ++attempt) {
    if (attempt > 1) {
      Serial.printf("WiFi connect retry %u/%u\n", attempt,
                    config::kWifiConnectAttempts);
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      delay(400);
    }

    startStaConnect(ssid, pass);

    if (waitForLinkWithUi(ui_ssid, config::kWifiConnectAttemptMs)) {
      return true;
    }
  }

  return false;
}

bool connectFallbackNetwork(bool show_ui) {
  const String ssid = config::kWifiFallbackSSID;

  if (ssid.length() == 0) {
    return false;
  }

  Serial.printf("Trying compiled fallback WiFi: %s\n", ssid.c_str());

  return tryConnectWithUi(ssid, config::kWifiFallbackPass, show_ui);
}

bool connectSavedNetwork(bool show_ui) {
  if (!storedWifiCredentials()) {
    return false;
  }

  ensureWifiManager();
  const String ssid = s_wm.getWiFiSSID();
  if (ssid.length() == 0) {
    return false;
  }
  const String pass = s_wm.getWiFiPass();
  return tryConnectWithUi(ssid, pass, show_ui);
}

bool openConfigPortal() {
  stopLanWebPortal();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  statusScreenPortal();
  s_wm.setConfigPortalBlocking(false);
  s_wm.startConfigPortal(config::kPortalApName);
  while (s_wm.getConfigPortalActive()) {
    bootButtonPollLongPress();
    if (s_wm.process()) {
      return true;
    }
    delay(10);
  }
  return wifiLinkUp();
}

}  // namespace

bool wifiShowsSetupScreenOnBoot() {
  if (s_force_config_portal) {
    return true;
  }
  Preferences prefs;
  if (!prefs.begin(kWifiPrefsNamespace, true)) {
    return false;
  }
  const bool pending = prefs.getBool(kPrefsForcePortalKey, false);
  prefs.end();
  return pending;
}

bool wifiBootButtonPressed() {
  return digitalRead(config::kBootPin) == LOW;
}

void bootButtonInit() { initBootButton(); }

bool bootButtonConsumeTap() {
  portENTER_CRITICAL(&s_boot_mux);
  const bool tap = s_boot_tap_pending;
  if (tap) {
    s_boot_tap_pending = false;
  }
  portEXIT_CRITICAL(&s_boot_mux);
  return tap;
}

void bootButtonPollLongPress() {
  if (wifiBootButtonPressed()) {
    portENTER_CRITICAL(&s_boot_mux);
    if (!s_boot_is_down) {
      s_boot_is_down = true;
      s_boot_down_ms = millis();
    }
    const unsigned long down_ms = s_boot_down_ms;
    portEXIT_CRITICAL(&s_boot_mux);

    if (!s_long_press_handled &&
        millis() - down_ms >= config::kBootResetHoldMs) {
      s_long_press_handled = true;
      Serial.println("BOOT held — resetting WiFi");
      wifiResetCredentialsAndReboot();
    }
  } else {
    portENTER_CRITICAL(&s_boot_mux);
    s_boot_is_down = false;
    portEXIT_CRITICAL(&s_boot_mux);
    s_long_press_handled = false;
  }
}

void wifiResetCredentialsAndReboot() {
  resetWifiCredentials();
  statusScreenWifiReset();
  delay(800);
  esp_restart();
}

bool wifiReconnect() {
  initBootButton();
  Serial.println("WiFi reconnecting...");
  return connectSavedNetwork(true) || connectFallbackNetwork(true);
}

void wifiLoop() {
  ensureWifiManager();
  if (wifiLinkUp()) {
    if (!s_wm.getWebPortalActive() && !s_wm.getConfigPortalActive()) {
      startLanWebPortal();
    }
    if (s_wm.getWebPortalActive() || s_wm.getConfigPortalActive()) {
      bootButtonPollLongPress();
      s_wm.process();
    }
  } else {
    stopLanWebPortal();
  }
}

bool wifiSetupConnect() {
  initBootButton();
  ensureWifiManager();

  const bool force_portal = consumeForceConfigPortal();
  WiFi.setAutoReconnect(false);

  if (force_portal) {
    eraseWifiCredentials();
    WiFi.mode(WIFI_OFF);
    delay(100);
  }

  if (force_portal) {
    Serial.println("Opening WiFi setup portal (after reset)");
    if (openConfigPortal() && wifiLinkUp()) {
      WiFi.setAutoReconnect(true);
      Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str());
      return true;
    }
    Serial.println("WiFi connection failed");
    statusScreenConnectFailed();
    return false;
  }

  Serial.println("Connecting to WiFi (portal opens if needed)...");

  if (wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials() && connectSavedNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (connectFallbackNetwork(true)) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected with fallback WiFi: %s  IP %s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  if (storedWifiCredentials()) {
    Serial.println("Saved and fallback WiFi could not connect — opening setup portal");
  } else {
    Serial.println("No saved or fallback WiFi — opening setup portal");
  }

  if (openConfigPortal() && wifiLinkUp()) {
    WiFi.setAutoReconnect(true);
    Serial.printf("Connected: %s  IP %s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("WiFi connection failed");
  statusScreenConnectFailed();
  return false;
}
