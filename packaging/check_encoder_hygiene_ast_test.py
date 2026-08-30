#!/usr/bin/env python3
"""REAL libclang mutation tests for packaging/check_encoder_hygiene.py's
AST-scanning core: _scan_headers()/_scan_sources() (source-only
declaration discovery, output/inout-parameter detection, base-class/
using-declaration inheritance-exposure attribution) and
_audit_inclusion_graph() (resolved-inclusion-graph dependency-direction
enforcement, independent of #include spelling, plus system-header/
external-root exemption).

Unlike packaging/check_encoder_hygiene_test.py (which deliberately stays
compiler-free, exercising only pure decision logic against synthetic
Finding/AllowlistEntry records -- see that file's own module docstring),
every test here writes small, real, on-disk C++ fixture files under a
scratch directory rooted in this repository's own gitignored build/
directory (never the system temp directory), parses them with the SAME
real libclang this project's actual encoder-hygiene check uses (found
via ceh._find_libclang(), the identical code path -- no separate/fake
Clang installation, no synthetic Finding construction standing in for a
real compiler result), and asserts on the REAL Finding/violation objects
those real AST-scanning functions produce.

Several review rounds specifically rejected prior test coverage for
being a source-text regex/parser, or for asserting only synthetic
Finding-shaped data rather than real compiler output -- this file exists
to close exactly that gap: every fail-before/pass-after pair below is a
real compile, not a hand-constructed Finding.

Requires a real libclang to be discoverable (see ceh._find_libclang());
this is the same requirement `mise run contracts:check-encoder-hygiene`
itself has, and this file is run as `mise run
contracts:test-encoder-hygiene-ast-mutations` in CI's existing
"encoder-hygiene" job (which already installs `clang libclang-dev`),
immediately alongside the other encoder-hygiene test/check steps.

Run directly: `python3 packaging/check_encoder_hygiene_ast_test.py`
"""

from __future__ import annotations

import ctypes
import platform
import shutil
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encoder_hygiene as ceh


def _sysroot_args() -> list[str]:
    # Mirrors run_check()'s own is_macos/_macos_sdk_sysroot() logic
    # exactly, so these fixtures compile identically to how the real
    # check itself invokes libclang on both this project's macOS
    # development machines and its Ubuntu CI runners.
    if platform.system() == "Darwin":
        return ["-isysroot", ceh._macos_sdk_sysroot()]
    return []


class _RealLibclangTestCase(unittest.TestCase):
    """Shared setup: a real libclang + CXIndex, and a fresh scratch
    directory tree (under this repo's own gitignored build/ directory)
    per test, so a mid-test failure or leftover fixture from a previous
    run can never mask or contaminate the next one."""

    def setUp(self) -> None:
        self.clang = ceh._LibClang(ceh._find_libclang())
        self.idx = self.clang.lib.clang_createIndex(0, 0)
        self.assertTrue(self.idx, "clang_createIndex() failed")

        repo_root = Path(__file__).resolve().parent.parent
        scratch_parent = repo_root / "build" / "ast-mutation-fixtures"
        scratch_parent.mkdir(parents=True, exist_ok=True)
        # A per-test-id subdirectory (not tempfile.mkdtemp()) keeps every
        # fixture's own on-disk layout stable/inspectable across a single
        # run for debugging, while still starting genuinely empty.
        self.scratch = scratch_parent / self.id().rsplit(".", 1)[-1]
        if self.scratch.exists():
            shutil.rmtree(self.scratch)
        self.scratch.mkdir(parents=True)
        self.repo_root = self.scratch
        self.sysroot_args = _sysroot_args()

    def tearDown(self) -> None:
        self.clang.lib.clang_disposeIndex(self.idx)
        shutil.rmtree(self.scratch, ignore_errors=True)

    def _write(self, relative: str, content: str) -> Path:
        path = self.scratch / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _scan_header_fixture(
        self, header: Path, allowed_closure: frozenset[Path], external_roots: frozenset[Path] = frozenset()
    ) -> tuple[list[ceh.Finding], list[str]]:
        seen: set[tuple] = set()
        structural_violations: list[str] = []
        findings = ceh._scan_headers(
            self.clang,
            self.idx,
            [header],
            ["-std=c++23"],
            self.sysroot_args,
            self.repo_root,
            external_roots,
            allowed_closure,
            seen,
            structural_violations,
        )
        return findings, structural_violations

    def _scan_source_fixture(
        self, source: Path, allowed_closure: frozenset[Path], seen: set[tuple] | None = None
    ) -> tuple[list[str], list[ceh.Finding]]:
        source_abs = str(source.resolve())
        compile_commands = [
            {
                "file": source_abs,
                "command": f"c++ -std=c++23 -c {source_abs} -o {source_abs}.o",
            }
        ]
        return ceh._scan_sources(
            self.clang,
            self.idx,
            [source],
            compile_commands,
            self.sysroot_args,
            self.repo_root,
            frozenset(),
            allowed_closure,
            seen if seen is not None else set(),
        )

    def _audit_inclusion_graph_fixture(
        self, header: Path, allowed_closure: frozenset[Path], external_roots: frozenset[Path] = frozenset()
    ) -> list[str]:
        tu, wrapper_filename = ceh._parse_header_as_own_tu(self.clang, self.idx, header, ["-std=c++23"], self.sysroot_args)
        try:
            return ceh._audit_inclusion_graph(
                self.clang, tu, header, wrapper_filename, allowed_closure, external_roots
            )
        finally:
            self.clang.lib.clang_disposeTranslationUnit(tu)


