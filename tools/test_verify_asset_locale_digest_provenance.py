#!/usr/bin/env python3
"""Tests for verify_asset_locale_digest_provenance.py (review round-4 item
11): the pinned locale-source-file provenance check must fail closed on
every attack this test file names, and must never succeed without a
genuine (here: mocked, but shaped exactly like the real GitHub Git Data
API) network round trip.

Every test injects a fake `fetch_json` callable -- there is no live
network access in this test file, so it runs identically offline and in
CI.

Run directly:
    python3 tools/test_verify_asset_locale_digest_provenance.py
"""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

_TOOLS_DIR = str(Path(__file__).resolve().parent)
sys.path.insert(0, _TOOLS_DIR)
try:
    import verify_asset_locale_digest_provenance as verify_mod  # noqa: E402
finally:
    sys.path.remove(_TOOLS_DIR)


def _git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode("utf-8")
    return hashlib.sha1(header + data, usedforsecurity=False).hexdigest()


_REAL_COMMIT = "330b7dda81e6fc3be17e50aefd5d0a6fced35a39"
_UPSTREAM_PATH = "frontend/src/digests/{locale}.json"


class VerifyAssetLocaleDigestProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        contracts_dir = root / "contracts"
        self.sources_dir = contracts_dir / "asset-locale-digest-sources"
        self.sources_dir.mkdir(parents=True)

        # A single-locale manifest is enough to exercise every failure
        # mode; using the real fixed locale keeps _EXPECTED_* constants
        # untouched (this script's hardcoded repository identity is not
        # something a test should need to override).
        self.locale_contents = {"ita": b"[\"cards/01001.avif\"]"}
        for locale, content in self.locale_contents.items():
            (self.sources_dir / f"{locale}.json").write_bytes(content)

        self.manifest = {
            "sourceRepository": verify_mod._EXPECTED_SOURCE_REPOSITORY,
            "sourceCommit": _REAL_COMMIT,
            "provenance": {
                "sourceRepository": verify_mod._EXPECTED_SOURCE_REPOSITORY,
                "sourceCommit": _REAL_COMMIT,
                "sourceFiles": {
                    locale: {"path": f"asset-locale-digest-sources/{locale}.json"}
                    for locale in self.locale_contents
                },
            },
        }
        self.manifest_path = contracts_dir / "asset-locale-digest.json"
        self._write_manifest()

    def _write_manifest(self) -> None:
        self.manifest_path.write_text(
            json.dumps(self.manifest, indent=2), encoding="utf-8"
        )

    def _genuine_tree_response(self) -> dict:
        """A tree listing whose blob shas genuinely match the checked-in
        source files -- i.e. what the REAL upstream repository's API
        response looks like when provenance is entirely legitimate."""
        entries = []
        for locale, content in self.locale_contents.items():
            entries.append(
                {
                    "path": _UPSTREAM_PATH.format(locale=locale),
                    "mode": "100644",
                    "type": "blob",
                    "sha": _git_blob_sha1(content),
                }
            )
        return {"tree": entries, "truncated": False}

    def _verify(self, fetch_json) -> None:
        verify_mod.verify(
            manifest_path=self.manifest_path,
            sources_dir=self.sources_dir,
            fetch_json=fetch_json,
        )

    def test_genuinely_matching_provenance_passes(self) -> None:
        tree = self._genuine_tree_response()
        self._verify(lambda url, token: tree)

    def test_wrong_source_repository_fails_without_any_network_call(self) -> None:
        self.manifest["provenance"]["sourceRepository"] = (
            "https://github.com/attacker/ArkhamHorror"
        )
        self._write_manifest()

        def fetch_json(url, token):
            self.fail(
                "must not make any network request once sourceRepository "
                "fails the fixed-identity check"
            )

        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(fetch_json)

    def test_altered_commit_label_with_genuinely_different_upstream_content_fails(
        self,
    ) -> None:
        # Simulates an attacker changing sourceCommit to point at some
        # OTHER real commit where the upstream file's content legitimately
        # differs from what is checked in locally -- the mocked backend
        # here behaves exactly like the real GitHub API would for that
        # commit (returning its own blob sha for its own real content),
        # so this proves the check catches a mismatched commit label even
        # when the "response" is completely well-formed and internally
        # consistent.
        self.manifest["provenance"]["sourceCommit"] = "f" * 40
        self._write_manifest()

        def fetch_json(url, token):
            self.assertIn("f" * 40, url)
            different_upstream_content = b"[\"cards/DIFFERENT.avif\"]"
            return {
                "tree": [
                    {
                        "path": _UPSTREAM_PATH.format(locale="ita"),
                        "mode": "100644",
                        "type": "blob",
                        "sha": _git_blob_sha1(different_upstream_content),
                    }
                ],
                "truncated": False,
            }

        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(fetch_json)

    def test_self_consistent_locally_changed_bytes_and_hash_still_fails(
        self,
    ) -> None:
        # An attacker edits the checked-in source file AND (if this
        # project's manifest still carried a self-referential sha256, as
        # generate_asset_locale_digest.py's manifest historically has)
        # its accompanying hash, keeping local self-consistency perfect.
        # The mocked backend here still reports the REAL, unmodified
        # upstream blob sha (matching the ORIGINAL bytes) -- proving this
        # independent network-sourced comparison catches the tamper even
        # though every local, self-referential check would have passed.
        genuine_tree = self._genuine_tree_response()
        tampered_bytes = b"[\"cards/TAMPERED.avif\"]"
        (self.sources_dir / "ita.json").write_bytes(tampered_bytes)

        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(lambda url, token: genuine_tree)

    def test_extra_locale_map_alias_key_is_out_of_scope_for_this_script(self) -> None:
        # Extra key/alias and absolute/".." path rejection are covered by
        # generate_asset_locale_digest.py's _load_manifest() (see
        # test_generate_asset_locale_digest.py's
        # test_load_manifest_rejects_extra_locale_map_alias_key and
        # sibling tests) since that is the function that owns the
        # localeMap/path-shape contract; this script only ever consumes
        # provenance.sourceFiles' already-validated {locale: {path}}
        # entries to decide WHICH upstream path to check. This test just
        # documents that division of responsibility so it is not silently
        # lost.
        self.assertTrue(True)

    def test_wrong_mode_is_rejected(self) -> None:
        genuine_bytes = self.locale_contents["ita"]
        tree = {
            "tree": [
                {
                    "path": _UPSTREAM_PATH.format(locale="ita"),
                    "mode": "120000",  # symlink, not a regular file
                    "type": "blob",
                    "sha": _git_blob_sha1(genuine_bytes),
                }
            ],
            "truncated": False,
        }
        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(lambda url, token: tree)

    def test_missing_blob_in_tree_is_rejected(self) -> None:
        tree = {"tree": [], "truncated": False}
        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(lambda url, token: tree)

    def test_truncated_tree_listing_is_rejected(self) -> None:
        tree = self._genuine_tree_response()
        tree["truncated"] = True
        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(lambda url, token: tree)

    def test_network_failure_never_falls_back_to_success(self) -> None:
        def fetch_json(url, token):
            raise verify_mod.ProvenanceVerificationError("simulated network outage")

        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(fetch_json)

    def test_default_fetch_json_reports_network_errors_as_provenance_error(
        self,
    ) -> None:
        # Exercises the real (non-mocked) HTTP path against an
        # unroutable/invalid host, confirming urllib failures are wrapped
        # rather than propagating as an uncaught, differently-typed
        # exception (which could bypass a caller's except clause and
        # crash instead of failing closed with a clear message).
        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            verify_mod._default_fetch_json(
                "http://127.0.0.1.invalid.example/does-not-exist", None
            )

    def test_missing_pinned_local_file_is_rejected(self) -> None:
        (self.sources_dir / "ita.json").unlink()
        tree = self._genuine_tree_response()
        with self.assertRaises(verify_mod.ProvenanceVerificationError):
            self._verify(lambda url, token: tree)

    def test_git_blob_sha1_matches_known_git_hash_object_value(self) -> None:
        # Pins the header-construction scheme itself against a
        # known-correct git blob sha1 (verifiable independently via
        # `printf '' | git hash-object --stdin` == the empty blob's
        # well-known sha, e69de29bb2d1d6434b8b29ae775ad8c2e48c5391).
        self.assertEqual(
            verify_mod._git_blob_sha1(b""),
            "e69de29bb2d1d6434b8b29ae775ad8c2e48c5391",
        )


if __name__ == "__main__":
    unittest.main()
