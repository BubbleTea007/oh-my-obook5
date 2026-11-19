#!/usr/bin/env python3
"""
Package workshop and its bundled apps into an OBOOK5 OTA upgrade image.

The package carries one self-contained directory:

  workshop_files/
    app/                    -> /user/workshop/app/
    bin/workshop            -> /user/workshop/bin/workshop
    bin/workshop.sh         -> /user/workshop/bin/workshop.sh
    lib/                    -> /user/workshop/lib/
    fonts/                  -> /user/workshop/fonts/
    assets/                 -> /user/workshop/assets/
    qtopia/bin/workshop     -> /opt/image/opt/Qtopia/bin/workshop
    qtopia/pics/mstar/workshop.png
                            -> /opt/image/opt/Qtopia/pics/mstar/workshop.png

The upgrade script copies those directories to /user/workshop, installs the
Qtopia entry, and adds [workshop] to mstarapplnk.conf.

The package keeps the official version metadata from firmware/:

  image/opt/Qtopia/home/version
  image/opt/Qtopia/home/upgrade.xml

At the end of upgraded.sh those official values are written to /home/version
and /home/upgrade.xml. The OTA trigger version (for example V9.9.9) is only a
server-side value and is never installed as the system version.

This script does not compile anything. Run build_apps.py first.

Usage:
  python3 build_apps.py
  python3 package_ota.py
  python3 package_ota.py --version V9.9.10
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import tarfile
import tempfile
from pathlib import Path

import official

ROOT = Path(__file__).resolve().parent
BASE = ROOT / "firmware"
OUT = ROOT / "workshop_ota_pkg"
IMAGE = ROOT / "custom_workshop.img"
MD5_FILE = ROOT / "custom_workshop.img.md5"
VERSION_FILE = ROOT / "custom_workshop.version"

WORKSHOP = ROOT / "workshop"
APP = WORKSHOP / "app"
WORKSHOP_BIN = WORKSHOP / "bin" / "workshop"
WORKSHOP_SH = WORKSHOP / "bin" / "workshop.sh"
GAME_2048_BIN = APP / "2048" / "bin" / "2048"
DEBUG_BIN = APP / "debug" / "bin" / "debug"
FONT_ROOT = WORKSHOP / "fonts" / "LxgwWenKai"
FONT_DIR = FONT_ROOT / "fonts" / "TTF"
FONT_FILES = ("LXGWWenKai-Regular.ttf", "LXGWWenKai-Medium.ttf")
FONT_LICENSE = FONT_ROOT / "OFL.txt"

APPS = ("2048", "return_desktop", "debug")
PAYLOAD_NAME = "workshop_payload.tar.gz"
PAYLOAD_PATH = f"image/opt/Qtopia/share/upgrade/{PAYLOAD_NAME}"

DEFAULT_VERSION = "V99.0.0"

LAUNCHER = """#!/bin/sh
/bin/busybox setsid /user/workshop/bin/workshop.sh >/tmp/workshop_launch.log 2>&1 &
exit 0
"""

APP_ENTRY = """[workshop]
name=workshop
en_US_name=Workshop
args=
"""


INSTALL_BLOCK = r'''
# workshop install
install_workshop_conf() {
    conf="$1"
    [ -f "$conf" ] || return 0
    if ! grep -q '^\[workshop\]' "$conf"; then
        cat >> "$conf" <<'WORKSHOP_ENTRY'

[workshop]
name=workshop
en_US_name=Workshop
args=
WORKSHOP_ENTRY
    fi
    if ! grep -q '^list=.*workshop' "$conf"; then
        awk '
            /^\[app\]/ { inapp=1 }
            /^\[/ && $0 !~ /^\[app\]/ { inapp=0 }
            inapp && /^list=/ && $0 !~ /workshop/ { $0 = $0 ",workshop" }
            { print }
        ' "$conf" > /tmp/workshop_mstar.tmp
        mv -f /tmp/workshop_mstar.tmp "$conf"
    fi
}

install_workshop_zls() {
    conf="$1"
    [ -f "$conf" ] || return 0
    if ! grep -q '^apps=.*workshop' "$conf"; then
        awk '
            /^\[Applications\]/ { inapps=1 }
            /^\[/ && $0 !~ /^\[Applications\]/ { inapps=0 }
            inapps && /^apps=/ && $0 !~ /workshop/ { $0 = $0 ",workshop" }
            { print }
        ' "$conf" > /tmp/workshop_zls.tmp
        mv -f /tmp/workshop_zls.tmp "$conf"
    fi
}

WORKSHOP_PKG=/opt/image/opt/Qtopia/share/upgrade/workshop_payload.tar.gz
WORKSHOP_TMP=/tmp/workshop_install
if [ -f "$WORKSHOP_PKG" ]; then
    echo "installing workshop"
    rm -rf "$WORKSHOP_TMP"
    mkdir -p "$WORKSHOP_TMP"
    tar -xzf "$WORKSHOP_PKG" -C "$WORKSHOP_TMP"
    SRC="$WORKSHOP_TMP/workshop_files"

    mkdir -p /user/workshop
    for d in app bin lib fonts assets; do
        [ -d "$SRC/$d" ] || continue
        mkdir -p "/user/workshop/$d"
        cp -af "$SRC/$d/." "/user/workshop/$d/"
    done

    for app in 2048 debug return_desktop; do
        mkdir -p "/user/workshop/app/$app/data" "/user/workshop/app/$app/logs"
    done
    chmod 755 /user/workshop/bin/workshop \
        /user/workshop/bin/workshop.sh \
        /user/workshop/app/2048/bin/2048 \
        /user/workshop/app/debug/bin/debug 2>/dev/null

    if [ -d "$SRC/qtopia" ]; then
        QTOPIASYS=/opt/image/opt/Qtopia
        mount -o remount,rw /opt 2>/dev/null || true
        mkdir -p "$QTOPIASYS/bin" "$QTOPIASYS/pics/mstar"
        cp -f "$SRC/qtopia/bin/workshop" "$QTOPIASYS/bin/workshop"
        cp -f "$SRC/qtopia/pics/mstar/workshop.png" "$QTOPIASYS/pics/mstar/workshop.png"
        chmod 755 "$QTOPIASYS/bin/workshop" 2>/dev/null
        chmod 644 "$QTOPIASYS/pics/mstar/workshop.png" 2>/dev/null
        install_workshop_conf "$QTOPIASYS/etc/mstarapplnk.conf"
        for conf in "$QTOPIASYS"/products/*/opt/image/opt/Qtopia/etc/mstarapplnk.conf; do
            install_workshop_conf "$conf"
        done

        install_workshop_zls "$QTOPIASYS/etc/zlsapplnk.conf"
        for conf in "$QTOPIASYS"/products/*/opt/image/opt/Qtopia/etc/zlsapplnk.conf; do
            install_workshop_zls "$conf"
        done

        sync
        mount -o remount,ro /opt 2>/dev/null || true
    fi

    find /user/workshop -name '._*' -exec rm -f {} \;
    rm -rf "$WORKSHOP_TMP"
    sync
fi

'''


def log(message: str) -> None:
    print(message, flush=True)


def md5(path: Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def png_size(data: bytes) -> tuple[int, int] | None:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", data[16:24])


def require_built() -> None:
    required = {
        "workshop": WORKSHOP_BIN,
        "workshop.sh": WORKSHOP_SH,
        "2048": GAME_2048_BIN,
        "debug": DEBUG_BIN,
    }
    missing = [f"{name}: {path}" for name, path in required.items()
               if not path.is_file()]
    for app_name in APPS:
        for name in ("app.conf", "icon.png"):
            path = APP / app_name / name
            if not path.is_file():
                missing.append(f"{app_name}/{name}: {path}")
    for name in FONT_FILES:
        path = FONT_DIR / name
        if not path.is_file():
            missing.append(f"font: {path}")
    if not FONT_LICENSE.is_file():
        missing.append(f"font license: {FONT_LICENSE}")
    if missing:
        raise SystemExit(
            "missing build artifacts; run python3 build_apps.py first:\n  "
            + "\n  ".join(missing)
        )


def copy_file(src: Path, dst: Path, mode: int | None = None) -> None:
    if not src.is_file():
        raise SystemExit(f"missing file: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    dst.chmod(mode if mode is not None else src.stat().st_mode & 0o777)


def render_icon(dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    svg = WORKSHOP / "assets" / "workshop.svg"
    fallback = WORKSHOP / "assets" / "workshop.png"

    if shutil.which("rsvg-convert") and svg.is_file():
        log(f"rendering icon: {svg} -> 64x64")
        subprocess.run([
            "rsvg-convert", "-w", "64", "-h", "64", "-f", "png",
            "--background-color=white", str(svg), "-o", str(dest),
        ], check=True)
    elif fallback.is_file():
        log(f"using pre-rendered icon: {fallback}")
        shutil.copyfile(fallback, dest)
    else:
        raise SystemExit("missing workshop icon (svg or png)")

    size = png_size(dest.read_bytes())
    if size != (64, 64):
        raise SystemExit(f"workshop icon must be 64x64, got {size}")


def stage_app(app_name: str, dest: Path) -> None:
    src = APP / app_name
    dest.mkdir(parents=True)
    copy_file(src / "app.conf", dest / "app.conf", 0o644)
    copy_file(src / "icon.png", dest / "icon.png", 0o644)

    src_bin = src / "bin"
    if src_bin.is_dir():
        bin_dest = dest / "bin"
        bin_dest.mkdir()
        for item in sorted(src_bin.iterdir()):
            if item.name.startswith("."):
                continue
            copy_file(item, bin_dest / item.name, 0o755)

    (dest / "data").mkdir()
    (dest / "logs").mkdir()


def add_workshop_to_app_list(path: Path) -> bool:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    original = text
    if not re.search(r"^\[workshop\]", text, re.MULTILINE):
        text = text.rstrip() + "\n\n" + APP_ENTRY
    match = re.search(
        r"(^\[app\][^\[]*?^list=)([^\n]*)",
        text, re.MULTILINE | re.DOTALL,
    )
    if match:
        entries = [item.strip() for item in match.group(2).split(",")
                   if item.strip()]
        if "workshop" not in entries:
            entries.append("workshop")
        text = text[:match.start(2)] + ",".join(entries) + text[match.end(2):]
    if text != original:
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        path.chmod(0o644)
        return True
    return False


def add_workshop_to_zls(path: Path) -> bool:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    original = text
    match = re.search(
        r"(^\[Applications\][^\[]*?^apps=)([^\n]*)",
        text, re.MULTILINE | re.DOTALL,
    )
    if match:
        entries = [item.strip() for item in match.group(2).split(",")
                   if item.strip()]
        if "workshop" not in entries:
            entries.append("workshop")
        text = text[:match.start(2)] + ",".join(entries) + text[match.end(2):]
    if text != original:
        path.write_text(text, encoding="utf-8", errors="surrogateescape")
        path.chmod(0o644)
        return True
    return False


def add_workshop_to_qtopia(root: Path) -> None:
    main = root / "image/opt/Qtopia/etc/mstarapplnk.conf"
    products = sorted(root.glob(
        "image/opt/Qtopia/products/*/opt/image/opt/Qtopia/etc/mstarapplnk.conf"))
    if not main.is_file() and products:
        preferred = next(
            (p for p in products if "86S2_PRO" in str(p)), products[0])
        shutil.copyfile(preferred, main)
        main.chmod(0o644)

    for conf in ([main] if main.is_file() else []) + products:
        changed = add_workshop_to_app_list(conf)
        log(f"  {'updated' if changed else 'ok     '} {conf.relative_to(root)}")

    zls = [root / "image/opt/Qtopia/etc/zlsapplnk.conf"]
    zls += sorted(root.glob(
        "image/opt/Qtopia/products/*/opt/image/opt/Qtopia/etc/zlsapplnk.conf"))
    for conf in zls:
        if conf.is_file():
            changed = add_workshop_to_zls(conf)
            log(f"  {'updated' if changed else 'ok     '} {conf.relative_to(root)}")


def stage_payload() -> tuple[Path, Path]:
    staging = Path(tempfile.mkdtemp(prefix="workshop_payload_"))
    root = staging / "workshop_files"
    for sub in ("app", "bin", "lib", "fonts", "assets",
                "qtopia/bin", "qtopia/pics/mstar"):
        (root / sub).mkdir(parents=True)

    copy_file(WORKSHOP_BIN, root / "bin" / "workshop", 0o755)
    copy_file(WORKSHOP_SH, root / "bin" / "workshop.sh", 0o755)

    for name in FONT_FILES:
        copy_file(FONT_DIR / name, root / "fonts" / name, 0o644)
    copy_file(FONT_LICENSE, root / "fonts" / "OFL.txt", 0o644)

    if (WORKSHOP / "assets").is_dir():
        shutil.copytree(WORKSHOP / "assets", root / "assets",
                        dirs_exist_ok=True)

    for app_name in APPS:
        stage_app(app_name, root / "app" / app_name)

    launcher = root / "qtopia" / "bin" / "workshop"
    launcher.write_text(LAUNCHER, encoding="utf-8")
    launcher.chmod(0o755)
    render_icon(root / "qtopia" / "pics" / "mstar" / "workshop.png")

    for path in root.rglob("._*"):
        path.unlink()

    tar_path = staging / PAYLOAD_NAME
    with tarfile.open(tar_path, "w:gz") as archive:
        archive.add(root, arcname="workshop_files")
    return staging, tar_path


def add_install_hook(path: Path) -> None:
    text = path.read_text(encoding="utf-8", errors="surrogateescape")
    if "installing workshop" in text:
        log(f"  ok      {path}")
        return
    marker = "[ -f /bin/get_epd_update_type ]"
    if marker not in text:
        raise SystemExit(f"cannot find install marker in {path}")
    text = text.replace(marker, INSTALL_BLOCK + marker, 1)
    path.write_text(text, encoding="utf-8", errors="surrogateescape")
    path.chmod(0o755)
    log(f"  updated {path}")


def update_file_list(root: Path) -> None:
    lst_path = root / "upgrade_files.lst"
    lines = [line.strip() for line in lst_path.read_text(encoding="utf-8").splitlines()
             if line.strip()]
    for rel in ("./" + PAYLOAD_PATH,
                "./image/opt/Qtopia/etc/mstarapplnk.conf",
                "./image/opt/Qtopia/bin/workshop",
                "./image/opt/Qtopia/pics/mstar/workshop.png"):
        if rel not in lines:
            lines.append(rel)
    lst_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

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
    log(f"file list: {len(lines)} entries, {len(checksums)} checksums")


def create_image(root: Path) -> None:
    if IMAGE.exists():
        IMAGE.unlink()
    env = os.environ.copy()
    env["COPYFILE_DISABLE"] = "1"
    log(f"creating {IMAGE.name}")
    subprocess.run(
        ["tar", "-czf", str(IMAGE),
         "image", "script", "script_ext",
         "upgrade_files.lst", "upgrade_files.chksum"],
        cwd=str(root), check=True, env=env,
    )


def verify_image(payload: Path, version: str) -> None:
    log("verifying package")
    with tarfile.open(IMAGE, "r:gz") as image:
        names = set(image.getnames())
        for required in (PAYLOAD_PATH,
                         "image/opt/Qtopia/bin/workshop",
                         "image/opt/Qtopia/pics/mstar/workshop.png",
                         "image/opt/Qtopia/etc/mstarapplnk.conf"):
            if required not in names:
                raise SystemExit(f"missing from image: {required}")
        if any("/._" in name for name in names):
            raise SystemExit("AppleDouble files found in image")
        for required in ("image/opt/Qtopia/home/version",
                         "image/opt/Qtopia/home/upgrade.xml"):
            if required not in names:
                raise SystemExit(f"missing from image: {required}")
        base_version = (BASE / "image/opt/Qtopia/home/version").read_text(
            encoding="utf-8").strip()
        packed_version = image.extractfile(
            "image/opt/Qtopia/home/version").read().decode(
            "utf-8", "replace").strip()
        if packed_version != base_version:
            raise SystemExit(
                f"package version {packed_version} != official {base_version}")
        for script in ("script/upgraded.sh", "script_ext/upgraded.sh"):
            if script not in names:
                raise SystemExit(f"missing from image: {script}")
            hook = image.extractfile(script).read()
            if hook.count(b"installing workshop") != 1:
                raise SystemExit(f"workshop install hook wrong in {script}")

        conf = image.extractfile(
            "image/opt/Qtopia/etc/mstarapplnk.conf").read().decode(
            "utf-8", "replace")
        if "[workshop]" not in conf or "list=" not in conf:
            raise SystemExit("live mstarapplnk.conf has no workshop entry")
        for rel in sorted(image.getnames()):
            if "products/" in rel and rel.endswith("/mstarapplnk.conf"):
                data = image.extractfile(rel).read().decode("utf-8", "replace")
                if "[workshop]" not in data:
                    raise SystemExit(f"product conf has no workshop entry: {rel}")

    with tarfile.open(payload, "r:gz") as archive:
        names = set(archive.getnames())
        required = {
            "workshop_files/bin/workshop",
            "workshop_files/bin/workshop.sh",
            "workshop_files/app/2048/app.conf",
            "workshop_files/app/2048/icon.png",
            "workshop_files/app/2048/bin/2048",
            "workshop_files/app/return_desktop/app.conf",
            "workshop_files/app/return_desktop/icon.png",
            "workshop_files/app/debug/app.conf",
            "workshop_files/app/debug/icon.png",
            "workshop_files/app/debug/bin/debug",
            "workshop_files/qtopia/bin/workshop",
            "workshop_files/qtopia/pics/mstar/workshop.png",
        }
        missing = sorted(required - names)
        if missing:
            raise SystemExit("payload missing: " + ", ".join(missing))
        for name in FONT_FILES:
            if f"workshop_files/fonts/{name}" not in names:
                raise SystemExit(f"payload missing font: {name}")
        if "workshop_files/fonts/OFL.txt" not in names:
            raise SystemExit("payload missing font license")
        if "workshop_files/lib" not in names:
            raise SystemExit("payload has no lib directory")
        if "workshop_files/assets" not in names:
            raise SystemExit("payload has no assets directory")

        icon = archive.extractfile(
            "workshop_files/qtopia/pics/mstar/workshop.png").read()
        if png_size(icon) != (64, 64):
            raise SystemExit("payload icon is not 64x64")

        launcher = archive.extractfile(
            "workshop_files/qtopia/bin/workshop").read()
        if b"/user/workshop/bin/workshop.sh" not in launcher:
            raise SystemExit("payload launcher points to the wrong path")

        run = archive.extractfile(
            "workshop_files/bin/workshop.sh").read()
        if b"/user/workshop/bin/workshop" not in run:
            raise SystemExit("workshop.sh points to the wrong binary")

    digest = md5(IMAGE)
    MD5_FILE.write_text(digest + "\n", encoding="utf-8")
    VERSION_FILE.write_text(version + "\n", encoding="utf-8")
    log(f"  {IMAGE.name}: {IMAGE.stat().st_size} bytes")
    log(f"  md5: {digest}")
    log(f"  version: {version}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Package workshop into an OBOOK5 OTA image")
    parser.add_argument("--version", default=DEFAULT_VERSION,
                        help=f"OTA trigger version, not installed (default: {DEFAULT_VERSION})")
    parser.add_argument("--keep-staging", action="store_true",
                        help="keep the temporary payload directory")
    args = parser.parse_args()

    global BASE
    BASE = official.ensure_firmware_tree()
    require_built()

    staging, payload = stage_payload()
    try:
        if OUT.exists():
            shutil.rmtree(OUT)
        log(f"copying base firmware: {BASE} -> {OUT}")
        shutil.copytree(BASE, OUT, symlinks=True)

        payload_dst = OUT / PAYLOAD_PATH
        payload_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(payload, payload_dst)
        payload_dst.chmod(0o644)

        launcher_out = OUT / "image/opt/Qtopia/bin/workshop"
        launcher_out.parent.mkdir(parents=True, exist_ok=True)
        launcher_out.write_text(LAUNCHER, encoding="utf-8")
        launcher_out.chmod(0o755)

        icon_out = OUT / "image/opt/Qtopia/pics/mstar/workshop.png"
        render_icon(icon_out)
        icon_out.chmod(0o644)

        add_workshop_to_qtopia(OUT)

        for script in (OUT / "script/upgraded.sh",
                       OUT / "script_ext/upgraded.sh"):
            add_install_hook(script)

        update_file_list(OUT)
        create_image(OUT)

        if args.keep_staging:
            log(f"payload staging kept: {staging}")
        verify_image(payload, args.version)
    finally:
        if not args.keep_staging:
            shutil.rmtree(staging, ignore_errors=True)

    log("")
    log("done; next:")
    log("  sudo python3 jailbreak_ota.py --target <device-ip>")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
