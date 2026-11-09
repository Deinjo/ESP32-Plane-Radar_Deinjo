"""Compile and run a selected firmware host regression, including memory helpers.

Use --compiler with a C++ compiler (g++, clang++, or zig). No Arduino SDK,
device connection or firmware changes are needed. Build output stays in Temp.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", help="Path to g++, clang++, or zig")
    parser.add_argument(
        "--test", choices=("radar_motion", "portal_html", "aircraft_snapshot",
                           "display_settings", "reusable_http_session", "flight_data_json",
                           "round_frame_layout", "tls_record_pool"),
        default="radar_motion", help="Host regression suite")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("g++") or shutil.which("clang++") or shutil.which("zig")
    if not compiler:
        parser.error("No host C++ compiler found; supply --compiler PATH")
    root = Path(__file__).resolve().parents[1]
    temp_root = Path(os.environ.get("LOCALAPPDATA", tempfile.gettempdir()))
    temp_root = temp_root / "Temp" / "opencode" if os.name == "nt" else temp_root
    temp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="radar-motion-", dir=temp_root) as work:
        name = args.test + "_test"
        binary = Path(work) / (name + ".exe" if os.name == "nt" else name)
        command = [str(compiler)]
        if Path(compiler).stem.lower() == "zig":
            command.append("c++")
        if args.test == "display_settings":
            command += ["-I", str(root / "tests" / "stubs" / "settings"),
                         str(root / "src" / "services" / "display_settings.cpp")]
        if args.test == "flight_data_json":
            command += ["-I", str(root / ".pio" / "libdeps" / "supermini" / "ArduinoJson" / "src")]
        command += ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-O0", "-g",
                    "-I", str(root / "include"), str(root / "tests" / (name + ".cpp")),
                    "-o", str(binary)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(binary)], cwd=work, check=True)


if __name__ == "__main__":
    main()
