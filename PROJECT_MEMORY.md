# Project Memory

## Project overview

- Firmware for an ESP32-C3 Super Mini and a 1.28-inch, round GC9A01 display (240 x 240, no PSRAM).
- PlatformIO environment: `supermini`; Arduino-ESP32 framework.
- Primary acceptance target is the physical display. The browser display preview/simulator is useful for development but is not a substitute for checking the panel.
- The public repository does not contain the device's ignored local configuration. Do not copy credentials, OTA passwords, or private location settings into tracked files or project notes.

## Build and device workflow

```powershell
pio run -e supermini
pio run -e supermini -t upload --upload-port COM10
```

- The hardware used for the recent tests enumerated as `COM10` at 115200 baud. Confirm the port before flashing; do not assume it is still COM10 on another host.
- For read-only serial monitoring, create the port closed, set DTR and RTS false,
  then open it; merely setting `dsrdtr=False`/`rtscts=False` does not deassert
  these lines. Earlier default-line monitor boundaries coincided with uptime
  resets and must not be mistaken for continuous endurance. The final monitor
  explicitly verified uptime before/after both open and close.
- Do not erase flash/NVS for routine firmware uploads. Preserve stored Wi-Fi, location, and display settings.
- `pio run -e supermini` is the targeted compile check. The installed PlatformIO reports an obsolete-core warning, but builds have completed successfully.
- Avoid web/BMP polling during long serial-only endurance tests; previous polling coincided with watchdog resets and adds unnecessary load.

## Main code map

| Area | Source | Notes |
|---|---|---|
| Application loop and diagnostics | `src/main.cpp` | Schedules network services, display refresh, heap diagnostics, and watchdog checks. |
| ADS-B acquisition and enrichment | `src/services/adsb_client.cpp` | Fetches nearby aircraft, parses JSON, retains the HTTPS connection, and rate-limits optional route/type lookups. |
| Weather and time | `src/services/weather_time.cpp` | Open-Meteo weather/timezone data and NTP-backed clock support. Weather API calls are skipped when neither the footer nor night mode needs the data. |
| Display rendering | `src/ui/radar_display.cpp` | Composes the 240 x 240 frame, map overlays, aircraft symbols, labels, and footer. |
| Packed RGB565 frame | `include/ui/round_frame.h`, `include/ui/round_frame_layout.h` | Static row-packed storage, full logical coordinates, physical-panel coverage plus preview padding; no color quantization. |
| TLS record reserve | `include/services/tls_record_pool.h`, `src/services/tls_record_pool.cpp` | Two fixed records via SDK allocator hook, SDK forwarding, zeroing, ownership and hit/spill diagnostics. |
| Motion/trailing-label helpers | `include/ui/radar_motion.h` | Platform-independent position aging, correction blending, angle smoothing, tail attachment and circular clip bands; shared with host regression tests. |
| Radar drawing constants | `include/ui/radar_theme.h` | Geometry and sizing for the grid, aircraft, labels, and footer. |
| Display visibility/settings | `include/services/display_settings.h` and `src/services/display_settings.cpp` | Persistent component toggles and display settings. |
| Setup portal | `src/services/wifi_setup.cpp`, `include/services/portal_html.h` | Streamed pages, bounded shared assets, completeness guards; native WiFiManager fragments and Wi-Fi save/connect handling. |
| WebDisplay and live data | `include/web/display_page.h`, `include/services/aircraft_snapshot.h` | Sequential image/table refresh and bounded streaming JSON from the accepted feed, without external requests. |
| Local build configuration | `include/config.h`, ignored `include/config_local.h` | Hardware and service configuration; keep private local settings private. |
| Browser simulator | `tools/radar_simulator/` | Framework-free preview; it does not automatically share every firmware rendering algorithm. |

## Runtime and memory decisions

