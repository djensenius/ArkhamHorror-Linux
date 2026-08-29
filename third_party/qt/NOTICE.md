# Third-party dependency: Qt (LGPL-3.0/GPL-3.0 replacement obligations)

This directory documents (it does not vendor or build) the Qt runtime
libraries every produced AppImage bundles:

- **Name:** Qt (Qt6 -- Core, Gui, Quick, QuickControls2, Network, WebSockets,
  DBus, and their own transitive Qt-internal dependencies; see
  `.github/workflows/ci.yml`'s `QT_VERSION` for the exact version this
  repository builds and packages against)
- **Upstream:** https://www.qt.io/download-open-source , source at
  https://code.qt.io/cgit/qt/qt5.git (Qt6's supermodule repository index) and
  https://download.qt.io/official_releases/qt/
- **License:** The Qt Company dual-licenses the Qt6 modules this project uses
  under GPL-3.0 and LGPL-3.0 (see [`LICENSE.LGPL3`](./LICENSE.LGPL3) and
  [`LICENSE.GPL3`](./LICENSE.GPL3), both reproduced verbatim from the Free
  Software Foundation's own canonical texts -- LGPL-3.0 is defined as GPL-3.0
  plus the additional permissions in `LICENSE.LGPL3`, so both files together
  are this component's complete license text, matching Qt's own convention
  of shipping both files side by side in its source tree)
- **Copyright:** The Qt Company Ltd. and other contributors (per Qt's own
  source file headers)

## Why this is bundled, and how LGPL-3.0's obligations are met

`packaging/build-appimage.sh` uses `linuxdeploy-plugin-qt` to bundle Qt's
shared libraries (`libQt6*.so*`) into every produced AppImage as ordinary
ELF `DT_NEEDED` dependencies -- Qt is never statically linked. LGPL-3.0
Section 4(d)(1) is satisfied by this dynamic-linking arrangement itself:
the AppImage's bundled `libQt6*.so*` files are ordinary, separately
replaceable shared objects (not baked into a single static binary), so a
recipient can substitute a modified, interface-compatible build of Qt by
replacing those files inside the AppImage's (or an extracted AppDir's)
`usr/lib/` directory and re-running/re-packaging it -- exactly the "suitable
shared library mechanism" Section 4(d)(1) describes, requiring no additional
relinking tooling from this project. Qt's own source is freely available at
the upstream locations above under the same terms, satisfying the
Corresponding Source availability this License requires.

`packaging/audit_codec_notices.py`'s full recursive ELF-closure classifier
(review round-4 item 12) requires every bundled non-ABI-allowlisted `.so`
found anywhere under the AppDir -- including Qt's own libraries, which
`linuxdeploy`/`linuxdeploy-plugin-qt` bundle like any other dependency but
(like every other bundled library) without any accompanying license
attribution -- to resolve to a known component with a notice source
directory here under `third_party/`; Qt is one of those components.
`packaging/lib/bundle_codec_notices.sh` copies this notice into the
distributed AppImage at `usr/share/doc/ArkhamHorror/third_party/qt/` -- see
`packaging/build-appimage.sh`.

## License scope

This NOTICE and the accompanying `LICENSE.LGPL3`/`LICENSE.GPL3` files
document Qt's own licensing terms for attribution purposes only.
ArkhamHorror-Linux itself remains unlicensed (see the repository root); this
file does not grant, and must not be read as granting, any license to
ArkhamHorror-Linux's own source code.
