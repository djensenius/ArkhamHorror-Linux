# Third-party dependency: Abseil (abseil-cpp)

This directory documents (it does not vendor or build) a third-party runtime
dependency of the ArkhamHorror-Linux client's AppImage packaging:

- **Name:** Abseil (abseil-cpp)
- **Upstream:** https://github.com/abseil/abseil-cpp
- **License:** Apache License, Version 2.0 (see [`LICENSE`](./LICENSE),
  reproduced verbatim from upstream -- byte-identical to the same standard
  Apache-2.0 boilerplate already reproduced for `third_party/libgav1/LICENSE`)
- **Copyright:** Google LLC (per upstream source file headers)

## Why this is bundled

Review round-4 item 12's real cumulative-review finding: the final produced
AppImage's `linuxdeploy`-resolved dependency closure additionally includes a
family of `libabsl_*.so*` libraries (Abseil's own convention of shipping one
shared object per component -- `libabsl_base`, `libabsl_strings`,
`libabsl_time`, `libabsl_synchronization`, and so on; the exact set and count
varies by Abseil/distro version, which is why
`packaging/audit_codec_notices.py` classifies the whole family by the
`^libabsl_.*\.so` SONAME prefix rather than an exhaustive fixed name list).
These are a transitive dependency of another already-bundled library in the
closure (e.g. an ICU/Qt/curl backend on the AppImage build host), never
linked directly by ArkhamHorror-Linux's own code -- but `linuxdeploy` bundles
them regardless, with no notion of license attribution, exactly like the AV1
codec backends and libjpeg documented in this directory's siblings.

`packaging/audit_codec_notices.py`'s full recursive ELF-closure classifier
requires every bundled non-ABI-allowlisted `.so` found anywhere under the
AppDir to resolve to a known component with a notice source directory here
under `third_party/`; Abseil is one of those components.
`packaging/lib/bundle_codec_notices.sh` copies this notice into the
distributed AppImage at `usr/share/doc/ArkhamHorror/third_party/abseil/` --
see `packaging/build-appimage.sh`.

## License scope

This NOTICE and the accompanying `LICENSE` file document Abseil's own
Apache-2.0 terms for attribution purposes only. ArkhamHorror-Linux itself
remains unlicensed (see the repository root); this file does not grant, and
must not be read as granting, any license to ArkhamHorror-Linux's own source
code.
