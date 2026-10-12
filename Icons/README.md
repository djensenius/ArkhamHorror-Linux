# Icons

The Linux icon uses the same owner-approved elder sign treatment as the Apple
client: the `X` (U+0058) glyph from the Arkham Horror LCG fan icon font is
rendered with `CTFontCreatePathForGlyph`, centered, with the glyph bounding box
filling 64% of a 1024px canvas. The icon uses ink `rgb(20,18,15)` on the
Telephone-booth parchment color `rgb(232,213,183)` in a rounded rectangle.

Run the deterministic renderer from the repository root:

```bash
./Icons/make-icon.sh /Users/david/Developer/ArkhamHorror/ArkhamHorror/frontend/public/fonts/arkham.ttf
```

`AppIcon-background.png` is copied from
`/Users/david/Developer/Art/Telephone-booth/Telephone-Booth-Operator-Mobile/Icons/AppIcon-background.png`.
The source font itself is not vendored. The generated foreground path, SVG, PNG
source, hicolor PNGs, and scalable SVG are committed so the artwork is
reviewable without rerunning the generator.

## Licensing caveat

The elder sign glyph comes from the unlicensed "AH LCG" fan icon font and uses
Fantasy Flight Games iconography. It is acceptable here for personal/internal
builds, but it **must be replaced with original art before any public store
submission**.
