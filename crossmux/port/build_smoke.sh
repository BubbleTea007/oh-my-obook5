#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$ROOT/build/smoke}"
ZIG="${ZIG:-zig}"

mkdir -p "$(dirname "$OUT")"

"$ZIG" c++ \
  -target mipsel-linux-musleabi \
  -mcpu=mips32r2 \
  -static \
  -O2 \
  -s \
  -std=c++17 \
  -I"$ROOT/linux" \
  "$ROOT/linux/fbdev.cpp" \
  "$ROOT/linux/evdev.cpp" \
  "$ROOT/smoke/smoke_test.cpp" \
  -o "$OUT"

file "$OUT"
ls -lh "$OUT"
