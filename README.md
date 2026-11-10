# Plane Radar

<img width="600" alt="Plane Radar device with round radar display" src="docs/images/plane-radar-device.png" />

Firmware for an **ESP32-C3 Super Mini** with a **1.28-inch round GC9A01 display** at 240 x 240 pixels. The device renders a live ADS-B radar around a configurable location and provides airport/runway data, routes, aircraft labels, weather, time, browser configuration, OTA updates, and a browser-based simulator.

**3D-printed case:** [MakerWorld](https://makerworld.com/de/models/3039652-esp32-plane-radar#profileId-3417884)
**Firmware releases:** [GitHub Releases](../../releases)

## Features

- Live nearby aircraft from [adsb.fi](https://opendata.adsb.fi/)
- Circular radar display with configurable 5, 10, 15, and 25 km range presets
- Aircraft heading triangles, speed vectors, altitude, callsign, route, and compact aircraft type labels
- Continuous display-only aircraft motion between ADS-B responses, with labels attached directly behind the heading triangles
- Persistent route/callsign preference and a live ADS-B table below the WebDisplay
- Direction markers for aircraft outside the display range but inside the ADS-B query radius
- Airport and runway overlay using embedded OurAirports data
- ICAO or IATA airport labels and airport selection by ICAO/IATA code
- Motorway, primary-road, city, water, grid, and aircraft map layers
- Local weather, humidity, time, date, temperature units, altitude units, and text scaling
- Configurable colors and layer visibility through the separate `Component Setup` page
- Authenticated OTA firmware updates
- Captive portal and LAN configuration through WiFiManager
- Runtime diagnostics, heap monitoring, temporary enrichment backoff, and task watchdog protection
- Browser-based simulator using the same airport, runway, and map data as the firmware

## Hardware

| Part | Specification |
|------|---------------|
| Controller | ESP32-C3 Super Mini |
| Display | 1.28-inch round GC9A01, 240 x 240 |
| Framework | Arduino via PlatformIO |
| Serial monitor | 115200 baud |

### Wiring

| GC9A01 | ESP32-C3 |
|--------|----------|
| VCC | 3V3 |
| GND | GND |
| RST | GPIO 0 |
| CS | GPIO 1 |
| DC | GPIO 10 |
| SDA / MOSI | GPIO 3 |
| SCL / SCLK | GPIO 4 |
| BOOT button | GPIO 9, active LOW |

The display SPI configuration is defined in `include/config.h`. The current hardware configuration uses 40 MHz SPI, display inversion, and RGB order configured for the GC9A01 module used by this project.

## First Start

If no valid Wi-Fi credentials are stored, the device opens the setup access point:

```text
SSID: PlaneRadar-Setup
URL: http://192.168.4.1
```

The setup screen also advertises the mDNS address:

```text
http://plane-radar.local
```

1. Connect a phone or computer to `PlaneRadar-Setup`.
2. Open `http://192.168.4.1` or `http://plane-radar.local`.
3. Enter the home Wi-Fi credentials.
4. Save the settings.
5. Reconnect the client to the home network.

After connecting to Wi-Fi, the device is available at:

```text
http://plane-radar.local
http://<device-ip>
```

Some clients resolve `.local` addresses slowly. The IP address shown in the serial monitor or router is the fallback.

## Portal Pages

The LAN portal and the setup access point expose the same configuration interface.

On the home network, every page that changes or reveals device settings
(Wi-Fi, Common/Component Setup, location reset, device information, restart,
exit, erase and firmware update) requires the user `admin` and the OTA
password. The default password is `plane-radar`; change it in `Common Setup`
after the first login. The home page, `Display`, the live image/table and the
airport search remain public. While the device runs its own `PlaneRadar-Setup`
access point, the settings pages need no password so first-time setup stays
simple; the firmware update always requires it.

If the setup access point opened only because the saved Wi-Fi was unreachable
(for example, a router outage during boot), the device retries the saved
network after five minutes without a connected setup client.

### Main Page

The main page provides links for:

- Wi-Fi configuration
- Common setup
- Device information
- Firmware update
- Restart
- Exit
- Display preview

The `Display` page shows the current round display and a live ADS-B table below
it. Callsign, route, type, hex ID, altitude, speed, track, distance and position
age come from the already received aircraft list, not a separate external
request or the animated positions. Retained aircraft outside the outer ring
are included (at most 64 rows); missing values appear as a dash.

The page requests `/aircraft.json` and `/display.bmp` sequentially, then waits
five seconds before the next cycle. Hidden tabs stop starting new requests and
resume on return. A failed refresh retains the last table/image and displays a
warning; position samples older than 15 seconds are marked stale. Mobile users
can scroll the table horizontally. This is periodic imagery, not live video.

### Common Setup

`Common Setup` contains general device settings:

- Radar latitude and longitude
- Airport selection by ICAO or IATA code
- Reset location to the configured default
- Kilometer or mile distance labels
- IATA or ICAO airport labels
- Show route instead of callsign when available (enabled by default)
- Runway overlay enable/disable
- Radar range
- Footer and weather visibility
- Fahrenheit/Celsius
- Feet/metres
- 12-hour/24-hour clock
- Radar text scale from 80% to 130%
- Night-mode enable/disable and start/end time
- OTA password

Common settings are stored persistently in NVS.
With the route checkbox enabled, an available route replaces the callsign;
without a route, callsign remains the fallback. Disable it to always show the
callsign. This affects aircraft labels, not the table's separate callsign/route
columns. Devices without the new NVS key retain the previous route preference.

Portal pages are sent in bounded 512-byte pieces instead of assembling
page-sized strings in the ESP32's fragmented Wi-Fi/TLS heap. The home, Wi-Fi
and information renderers reuse native WiFiManager fragments; Wi-Fi scanning,
credential saving and connecting still use WiFiManager. Shared portal CSS and
navigation JavaScript are cached separately with a build-specific URL. Setup
HTML is never cached. Streamed HTML/asset writes have a bounded timeout and
yield to network processing; the ADS-B TLS connection and watchdog remain unchanged.

Both setup forms reject incomplete submissions with HTTP 400 before writing
settings. Reload the page if that happens; a cached form from older firmware
also needs to be reloaded. Unchecked checkboxes are valid and are not required
in the request. The OTA password field stays blank: leave it blank to retain
the stored password. These completeness checks are separate from the password
protection above and are not CSRF protection; keep the configuration portal
on a trusted network.

### Component Setup

`Component Setup` is intentionally separate from WiFiManager so the common page stays small and reliable.

For each drawable radar component it provides:

- Color selection
- `Back to default`
- Visibility `On` checkbox where the component is independently hideable

Configured components include:

- Background
- Grid
- Center marker
- Labels
- Aircraft
- Track vector
- Aircraft type labels
- Altitude labels
- Runways
- Runway labels
- Motorways
- Primary roads
- Cities
- Footer background

The footer's overall visibility remains available in Common Setup. Background and footer background are color-only because they are not independently hideable components in the display model.

### Portal Regression Checks

Host checks for bounded HTML output, escaping, disconnect handling and form
completeness (no device required):

```powershell
python scripts/test_radar_motion.py --test portal_html --compiler <path-to-g++-clang++-or-zig>
```

For a bounded live-browser check (Python Playwright required):

```powershell
python scripts/check_setup_pages.py --url http://<device-ip> --channel msedge --passes 3 --check-rejections
```

Omit `--channel msedge` when using Playwright's bundled Chromium. The check
loads the portal pages, verifies both forms and mobile layout, exercises
browser-only controls, and checks OTA authentication. With `--check-rejections`
it sends deliberately incomplete requests only, then compares settings. It
never submits a valid Save, changes Wi-Fi credentials, resets the location,
restarts the device, or uploads firmware. Screenshots mask coordinates and the
password field; the JSON report contains no setting values. Evidence goes to
temporary storage by default, including a partial report on test failure.
Saving valid settings, first-time AP setup and Wi-Fi reconnection are separate
acceptance checks, not covered by this non-destructive regression.

## BOOT Button

The BOOT button is connected to GPIO 9 and is active LOW.

| Action | Effect |
|--------|--------|
| Short tap | Cycle the range preset: 5 -> 10 -> 15 -> 25 km |
| Hold for 3 seconds | Clear Wi-Fi, location, units, display settings, and OTA password, then restart into setup |
| Hold during power-on | Force the setup portal for credential recovery |

## Radar Display

### Range Presets

| Ring 3 label | Approximate outer aircraft scale |
|--------------|----------------------------------|
| 5 km / 3 mi | 6.7 km |
| 10 km / 6 mi | 13.3 km |
| 15 km / 9 mi | 20 km |
| 25 km / 16 mi | 33.3 km |

The selected range and distance unit survive reboot.

### Aircraft

- Aircraft inside the outer ring are rendered as red heading triangles.
- A magenta speed vector shows the projected movement direction.
- Aircraft outside the ring but inside the ADS-B query radius are shown as bearing markers on the display rim.
- Callsigns are shown when route preference is disabled or route enrichment is unavailable.
- With route preference enabled, routes are displayed as origin-destination pairs such as `FRA-DUS` when available.
- Aircraft types are compacted for the display, for example `Boeing 737-800` becomes `B737-800`.
- Ground aircraft are hidden by default.

Origin and destination are not part of the basic ADS-B position response. Active callsigns are optionally enriched through [ADSBDB](https://www.adsbdb.com/). Routes are cached for six hours; missing routes for ten minutes even if a type was found. Network/TLS failures impose a five-minute lookup backoff.

To fit the ESP32-C3's RAM without reducing display quality, route/type lookups
release the retained position-feed TLS session first. Two 17,408-byte TLS record
slots are permanently reserved and reused through mbedTLS's allocator hook;
smaller allocations keep the SDK allocator. This prevents the two large record
buffers from depending on a fragmented web/scan heap. Admission requires both
slots idle, at least 48,000 free heap bytes and a 12,000-byte allocatable internal
block for the remaining handshake/crypto/JSON/TCP work. Only
one optional HTTP request starts per 30-second slot; a callsign fallback waits
for a later slot. Filtered JSON retains only route codes and aircraft type.
Position polling rebuilds its connection afterward and keeps it between slots.
Optional lookups are skipped when position data is stale or acquisition is
failing. Custom colors, font and NVS settings are unchanged.

The RGB565 framebuffer is packed by row for the round panel, with an extra
eight-pixel radial margin for preview/cardinal text. All physical-panel pixels
retain full 16-bit color and alpha blending; only invisible outer square corners
are omitted from storage (the BMP shows background there). This saves 15,104 B
including layout overhead, without RGB332 color reduction or an extra framebuffer.
The reserved TLS slots increase reported static RAM: that memory was previously
allocated dynamically by each TLS session, so static-RAM percentage alone is not
a useful comparison of total runtime headroom.

This is a memory-sharing tradeoff: periodic TLS reconnects can briefly pause
animation and increase a position-poll gap. Enrichment remains best-effort;
private/general-aviation callsigns often have no published route. The route
checkbox prefers available data; it does not guarantee a route for every aircraft.

Bounded hardware validation (2026-11-07) includes a five-minute combined
setup/scan/WebDisplay stress run with 96 successful position responses, six
route requests and zero recorded errors or record-pool spills. The normal
firmware also retrieved routes for real received aircraft (DXB-DUS and PRN-DTM).
These checks do not replace extended endurance, Wi-Fi-outage recovery or
physical-panel acceptance; detailed evidence and remaining cases are tracked
in `PROJECT_MEMORY.md`.

### Motion and Label Continuity

- The panel targets a frame every 125 ms (about 8 FPS) without increasing ADS-B request frequency. Aircraft symbols, vectors, bearing markers, and labels share the same display position.
- Positions advance using valid ground speed and ground track. Prediction is limited to ten seconds including `seen_pos` and time since the actual response was accepted; zooming does not restart this limit. Missing/invalid ground motion produces no velocity extrapolation.
- Small position corrections are blended over one second. Corrections larger than 24 pixels snap to the new observation rather than drawing an invented flight across the radar.
- Labels stay directly behind the triangle with a small tail gap. Attachment follows its smoothed heading during turns; the text itself remains horizontal and centered. There is no alternative placement, collision avoidance/forecast, leader line or inward clamping.
- Labels, triangles and vectors are clipped just inside the outer radar ring. During entry/exit, the intersecting portion remains visible, including trailing text after the aircraft center has left the circle. They are not admitted or removed as a whole at the center crossing.
- Only enabled, non-empty fields occupy rows: disabling aircraft type places altitude directly below callsign/route. Symbols and vectors are drawn above label text.

Continuity deliberately takes priority over eliminating every overlap. Traffic can move into overlapping label positions; these overlaps are accepted instead of making text wander around the display. Synchronous HTTPS requests and WebDisplay BMP transfers can pause animation; this is not a real-time renderer or a navigation instrument. The WebDisplay's periodic images are not an 8-FPS video stream. Final visual acceptance belongs on the physical panel.

### Display Regression Checks

Host tests compile the same motion/trailing-label and circular-clipping helpers used by the firmware, without an Arduino SDK or connected device. Additional suites cover bounded JSON snapshots and route preference persistence using the real settings module with fake NVS:

```powershell
python scripts/test_radar_motion.py --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test aircraft_snapshot --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test display_settings --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test reusable_http_session --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test flight_data_json --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test round_frame_layout --compiler <path-to-g++-clang++-or-zig>
python scripts/test_radar_motion.py --test tls_record_pool --compiler <path-to-g++-clang++-or-zig>
```

The `flight_data_json` suite uses the actual ArduinoJson headers installed by
`pio run -e supermini`; it tests the production filter/parser without a device.

For live table/image cycles and clearly separated browser-only edge cases:

```powershell
python scripts/check_live_display.py --url http://<device-ip> --channel msedge
```

This read-only Playwright/Pillow check verifies sequential requests, simulated
background pause/resume, and synthetic empty/stale/64-row/error/mobile cases.
It never saves settings; host fake-NVS results do not replace target persistence
acceptance. JSON reports contain no raw aircraft/settings values; screenshots
of the live display may contain the currently visible traffic and map labels.

For a bounded sequence of actual WebDisplay images (Playwright and Pillow required):

```powershell
python scripts/capture_radar_animation.py --url http://<device-ip>/display --frames 4 --interval 1 --channel msedge
```

Omit `--channel msedge` when Playwright's bundled Chromium is installed. The inspection browser disables automatic BMP polling and closes after the requested frames. PNGs, a contact sheet, a GIF, and timing metadata go to temporary storage by default. The recorded intervals include transfer time; screenshots verify appearance, not panel FPS. Avoid continuous BMP polling during serial-only endurance tests.

### Airports and Runways

Airport and runway data is generated from [OurAirports](https://ourairports.com/data/).

- Only `large_airport` entries are embedded.
- Open runway strips are included.
- Helipads are excluded.
- Runways are drawn in teal when enabled.
- Airport labels can use ICAO or IATA codes.
- The browser setup can select airports by either code.

Regenerate the embedded data with:

```bash
python scripts/build_large_airports.py
```

The generated files are:

```text
include/data/large_airports.h
src/data/large_airports_data.cpp
```

### Roads and Map Layers

Road and water geometry is embedded in the firmware for the supported display area. Map data is rendered with circular clipping to the active radar range.

Map data is based on [OpenStreetMap](https://www.openstreetmap.org/) contributors and is distributed under the ODbL where applicable.

### Weather and Time

Current conditions and timezone data come from [Open-Meteo](https://open-meteo.com/). Weather refreshes periodically using the configured radar location. NTP is used for the clock.

The weather row and complete footer can be disabled independently in Common Setup.

## Runtime Stability

The firmware uses several safeguards for the limited ESP32-C3 heap:

- ADS-B and weather responses are parsed directly from HTTP streams instead of retaining complete response strings.
- ADS-B JSON is filtered to the fields required by the display.
- Optional ADSBDB flight enrichment is rate-limited and cached.
- ADS-B failures back off for 30 seconds initially, then at most 60 seconds.
- Healthy ADS-B HTTPS sessions are reused. After a peer disconnect or request/parse failure, both HTTP and TLS objects are discarded and recreated for the next permitted attempt; the TLS handshake has a five-second timeout. A transport failure does not trigger a reboot loop.
- Optional flight enrichment checks its record slots and remaining heap reserve after releasing position TLS, never while a second live position/route TLS session is retained. An insufficient slot is skipped until the next rate-limited attempt; network failures retain the five-minute backoff.
- TLS record storage is reusable, aligned, zero-initialized and wiped on release. `tls pool: free=... hits=... spills=...` diagnostics distinguish reserved-slot use from large allocations falling back to the normal allocator. The hook is installed once before Wi-Fi startup, not changed around active sessions. Active weather/night-mode combinations still require dedicated testing.
- The record-pool sizing is validated against the pinned ESP32 SDK. After an SDK/platform upgrade, recheck actual record allocations and hit/spill diagnostics; do not assume the same capacities or memory reserves remain sufficient.
- A 15-second task watchdog detects a genuine main-loop stall after the initial Wi-Fi/setup phase.
- A controlled restart is only considered when the current free heap or largest free block remains critically low for several seconds.

The historical `min_heap` value is diagnostic only. It does not trigger a reset by itself.

### Apparent freeze with old aircraft data

If `/aircraft.json` still responds but its `revision` no longer increases and
`sample_age_ms` keeps growing, the webserver/main loop may be healthy while
ADS-B acquisition is failing. Positions intentionally stop extrapolating after
ten seconds; that can look like a frozen panel. A task watchdog cannot detect
this logical loss of fresh data while the loop continues to run.

Capture diagnostics before restarting. `adsb: HTTP -1` means connection setup
failed, not that an HTTP server returned a status. New logs distinguish fresh
and reused sessions and include the transport error, revision and sample age.
These diagnostics cannot by themselves distinguish every network/provider
outage from a client fault. For serial inspection, set DTR and RTS false BEFORE
opening the port to avoid resetting the device under investigation.

The session-owner host regression is supplemental lifecycle evidence, not a
TLS/network or endurance test:

```powershell
python scripts/test_radar_motion.py --test reusable_http_session --compiler <path-to-g++-clang++-or-zig>
```

Typical serial diagnostics look like:

```text
diag: uptime=120s heap=73000 min_heap=16000 largest=38000 wifi=3 rssi=-25 radar=1
adsb: request-ok t=900ms heap=70000 min_heap=16000
adsb: 8 aircraft
```

## Browser Simulator

The simulator is a framework-free browser implementation of the 240 x 240 radar display. It uses the repository's airport, runway, road, and water data where available and can use a local proxy for live ADS-B data.

From the repository root:

```powershell
python tools/radar_simulator/server.py
```

Open:

```text
http://localhost:8080/tools/radar_simulator/
```

The simulator includes:

- Radar range projection
- Circular clipping
- Aircraft symbols and vectors
- Routes and aircraft type enrichment
- Airport and runway overlays
- Location selection
- Unit, footer, weather, altitude, clock, and text-scale controls
- Layer visibility and color controls
- Night mode preview
- Mock-data fallback when live data is unavailable

### Windows Executable

Build the standalone simulator executable with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/radar_simulator/build_windows.ps1
```

Output:

```text
dist/PlaneRadarSimulator.exe
```

Keep the console window open while using the executable because it hosts the local proxy/server.

## Configuration

Hardware and behavior defaults are defined in `include/config.h`.

| Area | Important values |
|------|------------------|
| Portal | AP name, portal IP, hostname, mDNS |
| Wi-Fi | Connect/reconnect timing and portal timeout |
| BOOT | GPIO pin, tap duration, reset hold duration |
| Display | SPI pins, inversion, RGB order, SPI frequency |
| Location | Default latitude and longitude |
| ADS-B | Fetch interval and ground-aircraft visibility |
| Flight enrichment | Lookup interval, timeout, cache durations, failure backoff |
| Weather | Endpoint, timeout, and refresh interval |
| Runtime protection | Heap thresholds and task-watchdog timeout |
| OTA | Initial username/password defaults |

Machine-specific Wi-Fi defaults can be placed in the ignored `include/config_local.h`. Use `include/config_local.example.h` as a starting point.

## Project Layout

```text
include/
  config.h
  data/large_airports.h
  hardware/
  services/
  ui/
data/
  ui_font.vlw
partitions/
  plane_radar.csv
scripts/
  build_large_airports.py
  merge-firmware.sh
  merge_firmware.py
src/
  main.cpp
  data/
  hardware/
  services/
  ui/
tools/
  radar_simulator/
  GetRoadsAndWaterForPlaneRadar/
```

## Build and Upload

The PlatformIO environment is `supermini`.

```bash
pio run -e supermini
pio run -t upload -e supermini
pio device monitor -b 115200
```

The project uses:

- PlatformIO
- Espressif32 platform 6.5.0
- Arduino framework
- C++17
- LovyanGFX
- WiFiManager
- ArduinoJson 7

## Web-Flashable Images

Build the merged full-flash image:

```bash
chmod +x scripts/merge-firmware.sh
./scripts/merge-firmware.sh
```

The merged image is written to:

```text
release/plane-radar-merged.bin
```

For an already-built firmware:

```bash
./scripts/merge-firmware.sh --no-build
```

The merged/full image contains the bootloader and partition table and is flashed at offset `0x0`. It must not be uploaded through the OTA page.

## OTA Updates

1. Open `http://plane-radar.local` or the device IP.
2. Choose **Firmware update**.
3. Log in as `admin` with the configured OTA password.
4. Upload the application image ending in `-ota.bin`.
5. Keep the device powered until it restarts.

The initial OTA password is:

```text
plane-radar
```

Change it before exposing the device to a shared network.

If migrating from the old single-application partition layout, flash the new full/merged image over USB once. Later updates can use the OTA image.

## CI and Releases

GitHub Actions provides:

| Workflow | Trigger | Output |
|----------|---------|--------|
| Build | Push or pull request to `main` | Firmware artifacts for `supermini` |
| Release | Git tag matching `v*` | Full image, OTA image, and checksums |

Create a release with:

```bash
git tag v1.0.0
git push origin v1.0.0
```

Use the full image for first installation or recovery. Use the OTA image in the authenticated firmware page.

## Runtime Services and Dependencies

External runtime services:

- [adsb.fi](https://opendata.adsb.fi/) - nearby aircraft
- [ADSBDB](https://www.adsbdb.com/) - optional route and aircraft-type enrichment
- [Open-Meteo](https://open-meteo.com/) - weather and timezone data
- [OurAirports](https://ourairports.com/data/) - generated airport/runway dataset
- [OpenStreetMap](https://www.openstreetmap.org/) - map data source

PlatformIO dependencies:

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

## Troubleshooting

### Setup page shows only Save and Connected

Use the current `Common Setup` and `Component Setup` pages. The color and layer settings are intentionally separate from the WiFiManager parameter page to reduce page-generation memory usage.

If `.local` does not resolve, open the device IP from the serial log.

### ADS-B requests fail with TLS or DNS errors

The radar keeps its last valid data while failed requests use a backoff. Check:

- Wi-Fi signal strength
- router DNS availability
- access to `opendata.adsb.fi`
- current heap and `largest` diagnostic values

Optional flight enrichment may pause temporarily while the main ADS-B radar continues running.

### Watchdog output appears during startup

The task watchdog is intentionally enabled only after the initial Wi-Fi/setup phase. A watchdog message after startup indicates that the main loop was blocked for longer than the configured timeout.

### First installation or recovery

Use the full/merged image at flash offset `0x0`. Do not upload it through the OTA form.

## Licensing, Attribution, and Origins

### Project License

The firmware source in this repository is distributed under the [MIT License](LICENSE). The license and the copyright notice in `LICENSE` must remain with copies or substantial portions of the software.

This repository is derived from the work of:

- [WatskeBart/ESP32-Plane-Radar](https://github.com/WatskeBart/ESP32-Plane-Radar)
- [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar)

The original project concept and substantial parts of the original implementation are credited to the original authors. Changes and additions in this repository are maintained by the current project contributors. The complete upstream copyright and license information remains in [`LICENSE`](LICENSE).

### Third-Party Software

The firmware uses third-party libraries. Each library remains subject to its own license and copyright notices:

- [LovyanGFX](https://github.com/lovyan03/LovyanGFX)
- [WiFiManager](https://github.com/tzapu/WiFiManager)
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

The versions used for a build are declared in `platformio.ini`. Refer to the respective upstream repositories for their current license texts and notices.

### Data Sources and Services

- Map geometry is derived from [OpenStreetMap](https://www.openstreetmap.org/). Where OSM data is included or displayed, attribution is required under the [ODbL](https://opendatacommons.org/licenses/odbl/) and should include: `© OpenStreetMap contributors`.
- Airport and runway data is generated from [OurAirports](https://ourairports.com/data/). The generated dataset is included in the firmware and should retain its source reference and any applicable current source terms.
- Live aircraft data is requested from [adsb.fi](https://opendata.adsb.fi/). Optional route and aircraft-type enrichment uses [ADSBDB](https://www.adsbdb.com/).
- Weather and timezone data are requested from [Open-Meteo](https://open-meteo.com/). Use of these services remains subject to their current terms, attribution requirements, rate limits, and availability.

### Project Assets

- The device photo in `docs/images/plane-radar-device.png` is a project asset. Before publishing or redistributing the repository, confirm that the repository has permission to redistribute this image and add a photographer/copyright credit here if required.
- The 3D-printable case is provided through [MakerWorld](https://makerworld.com/de/models/3039652-esp32-plane-radar#profileId-3417884). The case's license and redistribution terms are separate from the firmware license and must be checked on the MakerWorld project page.

This section describes the project's known origins and attribution requirements; it is not legal advice. When redistributing hardware, firmware, generated data, or project images, preserve the relevant license files, notices, and source attributions.

## Credits

Thanks to the original project authors, the maintainers of the third-party libraries, the providers of the runtime services, and the contributors to the open data used by this project.
