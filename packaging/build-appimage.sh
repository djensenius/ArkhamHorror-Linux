#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_root/build}"
app_dir="$repo_root/AppDir"

# shellcheck source=packaging/lib/find_bundled_libsecret.sh
source "$repo_root/packaging/lib/find_bundled_libsecret.sh"
# shellcheck source=packaging/lib/bundle_codec_notices.sh
source "$repo_root/packaging/lib/bundle_codec_notices.sh"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "AppImage packaging requires x86_64 Linux." >&2
  exit 2
fi

rm -rf "$app_dir"
DESTDIR="$app_dir" cmake --install "$build_dir" --prefix /usr

install -Dm644 "$repo_root/packaging/io.github.djensenius.ArkhamHorror.desktop" \
  "$app_dir/usr/share/applications/io.github.djensenius.ArkhamHorror.desktop"
install -Dm644 "$repo_root/packaging/io.github.djensenius.ArkhamHorror.svg" \
  "$app_dir/usr/share/icons/hicolor/scalable/apps/io.github.djensenius.ArkhamHorror.svg"

linuxdeploy="${LINUXDEPLOY:-linuxdeploy-x86_64.AppImage}"
qt_plugin="${LINUXDEPLOY_PLUGIN_QT:-linuxdeploy-plugin-qt-x86_64.AppImage}"

command -v "$linuxdeploy" >/dev/null 2>&1 || {
  echo "Set LINUXDEPLOY to a trusted linuxdeploy executable." >&2
  exit 2
}
command -v "$qt_plugin" >/dev/null 2>&1 || {
  echo "Set LINUXDEPLOY_PLUGIN_QT to a trusted Qt plugin executable." >&2
  exit 2
}

# packaging/audit_codec_notices.py's classifier requires a verified
# reference to the real Qt SDK actually used for this build before ever
# resolving a directory-matched bundled library (a Qt plugin or QML
# module, neither of which matches its own libQt6.* basename pattern) to
# the "qt" component -- see bundle_codec_notices()'s own doc comment for
# why. Resolved the same way linuxdeploy-plugin-qt itself locates Qt:
# `qmake -query QT_INSTALL_PREFIX`, honoring an explicit $QMAKE override
# first (matching linuxdeploy-plugin-qt's own documented QMAKE
# environment variable) so this never depends on jurplel/install-qt-action
# CI-specific env vars and works identically for a developer's own local
# Qt installation.
qmake_bin="${QMAKE:-}"
if [[ -z "$qmake_bin" ]]; then
  if command -v qmake6 >/dev/null 2>&1; then
    qmake_bin="qmake6"
  elif command -v qmake >/dev/null 2>&1; then
    qmake_bin="qmake"
  fi
fi
[[ -n "$qmake_bin" ]] || {
  echo "Could not locate qmake (set QMAKE or put qmake6/qmake on PATH)." \
    "Needed to resolve the real Qt SDK root for codec-notice" \
    "classification." >&2
  exit 2
}
qt_reference_dir="$("$qmake_bin" -query QT_INSTALL_PREFIX)"
[[ -n "$qt_reference_dir" && -d "$qt_reference_dir" ]] || {
  echo "qmake -query QT_INSTALL_PREFIX did not report a real directory:" \
    "'$qt_reference_dir'" >&2
  exit 2
}
qt_sdk_version="$("$qmake_bin" -query QT_VERSION)"
[[ -n "$qt_sdk_version" ]] || {
  echo "qmake -query QT_VERSION returned an empty value." >&2
  exit 2
}

