#!/usr/bin/env python3
"""Create the generated PlatformIO source tree for the OBOOK5 target.

Upstream and simulator stay untouched: every source file is a symlink from
this project's src/ directory.  PlatformIO sees a normal project tree, while
edits still go to the original files (or to crossmux/port/obook5).
"""

from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PORT = HERE.parent
CROSSMUX = PORT.parent
UPSTREAM = CROSSMUX / "upstream"
SHIMS = PORT / "shims"
SRC = HERE / "src"

# Same exclusions as upstream's simulator env.
UPSTREAM_EXCLUDES = {
    "network/FirmwareFlasher.cpp",
    "network/OtaBootSwitch.cpp",
    "network/OtaUpdater.cpp",
    "platform/skip_efuse_blk_check.c",
    "platform/S31NimbleMemory.cpp",
    # Device-side content protection needs wolfSSL; the simulator provides a
    # host stub (simulator/src/firmware_link_stubs.cpp) instead.  The matching
    # lib/Epub source is excluded by exclude_content_protection.py.
}

# The SDL-backed HAL and main() are replaced by port/obook5.  These files are
# not copied into port/shims, but keep the filter so a refreshed shim snapshot
# can be dropped in without reintroducing them.
SIMULATOR_EXCLUDES = {
    "HalDisplay.cpp",
    "HalGPIO.cpp",
    "simulator_main.cpp",
}

OVERLAY_SOURCES = {
    "HalDisplay.cpp": PORT / "obook5" / "HalDisplay.cpp",
    "HalGPIO.cpp": PORT / "obook5" / "HalGPIO.cpp",
    "main.cpp": PORT / "obook5" / "main.cpp",
}


def reset_tree() -> None:
    if SRC.exists():
        shutil.rmtree(SRC)
    SRC.mkdir(parents=True)


def link_file(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    os.symlink(source, target)


HEADER_SUFFIXES = (".h", ".hpp", ".hh", ".inc")


def collect_source_files(root: Path, excludes: set[str]) -> list[Path]:
    result: list[Path] = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in (".c", ".cpp", ".cc"):
            continue
        rel = path.relative_to(root).as_posix()
        if rel in excludes:
            continue
        if rel.startswith("network/html/") and rel.endswith(".cpp"):
            continue
        result.append(path)
    return result


def link_headers(root: Path, target_root: Path) -> int:
    """Mirror headers beside their sources.

    Quoted includes are resolved relative to the source path as spelled, and
    the staged sources are symlinks, so sibling headers must exist in the
    staging tree too.
    """
    count = 0
    for path in sorted(root.rglob("*")):
        if path.suffix not in HEADER_SUFFIXES:
            continue
        rel = path.relative_to(root)
        link_file(path, target_root / rel)
        count += 1
    return count


def main() -> int:
    if not (UPSTREAM / "src").is_dir():
        print("upstream submodule is missing; run git submodule update --init", file=sys.stderr)
        return 1
    if not SHIMS.is_dir():
        print("port/shims is missing", file=sys.stderr)
        return 1

    reset_tree()

    count = 0
    for path in collect_source_files(UPSTREAM / "src", UPSTREAM_EXCLUDES):
        rel = path.relative_to(UPSTREAM / "src")
        link_file(path, SRC / "upstream" / rel)
        count += 1
    count += link_headers(UPSTREAM / "src", SRC / "upstream")

    # lib/Epub/ContentProtection.cpp is part of the lib/Epub library, not
    # upstream/src, so it is excluded by exclude_content_protection.py.

    for path in collect_source_files(SHIMS, SIMULATOR_EXCLUDES):
        rel = path.relative_to(SHIMS)
        link_file(path, SRC / "shims" / rel)
        count += 1
    count += link_headers(SHIMS, SRC / "shims")

    # PlatformIO adds library include directories before build_flags, so the
    # simulator's headers must live in the project's own src/ root to shadow
    # same-named third-party headers (notably qrcode.h).  This mirrors the
    # upstream simulator build, where the simulator library is compiled with
    # -Isrc ahead of libdeps.
    for path in sorted(SHIMS.iterdir()):
        if path.is_file() and path.suffix == ".h":
            link_file(path, SRC / path.name)
            count += 1
        elif path.is_dir() and any(path.glob("*.h")):
            os.symlink(path, SRC / path.name)
            count += 1

    for name, source in OVERLAY_SOURCES.items():
        link_file(source, SRC / "obook5" / name)
        count += 1

    for name in ("fbdev.cpp", "evdev.cpp"):
        link_file(PORT / "linux" / name, SRC / "linux" / name)
        count += 1

    # ReleaseJsonParser has no library.json, so PlatformIO's LDF does not pull
    # it in from lib/ when only its header is referenced by the simulator OTA
    # shim.  Compile it as a project source instead.
    link_file(UPSTREAM / "lib" / "JsonParser" / "ReleaseJsonParser.cpp",
              SRC / "extra" / "ReleaseJsonParser.cpp")
    count += 1

    # The OPDS browser's LCP license parsing is freestanding (JsonSax only).
    # Its wolfSSL-backed sibling ProtectedBook/WolfsslCrypto stay out of this
    # build; simulator firmware_link_stubs.cpp provides openProtectedBook().
    link_file(UPSTREAM / "freeink-sdk" / "libs" / "book" / "ContentProtection" / "src" / "LcpLicense.cpp",
              SRC / "extra" / "LcpLicense.cpp")
    count += 1

    print(f"prepared {count} source links in {SRC}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
