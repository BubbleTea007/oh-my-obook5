#!/usr/bin/env python3
"""Rasterize the launcher's built-in UI icons into LVGL A8 descriptors.

App icons are not generated here: they are loaded from
``app/<id>/icon.png`` at runtime. This script only
handles the status-bar icons and the fallback app icon.

The SVGs are rendered by librsvg at 32px, 64px and 72px, then stored as A8
alpha masks. LVGL can recolor the mask black/white for e-ink inversion.
"""
from pathlib import Path
import shutil
import subprocess

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SVG_DIR = ROOT / "assets/icons/svg"
SIZES = (32, 64, 72)

ICONS = {
    "wifi_arc3": "wifi-arc3.svg",
    "wifi_arc2": "wifi-arc2.svg",
    "wifi_arc1": "wifi-arc1.svg",
    "wifi_dot": "wifi-dot.svg",
    "battery_full": "battery-full.svg",
    "battery_medium": "battery-medium.svg",
    "battery_low": "battery-low.svg",
    "battery_empty": "battery-empty.svg",
    "battery_charging": "battery-charging.svg",
    # Fallback for an app whose icon file is missing or invalid.
    "app_default": "layout-dashboard.svg",
}


def c_array(data):
    out = []
    for i in range(0, len(data), 16):
        out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + 16]) + ",")
    return "\n".join(out)


def render_svg(svg, size, out):
    subprocess.run(
        [
            shutil.which("rsvg-convert") or "/opt/homebrew/bin/rsvg-convert",
            "-w", str(size),
            "-h", str(size),
            "-f", "png",
            "--background-color=none",
            str(svg),
            "-o", str(out),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    image = Image.open(out).convert("RGBA")
    return image.getchannel("A")


def main():
    preview = Image.new("L", (len(ICONS) * 80, 96), 255)
    cparts = [
        '#include "icons.h"',
        "",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
    ]
    table = []

    for index, (key, filename) in enumerate(ICONS.items()):
        svg = SVG_DIR / filename
        if not svg.exists():
            raise SystemExit(f"missing {svg}")

        pointers = {}
        for size in SIZES:
            png = Path("/private/tmp") / f"workshop_icon_{key}_{size}.png"
            alpha = render_svg(svg, size, png)
            data = alpha.tobytes()
            name = f"icon_{key}_{size}"

            cparts.append(f"static const uint8_t {name}_data[] = {{")
            cparts.append(c_array(data))
            cparts.append("};")
            cparts.append("")
            cparts.append(f"static const lv_image_dsc_t {name} = {{")
            cparts.append("    .header = {")
            cparts.append("        .magic = LV_IMAGE_HEADER_MAGIC,")
            cparts.append("        .cf = LV_COLOR_FORMAT_A8,")
            cparts.append("        .flags = 0,")
            cparts.append(f"        .w = {size},")
            cparts.append(f"        .h = {size},")
            cparts.append(f"        .stride = {size},")
            cparts.append("    },")
            cparts.append(f"    .data_size = sizeof({name}_data),")
            cparts.append(f"    .data = {name}_data,")
            cparts.append("};")
            cparts.append("")
            pointers[size] = name

        table.append(
            f'    {{"{key}", &{pointers[32]}, &{pointers[64]}, &{pointers[72]}}},'
        )
        preview.paste(0, (index * 80 + 4, 4),
                      render_svg(svg, 72, Path("/private/tmp") / "workshop_icon_preview.png"))

    cparts.append("static const workshop_icon_t s_icons[] = {")
    cparts.extend(table)
    cparts.append("    {NULL, NULL, NULL},")
    cparts.append("};")
    cparts.append("")
    cparts.append("const workshop_icon_t *workshop_icon_find(const char *name)")
    cparts.append("{")
    cparts.append("    if (!name) return NULL;")
    cparts.append("    for (const workshop_icon_t *i = s_icons; i->name; i++) {")
    cparts.append("        if (!strcmp(i->name, name)) return i;")
    cparts.append("    }")
    cparts.append("    return NULL;")
    cparts.append("}")
    cparts.append("")

    (ROOT / "src/icons.c").write_text("\n".join(cparts), encoding="utf-8")
    (ROOT / "src/icons.h").write_text(
        "#ifndef WORKSHOP_ICONS_H\n"
        "#define WORKSHOP_ICONS_H\n\n"
        '#include "lvgl.h"\n\n'
        "typedef struct {\n"
        "    const char *name;\n"
        "    const lv_image_dsc_t *img32;\n"
        "    const lv_image_dsc_t *img64;\n"
        "    const lv_image_dsc_t *img72;\n"
        "} workshop_icon_t;\n\n"
        "const workshop_icon_t *workshop_icon_find(const char *name);\n\n"
        "#endif\n",
        encoding="utf-8",
    )

    preview.save("/private/tmp/workshop_icons_preview.png")
    print(f"generated {len(ICONS)} icons x {len(SIZES)} sizes")
    print("wrote src/icons.c, src/icons.h")
    print("preview /private/tmp/workshop_icons_preview.png")


if __name__ == "__main__":
    main()
