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
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from hashlib import md5
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

    def test_package_provenance_uses_the_content_bound_binding_not_bare_basename_lookup(
        self,
    ) -> None:
        # Finding #9 (distro provenance pinning, "SBOM calls legacy
        # basename capture instead of validated binding"): before this
        # fix, build_sbom_inventory() populated packageProvenance via
        # capture_package_provenance(basename) -- a lookup that never
        # actually inspects the real bundled file's own content at all,
        # so a substituted/downgraded bundled library could "match" an
        # unrelated same-named system file. This test proves the SBOM
        # path now calls the content-bound
        # bind_bundled_library_to_system_provenance(path) instead: it
        # mocks the two functions to return distinguishable sentinel
        # shapes and asserts the SBOM entry's packageProvenance is
        # EXACTLY the content-bound sentinel, never the basename-only
        # one, and that the basename-only function is never even
        # invoked by this code path.
        (self.lib_dir / "libavif.so.16.0.0").write_bytes(
            audit._ELF_MAGIC + b"fake libavif bytes"
        )
        content_bound_sentinel = {
            "status": "matched",
            "package": "libavif13",
            "version": "0.11.1-1",
            "sourcePackage": "libavif",
            "systemPath": "/usr/lib/x86_64-linux-gnu/libavif.so.16.0.0",
            "systemSha256": "content-bound-sentinel-sha",
            "bundledCanonicalLoadDigest": "content-bound-sentinel-digest",
        }
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_system_provenance",
            return_value=content_bound_sentinel,
        ) as bound_mock, mock.patch.object(
            audit,
            "capture_package_provenance",
            return_value={
                "package": "unrelated-basename-only-package",
                "version": "0.0.0-basename-only",
                "sourcePackage": "unrelated-basename-only-source",
            },
        ) as basename_mock:
            inventory = audit.build_sbom_inventory(self.lib_dir)
        by_path = {entry["path"]: entry for entry in inventory}
        self.assertEqual(
            by_path["libavif.so.16.0.0"]["packageProvenance"],
            content_bound_sentinel,
        )
        bound_mock.assert_called_once_with(
            self.lib_dir / "libavif.so.16.0.0", replay_toolset=None
        )
        basename_mock.assert_not_called()

    def test_package_provenance_surfaces_a_real_content_mismatch_in_the_sbom(
        self,
    ) -> None:
        # Companion to the mock-based test above, proving the actual
        # end-to-end behavior a real substituted bundled library would
        # produce in the SBOM: a same-basename "system" file exists and
        # IS dpkg-owned, but its content provably differs from the
        # bundled file -- the SBOM entry's packageProvenance must
        # honestly record "content_mismatch" (not silently "matched"),
        # with the exact system-side identity fields needed for
        # independent reconstruction.
        bundled_lib = self.lib_dir / "libavif.so.16.0.0"
        bundled_lib.write_bytes(audit._ELF_MAGIC + b"bundled bytes")
        system_dir = Path(self._tmp.name) / "system"
        system_dir.mkdir()
        system_copy = system_dir / "libavif.so.16.0.0"
        system_copy.write_bytes(audit._ELF_MAGIC + b"DIFFERENT system bytes")
        with mock.patch.object(
            audit, "_SYSTEM_LIBRARY_SEARCH_DIRS", (system_dir,)
        ), mock.patch.object(
            shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libavif13"
        ), mock.patch.object(
            audit,
            "_dpkg_package_metadata",
            return_value=("0.11.1-1", "libavif"),
        ), mock.patch.object(
            audit,
            "_canonical_load_digest",
            side_effect=lambda p: "digest-bundled"
            if p == bundled_lib
            else "digest-system",
        ):
            inventory = audit.build_sbom_inventory(self.lib_dir)
        by_path = {entry["path"]: entry for entry in inventory}
        provenance = by_path["libavif.so.16.0.0"]["packageProvenance"]
        self.assertEqual(provenance["status"], "content_mismatch")
        self.assertEqual(provenance["sourcePackage"], "libavif")
        self.assertEqual(provenance["systemPath"], str(system_copy.resolve()))
        self.assertEqual(
            provenance["systemSha256"], audit._sha256(system_copy)
        )
        self.assertEqual(provenance["systemCanonicalLoadDigest"], "digest-system")


def _compile_shared_object(cc_bin: str, source: str, output: Path) -> None:
    source_path = output.with_suffix(".c")
    source_path.write_text(source)
    subprocess.run(
        [cc_bin, "-shared", "-fPIC", "-o", str(output), str(source_path)],
        check=True,
        capture_output=True,
    )


def _compile_c_binary(
    cc_bin: str,
    source: str,
    output: Path,
    *,
    shared: bool = False,
    soname: str | None = None,
    library_dirs: tuple[Path, ...] = (),
    libraries: tuple[str, ...] = (),
    rpath_entries: tuple[str, ...] = (),
) -> None:
    """Small real-ELF fixture builder for dependency-graph tests."""
    source_path = output.with_suffix(".c")
    source_path.write_text(source)
    command = [cc_bin]
    if shared:
        command.extend(["-shared", "-fPIC"])
    command.extend(["-o", str(output), str(source_path), "-Wl,--no-as-needed"])
    if soname is not None:
        command.append(f"-Wl,-soname,{soname}")
    for library_dir in library_dirs:
        command.extend(["-L", str(library_dir)])
    if rpath_entries:
        command.append("-Wl,-rpath," + ":".join(rpath_entries))
    for library in libraries:
        command.append(f"-l{library}")
    subprocess.run(command, check=True, capture_output=True)


def _make_linker_name(versioned_library: Path) -> Path:
    """Creates/returns the unversioned libfoo.so symlink the linker
    expects for -lfoo against a versioned libfoo.so.N fixture."""
    versioned_name = versioned_library.name
    if ".so" not in versioned_name:
        raise AssertionError(f"{versioned_name!r} is not a shared-object name")
    linker_name = versioned_library.with_name(versioned_name.split(".so", 1)[0] + ".so")
    try:
        linker_name.unlink()
    except FileNotFoundError:
        pass
    linker_name.symlink_to(versioned_library.name)
    return linker_name


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


# Well-known ELF64 program-header p_type values (from <elf.h>) needed by
# the raw program-header patchers below -- deliberately hand-copied
# constants, not imported from production code, for the same
# independent-parser-integrity reason _TEST_LOAD_HEADER_RE is its own
# regex (see the module comment above it).
_PT_TLS = 7
_PT_GNU_STACK = 0x6474E551
_PT_GNU_RELRO = 0x6474E552
_PT_GNU_PROPERTY = 0x6474E553
_DT_INIT_ARRAY = 25


def _test_find_program_header_by_type(data: bytearray, p_type_wanted: int) -> int | None:
    """Returns the raw file offset of the FIRST Elf64_Phdr entry whose
    own p_type matches `p_type_wanted`, or None if no such entry exists
    -- parsed directly from the ELF header/program-header table,
    independent of readelf/production code, exactly as
    _test_flip_first_non_executable_load_segment() already does for
    PT_LOAD."""
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type == p_type_wanted:
            return entry_off
    return None


def _test_find_dynamic_tag_entry_offset(data: bytearray, tag_wanted: int) -> int | None:
    """Returns the raw file offset of the FIRST Elf64_Dyn entry (16
    bytes: an 8-byte little-endian d_tag followed by an 8-byte
    little-endian d_val/d_ptr union) inside the PT_DYNAMIC segment
    whose own d_tag matches `tag_wanted`, or None if PT_DYNAMIC is
    absent or has no such entry -- parsed directly from the ELF
    program-header table and raw `.dynamic` bytes, independent of
    readelf/production code."""
    PT_DYNAMIC = 2
    dynamic_off = _test_find_program_header_by_type(data, PT_DYNAMIC)
    if dynamic_off is None:
        return None
    p_offset = struct.unpack_from("<Q", data, dynamic_off + 8)[0]
    p_filesz = struct.unpack_from("<Q", data, dynamic_off + 32)[0]
    entry_off = p_offset
    end = p_offset + p_filesz
    while entry_off + 16 <= end:
        d_tag = struct.unpack_from("<q", data, entry_off)[0]
        if d_tag == tag_wanted:
            return entry_off
        if d_tag == 0:  # DT_NULL terminator
            break
        entry_off += 16
    return None


def _test_mutate_entry_point(path: Path) -> None:
    """Directly patches the raw ELF64 header's own e_entry field (an
    8-byte little-endian value at fixed file offset 0x18) to a
    different address -- simulating an execution-redirect attack that
    touches no section's own content, no program header, and no
    `.dynamic` tag at all, which only a check of the ELF header's own
    e_entry field can catch."""
    data = bytearray(path.read_bytes())
    e_entry = struct.unpack_from("<Q", data, 0x18)[0]
    struct.pack_into("<Q", data, 0x18, e_entry + 4)
    path.write_bytes(bytes(data))


def _test_add_write_permission_to_first_executable_load_segment(path: Path) -> None:
    """Directly patches the raw ELF64 program header table to grant the
    WRITE permission bit (PF_W) to the FIRST PT_LOAD segment that is
    already executable (PF_X) but not yet writable -- simulating the
    exact "RX->RWX" downgrade attack this round's review named: turning
    a legitimate read+execute mapping into a read+write+execute one,
    without touching any section's own content, declared sh_flags, or
    any other program header at all."""
    data = bytearray(path.read_bytes())
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    PT_LOAD = 1
    PF_X = 1
    PF_W = 2
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type != PT_LOAD:
            continue
        flags_off = entry_off + 4
        p_flags = struct.unpack_from("<I", data, flags_off)[0]
        if not (p_flags & PF_X) or (p_flags & PF_W):
            continue
        struct.pack_into("<I", data, flags_off, p_flags | PF_W)
        path.write_bytes(bytes(data))
        return
    raise AssertionError(
        f"{path} has no read+execute-but-not-write PT_LOAD segment to mutate"
    )


def _test_swap_flags_of_two_lowest_vaddr_load_segments(path: Path) -> None:
    """Directly patches the raw ELF64 program header table to SWAP the
    own declared protection flags of the two PT_LOAD segments with the
    lowest virtual addresses -- simulating a whole-segment permission
    change relative to the real load-time topology (e.g. what a real
    loader would treat as segment 0 becoming as permissive as segment
    1, or vice-versa) without touching any section's own content,
    declared sh_flags, or any per-section runtime-mapping bit this
    project's own section-keyed check already covers (since both
    involved segments keep exactly the sections they already had;
    only which FLAGS apply to each swaps)."""
    data = bytearray(path.read_bytes())
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    PT_LOAD = 1
    loads: list[tuple[int, int]] = []
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type != PT_LOAD:
            continue
        p_vaddr = struct.unpack_from("<Q", data, entry_off + 16)[0]
        loads.append((p_vaddr, entry_off))
    loads.sort(key=lambda item: item[0])
    if len(loads) < 2:
        raise AssertionError(f"{path} has fewer than two PT_LOAD segments to swap")
    _, off_a = loads[0]
    _, off_b = loads[1]
    flags_a = struct.unpack_from("<I", data, off_a + 4)[0]
    flags_b = struct.unpack_from("<I", data, off_b + 4)[0]
    if flags_a == flags_b:
        raise AssertionError(
            f"{path}'s two lowest-vaddr PT_LOAD segments already share "
            "identical flags -- this fixture cannot exercise a real swap"
        )
    struct.pack_into("<I", data, off_a + 4, flags_b)
    struct.pack_into("<I", data, off_b + 4, flags_a)
    path.write_bytes(bytes(data))


def _test_grant_execute_permission_to_highest_vaddr_load_segment(path: Path) -> None:
    """Directly patches the raw ELF64 program header table to grant the
    EXECUTE permission bit (PF_X) to the PT_LOAD segment with the
    HIGHEST virtual address -- simulating an attacker appending (or
    corrupting) a trailing segment so that it is executable, exactly
    the shape of a real patchelf-appended trailing segment (highest
    vaddr, at the tail of `_ordered_load_segment_flags()`'s own
    ordering) EXCEPT that its flags are no longer the exact "RW "
    triple `_canonical_load_segment_prefix()` treats as an always-safe,
    patchelf-only append. This is the fixture
    test_canonical_load_digest_detects_execute_bit_on_trailing_segment()
    uses to prove that canonicalization is narrowly scoped to the
    RW-only case and does not blind the digest to a genuinely hostile
    trailing segment."""
    data = bytearray(path.read_bytes())
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    PT_LOAD = 1
    PF_X = 1
    loads: list[tuple[int, int]] = []
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type != PT_LOAD:
            continue
        p_vaddr = struct.unpack_from("<Q", data, entry_off + 16)[0]
        loads.append((p_vaddr, entry_off))
    if not loads:
        raise AssertionError(f"{path} has no PT_LOAD segment to mutate")
    loads.sort(key=lambda item: item[0])
    _, highest_off = loads[-1]
    flags_off = highest_off + 4
    p_flags = struct.unpack_from("<I", data, flags_off)[0]
    struct.pack_into("<I", data, flags_off, p_flags | PF_X)
    path.write_bytes(bytes(data))


def _test_mutate_e_machine(path: Path) -> None:
    """Directly patches the raw ELF64 header's own e_machine field (a
    2-byte little-endian value at fixed file offset 0x12) -- simulating
    a whole-object, different-target-ISA substitution attack (e.g.
    swapping a real x86-64 plugin for one built for a different
    architecture entirely) that touches no section's own content, no
    program header, no `.dynamic` tag, and no PT_LOAD segment topology
    at all -- which only a check of the ELF header's own identity
    fields (class/data/type/OSABI/machine) can catch. Round-N+ review
    (HIGH, "canonical ELF identity misses ELF class/data/type/OSABI/
    machine")."""
    data = bytearray(path.read_bytes())
    e_machine = struct.unpack_from("<H", data, 0x12)[0]
    # EM_386 (3) is a real, distinct machine value readelf recognizes
    # by name (as opposed to an arbitrary, possibly-"<unknown>" one),
    # and is never the real value for a genuine x86-64 build this
    # project's own toolchain produces (EM_X86_64 == 62).
    struct.pack_into("<H", data, 0x12, 3 if e_machine != 3 else 4)
    path.write_bytes(bytes(data))


def _test_mutate_elf_header_identity_field(path: Path, field: str) -> None:
    """Directly patches one raw ELF-ident/header field among
    class/data/osabi/type, leaving every section/program-header byte
    otherwise untouched."""
    data = bytearray(path.read_bytes())
    if field == "class":
        current = data[4]
        data[4] = 1 if current != 1 else 2
    elif field == "data":
        current = data[5]
        data[5] = 2 if current != 2 else 1
    elif field == "osabi":
        current = data[7]
        data[7] = 3 if current != 3 else 6
    elif field == "type":
        current = struct.unpack_from("<H", data, 0x10)[0]
        struct.pack_into("<H", data, 0x10, 2 if current != 2 else 3)
    else:
        raise AssertionError(f"unknown ELF header identity field {field!r}")
    path.write_bytes(bytes(data))


def _test_append_orphan_trailing_load_segment(path: Path) -> None:
    """Appends a brand-new, genuinely real PT_LOAD program header entry
    -- together with the arbitrary "malicious" byte content it maps --
    entirely by APPENDING to the end of the file and relocating the
    program header TABLE itself into that newly appended region
    (updating only the ELF header's own e_phoff/e_phnum fields to
    point at it). Deliberately does NOT touch any existing section
    header, section content, or existing program header entry at all,
    and deliberately does NOT add any new section header for the
    appended bytes -- reproducing, via genuinely real ELF bytes real
    `readelf` will parse, the round-N+ review's exact "regular trailing
    data LOAD [segment] passes" finding: section headers are entirely
    optional, loader-irrelevant metadata (the kernel/dynamic loader
    only ever consults program headers, never section headers, to map
    memory), so this is a fully legal ELF object with a real,
    completely orphan (section-less) LOAD segment an attacker could
    use to smuggle arbitrary mapped bytes past any section-keyed check.
    Its own flags are deliberately the exact "RW " triple -- the same
    shape a genuine patchelf append always carries -- so that only a
    section-correlation-aware check (not a bare flags-based one) can
    tell the two apart."""
    data = bytearray(path.read_bytes())
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    old_table = data[e_phoff : e_phoff + e_phnum * e_phentsize]

    malicious_content = b"\xde\xad\xbe\xef" * 64
    malicious_offset = len(data)
    data.extend(malicious_content)

    PT_LOAD = 1
    PF_R = 4
    PF_W = 2
    # Any p_vaddr congruent to malicious_offset modulo p_align (here
    # 0x1000) satisfies the ELF spec's own PT_LOAD alignment
    # constraint; readelf itself never validates that the chosen
    # address range doesn't overlap another segment (it merely reports
    # the fields verbatim), so an arbitrary, sufficiently high
    # placeholder address is sufficient for a parse-only test fixture
    # -- this object is never actually executed by a real loader.
    align = 0x1000
    vaddr = 0x0F000000 + (malicious_offset % align)
    new_entry = struct.pack(
        "<IIQQQQQQ",
        PT_LOAD,
        PF_R | PF_W,
        malicious_offset,
        vaddr,
        vaddr,
        len(malicious_content),
        len(malicious_content),
        align,
    )

    new_table_offset = len(data)
    data.extend(old_table)
    data.extend(new_entry)

    struct.pack_into("<Q", data, 0x20, new_table_offset)
    struct.pack_into("<H", data, 0x38, e_phnum + 1)
    path.write_bytes(bytes(data))


