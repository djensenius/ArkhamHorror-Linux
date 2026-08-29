# shellcheck shell=bash
#
# bundle_codec_notices <bundled_lib_search_root> <third_party_root> <doc_dest_dir>
#
# Review round-3 item 17: libavif (mandatory, see CMakeLists.txt's
# pkg_check_modules(LIBAVIF REQUIRED ...)) and whichever AV1 codec
# backend(s) the system libavif package was actually built against
# (typically dav1d and/or aom on Debian/Ubuntu, see libavif15's own
# package dependencies) are ordinary linked (DT_NEEDED) libraries that
# linuxdeploy's automatic ldd-based bundling follows and copies into the
# AppImage on its own -- but linuxdeploy has no notion of *license*
# attribution, so nothing previously arranged for their required
# BSD/Apache notice texts to ship alongside them the way QtKeychain's
# already do (see the "Package the third-party attribution files" step
# in build-appimage.sh and third_party/qtkeychain/). Silently omitting
# them is exactly the gap this closes: never assume a notice "must have"
# shipped via some Ubuntu package doc path -- an AppImage bundles its
# libraries as bare .so files with no accompanying package metadata at
# all.
#
# Recursively scans <bundled_lib_search_root> (e.g. the AppDir's "usr"
# tree, after linuxdeploy has already run its automatic
# dependency-following pass but *before* the final --output appimage
# packaging step) for each known codec library's SONAME glob -- matching
# CI's own extracted-AppImage verification steps, which likewise `find`
# by filename rather than assuming one fixed lib subdirectory, since
# linuxdeploy's exact placement (usr/lib vs. a per-arch subdirectory) is
# an implementation detail this should not hard-code two different ways.
# For every one actually found, copies every regular file present under
# <third_party_root>/<name>/ (LICENSE, and PATENTS/NOTICE.md where
# present) to <doc_dest_dir>/third_party/<name>/ -- mirroring the
# existing third_party/qtkeychain/ layout so both attribution sources use
# the same on-disk shape inside the shipped AppImage.
#
# libavif itself is unconditionally required (the build cannot produce a
# working binary without it, so its absence here would indicate a
# packaging bug, not a legitimate "backend not used" case) -- every other
# name in the table is genuinely optional, since which AV1 backend(s) the
# system libavif package pulls in varies by distro/version. Returns
# non-zero (without partially bundling a mix of some-but-not-all
# licenses) if libavif's own bundled library or its notice source
# directory cannot be found, or if a recognized codec backend library is
# found bundled but this repository has no corresponding notice source
# for it -- fail loudly here rather than silently ship an unattributed
# binary dependency.
bundle_codec_notices() {
  local bundled_lib_search_root="$1"
  local third_party_root="$2"
  local doc_dest_dir="$3"

  # name -> SONAME glob, one pair per line. libavif is listed first and is
  # the only mandatory entry; the rest are optional AV1 codec backends
  # libavif may or may not have been linked against.
  local -a mandatory_names=(libavif)
  local -a optional_names=(dav1d libaom libgav1 rav1e svt-av1 libyuv)
  local -A globs=(
    [libavif]='libavif.so*'
    [dav1d]='libdav1d.so*'
    [libaom]='libaom.so*'
    [libgav1]='libgav1.so*'
    [rav1e]='librav1e.so*'
    [svt-av1]='libSvtAv1*.so*'
    [libyuv]='libyuv.so*'
  )

  _bundle_one_codec_notice() {
    local name="$1"
    local notice_src="$third_party_root/$name"
    if [[ ! -d "$notice_src" ]]; then
      echo "bundle_codec_notices: no notice source at $notice_src for" \
        "bundled codec library '$name'." >&2
      return 1
    fi
    local dest="$doc_dest_dir/third_party/$name"
    mkdir -p "$dest"
    local any_file=0
    local f
    for f in "$notice_src"/*; do
      [[ -f "$f" ]] || continue
      install -Dm644 "$f" "$dest/$(basename "$f")"
      any_file=1
    done
    if [[ "$any_file" -ne 1 ]]; then
      echo "bundle_codec_notices: $notice_src has no notice files to bundle" \
        "for '$name'." >&2
      return 1
    fi
    return 0
  }

  local name
  for name in "${mandatory_names[@]}"; do
    local glob="${globs[$name]}"
    local found
    found="$(find "$bundled_lib_search_root" -name "$glob" -print -quit 2>/dev/null || true)"
    if [[ -z "$found" ]]; then
      echo "bundle_codec_notices: mandatory codec library '$name'" \
        "(glob '$glob') was not found bundled under $bundled_lib_search_root." >&2
      return 1
    fi
    _bundle_one_codec_notice "$name" || return 1
  done

  for name in "${optional_names[@]}"; do
    local glob="${globs[$name]}"
    local found
    found="$(find "$bundled_lib_search_root" -name "$glob" -print -quit 2>/dev/null || true)"
    if [[ -n "$found" ]]; then
      _bundle_one_codec_notice "$name" || return 1
    fi
  done

  return 0
}
