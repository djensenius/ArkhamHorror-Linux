# shellcheck shell=bash
#
# bundle_codec_notices <bundled_lib_search_root> <third_party_root> <doc_dest_dir>
#
# Review round-3 item 17 (initial version) / round-4 item 12 (this
# rewrite): every ELF shared library linuxdeploy's automatic ldd-based
# bundling copies into the AppDir -- libavif and whichever AV1 codec
# backend(s) the system libavif package was built against, Qt's own
# libraries, and (as review round-4 item 12's real cumulative-review
# finding on the actual produced AppImage showed) libjpeg and an entire
# family of libabsl_* (Abseil) libraries neither of which had ever been
# anticipated by a small handwritten glob table -- needs a bundled
# license/notice text, since linuxdeploy has no notion of license
# attribution at all and an AppImage ships bare .so files with no
# accompanying package metadata whatsoever.
#
# This used to work "forwards": a small fixed table of exactly seven
# codec-library SONAME globs, each individually checked "if bundled, does
# it have a notice?" -- which could never catch a library the table's
# author simply never anticipated. It now works "backwards", delegating
# to packaging/audit_codec_notices.py's `classify` mode: that script
# recursively finds EVERY bundled `.so*` file under
# <bundled_lib_search_root>, and requires each one to resolve to either
# the small ABI allowlist (needs no notice) or one of its own
# COMPONENT_PATTERNS entries -- failing loudly, by exact library path, if
# even one bundled library cannot be classified at all. This function
# only ever bundles notices for whichever DISTINCT set of components that
# classification actually reports as present, so a brand-new
# never-before-seen bundled library fails packaging here rather than
# silently shipping unattributed (the "packaging fails on any unmapped
# new SONAME" contract from review round-4 item 12).
#
# For every classified component, copies every regular file present under
# <third_party_root>/<component>/ (LICENSE, and NOTICE.md/PATENTS/README
# where present) to <doc_dest_dir>/third_party/<component>/ -- mirroring
# the existing third_party/qtkeychain/ layout so every attribution source
# uses the same on-disk shape inside the shipped AppImage. Every possible
# validation failure (classification failure, a classified component with
# no notice source directory or no notice files) happens strictly before
# the first `install -Dm644` call, so a mid-list failure can never leave a
# partial some-but-not-all mix of notices on disk.
bundle_codec_notices() {
  local bundled_lib_search_root="$1"
  local third_party_root="$2"
  local doc_dest_dir="$3"

  # BASH_SOURCE[0] inside a function defined in a sourced file always
  # resolves to the file the function was *defined* in (this file, not
  # whichever script happens to have sourced it) -- this is a documented
  # bash property (see the shell's own manual: "the name of the file
  # where the current function ... was defined"), verified deliberately
  # here rather than assumed, so this never depends on the caller's own
  # $0/directory layout. Avoids requiring callers to pass a fourth
  # repo_root argument just to locate the sibling audit_codec_notices.py
  # script.
  local self_dir
  self_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
  local audit_script="$self_dir/../audit_codec_notices.py"

  local classify_output
  if ! classify_output="$(python3 "$audit_script" \
    classify "$bundled_lib_search_root" 2>&1)"; then
    echo "bundle_codec_notices: audit_codec_notices.py classify failed:" >&2
    echo "$classify_output" >&2
    return 1
  fi

  # classify_output is "<component>\t<path>" lines, one per bundled
  # library requiring a notice; reduce to the distinct component set.
  local -a names_to_bundle=()
  local component
  while IFS=$'\t' read -r component _path; do
    [[ -n "$component" ]] || continue
    local already_listed=0
    local existing
    for existing in "${names_to_bundle[@]+"${names_to_bundle[@]}"}"; do
      if [[ "$existing" == "$component" ]]; then
        already_listed=1
        break
      fi
    done
    if [[ "$already_listed" -eq 0 ]]; then
      names_to_bundle+=("$component")
    fi
  done <<<"$classify_output"

  # Phase 1: validate every classified component's notice source exists
  # and has at least one file, without copying anything yet.
  local notice_src dest f name
  for name in "${names_to_bundle[@]+"${names_to_bundle[@]}"}"; do
    notice_src="$third_party_root/$name"
    if [[ ! -d "$notice_src" ]]; then
      echo "bundle_codec_notices: no notice source at $notice_src for" \
        "bundled component '$name'." >&2
      return 1
    fi
    local has_file=0
    for f in "$notice_src"/*; do
      if [[ -f "$f" ]]; then
        has_file=1
        break
      fi
    done
    if [[ "$has_file" -ne 1 ]]; then
      echo "bundle_codec_notices: $notice_src has no notice files to bundle" \
        "for '$name'." >&2
      return 1
    fi
  done

  # Phase 2: every candidate validated successfully -- now, and only
  # now, actually copy files.
  for name in "${names_to_bundle[@]+"${names_to_bundle[@]}"}"; do
    notice_src="$third_party_root/$name"
    dest="$doc_dest_dir/third_party/$name"
    mkdir -p "$dest"
    for f in "$notice_src"/*; do
      [[ -f "$f" ]] || continue
      install -Dm644 "$f" "$dest/$(basename "$f")"
    done
  done

  return 0
}
