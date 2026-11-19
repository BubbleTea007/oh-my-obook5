#!/usr/bin/env python3
"""Locate or download the official OBOOK5 V1.8.1 application package.

The official package is never stored in Git. Scripts call this module and it:

  1. reuses a verified download in downloads/
  2. otherwise downloads the official .img and .md5 from e.obook.com.cn
  3. falls back to a local firmware.zip/firmware.md5 if the site is unreachable
  4. extracts the package into firmware/ for the OTA builders

The existing firmware/ tree is reused when present, so normal local builds do
not touch the network.
"""

from __future__ import annotations

import hashlib
import re
import shutil
import tarfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT / "firmware"
DOWNLOADS = ROOT / "downloads"
CACHE_IMAGE = DOWNLOADS / "official.img"
CACHE_MD5 = DOWNLOADS / "official.img.md5"
LOCAL_IMAGE = ROOT / "firmware.zip"
LOCAL_MD5 = ROOT / "firmware.md5"

OFFICIAL_IMAGE_URL = (
    "http://e.obook.com.cn/szdownload/1.0epd86s2_pro192.img"
)
OFFICIAL_MD5_URL = (
    "http://e.obook.com.cn/szdownload/1.0epd86s2_pro192.img.md5"
)


def log(message: str) -> None:
    print(message, flush=True)


def md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_md5(path: Path) -> str:
    text = path.read_text(encoding="utf-8", errors="replace").strip()
    match = re.search(r"\b([0-9a-fA-F]{32})\b", text)
    if not match:
        raise ValueError(f"no MD5 digest in {path}")
    return match.group(1).lower()


def verified(image: Path, md5_file: Path) -> bool:
    try:
        return image.is_file() and md5(image) == read_md5(md5_file)
    except (OSError, ValueError):
        return False


def download(url: str, dest: Path, timeout: float = 30.0) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    log(f"downloading {url}")
    request = urllib.request.Request(
        url, headers={"User-Agent": "workshop-ota/1.0"})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            with part.open("wb") as f:
                shutil.copyfileobj(response, f, 1024 * 256)
        part.replace(dest)
    finally:
        part.unlink(missing_ok=True)


def ensure_official_image() -> tuple[Path, Path]:
    """Return a verified official image and its MD5 file."""
    if verified(CACHE_IMAGE, CACHE_MD5):
        log(f"official package cache: {CACHE_IMAGE}")
        return CACHE_IMAGE, CACHE_MD5

    try:
        download(OFFICIAL_MD5_URL, CACHE_MD5)
        download(OFFICIAL_IMAGE_URL, CACHE_IMAGE, timeout=120.0)
        if verified(CACHE_IMAGE, CACHE_MD5):
            return CACHE_IMAGE, CACHE_MD5
        raise ValueError("downloaded package MD5 mismatch")
    except (OSError, ValueError, urllib.error.URLError) as exc:
        log(f"[!] official download unavailable: {exc}")

    if verified(LOCAL_IMAGE, LOCAL_MD5):
        log(f"using local fallback: {LOCAL_IMAGE}")
        return LOCAL_IMAGE, LOCAL_MD5

    raise SystemExit(
        "official package is unavailable and no verified local fallback "
        f"exists.\n  download URL: {OFFICIAL_IMAGE_URL}\n"
        f"  local fallback: {LOCAL_IMAGE} + {LOCAL_MD5}"
    )


def ensure_firmware_tree() -> Path:
    """Return firmware/ extracted from the official package."""
    marker = FIRMWARE / "image/opt/Qtopia/home/version"
    if marker.is_file():
        return FIRMWARE

    image, _ = ensure_official_image()
    log(f"extracting {image} -> {FIRMWARE}")
    FIRMWARE.mkdir(parents=True, exist_ok=True)
    with tarfile.open(image, "r:gz") as archive:
        archive.extractall(FIRMWARE)
    if not marker.is_file():
        raise SystemExit(f"official package did not create {marker}")
    return FIRMWARE


if __name__ == "__main__":
    start = time.time()
    path = ensure_firmware_tree()
    log(f"firmware ready: {path} ({time.time() - start:.1f}s)")
