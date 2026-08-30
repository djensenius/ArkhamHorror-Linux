#!/usr/bin/env python3
"""Unit tests for packaging/verify_contract_provenance.py's closure
discovery and verify() logic, using an in-memory FakeTree instead of a
real network fetch (fast, deterministic, no external dependency) plus a
scratch local "working tree" written under build/ (never /tmp) so file
existence/symlink/mode checks are exercised against real filesystem
objects rather than mocks.

Run directly: `python3 packaging/verify_contract_provenance_test.py`
"""

from __future__ import annotations

import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import verify_contract_provenance as vcp


class FakeTree(vcp.GitTree):
    """An in-memory GitTree: a fixed {path: bytes} table plus a mode/type
    override table, standing in for a real pinned backend commit."""

    def __init__(self, blobs: dict[str, bytes], modes: dict[str, tuple[str, str]] | None = None) -> None:
        self._blobs = blobs
        self._modes = modes or {}

    def blob_bytes(self, path: str) -> bytes:
        return self._blobs[path]

    def ls_tree(self, path: str):
        if path not in self._blobs:
            return None
        return self._modes.get(path, ("100644", "blob"))


CATALOG_SCHEMA = b'{"$defs": {"cardCost": {"$ref": "#/$defs/nested"}}}'
DECKS_SCHEMA = b'{"$defs": {"deckList": {}}}'
MANIFEST = (
    b'{"fixtures": ['
    b'{"path": "contracts/fixtures/catalog.json", "schema": "contracts/schemas/catalog.schema.json"},'
    b'{"path": "contracts/fixtures/decks.json", "schema": "contracts/schemas/decks.schema.json"},'
    b'{"path": "contracts/fixtures/account.json", "schema": "contracts/schemas/account.schema.json"}'
    b']}'
)
CAPABILITIES = b'{"schemaRevision": "0.1.12"}'
CATALOG_FIXTURE = b'{"cards": []}'
DECKS_FIXTURE = b'{"decks": []}'


def _baseline_blobs() -> dict[str, bytes]:
    return {
        "contracts/schemas/catalog.schema.json": CATALOG_SCHEMA,
        "contracts/schemas/decks.schema.json": DECKS_SCHEMA,
        "contracts/schemas/game-lifecycle.schema.json": b"{}",
        "contracts/schemas/game-list.schema.json": b"{}",
        "contracts/schemas/game-state.schema.json": b"{}",
        "contracts/manifest.json": MANIFEST,
        "contracts/fixtures/capabilities.json": CAPABILITIES,
        "contracts/fixtures/catalog.json": CATALOG_FIXTURE,
        "contracts/fixtures/decks.json": DECKS_FIXTURE,
    }


