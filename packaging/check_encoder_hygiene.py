#!/usr/bin/env python3
"""Prove, via real compiler-AST inspection (libclang), that no PUBLIC
declaration reachable from this project's domain-model header set
(src/CardCatalog.h, Decks.h, Games.h, Identifiers.h, JsonDecode.h,
RawJson.h, ValueOrError.h, ContractPin.h, ContractRevision.h -- see
CMakeLists.txt's ARKHAM_DOMAIN_HEADERS/the arkham_domain_models target)
returns a QJsonObject/QJsonArray/QJsonValue-family type, except for a
tiny, explicitly enumerated allowlist of legitimate symbols: the three
canonical, production-limited exact adapters in RawJson.h
(Value::toExactQJson/toExactQJsonObject/toExactQJsonArray) and the
decode-direction (inbound-only, never encoding) helpers in JsonDecode.h.

This replaces an earlier, purely source-text regex/parser-based version
of this check (see git history: tests/EncoderHygieneTests.cpp), which
repeated review rounds proved could not keep up with an open-ended set of
textual evasions: `auto&` returns, `decltype(...)` returns, type aliases,
macro-defined return types from an included/generated header, conversion
operators, overloads/duplicate identical class signatures colliding in a
basename-keyed allowlist, and comment/raw-string-literal desynchronization
of the stripper. Every one of those is a *textual* disguise for the exact
same *semantic* fact -- "this public function's return type, after full
compiler resolution of aliases/templates/decltype/auto/macros, is in the
QJson family" -- which only a real C++ compiler frontend can determine
with certainty. This script asks Clang to determine that fact directly,
via libclang's AST (https://clang.llvm.org/docs/LibClang.html), rather
than re-implementing an ever-growing fragment of C++ parsing by hand:

  - It parses the *real* production translation units (this project's
    own domain-model .cpp files) with their *real* compile flags (a
    dedicated Clang-toolchain build directory this script configures
    itself -- see `_configure_clang_build_dir()` -- rather than assuming
    whatever compiler the project's main/default build happens to use is
    Clang; ubuntu-latest's default toolchain is GCC, which does not
    expose libclang at all, so reusing the *default* build's
    compile_commands.json would not be portable to CI, per the review
    round that first required this rewrite).
  - For every public function-like declaration (ordinary methods, free
    functions, conversion operators, function templates) whose *location*
    resolves (after macro expansion) to one of the domain headers named
    above, it asks libclang for the *canonical* result type (i.e. with
    every typedef/using-alias/decltype/auto/template-parameter already
    resolved to its underlying real type by the compiler itself) and its
    USR (Unified Symbol Resolution -- a stable, fully qualified,
    signature-and-overload-aware identity Clang computes for every
    declaration; see
    https://clang.llvm.org/docs/USRs.html), together with its access
    specifier and exact source file.
  - A declaration is a *violation* if its canonical return type is in the
    QJson family (QJsonObject/QJsonArray/QJsonValue, with or without a
    reference/pointer/const qualifier) and its (file, USR) pair is not
    exactly one of the ALLOWLIST entries below. There is no general
    "looks like a decode helper" heuristic (e.g. "takes a QJson parameter,
    so it must be inbound-only") -- that itself would be a new textual/
    structural loophole (e.g. a lossy per-DTO `toJson(QJsonObject seed)`
    padded with an unused QJson-typed parameter purely to slip past such a
    rule). Every legitimate exception is named explicitly, by exact
    qualified USR *and* exact expected source file, so a same-named
    symbol accidentally (or deliberately) cloned into a different header
    -- even one with an identical namespace/class/signature, which would
    by definition share the same USR -- is still rejected unless it is
    physically declared in the one file this script expects it in.

No new third-party dependency is added for this: libclang is part of the
same Clang toolchain already used for clang-format elsewhere in this
project's tasks, loaded here directly via Python's built-in `ctypes`
against libclang's stable C ABI (not the unrelated, separately
pip-installed `clang`/`libclang` PyPI packages, which this script does
not use or require).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import glob as globmod
import json
import os
import platform
import re as _re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


# --- The tiny, explicit allowlist ------------------------------------------
#
# Every entry is (source_file_basename, qualified_usr). A declaration is
# permitted only if BOTH match exactly -- matching the USR alone would let
# an identically-named/signatured symbol cloned into an unexpected header
# slip through (the exact "duplicate identical class signature" evasion a
# prior review round demonstrated against a basename-only allowlist).
#
# RawJson.h: the three canonical, production-bounded exact adapters. These
# are the *only* encoding-direction (domain data -> QJson) public
# conversions permitted anywhere in the domain-model header set.
_CANONICAL_ADAPTERS = (
    ("RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJson#1"),
    ("RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonObject#1"),
    ("RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonArray#1"),
)

# JsonDecode.h: decode-direction (inbound QJson -> narrower/extracted
# QJson view) helpers. Every one of these takes an already-constructed
# QJsonObject/QJsonValue as an *input* parameter and returns a narrowed
# view of, or a member/field extracted from, that same already-existing
# value -- they never construct new QJson content from domain data, so
# they cannot introduce the numeric/duplicate-key/Undefined/surrogate
# fidelity loss the exact adapters above exist to prevent.
_DECODE_HELPERS = (
    ("JsonDecode.h", "c:@N@Arkham@N@Json@N@detail@F@findField#&1$@S@QJsonObject#$@S@QLatin1String#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObject#&1$@S@QJsonValue#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArray#&1$@S@QJsonValue#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@requireRawField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    ("JsonDecode.h", "c:@N@Arkham@N@Json@F@objectMembers#&1$@S@QJsonObject#"),
)

ALLOWLIST = frozenset(_CANONICAL_ADAPTERS + _DECODE_HELPERS)

_QJSON_FAMILY = ("QJsonObject", "QJsonArray", "QJsonValue")


class EncoderHygieneError(RuntimeError):
    """Raised for any condition this script treats as an outright failure
    (never silently downgraded to a skip/warning): libclang not found,
    compile_commands.json missing/unreadable, a translation unit that
    fails to parse without diagnostics, or an allowlisted symbol that
    could not be found at all (which would mean the allowlist itself has
    silently gone stale -- e.g. the adapter it names was renamed/removed
    -- and must be updated deliberately, not left passing for the wrong
    reason)."""


# --- Minimal ctypes bindings for libclang's stable C ABI -------------------


class _CXString(ctypes.Structure):
    _fields_ = [("data", ctypes.c_void_p), ("private_flags", ctypes.c_uint)]


class _CXType(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("data", ctypes.c_void_p * 2)]


class _CXCursor(ctypes.Structure):
    _fields_ = [("kind", ctypes.c_int), ("xdata", ctypes.c_int), ("data", ctypes.c_void_p * 3)]


class _CXSourceLocation(ctypes.Structure):
    _fields_ = [("ptr_data", ctypes.c_void_p * 2), ("int_data", ctypes.c_uint)]


class _CXUnsavedFile(ctypes.Structure):
    _fields_ = [("Filename", ctypes.c_char_p), ("Contents", ctypes.c_char_p), ("Length", ctypes.c_ulong)]


# Cursor kinds this script cares about (see clang-c/Index.h).
_CXCursor_FunctionDecl = 8
_CXCursor_ClassDecl = 4
_CXCursor_StructDecl = 3
_CXCursor_ClassTemplate = 31
_CXCursor_CXXMethod = 21
_CXCursor_Namespace = 22
_CXCursor_ConversionFunction = 26
_CXCursor_FunctionTemplate = 30

_FUNCTION_LIKE_KINDS = frozenset(
    {
        _CXCursor_FunctionDecl,
        _CXCursor_CXXMethod,
        _CXCursor_ConversionFunction,
        _CXCursor_FunctionTemplate,
    }
)

# CX_CXXAccessSpecifier (see clang-c/Index.h): 0 is "invalid" -- reported
# for cursors that are not class members at all (ordinary namespace-scope
# free functions), which are public by definition; 1 is explicitly public.
_CX_CXXInvalidAccessSpecifier = 0
_CX_CXXPublic = 1
_PUBLIC_ACCESS_SPECIFIERS = frozenset({_CX_CXXInvalidAccessSpecifier, _CX_CXXPublic})


_REAL_LIBCLANG_BASENAME_RE = _re.compile(r"^libclang(-\d+)?\.so(\.\d+)*$")


def _is_real_libclang_basename(basename: str) -> bool:
    """True for libclang.so, libclang.so.1, libclang-18.so,
    libclang-18.so.1, etc.; False for libclang-cpp.so* (Clang's internal,
    unstable C++ AST/frontend API library, which does not export the
    stable C ABI this script's ctypes bindings require -- see
    _find_libclang()'s doc comment)."""

    return bool(_REAL_LIBCLANG_BASENAME_RE.match(basename))


def _real_libclang_only(paths: list[str]) -> list[str]:
    return [p for p in paths if _is_real_libclang_basename(os.path.basename(p))]


def _find_libclang() -> Path:
    """Locate an already-installed libclang shared library. Never installs
    or downloads one -- if none of the well-known locations (or an
    explicit ARKHAM_LIBCLANG_PATH override) has it, this raises rather
    than silently skipping the whole check.

    Deliberately excludes `libclang-cpp.so*`: that library exposes
    Clang's internal, unstable C++ AST/frontend APIs, not the stable C
    ABI (`clang_getCString`, `clang_parseTranslationUnit2`, etc.) this
    script's ctypes bindings target -- loading it succeeds (ctypes.CDLL
    does not validate exported symbols at load time) but every
    `clang_*` C API call then fails with `undefined symbol`, since
    those C-ABI entry points are not exported by libclang-cpp at all.
    """

    override = os.environ.get("ARKHAM_LIBCLANG_PATH")
    if override:
        path = Path(override)
        if not path.is_file():
            raise EncoderHygieneError(
                f"ARKHAM_LIBCLANG_PATH={override!r} does not name an existing file"
            )
        return path

    candidates: list[str] = []
    system = platform.system()
    if system == "Darwin":
        candidates += [
            "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib",
            "/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/libclang.dylib",
        ]
        candidates += sorted(globmod.glob("/opt/homebrew/opt/llvm*/lib/libclang.dylib"), reverse=True)
        candidates += sorted(globmod.glob("/opt/homebrew/Cellar/llvm*/*/lib/libclang.dylib"), reverse=True)
        candidates += sorted(globmod.glob("/usr/local/opt/llvm*/lib/libclang.dylib"), reverse=True)
    else:
        # Debian/Ubuntu-style versioned packages (libclang-<N>-dev), plus
        # whatever `llvm-config` on PATH reports for its own install.
        llvm_config = shutil.which("llvm-config")
        if llvm_config:
            try:
                libdir = subprocess.check_output([llvm_config, "--libdir"], text=True).strip()
                candidates += _real_libclang_only(
                    sorted(globmod.glob(os.path.join(libdir, "libclang*.so*")), reverse=True)
                )
            except (subprocess.CalledProcessError, OSError):
                pass
        candidates += _real_libclang_only(
            sorted(globmod.glob("/usr/lib/llvm-*/lib/libclang*.so*"), reverse=True)
        )
        candidates += _real_libclang_only(
            sorted(globmod.glob("/usr/lib/*-linux-gnu/libclang*.so*"), reverse=True)
        )

    for candidate in candidates:
        if Path(candidate).is_file():
            return Path(candidate)

    found_by_ctypes = ctypes.util.find_library("clang")
    if found_by_ctypes and "cpp" not in Path(found_by_ctypes).name:
        return Path(found_by_ctypes)

    raise EncoderHygieneError(
        "Could not locate a real libclang (C ABI, not libclang-cpp) shared "
        "library anywhere. This check requires an installed Clang toolchain "
        "exposing libclang (already used elsewhere in this project for "
        "clang-format); set ARKHAM_LIBCLANG_PATH to an explicit path if it "
        "is installed somewhere non-standard. Refusing to silently skip "
        "this check."
    )


class _LibClang:
    """Thin, narrowly-scoped ctypes wrapper exposing only the libclang C
    API entry points this script actually calls."""

    def __init__(self, path: Path) -> None:
        self.lib = ctypes.CDLL(str(path))
        lib = self.lib

        lib.clang_getCString.restype = ctypes.c_char_p
        lib.clang_getCString.argtypes = [_CXString]
        lib.clang_disposeString.argtypes = [_CXString]

        lib.clang_createIndex.restype = ctypes.c_void_p
        lib.clang_createIndex.argtypes = [ctypes.c_int, ctypes.c_int]

        lib.clang_parseTranslationUnit2.restype = ctypes.c_int
        lib.clang_parseTranslationUnit2.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_char_p),
            ctypes.c_int,
            ctypes.POINTER(_CXUnsavedFile),
            ctypes.c_uint,
            ctypes.c_uint,
            ctypes.POINTER(ctypes.c_void_p),
        ]

        lib.clang_getTranslationUnitCursor.restype = _CXCursor
        lib.clang_getTranslationUnitCursor.argtypes = [ctypes.c_void_p]

        self._visitor_func_type = ctypes.CFUNCTYPE(
            ctypes.c_int, _CXCursor, _CXCursor, ctypes.c_void_p
        )
        lib.clang_visitChildren.restype = ctypes.c_uint
        lib.clang_visitChildren.argtypes = [_CXCursor, self._visitor_func_type, ctypes.c_void_p]

        lib.clang_getCursorKind.restype = ctypes.c_int
        lib.clang_getCursorKind.argtypes = [_CXCursor]
        lib.clang_getCursorDisplayName.restype = _CXString
        lib.clang_getCursorDisplayName.argtypes = [_CXCursor]
        lib.clang_getCursorUSR.restype = _CXString
        lib.clang_getCursorUSR.argtypes = [_CXCursor]
        lib.clang_getCXXAccessSpecifier.restype = ctypes.c_int
        lib.clang_getCXXAccessSpecifier.argtypes = [_CXCursor]
        lib.clang_getCursorResultType.restype = _CXType
        lib.clang_getCursorResultType.argtypes = [_CXCursor]
        lib.clang_getCanonicalType.restype = _CXType
        lib.clang_getCanonicalType.argtypes = [_CXType]
        lib.clang_getTypeSpelling.restype = _CXString
        lib.clang_getTypeSpelling.argtypes = [_CXType]

        lib.clang_getCursorLocation.restype = _CXSourceLocation
        lib.clang_getCursorLocation.argtypes = [_CXCursor]
        lib.clang_getExpansionLocation.argtypes = [
            _CXSourceLocation,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
        ]
        lib.clang_getFileName.restype = _CXString
        lib.clang_getFileName.argtypes = [ctypes.c_void_p]

        lib.clang_getNumDiagnostics.restype = ctypes.c_uint
        lib.clang_getNumDiagnostics.argtypes = [ctypes.c_void_p]
        lib.clang_getDiagnostic.restype = ctypes.c_void_p
        lib.clang_getDiagnostic.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        lib.clang_formatDiagnostic.restype = _CXString
        lib.clang_formatDiagnostic.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        lib.clang_getDiagnosticSeverity.restype = ctypes.c_int
        lib.clang_getDiagnosticSeverity.argtypes = [ctypes.c_void_p]

        lib.clang_disposeTranslationUnit.argtypes = [ctypes.c_void_p]
        lib.clang_disposeIndex.argtypes = [ctypes.c_void_p]

    def to_str(self, cxstr: _CXString) -> str:
        raw = self.lib.clang_getCString(cxstr)
        result = raw.decode("utf-8", "replace") if raw else ""
        self.lib.clang_disposeString(cxstr)
        return result

    def cursor_file_and_line(self, cursor: _CXCursor) -> tuple[str | None, int]:
        loc = self.lib.clang_getCursorLocation(cursor)
        file_ptr = ctypes.c_void_p()
        line = ctypes.c_uint()
        col = ctypes.c_uint()
        offset = ctypes.c_uint()
        self.lib.clang_getExpansionLocation(
            loc, ctypes.byref(file_ptr), ctypes.byref(line), ctypes.byref(col), ctypes.byref(offset)
        )
        if not file_ptr.value:
            return None, 0
        return self.to_str(self.lib.clang_getFileName(file_ptr)), line.value