def _test_load_program_header_offsets(data: bytearray) -> list[tuple[int, int, int]]:
    """Returns [(p_vaddr, p_offset, entry_off), ...] for every PT_LOAD
    program header in ascending p_vaddr order, parsed directly from the
    raw ELF program-header table independent of production code."""
    e_phoff = struct.unpack_from("<Q", data, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", data, 0x36)[0]
    e_phnum = struct.unpack_from("<H", data, 0x38)[0]
    PT_LOAD = 1
    loads: list[tuple[int, int, int]] = []
    for index in range(e_phnum):
        entry_off = e_phoff + index * e_phentsize
        p_type = struct.unpack_from("<I", data, entry_off)[0]
        if p_type != PT_LOAD:
            continue
        p_offset = struct.unpack_from("<Q", data, entry_off + 8)[0]
        p_vaddr = struct.unpack_from("<Q", data, entry_off + 16)[0]
        loads.append((p_vaddr, p_offset, entry_off))
    loads.sort(key=lambda item: item[0])
    return loads


def _test_mutate_first_load_segment_memsz(path: Path) -> None:
    """Directly patches the FIRST PT_LOAD segment's own p_memsz field,
    without touching flags, section headers, section contents, or any
    other segment, reproducing the exact "normal segment memsz mutation"
    review gap."""
    data = bytearray(path.read_bytes())
    loads = _test_load_program_header_offsets(data)
    if not loads:
        raise AssertionError(f"{path} has no PT_LOAD segment to mutate")
    memsz_off = loads[0][2] + 40
    memsz = struct.unpack_from("<Q", data, memsz_off)[0]
    struct.pack_into("<Q", data, memsz_off, memsz + 0x20)
    path.write_bytes(bytes(data))


def _test_mutate_first_load_segment_align(path: Path) -> None:
    """Directly patches the FIRST PT_LOAD segment's own p_align field,
    again leaving flags and mapped section bytes untouched."""
    data = bytearray(path.read_bytes())
    loads = _test_load_program_header_offsets(data)
    if not loads:
        raise AssertionError(f"{path} has no PT_LOAD segment to mutate")
    align_off = loads[0][2] + 48
    align = struct.unpack_from("<Q", data, align_off)[0]
    struct.pack_into("<Q", data, align_off, 1 if align != 1 else 0x1000)
    path.write_bytes(bytes(data))


def _test_mutate_second_load_segment_to_overlap_first(path: Path) -> None:
    """Directly patches the SECOND PT_LOAD segment's own p_offset and
    p_vaddr so it overlaps the FIRST one, changing topology without
    changing either segment's flags."""
    data = bytearray(path.read_bytes())
    loads = _test_load_program_header_offsets(data)
    if len(loads) < 2:
        raise AssertionError(f"{path} has fewer than two PT_LOAD segments to mutate")
    first_vaddr, first_offset, _ = loads[0]
    _, _, second_entry_off = loads[1]
    struct.pack_into("<Q", data, second_entry_off + 8, first_offset + 0x10)
    struct.pack_into("<Q", data, second_entry_off + 16, first_vaddr + 0x10)
    path.write_bytes(bytes(data))


def _test_section_virtual_range(path: Path, section_name: str) -> tuple[int, int]:
    """Returns (virtual_address, size) of the named section, parsed via
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
        + r"[ \t]+\S+[ \t]+(?P<addr>[0-9a-fA-F]+)[ \t]+[0-9a-fA-F]+[ \t]+"
        r"(?P<size>[0-9a-fA-F]+)[ \t]"
    )
    match = pattern.search(output)
    if not match:
        raise AssertionError(f"section {section_name!r} not found in {path}")
    return int(match.group("addr"), 16), int(match.group("size"), 16)


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

    def _build_shared_object_with_extra_note_section(self, output: Path) -> None:
        """Like _build_rich_shared_object(), but also embeds an extra
        real, allocated (SHF_ALLOC) `.note.qt.metadata` -- a synthetic
        stand-in for Qt's own genuine `Q_PLUGIN_METADATA`-driven note
        section, added via a hand-written assembly Elf64_Nhdr record so
        this test needs no real Qt SDK at all -- co-resident with GCC's
        own `.note.gnu.build-id`, exactly mirroring a real Qt plugin's
        note-section layout. This is the fixture
        test_canonical_load_digest_is_stable_across_a_patchelf_note_
        segment_regrouping() below needs to reproduce its own regression:
        see that test's docstring."""
        c_source = (
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
        asm_source = (
            '.section .note.qt.metadata,"a",@note\n'
            ".align 4\n"
            ".long 1f - 0f\n"
            ".long 3f - 2f\n"
            ".long 0\n"
            "0:\n"
            '.asciz "Qt"\n'
            "1:\n"
            ".align 4\n"
            "2:\n"
            ".byte 0,0,0,0\n"
            "3:\n"
            ".align 4\n"
        )
        c_path = output.with_suffix(".c")
        asm_path = output.with_name(output.stem + "_extra_note.s")
        c_path.write_text(c_source)
        asm_path.write_text(asm_source)
        subprocess.run(
            [self.cc_bin, "-shared", "-fPIC", "-o", str(output), str(c_path), str(asm_path)],
            check=True,
            capture_output=True,
        )

    def test_canonical_load_digest_is_stable_across_a_patchelf_note_segment_regrouping(
        self,
    ) -> None:
        # Real cumulative-review regression, found only against this
        # project's own actual produced AppImage using a real Qt SDK
        # and the real pinned linuxdeploy/linuxdeploy-plugin-qt binaries
        # (never reproduced by this module's own prior synthetic
        # fixtures, none of which had more than one real, allocated
        # NOTE section co-resident with `.note.gnu.build-id`): every
        # genuinely bundled Qt plugin/QML module (e.g.
        # plugins/imageformats/libqgif.so) legitimately has its own
        # `.note.qt.metadata` note section, in addition to the usual
        # `.note.gnu.build-id`/`.note.gnu.property`. This project's own
        # empirical testing found that a real patchelf 0.14.3 RUNPATH
        # rewrite, whenever it must relocate `.dynstr` (and whatever
        # else was co-resident with it, e.g. the note sections) into a
        # brand-new appended LOAD segment (see the two relocation tests
        # above), also legitimately CHANGES HOW MANY PT_NOTE program
        # headers cover those same, byte-for-byte-unchanged note
        # sections -- e.g. one PT_NOTE program header shrinking/
        # disappearing while another grows, or a single combined
        # PT_NOTE splitting into several. Before this was fixed,
        # _non_load_program_header_records() (and therefore
        # _canonical_load_digest()) folded PT_NOTE's own raw segment
        # count/filesz/memsz straight into the digest as if it were
        # content-stable, exactly like PT_GNU_STACK/PT_GNU_RELRO/PT_TLS
        # -- so this entirely legitimate, content-preserving repack
        # produced a DIFFERENT digest, wrongly rejecting every real,
        # unmodified Qt plugin/QML module with more than one loaded NOTE
        # section as an unrecognized/substituted library.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        reference = self.root / "reference_note.so"
        self._build_shared_object_with_extra_note_section(reference)
        patched = self.root / "patched_note.so"
        patched.write_bytes(reference.read_bytes())
        patched.chmod(0o755)
        long_rpath = "/".join(["deliberately-very-long-rpath-component"] * 40)
        subprocess.run(
            ["patchelf", "--set-rpath", long_rpath, str(patched)],
            check=True,
            capture_output=True,
        )

        def _note_header_count(path: Path) -> int:
            output = subprocess.run(
                ["readelf", "-lW", str(path)], check=True, capture_output=True, text=True
            ).stdout
            return sum(
                1 for line in output.splitlines() if line.strip().startswith("NOTE ")
            )

        # Sanity: the RPATH rewrite really did change how many PT_NOTE
        # program headers cover the (byte-for-byte-unchanged) note
        # sections -- proving this test actually exercises the reported
        # bug's exact precondition, not merely an in-place, same-layout
        # rewrite that would never have exposed it.
        self.assertNotEqual(
            _note_header_count(reference),
            _note_header_count(patched),
            "fixture did not actually change PT_NOTE program-header grouping -- "
            "this test would not have caught the reported bug",
        )
        # Every individual note SECTION's own bytes are genuinely
        # unchanged (only their program-header grouping moved).
        for note_name in (".note.gnu.build-id", ".note.qt.metadata"):
            ref_offset, ref_size = _test_section_file_range(reference, note_name)
            with reference.open("rb") as handle:
                handle.seek(ref_offset)
                ref_bytes = handle.read(ref_size)
            pat_offset, pat_size = _test_section_file_range(patched, note_name)
            with patched.open("rb") as handle:
                handle.seek(pat_offset)
                pat_bytes = handle.read(pat_size)
            self.assertEqual(ref_bytes, pat_bytes)
        # The real assertion: the fixed, production canonical digest
        # -- which must never treat PT_NOTE's own raw segment grouping
        # as content-stable -- is stable across this legitimate repack,
        # exactly as a genuinely unmodified, merely repackaged Qt
        # plugin/QML module must be.
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

    def _build_shared_object_with_two_constructors_and_tls(self, output: Path) -> None:
        """A variant of _build_rich_shared_object() with TWO
        `__attribute__((constructor))` functions (guaranteeing
        `.init_array` has at least two 8-byte pointer slots, needed by
        test_canonical_load_digest_detects_dynamic_tag_retarget_within_
        same_section below) and a real thread-local variable
        (guaranteeing a genuine PT_TLS program header, needed by
        test_canonical_load_digest_detects_tls_template_size_mutation
        below)."""
        source = (
            "#include <stdlib.h>\n"
            'static const char message[] = "canonical-load-digest-fixture-2";\n'
            "static char scratch_buffer[4096];\n"
            "__thread int tls_scratch = 7;\n"
            "__attribute__((constructor)) static void init_one(void) {\n"
            "    void *p = malloc(16);\n"
            "    scratch_buffer[0] = (char)(size_t)p;\n"
            "    free(p);\n"
            "}\n"
            "__attribute__((constructor)) static void init_two(void) {\n"
            "    scratch_buffer[1] = (char)tls_scratch;\n"
            "}\n"
            "const char *test_plugin_entry(void) { return message; }\n"
        )
        _compile_shared_object(self.cc_bin, source, output)

    def test_canonical_load_digest_detects_dynamic_tag_retarget_within_same_section(
        self,
    ) -> None:
        # Third-HIGH-round review ("canonical ELF digest ... collapses
        # dynamic pointer tags to section name, ignoring
        # section-relative offset ... DT_INIT/FINI/INIT_ARRAY pointer
        # changes within same section ... can pass with retained build
        # id"): this test constructs the EXACT attack described --
        # DT_INIT_ARRAY's own raw pointer value is retargeted from its
        # first `.init_array` slot to its SECOND slot. Both slots
        # already legitimately exist with real, unmutated content (two
        # real constructors), so NO section's own bytes change at all
        # -- only the `.dynamic` table's own DT_INIT_ARRAY entry does.
        # A section-name-only resolution (the previous, buggy
        # behavior) would see "-> .init_array" both before and after
        # and wrongly treat this as unchanged; only preserving the
        # offset within the section catches it.
        reference = self.root / "dtinitarray_ref.so"
        self._build_shared_object_with_two_constructors_and_tls(reference)
        mutated = self.root / "dtinitarray_mut.so"
        mutated.write_bytes(reference.read_bytes())

        section_addr, section_size = _test_section_virtual_range(
            reference, ".init_array"
        )
        self.assertGreaterEqual(
            section_size, 16, "fixture must have >= 2 init_array slots"
        )
        data = bytearray(mutated.read_bytes())
        entry_off = _test_find_dynamic_tag_entry_offset(data, _DT_INIT_ARRAY)
        self.assertIsNotNone(
            entry_off, "fixture's .dynamic table has no DT_INIT_ARRAY entry"
        )
        d_ptr = struct.unpack_from("<Q", data, entry_off + 8)[0]
        self.assertEqual(
            d_ptr, section_addr, "DT_INIT_ARRAY should point at the section start"
        )
        # Retarget 8 bytes later -- still squarely inside the SAME
        # `.init_array` section (asserted above to be >= 16 bytes/2
        # slots), so a section-name-only resolution sees no change.
        struct.pack_into("<Q", data, entry_off + 8, d_ptr + 8)
        mutated.write_bytes(bytes(data))

        # Sanity: no section's own content moved; only the `.dynamic`
        # table's own decoded DT_INIT_ARRAY value did.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        reference_tags = dict(audit._read_dynamic_tags(reference))
        mutated_tags = dict(audit._read_dynamic_tags(mutated))
        self.assertNotEqual(
            reference_tags.get("INIT_ARRAY"), mutated_tags.get("INIT_ARRAY")
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_gnu_stack_executable_flag_mutation(
        self,
    ) -> None:
        # Third-HIGH-round review ("... executable GNU_STACK ... can
        # pass with retained build id"): PT_GNU_STACK's own FLAGS field
        # is the sole authoritative record of whether the process stack
        # is mapped executable; it has no corresponding section at all,
        # so only direct program-header-table authentication (added by
        # this round's fix) can catch this flip.
        reference = self.root / "gnustack_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "gnustack_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        entry_off = _test_find_program_header_by_type(data, _PT_GNU_STACK)
        self.assertIsNotNone(
            entry_off, "fixture has no PT_GNU_STACK program header"
        )
        flags_off = entry_off + 4
        p_flags = struct.unpack_from("<I", data, flags_off)[0]
        PF_X = 1
        self.assertEqual(
            p_flags & PF_X, 0, "fixture's stack should not already be executable"
        )
        struct.pack_into("<I", data, flags_off, p_flags | PF_X)
        mutated.write_bytes(bytes(data))

        # Sanity: no section table or section content moved at all.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_gnu_relro_shrink_mutation(self) -> None:
        # Third-HIGH-round review ("... RELRO ... can pass with
        # retained build id"): PT_GNU_RELRO's own MemSiz is the
        # authoritative record of how many bytes the dynamic loader
        # will mprotect read-only after relocation processing; shrinking
        # it (a real hardening downgrade) touches no section's own
        # content or declared sh_flags at all.
        reference = self.root / "relro_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "relro_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        entry_off = _test_find_program_header_by_type(data, _PT_GNU_RELRO)
        self.assertIsNotNone(
            entry_off, "fixture has no PT_GNU_RELRO program header"
        )
        memsz_off = entry_off + 40
        memsz = struct.unpack_from("<Q", data, memsz_off)[0]
        self.assertGreater(memsz, 8, "fixture's RELRO range should be shrinkable")
        struct.pack_into("<Q", data, memsz_off, memsz - 8)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_tls_template_size_mutation(self) -> None:
        # Third-HIGH-round review ("... TLS ... can pass with retained
        # build id"): PT_TLS's own MemSiz is the thread-local-storage
        # template's declared size; an attacker growing it (e.g. to
        # reserve extra, attacker-usable per-thread storage) changes no
        # section's own content or declared sh_flags at all.
        reference = self.root / "tls_ref.so"
        self._build_shared_object_with_two_constructors_and_tls(reference)
        mutated = self.root / "tls_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        entry_off = _test_find_program_header_by_type(data, _PT_TLS)
        self.assertIsNotNone(entry_off, "fixture has no PT_TLS program header")
        memsz_off = entry_off + 40
        memsz = struct.unpack_from("<Q", data, memsz_off)[0]
        struct.pack_into("<Q", data, memsz_off, memsz + 8)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def _mutate_program_header_vaddr(
        self, data: bytearray, p_type: int, delta: int
    ) -> None:
        """Adds `delta` to the p_vaddr field of the FIRST program
        header of type `p_type` in-place, leaving p_offset/p_paddr and
        every section untouched -- breaks that segment's own
        offsetMinusVaddr (and, unless p_paddr already equalled p_vaddr,
        also its paddrMinusVaddr) bias term without moving any byte of
        real file content, exactly the "inconsistent repositioning"
        Round-N+ review ("Include PT_DYNAMIC and non-LOAD addresses")
        exists to catch."""
        entry_off = _test_find_program_header_by_type(data, p_type)
        self.assertIsNotNone(entry_off, f"fixture has no p_type={p_type:#x} header")
        vaddr_off = entry_off + 16
        vaddr = struct.unpack_from("<Q", data, vaddr_off)[0]
        struct.pack_into("<Q", data, vaddr_off, vaddr + delta)

    def _bind_isolated_program_header_mutation_via_captured_provenance(
        self, staged_reference: Path, mutated_bundled: Path
    ) -> dict[str, object]:
        """Builds a minimal formatVersion-2 captured-provenance manifest
        binding `staged_reference` (playing the role of the immutable
        staged pre-packaging source object) to a single AppDir-relative
        destination, then binds `mutated_bundled` (playing the role of
        the final shipped file) against it with NO replay_toolset --
        i.e. exactly the heuristic-only _canonical_load_digest() path
        exercised directly by the (removed) stability assertions this
        replaces. Uses "zlib"'s own real, pinned
        COMPONENT_EXPECTED_SOURCE_PACKAGES/COMPONENT_EXPECTED_SOURCE_VERSION
        entries so validate_bundled_library_package_provenance()'s
        unconditional (not require_provenance-gated) package-identity/
        version checks agree, isolating this test to exactly the
        evidenceStrength enforcement under test."""
        manifest = {
            "formatVersion": 2,
            "bundledPaths": {
                "usr/lib/isolated-mutation-fixture.so": {
                    "bundledPath": "usr/lib/isolated-mutation-fixture.so",
                    "stagedPath": str(staged_reference),
                    "sourceRealPath": str(staged_reference),
                    "sha256": audit._sha256(staged_reference),
                    "canonicalLoadDigest": audit._canonical_load_digest(staged_reference),
                    "package": "zlib1g",
                    "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["zlib"],
                    "sourcePackage": "zlib",
                }
            },
        }
        return audit.bind_bundled_library_to_captured_provenance(
            mutated_bundled, manifest, "usr/lib/isolated-mutation-fixture.so"
        )

    def test_dynamic_segment_isolated_relocation_passes_heuristic_but_fails_required_provenance(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "Tests currently require attacker
        # p_vaddr mutations to pass -- reverse them"): PT_DYNAMIC's own
        # location is intentionally excluded from
        # _canonical_load_digest() (see _non_load_program_header_
        # records()'s own module comment: a real `patchelf --set-rpath`
        # rewrite empirically relocates it), so this isolated
        # relocation -- no section or `.dynamic` tag content moved at
        # all -- legitimately still passes the pure heuristic
        # comparison. But it MUST now be rejected under
        # --require-package-provenance, which demands a byte-identical
        # replay of the real pinned packaging transform, never merely
        # heuristic agreement.
        PT_DYNAMIC = 2
        reference = self.root / "dynloc_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "dynloc_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        self._mutate_program_header_vaddr(data, PT_DYNAMIC, 4096)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

        binding = self._bind_isolated_program_header_mutation_via_captured_provenance(
            reference, mutated
        )
        self.assertEqual(binding["status"], "matched")
        self.assertEqual(binding["evidenceStrength"], "canonical_digest_heuristic")

        problem = audit.validate_bundled_library_package_provenance(
            "zlib", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("replay", problem)

        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "zlib", binding, require_provenance=False
            )
        )

    def test_dynamic_segment_isolated_relocation_is_rejected_when_replay_toolset_supplied(
        self,
    ) -> None:
        # Companion to the test above: when a (fake, deterministic)
        # replay toolset IS supplied, bind_bundled_library_to_captured_
        # provenance() must use replay_strip_and_rpath_transform()
        # instead of the heuristic, and a replay that disagrees (this
        # fake never claims to reproduce the mutated file) is always a
        # decisive "content_mismatch"/"replay_mismatch", never silently
        # downgraded back to the heuristic's "matched" verdict.
        PT_DYNAMIC = 2
        reference = self.root / "dynloc_ref2.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "dynloc_mut2.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        self._mutate_program_header_vaddr(data, PT_DYNAMIC, 4096)
        mutated.write_bytes(bytes(data))

        fake_toolset = {
            "toolLabel": "fake-linuxdeploy",
            "patchelfPath": "/nonexistent/patchelf",
            "patchelfSha256": "fake",
            "stripPath": "/nonexistent/strip",
            "stripSha256": "fake",
        }

        def fake_replay(
            reference_path: Path,
            final_path: Path,
            toolset: dict[str, str],
            expected_rpath: str | None,
        ) -> dict[str, object]:
            return {
                "toolLabel": toolset["toolLabel"],
                "matched": False,
                "replayedSha256": audit._sha256(reference_path),
                "finalSha256": audit._sha256(final_path),
            }

        with mock.patch.object(
            audit, "replay_strip_and_rpath_transform", side_effect=fake_replay
        ):
            manifest = {
                "formatVersion": 2,
                "bundledPaths": {
                    "usr/lib/isolated-mutation-fixture.so": {
                        "bundledPath": "usr/lib/isolated-mutation-fixture.so",
                        "stagedPath": str(reference),
                        "sourceRealPath": str(reference),
                        "sha256": audit._sha256(reference),
                        "canonicalLoadDigest": audit._canonical_load_digest(reference),
                        "package": "zlib1g",
                        "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["zlib"],
                        "sourcePackage": "zlib",
                    }
                },
            }
            binding = audit.bind_bundled_library_to_captured_provenance(
                mutated,
                manifest,
                "usr/lib/isolated-mutation-fixture.so",
                replay_toolset=fake_toolset,
            )

        self.assertEqual(binding["status"], "content_mismatch")
        self.assertEqual(binding["evidenceStrength"], "replay_mismatch")
        problem = audit.validate_bundled_library_package_provenance(
            "zlib", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)

    def test_gnu_property_segment_isolated_relocation_passes_heuristic_but_fails_required_provenance(
        self,
    ) -> None:
        # Same reasoning as the PT_DYNAMIC test above, for the other
        # location intentionally excluded from _canonical_load_digest()
        # -- PT_GNU_PROPERTY, which physically travels with
        # `.note.gnu.property` during a real `patchelf --set-rpath`
        # rewrite.
        reference = self.root / "propertyloc_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "propertyloc_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        self._mutate_program_header_vaddr(data, _PT_GNU_PROPERTY, 4096)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

        binding = self._bind_isolated_program_header_mutation_via_captured_provenance(
            reference, mutated
        )
        self.assertEqual(binding["status"], "matched")
        self.assertEqual(binding["evidenceStrength"], "canonical_digest_heuristic")

        problem = audit.validate_bundled_library_package_provenance(
            "zlib", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("replay", problem)

        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "zlib", binding, require_provenance=False
            )
        )

    def test_canonical_load_digest_detects_gnu_relro_segment_location_mutation(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "Include PT_DYNAMIC and non-LOAD
        # addresses"): PT_GNU_RELRO's own location (which range the
        # loader will actually mprotect read-only after relocation
        # processing) was previously never bound, only its size. Unlike
        # PT_DYNAMIC (see the test immediately above), PT_GNU_RELRO's
        # own location was verified STABLE across a real patchelf
        # RPATH-growth rewrite, so binding it here is safe.
        reference = self.root / "relroloc_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "relroloc_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        self._mutate_program_header_vaddr(data, _PT_GNU_RELRO, 0x1000)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_tls_segment_location_mutation(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "Include PT_DYNAMIC and non-LOAD
        # addresses"): PT_TLS's own location (where the loader's
        # thread-local-storage template is actually read from) was
        # previously never bound, only its size/alignment. Unlike
        # PT_DYNAMIC (see the test above), PT_TLS's own location was
        # verified STABLE across a real patchelf RPATH-growth rewrite,
        # so binding it here is safe.
        reference = self.root / "tlsloc_ref.so"
        self._build_shared_object_with_two_constructors_and_tls(reference)
        mutated = self.root / "tlsloc_mut.so"
        mutated.write_bytes(reference.read_bytes())
        data = bytearray(mutated.read_bytes())
        self._mutate_program_header_vaddr(data, _PT_TLS, 0x1000)
        mutated.write_bytes(bytes(data))

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_entry_point_redirect(self) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity misses load
        # security/mapping ... e_entry ... pass"): redirecting the raw
        # ELF header e_entry value (e.g. to attacker-controlled data
        # mistakenly treated as code, or to a different real function)
        # touches no section's own content, no program header, and no
        # `.dynamic` tag at all -- only a direct check of the ELF
        # header's own e_entry field can catch it.
        reference = self.root / "entry_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "entry_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_mutate_entry_point(mutated)

        # Sanity: nothing else about the object changed at all.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._read_program_headers(reference),
            audit._read_program_headers(mutated),
        )
        self.assertNotEqual(
            audit._read_entry_point(reference), audit._read_entry_point(mutated)
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_rx_segment_gaining_write_permission(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity misses load
        # security/mapping ... RX->RWX stays true and passes"): the
        # PREVIOUS digest recorded only the bare EXECUTABLE boolean of
        # each section's runtime mapping, never its write bit -- so an
        # attacker granting WRITE permission to an already-executable
        # segment (turning a legitimate RX mapping into a dangerous RWX
        # one) changed nothing this digest recorded at all, as long as
        # the executable bit itself stayed "true" and no section's own
        # content/declared sh_flags moved.
        reference = self.root / "rwx_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "rwx_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_add_write_permission_to_first_executable_load_segment(mutated)

        # Sanity: no section's own content or declared sh_flags moved
        # at all; only the program-header table's own flags changed.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_load_segment_permission_swap(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity misses load
        # security/mapping ... load addresses/ranges/order/overlap
        # pass"): the per-SECTION runtime-mapping check (keyed by
        # section NAME) says nothing about the SEGMENT topology itself
        # -- swapping which PT_LOAD segment gets which protection flags
        # (e.g. what a real loader would treat as segment 0 becoming as
        # permissive as segment 1) previously changed nothing this
        # digest recorded, as long as each individual section's own
        # name/type/declared-sh_flags/content/mapped-executable-bit
        # stayed exactly what it already was under ITS OWN segment.
        reference = self.root / "segswap_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "segswap_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_swap_flags_of_two_lowest_vaddr_load_segments(mutated)

        # Sanity: no section's own content or declared sh_flags moved
        # at all; only which PT_LOAD segment owns which flags did.
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._ordered_load_segment_flags(reference),
            audit._ordered_load_segment_flags(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_normal_load_segment_memsz_mutation(
        self,
    ) -> None:
        # Cumulative review (independent re-review, HIGH, "Canonical ELF
        # identity still omits actual PT_LOAD mappings"): mutating a
        # NORMAL (non-trailing-tolerated) PT_LOAD segment's own p_memsz
        # must now change the digest even though the flags-only prefix
        # and every section header remain unchanged.
        reference = self.root / "memsz_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "memsz_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_mutate_first_load_segment_memsz(mutated)

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._canonical_load_segment_prefix(reference),
            audit._canonical_load_segment_prefix(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_normal_load_segment_align_mutation(
        self,
    ) -> None:
        # Same review finding, different field: p_align on a normal
        # PT_LOAD segment was previously entirely outside the hashed
        # identity despite materially changing the loader mapping.
        reference = self.root / "align_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "align_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_mutate_first_load_segment_align(mutated)

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._canonical_load_segment_prefix(reference),
            audit._canonical_load_segment_prefix(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_normal_load_segment_topology_overlap_mutation(
        self,
    ) -> None:
        # Cumulative review (independent re-review, HIGH, "actual PT_LOAD
        # mappings"): a topology mutation changing p_offset/p_vaddr while
        # keeping both segments' own permission flags unchanged must no
        # longer pass under the old flags-only PT_LOAD identity.
        reference = self.root / "topology_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "topology_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_mutate_second_load_segment_to_overlap_first(mutated)

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._ordered_load_segment_flags(reference),
            audit._ordered_load_segment_flags(mutated),
        )
        self.assertEqual(
            audit._canonical_load_segment_prefix(reference),
            audit._canonical_load_segment_prefix(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_is_stable_across_patchelf_appended_trailing_segment(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "... load addresses/ranges/order/
        # overlap pass"): the fix must not itself become a false
        # positive against the SAME legitimate patchelf RUNPATH rewrite
        # every other stability test in this class already tolerates --
        # a real patchelf run can append a brand-new trailing PT_LOAD
        # segment (see _ordered_load_segment_flags()'s own docstring),
        # which must never, by itself, change the ordered PT_LOAD flags
        # sequence's EXISTING entries, only add one more at the end.
        if not shutil.which("patchelf"):
            self.skipTest("requires patchelf to simulate a real RUNPATH rewrite")
        reference = self.root / "loadseg_stable_ref.so"
        self._build_rich_shared_object(reference)
        bundled = self.root / "loadseg_stable_bundled.so"
        bundled.write_bytes(reference.read_bytes())
        bundled.chmod(0o755)
        long_rpath = "$ORIGIN/" + "a" * 512
        subprocess.run(
            ["patchelf", "--set-rpath", long_rpath, str(bundled)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(bundled))
        reference_flags = audit._ordered_load_segment_flags(reference)
        bundled_flags = audit._ordered_load_segment_flags(bundled)
        self.assertEqual(
            bundled_flags[: len(reference_flags)], reference_flags
        )
        self.assertEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(bundled),
        )

    def test_canonical_load_segment_prefix_strips_only_a_trailing_rw_only_run(
        self,
    ) -> None:
        # Direct, non-patchelf unit proof of _canonical_load_segment_
        # prefix()'s own exact narrowing rule (this round's follow-up
        # regression fix): a genuinely appended, exactly-"RW "-flagged
        # trailing segment (and ONLY that shape) is dropped from the
        # canonical prefix, while every earlier, non-trailing, or
        # non-"RW "-flagged entry is preserved untouched.
        reference = self.root / "prefix_ref.so"
        self._build_rich_shared_object(reference)
        full = audit._ordered_load_segment_flags(reference)
        prefix = audit._canonical_load_segment_prefix(reference)
        # This fixture's own last PT_LOAD segment (the real linker's
        # own writable data segment) is exactly "RW ", so the
        # canonical prefix must have dropped it...
        self.assertEqual(full[-1], "RW ")
        self.assertEqual(prefix, full[:-1])
        # ...while every earlier, non-trailing entry -- including any
        # OTHER "RW "-flagged segment that is not itself at the very
        # end -- must still be present, verbatim, in order.
        self.assertTrue(all(entry in full for entry in prefix))
        self.assertEqual(len(prefix), len(full) - 1)

    def test_canonical_load_digest_detects_execute_bit_on_trailing_segment(
        self,
    ) -> None:
        # Round-N+ review follow-up regression fix ("... load
        # addresses/ranges/order/overlap pass"): proves
        # _canonical_load_segment_prefix()'s trailing-"RW "-only
        # append tolerance is narrowly scoped and cannot be abused --
        # a trailing segment that is ALSO executable (never observed
        # from a real patchelf run; see
        # _canonical_load_segment_prefix()'s own docstring) is NOT
        # "RW " and therefore is never silently dropped, so granting
        # it execute permission still changes the canonical prefix and
        # the digest.
        reference = self.root / "trailing_exec_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "trailing_exec_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_grant_execute_permission_to_highest_vaddr_load_segment(mutated)

        self.assertNotEqual(
            audit._ordered_load_segment_flags(reference)[-1],
            audit._ordered_load_segment_flags(mutated)[-1],
        )
        self.assertNotEqual(
            audit._canonical_load_segment_prefix(reference),
            audit._canonical_load_segment_prefix(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_read_elf_header_identity_extracts_all_five_fields(self) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity misses ELF
        # class/data/type/OSABI/machine"): direct proof
        # _read_elf_header_identity() actually parses a real object's
        # own header rather than merely being wired in but silently
        # returning None.
        reference = self.root / "header_identity_ref.so"
        self._build_rich_shared_object(reference)
        identity = audit._read_elf_header_identity(reference)
        self.assertIsNotNone(identity)
        assert identity is not None
        self.assertEqual(identity["class"], "ELF64")
        self.assertIn("little endian", identity["data"])
        self.assertIn("DYN", identity["type"])
        self.assertIn("X86-64", identity["machine"])

    def test_canonical_load_digest_detects_e_machine_substitution(self) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity misses ELF
        # class/data/type/OSABI/machine"): a whole-object,
        # different-target-ISA substitution touches NO section content,
        # NO program header, and NO `.dynamic` tag at all -- before this
        # fix, nothing in this digest would have detected it as long as
        # the substitute coincidentally reused the reference's own
        # section/segment layout (exactly what this fixture, a byte-
        # for-byte copy with only e_machine mutated, guarantees).
        reference = self.root / "machine_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "machine_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_mutate_e_machine(mutated)

        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertEqual(
            audit._ordered_load_segment_flags(reference),
            audit._ordered_load_segment_flags(mutated),
        )
        reference_identity = audit._read_elf_header_identity(reference)
        mutated_identity = audit._read_elf_header_identity(mutated)
        assert reference_identity is not None and mutated_identity is not None
        self.assertNotEqual(
            reference_identity["machine"], mutated_identity["machine"]
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_canonical_load_digest_detects_class_data_osabi_and_type_substitution(
        self,
    ) -> None:
        # Completes the direct whole-object ELF-header identity coverage
        # alongside test_canonical_load_digest_detects_e_machine_
        # substitution(): each of the OTHER governed header identity
        # fields must likewise change the digest even when the program
        # headers and section bytes stay otherwise identical.
        #
        # "class" (32/64-bit) and "data" (endianness) are NOT like
        # "osabi"/"type": they are structural interpretation switches
        # that change how every OTHER field in the file is decoded, not
        # cosmetic identity metadata layered on top of an otherwise
        # unchanged byte layout. A real `readelf` asked to re-parse a
        # class/data-flipped copy of a genuine 64-bit little-endian
        # object legitimately produces DIFFERENT (or empty) section/
        # program-header listings -- not because any section or segment
        # byte actually moved, but because the exact same bytes are now
        # being decoded under the wrong word-width/endianness assumption
        # entirely. Asserting section/segment-table equality for those
        # two fields would therefore assert something real `readelf`
        # cannot honestly satisfy; only "osabi"/"type" -- which change
        # no structural offset or width at all -- support that stronger
        # same-shape assertion. Every field, including class/data, must
        # still (a) visibly change in the parsed header identity itself
        # and (b) change the canonical digest.
        reference = self.root / "header_fields_ref.so"
        self._build_rich_shared_object(reference)
        reference_identity = audit._read_elf_header_identity(reference)
        assert reference_identity is not None
        for field in ("class", "data", "osabi", "type"):
            with self.subTest(field=field):
                mutated = self.root / f"header_fields_{field}.so"
                mutated.write_bytes(reference.read_bytes())
                _test_mutate_elf_header_identity_field(mutated, field)
                if field in ("osabi", "type"):
                    self.assertEqual(
                        audit._read_section_headers(reference),
                        audit._read_section_headers(mutated),
                    )
                    self.assertEqual(
                        audit._ordered_load_segment_flags(reference),
                        audit._ordered_load_segment_flags(mutated),
                    )
                mutated_identity = audit._read_elf_header_identity(mutated)
                assert mutated_identity is not None
                self.assertNotEqual(
                    reference_identity[field], mutated_identity[field]
                )
                self.assertNotEqual(
                    audit._canonical_load_digest(reference),
                    audit._canonical_load_digest(mutated),
                )

    def test_canonical_load_digest_detects_orphan_appended_trailing_load_segment(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "canonical ELF identity ... strips all
        # trailing RW LOAD indiscriminately"): the exact "regular
        # trailing data LOAD [segment] passes" finding, reproduced with
        # genuinely real ELF bytes real readelf parses -- a brand-new,
        # real PT_LOAD segment appended with the SAME "RW " flags shape
        # a genuine patchelf append always carries, but with NO
        # section-header correlation at all (section headers are
        # entirely optional, loader-irrelevant metadata; the loader
        # only ever consults program headers to map memory). The prior,
        # purely flags-based trailing-strip heuristic would have
        # silently discarded this from the digest; the section-
        # correlation-aware fix must not.
        reference = self.root / "orphan_ref.so"
        self._build_rich_shared_object(reference)
        mutated = self.root / "orphan_mut.so"
        mutated.write_bytes(reference.read_bytes())
        _test_append_orphan_trailing_load_segment(mutated)

        # Sanity: the injected segment really is shaped exactly like a
        # tolerated, legitimate append (same trailing "RW " flags), so
        # only the section-correlation check -- not the flags alone --
        # can tell the two apart.
        mutated_flags = audit._ordered_load_segment_flags(mutated)
        self.assertEqual(mutated_flags[-1], "RW ")
        self.assertEqual(
            audit._read_section_headers(reference),
            audit._read_section_headers(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_segment_prefix(reference),
            audit._canonical_load_segment_prefix(mutated),
        )
        self.assertNotEqual(
            audit._canonical_load_digest(reference),
            audit._canonical_load_digest(mutated),
        )

    def test_load_segment_section_membership_reports_no_sections_for_the_orphan_segment(
        self,
    ) -> None:
        # Direct, non-digest unit proof of
        # _load_segment_section_membership()'s own contract for exactly
        # this shape of attack: the orphan trailing segment maps ZERO
        # section names (never a missing key -- see that function's
        # own docstring), which is what lets
        # _canonical_load_segment_prefix() distinguish it from a
        # genuine, section-correlated patchelf append.
        mutated = self.root / "orphan_membership_mut.so"
        self._build_rich_shared_object(mutated)
        _test_append_orphan_trailing_load_segment(mutated)
        headers = audit._read_program_headers(mutated)
        orphan_index = max(
            (
                index
                for index, header in enumerate(headers)
                if header["type"] == "LOAD"
            ),
            key=lambda index: int(headers[index]["vaddr"], 16),
        )
        membership = audit._load_segment_section_membership(mutated)
        self.assertEqual(membership.get(orphan_index, []), [])


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
                    "dpkgFileIntegrity": "unavailable",
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

        def fake_bind(
            bundled_path: Path, replay_toolset: dict[str, str] | None = None
        ) -> dict[str, object]:
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

        def fake_bind(
            bundled_path: Path, replay_toolset: dict[str, str] | None = None
        ) -> dict[str, object]:
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
        #
        # Round-N+ review ("No basename re-discovery"): the
        # authoritative --require-package-provenance path now also
        # requires a real --distro-provenance-manifest (see
        # test_cmd_classify_requires_provenance_requires_manifest in
        # CaptureBeforePackagingProvenanceTests for that configuration-
        # error case on its own); provide one here and exercise the
        # captured-manifest binder instead of the older basename-search
        # binder.
        lib_dir = Path(self._tmp.name) / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)
        manifest_path = Path(self._tmp.name) / "manifest.json"
        manifest_path.write_text(json.dumps({}))

        def fake_bind(
            bundled_path: Path,
            manifest: dict[str, object],
            bundled_relative_path: str | None = None,
            replay_toolset: dict[str, str] | None = None,
        ) -> dict[str, object]:
            if bundled_path.name == "libcom_err.so.2":
                return {"status": "not_found"}
            return {"status": "not_found"}

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_captured_provenance",
            side_effect=fake_bind,
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
                ]
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

        def fake_bind(
            bundled_path: Path, replay_toolset: dict[str, str] | None = None
        ) -> dict[str, object]:
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
                "dpkgFileIntegrity": "unavailable",
                "systemPath": str((system_dir / "libfoo.so.1").resolve()),
                "systemSha256": audit._sha256(system_dir / "libfoo.so.1"),
                "bundledCanonicalLoadDigest": "same-digest",
                "evidenceStrength": "canonical_digest_heuristic",
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

    def test_validate_bundled_provenance_rejects_architecture_mismatch(self) -> None:
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"],
                "sourcePackage": "e2fsprogs",
                "architecture": "i386",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("architecture", problem)

    def test_validate_bundled_provenance_rejects_version_revision_drift(
        self,
    ) -> None:
        # Round-7 review ("pin package revision/snapshot"): even a
        # matched, content-proven, correctly-source-packaged provenance
        # must still fail if its real installed version has drifted
        # away from the pinned COMPONENT_EXPECTED_SOURCE_VERSION
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

    def test_validate_bundled_provenance_rejects_version_revision_drift_even_for_a_routine_point_release(
        self,
    ) -> None:
        # Round-N+ review follow-up (HIGH, "`startswith(expected_
        # version_prefix)` lets 1.46.50 satisfy 1.46.5; security
        # revision drift"): this project's OLD prefix-based design used
        # to deliberately TOLERATE exactly this shape of drift (a
        # routine Debian-revision-only point release/security rebuild,
        # e.g. "-2ubuntu1.2" -> "-2ubuntu1.9") -- precisely the gap the
        # review named as unsafe, since a live security update silently
        # changing the exact bytes this project ships must be a
        # reviewable diff, never silently absorbed. The fixed, exact-
        # equality COMPONENT_EXPECTED_SOURCE_VERSION comparator now
        # rejects this too, exactly as it rejects a genuine upstream
        # revision bump.
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": "1.46.5-2ubuntu1.9",
                "sourcePackage": "e2fsprogs",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)

    def test_validate_bundled_provenance_rejects_version_prefix_collision(
        self,
    ) -> None:
        # Round-N+ review follow-up (HIGH, "`startswith(expected_
        # version_prefix)` lets 1.46.50 satisfy 1.46.5"): the exact,
        # literal-string bug this project's OLD prefix-based comparator
        # had -- "1.46.50-1ubuntu1".startswith("1.46.5") is True even
        # though "1.46.50" is a completely different, unreviewed
        # upstream version that merely happens to share "1.46.5" as a
        # literal string prefix, never a real point release of it. The
        # fixed, exact-equality comparator cannot be fooled by this: an
        # entirely different complete version string can never equal
        # the pinned one.
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": "1.46.50-1ubuntu1",
                "sourcePackage": "e2fsprogs",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("e2fsprogs", problem)

    def test_validate_bundled_provenance_accepts_the_exact_pinned_version(
        self,
    ) -> None:
        # The flip side of both drift tests above: the COMPLETE,
        # correctly pinned version string (exactly what
        # COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"] names) must
        # still pass.
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs",
                {
                    "status": "matched",
                    "package": "libcom-err2",
                    "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"],
                    "sourcePackage": "e2fsprogs",
                },
            )
        )

    def test_validate_bundled_provenance_requires_a_pinned_lock_entry_when_provenance_is_required(
        self,
    ) -> None:
        with mock.patch.object(
            audit,
            "_load_distro_package_lock",
            return_value={"distribution": "ubuntu-22.04", "packages": {}},
        ):
            problem = audit.validate_bundled_library_package_provenance(
                "zlib",
                {
                    "status": "matched",
                    "package": "zlib1g",
                    "version": "1:1.2.11.dfsg-2ubuntu9.2",
                    "sourcePackage": "zlib",
                    "architecture": "amd64",
                    "debSha256": "abc123",
                    "debArchiveVerification": "verified",
                    "evidenceStrength": "replay_byte_identical",
                },
                require_provenance=True,
            )
        self.assertIsNotNone(problem)
        self.assertIn("distro_package_lock.json", problem)
        self.assertIn("zlib1g", problem)

    def test_validate_bundled_provenance_rejects_a_locked_deb_sha256_mismatch(
        self,
    ) -> None:
        with mock.patch.object(
            audit,
            "_load_distro_package_lock",
            return_value={
                "distribution": "ubuntu-22.04",
                "packages": {
                    "zlib1g": {
                        "architecture": "amd64",
                        "version": "1:1.2.11.dfsg-2ubuntu9.2",
                        "debSha256": "expected-lock-sha",
                    }
                },
            },
        ):
            problem = audit.validate_bundled_library_package_provenance(
                "zlib",
                {
                    "status": "matched",
                    "package": "zlib1g",
                    "version": "1:1.2.11.dfsg-2ubuntu9.2",
                    "sourcePackage": "zlib",
                    "architecture": "amd64",
                    "debSha256": "different-live-sha",
                    "debArchiveVerification": "verified",
                    "evidenceStrength": "replay_byte_identical",
                },
                require_provenance=True,
            )
        self.assertIsNotNone(problem)
        self.assertIn("different-live-sha", problem)
        self.assertIn("expected-lock-sha", problem)

    def test_validate_bundled_provenance_rejects_metadata_mismatch_unconditionally(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "actual archive not downloaded/hashed/
        # extracted"): a freshly downloaded, self-hashed archive that
        # disagrees with this host's own local APT index metadata is a
        # hard failure regardless of --require-package-provenance --
        # neither value can be trusted until resolved.
        problem = audit.validate_bundled_library_package_provenance(
            "zlib",
            {
                "status": "matched",
                "package": "zlib1g",
                "version": "1:1.2.11.dfsg-2ubuntu9.2",
                "sourcePackage": "zlib",
                "architecture": "amd64",
                "debArchiveVerification": "metadataMismatch",
                "evidenceStrength": "replay_byte_identical",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("zlib1g", problem)

    def test_validate_bundled_provenance_rejects_archive_content_mismatch_unconditionally(
        self,
    ) -> None:
        # A live installed file that disagrees with the real archive's
        # own extracted bytes is a hard failure unconditionally -- the
        # live file has diverged from what the real distro archive
        # actually contains.
        problem = audit.validate_bundled_library_package_provenance(
            "zlib",
            {
                "status": "matched",
                "package": "zlib1g",
                "version": "1:1.2.11.dfsg-2ubuntu9.2",
                "sourcePackage": "zlib",
                "architecture": "amd64",
                "debArchiveVerification": "mismatch",
                "evidenceStrength": "replay_byte_identical",
            },
        )
        self.assertIsNotNone(problem)
        self.assertIn("zlib1g", problem)

    def test_validate_bundled_provenance_allows_unavailable_archive_verification_when_not_required(
        self,
    ) -> None:
        # An offline host/sandbox that cannot reach the network to
        # download a real archive must not spuriously fail merely for
        # lacking the STRONGER archive-download proof, unless
        # --require-package-provenance explicitly demands it.
        self.assertIsNone(
            audit.validate_bundled_library_package_provenance(
                "e2fsprogs",
                {
                    "status": "matched",
                    "package": "libcom-err2",
                    "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"],
                    "sourcePackage": "e2fsprogs",
                    "debArchiveVerification": "unavailable",
                },
            )
        )

    def test_validate_bundled_provenance_rejects_missing_archive_verification_when_required(
        self,
    ) -> None:
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"],
                "sourcePackage": "e2fsprogs",
                "architecture": "amd64",
                "evidenceStrength": "replay_byte_identical",
            },
            require_provenance=True,
        )
        self.assertIsNotNone(problem)
        self.assertIn("libcom-err2", problem)

    def test_validate_bundled_provenance_rejects_unavailable_archive_verification_when_required(
        self,
    ) -> None:
        problem = audit.validate_bundled_library_package_provenance(
            "e2fsprogs",
            {
                "status": "matched",
                "package": "libcom-err2",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["e2fsprogs"],
                "sourcePackage": "e2fsprogs",
                "architecture": "amd64",
                "debArchiveVerification": "unavailable",
                "evidenceStrength": "replay_byte_identical",
            },
            require_provenance=True,
        )
        self.assertIsNotNone(problem)
        self.assertIn("libcom-err2", problem)


class RealArchiveDownloadAndExtractionTests(unittest.TestCase):
    """Round-N+ review (HIGH, "distro provenance ... locked .deb SHA is
    copied from apt metadata; actual archive not downloaded/hashed/
    extracted, no signed snapshot, graph initially reads mutable paths,
    linuxdeploy automatic closure can reopen host"): proves
    _download_and_hash_deb_archive()/_extract_governed_file_from_deb_
    archive()/_dpkg_full_provenance_record()'s new archive-download
    authentication end-to-end -- both a REAL network download (skipped
    gracefully offline, matching this module's own established "skip
    when a real precondition is absent" convention) and fully
    deterministic mocked coverage of every failure/mismatch outcome, so
    the negative paths are never dependent on network availability."""

    def _real_libz_path(self) -> Path | None:
        for search_dir in audit._SYSTEM_LIBRARY_SEARCH_DIRS:
            candidate = search_dir / "libz.so.1"
            if candidate.exists():
                return candidate.resolve()
        return None

    def test_download_and_hash_returns_none_when_apt_get_missing(self) -> None:
        # apt-get missing AND the Launchpad snapshot fallback (see
        # LaunchpadArchiveFallbackTests below) also honestly cannot
        # verify here (mocked so this test never depends on real
        # network availability) -- overall result must be None.
        with mock.patch.object(
            audit.shutil, "which", return_value=None
        ), mock.patch.object(
            audit, "_download_and_hash_deb_archive_via_launchpad", return_value=None
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive.__wrapped__(
                    "zlib1g", "1:1.2.11-1", "amd64"
                )
            )

    def test_download_and_hash_returns_none_on_download_failure(self) -> None:
        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/apt-get"
        ), mock.patch.object(
            audit.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=[], returncode=1, stdout="", stderr="404 Not Found"
            ),
        ), mock.patch.object(
            audit, "_download_and_hash_deb_archive_via_launchpad", return_value=None
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive.__wrapped__(
                    "zlib1g", "1:1.2.11-1", "amd64"
                )
            )

    def test_download_and_hash_returns_none_when_zero_or_multiple_debs_produced(
        self,
    ) -> None:
        def fake_run(cmd, cwd=None, capture_output=True, text=True, timeout=None):
            # Simulate apt-get download succeeding but writing an
            # unexpected number of .deb files (e.g. a multi-arch
            # resolution ambiguity) -- must be an honest "cannot
            # verify", never a silent pick-one.
            (Path(cwd) / "one.deb").write_bytes(b"\x00")
            (Path(cwd) / "two.deb").write_bytes(b"\x00")
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/apt-get"
        ), mock.patch.object(
            audit.subprocess, "run", side_effect=fake_run
        ), mock.patch.object(
            audit, "_download_and_hash_deb_archive_via_launchpad", return_value=None
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive.__wrapped__(
                    "zlib1g", "1:1.2.11-1", "amd64"
                )
            )

    def test_download_and_hash_falls_back_to_launchpad_when_apt_get_download_fails(
        self,
    ) -> None:
        # Round-N+ review ("no signed snapshot ... locked .deb SHA is
        # copied from apt metadata"): reproduces the real production
        # failure this project's own CI hit -- the live apt mirror no
        # longer serves the exact pre-installed version (superseded by
        # a newer security upload) -- and proves the Launchpad snapshot
        # fallback is actually exercised and its result is what gets
        # returned, not silently ignored.
        fake_deb_path = Path(tempfile.mkdtemp()) / "libblkid1_2.37.2-4ubuntu3.5_amd64.deb"
        fake_deb_path.write_bytes(b"real-launchpad-archive-bytes")
        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/apt-get"
        ), mock.patch.object(
            audit.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                args=[], returncode=1, stdout="", stderr="404  Not Found"
            ),
        ), mock.patch.object(
            audit,
            "_download_and_hash_deb_archive_via_launchpad",
            return_value=(
                fake_deb_path,
                hashlib.sha256(b"real-launchpad-archive-bytes").hexdigest(),
            ),
        ) as fake_launchpad:
            result = audit._download_and_hash_deb_archive.__wrapped__(
                "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
            )
        fake_launchpad.assert_called_once_with(
            "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
        )
        self.assertIsNotNone(result)
        deb_path, deb_sha256 = result
        self.assertEqual(deb_path, fake_deb_path)
        self.assertEqual(
            deb_sha256, hashlib.sha256(b"real-launchpad-archive-bytes").hexdigest()
        )

    def test_download_and_hash_returns_our_own_sha256_of_downloaded_bytes(
        self,
    ) -> None:
        def fake_run(cmd, cwd=None, capture_output=True, text=True, timeout=None):
            (Path(cwd) / "zlib1g_1%3a1.2.11-1_amd64.deb").write_bytes(
                b"totally-fake-but-deterministic-archive-bytes"
            )
            return subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/apt-get"
        ), mock.patch.object(audit.subprocess, "run", side_effect=fake_run):
            result = audit._download_and_hash_deb_archive.__wrapped__(
                "zlib1g", "1:1.2.11-1", "amd64"
            )
        self.assertIsNotNone(result)
        deb_path, deb_sha256 = result
        self.assertEqual(
            deb_sha256,
            hashlib.sha256(
                b"totally-fake-but-deterministic-archive-bytes"
            ).hexdigest(),
        )
        self.assertEqual(deb_path.read_bytes(), b"totally-fake-but-deterministic-archive-bytes")

    def test_extract_governed_file_returns_none_when_dpkg_deb_missing(self) -> None:
        with mock.patch.object(audit.shutil, "which", return_value=None):
            self.assertIsNone(
                audit._extract_governed_file_from_deb_archive(
                    Path("/nonexistent.deb"), frozenset({"lib/libfoo.so.1"})
                )
            )

    def test_extract_governed_file_returns_none_when_member_absent(self) -> None:
        with tempfile.TemporaryDirectory() as scratch_dir:
            deb_path = Path(scratch_dir) / "fake.deb"
            deb_path.write_bytes(b"\x00")
            with mock.patch.object(
                audit.shutil, "which", return_value="/usr/bin/dpkg-deb"
            ), mock.patch.object(
                audit.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    args=[], returncode=0, stdout=b"", stderr=b""
                ),
            ):
                self.assertIsNone(
                    audit._extract_governed_file_from_deb_archive(
                        deb_path, frozenset({"lib/libfoo.so.1"})
                    )
                )

    def test_extract_governed_file_returns_real_bytes_from_a_real_tar_stream(
        self,
    ) -> None:
        import tarfile as tarfile_module

        with tempfile.TemporaryDirectory() as scratch_dir:
            tar_bytes_io = io.BytesIO()
            with tarfile_module.open(fileobj=tar_bytes_io, mode="w") as archive:
                info = tarfile_module.TarInfo(
                    name="./usr/lib/x86_64-linux-gnu/libfoo.so.1"
                )
                payload = b"real-payload-bytes"
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
            deb_path = Path(scratch_dir) / "fake.deb"
            deb_path.write_bytes(b"\x00")
            with mock.patch.object(
                audit.shutil, "which", return_value="/usr/bin/dpkg-deb"
            ), mock.patch.object(
                audit.subprocess,
                "run",
                return_value=subprocess.CompletedProcess(
                    args=[],
                    returncode=0,
                    stdout=tar_bytes_io.getvalue(),
                    stderr=b"",
                ),
            ):
                extracted = audit._extract_governed_file_from_deb_archive(
                    deb_path,
                    frozenset({"usr/lib/x86_64-linux-gnu/libfoo.so.1"}),
                )
        self.assertEqual(extracted, b"real-payload-bytes")

    def test_dpkg_full_provenance_record_reports_metadata_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as scratch_dir:
            scratch_path = Path(scratch_dir) / "libfoo.so.1"
            scratch_path.write_bytes(b"live-file-bytes")
            with mock.patch.object(
                audit, "_dpkg_owning_package", return_value="libfoo1"
            ), mock.patch.object(
                audit,
                "_dpkg_package_metadata",
                return_value=("1.2.3-1", "foosource"),
            ), mock.patch.object(
                audit, "_dpkg_package_architecture", return_value="amd64"
            ), mock.patch.object(
                audit,
                "_apt_cache_package_record",
                return_value={"architecture": "amd64", "debSha256": "a" * 64},
            ), mock.patch.object(
                audit,
                "_download_and_hash_deb_archive",
                return_value=(Path(scratch_dir) / "fake.deb", "b" * 64),
            ):
                record = audit._dpkg_full_provenance_record(scratch_path)
        self.assertIsNotNone(record)
        self.assertEqual(record["debSha256"], "b" * 64)
        self.assertEqual(record["debSha256AptMetadata"], "a" * 64)
        self.assertEqual(record["debArchiveVerification"], "metadataMismatch")

    def test_dpkg_full_provenance_record_reports_archive_content_mismatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as scratch_dir:
            scratch_path = Path(scratch_dir) / "libfoo.so.1"
            scratch_path.write_bytes(b"live-file-bytes-tampered-locally")
            deb_path = Path(scratch_dir) / "fake.deb"
            deb_path.write_bytes(b"\x00")
            with mock.patch.object(
                audit, "_dpkg_owning_package", return_value="libfoo1"
            ), mock.patch.object(
                audit,
                "_dpkg_package_metadata",
                return_value=("1.2.3-1", "foosource"),
            ), mock.patch.object(
                audit, "_dpkg_package_architecture", return_value="amd64"
            ), mock.patch.object(
                audit,
                "_apt_cache_package_record",
                return_value={"architecture": "amd64", "debSha256": "c" * 64},
            ), mock.patch.object(
                audit,
                "_download_and_hash_deb_archive",
                return_value=(deb_path, "c" * 64),
            ), mock.patch.object(
                audit,
                "_extract_governed_file_from_deb_archive",
                return_value=b"real-archive-bytes-different-from-live-file",
            ):
                record = audit._dpkg_full_provenance_record(scratch_path)
        self.assertIsNotNone(record)
        self.assertEqual(record["debArchiveVerification"], "mismatch")

    def test_dpkg_full_provenance_record_reports_verified_when_bytes_agree(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as scratch_dir:
            live_bytes = b"identical-bytes-both-places"
            scratch_path = Path(scratch_dir) / "libfoo.so.1"
            scratch_path.write_bytes(live_bytes)
            deb_path = Path(scratch_dir) / "fake.deb"
            deb_path.write_bytes(b"\x00")
            with mock.patch.object(
                audit, "_dpkg_owning_package", return_value="libfoo1"
            ), mock.patch.object(
                audit,
                "_dpkg_package_metadata",
                return_value=("1.2.3-1", "foosource"),
            ), mock.patch.object(
                audit, "_dpkg_package_architecture", return_value="amd64"
            ), mock.patch.object(
                audit,
                "_apt_cache_package_record",
                return_value={"architecture": "amd64", "debSha256": "d" * 64},
            ), mock.patch.object(
                audit,
                "_download_and_hash_deb_archive",
                return_value=(deb_path, "d" * 64),
            ), mock.patch.object(
                audit,
                "_extract_governed_file_from_deb_archive",
                return_value=live_bytes,
            ):
                record = audit._dpkg_full_provenance_record(scratch_path)
        self.assertIsNotNone(record)
        self.assertEqual(record["debArchiveVerification"], "verified")
        self.assertEqual(record["debSha256"], "d" * 64)

    def test_real_download_and_extract_against_the_real_installed_libz_package(
        self,
    ) -> None:
        # Real end-to-end proof (no mocks at all): download the ACTUAL
        # installed libz package's .deb over the network, hash it
        # ourselves, extract the real libz.so.1(.x) member from inside
        # it, and confirm those bytes agree with the real, live
        # installed file. Gracefully skipped offline/without apt-get,
        # matching this module's own established convention.
        if shutil.which("apt-get") is None or shutil.which("dpkg-deb") is None:
            self.skipTest("apt-get/dpkg-deb unavailable")
        libz_path = self._real_libz_path()
        if libz_path is None:
            self.skipTest("no real libz.so.1 system copy found on this host")
        package = audit._dpkg_owning_package(libz_path)
        if package is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        metadata = audit._dpkg_package_metadata(package)
        if metadata is None:
            self.skipTest(f"no package metadata available for {package!r}")
        version, _source_package = metadata
        architecture = audit._dpkg_package_architecture(package)
        if architecture is None:
            self.skipTest(f"no architecture available for {package!r}")
        downloaded = audit._download_and_hash_deb_archive(
            package, version, architecture
        )
        if downloaded is None:
            self.skipTest(
                "real 'apt-get download' did not succeed in this environment "
                "(offline sandbox/no network egress)"
            )
        deb_path, _deb_sha256 = downloaded
        relative_candidates = audit._merged_usr_relative_path_candidates(libz_path)
        extracted = audit._extract_governed_file_from_deb_archive(
            deb_path, relative_candidates
        )
        if extracted is None:
            self.skipTest(
                "could not locate the real libz.so.1 member inside the "
                "downloaded archive on this host's layout"
            )
        self.assertEqual(
            hashlib.sha256(extracted).hexdigest(),
            hashlib.sha256(libz_path.read_bytes()).hexdigest(),
        )


class LaunchpadArchiveFallbackTests(unittest.TestCase):
    """Round-N+ review (HIGH, "distro provenance ... no signed
    snapshot"): a real, currently-enabled apt mirror only ever serves
    whatever exact version is its CURRENT candidate -- Ubuntu's own
    archive routinely supersedes and prunes an older patch version's
    real `.deb` within roughly a day of a newer security upload, even
    though that older version is very often still the one actually
    pre-installed on a not-yet-refreshed CI runner image. This project's
    own real CI hit exactly that race for libblkid1/libmount1/libuuid1
    (all from source package util-linux): Launchpad's own publishing
    history confirmed those exact binaries were marked "Superseded"
    mere hours before the run. _download_and_hash_deb_archive_via_
    launchpad() is the pinned, queryable snapshot fallback for that gap
    -- these tests cover its mocked API/build/file-download plumbing
    end-to-end (deterministic, no network dependency), plus one real
    network smoke test proving the real API actually resolves this
    project's own pinned libblkid1 version to the identical bytes
    packaging/distro_package_lock.json already governs."""

    class _JsonResponse:
        def __init__(self, payload: object) -> None:
            self._body = json.dumps(payload).encode()

        def __enter__(self) -> "LaunchpadArchiveFallbackTests._JsonResponse":
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self) -> bytes:
            return self._body

    class _BytesResponse:
        def __init__(self, body: bytes) -> None:
            self._body = body

        def __enter__(self) -> "LaunchpadArchiveFallbackTests._BytesResponse":
            return self

        def __exit__(self, *_args: object) -> None:
            return None

        def read(self) -> bytes:
            return self._body

    def _fake_urlopen_for(self, deb_bytes: bytes):
        api_prefix = "https://api.launchpad.net/1.0/ubuntu/+archive/primary?"
        build_link = "https://api.launchpad.net/1.0/~ubuntu-security/+build/1"
        build_web_link = "https://launchpad.net/~ubuntu-security/+build/1"

        def fake_urlopen(url: str, timeout: float | None = None):
            if url.startswith(api_prefix):
                return self._JsonResponse(
                    {
                        "entries": [
                            {
                                "distro_arch_series_link": "https://api.launchpad.net/1.0/ubuntu/jammy/s390x",
                                "build_link": "https://api.launchpad.net/1.0/~ubuntu-security/+build/wrong-arch",
                            },
                            {
                                "distro_arch_series_link": "https://api.launchpad.net/1.0/ubuntu/jammy/amd64",
                                "build_link": build_link,
                            },
                        ]
                    }
                )
            if url == build_link:
                return self._JsonResponse({"web_link": build_web_link})
            if url == f"{build_web_link}/+files/libblkid1_2.37.2-4ubuntu3.5_amd64.deb":
                return self._BytesResponse(deb_bytes)
            raise AssertionError(f"unexpected urlopen({url!r})")

        return fake_urlopen

    def test_returns_none_for_a_distribution_with_no_series_mapping(self) -> None:
        with mock.patch.object(
            audit,
            "_load_distro_package_lock",
            return_value={"distribution": "debian-99", "packages": {}},
        ):
            self.assertIsNone(
                audit._launchpad_series_for_distro_package_lock()
            )
        with mock.patch.object(
            audit,
            "_launchpad_series_for_distro_package_lock",
            return_value=None,
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive_via_launchpad.__wrapped__(
                    "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
                )
            )

    def test_maps_the_pinned_lock_distribution_to_its_launchpad_series(self) -> None:
        self.assertEqual(
            audit._launchpad_series_for_distro_package_lock(), "jammy"
        )

    def test_finds_the_matching_architecture_build_and_downloads_the_real_bytes(
        self,
    ) -> None:
        deb_bytes = b"real-immutable-launchpad-archive-bytes"
        with mock.patch.object(
            audit, "_launchpad_series_for_distro_package_lock", return_value="jammy"
        ), mock.patch.object(
            audit.urllib.request,
            "urlopen",
            side_effect=self._fake_urlopen_for(deb_bytes),
        ):
            result = audit._download_and_hash_deb_archive_via_launchpad.__wrapped__(
                "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
            )
        self.assertIsNotNone(result)
        deb_path, deb_sha256 = result
        self.assertEqual(deb_path.read_bytes(), deb_bytes)
        self.assertEqual(deb_sha256, hashlib.sha256(deb_bytes).hexdigest())

    def test_returns_none_when_no_entry_matches_the_requested_architecture(
        self,
    ) -> None:
        def fake_urlopen(url: str, timeout: float | None = None):
            return self._JsonResponse(
                {
                    "entries": [
                        {
                            "distro_arch_series_link": "https://api.launchpad.net/1.0/ubuntu/jammy/s390x",
                            "build_link": "https://api.launchpad.net/1.0/~x/+build/1",
                        }
                    ]
                }
            )

        with mock.patch.object(
            audit, "_launchpad_series_for_distro_package_lock", return_value="jammy"
        ), mock.patch.object(
            audit.urllib.request, "urlopen", side_effect=fake_urlopen
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive_via_launchpad.__wrapped__(
                    "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
                )
            )

    def test_returns_none_on_any_network_failure(self) -> None:
        with mock.patch.object(
            audit, "_launchpad_series_for_distro_package_lock", return_value="jammy"
        ), mock.patch.object(
            audit.urllib.request,
            "urlopen",
            side_effect=OSError("network unreachable"),
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive_via_launchpad.__wrapped__(
                    "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
                )
            )

    def test_returns_none_on_malformed_json_response(self) -> None:
        class _BadResponse:
            def __enter__(self) -> "_BadResponse":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def read(self) -> bytes:
                return b"not-json"

        with mock.patch.object(
            audit, "_launchpad_series_for_distro_package_lock", return_value="jammy"
        ), mock.patch.object(
            audit.urllib.request, "urlopen", return_value=_BadResponse()
        ):
            self.assertIsNone(
                audit._download_and_hash_deb_archive_via_launchpad.__wrapped__(
                    "libblkid1", "2.37.2-4ubuntu3.5", "amd64"
                )
            )

    def test_real_network_finds_the_exact_pinned_libblkid1_bytes(self) -> None:
        # Real end-to-end proof (no mocks): resolves this project's own
        # already-pinned libblkid1 version against the REAL Launchpad
        # API and confirms the downloaded archive's sha256 matches
        # packaging/distro_package_lock.json's own governed pin --
        # gracefully skipped without network egress, matching this
        # module's own established convention for real-network tests.
        lock = audit._load_distro_package_lock()
        locked_entry = lock["packages"].get("libblkid1")
        if not isinstance(locked_entry, dict):
            self.skipTest("no pinned libblkid1 entry in distro_package_lock.json")
        result = audit._download_and_hash_deb_archive_via_launchpad(
            "libblkid1", locked_entry["version"], locked_entry["architecture"]
        )
        if result is None:
            self.skipTest(
                "real Launchpad API/download did not succeed in this "
                "environment (offline sandbox/no network egress)"
            )
        _deb_path, deb_sha256 = result
        self.assertEqual(deb_sha256, locked_entry["debSha256"])


class CaptureBeforePackagingProvenanceTests(unittest.TestCase):
    """Round-N+ review (HIGH, "distro provenance post-hoc/unpinned:
    after packaging it searches fixed system dirs by basename, not
    exact linuxdeploy-selected pre-copy file ... capture exact loader/
    copy source BEFORE packaging ... No basename re-discovery. Apply
    every distro component. Tests must substitute a same-basename
    library and reject absent/wrong/revision-drift provenance"):
    portable (no real dpkg/readelf/apt needed -- the DT_NEEDED parser,
    ld.so-cache lookup, and dpkg/APT metadata lookups are mocked
    directly) tests of resolve_ldd_dependencies()'s compatibility
    wrapper, resolve_dt_needed_dependency_graph(), capture_distro_
    source_provenance(), bind_bundled_library_to_captured_provenance(),
    and load_distro_provenance_manifest()."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmp_path = Path(self._tmp.name)

    def test_resolve_ldd_dependencies_returns_empty_when_readelf_cannot_inspect_the_requester(
        self,
    ) -> None:
        elf_path = self.tmp_path / "app"
        elf_path.write_bytes(b"\x7fELF")
        with mock.patch.object(
            audit, "_read_dynamic_tags", side_effect=audit.ElfIdentityError("boom")
        ):
            self.assertEqual(audit.resolve_ldd_dependencies(elf_path), {})

    def test_resolve_ldd_dependencies_resolves_dt_needed_entries_via_ldconfig_cache(
        self,
    ) -> None:
        elf_path = self.tmp_path / "app"
        elf_path.write_bytes(b"\x7fELF")
        resolved_path = self.tmp_path / "system" / "libfoo.so.1"
        resolved_path.parent.mkdir()
        resolved_path.write_bytes(b"\x00")
        with mock.patch.object(
            audit,
            "_read_dynamic_tags",
            return_value=[("NEEDED", "Shared library: [libfoo.so.1]")],
        ), mock.patch.object(
            audit,
            "_load_ldconfig_library_cache",
            return_value={"libfoo.so.1": (resolved_path,)},
        ):
            resolved = audit.resolve_ldd_dependencies(elf_path)
        self.assertEqual(resolved, {"libfoo.so.1": resolved_path})

    def test_resolve_ldd_dependencies_prefers_requesters_runpath_before_ldconfig_cache(
        self,
    ) -> None:
        requester_dir = self.tmp_path / "requester"
        private_dir = requester_dir / "private"
        private_dir.mkdir(parents=True)
        cached_dir = self.tmp_path / "cached"
        cached_dir.mkdir()
        elf_path = requester_dir / "app"
        elf_path.write_bytes(b"\x7fELF")
        runpath_candidate = private_dir / "libfoo.so.1"
        runpath_candidate.write_bytes(b"runpath")
        cached_candidate = cached_dir / "libfoo.so.1"
        cached_candidate.write_bytes(b"cached")
        with mock.patch.object(
            audit,
            "_read_dynamic_tags",
            return_value=[
                ("NEEDED", "Shared library: [libfoo.so.1]"),
                ("RUNPATH", "Library runpath: [$ORIGIN/private]"),
            ],
        ), mock.patch.object(
            audit,
            "_load_ldconfig_library_cache",
            return_value={"libfoo.so.1": (cached_candidate,)},
        ):
            resolved = audit.resolve_ldd_dependencies(elf_path)
        self.assertEqual(
            resolved["libfoo.so.1"].resolve(), runpath_candidate.resolve()
        )

    def test_resolve_ldd_dependencies_returns_empty_when_no_needed_entry_resolves(
        self,
    ) -> None:
        elf_path = self.tmp_path / "app"
        elf_path.write_bytes(b"\x7fELF")
        with mock.patch.object(
            audit,
            "_read_dynamic_tags",
            return_value=[("NEEDED", "Shared library: [libmissing.so.1]")],
        ), mock.patch.object(audit, "_load_ldconfig_library_cache", return_value={}):
            self.assertEqual(audit.resolve_ldd_dependencies(elf_path), {})

    def test_capture_distro_source_provenance_captures_full_identity(self) -> None:
        resolved_path = self.tmp_path / "libfoo.so.1"
        resolved_path.write_bytes(b"\x00\x01")
        with mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit,
            "_dpkg_package_metadata",
            return_value=("1.2.3-1ubuntu1", "foosource"),
        ):
            captured = audit.capture_distro_source_provenance(
                {"libfoo.so.1": [resolved_path]}
            )
        self.assertEqual(
            captured,
            {
                "libfoo.so.1": [
                    {
                        "path": str(resolved_path),
                        "sha256": audit._sha256(resolved_path),
                        "package": "libfoo1",
                        "version": "1.2.3-1ubuntu1",
                        "sourcePackage": "foosource",
                        "dpkgFileIntegrity": "unavailable",
                    }
                ]
            },
        )

    def test_capture_distro_source_provenance_omits_unresolvable_entries(
        self,
    ) -> None:
        # A resolved path that no longer exists, or is not dpkg-owned
        # at all (e.g. a project-vendored library `ldd` also happens to
        # resolve), is honestly omitted from that soname's candidate
        # list rather than fabricating a record -- exactly like every
        # other provenance mechanism in this module already does. The
        # soname key itself is still present, mapped to an empty list
        # (Round-N+ review, "distro provenance collapses identity"):
        # this lets a caller distinguish "we tried, but nothing was
        # dpkg-provable" from "this soname was never even a candidate".
        missing_path = self.tmp_path / "does-not-exist.so.1"
        unowned_path = self.tmp_path / "libunowned.so.1"
        unowned_path.write_bytes(b"\x00")
        with mock.patch.object(
            audit, "_dpkg_owning_package", return_value=None
        ):
            captured = audit.capture_distro_source_provenance(
                {"missing.so.1": [missing_path], "libunowned.so.1": [unowned_path]}
            )
        self.assertEqual(captured, {"missing.so.1": [], "libunowned.so.1": []})

    def test_capture_distro_source_provenance_preserves_distinct_requester_resolutions(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "distro provenance collapses identity
        # ... keyed SONAME/basename overwrites different requester
        # resolutions"): two different first-party requesters resolve
        # the SAME soname to two DIFFERENT real, dpkg-owned files --
        # both must survive as distinct candidates, never collapsed to
        # a single "winner".
        path_a = self.tmp_path / "a" / "libshared.so.1"
        path_a.parent.mkdir()
        path_a.write_bytes(b"\x00")
        path_b = self.tmp_path / "b" / "libshared.so.1"
        path_b.parent.mkdir()
        path_b.write_bytes(b"\x01")

        def fake_dpkg_owning_package(path: Path) -> str | None:
            return {path_a: "pkg-a", path_b: "pkg-b"}.get(path)

        def fake_dpkg_package_metadata(package: str) -> tuple[str, str]:
            return {
                "pkg-a": ("1.0-1", "source-a"),
                "pkg-b": ("2.0-1", "source-b"),
            }[package]

        with mock.patch.object(
            audit, "_dpkg_owning_package", side_effect=fake_dpkg_owning_package
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", side_effect=fake_dpkg_package_metadata
        ):
            captured = audit.capture_distro_source_provenance(
                {"libshared.so.1": [path_a, path_b]}
            )
        self.assertEqual(len(captured["libshared.so.1"]), 2)
        packages = {entry["package"] for entry in captured["libshared.so.1"]}
        self.assertEqual(packages, {"pkg-a", "pkg-b"})

    def test_bind_to_captured_provenance_returns_not_found_when_absent_from_manifest(
        self,
    ) -> None:
        bundled_lib = self.tmp_path / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        binding = audit.bind_bundled_library_to_captured_provenance(
            bundled_lib, {}
        )
        self.assertEqual(binding, {"status": "not_found"})

    def test_bind_to_captured_provenance_rejects_a_same_basename_decoy(self) -> None:
        # The exact scenario the review's own test list names: a
        # manifest entry for "libfoo.so.1" points at one real captured
        # path, but the CURRENT file at that captured path is a
        # completely different, substituted library -- this must never
        # be silently trusted merely because the basenames match.
        captured_path = self.tmp_path / "captured" / "libfoo.so.1"
        captured_path.parent.mkdir()
        captured_path.write_bytes(b"\x00\x01original")
        original_sha256 = audit._sha256(captured_path)
        # Substitute the captured path's content AFTER recording its
        # provenance -- simulating a decoy/TOCTOU swap between capture
        # time and this classify invocation.
        captured_path.write_bytes(b"\xff\xffdecoy-substituted-content")
        manifest = {
            "libfoo.so.1": [
                {
                    "path": str(captured_path),
                    "sha256": original_sha256,
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                }
            ]
        }
        bundled_lib = self.tmp_path / "bundled" / "libfoo.so.1"
        bundled_lib.parent.mkdir()
        bundled_lib.write_bytes(b"\x00\x01original")
        binding = audit.bind_bundled_library_to_captured_provenance(
            bundled_lib, manifest
        )
        self.assertEqual(binding["status"], "content_mismatch")
        self.assertIn("changed", binding["problem"])

    def test_bind_to_captured_provenance_returns_not_found_when_captured_path_missing(
        self,
    ) -> None:
        manifest = {
            "libfoo.so.1": [
                {
                    "path": str(self.tmp_path / "gone" / "libfoo.so.1"),
                    "sha256": "deadbeef",
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                }
            ]
        }
        bundled_lib = self.tmp_path / "libfoo.so.1"
        bundled_lib.write_bytes(b"\x00")
        binding = audit.bind_bundled_library_to_captured_provenance(
            bundled_lib, manifest
        )
        self.assertEqual(binding["status"], "content_mismatch")
        self.assertEqual(binding["candidateCount"], 1)

    def test_bind_to_captured_provenance_detects_content_mismatch_against_bundled(
        self,
    ) -> None:
        # The captured system file itself is unmodified since capture
        # (sha256 agrees), but its real compiled content genuinely
        # differs from the bundled file -- a substituted/downgraded
        # bundled library.
        captured_path = self.tmp_path / "captured" / "libfoo.so.1"
        captured_path.parent.mkdir()
        captured_path.write_bytes(b"\x00\x01")
        manifest = {
            "libfoo.so.1": [
                {
                    "path": str(captured_path),
                    "sha256": audit._sha256(captured_path),
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                }
            ]
        }
        bundled_lib = self.tmp_path / "bundled" / "libfoo.so.1"
        bundled_lib.parent.mkdir()
        bundled_lib.write_bytes(b"\x00\x01")
        with mock.patch.object(
            audit,
            "_canonical_load_digest",
            side_effect=lambda p: "digest-a" if p == bundled_lib else "digest-b",
        ):
            binding = audit.bind_bundled_library_to_captured_provenance(
                bundled_lib, manifest
            )
        self.assertEqual(binding["status"], "content_mismatch")
        self.assertEqual(binding["sourcePackage"], "foosource")

    def test_bind_to_captured_provenance_rejects_a_same_soname_replacement_even_when_the_manifest_pins_the_legitimate_basename(
        self,
    ) -> None:
        # Finding #18's explicit "same SONAME replacement" scenario:
        # the authoritative manifest proves usr/lib/libfoo.so.1 came
        # from one exact staged object, but the final bundled path now
        # carries bytes from a DIFFERENT libfoo.so.1 sharing the same
        # basename/SONAME. Exact-destination binding must reject this,
        # never silently accept it as "same name, close enough".
        staged_legitimate = self.tmp_path / "stage" / "aaaa--libfoo.so.1"
        staged_legitimate.parent.mkdir(parents=True)
        staged_legitimate.write_bytes(b"legitimate-libfoo")
        same_soname_replacement = self.tmp_path / "replacement" / "libfoo.so.1"
        same_soname_replacement.parent.mkdir()
        same_soname_replacement.write_bytes(b"replacement-with-same-soname")
        manifest = {
            "formatVersion": 2,
            "bundledPaths": {
                "usr/lib/libfoo.so.1": {
                    "bundledPath": "usr/lib/libfoo.so.1",
                    "stagedPath": str(staged_legitimate),
                    "sourceRealPath": "/lib/x86_64-linux-gnu/libfoo.so.1",
                    "sha256": audit._sha256(staged_legitimate),
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                }
            },
        }
        bundled_lib = self.tmp_path / "AppDir" / "usr" / "lib" / "libfoo.so.1"
        bundled_lib.parent.mkdir(parents=True)
        bundled_lib.write_bytes(same_soname_replacement.read_bytes())
        with mock.patch.object(
            audit,
            "_canonical_load_digest",
            side_effect=lambda path: (
                "digest-legitimate"
                if path == staged_legitimate
                else "digest-replacement"
                if path == bundled_lib
                else None
            ),
        ):
            binding = audit.bind_bundled_library_to_captured_provenance(
                bundled_lib, manifest, "usr/lib/libfoo.so.1"
            )
        self.assertEqual(binding["status"], "content_mismatch")
        self.assertEqual(binding["systemPath"], "/lib/x86_64-linux-gnu/libfoo.so.1")

    def test_bind_to_captured_provenance_matches_when_content_digests_agree(
        self,
    ) -> None:
        captured_path = self.tmp_path / "captured" / "libfoo.so.1"
        captured_path.parent.mkdir()
        captured_path.write_bytes(b"\x00\x01")
        manifest = {
            "libfoo.so.1": [
                {
                    "path": str(captured_path),
                    "sha256": audit._sha256(captured_path),
                    "package": "libfoo1",
                    "version": "1.2.3-1ubuntu1",
                    "sourcePackage": "foosource",
                }
            ]
        }
        bundled_lib = self.tmp_path / "bundled" / "libfoo.so.1"
        bundled_lib.parent.mkdir()
        bundled_lib.write_bytes(b"\x00\x01")
        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ):
            binding = audit.bind_bundled_library_to_captured_provenance(
                bundled_lib, manifest
            )
        self.assertEqual(
            binding,
            {
                "status": "matched",
                "path": str(captured_path),
                "sha256": audit._sha256(captured_path),
                "package": "libfoo1",
                "version": "1.2.3-1ubuntu1",
                "sourcePackage": "foosource",
                "systemPath": str(captured_path),
                "systemSha256": audit._sha256(captured_path),
                "bundledCanonicalLoadDigest": "same-digest",
            },
        )

    def test_bind_to_captured_provenance_matches_second_candidate_when_first_disagrees(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "distro provenance collapses identity
        # ... keyed SONAME/basename overwrites different requester
        # resolutions"): TWO distinct requesters resolved the same
        # soname to two different real files; the bundled copy only
        # needs to genuinely match ONE of them to be legitimate.
        first_candidate = self.tmp_path / "first" / "libshared.so.1"
        first_candidate.parent.mkdir()
        first_candidate.write_bytes(b"\x00\x01")
        second_candidate = self.tmp_path / "second" / "libshared.so.1"
        second_candidate.parent.mkdir()
        second_candidate.write_bytes(b"\x02\x03")
        manifest = {
            "libshared.so.1": [
                {
                    "path": str(first_candidate),
                    "sha256": audit._sha256(first_candidate),
                    "package": "pkg-a",
                    "version": "1.0-1",
                    "sourcePackage": "source-a",
                },
                {
                    "path": str(second_candidate),
                    "sha256": audit._sha256(second_candidate),
                    "package": "pkg-b",
                    "version": "2.0-1",
                    "sourcePackage": "source-b",
                },
            ]
        }
        bundled_lib = self.tmp_path / "bundled" / "libshared.so.1"
        bundled_lib.parent.mkdir()
        bundled_lib.write_bytes(b"\x02\x03")
        with mock.patch.object(
            audit,
            "_canonical_load_digest",
            side_effect=lambda p: {
                first_candidate: "digest-first",
                second_candidate: "digest-second",
                bundled_lib: "digest-second",
            }[p],
        ):
            binding = audit.bind_bundled_library_to_captured_provenance(
                bundled_lib, manifest
            )
        self.assertEqual(binding["status"], "matched")
        self.assertEqual(binding["sourcePackage"], "source-b")

    def test_build_distro_provenance_manifest_rejects_conflicting_same_basename_sources(
        self,
    ) -> None:
        # Cumulative review (independent re-review, HIGH, "same basename
        # force bundle"): two requester edges both claim they will land at
        # the SAME final bundled destination, but their actual source
        # objects differ. Capture must fail closed up front, never let one
        # silently overwrite the other by basename/SONAME.
        requester_a = self.tmp_path / "requester-a"
        requester_a.write_bytes(_FAKE_ELF_BYTES)
        requester_b = self.tmp_path / "requester-b"
        requester_b.write_bytes(_FAKE_ELF_BYTES)
        path_a = self.tmp_path / "a" / "libshared.so.1"
        path_a.parent.mkdir()
        path_a.write_bytes(b"first-real-source")
        path_b = self.tmp_path / "b" / "libshared.so.1"
        path_b.parent.mkdir()
        path_b.write_bytes(b"second-real-source")

        def fake_owner(path: Path) -> str | None:
            return {path_a.resolve(): "pkg-a", path_b.resolve(): "pkg-b"}.get(path)

        def fake_metadata(package: str) -> tuple[str, str]:
            return {
                "pkg-a": ("1.0-1", "source-a"),
                "pkg-b": ("1.0-1", "source-b"),
            }[package]

        with mock.patch.object(
            audit, "_dpkg_owning_package", side_effect=fake_owner
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", side_effect=fake_metadata
        ):
            manifest, conflicts = audit.build_distro_provenance_manifest(
                [
                    (requester_a, "libshared.so.1", path_a),
                    (requester_b, "libshared.so.1", path_b),
                ],
                self.tmp_path / "stage",
            )
        self.assertEqual(manifest["formatVersion"], 2)
        self.assertIn("usr/lib/libshared.so.1", conflicts[0])

    def test_bind_to_captured_provenance_uses_staged_copy_not_mutated_source_path(
        self,
    ) -> None:
        # Round-N+ review ("changed file after capture" / "hash/copy from
        # same nofollow descriptor or immutable staging object"): once
        # capture staged the exact source bytes, later mutations of the
        # original source path must not affect the binding result at all.
        requester = self.tmp_path / "requester"
        requester.write_bytes(_FAKE_ELF_BYTES)
        source_path = self.tmp_path / "system" / "libfoo.so.1"
        source_path.parent.mkdir()
        source_path.write_bytes(b"original-system-bytes")

        with mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", return_value=("1.2.3-1ubuntu1", "foosource")
        ):
            manifest, conflicts = audit.build_distro_provenance_manifest(
                [(requester, "libfoo.so.1", source_path)],
                self.tmp_path / "stage",
            )
        self.assertEqual(conflicts, [])
        source_path.write_bytes(b"mutated-after-capture")

        bundled_lib = self.tmp_path / "bundled" / "libfoo.so.1"
        bundled_lib.parent.mkdir()
        bundled_lib.write_bytes(b"original-system-bytes")
        binding = audit.bind_bundled_library_to_captured_provenance(
            bundled_lib, manifest, "usr/lib/libfoo.so.1"
        )
        self.assertEqual(binding["status"], "matched")
        self.assertNotEqual(binding["systemSha256"], audit._sha256(source_path))
        self.assertEqual(
            binding["systemSha256"],
            audit._sha256(Path(str(binding["stagedPath"]))),
        )

    def test_build_sbom_inventory_matches_real_distro_manifest_against_real_appdir_layout(
        self,
    ) -> None:
        # This segment's own real end-to-end build-appimage.sh run (the
        # first time any segment actually ran the full pipeline against
        # a real, produced distro-provenance-manifest.json instead of a
        # hand-keyed test fixture) discovered that EVERY real distro
        # component failed classify() with "not_found", despite the
        # manifest genuinely containing a fully-populated matching
        # entry. Root cause: build_sbom_inventory()'s manifest lookup
        # was keyed by `relative_path` -- the bundled path relative to
        # `lib_dir` -- while build-appimage.sh's own real invocation
        # passes `lib_dir="$app_dir/usr"` (see `bundled_search_root=
        # "$app_dir/usr"`), so `relative_path` for
        # "$app_dir/usr/lib/libfoo.so.1" is "lib/libfoo.so.1" -- missing
        # the leading "usr/" segment every real manifest key
        # (build_distro_provenance_manifest() always writes AppDir-ROOT-
        # relative "usr/lib/<basename>" keys) actually has. No prior
        # test exercised this exact production seam: every existing
        # manifest-binding test called
        # bind_bundled_library_to_captured_provenance() directly with a
        # hand-supplied (and coincidentally already-correct) key, and
        # every build_sbom_inventory()+distro_provenance_manifest test
        # used a `lib_dir` that was already the bundled file's own
        # immediate parent (so the missing "usr/" prefix never
        # surfaced). This test instead drives the REAL production shape
        # end-to-end: a real manifest built via
        # build_distro_provenance_manifest(), a real AppDir layout with
        # `usr/lib/`, and build_sbom_inventory() invoked with
        # `lib_dir=appdir/"usr"` exactly as cmd_classify() is invoked in
        # real CI/build-appimage.sh.
        requester = self.tmp_path / "requester"
        requester.write_bytes(_FAKE_ELF_BYTES)
        source_path = self.tmp_path / "system" / "libfoo.so.1"
        source_path.parent.mkdir()
        source_path.write_bytes(_FAKE_ELF_BYTES + b"-system-bytes")

        with mock.patch.object(
            audit, "_dpkg_owning_package", return_value="libfoo1"
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", return_value=("1.2.3-1ubuntu1", "foosource")
        ):
            manifest, conflicts = audit.build_distro_provenance_manifest(
                [(requester, "libfoo.so.1", source_path)],
                self.tmp_path / "stage",
            )
        self.assertEqual(conflicts, [])

        appdir = self.tmp_path / "AppDir"
        bundled_lib = appdir / "usr" / "lib" / "libfoo.so.1"
        bundled_lib.parent.mkdir(parents=True)
        bundled_lib.write_bytes(_FAKE_ELF_BYTES + b"-system-bytes")

        inventory = audit.build_sbom_inventory(
            appdir / "usr", distro_provenance_manifest=manifest
        )
        entry = next(e for e in inventory if e["basename"] == "libfoo.so.1")
        self.assertEqual(
            entry["packageProvenance"]["status"],
            "matched",
            entry["packageProvenance"],
        )

    def test_cmd_classify_end_to_end_matches_real_distro_manifest_against_real_appdir_layout(
        self,
    ) -> None:
        # Companion to the build_sbom_inventory() test above, proving
        # cmd_classify()'s OWN, independent provenance-validation loop
        # (a SECOND call site to
        # bind_bundled_library_to_captured_provenance() that carried the
        # exact same `path.relative_to(lib_dir)` key-shape bug, entirely
        # unaffected by the build_sbom_inventory() fix since it is a
        # separate code path) drives `main(["classify", ...])`
        # end-to-end with --require-package-provenance and a REAL
        # manifest exactly the way build-appimage.sh's own line-441
        # invocation does, and the real distro component's binding is
        # actually FOUND and content-matched (no false "not_found")
        # instead of failing every real distro component the way the
        # key-shape bug did.
        #
        # This test deliberately does not supply real linuxdeploy
        # patchelf/strip replay tooling, so --require-package-
        # provenance's SEPARATE, correctly-functioning "governed final
        # hash cannot be self-attested" replay-evidence-strength gate
        # (see validate_bundled_library_package_provenance()'s
        # evidenceStrength check) still legitimately fails here -- that
        # gate is unrelated to this bug and is exercised by this
        # project's own existing replay-evidence tests elsewhere. The
        # assertion below is therefore scoped precisely to the bug this
        # fix addresses: the false "not_found"/"none exists" text (the
        # symptom of the key-shape mismatch) must never appear, proving
        # the manifest lookup itself now succeeds.
        requester = self.tmp_path / "requester"
        requester.write_bytes(_FAKE_ELF_BYTES)
        zlib_source = self.tmp_path / "system" / "libz.so.1"
        zlib_source.parent.mkdir()
        zlib_source.write_bytes(_FAKE_ELF_BYTES + b"-zlib-system-bytes")
        avif_source = self.tmp_path / "system" / "libavif.so.13"
        avif_source.write_bytes(_FAKE_ELF_BYTES + b"-avif-system-bytes")

        def fake_owner(path: Path) -> str | None:
            return {
                zlib_source.resolve(): "zlib1g",
                avif_source.resolve(): "libavif13",
            }.get(path)

        def fake_metadata(package: str) -> tuple[str, str]:
            return {
                "zlib1g": ("1:1.2.11-1", "zlib"),
                "libavif13": ("0.11.1-1", "libavif"),
            }[package]

        with mock.patch.object(
            audit, "_dpkg_owning_package", side_effect=fake_owner
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", side_effect=fake_metadata
        ):
            manifest, conflicts = audit.build_distro_provenance_manifest(
                [
                    (requester, "libz.so.1", zlib_source),
                    (requester, "libavif.so.13", avif_source),
                ],
                self.tmp_path / "stage",
            )
        self.assertEqual(conflicts, [])
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text(json.dumps(manifest))

        appdir = self.tmp_path / "AppDir"
        usr_lib = appdir / "usr" / "lib"
        usr_lib.mkdir(parents=True)
        (usr_lib / "libz.so.1").write_bytes(_FAKE_ELF_BYTES + b"-zlib-system-bytes")
        # MANDATORY_COMPONENTS requires "libavif" to be present at all,
        # independent of this test's own zlib/distro-manifest focus.
        (usr_lib / "libavif.so.13").write_bytes(_FAKE_ELF_BYTES + b"-avif-system-bytes")

        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            audit.main(
                [
                    "classify",
                    str(appdir / "usr"),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
                ]
            )
        stderr_text = stderr.getvalue()
        self.assertNotIn("none exists", stderr_text, stderr_text)
        self.assertNotIn(
            "findable under _SYSTEM_LIBRARY_SEARCH_DIRS", stderr_text, stderr_text
        )

    def test_load_distro_provenance_manifest_round_trips_format_version_two_json(
        self,
    ) -> None:
        manifest = {
            "formatVersion": 2,
            "bundledPaths": {
                "usr/lib/libfoo.so.1": {
                    "bundledPath": "usr/lib/libfoo.so.1",
                    "stagedPath": "/stage/abc--libfoo.so.1",
                    "sourceRealPath": "/lib/libfoo.so.1",
                    "sha256": "abc",
                }
            },
        }
        manifest_path = self.tmp_path / "manifest-v2.json"
        manifest_path.write_text(json.dumps(manifest))
        self.assertEqual(audit.load_distro_provenance_manifest(manifest_path), manifest)

    def test_load_distro_provenance_manifest_round_trips_real_json(self) -> None:
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text(
            json.dumps({"libfoo.so.1": [{"path": "/x", "sha256": "abc"}]})
        )
        self.assertEqual(
            audit.load_distro_provenance_manifest(manifest_path),
            {"libfoo.so.1": [{"path": "/x", "sha256": "abc"}]},
        )

    def test_load_distro_provenance_manifest_rejects_missing_file(self) -> None:
        with self.assertRaises(ValueError):
            audit.load_distro_provenance_manifest(self.tmp_path / "nope.json")

    def test_load_distro_provenance_manifest_rejects_malformed_json(self) -> None:
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text("not json at all {{{")
        with self.assertRaises(ValueError):
            audit.load_distro_provenance_manifest(manifest_path)

    def test_load_distro_provenance_manifest_rejects_wrong_shape(self) -> None:
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text(json.dumps({"libfoo.so.1": "not-an-object"}))
        with self.assertRaises(ValueError):
            audit.load_distro_provenance_manifest(manifest_path)

    def test_cmd_classify_requires_provenance_requires_manifest(self) -> None:
        # Round-N+ review policy decision: --require-package-provenance
        # without --distro-provenance-manifest must fail closed as a
        # configuration error (for a real distro component that would
        # actually need it), not silently fall back to the after-the-
        # fact basename search this mechanism exists to prevent.
        lib_dir = self.tmp_path / "lib"
        lib_dir.mkdir()
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                ["classify", str(lib_dir), "--require-package-provenance"]
            )
        self.assertEqual(exit_code, 2)
        self.assertIn("--distro-provenance-manifest", stderr.getvalue())
        self.assertIn("libavif", stderr.getvalue())

    def test_cmd_classify_requires_provenance_allows_icu_only_without_manifest(
        self,
    ) -> None:
        # The flip side: --require-package-provenance on its own must
        # remain usable when the only components present are Qt-SDK-
        # provenance ones (currently only "icu"), since those never go
        # through the basename-search/captured-manifest binder at all
        # -- so no manifest is required in that case. MANDATORY_
        # COMPONENTS still requires "libavif" bundled too, so this test
        # supplies a real distro-provenance-manifest just for that
        # component to isolate the icu-only assertion.
        lib_dir = self.tmp_path / "lib"
        lib_dir.mkdir()
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text(json.dumps({}))
        stdout, stderr = io.StringIO(), io.StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
                ]
            )
        # libavif itself has no manifest entry, so provenance still
        # fails closed -- this test only proves the earlier
        # configuration-error short-circuit does not spuriously fire
        # once a manifest IS supplied.
        self.assertNotEqual(exit_code, 2)

    def test_cmd_classify_uses_captured_manifest_instead_of_system_search(
        self,
    ) -> None:
        # End-to-end proof that supplying a real
        # --distro-provenance-manifest routes provenance checks through
        # bind_bundled_library_to_captured_provenance() -- never
        # bind_bundled_library_to_system_provenance() -- for a
        # classified distro component.
        lib_dir = self.tmp_path / "lib"
        lib_dir.mkdir()
        (lib_dir / "libcom_err.so.2").write_bytes(_FAKE_ELF_BYTES)
        # MANDATORY_COMPONENTS requires "libavif" to be present under
        # any successful `classify` invocation -- unrelated to this
        # test's actual assertion, but needed for it to reach the
        # provenance-checking loop at all.
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)
        manifest_path = self.tmp_path / "manifest.json"
        manifest_path.write_text(json.dumps({}))
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_system_provenance",
            side_effect=AssertionError(
                "must not fall back to the basename-search binder when a "
                "captured manifest was supplied"
            ),
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--distro-provenance-manifest",
                    str(manifest_path),
                ]
            )
        self.assertEqual(exit_code, 0, stderr.getvalue())

    def test_cmd_capture_distro_provenance_captures_a_dlopen_only_force_bundled_input_itself(
        self,
    ) -> None:
        # Real `appimage-smoke` CI regression: libsecret-1.so.0 (and
        # every other force-bundled library build-appimage.sh passes
        # directly as an `elf_paths` argument specifically BECAUSE
        # linuxdeploy's automatic ldd-based bundling cannot discover it
        # on its own -- see that script's own comments) is never
        # DT_NEEDED-linked by anything this project bundles (QtKeychain
        # only ever dlopen()s it at runtime), so it never appears as a
        # *value* in resolve_ldd_dependencies()'s output for ANY input,
        # including its own. Before this fix, cmd_capture_distro_
        # provenance() only ever captured the *dependencies* of each
        # given ELF path, never the path itself, so a manifest built
        # from build-appimage.sh's own real, multi-input invocation
        # (main executable + several force-bundled libraries) silently
        # omitted libsecret-1.so.0 entirely -- reproduced here exactly:
        # the executable resolves one real dependency (as `ldd
        # arkham-horror` genuinely would), while the dlopen-only
        # libsecret input resolves to zero dependencies OF ITS OWN (as
        # `ldd libsecret-1.so.0` reporting only libsecret's own
        # dependencies, never "libsecret-1.so.0" itself, would).
        executable = self.tmp_path / "arkham-horror"
        executable.write_bytes(_FAKE_ELF_BYTES)
        libsecret_input = self.tmp_path / "libsecret-1.so.0"
        libsecret_input.write_bytes(_FAKE_ELF_BYTES)
        libc_path = self.tmp_path / "libc.so.6"
        libc_path.write_bytes(_FAKE_ELF_BYTES)
        output_path = self.tmp_path / "manifest.json"

        def fake_resolve_dt_needed_dependency_graph(
            elf_paths: list[Path] | tuple[Path, ...],
            ld_library_path: str | None = None,
        ) -> list[tuple[Path, str, Path]]:
            del ld_library_path
            self.assertEqual(tuple(elf_paths), (executable, libsecret_input))
            return [(executable, "libc.so.6", libc_path)]

        def fake_dpkg_owning_package(path: Path) -> str | None:
            return {
                libc_path.resolve(): "libc6",
                libsecret_input.resolve(): "libsecret-1-0",
            }.get(path)

        def fake_dpkg_package_metadata(package: str) -> tuple[str, str]:
            return {
                "libc6": ("2.35-0ubuntu3.8", "glibc"),
                "libsecret-1-0": ("0.20.5-2", "libsecret"),
            }[package]

        with mock.patch.object(
            audit,
            "resolve_dt_needed_dependency_graph",
            side_effect=fake_resolve_dt_needed_dependency_graph,
        ), mock.patch.object(
            audit, "_dpkg_owning_package", side_effect=fake_dpkg_owning_package
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", side_effect=fake_dpkg_package_metadata
        ):
            exit_code = audit.main(
                [
                    "capture-distro-provenance",
                    str(executable),
                    str(libsecret_input),
                    "--output",
                    str(output_path),
                ]
            )
        self.assertEqual(exit_code, 0)
        manifest = json.loads(output_path.read_text())
        self.assertEqual(manifest["formatVersion"], 2)
        self.assertIn(
            "usr/lib/libsecret-1.so.0",
            manifest["bundledPaths"],
            "the force-bundled input's own exact bundled destination must "
            "be captured even when resolve_ldd_dependencies() reports zero "
            "dependencies referencing it (dlopen-only, never DT_NEEDED-linked)",
        )
        libsecret_entry = manifest["bundledPaths"]["usr/lib/libsecret-1.so.0"]
        self.assertEqual(libsecret_entry["sourceRealPath"], str(libsecret_input.resolve()))
        self.assertEqual(libsecret_entry["package"], "libsecret-1-0")
        self.assertEqual(libsecret_entry["sourcePackage"], "libsecret")
        self.assertIn("usr/lib/libc.so.6", manifest["bundledPaths"])

    def test_cmd_capture_distro_provenance_self_entry_does_not_fabricate_ownership(
        self,
    ) -> None:
        # The flip side of the fix above: a first-party input with no
        # real dpkg owner at all (this project's own executable, or a
        # Qt plugin ELF file) must still be honestly OMITTED from the
        # manifest -- capturing every input's own basename as a
        # candidate entry must never bypass the real dpkg-ownership
        # check itself.
        first_party_input = self.tmp_path / "arkham-horror"
        first_party_input.write_bytes(_FAKE_ELF_BYTES)
        output_path = self.tmp_path / "manifest.json"
        with mock.patch.object(
            audit, "resolve_dt_needed_dependency_graph", return_value=[]
        ), mock.patch.object(audit, "_dpkg_owning_package", return_value=None):
            exit_code = audit.main(
                [
                    "capture-distro-provenance",
                    str(first_party_input),
                    "--output",
                    str(output_path),
                ]
            )
        # Zero real dependencies resolved and the self-entry itself is
        # unowned -- cmd_capture_distro_provenance() reports this as
        # its own honest "resolved zero dependencies" failure exit
        # code 1, exactly as it already does when given a dependency-
        # free input with no real distro ownership at all.
        self.assertEqual(exit_code, 1)

    def test_cmd_capture_distro_provenance_self_entry_does_not_override_real_dependency_resolution(
        self,
    ) -> None:
        # A self-entry must not silently discard a real, already-
        # resolved dependency captured for the SAME soname from
        # scanning a different ELF file -- it may only ever coincide
        # with (never contradict) the identical real system file.
        qt_plugin = self.tmp_path / "libqxcb.so"
        qt_plugin.write_bytes(_FAKE_ELF_BYTES)
        libz_input = self.tmp_path / "libz.so.1"
        libz_input.write_bytes(_FAKE_ELF_BYTES)
        output_path = self.tmp_path / "manifest.json"
        with mock.patch.object(
            audit,
            "resolve_dt_needed_dependency_graph",
            side_effect=lambda elf_paths, ld_library_path=None: (
                [(qt_plugin, "libz.so.1", libz_input)]
                if tuple(elf_paths) == (qt_plugin, libz_input)
                else []
            ),
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="zlib1g"
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", return_value=("1:1.2.11.dfsg-2ubuntu9", "zlib")
        ):
            exit_code = audit.main(
                [
                    "capture-distro-provenance",
                    str(qt_plugin),
                    str(libz_input),
                    "--output",
                    str(output_path),
                ]
            )
        self.assertEqual(exit_code, 0)
        manifest = json.loads(output_path.read_text())
        # Both the dependency-derived resolution (from scanning
        # qt_plugin) and the explicit self-entry resolve to the exact
        # same real path, so they dedupe into one bundled-destination
        # record carrying BOTH requester edges, never a second,
        # silently-overwriting basename candidate.
        entry = manifest["bundledPaths"]["usr/lib/libz.so.1"]
        self.assertEqual(entry["sourceRealPath"], str(libz_input.resolve()))
        self.assertEqual(len(entry["requesterEdges"]), 2)

    def test_cmd_capture_distro_provenance_rejects_conflicting_force_bundle_sources(
        self,
    ) -> None:
        executable = self.tmp_path / "arkham-horror"
        executable.write_bytes(_FAKE_ELF_BYTES)
        explicit_input = self.tmp_path / "force" / "libz.so.1"
        explicit_input.parent.mkdir()
        explicit_input.write_bytes(b"force-bundled")
        resolved_input = self.tmp_path / "resolved" / "libz.so.1"
        resolved_input.parent.mkdir()
        resolved_input.write_bytes(b"resolved-from-requester")
        output_path = self.tmp_path / "manifest.json"
        stdout, stderr = io.StringIO(), io.StringIO()

        with mock.patch.object(
            audit,
            "resolve_dt_needed_dependency_graph",
            side_effect=lambda elf_paths, ld_library_path=None: (
                [(executable, "libz.so.1", resolved_input)]
                if tuple(elf_paths) == (executable, explicit_input)
                else []
            ),
        ), mock.patch.object(
            audit, "_dpkg_owning_package", return_value="zlib1g"
        ), mock.patch.object(
            audit, "_dpkg_package_metadata", return_value=("1:1.2.11.dfsg-2ubuntu9", "zlib")
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "capture-distro-provenance",
                    str(executable),
                    str(explicit_input),
                    "--output",
                    str(output_path),
                ]
            )
        self.assertEqual(exit_code, 1)
        self.assertIn("conflicting distro provenance", stderr.getvalue())


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
        # This class's own fixtures are lightweight synthetic
        # directories, never byte-real copies of the actual installed
        # Qt SDK -- exactly like every other fixture in this module's
        # own _canonical_load_digest mocking convention. Default every
        # test in this class to a "matched" installed-tree
        # verification so existing digest/replay-focused assertions
        # are unaffected by Finding #6's new installed-tree check;
        # RealInstalledQtSdkTreeContentDigestTests below covers the
        # digest/verification functions themselves directly, with real
        # content (never mocked).
        patcher = mock.patch.object(
            audit,
            "compute_qt_sdk_tree_content_digest",
            return_value=audit._load_qt_sdk_lock()["installedTreeContentSha256"],
        )
        patcher.start()
        self.addCleanup(patcher.stop)

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
        binding = audit.bind_bundled_library_to_qt_sdk_provenance(bundled, None)
        self.assertEqual(binding["status"], "qt_reference_dir_unavailable")
        self.assertIn("allowedTransform", binding)
        self.assertEqual(
            binding["sdkSourceProvenance"], audit._qt_sdk_source_provenance()
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
        # require_provenance=False remains a legitimate, honest no-op:
        # a caller that never even attempted to supply --qt-reference-
        # dir at all (this module's own basename-only classification
        # unit tests, or a non-CI/non-required invocation) is not
        # asserting anything about Qt SDK provenance to begin with.
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance(
                "icu", {"status": "qt_reference_dir_unavailable"}, False
            )
        )

    def test_validate_qt_sdk_provenance_rejects_reference_dir_unavailable_when_required(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "require_provenance accepts
        # qt_reference_dir_unavailable"): this is the exact scenario
        # this project's own pinned `ubuntu-22.04` `appimage-smoke` CI
        # job passing --require-package-provenance must never silently
        # tolerate -- an accidentally-omitted --qt-reference-dir flag
        # must hard-fail, never silently pass as "nothing to check
        # here", exactly mirroring "not_found"'s own require_provenance
        # semantics.
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "icu", {"status": "qt_reference_dir_unavailable"}, True
        )
        self.assertIsNotNone(problem)
        self.assertIn("icu", problem)

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
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
            "sdkSourceProvenance": audit._qt_sdk_source_provenance(),
            "evidenceStrength": "replay_byte_identical",
        }
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance(
                "icu", binding, require_provenance=True
            )
        )

    def test_validate_qt_sdk_provenance_accepts_matched_without_replay_evidence_when_not_required(
        self,
    ) -> None:
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
            "sdkSourceProvenance": audit._qt_sdk_source_provenance(),
            "evidenceStrength": "canonical_digest_heuristic",
        }
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance("icu", binding)
        )

    def test_validate_qt_sdk_provenance_rejects_matched_without_replay_evidence_when_required(
        self,
    ) -> None:
        # Round-N+ review (HIGH, Qt/ICU provenance): a "matched" status
        # under --require-package-provenance must be backed by a
        # byte-identical replay of the real pinned linuxdeploy-plugin-qt
        # strip/patchelf transformation, never merely the older
        # _canonical_load_digest() heuristic -- same enforcement as
        # validate_bundled_library_package_provenance() above, mirrored
        # here for the Qt SDK reference path.
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
            "sdkSourceProvenance": audit._qt_sdk_source_provenance(),
            "evidenceStrength": "canonical_digest_heuristic",
        }
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "icu", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("replay", problem)

    def test_validate_qt_sdk_provenance_rejects_sdk_version_drift_unconditionally(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "sdkVersion ... trusted with zero
        # cross-check against anything the module itself governs"): a
        # binding claiming a DIFFERENT Qt SDK version than this
        # project's own governed EXPECTED_QT_SDK_VERSION pin is a real,
        # reported failure -- unconditionally, even without
        # require_provenance, exactly like a distro package's own
        # exact-version pin.
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkVersion": "1.2.3-not-the-real-pinned-version",
            "sdkSourceProvenance": audit._qt_sdk_source_provenance(),
        }
        problem = audit.validate_bundled_library_qt_sdk_provenance("icu", binding)
        self.assertIsNotNone(problem)
        self.assertIn("icu", problem)
        self.assertIn("1.2.3-not-the-real-pinned-version", problem)

    def test_validate_qt_sdk_provenance_requires_sdk_version_when_provenance_required(
        self,
    ) -> None:
        # Round-N+ review (HIGH): a "matched" binding with NO
        # "sdkVersion" at all (--qt-sdk-version was never supplied to
        # this exact invocation) must itself hard-fail once
        # require_provenance is True -- the governed pin can only ever
        # protect a run that actually supplies something to check it
        # against.
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkSourceProvenance": audit._qt_sdk_source_provenance(),
        }
        self.assertIsNone(
            audit.validate_bundled_library_qt_sdk_provenance("icu", binding)
        )
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "icu", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("icu", problem)

    def test_validate_qt_sdk_provenance_requires_sdk_source_provenance_when_required(
        self,
    ) -> None:
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "qt",
            {
                "status": "matched",
                "referencePath": "/qt/lib/libQt6Core.so.6",
                "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
            },
            require_provenance=True,
        )
        self.assertIsNotNone(problem)
        self.assertIn("sdkSourceProvenance", problem)

    def test_validate_qt_sdk_provenance_rejects_sdk_source_provenance_mismatch_even_with_same_version(
        self,
    ) -> None:
        tampered = dict(audit._qt_sdk_source_provenance())
        tampered["updatesXmlSha256"] = "0" * 64
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "qt",
            {
                "status": "matched",
                "referencePath": "/qt/lib/libQt6Core.so.6",
                "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
                "sdkSourceProvenance": tampered,
            },
            require_provenance=True,
        )
        self.assertIsNotNone(problem)
        self.assertIn("updatesXmlSha256", problem)

    def test_verify_qt_sdk_lock_accepts_matching_metadata(self) -> None:
        expected = audit._load_qt_sdk_lock()

        class _Response:
            def __enter__(self) -> "_Response":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def read(self) -> bytes:
                return b"matching-updates-xml"

        matching_lock = dict(expected)
        matching_lock["updatesXmlSha256"] = hashlib.sha256(
            b"matching-updates-xml"
        ).hexdigest()
        with mock.patch.object(
            audit.urllib.request, "urlopen", return_value=_Response()
        ):
            result = audit.verify_qt_sdk_lock(matching_lock)
        self.assertEqual(result["status"], "matched")

    def test_verify_qt_sdk_lock_rejects_metadata_digest_mismatch(self) -> None:
        lock = dict(audit._load_qt_sdk_lock())

        class _Response:
            def __enter__(self) -> "_Response":
                return self

            def __exit__(self, *_args: object) -> None:
                return None

            def read(self) -> bytes:
                return b"tampered-updates-xml"

        with mock.patch.object(
            audit.urllib.request, "urlopen", return_value=_Response()
        ):
            result = audit.verify_qt_sdk_lock(lock)
        self.assertEqual(result["status"], "content_mismatch")

    def test_cmd_classify_uses_qt_sdk_provenance_for_icu_never_dpkg(self) -> None:
        # End-to-end regression test for the actual CI failure: on a
        # real dpkg-equipped host with --require-package-provenance
        # passed (exactly this project's pinned `ubuntu-22.04`
        # `appimage-smoke` job), the real system ICU package is a
        # DIFFERENT version (70) than what Qt 6.11.1 bundles (73), so
        # bind_bundled_library_to_captured_provenance() would always
        # report "not_found" for "libicudata.so.73" -- classify() must
        # never even consult that captured-manifest path for "icu" at
        # all, and must instead succeed via the Qt SDK reference copy.
        lib_dir = self.root / "lib"
        lib_dir.mkdir()
        (lib_dir / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)

        # Round-N+ review ("No basename re-discovery"): a real distro
        # component ("libavif") is also mandatory/present, so
        # --require-package-provenance now requires a real
        # --distro-provenance-manifest too (see
        # CaptureBeforePackagingProvenanceTests); its own content is
        # irrelevant to this test's actual assertion (icu never
        # consults it at all), so its own binder is mocked to
        # unconditionally "match" (with a real-looking
        # "replay_byte_identical" evidenceStrength, matching this
        # project's own pinned appimage-smoke CI job's actual
        # --linuxdeploy-patchelf/--linuxdeploy-strip usage) so libavif's
        # own provenance never spuriously fails this specific test.
        #
        # Round-N+ review (HIGH, Qt/ICU provenance): the manifest now
        # also carries a real "linuxdeployPluginQt" replay toolset entry
        # (see cmd_capture_distro_provenance()'s own "replayTools"
        # embedding), rehydrated by _replay_toolset_from_manifest() and
        # passed through to bind_bundled_library_to_qt_sdk_provenance()
        # -- exercising the same --require-package-provenance
        # replay-evidence enforcement for the Qt SDK reference path that
        # validate_bundled_library_qt_sdk_provenance() above proves in
        # isolation.
        qt_patchelf_stub = self.root / "fake-linuxdeploy-qt-patchelf"
        qt_strip_stub = self.root / "fake-linuxdeploy-qt-strip"
        qt_patchelf_stub.write_bytes(b"fake patchelf binary")
        qt_strip_stub.write_bytes(b"fake strip binary")

        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(
            json.dumps(
                {
                    "formatVersion": 2,
                    "bundledPaths": {},
                    "replayTools": {
                        "linuxdeployPluginQt": {
                            "toolLabel": "linuxdeploy-plugin-qt",
                            "patchelfPath": str(qt_patchelf_stub),
                            "patchelfSha256": audit._sha256(qt_patchelf_stub),
                            "stripPath": str(qt_strip_stub),
                            "stripSha256": audit._sha256(qt_strip_stub),
                        }
                    },
                }
            )
        )

        def fail_if_called_for_icu(
            bundled_path: Path,
            manifest: dict[str, object],
            bundled_relative_path: str | None = None,
            replay_toolset: dict[str, str] | None = None,
        ) -> dict[str, object]:
            if bundled_path.name == "libicudata.so.73":
                self.fail(
                    "bind_bundled_library_to_captured_provenance() must "
                    "never be consulted for the 'icu' component at all -- "
                    "see _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE's own "
                    "docstring"
                )
            return {
                "status": "matched",
                "package": "libavif16",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["libavif"],
                "sourcePackage": "libavif",
                "architecture": "amd64",
                "debSha256": "locked-libavif16-sha",
                "debArchiveVerification": "verified",
                "evidenceStrength": "replay_byte_identical",
            }

        def fake_replay(
            reference_path: Path,
            final_path: Path,
            toolset: dict[str, str],
            expected_rpath: str | None,
        ) -> dict[str, object]:
            return {
                "toolLabel": toolset["toolLabel"],
                "patchelfSha256": toolset["patchelfSha256"],
                "stripSha256": toolset["stripSha256"],
                "referenceRealPath": str(reference_path),
                "referenceSha256": "fake-reference-sha",
                "finalSha256": "fake-final-sha",
                "referenceObservedRpath": None,
                "finalObservedRpath": None,
                "stripApplied": True,
                "patchelfSetRpathApplied": False,
                "replayedSha256": "fake-final-sha",
                "matched": True,
            }

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_captured_provenance",
            side_effect=fail_if_called_for_icu,
        ), mock.patch.object(
            audit,
            "_load_distro_package_lock",
            return_value={
                "distribution": "ubuntu-22.04",
                "packages": {
                    "libavif16": {
                        "architecture": "amd64",
                        "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["libavif"],
                        "debSha256": "locked-libavif16-sha",
                    }
                },
            },
        ), mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ), mock.patch.object(
            audit, "replay_strip_and_rpath_transform", side_effect=fake_replay
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--qt-reference-dir",
                    str(qt_reference_dir),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
                    "--qt-sdk-version",
                    audit.EXPECTED_QT_SDK_VERSION,
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

        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(json.dumps({}))

        def fail_if_called_for_icu(
            bundled_path: Path,
            manifest: dict[str, object],
            bundled_relative_path: str | None = None,
            replay_toolset: dict[str, str] | None = None,
        ) -> dict[str, object]:
            if bundled_path.name == "libicudata.so.73":
                self.fail(
                    "bind_bundled_library_to_captured_provenance() must "
                    "never be consulted for the 'icu' component at all"
                )
            return {
                "status": "matched",
                "package": "libavif16",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["libavif"],
                "sourcePackage": "libavif",
            }

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit,
            "bind_bundled_library_to_captured_provenance",
            side_effect=fail_if_called_for_icu,
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--qt-reference-dir",
                    str(qt_reference_dir),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
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

    # --- Round-N+ review ("qtSdkProvenance included only ICU, not core
    # Qt/plugins using same classification" / "Remove second lookup/
    # rebind ... Ensure reference mutation/race impossible") ---------

    def test_resolve_candidate_uses_plugin_shape_for_plugin_directory(
        self,
    ) -> None:
        path = Path("usr/lib/plugins/imageformats/libqjpeg.so")
        qt_reference_dir = self.root / "qtsdk"
        candidate = audit._resolve_qt_sdk_reference_candidate(
            path, qt_reference_dir
        )
        self.assertEqual(
            candidate, qt_reference_dir / "plugins" / "imageformats" / "libqjpeg.so"
        )

    def test_resolve_candidate_uses_qml_shape_for_qml_subpath(self) -> None:
        path = Path("usr/qml/QtQuick/Controls/libqtquickcontrols2plugin.so")
        qt_reference_dir = self.root / "qtsdk"
        candidate = audit._resolve_qt_sdk_reference_candidate(
            path, qt_reference_dir
        )
        self.assertEqual(
            candidate,
            qt_reference_dir / "qml" / "QtQuick" / "Controls" / "libqtquickcontrols2plugin.so",
        )

    def test_resolve_candidate_falls_back_to_lib_basename(self) -> None:
        # Both the core-Qt-library shape (libQt6Core.so.6) and ICU
        # (libicudata.so.73) share this same fallback shape.
        for basename in ("libQt6Core.so.6", "libicudata.so.73"):
            with self.subTest(basename=basename):
                path = Path("usr/lib") / basename
                qt_reference_dir = self.root / "qtsdk"
                candidate = audit._resolve_qt_sdk_reference_candidate(
                    path, qt_reference_dir
                )
                self.assertEqual(candidate, qt_reference_dir / "lib" / basename)

    def test_qt_is_in_components_with_qt_sdk_bundled_provenance(self) -> None:
        # The core review requirement: "qt" (core libraries AND
        # plugins/QML modules) must get the identical structured
        # qtSdkProvenance treatment "icu" already had, not merely a
        # discarded classification-time boolean.
        self.assertIn("qt", audit._COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE)
        self.assertNotIn("qt", audit.COMPONENT_EXPECTED_SOURCE_PACKAGES)

    def test_bind_qt_sdk_provenance_matched_records_all_four_digests_once(
        self,
    ) -> None:
        # Round-N+ review ("no SDK version/archive identity/reference
        # SHA/canonical digest/transform evidence"): a "matched" result
        # must record BOTH files' own sha256 and canonical-load-digest,
        # each computed exactly once (never re-hashed a second time
        # later for the SBOM).
        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libQt6Core.so.6").write_bytes(b"\x00identical")
        bundled_dir = self.root / "bundled"
        bundled_dir.mkdir()
        bundled = bundled_dir / "libQt6Core.so.6"
        bundled.write_bytes(b"\x00identical")

        sha256_calls: list[Path] = []
        digest_calls: list[Path] = []

        def counting_sha256(path: Path) -> str:
            sha256_calls.append(path)
            return f"sha-{path.name}"

        def counting_digest(path: Path) -> str:
            digest_calls.append(path)
            return "same-digest"

        with mock.patch.object(
            audit, "_sha256", side_effect=counting_sha256
        ), mock.patch.object(
            audit, "_canonical_load_digest", side_effect=counting_digest
        ):
            binding = audit.bind_bundled_library_to_qt_sdk_provenance(
                bundled, qt_reference_dir
            )
        self.assertEqual(binding["status"], "matched")
        self.assertEqual(binding["referenceSha256"], f"sha-{bundled.name}")
        self.assertEqual(binding["bundledSha256"], f"sha-{bundled.name}")
        self.assertEqual(binding["referenceCanonicalLoadDigest"], "same-digest")
        self.assertEqual(binding["bundledCanonicalLoadDigest"], "same-digest")
        # Exactly one sha256/digest computation per file (reference,
        # bundled) -- never a second, redundant re-hash of either.
        self.assertEqual(len(sha256_calls), 2)
        self.assertEqual(len(digest_calls), 2)
        self.assertCountEqual(sha256_calls, [qt_reference_dir / "lib" / "libQt6Core.so.6", bundled])
        self.assertCountEqual(digest_calls, [qt_reference_dir / "lib" / "libQt6Core.so.6", bundled])

    def test_bind_qt_sdk_provenance_records_sdk_version_when_supplied(
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
                bundled, qt_reference_dir, sdk_version="6.11.1"
            )
        self.assertEqual(binding["sdkVersion"], "6.11.1")

        # Also recorded on a "not_found" result -- the SDK release
        # identity claim is orthogonal to whether a reference copy was
        # actually found.
        missing_bundled = bundled_dir / "libicudata.so.99"
        missing_bundled.write_bytes(_FAKE_ELF_BYTES)
        not_found_binding = audit.bind_bundled_library_to_qt_sdk_provenance(
            missing_bundled, qt_reference_dir, sdk_version="6.11.1"
        )
        self.assertEqual(not_found_binding["status"], "not_found")
        self.assertEqual(not_found_binding["sdkVersion"], "6.11.1")

    def test_qt_plugin_content_verified_via_qt_sdk_provenance(self) -> None:
        qt_reference_dir = self.root / "qtsdk"
        plugin_reference = qt_reference_dir / "plugins" / "imageformats"
        plugin_reference.mkdir(parents=True)
        (plugin_reference / "libqjpeg.so").write_bytes(b"\x00identical-plugin")

        lib_dir = self.root / "lib"
        plugin_bundled = lib_dir / "plugins" / "imageformats"
        plugin_bundled.mkdir(parents=True)
        bundled = plugin_bundled / "libqjpeg.so"
        bundled.write_bytes(b"\x00identical-plugin")

        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ):
            binding = audit.bind_bundled_library_to_qt_sdk_provenance(
                bundled, qt_reference_dir
            )
        self.assertEqual(binding["status"], "matched")
        self.assertIn("plugins", binding["referencePath"])
        self.assertIn("imageformats", binding["referencePath"])

    def test_qt_core_library_content_mismatch_detected_via_qt_sdk_provenance(
        self,
    ) -> None:
        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libQt6Core.so.6").write_bytes(b"\x00genuine")
        lib_dir = self.root / "lib"
        lib_dir.mkdir()
        substituted = lib_dir / "libQt6Core.so.6"
        substituted.write_bytes(b"\x00tampered-different")

        with mock.patch.object(
            audit, "_canonical_load_digest", return_value=None
        ):
            binding = audit.bind_bundled_library_to_qt_sdk_provenance(
                substituted, qt_reference_dir
            )
        self.assertEqual(binding["status"], "content_mismatch")


def _real_qt_sdk_reference_dir() -> Path | None:
    """Locates a genuinely-installed Qt SDK prefix (containing at least
    a `lib/` directory) usable to prove
    packaging/qt_sdk_lock.json's checked-in `installedTreeContentSha256`
    pin against real content, never a synthetic fixture. Checks
    $QT_ROOT_DIR first (exported by jurplel/install-qt-action in the
    real CI environment this project's own pinned jobs run in), then
    falls back to this repository's own local dev/CI container
    convention (`/opt/qt/<version>/<arch>`, matching this module's own
    pinned `sdkVersion`) so the same real-tree proof also runs inside
    the `arkham-linux-test` development container without requiring
    $QT_ROOT_DIR to be manually exported there. Returns None (test
    skipped, never a false pass) when no real installation is found."""
    env_dir = os.environ.get("QT_ROOT_DIR")
    if env_dir and (Path(env_dir) / "lib").is_dir():
        return Path(env_dir)
    sdk_version = audit._load_qt_sdk_lock()["sdkVersion"]
    for candidate in sorted(Path("/opt/qt").glob(f"{sdk_version}/*")):
        if (candidate / "lib").is_dir():
            return candidate
    return None


class QtSdkInstalledTreeProvenanceTests(unittest.TestCase):
    """Round-N+ review (HIGH, "Qt/ICU provenance is version-only/self-
    attested ... Updates.xml hash is disconnected from independently
    cached install-qt SDK ... does not authenticate installed SDK tree
    ... same-version substituted cache must fail"): covers
    compute_qt_sdk_tree_content_digest()/verify_installed_qt_sdk_tree()/
    the tree-aware _qt_sdk_source_provenance()/
    validate_bundled_library_qt_sdk_provenance() wiring directly.
    Deliberately does NOT inherit QtSdkBundledProvenanceTests'/
    UnifiedQtSdkBindingTests' own class-level
    compute_qt_sdk_tree_content_digest mock -- this class exists
    specifically to exercise the REAL function against real (if
    synthetic) file content."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)

    def _make_sdk_tree(self, root: Path) -> None:
        (root / "lib").mkdir(parents=True)
        (root / "lib" / "libQt6Core.so.6").write_bytes(b"core-content")
        (root / "plugins" / "imageformats").mkdir(parents=True)
        (root / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(b"jpeg-plugin")
        (root / "qml" / "QtQuick").mkdir(parents=True)
        (root / "qml" / "QtQuick" / "libqtquick2plugin.so").write_bytes(b"qml-module")
        (root / "bin").mkdir(parents=True)
        (root / "bin" / "moc").write_bytes(b"moc-binary")
        (root / "include" / "QtCore").mkdir(parents=True)
        (root / "include" / "QtCore" / "qobject.h").write_bytes(b"header-content")
        (root / "mkspecs" / "linux-g++").mkdir(parents=True)
        (root / "mkspecs" / "linux-g++" / "qmake.conf").write_bytes(b"mkspecs-content")
        # A SONAME-style symlink, exactly like Qt's own real packaging
        # convention -- MUST now affect the digest (see
        # _qt_sdk_tree_content_files()'s own docstring for why: the
        # link itself, not merely its target's content, is real linked
        # behavior a retarget attack could change).
        (root / "lib" / "libQt6Core.so").symlink_to("libQt6Core.so.6")

    def test_compute_qt_sdk_tree_content_digest_is_deterministic(self) -> None:
        self._make_sdk_tree(self.root)
        first = audit.compute_qt_sdk_tree_content_digest(self.root)
        second = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 64)

    def test_compute_qt_sdk_tree_content_digest_detects_a_symlink_retarget(self) -> None:
        # Round-N+ review (HIGH, "SONAME retarget ... changes behavior
        # without pin"): retargeting the SONAME-versioned convenience
        # symlink to a DIFFERENT (but still real, still hashed) file in
        # the same tree must change the digest, even though every
        # individual regular file's own content is completely
        # untouched by this retarget.
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        (self.root / "lib" / "libQt6Other.so.6").write_bytes(b"other-content")
        (self.root / "lib" / "libQt6Core.so").unlink()
        (self.root / "lib" / "libQt6Core.so").symlink_to("libQt6Other.so.6")
        retargeted = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, retargeted)

    def test_compute_qt_sdk_tree_content_digest_detects_a_change_in_bin(self) -> None:
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        (self.root / "bin" / "moc").write_bytes(b"substituted-moc-binary")
        tampered = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, tampered)

    def test_compute_qt_sdk_tree_content_digest_detects_a_change_in_include(self) -> None:
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        (self.root / "include" / "QtCore" / "qobject.h").write_bytes(
            b"substituted-header-content"
        )
        tampered = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, tampered)

    def test_compute_qt_sdk_tree_content_digest_detects_a_change_in_mkspecs(self) -> None:
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        (self.root / "mkspecs" / "linux-g++" / "qmake.conf").write_bytes(
            b"substituted-mkspecs-content"
        )
        tampered = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, tampered)

    def test_qt_sdk_tree_content_files_rejects_an_absolute_symlink_target(self) -> None:
        self._make_sdk_tree(self.root)
        (self.root / "lib" / "libQt6Core.so").unlink()
        (self.root / "lib" / "libQt6Core.so").symlink_to("/etc/passwd")
        with self.assertRaises(audit.QtSdkTreeIntegrityError) as raised:
            audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertIn("absolute", str(raised.exception))

    def test_qt_sdk_tree_content_files_rejects_a_root_escaping_symlink_target(
        self,
    ) -> None:
        self._make_sdk_tree(self.root)
        (self.root / "lib" / "libQt6Core.so").unlink()
        (self.root / "lib" / "libQt6Core.so").symlink_to("../../../../etc/passwd")
        with self.assertRaises(audit.QtSdkTreeIntegrityError) as raised:
            audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertIn("outside", str(raised.exception))

    def test_qt_sdk_tree_content_files_rejects_a_directory_symlink(self) -> None:
        self._make_sdk_tree(self.root)
        (self.root / "lib" / "extra-real-dir").mkdir()
        (self.root / "lib" / "extra-real-dir" / "real.so").write_bytes(b"real")
        (self.root / "lib" / "extra-symlinked-dir").symlink_to(
            "extra-real-dir", target_is_directory=True
        )
        with self.assertRaises(audit.QtSdkTreeIntegrityError) as raised:
            audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertIn("directory symlink", str(raised.exception))

    def test_compute_qt_sdk_tree_content_digest_detects_a_single_byte_change_anywhere(
        self,
    ) -> None:
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        # Mutate ONLY the QML module -- deliberately not the core
        # library or the plugin -- proving the digest binds the WHOLE
        # tree, not merely core Qt libraries.
        (self.root / "qml" / "QtQuick" / "libqtquick2plugin.so").write_bytes(
            b"qml-module-tampered"
        )
        tampered = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, tampered)

    def test_compute_qt_sdk_tree_content_digest_detects_an_added_or_removed_file(
        self,
    ) -> None:
        self._make_sdk_tree(self.root)
        baseline = audit.compute_qt_sdk_tree_content_digest(self.root)
        (self.root / "plugins" / "imageformats" / "libqwebp.so").write_bytes(b"new")
        added = audit.compute_qt_sdk_tree_content_digest(self.root)
        self.assertNotEqual(baseline, added)

    def test_verify_installed_qt_sdk_tree_accepts_matching_pin(self) -> None:
        self._make_sdk_tree(self.root)
        pinned = dict(audit._load_qt_sdk_lock())
        pinned["installedTreeContentSha256"] = audit.compute_qt_sdk_tree_content_digest(
            self.root
        )
        result = audit.verify_installed_qt_sdk_tree(self.root, pinned)
        self.assertEqual(result["status"], "matched")

    def test_verify_installed_qt_sdk_tree_rejects_tampered_tree(self) -> None:
        self._make_sdk_tree(self.root)
        pinned = dict(audit._load_qt_sdk_lock())
        pinned["installedTreeContentSha256"] = audit.compute_qt_sdk_tree_content_digest(
            self.root
        )
        # A same-version cache substitution: the tree still "looks"
        # like a genuine Qt SDK install (same directory layout, same
        # file count/names), but one file's real bytes differ.
        (self.root / "lib" / "libQt6Core.so.6").write_bytes(b"substituted-core-content")
        result = audit.verify_installed_qt_sdk_tree(self.root, pinned)
        self.assertEqual(result["status"], "content_mismatch")
        self.assertNotEqual(
            result["actualInstalledTreeContentSha256"],
            result["expectedInstalledTreeContentSha256"],
        )

    def test_qt_sdk_source_provenance_carries_qt_reference_dir_unavailable_without_a_dir(
        self,
    ) -> None:
        provenance = audit._qt_sdk_source_provenance()
        self.assertEqual(
            provenance["installedTreeVerificationStatus"], "qt_reference_dir_unavailable"
        )
        self.assertNotIn("actualInstalledTreeContentSha256", provenance)

    def test_qt_sdk_source_provenance_reports_content_mismatch_for_a_tampered_tree(
        self,
    ) -> None:
        self._make_sdk_tree(self.root)
        with mock.patch.object(
            audit,
            "_load_qt_sdk_lock",
            return_value={
                **audit._load_qt_sdk_lock(),
                "installedTreeContentSha256": "0" * 64,
            },
        ):
            provenance = audit._qt_sdk_source_provenance(self.root)
        self.assertEqual(provenance["installedTreeVerificationStatus"], "content_mismatch")

    def test_validate_qt_sdk_provenance_rejects_matched_file_inside_a_content_mismatched_tree(
        self,
    ) -> None:
        # The exact scenario this finding exists to close: this ONE
        # file's own per-file digest/replay comparison legitimately
        # says "matched" (e.g. it happens to be byte-identical to
        # SOME reference copy), but the sdkSourceProvenance sub-object
        # shared by every artifact this run produced reports the WHOLE
        # installed tree as content-mismatched -- proving the reference
        # tree itself is not the genuine, pinned one. This must be
        # rejected regardless, never accepted merely because this one
        # file happened to still match.
        binding = {
            "status": "matched",
            "referencePath": "/qt/lib/libicudata.so.73",
            "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
            "sdkSourceProvenance": {
                **audit._qt_sdk_source_provenance(),
                "installedTreeVerificationStatus": "content_mismatch",
                "actualInstalledTreeContentSha256": "1" * 64,
            },
            "evidenceStrength": "replay_byte_identical",
        }
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "icu", binding, require_provenance=True
        )
        self.assertIsNotNone(problem)
        self.assertIn("content_mismatch", problem)

    def test_validate_qt_sdk_provenance_accepts_matched_file_inside_a_matched_tree(
        self,
    ) -> None:
        # The companion "new-pass" half of the test directly above: an
        # otherwise-identical binding whose tree verification legitimately
        # reports "matched" (against the SAME qt_reference_dir the
        # binding itself, and this validate() call, both use) must
        # still be accepted -- proving the new check is decisive in
        # BOTH directions, never merely a blanket failure.
        self._make_sdk_tree(self.root)
        pinned_lock = {
            **audit._load_qt_sdk_lock(),
            "installedTreeContentSha256": audit.compute_qt_sdk_tree_content_digest(
                self.root
            ),
        }
        with mock.patch.object(audit, "_load_qt_sdk_lock", return_value=pinned_lock):
            binding = {
                "status": "matched",
                "referencePath": "/qt/lib/libicudata.so.73",
                "sdkVersion": audit.EXPECTED_QT_SDK_VERSION,
                "sdkSourceProvenance": audit._qt_sdk_source_provenance(self.root),
                "evidenceStrength": "replay_byte_identical",
            }
            self.assertEqual(
                binding["sdkSourceProvenance"]["installedTreeVerificationStatus"],
                "matched",
            )
            self.assertIsNone(
                audit.validate_bundled_library_qt_sdk_provenance(
                    "icu",
                    binding,
                    require_provenance=True,
                    qt_reference_dir=self.root,
                )
            )

    def test_end_to_end_substituted_cache_tree_fails_even_when_the_specific_file_still_matches(
        self,
    ) -> None:
        # Full production seam: compute_qt_sdk_bindings() ->
        # validate_bundled_library_qt_sdk_provenance(), exactly as
        # cmd_classify() itself calls them, with a REAL qt_reference_dir
        # fixture whose overall tree content does NOT match the pinned
        # lock (an unrelated file differs -- simulating a same-version
        # cache substitution) even though the ONE bundled file actually
        # under validation is untouched and still byte-identical to its
        # own reference copy.
        self._make_sdk_tree(self.root)
        # Add one more file so the fixture's aggregate digest can never
        # accidentally coincide with the real checked-in
        # installedTreeContentSha256 pin (an astronomically unlikely
        # sha256 collision aside).
        (self.root / "lib" / "libQt6Network.so.6").write_bytes(b"unrelated-substituted")
        bundled_dir = self.root / "bundled"
        bundled_dir.mkdir()
        bundled = bundled_dir / "libQt6Core.so.6"
        bundled.write_bytes(b"core-content")  # byte-identical to the reference copy

        by_component = {"qt": [bundled]}
        with mock.patch.object(audit, "_canonical_load_digest", return_value=None):
            bindings = audit.compute_qt_sdk_bindings(
                by_component, self.root, audit.EXPECTED_QT_SDK_VERSION
            )
        binding = bindings[bundled]
        # The per-file comparison alone would say "matched" (identical
        # bytes) -- but the overall binding must still be rejected once
        # the tree itself cannot be authenticated.
        self.assertEqual(binding["status"], "matched")
        problem = audit.validate_bundled_library_qt_sdk_provenance(
            "qt", binding, require_provenance=True, qt_reference_dir=self.root
        )
        self.assertIsNotNone(problem)
        self.assertIn("content_mismatch", problem)

    @unittest.skipUnless(
        _real_qt_sdk_reference_dir() is not None,
        "requires a real, genuinely-installed Qt SDK ($QT_ROOT_DIR or "
        "/opt/qt/<sdkVersion>/<arch>) to prove the checked-in "
        "installedTreeContentSha256 pin against actual content, never a "
        "synthetic fixture",
    )
    def test_checked_in_lock_pin_matches_a_real_genuinely_installed_qt_sdk_tree(
        self,
    ) -> None:
        # Round-N+ review follow-up: confirmed empirically (installing
        # the SAME pinned Qt version via aqtinstall at two different
        # prefixes and diffing the resulting trees) that several
        # genuine, unmodified files this digest now binds -- e.g. every
        # `lib/pkgconfig/Qt6*.pc` file's own `prefix=` line -- embed the
        # SDK's own absolute install path as literal text. The
        # checked-in installedTreeContentSha256 pin is therefore bound
        # to install-qt-action's own deterministic real-CI install path
        # ($GITHUB_WORKSPACE/../Qt), which $QT_ROOT_DIR reflects exactly
        # when this test runs for real in this project's own CI. The
        # "/opt/qt/<sdkVersion>/<arch>" local dev-container fallback
        # remains useful to prove the DOWNLOAD/DIGEST MACHINERY itself
        # runs cleanly (no QtSdkTreeIntegrityError, matching sdkVersion)
        # against real content, but -- unless that container installs
        # Qt at the exact same absolute path CI does -- it can
        # legitimately report content_mismatch even for a byte-identical
        # upstream install, so this assertion only hard-requires an
        # exact "matched" pin when $QT_ROOT_DIR (the real, CI-authentic
        # path) is what was actually found.
        real_qt_dir = _real_qt_sdk_reference_dir()
        assert real_qt_dir is not None
        result = audit.verify_installed_qt_sdk_tree(real_qt_dir)
        if os.environ.get("QT_ROOT_DIR"):
            self.assertEqual(result["status"], "matched", result)
        else:
            self.assertIn(result["status"], ("matched", "content_mismatch"))
            if result["status"] == "content_mismatch":
                self.skipTest(
                    "local dev-container Qt install path differs from "
                    "install-qt-action's real CI path, so an exact pin "
                    f"match is not expected here: {result}"
                )


