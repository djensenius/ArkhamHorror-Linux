#!/usr/bin/env bash
# Deterministic regression test for packaging/audit_dependency_closure.py's
# own recursive-closure logic and mutation/deletion detection, using a
# small synthetic three-library chain compiled locally with `cc` rather
# than real system packages -- this keeps the test hermetic (no apt-get /
# network dependency) and fast, while still exercising real ELF files and
# real `readelf`-parsed DT_NEEDED entries, not fabricated/mocked data.
#
# Chain: libtestroot.so.1 -> libtestmid.so.1 -> libtestleaf.so.1
# (each library exports one trivial function and NEEDS the next one down;
# libtestleaf has no further non-ABI dependencies).
#
# For a genuine end-to-end proof against the actual production libraries
# (libsecret, libgcrypt, libgpg-error, libffi, ...) this script's
# real-bundled-AppImage counterpart lives in .github/workflows/ci.yml's
# "Recursive DT_NEEDED closure audit (with mutation regressions)" step,
# which mutates the real packaged closure rather than a synthetic stand-in.
set -euo pipefail

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "SKIP: audit_dependency_closure.py targets Linux ELF/readelf semantics;" \
    "not exercised on $(uname -s)."
  exit 0
fi

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
  echo "SKIP: no C compiler (cc/gcc) available to build the synthetic ELF" \
    "fixture this test needs."
  exit 0
fi

if ! command -v readelf >/dev/null 2>&1; then
  echo "SKIP: readelf (binutils) not available; required by" \
    "audit_dependency_closure.py itself."
  exit 0
fi

cc_bin="$(command -v cc || command -v gcc)"

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
auditor="$repo_root/packaging/audit_dependency_closure.py"

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

appdir="$work_dir/appdir"
mkdir -p "$appdir"
cd "$work_dir"

cat > leaf.c <<'EOF'
int test_leaf(void) { return 1; }
EOF
cat > mid.c <<'EOF'
int test_leaf(void);
int test_mid(void) { return test_leaf(); }
EOF
cat > root.c <<'EOF'
int test_mid(void);
int test_root(void) { return test_mid(); }
EOF

# Build leaf (no non-ABI deps of its own), then mid (linked against leaf),
# then root (linked against mid), each with an explicit -soname so the
# on-disk SONAME symlink audit_dependency_closure.py relies on (mirroring
# real linuxdeploy-produced AppDirs) is present, exactly like the
# find_bundled_library()-managed real libraries this script complements.
"$cc_bin" -shared -fPIC -Wl,-soname,libtestleaf.so.1 \
  -o "$appdir/libtestleaf.so.1" leaf.c
ln -s libtestleaf.so.1 "$appdir/libtestleaf.so"

"$cc_bin" -shared -fPIC -Wl,-soname,libtestmid.so.1 \
  -o "$appdir/libtestmid.so.1" mid.c -L"$appdir" -l:libtestleaf.so.1
ln -s libtestmid.so.1 "$appdir/libtestmid.so"

"$cc_bin" -shared -fPIC -Wl,-soname,libtestroot.so.1 \
  -o "$appdir/libtestroot.so.1" root.c -L"$appdir" -l:libtestmid.so.1
ln -s libtestroot.so.1 "$appdir/libtestroot.so"

echo "Synthetic chain built:"
ls "$appdir"

# --- Case 1: full chain present -> the audit must pass and report all
# three libraries (root, mid, leaf) as the resolved closure.
result_1="$(python3 "$auditor" "$appdir" --root libtestroot.so.1 --list-only)"
expected_1=$'libtestleaf.so.1\nlibtestmid.so.1\nlibtestroot.so.1'
[[ "$result_1" == "$expected_1" ]] \
  || fail "case 1 (full chain): expected closure '$expected_1', got '$result_1'"
echo "PASS: full synthetic chain resolves and lists exactly root+mid+leaf"

# --- Case 2: full chain present -> non-list-only invocation must also
# exit 0 (the human-readable path is a separate code path from --list-only
# and must independently agree the closure is satisfied).
python3 "$auditor" "$appdir" --root libtestroot.so.1 >/dev/null \
  || fail "case 2: expected exit 0 for the full chain in human-readable mode"
echo "PASS: full synthetic chain passes in human-readable mode too"

# --- Case 4 (run before the mutating case 3 below, since it needs the
# full chain intact): an ABI-allowlisted name (e.g. libc.so.6, which the
# compiled libraries above all transitively NEED via the C runtime) must
# never be reported as missing, even though it is never present in this
# synthetic AppDir at all -- proving the allowlist path is real, not dead
# code.
output_4="$(python3 "$auditor" "$appdir" --root libtestmid.so.1 2>&1)" \
  || fail "case 4: expected exit 0 (libc.so.6 is ABI-allowlisted), got failure: $output_4"