@dataclass(frozen=True)
class Finding:
    file: str  # basename only, e.g. "RawJson.h"
    line: int
    display_name: str
    canonical_return_type: str
    usr: str

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


def _is_qjson_family(canonical_type_spelling: str) -> bool:
    return any(family in canonical_type_spelling for family in _QJSON_FAMILY)


def classify(finding: Finding) -> str:
    """Pure decision function (no I/O, directly unit-tested): 'violation'
    if this finding's canonical return type is QJson-family and it is not
    named in ALLOWLIST by exact (file, USR); 'allowed' otherwise
    (including every non-QJson-returning declaration, which this script
    never even constructs a Finding for -- see `_should_record()` -- but
    classify() stays total/defensive regardless)."""

    if not _is_qjson_family(finding.canonical_return_type):
        return "allowed"
    if finding.key() in ALLOWLIST:
        return "allowed"
    return "violation"


def _sanitize_compile_args(command: str, source_file: str) -> list[str]:
    """Turn one compile_commands.json entry's shell command string into the
    argv libclang's clang_parseTranslationUnit2() expects: drop the
    compiler executable itself (argv[0]), -o/-c/-arch <value>/-g (all
    irrelevant to AST-only parsing; some, like a stray -arch on a
    cross-compile entry, could even cause a spurious parse failure), and
    the trailing source file (passed to clang_parseTranslationUnit2()
    separately, as its own file argument, not duplicated in argv)."""

    tokens = shlex.split(command)
    args = tokens[1:]  # drop argv[0] (the compiler executable)
    cleaned: list[str] = []
    skip_next = False
    for token in args:
        if skip_next:
            skip_next = False
            continue
        if token in ("-o", "-arch"):
            skip_next = True
            continue
        if token in ("-c", "-g"):
            continue
        cleaned.append(token)
    if cleaned and cleaned[-1] == source_file:
        cleaned = cleaned[:-1]
    return cleaned