- ADS-B data is fetched from adsb.fi. Its HTTP/1.1 response can be chunked; the response framing must be decoded before ArduinoJson sees the JSON body.
- A persistent keep-alive TLS connection avoids repeatedly allocating TLS state for each ADS-B poll. After the 2026-11-07 incident, `ReusableHttpSession` preserves healthy objects but destroys HTTP before TLS on a peer disconnect/request failure; the next backoff-permitted attempt constructs fresh objects in inline storage. Do not churn TLS objects on successful polls or substitute a reboot loop. The ADS-B handshake is bounded to five seconds; connect/read timeouts and 30/60-second retry backoff remain in force.
- Optional ADSBDB enrichment must not starve core ADS-B polling. Release position TLS before checking a dedicated slot budget: both reserved record slots idle, 48,000 B remaining heap and 12,000 B allocatable/internal blocks. One request per 30 s; five-minute failure backoff; skip while positions are older than six seconds or ADS-B is failing. Avoid two concurrent position/route TLS contexts; callsign fallback uses a later slot. Recheck the position deadline after weather I/O before starting optional work.
- RGB565 frame storage is static and packed by row with radius 128 (physical radius 120 plus 8 px preview padding), 49,551 pixels instead of 57,600. All physical-panel pixels retain RGB565/alpha; omitted outer square corners read as background in the BMP. `round_frame.h` uses LovyanGFX's public flexible-panel backend without modifying dependencies. Net saving 15,104 B including layout/backend overhead. Full-frame compositing stays offscreen before the panel push.
- Two aligned 17,408 B TLS record slots are statically reserved through the precompiled SDK's supported `mbedtls_platform_set_calloc_free` hook. The SDK setup was disassembled and allocates two 16,717 B buffers: total/largest heap numbers alone did not guarantee both allocations after scan fragmentation. Other requests go through the original `esp_mbedtls_mem_calloc/free` pair (C linkage); slots are zeroed/released under a port lock, with hit/spill diagnostics. Install before Wi-Fi startup, NEVER around live sessions. Static RAM rises because previously dynamic buffers are now pinned; this is not 34 KB extra peak TLS data. Version-coupled to the pinned ESP32 SDK/record size, not a custom SDK rebuild. Concurrent weather use can spill and still needs explicit testing.
- Do not introduce dynamic heap allocations in the per-frame aircraft-label placement path. The C3 has limited internal RAM and no PSRAM.
- The panel and framebuffer share an externally owned VLW font and data wrapper. Do not revert to separate `loadFont()` calls: changing to a bitmap font would otherwise destroy/reallocate the runtime font and duplicate glyph tables. Keep this sharing when changing framebuffer backends.
- Ring octant pixels are cached in a bounded 1.5 KB array; the current four two-pixel rings need 379 of 512 entries. Airport range flags are bit-packed and recomputed only after location/query-radius changes. Neither optimization uses a second full-screen framebuffer.
- Ground aircraft are intentionally hidden by default (`kAdsbShowGroundAircraft = false`). A valid empty visible-aircraft result is not by itself evidence of a parser failure.

## Portal authentication, Wi-Fi recovery and OTA abort (2026-11-10)

- User decision: on the home network every settings-changing/revealing route
  requires `admin` + OTA password via `services::ota::authorizeSettingsRequest()`
  (`/param`, `/paramsave`, `/component*`, `/location-default`, `/wifi`, `/0wifi`,
  `/info`, and wrapped native `/wifisave`, `/restart`, `/exit`, `/close`, `/erase`).
  Display/table/BMP, home page, airport search and assets stay public. While the
  setup AP is active the settings routes are open; `/firmware` always needs auth.
- Runtime reconnect feeds the task watchdog inside `waitForLinkWithUi()`; the
  wait is rollover-safe. `openConfigPortal()` retries known credentials after
  `kWifiPortalRetryKnownMs` (5 min) without an AP client; after BOOT reset there
  are none, so the portal stays open as before.
- OTA: aborted uploads (no done handler from WebServer) release `inProgress()`;
  30 s without upload data also releases it. Upload chunks feed the watchdog.
- Flashed without NVS erase (device enumerated as COM9 on this host). Verified
  on the device: unauthenticated settings routes return 401, public routes 200,
  authenticated `/param` 200; 40 s serial sample without errors/resets. NOT yet
  tested on the device: OTA abort recovery, router outage/reconnect, portal
  retry. `scripts/check_setup_pages.py` now needs `PLANE_RADAR_OTA_PASSWORD`.

## Setup portal reliability

- Missing Common Setup fields were reproduced in HTTP 200 responses: the page
  ended normally but the entire parameter block was absent. Roughly 27 KB total
  free heap did not imply a sufficiently large contiguous block (about 9 KB).
  WiFiManager builds whole pages with several large temporary `String`s.
- Do not restore whole-page assembly. `HtmlWriter` uses a 512-byte stack buffer,
  escapes field values and stops after a sink failure. Reuse all registered
  common parameters, refresh current values on reads and keep OTA input blank.
- Streaming common/component pages alone was insufficient: repeated home/Wi-Fi
  loads could truncate too, and information/page-transfer testing produced
  watchdog restarts. The captured abort address resolved to `task_wdt_isr`, not
  the unrelated SPI startup warning. Intermediate builds also reached sub-1 KB
  minimum heap and one ADS-B HTTP -11; these were failed tests, not clean evidence.
- Home/Wi-Fi/info rendering now streams native fragments through a local
  `PortalWiFiManager` adapter. Native scan results, form controls, menu and
  diagnostics are reused; `/wifisave` and credential/connect handling are not
  replaced. Keep custom routes registered before the library handlers.
