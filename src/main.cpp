/**
 * Plane Radar — WiFi setup, then radar UI on the round GC9A01 display.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "hardware/display.h"
#include "services/adsb_client.h"
#include "services/display_settings.h"
#include "services/ota_update.h"
#include "services/radar_location.h"
#include "services/weather_time.h"
#include "services/wifi_setup.h"
#include "ui/radar_display.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool g_radar_visible = false;
unsigned long g_wifi_down_since = 0;
unsigned long g_last_reconnect_ms = 0;
unsigned long g_last_adsb_fetch_ms = 0;
unsigned long g_last_diagnostic_ms = 0;
unsigned long g_critical_heap_since_ms = 0;

void printRuntimeDiagnostics() {
  const wl_status_t wifi_status = WiFi.status();
  const int rssi = wifi_status == WL_CONNECTED ? WiFi.RSSI() : 0;
  Serial.printf("diag: uptime=%lus heap=%u min_heap=%u largest=%u largest_internal=%u wifi=%d rssi=%d radar=%d\n",
                millis() / 1000UL, static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                static_cast<int>(wifi_status), rssi, g_radar_visible ? 1 : 0);
}

void initRuntimeWatchdog() {
  const esp_err_t init_result =
      esp_task_wdt_init(config::kTaskWatchdogTimeoutSec, true);
  if (init_result != ESP_OK && init_result != ESP_ERR_INVALID_STATE) {
    Serial.printf("watchdog: init failed: %s\n", esp_err_to_name(init_result));
    return;
  }

  const esp_err_t status = esp_task_wdt_status(nullptr);
  if (status == ESP_ERR_NOT_FOUND) {
    const esp_err_t add_result = esp_task_wdt_add(nullptr);
    if (add_result != ESP_OK) {
      Serial.printf("watchdog: subscribe failed: %s\n",
                    esp_err_to_name(add_result));
      return;
    }
  }
  Serial.printf("watchdog: enabled, timeout=%lus\n",
                static_cast<unsigned long>(config::kTaskWatchdogTimeoutSec));
}

void monitorCriticalHeap() {
  const uint32_t free_heap = ESP.getFreeHeap();
  const size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const bool critical = free_heap < config::kCriticalFreeHeap ||
                        largest_block < config::kCriticalLargestBlock;
  if (!critical) {
    g_critical_heap_since_ms = 0;
    return;
  }

  if (g_critical_heap_since_ms == 0) {
    g_critical_heap_since_ms = millis();
    Serial.printf("watchdog: critical heap, heap=%u largest=%u\n",
                  static_cast<unsigned>(free_heap),
                  static_cast<unsigned>(largest_block));
    return;
  }

  if (millis() - g_critical_heap_since_ms <
      config::kCriticalHeapDurationMs) {
    return;
  }

  Serial.printf("watchdog: restarting after critical heap, heap=%u largest=%u\n",
                static_cast<unsigned>(free_heap),
                static_cast<unsigned>(largest_block));
  Serial.flush();
  ESP.restart();
}

void showRadarIfConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    g_radar_visible = false;
    return;
  }
  services::weather::begin();
  ui::radarDisplayDraw();
  g_radar_visible = true;
}

void onRangeTap() {
  ui::radar::rangeNext();
  char range_label[12];
  ui::radar::formatCurrentRing3Label(range_label, sizeof(range_label));
  Serial.printf("Range: %s (outer ~%.0f km)\n", range_label,
                ui::radar::rangeCurrent().outer_km);

  if (g_radar_visible && WiFi.status() == WL_CONNECTED) {
    ui::radarDisplayDraw();
  }
}

void handleBootButton() {
  bootButtonPollLongPress();
  if (bootButtonConsumeTap()) {
    onRangeTap();
  }
}

void fetchAndDrawAircraft() {
  const float fetch_km = ui::radar::fetchRadiusKm();
  if (!services::adsb::fetchUpdate(services::location::lat(),
                                   services::location::lon(), fetch_km)) {
    handleBootButton();
    return;
  }
  ui::radarDisplayRefreshAircraft();
  handleBootButton();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Plane Radar");

  bootButtonInit();
  displayInit();
  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  services::location::init();
  ui::radar::rangeInit();
  services::settings::init();
  services::adsb::setPollFn(wifiLoop);
  services::weather::setPollFn(wifiLoop);

  if (wifiSetupConnect()) {
    showRadarIfConnected();
  }
  // WiFiManager may block during the initial connection or configuration
  // portal. Start the runtime watchdog only after that startup phase.
  initRuntimeWatchdog();
}

void loop() {
  esp_task_wdt_reset();
  handleBootButton();
  wifiLoop();
  monitorCriticalHeap();
  if (millis() - g_last_diagnostic_ms >= 10000UL) {
    g_last_diagnostic_ms = millis();
    printRuntimeDiagnostics();
  }
  if (g_radar_visible) {
    ui::radarDisplayTick();
  }

  if (services::ota::inProgress()) {
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (g_radar_visible) {
      Serial.println("WiFi lost — will reconnect");
      g_radar_visible = false;
    }

    if (g_wifi_down_since == 0) {
      g_wifi_down_since = millis();
    }

    const unsigned long down_ms = millis() - g_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        millis() - g_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      g_last_reconnect_ms = millis();
      if (wifiReconnect()) {
        g_wifi_down_since = 0;
        showRadarIfConnected();
      }
    }
  } else {
    g_wifi_down_since = 0;
    if (!g_radar_visible) {
      showRadarIfConnected();
    } else if (millis() - g_last_adsb_fetch_ms >= config::kAdsbFetchIntervalMs) {
      g_last_adsb_fetch_ms = millis();
      fetchAndDrawAircraft();
    } else if (services::weather::refreshIfDue(
                   services::location::lat(), services::location::lon())) {
      ui::radarDisplayRefreshAircraft();
    } else if (services::adsb::enrichmentAllowed() &&
               services::adsb::enrichOnePending()) {
      ui::radarDisplayRefreshAircraft();
    }
  }

  delay(10);
}
