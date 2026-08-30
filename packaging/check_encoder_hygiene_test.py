#!/usr/bin/env python3
"""Unit tests for packaging/check_encoder_hygiene.py's pure decision logic
(AllowlistEntry, classify(), the ALLOWLIST/ALLOWLIST_BY_KEY exact-count
membership rule, compile-argument sanitization, representative-compile-
args selection, the "missing/miscounted allowlist entry" staleness
check, and _validate_closure_rootedness()'s symlink-aware self-rootedness
enforcement), using synthetic Finding/AllowlistEntry records, fake
compile_commands.json-shaped data, and (for the rootedness tests, which
genuinely need real filesystem entries/symlinks -- see
ClosureRootednessTests) a scratch directory tree created under this
repository's own gitignored build/ directory rather than the real
project layout or the system temp directory, rather than a real compiler
invocation (fast, deterministic, no Clang/libclang dependency for this
file specifically -- the real, header-driven AST walk and
clang_getInclusions()-based inclusion-graph audit against the actual
project sources is exercised separately by
`mise run contracts:check-encoder-hygiene`, which this test suite
intentionally does not invoke).

Run directly: `python3 packaging/check_encoder_hygiene_test.py`
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encoder_hygiene as ceh


def _finding(
    file: str = "src/domain/RawJson.h",
    line: int = 1,
    display_name: str = "someMethod()",
    canonical_return_type: str = "QJsonObject",
    usr: str = "c:@N@Arkham@F@someMethod#1",
) -> ceh.Finding:
    return ceh.Finding(
        file=file,
        line=line,
        display_name=display_name,
        canonical_return_type=canonical_return_type,
        usr=usr,
    )


class ClassifyTests(unittest.TestCase):
    def test_non_qjson_return_type_is_always_allowed(self) -> None:
        finding = _finding(canonical_return_type="Arkham::ValueOrError<QString>", usr="c:@N@Foo@F@bar#1")
        self.assertEqual(ceh.classify(finding), "allowed")

    def test_exact_allowlisted_entry_is_allowed_with_matching_count(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file, usr=entry.usr, canonical_return_type="Arkham::ValueOrError<QJsonObject>"
        )
        counts = Counter({entry.key(): entry.expected_count})
        self.assertEqual(ceh.classify(finding, counts), "allowed")

    def test_allowlisted_entry_is_allowed_without_counts_argument(self) -> None:
        # classify() must stay usable for a single ad-hoc Finding with no
        # surrounding dataset (counts=None) -- membership alone is enough
        # when no occurrence-count context is supplied.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file, usr=entry.usr, canonical_return_type="Arkham::ValueOrError<QJsonObject>"
        )
        self.assertEqual(ceh.classify(finding), "allowed")

    def test_allowlisted_entry_with_too_few_occurrences_is_a_violation(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file, usr=entry.usr, canonical_return_type="Arkham::ValueOrError<QJsonObject>"
        )
        counts = Counter({entry.key(): 0})
        self.assertEqual(ceh.classify(finding, counts), "violation")

    def test_allowlisted_entry_with_too_many_occurrences_is_a_violation(self) -> None:
        # This is the "a third identical encoder is unobserved" evasion a
        # review round demonstrated: membership alone is not enough, the
        # EXACT expected occurrence count must match too.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file, usr=entry.usr, canonical_return_type="Arkham::ValueOrError<QJsonObject>"
        )
        counts = Counter({entry.key(): entry.expected_count + 1})
        self.assertEqual(ceh.classify(finding, counts), "violation")

    def test_qjson_return_type_not_in_allowlist_is_a_violation(self) -> None:
        finding = _finding(
            file="src/domain/Decks.h",
            usr="c:@N@Arkham@S@DeckOperationError@F@sneaky#1",
            canonical_return_type="QJsonObject",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_qjson_family_matches_object_array_and_value(self) -> None:
        for family_type in ("QJsonObject", "QJsonArray", "QJsonValue"):
            with self.subTest(family_type=family_type):
                finding = _finding(
                    file="src/domain/Decks.h",
                    usr="c:@N@Arkham@S@X@F@y#1",
                    canonical_return_type=family_type,
                )
                self.assertEqual(ceh.classify(finding), "violation")

    def test_wrapped_qjson_family_in_template_argument_is_recognized(self) -> None:
        # Mirrors the real ValueOrError<QJsonObject>-style canonical
        # spellings libclang reports for the legitimate exact adapters --
        # substring containment must still catch these when NOT allowlisted.
        finding = _finding(
            file="src/domain/Decks.h",
            usr="c:@N@Arkham@S@X@F@y#1",
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_reference_and_pointer_qualified_qjson_returns_are_recognized(self) -> None:
        for spelling in ("QJsonObject &", "const QJsonObject &", "QJsonObject *"):
            with self.subTest(spelling=spelling):
                finding = _finding(
                    file="src/domain/Decks.h", usr="c:@N@Arkham@S@X@F@y#1", canonical_return_type=spelling
                )
                self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_usr_in_an_unexpected_file_is_still_a_violation(self) -> None:
        # The core defense against a "duplicate identical class/method
        # signature" evasion: an identically-qualified clone declared in a
        # DIFFERENT file than the one legitimate adapter/helper it is
        # named after must not silently match by USR alone. (file, usr)
        # must BOTH match.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file="src/domain/SomeUnexpectedNewHeader.h",
            usr=entry.usr,
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_file_with_an_unexpected_usr_is_still_a_violation(self) -> None:
        # Symmetric case: right file, but a different (e.g. renamed/
        # overloaded) symbol -- must not match by file alone either.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr="c:@N@Arkham@N@Json@S@Value@F@toExactQJsonButDifferentOverload#I#1",
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_same_basename_different_full_path_does_not_collide(self) -> None:
        # Direct regression coverage for the reviewer-identified dedup
        # bug: a Finding.file that is only a basename would let a
        # DIFFERENT file sharing that basename (e.g. a same-named header
        # cloned into an unrelated directory) incorrectly match an
        # allowlist entry. Finding.file/AllowlistEntry.file are now full,
        # repo-root-relative paths specifically to prevent this.
        entry = next(e for e in ceh.ALLOWLIST if "/" in e.file)
        basename = entry.file.rsplit("/", 1)[-1]
        cloned_path = f"tests/probes/{basename}"
        self.assertNotEqual(cloned_path, entry.file)
        finding = _finding(
            file=cloned_path,
            usr=entry.usr,
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")


class AllowlistEntryTests(unittest.TestCase):
    def test_key_is_file_and_usr_pair(self) -> None:
        entry = ceh.AllowlistEntry(file="src/domain/RawJson.h", usr="some-usr")
        self.assertEqual(entry.key(), ("src/domain/RawJson.h", "some-usr"))

    def test_default_expected_count_is_one(self) -> None:
        entry = ceh.AllowlistEntry(file="src/domain/RawJson.h", usr="some-usr")
        self.assertEqual(entry.expected_count, 1)

    def test_explicit_expected_count_is_preserved(self) -> None:
        entry = ceh.AllowlistEntry(file="src/domain/RawJson.h", usr="some-usr", expected_count=3)
        self.assertEqual(entry.expected_count, 3)


class AllowlistShapeTests(unittest.TestCase):
    def test_domain_allowlist_has_exactly_twelve_entries(self) -> None:
        self.assertEqual(len(ceh.DOMAIN_ALLOWLIST), 12)

    def test_foundation_allowlist_has_exactly_two_entries(self) -> None:
        self.assertEqual(len(ceh.FOUNDATION_ALLOWLIST), 2)

    def test_combined_allowlist_has_fourteen_entries(self) -> None:
        self.assertEqual(len(ceh.ALLOWLIST), 14)

    def test_allowlist_entries_are_unique(self) -> None:
        # Guards against an accidental duplicate entry silently shrinking
        # the effective coverage without anyone noticing; ALLOWLIST_BY_KEY
        # itself already asserts this at import time -- this test
        # additionally proves the real, current ALLOWLIST content passes.
        keys = [e.key() for e in ceh.ALLOWLIST]
        self.assertEqual(len(keys), len(set(keys)))

    def test_allowlist_by_key_has_one_entry_per_allowlist_member(self) -> None:
        self.assertEqual(len(ceh.ALLOWLIST_BY_KEY), len(ceh.ALLOWLIST))

    def test_domain_allowlist_only_names_domain_rawjson_and_jsondecode_headers(self) -> None:
        for entry in ceh.DOMAIN_ALLOWLIST:
            self.assertIn(entry.file, ("src/domain/RawJson.h", "src/domain/JsonDecode.h"))

    def test_foundation_allowlist_only_names_authmodels_header(self) -> None:
        for entry in ceh.FOUNDATION_ALLOWLIST:
            self.assertEqual(entry.file, "src/AuthModels.h")

    def test_domain_and_foundation_allowlists_share_no_files(self) -> None:
        # Structural proof the two audited header sets are disjoint by
        # construction, matching the physical src/domain vs. src include-
        # root separation this same change enforces at the CMake level.
        domain_files = {e.file for e in ceh.DOMAIN_ALLOWLIST}
        foundation_files = {e.file for e in ceh.FOUNDATION_ALLOWLIST}
        self.assertEqual(domain_files & foundation_files, set())


class MissingOrMiscountedAllowlistEntryDetectionTests(unittest.TestCase):
    """Exercises the same Counter-based exact-occurrence-count logic
    main() uses to fail loudly if an allowlisted symbol was renamed/
    removed/duplicated (rather than silently reporting "zero violations"
    for the wrong reason -- an allowlist that no longer matches reality
    at its exact expected count has quietly stopped constraining
    anything)."""

    def test_missing_entry_detected_when_not_observed_at_all(self) -> None:
        counts: Counter[tuple[str, str]] = Counter()
        missing = [e for e in ceh.ALLOWLIST if counts[e.key()] != e.expected_count]
        self.assertEqual(len(missing), len(ceh.ALLOWLIST))

    def test_no_missing_entries_when_all_observed_at_exact_count(self) -> None:
        counts = Counter({e.key(): e.expected_count for e in ceh.ALLOWLIST})
        missing = [e for e in ceh.ALLOWLIST if counts[e.key()] != e.expected_count]
        self.assertEqual(missing, [])

    def test_single_entry_observed_twice_instead_of_once_is_flagged(self) -> None:
        counts = Counter({e.key(): e.expected_count for e in ceh.ALLOWLIST})
        target = ceh.ALLOWLIST[0]
        counts[target.key()] += 1
        missing = [e for e in ceh.ALLOWLIST if counts[e.key()] != e.expected_count]
        self.assertEqual(missing, [target])


class RepresentativeCompileArgsTests(unittest.TestCase):
    def test_selects_and_sanitizes_first_source_entry(self) -> None:
        compile_commands = [
            {
                "file": "/repo/src/domain/RawJson.cpp",
                "command": "/usr/bin/clang++ -std=c++23 -Isrc/domain -o x.o -c /repo/src/domain/RawJson.cpp",
            },
            {
                "file": "/repo/src/domain/Decks.cpp",
                "command": "/usr/bin/clang++ -std=c++23 -Isrc/domain -DOTHER -o y.o -c /repo/src/domain/Decks.cpp",
            },
        ]
        sources = [Path("/repo/src/domain/RawJson.cpp")]
        result = ceh._representative_compile_args(compile_commands, sources, "domain")
        self.assertEqual(result, ["-std=c++23", "-Isrc/domain"])

    def test_raises_when_no_sources_given(self) -> None:
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._representative_compile_args([], [], "domain")

    def test_raises_when_representative_source_missing_from_compile_commands(self) -> None:
        compile_commands = [
            {"file": "/repo/src/domain/Decks.cpp", "command": "clang++ -DFOO /repo/src/domain/Decks.cpp"},
        ]
        sources = [Path("/repo/src/domain/RawJson.cpp")]
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._representative_compile_args(compile_commands, sources, "domain")


class SanitizeCompileArgsTests(unittest.TestCase):
    def test_drops_compiler_executable_and_output_and_source(self) -> None:
        command = "/usr/bin/clang++ -std=c++23 -Isrc -o CMakeFiles/x.o -c src/domain/RawJson.cpp"
        result = ceh._sanitize_compile_args(command, "src/domain/RawJson.cpp")
        self.assertEqual(result, ["-std=c++23", "-Isrc"])

    def test_drops_arch_and_debug_flags(self) -> None:
        command = "clang++ -arch arm64 -g -DFOO=1 src/domain/Decks.cpp"
        result = ceh._sanitize_compile_args(command, "src/domain/Decks.cpp")
        self.assertEqual(result, ["-DFOO=1"])

    def test_preserves_order_and_values_of_remaining_flags(self) -> None:
        command = "clang++ -DA -DB -Ifoo -Ibar src/domain/Games.cpp"
        result = ceh._sanitize_compile_args(command, "src/domain/Games.cpp")
        self.assertEqual(result, ["-DA", "-DB", "-Ifoo", "-Ibar"])

    def test_source_file_not_trailing_is_left_untouched(self) -> None:
        # Defensive: if a compile_commands.json entry's command string
        # does not end with the exact source file (e.g. a quoting
        # difference), this must not accidentally eat an unrelated
        # trailing flag.
        command = "clang++ -DFOO src/domain/Games.cpp -Wall"
        result = ceh._sanitize_compile_args(command, "src/domain/Games.cpp")
        self.assertEqual(result, ["-DFOO", "src/domain/Games.cpp", "-Wall"])


class FindingKeyTests(unittest.TestCase):
    def test_key_is_file_and_usr_pair(self) -> None:
        finding = _finding(file="src/domain/RawJson.h", usr="some-usr")
        self.assertEqual(finding.key(), ("src/domain/RawJson.h", "some-usr"))


class ClosureRootednessTests(unittest.TestCase):
    """Coverage for _validate_closure_rootedness(): every manifest entry
    must, after following any symlink, physically reside inside its
    claimed root -- the self-rootedness check that stops a same-named
    symlink (whether smuggled directly into a manifest, or captured
    automatically because a rogue second FILE_SET registered an
    already-existing foundation header onto the domain target -- see
    arkham_write_target_header_set_manifest() in
    cmake/PathManifest.cmake) from silently widening a closure to
    include a file it does not actually, physically contain.

    Uses a real scratch directory tree (regular files and an actual
    symlink -- Path.resolve() genuinely needs a real filesystem entry to
    prove symlink-following, not merely lexical path manipulation) rooted
    under this repository's own gitignored build/ directory, never the
    system temp directory."""

    def setUp(self) -> None:
        repo_root = Path(__file__).resolve().parent.parent
        scratch_parent = repo_root / "build"
        scratch_parent.mkdir(exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=str(scratch_parent))
        self.scratch = Path(self._tmp.name)
        self.expected_root = self.scratch / "src" / "domain"
        self.other_root = self.scratch / "src"
        self.expected_root.mkdir(parents=True)
        (self.other_root / "AuthModels.h").write_text("// foundation-only\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_entries_physically_inside_expected_root_are_accepted(self) -> None:
        real_header = self.expected_root / "RawJson.h"
        real_header.write_text("// domain header\n", encoding="utf-8")
        resolved = ceh._validate_closure_rootedness([real_header], self.expected_root, "Domain")
        self.assertEqual(resolved, frozenset({real_header.resolve()}))

    def test_entry_outside_expected_root_is_rejected(self) -> None:
        outside_header = self.other_root / "AuthModels.h"
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._validate_closure_rootedness([outside_header], self.expected_root, "Domain")

    def test_symlink_whose_real_target_escapes_expected_root_is_rejected(self) -> None:
        # Reproduces the exact reviewer-reported attack: a same-named
        # entry that lives lexically inside src/domain/ but is really a
        # symlink pointing at a foundation-only file elsewhere.
        alias = self.expected_root / "SneakyAlias.h"
        alias.symlink_to(self.other_root / "AuthModels.h")
        with self.assertRaises(ceh.EncoderHygieneError) as ctx:
            ceh._validate_closure_rootedness([alias], self.expected_root, "Domain")
        self.assertIn(str(self.other_root / "AuthModels.h"), str(ctx.exception))

    def test_symlink_whose_real_target_stays_inside_expected_root_is_accepted(self) -> None:
        real_header = self.expected_root / "RawJson.h"
        real_header.write_text("// domain header\n", encoding="utf-8")
        alias = self.expected_root / "AliasedHeader.h"
        alias.symlink_to(real_header)
        resolved = ceh._validate_closure_rootedness([alias], self.expected_root, "Domain")
        self.assertEqual(resolved, frozenset({real_header.resolve()}))


class RealLibclangBasenameTests(unittest.TestCase):
    """Regression coverage for the CI failure where the Ubuntu
    libclang-dev glob matched libclang-cpp.so (Clang's internal,
    unstable C++ API library -- loads fine via ctypes.CDLL but is
    missing every clang_* C ABI symbol this script calls) instead of the
    real libclang.so/libclang-<N>.so."""

    def test_accepts_unversioned_and_dev_symlink_names(self) -> None:
        for name in ("libclang.so", "libclang.so.1", "libclang-18.so", "libclang-18.so.1"):
            with self.subTest(name=name):
                self.assertTrue(ceh._is_real_libclang_basename(name))

    def test_rejects_libclang_cpp_variants(self) -> None:
        for name in ("libclang-cpp.so", "libclang-cpp.so.18", "libclang-cpp.so.18.1"):
            with self.subTest(name=name):
                self.assertFalse(ceh._is_real_libclang_basename(name))

    def test_filter_drops_only_cpp_variant_from_a_mixed_glob_result(self) -> None:
        paths = [
            "/usr/lib/llvm-18/lib/libclang-cpp.so.18.1",
            "/usr/lib/llvm-18/lib/libclang.so.1",
            "/usr/lib/llvm-18/lib/libclang-18.so",
        ]
        self.assertEqual(
            ceh._real_libclang_only(paths),
            [
                "/usr/lib/llvm-18/lib/libclang.so.1",
                "/usr/lib/llvm-18/lib/libclang-18.so",
            ],
        )


if __name__ == "__main__":
    unittest.main()