# QtKeychain's Secret Service backend loads libsecret-1 at runtime via
# QLibrary (dlopen), not as a linked (DT_NEEDED) dependency of
# libqt6keychain -- see qtkeychain/libsecret.cpp's
# "QLibrary(QLatin1String("secret-1"), 0)". linuxdeploy's automatic
# dependency bundling only follows the ELF-linked dependency graph via
# ldd, so it never discovers or bundles a dlopen()-only library on its
# own. Without this, the AppImage would silently fall back to
# TokenStoreOutcome::Unavailable on any host lacking a system copy of
# libsecret-1, defeating the point of bundling QtKeychain at all.
# Passing the resolved path via linuxdeploy's --library flag makes
# linuxdeploy treat it as a first-class dependency: it is copied into the
# AppDir and its own transitive shared-library dependencies (glib,
# gobject, gio, ...) are resolved and bundled the same way as any other
# linked library.
#
# Discovery itself lives in find_bundled_libsecret() (see
# packaging/lib/find_bundled_libsecret.sh) so it is unit-testable in
# isolation -- see packaging/tests/test_find_bundled_libsecret.sh, which
# exercises the ldconfig-unavailable/failing fallback path deterministically
# rather than relying on this runner's own environment happening to lack
# ldconfig.
# shellcheck disable=SC2119 # intentionally called with no args: this
# script's own $1 (build_dir) must not be forwarded as a search root.
libsecret_so="$(find_bundled_libsecret)"
[[ -n "$libsecret_so" && -e "$libsecret_so" ]] || {
  echo "Could not locate libsecret-1.so.0 to bundle into the AppImage." \
    "Install libsecret-1-0 (runtime) or libsecret-1-dev." >&2
  exit 2
}

# libsecret-1 pulls in libgcrypt transitively (via gnome-keyring/GPG-Agent
# integration), and libgcrypt in turn requires libgpg-error -- but
# linuxdeploy's own default library blacklist excludes libgpg-error from
# automatic bundling (it assumes, incorrectly for a portable AppImage
# intended to run on arbitrary distros, that a compatible system copy is
# always already present). Left unbundled, a target host without its own
# libgpg-error would fail to dlopen() libsecret-1 at all -- an entirely
# different, and much less obvious, failure mode than the "missing
# libsecret-1 itself" case above. Force-bundling it here the same way
# closes that gap; recursive-closure verification in CI
# (packaging/audit_dependency_closure.py) then proves no other transitive
# dependency is silently missing.
# shellcheck disable=SC2119
libgpgerror_so="$(find_bundled_libgpgerror)"
[[ -n "$libgpgerror_so" && -e "$libgpgerror_so" ]] || {
  echo "Could not locate libgpg-error.so.0 to bundle into the AppImage." \
    "Install libgpg-error0 (runtime) or libgpg-error-dev." >&2
  exit 2
}

# libgcc_s, libstdc++, and zlib are all excluded from linuxdeploy's
# automatic bundling by its own default blacklist (it treats them as
# "always present on the target system"), but that assumption is unsafe
# for a portable AppImage: libgcc_s/libstdc++'s C++ ABI is not guaranteed
# compatible across distros/ages the way glibc's C ABI is (a well-known
# AppImage portability failure mode), and zlib -- required transitively
# by both bundled Qt and bundled libsecret's own closure -- is not part
# of glibc either. Bundling all three explicitly here, rather than
# excusing them via the recursive closure audit's ABI_ALLOWLIST, keeps
# that allowlist narrowly limited to the dynamic loader and true
# core-glibc libraries only, matching this project's own documented
# policy.
# shellcheck disable=SC2119
libgccs_so="$(find_bundled_libgccs)"
[[ -n "$libgccs_so" && -e "$libgccs_so" ]] || {
  echo "Could not locate libgcc_s.so.1 to bundle into the AppImage." >&2
  exit 2
}
# shellcheck disable=SC2119
libstdcxx_so="$(find_bundled_libstdcxx)"
[[ -n "$libstdcxx_so" && -e "$libstdcxx_so" ]] || {
  echo "Could not locate libstdc++.so.6 to bundle into the AppImage." >&2
  exit 2
}
# shellcheck disable=SC2119
libz_so="$(find_bundled_libz)"
[[ -n "$libz_so" && -e "$libz_so" ]] || {
  echo "Could not locate libz.so.1 to bundle into the AppImage." \
    "Install zlib1g (runtime) or zlib1g-dev." >&2
  exit 2
}