- Shared CSS/JS have build-specific URLs and one-day caching to avoid repeatedly
  loading them on navigation. HTML is `no-store`. Close-delimited HTTP/1.0 output
  works with HTTP/1.0 and HTTP/1.1 clients without heap-allocated chunk headers.
  Socket writes are nonblocking, limited to two seconds per piece and paced;
  timeout/disconnect shuts down only the portal socket, never the ADS-B TLS
  client. Do not extend/disable the runtime watchdog to mask transfer hangs.
- WiFiManager's information fragments are small, but its full page is not.
  Query AP fields only when an AP interface exists; use the ESP-IDF reset reason
  instead of the native two-core reset item on the single-core C3.
- Common/component saves require the final form-version marker and every
  non-checkbox setting before ANY settings/NVS writes; a blank OTA password is
  valid. Incomplete requests return 400; common GET-save returns 405. This is
  form completeness checking, NOT CSRF protection or authentication. A stale
  cached pre-fix form must be reloaded.
- `scripts/check_setup_pages.py` never submits a valid Save, location reset,
  restart or OTA upload. It compares settings after rejected saves, exercises
  unsaved DOM controls, checks mobile layouts and OTA authentication, masks
  private screenshots and writes even partial failure reports outside the repo.
- Native scan-list/static-IP fragments still use small temporary `String`s;
  do not treat streaming as a proof against all future traffic, scan density,
  slow clients or heap pressure. AP provisioning and valid-save persistence
  require separate, explicitly settings-changing acceptance tests.

## Aircraft label rendering

- The label block can contain route/callsign, aircraft type, and altitude. The global label toggle controls the block; type and altitude have their own visibility toggles.
- Only fields that are enabled and non-empty are included in the measured/drawn block. Thus disabling aircraft type places altitude directly below the identity without a blank row.
- Current user decision supersedes BOTH the original 24-candidate placement and the intermediate immutable screen-offset policy: attach labels directly behind the displayed triangle, following its smoothed heading during turns. Text stays horizontal/centered with a small tail gap.
- No collision avoidance, forecasting, alternative placement, leader lines or inward clamping. Dense-traffic overlaps are accepted. Keep aircraft position extrapolation and heading smoothing unchanged.
- A bounded static cache keyed by aircraft hex ID holds motion and smoothed angles only; there is no label-layout cache. Geometry is calculated from current text dimensions and heading without heap allocation.
- Labels, triangles and vectors are clipped just inside the outer ring using disjoint rectangular clip bands; fully interior objects draw once. Keep outside-center aircraft drawable while their label/symbol/vector intersects the circle. Text may remain partly visible after center exit. No second framebuffer is used.
- Vectors and then aircraft triangles are rendered after text to preserve symbol priority. Existing city/airport admission margin is separate (`kMapLabelInsideRingInsetPx`), not reused to admit aircraft.
- Route preference is `prefer_route` in Common Setup, persisted as `display/showRoute`, default true for older NVS. Enabled uses route with callsign fallback; disabled always uses callsign. The common completeness marker is now `common-v2`, rejecting old cached forms before writes.
- Route availability check (2026-11-07, user question during recovery test):
  preference is checked; a fresh snapshot at revision 217 contains nine aircraft,
  nine callsigns and ZERO routes. Routes require the separate ADSBDB enrichment,
  not the basic ADS-B position feed. Current live heap has only a 9,204 B largest
  block; `enrichmentAllowed()` requires at least 48,000 B largest blocks and
  24,000 B total free to avoid pausing, then 56,000 B largest/40,000 B total
  for 15 s to resume. Thus enrichment is effectively blocked in the observed
  operating state; empty routes do NOT prove ADSBDB has no matching routes.
  The checkbox is a display preference, not a bypass of the RAM guard. This
  describes the pre-iteration state; the later exclusive-slot design below
  supersedes these concurrent-session thresholds.

## Live WebDisplay data

- `/aircraft.json` streams at most 64 entries from the accepted list using the shared 512-byte writer/JSON escaping, without an external ADS-B call, JSON tree or full-list copy. Callsign/route are independent; invalid numbers are null. Distance uses current units; position age adds source age and receipt elapsed time with rollover-safe subtraction.
- `/display` loads build-versioned `/display.js`. Fetch snapshot, then image, then wait five seconds; never parallel image/snapshot requests. Hidden tabs start no new requests. Failed refreshes retain last content with a warning; samples older than 15 seconds are stale.
- Table columns: callsign, route, type, hex, altitude, speed (kn), track, distance (km/mi), position age. These are received values, not extrapolated animation positions; retained outside-ring aircraft remain included. Render strings with `textContent`; allow mobile horizontal table scrolling, not whole-page overflow.
- Manual captures set `window.__RADAR_MANUAL_PREVIEW__` before page load to suppress automatic table/image polling. Browser regression separates live-device checks from synthetic empty/stale/64-row/error cases and a simulated visibility event. Never imply these prove real OS tab lifecycle or all panel entry/exit scenarios.

