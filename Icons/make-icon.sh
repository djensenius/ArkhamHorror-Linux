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
PNG_ARGS=(-define png:exclude-chunks=date,time)

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

BACKGROUND="$ICONS/AppIcon-background.png"
if [[ ! -f "$BACKGROUND" ]]; then
  echo "missing $BACKGROUND" >&2
  exit 1
fi

mkdir -p "$ICONS"
"$ICONS/render-elder-sign.swift" --font "$FONT_PATH" --output-dir "$ICONS"

PATH_DATA="$(cat "$ICONS/AppIcon-foreground.path")"
FOREGROUND="$ICONS/AppIcon-foreground.png"
SVG="$ROOT/packaging/$APP_ID.svg"
mkdir -p "$(dirname "$SVG")" "$ROOT/packaging/hicolor/scalable/apps"
cat > "$SVG" <<SVG_EOF
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect width="1024" height="1024" rx="192" fill="#E8D5B7"/>
  <path fill="#14120F" d="$PATH_DATA"/>
</svg>
SVG_EOF
cp "$SVG" "$ROOT/packaging/hicolor/scalable/apps/$APP_ID.svg"

render_icon_png() {
  local out="$1" size="$2"
  local edge=$((size - 1))
  local radius=$((size * 3 / 16))
  mkdir -p "$(dirname "$out")"
  "$MAGICK" \
    \( "$BACKGROUND" -resize "${size}x${size}!" \) \
    \( "$FOREGROUND" -resize "${size}x${size}!" \) \
    -compose over -composite \
    \( -size "${size}x${size}" xc:none -fill white -draw "roundrectangle 0,0 ${edge},${edge} ${radius},${radius}" \) \
    -compose CopyOpacity -composite \
    -depth 8 "${PNG_ARGS[@]}" "$out"
}

render_icon_png "$ICONS/AppIcon-composite.png" 1024
for size in 16 24 32 48 64 128 256 512; do
  out="$ROOT/packaging/hicolor/${size}x${size}/apps/$APP_ID.png"
  render_icon_png "$out" "$size"
  echo "  ${size}x${size} -> ${out#$ROOT/}"
done

echo "Done. Rendered elder-sign artwork into Linux hicolor icons."
