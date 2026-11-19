#!/usr/bin/env python3
"""
Build an official V1.8.1 restore package that also removes legacy 2048/workshop
entries left by earlier custom OTA packages.

The upgrade scripts in the official package do not overwrite the live
/opt/image/opt/Qtopia/etc/mstarapplnk.conf, so an old [obook2048] entry can
survive a plain official restore. This package removes that entry, its icon,
its launcher, and the old /user payload before rebooting.

Usage:
  python3 restore_official.py
  sudo python3 jailbreak_ota.py --target <device-ip> \
      --package custom_restore.img --version V9.9.9
"""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

import official

ROOT = Path(__file__).resolve().parent
BASE = ROOT / "firmware"
OUT = ROOT / "restore_ota_pkg"
IMAGE = ROOT / "custom_restore.img"
MD5_FILE = ROOT / "custom_restore.img.md5"
VERSION_FILE = ROOT / "custom_restore.version"
VERSION = "V1.8.1"

CLEAN_BLOCK = r'''
# remove legacy 2048/workshop entries and files
clean_legacy_conf() {
    conf="$1"
    [ -f "$conf" ] || return 0
    awk '
        /^\[obook2048\]/ { skip=1; next }
        /^\[workshop\]/  { skip=1; next }
        /^\[/ { skip=0 }
        skip { next }
        /^list=/ {
            n=split($0,a,",");
            out=a[1];
            for (i=2; i<=n; i++)
                if (a[i] != "obook2048" && a[i] != "workshop")
                    out=out "," a[i];
            print out;
            next
        }
        { print }
    ' "$conf" > /tmp/clean_mstar.tmp
    mv -f /tmp/clean_mstar.tmp "$conf"
}

clean_legacy_zls() {
    conf="$1"
    [ -f "$conf" ] || return 0
    awk '
        /^apps=/ {
            n=split($0,a,",");
            out=a[1];
            for (i=2; i<=n; i++)
                if (a[i] != "obook2048" && a[i] != "workshop")
                    out=out "," a[i];
            print out;
            next
        }
        { print }
    ' "$conf" > /tmp/clean_zls.tmp
    mv -f /tmp/clean_zls.tmp "$conf"
}

echo "cleaning legacy 2048/workshop entries"
QTOPIASYS=/opt/image/opt/Qtopia
mount -o remount,rw /opt 2>/dev/null || true

for conf in "$QTOPIASYS/etc/mstarapplnk.conf" \
            "$QTOPIASYS"/products/*/opt/image/opt/Qtopia/etc/mstarapplnk.conf; do
    clean_legacy_conf "$conf"
done
for conf in "$QTOPIASYS/etc/zlsapplnk.conf" \
            "$QTOPIASYS"/products/*/opt/image/opt/Qtopia/etc/zlsapplnk.conf; do
    clean_legacy_zls "$conf"
done

rm -f "$QTOPIASYS/bin/obook2048" \
      "$QTOPIASYS/pics/mstar/obook2048.png" \
      "$QTOPIASYS/bin/workshop" \
      "$QTOPIASYS/pics/mstar/workshop.png"
sync
mount -o remount,ro /opt 2>/dev/null || true

rm -rf /user/workshop /user/firmware_restore
rm -f /user/firmware_official.zip
rm -f /user/bin/2048 \
      /user/bin/run_2048.sh \
      /user/bin/run_2048.sh.bak \
      /user/bin/obook2048 \
      /user/2048_best

cp -f "$QTOPIASYS/home/version" /home/version 2>/dev/null
cp -f "$QTOPIASYS/home/upgrade.xml" /home/upgrade.xml 2>/dev/null
sync
'''


def md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def add_block(path: Path) -> None:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    if "cleaning legacy 2048/workshop entries" in text:
        return
    marker = "[ -f /bin/get_epd_update_type ]"
    if marker not in text:
        raise SystemExit(f"cannot find install marker in {path}")
    text = text.replace(marker, CLEAN_BLOCK + "\n" + marker, 1)
    path.write_text(text, encoding="utf-8", errors="surrogateescape")
    path.chmod(0o755)


def update_version(root: Path) -> None:
    xml = root / "image/opt/Qtopia/home/upgrade.xml"
    ver = root / "image/opt/Qtopia/home/version"
    text = xml.read_text(encoding="utf-8", errors="surrogateescape")
    text, count = re.subn(
        r"<Version>[^<]*</Version>", f"<Version>{VERSION}</Version>", text)
    if count == 0:
        raise SystemExit(f"no <Version> tag in {xml}")
    xml.write_text(text, encoding="utf-8", errors="surrogateescape")
    ver.write_text(VERSION + "\n", encoding="utf-8")


def update_checksums(root: Path) -> None:
    lines = [line.strip() for line in
             (root / "upgrade_files.lst").read_text(encoding="utf-8").splitlines()
             if line.strip()]
    checksums = []
    for line in lines:
        rel = line[2:] if line.startswith("./") else line
        if rel in ("upgrade_files.lst", "upgrade_files.chksum"):
            continue
        path = root / rel
        if path.is_symlink() or not path.is_file():
            continue
        checksums.append(f"{line}:{md5(path)}")
    (root / "upgrade_files.chksum").write_text(
        "\n".join(checksums) + "\n", encoding="utf-8")


def main() -> int:
    global BASE
    BASE = official.ensure_firmware_tree()

    if OUT.exists():
        shutil.rmtree(OUT)
    shutil.copytree(BASE, OUT, symlinks=True)

    for script in (OUT / "script/upgraded.sh",
                   OUT / "script_ext/upgraded.sh"):
        add_block(script)

    update_version(OUT)
    update_checksums(OUT)

    if IMAGE.exists():
        IMAGE.unlink()
    env = os.environ.copy()
    env["COPYFILE_DISABLE"] = "1"
    subprocess.run(
        ["tar", "-czf", str(IMAGE),
         "image", "script", "script_ext",
         "upgrade_files.lst", "upgrade_files.chksum"],
        cwd=str(OUT), check=True, env=env,
    )

    digest = md5(IMAGE)
    MD5_FILE.write_text(digest + "\n", encoding="utf-8")
    VERSION_FILE.write_text(VERSION + "\n", encoding="utf-8")

    with tarfile.open(IMAGE, "r:gz") as archive:
        names = set(archive.getnames())
        for script in ("script/upgraded.sh", "script_ext/upgraded.sh"):
            data = archive.extractfile(script).read()
            if data.count(b"cleaning legacy 2048/workshop entries") != 1:
                raise SystemExit(f"cleanup block missing in {script}")

    print(f"built {IMAGE.name}: {IMAGE.stat().st_size} bytes")
    print(f"md5: {digest}")
    print(f"version: {VERSION}")
    print("serve with:")
    print("  sudo python3 jailbreak_ota.py --target <device-ip> "
          f"--package {IMAGE.name} --version V9.9.9")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