## Display animation

- The main loop targets a frame every 125 ms (about 8 FPS), while retaining the existing ADS-B polling frequency. OTA-in-progress takes priority over animation.
- Symbols, vectors, labels, and beyond-ring bearing markers use the same extrapolated screen position. Only valid ground track/speed drive velocity; TAS/IAS fallbacks used by the old vector drawing do not become position velocity.
- `seen_pos` plus elapsed time since `aircraftReceivedMs()` limits prediction to ten seconds. After that the last bounded prediction freezes. A context/zoom reset retains the original response timestamp rather than treating old data as a new sample.
- Small measurement corrections blend over one second; corrections over 24 pixels snap instead of inventing a flight. Heading interpolation takes the short path across north. Unsigned elapsed-time calculations support the `millis()` rollover.
- Network activity remains synchronous: HTTPS and especially BMP transfers can pause the panel. Do not claim uninterrupted 8 FPS. A dense WebDisplay sequence showed temporary overlapping labels while the main loop was occupied with image transfers.
- `scripts/test_radar_motion.py` compiles actual firmware helpers. Motion coverage includes aging/freeze, invalid velocity, rollover, correction continuity, repeated samples, stale-context timestamps, angle wrap, heading-relative tail attachment, 600-update translation invariance, text resizing and exhaustive clip pixels/partial exit. Additional suites: `portal_html`, `aircraft_snapshot`, `display_settings` (real settings module, fake NVS). These do not test the entire renderer or target NVS.
- `scripts/capture_radar_animation.py` captures a bounded Playwright sequence with automatic BMP refresh disabled; `--channel msedge` works on the development host. It writes PNG/GIF/JSON evidence outside the repository and closes its own browser. Image changes are not proof of a specific FPS.

## Verified stability history

The following results were observed on the physical ESP32-C3 over serial at 115200 baud:

### 2026-11-07 live-data freeze incident and recovery candidate

- User reported a frozen panel and one aircraft roughly 62,600 seconds old.
  Before ANY reset/upload, direct `/aircraft.json` showed revision exactly 1,000,
  age 62,630.792 seconds and one row. `/info` showed uptime 1,094 min 36 s.
  The firmware's renderer/main loop was still running; this was sustained loss
  of fresh ADS-B data, not a proven CPU deadlock.
- Non-resetting 85.2-second serial capture confirms ongoing animation/diagnostics,
  Wi-Fi connected, roughly 49-71 KB free heap and 15-33 KB largest blocks. ADS-B
  still retries but returns HTTPClient -1 (one observed connect attempt took
  3,003 ms), with 60-second backoff/count 10. No successful update occurred.
  Evidence: `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-freeze-20261107-111319.log`.
- Exactly 1,000 accepted responses suggests a keep-alive/server-close boundary,
  but the original first failure was not captured. Do NOT claim a confirmed
  server request limit or proven original root cause. A desktop request from a
  different network path received a Cloudflare 403 challenge; that is not
  evidence that the ESP32 received 403 (its observed error was -1).
- Candidate repair rebuilds BOTH HTTP/TLS objects after peer close/failure,
  releases failed-session resources during the existing backoff, bounds the
  ADS-B handshake to five seconds and logs fresh/reused session, peer-close
  header, TLS error/revision/age. No NVS, drawing, watchdog or API-rate changes.
  Source: `include/services/reusable_http_session.h`, `src/services/adsb_client.cpp`.
- Lifecycle regression uses the production owner with fake transport/HTTP:
  1,500 healthy prepares without reconstruction, peer-close replacement,
  failed-session release/deferred reallocation and HTTP-before-TLS destruction.
  All five host suites pass. This is NOT a reproduction of the original TCP/TLS
  fault. The effective Arduino build mode is C++11 despite the existing C++17
  flag; use inline aligned storage rather than requiring std::optional or
  changing dependency-wide build flags. Candidate builds/flashes successfully:
  176,116 B static RAM, 1,351,020 B flash; NVS not erased.
- After the required upload reset, snapshot freshness recovers (revision 3,
  age 0.8 s, eight rows). A 150.3-second serial sample records 48 successes,
  no captured errors and no session replacement within that sample. Largest
  block 9,204 B; later idle heap about 27-28 KB; historical minimum 4,356 B.
  Evidence: `radar-recovery-20261107-112030.log` in the Temp root. A reboot plus
  short successful run is NOT proof that the recurrence has been fixed.