# libcom_err is required transitively by bundled libgssapi_krb5/libkrb5
# (themselves pulled in by libsecret-1's own glib/gio closure on some
# distros), but -- verified directly against a real linuxdeploy run's own
# "Skipping deployment of blacklisted library" diagnostic -- is excluded
# from linuxdeploy's automatic bundling by its own default blacklist, the
# same (for a portable AppImage, incorrect) assumption as libgpg-error
# above. Force-bundling it here closes that gap the same way.
# shellcheck disable=SC2119
libcomerr_so="$(find_bundled_libcomerr)"
[[ -n "$libcomerr_so" && -e "$libcomerr_so" ]] || {
  echo "Could not locate libcom_err.so.2 to bundle into the AppImage." \
    "Install libkrb5-3 (runtime) or libkrb5-dev." >&2
  exit 2
}


# AVIF card art (djensenius/ArkhamHorror-Linux#17) is decoded directly
# against libavif's own C API (see src/AssetAvifDecoder.cpp), linked as an
# ordinary ELF DT_NEEDED dependency of arkham_foundation/arkham-horror --
# unlike libsecret above (loaded via dlopen()/QLibrary at runtime, which
# linuxdeploy's ldd-based automatic bundling can never discover on its
# own), libavif.so and its own transitive AV1 codec-backend dependencies
# (dav1d and/or aom) are ordinary linked libraries that linuxdeploy's
# default ldd-based dependency resolution follows and bundles
# automatically, exactly like Qt's own dependencies -- no explicit
# --library flag is needed or added for it here. CI's recursive
# DT_NEEDED closure audit (packaging/audit_dependency_closure.py) and
# offline bundled-only round-trip smoke
# (packaging/tests/avif_bundled_roundtrip_smoke.c) verify this
# expectation against the real produced AppImage rather than merely
# assuming it.

# Round-N+ review (HIGH, "distro provenance post-hoc/unpinned: after
# packaging it searches fixed system dirs by basename, not exact
# linuxdeploy-selected pre-copy file ... capture exact loader/copy
# source BEFORE packaging"): capture real distro-package provenance
# (path/sha256/package/version/sourcePackage) for every dynamically
# resolved dependency of this project's own first-party artifacts,
# using this repository's own requester-specific DT_NEEDED walk
# (`readelf -d -W` + the real loader's DT_RPATH/DT_RUNPATH/
# LD_LIBRARY_PATH/ld.so.cache/default-path precedence) against the
# exact pre-packaging files -- strictly BEFORE linuxdeploy ever runs
# and copies/patches anything -- rather than an independent, after-the-
# fact directory search once packaging is already complete. Covers:
#   - the main application executable itself (already installed via
#     `cmake --install` above, but not yet touched by linuxdeploy):
#     the recursive requester-specific walk picks up every ordinary
#     ELF-linked (DT_NEEDED) dependency, including Qt/ICU and libavif's
#     own directly-linked AV1 codec backends (dav1d/aom/etc. -- see the
#     comment above this block).
#   - libsecret/libgpg-error/libgcc_s/libstdc++/zlib/libcom_err: these
#     are force-bundled via linuxdeploy's --library flag specifically
#     BECAUSE they are otherwise invisible to ldd-based automatic
#     bundling (dlopen()'d or default-blacklisted -- see each variable's
#     own resolution comment above); the same recursive DT_NEEDED walk
#     captures their own transitive closure (glib, gobject, gio,
#     libffi, pcre2, ...) before packaging too.
#   - every real ELF file under the actual Qt SDK reference tree's own
#     plugins/ directory ($qt_reference_dir, already resolved above via
#     `qmake -query QT_INSTALL_PREFIX`): Qt's platform/imageformat/etc.
#     plugins are dlopen()'d by Qt's own plugin loader at runtime, never
#     linked (DT_NEEDED) into the main executable at all, so they are
#     otherwise entirely invisible if you only scan arkham-horror --
#     yet they are exactly where libjpeg/libpng/xcb-family distro
#     dependencies actually come from once linuxdeploy-plugin-qt bundles
#     them.
distro_provenance_manifest="$repo_root/distro-provenance-manifest.json"
distro_provenance_stage_dir="$repo_root/.distro-provenance-stage"
rm -rf "$distro_provenance_stage_dir"
mapfile -d '' -t qt_plugin_elf_files < <(
  find "$qt_reference_dir/plugins" -type f -print0 2>/dev/null
)

