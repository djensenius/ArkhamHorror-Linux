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

import os
import sys
import tempfile
import unittest
from collections import Counter
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encoder_hygiene as ceh

_FIRST_ALLOWLIST_FULL_SIGNATURE = (
    "kind=21;owner=c:@N@Arkham@N@Json@S@Value;"
    "type=Arkham::ValueOrError<QJsonValue> () const;"
    "result=Arkham::ValueOrError<QJsonValue>;params=[];static=0;"
    "variadic=0;exception=0;calling_conv=1;ref_qualifier=0;"
    "forbidden=[result:Arkham::ValueOrError<QJsonValue>]"
)


def _finding(
    file: str = "src/domain/RawJson.h",
    line: int = 1,
    display_name: str = "someMethod()",
    canonical_return_type: str = "QJsonObject",
    usr: str = "c:@N@Arkham@F@someMethod#1",
    access: int = 0,
    linkage: int = 0,
) -> ceh.Finding:
    return ceh.Finding(
        file=file,
        line=line,
        display_name=display_name,
        canonical_return_type=canonical_return_type,
        usr=usr,
        access=access,
        linkage=linkage,
    )


class ClassifyTests(unittest.TestCase):
    def test_non_qjson_return_type_is_always_allowed(self) -> None:
        finding = _finding(canonical_return_type="Arkham::ValueOrError<QString>", usr="c:@N@Foo@F@bar#1")
        self.assertEqual(ceh.classify(finding), "allowed")

    def test_exact_allowlisted_entry_is_allowed_with_matching_count(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
            access=entry.access,
            linkage=entry.linkage,
        )
        counts = Counter({entry.key(): entry.expected_count})
        self.assertEqual(ceh.classify(finding, counts), "allowed")

    def test_allowlisted_entry_is_allowed_without_counts_argument(self) -> None:
        # classify() must stay usable for a single ad-hoc Finding with no
        # surrounding dataset (counts=None) -- membership alone is enough
        # when no occurrence-count context is supplied.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
            access=entry.access,
            linkage=entry.linkage,
        )
        self.assertEqual(ceh.classify(finding), "allowed")

    def test_allowlisted_usr_with_changed_semantic_signature_is_rejected(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type="result=const QJsonObject &",
            access=entry.access,
            linkage=entry.linkage,
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_usr_with_changed_access_is_rejected(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
            access=2,
            linkage=entry.linkage,
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_entry_with_too_few_occurrences_is_a_violation(self) -> None:
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
            access=entry.access,
            linkage=entry.linkage,
        )
        counts = Counter({entry.key(): 0})
        self.assertEqual(ceh.classify(finding, counts), "violation")

    def test_allowlisted_entry_with_too_many_occurrences_is_a_violation(self) -> None:
        # This is the "a third identical encoder is unobserved" evasion a
        # review round demonstrated: membership alone is not enough, the
        # EXACT expected occurrence count must match too.
        entry = ceh.ALLOWLIST[0]
        finding = _finding(
            file=entry.file,
            usr=entry.usr,
            canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
            access=entry.access,
            linkage=entry.linkage,
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


class ExactIdentitySetTests(unittest.TestCase):
    def _pinned_allowlisted_finding(self) -> ceh.Finding:
        entry = ceh.ALLOWLIST[0]
        return replace(
            _finding(
                file=entry.file,
                usr=entry.usr,
                canonical_return_type=_FIRST_ALLOWLIST_FULL_SIGNATURE,
                access=entry.access,
                linkage=entry.linkage,
            ),
            physical_identity_sha256=entry.physical_identity_sha256 or "",
            observation_set_sha256=entry.observation_set_sha256 or "",
        )

    def test_allowance_compares_exact_physical_identity(self) -> None:
        finding = self._pinned_allowlisted_finding()
        self.assertEqual(ceh.classify(finding), "allowed")
        self.assertEqual(
            ceh.classify(
                replace(finding, physical_identity_sha256="relocated")
            ),
            "violation",
        )

    def test_allowance_compares_exact_observation_set(self) -> None:
        finding = self._pinned_allowlisted_finding()
        self.assertEqual(
            ceh.classify(
                replace(finding, observation_set_sha256="extra-target-config-tu")
            ),
            "violation",
        )

    def test_physical_relocation_changes_exact_allowance_digest(self) -> None:
        finding = replace(
            _finding(canonical_return_type="QJsonObject"),
            physical_identity_sha256="physical-a",
            observation_set_sha256="observations-a",
        )
        relocated = replace(finding, physical_identity_sha256="physical-b")
        self.assertNotEqual(
            ceh._identity_set_digest([finding]),
            ceh._identity_set_digest([relocated]),
        )

    def test_extra_target_config_or_tu_observation_changes_digest(self) -> None:
        finding = replace(
            _finding(canonical_return_type="QJsonObject"),
            physical_identity_sha256="physical",
            observation_set_sha256="target|Debug|source:Wire.cpp",
        )
        additionally_observed = replace(
            finding,
            observation_set_sha256=(
                "target|Debug|source:Wire.cpp;"
                "other|Release|source:Wire.cpp"
            ),
        )
        self.assertNotEqual(
            ceh._identity_set_digest([finding]),
            ceh._identity_set_digest([additionally_observed]),
        )


class QJsonFamilyWrappedFormsAreDetectedTests(unittest.TestCase):
    """Coverage for _is_qjson_family()'s handling of QJsonDocument (a
    review-round addition to _QJSON_FAMILY) and of every
    const/reference/pointer-qualified and standard-library
    wrapper/callable form checked empirically against a real libclang
    probe before that fix was written -- see the doc comment above
    _QVARIANT_JSON_CONTAINER_CANONICAL_FORMS in check_encoder_hygiene.py
    for the full narrative of what was and was not already covered by
    the pre-existing substring match."""

    def test_bare_qjsondocument_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("QJsonDocument"))

    def test_qualified_and_pointer_qjsondocument_forms_are_prohibited(self) -> None:
        for spelling in ("const QJsonDocument &", "QJsonDocument *", "QJsonDocument &&"):
            with self.subTest(spelling=spelling):
                self.assertTrue(ceh._is_qjson_family(spelling))

    def test_qualified_and_pointer_qjsonobject_forms_remain_prohibited(self) -> None:
        for spelling in ("QJsonObject", "const QJsonObject &", "QJsonObject *"):
            with self.subTest(spelling=spelling):
                self.assertTrue(ceh._is_qjson_family(spelling))

    def test_standard_library_wrapper_and_callable_forms_are_prohibited(self) -> None:
        for spelling in (
            "std::optional<QJsonObject>",
            "std::shared_ptr<QJsonObject>",
            "std::unique_ptr<QJsonObject>",
            "std::function<QJsonObject ()>",
        ):
            with self.subTest(spelling=spelling):
                self.assertTrue(ceh._is_qjson_family(spelling))

    def test_unrelated_return_type_is_not_prohibited(self) -> None:
        self.assertFalse(ceh._is_qjson_family("Arkham::ValueOrError<QString>"))


class QVariantJsonContainerTests(unittest.TestCase):
    """Coverage for _is_qvariant_json_container() and its wiring into
    _is_qjson_family(): QVariantMap/QVariantList/QVariantHash are Qt
    typedefs whose canonical (post-typedef-resolution) spelling is their
    underlying template instantiation, never the typedef name itself --
    see the doc comment above _QVARIANT_JSON_CONTAINER_CANONICAL_FORMS
    for why a naive literal "QVariantMap" substring would silently match
    nothing."""

    def test_canonical_qvariantmap_spelling_with_space_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("QMap<QString, QVariant>"))

    def test_canonical_qvariantmap_spelling_without_space_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("QMap<QString,QVariant>"))

    def test_canonical_qvariantlist_spelling_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("QList<QVariant>"))

    def test_canonical_qvarianthash_spelling_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("QHash<QString, QVariant>"))

    def test_qualified_qvariantmap_form_is_prohibited(self) -> None:
        self.assertTrue(ceh._is_qjson_family("const QMap<QString, QVariant> &"))

    def test_bare_qvariant_is_not_prohibited(self) -> None:
        # A bare QVariant's static type carries no information about
        # whether it happens to hold JSON-shaped content at runtime --
        # deliberately not flagged (see the doc comment in
        # check_encoder_hygiene.py just above _QVARIANT_JSON_CONTAINER_CANONICAL_FORMS).
        self.assertFalse(ceh._is_qjson_family("QVariant"))

    def test_unrelated_qt_container_is_not_prohibited(self) -> None:
        self.assertFalse(ceh._is_qjson_family("QMap<QString, int>"))


