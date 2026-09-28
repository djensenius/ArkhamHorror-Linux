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

import os
import shutil
import sys
import unittest
from pathlib import Path
from unittest import mock

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

    def test_manifest_missing_from_backend_tree_rejected(self):
        # If the pinned backend commit lacked contracts/manifest.json
        # entirely, RemoteGitTree.blob_bytes() would shell out to `git
        # cat-file -p <commit>:<path>`, which exits non-zero for a path
        # that does not exist -- raising an unhandled
        # subprocess.CalledProcessError that would bypass verify()'s
        # intended clean RuntimeError/RefEscapeError reporting and surface
        # as a raw traceback. compute_governed_fixtures() must check
        # existence via ls_tree() first and fail with a clean RuntimeError
        # instead, mirroring compute_schema_closure()'s own
        # ls_tree()-before-blob_bytes() pattern.
        blobs = _baseline_blobs()
        del blobs["contracts/manifest.json"]
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


class RunEnvTests(unittest.TestCase):
    """_run() shells out to git for every RemoteGitTree operation
    (init/fetch/cat-file); these tests prove it disables interactive
    credential prompting deterministically rather than actually invoking
    git, since the point under test is the environment _run() passes to
    subprocess.run, not git's own behavior."""

    def test_disables_git_terminal_prompt_and_askpass_by_default(self):
        # Start from a clean environment (rather than this process's own,
        # which may already happen to set these two variables in some
        # sandboxes/CI images) so the assertion genuinely proves _run()
        # injects them, not that they were merely already present.
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(vcp.subprocess, "run") as run:
                run.return_value = mock.Mock(stdout=b"", returncode=0)
                vcp._run(["git", "status"])
        _, kwargs = run.call_args
        self.assertEqual(kwargs["env"].get("GIT_TERMINAL_PROMPT"), "0")
        self.assertEqual(kwargs["env"].get("GIT_ASKPASS"), "echo")

    def test_preserves_caller_provided_env_entries(self):
        # The injected defaults must not clobber the rest of a caller-
        # supplied environment (or, when none is given, the current
        # process's own os.environ) -- only add the two prompt-disabling
        # keys if genuinely absent.
        with mock.patch.object(vcp.subprocess, "run") as run:
            run.return_value = mock.Mock(stdout=b"", returncode=0)
            vcp._run(["git", "status"], env={"SOME_OTHER_VAR": "kept"})
        _, kwargs = run.call_args
        self.assertEqual(kwargs["env"].get("SOME_OTHER_VAR"), "kept")
        self.assertEqual(kwargs["env"].get("GIT_TERMINAL_PROMPT"), "0")
        self.assertEqual(kwargs["env"].get("GIT_ASKPASS"), "echo")

    def test_does_not_override_an_explicit_caller_choice(self):
        # A caller that deliberately wants a real prompt (or a specific
        # askpass helper) is respected rather than silently overridden.
        with mock.patch.object(vcp.subprocess, "run") as run:
            run.return_value = mock.Mock(stdout=b"", returncode=0)
            vcp._run(
                ["git", "status"],
                env={"GIT_TERMINAL_PROMPT": "1", "GIT_ASKPASS": "my-helper"},
            )
        _, kwargs = run.call_args
        self.assertEqual(kwargs["env"].get("GIT_TERMINAL_PROMPT"), "1")
        self.assertEqual(kwargs["env"].get("GIT_ASKPASS"), "my-helper")

    def test_defaults_from_process_environment_when_no_env_kwarg_given(self):
        sentinel = "verify-contract-provenance-test-sentinel"
        with mock.patch.dict(os.environ, {"VCP_TEST_SENTINEL": sentinel}):
            with mock.patch.object(vcp.subprocess, "run") as run:
                run.return_value = mock.Mock(stdout=b"", returncode=0)
                vcp._run(["git", "status"])
            _, kwargs = run.call_args
            self.assertEqual(kwargs["env"].get("VCP_TEST_SENTINEL"), sentinel)


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

    def test_symlinked_directory_under_scanned_dir_is_not_traversed(self):
        # A symlinked directory placed under a _SCANNED_DIRS entry must be
        # reported as an extra/ungoverned entry itself, and its contents
        # (which live entirely outside contracts/schemas) must never be
        # walked into or reported -- proving os.walk(followlinks=False)
        # actually stops descent rather than merely being requested.
        outside = self._scratch.parent / "outside-secret"
        if outside.exists():
            shutil.rmtree(outside)
        outside.mkdir(parents=True)
        (outside / "leaked.schema.json").write_bytes(b"{}")
        self.addCleanup(lambda: shutil.rmtree(outside, ignore_errors=True))

        link = self._scratch / "contracts/schemas/evil-link"
        link.symlink_to(outside, target_is_directory=True)

        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("contracts/schemas/evil-link" in f and "not reachable" in f for f in failures),
            f"expected the symlinked directory itself to be flagged as extra, got: {failures}",
        )
        self.assertFalse(
            any("leaked.schema.json" in f for f in failures),
            "must never traverse through a symlinked directory to report its contents",
        )

    def test_symlinked_file_under_scanned_dir_flagged_without_following(self):
        # A symlink FILE (not directory) under a scanned dir must also be
        # reported as extra by its own path, never silently accepted just
        # because os.walk()'s filenames listing includes it.
        outside = self._scratch.parent / "outside-secret-file"
        if outside.exists():
            shutil.rmtree(outside)
        outside.mkdir(parents=True)
        target = outside / "elsewhere.schema.json"
        target.write_bytes(b"{}")
        self.addCleanup(lambda: shutil.rmtree(outside, ignore_errors=True))

        link = self._scratch / "contracts/schemas/sneaky.schema.json"
        link.symlink_to(target)

        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("contracts/schemas/sneaky.schema.json" in f and "not reachable" in f for f in failures),
            f"expected the symlink file itself to be flagged as extra, got: {failures}",
        )

    def test_symlinked_directory_cycle_under_scanned_dir_does_not_hang(self):
        # A directory symlink pointing back at its own scanned ancestor
        # (a cycle) must not cause unbounded recursion/hang; it must be
        # reported as an extra entry like any other symlinked directory.
        link = self._scratch / "contracts/schemas/self-loop"
        link.symlink_to(self._scratch / "contracts/schemas", target_is_directory=True)

        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("contracts/schemas/self-loop" in f and "not reachable" in f for f in failures),
            f"expected the cyclic symlink itself to be flagged as extra, got: {failures}",
        )

    def test_symlinked_scanned_root_itself_is_rejected_before_traversal(self):
        # A _SCANNED_DIRS entry -- contracts/schemas itself, the very
        # argument os.walk() is invoked with -- being replaced by a
        # symlink is NOT protected against by os.walk(..., followlinks=
        # False): that flag only stops descent into a symlinked
        # subdirectory encountered *during* the walk, never the walk's
        # own root. Plant byte-identical copies of every governed schema
        # under the symlink target so a naive byte-comparison-only check
        # would otherwise be fooled; this must still fail closed, both
        # as an extra/ungoverned entry for the scanned root itself and as
        # a symlink-component failure for every governed file
        # find_local_extra_files/verify() would otherwise read straight
        # through it.
        outside = self._scratch.parent / "outside-root-swap"
        if outside.exists():
            shutil.rmtree(outside)
        outside.mkdir(parents=True)
        for rel, data in _baseline_blobs().items():
            if rel.startswith("contracts/schemas/"):
                (outside / Path(rel).name).write_bytes(data)
        self.addCleanup(lambda: shutil.rmtree(outside, ignore_errors=True))

        real_schemas = self._scratch / "contracts/schemas"
        shutil.rmtree(real_schemas)
        real_schemas.symlink_to(outside, target_is_directory=True)

        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("contracts/schemas" in f and "not reachable" in f for f in failures),
            f"expected contracts/schemas itself to be flagged as an extra, got: {failures}",
        )
        self.assertTrue(
            any("catalog.schema.json" in f and "is a symlink" in f for f in failures),
            f"expected a symlink-component failure for a governed file living under the "
            f"symlinked scanned root, got: {failures}",
        )

    def test_symlinked_contracts_ancestor_is_rejected_before_traversal(self):
        # A still-more-upstream variant of the same bypass: `contracts`
        # itself -- the shared parent of BOTH contracts/schemas and
        # contracts/fixtures -- being replaced by a symlink. Neither
        # os.walk(..., followlinks=False) nor a leaf-only is_symlink()
        # check on an individual governed file protects against an
        # ancestor directory component being a symlink; only a full
        # root-to-leaf component walk (_first_symlink_path_component)
        # catches this.
        outside = self._scratch.parent / "outside-contracts-swap"
        if outside.exists():
            shutil.rmtree(outside)
        shutil.copytree(self._scratch / "contracts", outside)
        self.addCleanup(lambda: shutil.rmtree(outside, ignore_errors=True))

        real_contracts = self._scratch / "contracts"
        shutil.rmtree(real_contracts)
        real_contracts.symlink_to(outside, target_is_directory=True)

        tree = FakeTree(_baseline_blobs())
        failures = vcp.verify(tree, self._scratch)
        self.assertTrue(
            any("contracts" in f and "not reachable" in f for f in failures),
            f"expected the 'contracts' ancestor itself to be flagged as an extra, got: {failures}",
        )
        self.assertTrue(
            any("manifest.json" in f and "'contracts'" in f and "is a symlink" in f for f in failures),
            f"expected a symlink-component failure naming 'contracts' for a governed file "
            f"living underneath it, got: {failures}",
        )
        # Every governed schema/fixture file underneath the symlinked
        # ancestor must fail closed the same way, not just one example.
        self.assertTrue(
            any("decks.schema.json" in f and "is a symlink" in f for f in failures),
            f"expected the symlink-component failure to apply to every governed path "
            f"beneath the symlinked ancestor, got: {failures}",
        )

    def test_first_symlink_path_component_helper_directly(self):
        # Unit-level coverage of the helper itself, independent of
        # verify()/find_local_extra_files() call sites: absent
        # components, a plain nested file, a leaf symlink, and an
        # ancestor symlink all resolve as documented.
        self.assertIsNone(
            vcp._first_symlink_path_component(self._scratch, "contracts/does-not-exist.json")
        )
        self.assertIsNone(
            vcp._first_symlink_path_component(self._scratch, "contracts/manifest.json")
        )

        leaf_link = self._scratch / "contracts/fixtures/leaf-link.json"
        leaf_link.symlink_to(self._scratch / "contracts/fixtures/catalog.json")
        self.assertEqual(
            vcp._first_symlink_path_component(self._scratch, "contracts/fixtures/leaf-link.json"),
            "contracts/fixtures/leaf-link.json",
        )

        real_schemas = self._scratch / "contracts/schemas"
        shutil.rmtree(real_schemas)
        real_schemas.symlink_to(self._scratch / "contracts/fixtures", target_is_directory=True)
        self.assertEqual(
            vcp._first_symlink_path_component(
                self._scratch, "contracts/schemas/catalog.schema.json"
            ),
            "contracts/schemas",
        )


if __name__ == "__main__":
    unittest.main()
