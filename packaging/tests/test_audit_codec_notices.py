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

    def test_second_review_wave_of_bundled_system_libraries_classifies(
        self,
    ) -> None:
        # A later cumulative review (round-4/5 item 12's continuation) ran
        # this exact classifier against the real produced AppImage's full
        # `usr/` tree (not just `usr/lib`, which the original round-4
        # item-12 work above was validated against) and found 72
        # previously-unmapped bundled libraries -- the xcb/X11/xkbcommon
        # family Qt's xcb platform plugin transitively needs,
        # QtKeychain's own bundled libqt6keychain.so* (previously only
        # its *dependencies* were mapped, never the library itself), and
        # a further wave of glibc-adjacent system libraries various
        # distributions' glib/D-Bus/Qt builds transitively pull in. See
        # the individual third_party/<name>/NOTICE.md files this review
        # added for exactly why each is bundled.
        cases = {
            "libXau.so.6": "libxau",
            "libXdmcp.so.6": "libxdmcp",
            "libxcb.so.1": "xcb",
            "libxcb-glx.so.0": "xcb",
            "libxcb-randr.so.0": "xcb",
            "libxcb-render.so.0": "xcb",
            "libxcb-shape.so.0": "xcb",
            "libxcb-shm.so.0": "xcb",
            "libxcb-sync.so.1": "xcb",
            "libxcb-xfixes.so.0": "xcb",
            "libxcb-xkb.so.1": "xcb",
            "libxkbcommon.so.0": "xkbcommon",
            "libxkbcommon-x11.so.0": "xkbcommon",
            "libbrotlicommon.so.1": "brotli",
            "libbrotlidec.so.1": "brotli",
            "libbsd.so.0": "libbsd",
            "libmd.so.0": "libmd",
            "libcap.so.2": "libcap",
            "libdbus-1.so.3": "dbus",
            "libgssapi_krb5.so.2": "krb5",
            "libk5crypto.so.3": "krb5",
            "libkrb5.so.3": "krb5",
            "libkrb5support.so.0": "krb5",
            "libicudata.so.73": "icu",
            "libicui18n.so.73": "icu",
            "libicuuc.so.73": "icu",
            "libkeyutils.so.1": "libkeyutils",
            "liblz4.so.1": "lz4",
            "libpcre.so.3": "pcre",
            "libpng16.so.16": "libpng",
            "libqt6keychain.so": "qtkeychain",
            "libqt6keychain.so.0.17.0": "qtkeychain",
            "libqt6keychain.so.1": "qtkeychain",
            "libsystemd.so.0": "systemd",
            "libzstd.so.1": "zstd",
        }
        for basename, expected in cases.items():
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), expected)

    def test_xcb_util_family_are_distinct_components_from_base_xcb(self) -> None:
        # A later cumulative review found the single wildcard `libxcb.*`
        # pattern this test previously encoded incorrectly conflated
        # base libxcb (and its own built-in protocol extensions, still
        # legitimately "xcb") with FIVE further libxcb-* libraries that
        # are each their own genuinely separate upstream git
        # repository/project with distinct, differently-dated copyright
        # holders -- see third_party/xcb/NOTICE.md and the sibling
        # third_party/xcb-util*/NOTICE.md files. Each must resolve to
        # its own distinct component, never collapse onto "xcb".
        cases = {
            "libxcb-util.so.1": "xcb-util",
            "libxcb-image.so.0": "xcb-util-image",
            "libxcb-keysyms.so.1": "xcb-util-keysyms",
            "libxcb-render-util.so.0": "xcb-util-renderutil",
            "libxcb-icccm.so.4": "xcb-util-wm",
            "libxcb-ewmh.so.2": "xcb-util-wm",
            "libxcb-cursor.so.0": "xcb-util-cursor",
        }
        for basename, expected in cases.items():
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), expected)
        # And base libxcb / its own built-in extensions must still
        # classify as "xcb", never accidentally swept into one of the
        # xcb-util-* components above by an overly broad regex.
        for basename in ("libxcb.so.1", "libxcb-randr.so.0", "libxcb-dri3.so.0"):
            with self.subTest(basename=basename):
                self.assertEqual(audit.classify(basename), "xcb")

    def test_unrecognized_future_xcb_library_is_unmapped_not_xcb(self) -> None:
        # The explicit, closed per-component list replacing the old
        # `libxcb.*` wildcard must never silently accept a hypothetical
        # future libxcb-something-new.so this list has no entry for --
        # proving the "unknown binary must fail" requirement holds for
        # the xcb family specifically, not just in the general case.
        self.assertIsNone(audit.classify("libxcb-somethingnew.so.1"))


    def test_legacy_pcre1_and_pcre2_remain_distinct_components(self) -> None:
        # libpcre.so.3 (legacy PCRE1) and libpcre2-8.so.0 (PCRE2, already
        # covered by test_libsecret_transitive_closure_classifies above)
        # must resolve to two different components, not collapse onto
        # one -- they are separate upstream projects with separate
        # third_party/ notice directories.
        self.assertEqual(audit.classify("libpcre.so.3"), "pcre")
        self.assertEqual(audit.classify("libpcre2-8.so.0.11.2"), "pcre2")

    def test_completely_unknown_library_is_unmapped(self) -> None:
        self.assertIsNone(audit.classify("libtotallyunknownvendorlib.so.1"))

    def test_qt_plugin_directory_classifies_against_verified_qt_reference(
        self,
    ) -> None:
        # Qt's own plugins (imageformats/libqjpeg.so, platforms/libqxcb.so,
        # generic/libqoffscreen.so -- the latter explicitly force-bundled
        # by this project's own build-appimage.sh via
        # EXTRA_PLATFORM_PLUGINS) do not match the libQt6.* basename
        # pattern at all; classify_path() must still resolve them to "qt"
        # by directory location, but -- per a later cumulative review
        # that found the directory-name-only version of this check
        # fail-open -- ONLY once a file with the identical relative
        # sub-path is verified to genuinely exist under a supplied
        # qt_reference_dir (standing in for the real Qt SDK install).
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "imageformats").mkdir(parents=True)
            (qt_root / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(b"")
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(b"")
            (qt_root / "plugins" / "generic").mkdir(parents=True)
            (qt_root / "plugins" / "generic" / "libqoffscreen.so").write_bytes(b"")
            (qt_root / "plugins" / "tls").mkdir(parents=True)
            (qt_root / "plugins" / "tls" / "libqopensslbackend.so").write_bytes(b"")

            cases = {
                Path("/AppDir/usr/plugins/imageformats/libqjpeg.so"): "qt",
                Path("/AppDir/usr/plugins/platforms/libqxcb.so"): "qt",
                Path("/AppDir/usr/plugins/generic/libqoffscreen.so"): "qt",
                Path("/AppDir/usr/plugins/tls/libqopensslbackend.so"): "qt",
            }
            for path, expected in cases.items():
                with self.subTest(path=str(path)):
                    self.assertEqual(audit.classify_path(path, qt_root), expected)

    def test_qt_plugin_directory_without_reference_dir_is_unmapped(self) -> None:
        # The core fail-closed-by-default contract: omitting
        # qt_reference_dir entirely must NOT fall back to trusting the
        # directory name alone (that was the exact fail-open defect a
        # later cumulative review found and required be fixed).
        self.assertIsNone(
            audit.classify_path(Path("/AppDir/usr/plugins/platforms/libqxcb.so"))
        )

    def test_qt_plugin_directory_with_unverified_binary_is_unmapped(self) -> None:
        # The core regression this review finding specifically requires:
        # an attacker (or a broken build step) placing an arbitrary,
        # unaudited .so directly inside a real Qt plugin directory must
        # be reported unmapped (and fail packaging), not silently
        # accepted as "qt" merely because of its parent directory's name.
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(b"")
            # Note: no "libevil.so" exists anywhere under qt_root.
            self.assertIsNone(
                audit.classify_path(
                    Path("/AppDir/usr/plugins/platforms/libevil.so"), qt_root
                )
            )

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

    def test_qml_module_directory_classifies_against_verified_qt_reference(
        self,
    ) -> None:
        # linuxdeploy's Qt plugin deploys every Qt Quick/QML module under
        # a fixed top-level "qml" directory name, arbitrarily nested
        # (e.g. usr/qml/QtQuick/Controls/Basic/impl/...); none of these
        # basenames match libQt6.* or live in a QT_PLUGIN_DIRECTORIES
        # entry, so without this directory-based fallback every one of
        # them is reported unmapped the first time this classifier runs
        # against a real AppImage built with QML content -- but, as with
        # Qt plugins above, only once verified against qt_reference_dir.
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "qml" / "QtQml" / "Models").mkdir(parents=True)
            (
                qt_root / "qml" / "QtQml" / "Models" / "libmodelsplugin.so"
            ).write_bytes(b"")
            (
                qt_root
                / "qml"
                / "QtQuick"
                / "Controls"
                / "Basic"
                / "impl"
            ).mkdir(parents=True)
            (
                qt_root
                / "qml"
                / "QtQuick"
                / "Controls"
                / "Basic"
                / "impl"
                / "libqtquickcontrols2basicstyleimplplugin.so"
            ).write_bytes(b"")
            (qt_root / "qml" / "QtQuick" / "Effects").mkdir(parents=True)
            (
                qt_root / "qml" / "QtQuick" / "Effects" / "libeffectsplugin.so"
            ).write_bytes(b"")
            (qt_root / "qml" / "QtQuick").mkdir(parents=True, exist_ok=True)
            (qt_root / "qml" / "QtQuick" / "libqtquick2plugin.so").write_bytes(b"")

            cases = {
                Path("/AppDir/usr/qml/QtQml/Models/libmodelsplugin.so"): "qt",
                Path(
                    "/AppDir/usr/qml/QtQuick/Controls/Basic/impl/"
                    "libqtquickcontrols2basicstyleimplplugin.so"
                ): "qt",
                Path("/AppDir/usr/qml/QtQuick/Effects/libeffectsplugin.so"): "qt",
                Path("/AppDir/usr/qml/QtQuick/libqtquick2plugin.so"): "qt",
            }
            for path, expected in cases.items():
                with self.subTest(path=str(path)):
                    self.assertEqual(audit.classify_path(path, qt_root), expected)

    def test_qml_directory_without_reference_dir_is_unmapped(self) -> None:
        self.assertIsNone(
            audit.classify_path(
                Path("/AppDir/usr/qml/QtQml/Models/libmodelsplugin.so")
            )
        )

    def test_qml_directory_with_unverified_binary_is_unmapped(self) -> None:
        # Same fail-closed regression as the Qt plugin-directory case
        # above, for the "qml" root instead.
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "qml" / "QtQml" / "Models").mkdir(parents=True)
            (
                qt_root / "qml" / "QtQml" / "Models" / "libmodelsplugin.so"
            ).write_bytes(b"")
            self.assertIsNone(
                audit.classify_path(
                    Path("/AppDir/usr/qml/QtQml/Models/libevilplugin.so"), qt_root
                )
            )

    def test_qml_directory_match_requires_whole_path_component(self) -> None:
        # QT_QML_ROOT_DIRNAME must match a literal "qml" path *component*,
        # never a bare substring -- a hypothetical unrelated
        # "libqmlfoo.so" living outside any real "qml" directory must not
        # be misclassified as Qt merely because its basename contains the
        # substring "qml". Verified both without and with a qt_reference_dir
        # present (containing an unrelated real qml module), so this
        # isn't merely passing because no reference directory was
        # supplied at all.
        self.assertIsNone(
            audit.classify_path(Path("/AppDir/usr/lib/libqmlfoo.so.1"))
        )
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "qml" / "QtQml" / "Models").mkdir(parents=True)
            (
                qt_root / "qml" / "QtQml" / "Models" / "libmodelsplugin.so"
            ).write_bytes(b"")
            self.assertIsNone(
                audit.classify_path(Path("/AppDir/usr/lib/libqmlfoo.so.1"), qt_root)
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
        # test_qt_plugin_directory_classifies_against_verified_qt_reference
        # above.
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
        # output. Requires a verified qt_reference_dir (see
        # test_qt_plugin_directory_without_reference_dir_is_unmapped for
        # the fail-closed-by-default case).
        (self.lib_dir / "imageformats").mkdir()
        (self.lib_dir / "imageformats" / "libqjpeg.so").write_bytes(b"")
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(b"")
        (self.lib_dir / "generic").mkdir()
        (self.lib_dir / "generic" / "libqoffscreen.so").write_bytes(b"")
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "imageformats").mkdir(parents=True)
            (qt_root / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(b"")
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(b"")
            (qt_root / "plugins" / "generic").mkdir(parents=True)
            (qt_root / "plugins" / "generic" / "libqoffscreen.so").write_bytes(b"")
            by_component, unmapped = audit.classify_all(self.lib_dir, qt_root)
            self.assertEqual(unmapped, [])
            self.assertEqual(len(by_component.get("qt", [])), 3)

    def test_classify_all_without_reference_dir_reports_qt_plugins_unmapped(
        self,
    ) -> None:
        # classify_all's own default (qt_reference_dir=None) must fail
        # closed exactly like classify_path's -- a caller that forgets to
        # pass a reference directory does not silently regress to the
        # old fail-open directory-name-only behavior.
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(b"")
        by_component, unmapped = audit.classify_all(self.lib_dir)
        self.assertEqual(len(unmapped), 1)
        self.assertNotIn("qt", by_component)

    def _run_classify_cli(self, *extra_args: str) -> tuple[int, str, str]:
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(["classify", str(self.lib_dir), *extra_args])
        return exit_code, stdout.getvalue(), stderr.getvalue()

    def test_classify_cli_qt_reference_dir_resolves_real_qt_plugins(self) -> None:
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(b"")
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(b"")
            exit_code, _stdout, stderr = self._run_classify_cli(
                "--qt-reference-dir", str(qt_root)
            )
            # libavif is still mandatory and missing here, but the Qt
            # plugin itself must not additionally show up as unmapped.
            self.assertEqual(exit_code, 1)
            self.assertNotIn("libqxcb.so", stderr)

    def test_classify_cli_without_qt_reference_dir_fails_on_qt_plugin(self) -> None:
        self._touch("libavif.so.16.0.0")
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(b"")
        exit_code, _stdout, stderr = self._run_classify_cli()
        self.assertEqual(exit_code, 1)
        self.assertIn("libqxcb.so", stderr)

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