def _macos_sdk_sysroot() -> str:
    return subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True
    ).strip()


def _collect_findings(
    clang: _LibClang,
    compile_commands: list[dict],
    domain_sources: Sequence[Path],
    domain_headers: frozenset[Path],
) -> list[Finding]:
    commands_by_file = {Path(entry["file"]).resolve(): entry for entry in compile_commands}
    is_macos = platform.system() == "Darwin"
    sysroot_args = ["-isysroot", _macos_sdk_sysroot()] if is_macos else []

    domain_header_basenames = {p.resolve() for p in domain_headers}

    findings: dict[tuple[str, str], Finding] = {}

    idx = clang.lib.clang_createIndex(0, 0)
    if not idx:
        raise EncoderHygieneError("clang_createIndex() failed")

    # CXChildVisit_Break = 0, CXChildVisit_Continue = 1, CXChildVisit_Recurse = 2.
    # Returning Recurse lets libclang itself walk into a cursor's children
    # (rather than this script re-invoking clang_visitChildren reentrantly,
    # which would create a fresh ctypes trampoline per node); once inside a
    # function-like cursor's own body, further recursion is pruned (function
    # bodies can never contain nested declarations reachable from outside,
    # only statements/expressions), which keeps this from needlessly
    # descending into every statement of every implementation file.
    def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        if kind in _FUNCTION_LIKE_KINDS:
            filename, line = clang.cursor_file_and_line(cursor)
            if filename is not None:
                resolved = Path(filename).resolve()
                if resolved in domain_header_basenames:
                    access = clang.lib.clang_getCXXAccessSpecifier(cursor)
                    if access in _PUBLIC_ACCESS_SPECIFIERS:
                        result_type = clang.lib.clang_getCursorResultType(cursor)
                        canonical = clang.lib.clang_getCanonicalType(result_type)
                        spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
                        if _is_qjson_family(spelling):
                            usr = clang.to_str(clang.lib.clang_getCursorUSR(cursor))
                            display = clang.to_str(clang.lib.clang_getCursorDisplayName(cursor))
                            finding = Finding(
                                file=resolved.name,
                                line=line,
                                display_name=display,
                                canonical_return_type=spelling,
                                usr=usr,
                            )
                            findings[finding.key()] = finding
            return 1  # CXChildVisit_Continue: do not descend into the body.
        return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

    for source in domain_sources:
        entry = commands_by_file.get(source.resolve())
        if entry is None:
            raise EncoderHygieneError(
                f"No compile_commands.json entry found for domain source {source} "
                "-- the dedicated Clang build directory may not have configured/"
                "built this target; see _configure_clang_build_dir()."
            )
        args = _sanitize_compile_args(entry["command"], entry["file"]) + sysroot_args
        args_bytes = [a.encode("utf-8") for a in args]
        argv = (ctypes.c_char_p * len(args_bytes))(*args_bytes)

        tu_ptr = ctypes.c_void_p()
        err = clang.lib.clang_parseTranslationUnit2(
            idx, entry["file"].encode("utf-8"), argv, len(args_bytes), None, 0, 0x0, ctypes.byref(tu_ptr)
        )
        if err != 0 or not tu_ptr.value:
            raise EncoderHygieneError(
                f"libclang failed to parse {entry['file']} (CXErrorCode={err}); "
                "this must never be silently skipped, since a translation unit "
                "this script cannot parse is a translation unit it cannot prove "
                "anything about."
            )
        tu = tu_ptr.value

        diag_count = clang.lib.clang_getNumDiagnostics(tu)
        fatal_diagnostics = []
        for i in range(diag_count):
            diag = clang.lib.clang_getDiagnostic(tu, i)
            severity = clang.lib.clang_getDiagnosticSeverity(diag)
            if severity >= 3:  # CXDiagnostic_Error or CXDiagnostic_Fatal
                fatal_diagnostics.append(clang.to_str(clang.lib.clang_formatDiagnostic(diag, 0)))
        if fatal_diagnostics:
            raise EncoderHygieneError(
                f"Clang reported {len(fatal_diagnostics)} error diagnostic(s) parsing "
                f"{entry['file']}; a translation unit that does not compile cleanly "
                "cannot be trusted to have a correct AST, so this check refuses to "
                "proceed rather than silently scan a partial/error-recovery AST:\n"
                + "\n".join(f"  {d}" for d in fatal_diagnostics)
            )

        root = clang.lib.clang_getTranslationUnitCursor(tu)
        clang.lib.clang_visitChildren(root, clang._visitor_func_type(visitor), None)
        clang.lib.clang_disposeTranslationUnit(tu)

    clang.lib.clang_disposeIndex(idx)
    return list(findings.values())


