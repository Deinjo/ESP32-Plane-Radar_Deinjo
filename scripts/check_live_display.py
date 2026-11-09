"""Bounded, read-only WebDisplay/table browser regression on a live device.

First checks real snapshot/BMP cycles; then clearly labelled browser-only
fixtures cover empty/stale/error/64-row data, safe text and mobile overflow.
No settings are saved, no external ADS-B requests are created by this script,
and no raw aircraft/settings values are written to the JSON report.
"""

import argparse
from datetime import datetime
from io import BytesIO
import json
import os
from pathlib import Path
import tempfile
from urllib.parse import urlsplit

from PIL import Image
from playwright.sync_api import sync_playwright, expect


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Device base URL")
    parser.add_argument("--channel")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    base = args.url.rstrip("/")
    temp = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
    temp = temp / "Temp" / "opencode" if os.name == "nt" else temp
    out = args.output_dir or temp / ("radar-live-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "valid_save_submitted": False, "stage": "live cycles"}
    errors, active = [], set()
    counts = {"/aircraft.json": 0, "/display.bmp": 0}
    max_inflight = 0
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, channel=args.channel)
        try:
            page = browser.new_page(viewport={"width": 1100, "height": 1000})
            page.on("pageerror", lambda error: errors.append(type(error).__name__))

            def started(request):
                nonlocal max_inflight
                path = urlsplit(request.url).path
                if path in counts:
                    counts[path] += 1
                    active.add(request)
                    max_inflight = max(max_inflight, len(active))

            page.on("request", started)
            page.on("requestfinished", lambda request: active.discard(request))
            page.on("requestfailed", lambda request: active.discard(request))
            with page.expect_response(lambda r: urlsplit(r.url).path == "/display.js", timeout=30000) as script_response:
                page_response = page.goto(base + "/display", wait_until="networkidle", timeout=30000)
            fixture_html = page_response.body()
            fixture_script = script_response.value.body()
            expect(page.locator("#image-state")).to_contain_text("Display updated", timeout=20000)
            expect(page.locator("#data-state")).to_have_attribute("data-state", "live", timeout=20000)
            page.evaluate("window.firstImageUrl = document.querySelector('#display').src")
            with page.expect_request_finished(lambda r: urlsplit(r.url).path == "/display.bmp", timeout=25000):
                with page.expect_response(lambda r: urlsplit(r.url).path == "/aircraft.json", timeout=20000):
                    pass
            page.wait_for_function("document.querySelector('#display').src !== window.firstImageUrl")
            page.wait_for_function("document.querySelector('#display').complete && document.querySelector('#display').naturalWidth === 240")
            assert counts["/aircraft.json"] >= 2 and counts["/display.bmp"] >= 2
            assert max_inflight == 1, "Snapshot and BMP requests must not overlap"
            report["live_requests"] = dict(counts)
            report["max_live_requests_inflight"] = max_inflight
            report["live_table_rows"] = page.locator("#aircraft-body tr:has(td:not(.empty))").count()
            page.screenshot(path=str(out / "display-live.png"), full_page=True)

            # Simulate the visibility event deterministically, without touching
            # any of the user's browser windows or claiming an OS-level test.
            page.evaluate("""() => {
                window.testHidden = true;
                Object.defineProperty(document, 'hidden', {get: () => window.testHidden, configurable: true});
                document.dispatchEvent(new Event('visibilitychange'));
            }""")
            page.wait_for_timeout(1500)  # an already-started image may finish
            before = dict(counts)
            page.wait_for_timeout(6000)
            assert counts == before, "Background state started more requests"
            report["simulated_background_pause"] = True
            page.evaluate("window.firstImageUrl = document.querySelector('#display').src")
            page.evaluate("window.testHidden = false; document.dispatchEvent(new Event('visibilitychange'))")
            expect(page.locator("#data-state")).to_have_attribute("data-state", "live", timeout=20000)
            assert counts["/aircraft.json"] > before["/aircraft.json"]
            page.wait_for_function("document.querySelector('#display').src !== window.firstImageUrl")
            report["foreground_resumes"] = True
            # Restore a hidden state before closing: a real close emits another
            # visibility event, which our synthetic getter must not call visible.
            page.evaluate("window.testHidden = true; document.dispatchEvent(new Event('visibilitychange'))")
            page.close()

            report["stage"] = "browser-only fixtures"
            page = browser.new_page(viewport={"width": 390, "height": 844})
            page.on("pageerror", lambda error: errors.append(type(error).__name__))
            # These are browser-only cases: serve the actual live firmware's
            # captured assets locally, rather than stress/reload the ESP32 for
            # every synthetic response (and conflate weak Wi-Fi with JS logic).
            page.route("**/display", lambda route: route.fulfill(content_type="text/html; charset=utf-8", body=fixture_html))
            page.route("**/display.js?*", lambda route: route.fulfill(content_type="application/javascript; charset=utf-8", body=fixture_script))
            report["fixture_assets_captured_from_live_firmware"] = True
            fixture = {"has_sample": True, "revision": 1, "sample_age_ms": 100,
                       "distance_unit": "km", "aircraft": []}
            failing = False

            def snapshot(route):
                if failing:
                    route.fulfill(status=503, content_type="text/plain", body="Synthetic failure")
                else:
                    route.fulfill(json=fixture)

            page.route("**/aircraft.json", snapshot)
            image_bytes = BytesIO()
            Image.new("RGB", (1, 1), "black").save(image_bytes, format="PNG")
            tiny_png = image_bytes.getvalue()
            image_failing = False

            def preview(route):
                if image_failing:
                    route.fulfill(status=503, content_type="text/plain", body="Synthetic image failure")
                else:
                    route.fulfill(content_type="image/png", body=tiny_png)

            page.route("**/display.bmp?*", preview)
            page.goto(base + "/display", wait_until="networkidle", timeout=30000)
            expect(page.locator("#aircraft-body")).to_contain_text("No aircraft")
            report["empty_feed"] = True
            fixture["has_sample"] = False
            fixture["sample_age_ms"] = None
            page.reload(wait_until="networkidle")
            expect(page.locator("#data-state")).to_contain_text("Waiting for ADS-B")
            report["no_sample_yet"] = True

            fixture["has_sample"] = True
            fixture["sample_age_ms"] = 60000
            page.reload(wait_until="networkidle")
            expect(page.locator("#data-state")).to_have_attribute("data-state", "stale")
            report["stale_sample"] = True

            fixture["sample_age_ms"] = 100
            fixture["distance_unit"] = "mi"
            fixture["aircraft"] = [{"callsign": "<img src=x onerror=alert(1)>", "route": "AAA-BBB",
                "hex": f"{i:06x}", "type": "TEST-TYPE", "altitude": "12345 ft",
                "speed_knots": 200, "track_deg": 90, "distance": 12.3, "position_age_s": 1.2}
                for i in range(64)]
            page.reload(wait_until="networkidle")
            expect(page.locator("#aircraft-body tr")).to_have_count(64)
            assert page.locator("#aircraft-body img").count() == 0
            expect(page.locator("#distance-heading")).to_have_text("Distance (mi)")
            assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
            assert page.locator(".table-scroll").evaluate("e => e.scrollWidth > e.clientWidth")
            page.screenshot(path=str(out / "display-mobile-synthetic.png"), full_page=True)
            report["synthetic_64_rows_safe_text_mobile_scroll"] = True
            expect(page.locator("#image-state")).to_contain_text("Display updated")
            last_image = page.locator("#display").get_attribute("src")
            failing = True
            image_failing = True
            expect(page.locator("#data-state")).to_have_attribute("data-state", "error", timeout=20000)
            expect(page.locator("#aircraft-body tr")).to_have_count(64)
            report["failed_request_keeps_table"] = True
            expect(page.locator("#image-state")).to_contain_text("keeping the last image", timeout=20000)
            assert page.locator("#display").get_attribute("src") == last_image
            report["failed_request_keeps_image"] = True
            assert not errors, "Browser JavaScript errors"
            report["passed"] = True
            report["stage"] = "complete"
        finally:
            browser.close()
            report["javascript_errors"] = errors
            (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            print(json.dumps({"output_directory": str(out), **report}, indent=2), flush=True)


if __name__ == "__main__":
    main()