- Live-browser checks pass under
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-live-20261107-112233`.
  An earlier run's real-device phase passed but its synthetic phase failed to
  load the JS reliably. Browser-only fixtures now use the exact HTML/JS captured
  during the live phase, keeping synthetic reloads off the ESP32. No valid Save.
- The passive-serial follow-up toward revision 1,100 was deliberately stopped
  for the user's requested route-memory firmware iterations. It ran 907.4 s,
  logged 298 successes, no failures/resets and NO new session. Final revision
  362, age 1,655 ms. This run is STOPPED/NOT PASSED, not >1,000/reconnect evidence.
  COM10 was closed automatically before any subsequent firmware upload.
  Directory/report:
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-recovery-1000-20261107-1125`.

### 2026-11-07 route-memory iterations

- Evaluated 8-bit RGB332 framebuffer (57,600 B potential saving) offline from a
  real BMP. Color count fell 249 -> 56 and the dark-blue background disappeared;
  rejected as the default because 16-bit/custom-color/antialias quality should
  remain intact. No 8-bit firmware was flashed. Snapshot/quantized candidate:
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-route-memory-baseline-20261107`.
- Reducing mbedTLS buffer sizes is NOT a reliable local macro-only fix: this
  Arduino SDK contains precompiled mbedTLS with a 16,384-byte record size. A
  custom SDK/provider-side proxy would introduce a larger maintenance or runtime
  dependency. Prefer sequential resource ownership in the existing firmware.
- Iteration 1 retained RGB565 and released the retained position TLS before
  route admission, without changing the old 48/56 KB block guard. Measured
  73,216-73,464 B free / 38,900-40,948 B largest instead of ~30 KB / 9,204 B.
  Old guard still blocked routes. 70 s, 24 ADS-B successes/no observed errors.
  `radar-route-iter1.log` in Temp. First upload stopped at 92%; immediate full
  retry succeeded and verified hashes. NVS was not erased.
- Iteration 2 used an exclusive slot budget (68,000 B total, 32,000 B block),
  filtered JSON and a three-second route handshake timeout. Route requests now
  execute and core ADS-B reconnects afterward. 130 s/41 ADS-B successes; one
  route HTTP -11 read timeout, no recorded ADS-B or allocation failures. The
  current traffic was GA/registration callsigns without matches. Not a clean
  all-network-error-free sample. An instrumentation label was accidentally
  attached to the core GET; corrected in later builds. `radar-route-iter2.log`.
- Iteration 3 separates a missing combined-endpoint route fallback into a later
  30 s slot (one optional HTTP request/loop turn), preserves partial type data
  and gives missing routes 10 min TTL even when a type exists. Healthy route
  hits retain 6 h TTL, keyed by hex AND callsign. Reserve checks are tested at
  total/largest/internal boundary values. Filter/parser host regression uses the
  actual installed ArduinoJson; it checks IATA/ICAO/whitespace, missing/unknown
  response, bounded output and discarding 20 KB of unused JSON fields.
- A test-build-only `PLANE_RADAR_ROUTE_MEMORY_PROBE` exercises public callsign
  DLH400 once through the actual TLS/filter/route parser, logs separately and
  NEVER inserts it into the accepted aircraft list. First probe log missed the
  initial lookup; its 95 s/31 ADS-B-success sample only proves several actual
  HTTP-404 negative lookups/reconnects, no errors. Do not claim a probe success
  from that sample. The repeated probe is delayed to >=30 s for reliable capture.
  Production must be rebuilt/flashed with the probe disabled after validation.
  Probe evidence: `radar-route-iter3-probe.log`, then
  `radar-route-known-verified.log` in Temp.
- Repeated delayed probe PASSED on the real ESP32: HTTP 200 at 1,112 ms,
  parsed `FRA-JFK`, after cleanup 73,236 B free, historical minimum 14,052 B.
  Its full 75 s sample has 24 ADS-B successes, a later expected HTTP-404 route
  miss, no captured ADS-B/TLS/runtime errors, and minimum later 13,764 B.
  This is real acquisition/filter/parser evidence for a KNOWN PUBLIC TEST
  callsign, not proof that a currently received GA aircraft has a route or an
  actual route-bearing aircraft was rendered. Later production outcomes below
  supersede this intermediate probe.
- The first production stress validation FAILED despite the known-route probe:
  `radar-route-final-20261107-120058` (300.3 s, 66 successes) captured route/core
  allocation failures after repeated setup/scan, plus a truncated first Wi-Fi
  page. The feed later recovered without reset, but this is NOT clean evidence.
  The 68 KB / 32 KB exclusive guard was insufficient under fragmentation.
- Packed RGB565 iteration saved 15,104 B: 161,004 B static RAM / 1,349,888 B
  flash, typical frame maxima ~90-111 ms (later ~7.5-7.9 FPS windows), correct
  full-color BMP and exhaustive storage mapping host test. It STILL FAILED
  stress: largest blocks fragmented to 20-22 KB, core reconnect allocation
  failed, route slots skipped. Controlled stop after 262 s / 52 successes;
  `radar-route-packed-stress/report.json`. Do NOT present packing alone as a fix.
- Dedicated record-pool iteration adds fragmentation-independent storage for
  exactly the large TLS buffers, retains packed RGB565 for additional real
  reserve, and uses the 48 KB/12 KB remaining-heap budget. Build/flash:
  195,844 B static RAM / 1,350,712 B flash; no NVS erase. Host pool tests cover
  two buffers, alignment, size bounds, zeroing, isolation, exhaustion, forwarding
  decisions and 1,500 lifecycles. Production `PLANE_RADAR_ROUTE_MEMORY_PROBE`
  remains disabled in production.
- Reproducible first/expired-cache `/wifi` truncation was NOT memory proof: its
  synchronous native scan ran inside the eight-second transfer deadline. Scan
  results now finish BEFORE constructing the bounded HTTP response; the small
  scan fragment is released immediately after streaming. Native credential
  saves, two/eight-second socket deadlines and watchdog remain unchanged.
  First three-pass setup regression now passes, including rejected incomplete
  forms, unchanged settings and no JavaScript errors:
  `radar-setup-20261107-121603`. LiveDisplay regression also passes:
  `radar-live-20261107-121638`. Both are in the approved Temp root.
- Dedicated-pool production stress PASSED: 300.6 s, 96 ADS-B successes, six
  optional HTTP-404 misses, six fresh position sessions, final revision 101 /
  age 196 ms. No captured HTTP negatives, parse/TLS allocation/critical-heap
  errors, portal aborts or resets. Uptime diagnostics increase 20 -> 311 s;
  record pool hits 30, spills ZERO; minimum heap 14,352 B after scans/WebDisplay,
  later idle ~40.7-43.3 KB, largest 12,276 B. Later animation windows ~7.6-7.9 FPS,
  maximum frame 85-108 ms; scan/HTTPS/BMP pauses are still synchronous.
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-route-pool-20261107-121559`.
- The stress includes TWO separate three-pass setup runs, initial and expired
  scan-cache paths (`radar-setup-20261107-121603`, `...-121829`), LiveDisplay
  (`radar-live-20261107-121638`) and four changed BMPs
  (`radar-animation-20261107-121814`). Settings stay unchanged; no valid Save.
  All eight host suites pass. During this stress sample the received aircraft
  were GA callsigns without provider route matches; do not claim its zero-route
  list means the allocator gate is still blocking (actual HTTP-404 requests
  prove otherwise). Real positive routes arrived in the restored run below.