class ExternalRootsTests(unittest.TestCase):
    """Coverage for _external_roots(): the small, explicit set of
    subtrees exempted from the domain/foundation dependency-direction
    closure check -- sourced from a REAL, CMake-generated
    `<clang-build-dir>/generated/external_roots.txt` manifest (see
    CMakeLists.txt's FetchContent-derived writer), never a hand-authored/
    hardcoded `_deps` guess -- replacing the earlier, unsound blanket
    "anything under the build directory" exemption a review round
    demonstrated let a genuinely project-generated header/fragment
    placed anywhere else under the build directory evade the audit (see
    _audit_inclusion_graph()'s own doc comment).

    Uses a real scratch directory tree (rooted under this repository's
    own gitignored build/ directory, never the system temp directory)
    with an actual `generated/external_roots.txt` file written directly,
    exactly as CMake itself would -- proving _external_roots() reads
    real manifest content rather than deriving anything lexically from
    the build directory path itself."""

    def setUp(self) -> None:
        repo_root = Path(__file__).resolve().parent.parent
        scratch_parent = repo_root / "build"
        scratch_parent.mkdir(exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=str(scratch_parent))
        self.build_dir = Path(self._tmp.name)
        self.generated_dir = self.build_dir / "generated"
        self.generated_dir.mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_manifest(self, *roots: Path) -> None:
        (self.generated_dir / "external_roots.txt").write_text(
            "".join(f"{root}\n" for root in roots), encoding="utf-8"
        )

    def test_returns_exactly_the_manifested_roots(self) -> None:
        deps = self.build_dir / "_deps"
        autogen = self.build_dir / "arkham_foundation_autogen"
        self._write_manifest(deps, autogen)
        roots = ceh._external_roots(self.build_dir)
        self.assertEqual(roots, frozenset({deps.resolve(), autogen.resolve()}))

    def test_does_not_exempt_the_build_dir_itself(self) -> None:
        self._write_manifest(self.build_dir / "_deps")
        roots = ceh._external_roots(self.build_dir)
        self.assertNotIn(self.build_dir.resolve(), roots)

    def test_does_not_exempt_a_generated_subdirectory_never_manifested(self) -> None:
        # Direct regression coverage for the reviewer-reported bypass: a
        # genuinely project-generated header/fragment placed at
        # <build-dir>/generated/Lossy.inc must NOT be exempt merely for
        # residing somewhere under the build directory -- only entries
        # this manifest actually lists are ever exempt.
        self._write_manifest(self.build_dir / "_deps")
        roots = ceh._external_roots(self.build_dir)
        generated = (self.generated_dir / "Lossy.inc").resolve()
        self.assertFalse(any(generated.is_relative_to(root) for root in roots))

    def test_exempts_a_real_path_nested_under_a_manifested_root(self) -> None:
        self._write_manifest(self.build_dir / "_deps")
        roots = ceh._external_roots(self.build_dir)
        nested = (self.build_dir / "_deps" / "qtkeychain-src" / "keychain.h").resolve()
        self.assertTrue(any(nested.is_relative_to(root) for root in roots))

    def test_missing_manifest_is_a_hard_failure_never_a_silent_empty_result(self) -> None:
        # No generated/external_roots.txt written at all in this scratch
        # build_dir -- must raise, never silently return an empty
        # frozenset (which would be indistinguishable from "this build
        # genuinely has zero external roots" and could mask a
        # misconfigured/unconfigured build directory).
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._external_roots(self.build_dir)


