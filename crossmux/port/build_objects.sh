#!/bin/sh
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
CROSSMUX="$(cd "$ROOT/.." && pwd)"
OUT="$ROOT/build/objects"
ZIG="${ZIG:-zig}"

mkdir -p "$OUT"

INCLUDES="-I$CROSSMUX/simulator/src -I$CROSSMUX/upstream/src -I$ROOT/linux"
for d in $(find "$CROSSMUX/upstream/lib" "$CROSSMUX/upstream/freeink-sdk/libs" -type d -name include 2>/dev/null); do
  INCLUDES="$INCLUDES -I$d"
done

COMMON="-target mipsel-linux-musleabi -mcpu=mips32r2 -c -std=c++20 -O2 -s \
  -DSIMULATOR -DCROSSPOINT_EMULATED=1 -DFREEINK_DEVICE_X4PRO=1 -DBOARD_HAS_PSRAM"

# shellcheck disable=SC2086
"$ZIG" c++ $COMMON $INCLUDES "$ROOT/obook5/HalDisplay.cpp" -o "$OUT/HalDisplay.o"
# shellcheck disable=SC2086
"$ZIG" c++ $COMMON $INCLUDES "$ROOT/obook5/HalGPIO.cpp" -o "$OUT/HalGPIO.o"
# shellcheck disable=SC2086
"$ZIG" c++ $COMMON $INCLUDES "$ROOT/obook5/main.cpp" -o "$OUT/main.o"

ls -lh "$OUT"/*.o
