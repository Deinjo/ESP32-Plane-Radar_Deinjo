"""Capture a bounded WebDisplay sequence, with automatic BMP polling disabled.

Example: python scripts/capture_radar_animation.py --url http://DEVICE/display
Requires Playwright with Chromium (or --channel msedge) and Pillow. Only the inspection tab is
changed; no firmware settings are written. Evidence is saved outside the repo.
"""

import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import tempfile
import time

from PIL import Image, ImageChops, ImageDraw
from playwright.sync_api import sync_playwright


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--frames", type=int, default=6)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--channel", help="Installed browser channel, e.g. msedge")
    args = parser.parse_args()
    if not 2 <= args.frames <= 12 or args.interval < 0.5:
        parser.error("Use 2..12 frames and an interval of at least 0.5 seconds")
    base = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
    base = base / "Temp" / "opencode" if os.name == "nt" else base
    out = args.output_dir or base / ("radar-animation-" + datetime.now().strftime("%Y%m%d-%H%M%S"))
    out.mkdir(parents=True, exist_ok=True)
    images, samples = [], []
    started = time.monotonic()
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, channel=args.channel)
        try:
            page = browser.new_page(viewport={"width": 800, "height": 950})
            page.add_init_script("window.__RADAR_MANUAL_PREVIEW__ = true; window.setInterval = () => 0;")
            page.goto(args.url, wait_until="networkidle", timeout=20000)
            for index in range(args.frames):
                if index:
                    page.wait_for_timeout(args.interval * 1000)
                    page.evaluate("""async () => {
                        const img = document.querySelector('#display');
                        await new Promise((resolve, reject) => {
                            const timeout = setTimeout(() => reject(
                                new Error('BMP request timed out')), 15000);
                            img.onload = () => { clearTimeout(timeout); resolve(); };
                            img.onerror = () => { clearTimeout(timeout);
                                reject(new Error('BMP request failed')); };
                            img.src = '/display.bmp?inspection=' + Date.now();
                        });
                    }""")
                image = page.locator("#display")
                image.evaluate("img => img.decode()")
                path = out / f"frame-{index:02d}.png"
                image.screenshot(path=str(path), timeout=15000)
                images.append(Image.open(path).convert("RGB"))
                samples.append({"frame": index, "elapsed_s": round(time.monotonic() - started, 2),
                                "file": path.name})
        finally:
            browser.close()

    width, height = images[0].size
    sheet = Image.new("RGB", (width * 2, (height + 24) * ((len(images) + 1) // 2)), "#14202b")
    draw = ImageDraw.Draw(sheet)
    for index, image in enumerate(images):
        x, y = (index % 2) * width, (index // 2) * (height + 24)
        draw.text((x + 8, y + 4), f"Frame {index} - {samples[index]['elapsed_s']} s", fill="white")
        sheet.paste(image, (x, y + 24))
    sheet.save(out / "sequence.png")
    durations = [int((b['elapsed_s'] - a['elapsed_s']) * 1000)
                 for a, b in zip(samples, samples[1:])]
    durations.append(durations[-1])
    images[0].save(out / "sequence.gif", save_all=True, append_images=images[1:],
                   duration=durations, loop=0)
    changes = [ImageChops.difference(a, b).getbbox() is not None
               for a, b in zip(images, images[1:])]
    report = {"frames": samples, "successive_frames_changed": changes,
              "automatic_refresh_disabled": True,
              "note": "Live ADS-B changes between frames; this is bounded visual evidence, not an FPS measurement."}
    (out / "capture.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({"output_directory": str(out), **report}, indent=2))


if __name__ == "__main__":
    main()
