#!/bin/bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$APP_DIR/../.." && pwd)"
WORKSHOP="$ROOT"
OUT="${1:-$APP_DIR/bin/debug}"
ZIG="${ZIG:-zig}"

mkdir -p "$(dirname "$OUT")"

LVGL_SRCS="$(find "$WORKSHOP/third_party/lvgl/src" -name '*.c' -print)"

# shellcheck disable=SC2086
"$ZIG" cc \
  -target mipsel-linux-musleabi \
  -mcpu=mips32r2 \
  -static \
  -Os \
  -std=gnu11 \
  -DLV_KCONFIG_IGNORE \
  -DLV_LVGL_H_INCLUDE_SIMPLE \
  -DLV_CONF_PATH="\"$WORKSHOP/lv_conf.h\"" \
  -I"$WORKSHOP" \
  -I"$WORKSHOP/third_party/lvgl" \
  "$APP_DIR/src/debug_main.c" \
  $LVGL_SRCS \
  -o "$OUT" \
  -lm

file "$OUT"
ls -lh "$OUT"
