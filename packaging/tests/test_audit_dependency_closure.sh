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

# --- Case 11/12: --auto-roots. Builds a small synthetic AppDir/usr-style
# tree with a "plugin" ELF nested two directories deep (mirroring a real
# usr/lib/plugins/imageformats/libqjpeg.so-style layout) that NEEDs a
# library which is bundled elsewhere in the tree (usr/lib), and which is
# NOT transitively reachable from any of the existing --root chain above
# at all. This is exactly the class of gap the review item this flag
# addresses identified: a hand-picked --root list can prove the libsecret
# closure complete while never noticing a *different* bundled ELF (a Qt
# plugin, or the app's own executable) requires something never bundled.
auto_root_tree="$work_dir/usr_auto_roots"
mkdir -p "$auto_root_tree/lib" "$auto_root_tree/lib/plugins/imageformats" "$auto_root_tree/bin"

cat > pluginleaf.c <<'EOF'
int test_pluginleaf(void) { return 1; }
EOF
cat > plugin.c <<'EOF'
int test_pluginleaf(void);
int test_plugin_entry(void) { return test_pluginleaf(); }
EOF
cat > appexe.c <<'EOF'
int main(void) { return 0; }
EOF

"$cc_bin" -shared -fPIC -Wl,-soname,libpluginleaf.so.1 \
  -o "$auto_root_tree/lib/libpluginleaf.so.1" pluginleaf.c
ln -s libpluginleaf.so.1 "$auto_root_tree/lib/libpluginleaf.so"

"$cc_bin" -shared -fPIC -Wl,-soname,libqtestplugin.so \
  -o "$auto_root_tree/lib/plugins/imageformats/libqtestplugin.so" plugin.c \
  -L"$auto_root_tree/lib" -l:libpluginleaf.so.1

"$cc_bin" -o "$auto_root_tree/bin/testapp" appexe.c

# Without --auto-roots, only explicitly-named roots are walked, so the
# nested plugin's own dependency on libpluginleaf.so.1 is never even
# considered -- the audit reports success (0 required libraries) despite
# the plugin's real dependency existing, unaudited, elsewhere in the tree.
# This first assertion documents the exact gap being closed, not merely
# that the new flag works in isolation.
output_11_before="$(python3 "$auditor" "$auto_root_tree" --root testapp --list-only 2>&1)" \
  || fail "case 11 (baseline): expected exit 0 auditing only the app executable's own (empty) closure"
echo "$output_11_before" | grep -q "libpluginleaf" \
  && fail "case 11 (baseline): did not expect the unreachable plugin's dependency to be audited without --auto-roots: $output_11_before"
echo "PASS: without --auto-roots, a plugin's own dependency outside any --root chain is not audited (documents the gap)"

# With --auto-roots pointed at the whole tree, every real ELF (the app
# executable, the nested plugin, and the library it needs) is discovered
# and rooted, so the plugin's dependency on libpluginleaf.so.1 is now
# actually walked and resolved.
result_11="$(python3 "$auditor" "$auto_root_tree" --auto-roots "$auto_root_tree" --list-only)"
echo "$result_11" | grep -q "^libpluginleaf.so.1$" \
  || fail "case 11: expected --auto-roots to discover and resolve the nested plugin's own dependency, got: $result_11"
echo "$result_11" | grep -q "^libqtestplugin.so$" \
  || fail "case 11: expected --auto-roots to include the nested plugin itself as a discovered root, got: $result_11"
echo "PASS: --auto-roots discovers every real ELF (nested plugin + app executable) and resolves their dependencies"

# Case 12: deleting the plugin's own dependency must now be caught by
# --auto-roots (proving this is a real, load-bearing check, not a
# vacuous pass), even though it would have gone completely unnoticed by
# the --root testapp-only baseline above.
rm "$auto_root_tree/lib/libpluginleaf.so.1" "$auto_root_tree/lib/libpluginleaf.so"
set +e
output_12="$(python3 "$auditor" "$auto_root_tree" --auto-roots "$auto_root_tree" 2>&1)"
case12_status=$?
set -e
[[ $case12_status -ne 0 ]] \
  || fail "case 12: expected non-zero exit after deleting the auto-rooted plugin's own dependency"
echo "$output_12" | grep -q "libpluginleaf.so.1" \
  || fail "case 12: failure output did not name the missing plugin dependency: $output_12"
echo "PASS: --auto-roots catches a missing dependency of a plugin/executable no hand-picked --root list named"

# --- Case 13: --allow-x11-desktop-stack. Builds a root that NEEDs a
# stub library sharing an exact X11_DESKTOP_ABI_ALLOWLIST SONAME
# (libxcb.so.1) -- never a real system libxcb, just a same-named stub
# compiled locally -- then removes it from the AppDir entirely (as real
# packaging deliberately does not bundle base X11/xcb libraries) and
# proves the audit fails by default (the flag is not silently on) but
# passes once --allow-x11-desktop-stack is explicitly given.
x11_dir="$work_dir/appdir_x11"
mkdir -p "$x11_dir"
cat > x11root.c <<'EOF'
int test_x11_stub(void);
int test_x11_root(void) { return test_x11_stub(); }
EOF
cat > x11stub.c <<'EOF'
int test_x11_stub(void) { return 1; }
EOF
"$cc_bin" -shared -fPIC -Wl,-soname,libxcb.so.1 \
  -o "$work_dir/libxcb.so.1.tmp" x11stub.c