class SourceOnlyDeclarationScanTests(_RealLibclangTestCase):
    """_scan_sources() must independently collect QJson-family Findings
    from a genuinely new, source-only, externally-linked declaration --
    not merely audit its #include graph -- while never double-flagging
    an out-of-line definition of an already header-declared/counted
    symbol, and never flagging something no other translation unit could
    actually call (internal linkage, or a private member of a
    source-only class)."""

    def test_new_public_namespace_scope_function_with_no_header_declaration_is_flagged(self) -> None:
        # No header at all declares encodeSomethingNew(): a genuinely new,
        # source-only, externally-linked encoder.
        source = self._write(
            "Foo.cpp",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "QJsonObject encodeSomethingNew() { return QJsonObject{}; }\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 1)
        self.assertIn("encodeSomethingNew", findings[0].display_name)
        self.assertEqual(findings[0].file, "Foo.cpp")

    def test_internal_linkage_static_function_is_not_flagged(self) -> None:
        source = self._write(
            "Bar.cpp",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "static QJsonObject helperEncode() { return QJsonObject{}; }\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        self.assertEqual(findings, [])

    def test_anonymous_namespace_function_is_not_flagged(self) -> None:
        source = self._write(
            "Baz.cpp",
            "struct QJsonObject {};\n"
            "namespace {\n"
            "QJsonObject helperEncode2() { return QJsonObject{}; }\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        self.assertEqual(findings, [])

    def test_private_member_of_source_only_class_is_not_flagged(self) -> None:
        source = self._write(
            "Qux.cpp",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "class OnlyInSource {\n"
            "public:\n"
            "    OnlyInSource() = default;\n"
            "private:\n"
            "    QJsonObject encodePrivate() { return QJsonObject{}; }\n"
            "};\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        self.assertEqual(findings, [])

    def test_out_of_line_definition_of_already_header_declared_function_is_not_double_flagged(self) -> None:
        header = self._write(
            "Bar2.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "QJsonObject encodeBar2();\n"
            "}\n",
        )
        source = self._write(
            "Bar2.cpp",
            f'#include "{header.resolve()}"\n'
            "namespace Arkham {\n"
            "QJsonObject encodeBar2() { return QJsonObject{}; }\n"
            "}\n",
        )
        allowed_closure = frozenset({header.resolve()})
        # A single, shared `seen` set across both phases -- exactly like
        # run_check() itself -- so the source scan can observe that this
        # declaration was already counted by the header scan.
        seen: set[tuple] = set()
        header_violations: list[str] = []
        header_findings = ceh._scan_headers(
            self.clang,
            self.idx,
            [header],
            ["-std=c++23"],
            self.sysroot_args,
            self.repo_root,
            frozenset(),
            allowed_closure,
            seen,
            header_violations,
        )
        self.assertEqual(header_violations, [])
        self.assertEqual(len(header_findings), 1)

        source_violations, source_findings = self._scan_source_fixture(source, allowed_closure, seen=seen)
        self.assertEqual(source_violations, [])
        self.assertEqual(
            source_findings,
            [],
            "the out-of-line .cpp definition of an already header-declared/counted "
            "function must never be re-flagged as a NEW source-only declaration",
        )


class OutputParameterAndInheritanceExposureTests(_RealLibclangTestCase):
    """_is_encoder_shaped()'s non-const output/inout-parameter detection,
    and _inherited_and_reexported_encoders()'s base-class/using-
    declaration exposure walk, both exercised end-to-end through the
    real _scan_headers() entry point."""

    def test_public_non_const_reference_output_parameter_is_flagged(self) -> None:
        header = self._write(
            "OutParam.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "void encode(QJsonObject &out);\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 1)
        self.assertIn("output/inout parameter", findings[0].canonical_return_type)

    def test_public_non_const_pointer_output_parameter_is_flagged(self) -> None:
        header = self._write(
            "OutParamPtr.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "void encodePtr(QJsonObject *out);\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 1)
        self.assertIn("output/inout parameter", findings[0].canonical_return_type)

    def test_public_const_reference_input_parameter_is_not_flagged(self) -> None:
        header = self._write(
            "InParam.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "void decode(const QJsonObject &in);\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        self.assertEqual(findings, [])

    def test_public_base_class_encoder_is_exposed_through_derived_class(self) -> None:
        header = self._write(
            "PublicInherit.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "class Base {\n"
            "public:\n"
            "    QJsonObject toJson() const;\n"
            "};\n"
            "class Derived : public Base {\n"
            "public:\n"
            "    int extra = 0;\n"
            "};\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        # One direct Finding for Base::toJson()'s own public declaration,
        # plus one SEPARATE inherited-exposure Finding attributed to
        # Derived (proving a derived class newly exposing an
        # already-declared encoder via plain public inheritance -- no
        # new textual declaration of its own -- is not silently missed).
        self.assertEqual(len(findings), 2)
        own_declaration = [f for f in findings if "exposed via inheritance" not in f.display_name]
        inherited = [f for f in findings if "exposed via inheritance" in f.display_name]
        self.assertEqual(len(own_declaration), 1)
        self.assertEqual(len(inherited), 1)
        self.assertIn("toJson", inherited[0].display_name)

    def test_private_inheritance_does_not_expose_base_encoder(self) -> None:
        header = self._write(
            "PrivateInherit.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "class Base5 {\n"
            "public:\n"
            "    QJsonObject toJson5() const;\n"
            "};\n"
            "class Derived5 : private Base5 {\n"
            "public:\n"
            "    int z = 0;\n"
            "};\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        # ONLY Base5::toJson5's own direct declaration -- private
        # inheritance must never additionally expose it through Derived5.
        self.assertEqual(len(findings), 1)
        self.assertNotIn("exposed via inheritance", findings[0].display_name)

    def test_using_declaration_reexports_an_otherwise_private_base_encoder(self) -> None:
        header = self._write(
            "UsingReexport.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "class Base4 {\n"
            "public:\n"
            "    QJsonObject toJson4() const;\n"
            "};\n"
            "class Derived4 : private Base4 {\n"
            "public:\n"
            "    using Base4::toJson4;\n"
            "};\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        # Private inheritance alone would block exposure (see the
        # previous test) -- the explicit `using Base4::toJson4;`
        # re-exports it anyway, and must still be caught: one direct
        # Finding for Base4::toJson4's own declaration, one inherited
        # Finding attributed to Derived4's using-declaration.
        self.assertEqual(len(findings), 2)
        inherited = [f for f in findings if "exposed via inheritance" in f.display_name]
        self.assertEqual(len(inherited), 1)
        self.assertIn("toJson4", inherited[0].display_name)


class InclusionGraphDirectionAndClassificationTests(_RealLibclangTestCase):
    """_audit_inclusion_graph(): every resolved file the compiler itself
    reports reaching, regardless of #include spelling, must be a member
    of allowed_closure unless it is a real system header or an
    explicitly-registered external root."""

    def _forbidden_header(self) -> Path:
        return self._write("forbidden/Forbidden.h", "struct QJsonObject {};\n")

    def test_bare_include_of_forbidden_header_via_search_path_is_a_violation(self) -> None:
        forbidden = self._forbidden_header()
        allowed = self._write("domain/Allowed.h", '#include "Forbidden.h"\n')
        # Bare spelling only resolves via an explicit -I search path onto
        # the forbidden directory -- exactly like a real project's
        # include-path configuration might permit.
        tu, wrapper_filename = ceh._parse_header_as_own_tu(
            self.clang, self.idx, allowed, ["-std=c++23", "-I", str(forbidden.parent)], self.sysroot_args
        )
        try:
            violations = ceh._audit_inclusion_graph(
                self.clang, tu, allowed, wrapper_filename, frozenset({allowed.resolve()}), frozenset()
            )
        finally:
            self.clang.lib.clang_disposeTranslationUnit(tu)
        self.assertEqual(len(violations), 1)
        self.assertIn(str(forbidden.resolve()), violations[0])

    def test_relative_dotdot_include_of_forbidden_header_is_a_violation(self) -> None:
        forbidden = self._forbidden_header()
        allowed = self._write("domain/Allowed.h", '#include "../forbidden/Forbidden.h"\n')
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(len(violations), 1)
        self.assertIn(str(forbidden.resolve()), violations[0])

    def test_absolute_include_of_forbidden_header_is_a_violation(self) -> None:
        forbidden = self._forbidden_header()
        allowed = self._write("domain/Allowed.h", f'#include "{forbidden.resolve()}"\n')
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(len(violations), 1)
        self.assertIn(str(forbidden.resolve()), violations[0])

    def test_symlink_alias_include_of_forbidden_header_resolves_to_same_violation(self) -> None:
        forbidden = self._forbidden_header()
        alias = self.scratch / "domain" / "ForbiddenAlias.h"
        alias.parent.mkdir(parents=True, exist_ok=True)
        alias.symlink_to(forbidden)
        allowed = self._write("domain/Allowed.h", '#include "ForbiddenAlias.h"\n')
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(len(violations), 1)
        # The violation must name the symlink's REAL (resolved) target,
        # not merely its lexical alias name -- proving identity is
        # checked by resolved path, not spelling.
        self.assertIn(str(forbidden.resolve()), violations[0])

    def test_include_of_another_closure_member_is_not_a_violation(self) -> None:
        sibling = self._write("domain/Sibling.h", "struct QJsonObject {};\n")
        allowed = self._write("domain/Allowed.h", '#include "Sibling.h"\n')
        violations = self._audit_inclusion_graph_fixture(
            allowed, frozenset({allowed.resolve(), sibling.resolve()})
        )
        self.assertEqual(violations, [])

    def test_include_of_registered_external_root_member_is_exempt(self) -> None:
        external_header = self._write("external/ExternalLib.h", "struct QJsonObject {};\n")
        allowed = self._write("domain/Allowed.h", f'#include "{external_header.resolve()}"\n')
        violations = self._audit_inclusion_graph_fixture(
            allowed,
            frozenset({allowed.resolve()}),
            external_roots=frozenset({(self.scratch / "external").resolve()}),
        )
        self.assertEqual(violations, [])

    def test_include_of_unregistered_root_member_with_same_basename_as_external_is_still_a_violation(self) -> None:
        # A file that merely shares a basename with something living
        # under a real external root, but is NOT itself under that root,
        # must still be audited normally -- proving the exemption is
        # keyed on resolved path membership, never basename.
        self._write("external/ExternalLib.h", "// the real external one\n")
        forbidden_lookalike = self._write("forbidden/ExternalLib.h", "struct QJsonObject {};\n")
        allowed = self._write("domain/Allowed.h", f'#include "{forbidden_lookalike.resolve()}"\n')
        violations = self._audit_inclusion_graph_fixture(
            allowed,
            frozenset({allowed.resolve()}),
            external_roots=frozenset({(self.scratch / "external").resolve()}),
        )
        self.assertEqual(len(violations), 1)
        self.assertIn(str(forbidden_lookalike.resolve()), violations[0])

    def test_include_of_real_system_header_is_exempt(self) -> None:
        allowed = self._write("domain/Allowed.h", "#include <vector>\n")
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(
            violations,
            [],
            "a genuine compiler/system-toolchain header (<vector>, reached via the "
            "compiler's own default system include paths) must be exempt via "
            "clang_Location_isInSystemHeader(), never treated as an unregistered "
            "project file",
        )

    def test_wrappers_own_synthetic_main_file_is_never_misclassified_as_a_violation(self) -> None:
        # A header with no #includes of its own at all: the ONLY file
        # clang_getInclusions() reports is the wrapper's own synthetic
        # "main file" entry, which must be recognized and skipped by
        # exact resolved identity -- never appear as a violation itself.
        allowed = self._write("domain/Standalone.h", "struct QJsonObject {};\n")
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(violations, [])


if __name__ == "__main__":
    unittest.main()
