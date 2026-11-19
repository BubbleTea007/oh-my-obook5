#!/usr/bin/env python3
"""
Build the workshop launcher and its bundled apps for OBOOK5.

This script only compiles binaries. It does not create an OTA package and it
does not talk to the device.

Artifacts:
  workshop/bin/workshop
  workshop/app/2048/bin/2048
  workshop/app/debug/bin/debug

Usage:
  python3 build_apps.py
  python3 build_apps.py --only workshop
  python3 build_apps.py --only 2048 --only debug
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WORKSHOP = ROOT / "workshop"
APP = WORKSHOP / "app"

WORKSHOP_BIN = WORKSHOP / "bin" / "workshop"
GAME_2048_SRC = APP / "2048" / "src" / "game2048.c"
GAME_2048_BIN = APP / "2048" / "bin" / "2048"
DEBUG_BIN = APP / "debug" / "bin" / "debug"


def log(message: str) -> None:
    print(message, flush=True)


def run(cmd: list[str], cwd: Path = ROOT) -> None:
    log("+ " + " ".join(str(part) for part in cmd))
    subprocess.run(cmd, cwd=str(cwd), check=True)


def require_zig() -> None:
    if not shutil.which("zig"):
        raise SystemExit("zig not found in PATH")


def build_workshop() -> None:
    log("[workshop]")
    run([str(WORKSHOP / "build_obook.sh")])


def build_2048() -> None:
    log("[2048]")
    if not GAME_2048_SRC.is_file():
        raise SystemExit(f"missing source: {GAME_2048_SRC}")
    GAME_2048_BIN.parent.mkdir(parents=True, exist_ok=True)
    run([
        "zig", "cc",
        "-target", "mipsel-linux-musleabi",
        "-mcpu=mips32r2",
        "-static", "-Os",
        str(GAME_2048_SRC),
        "-o", str(GAME_2048_BIN),
    ])


def build_debug() -> None:
    log("[debug]")
    run([str(APP / "debug" / "build.sh")], cwd=APP / "debug")


def verify() -> None:
    expected = {
        "workshop": (WORKSHOP_BIN, "workshop"),
        "2048": (GAME_2048_BIN, "2048"),
        "debug": (DEBUG_BIN, "debug"),
    }
    log("")
    log("artifacts:")
    for name, (path, _) in expected.items():
        if not path.is_file():
            raise SystemExit(f"missing {name} binary: {path}")
        size = path.stat().st_size
        log(f"  {name:8s} {size / 1024 / 1024:7.2f} MiB  {path}")

    if shutil.which("file"):
        for name, (path, _) in expected.items():
            result = subprocess.run(
                ["file", str(path)],
                check=False, capture_output=True, text=True,
            )
            if "MIPS" not in result.stdout:
                raise SystemExit(
                    f"{name} does not look like a MIPS binary:\n{result.stdout}"
                )


def build_all(selected: list[str] | None = None,
              verify_artifacts: bool = True) -> None:
    selected = selected or ["workshop", "2048", "debug"]
    require_zig()

    if "workshop" in selected:
        build_workshop()
    if "2048" in selected:
        build_2048()
    if "debug" in selected:
        build_debug()

    if verify_artifacts:
        verify()


def main() -> int:
    parser = argparse.ArgumentParser(description="Build workshop and bundled apps")
    parser.add_argument(
        "--only", action="append", choices=("workshop", "2048", "debug"),
        help="build only the named artifact (repeatable; default: all)",
    )
    parser.add_argument(
        "--no-verify", action="store_true",
        help="skip artifact existence/MIPS checks",
    )
    args = parser.parse_args()

    build_all(args.only, verify_artifacts=not args.no_verify)

    log("")
    log("build complete; next: python3 package_ota.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