def _read_manifest(path: Path) -> list[Path]:
    if not path.is_file():
        raise EncoderHygieneError(
            f"Manifest {path} does not exist. Run `cmake` configure (see "
            "CMakeLists.txt's ARKHAM_DOMAIN_HEADERS/ARKHAM_DOMAIN_SOURCES, "
            "which generate this file) before running this script."
        )
    lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()]
    return [Path(line) for line in lines if line]


def _configure_clang_build_dir(repo_root: Path, build_dir: Path) -> None:
    """Configure (and build just the arkham_domain_models target of) a
    dedicated CMake build directory using Clang explicitly as the
    compiler, independent of whatever compiler this project's default/
    main build directory happens to use (ubuntu-latest's default is GCC,
    which exposes no libclang at all). This is what lets this script's
    compile_commands.json entries be handed to libclang with minimal,
    predictable sanitization (see _sanitize_compile_args()) rather than
    guessing which of an arbitrary other compiler's flags libclang would
    accept."""

    clangxx = os.environ.get("ARKHAM_CLANGXX", "clang++")
    if shutil.which(clangxx) is None:
        raise EncoderHygieneError(
            f"{clangxx!r} was not found on PATH. This check requires a Clang "
            "C++ compiler (set ARKHAM_CLANGXX to an explicit path if it is "
            "installed somewhere not on PATH); refusing to silently skip."
        )

    qt_prefix = os.environ.get("QT_PREFIX") or os.environ.get("QTDIR")
    if not qt_prefix:
        brew_prefix = shutil.which("brew")
        if brew_prefix:
            try:
                qt_prefix = subprocess.check_output(["brew", "--prefix"], text=True).strip()
            except (subprocess.CalledProcessError, OSError):
                qt_prefix = None

    configure_cmd = [
        "cmake",
        "-S",
        str(repo_root),
        "-B",
        str(build_dir),
        "-G",
        "Ninja",
        f"-DCMAKE_CXX_COMPILER={clangxx}",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DBUILD_TESTING=OFF",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    if qt_prefix:
        configure_cmd.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")

    subprocess.run(configure_cmd, check=True, cwd=repo_root)
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", "arkham_domain_models"],
        check=True,
        cwd=repo_root,
    )