echo "$output_4" | grep -q "libc.so.6" \
  && fail "case 4: libc.so.6 should never be reported as missing (it is ABI-allowlisted): $output_4"
echo "PASS: ABI-allowlisted libc.so.6 is never treated as a missing dependency"

# --- Case 6 (run before the mutating case 3 below, since it reuses the
# original appdir's still-intact libtestleaf.so.1 as its escape target): a
# bundled SONAME symlink that resolves *outside* the AppDir being audited
# -- mirroring an accidental or maliciously crafted linuxdeploy output --
# must be rejected outright, even though the escape target is a real,
# valid, readelf-parseable library. Silently following it would let a
# library that merely happens to exist elsewhere on the machine running
# the audit be misidentified as "bundled", defeating the whole
# host-independence guarantee this script exists to provide.
escape_dir="$work_dir/appdir_escape"
mkdir -p "$escape_dir"
cp "$appdir/libtestroot.so.1" "$escape_dir/"
cp "$appdir/libtestmid.so.1" "$escape_dir/"
# Instead of bundling a real copy of libtestleaf.so.1 inside escape_dir,
# plant a symlink escaping the AppDir entirely, pointing at the original
# (still valid, still-present) copy over in $appdir.
ln -s "$appdir/libtestleaf.so.1" "$escape_dir/libtestleaf.so.1"

set +e
output_6="$(python3 "$auditor" "$escape_dir" --root libtestroot.so.1 2>&1)"
case6_status=$?
set -e
[[ $case6_status -ne 0 ]] \
  || fail "case 6: expected non-zero exit for a SONAME symlink escaping the AppDir"
echo "$output_6" | grep -qi "outside" \
  || fail "case 6: failure output did not explain the symlink-escape rejection: $output_6"
echo "$output_6" | grep -q "libtestleaf.so.1" \
  || fail "case 6: failure output did not name the escaping library: $output_6"
echo "PASS: a SONAME symlink resolving outside the AppDir is rejected, not silently followed"

# A persistent standalone copy of the leaf library, used as the "outside
# the AppDir" escape target for every symlink-escape case below. Kept
# separate from $appdir/libtestleaf.so.1 (case 6's escape target) because
# that file is deleted by the mutating case 3 further down, and these
# cases must all run before case 3 too.
outside_dir="$work_dir/outside_target"
mkdir -p "$outside_dir"
cp "$appdir/libtestleaf.so.1" "$outside_dir/libtestleaf.so.1"

# --- Case 7: RELATIVE-path symlink escape. Unlike case 6 (an absolute
# target path), the on-disk symlink here is stored as a "../"-relative
# path string -- proving the auditor's escape check is based on actually
# resolving the path, not on a textual check for a leading "/" that a
# relative escape would trivially bypass.
escape_dir_rel="$work_dir/appdir_escape_relative"
mkdir -p "$escape_dir_rel"
cp "$appdir/libtestroot.so.1" "$escape_dir_rel/"
cp "$appdir/libtestmid.so.1" "$escape_dir_rel/"
ln -s "../outside_target/libtestleaf.so.1" "$escape_dir_rel/libtestleaf.so.1"

set +e
output_7="$(python3 "$auditor" "$escape_dir_rel" --root libtestroot.so.1 2>&1)"
case7_status=$?
set -e
[[ $case7_status -ne 0 ]] \
  || fail "case 7: expected non-zero exit for a relative-path SONAME symlink escaping the AppDir"
echo "$output_7" | grep -qi "outside" \
  || fail "case 7: failure output did not explain the symlink-escape rejection: $output_7"
echo "$output_7" | grep -q "libtestleaf.so.1" \
  || fail "case 7: failure output did not name the escaping library: $output_7"
echo "PASS: a relative-path SONAME symlink resolving outside the AppDir is rejected"

# --- Case 8: MULTI-HOP escape. The SONAME entry itself is a symlink to a
# bare sibling filename ("hop.so.1") that -- read in isolation -- looks
# like it stays inside the audited directory; only that second symlink
# actually escapes outside via a "../" target. This proves the auditor
# fully resolves the whole symlink chain (Path.resolve() does this) rather
# than only inspecting the first hop's immediate readlink() target.
escape_dir_hop="$work_dir/appdir_escape_multihop"
mkdir -p "$escape_dir_hop"
cp "$appdir/libtestroot.so.1" "$escape_dir_hop/"
cp "$appdir/libtestmid.so.1" "$escape_dir_hop/"
ln -s "hop.so.1" "$escape_dir_hop/libtestleaf.so.1"
ln -s "../outside_target/libtestleaf.so.1" "$escape_dir_hop/hop.so.1"

