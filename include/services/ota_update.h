#pragma once

class WiFiManager;

namespace services::ota {

using AdditionalRoutesFn = void (*)();

/**
 * Add an authenticated firmware page to the WiFiManager portal. The optional
 * callback runs while the portal server is being created, before WiFiManager
 * installs its default routes.
 */
void configure(WiFiManager& manager,
               AdditionalRoutesFn additional_routes = nullptr);

/**
 * Guard for settings-changing portal routes. Always allowed while the setup
 * access point is active; on the home network it requires the OTA user and
 * password and sends a 401 challenge otherwise. Returns true when the handler
 * may continue.
 */
bool authorizeSettingsRequest();

/** True while an OTA upload is actively writing flash. */
bool inProgress();

}  // namespace services::ota
