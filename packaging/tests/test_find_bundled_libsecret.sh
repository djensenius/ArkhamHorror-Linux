#!/usr/bin/env bash
# Deterministic regression test for packaging/lib/find_bundled_libsecret.sh.
#
# Exercises the exact hazards find_bundled_libsecret() exists to avoid:
#   1. ldconfig entirely absent from PATH.
#   2. ldconfig present but failing (non-zero exit), which is the scenario
#      that would previously abort the whole build-appimage.sh script
#      under `set -euo pipefail` before ever reaching the find() fallback.
#   3. ldconfig present and succeeding, so its answer is used directly and
#      the find() fallback is not needed.
#   4. Neither ldconfig nor find() locate a candidate: the function must
#      return an empty result, not abort the caller.
#
# This script itself runs under `set -euo pipefail` so a regression that
# reintroduces the original bug (an unguarded pipeline aborting the script)
# would fail this test the same way it would fail the real packaging
# script, without needing a real Linux runner lacking ldconfig.
set -euo pipefail

# find_bundled_libsecret() relies on `find ... -print -quit`, a GNU find
# extension; the production packaging script this guards already refuses
# to run outside Linux x86_64 (see build-appimage.sh's own uname check), so
# this test mirrors that same platform gate rather than failing on a
# developer's non-Linux machine where the fallback's GNU-specific syntax
# genuinely is not what would run in production.
if [[ "$(uname -s)" != "Linux" ]]; then
  echo "SKIP: find_bundled_libsecret() targets Linux find semantics" \
    "(GNU find's -print -quit); not exercised on $(uname -s)."
  exit 0
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck source=packaging/lib/find_bundled_libsecret.sh
source "$repo_root/packaging/lib/find_bundled_libsecret.sh"

work_dir="$(mktemp -d 2>/dev/null || mktemp -d -t find_bundled_libsecret_test)"
trap 'rm -rf "$work_dir"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

# A PATH containing $fake_bin_dir (first, so any fake ldconfig a test case
# installs there takes priority) plus /usr/bin and /bin for the real
# coreutils/awk/find this function and this test script need. This
# deliberately omits /sbin and /usr/sbin, where ldconfig itself actually
# lives on Debian/Ubuntu-based systems (including the GitHub Actions
# runner and typical AppImage build hosts this packaging script targets)
# -- so unless a test case installs its own fake ldconfig into
# fake_bin_dir, `command -v ldconfig` genuinely finds nothing here. This
# is not an absolute guarantee on every possible distro layout (some
# non-Debian-family systems place ldconfig under /usr/bin), only that it
# matches the target deployment environment this test is guarding.
fake_bin_dir="$work_dir/bin"
mkdir -p "$fake_bin_dir"
restricted_path="$fake_bin_dir:/usr/bin:/bin"

install_fake_ldconfig() {
  local exit_code="$1"
  local output="$2"
  cat > "$fake_bin_dir/ldconfig" <<EOF
#!/usr/bin/env bash
printf '%s' "$output"
exit $exit_code
EOF
  chmod +x "$fake_bin_dir/ldconfig"
}

remove_fake_ldconfig() {
  rm -f "$fake_bin_dir/ldconfig"
}

# --- Case 1: ldconfig entirely absent from PATH; find() fallback finds the
# candidate in a fake search root. Proves the fallback is reached and used
# when ldconfig is unavailable, without touching real system paths.
remove_fake_ldconfig
fake_root_1="$work_dir/root1/usr/lib/x86_64-linux-gnu"
mkdir -p "$fake_root_1"
expected_1="$fake_root_1/libsecret-1.so.0.0.0"
: > "$expected_1"

result_1="$(PATH="$restricted_path" find_bundled_libsecret "$fake_root_1")"
[[ "$result_1" == "$expected_1" ]] \
  || fail "case 1 (ldconfig absent): expected '$expected_1', got '$result_1'"
echo "PASS: ldconfig absent from PATH -> find() fallback used"

# --- Case 2: ldconfig present but failing (non-zero exit). This is the
# exact hazard that previously aborted build-appimage.sh under
# `set -euo pipefail` before the find() fallback ever ran. Proves the
# fallback is still reached and its result still returned.
install_fake_ldconfig 1 ""
fake_root_2="$work_dir/root2/lib"
mkdir -p "$fake_root_2"
expected_2="$fake_root_2/libsecret-1.so.0"
: > "$expected_2"

result_2="$(PATH="$restricted_path" find_bundled_libsecret "$work_dir/root2")"
[[ "$result_2" == "$expected_2" ]] \
  || fail "case 2 (ldconfig failing): expected '$expected_2', got '$result_2'"
echo "PASS: failing ldconfig -> find() fallback used, script not aborted"

# --- Case 3: ldconfig present and succeeding with a matching line; its
# answer must be used directly, without needing the find() fallback at
# all (the fake search root below is deliberately empty/nonexistent).
install_fake_ldconfig 0 \
  "libsecret-1.so.0 (libc6,x86-64) => /opt/fake/libsecret-1.so.0"
result_3="$(PATH="$restricted_path" find_bundled_libsecret "$work_dir/does-not-exist")"
[[ "$result_3" == "/opt/fake/libsecret-1.so.0" ]] \
  || fail "case 3 (ldconfig succeeding): expected '/opt/fake/libsecret-1.so.0', got '$result_3'"
echo "PASS: succeeding ldconfig output parsed directly"

# --- Case 4: neither ldconfig nor find() locate anything. The function
# must return an empty string and must not abort this test script (which
# itself runs under set -euo pipefail).
remove_fake_ldconfig
result_4="$(PATH="$restricted_path" find_bundled_libsecret "$work_dir/also-does-not-exist")"
[[ -z "$result_4" ]] \
  || fail "case 4 (nothing found): expected empty result, got '$result_4'"
