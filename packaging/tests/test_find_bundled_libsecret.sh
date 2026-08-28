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

# A PATH containing only coreutils-equivalent tools plus whatever fake
# binaries a given test case installs into fake_bin_dir -- deliberately
# excludes any real system ldconfig.
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

echo "All find_bundled_libsecret() regression cases passed."
