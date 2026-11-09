"""Bounded browser regression for live setup pages without saving valid settings.

Requires Playwright. --check-rejections sends only incomplete POSTs after the
new form markers have been verified. No valid Save, restart or reset is used.
Values stay in memory; screenshots mask coordinates/passwords and the report
contains no setting values. Browser closes even on failure.
"""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import tempfile

from playwright.sync_api import sync_playwright


COMMON = ("radar_lat", "radar_lon", "use_miles", "use_iata", "prefer_route", "show_runways",
          "range_index", "show_footer", "show_weather", "temp_f", "alt_m",
          "clock_24", "text_scale", "night_enabled", "night_start", "night_end",
          "ota_password")
COLORS = ("color_bg", "color_grid", "color_center", "color_label", "color_aircraft",
          "color_track", "color_tag_type", "color_tag_alt", "color_runway",
          "color_runway_label", "color_road", "color_road_primary", "color_city",
          "color_footer")
COMPONENT = COLORS + tuple(name + "_visible" for name in COLORS
                          if name not in ("color_bg", "color_footer"))


def state(page):
    return page.locator("form input[name]").evaluate_all("""items =>
        items.map(e => [e.name, e.value, e.type === 'checkbox' ? e.checked : null])
    """)


def check_fields(page, names, marker):
    for name in names:
        field = page.locator(f"[name='{name}']")
        assert field.count() == 1 and field.is_visible(), f"Missing/hidden field: {name}"
    assert page.locator("[name='_form_complete']").input_value() == marker
    assert page.locator("form [type='submit']").count() == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Device base URL")
    parser.add_argument("--channel", help="Installed browser, e.g. msedge")
    parser.add_argument("--passes", type=int, default=3)
    parser.add_argument("--check-rejections", action="store_true")
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if not 1 <= args.passes <= 5:
        parser.error("Use 1..5 passes")
    base = args.url.rstrip("/")
    temp = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
    temp = temp / "Temp" / "opencode" if os.name == "nt" else temp
    out = args.output_dir or temp / ("radar-setup-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    report = {"pages": [], "rejected_requests": [], "valid_save_submitted": False,
              "passed": False}
    errors = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, channel=args.channel)
        try:
            page = browser.new_page(viewport={"width": 950, "height": 900})
            # Never persist runtime error text that could contain field values.
            page.on("pageerror", lambda error: errors.append(type(error).__name__))
            report["stage"] = "shared assets"
            for path, content_type in (("/portal.css", "text/css"),
                                       ("/portal.js", "application/javascript")):
                response = page.request.get(base + path, timeout=20000)
                assert response.status == 200, f"Asset unavailable: {path}"
                assert response.headers.get("content-type", "").startswith(content_type)
                assert len(response.body()) == int(response.headers["content-length"])
                report["pages"].append({"path": path, "status": 200,
                                        "bytes": len(response.body())})
            initial = {}
            for iteration in range(args.passes):
                for path in ("/", "/wifi", "/0wifi", "/info"):
                    report["stage"] = f"pass {iteration + 1}: {path}"
                    response = page.goto(base + path, wait_until="networkidle", timeout=30000)
                    assert response.status == 200, f"{path}: HTTP {response.status}"
                    assert response.body().rstrip().endswith(b"</html>"), f"Truncated response: {path}"
                    assert page.locator("link[href^='/portal.css?v=']").count() == 1
                    assert page.locator("script[src^='/portal.js?v='][defer]").count() == 1
                    assert page.locator("body").evaluate(
                        "e => getComputedStyle(e).backgroundColor") == "rgb(13, 21, 30)"
                    if path == "/info":
                        assert page.locator("dl").count() >= 1
                        assert page.locator("a.home-link[href='/']").count() == 1
                    elif path in ("/wifi", "/0wifi"):
                        for name in ("s", "p"):
                            assert page.locator(f"input[name='{name}']").is_visible()
                        assert page.locator("a.home-link[href='/']").count() == 1
                        assert page.locator("#p").input_value() == ""
                        assert page.evaluate("typeof c === 'function' && typeof f === 'function'")
                    else:
                        assert page.locator("a[href='/display']").count() == 1
                        for action in ("/wifi", "/param", "/info", "/firmware"):
                            assert page.locator(f"form[action='{action}']").count() == 1
                    report["pages"].append({"path": path, "pass": iteration + 1, "status": 200})
                for path, names, marker in (("/param", COMMON, "common-v2"),
                                            ("/component", COMPONENT, "component-v1")):
                    report["stage"] = f"pass {iteration + 1}: {path}"
                    response = page.goto(base + path, wait_until="networkidle", timeout=30000)
                    assert response.status == 200, f"{path}: HTTP {response.status}"
                    assert response.body().rstrip().endswith(b"</html>"), f"Truncated response: {path}"
                    check_fields(page, names, marker)
                    current = state(page)
                    if path not in initial:
                        initial[path] = current
                    assert current == initial[path], f"Settings changed on read: {path}"
                    if path == "/param":
                        assert page.locator("#ota_password").input_value() == ""
                        assert page.locator("a[href='/component']").count() == 1
                        assert page.locator("#range_value").inner_text().strip()
                        assert page.locator("#text_scale_value").inner_text().strip().endswith("%")
                    report["pages"].append({"path": path, "pass": iteration + 1,
                        "named_settings": len(names), "complete": True,
                        "transfer_encoding": response.headers.get("transfer-encoding")})
                    if iteration == 0:
                        masks = ([page.locator("#radar_lat"), page.locator("#radar_lon"),
                                  page.locator("#ota_password")] if path == "/param" else [])
                        page.screenshot(path=str(out / (path[1:] + ".png")), full_page=True, mask=masks)
                        # These controls only change the browser DOM until a
                        # valid Save. Exercise JavaScript without submitting it.
                        if path == "/param":
                            page.locator("#range_index").evaluate("""e => {
                                e.value = '0'; e.dispatchEvent(new Event('input', {bubbles: true}));
                            }""")
                            assert page.locator("#range_value").inner_text().strip() == "5 km"
                            page.locator("#text_scale").evaluate("""e => {
                                e.value = '80'; e.dispatchEvent(new Event('input', {bubbles: true}));
                            }""")
                            assert page.locator("#text_scale_value").inner_text().strip() == "80%"
                        else:
                            page.locator("[data-reset='color_bg']").click()
                            field = page.locator("#color_bg")
                            assert field.input_value() == field.get_attribute("data-default").lower()
                        page.reload(wait_until="networkidle", timeout=30000)
                        assert state(page) == initial[path], f"Unsaved DOM change persisted: {path}"

            if args.check_rejections:
                report["stage"] = "incomplete save rejection"
                # Marker verification above prevents running this against the
                # old, unguarded firmware. Never submit a complete valid form.
                cases = (("/paramsave", {}),
                         ("/paramsave", {"_form_complete": "common-v2"}),
                         ("/paramsave", {"_form_complete": "common-v1"}),
                         ("/component-save", {}),
                         ("/component-save", {"_form_complete": "component-v1"}))
                for path, payload in cases:
                    response = page.request.post(base + path, form=payload, timeout=20000)
                    assert response.status == 400, f"Incomplete form accepted: {path}"
                    report["rejected_requests"].append({"path": path, "status": response.status})
                response = page.request.get(base + "/paramsave", timeout=20000)
                assert response.status == 405, "GET must not save settings"
                report["rejected_requests"].append({"path": "/paramsave", "method": "GET", "status": 405})
                for path in initial:
                    page.goto(base + path, wait_until="networkidle", timeout=30000)
                    assert state(page) == initial[path], f"Rejected request changed settings: {path}"

            report["stage"] = "OTA authentication"
            response = page.request.get(base + "/firmware", timeout=20000)
            assert response.status == 401, "Firmware page must require authentication"
            report["ota_requires_authentication"] = True
            report["stage"] = "mobile layout"
            page.set_viewport_size({"width": 390, "height": 844})
            for path, names, marker in (("/param", COMMON, "common-v2"),
                                        ("/component", COMPONENT, "component-v1")):
                page.goto(base + path, wait_until="networkidle", timeout=30000)
                check_fields(page, names, marker)
                assert page.evaluate("document.documentElement.scrollWidth <= innerWidth"), \
                    f"Mobile overflow: {path}"
                assert state(page) == initial[path], f"Settings changed: {path}"
                masks = ([page.locator("#radar_lat"), page.locator("#radar_lon"),
                          page.locator("#ota_password")] if path == "/param" else [])
                page.screenshot(path=str(out / (path[1:] + "-mobile.png")), full_page=True, mask=masks)
            report["mobile_forms_complete"] = True
            assert not errors, "Browser JavaScript errors"
            report["settings_unchanged"] = True
            report["passed"] = True
            report["stage"] = "complete"
        finally:
            report["javascript_errors"] = errors
            browser.close()
            (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
            print("REPORT " + str(out / "report.json"), flush=True)
    print(json.dumps({"output_directory": str(out), **report}, indent=2))


if __name__ == "__main__":
    main()