class UnifiedQtSdkBindingTests(unittest.TestCase):
    """Round-N+ review ("build_sbom_inventory binds separately before
    cmd_classify later rebind/validate, so serialized object not
    necessarily exact validated proof ... produce ONE immutable
    validation-binding object per artifact used both for acceptance and
    SBOM ... Remove second lookup/rebind ... Ensure reference mutation/
    race impossible (open descriptor/hash once or verify identity)"):
    compute_qt_sdk_bindings() must compute each qt/icu bundled file's
    binding EXACTLY ONCE, and that SAME object -- not a fresh
    recomputation -- must be what both build_sbom_inventory() and
    cmd_classify()'s own provenance-validation loop use."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        # See QtSdkBundledProvenanceTests.setUp()'s identical patch for
        # why this class's own lightweight synthetic qt_reference_dir
        # fixtures need a default "matched" installed-tree
        # verification.
        patcher = mock.patch.object(
            audit,
            "compute_qt_sdk_tree_content_digest",
            return_value=audit._load_qt_sdk_lock()["installedTreeContentSha256"],
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def _make_matched_pair(self, relative: str) -> tuple[Path, Path]:
        qt_reference_dir = self.root / "qtsdk"
        reference = qt_reference_dir / relative
        reference.parent.mkdir(parents=True, exist_ok=True)
        reference.write_bytes(_FAKE_ELF_BYTES + b"identical-content")
        bundled = self.root / "lib" / Path(relative).name
        bundled.parent.mkdir(parents=True, exist_ok=True)
        bundled.write_bytes(_FAKE_ELF_BYTES + b"identical-content")
        return qt_reference_dir, bundled

    def test_compute_qt_sdk_bindings_computes_each_qualifying_file_exactly_once(
        self,
    ) -> None:
        qt_reference_dir, icu_bundled = self._make_matched_pair("lib/libicudata.so.73")
        core_bundled = self.root / "lib" / "libQt6Core.so.6"
        (qt_reference_dir / "lib" / "libQt6Core.so.6").write_bytes(b"\x00core")
        core_bundled.write_bytes(b"\x00core")
        non_qt_bundled = self.root / "lib" / "libavif.so.16"
        non_qt_bundled.write_bytes(_FAKE_ELF_BYTES)

        by_component = {
            "icu": [icu_bundled],
            "qt": [core_bundled],
            "libavif": [non_qt_bundled],
        }
        call_count = 0
        real_bind = audit.bind_bundled_library_to_qt_sdk_provenance

        def counting_bind(*args: object, **kwargs: object) -> dict[str, object]:
            nonlocal call_count
            call_count += 1
            return real_bind(*args, **kwargs)  # type: ignore[arg-type]

        with mock.patch.object(
            audit, "bind_bundled_library_to_qt_sdk_provenance", side_effect=counting_bind
        ), mock.patch.object(audit, "_canonical_load_digest", return_value="same-digest"):
            bindings = audit.compute_qt_sdk_bindings(by_component, qt_reference_dir)

        # Exactly one call per qt/icu file; the distro ("libavif") file
        # must never be looked up via this mechanism at all.
        self.assertEqual(call_count, 2)
        self.assertEqual(set(bindings.keys()), {icu_bundled, core_bundled})
        self.assertNotIn(non_qt_bundled, bindings)
        self.assertEqual(bindings[icu_bundled]["status"], "matched")
        self.assertEqual(bindings[core_bundled]["status"], "matched")

    def test_build_sbom_inventory_reuses_precomputed_binding_without_recomputing(
        self,
    ) -> None:
        qt_reference_dir, icu_bundled = self._make_matched_pair("lib/libicudata.so.73")
        lib_dir = icu_bundled.parent
        by_component = {"icu": [icu_bundled]}
        with mock.patch.object(audit, "_canonical_load_digest", return_value="same-digest"):
            precomputed = audit.compute_qt_sdk_bindings(by_component, qt_reference_dir)

        def fail_if_called(*_args: object, **_kwargs: object) -> None:
            self.fail(
                "build_sbom_inventory() must reuse the precomputed "
                "qt_sdk_bindings dict, never recompute it a second time"
            )

        with mock.patch.object(
            audit, "bind_bundled_library_to_qt_sdk_provenance", side_effect=fail_if_called
        ), mock.patch.object(
            audit, "capture_package_provenance", return_value=None
        ):
            inventory = audit.build_sbom_inventory(
                lib_dir, qt_reference_dir, qt_sdk_bindings=precomputed
            )
        entry = next(e for e in inventory if e["basename"] == "libicudata.so.73")
        # The exact same object (identity, not merely equality) that
        # was precomputed must be what ends up in the SBOM.
        self.assertIs(entry["qtSdkProvenance"], precomputed[icu_bundled])

    def test_reference_mutation_after_compute_cannot_change_recorded_sbom_result(
        self,
    ) -> None:
        # The core anti-TOCTOU guarantee: once compute_qt_sdk_bindings()
        # has run, no later mutation of either the bundled file or the
        # Qt SDK reference copy can retroactively change what the SBOM
        # (or cmd_classify()'s own validation) reports, because both
        # consume the SAME already-computed dict rather than performing
        # their own fresh, independently-timed re-inspection.
        qt_reference_dir, bundled = self._make_matched_pair("lib/libicudata.so.73")
        lib_dir = bundled.parent
        by_component = {"icu": [bundled]}
        with mock.patch.object(audit, "_canonical_load_digest", return_value="same-digest"):
            precomputed = audit.compute_qt_sdk_bindings(by_component, qt_reference_dir)
        self.assertEqual(precomputed[bundled]["status"], "matched")

        # Mutate the bundled file AFTER the binding was already computed
        # -- if build_sbom_inventory() ever recomputed instead of
        # reusing `precomputed`, this would flip to content_mismatch.
        bundled.write_bytes(_FAKE_ELF_BYTES + b"mutated-after-compute")

        with mock.patch.object(
            audit, "capture_package_provenance", return_value=None
        ):
            inventory = audit.build_sbom_inventory(
                lib_dir, qt_reference_dir, qt_sdk_bindings=precomputed
            )
        entry = next(e for e in inventory if e["basename"] == "libicudata.so.73")
        self.assertEqual(entry["qtSdkProvenance"]["status"], "matched")

    def test_cmd_classify_computes_qt_sdk_binding_exactly_once_end_to_end(
        self,
    ) -> None:
        qt_reference_dir, icu_bundled = self._make_matched_pair("lib/libicudata.so.73")
        lib_dir = icu_bundled.parent
        (lib_dir / "libavif.so.16").write_bytes(_FAKE_ELF_BYTES)

        # Round-N+ review (HIGH, Qt/ICU provenance): exercise the same
        # real "linuxdeployPluginQt" replay-toolset manifest embedding
        # as test_cmd_classify_uses_qt_sdk_provenance_for_icu_never_dpkg
        # above, so this end-to-end coalescing test also proves the
        # replay-evidence path (not merely the pre-existing heuristic
        # one) is only ever consumed once.
        qt_patchelf_stub = self.root / "fake-linuxdeploy-qt-patchelf"
        qt_strip_stub = self.root / "fake-linuxdeploy-qt-strip"
        qt_patchelf_stub.write_bytes(b"fake patchelf binary")
        qt_strip_stub.write_bytes(b"fake strip binary")

        manifest_path = self.root / "manifest.json"
        manifest_path.write_text(
            json.dumps(
                {
                    "formatVersion": 2,
                    "bundledPaths": {},
                    "replayTools": {
                        "linuxdeployPluginQt": {
                            "toolLabel": "linuxdeploy-plugin-qt",
                            "patchelfPath": str(qt_patchelf_stub),
                            "patchelfSha256": audit._sha256(qt_patchelf_stub),
                            "stripPath": str(qt_strip_stub),
                            "stripSha256": audit._sha256(qt_strip_stub),
                        }
                    },
                }
            )
        )
        sbom_path = self.root / "sbom.json"

        call_count = 0
        real_bind = audit.bind_bundled_library_to_qt_sdk_provenance

        def counting_bind(*args: object, **kwargs: object) -> dict[str, object]:
            nonlocal call_count
            call_count += 1
            return real_bind(*args, **kwargs)  # type: ignore[arg-type]

        def fake_replay(
            reference_path: Path,
            final_path: Path,
            toolset: dict[str, str],
            expected_rpath: str | None,
        ) -> dict[str, object]:
            return {
                "toolLabel": toolset["toolLabel"],
                "patchelfSha256": toolset["patchelfSha256"],
                "stripSha256": toolset["stripSha256"],
                "referenceRealPath": str(reference_path),
                "referenceSha256": "fake-reference-sha",
                "finalSha256": "fake-final-sha",
                "referenceObservedRpath": None,
                "finalObservedRpath": None,
                "stripApplied": True,
                "patchelfSetRpathApplied": False,
                "replayedSha256": "fake-final-sha",
                "matched": True,
            }

        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(
            audit, "bind_bundled_library_to_qt_sdk_provenance", side_effect=counting_bind
        ), mock.patch.object(
            audit,
            "bind_bundled_library_to_captured_provenance",
            return_value={
                "status": "matched",
                "package": "libavif16",
                "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["libavif"],
                "sourcePackage": "libavif",
                "architecture": "amd64",
                "debSha256": "locked-libavif16-sha",
                "debArchiveVerification": "verified",
                "evidenceStrength": "replay_byte_identical",
            },
        ), mock.patch.object(
            audit,
            "_load_distro_package_lock",
            return_value={
                "distribution": "ubuntu-22.04",
                "packages": {
                    "libavif16": {
                        "architecture": "amd64",
                        "version": audit.COMPONENT_EXPECTED_SOURCE_VERSION["libavif"],
                        "debSha256": "locked-libavif16-sha",
                    }
                },
            },
        ), mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ), mock.patch.object(
            audit, "replay_strip_and_rpath_transform", side_effect=fake_replay
        ), redirect_stdout(stdout), redirect_stderr(stderr):
            exit_code = audit.main(
                [
                    "classify",
                    str(lib_dir),
                    "--json-out",
                    str(sbom_path),
                    "--qt-reference-dir",
                    str(qt_reference_dir),
                    "--require-package-provenance",
                    "--distro-provenance-manifest",
                    str(manifest_path),
                    "--qt-sdk-version",
                    "6.11.1",
                ]
            )
        self.assertEqual(exit_code, 0, stderr.getvalue())
        # Exactly one call for the single qualifying ("icu") file --
        # previously this was called once by build_sbom_inventory() and
        # again, independently, by the provenance-validation loop.
        self.assertEqual(call_count, 1)

        sbom = json.loads(sbom_path.read_text())
        icu_entry = next(
            e for e in sbom["inventory"] if e["basename"] == "libicudata.so.73"
        )
        self.assertEqual(icu_entry["qtSdkProvenance"]["status"], "matched")
        self.assertEqual(icu_entry["qtSdkProvenance"]["sdkVersion"], "6.11.1")

    def test_build_sbom_inventory_falls_back_to_inline_computation_when_omitted(
        self,
    ) -> None:
        # Backward-compat path: a caller that never precomputed bindings
        # (this module's own narrower direct unit tests, or any future
        # caller only ever needing the SBOM in isolation) still gets a
        # correct, freshly-computed qtSdkProvenance rather than a
        # missing field or an exception.
        qt_reference_dir, bundled = self._make_matched_pair("lib/libQt6Core.so.6")
        lib_dir = bundled.parent
        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ), mock.patch.object(audit, "capture_package_provenance", return_value=None):
            inventory = audit.build_sbom_inventory(
                lib_dir, qt_reference_dir, qt_sdk_version="6.11.1"
            )
        entry = next(e for e in inventory if e["basename"] == "libQt6Core.so.6")
        self.assertEqual(entry["classification"], "qt")
        self.assertEqual(entry["qtSdkProvenance"]["status"], "matched")
        self.assertEqual(entry["qtSdkProvenance"]["sdkVersion"], "6.11.1")

    def test_qt_sdk_provenance_is_serialized_for_core_qt_plugin_qml_and_icu_with_the_same_locked_source_identity(
        self,
    ) -> None:
        appdir = self.root / "appdir"
        (appdir / "usr" / "lib").mkdir(parents=True)
        (appdir / "usr" / "lib" / "libQt6Core.so.6").write_bytes(_FAKE_ELF_BYTES)
        (appdir / "usr" / "lib" / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)
        (appdir / "usr" / "lib" / "plugins" / "imageformats").mkdir(parents=True)
        (appdir / "usr" / "lib" / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(
            _FAKE_ELF_BYTES
        )
        (appdir / "usr" / "qml" / "QtQuick" / "Controls").mkdir(parents=True)
        (
            appdir
            / "usr"
            / "qml"
            / "QtQuick"
            / "Controls"
            / "libqtquickcontrols2plugin.so"
        ).write_bytes(_FAKE_ELF_BYTES)

        qt_reference_dir = self.root / "qtsdk"
        (qt_reference_dir / "lib").mkdir(parents=True)
        (qt_reference_dir / "lib" / "libQt6Core.so.6").write_bytes(_FAKE_ELF_BYTES)
        (qt_reference_dir / "lib" / "libicudata.so.73").write_bytes(_FAKE_ELF_BYTES)
        (qt_reference_dir / "plugins" / "imageformats").mkdir(parents=True)
        (qt_reference_dir / "plugins" / "imageformats" / "libqjpeg.so").write_bytes(
            _FAKE_ELF_BYTES
        )
        (qt_reference_dir / "qml" / "QtQuick" / "Controls").mkdir(parents=True)
        (
            qt_reference_dir
            / "qml"
            / "QtQuick"
            / "Controls"
            / "libqtquickcontrols2plugin.so"
        ).write_bytes(_FAKE_ELF_BYTES)

        with mock.patch.object(
            audit, "_canonical_load_digest", return_value="same-digest"
        ):
            by_component, unmapped = audit.classify_all(appdir, qt_reference_dir)
            self.assertEqual(unmapped, [])
            bindings = audit.compute_qt_sdk_bindings(
                by_component, qt_reference_dir, audit.EXPECTED_QT_SDK_VERSION
            )
            inventory = audit.build_sbom_inventory(
                appdir,
                qt_reference_dir,
                qt_sdk_bindings=bindings,
            )

        interesting = {
            entry["path"]: entry["qtSdkProvenance"]
            for entry in inventory
            if entry["path"]
            in {
                "usr/lib/libQt6Core.so.6",
                "usr/lib/libicudata.so.73",
                "usr/lib/plugins/imageformats/libqjpeg.so",
                "usr/qml/QtQuick/Controls/libqtquickcontrols2plugin.so",
            }
        }
        self.assertEqual(len(interesting), 4)
        expected_source = audit._qt_sdk_source_provenance(qt_reference_dir)
        for binding in interesting.values():
            self.assertEqual(binding["status"], "matched")
            self.assertEqual(binding["sdkSourceProvenance"], expected_source)
            self.assertEqual(binding["sdkVersion"], audit.EXPECTED_QT_SDK_VERSION)


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

    def test_every_expected_version_agrees_with_whatever_is_really_installed(
        self,
    ) -> None:
        """Finding #9 (distro provenance pinning): now that
        COMPONENT_EXPECTED_SOURCE_VERSION has a real,
        Docker-derived entry for all 39 COMPONENT_EXPECTED_SOURCE_PACKAGES
        components (not merely e2fsprogs), this test proves every one of
        those prefixes actually agrees with whatever real dpkg-owned
        system copy happens to be installed on THIS host -- the exact
        same "checked for real, never merely hand-typed" guarantee
        test_every_expected_source_package_agrees_with_whatever_is_really_
        installed above already gives the source-package NAME mapping.

        bind_bundled_library_to_system_provenance() only ever reaches
        its version-prefix check on a "matched" (content-identical)
        binding -- by design, so a version claim can never be pinned to
        an unrelated same-basename file (see that function's own
        docstring). This test satisfies that honestly, without faking
        content equality: for every real system library under
        _SYSTEM_LIBRARY_SEARCH_DIRS that COMPONENT_PATTERNS classifies
        into a component with both an expected source package AND an
        expected version prefix, it copies that exact real file's bytes
        into a scratch "bundled" location under the same basename, then
        calls the real, unmocked bind_bundled_library_to_system_provenance()
        against the copy. Because the copy's bytes -- and therefore its
        _canonical_load_digest() -- are byte-for-byte identical to the
        real system original it was copied from, this genuinely reaches
        "matched" and exercises the real version-prefix comparison
        against this host's real installed version, with zero mocking of
        dpkg, digests, or metadata anywhere in the chain."""
        problems: list[str] = []
        checked = 0
        seen_paths: set[Path] = set()
        candidate_ordinal = 0
        with tempfile.TemporaryDirectory() as scratch_dir_name:
            scratch_dir = Path(scratch_dir_name)
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
                    if (
                        component
                        not in audit.COMPONENT_EXPECTED_SOURCE_VERSION
                    ):
                        continue
                    seen_paths.add(resolved)
                    try:
                        original_bytes = resolved.read_bytes()
                    except OSError:
                        continue
                    # bind_bundled_library_to_system_provenance() looks
                    # up the real system candidate by
                    # `bundled_path.name` alone, so the scratch copy
                    # MUST keep the exact same basename as the real
                    # file it was copied from (never a disambiguated
                    # name) -- give each candidate its own subdirectory
                    # instead to avoid basename collisions between
                    # distinct search_dir entries sharing a basename.
                    candidate_ordinal += 1
                    candidate_dir = scratch_dir / str(candidate_ordinal)
                    candidate_dir.mkdir()
                    bundled_copy = candidate_dir / basename
                    bundled_copy.write_bytes(original_bytes)
                    binding = audit.bind_bundled_library_to_system_provenance(
                        bundled_copy
                    )
                    if binding["status"] != "matched":
                        # A real but currently-uninstalled/undiscoverable
                        # counterpart (or one whose digest genuinely could
                        # not be computed, e.g. readelf missing) is not a
                        # version-prefix disagreement -- only "matched"
                        # bindings are ever version-checked at all, by
                        # design (see this function's own docstring).
                        continue
                    checked += 1
                    problem = audit.validate_bundled_library_package_provenance(
                        component, binding
                    )
                    if problem is not None:
                        problems.append(problem)
        if checked == 0:
            self.skipTest(
                "no real system library on this host both matched a "
                "COMPONENT_EXPECTED_SOURCE_VERSION entry and could "
                "be content-bound as its own scratch copy -- nothing to "
                "cross-check here"
            )
        self.assertEqual(
            problems,
            [],
            f"COMPONENT_EXPECTED_SOURCE_VERSION disagreed with "
            f"{len(problems)} real, currently-installed system "
            f"package(s) (checked {checked} matched libraries): "
            + "; ".join(problems),
        )


class DpkgOwningPackageUsrMergePathFormTests(unittest.TestCase):
    """`appimage-smoke` regression (fresh CI run against exact head
    5f5886c: "captured distro provenance for 16/176 resolved
    dependencies" -- nearly every genuinely dpkg-owned real distro
    library, e.g. libxau6's libXau.so.6, was wrongly reported as NOT
    dpkg-owned): _dpkg_owning_package() previously only ever tried
    STRIPPING a leading `/usr` prefix as its one merged-/usr fallback.
    Directly reproducing this against a real, freshly `apt-get
    install`ed `ubuntu:22.04` container proved the opposite direction is
    what actually happens for the vast majority of real packages there:
    `ldd` reports dependency paths in the pre-merge-compatibility-
    symlink form (`/lib/x86_64-linux-gnu/...` -- the exact bytes baked
    into `/etc/ld.so.cache` by `ldconfig`), while dpkg's own package-
    contents database records the CANONICAL, `/usr`-merged form
    (`/usr/lib/x86_64-linux-gnu/...`) for those same packages -- the
    old strip-only fallback could never bridge that gap at all, since
    stripping `/usr` from an already-`/lib`-form path is a no-op that
    just retries the identical failing candidate a second time.

    These tests mock `subprocess.run` directly (portable, no real dpkg
    needed, and deterministic regardless of which direction happens to
    be true on whatever host runs the test suite) to prove the exact
    fail-before/pass-after shape of the regression: given a real dpkg
    database that only recognizes the `/usr`-merged form of a path,
    `_dpkg_owning_package()` must still find the owner when handed the
    bare `/lib`-form path `ldd` actually reports (the previously-broken
    direction), and vice versa -- proving the fix is symmetric rather
    than merely flipping which single direction happens to work on
    today's exact CI image (a fragile, non-portable fix this project
    explicitly wants to avoid repeating)."""

    def test_finds_owner_when_dpkg_only_recognizes_the_usr_merged_form(
        self,
    ) -> None:
        """The exact regression: `ldd` reports the bare, pre-merge
        `/lib/...` form, but dpkg's database only recognizes the
        canonical `/usr/lib/...` form (this was the majority case for
        real packages -- libxau6, libglib2.0-0, libxcb1, ... -- on the
        real `appimage-smoke` CI runner)."""

        def fake_run(cmd, **kwargs):
            queried_path = cmd[2]
            result = mock.Mock()
            if queried_path == "/usr/lib/x86_64-linux-gnu/libXau.so.6":
                result.returncode = 0
                result.stdout = (
                    "libxau6:amd64: /usr/lib/x86_64-linux-gnu/libXau.so.6\n"
                )
            else:
                result.returncode = 1
                result.stdout = ""
            return result

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(audit.subprocess, "run", side_effect=fake_run):
            owner = audit._dpkg_owning_package(
                Path("/lib/x86_64-linux-gnu/libXau.so.6")
            )
        self.assertEqual(owner, "libxau6")

    def test_finds_owner_when_dpkg_only_recognizes_the_pre_merge_form(
        self,
    ) -> None:
        """The opposite direction (this project's own earlier testing
        against `libz1g` observed exactly this): `ldd` reports the
        canonical `/usr/lib/...` form, but dpkg's database only
        recognizes the older, pre-merge `/lib/...` form."""

        def fake_run(cmd, **kwargs):
            queried_path = cmd[2]
            result = mock.Mock()
            if queried_path == "/lib/x86_64-linux-gnu/libz.so.1":
                result.returncode = 0
                result.stdout = "zlib1g:amd64: /lib/x86_64-linux-gnu/libz.so.1\n"
            else:
                result.returncode = 1
                result.stdout = ""
            return result

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(audit.subprocess, "run", side_effect=fake_run):
            owner = audit._dpkg_owning_package(
                Path("/usr/lib/x86_64-linux-gnu/libz.so.1")
            )
        self.assertEqual(owner, "zlib1g")

    def test_never_retries_an_identical_no_op_candidate(self) -> None:
        """Guards directly against the exact old bug shape: stripping
        `/usr` from a path that is not `/usr`-prefixed is a no-op, so
        the OLD code's `(path, Path(str(path).removeprefix("/usr")))`
        tuple silently retried the IDENTICAL failing candidate twice for
        any `/lib`-form input. Asserts `dpkg -S` is never invoked twice
        with the same candidate path, and that both the given `/lib`-
        form path AND its `/usr`-prefixed counterpart are actually
        tried."""
        queried: list[str] = []

        def fake_run(cmd, **kwargs):
            queried.append(cmd[2])
            result = mock.Mock()
            result.returncode = 1
            result.stdout = ""
            return result

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(audit.subprocess, "run", side_effect=fake_run):
            audit._dpkg_owning_package(
                Path("/lib/x86_64-linux-gnu/libnothing.so.1")
            )
        self.assertEqual(
            len(queried),
            len(set(queried)),
            f"duplicate candidate(s) queried: {queried}",
        )
        self.assertIn("/lib/x86_64-linux-gnu/libnothing.so.1", queried)
        self.assertIn("/usr/lib/x86_64-linux-gnu/libnothing.so.1", queried)

    def test_does_not_add_a_usr_prefix_for_an_unrelated_top_level_path(
        self,
    ) -> None:
        """A path that is not itself under one of the classic usr-merged
        compatibility directories (`/lib`, `/bin`, `/sbin`, ...) must
        never gain a synthetic, meaningless `/usr`-prefixed candidate --
        e.g. a Qt SDK reference path like `/opt/qt/lib/libQt6Core.so.6`
        is never itself a `/usr`-merge compatibility symlink target."""
        queried: list[str] = []

        def fake_run(cmd, **kwargs):
            queried.append(cmd[2])
            result = mock.Mock()
            result.returncode = 1
            result.stdout = ""
            return result

        with mock.patch.object(
            audit.shutil, "which", return_value="/usr/bin/dpkg"
        ), mock.patch.object(audit.subprocess, "run", side_effect=fake_run):
            audit._dpkg_owning_package(Path("/opt/qt/lib/libQt6Core.so.6"))
        self.assertEqual(queried, ["/opt/qt/lib/libQt6Core.so.6"])


class DpkgRecordedFileMd5UsrMergePathFormTests(unittest.TestCase):
    """Docker/`ubuntu:22.04` validation of this exact PR (real dpkg,
    real installed zlib1g) surfaced a SECOND, independent usrmerge path-
    form gap from the one `DpkgOwningPackageUsrMergePathFormTests`
    above already covers: `_dpkg_owning_package()` bridges the gap for
    `dpkg -S <path>` subprocess queries, but `_dpkg_recorded_file_md5()`
    is a completely separate code path -- it parses a package's own
    `.md5sums` FILE CONTENT directly rather than invoking `dpkg -S` --
    and, before this fix, matched only the single, literal resolved-
    path form, with no usrmerge fallback of its own at all. Real
    zlib1g on a real `ubuntu:22.04` container records
    `lib/x86_64-linux-gnu/libz.so.1.2.11` (pre-merge form) in its
    `.md5sums`, while `Path.resolve()` on the live, merged-/usr
    filesystem returns `/usr/lib/x86_64-linux-gnu/libz.so.1.2.11` --
    so the old code always reported "no matching entry" (silently
    degrading to `dpkgFileIntegrity: unavailable`) for this and nearly
    every other real system library under a merged-/usr top-level
    directory, never actually verifying real file integrity at all.
    These tests reproduce the exact fail-before/pass-after shape using
    a real temporary `.md5sums` file (no real dpkg database needed --
    portable and deterministic on any host)."""

    def test_matches_a_pre_merge_recorded_path_from_a_post_merge_resolved_path(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            info_dir = Path(tmp) / "info"
            info_dir.mkdir()
            (info_dir / "zlib1g.md5sums").write_text(
                "e6e98f694c050c5daa6a622672bdbf4d  "
                "lib/x86_64-linux-gnu/libz.so.1.2.11\n"
            )
            with mock.patch.object(audit, "_DPKG_INFO_DIR", info_dir):
                recorded = audit._dpkg_recorded_file_md5(
                    "zlib1g",
                    Path("/usr/lib/x86_64-linux-gnu/libz.so.1.2.11"),
                )
        self.assertEqual(recorded, "e6e98f694c050c5daa6a622672bdbf4d")

    def test_matches_a_post_merge_recorded_path_from_a_pre_merge_resolved_path(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            info_dir = Path(tmp) / "info"
            info_dir.mkdir()
            (info_dir / "somepkg.md5sums").write_text(
                "0123456789abcdef0123456789abcdef  "
                "usr/lib/x86_64-linux-gnu/libsomething.so.1\n"
            )
            with mock.patch.object(audit, "_DPKG_INFO_DIR", info_dir):
                recorded = audit._dpkg_recorded_file_md5(
                    "somepkg",
                    Path("/lib/x86_64-linux-gnu/libsomething.so.1"),
                )
        self.assertEqual(recorded, "0123456789abcdef0123456789abcdef")

    def test_does_not_add_a_usrmerge_candidate_for_an_unrelated_top_level_path(
        self,
    ) -> None:
        """A path outside the classic usrmerge compatibility directories
        (e.g. a Qt SDK reference tree under `/opt/qt/...`) must never be
        matched against a synthetic, meaningless alternate form -- an
        entry recorded at `opt/qt/lib/libQt6Core.so.6` must not satisfy
        a lookup for `usr/opt/qt/lib/libQt6Core.so.6` or vice versa."""
        candidates = audit._merged_usr_relative_path_candidates(
            Path("/opt/qt/lib/libQt6Core.so.6")
        )
        self.assertEqual(candidates, frozenset({"opt/qt/lib/libQt6Core.so.6"}))

    def test_candidate_set_is_exactly_two_forms_for_a_usrmerge_directory(
        self,
    ) -> None:
        candidates = audit._merged_usr_relative_path_candidates(
            Path("/usr/lib/x86_64-linux-gnu/libz.so.1.2.11")
        )
        self.assertEqual(
            candidates,
            frozenset(
                {
                    "usr/lib/x86_64-linux-gnu/libz.so.1.2.11",
                    "lib/x86_64-linux-gnu/libz.so.1.2.11",
                }
            ),
        )


@unittest.skipUnless(
    shutil.which("dpkg") and shutil.which("dpkg-query") and shutil.which("readelf"),
    "requires a real Debian/Ubuntu dpkg database and readelf to authenticate "
    "a genuine system dependency",
)
class RealCaptureDistroSourceProvenancePathFormTests(unittest.TestCase):
    """Companion to DpkgOwningPackageUsrMergePathFormTests above, proven
    against a REAL dpkg database/readelf-backed DT_NEEDED walk rather than mocks: resolves the
    real `dpkg` binary's own real dynamic dependencies (guaranteed to
    exist and be dynamically linked on any real Debian/Ubuntu host) and
    asserts every soname this resolver resolves to a real, existing, dpkg-owned
    file is actually captured by capture_distro_source_provenance() --
    i.e. it does not silently drop a real, dpkg-owned dependency because
    of a path-form mismatch between what the loader cache/search rules
    resolve and what dpkg's
    database recognizes, regardless of which real libraries happen to
    be installed on whatever host runs this test."""

    def test_capture_distro_source_provenance_drops_no_real_dpkg_owned_dependency(
        self,
    ) -> None:
        dpkg_path = shutil.which("dpkg")
        resolved = audit.resolve_ldd_dependencies(Path(dpkg_path))
        if not resolved:
            self.skipTest(
                "the DT_NEEDED resolver found zero dependencies for the real dpkg binary"
            )
        captured = audit.capture_distro_source_provenance(
            {soname: [path] for soname, path in resolved.items()}
        )
        missing_but_real = {
            soname: path
            for soname, path in resolved.items()
            if not captured.get(soname) and path.is_file()
        }
        # A real dependency may still legitimately be omitted if it is
        # genuinely not dpkg-owned on this host (e.g. a locally built
        # library shadowing a system one) -- but every such omission
        # must be independently re-provable as "no real dpkg owner", not
        # merely a silent path-form mismatch this test can directly
        # detect by calling _dpkg_owning_package() itself.
        for soname, path in missing_but_real.items():
            self.assertIsNone(
                audit._dpkg_owning_package(path),
                f"capture_distro_source_provenance() silently dropped "
                f"{soname!r} ({path}), but _dpkg_owning_package() proves "
                f"it IS really dpkg-owned -- this is exactly the "
                f"path-form regression this test class guards against",
            )


@unittest.skipUnless(
    shutil.which("cc") is not None and shutil.which("readelf") is not None,
    "requires a real C toolchain and readelf to build requester-specific "
    "DT_NEEDED graph fixtures",
)
class RealDtNeededDependencyGraphWalkTests(unittest.TestCase):
    """Real-ELF regression tests for finding #18's core architectural fix:
    requester-specific DT_NEEDED graph walking, never a flattened `ldd`
    closure that discards which exact requester resolved which exact
    SONAME/path edge."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.cc_bin = shutil.which("cc")
        if self.cc_bin is None:
            self.skipTest("cc not available on this host")

    def _build_shared(
        self,
        relative_dir: str,
        soname: str,
        source: str,
        *,
        library_dirs: tuple[Path, ...] = (),
        libraries: tuple[str, ...] = (),
        rpath_entries: tuple[str, ...] = (),
    ) -> Path:
        output_dir = self.root / relative_dir
        output_dir.mkdir(parents=True, exist_ok=True)
        output_path = output_dir / soname
        _compile_c_binary(
            self.cc_bin,
            source,
            output_path,
            shared=True,
            soname=soname,
            library_dirs=library_dirs,
            libraries=libraries,
            rpath_entries=rpath_entries,
        )
        _make_linker_name(output_path)
        return output_path

    def _build_executable(
        self,
        relative_path: str,
        source: str,
        *,
        library_dirs: tuple[Path, ...] = (),
        libraries: tuple[str, ...] = (),
        rpath_entries: tuple[str, ...] = (),
    ) -> Path:
        output_path = self.root / relative_path
        output_path.parent.mkdir(parents=True, exist_ok=True)
        _compile_c_binary(
            self.cc_bin,
            source,
            output_path,
            library_dirs=library_dirs,
            libraries=libraries,
            rpath_entries=rpath_entries,
        )
        return output_path

    def test_resolve_dt_needed_dependency_graph_records_the_actual_requester_per_edge(
        self,
    ) -> None:
        child = self._build_shared(
            "child",
            "libchild.so.1",
            "int child_value(void) { return 7; }\n",
        )
        parent = self._build_shared(
            "parent",
            "libparent.so.1",
            "extern int child_value(void);\n"
            "int parent_value(void) { return child_value(); }\n",
            library_dirs=(child.parent,),
            libraries=("child",),
            rpath_entries=("$ORIGIN/../child",),
        )
        executable = self._build_executable(
            "bin/app",
            "extern int parent_value(void);\n"
            "int main(void) { return parent_value(); }\n",
            library_dirs=(parent.parent,),
            libraries=("parent",),
            rpath_entries=("$ORIGIN/../parent",),
        )

        edges = audit.resolve_dt_needed_dependency_graph([executable])
        self.assertIn((executable, "libparent.so.1", parent), edges)
        self.assertIn((parent, "libchild.so.1", child), edges)
        self.assertNotIn((executable, "libchild.so.1", child), edges)

    def test_resolve_dt_needed_dependency_graph_preserves_two_distinct_same_soname_resolutions_from_two_requesters(
        self,
    ) -> None:
        shared_a = self._build_shared(
            "a",
            "libshared.so.1",
            "int shared_value(void) { return 1; }\n",
        )
        shared_b = self._build_shared(
            "b",
            "libshared.so.1",
            "int shared_value(void) { return 2; }\n",
        )
        requester_a = self._build_shared(
            "req-a",
            "librequester_a.so.1",
            "extern int shared_value(void);\n"
            "int requester_a(void) { return shared_value(); }\n",
            library_dirs=(shared_a.parent,),
            libraries=("shared",),
            rpath_entries=("$ORIGIN/../a",),
        )
        requester_b = self._build_shared(
            "req-b",
            "librequester_b.so.1",
            "extern int shared_value(void);\n"
            "int requester_b(void) { return shared_value(); }\n",
            library_dirs=(shared_b.parent,),
            libraries=("shared",),
            rpath_entries=("$ORIGIN/../b",),
        )

        edges = audit.resolve_dt_needed_dependency_graph([requester_a, requester_b])
        shared_edges = [edge for edge in edges if edge[1] == "libshared.so.1"]
        self.assertCountEqual(
            shared_edges,
            [
                (requester_a, "libshared.so.1", shared_a),
                (requester_b, "libshared.so.1", shared_b),
            ],
        )

    def test_resolve_dt_needed_dependency_graph_walks_a_transitive_two_hop_chain(
        self,
    ) -> None:
        leaf = self._build_shared(
            "leaf",
            "libleaf.so.1",
            "int leaf_value(void) { return 11; }\n",
        )
        middle = self._build_shared(
            "middle",
            "libmiddle.so.1",
            "extern int leaf_value(void);\n"
            "int middle_value(void) { return leaf_value(); }\n",
            library_dirs=(leaf.parent,),
            libraries=("leaf",),
            rpath_entries=("$ORIGIN/../leaf",),
        )
        root = self._build_shared(
            "root",
            "libroot.so.1",
            "extern int middle_value(void);\n"
            "int root_value(void) { return middle_value(); }\n",
            library_dirs=(middle.parent,),
            libraries=("middle",),
            rpath_entries=("$ORIGIN/../middle",),
        )

        edges = audit.resolve_dt_needed_dependency_graph([root])
        self.assertEqual(
            {
                (requester.name, needed, resolved.name)
                for requester, needed, resolved in edges
                if needed in {"libmiddle.so.1", "libleaf.so.1"}
            },
            {
                ("libroot.so.1", "libmiddle.so.1", "libmiddle.so.1"),
                ("libmiddle.so.1", "libleaf.so.1", "libleaf.so.1"),
            },
        )


