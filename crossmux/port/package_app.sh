#!/bin/sh
# Package the built binary as a workshop app tree.
set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT/build/crossmux"
OUT="$ROOT/build/app/crossmux"

if [ ! -f "$BIN" ]; then
  echo "missing $BIN; run build_crossmux.sh first" >&2
  exit 1
fi

rm -rf "$ROOT/build/app"
mkdir -p "$OUT/bin" "$OUT/logs"
cp "$BIN" "$OUT/bin/crossmux"
chmod 755 "$OUT/bin/crossmux"

cat > "$OUT/app.conf" <<'EOF'
name=CrossMux
exec=bin/crossmux
EOF

mkdir -p "$ROOT/build"
# macOS tar otherwise adds AppleDouble ._* siblings for every entry.
COPYFILE_DISABLE=1 tar -C "$ROOT/build/app" -czf "$ROOT/build/crossmux-app.tar.gz" crossmux

echo "packaged:"
find "$ROOT/build/app" -type f -maxdepth 4 -exec ls -lh {} \;
ls -lh "$ROOT/build/crossmux-app.tar.gz"