class CompileContextsForSourceTests(unittest.TestCase):
    def _entry(
        self,
        source: str,
        target: str,
        *,
        define: str = "",
        output_suffix: str = "",
    ) -> dict:
        return {
            "directory": "/repo/build",
            "file": source,
            "command": (
                f"clang++ -std=c++23 {define} -c {source} "
                f"-o CMakeFiles/{target}.dir/{output_suffix}source.o"
            ),
            "output": f"CMakeFiles/{target}.dir/{output_suffix}source.o",
        }

    def test_returns_every_target_context_for_same_physical_source(self) -> None:
        compile_commands = [
            self._entry("/repo/src/domain/Decks.cpp", "domain", define="-DDOMAIN"),
            self._entry("/repo/src/domain/Decks.cpp", "foundation", define="-DFOUNDATION"),
            self._entry(
                "/repo/src/domain/Decks.cpp",
                "consumer",
                define="-DCONSUMER",
                output_suffix="Debug/",
            ),
            self._entry(
                "/repo/src/domain/Decks.cpp",
                "consumer",
                define="-DCONSUMER_RELEASE",
                output_suffix="Release/",
            ),
        ]
        contexts = ceh._compile_contexts_for_source(
            compile_commands, Path("/repo/src/domain/Decks.cpp")
        )
        self.assertEqual(
            [context.target for context in contexts],
            ["domain", "foundation", "consumer", "consumer"],
        )
        self.assertEqual(
            [context.configuration for context in contexts[-2:]],
            ["Debug", "Release"],
        )
        self.assertIn("-DCONSUMER_RELEASE", contexts[-1].arguments)

    def test_raises_when_no_matching_entry_exists(self) -> None:
        compile_commands = [
            self._entry("/repo/src/domain/RawJson.cpp", "domain"),
        ]
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._compile_contexts_for_source(
                compile_commands, Path("/repo/src/domain/Decks.cpp")
            )

    def test_rejects_entry_without_object_target_identity(self) -> None:
        entry = self._entry("/repo/src/domain/Decks.cpp", "domain")
        entry["output"] = "/repo/build/unowned.o"
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._compile_contexts_for_source(
                [entry], Path("/repo/src/domain/Decks.cpp")
            )

    def test_honors_arguments_array_without_shell_reparsing(self) -> None:
        source = "/repo/src/domain/Decks.cpp"
        entry = self._entry(source, "domain")
        del entry["command"]
        entry["arguments"] = ["clang++", "-DVALUE=a b", "-c", source]
        contexts = ceh._compile_contexts_for_source([entry], Path(source))
        self.assertIn("-DVALUE=a b", contexts[0].arguments)

    def test_rejects_duplicate_object_output(self) -> None:
        entry = self._entry("/repo/src/domain/Decks.cpp", "domain")
        duplicate = dict(entry)
        duplicate["command"] = duplicate["command"].replace("-std=c++23", "-std=c++23 -DOTHER")
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._compile_contexts_for_source(
                [entry, duplicate], Path("/repo/src/domain/Decks.cpp")
            )


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
    def test_domain_allowlist_has_exactly_seventy_five_entries(self) -> None:
        self.assertEqual(len(ceh.DOMAIN_ALLOWLIST), 75)

    def test_foundation_allowlist_has_exactly_thirteen_entries(self) -> None:
        self.assertEqual(len(ceh.FOUNDATION_ALLOWLIST), 13)

    def test_combined_allowlist_has_eighty_eight_entries(self) -> None:
        self.assertEqual(len(ceh.ALLOWLIST), 88)

    def test_allowlist_entries_are_unique(self) -> None:
        # Guards against an accidental duplicate entry silently shrinking
        # the effective coverage without anyone noticing; ALLOWLIST_BY_KEY
        # itself already asserts this at import time -- this test
        # additionally proves the real, current ALLOWLIST content passes.
        keys = [e.key() for e in ceh.ALLOWLIST]
        self.assertEqual(len(keys), len(set(keys)))

    def test_allowlist_by_key_has_one_entry_per_allowlist_member(self) -> None:
        self.assertEqual(len(ceh.ALLOWLIST_BY_KEY), len(ceh.ALLOWLIST))

    def test_domain_allowlist_only_names_domain_sources(self) -> None:
        for entry in ceh.DOMAIN_ALLOWLIST:
            self.assertTrue(entry.file.startswith("src/domain/"))

    def test_foundation_allowlist_names_only_exact_foundation_decoder_files(self) -> None:
        for entry in ceh.FOUNDATION_ALLOWLIST:
            self.assertTrue(
                entry.file.startswith("src/")
                and not entry.file.startswith("src/domain/")
            )

    def test_every_allowlist_entry_pins_full_declaration_identity(self) -> None:
        for entry in ceh.ALLOWLIST:
            self.assertRegex(entry.full_signature_sha256 or "", r"^[0-9a-f]{64}$")
            self.assertRegex(
                entry.physical_identity_sha256 or "", r"^[0-9a-f]{64}$"
            )
            self.assertRegex(
                entry.observation_set_sha256 or "", r"^[0-9a-f]{64}$"
            )
            self.assertIsNotNone(entry.access)
            self.assertIsNotNone(entry.linkage)

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


