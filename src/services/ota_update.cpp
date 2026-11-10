#include "services/ota_update.h"

#include <Update.h>
#include <WiFiManager.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "services/display_settings.h"
#include "ui/status_screens.h"

namespace services::ota {
namespace {

constexpr char kFirmwarePage[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Plane Radar firmware</title>
<style>
 body{font-family:Segoe UI,Arial,sans-serif;background:#0d151e;color:#d7e0e9;margin:0}
 main{max-width:34rem;margin:3rem auto;padding:1.5rem}
 .card{background:#141f2a;border:1px solid #2a3a49;border-radius:12px;padding:1.5rem;box-shadow:0 12px 32px #0005}
 input,button{box-sizing:border-box;width:100%;margin-top:1rem;padding:.8rem;background:#0f1923;color:#e4edf4;border:1px solid #35495b;border-radius:6px}
 button{background:#38596b;color:#eef5f8;border-color:#5e8191;font:inherit;font-weight:600;cursor:pointer}
 button:hover{background:#486f80;border-color:#83a8b7}
  small{color:#9aaabd}a{color:#8eb5c5}
  .home-link{display:inline-block;padding:.8rem 1rem;background:#38596b;color:#eef5f8;border:1px solid #5e8191;border-radius:6px;text-decoration:none;font-weight:600}
  .home-link:hover{background:#486f80;border-color:#83a8b7}
</style></head><body><main><div class="card">
<h1>Firmware update</h1>
<p>Upload the OTA application image ending in <strong>-ota.bin</strong>
or PlatformIO's <strong>firmware.bin</strong>.</p>
<form method="post" action="/firmware-upload" enctype="multipart/form-data">
<input type="file" name="firmware" accept=".bin,application/octet-stream" required>
<button type="submit">Install and restart</button></form>
<p><small>Do not upload the merged/full flash image here. Keep power connected
 until the device restarts.</small></p><p><a class="home-link" href="/">Home</a></p>
</div></main></body></html>
)HTML";

/** Release the radar if an upload stops without reaching the done handler. */
constexpr unsigned long kUploadIdleTimeoutMs = 30000UL;

WiFiManager* s_manager = nullptr;
AdditionalRoutesFn s_additional_routes = nullptr;
bool s_in_progress = false;
bool s_upload_authenticated = false;
unsigned long s_last_upload_activity_ms = 0;
String s_upload_error;

void abortUpload(const char* reason) {
  if (Update.isRunning()) {
    Update.abort();
  }
  if (s_in_progress) {
    Serial.printf("OTA aborted: %s\n", reason);
  }
  s_in_progress = false;
}

WebServer* server() {
  if (s_manager == nullptr || !s_manager->server) {
    return nullptr;
  }
  return s_manager->server.get();
}

bool requestAuthenticated() {
  WebServer* web = server();
  if (web == nullptr) {
    return false;
  }
  if (web->authenticate(config::kOtaUsername, settings::otaPassword())) {
    return true;
  }
  web->requestAuthentication();
  return false;
}

void showFirmwarePage() {
  WebServer* web = server();
  if (web == nullptr || !requestAuthenticated()) {
    return;
  }
  web->send_P(200, PSTR("text/html"), kFirmwarePage);
}

void recordUpdateError() {
  s_upload_error = Update.errorString();
  if (s_upload_error.length() == 0) {
    s_upload_error = "Unknown flash error";
  }
  Update.end();
}

void handleUploadChunk() {
  WebServer* web = server();
  if (web == nullptr) {
    return;
  }
  HTTPUpload& upload = web->upload();
  // The whole multipart upload is parsed inside one handleClient() call, so
  // loop() cannot feed the runtime watchdog while a large image is written.
  esp_task_wdt_reset();
  s_last_upload_activity_ms = millis();

  // WebServer does not call the done handler after an aborted upload. Handle
  // it before the auth/error guard so the radar loop is always released.
  if (upload.status == UPLOAD_FILE_ABORTED) {
    if (s_upload_authenticated && s_upload_error.length() == 0) {
      s_upload_error = "Upload aborted";
    }
    abortUpload("client disconnected");
    return;
  }

  if (upload.status == UPLOAD_FILE_START) {
    s_upload_authenticated =
        web->authenticate(config::kOtaUsername, settings::otaPassword());
    s_upload_error.clear();
    if (!s_upload_authenticated) {
      return;
    }
    s_in_progress = true;
    if (!upload.filename.endsWith(".bin")) {
      s_upload_error = "Please select an OTA .bin application image";
      return;
    }

    statusScreenFirmwareUpdate();
    Serial.printf("OTA: receiving %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      recordUpdateError();
    }
    return;
  }

  if (!s_upload_authenticated || s_upload_error.length() != 0) {
    return;
  }

  if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      recordUpdateError();
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      recordUpdateError();
    } else {
      Serial.printf("OTA: wrote %u bytes\n", upload.totalSize);
    }
  }
}

void handleUploadDone() {
  WebServer* web = server();
  if (web == nullptr) {
    return;
  }
  if (!s_upload_authenticated) {
    s_in_progress = false;
    web->requestAuthentication();
    return;
  }

  if (s_upload_error.length() != 0 || Update.hasError()) {
    s_in_progress = false;
    const String message =
        String("<!doctype html><meta name=viewport content='width=device-width'>"
               "<h1>Update failed</h1><p>") +
        (s_upload_error.length() != 0 ? s_upload_error : Update.errorString()) +
        "</p><p><a href='/firmware'>Try again</a></p>";
    web->send(500, "text/html", message);
    Serial.printf("OTA failed: %s\n", s_upload_error.c_str());
    return;
  }

  web->client().setNoDelay(true);
  web->send(
      200, "text/html",
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<h1>Update installed</h1><p>Plane Radar is restarting...</p>");
  delay(250);
  web->client().stop();
  ESP.restart();
}

void attachRoutes() {
  WebServer* web = server();
  if (web == nullptr) {
    return;
  }

  web->on("/firmware", HTTP_GET, showFirmwarePage);
  web->on("/firmware-upload", HTTP_POST, handleUploadDone,
          handleUploadChunk);

  // Claim WiFiManager's legacy unauthenticated paths before it registers them.
  web->on("/update", HTTP_GET, showFirmwarePage);
  web->on("/u", HTTP_POST, handleUploadDone, handleUploadChunk);

  if (s_additional_routes != nullptr) {
    s_additional_routes();
  }
}

}  // namespace

void configure(WiFiManager& manager, AdditionalRoutesFn additional_routes) {
  s_manager = &manager;
  s_additional_routes = additional_routes;
  manager.setShowInfoUpdate(false);
  manager.setCustomMenuHTML(
      "<form action='/firmware' method='get'><button>Firmware update</button>"
      "</form><br/>\n");
  const char* menu[] = {"wifi", "param", "info", "custom",
                        "sep",  "restart", "exit"};
  manager.setMenu(menu, sizeof(menu) / sizeof(menu[0]));
  manager.setWebServerCallback(attachRoutes);
}

bool authorizeSettingsRequest() {
  // The setup access point is the first-run/reset path with physical access;
  // only the home-network portal requires credentials for settings.
  if (s_manager != nullptr && s_manager->getConfigPortalActive()) {
    return true;
  }
  return requestAuthenticated();
}

bool inProgress() {
  if (s_in_progress &&
      millis() - s_last_upload_activity_ms >= kUploadIdleTimeoutMs) {
    abortUpload("no upload data for 30 s");
  }
  return s_in_progress;
}

}  // namespace services::ota