- Final known-public-route probe with the dedicated pool PASSED: 75 s / 25
  ADS-B successes, DLH400 parsed as FRA-JFK (HTTP 200, 1,105 ms), after cleanup
  54,204 B heap, minimum then 31,984 B; later minimum 31,716 B. A later expected
  HTTP-404 miss also completed. No captured errors, pool hits 14 / spills ZERO.
  Evidence: `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-route-pool-known.log`.
  The probe never inserts synthetic aircraft.
- Production firmware restored successfully on COM10 WITHOUT the diagnostic
  macro; binary string check confirms probe absent / pool present, upload hashes
  verified, no NVS erase. 195,844 B static RAM / 1,350,712 B flash. Firmware SHA256:
  `6e9fb51f779c1310c17f33cf58502dec6a03a26bf00b9476440fdc811621659e`.
- Restored-production smoke PASSED: 90.3 s / 28 ADS-B successes, three optional
  requests (404, 404, 200), no captured errors, no uptime resets (20 -> 92 s),
  no diagnostic probe or pool spills; historical minimum 24,928 B. Final
  accepted snapshot revision 31 / age 2,631 ms / six aircraft / TWO routes.
  Live request GPX7FQ parsed PRN-DTM (HTTP 200, 1,099 ms). Immediate read-only
  follow-up at revision 35 / age 426 ms confirms real received routes
  UAE58Q / DXB-DUS / B77W and GPX7FQ / PRN-DTM / A319. These are actual feed
  aircraft, unlike the separately logged DLH400 diagnostic probe.
  Evidence: `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-route-restored-20261107-122730`.
- Restored firmware also passes LiveDisplay regression
  (`radar-live-20261107-122732`) and another three-pass setup regression
  (`radar-setup-20261107-122804`): no valid Save, settings unchanged, no JS errors.
  All eight host suites pass again and `git diff --check` has no whitespace
  errors (only existing LF/CRLF conversion warnings). All test-owned serial and
  browser handles are closed. No commit or push was performed.
  No endurance >1,000 or physical-panel acceptance has been completed for this
  variant; active weather/night mode and AP provisioning remain untested.

