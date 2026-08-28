#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-$repo_root/build}"
app_dir="$repo_root/AppDir"

# shellcheck source=packaging/lib/find_bundled_libsecret.sh
source "$repo_root/packaging/lib/find_bundled_libsecret.sh"

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
"$linuxdeploy" \
  --appdir "$app_dir" \
  --desktop-file "$repo_root/packaging/io.github.djensenius.ArkhamHorror.desktop" \
  --icon-file "$repo_root/packaging/io.github.djensenius.ArkhamHorror.svg" \
  --library "$libsecret_so" \
  --plugin qt \
  --output appimage
