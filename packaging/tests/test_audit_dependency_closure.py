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


if __name__ == "__main__":
    unittest.main()
