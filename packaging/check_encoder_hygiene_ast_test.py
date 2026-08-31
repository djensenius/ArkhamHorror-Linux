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
import json
import os
import platform
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_encoder_hygiene as ceh


def _declaration_findings(
    findings: list[ceh.Finding],
) -> list[ceh.Finding]:
    return [
        finding
        for finding in findings
        if "@type-surface@" not in finding.usr
    ]


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
        self,
        header: Path,
        allowed_closure: frozenset[Path],
        external_roots: frozenset[Path] = frozenset(),
        arguments: tuple[str, ...] = ("-std=c++23",),
        owned_paths: ceh.OwnedPathPolicy | None = None,
    ) -> tuple[list[ceh.Finding], list[str]]:
        seen: set[tuple] = set()
        structural_violations: list[str] = []
        context = ceh.CompileContext(
            source=header.resolve(),
            directory=self.scratch.resolve(),
            arguments=arguments,
            output=(self.scratch / "CMakeFiles/fixture.dir/header.o").resolve(),
            target="fixture",
            configuration="",
        )
        findings = ceh._scan_headers(
            self.clang,
            self.idx,
            [header],
            [context],
            self.sysroot_args,
            self.repo_root,
            external_roots,
            allowed_closure,
            seen,
            structural_violations,
            owned_paths,
        )
        return findings, structural_violations

    def _scan_source_fixture(
        self, source: Path, allowed_closure: frozenset[Path], seen: set[tuple] | None = None
    ) -> tuple[list[str], list[ceh.Finding]]:
        source_abs = str(source.resolve())
        compile_commands = [
            {
                "directory": str(self.scratch.resolve()),
                "file": source_abs,
                "command": f"c++ -std=c++23 -c {source_abs} -o {source_abs}.o",
                "output": str(
                    (self.scratch / f"CMakeFiles/fixture.dir/{source.name}.o").resolve()
                ),
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

    def test_cursor_kind_abi_is_runtime_verified_and_drift_fails_closed(
        self,
    ) -> None:
        self.assertEqual(ceh._CXCursor_LambdaExpr, 144)
        self.assertEqual(ceh._CXCursor_ObjCBoolLiteralExpr, 145)
        self.assertEqual(ceh._CXCursor_TranslationUnit, 350)
        expected = ceh._EXPECTED_CURSOR_KIND_SPELLINGS[
            ceh._CXCursor_LambdaExpr
        ]
        ceh._EXPECTED_CURSOR_KIND_SPELLINGS[
            ceh._CXCursor_LambdaExpr
        ] = "NotLambdaExpr"
        try:
            with self.assertRaisesRegex(
                ceh.EncoderHygieneError,
                "cursor-kind ABI does not match",
            ):
                self.clang._verify_cursor_kind_constants()
        finally:
            ceh._EXPECTED_CURSOR_KIND_SPELLINGS[
                ceh._CXCursor_LambdaExpr
            ] = expected

    def test_new_public_namespace_scope_function_with_no_header_declaration_is_flagged(self) -> None:
        # No header at all declares encodeSomethingNew(): a genuinely new,
        # source-only, externally-linked encoder.
        source = self._write(
            "Foo.cpp",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "QJsonObject encodeSomethingNew() { return QJsonObject{}; }\n"
            "}\n"
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 1)
        self.assertIn("encodeSomethingNew", findings[0].display_name)
        self.assertEqual(findings[0].file, "Foo.cpp")
        self.assertFalse(findings[0].body_surface)

    def test_function_body_locals_temporaries_lambdas_and_local_classes_are_flagged(
        self,
    ) -> None:
        source = self._write(
            "LocalWire.cpp",
            "struct Bytes {};\n"
            "struct QString {};\n"
            "struct QVariant {};\n"
            "template<class K, class V> struct QMap {};\n"
            "template<class T> struct QList {};\n"
            "template<class K, class V> struct QHash {};\n"
            "using QVariantMap = QMap<QString, QVariant>;\n"
            "using QVariantList = QList<QVariant>;\n"
            "using QVariantHash = QHash<QString, QVariant>;\n"
            "struct QJsonObject {};\n"
            "struct QJsonDocument {\n"
            "  QJsonDocument(QJsonObject);\n"
            "  Bytes toJson() const;\n"
            "};\n"
            "void submit() {\n"
            "  QJsonObject body;\n"
            "  static QJsonDocument cached(body);\n"
            "  auto bytes = QJsonDocument(body).toJson();\n"
            "  auto lambda = [] { QJsonObject nested; };\n"
            "  struct Local {\n"
            "    QJsonObject member;\n"
            "    QJsonObject encode() { return {}; }\n"
            "  };\n"
            "  Local local;\n"
            "  QVariantMap map; QVariantList list; QVariantHash hash;\n"
            "  int harmless = 0;\n"
            "  (void)bytes; (void)lambda; (void)local;\n"
            "  (void)map; (void)list; (void)hash; (void)harmless;\n"
            "}\n"
            "QJsonObject alreadyForbidden() {\n"
            "  QJsonDocument nestedDoc(QJsonObject{});\n"
            "  return {};\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        displays = {finding.display_name for finding in findings}
        self.assertTrue(
            {
                "body",
                "cached",
                "nested",
                "member",
                "encode()",
                "map",
                "list",
                "hash",
                "nestedDoc",
            }.issubset(displays)
        )
        self.assertFalse(any("harmless" in display for display in displays))
        self.assertTrue(
            all(
                finding.body_surface
                for finding in findings
                if finding.display_name
                in {
                    "body",
                    "cached",
                    "nested",
                    "member",
                    "encode()",
                    "map",
                    "list",
                    "hash",
                    "nestedDoc",
                }
            )
        )
        self.assertTrue(
            any(
                "QJsonDocument" in finding.canonical_return_type
                and "type-surface" in finding.usr
                for finding in findings
            )
        )

    def test_direct_lambda_parameters_are_audited_without_local_type_crutches(
        self,
    ) -> None:
        source = self._write(
            "LambdaCallbacks.cpp",
            "struct QJsonObject {};\n"
            "template<class Callback> void registerCallback(Callback) {}\n"
            "void callbacks() {\n"
            "  registerCallback([](QJsonObject &output) { (void)output; });\n"
            "  registerCallback([](const QJsonObject &mutableInput) mutable {\n"
            "    (void)mutableInput;\n"
            "  });\n"
            "  registerCallback([]<class T>(T &generic, QJsonObject &genericOutput) {\n"
            "    (void)generic; (void)genericOutput;\n"
            "  });\n"
            "  registerCallback([](int &) {\n"
            "    registerCallback([](QJsonObject &nested) { (void)nested; });\n"
            "  });\n"
            "  registerCallback([captured = QJsonObject{}]() -> QJsonObject {\n"
            "    return captured;\n"
            "  });\n"
            "  registerCallback([](int &safe) { (void)safe; });\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        names = {finding.display_name for finding in findings}
        self.assertTrue(
            {"output", "mutableInput", "genericOutput", "nested"}.issubset(names)
        )
        self.assertNotIn("safe", names)
        self.assertFalse(
            any(
                str(self.repo_root) in finding.canonical_return_type
                for finding in findings
            )
        )
        self.assertTrue(
            any(
                "unresolved-dependent" in finding.canonical_return_type
                for finding in findings
            )
        )
        self.assertTrue(
            any(
                finding.line in {14, 15, 16}
                and "QJsonObject" in finding.canonical_return_type
                for finding in findings
            )
        )
        self.assertTrue(
            all(
                finding.body_surface
                for finding in findings
                if finding.display_name
                in {"output", "mutableInput", "genericOutput", "nested"}
            )
        )

    def test_concrete_function_template_arguments_close_dependent_bodies(
        self,
    ) -> None:
        source = self._write(
            "DependentBody.cpp",
            "struct QJsonObject {};\n"
            "struct QJsonDocument {\n"
            "  template<class Value> explicit QJsonDocument(const Value &);\n"
            "};\n"
            "struct SafeDocument {\n"
            "  template<class Value> explicit SafeDocument(const Value &);\n"
            "};\n"
            "template<class Object, class Document>\n"
            "void encode(int) { Object body; Document doc(body); }\n"
            "template<class Object, class Document>\n"
            "void encode(double) { Object body; Document doc(body); }\n"
            "template<class Object, class Document = QJsonDocument>\n"
            "void encodeDefault() { Object body; Document doc(body); }\n"
            "using ObjectAlias = QJsonObject;\n"
            "void instantiate() {\n"
            "  encode<QJsonObject, QJsonDocument>(0);\n"
            "  encode<ObjectAlias, QJsonDocument>(0.0);\n"
            "  encodeDefault<int>();\n"
            "  encode<int, SafeDocument>(0);\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        call_lines = {16, 17, 18}
        observed_call_lines = {
            finding.line
            for finding in findings
            if finding.line in call_lines
            and (
                "QJsonObject" in finding.canonical_return_type
                or "QJsonDocument" in finding.canonical_return_type
            )
        }
        self.assertEqual(observed_call_lines, call_lines)
        self.assertFalse(
            any(finding.line == 19 for finding in findings)
        )

    def test_namespace_specialization_references_are_not_hidden_by_auto(
        self,
    ) -> None:
        source = self._write(
            "NamespaceSpecializations.cpp",
            "struct QJsonObject {};\n"
            "struct Safe {};\n"
            "template<class T> int serializeInternally() { return sizeof(T); }\n"
            "template<class T> struct Worker {\n"
            "  int run() { return sizeof(T); }\n"
            "};\n"
            "static auto encoder = &serializeInternally<QJsonObject>;\n"
            "struct Registry {\n"
            "  static inline auto member = &Worker<QJsonObject>::run;\n"
            "};\n"
            "template int serializeInternally<QJsonObject>();\n"
            "static auto safeEncoder = &serializeInternally<Safe>;\n"
            "static auto safeMember = &Worker<Safe>::run;\n"
            "struct QualifiedOwner {\n"
            "  enum Status { Ready };\n"
            "  QJsonObject unrelated;\n"
            "};\n"
            "static int safeQualified(QualifiedOwner::Status) { return 0; }\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        forbidden_lines = {
            finding.line
            for finding in findings
            if "QJsonObject" in finding.canonical_return_type
        }
        self.assertTrue({7, 9}.issubset(forbidden_lines))
        self.assertFalse(any(finding.line in {12, 13} for finding in findings))
        self.assertFalse(
            any(
                finding.line == 18
                for finding in findings
            )
        )
        self.assertTrue(
            any(
                finding.line == 7
                and "kind=43" in finding.canonical_return_type
                and not finding.body_surface
                for finding in findings
            )
        )

    def test_header_namespace_and_static_member_specializations_are_audited(
        self,
    ) -> None:
        header = self._write(
            "NamespaceSpecializations.h",
            "#pragma once\n"
            "struct QJsonObject {};\n"
            "struct Safe {};\n"
            "template<class T> inline int serializeInternally() {\n"
            "  return sizeof(T);\n"
            "}\n"
            "inline auto headerEncoder = &serializeInternally<QJsonObject>;\n"
            "struct HeaderRegistry {\n"
            "  static inline auto encoder = &serializeInternally<QJsonObject>;\n"
            "};\n"
            "inline auto safeEncoder = &serializeInternally<Safe>;\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        forbidden_lines = {
            finding.line
            for finding in findings
            if "QJsonObject" in finding.canonical_return_type
        }
        self.assertTrue({7, 9}.issubset(forbidden_lines))
        self.assertFalse(any(finding.line == 11 for finding in findings))

    def test_internal_linkage_static_function_is_structurally_rejected(self) -> None:
        source = self._write(
            "Bar.cpp",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "static QJsonObject helperEncode() { return QJsonObject{}; }\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].linkage, ceh._CXLinkage_Internal)

    def test_anonymous_namespace_function_is_structurally_rejected(self) -> None:
        source = self._write(
            "Baz.cpp",
            "struct QJsonObject {};\n"
            "namespace {\n"
            "QJsonObject helperEncode2() { return QJsonObject{}; }\n"
            "}\n",
        )
        violations, findings = self._scan_source_fixture(source, frozenset())
        self.assertEqual(violations, [])
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 1)
        self.assertIn(
            findings[0].linkage,
            (ceh._CXLinkage_Internal, ceh._CXLinkage_UniqueExternal),
        )

    def test_private_member_of_source_only_class_is_structurally_rejected(self) -> None:
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
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].access, ceh._CX_CXXPrivate)

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
        # A single shared `seen` set across both phases, exactly like
        # run_check(). The physical declaration is the same, but its header
        # wrapper and source-TU observations must remain separately visible.
        seen: set[tuple] = set()
        header_violations: list[str] = []
        header_findings = ceh._scan_headers(
            self.clang,
            self.idx,
            [header],
            [
                ceh.CompileContext(
                    source=header.resolve(),
                    directory=self.scratch.resolve(),
                    arguments=("-std=c++23",),
                    output=(self.scratch / "CMakeFiles/fixture.dir/header.o").resolve(),
                    target="fixture",
                    configuration="",
                )
            ],
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
        source_findings = _declaration_findings(source_findings)
        self.assertEqual(len(source_findings), 1)
        self.assertEqual(source_findings[0].usr, header_findings[0].usr)
        self.assertEqual(source_findings[0].offset, header_findings[0].offset)

    def test_source_tu_macro_context_discovers_conditional_header_declarations(self) -> None:
        header = self._write(
            "Conditional.h",
            "struct QJsonObject {};\n"
            "#ifdef SOURCE_LOCAL_ENCODER\n"
            "namespace Arkham {\n"
            "QJsonObject conditionalNamespace();\n"
            "struct ConditionalMember {\n"
            "  QJsonObject member();\n"
            "  friend QJsonObject friendEncoder(const ConditionalMember &);\n"
            "  template<class T> QJsonObject templated(T) { return {}; }\n"
            "};\n"
            "#define DECLARE_ENCODER(name) QJsonObject name();\n"
            "DECLARE_ENCODER(macroEncoder)\n"
            "}\n"
            "#endif\n",
        )
        source = self._write(
            "Conditional.cpp",
            "#define SOURCE_LOCAL_ENCODER\n"
            '#include "Conditional.h"\n'
            "namespace Arkham {\n"
            "QJsonObject conditionalNamespace() { return {}; }\n"
            "QJsonObject ConditionalMember::member() { return {}; }\n"
            "QJsonObject friendEncoder(const ConditionalMember &) { return {}; }\n"
            "QJsonObject macroEncoder() { return {}; }\n"
            "}\n",
        )
        closure = frozenset({header.resolve()})
        standalone, violations = self._scan_header_fixture(header, closure)
        self.assertEqual(violations, [])
        self.assertEqual(standalone, [])

        source_violations, findings = self._scan_source_fixture(source, closure)
        self.assertEqual(source_violations, [])
        names = {
            finding.display_name.split("(", 1)[0]
            for finding in _declaration_findings(findings)
        }
        self.assertEqual(
            names,
            {
                "conditionalNamespace",
                "member",
                "friendEncoder",
                "templated",
                "macroEncoder",
            },
        )

    def test_distinct_target_macro_contexts_do_not_suppress_overloads(self) -> None:
        header = self._write(
            "ContextOverload.h",
            "struct QJsonObject {};\n"
            "#if MODE == 1\n"
            "QJsonObject contextual(int);\n"
            "#elif MODE == 2\n"
            "QJsonObject contextual(double);\n"
            "#endif\n",
        )
        source = self._write(
            "ContextOverload.cpp",
            '#include "ContextOverload.h"\n'
            "#if MODE == 1\n"
            "QJsonObject contextual(int) { return {}; }\n"
            "#elif MODE == 2\n"
            "QJsonObject contextual(double) { return {}; }\n"
            "#endif\n",
        )
        source_abs = str(source.resolve())
        commands = []
        for mode, target in ((1, "domain"), (2, "consumer")):
            output = self.scratch / f"CMakeFiles/{target}.dir/context.o"
            commands.append(
                {
                    "directory": str(self.scratch.resolve()),
                    "file": source_abs,
                    "arguments": [
                        "clang++",
                        "-std=c++23",
                        f"-DMODE={mode}",
                        "-c",
                        source_abs,
                    ],
                    "output": str(output.resolve()),
                }
            )
        violations, findings = ceh._scan_sources(
            self.clang,
            self.idx,
            [source],
            commands,
            self.sysroot_args,
            self.repo_root,
            frozenset(),
            frozenset({header.resolve()}),
            set(),
        )
        self.assertEqual(violations, [])
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 2)
        self.assertEqual(len({finding.usr for finding in findings}), 2)

    def test_compile_entry_working_directory_and_arguments_array_are_exact(self) -> None:
        work = self.scratch / "target-build"
        include = work / "relative-include"
        include.mkdir(parents=True)
        header = include / "Context.h"
        header.write_text("struct QJsonObject {};\n", encoding="utf-8")
        source = work / "Source.cpp"
        source.write_text(
            '#include "Context.h"\nQJsonObject exactDirectoryEncoder() { return {}; }\n',
            encoding="utf-8",
        )
        commands = [
            {
                "directory": str(work),
                "file": "Source.cpp",
                "arguments": [
                    "clang++",
                    "-std=c++23",
                    "-Irelative-include",
                    "-c",
                    "Source.cpp",
                ],
                "output": "CMakeFiles/exact-context.dir/Source.cpp.o",
            }
        ]
        violations, findings = ceh._scan_sources(
            self.clang,
            self.idx,
            [source],
            commands,
            self.sysroot_args,
            self.repo_root,
            frozenset(),
            frozenset({header.resolve()}),
            set(),
        )
        self.assertEqual(violations, [])
        findings = _declaration_findings(findings)
        self.assertEqual(len(findings), 1)
        self.assertIn("exactDirectoryEncoder", findings[0].display_name)

    def test_redeclared_surface_keeps_two_physical_occurrences(self) -> None:
        header = self._write(
            "DuplicateDeclaration.h",
            "struct QJsonObject {};\n"
            "QJsonObject duplicated();\n"
            "QJsonObject duplicated();\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        duplicates = [
            finding
            for finding in findings
            if finding.display_name.startswith("duplicated(")
        ]
        self.assertEqual(len(duplicates), 2)
        self.assertEqual(len({finding.usr for finding in duplicates}), 1)
        self.assertEqual(len({finding.offset for finding in duplicates}), 2)

    def test_macro_redeclaration_keeps_spelling_and_expansion_identity(self) -> None:
        header = self._write(
            "MacroDuplicate.h",
            "struct QJsonObject {};\n"
            "#define DECLARE_WIRE() QJsonObject macroDuplicated();\n"
            "DECLARE_WIRE()\n"
            "DECLARE_WIRE()\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        duplicates = [
            finding
            for finding in findings
            if finding.display_name.startswith("macroDuplicated(")
        ]
        self.assertEqual(len(duplicates), 2)
        self.assertEqual(len({finding.offset for finding in duplicates}), 2)
        self.assertTrue(all(finding.spelling_offset for finding in duplicates))


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
        self.assertIn("param[0]:QJsonObject &", findings[0].canonical_return_type)

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
        self.assertIn("param[0]:QJsonObject *", findings[0].canonical_return_type)

    def test_unallowlisted_const_input_is_structurally_rejected(self) -> None:
        header = self._write(
            "InParam.h",
            "struct QJsonObject {};\n"
            "namespace Arkham {\n"
            "void decode(const QJsonObject &in);\n"
            "}\n",
        )
        findings, violations = self._scan_header_fixture(header, frozenset({header.resolve()}))
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 1)
        self.assertIn("const QJsonObject &", findings[0].canonical_return_type)

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
        self.assertEqual(len(findings), 3)
        own_declaration = [f for f in findings if "exposed via inheritance" not in f.display_name]
        inherited = [f for f in findings if "exposed via inheritance" in f.display_name]
        self.assertEqual(len(own_declaration), 2)
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
        self.assertEqual(len(findings), 2)
        self.assertNotIn("exposed via inheritance", findings[0].display_name)

    def test_protected_inheritance_remains_exposable_to_subclasses(self) -> None:
        header = self._write(
            "ProtectedInherit.h",
            "struct QJsonObject {};\n"
            "class BaseProtected {\n"
            "public:\n"
            "  QJsonObject toJsonProtected() const;\n"
            "};\n"
            "class DerivedProtected : protected BaseProtected {};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        inherited = [
            finding
            for finding in findings
            if "exposed via inheritance" in finding.display_name
        ]
        self.assertEqual(len(inherited), 1)

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
        self.assertEqual(len(findings), 3)
        inherited = [f for f in findings if "exposed via inheritance" in f.display_name]
        self.assertEqual(len(inherited), 1)
        self.assertIn("toJson4", inherited[0].display_name)

    def test_nested_cv_pointer_alias_and_template_output_mutability(self) -> None:
        rejected = {
            "const pointer to mutable": "QJsonObject * const &",
            "double pointer": "QJsonObject **",
            "typedef pointer": "MutableAlias &",
            "alias-template pointer": "PointerAlias<QJsonObject> const &",
            "mutable wrapper": "Box<QJsonObject> &",
            "const wrapper containing mutable pointer": "const Box<QJsonObject *> &",
        }
        prelude = (
            "struct QJsonObject {};\n"
            "using MutableAlias = QJsonObject * const;\n"
            "template<class T> using PointerAlias = T *;\n"
            "template<class T> struct Box { T value; };\n"
        )
        for label, parameter in rejected.items():
            with self.subTest(label=label):
                header = self._write(
                    f"output-{label.replace(' ', '-')}.h",
                    prelude + f"void encode({parameter} out);\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("encode(")
                        for finding in findings
                    )
                )

        structurally_forbidden_even_when_const = {
            "pointer to const": "const QJsonObject * const &",
            "const direct wrapper": "const Box<QJsonObject> &",
            "const alias pointee": "ConstAlias &",
        }
        accepted_prelude = prelude + "using ConstAlias = const QJsonObject * const;\n"
        for label, parameter in structurally_forbidden_even_when_const.items():
            with self.subTest(label=label):
                header = self._write(
                    f"input-{label.replace(' ', '-')}.h",
                    accepted_prelude + f"void decode({parameter} in);\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("decode(")
                        for finding in findings
                    )
                )

    def test_smart_pointer_output_channels_and_const_pointee_controls(self) -> None:
        prelude = (
            "#include <functional>\n"
            "#include <memory>\n"
            "#include <optional>\n"
            "struct QJsonObject {};\n"
            "template<class T> struct Box { T value; };\n"
            "template<class T> struct QSharedPointer {};\n"
            "template<class T> struct QPointer {};\n"
            "using SharedAlias = std::shared_ptr<QJsonObject>;\n"
        )
        rejected = {
            "shared value": "std::shared_ptr<QJsonObject>",
            "shared const ref mutable pointee": "const std::shared_ptr<QJsonObject> &",
            "nested optional shared": "std::optional<std::shared_ptr<QJsonObject>>",
            "unique value": "std::unique_ptr<QJsonObject>",
            "qt shared": "QSharedPointer<QJsonObject>",
            "qt pointer const ref": "const QPointer<QJsonObject> &",
            "alias": "SharedAlias",
            "nested wrapper pointee": "std::shared_ptr<Box<QJsonObject>>",
        }
        for label, parameter in rejected.items():
            with self.subTest(label=label):
                header = self._write(
                    f"smart-{label.replace(' ', '-')}.h",
                    prelude + f"void mutate({parameter} value);\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("mutate(")
                        for finding in findings
                    )
                )

        immutable_or_value_forms_still_require_exact_allowlisting = {
            "shared const pointee": "std::shared_ptr<const QJsonObject>",
            "shared const pointee ref": "const std::shared_ptr<const QJsonObject> &",
            "direct optional value": "std::optional<QJsonObject>",
        }
        for label, parameter in immutable_or_value_forms_still_require_exact_allowlisting.items():
            with self.subTest(label=label):
                header = self._write(
                    f"smart-safe-{label.replace(' ', '-')}.h",
                    prelude + f"void consume({parameter} value);\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("consume(")
                        for finding in findings
                    )
                )

    def test_callback_parameters_reverse_json_direction(self) -> None:
        prelude = (
            "#include <functional>\n"
            "#include <memory>\n"
            "#include <optional>\n"
            "struct QJsonObject {};\n"
            "struct Sink { void receive(const QJsonObject &); };\n"
            "using CallbackAlias = void (*)(const QJsonObject &);\n"
            "using LambdaLike = std::function<void(const QJsonObject &)>;\n"
        )
        rejected = {
            "function pointer": "void (*callback)(const QJsonObject &)",
            "std function": "std::function<void(const QJsonObject &)>",
            "function alias": "CallbackAlias",
            "lambda-like alias": "LambdaLike",
            "member pointer": "void (Sink::*callback)(const QJsonObject &)",
            "optional callback": "std::optional<std::function<void(QJsonObject)>>",
            "immutable shared payload callback": "std::function<void(std::shared_ptr<const QJsonObject>)>",
        }
        for label, parameter in rejected.items():
            with self.subTest(label=label):
                header = self._write(
                    f"callback-{label.replace(' ', '-')}.h",
                    prelude + f"void publish({parameter});\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("publish(")
                        for finding in findings
                    )
                )

        header = self._write(
            "callback-safe-decoders.h",
            prelude
            + "void decodeWith(std::function<QJsonObject(int)> decoder);\n"
            + "void notifyWith(std::function<void(int)> callback);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertIn("decodeWith", names)
        self.assertNotIn("notifyWith", names)

    def test_qt_signal_json_payload_is_outbound_but_normal_input_is_not(self) -> None:
        header = self._write(
            "SignalDirection.h",
            "struct QJsonObject {};\n"
            "struct Publisher {\n"
            "  __attribute__((annotate(\"qt_signal\"))) "
            "void emitted(const QJsonObject &value);\n"
            "  void decode(const QJsonObject &value);\n"
            "};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertEqual(
            {finding.display_name.split("(", 1)[0] for finding in findings},
            {"emitted", "decode"},
        )

    def test_record_functor_callback_graph_is_reverse_directed(self) -> None:
        header = self._write(
            "FunctorCallbacks.h",
            "struct QJsonObject {};\n"
            "template<class T> struct Sink {\n"
            "  void operator()(const T &) const;\n"
            "};\n"
            "using JsonSink = Sink<QJsonObject>;\n"
            "struct InheritedSink : public Sink<QJsonObject> {};\n"
            "struct OverloadedSink {\n"
            "  void operator()(int) const;\n"
            "  void operator()(const QJsonObject &) const;\n"
            "};\n"
            "struct GenericSink {\n"
            "  template<class U> void operator()(const U &) const;\n"
            "};\n"
            "void publishTemplate(Sink<QJsonObject> sink);\n"
            "void publishAlias(JsonSink sink);\n"
            "void publishInherited(InheritedSink sink);\n"
            "void publishOverloaded(OverloadedSink sink);\n"
            "void publishGeneric(GenericSink sink);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        published = {
            finding.display_name.split("(", 1)[0]
            for finding in findings
            if finding.display_name.startswith("publish")
        }
        self.assertEqual(
            published,
            {
                "publishTemplate",
                "publishAlias",
                "publishInherited",
                "publishOverloaded",
            },
        )
        self.assertTrue(
            any(
                finding.display_name.startswith("operator()")
                for finding in findings
            )
        )

    def test_private_functor_and_value_wrapper_are_both_structural_surfaces(self) -> None:
        header = self._write(
            "FunctorControls.h",
            "struct QJsonObject {};\n"
            "struct PrivateSink {\n"
            "private:\n"
            "  void operator()(const QJsonObject &) const;\n"
            "};\n"
            "template<class T> struct NonCallable { T value; };\n"
            "void privateControl(PrivateSink sink);\n"
            "void valueControl(NonCallable<QJsonObject> value);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertIn("valueControl", names)
        self.assertIn("privateControl", names)
        self.assertTrue(
            any(finding.access == ceh._CX_CXXPrivate for finding in findings)
        )

    def test_arrays_and_smart_pointer_arrays_follow_element_types(self) -> None:
        prelude = (
            "#include <memory>\n"
            "struct QJsonObject {};\n"
            "using JsonArray = QJsonObject[2];\n"
        )
        rejected = {
            "constant array ref": "QJsonObject (&values)[2]",
            "incomplete array ref": "QJsonObject (&values)[]",
            "pointer to array": "QJsonObject (*values)[2]",
            "array alias ref": "JsonArray &values",
            "shared incomplete array": "std::shared_ptr<QJsonObject[]> values",
            "shared constant array": "std::shared_ptr<QJsonObject[2]> values",
        }
        for label, parameter in rejected.items():
            with self.subTest(label=label):
                header = self._write(
                    f"array-{label.replace(' ', '-')}.h",
                    prelude + f"void emitArray({parameter});\n",
                )
                findings, violations = self._scan_header_fixture(
                    header, frozenset({header.resolve()})
                )
                self.assertEqual(violations, [])
                self.assertTrue(
                    any(
                        finding.display_name.startswith("emitArray(")
                        for finding in findings
                    )
                )

        header = self._write(
            "array-const-controls.h",
            prelude
            + "void consumeConst(const QJsonObject (&values)[2]);\n"
            + "void consumeShared(std::shared_ptr<const QJsonObject[]> values);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertEqual(
            {
                finding.display_name.split("(", 1)[0]
                for finding in findings
                if finding.display_name.startswith("consume")
            },
            {"consumeConst", "consumeShared"},
        )

        dependent = self._write(
            "array-dependent-and-variable.h",
            "struct QJsonObject {};\n"
            "template<int N> void emitDependent(QJsonObject (&values)[N]);\n"
            "void emitVariable(int count, QJsonObject (&values)[count]);\n",
        )
        findings, violations = self._scan_header_fixture(
            dependent, frozenset({dependent.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertEqual(
            {
                finding.display_name.split("(", 1)[0]
                for finding in findings
            },
            {"emitDependent", "emitVariable"},
        )

    def test_unresolved_wrapper_without_forbidden_argument_is_not_invented(self) -> None:
        header = self._write(
            "UnresolvedSmartPointer.h",
            "#include <memory>\n"
            "template<class T> void expose(std::shared_ptr<T> value);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertEqual(findings, [])

    def test_custom_pointer_wrapper_is_rejected_by_outer_type_reference(self) -> None:
        header = self._write(
            "CustomHandleMatrix.h",
            "struct QJsonObject {};\n"
            "template<class T> struct Handle {\n"
            "  T &operator*() const;\n"
            "  T *operator->() const;\n"
            "  T *get() const;\n"
            "  operator T *() const;\n"
            "};\n"
            "template<class T> struct Outer { T value; };\n"
            "using JsonHandle = Handle<QJsonObject>;\n"
            "void byValue(Handle<QJsonObject> value);\n"
            "void byConstRef(const Handle<QJsonObject> &value);\n"
            "void nested(Outer<Handle<QJsonObject>> value);\n"
            "void alias(JsonHandle value);\n"
            "void safe(Handle<int> value);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertTrue({"JsonHandle", "byValue", "byConstRef", "nested", "alias"}.issubset(names))
        self.assertNotIn("safe", names)

    def test_multi_parameter_template_matrix_checks_every_argument(self) -> None:
        header = self._write(
            "MultiParameterMatrix.h",
            "struct QJsonObject {};\n"
            "template<class A, class B> struct Pair { A first; B second; };\n"
            "template<class A, class B> struct Sink {\n"
            "  void operator()(const A &, const B &) const;\n"
            "};\n"
            "void second(Pair<int, QJsonObject> value);\n"
            "void first(Pair<QJsonObject, int> value);\n"
            "void sinkSecond(Sink<int, QJsonObject> value);\n"
            "void sinkFirst(Sink<QJsonObject, int> value);\n"
            "void safe(Pair<int, long> value);\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertTrue(
            {"second", "first", "sinkSecond", "sinkFirst"}.issubset(names)
        )
        self.assertNotIn("safe", names)

    def test_dependent_encoder_alias_fails_at_forbidden_instantiation(self) -> None:
        header = self._write(
            "DependentSignatureMatrix.h",
            "struct QJsonObject {};\n"
            "template<class T> struct Base {\n"
            "  T toJson() const;\n"
            "  void write(T &out) const;\n"
            "};\n"
            "template<class T> struct Adapter : public Base<T> {};\n"
            "template<class T> struct Layer : public Adapter<T> {};\n"
            "using Lossy = Adapter<QJsonObject>;\n"
            "using DeepLossy = Layer<QJsonObject>;\n"
            "using Safe = Layer<int>;\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name for finding in findings}
        self.assertIn("Lossy", names)
        self.assertIn("DeepLossy", names)
        self.assertNotIn("Safe", names)

    def test_opaque_record_fields_unions_and_nested_aliases_are_reachable(self) -> None:
        header = self._write(
            "OpaqueRecords.h",
            "struct QJsonObject {};\n"
            "struct WireBox {\n"
            "private:\n"
            "  QJsonObject value;\n"
            "public:\n"
            "  union Payload { QJsonObject object; int number; };\n"
            "  using Nested = QJsonObject;\n"
            "};\n"
            "WireBox fetchOpaque();\n"
            "struct SafeCycle { SafeCycle *next; int value; };\n"
            "SafeCycle fetchSafe();\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertIn("fetchOpaque", names)
        self.assertIn("value", names)
        self.assertIn("Nested", names)
        self.assertNotIn("fetchSafe", names)

    def test_external_system_opaque_record_is_traversed_from_project_surface(self) -> None:
        system_header = self._write(
            "system/WireDependency.h",
            "struct QJsonObject {};\n"
            "struct ExternalLeaf { private: QJsonObject value; };\n"
            "struct ExternalWireBox {\n"
            "  ExternalWireBox(ExternalLeaf);\n"
            "  ExternalLeaf result();\n"
            "  void accept(ExternalLeaf);\n"
            "};\n"
            "struct ExternalSafe { int result(); };\n",
        )
        header = self._write(
            "domain/SystemOpaque.h",
            "#include <WireDependency.h>\n"
            "ExternalWireBox fetchSystemOpaque();\n"
            "ExternalSafe fetchSystemSafe();\n",
        )
        findings, violations = self._scan_header_fixture(
            header,
            frozenset({header.resolve()}),
            external_roots=frozenset({system_header.parent.resolve()}),
            arguments=(
                "-std=c++23",
                "-isystem",
                str(system_header.parent),
            ),
        )
        self.assertEqual(violations, [])
        self.assertTrue(
            any(
                finding.display_name.startswith("fetchSystemOpaque(")
                for finding in findings
            )
        )
        self.assertFalse(
            any(
                finding.display_name.startswith("fetchSystemSafe(")
                for finding in findings
            )
        )

    def test_opaque_record_traversal_continues_through_nested_method_signatures(
        self,
    ) -> None:
        header = self._write(
            "OpaqueMethodChain.h",
            "struct QJsonObject {};\n"
            "struct Leaf { Leaf *cycle(); QJsonObject value(); };\n"
            "struct ThroughResult { Leaf result(); };\n"
            "struct ThroughParameter { void accept(Leaf); };\n"
            "struct ThroughConstructor { ThroughConstructor(Leaf); };\n"
            "ThroughResult resultChain();\n"
            "ThroughParameter parameterChain();\n"
            "ThroughConstructor constructorChain();\n"
            "struct CycleB;\n"
            "struct CycleA { CycleB *back; QJsonObject payload; };\n"
            "struct CycleB { CycleA *forward; };\n"
            "CycleB cycleChain();\n"
            "struct SafeCycle { SafeCycle *next(); int value(); };\n"
            "SafeCycle safeChain();\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertTrue(
            {
                "resultChain",
                "parameterChain",
                "constructorChain",
                "cycleChain",
            }.issubset(names)
        )
        self.assertNotIn("safeChain", names)

    def test_type_graph_complexity_limit_records_a_fail_closed_error(self) -> None:
        nested = "int"
        for _ in range(ceh._MAX_TYPE_DEPTH + 2):
            nested = f"Box<{nested}>"
        header = self._write(
            "TooDeep.h",
            "template<class T> struct Box { T value; };\n"
            f"using TooDeep = {nested};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertTrue(findings)
        self.assertTrue(self.clang.type_graph_errors)
        self.assertTrue(
            any(
                "semantic type graph exceeded" in error
                for error in self.clang.type_graph_errors
            )
        )

    def test_full_signature_pins_non_wire_types_and_noexcept(self) -> None:
        signatures = []
        for label, declaration in (
            ("int", "int decode(const QJsonObject &, int);"),
            ("long", "long decode(const QJsonObject &, long);"),
            ("noexcept", "int decode(const QJsonObject &, int) noexcept;"),
        ):
            header = self._write(
                f"FullSignature-{label}.h",
                "struct QJsonObject {};\n" + declaration + "\n",
            )
            findings, violations = self._scan_header_fixture(
                header, frozenset({header.resolve()})
            )
            self.assertEqual(violations, [])
            finding = next(
                finding
                for finding in findings
                if finding.display_name.startswith("decode(")
            )
            signatures.append(finding.canonical_return_type)
        self.assertEqual(len(set(signatures)), 3)

    def test_dependent_public_and_protected_inheritance_fail_closed(self) -> None:
        header = self._write(
            "DependentInheritance.h",
            "struct QJsonObject {};\n"
            "struct AuthenticateRequest { QJsonObject toJson() const; };\n"
            "template<class T> struct PublicWrapper : public T {};\n"
            "using WrappedAuth = PublicWrapper<AuthenticateRequest>;\n"
            "template<class T> struct ProtectedWrapper : protected T {};\n"
            "template<class T> struct PrivateWrapper : private T {};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        dependent = [
            finding
            for finding in findings
            if "dependent base" in finding.display_name
        ]
        self.assertEqual(len(dependent), 2)
        self.assertFalse(
            any("PrivateWrapper" in finding.usr for finding in dependent)
        )

    def test_public_dependent_using_reexport_from_private_base_fails_closed(self) -> None:
        header = self._write(
            "DependentUsing.h",
            "struct QJsonObject {};\n"
            "struct AuthenticateRequest { QJsonObject toJson() const; };\n"
            "template<class T> struct Forwarder : private T {\n"
            "public:\n"
            "  using T::toJson;\n"
            "};\n"
            "using ForwardAuth = Forwarder<AuthenticateRequest>;\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        dependent_using = [
            finding
            for finding in findings
            if "dependent using-declaration" in finding.display_name
        ]
        self.assertEqual(len(dependent_using), 1)

    def test_private_nested_dependent_wrapper_is_not_exposed(self) -> None:
        header = self._write(
            "PrivateNestedWrapper.h",
            "struct QJsonObject {};\n"
            "class Holder {\n"
            "private:\n"
            "  template<class T> struct Hidden : public T {};\n"
            "};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].access, ceh._CX_CXXPrivate)

    def test_public_and_protected_aliases_reexport_encoder_type(self) -> None:
        header = self._write(
            "AliasExposure.h",
            "struct QJsonObject {};\n"
            "struct AuthenticateRequest { QJsonObject toJson() const; };\n"
            "using PublicAlias = AuthenticateRequest;\n"
            "class AliasHolder {\n"
            "protected:\n"
            "  using ProtectedAlias = AuthenticateRequest;\n"
            "private:\n"
            "  using PrivateAlias = AuthenticateRequest;\n"
            "};\n",
        )
        findings, violations = self._scan_header_fixture(
            header, frozenset({header.resolve()})
        )
        self.assertEqual(violations, [])
        alias_findings = [
            finding
            for finding in findings
            if "exposed via inheritance" in finding.display_name
        ]
        self.assertEqual(len(alias_findings), 3)

    def test_external_dependent_wrapper_alias_is_audited_at_project_alias(self) -> None:
        external = self._write(
            "external/Wrapper.h",
            "struct QJsonObject {};\n"
            "struct AuthenticateRequest { QJsonObject toJson() const; };\n"
            "template<class T> struct Wrapper : public T {};\n",
        )
        header = self._write(
            "domain/Alias.h",
            '#include "../external/Wrapper.h"\n'
            "using WrappedAuth = Wrapper<AuthenticateRequest>;\n",
        )
        findings, violations = self._scan_header_fixture(
            header,
            frozenset({header.resolve()}),
            external_roots=frozenset({external.parent.resolve()}),
        )
        self.assertEqual(violations, [])
        self.assertEqual(len(findings), 2)
        self.assertTrue(
            all(finding.file == "domain/Alias.h" for finding in findings)
        )
        self.assertTrue(
            any("toJson" in finding.display_name for finding in findings)
        )

    def test_concrete_and_composed_template_bases_are_not_skipped(self) -> None:
        external = self._write(
            "external/ConcreteBases.h",
            "struct QJsonObject {};\n"
            "struct LossyBase { QJsonObject toJson() const; };\n"
            "struct HarmlessBase { int value() const; };\n"
            "template<class T> struct Wrapper : public LossyBase {};\n"
            "template<class T> struct Adapter : public Wrapper<T> {};\n"
            "template<class T> struct Multiple : public HarmlessBase, "
            "public virtual LossyBase {};\n"
            "template<class T> struct Protected : protected LossyBase {};\n"
            "template<class T> struct Private : private LossyBase {};\n",
        )
        header = self._write(
            "domain/ConcreteAliases.h",
            '#include "../external/ConcreteBases.h"\n'
            "using Direct = Wrapper<int>;\n"
            "using Composed = Adapter<int>;\n"
            "using NestedAlias = Composed;\n"
            "using Multi = Multiple<int>;\n"
            "using ProtectedAlias = Protected<int>;\n"
            "using PrivateAlias = Private<int>;\n",
        )
        findings, violations = self._scan_header_fixture(
            header,
            frozenset({header.resolve()}),
            external_roots=frozenset({external.parent.resolve()}),
        )
        self.assertEqual(violations, [])
        alias_findings = [
            finding
            for finding in findings
            if finding.file == "domain/ConcreteAliases.h"
        ]
        self.assertEqual(len(alias_findings), 5)
        self.assertTrue(
            all("toJson" in finding.display_name for finding in alias_findings)
        )


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

    def test_project_source_marked_system_remains_owned_and_rejected(self) -> None:
        forbidden = self._write("src/forbidden/Lossy.h", "struct QJsonObject {};\n")
        allowed = self._write("src/domain/Allowed.h", '#include "Lossy.h"\n')
        owned = ceh._owned_path_policy(self.repo_root, frozenset())
        tu, wrapper_filename = ceh._parse_header_as_own_tu(
            self.clang,
            self.idx,
            allowed,
            ["-std=c++23", "-isystem", str(forbidden.parent)],
            self.sysroot_args,
        )
        try:
            violations = ceh._audit_inclusion_graph(
                self.clang,
                tu,
                allowed,
                wrapper_filename,
                frozenset({allowed.resolve()}),
                frozenset(),
                owned,
            )
        finally:
            self.clang.lib.clang_disposeTranslationUnit(tu)
        self.assertEqual(len(violations), 1)
        self.assertIn("owned file", violations[0])

    def test_hardlink_under_external_root_cannot_declassify_owned_source(self) -> None:
        owned_header = self._write("src/forbidden/Owned.h", "struct Owned {};\n")
        external_root = self.scratch / "external"
        external_root.mkdir()
        alias = external_root / "Alias.h"
        os.link(owned_header, alias)
        allowed = self._write("src/domain/Allowed.h", f'#include "{alias}"\n')
        owned = ceh._owned_path_policy(self.repo_root, frozenset())
        tu, wrapper_filename = ceh._parse_header_as_own_tu(
            self.clang, self.idx, allowed, ["-std=c++23"], self.sysroot_args
        )
        try:
            violations = ceh._audit_inclusion_graph(
                self.clang,
                tu,
                allowed,
                wrapper_filename,
                frozenset({allowed.resolve()}),
                frozenset({external_root.resolve()}),
                owned,
            )
        finally:
            self.clang.lib.clang_disposeTranslationUnit(tu)
        self.assertEqual(len(violations), 1)
        self.assertIn("owned file", violations[0])

    def test_wrappers_own_synthetic_main_file_is_never_misclassified_as_a_violation(self) -> None:
        # A header with no #includes of its own at all: the ONLY file
        # clang_getInclusions() reports is the wrapper's own synthetic
        # "main file" entry, which must be recognized and skipped by
        # exact resolved identity -- never appear as a violation itself.
        allowed = self._write("domain/Standalone.h", "struct QJsonObject {};\n")
        violations = self._audit_inclusion_graph_fixture(allowed, frozenset({allowed.resolve()}))
        self.assertEqual(violations, [])


class ProductionCMakeSeamTests(unittest.TestCase):
    def setUp(self) -> None:
        repo = Path(__file__).resolve().parent.parent
        self.module = (repo / "cmake" / "PathManifest.cmake").resolve()
        self.root = repo / "build" / "encoder-hygiene-production-seam" / self._testMethodName
        if self.root.exists():
            shutil.rmtree(self.root)
        self.root.mkdir(parents=True)
        self.build = self.root / "build"

    def tearDown(self) -> None:
        shutil.rmtree(self.root, ignore_errors=True)

    def _write(self, relative: str, content: str) -> Path:
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _configure_and_build(
        self,
        targets: list[str],
        *,
        qt: bool = False,
        multi_config: bool = False,
    ) -> None:
        clangxx = os.environ.get("ARKHAM_CLANGXX", "clang++")
        command = [
            "cmake",
            "-S",
            str(self.root),
            "-B",
            str(self.build),
            "-G",
            "Ninja Multi-Config" if multi_config else "Ninja",
            f"-DCMAKE_CXX_COMPILER={clangxx}",
            "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        ]
        if not multi_config:
            command.append("-DCMAKE_BUILD_TYPE=Debug")
        ninja = shutil.which("ninja")
        if ninja is None and shutil.which("mise"):
            ninja = subprocess.check_output(
                ["mise", "which", "ninja"], text=True
            ).strip()
        if ninja:
            command.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
        if qt:
            qt_prefix = os.environ.get("QT_PREFIX") or os.environ.get("QTDIR")
            if not qt_prefix and shutil.which("brew"):
                qt_prefix = subprocess.check_output(
                    ["brew", "--prefix"], text=True
                ).strip()
            if qt_prefix:
                command.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")
        try:
            subprocess.run(
                command, check=True, cwd=self.root, capture_output=True, text=True
            )
            subprocess.run(
                [
                    "cmake",
                    "--build",
                    str(self.build),
                    *(["--config", "Debug"] if multi_config else []),
                    "--target",
                    *targets,
                ],
                check=True,
                cwd=self.root,
                capture_output=True,
                text=True,
            )
        except subprocess.CalledProcessError as exc:
            self.fail(
                f"CMake production-seam command failed: {exc.cmd}\n"
                f"stdout:\n{exc.stdout}\nstderr:\n{exc.stderr}"
            )

    def _manifest_prelude(self) -> str:
        return (
            f'include("{self.module}")\n'
            'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/external_roots.txt" "")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/autogen_targets.txt" "")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/target_policy.txt" "")\n'
            'if(CMAKE_CONFIGURATION_TYPES)\n'
            '  string(REPLACE ";" "\\n" _configs "${CMAKE_CONFIGURATION_TYPES}")\n'
            'else()\n'
            '  set(_configs "${CMAKE_BUILD_TYPE}")\n'
            'endif()\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/audited_configs.txt" "${_configs}\\n")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/domain_fragments.txt" "")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/foundation_fragments.txt" "")\n'
        )

    def _register_manifests(
        self,
        domain: str,
        foundation: str,
        *,
        automoc: bool = False,
        universe: bool = True,
    ) -> str:
        text = (
            f'arkham_append_encoder_hygiene_target(TARGET {domain} CLASSIFICATION SCAN POLICY domain OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/target_policy.txt")\n'
            f'arkham_append_encoder_hygiene_target(TARGET {foundation} CLASSIFICATION SCAN POLICY foundation OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/target_policy.txt")\n'
            f'cmake_language(DEFER CALL arkham_write_target_header_set_manifest TARGET {domain} OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/domain_headers.txt")\n'
            f'cmake_language(DEFER CALL arkham_write_target_source_manifest TARGET {domain} OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/domain_sources.txt")\n'
            f'cmake_language(DEFER CALL arkham_write_target_header_set_manifest TARGET {foundation} OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/foundation_headers.txt")\n'
            f'cmake_language(DEFER CALL arkham_write_target_source_manifest TARGET {foundation} OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/foundation_sources.txt")\n'
        )
        if automoc:
            text += (
                f'cmake_language(DEFER CALL arkham_append_target_autogen_manifest TARGET {domain} POLICY domain OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/autogen_targets.txt")\n'
                f'cmake_language(DEFER CALL arkham_append_target_autogen_manifest TARGET {foundation} POLICY foundation OUTPUT_FILE "${{CMAKE_BINARY_DIR}}/generated/autogen_targets.txt")\n'
            )
        if universe:
            text += (
                'cmake_language(DEFER CALL arkham_write_encoder_hygiene_target_universe '
                'OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_universe.txt" '
                'HEADER_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_header_index.txt" '
                'HEADER_DIR "${CMAKE_BINARY_DIR}/generated/target_headers" '
                'SOURCE_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_source_index.txt" '
                'SOURCE_DIR "${CMAKE_BINARY_DIR}/generated/target_sources")\n'
            )
        return text

    def test_scans_every_target_object_context_for_shared_source(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write(
            "src/domain/Conditional.h",
            "#pragma once\n"
            "#ifdef CONSUMER_ENCODER\n"
            "struct QJsonObject {};\n"
            "inline QJsonObject consumerContextEncoder() { return {}; }\n"
            "#endif\n",
        )
        self._write("src/Foundation.h", "#pragma once\n")
        self._write(
            "src/domain/Shared.cpp",
            "#ifdef CONSUMER_ENCODER\n"
            '#include "Conditional.h"\n'
            "#endif\n",
        )
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(TargetContexts CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Shared.cpp)\n"
            'target_include_directories(domain PRIVATE "${CMAKE_SOURCE_DIR}/src/domain")\n'
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h" "${CMAKE_SOURCE_DIR}/src/domain/Conditional.h")\n'
            + "add_library(foundation STATIC src/domain/Shared.cpp)\n"
            'target_include_directories(foundation PRIVATE "${CMAKE_SOURCE_DIR}/src/domain")\n'
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_library(consumer STATIC src/domain/Shared.cpp)\n"
            'target_include_directories(consumer PRIVATE "${CMAKE_SOURCE_DIR}/src/domain")\n'
            "target_compile_definitions(consumer PRIVATE CONSUMER_ENCODER)\n"
            'arkham_append_encoder_hygiene_target(TARGET consumer CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation", "consumer"])

        commands = json.loads(
            (self.build / "compile_commands.json").read_text(encoding="utf-8")
        )
        contexts = ceh._compile_contexts_for_source(
            commands, self.root / "src/domain/Shared.cpp"
        )
        self.assertEqual(
            {context.target for context in contexts},
            {"domain", "foundation", "consumer"},
        )
        findings = ceh.run_check(self.root, self.build, skip_configure=True)
        self.assertEqual(
            [
                finding.display_name
                for finding in _declaration_findings(findings)
            ],
            ["consumerContextEncoder()"],
        )

    def test_system_include_marking_cannot_declassify_project_source(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/hidden/Lossy.h", "struct Lossy {};\n")
        self._write("src/domain/Domain.cpp", '#include "Lossy.h"\n')
        self._write("src/Foundation.cpp", "int foundationSource = 0;\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(SystemOwned CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_include_directories(domain SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src/hidden")\n'
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation"])
        with self.assertRaisesRegex(ceh.EncoderHygieneError, "owned file"):
            ceh.run_check(self.root, self.build, skip_configure=True)

    def test_application_main_macro_and_generated_tus_are_scanned(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write(
            "src/main.cpp",
            "struct QJsonObject {};\n"
            "QJsonObject appSourceEncoder() { return {}; }\n"
            "void submitAppBody() { QJsonObject localAppBody; }\n"
            "#ifdef APP_CONTEXT_ENCODER\n"
            "QJsonObject macroAppEncoder() { return {}; }\n"
            "#endif\n"
            "int main() { return 0; }\n",
        )
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(ApplicationInventory CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + 'file(WRITE "${CMAKE_BINARY_DIR}/GeneratedApp.cpp" "struct QJsonObject {}; QJsonObject generatedAppEncoder() { return {}; } void generatedSubmit() { QJsonObject generatedBody; }\\n")\n'
            + 'add_executable(app src/main.cpp "${CMAKE_BINARY_DIR}/GeneratedApp.cpp")\n'
            + "target_compile_definitions(app PRIVATE APP_CONTEXT_ENCODER)\n"
            + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation", "app"])
        findings = ceh.run_check(self.root, self.build, skip_configure=True)
        self.assertTrue(
            {
                "appSourceEncoder()",
                "localAppBody",
                "macroAppEncoder()",
                "generatedAppEncoder()",
                "generatedBody",
            }.issubset(
                {finding.display_name for finding in findings}
            )
        )

    def test_closed_target_universe_scans_unused_and_header_only_sets(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write("src/main.cpp", "int main() { return 0; }\n")
        self._write(
            "src/AppWire.h",
            "struct QJsonObject {}; QJsonObject unusedAppEncoder();\n",
        )
        self._write(
            "src/InterfaceWire.h",
            "struct QJsonObject {};\n"
            "QJsonObject interfaceOnlyEncoder();\n"
            "#ifdef INTERFACE_OWN_ENCODER\n"
            "QJsonObject interfaceDefinitionEncoder();\n"
            "#endif\n"
            "#ifdef CONSUMER_DEBUG_ENCODER\n"
            "QJsonObject consumerDebugEncoder();\n"
            "#endif\n"
            "#ifdef CONSUMER_RELEASE_ENCODER\n"
            "QJsonObject consumerReleaseEncoder();\n"
            "#endif\n"
            "#ifdef CONSUMER_RELWITH_ENCODER\n"
            "QJsonObject consumerRelWithEncoder();\n"
            "#endif\n"
            "#ifdef CONSUMER_TWO_ENCODER\n"
            "QJsonObject consumerTwoEncoder();\n"
            "#endif\n"
            "#ifdef DIRECT_ENCODER\n"
            "QJsonObject directEncoder();\n"
            "#endif\n"
            "#ifdef EXCLUDED_ENCODER\n"
            "QJsonObject excludedEncoder();\n"
            "#endif\n",
        )
        self._write(
            "src/InterfaceSource.cpp",
            "struct QJsonObject {}; QJsonObject interfaceSourceEncoder() { return {}; }\n",
        )
        self._write("src/ConsumerOne.cpp", "int main() { return 0; }\n")
        self._write("src/ConsumerTwo.cpp", "int main() { return 0; }\n")
        self._write("src/ConsumerDirect.cpp", "int main() { return 0; }\n")
        self._write("src/ConsumerExcluded.cpp", "int main() { return 0; }\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(ClosedHeaderUniverse CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_executable(app src/main.cpp)\n"
            + 'target_sources(app PUBLIC FILE_SET app_first TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/AppWire.h")\n'
            + "add_library(wire_interface INTERFACE)\n"
            + 'target_sources(wire_interface INTERFACE FILE_SET wire_headers TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/InterfaceWire.h")\n'
            + 'target_sources(wire_interface INTERFACE "${CMAKE_SOURCE_DIR}/src/InterfaceSource.cpp")\n'
            + "target_compile_definitions(wire_interface INTERFACE INTERFACE_OWN_ENCODER)\n"
            + "add_library(WireAlias ALIAS wire_interface)\n"
            + "add_library(wire_bridge INTERFACE)\n"
            + "target_link_libraries(wire_bridge INTERFACE WireAlias)\n"
            + "add_library(ImportedMid INTERFACE IMPORTED)\n"
            + 'set_property(TARGET ImportedMid PROPERTY INTERFACE_LINK_LIBRARIES "$<$<BOOL:$<TARGET_PROPERTY:ENABLE_WIRE>>:$<IF:$<CONFIG:Debug>,$<LOWER_CASE:WIRE_INTERFACE>,$<IF:$<CONFIG:Release>,$<JOIN:wire_;interface,>,$<TARGET_NAME_IF_EXISTS:WireAlias>>>>")\n'
            + "add_library(ImportedMidAlias ALIAS ImportedMid)\n"
            + "add_library(ImportedBridge INTERFACE IMPORTED)\n"
            + 'set_property(TARGET ImportedBridge PROPERTY INTERFACE_LINK_LIBRARIES "$<IF:$<BOOL:1>,$<TARGET_NAME_IF_EXISTS:ImportedMidAlias>,missing>")\n'
            + "add_library(exempt_bridge INTERFACE)\n"
            + "target_link_libraries(exempt_bridge INTERFACE ImportedBridge)\n"
            + "add_executable(consumer_one src/ConsumerOne.cpp)\n"
            + "target_link_libraries(consumer_one PRIVATE exempt_bridge)\n"
            + "set_property(TARGET consumer_one PROPERTY ENABLE_WIRE TRUE)\n"
            + 'target_compile_definitions(consumer_one PRIVATE "$<$<CONFIG:Debug>:CONSUMER_DEBUG_ENCODER>" "$<$<CONFIG:Release>:CONSUMER_RELEASE_ENCODER>" "$<$<CONFIG:RelWithDebInfo>:CONSUMER_RELWITH_ENCODER>")\n'
            + "add_executable(consumer_two src/ConsumerTwo.cpp)\n"
            + "target_link_libraries(consumer_two PRIVATE wire_interface)\n"
            + "target_compile_definitions(consumer_two PRIVATE CONSUMER_TWO_ENCODER)\n"
            + "add_library(direct_provider INTERFACE)\n"
            + 'set_property(TARGET direct_provider PROPERTY INTERFACE_LINK_LIBRARIES_DIRECT "$<$<BOOL:$<TARGET_PROPERTY:ENABLE_DIRECT>>:wire_interface>")\n'
            + "add_library(direct_bridge INTERFACE)\n"
            + "target_link_libraries(direct_bridge INTERFACE direct_provider)\n"
            + "add_executable(consumer_direct src/ConsumerDirect.cpp)\n"
            + "target_link_libraries(consumer_direct PRIVATE direct_bridge)\n"
            + "set_property(TARGET consumer_direct PROPERTY ENABLE_DIRECT TRUE)\n"
            + "target_compile_definitions(consumer_direct PRIVATE DIRECT_ENCODER)\n"
            + "add_library(exclude_provider INTERFACE)\n"
            + "target_link_libraries(exclude_provider INTERFACE direct_provider)\n"
            + 'set_property(TARGET exclude_provider PROPERTY INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE "$<$<BOOL:$<TARGET_PROPERTY:DISABLE_DIRECT>>:wire_interface>")\n'
            + "add_executable(consumer_excluded src/ConsumerExcluded.cpp)\n"
            + "target_link_libraries(consumer_excluded PRIVATE exclude_provider)\n"
            + "set_property(TARGET consumer_excluded PROPERTY ENABLE_DIRECT TRUE)\n"
            + "set_property(TARGET consumer_excluded PROPERTY DISABLE_DIRECT TRUE)\n"
            + "target_compile_definitions(consumer_excluded PRIVATE EXCLUDED_ENCODER)\n"
            + 'file(WRITE "${CMAKE_BINARY_DIR}/GeneratedWire.h" "struct QJsonObject {}; QJsonObject generatedHeaderEncoder();\\n")\n'
            + 'target_sources(app PUBLIC FILE_SET app_late_generated TYPE HEADERS BASE_DIRS "${CMAKE_BINARY_DIR}" FILES "${CMAKE_BINARY_DIR}/GeneratedWire.h")\n'
            + "add_library(Imported::Legitimate INTERFACE IMPORTED)\n"
            + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET consumer_one CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET consumer_two CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET consumer_direct CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET consumer_excluded CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET wire_interface CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET wire_bridge CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET exempt_bridge CLASSIFICATION EXTERNAL EXEMPT_REASON "test imported/exempt graph wrapper" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET direct_provider CLASSIFICATION EXTERNAL EXEMPT_REASON "test direct injection provider" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET direct_bridge CLASSIFICATION EXTERNAL EXEMPT_REASON "test direct injection bridge" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET exclude_provider CLASSIFICATION EXTERNAL EXEMPT_REASON "test direct exclusion provider" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(
            [
                "domain",
                "foundation",
                "app",
                "consumer_one",
                "consumer_two",
                "consumer_direct",
                "consumer_excluded",
            ],
            multi_config=True,
        )
        findings = ceh.run_check(self.root, self.build, skip_configure=True)
        self.assertEqual(
            {
                finding.display_name.split("(", 1)[0]
                for finding in findings
                if finding.display_name
            },
            {
                "unusedAppEncoder",
                "interfaceOnlyEncoder",
                "interfaceDefinitionEncoder",
                "consumerDebugEncoder",
                "consumerReleaseEncoder",
                "consumerRelWithEncoder",
                "consumerTwoEncoder",
                "directEncoder",
                "interfaceSourceEncoder",
                "generatedHeaderEncoder",
            },
        )
        index = (
            self.build / "generated/target_header_index.txt"
        ).read_text(encoding="utf-8")
        self.assertIn("app\tapplication\tapp\t", index)
        wire_line = next(
            line
            for line in index.splitlines()
            if line.startswith("wire_interface\t")
        )
        self.assertIn("__arkham_encoder_context_", wire_line)
        commands = json.loads(
            (self.build / "compile_commands.json").read_text(encoding="utf-8")
        )
        _, evaluated_contexts = ceh._load_target_header_manifests(
            self.root,
            self.build,
            ceh._load_target_policies(self.build),
            ceh._all_compile_contexts(commands),
            ("Debug", "Release", "RelWithDebInfo"),
        )
        for configuration in ("Debug", "Release", "RelWithDebInfo"):
            self.assertIn(
                ("consumer_one", configuration),
                evaluated_contexts["wire_interface"],
            )
            self.assertIn(
                ("consumer_two", configuration),
                evaluated_contexts["wire_interface"],
            )
            self.assertIn(
                ("consumer_direct", configuration),
                evaluated_contexts["wire_interface"],
            )
            self.assertNotIn(
                ("consumer_excluded", configuration),
                evaluated_contexts["wire_interface"],
            )
        stale = (
            self.build
            / "generated/target_link_graph/Debug/stale.txt"
        )
        stale.write_text(
            "target\tstale\nlinks\t\ninterface\t\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(
            ceh.EncoderHygieneError,
            "differ from the exact index",
        ):
            ceh._load_target_header_manifests(
                self.root,
                self.build,
                ceh._load_target_policies(self.build),
                ceh._all_compile_contexts(commands),
                ("Debug", "Release", "RelWithDebInfo"),
            )

    def test_imported_generator_edge_with_unknown_result_fails_closed(
        self,
    ) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write("src/main.cpp", "int main() { return 0; }\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(UnresolvedImportedEdge CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_library(ImportedBridge INTERFACE IMPORTED)\n"
            + 'set_property(TARGET ImportedBridge PROPERTY INTERFACE_LINK_LIBRARIES "$<LOWER_CASE:NOT_ENUMERATED>")\n'
            + "add_executable(app src/main.cpp)\n"
            + "target_link_libraries(app PRIVATE ImportedBridge)\n"
            + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation"])
        with self.assertRaisesRegex(
            ceh.EncoderHygieneError,
            "unclassified evaluated link edge",
        ):
            ceh.run_check(self.root, self.build, skip_configure=True)

    def test_unknown_compile_target_fails_reverse_inventory(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write("tests/UnownedTest.cpp", "int rogueValue = 0;\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(UnknownTarget CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_library(apparently_test STATIC tests/UnownedTest.cpp)\n"
            + self._register_manifests("domain", "foundation", universe=False)
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation", "apparently_test"])
        with self.assertRaisesRegex(
            ceh.EncoderHygieneError,
            "unowned compile-command target: apparently_test",
        ):
            ceh.run_check(self.root, self.build, skip_configure=True)
        with (self.root / "CMakeLists.txt").open("a", encoding="utf-8") as stream:
            stream.write(
                'cmake_language(DEFER CALL arkham_write_encoder_hygiene_target_universe '
                'OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_universe.txt" '
                'HEADER_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_header_index.txt" '
                'HEADER_DIR "${CMAKE_BINARY_DIR}/generated/target_headers" '
                'SOURCE_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_source_index.txt" '
                'SOURCE_DIR "${CMAKE_BINARY_DIR}/generated/target_sources")\n'
            )
        configure = subprocess.run(
            ["cmake", "-S", str(self.root), "-B", str(self.build)],
            cwd=self.root,
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(configure.returncode, 0)
        self.assertIn(
            "has no explicit SCAN/EXEMPT classification",
            configure.stdout + configure.stderr,
        )

    def test_scan_target_generator_expressions_fail_cmake_finalization(self) -> None:
        expressions = {
            "current-config-header-source": (
                'target_sources(app PRIVATE "$<$<CONFIG:Debug>:${CMAKE_SOURCE_DIR}/Lossy.h>")\n'
            ),
            "release-only-source": (
                'target_sources(app PRIVATE "$<$<CONFIG:Release>:${CMAKE_SOURCE_DIR}/Lossy.cpp>")\n'
            ),
            "nested-expression": (
                'target_sources(app PRIVATE "$<$<AND:$<CONFIG:Debug>,$<BOOL:1>>:${CMAKE_SOURCE_DIR}/Lossy.h>")\n'
            ),
            "conditional-file-set": (
                'target_sources(app PUBLIC FILE_SET conditional TYPE HEADERS '
                'BASE_DIRS "${CMAKE_SOURCE_DIR}" '
                'FILES "$<$<CONFIG:Release>:${CMAKE_SOURCE_DIR}/Lossy.h>")\n'
            ),
            "unsupported-config-definition": (
                'target_compile_definitions(app PRIVATE "$<$<CONFIG:Release>:LOSSY>")\n'
            ),
            "unresolved-install-interface": (
                'target_include_directories(app PRIVATE "$<INSTALL_INTERFACE:include>")\n'
            ),
        }
        ninja = shutil.which("ninja") or subprocess.check_output(
            ["mise", "which", "ninja"], text=True
        ).strip()
        for label, mutation in expressions.items():
            with self.subTest(label=label):
                case_root = self.root / label
                case_root.mkdir()
                (case_root / "Main.cpp").write_text(
                    "int main() { return 0; }\n", encoding="utf-8"
                )
                (case_root / "Lossy.cpp").write_text(
                    "struct QJsonObject {}; QJsonObject encode();\n",
                    encoding="utf-8",
                )
                (case_root / "Lossy.h").write_text(
                    "struct QJsonObject {}; QJsonObject encode();\n",
                    encoding="utf-8",
                )
                cmake = (
                    "cmake_minimum_required(VERSION 3.25)\n"
                    "project(GenexInventory CXX)\n"
                    f'include("{self.module}")\n'
                    'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")\n'
                    'file(WRITE "${CMAKE_BINARY_DIR}/generated/target_policy.txt" "")\n'
                    "add_executable(app Main.cpp)\n"
                    + mutation
                    + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
                    + 'cmake_language(DEFER CALL arkham_write_encoder_hygiene_target_universe '
                    'OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_universe.txt" '
                    'HEADER_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_header_index.txt" '
                    'HEADER_DIR "${CMAKE_BINARY_DIR}/generated/target_headers" '
                    'SOURCE_INDEX_FILE "${CMAKE_BINARY_DIR}/generated/target_source_index.txt" '
                    'SOURCE_DIR "${CMAKE_BINARY_DIR}/generated/target_sources")\n'
                )
                (case_root / "CMakeLists.txt").write_text(
                    cmake, encoding="utf-8"
                )
                configured = subprocess.run(
                    [
                        "cmake",
                        "-S",
                        str(case_root),
                        "-B",
                        str(case_root / "build"),
                        "-G",
                        "Ninja",
                        f"-DCMAKE_MAKE_PROGRAM={ninja}",
                        "-DCMAKE_BUILD_TYPE=Debug",
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertNotEqual(configured.returncode, 0)
                self.assertRegex(
                    configured.stdout + configured.stderr,
                    "generator.expression|configuration-closed|unsupported config",
                )

    def test_interface_manual_context_must_be_a_real_consumer(self) -> None:
        self._write("Main.cpp", "int main() { return 0; }\n")
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(InvalidInterfaceContext CXX)\n"
            f'include("{self.module}")\n'
            'file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/generated/target_policy.txt" "")\n'
            "add_executable(app Main.cpp)\n"
            "add_library(wire INTERFACE)\n"
            + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET wire CLASSIFICATION SCAN POLICY application CONTEXT_TARGET app OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
        )
        self._write("CMakeLists.txt", cmake)
        ninja = shutil.which("ninja") or subprocess.check_output(
            ["mise", "which", "ninja"], text=True
        ).strip()
        configured = subprocess.run(
            [
                "cmake",
                "-S",
                str(self.root),
                "-B",
                str(self.build),
                "-G",
                "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={ninja}",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(configured.returncode, 0)
        self.assertRegex(
            configured.stdout + configured.stderr,
            r"does\s+not consume INTERFACE target",
        )

    def test_test_trycompile_and_external_targets_need_explicit_metadata(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write(
            "tests/TestOnly.cpp",
            "struct QJsonObject {}; QJsonObject testOnlyEncoder() { return {}; }\n",
        )
        self._write(
            "checks/TryOnly.cpp",
            "struct QJsonObject {}; QJsonObject tryOnlyEncoder() { return {}; }\n",
        )
        self._write(
            "external/External.cpp",
            "struct QJsonObject {}; QJsonObject dependencyEncoder() { return {}; }\n",
        )
        self._write(
            "external/CMakeLists.txt",
            "add_library(external_dep STATIC External.cpp)\n",
        )
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(ExplicitExclusions CXX)\n"
            + self._manifest_prelude()
            + 'file(WRITE "${CMAKE_BINARY_DIR}/generated/external_roots.txt" "${CMAKE_SOURCE_DIR}/external\\n${CMAKE_BINARY_DIR}/external\\n")\n'
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_library(test_only STATIC tests/TestOnly.cpp)\n"
            + "add_library(try_only STATIC checks/TryOnly.cpp)\n"
            + "add_subdirectory(external)\n"
            + 'arkham_append_encoder_hygiene_target(TARGET test_only CLASSIFICATION TEST EXEMPT_REASON "explicit test-only target" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET try_only CLASSIFICATION TRY_COMPILE EXEMPT_REASON "explicit CMake probe target" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET external_dep CLASSIFICATION EXTERNAL EXEMPT_REASON "disposable external dependency" OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(
            ["domain", "foundation", "test_only", "try_only", "external_dep"]
        )
        self.assertEqual(
            ceh.run_check(self.root, self.build, skip_configure=True), []
        )

    def test_source_local_moc_uses_exact_built_configuration_paths(self) -> None:
        self._write(
            "src/domain/Source.cpp",
            "#include <QObject>\n"
            "class MultiConfigObject : public QObject {\n"
            "  Q_OBJECT\n"
            "};\n"
            '#include "Source.moc"\n',
        )
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(MultiConfigSourceMoc CXX)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core)\n"
            "set(CMAKE_AUTOMOC ON)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Source.cpp)\n"
            + "target_link_libraries(domain PUBLIC Qt6::Core)\n"
            + 'cmake_language(DEFER CALL arkham_append_target_autogen_manifest TARGET domain POLICY domain OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/autogen_targets.txt")\n'
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(
            ["domain"], qt=True, multi_config=True
        )
        closures = ceh._load_autogen_closures(self.build)
        self.assertEqual(len(closures["domain"]), 1)
        source_mocs = dict(closures["domain"][0].source_moc_owners)
        self.assertEqual(len(source_mocs), 1)
        self.assertIn("include_Debug", str(next(iter(source_mocs))))

    def test_release_compile_properties_are_independently_audited(self) -> None:
        self._write("src/domain/Domain.h", "#pragma once\n")
        self._write("src/domain/Domain.cpp", "int domainValue = 0;\n")
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", "int foundationValue = 0;\n")
        self._write(
            "src/App.h",
            "#pragma once\n"
            "struct QJsonObject {};\n"
            "#ifdef RELEASE_DEFINITION_ENCODER\n"
            "QJsonObject releaseDefinitionEncoder();\n"
            "#endif\n"
            "#ifdef RELEASE_OPTION_ENCODER\n"
            "QJsonObject releaseOptionEncoder();\n"
            "#endif\n"
            "#ifdef DEBUG_SAFE_CONTROL\n"
            "int debugSafeControl();\n"
            "#endif\n",
        )
        self._write(
            "release-include/ReleaseOnly.h",
            "#pragma once\n"
            "struct QJsonObject {};\n"
            "#ifdef RELEASE_INCLUDE_ENCODER\n"
            "QJsonObject releaseIncludeEncoder();\n"
            "#endif\n",
        )
        self._write("src/App.cpp", '#include "App.h"\nint main() { return 0; }\n')
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(ReleaseContext CXX)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + "add_executable(app src/App.cpp)\n"
            + 'target_include_directories(app PRIVATE "${CMAKE_SOURCE_DIR}/src" "$<$<CONFIG:Release>:${CMAKE_SOURCE_DIR}/release-include>")\n'
            + 'target_compile_definitions(app PRIVATE "$<$<CONFIG:Release>:RELEASE_DEFINITION_ENCODER;RELEASE_INCLUDE_ENCODER>" "$<$<CONFIG:Debug>:DEBUG_SAFE_CONTROL>")\n'
            + 'target_compile_options(app PRIVATE "$<$<CONFIG:Release>:-DRELEASE_OPTION_ENCODER>")\n'
            + 'target_sources(app PUBLIC FILE_SET app_headers TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/release-include" FILES "${CMAKE_SOURCE_DIR}/src/App.h" "${CMAKE_SOURCE_DIR}/release-include/ReleaseOnly.h")\n'
            + 'arkham_append_encoder_hygiene_target(TARGET app CLASSIFICATION SCAN POLICY application OUTPUT_FILE "${CMAKE_BINARY_DIR}/generated/target_policy.txt")\n'
            + self._register_manifests("domain", "foundation")
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(
            ["domain", "foundation", "app"], multi_config=True
        )
        findings = ceh.run_check(self.root, self.build, skip_configure=True)
        names = {finding.display_name.split("(", 1)[0] for finding in findings}
        self.assertTrue(
            {
                "releaseDefinitionEncoder",
                "releaseOptionEncoder",
                "releaseIncludeEncoder",
            }.issubset(names)
        )
        self.assertNotIn("debugSafeControl", names)

    def test_owned_autogen_is_exact_and_genuine_moc_still_passes(self) -> None:
        self._write(
            "src/domain/Domain.h",
            "#pragma once\n"
            "#include <QObject>\n"
            "class DomainObject : public QObject {\n"
            "  Q_OBJECT\n"
            "};\n",
        )
        domain_source = self._write(
            "src/domain/Domain.cpp",
            '#include "Domain.h"\n'
            "#include <QObject>\n"
            "class SourceLocalObject : public QObject {\n"
            "  Q_OBJECT\n"
            "};\n"
            '#include "Domain.moc"\n',
        )
        self._write("src/Foundation.h", "#pragma once\n")
        self._write("src/Foundation.cpp", '#include "Foundation.h"\n')
        cmake = (
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(OwnedAutogen CXX)\n"
            "find_package(Qt6 REQUIRED COMPONENTS Core)\n"
            "set(CMAKE_AUTOMOC ON)\n"
            + self._manifest_prelude()
            + "add_library(domain STATIC src/domain/Domain.cpp)\n"
            "target_link_libraries(domain PUBLIC Qt6::Core)\n"
            'target_sources(domain PUBLIC FILE_SET dh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src/domain" FILES "${CMAKE_SOURCE_DIR}/src/domain/Domain.h")\n'
            + "add_library(foundation STATIC src/Foundation.cpp)\n"
            "target_link_libraries(foundation PUBLIC Qt6::Core)\n"
            'target_sources(foundation PUBLIC FILE_SET fh TYPE HEADERS BASE_DIRS "${CMAKE_SOURCE_DIR}/src" FILES "${CMAKE_SOURCE_DIR}/src/Foundation.h")\n'
            + self._register_manifests("domain", "foundation", automoc=True)
        )
        self._write("CMakeLists.txt", cmake)
        self._configure_and_build(["domain", "foundation"], qt=True)
        self.assertEqual(ceh.run_check(self.root, self.build, skip_configure=True), [])

        autogen = self.build / "domain_autogen"
        source_moc = autogen / "include" / "Domain.moc"
        self.assertTrue(source_moc.is_file())
        self.assertNotIn(
            "Domain.moc",
            (autogen / "mocs_compilation.cpp").read_text(encoding="utf-8"),
        )
        source_moc_bytes = source_moc.read_bytes()
        source_moc.unlink()
        with self.assertRaisesRegex(ceh.EncoderHygieneError, "missing generated"):
            ceh.run_check(self.root, self.build, skip_configure=True)
        source_moc.write_bytes(source_moc_bytes)

        autogen_info = self.build / "CMakeFiles/domain_autogen.dir/AutogenInfo.json"
        metadata = json.loads(autogen_info.read_text(encoding="utf-8"))
        malformed = dict(metadata)
        malformed["SOURCES"] = [["malformed"]]
        autogen_info.write_text(json.dumps(malformed), encoding="utf-8")
        with self.assertRaisesRegex(ceh.EncoderHygieneError, "Malformed SOURCES"):
            ceh.run_check(self.root, self.build, skip_configure=True)
        autogen_info.write_text(json.dumps(metadata), encoding="utf-8")

        with domain_source.open("a", encoding="utf-8") as stream:
            stream.write(
                "\n#include <QJsonObject>\n"
                "QJsonObject sourceLocalAdjacentEncoder() { return {}; }\n"
            )
        adjacent_findings = ceh.run_check(
            self.root, self.build, skip_configure=True
        )
        self.assertTrue(
            any(
                "sourceLocalAdjacentEncoder" in finding.display_name
                for finding in adjacent_findings
            )
        )

        moc = next(path for path in autogen.rglob("moc_Domain.cpp"))
        with moc.open("a", encoding="utf-8") as stream:
            stream.write(
                "\n#include <QJsonObject>\n"
                "QJsonObject generatedWrapperEncoder() { return {}; }\n"
            )
        findings = ceh.run_check(self.root, self.build, skip_configure=True)
        self.assertTrue(
            any("generatedWrapperEncoder" in finding.display_name for finding in findings)
        )

        (autogen / "include" / "Rogue.moc").write_text(
            "struct QJsonObject {}; QJsonObject hiddenEncoder();\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(ceh.EncoderHygieneError, "unexplained generated"):
            ceh.run_check(self.root, self.build, skip_configure=True)


if __name__ == "__main__":
    unittest.main()
