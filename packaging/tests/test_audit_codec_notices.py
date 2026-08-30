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
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

_PACKAGING_DIR = str(Path(__file__).resolve().parent.parent)
sys.path.insert(0, _PACKAGING_DIR)
try:
    import audit_codec_notices as audit  # noqa: E402
finally:
    sys.path.remove(_PACKAGING_DIR)

# Round-9+ review item 10 ("rglob *.so* omits main executable, helper
# ELFs, AppRun"): find_bundled_libraries() now discovers files by their
# own ELF magic bytes rather than by a `*.so*` basename glob (see its
# docstring in audit_codec_notices.py) -- every synthetic fake ".so" file
# this test module creates must therefore actually carry the ELF magic
# number to still be discovered at all, even though (as this module's
# own top-of-file docstring notes) classify()/classify_path() never
# themselves inspect any ELF content beyond that.
_FAKE_ELF_BYTES = audit._ELF_MAGIC + b"\x00" * 12


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

    def test_third_review_wave_of_bundled_system_libraries_classifies(
        self,
    ) -> None:
        # A real Linux+Qt+linuxdeploy build (used to investigate a
        # different, genuine CI-only closure-audit failure -- missing
        # libcom_err/libfontconfig/libfreetype) surfaced two further
        # previously-unmapped bundled libraries in the same produced
        # AppImage's `usr/lib`: libbz2 (pulled in transitively by Qt/
        # util-linux's own closure) and libsharpyuv (libwebp's own
        # standalone YUV<->RGB helper, pulled in transitively by
        # libavif's closure -- a genuinely separate upstream project
        # from Google's libyuv above, despite the similarly named
        # libraries). libcom_err (force-bundled the same way as
        # libgpg-error, since linuxdeploy's own default blacklist
        # excludes it too) is its OWN "e2fsprogs" component (round-9+
        # review item 11 -- see third_party/e2fsprogs/NOTICE.md for why
        # it is NOT part of the "krb5" component despite superficially
        # looking like it belongs to the same MIT Kerberos 5
        # distribution: the actual distribution package that builds and
        # ships it, Ubuntu Jammy's libcom-err2, has an entirely separate
        # e2fsprogs source package/version/license). See
        # third_party/bzip2/NOTICE.md and third_party/sharpyuv/NOTICE.md
        # for exactly why each of the other two is bundled.
        cases = {
            "libbz2.so.1.0": "bzip2",
            "libsharpyuv.so.0": "sharpyuv",
            "libcom_err.so.2": "e2fsprogs",
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
            (qt_root / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "plugins" / "generic").mkdir(parents=True)
            (qt_root / "plugins" / "generic" / "libqoffscreen.so").write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "plugins" / "tls").mkdir(parents=True)
            (qt_root / "plugins" / "tls" / "libqopensslbackend.so").write_bytes(_FAKE_ELF_BYTES)

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
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
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
            ).write_bytes(_FAKE_ELF_BYTES)
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
            ).write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "qml" / "QtQuick" / "Effects").mkdir(parents=True)
            (
                qt_root / "qml" / "QtQuick" / "Effects" / "libeffectsplugin.so"
            ).write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "qml" / "QtQuick").mkdir(parents=True, exist_ok=True)
            (qt_root / "qml" / "QtQuick" / "libqtquick2plugin.so").write_bytes(_FAKE_ELF_BYTES)

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
            ).write_bytes(_FAKE_ELF_BYTES)
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
            ).write_bytes(_FAKE_ELF_BYTES)
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
            (self.lib_dir / name).write_bytes(_FAKE_ELF_BYTES)

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
        (nested / "libjpeg.so.8").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "imageformats" / "libqjpeg.so").write_bytes(_FAKE_ELF_BYTES)
        (self.lib_dir / "platforms").mkdir()
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
        (self.lib_dir / "generic").mkdir()
        (self.lib_dir / "generic" / "libqoffscreen.so").write_bytes(_FAKE_ELF_BYTES)
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "imageformats").mkdir(parents=True)
            (qt_root / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
            (qt_root / "plugins" / "generic").mkdir(parents=True)
            (qt_root / "plugins" / "generic" / "libqoffscreen.so").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
        with tempfile.TemporaryDirectory() as qt_root_name:
            qt_root = Path(qt_root_name)
            (qt_root / "plugins" / "platforms").mkdir(parents=True)
            (qt_root / "plugins" / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "platforms" / "libqxcb.so").write_bytes(_FAKE_ELF_BYTES)
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


class FullElfDiscoveryAndFirstPartyExecutableTests(unittest.TestCase):
    """Round-9+ review item 10 ("rglob *.so* omits main executable,
    helper ELFs, AppRun; core Qt classified by basename only and
    unauthenticated"): find_bundled_libraries() must discover EVERY real
    ELF object under the audited root by its own magic bytes, regardless
    of its basename -- and this project's own first-party executables
    (the main application binary, AppRun) must be explicitly classified
    (never silently invisible, never reported as an unmapped failure),
    while a hostile file merely sharing one of those basenames at some
    OTHER path is not."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name) / "squashfs-root"
        self.root.mkdir()

    def _write_fake_elf(self, relative_path: str, payload: bytes = b"") -> Path:
        path = self.root / relative_path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(_FAKE_ELF_BYTES + payload)
        return path

    def test_a_non_so_named_elf_helper_binary_is_discovered_and_classified(
        self,
    ) -> None:
        # Round-8's own "usr/" widening already made a file like this
        # theoretically reachable by directory, but the OLD `*.so*` glob
        # inside find_bundled_libraries() still could never discover it,
        # since its basename has no ".so" in it at all -- it was
        # entirely invisible to classify_all()/build_sbom_inventory(),
        # regardless of what directory it lived in.
        self._write_fake_elf("usr/bin/some-helper-tool")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(by_component, {})
        self.assertEqual(
            [str(p.relative_to(self.root)) for p in unmapped],
            ["usr/bin/some-helper-tool"],
        )
        inventory = audit.build_sbom_inventory(self.root)
        self.assertEqual(len(inventory), 1)
        self.assertEqual(inventory[0]["path"], "usr/bin/some-helper-tool")
        self.assertEqual(inventory[0]["classification"], "unmapped")

    def test_main_application_executable_is_classified_first_party(self) -> None:
        self._write_fake_elf("usr/bin/arkham-horror")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(by_component, {})
        self.assertEqual(unmapped, [])
        inventory = audit.build_sbom_inventory(self.root)
        self.assertEqual(len(inventory), 1)
        self.assertEqual(inventory[0]["path"], "usr/bin/arkham-horror")
        self.assertEqual(inventory[0]["classification"], "first-party")

    def test_main_application_executable_is_classified_first_party_when_scanned_from_usr_root(
        self,
    ) -> None:
        # packaging/lib/bundle_codec_notices.sh's real pre-packaging
        # build-time call site (bundle_codec_notices(), invoked from
        # packaging/build-appimage.sh) passes "$app_dir/usr" as lib_dir,
        # NOT the full AppDir root that the later CI verify-notices step
        # scans -- so the main executable's path relative to THIS
        # lib_dir is "bin/arkham-horror", one path segment shorter than
        # the "usr/bin/arkham-horror" the sibling test above exercises.
        # Both conventions must independently classify it as first-party
        # (this exact mismatch was a real regression caught only by
        # actually running the produced CI workflow, not by this test
        # suite alone, since it previously only exercised the full-root
        # convention).
        usr_root = self.root / "usr"
        usr_root.mkdir()
        self._write_fake_elf("usr/bin/arkham-horror")
        by_component, unmapped = audit.classify_all(usr_root)
        self.assertEqual(by_component, {})
        self.assertEqual(unmapped, [])
        inventory = audit.build_sbom_inventory(usr_root)
        self.assertEqual(len(inventory), 1)
        self.assertEqual(inventory[0]["path"], "bin/arkham-horror")
        self.assertEqual(inventory[0]["classification"], "first-party")

    def test_apprun_launcher_is_classified_first_party(self) -> None:
        self._write_fake_elf("AppRun")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(by_component, {})
        self.assertEqual(unmapped, [])
        inventory = audit.build_sbom_inventory(self.root)
        self.assertEqual(inventory[0]["classification"], "first-party")

    def test_apprun_wrapped_alias_is_classified_first_party(self) -> None:
        # A real produced AppImage's second (--plugin qt) linuxdeploy
        # invocation renames the first invocation's AppRun (a symlink to
        # usr/bin/arkham-horror) to "AppRun.wrapped", then writes its own
        # generated launcher stub as the new "AppRun" -- both files
        # coexist at the AppDir root and neither is a distinct
        # third-party artifact. This exact scenario was only caught by
        # actually running the CI packaging workflow.
        self._write_fake_elf("AppRun")
        self._write_fake_elf("AppRun.wrapped")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(by_component, {})
        self.assertEqual(unmapped, [])
        inventory = audit.build_sbom_inventory(self.root)
        classifications = {entry["path"]: entry["classification"] for entry in inventory}
        self.assertEqual(
            classifications,
            {"AppRun": "first-party", "AppRun.wrapped": "first-party"},
        )

    def test_arkham_horror_named_file_at_the_wrong_path_is_not_first_party(
        self,
    ) -> None:
        # The first-party allowance is an exact relative-PATH match, never
        # a basename-only heuristic: a hostile (or merely misplaced) file
        # sharing the real executable's basename at some other location
        # must still be classified normally (and reported unmapped here,
        # since it matches no COMPONENT_PATTERNS entry either).
        self._write_fake_elf("usr/lib/plugins/generic/arkham-horror")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(by_component, {})
        self.assertEqual(
            [str(p.relative_to(self.root)) for p in unmapped],
            ["usr/lib/plugins/generic/arkham-horror"],
        )

    def test_appimage_root_level_apprun_and_usr_bin_are_both_discovered_together(
        self,
    ) -> None:
        # Confirms the fix also actually needs (and this test exercises)
        # scanning from the FULL extracted AppImage root, not merely
        # usr/ -- AppRun lives as a sibling of usr/, not beneath it.
        self._write_fake_elf("AppRun")
        self._write_fake_elf("usr/bin/arkham-horror")
        self._write_fake_elf("usr/lib/libavif.so.16.0.0")
        by_component, unmapped = audit.classify_all(self.root)
        self.assertEqual(unmapped, [])
        self.assertEqual(by_component, {"libavif": [self.root / "usr/lib/libavif.so.16.0.0"]})
        inventory = audit.build_sbom_inventory(self.root)
        classifications = {entry["path"]: entry["classification"] for entry in inventory}
        self.assertEqual(
            classifications,
            {
                "AppRun": "first-party",
                "usr/bin/arkham-horror": "first-party",
                "usr/lib/libavif.so.16.0.0": "libavif",
            },
        )


class CoreQtLibraryAuthenticationTests(unittest.TestCase):
    """Round-9+ review item 10's other half: core Qt shared libraries
    (as opposed to plugins/QML modules, already covered by
    QtPluginContentProvenanceTests) were previously classified purely by
    an unauthenticated basename pattern. These tests use fake (non-real-
    ELF-content) files -- unlike QtPluginContentProvenanceTests, which
    needs genuine compiled objects specifically to exercise the
    build-id-survives-patchelf case -- since the sha256-fallback
    comparison path (exercised here because these fake files have no
    real build-id note at all) needs no readelf/compiler and is
    therefore fully portable."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.qt_root = self.root / "qt_sdk"
        (self.qt_root / "lib").mkdir(parents=True)
        self.appdir = self.root / "appdir" / "usr" / "lib"
        self.appdir.mkdir(parents=True)

    def test_genuine_core_qt_library_with_identical_bytes_classifies_as_qt(
        self,
    ) -> None:
        reference = self.qt_root / "lib" / "libQt6Core.so.6"
        reference.write_bytes(_FAKE_ELF_BYTES + b"genuine qt core payload")
        bundled = self.appdir / "libQt6Core.so.6"
        bundled.write_bytes(reference.read_bytes())
        self.assertEqual(audit.classify_path(bundled, self.qt_root), "qt")

    def test_substituted_core_qt_library_is_not_classified_as_qt(self) -> None:
        # The exact HIGH-severity attack this finding describes: a
        # different (substituted) libQt6Core.so.6 -- different bytes --
        # placed at the identical bundled path. Previously accepted
        # unconditionally by classify()'s bare `^libQt6.*\.so` basename
        # pattern; must now be rejected once a qt_reference_dir is
        # available.
        reference = self.qt_root / "lib" / "libQt6Core.so.6"
        reference.write_bytes(_FAKE_ELF_BYTES + b"genuine qt core payload")
        substituted = self.appdir / "libQt6Core.so.6"
        substituted.write_bytes(_FAKE_ELF_BYTES + b"a completely different payload")
        self.assertNotEqual(
            audit._sha256(reference), audit._sha256(substituted)
        )
        self.assertIsNone(audit.classify_path(substituted, self.qt_root))

    def test_fake_qt_named_library_with_no_reference_counterpart_is_unmapped(
        self,
    ) -> None:
        # A hostile "libQt6Backdoor.so.6" -- matching the core Qt naming
        # convention by basename alone, but never actually present in the
        # real Qt SDK at all.
        backdoor = self.appdir / "libQt6Backdoor.so.6"
        backdoor.write_bytes(_FAKE_ELF_BYTES + b"not really qt")
        self.assertIsNone(audit.classify_path(backdoor, self.qt_root))

    def test_without_a_reference_dir_core_qt_still_falls_back_to_basename(
        self,
    ) -> None:
        # Preserves classify_path()'s own documented backward-compatible
        # behavior for callers that never had a Qt SDK reference
        # available at all (qt_reference_dir=None) -- only ever a
        # degraded mode relative to the authenticated path above, never
        # the production CI invocation (which always supplies
        # --qt-reference-dir).
        unverified = self.appdir / "libQt6Core.so.6"
        unverified.write_bytes(_FAKE_ELF_BYTES)
        self.assertEqual(audit.classify_path(unverified, None), "qt")

    def test_classify_all_end_to_end_rejects_substituted_core_qt_library(
        self,
    ) -> None:
        reference = self.qt_root / "lib" / "libQt6Core.so.6"
        reference.write_bytes(_FAKE_ELF_BYTES + b"genuine qt core payload")
        substituted = self.appdir / "libQt6Core.so.6"
        substituted.write_bytes(_FAKE_ELF_BYTES + b"a completely different payload")
        by_component, unmapped = audit.classify_all(
            self.appdir, qt_reference_dir=self.qt_root
        )
        self.assertEqual(by_component, {})
        self.assertEqual(
            [str(p.relative_to(self.appdir)) for p in unmapped],
            ["libQt6Core.so.6"],
        )


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
        (self.lib_dir / f"{component}-marker.so.1").write_bytes(_FAKE_ELF_BYTES)
        source_dir = self.third_party_root / component
        source_dir.mkdir(parents=True, exist_ok=True)
        (source_dir / "LICENSE").write_bytes(content)

    def test_matching_notice_content_passes(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        bundled_dir = self.doc_root / "libavif"
        bundled_dir.mkdir()
        (bundled_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        exit_code, _stdout, _stderr = self._run()
        self.assertEqual(exit_code, 0)

    def test_missing_bundled_notice_file_fails(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
        source_dir = self.third_party_root / "libavif"
        source_dir.mkdir()
        (source_dir / "LICENSE").write_bytes(b"BSD-2-Clause text")
        # doc_root/libavif/ intentionally left empty -- notice never bundled.
        (self.doc_root / "libavif").mkdir()
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("missing its required non-empty", stderr)

    def test_content_drifted_bundled_notice_fails_checksum(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
        # third_party/libavif/ never created at all.
        exit_code, _stdout, stderr = self._run()
        self.assertEqual(exit_code, 1)
        self.assertIn("no checked-in notice source directory", stderr)

    def test_unmapped_bundled_library_fails_before_any_notice_check(self) -> None:
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
        (self.lib_dir / "libunexpectedvendor.so.1").write_bytes(_FAKE_ELF_BYTES)
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
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(_FAKE_ELF_BYTES)
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


class SbomInventoryTests(unittest.TestCase):
    """Review round (2 HIGH + 8 MEDIUM) item 9: the SBOM/manifest must
    never omit an allowlisted bundled library, and must carry a
    cryptographic identity (sha256 at minimum, always computable without
    any ELF toolchain) for every single entry."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.lib_dir = Path(self._tmp.name) / "lib"
        self.lib_dir.mkdir()

    def test_allowlisted_library_is_never_omitted_from_the_inventory(self) -> None:
        # Prior to this fix, classify_all()/the JSON manifest's
        # "components"/"unmapped" keys silently excluded ABI_ALLOWLIST
        # members entirely -- an SBOM consumer had no way to even know
        # libc.so.6 was bundled at all, let alone its own identity.
        (self.lib_dir / "libc.so.6").write_bytes(audit._ELF_MAGIC + b"fake libc bytes")
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(
            audit._ELF_MAGIC + b"fake libavif bytes"
        )
        inventory = audit.build_sbom_inventory(self.lib_dir)
        by_path = {entry["path"]: entry for entry in inventory}
        self.assertIn("libc.so.6", by_path)
        self.assertEqual(by_path["libc.so.6"]["classification"], "allowlisted")
        self.assertEqual(
            by_path["libc.so.6"]["sha256"],
            audit._sha256(self.lib_dir / "libc.so.6"),
        )
        self.assertEqual(by_path["libavif.so.16.0.0"]["classification"], "libavif")
        # The new canonicalLoadDigest field must be present (even
        # if None for a fake, non-parseable ELF fixture) on every entry
        # -- never a silently-missing key.
        self.assertIn("canonicalLoadDigest", by_path["libc.so.6"])

    def test_unmapped_library_still_appears_in_the_inventory(self) -> None:
        (self.lib_dir / "libmysteryvendor.so.3").write_bytes(audit._ELF_MAGIC + b"???")
        inventory = audit.build_sbom_inventory(self.lib_dir)
        by_path = {entry["path"]: entry for entry in inventory}
        self.assertEqual(
            by_path["libmysteryvendor.so.3"]["classification"], "unmapped"
        )

    def test_json_manifest_inventory_includes_every_bundled_library(self) -> None:
        (self.lib_dir / "libc.so.6").write_bytes(audit._ELF_MAGIC + b"fake libc bytes")
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(
            audit._ELF_MAGIC + b"fake libavif bytes"
        )
        (self.lib_dir / "libmysteryvendor.so.3").write_bytes(audit._ELF_MAGIC + b"???")
        json_path = Path(self._tmp.name) / "sbom.json"
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            audit.main(
                ["classify", str(self.lib_dir), "--json-out", str(json_path)]
            )
        manifest = json.loads(json_path.read_text())
        inventory_paths = {entry["path"] for entry in manifest["inventory"]}
        self.assertEqual(
            inventory_paths,
            {"libc.so.6", "libavif.so.16.0.0", "libmysteryvendor.so.3"},
        )
        # Every entry must carry (at minimum) a sha256 -- never entirely
        # bare identity, even in an environment where readelf could not
        # determine a build-id/SONAME for a non-ELF test fixture.
        for entry in manifest["inventory"]:
            self.assertIsNotNone(entry["sha256"])


def _compile_shared_object(cc_bin: str, source: str, output: Path) -> None:
    source_path = output.with_suffix(".c")
    source_path.write_text(source)
    subprocess.run(
        [cc_bin, "-shared", "-fPIC", "-o", str(output), str(source_path)],
        check=True,
        capture_output=True,
    )


# Deliberately its own tiny, independently-reviewable `readelf -lW`
# parser -- NOT a reuse/import of audit_codec_notices.py's own
# _PROGRAM_HEADER_RE/_canonical_load_digest() -- so this
# regression test's own understanding of "where is a real executable
# segment" can never coincidentally share, and therefore never mask, a
# bug in the production parser it exists to exercise.
_TEST_LOAD_HEADER_RE = re.compile(
    r"^\s*LOAD\s+(?P<offset>0x[0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+0x[0-9a-fA-F]+\s+"
    r"(?P<filesz>0x[0-9a-fA-F]+)\s+0x[0-9a-fA-F]+\s+(?P<flags>[R ][W ][E ])\s+",
    re.MULTILINE,
)


def _first_executable_load_segment(path: Path) -> tuple[int, int]:
    """Returns (file_offset, file_size) of the first PT_LOAD segment in
    `path` with the executable (E) permission bit set, parsed
    independently of production code (see the module comment above)."""
    output = subprocess.run(
        ["readelf", "-lW", str(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    for match in _TEST_LOAD_HEADER_RE.finditer(output):
        if match.group("flags")[2] == "E":
            return int(match.group("offset"), 16), int(match.group("filesz"), 16)
    raise AssertionError(f"{path} has no executable PT_LOAD segment")


def _test_section_file_range(path: Path, section_name: str) -> tuple[int, int]:
    """Returns (file_offset, file_size) of the named section, parsed via
    `readelf -SW` independently of production code (see the module
    comment above `_TEST_LOAD_HEADER_RE`)."""
    output = subprocess.run(
        ["readelf", "-SW", str(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    pattern = re.compile(
        r"\[\s*\d+\]\s+"
        + re.escape(section_name)
        + r"[ \t]+\S+[ \t]+[0-9a-fA-F]+[ \t]+"
        r"(?P<offset>[0-9a-fA-F]+)[ \t]+(?P<size>[0-9a-fA-F]+)[ \t]"
    )
    match = pattern.search(output)
    if not match:
        raise AssertionError(f"section {section_name!r} not found in {path}")
    return int(match.group("offset"), 16), int(match.group("size"), 16)


def _test_flip_first_non_executable_load_segment(path: Path) -> None:
    """Directly patches the raw ELF64 program header table (bypassing
    any tool, including patchelf) to flip on the PF_X bit of the first
    PT_LOAD segment that does not already have it set -- this simulates
    a "segment header mutation" attack that never touches any section's
    own content or declared sh_flags at all, which only a check of the
    section's actual runtime LOAD-segment mapping can catch."""
    data = bytearray(path.read_bytes())
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    PT_LOAD = 1
    PF_X = 1
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type != PT_LOAD:
            continue
        flags_off = entry_off + 4
        p_flags = struct.unpack_from("<I", data, flags_off)[0]
        if p_flags & PF_X:
            continue
        struct.pack_into("<I", data, flags_off, p_flags | PF_X)
        path.write_bytes(bytes(data))
        return
    raise AssertionError(f"{path} has no non-executable PT_LOAD segment to mutate")


@unittest.skipUnless(
    shutil.which("cc") or shutil.which("gcc"),
    "requires a real C compiler to build genuine ELF fixtures",
)
@unittest.skipUnless(
    shutil.which("readelf"), "requires readelf (binutils) to read build-id/SONAME"
)
class QtPluginContentProvenanceTests(unittest.TestCase):
    """Review round (2 HIGH + 8 MEDIUM) item 9's core regression: a Qt
    plugin classification that only ever checked relative-path existence
    against the reference Qt SDK, never the bundled file's own content,
    would wrongly accept an attacker-substituted binary placed at the
    exact same path with the exact same name. These tests use genuine,
    compiled ELF shared objects (unlike the rest of this module's
    basename-only fake-file tests) since a real build-id/content
    comparison is precisely the behavior under test."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.cc_bin = shutil.which("cc") or shutil.which("gcc")

    def test_genuine_unmodified_qt_plugin_still_classifies_as_qt(self) -> None:
        qt_root = self.root / "qt_sdk"
        (qt_root / "plugins" / "imageformats").mkdir(parents=True)
        reference = qt_root / "plugins" / "imageformats" / "libqjpeg.so"
        _compile_shared_object(
            self.cc_bin, "int test_plugin_entry(void) { return 1; }\n", reference
        )

        bundled_dir = self.root / "appdir" / "plugins" / "imageformats"
        bundled_dir.mkdir(parents=True)
        bundled = bundled_dir / "libqjpeg.so"
        # An exact copy models the common case of an unmodified bundled
        # plugin (or one only rewritten in ways that happen to preserve
        # every byte, e.g. a no-op patchelf pass); the build-id-based
        # comparison and the sha256 fallback both agree here.
        bundled.write_bytes(reference.read_bytes())

        self.assertEqual(
            audit.classify_path(bundled, qt_root),
            "qt",
        )

    def test_substituted_qt_plugin_at_the_same_path_is_not_classified_as_qt(
        self,
    ) -> None:
        # The exact attack this finding is about: a different compiled
        # object (different source, different build-id, different
        # bytes) is placed at the identical relative sub-path and
        # basename a genuine Qt plugin would occupy.
        qt_root = self.root / "qt_sdk"
        (qt_root / "plugins" / "imageformats").mkdir(parents=True)
        reference = qt_root / "plugins" / "imageformats" / "libqjpeg.so"
        _compile_shared_object(
            self.cc_bin, "int test_plugin_entry(void) { return 1; }\n", reference
        )

        bundled_dir = self.root / "appdir" / "plugins" / "imageformats"
        bundled_dir.mkdir(parents=True)
        substituted = bundled_dir / "libqjpeg.so"
        _compile_shared_object(
            self.cc_bin,
            "int test_plugin_entry(void) { return 0xDEAD; }\n"
            "int extra_unexpected_symbol(void) { return 2; }\n",
            substituted,
        )
        # Sanity: really is different content, not an accidental copy.
        self.assertNotEqual(
            audit._sha256(reference), audit._sha256(substituted)
        )

        self.assertIsNone(
            audit.classify_path(substituted, qt_root),
        )

    def test_build_id_survives_a_patchelf_style_rpath_edit(self) -> None:
        # Approximates linuxdeploy's own patchelf RUNPATH-rewriting step
        # (which this classifier's provenance check must tolerate,
        # per this finding's own design constraint) by compiling the
        # SAME source twice with different RPATH values -- an
        # RPATH/RUNPATH difference alone must never make a genuinely
        # unmodified plugin look "substituted".
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        qt_root = self.root / "qt_sdk"
        (qt_root / "plugins" / "imageformats").mkdir(parents=True)
        reference = qt_root / "plugins" / "imageformats" / "libqjpeg.so"
        _compile_shared_object(
            self.cc_bin, "int test_plugin_entry(void) { return 1; }\n", reference
        )

        bundled_dir = self.root / "appdir" / "plugins" / "imageformats"
        bundled_dir.mkdir(parents=True)
        bundled = bundled_dir / "libqjpeg.so"
        bundled.write_bytes(reference.read_bytes())
        bundled.chmod(0o755)
        subprocess.run(
            ["patchelf", "--set-rpath", "$ORIGIN/../..", str(bundled)],
            check=True,
            capture_output=True,
        )
        # The patchelf rewrite legitimately changed the bytes...
        self.assertNotEqual(audit._sha256(reference), audit._sha256(bundled))
        # ...but the build-id (and therefore this classifier's own
        # verdict) must be unaffected by it.
        self.assertEqual(
            audit._read_build_id(reference), audit._read_build_id(bundled)
        )
        self.assertEqual(audit.classify_path(bundled, qt_root), "qt")

    def test_code_mutation_preserving_build_id_note_is_rejected(self) -> None:
        # Round-N+ review (HIGH, "Build ID not signature"): this is the
        # exact attack a bare build-id comparison cannot detect -- a
        # single byte inside a real, mapped EXECUTABLE PT_LOAD segment
        # is flipped in place (a stand-in for any tool/attacker capable
        # of patching compiled code after linking) while the
        # `.note.gnu.build-id` section itself is never touched at all,
        # so it stays byte-identical to the reference. The OLD
        # build-id-only check would have wrongly classified this as
        # "qt" (same compiled object); the fixed executable-segment
        # digest must reject it.
        qt_root = self.root / "qt_sdk"
        (qt_root / "plugins" / "imageformats").mkdir(parents=True)
        reference = qt_root / "plugins" / "imageformats" / "libqjpeg.so"
        _compile_shared_object(
            self.cc_bin,
            "int test_plugin_entry(void) { return 1; }\n"
            "int another_function(int x) { return x * 2 + 1; }\n",
            reference,
        )

        bundled_dir = self.root / "appdir" / "plugins" / "imageformats"
        bundled_dir.mkdir(parents=True)
        mutated = bundled_dir / "libqjpeg.so"
        mutated.write_bytes(reference.read_bytes())

        offset, size = _first_executable_load_segment(reference)
        self.assertGreater(size, 0)
        data = bytearray(mutated.read_bytes())
        # Flip one bit near the middle of the mapped executable range --
        # anywhere inside [offset, offset + size) is real, mapped,
        # executable code, never section-table/relocation metadata.
        mutate_at = offset + size // 2
        data[mutate_at] ^= 0xFF
        mutated.write_bytes(bytes(data))

        # Sanity: the mutation really did change the bytes...
        self.assertNotEqual(audit._sha256(reference), audit._sha256(mutated))
        # ...while leaving the stored build-id note completely
        # untouched -- exactly the scenario a build-id-only check could
        # never catch.
        self.assertEqual(
            audit._read_build_id(reference), audit._read_build_id(mutated)
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )
        self.assertIsNone(audit.classify_path(mutated, qt_root))

    def test_canonical_load_digest_is_stable_across_patchelf_rpath_edit(
        self,
    ) -> None:
        # The flip side of the mutation test above: a real patchelf
        # RUNPATH rewrite (which only ever touches .dynamic/.dynstr,
        # never a mapped executable segment) must NOT change the
        # executable-segment digest, or every genuinely-repackaged Qt
        # plugin would start failing this check.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        reference = self.root / "reference.so"
        _compile_shared_object(
            self.cc_bin, "int test_plugin_entry(void) { return 1; }\n", reference
        )
        patched = self.root / "patched.so"
        patched.write_bytes(reference.read_bytes())
        patched.chmod(0o755)
        subprocess.run(
            ["patchelf", "--set-rpath", "$ORIGIN/../..", str(patched)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(patched))
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(patched),
        )

    def test_canonical_load_digest_none_when_no_loaded_section(
        self,
    ) -> None:
        # A plain, non-ELF file (or one whose readelf output has no
        # loaded (SHF_ALLOC) section at all) must return None, not raise
        # or silently produce a hash over unrelated bytes.
        not_elf = self.root / "not-a-real-elf.so"
        not_elf.write_bytes(b"\x00" * 64)
        self.assertIsNone(audit._canonical_load_digest(not_elf))

    def _build_rich_shared_object(self, output: Path) -> None:
        """A single small `.so` deliberately exercising every ELF
        feature this round's findings are concerned with: a real
        `DT_NEEDED` (libc), an `.init_array` constructor entry, real
        `.got`/`.got.plt` relocations (via malloc/free calls), a
        non-empty `.bss` (an uninitialized global -- see
        test_canonical_load_digest_is_stable_across_a_bss_bearing_
        patchelf_relocation()'s own docstring for why this specific,
        real cumulative-review regression required one), and `.rodata`
        content -- so every mutation test below exercises a genuine,
        realistic binary rather than a degenerate fixture."""
        source = (
            "#include <stdlib.h>\n"
            'static const char message[] = "canonical-load-digest-fixture";\n'
            "static char scratch_buffer[4096];\n"
            "__attribute__((constructor)) static void init(void) {\n"
            "    void *p = malloc(16);\n"
            "    scratch_buffer[0] = (char)(size_t)p;\n"
            "    free(p);\n"
            "}\n"
            "const char *test_plugin_entry(void) { return message; }\n"
        )
        _compile_shared_object(self.cc_bin, source, output)

    def test_canonical_load_digest_is_stable_across_patchelf_rpath_relocation(
        self,
    ) -> None:
        # Harder than the existing short-rpath stability test above:
        # this project's own empirical testing found that patchelf
        # (0.14.3) must PHYSICALLY RELOCATE `.dynstr` (and whatever
        # else was co-resident with it, e.g. `.gnu.hash`/`.note.*`)
        # into a brand-new, appended PT_LOAD segment when the new
        # RPATH string no longer fits in the original slack -- a
        # materially different code path than an in-place rewrite, and
        # the one that actually exercises _resolve_dynamic_tag_value(),
        # _read_dynamic_symbols()' index-renumbering tolerance, and the
        # execute-bit-only (not full R/W/E) segment-flag comparison.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        reference = self.root / "reference_reloc.so"
        self._build_rich_shared_object(reference)
        patched = self.root / "patched_reloc.so"
        patched.write_bytes(reference.read_bytes())
        patched.chmod(0o755)
        long_rpath = "/".join(["deliberately-very-long-rpath-component"] * 40)
        subprocess.run(
            ["patchelf", "--set-rpath", long_rpath, str(patched)],
            check=True,
            capture_output=True,
        )
        # Sanity: the RPATH rewrite really did force new segments to
        # appear (proving the relocation path, not just an in-place
        # rewrite, was actually exercised).
        load_count_before = len(
            _TEST_LOAD_HEADER_RE.findall(
                subprocess.run(
                    ["readelf", "-lW", str(reference)],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout
            )
        )
        load_count_after = len(
            _TEST_LOAD_HEADER_RE.findall(
                subprocess.run(
                    ["readelf", "-lW", str(patched)],
                    check=True,
                    capture_output=True,
                    text=True,
                ).stdout
            )
        )
        self.assertGreater(load_count_after, load_count_before)
        self.assertNotEqual(audit._sha256(reference), audit._sha256(patched))
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(patched),
        )

    def test_canonical_load_digest_is_stable_across_a_bss_bearing_patchelf_relocation(
        self,
    ) -> None:
        # Real cumulative-review regression, found only against this
        # project's own actual produced AppImage (never against this
        # module's own prior synthetic fixtures, none of which had a
        # non-empty `.bss`): a SHT_NOBITS section such as `.bss` has NO
        # real file content at all -- its own sh_offset is merely
        # wherever a LATER, real section happens to start, not a
        # pointer to reserved storage for `.bss` itself. Two genuinely
        # bundled Qt files in the real AppImage build
        # (plugins/tls/libqopensslbackend.so and
        # qml/.../FluentWinUI3/libqtquickcontrols2fluentwinui3
        # styleplugin.so) were wrongly reported unmapped/substituted
        # because patchelf's RPATH-relocation path (see the previous
        # test) moved whichever real section used to occupy the file
        # bytes at `.bss`'s own sh_offset, changing the ACCIDENTAL
        # bytes previously (and wrongly) read as `.bss`'s own
        # "content" even though `.bss`, having no content, could not
        # itself have changed at all.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        reference = self.root / "reference_bss.so"
        self._build_rich_shared_object(reference)
        patched = self.root / "patched_bss.so"
        patched.write_bytes(reference.read_bytes())
        patched.chmod(0o755)
        long_rpath = "/".join(["deliberately-very-long-rpath-component"] * 40)
        subprocess.run(
            ["patchelf", "--set-rpath", long_rpath, str(patched)],
            check=True,
            capture_output=True,
        )
        # Sanity: a real, non-empty `.bss` genuinely exists in both
        # files (the fixture's whole reason for existing), and the
        # RPATH rewrite really did relocate other sections around it
        # (proving this test actually exercises the reported bug's
        # exact precondition, not merely an in-place, same-offset
        # rewrite that would never have exposed it).
        bss_offset_before, bss_size = _test_section_file_range(reference, ".bss")
        bss_offset_after, bss_size_after = _test_section_file_range(patched, ".bss")
        self.assertGreater(bss_size, 0)
        self.assertEqual(bss_size, bss_size_after)
        # The actual regression proof: independently (using this test's
        # own reader, never production code) read the raw bytes that
        # USED to be wrongly hashed as ".bss content" at its own
        # sh_offset in each file. These must genuinely DIFFER here --
        # proving that a digest implementation which naively read
        # `.bss`'s declared size at its declared file offset (the
        # actual, now-fixed bug) would have produced two DIFFERENT
        # hashes for this entirely legitimate, content-preserving
        # repack, i.e. would have wrongly rejected it exactly as the
        # real AppImage build did.
        with reference.open("rb") as handle:
            handle.seek(bss_offset_before)
            stale_bytes_before = handle.read(bss_size)
        with patched.open("rb") as handle:
            handle.seek(bss_offset_after)
            stale_bytes_after = handle.read(bss_size)
        self.assertNotEqual(
            stale_bytes_before,
            stale_bytes_after,
            "fixture did not actually relocate file content around .bss's own "
            "sh_offset -- this test would not have caught the reported bug",
        )
        # The real assertion: the fixed, production canonical digest
        # -- which must never read `.bss`'s nonexistent "content" at
        # all -- is stable across this legitimate repack, exactly as a
        # genuinely unmodified, merely repackaged Qt plugin must be.
        self.assertNotEqual(audit._sha256(reference), audit._sha256(patched))
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(patched),
        )

    def test_canonical_load_digest_detects_rodata_mutation(self) -> None:
        reference = self.root / "rodata_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "rodata_mut.so"
        mutated.write_bytes(reference.read_bytes())
        offset, size = _test_section_file_range(reference, ".rodata")
        self.assertGreater(size, 0)
        data = bytearray(mutated.read_bytes())
        data[offset] ^= 0xFF
        mutated.write_bytes(bytes(data))
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_init_array_mutation(self) -> None:
        reference = self.root / "initarray_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "initarray_mut.so"
        mutated.write_bytes(reference.read_bytes())
        offset, size = _test_section_file_range(reference, ".init_array")
        self.assertGreater(size, 0)
        data = bytearray(mutated.read_bytes())
        data[offset] ^= 0xFF
        mutated.write_bytes(bytes(data))
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_got_mutation(self) -> None:
        reference = self.root / "got_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "got_mut.so"
        mutated.write_bytes(reference.read_bytes())
        section_name = ".got.plt" if b".got.plt" in reference.read_bytes() else ".got"
        offset, size = _test_section_file_range(reference, section_name)
        self.assertGreater(size, 0)
        data = bytearray(mutated.read_bytes())
        data[offset] ^= 0xFF
        mutated.write_bytes(bytes(data))
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_dynamic_metadata_mutation(
        self,
    ) -> None:
        # A redirected DT_NEEDED SONAME is exactly the kind of dynamic
        # -linking directive tampering that a wholesale-excluded
        # `.dynamic` byte-range hash would miss entirely (since
        # `.dynamic`'s raw bytes are excluded due to legitimate
        # relocation); only the decoded-tag folding catches it.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to rewrite DT_NEEDED")
        reference = self.root / "needed_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "needed_mut.so"
        mutated.write_bytes(reference.read_bytes())
        mutated.chmod(0o755)
        subprocess.run(
            ["patchelf", "--replace-needed", "libc.so.6", "libc.so.99", str(mutated)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_segment_header_mutation(
        self,
    ) -> None:
        # An attacker flips a PT_LOAD program header's own PF_X bit to
        # make a previously non-executable segment (e.g. one holding
        # `.rodata`) executable, without touching any section's content
        # or its own, independent sh_flags field at all. Only a check
        # of the section's actual runtime LOAD-segment mapping (not
        # merely its content bytes or declared sh_flags) can catch
        # this -- exactly what _canonical_load_digest()'s
        # execute-bit-only segment check exists for.
        reference = self.root / "segflip_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "segflip_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_flip_first_non_executable_load_segment(mutated)
        # Sanity: no section's own content or declared sh_flags moved
        # at all; only the program-header table changed.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )



class PackageProvenanceTests(unittest.TestCase):
    """Round-N+ review (MEDIUM, "libcom_err provenance/version inferred
    basename; CI apt unpinned, notice hardcodes Jammy 1.46.5-2ubuntu1.1
    while updates may .2"): portable (no real dpkg needed --
    _dpkg_owning_package()/_dpkg_package_metadata() are mocked directly)
    tests of capture_package_provenance()/
    validate_component_package_provenance()'s own logic."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.search_dir = Path(self._tmp.name)

    def test_capture_returns_none_when_no_system_copy_exists_anywhere(
        self,
    ) -> None:
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (self.search_dir,)
        ):
            self.assertIsNone(
                audit.capture_package_provenance("libnothing-like-this.so.1")
            )

    def test_capture_returns_none_when_dpkg_does_not_own_the_found_file(
        self,
    ) -> None:
        (self.search_dir / "libfoo.so.1").write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (self.search_dir,)
        ), mock.patch.object(audit, "_dpkg_owning_package", return_value=None):
            self.assertIsNone(audit.capture_package_provenance("libfoo.so.1"))

    def test_capture_returns_full_identity_when_a_real_system_copy_is_dpkg_owned(
        self,
    ) -> None:
        (self.search_dir / "libfoo.so.1").write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (self.search_dir,)
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit,
            "_dpkg_package_metadata",
            return_value=("1.2.3-1ubuntu1", "foosource"),
        ):
            self.assertEqual(
                audit.capture_package_provenance("libfoo.so.1"),
                {
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                },
            )

    def test_validate_accepts_matching_expected_source_package(self) -> None:
        self.assertIsNone(
            audit.validate_component_package_provenance(
                "e2fsprogs",
                {
                    "package": "libcom-err2",
                    "version": "1.46.5-2ubuntu1.2",
                    "sourcePackage": "e2fsprogs",
                },
            )
        )

    def test_validate_rejects_wrong_source_package(self) -> None:
        # The exact regression this finding is about: a captured
        # provenance record whose real source package does not match
        # what the component claims (e.g. a future conflation bug
        # re-introducing "libcom_err is part of krb5") must fail
        # outright, not silently pass.
        problem = audit.validate_component_package_provenance(
            "e2fsprogs",
            {
                "package": "libkrb5-3",
                "version": "1.19.2-2ubuntu0.8",
                "sourcePackage": "krb5",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)
        self.assertIn("krb5", problem)

    def test_validate_is_a_silent_no_op_for_a_component_with_no_mapping(
        self,
    ) -> None:
        # qt/qtkeychain (and any future component this project cannot
        # currently make a real claim about) must never be
        # spuriously flagged merely for lacking a mapping entry.
        self.assertIsNone(
            audit.validate_component_package_provenance(
                "not-a-real-component",
                {
                    "package": "anything",
                    "version": "0",
                    "sourcePackage": "anything-at-all",
                },
            )
        )

    def test_cmd_classify_fails_when_real_provenance_disagrees_with_component(
        self,
    ) -> None:
        # End-to-end wiring test: classify() itself must fail closed
        # when a bundled library's real captured provenance disagrees
        # with its COMPONENT_PATTERNS mapping, even though the fixture
        # file here is only a fake-magic-bytes stand-in (matching this
        # module's own established pure-basename-classification testing
        # convention -- see the top-of-file docstring).
        lib_dir = Path(self._tmp.name) / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        def fake_bind(bundled_path: Path) -> dict[str, object]:
            if bundled_path.name == "libcom_err.so.2":
                return {
                    "status": "matched",
                    "package": "libkrb5-3",
                    "version": "1.19.2-2ubuntu0.8",
                    "sourcePackage": "krb5",
                }
            return {"status": "dpkg_unavailable"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit, "bind_bundled_library_to_system_provenance", side_effect=fake_bind
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(["classify", str(lib_dir)])
        self.assertNotEqual(exit_code, 0)
        self.assertIn("e2fsprogs", stderr.getvalue())
        self.assertIn("krb5", stderr.getvalue())

    def test_cmd_classify_succeeds_when_provenance_agrees_or_is_unavailable(
        self,
    ) -> None:
        lib_dir = Path(self._tmp.name) / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        def fake_bind(bundled_path: Path) -> dict[str, object]:
            if bundled_path.name == "libcom_err.so.2":
                return {
                    "status": "matched",
                    "package": "libcom-err2",
                    "version": "1.46.5-2ubuntu1.2",
                    "sourcePackage": "e2fsprogs",
                }
            return {"status": "dpkg_unavailable"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit, "bind_bundled_library_to_system_provenance", side_effect=fake_bind
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(["classify", str(lib_dir)])
        self.assertEqual(exit_code, 0, stderr.getvalue())

    def test_cmd_classify_fails_when_expected_distro_component_provenance_not_found(
        self,
    ) -> None:
        # Round-7 review (HIGH, "missing provenance passes"): with
        # --require-package-provenance passed (this project's own
        # governed-expectations opt-in for a host/CI-job where every
        # expected distro component's system counterpart genuinely IS
        # installed -- see validate_bundled_library_package_provenance()'s
        # own docstring), an expected distro component whose provenance
        # simply cannot be found at all must fail closed -- the OLD
        # behavior (capture_package_provenance() returning None) was
        # always silently skipped, never reported, even here.
        lib_dir = Path(self._tmp.name) / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        def fake_bind(bundled_path: Path) -> dict[str, object]:
            if bundled_path.name == "libcom_err.so.2":
                return {"status": "not_found"}
            return {"status": "dpkg_unavailable"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit, "bind_bundled_library_to_system_provenance", side_effect=fake_bind
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                ["classify", str(lib_dir), "--require-package-provenance"]
            )
        self.assertNotEqual(exit_code, 0)
        self.assertIn("e2fsprogs", stderr.getvalue())

    def test_cmd_classify_succeeds_when_provenance_not_found_and_not_required(
        self,
    ) -> None:
        # The flip side: without --require-package-provenance, the same
        # "not_found" outcome must remain a legitimate, harmless skip
        # (preserving this project's existing, portable basename-only
        # classification test fixtures' behavior on any host, dpkg
        # -equipped or not).
        lib_dir = Path(self._tmp.name) / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        def fake_bind(bundled_path: Path) -> dict[str, object]:
            return {"status": "not_found"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit, "bind_bundled_library_to_system_provenance", side_effect=fake_bind
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(["classify", str(lib_dir)])
        self.assertEqual(exit_code, 0, stderr.getvalue())

    def test_bind_returns_dpkg_unavailable_when_dpkg_tools_missing(self) -> None:
        with mock.patch.object(shutil, "which", return_value=None):
            binding = audit.bind_bundled_library_to_system_provenance(
                self.search_dir / "libfoo.so.1"
            )
        self.assertEqual(binding, {"status": "dpkg_unavailable"})

    def test_bind_returns_not_found_when_no_system_copy_exists(self) -> None:
        bundled = self.search_dir / "bundled"
        bundled.mkdir()
        bundled_lib = bundled / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (self.search_dir / "system",)
        ), mock.patch.object(shutil, "which", return_value="/usr/bin/dpkg"):
            binding = audit.bind_bundled_library_to_system_provenance(bundled_lib)
        self.assertEqual(binding, {"status": "not_found"})

    def test_bind_returns_not_dpkg_owned_when_system_copy_is_unowned(self) -> None:
        system_dir = self.search_dir / "system"
        system_dir.mkdir()
        (system_dir / "libfoo.so.1").write_bytes(b"\x00")
        bundled_dir = self.search_dir / "bundled"
        bundled_dir.mkdir()
        bundled_lib = bundled_dir / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (system_dir,)
        ), mock.patch.object(
            shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(audit, "_dpkg_owning_package", return_value=None):
            binding = audit.bind_bundled_library_to_system_provenance(bundled_lib)
        self.assertEqual(binding["status"], "not_dpkg_owned")

    def test_bind_returns_content_mismatch_for_a_substituted_same_basename_library(
        self,
    ) -> None:
        # The exact "unrelated host file" attack this finding is about:
        # a same-basename system file exists and IS dpkg-owned, but its
        # actual compiled content differs from the bundled file -- a
        # bare basename-only lookup would wrongly "match" this.
        system_dir = self.search_dir / "system"
        system_dir.mkdir()
        (system_dir / "libfoo.so.1").write_bytes(b"\x00")
        bundled_dir = self.search_dir / "bundled"
        bundled_dir.mkdir()
        bundled_lib = bundled_dir / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (system_dir,)
        ), mock.patch.object(
            shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit,
            "_dpkg_package_metadata",
            return_value=("1.2.3-1ubuntu1", "foosource"),
        ), mock.patch.object(
            audit,
            "_canonical_load_digest",
            side_effect=lambda p: "digest-a" if p == bundled_lib else "digest-b",
        ):
            binding = audit.bind_bundled_library_to_system_provenance(bundled_lib)
        self.assertEqual(binding["status"], "content_mismatch")
        self.assertEqual(binding["sourcePackage"], "foosource")

    def test_bind_returns_matched_when_content_digests_agree(self) -> None:
        system_dir = self.search_dir / "system"
        system_dir.mkdir()
        (system_dir / "libfoo.so.1").write_bytes(b"\x00")
        bundled_dir = self.search_dir / "bundled"
        bundled_dir.mkdir()
        bundled_lib = bundled_dir / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (system_dir,)
        ), mock.patch.object(
            shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit,
            "_dpkg_package_metadata",
            return_value=("1.2.3-1ubuntu1", "foosource"),
        ), mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ):
            binding = audit.bind_bundled_library_to_system_provenance(bundled_lib)
        self.assertEqual(
            binding,
            {
                "status": "matched",
                "package": "libfoo1",
                "version": "1.2.3-1ubuntu1",
                "sourcePackage": "foosource",
            },
        )

    def test_validate_bundled_provenance_accepts_dpkg_unavailable_as_no_op(
        self,
    ) -> None:
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs", {"status": "dpkg_unavailable"}
            )
        )

    def test_validate_bundled_provenance_no_op_for_unmapped_component(self) -> None:
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "not-a-real-component", {"status": "not_found"}
            )
        )

    def test_validate_bundled_provenance_rejects_not_found(self) -> None:
        # "not_found" is only a hard failure when require_provenance=True
        # (governed expectations -- see this function's own docstring);
        # a merely dpkg-equipped but incomplete host must not spuriously
        # fail here without that explicit opt-in.
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs", {"status": "not_found"}
            )
        )
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs", {"status": "not_found"}, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)

    def test_validate_bundled_provenance_rejects_not_dpkg_owned(self) -> None:
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs", {"status": "not_dpkg_owned"}
            )
        )
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs", {"status": "not_dpkg_owned"}, require_provenance=True
        )
        self.assertIsNotNone(problem)

    def test_validate_bundled_provenance_rejects_content_mismatch(self) -> None:
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "content_mismatch",
                "package": "libcom-err2",
                "version": "1.46.5-2ubuntu1.2",
                "sourcePackage": "e2fsprogs",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)

    def test_validate_bundled_provenance_rejects_version_revision_drift(
        self,
    ) -> None:
        # Round-7 review ("pin package revision/snapshot"): even a
        # matched, content-proven, correctly-source-packaged provenance
        # must still fail if its real installed version has drifted
        # away from the pinned COMPONENT_EXPECTED_SOURCE_VERSION_PREFIX
        # major/minor baseline -- a genuine upstream revision change,
        # not a routine Debian point-release bump.
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": "1.47.0-1ubuntu1",
                "sourcePackage": "e2fsprogs",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)

    def test_validate_bundled_provenance_tolerates_routine_point_release(
        self,
    ) -> None:
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs",
                {
                    "status": "matched",
                    "package": "libcom-err2",
                    "version": "1.46.5-2ubuntu1.9",
                    "sourcePackage": "e2fsprogs",
                },
            )
        )


