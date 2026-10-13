# Icons

The Linux icon uses the same owner-approved elder sign treatment as the Apple
client: the `X` (U+0058) glyph from the Arkham Horror LCG fan icon font is
rendered with `CTFontCreatePathForGlyph`, centered, with the glyph bounding box
filling 64% of a 1024px canvas. The PNG icons composite that glyph over
`AppIcon-background.png`, the Telephone-booth parchment background color
`rgb(232,213,183)`, then apply rounded transparent corners. The scalable SVG
uses the same parchment color and glyph path.

Run the deterministic renderer on macOS from the repository root (the Swift
renderer imports AppKit/CoreText):

```bash
./Icons/make-icon.sh <path-to-arkham.ttf>
# Example, when this repo sits beside the backend checkout:
./Icons/make-icon.sh ../ArkhamHorror/frontend/public/fonts/arkham.ttf
```

`AppIcon-background.png` is copied from the Telephone-booth app-icon background
(`Telephone-Booth-Operator-Mobile/Icons/AppIcon-background.png`). The source
font itself is not vendored. The generated foreground path, foreground PNG, PNG
review composite, hicolor PNGs, and scalable SVGs are committed so the artwork
is reviewable without rerunning the generator. PNG output is byte-stable for a
fixed Swift and ImageMagick toolchain; `make-icon.sh` strips ImageMagick
date/time chunks from PNG files.

## Desktop integration

The installed desktop file and icon use the app id
`io.github.djensenius.ArkhamHorror`. On Wayland, Qt publishes the same id via
`QGuiApplication::setDesktopFileName(...)` so compositors can match the running
window to `io.github.djensenius.ArkhamHorror.desktop`. On X11, Qt 6.11's XCB
platform reports `WM_CLASS(STRING) = "arkham-horror", "Arkham Horror"` for this
binary after `setApplicationName("Arkham Horror")`, so the desktop file uses
`StartupWMClass=Arkham Horror`.

## Licensing caveat

The elder sign glyph comes from the unlicensed "AH LCG" fan icon font and uses
Fantasy Flight Games iconography. It is acceptable here for personal/internal
builds, but it **must be replaced with original art before any public
distribution, including Flathub, GitHub Releases, or app stores**.
