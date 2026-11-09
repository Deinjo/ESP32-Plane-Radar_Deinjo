# Changelog

Notable project changes are recorded here. Unreleased entries describe source changes that have not yet been tagged as a firmware release.

## [Unreleased]

### Fixed

- Enable best-effort route enrichment without reducing RGB565 display quality:
  release position TLS before an exclusive route slot, require two idle reserved
  TLS records plus 48,000 B remaining heap / 12,000 B largest internal blocks,
  and reconstruct position TLS
  afterward. Retain normal keep-alive between slots; skip auxiliary work while
  the position feed is stale or failing. No NVS or watchdog changes.
- Keep route/type JSON filtered to required fields and bound the route handshake
  to three seconds. Perform at most one optional request per 30-second slot;
  defer callsign fallback instead of chaining two requests in one loop turn.
  Preserve partial type results and cache missing routes for ten minutes, not
  six hours merely because an aircraft type was found.
- Reserve two aligned, reusable 17,408-byte mbedTLS record slots through the
  SDK's existing allocator hook; forward other allocations to the SDK. Wipe on
  release and report slot hits/spills. This prevents web/scan heap fragmentation
  from blocking the two large record-buffer allocations.
- Pack static RGB565 frame rows around the physical round screen plus 8 px
  preview padding, saving 15,104 B without reducing visible colors or alpha.
  Omitted outer square corners of the BMP are background. No dependency edits.
- Finish native Wi-Fi scans before starting the bounded response-transfer timer;
  slow initial/expired-cache scans no longer truncate their own Wi-Fi form.
  Socket deadlines and watchdog remain unchanged.
- After the 2026-11-07 stale-feed incident (revision 1,000, one aircraft over
  17 hours old), harden ADS-B session recovery: preserve healthy keep-alive,
  discard failed/closed HTTP and TLS objects in dependency order, and recreate
  them only for a permitted retry. Bound the ADS-B TLS handshake to five seconds
  and add session/transport/revision/age diagnostics. Keep watchdog, retry
  backoff, NVS and polling frequency unchanged. The original trigger is not yet
  proven; exactly 1,000 responses is a clue, not a confirmed provider limit.
- Restore missing Common Setup controls by streaming the registered parameters
  through a bounded 512-byte HTML writer instead of WiFiManager's large
  temporary strings. Refresh current defaults on each page load and never echo
  the stored OTA password.
- Stream Component Setup, home, Wi-Fi and information pages as well, reusing
  native WiFiManager menu/form/scan/diagnostic fragments. Serve shared portal
  CSS/navigation JavaScript separately with build-versioned caching. Avoid
  heap-allocated HTTP chunk framing, use close-delimited HTML for HTTP/1.0 and
  HTTP/1.1 clients, and bound plain-HTTP writes so a stalled peer cannot keep
  retrying indefinitely. Do not change ADS-B TLS or watchdog policy. Avoid
  querying a nonexistent AP interface in LAN mode and report the C3's reset
  reason without assuming a second CPU. Keep native WiFiManager credential handling.
- Reject incomplete common/component forms before any settings writes using a
  final version marker and required non-checkbox fields. Return 400 without
  saving; reject GET on the common save endpoint with 405. Unchecked checkboxes
  remain valid. This is a completeness guard, not CSRF/authentication.

### Changed

- Compact aircraft label rows so hidden or unavailable fields do not leave blank lines (for example, altitude moves directly beneath the identity when aircraft type is disabled).
- Attach aircraft labels directly behind the displayed triangle with a small tail gap; follow its smoothed heading during turns while keeping text horizontal/centered. Remove both the earlier 24-candidate placement and the intermediate immutable screen-offset policy, including leader lines, collision checks/forecast and inward clamping. Accepted overlaps never reposition labels. Draw vectors and then triangles above text.
- Clip labels, triangles and vectors just inside the outer radar ring. Keep partially intersecting text/symbols visible during entry/exit, including trailing text after aircraft-center exit. Use bounded rectangular clip bands with no second framebuffer or heap allocation; preserve the separate city/airport admission margin.
- Animate symbols, vectors, labels, and bearing markers from a shared screen position, targeting a 125 ms frame interval without increasing ADS-B polling frequency. Use ground motion and source age with a ten-second prediction limit; blend small position corrections over one second and snap large corrections instead of inventing flight paths.
- Track the actual response acceptance timestamp so zoom/context changes cannot restart prediction of stale samples.
- Share the loaded smooth font between panel and framebuffer. Cache dashed ring pixels and airport range selection, bit-pack airport flags, and accelerate map projection/clipping to make animation practical without a second full-screen buffer.
- Stop BMP output after a failed/partial write and yield in small row batches rather than delaying every row.
- Bound BMP sends to eight seconds overall/two seconds per piece, coalesce TCP rows and wait for heap headroom before queuing more image data. Leave ADS-B TLS and watchdog safeguards unchanged.

### Added

