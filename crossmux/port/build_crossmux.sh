#!/bin/sh
# Build the CrossMux OBOOK5 binary.
#
# Requires: zig 0.17+, python3, and PlatformIO in .pio-venv (or set PIO).
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
CROSSMUX="$(cd "$ROOT/.." && pwd)"
UPSTREAM="$CROSSMUX/upstream"
PIO_DIR="$ROOT/pio"
OUT="$ROOT/build/crossmux"
PIO="${PIO:-$CROSSMUX/../.pio-venv/bin/pio}"

if [ ! -x "$PIO" ]; then
  echo "PlatformIO not found at $PIO (set PIO=/path/to/pio)" >&2
  exit 1
fi

# Generated, gitignored upstream files.  Running the generators from the
# upstream root keeps their relative paths intact.
cd "$UPSTREAM"
if [ ! -f lib/I18n/I18nStrings.cpp ]; then
  echo "[build] generating i18n"
  python3 scripts/gen_i18n.py --strip-unused
fi
if [ ! -f src/util/UserGuide.generated.h ]; then
  echo "[build] generating user guide"
  python3 - <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, "scripts")
import generate_userguide_epub as guide
guide.build_bundled(Path("docs/user-guide"), Path("build/user-guide"),
                    Path("src/util/UserGuide.generated.h"))
PY
fi
if [ ! -f src/network/html/en/HomePageHtml.generated.h ]; then
  echo "[build] generating web assets"
  python3 scripts/build_html.py
fi

cd "$PIO_DIR"
echo "[build] preparing source tree"
python3 prepare.py

echo "[build] compiling CrossMux (zig mipsel-linux-musleabi)"
PATH="$ROOT/toolchain/bin:$PATH" "$PIO" run -e obook5

mkdir -p "$ROOT/build"
cp "$PIO_DIR/.pio/build/obook5/program" "$OUT"

# zig objcopy does not implement ELF stripping yet; use LLVM's strip when the
# host has it, otherwise keep the unstripped binary.
STRIP="${STRIP:-}"
if [ -z "$STRIP" ]; then
  for candidate in /opt/homebrew/opt/llvm/bin/llvm-strip /usr/local/opt/llvm/bin/llvm-strip; do
    if [ -x "$candidate" ]; then STRIP="$candidate"; break; fi
  done
fi
if [ -n "$STRIP" ] && [ -x "$STRIP" ]; then
  "$STRIP" --strip-all "$OUT" -o "$OUT.stripped"
  mv "$OUT.stripped" "$OUT"
fi

echo
file "$OUT"
ls -lh "$OUT"

"$ROOT/package_app.sh"
