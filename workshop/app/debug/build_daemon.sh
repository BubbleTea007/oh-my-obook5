#!/bin/bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$APP_DIR/bin/debugd}"
ZIG="${ZIG:-zig}"

mkdir -p "$(dirname "$OUT")"

"$ZIG" cc \
  -target mipsel-linux-musleabi \
  -mcpu=mips32r2 \
  -static \
  -Os \
  -std=gnu11 \
  "$APP_DIR/src/debug_daemon.c" \
  -o "$OUT"

file "$OUT"
ls -lh "$OUT"