- Persistent Common Setup checkbox `Show route instead of callsign when available` (`prefer_route`, NVS `showRoute`). Default enabled preserves existing behavior; disabled always uses callsign. Advance common form marker to `common-v2` so old forms cannot silently clear the new checkbox.
- Live ADS-B table below `/display`, backed by bounded `/aircraft.json` streaming from the accepted list (up to 64 rows, independent callsign/route, finite/null numbers, source-plus-receipt age and current distance units). No additional external ADS-B requests or full-list/JSON-tree allocation.
- Sequential table/BMP refresh, background pause/resume, retained last content on failure, stale/empty states, safe text rendering and horizontally scrollable mobile table. Serve build-versioned `/display.js` separately.
- Host C++ regressions for actual motion/smoothing, trailing attachment/translation invariance, text resize and exhaustive circle clipping, run with `scripts/test_radar_motion.py`. Add `aircraft_snapshot` and `display_settings` suites; settings use the real module with fake NVS, never live-device Save.
- Bounded Playwright WebDisplay PNG/GIF capture with automatic image polling disabled, through `scripts/capture_radar_animation.py`.
- Project decisions, code map, validation history, and outstanding limitations in `PROJECT_MEMORY.md`.
- Host portal writer/completeness regressions (`--test portal_html`) and a
  bounded live-browser portal check (`scripts/check_setup_pages.py`) that masks
  private fields, never submits a valid Save and retains a partial failure report.
- `scripts/check_live_display.py` verifies real sequential cycles and simulated background handling, then explicitly synthetic empty/stale/64-row/safe-text/mobile/error retention cases. Manual frame capture suppresses automatic table/image cycles.

### Validation

- Recovery candidate builds/flashes (176,116 B static RAM, 1,351,020 B flash),
  passes all five host suites and restores fresh snapshots after the upload
  reset. A 150.3-second serial sample records 48 successes/no captured errors;
  no reconnect happened in that sample, so it is not recurrence proof. The
  revision-1,100 check was deliberately stopped for route-memory iterations
  after 907.4 s / 298 successes, no failures or resets, final revision 362.
  It did NOT pass its >1,000/reconnect acceptance target.
- New `reusable_http_session` suite verifies 1,500 healthy reuses, peer-close
  replacement, deferred retry allocation and destructor ordering with fake
  transports. Live table browser regression passes; synthetic fixtures now use
  captured firmware assets locally instead of loading them repeatedly over Wi-Fi.
- Route-memory iterations measured roughly 43 KB recovered by releasing the
  position TLS owner. Keeping the old 48/56 KB contiguous guard still blocked
  routes; the exclusive slot budget permits real lookups. Iteration 2 observed
  41 ADS-B successes / 130 s with successful post-slot reconnections and one
  route read timeout (not a clean all-network-error-free test). Negative route
  lookups are expected for registration/GA callsigns. Subsequent filtered-JSON
  host tests pass. A test-build-only known-callsign probe on the ESP32 retrieved
  and parsed DLH400/FRA-JFK (HTTP 200, 1,112 ms); it never inserted a fake row in
  the feed. Its 75 s sample had 24 ADS-B successes and a historical heap minimum
  of 13,764 B. Later stress exposed allocation failures in the sequential-only
  and packed-frame-only variants; neither is considered accepted. The dedicated
  record-pool candidate builds/flashes (195,844 B static RAM, 1,350,712 B flash)
  and passes two three-pass setup runs and LiveDisplay browser regression.
  Its 300.6-second serial stress passes: 96 ADS-B successes, six route HTTP-404
  misses, six fresh position sessions, zero recorded errors/resets/pool spills,
  and 14,352 B historical minimum heap. Eight host suites pass. The final
  known-route/pool probe also passes: DLH400/FRA-JFK, HTTP 200 in 1,105 ms,
  25 ADS-B successes in 75 s, no captured errors or pool spills, minimum heap
  31,716 B. Restored normal firmware (probe absent) passes another 90.3-second
  sample: 28 ADS-B successes, no captured errors/resets/spills, minimum 24,928 B.
  Real feed aircraft now have positive routes (UAE58Q/DXB-DUS, GPX7FQ/PRN-DTM),
  not just the separate test callsign. LiveDisplay and three-pass setup tests
  pass again with unchanged settings. Bounded results are not an endurance or
  physical-panel acceptance guarantee. Detailed evidence: PROJECT_MEMORY.md.