# Round-N+ review (HIGH, "Prefer replaying the exact trusted packaging
# transform ... byte-compare final with a transformation receipt"):
# extract the REAL patchelf/strip binaries bundled inside this
# project's own pinned, sha256-verified $linuxdeploy/$qt_plugin
# AppImages (see .github/workflows/ci.yml's "Download packaging tools"
# step, which verifies both AppImages' own sha256 BEFORE this script
# ever runs) so audit_codec_notices.py can later REPLAY each bundled
# library's real strip-then-patchelf transformation and byte-compare
# the result, rather than only ever trusting a heuristic digest
# comparison. `--appimage-extract` is each AppImage's own documented,
# no-FUSE-required extraction mode (this project's own empirical
# research already used it directly against these exact pinned
# releases to derive the transformation recipe replay_strip_and_rpath_
# transform() below implements). Extracted into a dedicated staging
# directory (never the repo root) so these tool binaries can never be
# mistaken for, or accidentally bundled alongside, this project's own
# first-party artifacts.
replay_tool_stage_dir="$repo_root/.linuxdeploy-replay-tools-stage"
rm -rf "$replay_tool_stage_dir"
mkdir -p "$replay_tool_stage_dir/linuxdeploy" "$replay_tool_stage_dir/linuxdeploy-plugin-qt"
linuxdeploy_real_path="$(command -v "$linuxdeploy")"
qt_plugin_real_path="$(command -v "$qt_plugin")"
(
  cd "$replay_tool_stage_dir/linuxdeploy"
  "$linuxdeploy_real_path" --appimage-extract >/dev/null
)
(
  cd "$replay_tool_stage_dir/linuxdeploy-plugin-qt"
  "$qt_plugin_real_path" --appimage-extract >/dev/null
)
linuxdeploy_patchelf="$replay_tool_stage_dir/linuxdeploy/squashfs-root/usr/bin/patchelf"
linuxdeploy_strip="$replay_tool_stage_dir/linuxdeploy/squashfs-root/usr/bin/strip"
linuxdeploy_qt_patchelf="$replay_tool_stage_dir/linuxdeploy-plugin-qt/squashfs-root/usr/bin/patchelf"
linuxdeploy_qt_strip="$replay_tool_stage_dir/linuxdeploy-plugin-qt/squashfs-root/usr/bin/strip"
for extracted_tool in \
  "$linuxdeploy_patchelf" "$linuxdeploy_strip" \
  "$linuxdeploy_qt_patchelf" "$linuxdeploy_qt_strip"; do
  [[ -x "$extracted_tool" ]] || {
    echo "Extracted linuxdeploy/linuxdeploy-plugin-qt replay tool" \
      "'$extracted_tool' is missing or not executable -- cannot capture" \
      "replay evidence for distro provenance." >&2
    exit 1
  }
done

# Independent review (HIGH, repeat finding, "live deps reach
# linuxdeploy ... capture returns lockMismatch/mismatch instead of
# abort ... abort capture before linuxdeploy on any nonverified
# state"): --require-verified-archive-provenance makes THIS step
# itself (not a later, deferred `classify --require-package-
# provenance` call) the single, earliest gate -- if any dpkg-owned
# captured library cannot be authenticated against a real, freshly
# downloaded-and-hashed distro archive, this command exits nonzero
# and this script's own `set -euo pipefail` aborts immediately, BEFORE
# the "for-linuxdeploy" redirect below is even populated and BEFORE
# linuxdeploy is ever invoked (see the "Capture distro provenance"
# CLI subcommand's own docstring for the full rationale).
python3 "$repo_root/packaging/audit_codec_notices.py" capture-distro-provenance \
  "$app_dir/usr/bin/arkham-horror" \
  "$libsecret_so" "$libgpgerror_so" "$libgccs_so" "$libstdcxx_so" \
  "$libz_so" "$libcomerr_so" \
  "${qt_plugin_elf_files[@]}" \
  --output "$distro_provenance_manifest" \
  --staging-dir "$distro_provenance_stage_dir" \
  --linuxdeploy-patchelf "$linuxdeploy_patchelf" \
  --linuxdeploy-strip "$linuxdeploy_strip" \
  --linuxdeploy-qt-patchelf "$linuxdeploy_qt_patchelf" \
  --linuxdeploy-qt-strip "$linuxdeploy_qt_strip" \
  --require-verified-archive-provenance
