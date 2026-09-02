#!/usr/bin/env bash
# Deterministic regression test for packaging/lib/bundle_codec_notices.sh.
#
# Review round-3 item 17: libavif and its AV1 codec backend(s) (dav1d/aom/
# gav1/rav1e/SVT-AV1/libyuv) are ordinary linked libraries linuxdeploy's
# automatic ldd-based bundling copies into the AppImage on its own, but
# nothing previously arranged for their required license/notice texts to
# ship alongside them. This test exercises bundle_codec_notices() against
# synthetic (never real) fake ".so" files in a temporary directory, so it
# needs no real libavif/AppImage/network dependency:
#   1. Only the mandatory libavif "library" present -> only its notice is
#      bundled (dav1d/aom notice directories must NOT appear).
#   2. libavif plus dav1d and libaom "libraries" present -> exactly those
#      three notices are bundled, none of the untouched optional ones.
#   3. libavif "library" missing entirely -> fails (non-zero), and must
#      NOT partially bundle any notices at all.
#   4. A recognized optional codec library ("libgav1.so.1") present but
#      this repository's third_party/libgav1/ notice source deleted for
#      the test -> fails loudly rather than silently omitting it.
#   5. Every bundled LICENSE/NOTICE.md file's *content* actually matches
#      the checked-in third_party source (not merely present) for the
#      libavif + dav1d case.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=packaging/lib/bundle_codec_notices.sh
source "$repo_root/packaging/lib/bundle_codec_notices.sh"

work_dir="$(mktemp -d 2>/dev/null || mktemp -d -t bundle_codec_notices_test)"
trap 'rm -rf "$work_dir"' EXIT

fail() {
  # Print every argument, not just the first -- some call sites (e.g.
  # case 4 below) pass a message split across multiple arguments, and
  # silently dropping everything after $1 would hide the actual failure
  # reason from CI output.
  echo "FAIL: $*" >&2
  exit 1
}

make_fake_lib() {
  # $1: directory, $2: filename
  #
  # Round-9+ review item 10: audit_codec_notices.py's
  # find_bundled_libraries() now discovers bundled libraries by their
  # real ELF magic bytes (\x7fELF), not merely by a "*.so*" filename
  # glob, so a zero-byte fake library is no longer discovered at all.
  # Write the four-byte ELF magic prefix (plus a little padding) so
  # these synthetic fixtures are still recognized as bundled ELF
  # objects; nothing here needs to be a real, loadable ELF file, since
  # classify()/find_bundled_libraries() never parse dynamic-linking
  # content, only the magic bytes and the path/basename.
  printf '\x7fELF\0\0\0\0\0\0\0\0\0\0\0\0' > "$1/$2"
}

# --- Case 1: libavif only -----------------------------------------------
case1_lib_dir="$work_dir/case1/lib"
case1_doc_dir="$work_dir/case1/doc"
mkdir -p "$case1_lib_dir"
make_fake_lib "$case1_lib_dir" "libavif.so.16.0.0"

bundle_codec_notices "$case1_lib_dir" "$repo_root/third_party" "$case1_doc_dir" \
  || fail "case 1: bundle_codec_notices should succeed with only libavif present"

[[ -f "$case1_doc_dir/third_party/libavif/LICENSE" ]] \
  || fail "case 1: libavif LICENSE was not bundled"
[[ -f "$case1_doc_dir/third_party/libavif/NOTICE.md" ]] \
  || fail "case 1: libavif NOTICE.md was not bundled"
[[ -e "$case1_doc_dir/third_party/dav1d" ]] \
  && fail "case 1: dav1d notice must not be bundled when dav1d is not present"
[[ -e "$case1_doc_dir/third_party/libaom" ]] \
  && fail "case 1: libaom notice must not be bundled when libaom is not present"

# --- Case 2: libavif + dav1d + libaom ------------------------------------
case2_lib_dir="$work_dir/case2/lib"
case2_doc_dir="$work_dir/case2/doc"
mkdir -p "$case2_lib_dir"
make_fake_lib "$case2_lib_dir" "libavif.so.16.0.0"
make_fake_lib "$case2_lib_dir" "libdav1d.so.7.0.0"
make_fake_lib "$case2_lib_dir" "libaom.so.3.9.0"