- `pio run -e supermini` succeeds (ESP32-C3 Super Mini).
- Flashed successive animation iterations to the connected ESP32-C3 without erasing NVS; current motion, portal, snapshot and fake-NVS settings host suites pass, including timer rollover, stale samples, 600-update translation invariance, text resize, partial exit and exhaustive circle clipping.
- Background optimization improved measured normal-operation windows from about 3.4 FPS to roughly 7.3-7.8 FPS. Startup, synchronous HTTPS, and BMP-transfer windows are slower; uninterrupted 8 FPS is not guaranteed.
- An optimized intermediate build logged 53 successful ADS-B requests without recorded ADS-B/runtime errors or restarts in 180 seconds. Bounded four- and six-frame WebDisplay sequences transferred successfully; temporary overlaps were still visible in the denser sequence.
- Previous avoidance-enabled build: 183,276 B static RAM, 1,325,914 B flash. A 120-second control run logged 39 successful ADS-B responses without ADS-B/runtime errors or restarts; normal later windows measured about 7.5-7.6 FPS. A four-frame WebDisplay sequence showed moving aircraft and labels; temporary label/vector overlap remained visible. Historical minimum heap was 1,228 B during this test.
- Fixed-label follow-up builds and flashes successfully: 180,452 B static RAM, 1,323,724 B flash (2,824 B less static RAM). The new fixed-anchor host regressions pass, and a bounded four-frame WebDisplay sequence shows moving aircraft with fixed relative labels.
- Fixed-label 75-second smoke check: 24 successful ADS-B responses, no recorded ADS-B/runtime errors or restarts; normal later windows about 7.2-7.4 FPS. Historical minimum heap was 4,856 B in this bounded test, including the WebDisplay sequence.
- Final portal repair builds and flashes without NVS erase: 180,452 B static
  RAM and 1,336,032 B flash. Portal HTML/completeness and motion host suites pass.
- Two three-pass browser runs pass on the final firmware, including home,
  Wi-Fi, information, all 16 common and 26 component controls, unsaved DOM
  interactions, mobile layouts, OTA authentication and rejected incomplete
  saves. The later run also covers no-scan Wi-Fi. Settings match before/after;
  no valid Save/reset/restart/upload was submitted. Direct HTTP/1.0 and HTTP/1.1
  reads also return complete pages.
- Separate 120.6-second and 90.4-second serial samples during/after final web
  testing record 38 and 28 successful ADS-B responses respectively, with no
  recorded ADS-B/TLS/runtime errors or unintended restarts. Historical minimum
  heap is 3,440 B after the later run; idle free heap recovers to about 22.5 KB,
  largest block 9,204 B. Normal later animation windows are about 7.4-7.7 FPS;
  synchronous scans/transfers still pause animation. These are short smoke
  checks, not endurance or AP-provisioning/valid-save validation.
- Current trailing-label/route/table firmware builds and flashes without NVS
  erase: 176,100 B static RAM (53.7%), 1,350,754 B flash (73.6%), saving 4,352 B
  static RAM against the preceding portal build. All four host suites and
  JavaScript/Python syntax checks pass. Route persistence is host fake-NVS
  evidence, not a settings-changing target acceptance test.
- A first new-feature hardware attempt exposed BMP heap pressure followed by
  ADS-B errors/backoff; it is failed evidence. With image-only heap pacing and
  TCP coalescing, a 150.1-second sample records 47 successful ADS-B responses,
  no ADS-B errors/critical-heap warnings/resets, and one immediate disconnect
  when the first harness closed an in-flight preview. The harness now waits
  for completion before closing. Historical minimum heap remains 2,384 B.
- Paced firmware passes three-pass portal testing (17 common/26 component
  controls, unchanged settings, old/incomplete-save rejection, mobile and OTA
  authentication) and repeated live-table tests with no JavaScript errors.
  Snapshot/image requests remain sequential; browser-only fixtures exercise
  background handling, empty/stale/64-row data, safe text, mobile scrolling and
  retaining the previous table/image after errors.
- A bounded four-frame capture transfers fully with every consecutive image
  changed. Live screenshots show trailing labels and partial ring clipping;
  incidental traffic and screenshots are not exhaustive physical-panel or
  endurance evidence.
- Final serial check with DTR/RTS explicitly deasserted: 90 seconds of capture
  (91.5 seconds including final info reads), 28 ADS-B successes and no recorded
  ADS-B/TLS/runtime errors, portal aborts, critical-heap warnings or resets.
  Uptime also increases across serial open/close. Idle heap is about 27.3-27.8 KB,
  largest block 9,204 B; this boot's minimum is 2,816 B. This is a smoke check,
  not an endurance guarantee. The final browser harness also prevents its
  synthetic visibility getter from starting a request on tab close. Direct
  HTTP/1.0/1.1 reads of the new page/script/JSON routes pass.
- Physical-panel acceptance under sparse/dense traffic and endurance/reconnection testing of the final animation firmware remain pending. Transient heap during web inspection can still approach 1 KB; successful short samples do not establish long-term stability.

## 2026-11-06 — `ad442a0` Stabilize ADS-B TLS and chunked parsing

### Fixed

- Decode chunked HTTP responses before parsing ADS-B JSON, preventing HTTP chunk framing from being mistaken for response content.
- Reuse the ADS-B HTTPS keep-alive connection to avoid repeated TLS setup and allocation pressure.
- Avoid the optional weather TLS request when neither footer nor night mode needs weather/timezone API data.
- Move the full-screen RGB565 framebuffer to static storage and add memory diagnostics/backoff protections for optional enrichment.

### Hardware verification

- A three-hour serial-only ADS-B run recorded 3,587 successful responses with no ADS-B errors, backoffs, device restarts, or watchdog events.
- A 40-minute follow-up after the weather-request change recorded 798 successful responses with no TLS allocation failures or device restarts.
- These test results are bounded observations, not a guarantee of indefinite operation.