cp "$work_dir/libxcb.so.1.tmp" "$x11_dir/libxcb.so.1"
"$cc_bin" -shared -fPIC -Wl,-soname,libtestx11root.so.1 \
  -o "$x11_dir/libtestx11root.so.1" x11root.c -L"$x11_dir" -l:libxcb.so.1
# Remove the stub itself -- only the NEEDED entry naming it remains --
# simulating linuxdeploy correctly refusing to bundle base X11/xcb.
rm "$x11_dir/libxcb.so.1"

set +e
output_13_default="$(python3 "$auditor" "$x11_dir" --root libtestx11root.so.1 2>&1)"
case13_default_status=$?
set -e
[[ $case13_default_status -ne 0 ]] \
  || fail "case 13 (default): expected non-zero exit for missing libxcb.so.1 without --allow-x11-desktop-stack"
echo "$output_13_default" | grep -q "libxcb.so.1" \
  || fail "case 13 (default): failure output did not name libxcb.so.1: $output_13_default"
echo "PASS: libxcb.so.1 is reported missing by default (the X11 desktop-stack allowlist is opt-in, never implicit)"

python3 "$auditor" "$x11_dir" --root libtestx11root.so.1 --allow-x11-desktop-stack >/dev/null \
  || fail "case 13 (--allow-x11-desktop-stack): expected exit 0 once the flag is explicitly given"
echo "PASS: --allow-x11-desktop-stack explicitly permits libxcb.so.1 to resolve from the host"

# --- Case 14: ambiguous duplicate basename. Two different files (proven
# different by distinct real content, not merely different paths) sharing
# the exact same basename in two different subdirectories of the same
# audited tree must be rejected outright -- silently picking one could
# resolve a NEEDED entry to the wrong file with different real content,
# masking a substitution risk the recursive (rglob) index introduced by
# --auto-roots makes newly possible (a flat, single-directory index could
# never have two entries with the same basename at all).
dup_tree="$work_dir/appdir_dup_basename"
mkdir -p "$dup_tree/a" "$dup_tree/b"
cat > dupa.c <<'EOF'
int test_dup_a(void) { return 1; }
EOF
cat > dupb.c <<'EOF'
int test_dup_b(void) { return 2; }
EOF
"$cc_bin" -shared -fPIC -Wl,-soname,libdup.so.1 -o "$dup_tree/a/libdup.so.1" dupa.c
"$cc_bin" -shared -fPIC -Wl,-soname,libdup.so.1 -o "$dup_tree/b/libdup.so.1" dupb.c

set +e
output_14="$(python3 "$auditor" "$dup_tree" --root libdup.so.1 2>&1)"
case14_status=$?
set -e
[[ $case14_status -ne 0 ]] \
  || fail "case 14: expected non-zero exit for an ambiguous duplicate basename with different real content"
echo "$output_14" | grep -qi "ambiguous" \
  || fail "case 14: failure output did not explain the ambiguous-duplicate rejection: $output_14"
echo "PASS: an ambiguous duplicate basename with genuinely different content is rejected, not silently resolved"

# --- Case 15: non-ELF files sharing a basename must NEVER be treated as
# ambiguous, even with genuinely different content -- this is the exact
# real regression this project's own CI hit: every bundled Qt QML module
# ships its own "plugins.qmltypes" (module-specific metadata, never a
# shared object, never nameable by any DT_NEEDED tag), so two different,
# legitimately-present QML modules' own "plugins.qmltypes" files sharing
# that basename must be silently ignored by the indexer entirely, not
# raise the same "ambiguous duplicate basename" error case 14 above
# rightly raises for two genuinely-different ELF libraries.
nonelf_tree="$work_dir/appdir_nonelf_dup_basename"
mkdir -p "$nonelf_tree/qml/QtQml" "$nonelf_tree/qml/QML"
printf 'module A metadata\n' >"$nonelf_tree/qml/QtQml/plugins.qmltypes"
printf 'module B metadata, deliberately different content\n' \
  >"$nonelf_tree/qml/QML/plugins.qmltypes"
# A real root library must still be present so the walk has something to
# resolve; the two non-ELF files above are otherwise-unrelated bystanders
# in the very same recursively-audited tree.
"$cc_bin" -shared -fPIC -Wl,-soname,libnonelfcheck.so.1 \
  -o "$nonelf_tree/libnonelfcheck.so.1" dupa.c
output_15="$(python3 "$auditor" "$nonelf_tree" --root libnonelfcheck.so.1 2>&1)" \
  || fail "case 15: expected success auditing a tree with same-basename non-ELF files, got: $output_15"
echo "$output_15" | grep -qi "ambiguous" \
  && fail "case 15: non-ELF same-basename files were incorrectly treated as ambiguous: $output_15"
echo "PASS: same-basename non-ELF files (e.g. Qt QML modules' own plugins.qmltypes) are never treated as ambiguous"

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