echo "PASS: no candidate anywhere -> empty result, no abort"

# --- Case 5: find_bundled_libgpgerror() is a distinct wrapper over the
# same generic find_bundled_library() helper, for the separate library
# force-bundled to satisfy bundled libgcrypt's dependency (see
# build-appimage.sh). Proves it resolves its own library name correctly
# via ldconfig and never cross-matches libsecret's name/result.
install_fake_ldconfig 0 \
  "libgpg-error.so.0 (libc6,x86-64) => /opt/fake/libgpg-error.so.0.32.1"
result_5="$(PATH="$restricted_path" find_bundled_libgpgerror "$work_dir/does-not-exist")"
[[ "$result_5" == "/opt/fake/libgpg-error.so.0.32.1" ]] \
  || fail "case 5 (libgpgerror via ldconfig): expected '/opt/fake/libgpg-error.so.0.32.1', got '$result_5'"
echo "PASS: find_bundled_libgpgerror resolves its own library via ldconfig"

# --- Case 6: find_bundled_libgpgerror() falls back to find() when
# ldconfig is absent, exactly like find_bundled_libsecret() does, and
# locates only the gpg-error file, not a co-located libsecret file.
remove_fake_ldconfig
fake_root_6="$work_dir/root6/lib"
mkdir -p "$fake_root_6"
: > "$fake_root_6/libsecret-1.so.0"
expected_6="$fake_root_6/libgpg-error.so.0"
: > "$expected_6"
result_6="$(PATH="$restricted_path" find_bundled_libgpgerror "$fake_root_6")"
[[ "$result_6" == "$expected_6" ]] \
  || fail "case 6 (libgpgerror find fallback): expected '$expected_6', got '$result_6'"
echo "PASS: find_bundled_libgpgerror find() fallback locates only its own library"

# --- Case 7: find_bundled_libgccs()/find_bundled_libstdcxx()/
# find_bundled_libz() are further thin wrappers over the same generic
# helper, added to force-bundle libgcc_s.so.1, libstdc++.so.6, and
# libz.so.1 -- all three excluded from linuxdeploy's automatic bundling
# by its own default blacklist, discovered missing from a real produced
# AppImage by the recursive closure audit. Proves each resolves its own
# library via ldconfig without cross-matching either of the others or
# libsecret/libgpg-error.
install_fake_ldconfig 0 "$(cat <<'LDCONFIG_EOF'
libgcc_s.so.1 (libc6,x86-64) => /opt/fake/libgcc_s.so.1
libstdc++.so.6 (libc6,x86-64) => /opt/fake/libstdc++.so.6.0.32
libz.so.1 (libc6,x86-64) => /opt/fake/libz.so.1.3.1
LDCONFIG_EOF
)"
result_7a="$(PATH="$restricted_path" find_bundled_libgccs "$work_dir/does-not-exist")"
[[ "$result_7a" == "/opt/fake/libgcc_s.so.1" ]] \
  || fail "case 7a (libgccs via ldconfig): expected '/opt/fake/libgcc_s.so.1', got '$result_7a'"
result_7b="$(PATH="$restricted_path" find_bundled_libstdcxx "$work_dir/does-not-exist")"
[[ "$result_7b" == "/opt/fake/libstdc++.so.6.0.32" ]] \
  || fail "case 7b (libstdcxx via ldconfig): expected '/opt/fake/libstdc++.so.6.0.32', got '$result_7b'"
result_7c="$(PATH="$restricted_path" find_bundled_libz "$work_dir/does-not-exist")"
[[ "$result_7c" == "/opt/fake/libz.so.1.3.1" ]] \
  || fail "case 7c (libz via ldconfig): expected '/opt/fake/libz.so.1.3.1', got '$result_7c'"
echo "PASS: find_bundled_libgccs/find_bundled_libstdcxx/find_bundled_libz resolve via ldconfig"

# --- Case 8: the same three wrappers fall back to find() when ldconfig
# is absent, each locating only its own file among several co-located
# candidates.
remove_fake_ldconfig
fake_root_8="$work_dir/root8/lib"
mkdir -p "$fake_root_8"
: > "$fake_root_8/libgcc_s.so.1"
: > "$fake_root_8/libstdc++.so.6"
: > "$fake_root_8/libz.so.1"
: > "$fake_root_8/libsecret-1.so.0"
result_8a="$(PATH="$restricted_path" find_bundled_libgccs "$fake_root_8")"
[[ "$result_8a" == "$fake_root_8/libgcc_s.so.1" ]] \
  || fail "case 8a (libgccs find fallback): expected '$fake_root_8/libgcc_s.so.1', got '$result_8a'"
result_8b="$(PATH="$restricted_path" find_bundled_libstdcxx "$fake_root_8")"
[[ "$result_8b" == "$fake_root_8/libstdc++.so.6" ]] \
  || fail "case 8b (libstdcxx find fallback): expected '$fake_root_8/libstdc++.so.6', got '$result_8b'"
result_8c="$(PATH="$restricted_path" find_bundled_libz "$fake_root_8")"
[[ "$result_8c" == "$fake_root_8/libz.so.1" ]] \
  || fail "case 8c (libz find fallback): expected '$fake_root_8/libz.so.1', got '$result_8c'"
echo "PASS: find_bundled_libgccs/find_bundled_libstdcxx/find_bundled_libz find() fallback works"

echo "All find_bundled_libsecret()/find_bundled_libgpgerror()/find_bundled_libgccs()/find_bundled_libstdcxx()/find_bundled_libz() regression cases passed."
