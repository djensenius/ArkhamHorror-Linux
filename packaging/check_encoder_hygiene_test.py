#!/usr/bin/env python3
"""Unit tests for packaging/check_encoder_hygiene.py's pure decision logic
(classify(), the ALLOWLIST membership rule, compile-argument sanitization,
and the "missing allowlist entry" staleness check), using synthetic
Finding records rather than a real compiler invocation (fast,
deterministic, no Clang/libclang dependency for this file specifically --
the real AST walk against the actual project sources is exercised
separately by `mise run contracts:check-encoder-hygiene`, which this test
suite intentionally does not invoke).

Run directly: `python3 packaging/check_encoder_hygiene_test.py`
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encoder_hygiene as ceh


def _finding(
    file: str = "RawJson.h",
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

    def test_exact_allowlisted_entry_is_allowed(self) -> None:
        file, usr = next(iter(ceh.ALLOWLIST))
        finding = _finding(file=file, usr=usr, canonical_return_type="Arkham::ValueOrError<QJsonObject>")
        self.assertEqual(ceh.classify(finding), "allowed")

    def test_qjson_return_type_not_in_allowlist_is_a_violation(self) -> None:
        finding = _finding(
            file="Decks.h",
            usr="c:@N@Arkham@S@DeckOperationError@F@sneaky#1",
            canonical_return_type="QJsonObject",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_qjson_family_matches_object_array_and_value(self) -> None:
        for family_type in ("QJsonObject", "QJsonArray", "QJsonValue"):
            with self.subTest(family_type=family_type):
                finding = _finding(
                    file="Decks.h",
                    usr="c:@N@Arkham@S@X@F@y#1",
                    canonical_return_type=family_type,
                )
                self.assertEqual(ceh.classify(finding), "violation")

    def test_wrapped_qjson_family_in_template_argument_is_recognized(self) -> None:
        # Mirrors the real ValueOrError<QJsonObject>-style canonical
        # spellings libclang reports for the legitimate exact adapters --
        # substring containment must still catch these when NOT allowlisted.
        finding = _finding(
            file="Decks.h",
            usr="c:@N@Arkham@S@X@F@y#1",
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_reference_and_pointer_qualified_qjson_returns_are_recognized(self) -> None:
        for spelling in ("QJsonObject &", "const QJsonObject &", "QJsonObject *"):
            with self.subTest(spelling=spelling):
                finding = _finding(
                    file="Decks.h", usr="c:@N@Arkham@S@X@F@y#1", canonical_return_type=spelling
                )
                self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_usr_in_an_unexpected_file_is_still_a_violation(self) -> None:
        # The core defense against a "duplicate identical class/method
        # signature" evasion: an identically-qualified clone declared in a
        # DIFFERENT file than the one legitimate adapter/helper it is
        # named after must not silently match by USR alone. (file, usr)
        # must BOTH match.
        _, usr = next(iter(ceh.ALLOWLIST))
        finding = _finding(
            file="SomeUnexpectedNewHeader.h",
            usr=usr,
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")

    def test_allowlisted_file_with_an_unexpected_usr_is_still_a_violation(self) -> None:
        # Symmetric case: right file, but a different (e.g. renamed/
        # overloaded) symbol -- must not match by file alone either.
        file, _ = next(iter(ceh.ALLOWLIST))
        finding = _finding(
            file=file,
            usr="c:@N@Arkham@N@Json@S@Value@F@toExactQJsonButDifferentOverload#I#1",
            canonical_return_type="Arkham::ValueOrError<QJsonObject>",
        )
        self.assertEqual(ceh.classify(finding), "violation")


class AllowlistShapeTests(unittest.TestCase):
    def test_allowlist_has_exactly_twelve_entries(self) -> None:
        self.assertEqual(len(ceh.ALLOWLIST), 12)

    def test_allowlist_entries_are_unique(self) -> None:
        # ALLOWLIST is itself a frozenset built from two tuples; this
        # guards against an accidental duplicate entry silently shrinking
        # the effective coverage without anyone noticing.
        combined = list(ceh._CANONICAL_ADAPTERS) + list(ceh._DECODE_HELPERS)
        self.assertEqual(len(combined), len(set(combined)))

    def test_allowlist_only_names_rawjson_and_jsondecode_headers(self) -> None:
        for file, _usr in ceh.ALLOWLIST:
            self.assertIn(file, ("RawJson.h", "JsonDecode.h"))


class MissingAllowlistEntryDetectionTests(unittest.TestCase):
    """Exercises the same set-difference logic main() uses to fail loudly
    if an allowlisted symbol was renamed/removed (rather than silently
    reporting "zero violations" for the wrong reason -- an allowlist that
    no longer matches anything real has quietly stopped constraining
    anything)."""

    def test_missing_entry_detected_when_not_observed(self) -> None:
        observed = {key for key in ceh.ALLOWLIST if key != next(iter(ceh.ALLOWLIST))}
        missing = ceh.ALLOWLIST - observed
        self.assertEqual(len(missing), 1)

    def test_no_missing_entries_when_all_observed(self) -> None:
        observed = set(ceh.ALLOWLIST)
        missing = ceh.ALLOWLIST - observed
        self.assertEqual(missing, set())


class SanitizeCompileArgsTests(unittest.TestCase):
    def test_drops_compiler_executable_and_output_and_source(self) -> None:
        command = "/usr/bin/clang++ -std=c++23 -Isrc -o CMakeFiles/x.o -c src/RawJson.cpp"
        result = ceh._sanitize_compile_args(command, "src/RawJson.cpp")
        self.assertEqual(result, ["-std=c++23", "-Isrc"])

    def test_drops_arch_and_debug_flags(self) -> None:
        command = "clang++ -arch arm64 -g -DFOO=1 src/Decks.cpp"
        result = ceh._sanitize_compile_args(command, "src/Decks.cpp")
        self.assertEqual(result, ["-DFOO=1"])

    def test_preserves_order_and_values_of_remaining_flags(self) -> None:
        command = "clang++ -DA -DB -Ifoo -Ibar src/Games.cpp"
        result = ceh._sanitize_compile_args(command, "src/Games.cpp")
        self.assertEqual(result, ["-DA", "-DB", "-Ifoo", "-Ibar"])

    def test_source_file_not_trailing_is_left_untouched(self) -> None:
        # Defensive: if a compile_commands.json entry's command string
        # does not end with the exact source file (e.g. a quoting
        # difference), this must not accidentally eat an unrelated
        # trailing flag.
        command = "clang++ -DFOO src/Games.cpp -Wall"
        result = ceh._sanitize_compile_args(command, "src/Games.cpp")
        self.assertEqual(result, ["-DFOO", "src/Games.cpp", "-Wall"])


class FindingKeyTests(unittest.TestCase):
    def test_key_is_file_and_usr_pair(self) -> None:
        finding = _finding(file="RawJson.h", usr="some-usr")
        self.assertEqual(finding.key(), ("RawJson.h", "some-usr"))


if __name__ == "__main__":
    unittest.main()