echo "Wrote distro-package provenance manifest to $distro_provenance_manifest"

# Round-N+ review (HIGH, "staged capture exists, but linuxdeploy still
# consumes original host paths"): every CAPTURED, GOVERNED distro
# component this repository actually intends to bundle (i.e. a manifest
# entry whose basename audit_codec_notices.py itself classifies to a
# concrete third-party component, not a host-only ABI_ALLOWLIST library
# or an unmapped straggler that the later classify hard gate would fail
# anyway) is redirected back to its immutable staged snapshot here, not
# just the previous hard-coded six. That closes the architecture hole:
# once capture-distro-provenance above has nofollow-opened, hashed, and
# staged the exact bytes a real loader resolved for each requester edge,
# linuxdeploy never needs the mutable host pathname again. A same-
# basename symlink is still required because linuxdeploy's --library flag
# names the bundled copy after the basename of the path it is given, and
# that basename must stay the real SONAME.
staged_library_symlink_dir="$distro_provenance_stage_dir/for-linuxdeploy"
mkdir -p "$staged_library_symlink_dir"
mapfile -t linuxdeploy_staged_libraries < <(
  python3 - "$distro_provenance_manifest" "$staged_library_symlink_dir" <<'PY'
import json
import os
from pathlib import Path, PurePosixPath
import sys

manifest_path = Path(sys.argv[1])
symlink_dir = Path(sys.argv[2])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
bundled_paths = manifest.get("bundledPaths")
if not isinstance(bundled_paths, dict):
    raise SystemExit(
        f"{manifest_path} is missing the expected bundledPaths object"
    )
printed_any = False
for bundled_path, entry in sorted(bundled_paths.items()):
    if not isinstance(entry, dict):
        continue
    component = entry.get("component")
    staged_path = entry.get("stagedPath")
    if not isinstance(component, str) or not component:
        continue
    if not isinstance(staged_path, str) or not Path(staged_path).is_file():
        raise SystemExit(
            f"{manifest_path}: bundled path {bundled_path!r} is missing a real stagedPath"
        )
    basename = PurePosixPath(bundled_path).name
    symlink_path = symlink_dir / basename
    try:
        symlink_path.unlink()
    except FileNotFoundError:
        pass
    os.symlink(staged_path, symlink_path)
    print(symlink_path)
    printed_any = True
if not printed_any:
    raise SystemExit(
        f"{manifest_path}: no governed staged libraries were found to redirect"
    )
PY
)
linuxdeploy_library_args=()
for staged_library in "${linuxdeploy_staged_libraries[@]}"; do
  [[ -L "$staged_library" ]] || {
    echo "Expected $staged_library to be a symlink onto a staged immutable" \
      "library snapshot." >&2
    exit 2
  }
  linuxdeploy_library_args+=(--library "$staged_library")
done

# Package the third-party attribution files (QtKeychain's BSD-3-Clause
# LICENSE and this project's own NOTICE.md) into the distributed AppImage
# so end users receive accurate attribution without this unlicensed
# client repo gaining a license of its own.
doc_dir="$app_dir/usr/share/doc/ArkhamHorror"
install -Dm644 "$repo_root/third_party/qtkeychain/LICENSE" \
  "$doc_dir/third_party/qtkeychain/LICENSE"
