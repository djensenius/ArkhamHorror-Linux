#!/usr/bin/env python3
"""Tests for packaging/audit_codec_notices.py (review round-4 item 12):
the full recursive ELF-closure classifier that requires every bundled
library under an AppDir to map to a known, notice-bearing third-party
component, or fail packaging outright.

These tests use synthetic, empty, zero-byte fake ".so" files -- classify()
only ever inspects a library's basename via COMPONENT_PATTERNS, never its
actual ELF content, so no real compiler/readelf/Linux dependency is needed
here (unlike packaging/tests/test_audit_dependency_closure.sh, which needs
genuine DT_NEEDED entries and therefore does need a real ELF toolchain).

Run directly:
    python3 packaging/tests/test_audit_codec_notices.py
"""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

_PACKAGING_DIR = str(Path(__file__).resolve().parent.parent)
sys.path.insert(0, _PACKAGING_DIR)
try:
    import audit_codec_notices as audit  # noqa: E402
finally:
    sys.path.remove(_PACKAGING_DIR)


class ClassifyTests(unittest.TestCase):
    def test_mandatory_libavif_classifies(self) -> None:
        self.assertEqual(audit.classify("libavif.so.16.0.0"), "libavif")

    def test_every_known_av1_backend_classifies(self) -> None:
        cases = {
            "libdav1d.so.7.0.0": "dav1d",
            "libaom.so.3": "libaom",
            "libgav1.so.1": "libgav1",
            "librav1e.so.0": "rav1e",
            "libSvtAv1Enc.so.2": "svt-av1",
            "libyuv.so.0": "libyuv",
        }
        for basename, expected in cases.items():
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), expected)

    def test_libjpeg_classifies(self) -> None:
        self.assertEqual(audit.classify("libjpeg.so.8.3.2"), "libjpeg")

    def test_libsecret_transitive_closure_classifies(self) -> None:
        # Round-4 review item 12 named libjpeg/abseil as the two concrete
        # gaps found on the real produced AppImage, but the underlying
        # defect -- a small handwritten glob table that only anticipated
        # codec libraries -- applies identically to bundled libsecret's
        # own transitive ELF-linked closure (see build-appimage.sh's
        # force-bundled libsecret/libgpg-error/libgcc_s/libstdc++/zlib
        # plus linuxdeploy's automatically-resolved libgcrypt/glib
        # family/libffi/pcre2/util-linux/libselinux/liblzma). Every one of
        # these was previously entirely unmapped and would have silently
        # shipped without attribution the very first time this stricter
        # audit ran against a real build.
        cases = {
            "libsecret-1.so.0.0.0": "libsecret",
            "libgpg-error.so.0.34.0": "libgpg-error",
            "libgcrypt.so.20.4.4": "libgcrypt",
            "libffi.so.8.1.2": "libffi",
            "libglib-2.0.so.0.8400.0": "glib",
            "libgobject-2.0.so.0.8400.0": "glib",
            "libgio-2.0.so.0.8400.0": "glib",
            "libgmodule-2.0.so.0.8400.0": "glib",
            "libgthread-2.0.so.0.8400.0": "glib",
            "libz.so.1.3.1": "zlib",
            "libpcre2-8.so.0.11.2": "pcre2",
            "libmount.so.1.1.0": "util-linux",
            "libblkid.so.1.1.0": "util-linux",
            "libuuid.so.1.3.0": "util-linux",
            "libselinux.so.1": "libselinux",
            "liblzma.so.5.4.5": "liblzma",
            "libgcc_s.so.1": "gcc-runtime",
            "libstdc++.so.6.0.33": "gcc-runtime",
        }
        for basename, expected in cases.items():
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), expected)

    def test_abseil_family_classifies_by_prefix(self) -> None:
        # Round-4 review item 12's named gap: an entire family of
        # libabsl_*.so* libraries, not a single fixed name.
        for basename in (
            "libabsl_base.so.2407.0.0",
            "libabsl_strings.so.2407.0.0",
            "libabsl_synchronization.so.2407.0.0",
            "libabsl_time.so.2407.0.0",
        ):
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), "abseil")

    def test_qt_family_classifies_by_prefix(self) -> None:
        for basename in (
            "libQt6Core.so.6",
            "libQt6Gui.so.6",
            "libQt6Quick.so.6",
            "libQt6Network.so.6",
        ):
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), "qt")

    def test_completely_unknown_library_is_unmapped(self) -> None:
        self.assertIsNone(audit.classify("libtotallyunknownvendorlib.so.1"))

    def test_qt_plugin_directory_classifies_regardless_of_basename(
        self,
    ) -> None:
        # Qt's own plugins (imageformats/libqjpeg.so, platforms/libqxcb.so,
        # generic/libqoffscreen.so -- the latter explicitly force-bundled
        # by this project's own build-appimage.sh via
        # EXTRA_PLATFORM_PLUGINS) do not match the libQt6.* basename
        # pattern at all; classify_path() must still resolve them to "qt"
        # by directory location, or every real AppImage build's bundled
        # Qt plugins would be reported unmapped.
        cases = {
            Path("/AppDir/usr/plugins/imageformats/libqjpeg.so"): "qt",
            Path("/AppDir/usr/plugins/platforms/libqxcb.so"): "qt",
            Path("/AppDir/usr/plugins/generic/libqoffscreen.so"): "qt",
            Path("/AppDir/usr/plugins/tls/libqopensslbackend.so"): "qt",
        }
        for path, expected in cases.items():
            with self.subTest(path=str(path)):
                self.assertEqual(audit.classify_path(path), expected)

    def test_qt_plugin_directory_does_not_shadow_unrelated_library(
        self,
    ) -> None:
        # A library that happens to share a basename pattern but lives
        # outside any Qt plugin directory must still be classified (or
        # left unmapped) purely by its own basename.
        self.assertEqual(
            audit.classify_path(Path("/AppDir/usr/lib/libQt6Core.so.6")), "qt"
        )
        self.assertIsNone(
            audit.classify_path(Path("/AppDir/usr/lib/libtotallyunknown.so.1"))
        )

    def test_abi_allowlisted_library_is_not_itself_matched_by_a_component(
        self,
    ) -> None:
        # classify() alone does not special-case the ABI allowlist (that is
        # deliberately the caller's job, exercised via classify_all below);
        # this documents that libc.so.6 simply matches no COMPONENT_PATTERNS
        # entry either.
        self.assertIsNone(audit.classify("libc.so.6"))


class ClassifyAllAndCliTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.lib_dir = Path(self._tmp.name) / "lib"
        self.lib_dir.mkdir()

    def _touch(self, *names: str) -> None:
        for name in names:
            (self.lib_dir / name).write_bytes(b"")

    def test_classify_all_groups_by_component_and_excludes_allowlist(
        self,
    ) -> None:
        self._touch(
            "libavif.so.16.0.0",
            "libdav1d.so.7.0.0",
            "libc.so.6",
            "ld-linux-x86-64.so.2",
        )
        by_component, unmapped = audit.classify_all(self.lib_dir)
        self.assertEqual(set(by_component.keys()), {"libavif", "dav1d"})
        self.assertEqual(unmapped, [])

    def test_classify_all_reports_unmapped_library(self) -> None:
        self._touch("libavif.so.16.0.0", "libmysteryvendor.so.3")
        by_component, unmapped = audit.classify_all(self.lib_dir)
        self.assertEqual(len(unmapped), 1)
        self.assertEqual(unmapped[0].name, "libmysteryvendor.so.3")
        self.assertIn("libavif", by_component)

    def test_classify_all_finds_libraries_in_nested_plugin_directories(
        self,
    ) -> None:
        # linuxdeploy's exact placement (usr/lib vs. a per-arch subdirectory
        # vs. a nested plugin-style directory) is an implementation detail
        # this must not hard-code one particular layout for. Uses a
        # non-Qt-plugin directory name so this is purely a nested-directory
        # discovery check, independent of the dedicated Qt-plugin-directory
        # classification behavior covered by
        # test_qt_plugin_directory_classifies_regardless_of_basename above.
        nested = self.lib_dir / "x86_64-linux-gnu" / "codecs"
        nested.mkdir(parents=True)
        (nested / "libjpeg.so.8").write_bytes(b"")
        by_component, unmapped = audit.classify_all(self.lib_dir)
        self.assertEqual(unmapped, [])
        self.assertIn("libjpeg", by_component)

    def test_classify_all_groups_real_qt_plugin_layout_as_qt(self) -> None:
        # End-to-end (via classify_all, not classify_path directly): Qt's
        # real plugin basenames (libqjpeg.so, libqxcb.so, libqoffscreen.so
        # -- the latter explicitly force-included by this project's own
        # build-appimage.sh via EXTRA_PLATFORM_PLUGINS) never match the
        # libQt6.* basename prefix, so without directory-based
        # classification every one of these would show up as unmapped the
        # first time this ran against a real linuxdeploy --plugin qt
        # output.
        (self.lib_dir / "imageformats").mkdir()
        (self.lib_dir / "imageformats" / "libqjpeg.so").write_bytes(b"")
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(b"")
        (self.lib_dir / "generic").mkdir()
        (self.lib_dir / "generic" / "libqoffscreen.so").write_bytes(b"")
        by_component, unmapped = audit.classify_all(self.lib_dir)
        self.assertEqual(unmapped, [])
        self.assertEqual(len(by_component.get("qt", [])), 3)

    def _run_classify_cli(self, *extra_args: str) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(["classify", str(self.lib_dir), *extra_args])
        return exit_code, stdout.getvalue(), stderr.getvalue()

    def test_classify_cli_fails_when_mandatory_libavif_missing(self) -> None:
        self._touch("libdav1d.so.7.0.0")
        exit_code, _stdout, stderr = self._run_classify_cli()
        self.assertEqual(exit_code, 1)
        self.assertIn("libavif", stderr)

    def test_classify_cli_fails_on_unmapped_new_library(self) -> None:
        # The core mutation-test contract review round-4 item 12 requires:
        # adding one brand-new, never-classified ".so" must fail packaging.
        self._touch("libavif.so.16.0.0", "libbrandnewvendorlib.so.1")
        exit_code, _stdout, stderr = self._run_classify_cli()
        self.assertEqual(exit_code, 1)
        self.assertIn("libbrandnewvendorlib.so.1", stderr)

    def test_classify_cli_succeeds_and_prints_tsv_lines(self) -> None:
        self._touch("libavif.so.16.0.0", "libdav1d.so.7.0.0", "libc.so.6")
        exit_code, stdout, _stderr = self._run_classify_cli()
        self.assertEqual(exit_code, 0)
        lines = sorted(stdout.strip().splitlines())
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("dav1d\t"))
        self.assertTrue(lines[1].startswith("libavif\t"))

    def test_classify_cli_json_out_reports_full_manifest(self) -> None:
        self._touch("libavif.so.16.0.0", "libc.so.6", "libunknownthing.so.1")
        json_path = Path(self._tmp.name) / "sbom.json"
        exit_code, _stdout, _stderr = self._run_classify_cli(
            "--json-out", str(json_path)
        )
        self.assertEqual(exit_code, 1)  # unmapped library still fails
        manifest = json.loads(json_path.read_text())
        self.assertEqual(manifest["components"]["libavif"], ["libavif.so.16.0.0"])
        self.assertEqual(manifest["unmapped"], ["libunknownthing.so.1"])


class VerifyNoticesTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        root = Path(self._tmp.name)
        self.lib_dir = root / "lib"
        self.third_party_root = root / "third_party"
        self.doc_root = root / "doc"
        self.lib_dir.mkdir()
        self.third_party_root.mkdir()
        self.doc_root.mkdir()

    def _run(self) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "verify-notices",
                    str(self.lib_dir),
                    str(self.third_party_root),
                    str(self.doc_root),
                ]
            )
        return exit_code, stdout.getvalue(), stderr.getvalue()

    def _seed_component(self, component: str, content: bytes = b"LICENSE TEXT") -> None:
        (self.lib_dir / f"{component}-marker.so.1").write_bytes(b"")
        source_dir = self.third_party_root / component
        source_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "LICENSE").write_bytes(content)

    def test_matching_notice_content_passes(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        bundled_dir = self.doc_root / "libavif"
        bundled_dir.mkdir()
        (bundled_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        exit_code, _stdout, _stderr = self._run()
        self.assertEqual(exit_code, 0)

    def test_missing_bundled_notice_file_fails(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        # doc_root/libavif/ intentionally left empty -- notice never bundled.
        (self.doc_root / "libavif").mkdir()
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("missing its required non-empty", stderr)

    def test_content_drifted_bundled_notice_fails_checksum(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"the real license text")
        bundled_dir = self.doc_root / "libavif"
        bundled_dir.mkdir()
        # Same filename, but content silently differs from the checked-in
        # source -- e.g. a stale copy left over from an older release.
        (bundled_dir / "LICENSE").write_bytes(b"a DIFFERENT, drifted license text")
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("does not match the checked-in source", stderr)

    def test_missing_third_party_source_directory_fails(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        # third_party/libavif/ never created at all.
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("no checked-in notice source directory", stderr)

    def test_unmapped_bundled_library_fails_before_any_notice_check(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        (self.lib_dir / "libunexpectedvendor.so.1").write_bytes(b"")
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        bundled_dir = self.doc_root / "libavif"
        bundled_dir.mkdir()
        (bundled_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("libunexpectedvendor.so.1", stderr)

    def test_second_required_file_missing_is_also_caught(self) -> None:
        # Mirrors bundle_codec_notices.sh's own existing guarantee that
        # every file in a source directory (not just the first/LICENSE)
        # must be verified -- e.g. libaom's extra PATENTS file.
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(b"")
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"license text")
        (source_dir / "NOTICE.md").write_bytes(b"notice text")
        bundled_dir = self.doc_root / "libavif"
        bundled_dir.mkdir()
        (bundled_dir / "LICENSE").write_bytes(b"license text")
        # NOTICE.md deliberately never bundled.
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("NOTICE.md", stderr)


if __name__ == "__main__":
    unittest.main()