def run_check(repo_root: Path, clang_build_dir: Path, skip_configure: bool) -> list[Finding]:
    if not skip_configure:
        _configure_clang_build_dir(repo_root, clang_build_dir)

    compile_commands_path = clang_build_dir / "compile_commands.json"
    if not compile_commands_path.is_file():
        raise EncoderHygieneError(f"{compile_commands_path} does not exist after configuring")
    compile_commands = json.loads(compile_commands_path.read_text(encoding="utf-8"))

    generated_dir = clang_build_dir / "generated"
    domain_headers = frozenset(_read_manifest(generated_dir / "domain_headers.txt"))
    domain_sources = _read_manifest(generated_dir / "domain_sources.txt")

    libclang_path = _find_libclang()
    clang = _LibClang(libclang_path)

    findings = _collect_findings(clang, compile_commands, domain_sources, domain_headers)
    return findings


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root (default: this script's parent directory's parent).",
    )
    parser.add_argument(
        "--clang-build-dir",
        type=Path,
        default=None,
        help="Dedicated Clang-toolchain build directory this script configures "
        "and builds arkham_domain_models in (default: <repo-root>/build-encoder-hygiene).",
    )
    parser.add_argument(
        "--skip-configure",
        action="store_true",
        help="Reuse an already-configured/-built --clang-build-dir instead of "
        "reconfiguring it (useful for repeated local runs).",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="Print every QJson-family-returning declaration found (with its "
        "exact USR and classification) and exit, without applying pass/fail "
        "policy. Intended for maintainers updating ALLOWLIST after a "
        "deliberate, reviewed change to one of the 12 legitimate adapters/"
        "helpers -- never as a way to silence a real violation.",
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    clang_build_dir = (args.clang_build_dir or (repo_root / "build-encoder-hygiene")).resolve()

    try:
        findings = run_check(repo_root, clang_build_dir, args.skip_configure)
    except EncoderHygieneError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.list:
        for f in sorted(findings, key=lambda f: (f.file, f.line)):
            print(f"{classify(f):9s} {f.file}:{f.line}  {f.display_name}")
            print(f"          canonical return type: {f.canonical_return_type}")
            print(f"          USR: {f.usr}")
        return 0

    violations = [f for f in findings if classify(f) == "violation"]

    found_allowlist_keys = {f.key() for f in findings}
    missing_allowlist_entries = sorted(ALLOWLIST - found_allowlist_keys)

    if missing_allowlist_entries:
        print(
            "error: the following allowlisted canonical adapter/decode-helper "
            "symbols were NOT found in the scanned AST at all -- the allowlist "
            "itself has gone stale (renamed/removed?) and must be updated "
            "deliberately rather than silently left passing for the wrong "
            "reason:",
            file=sys.stderr,
        )
        for file, usr in missing_allowlist_entries:
            print(f"  {file}: {usr}", file=sys.stderr)
        return 1

    if violations:
        print(
            f"error: {len(violations)} public QJson-returning declaration(s) in the "
            "domain-model header set are not in the tiny explicit allowlist:",
            file=sys.stderr,
        )
        for v in sorted(violations, key=lambda f: (f.file, f.line)):
            print(f"  {v.file}:{v.line}  {v.display_name}", file=sys.stderr)
            print(f"      canonical return type: {v.canonical_return_type}", file=sys.stderr)
            print(f"      USR: {v.usr}", file=sys.stderr)
        return 1

    print(
        f"Encoder hygiene: {len(findings)} public QJson-returning declaration(s) "
        f"found in the domain-model header set, all {len(ALLOWLIST)} allowlist "
        "entries accounted for, zero violations."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