### Earlier bounded observations (not validation of the incident fix)

- Three-hour ADS-B endurance run: 3,587 logged successful responses, 3,594 aircraft-list reports, no ADS-B errors/backoffs/restarts/watchdog events, and up to 7 aircraft in a response. Some valid responses contained zero displayable aircraft because ground traffic is hidden.
- After skipping unnecessary weather API requests for the tested settings, a 40-minute run logged 798 successful ADS-B responses and no TLS allocation failures, weather errors, ADS-B errors, or device restarts.
- The earlier near-range label-placement iteration was flashed without erasing NVS, checked through the WebDisplay framebuffer, and passed a 30-second serial smoke check: 10 successful ADS-B requests and no ADS-B/runtime error logs.
- Animation performance before background caching was about 3.4 FPS, with 277-285 ms maximum frame time. Caching reduced normal observed windows to roughly 7.3-7.8 FPS and 75-94 ms maximum frame times; startup and synchronous network/BMP windows were slower. This is measured throughput, not a guaranteed refresh rate.
- An optimized intermediate build completed a 180-second sample with 53 successful ADS-B requests and no recorded ADS-B/runtime error or restart. Its six-frame WebDisplay sequence transferred successfully, but overlap and multi-second BMP-induced pauses remained visible; that iteration's per-row yielding was subsequently reduced to small batches.
- The previous avoidance-enabled build (including original-sample timestamps and batched BMP yielding) used 183,276 B static RAM (55.9%) and 1,325,914 B flash (72.3%). A 120-second sample recorded 39 successful ADS-B responses without logged ADS-B/runtime errors or restarts. Normal later windows measured about 7.5-7.6 FPS, with 79-102 ms maximum frame times; startup/web windows were slower. Idle free heap was roughly 23-25 KB, largest block 9,204 B, and historical minimum 1,228 B.
- A four-frame sequence from that previous build showed moving aircraft/vectors and persistent compact labels; all successive frames changed. It also showed a label/vector overlap in the first congested frame, so it is evidence of operation, not a claim of overlap-free rendering. Actual frame-capture intervals were about 3.2-3.5 seconds including transfer time, not the panel animation interval.
- Fixed-label follow-up: removed avoidance gliding and all collision-forecast state/helpers. Build and flash succeeded without NVS erase; static RAM is 180,452 B (55.1%), flash 1,323,724 B (72.1%), saving 2,824 B static RAM against the preceding build. Host tests pass with the new fixed-anchor regressions. A bounded four-frame WebDisplay sequence transferred successfully and showed moving aircraft with fixed relative labels, while later overlaps remained possible as intended.
- Fixed-label smoke check: 75 seconds, 24 successful ADS-B responses, no recorded ADS-B/runtime errors or restarts. Later windows measured about 7.2-7.4 FPS with 69-74 ms maximum frame times; startup/BMP windows were slower. Free heap recovered to 25,968 B, largest block 9,204 B, historical minimum 4,856 B. This is a short observation under the current traffic/settings, not proof of long-term memory safety or a new guaranteed minimum.
- Final portal repair (2026-11-06): flashed without NVS erase; 180,452 B static
  RAM, 1,336,032 B flash. Both host suites pass. Two three-pass Playwright runs
  pass on this firmware (home, Wi-Fi, info, 16 common/26 component controls,
  unsaved controls, mobile, four HTTP-400 incomplete saves, HTTP-405 GET-save,
  unchanged settings, HTTP-401 OTA authentication, no JS errors). The later run
  additionally checks `/0wifi` and complete closing HTML. HTTP/1.0 and HTTP/1.1
  direct reads of all five non-scan page routes also complete successfully.
- Final portal serial observations are TWO separate bounded samples, not an
  endurance claim: 120.6 seconds/38 ADS-B successes, then 90.4 seconds/28
  successes. No logged ADS-B/TLS/runtime errors or unintended resets in either.
  The initial startup banner in the first sample belongs to the firmware flash;
  subsequent diagnostic uptime increases normally. Historical minimum heap
  after the second sample is 3,440 B; idle heap recovers to roughly 22.5 KB,
  largest block 9,204 B. Later normal windows show about 7.4-7.7 FPS, while scan
  windows remain slower. Flashing and these tests did not change stored settings.