class HeaderCompileContextsTests(unittest.TestCase):
    def test_preserves_distinct_target_contexts(self) -> None:
        compile_commands = [
            {
                "directory": "/repo/build",
                "file": "/repo/src/domain/RawJson.cpp",
                "command": "/usr/bin/clang++ -std=c++23 -Isrc/domain -o x.o -c /repo/src/domain/RawJson.cpp",
                "output": "CMakeFiles/domain.dir/x.o",
            },
            {
                "directory": "/repo/build",
                "file": "/repo/src/domain/Decks.cpp",
                "command": "/usr/bin/clang++ -std=c++23 -Isrc/domain -DOTHER -o y.o -c /repo/src/domain/Decks.cpp",
                "output": "CMakeFiles/consumer.dir/y.o",
            },
        ]
        sources = [
            Path("/repo/src/domain/RawJson.cpp"),
            Path("/repo/src/domain/Decks.cpp"),
        ]
        result = ceh._header_compile_contexts(
            compile_commands, sources, "domain"
        )
        self.assertEqual([context.target for context in result], ["domain", "consumer"])

    def test_raises_when_no_sources_given(self) -> None:
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._header_compile_contexts([], [], "domain")

    def test_raises_when_representative_source_missing_from_compile_commands(self) -> None:
        compile_commands = [
            {
                "directory": "/repo/build",
                "file": "/repo/src/domain/Decks.cpp",
                "command": "clang++ -DFOO /repo/src/domain/Decks.cpp",
                "output": "CMakeFiles/domain.dir/Decks.o",
            },
        ]
        sources = [Path("/repo/src/domain/RawJson.cpp")]
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._header_compile_contexts(compile_commands, sources, "domain")