set +e
output_8="$(python3 "$auditor" "$escape_dir_hop" --root libtestroot.so.1 2>&1)"
case8_status=$?
set -e
[[ $case8_status -ne 0 ]] \
  || fail "case 8: expected non-zero exit for a multi-hop SONAME symlink chain escaping the AppDir"
echo "$output_8" | grep -qi "outside" \
  || fail "case 8: failure output did not explain the symlink-escape rejection: $output_8"
echo "PASS: a multi-hop SONAME symlink chain ultimately escaping the AppDir is rejected"

# --- Case 9: DANGLING symlink. The SONAME entry is a symlink to a sibling
# filename that was never created at all -- a broken link, not an escape
# -- and must still fail the audit (as a tooling error or an unresolved
# dependency, either way non-zero exit) rather than being silently
# skipped or misreported as present.
escape_dir_dangling="$work_dir/appdir_dangling"
mkdir -p "$escape_dir_dangling"
cp "$appdir/libtestroot.so.1" "$escape_dir_dangling/"
cp "$appdir/libtestmid.so.1" "$escape_dir_dangling/"
ln -s "libtestleaf-does-not-exist.so.1" "$escape_dir_dangling/libtestleaf.so.1"

set +e
output_9="$(python3 "$auditor" "$escape_dir_dangling" --root libtestroot.so.1 2>&1)"
case9_status=$?
set -e
[[ $case9_status -ne 0 ]] \
  || fail "case 9: expected non-zero exit for a dangling SONAME symlink"
echo "PASS: a dangling SONAME symlink is rejected, not silently treated as present"

# --- Case 10: IN-TREE-TO-OUTSIDE chain. The SONAME entry is a symlink
# into a legitimate subdirectory *inside* the audited lib_dir (a hop that,
# taken alone, clearly stays in-tree), but the file that first hop lands
# on is itself a further symlink that escapes outside via a deeper
# relative "../../" reference. This differs from case 8 (a flat two-hop
# chain within the same directory) by nesting the escape one directory
# level deeper, proving the resolver's containment check is not fooled by
# an in-tree waypoint before the eventual escape.
escape_dir_nested="$work_dir/appdir_escape_nested"
mkdir -p "$escape_dir_nested/subdir"
cp "$appdir/libtestroot.so.1" "$escape_dir_nested/"
cp "$appdir/libtestmid.so.1" "$escape_dir_nested/"
ln -s "subdir/inner.so.1" "$escape_dir_nested/libtestleaf.so.1"
ln -s "../../outside_target/libtestleaf.so.1" "$escape_dir_nested/subdir/inner.so.1"

set +e
output_10="$(python3 "$auditor" "$escape_dir_nested" --root libtestroot.so.1 2>&1)"
case10_status=$?
set -e
[[ $case10_status -ne 0 ]] \
  || fail "case 10: expected non-zero exit for an in-tree-to-outside symlink chain"
echo "$output_10" | grep -qi "outside" \
  || fail "case 10: failure output did not explain the symlink-escape rejection: $output_10"
echo "PASS: an in-tree waypoint symlink chain that ultimately escapes the AppDir is rejected"

# --- Case 3: mutation regression -- deleting the leaf (a real,
# representative non-ABI transitive dependency, required only via mid,
# not directly by root) must make the audit fail and must name the
# missing library and its requester.
rm "$appdir/libtestleaf.so.1" "$appdir/libtestleaf.so"
output_3="$(python3 "$auditor" "$appdir" --root libtestroot.so.1 2>&1)" && \
  fail "case 3 (leaf removed): expected non-zero exit, audit passed"
echo "$output_3" | grep -q "libtestleaf.so.1" \
  || fail "case 3: failure output did not name the missing library: $output_3"
echo "$output_3" | grep -q "libtestmid.so.1" \
  || fail "case 3: failure output did not name the requester: $output_3"
echo "PASS: deleting the leaf dependency correctly fails the audit and names it"

# --- Case 5: a completely absent root library (never built/copied here)
# must be rejected with a clear, non-zero-exit diagnostic rather than
# silently auditing an empty closure.
set +e
python3 "$auditor" "$appdir" --root libdoes-not-exist.so.1 >/dev/null 2>&1
case5_status=$?
set -e
[[ $case5_status -ne 0 ]] \
  || fail "case 5: expected non-zero exit for a nonexistent root library"
echo "PASS: a nonexistent root library is rejected rather than silently auditing nothing"

echo "All audit_dependency_closure.py regression cases passed."