install -Dm644 "$repo_root/third_party/qtkeychain/NOTICE.md" \
  "$doc_dir/third_party/qtkeychain/NOTICE.md"

export QML_SOURCES_PATHS="$repo_root/qml"
export LINUXDEPLOY_PLUGIN_QT="$qt_plugin"
# linuxdeploy-plugin-qt only bundles the "xcb" platform plugin by
# default. Bundling "offscreen" too (via its EXTRA_PLATFORM_PLUGINS
# hook) lets the exact same shipped AppImage be launched headlessly
# with QT_QPA_PLATFORM=offscreen -- this is how CI's AppImage smoke
# test actually proves the packaged app starts, and it may also help on
# headless SteamOS/CI-like environments -- without changing anything
# about normal interactive use, which still defaults to "xcb".
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so"
# Deliberately split into two linuxdeploy invocations (populate, then
# package) rather than one combined "--output appimage" call: which AV1
# codec backend(s) (dav1d/aom/gav1/rav1e/SVT-AV1/libyuv) end up bundled
# alongside libavif is only known once linuxdeploy's automatic ldd-based
# dependency resolution has actually run and populated "$app_dir/usr/lib"
# -- review round-3 item 17 requires every one of those actually-bundled
# codec libraries to ship with its required license/notice text, so
# bundle_codec_notices() below must run (and be allowed to fail the build
# loudly on an unrecognized/missing notice) strictly between the
# "populate" and "package" phases, before the final .AppImage is ever
# produced.
"$linuxdeploy" \
  --appdir "$app_dir" \
  --desktop-file "$repo_root/packaging/io.github.djensenius.ArkhamHorror.desktop" \
  --icon-file "$repo_root/packaging/io.github.djensenius.ArkhamHorror.svg" \
  "${linuxdeploy_library_args[@]}" \
  --plugin qt

bundled_search_root="$app_dir/usr"
[[ -d "$bundled_search_root" ]] || {
  echo "linuxdeploy did not populate the expected $bundled_search_root." >&2
  exit 2
}
python3 - "$distro_provenance_manifest" "$app_dir" <<'PY'
import json
from pathlib import Path
import sys

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
app_dir = Path(sys.argv[2])
bundled_paths = manifest.get("bundledPaths")
if not isinstance(bundled_paths, dict):
    raise SystemExit("distro provenance manifest is missing bundledPaths")
missing = []
for bundled_path, entry in sorted(bundled_paths.items()):
    if not isinstance(entry, dict):
        continue
    component = entry.get("component")
    if not isinstance(component, str) or not component:
        continue
    final_path = app_dir / bundled_path
    if not final_path.is_file():
        missing.append(bundled_path)
if missing:
    raise SystemExit(
        "linuxdeploy failed to materialize redirected governed bundled path(s): "
        + ", ".join(missing)
    )
PY
bundle_codec_notices "$bundled_search_root" "$repo_root/third_party" "$doc_dir" \
  "$qt_reference_dir" || {
  echo "Failed to bundle required codec library license/notice text --" \
    "see the message(s) above. This is a hard failure: a codec library" \
    "must never ship inside the AppImage without its required" \
    "attribution." >&2
  exit 2
}

# Hard-gate the final pre-AppImage AppDir on the same provenance proof
# cmd_classify later enforces in CI after extraction: every governed
# bundled library's FINAL bytes must bind either to the immutable staged
# distro snapshot captured above or to the real Qt SDK reference tree's
# own copy, with replay evidence where available. This way packaging
# cannot quietly proceed to the final .AppImage step with an unproven
# bundled dependency even if linuxdeploy itself found some new host path.
python3 "$repo_root/packaging/audit_codec_notices.py" classify "$bundled_search_root" \
  --qt-reference-dir "$qt_reference_dir" \
  --require-package-provenance \
  --distro-provenance-manifest "$distro_provenance_manifest" \
  --qt-sdk-version "$qt_sdk_version"

"$linuxdeploy" --appdir "$app_dir" --output appimage
