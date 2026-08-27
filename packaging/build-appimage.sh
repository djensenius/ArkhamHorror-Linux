#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
app_dir="${APPDIR:-$PWD/AppDir}"

if [[ "$(uname -s)" != "Linux" || "$(uname -m)" != "x86_64" ]]; then
  echo "AppImage packaging requires x86_64 Linux." >&2
  exit 2
fi

rm -rf "$app_dir"
DESTDIR="$app_dir" cmake --install "$build_dir" --prefix /usr

install -Dm644 packaging/io.github.djensenius.ArkhamHorror.desktop \
  "$app_dir/usr/share/applications/io.github.djensenius.ArkhamHorror.desktop"
install -Dm644 packaging/io.github.djensenius.ArkhamHorror.svg \
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

export QML_SOURCES_PATHS="$PWD/qml"
export LINUXDEPLOY_PLUGIN_QT="$qt_plugin"
"$linuxdeploy" \
  --appdir "$app_dir" \
  --desktop-file packaging/io.github.djensenius.ArkhamHorror.desktop \
  --icon-file packaging/io.github.djensenius.ArkhamHorror.svg \
  --plugin qt \
  --output appimage
