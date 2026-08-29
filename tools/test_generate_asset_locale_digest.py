#!/usr/bin/env python3
"""Tests for generate_asset_locale_digest.py's --check mode (review round-3
item 16): a hash-only fallback comparison, used when clang-format is
unavailable, could previously be fooled by a hand-edited generated header
whose embedded kManifestJsonSha256/kSourceFileHashes comments were left
untouched but whose actual kEntries[] rows were mutated. --check must now
fail closed whenever clang-format did not actually run to completion,
rather than trusting that weaker signal.

Run directly:
    python3 tools/test_generate_asset_locale_digest.py
or via the standard library test runner:
    python3 -m unittest tools.test_generate_asset_locale_digest -v

This uses only the Python standard library (unittest/tempfile) -- no new
tooling/dependency is introduced for this test.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

_TOOLS_DIR = str(Path(__file__).resolve().parent)
sys.path.insert(0, _TOOLS_DIR)
try:
    import generate_asset_locale_digest as gen  # noqa: E402
finally:
    # Restore sys.path immediately after the import completes: leaving
    # tools/ permanently on sys.path for the rest of the process is
    # unnecessary once `gen` is cached in sys.modules, and keeping this
    # hermetic avoids surprising import-order coupling if more Python
    # tests/tools are ever added to this repository.
    sys.path.remove(_TOOLS_DIR)


class GenerateAssetLocaleDigestCheckTests(unittest.TestCase):
    """Every test here monkeypatches the module's own path constants to
    point at an isolated temporary directory tree -- never the real
    checked-in contracts/src files -- so nothing here can accidentally
    write to (or depend on the current state of) this repository's real
    generated header or digest sources.
    """

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        contracts_dir = root / "contracts"
        sources_dir = contracts_dir / "asset-locale-digest-sources"
        sources_dir.mkdir(parents=True)
        generated_header = root / "src" / "AssetLocaleDigestData.generated.h"
        generated_header.parent.mkdir(parents=True)

        # Minimal per-locale source files: it/fr/es/zh each have one real
        # entry; ko (Korean) is intentionally empty, mirroring this
        # issue's documented real-world provenance gap.
        source_entries: dict[str, list[str]] = {
            "ita": ["cards/01001.avif"],
            "fr": ["cards/01002.avif"],
            "es": ["cards/01003.avif"],
            "ko": [],
            "zh": ["cards/01004.avif"],
        }
        source_files_manifest: dict[str, dict[str, str]] = {}
        for locale, entries in source_entries.items():
            path = sources_dir / f"{locale}.json"
            raw = json.dumps(entries).encode("utf-8")
            path.write_bytes(raw)
            source_files_manifest[locale] = {
                "path": f"asset-locale-digest-sources/{locale}.json",
                "sha256": hashlib.sha256(raw).hexdigest(),
                "sourceCommit": "0" * 40,
            }

        manifest = {
            "localeMap": {
                "it": "ita",
                "fr": "fr",
                "es": "es",
                "ko": "ko",
                "zh": "zh",
            },
            "acceptedCategoryRoots": {"cards": "card"},
            "ignoredCategoryRoots": {},
            "provenance": {"sourceFiles": source_files_manifest},
        }
        manifest_path = contracts_dir / "asset-locale-digest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

        self._orig_constants = {
            name: getattr(gen, name)
            for name in (
                "REPO_ROOT",
                "CONTRACTS_DIR",
                "MANIFEST_JSON",
                "SOURCES_DIR",
                "GENERATED_HEADER",
            )
        }
        gen.REPO_ROOT = root
        gen.CONTRACTS_DIR = contracts_dir
        gen.MANIFEST_JSON = manifest_path
        gen.SOURCES_DIR = sources_dir
        gen.GENERATED_HEADER = generated_header
        self.addCleanup(lambda: gen.__dict__.update(self._orig_constants))

        self.generated_header = generated_header

    def _hide_clang_format(self) -> None:
        """Makes shutil.which("clang-format") report absent, regardless of
        whether it is actually installed in this environment, so every
        test here is deterministic across machines/CI without depending
        on local tooling.
        """
        original_which = shutil.which

        def patched_which(name: str, *args, **kwargs):
            if name == "clang-format":
                return None
            return original_which(name, *args, **kwargs)

        shutil.which = patched_which
        self.addCleanup(lambda: setattr(shutil, "which", original_which))

    def test_check_passes_immediately_after_a_fresh_generation(self) -> None:
        self.assertEqual(gen.main([]), 0)
        self.assertTrue(self.generated_header.exists())
        self.assertEqual(gen.main(["--check"]), 0)

    def test_check_fails_closed_when_formatter_missing_even_though_up_to_date(
        self,
    ) -> None:
        # The header is written fresh (with whatever real clang-format is
        # actually on this machine's PATH, if any) and is therefore
        # genuinely up to date -- but --check must still refuse to trust
        # that once the formatter is hidden from it, rather than falling
        # back to a hash-only comparison that cannot detect row-level
        # tampering (see the module docstring above).
        self.assertEqual(gen.main([]), 0)
        self._hide_clang_format()
        self.assertEqual(gen.main(["--check"]), 1)

    def test_check_fails_on_hand_mutated_row_with_hashes_left_stale_and_formatter_hidden(
        self,
    ) -> None:
        # Review round-3 item 16's exact named scenario: a single row is
        # hand-mutated directly in the checked-in generated header (the
        # embedded kManifestJsonSha256/per-locale hash constants are left
        # completely untouched), and clang-format is unavailable. The old
        # hash-only fallback would have reported this as "up to date"
        # (the hashes describe the INPUTS, not this mutated OUTPUT row);
        # --check must now fail regardless.
        self.assertEqual(gen.main([]), 0)
        original_text = self.generated_header.read_text(encoding="utf-8")
        self.assertIn("01001", original_text, "fixture row must be present")
        mutated_text = original_text.replace("01001", "99999")
        self.assertNotEqual(mutated_text, original_text)
        self.generated_header.write_text(mutated_text, encoding="utf-8")

        self._hide_clang_format()
        self.assertEqual(gen.main(["--check"]), 1)

    def test_check_fails_on_hand_mutated_row_even_with_formatter_present(
        self,
    ) -> None:
        # Companion to the test above: with a real clang-format available
        # (the ordinary CI/local-dev case), the full-text comparison path
        # already independently catches the same mutation -- this is not
        # a NEW behaviour of this fix, but pins that the fail-closed
        # change above did not accidentally regress the normal path.
        if shutil.which("clang-format") is None:
            self.skipTest("clang-format is not installed in this environment")
        self.assertEqual(gen.main([]), 0)
        original_text = self.generated_header.read_text(encoding="utf-8")
        mutated_text = original_text.replace("01001", "99999")
        self.generated_header.write_text(mutated_text, encoding="utf-8")
        self.assertEqual(gen.main(["--check"]), 1)

    def test_check_fails_when_generated_header_does_not_exist_at_all(self) -> None:
        self.assertFalse(self.generated_header.exists())
        self.assertEqual(gen.main(["--check"]), 1)


if __name__ == "__main__":
    unittest.main()
