#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$ROOT/bin/workshop}"
ZIG="${ZIG:-zig}"

mkdir -p "$(dirname "$OUT")"

LVGL_SRCS="$(find "$ROOT/third_party/lvgl/src" -name '*.c' -print)"
APP_SRCS="\
$ROOT/src/main.c \
$ROOT/src/launcher.c \
$ROOT/src/manifest.c \
$ROOT/src/icons.c \
$ROOT/src/settings.c \
$ROOT/lib/workshop_ime/workshop_ime.c \
$ROOT/src/platform_obook.c"

# shellcheck disable=SC2086
"$ZIG" cc \
  -target mipsel-linux-musleabi \
  -mcpu=mips32r2 \
  -static \
  -Os \
  -std=gnu11 \
  -DWORKSHOP_DEVICE=1 \
  -DLV_KCONFIG_IGNORE \
  -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DLV_CONF_PATH="\"$ROOT/lv_conf.h\"" \
  -I"$ROOT" \
  -I"$ROOT/src" \
  -I"$ROOT/lib/workshop_ime" \
  -I"$ROOT/third_party/lvgl" \
  $LVGL_SRCS \
  $APP_SRCS \
  -o "$OUT" \
  -lm

file "$OUT"
ls -lh "$OUT"
