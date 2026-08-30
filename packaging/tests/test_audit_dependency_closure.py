#!/usr/bin/env python3
"""Tests for packaging/audit_dependency_closure.py's internal per-edge
reachability/BFS logic (round-9+ review item 4, "seen keyed only SONAME
skips same dependency from different requester/RPATH contexts; roots
collapsed basename; boolean resolver permits external path and recurses
arbitrary bundled same-basename; RUNPATH precedence vs LD_LIBRARY_PATH
wrong").

Unlike packaging/tests/test_audit_dependency_closure.sh (which needs a
real C compiler + readelf + genuine compiled ELF files, and therefore only
runs on Linux), these tests fabricate minimal fake ".so" files -- real
enough to pass _is_elf_file()'s own magic-byte sniff -- and monkeypatch
_readelf_dynamic_text() to return canned `readelf -d -W`-shaped text for
each fake file, so the actual BFS/reachability/precedence logic under test
runs identically, deterministically, and portably on macOS/Linux alike,
with no compiler or real ELF toolchain dependency at all.

Run directly:
    python3 packaging/tests/test_audit_dependency_closure.py
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

_PACKAGING_DIR = str(Path(__file__).resolve().parent.parent)
sys.path.insert(0, _PACKAGING_DIR)
try:
    import audit_dependency_closure as audit  # noqa: E402
finally:
    sys.path.remove(_PACKAGING_DIR)


def _write_fake_elf(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(audit._ELF_MAGIC + b"\x00" * 32)


def _needed_and_runpath_text(needed: list[str], runpath: str | None) -> str:
    """Builds fake `readelf -d -W` stdout text carrying exactly the given
    NEEDED entries (in order) and, if given, a single DT_RUNPATH entry --
    matching the exact regex shapes _NEEDED_RE/_RUNPATH_RE parse."""
    lines = [
        f" 0x0000000000000001 (NEEDED)             Shared library: [{name}]"
        for name in needed
    ]
    if runpath is not None:
        lines.append(
            f" 0x000000000000001d (RUNPATH)            Library runpath: [{runpath}]"
        )
    return "\n".join(lines) + "\n"


def _needed_and_rpath_text(needed: list[str], rpath: str | None) -> str:
    lines = [
        f" 0x0000000000000001 (NEEDED)             Shared library: [{name}]"
        for name in needed
    ]
    if rpath is not None:
        lines.append(
            f" 0x000000000000000f (RPATH)              Library rpath: [{rpath}]"
        )
    return "\n".join(lines) + "\n"


class EffectiveSearchDirsPrecedenceTests(unittest.TestCase):
    """Round-9+ review item 4 ("RUNPATH precedence vs LD_LIBRARY_PATH
    wrong"): real ld.so precedence is the OPPOSITE for the two tags --
    DT_RPATH (legacy) is searched BEFORE LD_LIBRARY_PATH, while DT_RUNPATH
    (modern) is searched AFTER it -- so a caller-supplied global search
    dir (representing LD_LIBRARY_PATH) must appear in a DIFFERENT
    position in the returned ordering depending on which tag the object
    actually carries."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()
        self.own_subdir = self.lib_dir / "own"
        self.own_subdir.mkdir()
        self.requester = self.lib_dir / "requester.so.1"
        _write_fake_elf(self.requester)
        # audit_closure() always resolves global_search_dirs entries
        # before calling _effective_search_dirs (see its own
        # `global_search_dirs = [d.resolve() for d in global_search_dirs]`
        # line) -- match that here so this direct unit test exercises the
        # exact same already-resolved inputs the real caller supplies.
        self.global_dir = self.lib_dir.resolve()  # LD_LIBRARY_PATH stand-in

    def test_runpath_is_searched_after_global_search_dirs(self) -> None:
        text = _needed_and_runpath_text([], "$ORIGIN/own")
        dirs = audit._effective_search_dirs(
            self.requester, text, [self.global_dir], self.lib_dir.resolve()
        )
        self.assertEqual(
            dirs, [self.global_dir, self.own_subdir.resolve()]
        )

    def test_rpath_is_searched_before_global_search_dirs(self) -> None:
        text = _needed_and_rpath_text([], "$ORIGIN/own")
        dirs = audit._effective_search_dirs(
            self.requester, text, [self.global_dir], self.lib_dir.resolve()
        )
        self.assertEqual(
            dirs, [self.own_subdir.resolve(), self.global_dir]
        )

    def test_runpath_entry_outside_appdir_is_never_included(self) -> None:
        # Round-9+ review item 4 ("boolean resolver permits external
        # path"): an absolute RUNPATH entry naming a directory OUTSIDE
        # the AppDir being audited must never be silently trusted --
        # whatever happens to exist at that path on the specific machine
        # running the audit is never guaranteed to exist, or to be
        # ABI-compatible, on a real target machine.
        #
        # Round-N+ review (MEDIUM, "silently drops external DT_RPATH
        # that runtime may search before LD_LIBRARY_PATH"): merely
        # OMITTING it (as if it simply weren't present) is not enough
        # either -- a real ld.so still searches that exact external
        # directory, so this must be a hard, reported failure, never a
        # silent pass-through to whatever bundled directory comes next.
        with tempfile.TemporaryDirectory() as external:
            text = _needed_and_runpath_text([], external)
            with self.assertRaises(audit.ClosureAuditError):
                audit._effective_search_dirs(
                    self.requester, text, [], self.lib_dir.resolve()
                )

    def test_rpath_entry_outside_appdir_is_never_included(self) -> None:
        with tempfile.TemporaryDirectory() as external:
            text = _needed_and_rpath_text([], external)
            with self.assertRaises(audit.ClosureAuditError):
                audit._effective_search_dirs(
                    self.requester, text, [], self.lib_dir.resolve()
                )



class PerEdgeReachabilityTests(unittest.TestCase):
    """Round-9+ review item 4 ("seen keyed only SONAME skips same
    dependency from different requester/RPATH contexts"): the core BFS
    fix -- two different requesters naming the identical dependency name
    must each be independently reachability-checked, never short-
    circuited by whichever requester happened to be processed first."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()

        # libroot.so.1 (root) NEEDs libb.so.1 then liba.so.1 (in THIS
        # exact order -- see the queue-ordering comment below for why the
        # order matters to reliably reproduce the masking bug pre-fix).
        self.root = self.lib_dir / "libroot.so.1"
        _write_fake_elf(self.root)

        # liba.so.1 lives in subdir A, whose own $ORIGIN-relative RUNPATH
        # is just itself -- and it, in turn, NEEDs libshared.so.1, which
        # is bundled ONLY inside subdir A.
        self.dir_a = self.lib_dir / "dir_a"
        self.lib_a = self.dir_a / "liba.so.1"
        _write_fake_elf(self.lib_a)

        # libb.so.1 lives in subdir B, whose own RUNPATH is subdir B
        # itself -- which does NOT contain libshared.so.1 at all. libb
        # also NEEDs libshared.so.1.
        self.dir_b = self.lib_dir / "dir_b"
        self.lib_b = self.dir_b / "libb.so.1"
        _write_fake_elf(self.lib_b)

        self.lib_shared = self.dir_a / "libshared.so.1"
        _write_fake_elf(self.lib_shared)

        self.dynamic_text = {
            self.root: _needed_and_runpath_text(
                ["libb.so.1", "liba.so.1"], "$ORIGIN/dir_a:$ORIGIN/dir_b"
            ),
            self.lib_a: _needed_and_runpath_text(["libshared.so.1"], "$ORIGIN"),
            self.lib_b: _needed_and_runpath_text(["libshared.so.1"], "$ORIGIN"),
            self.lib_shared: _needed_and_runpath_text([], None),
        }

    def _run_audit(self):
        def fake_dynamic_text(path: Path) -> str:
            return self.dynamic_text[path]

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ):
            return audit.audit_closure(self.lib_dir, ["libroot.so.1"])

    def test_second_requesters_own_unreachable_edge_is_independently_reported(
        self,
    ) -> None:
        # Queue processing order (LIFO queue.pop()): root's own NEEDED
        # list is pushed as [libb, liba], so liba (pushed last) is popped
        # -- and therefore reachability-checked and recursed into --
        # FIRST. Its own edge to libshared.so.1 (reachable, since
        # dir_a's RUNPATH is dir_a itself) is therefore processed BEFORE
        # libb's identical-named edge to the SAME libshared.so.1 (NOT
        # reachable from dir_b's own RUNPATH). Pre-fix (name-only `seen`)
        # this ordering is exactly what let libb's own genuinely-broken
        # edge be silently skipped -- `if name in seen: continue` fired
        # the instant libshared.so.1 was first seen via liba's (reachable)
        # edge, so libb's edge was never even reachability-checked, and
        # no error was ever reported for it at all.
        bundled_closure, missing, unreachable, _ = self._run_audit()

        self.assertEqual(missing, {})
        self.assertIn("liba.so.1", bundled_closure)
        self.assertIn("libb.so.1", bundled_closure)
        # The decisive assertion: libb's own broken edge to libshared.so.1
        # is reported, even though liba's edge to the identical name
        # already succeeded first.
        self.assertIn("libshared.so.1", unreachable)
        self.assertEqual(unreachable["libshared.so.1"], ["libb.so.1"])
        # liba's own successful edge is still correctly recorded too --
        # the fix must not turn a genuinely reachable edge into a false
        # negative for the sibling requester that DID resolve it.
        self.assertIn("libshared.so.1", bundled_closure)

    def test_root_name_never_masks_an_unrelated_later_edge(self) -> None:
        # Round-9+ review item 4 ("roots collapsed basename"): a root is
        # exempt from reachability entirely (requester=None) -- this must
        # never also silently exempt an unrelated LATER dependency edge
        # that happens to share the same bare basename as a root. Add a
        # second, independent root sharing "liba.so.1"'s own name is not
        # meaningful here (roots are already deduplicated at the CLI
        # layer) -- the real risk this test targets is the (name,
        # requester) tuple keying itself: a root's edge key is
        # (name, None), which can never collide with any real dependency
        # edge's key (name, <some Path>), by construction.
        bundled_closure, missing, unreachable, seen_names = self._run_audit()
        self.assertIn("liba.so.1", bundled_closure)
        self.assertIn("libb.so.1", bundled_closure)


class ExternalPathLeakageTests(unittest.TestCase):
    """Round-9+ review item 4 ("boolean resolver permits external path
    and recurses arbitrary bundled same-basename") plus round-N+ review
    (MEDIUM, "silently drops external DT_RPATH that runtime may search
    before LD_LIBRARY_PATH"): an absolute RUNPATH entry pointing outside
    the AppDir must never make a dependency appear reachable, even when a
    same-named file genuinely exists at that external path on the
    machine running the audit -- and, since a REAL loader on some target
    machine would still search that exact external directory (possibly
    before this project's own bundled search directories for a legacy
    DT_RPATH), the whole audit must fail loudly rather than silently
    reporting the edge as merely "unreachable" and continuing."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()
        self.external_tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.external_tmp.cleanup)
        self.external_dir = Path(self.external_tmp.name).resolve()

        self.root = self.lib_dir / "libroot.so.1"
        _write_fake_elf(self.root)

        # libshared.so.1 IS bundled inside the AppDir -- but only in a
        # subdirectory the root's own RUNPATH never actually names.
        self.dir_c = self.lib_dir / "dir_c"
        self.lib_shared_bundled = self.dir_c / "libshared.so.1"
        _write_fake_elf(self.lib_shared_bundled)

        # A same-named file also happens to exist at the external,
        # audit-host-only absolute path named by the root's own RUNPATH.
        self.lib_shared_external = self.external_dir / "libshared.so.1"
        _write_fake_elf(self.lib_shared_external)

        self.dynamic_text = {
            self.root: _needed_and_runpath_text(
                ["libshared.so.1"], str(self.external_dir)
            ),
            self.lib_shared_bundled: _needed_and_runpath_text([], None),
        }

    def test_external_runpath_directory_fails_the_whole_audit(self) -> None:
        def fake_dynamic_text(path: Path) -> str:
            return self.dynamic_text[path]

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ):
            with self.assertRaises(audit.ClosureAuditError) as ctx:
                audit.audit_closure(self.lib_dir, ["libroot.so.1"])

        # The error must name the offending requester and the external
        # directory itself, so a real failure is actually actionable.
        self.assertIn(str(self.root), str(ctx.exception))
        self.assertIn(str(self.external_dir), str(ctx.exception))



class ExactReachablePathIsUsedForRecursionTests(unittest.TestCase):
    """Round-N+ review (HIGH, "index/root still one path per basename;
    reachability boolean then recursion chooses index[name], not
    loader-selected duplicate"): when the identical basename exists at
    TWO different paths in the tree, the file actually explored for
    FURTHER dependencies must be the exact one a real loader would select
    for THIS requester (via its own effective RUNPATH/RPATH/global search
    dirs) -- never whichever single occurrence the flat basename index
    happens to remember (a pure artifact of directory-traversal order,
    unrelated to real loader precedence).

    Deliberately constructed so the two occurrences' own DT_NEEDED
    entries genuinely differ (one leads to a real, present dependency;
    the other names a dependency that exists nowhere at all), and so
    the WRONG occurrence sorts LAST alphabetically/by rglob traversal
    order (making it the one a naive last-write-wins `index[name]`
    would remember) while the CORRECT, reachable one is only named by
    the requester's own RUNPATH. Pre-fix, this made the audit either
    silently miss the real dependency (never explored) or falsely
    report a phantom dependency that was never actually going to be
    loaded at all -- either way, a wrong answer produced with full
    confidence."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()

        self.root = self.lib_dir / "libroot.so.1"
        _write_fake_elf(self.root)

        # dir_good is the ONLY directory the root's own RUNPATH actually
        # names -- this is the occurrence a real loader would select.
        self.dir_good = self.lib_dir / "dir_good"
        self.dir_good_dup = self.dir_good / "libdup.so.1"
        _write_fake_elf(self.dir_good_dup)
        self.good_leaf = self.dir_good / "libgoodleaf.so.1"
        _write_fake_elf(self.good_leaf)

        # dir_zzz_wrong sorts AFTER dir_good (both alphabetically and in
        # rglob's own traversal order), so a last-write-wins flat index
        # keyed only by basename would remember THIS occurrence instead
        # -- despite the root's own RUNPATH never naming this directory
        # at all.
        self.dir_wrong = self.lib_dir / "dir_zzz_wrong"
        self.dir_wrong_dup = self.dir_wrong / "libdup.so.1"
        _write_fake_elf(self.dir_wrong_dup)

        self.dynamic_text = {
            self.root: _needed_and_runpath_text(
                ["libdup.so.1"], "$ORIGIN/dir_good"
            ),
            self.dir_good_dup: _needed_and_runpath_text(
                ["libgoodleaf.so.1"], "$ORIGIN"
            ),
            self.good_leaf: _needed_and_runpath_text([], None),
            # The wrong occurrence's own dependency names something that
            # is never bundled anywhere in the tree at all -- if the
            # audit ever mistakenly explores THIS file instead of the
            # genuinely-reachable one, this phantom name would appear
            # somewhere in the result (missing or bundled_closure);
            # since it is never actually loadable, it must never appear
            # in either.
            self.dir_wrong_dup: _needed_and_runpath_text(
                ["libphantom-never-bundled.so.1"], "$ORIGIN"
            ),
        }

    def test_recursion_uses_the_reachable_occurrence_not_the_indexed_one(
        self,
    ) -> None:
        def fake_dynamic_text(path: Path) -> str:
            return self.dynamic_text[path]

        # Force the flat basename index to deliberately remember the
        # WRONG occurrence for "libdup.so.1" -- regardless of this
        # filesystem's own real (unspecified, non-deterministic) rglob
        # traversal order -- so this regression reliably exercises
        # exactly the bug being fixed on every platform/filesystem,
        # rather than depending on incidental directory-iteration order
        # to happen to produce the vulnerable index contents.
        forced_index = {
            "libroot.so.1": [self.root],
            "libdup.so.1": [self.dir_wrong_dup],
            "libgoodleaf.so.1": [self.good_leaf],
        }

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ), mock.patch.object(
            audit, "_index_lib_dir", return_value=forced_index
        ):
            bundled_closure, missing, unreachable, _ = audit.audit_closure(
                self.lib_dir, ["libroot.so.1"]
            )

        # The genuinely-reachable copy's own real dependency must be
        # discovered and marked bundled, EVEN THOUGH the (deliberately
        # wrong) flat index above claims a completely different file for
        # this exact same basename.
        self.assertIn("libdup.so.1", bundled_closure)
        self.assertIn("libgoodleaf.so.1", bundled_closure)
        self.assertEqual(missing, {})
        self.assertEqual(unreachable, {})
        # The WRONG (unreachable-from-this-requester) occurrence's own
        # phantom dependency must never leak into the result in any
        # form -- proving the wrong file's own DT_NEEDED entries were
        # never actually consulted at all.
        self.assertNotIn("libphantom-never-bundled.so.1", bundled_closure)
        self.assertNotIn("libphantom-never-bundled.so.1", missing)
        self.assertNotIn("libphantom-never-bundled.so.1", unreachable)


class DuplicateBasenameRootsTests(unittest.TestCase):
    """Round-N+ review (HIGH, "auto roots still dict[basename,Path],
    losing duplicates/location"): --auto-roots must preserve EVERY
    discovered root's own exact, distinct path -- never collapse two
    different ELF files that happen to share a bare basename (in
    different real directories) down to a single "first match wins"
    entry, which would silently drop the second one's own dependency
    edges from the audit entirely."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()

        # Two DIFFERENT real files, deliberately sharing the exact same
        # basename ("libplugin.so.1"), in two different directories --
        # each with ITS OWN, different, real dependency reachable only
        # from ITS OWN directory's $ORIGIN.
        self.dir_alpha = self.lib_dir / "dir_alpha"
        self.dir_zeta = self.lib_dir / "dir_zeta"
        self.plugin_in_alpha = self.dir_alpha / "libplugin.so.1"
        self.plugin_in_zeta = self.dir_zeta / "libplugin.so.1"
        _write_fake_elf(self.plugin_in_alpha)
        _write_fake_elf(self.plugin_in_zeta)

        self.leaf_in_alpha = self.dir_alpha / "libonlyinalpha.so.1"
        self.leaf_in_zeta = self.dir_zeta / "libonlyinzeta.so.1"
        _write_fake_elf(self.leaf_in_alpha)
        _write_fake_elf(self.leaf_in_zeta)

        self.dynamic_text = {
            self.plugin_in_alpha: _needed_and_runpath_text(
                ["libonlyinalpha.so.1"], "$ORIGIN"
            ),
            self.plugin_in_zeta: _needed_and_runpath_text(
                ["libonlyinzeta.so.1"], "$ORIGIN"
            ),
            self.leaf_in_alpha: _needed_and_runpath_text([], None),
            self.leaf_in_zeta: _needed_and_runpath_text([], None),
        }

    def _run_audit_with_both_roots_as_tuples(self):
        def fake_dynamic_text(path: Path) -> str:
            return self.dynamic_text[path]

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ):
            return audit.audit_closure(
                self.lib_dir,
                [
                    ("libplugin.so.1", self.plugin_in_alpha),
                    ("libplugin.so.1", self.plugin_in_zeta),
                ],
            )

    def test_both_same_basename_roots_are_independently_audited(self) -> None:
        # Without the fix, only ONE of these two same-named roots would
        # ever be processed (whichever the flat index happened to
        # remember), silently dropping the other's own real dependency
        # edge from the audit -- this would show up as bundled_closure
        # missing one of the two leaf libraries even though both are
        # genuinely present and reachable.
        bundled_closure, missing, unreachable, _ = (
            self._run_audit_with_both_roots_as_tuples()
        )
        self.assertEqual(missing, {})
        self.assertEqual(unreachable, {})
        self.assertIn("libonlyinalpha.so.1", bundled_closure)
        self.assertIn("libonlyinzeta.so.1", bundled_closure)

    def test_discover_elf_roots_preserves_every_distinct_path_regardless_of_name_order(
        self,
    ) -> None:
        # Round-N+ review ("reverse names mutation still catches broken
        # copy"): must not depend on alphabetical/traversal-order luck.
        # Rename the two directories so the SECOND one alphabetically
        # ("dir_zzz_first_alphabetically" isn't right -- construct
        # explicit reversed names) still must not lose either path.
        reversed_root = Path(self.tmp.name) / "reversed_order_check"
        dir_first = reversed_root / "aaa_dir"
        dir_second = reversed_root / "zzz_dir"
        plugin_first = dir_first / "libsameorder.so.1"
        plugin_second = dir_second / "libsameorder.so.1"
        _write_fake_elf(plugin_first)
        _write_fake_elf(plugin_second)

        discovered = audit._discover_elf_roots(reversed_root)
        discovered_paths = {p.resolve() for p in discovered}
        self.assertIn(plugin_first.resolve(), discovered_paths)
        self.assertIn(plugin_second.resolve(), discovered_paths)
        self.assertEqual(
            len([p for p in discovered if p.name == "libsameorder.so.1"]), 2
        )

    def test_bare_string_root_still_resolves_via_flat_index_for_backward_compat(
        self,
    ) -> None:
        # A plain str entry (the pre-existing --root convention, still
        # used by every hand-picked --root caller) must keep resolving
        # via the flat, name-only index exactly as before -- this is the
        # deliberately-narrower fallback path for callers that never
        # claimed to know any particular root's real location.
        def fake_dynamic_text(path: Path) -> str:
            return self.dynamic_text[path]

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ):
            bundled_closure, missing, unreachable, _ = audit.audit_closure(
                self.lib_dir, ["libonlyinalpha.so.1"]
            )
        self.assertEqual(missing, {})
        self.assertIn("libonlyinalpha.so.1", bundled_closure)


