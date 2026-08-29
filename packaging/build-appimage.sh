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
  --library "$libsecret_so" \
  --library "$libgpgerror_so" \
  --library "$libgccs_so" \
  --library "$libstdcxx_so" \
  --library "$libz_so" \
  --plugin qt

bundled_search_root="$app_dir/usr"
[[ -d "$bundled_search_root" ]] || {
  echo "linuxdeploy did not populate the expected $bundled_search_root." >&2
  exit 2
}
bundle_codec_notices "$bundled_search_root" "$repo_root/third_party" "$doc_dir" \
  "$qt_reference_dir" || {
  echo "Failed to bundle required codec library license/notice text --" \
    "see the message(s) above. This is a hard failure: a codec library" \
    "must never ship inside the AppImage without its required" \
    "attribution." >&2
  exit 2
}

"$linuxdeploy" --appdir "$app_dir" --output appimage