@unittest.skipUnless(
    shutil.which("dpkg-query") is not None and Path("/var/lib/dpkg/info").is_dir(),
    "requires a real Debian/Ubuntu dpkg database to authenticate a genuine "
    "system library's own architecture/per-file integrity record",
)
class RealDpkgArchitectureAndFileIntegrityTests(unittest.TestCase):
    """Round-N+ review (HIGH, "distro provenance collapses identity ...
    arch stripped" / "substituted self-consistent bytes pass"): proves
    _dpkg_package_architecture()/_dpkg_recorded_file_md5()/
    _dpkg_full_provenance_record() against a REAL dpkg database, never
    only mocks -- libz.so.1 (zlib1g) is used for the same "always
    already installed, no network/apt-get needed" reason
    RealSystemPackageProvenanceTests does."""

    def _real_libz_path(self) -> Path | None:
        for search_dir in audit._SYSTEM_LIBRARY_SEARCH_DIRS:
            candidate = search_dir / "libz.so.1"
            if candidate.exists():
                return candidate.resolve()
        return None

    def test_dpkg_package_architecture_reports_a_real_debian_architecture(
        self,
    ) -> None:
        libz_path = self._real_libz_path()
        if libz_path is None:
            self.skipTest("no real libz.so.1 system copy found on this host")
        package = audit._dpkg_owning_package(libz_path)
        if package is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        architecture = audit._dpkg_package_architecture(package)
        self.assertIsNotNone(architecture)
        # A real dpkg-reported architecture is never empty/whitespace;
        # the EXACT expected value (amd64) is asserted by
        # EXPECTED_DISTRO_PACKAGE_ARCHITECTURE's own consuming
        # validate_bundled_library_package_provenance() test coverage
        # below -- this test only proves the raw dpkg-query plumbing
        # itself returns a real, non-degenerate value.
        self.assertTrue(architecture)

    def test_dpkg_full_provenance_record_includes_a_real_package_archive_sha256(
        self,
    ) -> None:
        libz_path = self._real_libz_path()
        if libz_path is None:
            self.skipTest("no real libz.so.1 system copy found on this host")
        package = audit._dpkg_owning_package(libz_path)
        if package is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        metadata = audit._dpkg_package_metadata(package)
        if metadata is None:
            self.skipTest(f"no package metadata available for {package!r}")
        version, _source_package = metadata
        apt_show = subprocess.run(
            ["apt-cache", "show", f"{package}={version}"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        if apt_show.returncode != 0:
            self.skipTest(
                f"apt-cache could not describe the installed {package!r} package"
            )
        expected_sha = None
        for stanza in apt_show.stdout.split("\n\n"):
            fields: dict[str, str] = {}
            for line in stanza.splitlines():
                if ": " not in line:
                    continue
                key, value = line.split(": ", 1)
                fields[key] = value
            if fields.get("Package") == package and fields.get("Version") == version:
                expected_sha = fields.get("SHA256")
                break
        if not expected_sha:
            self.skipTest(
                "local APT package indexes do not expose a SHA256 for the "
                "installed libz package on this host"
            )
        record = audit._dpkg_full_provenance_record(libz_path)
        if record is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        # Round-N+ review (HIGH, "locked .deb SHA is copied from apt
        # metadata; actual archive not downloaded/hashed/extracted"):
        # the local-index-only metadata value now lives under
        # "debSha256AptMetadata" -- it is still cross-checked here
        # (both must agree on a genuine, untampered host), but it is no
        # longer the AUTHORITATIVE "debSha256" this record/the lock
        # file are validated against.
        metadata_sha = record.get("debSha256AptMetadata")
        self.assertRegex(str(metadata_sha), r"^[0-9a-f]{64}$")
        self.assertEqual(metadata_sha, expected_sha)
        if shutil.which("apt-get") is None:
            self.skipTest("apt-get unavailable -- cannot exercise real archive download")
        deb_sha256 = record.get("debSha256")
        if deb_sha256 is None:
            self.skipTest(
                "real 'apt-get download' of the installed libz package did not "
                "succeed in this environment (offline sandbox/no network egress) "
                "-- covered deterministically by the mocked "
                "RealArchiveDownloadAndExtractionTests below instead"
            )
        self.assertRegex(str(deb_sha256), r"^[0-9a-f]{64}$")
        self.assertEqual(
            deb_sha256,
            expected_sha,
            "a real, freshly downloaded .deb archive's own independently "
            "computed sha256 must agree with this host's local APT index "
            "metadata on a genuine, untampered host/mirror",
        )
        self.assertEqual(record.get("debArchiveVerification"), "verified")


    def test_dpkg_recorded_file_md5_matches_a_real_unmodified_system_file(
        self,
    ) -> None:
        libz_path = self._real_libz_path()
        if libz_path is None:
            self.skipTest("no real libz.so.1 system copy found on this host")
        package = audit._dpkg_owning_package(libz_path)
        if package is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        recorded = audit._dpkg_recorded_file_md5(package, libz_path)
        if recorded is None:
            self.skipTest(
                f"dpkg has no md5sums database entry for {package!r} on "
                "this host"
            )
        actual = md5(libz_path.read_bytes()).hexdigest()
        self.assertEqual(
            recorded,
            actual,
            "dpkg's own recorded install-time checksum for the REAL, "
            "unmodified system libz.so.1 must agree with that file's "
            "own current content -- if this ever disagrees on a real, "
            "untampered host, _dpkg_recorded_file_md5()'s own path-"
            "matching logic (not the system itself) is broken",
        )

    def test_dpkg_full_provenance_record_detects_local_tampering_of_a_real_file(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "substituted self-consistent bytes
        # pass"): copy the REAL system libz.so.1 to a scratch path,
        # locally tamper with its bytes, then force _dpkg_owning_
        # package()/_dpkg_package_metadata() to (correctly, honestly)
        # report the REAL package/version for that scratch path -- the
        # exact "package/version metadata untouched, but the actual
        # file bytes were replaced locally" scenario dpkg's own
        # per-file md5 (not the version string) is the only thing that
        # can catch.
        libz_path = self._real_libz_path()
        if libz_path is None:
            self.skipTest("no real libz.so.1 system copy found on this host")
        package = audit._dpkg_owning_package(libz_path)
        if package is None:
            self.skipTest("real libz.so.1 copy is not dpkg-owned on this host")
        recorded = audit._dpkg_recorded_file_md5(package, libz_path)
        if recorded is None:
            self.skipTest(
                f"dpkg has no md5sums database entry for {package!r} on "
                "this host"
            )
        with tempfile.TemporaryDirectory() as scratch_dir:
            scratch_path = Path(scratch_dir) / "libz.so.1"
            original_bytes = libz_path.read_bytes()
            scratch_path.write_bytes(original_bytes + b"\x00tampered-locally")
            with mock.patch.object(
                audit, "_dpkg_owning_package", return_value=package
            ), mock.patch.object(
                audit,
                "_dpkg_recorded_file_md5",
                side_effect=lambda pkg, path: (
                    recorded if path == scratch_path else None
                ),
            ):
                record = audit._dpkg_full_provenance_record(scratch_path)
            self.assertIsNotNone(record)
            self.assertEqual(record["dpkgFileIntegrity"], "mismatch")
            self.assertEqual(record["dpkgRecordedMd5"], recorded)
            self.assertNotEqual(record["dpkgActualMd5"], recorded)


class RealReplayStripAndRpathTransformTests(unittest.TestCase):
    """Round-N+ review (HIGH, "Prefer replaying the exact trusted
    packaging transform on trusted source in hermetic toolchain and
    byte-compare final"): proves replay_strip_and_rpath_transform()
    itself against a REAL patchelf/strip invocation (this project's own
    exhaustive interactive research already proved the exact pinned
    linuxdeploy/linuxdeploy-plugin-qt release's own bundled
    patchelf/strip binaries reproduce a byte-identical result this same
    way; this suite proves the PRODUCTION CODE implementing that same
    recipe does too, using whatever real patchelf/strip this host
    provides -- the replay mechanism itself is tool-version-agnostic,
    only the specific pinned binaries build-appimage.sh supplies in
    real CI are version-pinned) -- never only against a mocked
    replay_strip_and_rpath_transform() the way this module's other,
    portable (non-Linux-host) tests must."""

    def setUp(self) -> None:
        if not (shutil.which("patchelf") and shutil.which("strip")):
            self.skipTest("requires real patchelf and strip binaries")
        self.cc_bin = shutil.which("cc") or shutil.which("gcc")
        if self.cc_bin is None:
            self.skipTest("requires a real C compiler to build a genuine ELF fixture")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.root = Path(self._tmp.name)
        self.toolset = audit._build_replay_toolset(
            Path(shutil.which("patchelf")),
            Path(shutil.which("strip")),
            "system-toolchain",
        )
        self.assertIsNotNone(self.toolset)

    def _build_reference(self) -> Path:
        reference = self.root / "reference.so"
        _compile_shared_object(
            self.cc_bin,
            "int replay_fixture_entry(void) { return 1; }\n",
            reference,
        )
        return reference

    def _bundled_destination(self, *relative_parts: str) -> Path:
        # Realistic AppDir-rooted destination, so
        # _expected_rpath_for_bundled_path()'s own "usr"-anchored
        # derivation applies exactly as it does in production --
        # never a flat, non-AppDir-shaped temp filename.
        destination = self.root / "AppDir" / Path(*relative_parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        return destination

    def test_replay_reproduces_a_genuine_short_inplace_rpath_edit(self) -> None:
        reference = self._build_reference()
        final = self._bundled_destination("usr", "lib", "final_short.so")
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        self.assertEqual(expected_rpath, "$ORIGIN")
        subprocess.run(
            ["strip", "--strip-unneeded", str(final)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["patchelf", "--set-rpath", expected_rpath, str(final)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(final))

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertTrue(receipt["matched"], receipt)
        self.assertEqual(receipt["replayedSha256"], receipt["finalSha256"])

    def test_replay_accepts_a_genuine_file_linuxdeploy_left_completely_untouched(
        self,
    ) -> None:
        # Regression test for a genuine, previously-undetected bug (NOT
        # one of the numbered review findings): linuxdeploy-plugin-qt
        # does NOT unconditionally strip+patchelf EVERY bundled plugin/
        # QML module -- empirically, a file whose already-built-in
        # rpath is already sufficient for its AppDir destination is
        # copied over completely untouched (no "Setting rpath in ELF
        # file ..." log line at all; real-world example: "plugins/
        # platformthemes/libqxdgdesktopportal.so", copied byte-for-byte
        # identical to its Qt SDK reference copy, its rpath left as
        # whatever the SDK itself originally built in). The pre-fix
        # replay_strip_and_rpath_transform() only ever accepted a file
        # whose OBSERVED rpath exactly matched this project's own
        # derived `expected_rpath` -- rejecting this genuine,
        # completely-untouched file as "content_mismatch" purely
        # because ITS rpath (never rewritten by any packaging step at
        # all) does not happen to equal the value a REWRITE would have
        # produced. A bundled file whose bytes are bit-for-bit
        # identical to the pinned, hash-verified Qt SDK's own reference
        # copy is unconditionally the strongest possible proof of
        # authenticity regardless of its own (possibly untouched)
        # rpath, and must be accepted -- decided BEFORE, and
        # independently of, the expected-rpath precondition, which only
        # ever applies to a file that a packaging step actually
        # rewrote.
        reference = self._build_reference()
        # A destination shape whose derived `expected_rpath` a real
        # rewrite WOULD have produced, deliberately never applied here
        # -- reproducing "copied verbatim, no patchelf/strip step ran
        # at all" exactly as the real untouched linuxdeploy-plugin-qt
        # output does.
        final = self._bundled_destination(
            "usr", "plugins", "platformthemes", "final_untouched.so"
        )
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        self.assertEqual(audit._sha256(reference), audit._sha256(final))
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        self.assertEqual(expected_rpath, "$ORIGIN/../../lib:$ORIGIN")
        # `final`'s own real, unmodified rpath (whatever the fixture
        # compiler produced, almost certainly NOT the two-entry
        # `expected_rpath` value) must still not matter at all.
        self.assertNotEqual(
            audit._observed_rpath_or_runpath(final) or "", expected_rpath
        )

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertTrue(receipt["matched"], receipt)
        self.assertTrue(receipt.get("untouchedByPackaging"))
        self.assertFalse(receipt["stripApplied"])
        self.assertFalse(receipt["patchelfSetRpathApplied"])
        self.assertEqual(receipt["replayedSha256"], receipt["finalSha256"])

    def test_replay_reproduces_a_genuine_long_relocating_rpath_edit(self) -> None:
        reference = self._build_reference()
        # A deeply nested, but entirely LEGITIMATE, QML-module-shaped
        # destination naturally derives a long "$ORIGIN/../../.../lib"
        # expected rpath (forcing patchelf to physically relocate
        # PT_DYNAMIC/.dynstr into a freshly appended trailing LOAD
        # segment) -- never an attacker-chosen arbitrary string the way
        # the pre-fix version of this test used.
        final = self._bundled_destination(
            "usr",
            "qml",
            "Deeply",
            "Nested",
            "Module",
            "Tree",
            "With",
            "Many",
            "Levels",
            "final_long.so",
        )
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        # A QML module destination always gets a real linuxdeploy-
        # plugin-qt "<relative-to-usr/lib>:$ORIGIN" TWO-entry rpath --
        # the trailing bare "$ORIGIN" lets a module still load sibling
        # helper plugins from its own directory -- never just the
        # first entry alone.
        self.assertEqual(
            expected_rpath, "$ORIGIN/../../../../../../../../lib:$ORIGIN"
        )
        subprocess.run(
            ["strip", "--strip-unneeded", str(final)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["patchelf", "--set-rpath", expected_rpath, str(final)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(final))

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertTrue(receipt["matched"], receipt)
        self.assertEqual(receipt["replayedSha256"], receipt["finalSha256"])

    def test_replay_rejects_a_final_file_tampered_after_the_genuine_transform(
        self,
    ) -> None:
        reference = self._build_reference()
        final = self._bundled_destination("usr", "lib", "final_tampered.so")
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        subprocess.run(
            ["strip", "--strip-unneeded", str(final)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["patchelf", "--set-rpath", expected_rpath, str(final)],
            check=True,
            capture_output=True,
        )
        genuine_receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertTrue(genuine_receipt["matched"], genuine_receipt)

        tampered_bytes = bytearray(final.read_bytes())
        flip_index = len(tampered_bytes) // 2
        tampered_bytes[flip_index] ^= 0xFF
        final.write_bytes(bytes(tampered_bytes))

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertFalse(receipt["matched"])
        self.assertNotEqual(receipt["replayedSha256"], receipt["finalSha256"])

    def test_replay_skips_strip_when_reference_rpath_already_dollar_prefixed(
        self,
    ) -> None:
        # Companion to setUp's own recipe: when the STAGED reference's
        # own observed rpath is already "$"-prefixed (patchelf already
        # ran against it upstream, e.g. a linuxdeploy-plugin-qt-bundled
        # Qt library that already carries a $ORIGIN-relative rpath),
        # replay must skip the strip step entirely -- stripping an
        # already-stripped, already-patched reference a second time is
        # not part of the real observed linuxdeploy recipe.
        base = self._build_reference()
        reference = self.root / "reference_dollar.so"
        reference.write_bytes(base.read_bytes())
        reference.chmod(0o755)
        subprocess.run(
            ["patchelf", "--set-rpath", "$ORIGIN/../lib", str(reference)],
            check=True,
            capture_output=True,
        )
        final = self._bundled_destination(
            "usr", "plugins", "platforms", "final_dollar.so"
        )
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        # Real production Qt plugin destination shape ("usr/plugins/
        # <subdir>/<file>", a SIBLING of "usr/lib" -- never nested
        # beneath it): a two-entry rpath, "$ORIGIN/../../lib" back to
        # the shared library directory PLUS a trailing bare "$ORIGIN"
        # for same-directory sibling plugins, matching linuxdeploy-
        # plugin-qt's own real, observed output exactly.
        self.assertEqual(expected_rpath, "$ORIGIN/../../lib:$ORIGIN")
        subprocess.run(
            ["patchelf", "--set-rpath", expected_rpath, str(final)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(final))

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertTrue(receipt["matched"], receipt)
        self.assertFalse(receipt["stripApplied"])
        self.assertTrue(receipt["patchelfSetRpathApplied"])

    def test_replay_rejects_an_attacker_chosen_rpath_that_disagrees_with_the_trusted_destination_policy(
        self,
    ) -> None:
        # Round-N+ review (HIGH, "replay derives final_rpath from
        # candidate, authenticating attacker-selected behavior"): this
        # is the direct reversal of the pre-fix behavior, which read
        # whatever rpath `final` itself carried and replayed exactly
        # that value -- trivially "matching" any payload an attacker
        # chose, as long as the rest of a genuine strip+patchelf output
        # was otherwise reproduced. A `final` file destined for
        # "usr/lib/..." can only ever legitimately carry "$ORIGIN"; one
        # carrying a completely different, attacker-chosen rpath string
        # must now be rejected even though the rest of the transform
        # (strip + a real patchelf --set-rpath rewrite) is byte-for-byte
        # exactly what a genuine linuxdeploy run would also produce for
        # THAT (wrong) value.
        reference = self._build_reference()
        final = self._bundled_destination("usr", "lib", "final_attacker.so")
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        expected_rpath = audit._expected_rpath_for_bundled_path(final)
        self.assertEqual(expected_rpath, "$ORIGIN")
        subprocess.run(
            ["strip", "--strip-unneeded", str(final)],
            check=True,
            capture_output=True,
        )
        attacker_rpath = "/tmp/attacker-controlled-search-path"
        subprocess.run(
            ["patchelf", "--set-rpath", attacker_rpath, str(final)],
            check=True,
            capture_output=True,
        )

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, expected_rpath
        )
        self.assertFalse(receipt["matched"], receipt)
        self.assertEqual(receipt["finalObservedRpath"], attacker_rpath)
        self.assertEqual(receipt["expectedRpath"], "$ORIGIN")
        # The replay must never have even ATTEMPTED to reproduce the
        # attacker's own value -- no "replayedSha256" key at all, since
        # the mismatch is decided before ever invoking patchelf/strip a
        # second time.
        self.assertNotIn("replayedSha256", receipt)

    def test_replay_rejects_a_final_file_carrying_a_neighboring_destinations_correct_rpath(
        self,
    ) -> None:
        # Order/precedence coverage: a value that IS a genuine,
        # correctly-derived expected rpath for SOME real destination
        # shape (a Qt plugin one level under "usr/lib/plugins/<dir>")
        # is still wrong -- and must still be rejected -- when it
        # appears on a file actually destined for a DIFFERENT depth
        # (plain "usr/lib/"), proving the policy is bound to each
        # artifact's own exact destination, never merely "some
        # plausible-looking Qt-shaped value".
        reference = self._build_reference()
        final = self._bundled_destination("usr", "lib", "final_wrong_depth.so")
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        own_expected_rpath = audit._expected_rpath_for_bundled_path(final)
        self.assertEqual(own_expected_rpath, "$ORIGIN")
        neighbor_expected_rpath = audit._expected_rpath_for_appdir_relative_path(
            "usr/plugins/platforms/libqxcb.so"
        )
        self.assertEqual(neighbor_expected_rpath, "$ORIGIN/../../lib:$ORIGIN")
        self.assertNotEqual(own_expected_rpath, neighbor_expected_rpath)
        subprocess.run(
            ["strip", "--strip-unneeded", str(final)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["patchelf", "--set-rpath", neighbor_expected_rpath, str(final)],
            check=True,
            capture_output=True,
        )

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, own_expected_rpath
        )
        self.assertFalse(receipt["matched"], receipt)

    def test_replay_rejects_when_no_trusted_expected_rpath_can_be_derived(
        self,
    ) -> None:
        # A destination outside any recognizable "usr/..." AppDir
        # shape has no trusted policy value at all
        # (_expected_rpath_for_bundled_path() returns None) --
        # replay_strip_and_rpath_transform() must treat that as a hard
        # failure, never silently falling back to trusting whatever
        # rpath the candidate happens to carry.
        reference = self._build_reference()
        final = self.root / "not_an_appdir_shape.so"
        final.write_bytes(reference.read_bytes())
        final.chmod(0o755)
        subprocess.run(
            ["patchelf", "--set-rpath", "$ORIGIN", str(final)],
            check=True,
            capture_output=True,
        )
        self.assertIsNone(audit._expected_rpath_for_bundled_path(final))

        receipt = audit.replay_strip_and_rpath_transform(
            reference, final, self.toolset, None
        )
        self.assertFalse(receipt["matched"], receipt)

    def test_bind_qt_sdk_provenance_via_replay_matches_a_genuine_qt_plugin_destination(
        self,
    ) -> None:
        # Regression test for a genuine, previously-undetected bug (NOT
        # one of the numbered review findings): a real
        # linuxdeploy-plugin-qt run ALWAYS writes a TWO-entry rpath for
        # every Qt plugin/QML module destination -- the usual
        # "$ORIGIN/<relative-path-to-usr/lib>" entry, PLUS a trailing
        # bare "$ORIGIN" so the module can still load sibling plugins
        # from its own directory (empirically confirmed against real
        # `packaging/build-appimage.sh` "Setting rpath in ELF file ..."
        # log output for every "usr/plugins/**"/"usr/qml/**"
        # destination). The pre-fix _expected_rpath_for_appdir_
        # relative_path() only ever returned the FIRST entry, so
        # bind_bundled_library_to_qt_sdk_provenance() -- when given a
        # real replay_toolset, exactly as cmd_classify() always
        # supplies in production -- rejected EVERY real Qt plugin/QML
        # module as "content_mismatch" even though the bundled file was
        # byte-for-byte the genuine, untampered linuxdeploy-plugin-qt
        # output. This is the real, root cause of the "Qt plugin
        # byte-mismatch" failure category `build-appimage.sh` produced
        # on every single real end-to-end run before this fix -- fixed
        # in `_expected_rpath_for_appdir_relative_path()` itself, proven
        # here through the SAME `bind_bundled_library_to_qt_sdk_
        # provenance()` entry point cmd_classify() actually calls,
        # never merely a lower-level unit test of the rpath string
        # alone.
        qt_reference_dir = self.root / "qtsdk"
        reference = qt_reference_dir / "plugins" / "imageformats" / "libqgif.so"
        reference.parent.mkdir(parents=True)
        _compile_shared_object(
            self.cc_bin,
            "int qt_plugin_fixture_entry(void) { return 1; }\n",
            reference,
        )

        bundled = self._bundled_destination(
            "usr", "plugins", "imageformats", "libqgif.so"
        )
        bundled.write_bytes(reference.read_bytes())
        bundled.chmod(0o755)
        subprocess.run(
            ["strip", "--strip-unneeded", str(bundled)],
            check=True,
            capture_output=True,
        )
        # The exact real, two-entry rpath linuxdeploy-plugin-qt writes
        # for this exact destination depth -- a literal, independently
        # hardcoded ground-truth value (matching real observed build
        # log output), never derived by calling the function under
        # test.
        real_linuxdeploy_rpath = "$ORIGIN/../../lib:$ORIGIN"
        subprocess.run(
            ["patchelf", "--set-rpath", real_linuxdeploy_rpath, str(bundled)],
            check=True,
            capture_output=True,
        )
        self.assertNotEqual(audit._sha256(reference), audit._sha256(bundled))

        binding = audit.bind_bundled_library_to_qt_sdk_provenance(
            bundled, qt_reference_dir, replay_toolset=self.toolset
        )
        self.assertEqual(binding["status"], "matched", binding)
        self.assertEqual(binding["evidenceStrength"], "replay_byte_identical")
        self.assertTrue(binding["transformationReceipt"]["matched"])
        self.assertEqual(
            binding["transformationReceipt"]["expectedRpath"], real_linuxdeploy_rpath
        )

    def test_replay_toolset_from_manifest_rejects_a_tampered_tool_binary(
        self,
    ) -> None:
        patchelf_copy = self.root / "patchelf-copy"
        shutil.copyfile(shutil.which("patchelf"), patchelf_copy)
        patchelf_copy.chmod(0o755)
        original_sha256 = audit._sha256(patchelf_copy)

        manifest = {
            "replayTools": {
                "linuxdeploy": {
                    "toolLabel": "linuxdeploy",
                    "patchelfPath": str(patchelf_copy),
                    "patchelfSha256": original_sha256,
                    "stripPath": shutil.which("strip"),
                    "stripSha256": audit._sha256(Path(shutil.which("strip"))),
                }
            }
        }

        toolset = audit._replay_toolset_from_manifest(manifest, "linuxdeploy")
        self.assertIsNotNone(toolset)

        with open(patchelf_copy, "ab") as handle:
            handle.write(b"\x00tampered-tool-binary")

        toolset_after_tamper = audit._replay_toolset_from_manifest(
            manifest, "linuxdeploy"
        )
        self.assertIsNone(toolset_after_tamper)


if __name__ == "__main__":
    unittest.main()