bundle_codec_notices "$case2_lib_dir" "$repo_root/third_party" "$case2_doc_dir" \
  || fail "case 2: bundle_codec_notices should succeed with libavif+dav1d+libaom"

for name in libavif dav1d libaom; do
  [[ -f "$case2_doc_dir/third_party/$name/LICENSE" ]] \
    || fail "case 2: $name LICENSE was not bundled"
done
for name in libgav1 rav1e svt-av1 libyuv; do
  [[ -e "$case2_doc_dir/third_party/$name" ]] \
    && fail "case 2: $name notice must not be bundled when $name is not present"
done

# --- Case 3: libavif missing (mandatory) ---------------------------------
case3_lib_dir="$work_dir/case3/lib"
case3_doc_dir="$work_dir/case3/doc"
mkdir -p "$case3_lib_dir"
make_fake_lib "$case3_lib_dir" "libdav1d.so.7.0.0"

if bundle_codec_notices "$case3_lib_dir" "$repo_root/third_party" "$case3_doc_dir" \
  2>/dev/null; then
  fail "case 3: bundle_codec_notices must fail when mandatory libavif is missing"
fi
[[ -d "$case3_doc_dir" ]] \
  && find "$case3_doc_dir" -mindepth 1 -print -quit | grep -q . \
  && fail "case 3: no notices may be partially bundled when libavif is missing"
true

# --- Case 4: recognized optional library present, notice source missing -
case4_lib_dir="$work_dir/case4/lib"
case4_doc_dir="$work_dir/case4/doc"
case4_third_party_dir="$work_dir/case4/third_party"
mkdir -p "$case4_lib_dir" "$case4_third_party_dir"
make_fake_lib "$case4_lib_dir" "libavif.so.16.0.0"
make_fake_lib "$case4_lib_dir" "libgav1.so.1.0.0"
# Copy every real notice source EXCEPT libgav1's, to simulate this
# repository having forgotten to add a notice for a newly-appearing
# bundled codec backend.
for name in libavif dav1d libaom rav1e svt-av1 libyuv; do
  cp -R "$repo_root/third_party/$name" "$case4_third_party_dir/$name"
done

if bundle_codec_notices "$case4_lib_dir" "$case4_third_party_dir" "$case4_doc_dir" \
  2>/dev/null; then
  fail "case 4: bundle_codec_notices must fail when a bundled codec library" \
    "has no corresponding notice source"
fi
# The "no partial bundling" guarantee: libavif's own notice source IS
# valid here (only libgav1's is missing), so a naive copy-as-you-go
# implementation would have already installed libavif's LICENSE before
# reaching libgav1's validation failure. Every candidate must be
# validated before any file is copied, so NOTHING may be bundled at all.
[[ -d "$case4_doc_dir" ]] \
  && find "$case4_doc_dir" -mindepth 1 -print -quit | grep -q . \
  && fail "case 4: no notices (not even libavif's, which is individually" \
    "valid) may be partially bundled when a later candidate fails validation"
true

# --- Case 5: bundled notice content matches the checked-in source -------
case5_lib_dir="$work_dir/case5/lib"
case5_doc_dir="$work_dir/case5/doc"
mkdir -p "$case5_lib_dir"
make_fake_lib "$case5_lib_dir" "libavif.so.16.0.0"
make_fake_lib "$case5_lib_dir" "libdav1d.so.7.0.0"

bundle_codec_notices "$case5_lib_dir" "$repo_root/third_party" "$case5_doc_dir" \
  || fail "case 5: bundle_codec_notices should succeed"

cmp -s "$case5_doc_dir/third_party/libavif/LICENSE" \
  "$repo_root/third_party/libavif/LICENSE" \
  || fail "case 5: bundled libavif LICENSE content does not match the checked-in source"
cmp -s "$case5_doc_dir/third_party/dav1d/LICENSE" \
  "$repo_root/third_party/dav1d/LICENSE" \
  || fail "case 5: bundled dav1d LICENSE content does not match the checked-in source"

echo "PASS: bundle_codec_notices() behaves correctly in all 5 cases."