- Final portal browser evidence is outside the repository under
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-setup-20261106-161810`;
  screenshots mask location/password fields and `report.json` contains no
  setting values. Serial summaries are in `radar-portal-monitor-20261106-161513.log`
  and `radar-portal-final-20261106-161809.log` in the same temporary-storage root.
- Trailing-label/route/table follow-up (2026-11-06): current build and NVS-preserving
  flash use 176,100 B static RAM (53.7%) and 1,350,754 B flash (73.6%); 4,352 B
  less static RAM than the preceding portal firmware. All four host suites and
  JavaScript/Python syntax checks pass. Route on/off persistence is tested with
  the real settings module/fake NVS; no valid device Save was submitted.
- The FIRST new-feature hardware iteration FAILED: the first BMP aborted,
  heap fell to 9,008 B (largest block 2,036 B), ADS-B returned HTTP -11 followed
  by HTTP -1/backoff, and the table correctly displayed stale data. Do not count
  that 180.1-second/12-success sample as clean validation. Subsequent image-only
  pacing waits below 16,000 B free/4,096 B largest allocatable heap, coalesces
  720-byte rows with Nagle enabled, and keeps bounded nonblocking writes. These
  are HTTP changes only; ADS-B TLS and watchdog policies are unchanged.
- Paced firmware: 150.1-second sample/47 ADS-B successes, no ADS-B errors,
  critical-heap warnings or resets. One immediate BMP disconnect at 20.4 seconds
  coincided with the first test closing its resumed tab before image completion;
  the browser harness now waits for that image before closing. Historical
  minimum heap is still only 2,384 B after repeated setup/scans; later idle heap
  is about 26.3 KB, largest block 9,204 B. This remains limited transient headroom,
  not a guaranteed minimum or endurance result.
- On paced firmware, three-pass portal regression passes (17 common/26 component
  controls, mobile, five rejected incomplete/old forms, GET-save 405, OTA 401,
  settings unchanged, no JS errors). Evidence:
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-setup-20261106-165415`.
- Corrected live-table regression passes repeatedly: sequential image/snapshot
  requests (maximum one in flight), simulated hidden/resumed lifecycle, plus
  browser-only empty/no-sample/stale/64-row/safe-text/mobile-scroll and retained
  image/table after synthetic errors. Intermediate harness evidence:
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-live-20261106-165502`
  and `radar-live-20261106-165630` in the same root. These fixtures do not modify
  the firmware's data or settings.
- The separate 120.1-second/39-success sample also had no ADS-B errors or
  critical-heap warnings, but logged two immediate writes to one disconnected
  snapshot socket: the synthetic `document.hidden` getter still returned false
  while closing the tab and its visibility event started an extra request.
  The final harness returns its simulated tab to hidden before closing. Do not
  describe the earlier disconnect-bearing samples as completely error-free.
- Final verification uses DTR/RTS false before serial open and checks uptime
  across open/close: 48 s before opening, 49 s after, 140 s before close, 141 s
  after. The 90-second capture (91.5 s including final info requests) records
  28 ADS-B successes with NO logged ADS-B/TLS/runtime errors, portal aborts,
  critical-heap warnings or resets. Idle heap recovers to roughly 27.3-27.8 KB,
  largest block 9,204 B; this boot's historical minimum is 2,816 B. Later normal
  windows are about 7.4-7.6 FPS, maximum frame times 92-105 ms; transfers remain
  synchronous. Log: `radar-trailing-verified-20261106-165915.log` in the Temp root.
- Final corrected browser report/screenshot:
  `C:\Users\deinin01\AppData\Local\Temp\opencode\radar-live-20261106-165918`.
  Direct HTTP/1.0 and HTTP/1.1 checks of `/display`, `/display.js` and
  `/aircraft.json` also pass with complete content, valid bounded snapshot schema
  and a fresh sample. All serial/browser handles opened for these tests are closed.
- Four-frame capture `radar-animation-20261106-165535` transferred fully; all
  successive frames changed. Live screenshots show heading-relative labels and
  partial bottom-ring text clipping. Capture intervals (including transfer)
  are roughly 2.0-4.3 seconds, not panel FPS. Incidental traffic does not validate
  every entry/exit/turn case or replace physical-panel acceptance.
- During web inspection, historical minimum heap approached 1 KB despite roughly 22-25 KB idle free heap. Bit-packing airport flags freed about 2 KB of static DRAM, but low transient headroom remains a risk, not a solved endurance claim.
- These are finite observations, not a guarantee against future service, Wi-Fi, or reconnection failures.
- Last pushed baseline before the current label-placement work: `ad442a0` (`Stabilize ADS-B TLS and chunked parsing`) on `main` / `origin/main`.

## Workspace care

- At the time this note was created, `.vscode/extensions.json` had a pre-existing user modification. Keep it separate from unrelated commits and do not overwrite it.
- Keep generated/build output such as `.pio/` out of source commits unless explicitly requested.
- On 2026-11-09 the user authorized commit and push of the related animation,
  trailing-label, route preference, live-table, portal and route-memory changes,
  including tests and documentation. Exclude the pre-existing
  `.vscode/extensions.json` modification and ignored local configuration/build output.
