#!/usr/bin/env bash
#
# Render the AH LCG elder-sign glyph into Linux hicolor icons and the Qt window
# icon resource. The font path is an input so the unlicensed fan font is not
# vendored into this repository.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ICONS="$ROOT/Icons"
APP_ID="io.github.djensenius.ArkhamHorror"
FONT_PATH="${1:-}"
MAGICK="${MAGICK:-/opt/homebrew/bin/magick}"

if [[ -z "$FONT_PATH" ]]; then
  echo "usage: $0 /path/to/arkham.ttf" >&2
  exit 2
fi
if [[ ! -f "$FONT_PATH" ]]; then
  echo "missing font: $FONT_PATH" >&2
  exit 1
fi
if [[ ! -x "$MAGICK" ]]; then
  if command -v magick >/dev/null 2>&1; then
    MAGICK="$(command -v magick)"
  else
    echo "magick not found — install ImageMagick or set MAGICK" >&2
    exit 1
  fi
fi
if [[ ! -f "$ICONS/AppIcon-background.png" ]]; then
  echo "missing $ICONS/AppIcon-background.png" >&2
  exit 1
fi

mkdir -p "$ICONS"
"$ICONS/render-elder-sign.swift" --font "$FONT_PATH" --output-dir "$ICONS"

PATH_DATA="$(cat "$ICONS/AppIcon-foreground.path")"
SVG="$ROOT/packaging/$APP_ID.svg"
mkdir -p "$(dirname "$SVG")" "$ROOT/packaging/hicolor/scalable/apps"
cat > "$SVG" <<SVG_EOF
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect width="1024" height="1024" rx="192" fill="#E8D5B7"/>
  <path fill="#14120F" d="$PATH_DATA"/>
</svg>
SVG_EOF
cp "$SVG" "$ROOT/packaging/hicolor/scalable/apps/$APP_ID.svg"

"$MAGICK" -background none "$SVG" -resize 1024x1024 -depth 8 "$ICONS/AppIcon-composite.png"
for size in 16 24 32 48 64 128 256 512; do
  out="$ROOT/packaging/hicolor/${size}x${size}/apps/$APP_ID.png"
  mkdir -p "$(dirname "$out")"
  "$MAGICK" -background none "$SVG" -resize "${size}x${size}" -depth 8 "$out"
  echo "  ${size}x${size} -> ${out#$ROOT/}"
done

echo "Done. Rendered elder-sign artwork into Linux hicolor icons."