class InheritedRpathTransitivityTests(unittest.TestCase):
    """Round-N+ review (HIGH, "inherited DT_RPATH missing ... glibc
    applies DT_RPATH transitively when child lacks own RUNPATH/RPATH;
    auditor may select valid global C while runtime chooses corrupt
    higher-priority parent/private C"): real glibc dependency resolution
    treats an ancestor's legacy DT_RPATH as part of a single, cumulative
    search scope that keeps applying to every further DT_NEEDED
    resolution down the whole chain -- for as long as no intermediate
    object resets it with its own DT_RUNPATH -- never just the immediate
    requester's own tag in isolation."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.lib_dir = Path(self.tmp.name).resolve()

    def _write(self, rel: str) -> Path:
        path = self.lib_dir / rel
        _write_fake_elf(path)
        return path

    def _run_audit(self, dynamic_text: dict[Path, str], root: str = "libroot.so.1"):
        def fake_dynamic_text(path: Path) -> str:
            return dynamic_text[path]

        with mock.patch.object(
            audit, "_readelf_dynamic_text", side_effect=fake_dynamic_text
        ):
            return audit.audit_closure(self.lib_dir, [root])

    def test_ancestor_rpath_reaches_a_grandchilds_own_dependency_resolution(
        self,
    ) -> None:
        # A (root, DT_RPATH="$ORIGIN/zzz_private") -> B (no own tag) ->
        # "libc.so.1", which exists at BOTH a private location (only
        # reachable via A's inherited, transitive DT_RPATH) and a global
        # one (reachable via global_search_dirs alone). A real ld.so
        # load of B's own "libc.so.1" NEEDED entry searches A's still-
        # live DT_RPATH scope FIRST (legacy DT_RPATH precedes
        # LD_LIBRARY_PATH) -- so the REAL runtime resolves to the
        # PRIVATE copy, never the global one, regardless of which one
        # this audit's own global_search_dirs happens to name.
        #
        # The private directory is deliberately named to sort AFTER the
        # global one (both alphabetically and in any traversal order),
        # so this test cannot pass merely by incidental directory-name
        # ordering luck.
        root = self._write("libroot.so.1")
        libb = self._write("libb.so.1")
        libc_private = self._write("zzz_private/libc.so.1")
        libc_global = self._write("libc.so.1")
        private_marker = self._write("zzz_private/libprivatemarker.so.1")
        global_marker = self._write("libglobalmarker.so.1")

        dynamic_text = {
            root: _needed_and_rpath_text(["libb.so.1"], "$ORIGIN/zzz_private"),
            libb: _needed_and_runpath_text(["libc.so.1"], None),
            libc_private: _needed_and_runpath_text(
                ["libprivatemarker.so.1"], None
            ),
            libc_global: _needed_and_runpath_text(["libglobalmarker.so.1"], None),
            private_marker: _needed_and_runpath_text([], None),
            global_marker: _needed_and_runpath_text([], None),
        }

        bundled_closure, missing, unreachable, _ = self._run_audit(dynamic_text)

        self.assertEqual(missing, {})
        self.assertEqual(unreachable, {})
        # Decisive assertion: B's own "libc.so.1" resolved to the
        # PRIVATE copy (its own further dependency is discovered),
        # never the global one -- matching a real loader, which
        # searches A's inherited DT_RPATH before global_search_dirs.
        self.assertIn("libprivatemarker.so.1", bundled_closure)
        self.assertNotIn("libglobalmarker.so.1", bundled_closure)

    def test_intermediate_runpath_stops_further_propagation_to_grandchildren(
        self,
    ) -> None:
        # A (root, DT_RPATH="$ORIGIN/zzz_private") -> B (its OWN
        # DT_RUNPATH="$ORIGIN/ownrunpath", non-transitive) -> C (no own
        # tag) -> "libd.so.1", present at both a private location (only
        # reachable via A's DT_RPATH) and a global one. Because B
        # carries its own DT_RUNPATH, real glibc scoping resets: A's
        # DT_RPATH must NOT keep propagating past B to C's own
        # resolution of libd.so.1 -- C must resolve the GLOBAL copy,
        # never the private one, exactly the opposite outcome from the
        # previous test (where no DT_RUNPATH was ever in the chain).
        root = self._write("libroot.so.1")
        libb = self._write("libb.so.1")
        own_runpath_dir = self.lib_dir / "ownrunpath"
        own_runpath_dir.mkdir()
        libc = self._write("libc.so.1")
        libd_private = self._write("zzz_private/libd.so.1")
        libd_global = self._write("libd.so.1")
        private_marker = self._write("zzz_private/libdprivatemarker.so.1")
        global_marker = self._write("libdglobalmarker.so.1")

        dynamic_text = {
            root: _needed_and_rpath_text(["libb.so.1"], "$ORIGIN/zzz_private"),
            libb: _needed_and_runpath_text(["libc.so.1"], "$ORIGIN/ownrunpath"),
            libc: _needed_and_runpath_text(["libd.so.1"], None),
            libd_private: _needed_and_runpath_text(
                ["libdprivatemarker.so.1"], None
            ),
            libd_global: _needed_and_runpath_text(["libdglobalmarker.so.1"], None),
            private_marker: _needed_and_runpath_text([], None),
            global_marker: _needed_and_runpath_text([], None),
        }

        bundled_closure, missing, unreachable, _ = self._run_audit(dynamic_text)

        self.assertEqual(missing, {})
        self.assertEqual(unreachable, {})
        # Decisive assertion: propagation stopped at B (its own
        # DT_RUNPATH resets inheritance for everything below it) -- C's
        # own resolution of libd.so.1 must use the GLOBAL copy, never
        # the private one that only A's (now-severed) DT_RPATH reached.
        self.assertIn("libdglobalmarker.so.1", bundled_closure)
        self.assertNotIn("libdprivatemarker.so.1", bundled_closure)

    def test_same_edge_reached_via_two_different_inherited_chains_is_independently_resolved(
        self,
    ) -> None:
        # Round-N+ review ("BFS state/resolution key includes ordered
        # inherited legacy-RPATH chain"): a diamond shape where the
        # IDENTICAL (name, requester) edge -- "leaf.so.1" needed by the
        # single shared "libshared.so.1" file -- is reached via TWO
        # different accumulated ancestor contexts: once via "libp.so.1"
        # (which carries its own DT_RPATH, extending the inherited
        # chain), and once via "libq.so.1" (which carries no tag at all,
        # so the chain it passes through is empty). A context-blind
        # (name, requester) key would process only whichever of these
        # two edges the LIFO queue happens to pop first, silently
        # masking the other context's own, genuinely different,
        # resolution outcome.
        root = self._write("libroot.so.1")
        libp = self._write("libp.so.1")
        libq = self._write("libq.so.1")
        libshared = self._write("libshared.so.1")
        leaf_private = self._write("zzz_p_private/leaf.so.1")
        leaf_global = self._write("leaf.so.1")
        private_marker = self._write("zzz_p_private/leaf_private_marker.so.1")
        global_marker = self._write("leaf_global_marker.so.1")

        dynamic_text = {
            root: _needed_and_runpath_text(["libp.so.1", "libq.so.1"], None),
            libp: _needed_and_rpath_text(
                ["libshared.so.1"], "$ORIGIN/zzz_p_private"
            ),
            libq: _needed_and_runpath_text(["libshared.so.1"], None),
            libshared: _needed_and_runpath_text(["leaf.so.1"], None),
            leaf_private: _needed_and_runpath_text(
                ["leaf_private_marker.so.1"], None
            ),
            leaf_global: _needed_and_runpath_text(
                ["leaf_global_marker.so.1"], None
            ),
            private_marker: _needed_and_runpath_text([], None),
            global_marker: _needed_and_runpath_text([], None),
        }

        bundled_closure, missing, unreachable, _ = self._run_audit(dynamic_text)

        self.assertEqual(missing, {})
        self.assertEqual(unreachable, {})
        # Both contexts' own distinct resolutions of "leaf.so.1" must be
        # independently discovered -- neither masks the other.
        self.assertIn("leaf_private_marker.so.1", bundled_closure)
        self.assertIn("leaf_global_marker.so.1", bundled_closure)


class NextInheritedRpathChainUnitTests(unittest.TestCase):
    """Direct unit coverage of _next_inherited_rpath_chain()'s exact
    propagation rule, independent of the full BFS."""

    def test_runpath_resets_propagation_to_empty(self) -> None:
        inherited = (Path("/a"), Path("/b"))
        result = audit._next_inherited_rpath_chain(
            "runpath", [Path("/own")], inherited
        )
        self.assertEqual(result, ())

    def test_rpath_extends_the_inherited_chain(self) -> None:
        inherited = (Path("/a"),)
        result = audit._next_inherited_rpath_chain(
            "rpath", [Path("/own")], inherited
        )
        self.assertEqual(result, (Path("/a"), Path("/own")))

    def test_no_tag_passes_the_inherited_chain_through_unchanged(self) -> None:
        inherited = (Path("/a"), Path("/b"))
        result = audit._next_inherited_rpath_chain(None, [], inherited)
        self.assertEqual(result, inherited)

    def test_duplicate_entries_are_not_repeated_when_extending(self) -> None:
        inherited = (Path("/a"),)
        result = audit._next_inherited_rpath_chain(
            "rpath", [Path("/a"), Path("/new")], inherited
        )
        self.assertEqual(result, (Path("/a"), Path("/new")))


if __name__ == "__main__":
    unittest.main()