class QtSdkBundledProvenanceTests(unittest.TestCase):
    """New review item ("ICU library package-provenance mismatch",
    found only once the AppImage-smoke job's "Verify every bundled ELF
    library ships its required license notice" step finally ran to
    completion for the first time against the real produced AppImage on
    the real pinned `ubuntu-22.04` CI runner): "icu" is bundled directly
    from the Qt SDK's own `lib/` directory in this project's actual
    pipeline, never from a dpkg-owned distro package (Ubuntu 22.04 ships
    ICU 70, never the ICU 73 Qt 6.11.1 bundles) -- so its provenance is
    authenticated against a real Qt SDK reference copy instead. These
    tests cover bind_bundled_library_to_qt_sdk_provenance()/
    validate_bundled_library_qt_sdk_provenance() directly (mirroring
    PackageProvenanceTests' own dpkg-based coverage above), plus the
    end-to-end `classify` CLI wiring that must route "icu" through this
    mechanism instead of the dpkg one."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def test_icu_is_not_in_component_expected_source_packages(self) -> None:
        # "icu" must never be validated via the dpkg cross-check
        # mechanism at all -- see _COMPONENTS_WITH_QT_SDK_BUNDLED_
        # PROVENANCE's own docstring for why that check can structurally
        # never succeed for it on this project's real pinned CI runner.
        self.assertNotIn("icu", audit.COMPONENT_EXPECTED_SOURCE_PACKAGES)
        self.assertIn("icu", audit._COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE)

    def test_bind_qt_sdk_returns_unavailable_when_no_reference_dir_supplied(
        self,
    ) -> None:
        bundled = self.root / "libicudata.so.73"
        bundled.write_bytes(_FAKE_ELF_BYTES)
        self.assertEqual(
            audit.bind_bundled_library_to_qt_sdk_provenance(bundled, None),
            {"status": "qt_reference_dir_unavailable"},
        )

    def test_bind_qt_sdk_returns_not_found_when_reference_copy_absent(
        self,
    ) -> None:
        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        bundled_dir = self.root / "bundled"
        bundled_dir.mkdir()
        bundled = bundled_dir / "libicudata.so.73"
        bundled.write_bytes(_FAKE_ELF_BYTES)
        binding = audit.bind_bundled_library_to_qt_sdk_provenance(
            bundled, qt_reference_dir
        )
        self.assertEqual(binding["status"], "not_found")
        self.assertIn("libicudata.so.73", str(binding["referencePath"]))

    def test_bind_qt_sdk_returns_content_mismatch_for_a_substituted_library(
        self,
    ) -> None:
        # The exact "unrelated/substituted file" scenario this check
        # exists to catch: a reference copy exists at the expected path
        # but is NOT the same compiled object as the bundled file.
        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(b"\x00reference")
        bundled_dir = self.root / "bundled"
        bundled_dir.mkdir()
        bundled = bundled_dir / "libicudata.so.73"
        bundled.write_bytes(b"\x00tampered-different-content")
        with mock.patch.object(
            audit, "_canonical_load_digest", return_value=None
        ):
            binding = audit.bind_bundled_library_to_qt_sdk_provenance(
                bundled, qt_reference_dir
            )
        self.assertEqual(binding["status"], "content_mismatch")

    def test_bind_qt_sdk_returns_matched_when_content_is_proven_identical(
        self,
    ) -> None:
        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(b"\x00identical")
        bundled_dir = self.root / "bundled"
        bundled_dir.mkdir()
        bundled = bundled_dir / "libicudata.so.73"
        bundled.write_bytes(b"\x00identical")
        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ):
            binding = audit.bind_bundled_library_to_qt_sdk_provenance(
                bundled, qt_reference_dir
            )
        self.assertEqual(binding["status"], "matched")

    def test_validate_qt_sdk_provenance_no_op_when_reference_dir_unavailable(
        self,
    ) -> None:
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance(
                "icu", {"status": "qt_reference_dir_unavailable"}, True
            )
        )

    def test_validate_qt_sdk_provenance_rejects_not_found_only_when_required(
        self,
    ) -> None:
        binding = {"status": "not_found", "referencePath": "/qt/lib/libicudata.so.73"}
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance("icu", binding)
        )
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "icu", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("icu", problem)

    def test_validate_qt_sdk_provenance_rejects_content_mismatch_unconditionally(
        self,
    ) -> None:
        binding = {
            "status": "content_mismatch",
            "referencePath": "/qt/lib/libicudata.so.73",
        }
        problem = audit.validate_bundled_library_qt_sdk_provenance("icu", binding)
        self.assertIsNotNone(problem)
        self.assertIn("icu", problem)

    def test_validate_qt_sdk_provenance_accepts_matched(self) -> None:
        binding = {"status": "matched", "referencePath": "/qt/lib/libicudata.so.73"}
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance(
                "icu", binding, require_provenance=True
            )
        )

    def test_cmd_classify_uses_qt_sdk_provenance_for_icu_never_dpkg(self) -> None:
        # End-to-end regression test for the actual CI failure: on a
        # real dpkg-equipped host with --require-package-provenance
        # passed (exactly this project's pinned `ubuntu-22.04`
        # `appimage-smoke` job), the real system ICU package is a
        # DIFFERENT version (70) than what Qt 6.11.1 bundles (73), so
        # bind_bundled_library_to_system_provenance() would always
        # report "not_found" for "libicudata.so.73" -- classify() must
        # never even consult that dpkg-based path for "icu" at all, and
        # must instead succeed via the Qt SDK reference copy.
        lib_dir = self.root / "lib"
        lib_dir.mkdir()
        (lib_dir / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)

        def fail_if_called_for_icu(bundled_path: Path) -> dict[str, object]:
            if bundled_path.name == "libicudata.so.73":
                self.fail(
                    "bind_bundled_library_to_system_provenance() must never "
                    "be consulted for the 'icu' component at all -- see "
                    "_COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE's own "
                    "docstring"
                )
            return {"status": "dpkg_unavailable"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_system_provenance",
            side_effect=fail_if_called_for_icu,
        ), mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--qt-reference-dir",
                    str(qt_reference_dir),
                    "--require-package-provenance",
                ]
            )
        self.assertEqual(exit_code, 0, stderr.getvalue())

    def test_cmd_classify_fails_when_icu_qt_sdk_reference_copy_not_found(
        self,
    ) -> None:
        # The flip side of the above: with --require-package-provenance
        # passed and a --qt-reference-dir supplied, an "icu" library
        # with NO matching reference copy under the Qt SDK must still
        # fail closed, never silently pass.
        lib_dir = self.root / "lib"
        lib_dir.mkdir()
        (lib_dir / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        # Deliberately no libicudata.so.73 placed under qt_reference_dir.

        def fail_if_called_for_icu(bundled_path: Path) -> dict[str, object]:
            if bundled_path.name == "libicudata.so.73":
                self.fail(
                    "bind_bundled_library_to_system_provenance() must never "
                    "be consulted for the 'icu' component at all"
                )
            return {"status": "dpkg_unavailable"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_system_provenance",
            side_effect=fail_if_called_for_icu,
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--qt-reference-dir",
                    str(qt_reference_dir),
                    "--require-package-provenance",
                ]
            )
        self.assertNotEqual(exit_code, 0)
        self.assertIn("icu", stderr.getvalue())

    def test_sbom_inventory_records_qt_sdk_provenance_for_icu(self) -> None:
        lib_dir = self.root / "lib"
        lib_dir.mkdir()
        (lib_dir / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)

        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)

        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ), mock.patch.object(
            audit, "capture_package_provenance", return_value=None
        ):
            inventory = audit.build_sbom_inventory(lib_dir, qt_reference_dir)
        entry = next(
            e for e in inventory if e["basename"] == "libicudata.so.73"
        )
        self.assertEqual(entry["classification"], "icu")
        self.assertIn("qtSdkProvenance", entry)
        self.assertEqual(entry["qtSdkProvenance"]["status"], "matched")


@unittest.skipUnless(
    shutil.which("dpkg") and shutil.which("dpkg-query"),
    "requires a real Debian/Ubuntu dpkg database to authenticate a genuine "
    "system library",
)
class RealSystemPackageProvenanceTests(unittest.TestCase):
    """Round-N+ review (MEDIUM, package provenance): the same mechanism
    tested with fully mocked dpkg above, proven here against a REAL
    dpkg database. libz.so.1 (from zlib1g) is used because it is one of
    the small set of packages every Debian/Ubuntu system -- including a
    bare, freshly-created container -- always already has installed (it
    is a transitive dependency of dpkg/apt/bash themselves), so this
    test needs no network access or `apt-get install` of its own to be
    hermetic; it is still skipped outright (never silently vacuous) on
    any non-Debian host via the class-level skip above."""

    def test_real_system_library_matches_its_expected_component(self) -> None:
        provenance = audit.capture_package_provenance("libz.so.1")
        if provenance is None:
            self.skipTest(
                "no real libz.so.1 system copy found under "
                "_SYSTEM_LIBRARY_SEARCH_DIRS on this host"
            )
        self.assertIsNone(
            audit.validate_component_package_provenance("zlib", provenance)
        )

    def test_real_system_library_rejects_a_wrong_expected_component(self) -> None:
        provenance = audit.capture_package_provenance("libz.so.1")
        if provenance is None:
            self.skipTest(
                "no real libz.so.1 system copy found under "
                "_SYSTEM_LIBRARY_SEARCH_DIRS on this host"
            )
        problem = audit.validate_component_package_provenance("krb5", provenance)
        self.assertIsNotNone(problem)
        self.assertIn("krb5", problem)

    def test_every_expected_source_package_agrees_with_whatever_is_really_installed(
        self,
    ) -> None:
        """Round-N+ review regression: a previous version of this table
        mapped component "zstd" to expected Debian source package
        "zstd", but Ubuntu 22.04's real libzstd1 binary package is
        actually built from a source package literally named "libzstd"
        -- there is no bare "zstd" source package in that release at
        all. That one-line typo was NOT caught by
        test_real_system_library_matches_its_expected_component above
        (which only ever exercises libz.so.1/zlib1g, a library
        guaranteed present even on the barest container) -- it only
        surfaced later, against the real appimage-smoke CI runner, once
        libzstd1 happened to actually be installed there as a
        transitive dependency.

        This test closes that gap generically, for every entry in
        COMPONENT_EXPECTED_SOURCE_PACKAGES at once, rather than only the
        one or two libraries a hermetic bare-container test can rely on
        being present: it globs every real file under
        _SYSTEM_LIBRARY_SEARCH_DIRS, matches each basename against
        COMPONENT_PATTERNS to find which component (if any) it would be
        classified as, and -- for every match whose component has an
        entry in COMPONENT_EXPECTED_SOURCE_PACKAGES and whose real dpkg
        provenance can actually be captured -- asserts the table agrees
        with the live system. On a typical Debian/Ubuntu development or
        CI host with a reasonably full base system plus whatever
        packages this project's own CI steps installed, this exercises
        a large fraction of the table for free, with zero new
        hardcoded package names to keep in sync by hand; on a
        near-empty container it simply covers less (never spuriously
        failing), so it is deliberately additive/best-effort rather
        than an exhaustive guarantee -- exactly the same honest
        degradation policy as capture_package_provenance() itself."""
        problems: list[str] = []
        checked = 0
        seen_paths: set[Path] = set()
        for search_dir in audit._SYSTEM_LIBRARY_SEARCH_DIRS:
            if not search_dir.is_dir():
                continue
            try:
                entries = list(search_dir.iterdir())
            except OSError:
                continue
            for entry in entries:
                try:
                    resolved = entry.resolve()
                except OSError:
                    continue
                if not resolved.is_file() or resolved in seen_paths:
                    continue
                basename = entry.name
                component = None
                for pattern, pattern_component in audit.COMPONENT_PATTERNS:
                    if pattern.match(basename):
                        component = pattern_component
                        break
                if component is None:
                    continue
                if component not in audit.COMPONENT_EXPECTED_SOURCE_PACKAGES:
                    continue
                provenance = audit.capture_package_provenance(basename)
                if provenance is None:
                    continue
                seen_paths.add(resolved)
                checked += 1
                problem = audit.validate_component_package_provenance(
                    component, provenance
                )
                if problem is not None:
                    problems.append(problem)
        if checked == 0:
            self.skipTest(
                "no real system library on this host matched any "
                "COMPONENT_PATTERNS entry with a COMPONENT_EXPECTED_"
                "SOURCE_PACKAGES mapping -- nothing to cross-check here"
            )
        self.assertEqual(
            problems,
            [],
            f"COMPONENT_EXPECTED_SOURCE_PACKAGES disagreed with "
            f"{len(problems)} real, currently-installed system "
            f"package(s) (checked {checked} matched libraries): "
            + "; ".join(problems),
        )


if __name__ == "__main__":
    unittest.main()