class ClosureTests(unittest.TestCase):
    def test_governed_paths_includes_roots_and_manifest_derived_fixtures(self):
        tree = FakeTree(_baseline_blobs())
        governed = vcp.compute_governed_paths(tree)
        self.assertIn("contracts/schemas/catalog.schema.json", governed)
        self.assertIn("contracts/manifest.json", governed)
        self.assertIn("contracts/fixtures/capabilities.json", governed)
        self.assertIn("contracts/fixtures/catalog.json", governed)
        self.assertIn("contracts/fixtures/decks.json", governed)
        # account.json's schema (account.schema.json) is not in the
        # ROOT_SCHEMAS closure, so it must NOT be pulled in.
        self.assertNotIn("contracts/fixtures/account.json", governed)

    def test_omitted_referenced_schema_raises(self):
        # catalog.schema.json's $ref points at a schema that is never
        # actually present in the backend tree -- this must be a hard
        # failure, not a silently-skipped dependency.
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = (
            b'{"$defs": {"x": {"$ref": "missing.schema.json#/$defs/y"}}}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_invalid_utf8_schema_bytes_rejected(self):
        # A schema containing a byte sequence that is not valid UTF-8 must
        # be a hard failure (RuntimeError), never silently decoded with a
        # replacement character (which could mask corrupted bytes and
        # compute an incorrect, or merely coincidentally-still-correct,
        # $ref closure).
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = b'{"$defs": {"x": \xff\xfe}}'
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_ref_path_traversal_escape_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = (
            b'{"$defs": {"x": {"$ref": "../../etc/passwd#/$defs/y"}}}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(vcp.RefEscapeError):
            vcp.compute_governed_paths(tree)

    def test_ref_absolute_path_escape_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = (
            b'{"$defs": {"x": {"$ref": "/etc/passwd#/$defs/y"}}}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(vcp.RefEscapeError):
            vcp.compute_governed_paths(tree)

    def test_same_document_fragment_ref_is_not_a_file_dependency(self):
        tree = FakeTree(_baseline_blobs())
        # CATALOG_SCHEMA's only $ref is "#/$defs/nested" (same document);
        # closure must not try to fetch a file for it.
        closure = vcp.compute_schema_closure(tree, ["contracts/schemas/catalog.schema.json"])
        self.assertEqual(list(closure), ["contracts/schemas/catalog.schema.json"])

    def test_dot_slash_prefixed_cross_file_ref_resolves(self):
        # The real backend's contracts/schemas/game-list.schema.json
        # contains a genuine cross-file ref written as
        # "$ref": "./game-state.schema.json" (a leading "./" segment, not
        # just a bare filename). The closure walk must resolve this to
        # "contracts/schemas/game-state.schema.json" and actually fetch
        # it, not silently drop the dependency or look up a literal
        # "contracts/schemas/./game-state.schema.json" path that does not
        # exist in the backend tree.
        blobs = _baseline_blobs()
        blobs["contracts/schemas/game-list.schema.json"] = (
            b'{"$defs": {"row": {"$ref": "./game-state.schema.json"}}}'
        )
        tree = FakeTree(blobs)
        closure = vcp.compute_schema_closure(
            tree, ["contracts/schemas/game-list.schema.json"]
        )
        self.assertIn("contracts/schemas/game-state.schema.json", closure)
        self.assertEqual(
            closure["contracts/schemas/game-state.schema.json"],
            blobs["contracts/schemas/game-state.schema.json"],
        )

    def test_ref_substring_in_prose_is_not_a_dependency(self):
        # A schema author writing prose that happens to contain the four
        # characters "$ref" (e.g. documenting $ref syntax itself in a
        # description/examples string) must NOT be treated as a real
        # dependency: only an actual JSON object key literally named
        # "$ref" may name one. A naive text-level regex scan for the
        # substring '"$ref": "..."' would false-positive here and try to
        # fetch a schema that was never really referenced.
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = (
            b'{"description": "See the \\"$ref\\": \\"missing.schema.json\\" '
            b'syntax for details.", "$defs": {"x": {}}}'
        )
        tree = FakeTree(blobs)
        closure = vcp.compute_schema_closure(tree, ["contracts/schemas/catalog.schema.json"])
        self.assertEqual(list(closure), ["contracts/schemas/catalog.schema.json"])

    def test_malformed_json_schema_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/schemas/catalog.schema.json"] = b'{"$defs": {'
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_invalid_utf8_manifest_bytes_rejected(self):
        # contracts/manifest.json itself must be decoded just as strictly
        # as any schema: a corrupted/non-UTF-8 manifest blob at the
        # pinned backend commit is a hard failure (RuntimeError), never a
        # silently-replaced-character decode that could mask corruption
        # and compute an incorrect governed-fixture set.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b'{"fixtures": [\xff\xfe]}'
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_malformed_json_manifest_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b'{"fixtures": ['
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_top_level_not_object_rejected(self):
        # A syntactically-valid-JSON manifest whose top level is a bare
        # array (not an object) must still be a clean RuntimeError, never
        # an unhandled AttributeError from calling .get() on a list.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b"[]"
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_missing_fixtures_key_rejected(self):
        # A manifest object entirely lacking the "fixtures" key must be a
        # hard RuntimeError, distinct from a present-but-empty fixtures
        # list: silently defaulting a missing key to [] would let a
        # regressed/malformed backend manifest skip verifying every
        # fixture this client's modeled schemas govern without any
        # signal, defeating this script's fixture-provenance guarantee.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b"{}"
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_present_but_empty_fixtures_list_accepted(self):
        # By contrast, a present-and-empty "fixtures" list is a valid,
        # distinct state (a backend manifest that genuinely governs no
        # fixtures yet) and must not raise.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b'{"fixtures": []}'
        tree = FakeTree(blobs)
        governed = vcp.compute_governed_paths(tree)
        self.assertNotIn("contracts/fixtures/catalog.json", governed)

    def test_manifest_fixtures_not_a_list_rejected(self):
        # "fixtures" present but bound to an object, not a list, must be a
        # clean RuntimeError, never an unhandled exception from iterating
        # over dict keys as if they were {schema, path} entries.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b'{"fixtures": {"not": "a list"}}'
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_fixture_entry_not_an_object_rejected(self):
        # A fixtures[] entry that is a bare string (not an object) must be
        # a clean RuntimeError, never an unhandled TypeError from
        # subscripting a str with ["schema"].
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = b'{"fixtures": ["not-an-object"]}'
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_fixture_entry_missing_schema_key_rejected(self):
        # A fixtures[] entry missing the required "schema" key must be a
        # clean RuntimeError, never an unhandled KeyError escaping
        # verify()'s narrow exception handling as a raw traceback.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = (
            b'{"fixtures": [{"path": "contracts/fixtures/catalog.json"}]}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_fixture_entry_missing_path_key_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = (
            b'{"fixtures": [{"schema": '
            b'"contracts/schemas/catalog.schema.json"}]}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_fixture_entry_non_string_schema_rejected(self):
        # A fixtures[] entry whose "schema" value is not a string (e.g. an
        # integer) must be a clean RuntimeError, never an unhandled
        # exception from passing a non-str to _resolve_manifest_path's
        # str.startswith()/Path() calls.
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = (
            b'{"fixtures": [{"schema": 1, '
            b'"path": "contracts/fixtures/catalog.json"}]}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)

    def test_manifest_fixture_entry_non_string_path_rejected(self):
        blobs = _baseline_blobs()
        blobs["contracts/manifest.json"] = (
            b'{"fixtures": [{"schema": '
            b'"contracts/schemas/catalog.schema.json", "path": null}]}'
        )
        tree = FakeTree(blobs)
        with self.assertRaises(RuntimeError):
            vcp.compute_governed_paths(tree)


class VerifyTests(unittest.TestCase):
    def setUp(self):
        self._scratch = Path(__file__).resolve().parent.parent / "build" / "provenance-test-scratch"
        if self._scratch.exists():
            shutil.rmtree(self._scratch)
        self._scratch.mkdir(parents=True)
        for rel, data in _baseline_blobs().items():
            local = self._scratch / rel
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(data)

    def tearDown(self):
        shutil.rmtree(self._scratch, ignore_errors=True)

    def test_matching_tree_has_no_failures(self):
        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertEqual(failures, [])

    def test_altered_bytes_detected(self):
        tree = FakeTree(_baseline_blobs())
        (self._scratch / "contracts/fixtures/catalog.json").write_bytes(b'{"cards": ["tampered"]}')
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(any("do NOT match" in f for f in failures))

    def test_missing_file_detected(self):
        tree = FakeTree(_baseline_blobs())
        (self._scratch / "contracts/fixtures/decks.json").unlink()
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(any("is missing" in f for f in failures))

    def test_symlink_to_byte_identical_file_rejected(self):
        # A symlink whose TARGET has byte-identical content must still be
        # rejected: byte equality alone is not sufficient provenance.
        tree = FakeTree(_baseline_blobs())
        real = self._scratch / "contracts/fixtures/catalog-real.json"
        real.write_bytes(CATALOG_FIXTURE)
        link = self._scratch / "contracts/fixtures/catalog.json"
        link.unlink()
        link.symlink_to(real)
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(any("symlink" in f for f in failures))

    def test_wrong_backend_mode_detected(self):
        blobs = _baseline_blobs()
        tree = FakeTree(
            blobs,
            modes={"contracts/fixtures/catalog.json": ("100755", "blob")},
        )
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(any("expected a plain non-executable blob" in f for f in failures))

    def test_locally_executable_file_rejected_even_if_bytes_match(self):
        # Byte-identical is not enough: the backend blob is a plain
        # non-executable 100644 file, so a locally-chmod'd +x copy must be
        # rejected even though verify() never inspected the local mode bit
        # via git (there is no local git object -- only a real filesystem
        # file whose st_mode must be checked directly).
        tree = FakeTree(_baseline_blobs())
        target = self._scratch / "contracts/fixtures/catalog.json"
        target.chmod(0o755)
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("locally executable" in f for f in failures),
            f"expected a locally-executable failure, got: {failures}",
        )

    def test_added_unregistered_governed_file_detected(self):
        tree = FakeTree(_baseline_blobs())
        extra = self._scratch / "contracts/schemas/mystery.schema.json"
        extra.write_bytes(b"{}")
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(any("mystery.schema.json" in f and "not reachable" in f for f in failures))

    def test_stale_contract_pin_digest_flagged(self):
        tree = FakeTree(_baseline_blobs())
        stale_digests = {"contracts/fixtures/catalog.json": "0" * 64}
        failures = vcp.verify(tree, self._scratch, governed_digests=stale_digests)
        self.assertTrue(any("ContractPin.cpp is stale" in f for f in failures))


if __name__ == "__main__":
    unittest.main()