class ProductionConfigurationMatrixTests(unittest.TestCase):
    def test_debug_release_and_relwithdebinfo_are_mandatory(self) -> None:
        self.assertEqual(
            ceh.SUPPORTED_PRODUCTION_CONFIGS,
            ("Debug", "Release", "RelWithDebInfo"),
        )
        self.assertIn("Release", ceh.SUPPORTED_PRODUCTION_CONFIGS)

    def test_standard_library_abi_namespaces_normalize_in_usrs(self) -> None:
        libcxx = "c:@N@std@N@__1@S@function"
        libstdcxx = "c:@N@std@N@__cxx11@S@function"
        expected = "c:@N@std@S@function"
        self.assertEqual(ceh._stable_usr(libcxx), expected)
        self.assertEqual(ceh._stable_usr(libstdcxx), expected)


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

    def test_source_file_is_removed_even_when_not_trailing(self) -> None:
        command = "clang++ -DFOO src/domain/Games.cpp -Wall"
        result = ceh._sanitize_compile_args(command, "src/domain/Games.cpp")
        self.assertEqual(result, ["-DFOO", "-Wall"])


class FindingKeyTests(unittest.TestCase):
    def test_key_is_file_and_usr_pair(self) -> None:
        finding = _finding(file="src/domain/RawJson.h", usr="some-usr")
        self.assertEqual(finding.key(), ("src/domain/RawJson.h", "some-usr"))


class CanonicalSourceManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        repo = Path(__file__).resolve().parent.parent
        scratch = repo / "build"
        scratch.mkdir(exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=str(scratch))
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_public_duplicate_is_expected_to_arrive_deduplicated(self) -> None:
        source = self.root / "Public.cpp"
        source.write_text("int value = 0;\n", encoding="utf-8")
        self.assertEqual(
            ceh._canonical_source_manifest([source], "Probe"),
            [source.resolve()],
        )

    def test_symlink_alias_collision_fails_closed(self) -> None:
        source = self.root / "Source.cpp"
        alias = self.root / "Alias.cpp"
        source.write_text("int value = 0;\n", encoding="utf-8")
        alias.symlink_to(source)
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._canonical_source_manifest([source, alias], "Probe")

    def test_hardlink_collision_fails_closed(self) -> None:
        source = self.root / "Source.cpp"
        alias = self.root / "Alias.cpp"
        source.write_text("int value = 0;\n", encoding="utf-8")
        os.link(source, alias)
        with self.assertRaises(ceh.EncoderHygieneError):
            ceh._canonical_source_manifest([source, alias], "Probe")


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
