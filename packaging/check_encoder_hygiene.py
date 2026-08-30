#!/usr/bin/env python3
"""Prove, via real compiler-AST inspection (libclang), that no PUBLIC
declaration in EITHER of this project's two production public header sets
returns a QJsonObject/QJsonArray/QJsonValue-family type, except for a tiny,
explicitly enumerated allowlist of legitimate symbols:

  - The domain-model header set (src/domain/CardCatalog.h, ContractPin.h,
    ContractRevision.h, Decks.h, Games.h, Identifiers.h, JsonDecode.h,
    RawJson.h, ValueOrError.h -- see CMakeLists.txt's
    ARKHAM_DOMAIN_HEADERS / the arkham_domain_models target). Its only
    permitted QJson-returning symbols are the three canonical,
    production-limited exact adapters in RawJson.h
    (Value::toExactQJson/toExactQJsonObject/toExactQJsonArray) and the
    decode-direction (inbound-only, never encoding) helpers in
    JsonDecode.h.
  - The foundation header set (every other public header this build
    compiles -- networking, auth, session, keychain, input/controller UI
    glue, app composition -- see ARKHAM_FOUNDATION_HEADERS / the
    arkham_foundation target). Its only permitted QJson-returning symbols
    are AuthModels.h's two legitimate request-body encoders
    (AuthenticateRequest::toJson, RegisterRequest::toJson).

Both header sets are audited by this ONE script/policy, not just the
domain one: a review round demonstrated that scoping the AST scan to
domain headers alone left the *entire* foundation FILE_SET (including
AuthModels.h) completely unobserved, so a third undesired QJson-returning
encoder anywhere in foundation would never be caught at all.

This also replaces an earlier, source-*file*-driven version of this
script: it used to parse only the real production .cpp files as
translation units and filter cursors by whether they resolved to a
header in the domain set. That meant a header registered in the manifest
but never #include-d by any *current* .cpp was invisible to the scan
entirely -- proven by a review round that showed adding a lossy-encoder
header to the domain manifest, without changing any real source file,
produced zero findings. This script now parses every header in BOTH
manifests directly and independently: for each header path, it builds a
synthetic in-memory ("unsaved") translation unit containing nothing but
`#include "<that exact header, by absolute path>"`, using compile flags
borrowed from a real source file belonging to that header's own CMake
target (every source in one of this project's two library targets shares
identical target-level -I/-D/-std flags, verified directly against
CMakeLists.txt: neither target uses set_source_files_properties() or
per-file target_compile_definitions() to vary flags source-by-source).
This guarantees every registered header is independently, exhaustively
parsed regardless of what any .cpp currently #includes, and
clang_getFile() is used afterwards to hard-fail if the wrapper's
`#include` somehow did not resolve to the exact intended file (rather
than silently trusting that it did).

A review round subsequently demonstrated that headers/fragments were the
*only* thing ever independently parsed this way: domain_sources.txt/
foundation_sources.txt were read only to borrow one "representative"
compile-args list for scanning headers, so a REAL production .cpp file
was never itself parsed/audited at all -- it could #include an
absolute/"../"-relative/symlinked/generated forbidden header, inherit
whatever lossy encoders that header declares, and compile with this
check staying green throughout. This script now ALSO independently
parses every real source in both manifests directly (see
_parse_source_as_own_tu()/_scan_sources()) -- each with its own exact
compile_commands.json entry, never a borrowed one, since (unlike a
header) a real .cpp already has its own -- and audits its complete
resolved inclusion graph exactly like a header's. Source scanning
deliberately never collects new QJson-family findings, only inclusion-
graph violations (see _scan_sources()'s own doc comment for why: every
allowlisted encoder is declared in a header but *defined* out-of-line in
its own .cpp, so naively scanning declarations located in a source
itself would misclassify every legitimate encoder's own definition as an
unrecognized violation).

An even earlier version of this check (see git history:
tests/EncoderHygieneTests.cpp) was a purely source-text regex/parser,
which repeated review rounds proved could not keep up with an
open-ended set of textual evasions: `auto&` returns, `decltype(...)`
returns, type aliases, macro-defined return types from an
included/generated header, conversion operators, overloads/duplicate
identical class signatures colliding in a basename-keyed allowlist, and
comment/raw-string-literal desynchronization of the stripper. Every one
of those is a *textual* disguise for the exact same *semantic* fact --
"this public function's return type, after full compiler resolution of
aliases/templates/decltype/auto/macros, is in the QJson family" -- which
only a real C++ compiler frontend can determine with certainty. This
script asks Clang to determine that fact directly, via libclang's AST
(https://clang.llvm.org/docs/LibClang.html), rather than re-implementing
an ever-growing fragment of C++ parsing by hand:

  - For every public function-like declaration (ordinary methods, free
    functions, conversion operators, function templates) whose *own*
    location (after macro expansion) is exactly the one header currently
    being probed, this script asks libclang for the *canonical* result
    type (i.e. with every typedef/using-alias/decltype/auto/template
    parameter already resolved to its underlying real type by the
    compiler itself) and its USR (Unified Symbol Resolution -- a stable,
    fully qualified, signature-and-overload-aware identity Clang
    computes for every declaration; see
    https://clang.llvm.org/docs/USRs.html), together with its access
    specifier and exact source file.
  - A declaration is a *violation* if its canonical return type is in the
    QJson family (QJsonObject/QJsonArray/QJsonValue, with or without a
    reference/pointer/const qualifier) and its (file, USR) pair, counted
    by *exact occurrence count*, is not one of the ALLOWLIST entries
    below. There is no general "looks like a decode helper" heuristic
    (e.g. "takes a QJson parameter, so it must be inbound-only") -- that
    itself would be a new textual/structural loophole (e.g. a lossy
    per-DTO `toJson(QJsonObject seed)` padded with an unused QJson-typed
    parameter purely to slip past such a rule). Every legitimate
    exception is named explicitly, by exact qualified USR, exact expected
    source file (a full, repo-root-relative path -- never a bare
    basename, which would incorrectly collide two different files that
    happen to share a name), and exact expected occurrence count, so
    neither a same-named symbol cloned into a different header nor a
    same-named symbol declared an unexpected number of times in its
    correct header can slip through.

No new third-party dependency is added for this: libclang is part of the
same Clang toolchain already used for clang-format elsewhere in this
project's tasks, loaded here directly via Python's built-in `ctypes`
against libclang's stable C ABI (not the unrelated, separately
pip-installed `clang`/`libclang` PyPI packages, which this script does
not use or require).

This script also independently, structurally proves the domain/
foundation *dependency direction* boundary a review round demonstrated
was not actually enforced by the compiler in the way an earlier
CMakeLists.txt revision's comments (and a since-rewritten, now-removed
compiled probe target) claimed: narrowing arkham_domain_models's public
include path to only "src/domain" blocks a *bare*
`#include "AuthModels.h"` (no `-I src` left to search), but it never
blocked -- and cannot block, no matter how include paths are narrowed --
a *relative-parent* `#include "../AuthModels.h"` written inside a file
that itself lives in src/domain/, because quote-form #include always
searches relative to the including file's own directory *before*
consulting any `-I`/FILE_SET path at all. The same is true of an
absolute-path #include, or a same-directory symlink whose target
happens to resolve outside src/domain/. None of these are blockable by
any compile-flag/include-path configuration; they can only be caught by
inspecting what a header's #include actually, truly resolved to.

So, for every header/fragment in BOTH manifests, this script also asks
libclang for the *complete resolved inclusion graph* of that header's
own wrapper translation unit (via `clang_getInclusions()` -- see
https://clang.llvm.org/docs/LibClang.html -- which reports every file
the compiler actually entered, transitively, no matter how the
#include that reached it was spelled: bare, "../"-relative, absolute,
via a symlink, or via a project-generated wrapper header) and requires
every *project-owned* file that graph reaches (i.e. every included file
whose own canonicalized, symlink-resolved real path lies inside this
repository's tracked source tree -- anything outside it entirely, such
as a Qt/system/toolchain header, is unconditionally external and
exempt) to be a member of the closure this script was told is
legitimate for the header currently being probed. "This repository's
tracked source tree" deliberately excludes this script's own dedicated
Clang-toolchain build directory (see _configure_clang_build_dir() and
_audit_inclusion_graph()): that directory is, by default, physically
nested inside the repository itself, and CMake's FetchContent (see
CMakeLists.txt's QtKeychain declaration) downloads/builds genuinely
external third-party source and generated build artifacts directly
under it -- none of that is this repository's own tracked source,
merely a byproduct of where this check happens to configure its
scratch build, and must be classified exactly like any other external
header:

  - Scanning a DOMAIN header/fragment: the only permitted project-owned
    included files are other members of the domain closure itself
    (ARKHAM_DOMAIN_HEADERS + ARKHAM_DOMAIN_FRAGMENTS) -- reaching
    *anything* foundation-owned (AuthModels.h or otherwise), by any
    spelling whatsoever, is a hard structural violation. This is the
    domain -> foundation direction the review round showed must be
    forbidden.
  - Scanning a FOUNDATION header/fragment: the domain closure is
    additionally permitted (foundation -> domain is the allowed
    direction; arkham_foundation legitimately links
    arkham_domain_models), but nothing else project-owned/unregistered
    is -- an unregistered fragment, a project-generated header outside
    both closures, etc. is still a hard structural violation there too.

Every declared entry in every manifest is also independently required
to resolve (after following any symlink) to a real path *physically
located inside its own claimed root* (src/domain/ for every domain
entry, src/ for every foundation entry) before any of the above even
runs -- this is what stops a same-named symlink smuggled directly into
one of the manifests itself (rather than merely #include-d from
elsewhere) from silently widening a closure to include a file it does
not actually, physically contain.

Since a header's own declarations may now legitimately be discovered
while scanning a *different* header's wrapper TU (e.g. Decks.h's
wrapper TU also enters ValueOrError.h, which Decks.h legitimately
#includes), every declaration is attributed to its own true resolved
source file (never to whichever header's wrapper TU happened to reach
it) and recorded in one global, whole-run (file, line, USR) dedup set,
so a legitimately shared/cross-included file's declarations are counted
exactly once no matter how many other headers in its own closure
#include it -- never once per including header.
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
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


# --- The tiny, explicit allowlists ------------------------------------------
#
# Every entry names a source_file (a full path, relative to the repo root,
# with forward slashes -- portable across checkouts/CI runners, and never
# a bare basename), a qualified_usr, and an expected_count (how many times
# that exact declaration must be found in that exact file -- not merely
# "at least once"). A declaration is permitted only if ALL THREE match
# exactly for its (file, usr) pair's total observed count.


@dataclass(frozen=True)
class AllowlistEntry:
    file: str  # repo-root-relative, forward-slash path, e.g. "src/domain/RawJson.h"
    usr: str
    expected_count: int = 1

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


# src/domain/RawJson.h: the three canonical, production-bounded exact
# adapters. These are the *only* encoding-direction (domain data -> QJson)
# public conversions permitted anywhere in the domain-model header set.
_CANONICAL_ADAPTERS = (
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJson#1"),
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonObject#1"),
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonArray#1"),
)

# src/domain/JsonDecode.h: decode-direction (inbound QJson -> narrower/
# extracted QJson view) helpers. Every one of these takes an
# already-constructed QJsonObject/QJsonValue as an *input* parameter and
# returns a narrowed view of, or a member/field extracted from, that same
# already-existing value -- they never construct new QJson content from
# domain data, so they cannot introduce the numeric/duplicate-key/
# Undefined/surrogate fidelity loss the exact adapters above exist to
# prevent.
_DECODE_HELPERS = (
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@N@detail@F@findField#&1$@S@QJsonObject#$@S@QLatin1String#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObject#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArray#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireRawField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#"),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@objectMembers#&1$@S@QJsonObject#"),
)

DOMAIN_ALLOWLIST: tuple[AllowlistEntry, ...] = _CANONICAL_ADAPTERS + _DECODE_HELPERS

# src/AuthModels.h: the two legitimate foundation-layer request-body
# encoders. These deliberately live outside the domain-model header set
# (they carry secrets -- see AuthModels.h's own module comment on why
# they have no QDebug/toString either) and are the *only* QJson-returning
# public declarations permitted anywhere in the foundation header set.
FOUNDATION_ALLOWLIST: tuple[AllowlistEntry, ...] = (
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@AuthenticateRequest@F@toJson#1"),
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@RegisterRequest@F@toJson#1"),
)

ALLOWLIST: tuple[AllowlistEntry, ...] = DOMAIN_ALLOWLIST + FOUNDATION_ALLOWLIST
ALLOWLIST_BY_KEY: dict[tuple[str, str], AllowlistEntry] = {e.key(): e for e in ALLOWLIST}

if len(ALLOWLIST_BY_KEY) != len(ALLOWLIST):
    raise AssertionError(
        "ALLOWLIST contains two entries with the identical (file, usr) key; "
        "that can only be an authoring mistake in this script itself."
    )

_QJSON_FAMILY = ("QJsonObject", "QJsonArray", "QJsonValue", "QJsonDocument")

# QJsonDocument was added above after a review round demonstrated it was
# absent from the prohibited family despite carrying the identical
# numeric/duplicate-key/Undefined fidelity-loss risk as
# QJsonObject/QJsonArray/QJsonValue -- it is a distinct Qt class (not a
# typedef), so, exactly like the other three, its canonical type spelling
# is simply "QJsonDocument" (with any const/reference/pointer qualifier
# preserved verbatim around it), caught by the same substring match
# _is_qjson_family() already applies to the rest of the family.
#
# Wrapped/reference/pointer-qualified forms of any family member (e.g.
# "const QJsonObject &", "QJsonObject *") were empirically confirmed (see
# packaging/check_encoder_hygiene_test.py's
# QJsonFamilyWrappedFormsAreDetectedTests) to already be caught by this
# same substring match against Clang's canonical type spelling, since
# Clang's canonical spelling of a qualified/pointer type still contains
# the unqualified class name verbatim. The same is true of every
# standard-library wrapper/callable form checked
# (std::optional<QJsonObject>, std::shared_ptr<QJsonObject>,
# std::unique_ptr<QJsonObject>, std::function<QJsonObject()>) -- none of
# those needed any code change.
#
# Qt's own QVariantMap/QVariantList/QVariantHash are a DIFFERENT,
# real bypass, however: they are typedefs for QMap<QString,
# QVariant>/QList<QVariant>/QHash<QString, QVariant> respectively, so
# Clang's canonical type resolution replaces the typedef name entirely
# with its underlying template instantiation -- "QVariantMap" itself
# never appears anywhere in the canonical spelling libclang reports, so
# simply adding it as another _QJSON_FAMILY substring (as this comment's
# neighboring QJsonDocument entry could) would silently match nothing.
# Qt itself defines lossless two-way conversions between these and the
# QJson family (QJsonObject::toVariantMap()/fromVariantMap(),
# QJsonArray::toVariantList()/fromVariantList()), so a public encoder
# returning one of these three is exactly as capable of reintroducing
# the numeric/duplicate-key/Undefined fidelity loss the exact adapters
# exist to prevent -- see _is_qvariant_json_container() below, which
# recognizes them by their actual canonical (post-typedef-resolution)
# spelling instead.
#
# A bare `QVariant` return, by contrast, is NOT added to any prohibited
# family: its own static type says nothing about what it happens to
# contain at runtime (unlike QVariantMap/QVariantList/QVariantHash,
# whose static type IS a JSON-shaped container), or a body would need to
# be inspected to tell -- which this declaration-based AST policy
# deliberately never does (see the module docstring's rationale for why
# a purely source-text/body-parsing rule was abandoned as fundamentally
# unable to keep up with an open-ended set of evasions; the same
# argument for why the AST approach was adopted in the first place is
# exactly why it must stay declaration-shape-based, not body-content-
# based, here too). A legitimate, unrelated public `QVariant` getter
# (e.g. a Q_PROPERTY-style accessor in the input/controller UI glue
# layer) remains unaffected.
_QVARIANT_JSON_CONTAINER_CANONICAL_FORMS = (
    "QMap<QString,QVariant>",
    "QList<QVariant>",
    "QHash<QString,QVariant>",
)


def _is_qvariant_json_container(canonical_type_spelling: str) -> bool:
    """True if `canonical_type_spelling` (Clang's canonical, fully
    typedef-resolved type spelling) names QVariantMap/QVariantList/
    QVariantHash, in any const/reference/pointer-qualified form -- see
    the doc comment above _QVARIANT_JSON_CONTAINER_CANONICAL_FORMS for
    why these must be matched by their post-typedef-resolution
    template-instantiation spelling rather than their typedef'd name.

    All whitespace is stripped from both sides of the comparison before
    matching: Clang's exact template-argument-list spelling has been
    empirically observed to vary in incidental whitespace (e.g. a space
    or no space after a template-argument comma) across Clang
    versions/platforms, and this check must not depend on that."""

    normalized = "".join(canonical_type_spelling.split())
    return any(form in normalized for form in _QVARIANT_JSON_CONTAINER_CANONICAL_FORMS)


class EncoderHygieneError(RuntimeError):
    """Raised for any condition this script treats as an outright failure
    (never silently downgraded to a skip/warning): libclang not found,
    compile_commands.json missing/unreadable, a translation unit that
    fails to parse without diagnostics, a manifest-registered header this
    script could not independently observe, or an allowlisted symbol
    whose exact expected count was not matched (which would mean the
    allowlist itself has silently gone stale -- e.g. the adapter it names
    was renamed/removed/duplicated -- and must be updated deliberately,
    not left passing for the wrong reason)."""


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

        # clang_getFile() is what proves a header this script asked to be
        # #include-d from a synthetic wrapper TU (see
        # _parse_header_as_own_tu()) was actually resolved and entered by
        # the compiler -- returning NULL means the wrapper's #include did
        # not reach that exact file at all, which this script treats as a
        # hard failure rather than silently trusting that it did.
        lib.clang_getFile.restype = ctypes.c_void_p
        lib.clang_getFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]

        # clang_getInclusions() is what makes the *resolved inclusion
        # graph* of a wrapper TU (see _audit_inclusion_graph()) directly
        # observable: it visits every file the compiler actually entered
        # while parsing, transitively, regardless of how the #include
        # that reached it was spelled (bare, "../"-relative, absolute,
        # through a symlink, or via a project-generated wrapper) -- see
        # the module docstring for why include-path/compile-flag
        # configuration alone cannot block every one of those spellings,
        # so only inspecting the compiler's own resolved graph can.
        self._inclusion_visitor_func_type = ctypes.CFUNCTYPE(
            None, ctypes.c_void_p, ctypes.POINTER(_CXSourceLocation), ctypes.c_uint, ctypes.c_void_p
        )
        lib.clang_getInclusions.restype = None
        lib.clang_getInclusions.argtypes = [
            ctypes.c_void_p, self._inclusion_visitor_func_type, ctypes.c_void_p
        ]

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
    file: str  # repo-root-relative, forward-slash path, e.g. "src/domain/RawJson.h"
    line: int
    display_name: str
    canonical_return_type: str
    usr: str

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


def _is_qjson_family(canonical_type_spelling: str) -> bool:
    return any(family in canonical_type_spelling for family in _QJSON_FAMILY) or _is_qvariant_json_container(
        canonical_type_spelling
    )


def classify(finding: Finding, counts: Counter[tuple[str, str]] | None = None) -> str:
    """Pure decision function (no I/O, directly unit-tested): 'violation'
    if this finding's canonical return type is QJson-family and either
    its (file, USR) key is not in ALLOWLIST_BY_KEY at all, or `counts`
    (when supplied) shows its total observed occurrence count does not
    exactly match that entry's expected_count; 'allowed' otherwise
    (including every non-QJson-returning declaration, which this script
    never even constructs a Finding for -- see `_collect_findings()` --
    but classify() stays total/defensive regardless).

    `counts` is optional so this function stays usable (and directly
    unit-testable) for a single ad-hoc Finding with no surrounding
    dataset; run_check()'s real pass-through always supplies it, since
    exact-count enforcement is the whole point of AllowlistEntry."""

    if not _is_qjson_family(finding.canonical_return_type):
        return "allowed"
    entry = ALLOWLIST_BY_KEY.get(finding.key())
    if entry is None:
        return "violation"
    if counts is not None and counts[finding.key()] != entry.expected_count:
        return "violation"
    return "allowed"


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


def _representative_compile_args(
    compile_commands: list[dict], sources: Sequence[Path], target_label: str
) -> list[str]:
    """Borrow one representative, sanitized compile-args list for a whole
    CMake target, taken from any one of its real source files' actual
    compile_commands.json entry.

    Every source belonging to one of this project's two library targets
    (arkham_domain_models, arkham_foundation) shares identical
    target-level -I/-D/-std flags -- verified directly against
    CMakeLists.txt: neither target uses set_source_files_properties() or
    per-file target_compile_definitions() to vary flags source-by-source
    -- so any single one of them is representative for the purpose of
    parsing a *header* belonging to that same target as its own
    standalone translation unit. This is what lets every header be
    scanned on its own, independent of whether any particular .cpp
    currently #includes it (see the module docstring)."""

    if not sources:
        raise EncoderHygieneError(
            f"No {target_label} sources were listed in its manifest -- cannot "
            "borrow representative compile flags for its headers."
        )
    commands_by_file = {Path(entry["file"]).resolve(): entry for entry in compile_commands}
    first = sources[0]
    entry = commands_by_file.get(first.resolve())
    if entry is None:
        raise EncoderHygieneError(
            f"No compile_commands.json entry found for {target_label} source {first} "
            "-- the dedicated Clang build directory may not have configured/built "
            "this target; see _configure_clang_build_dir()."
        )
    return _sanitize_compile_args(entry["command"], entry["file"])


def _parse_header_as_own_tu(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    header: Path,
    compile_args: list[str],
    sysroot_args: list[str],
) -> tuple[ctypes.c_void_p, str]:
    """Parse `header` as the sole content of its own synthetic translation
    unit: an in-memory ("unsaved") wrapper file containing exactly one
    line, `#include "<header, absolute path>"`, fed to libclang via
    clang_parseTranslationUnit2()'s existing unsaved-files parameter (no
    new ctypes struct/function bindings were needed for this -- this
    project's ctypes wrapper already declared CXUnsavedFile/the
    unsaved-files parameter, even though an earlier revision never
    exercised it).

    This is what makes every manifest-registered header independently,
    exhaustively observed regardless of what any real .cpp file currently
    #includes -- the root fix for the coverage gap a review round
    demonstrated (see module docstring).

    Returns `(tu, wrapper_filename)`: the raw `CXTranslationUnit` pointer
    (caller must dispose it) and the synthetic wrapper's own filename
    (the caller needs this to recognize and skip the wrapper's own
    "main file" entry when auditing clang_getInclusions() -- see
    _audit_inclusion_graph()). Raises EncoderHygieneError on any parse
    failure, fatal diagnostic, or if clang_getFile() cannot confirm the
    wrapper's #include actually resolved to this exact header file.
    """

    header_abs = str(header.resolve())
    wrapper_contents = f'#include "{header_abs}"\n'.encode("utf-8")
    # The wrapper's own "filename" is a synthetic, never-written path (it
    # exists only as an unsaved-file entry in libclang's in-memory
    # buffer); its extension must look like C++ so Clang selects the
    # right language, but it is otherwise never read from disk.
    wrapper_filename = f"{header_abs}.__arkham_encoder_hygiene_wrapper__.cpp"

    unsaved = _CXUnsavedFile(
        Filename=wrapper_filename.encode("utf-8"),
        Contents=wrapper_contents,
        Length=len(wrapper_contents),
    )
    unsaved_array = (_CXUnsavedFile * 1)(unsaved)

    args_bytes = [a.encode("utf-8") for a in (compile_args + sysroot_args)]
    argv = (ctypes.c_char_p * len(args_bytes))(*args_bytes)

    tu_ptr = ctypes.c_void_p()
    err = clang.lib.clang_parseTranslationUnit2(
        idx,
        wrapper_filename.encode("utf-8"),
        argv,
        len(args_bytes),
        unsaved_array,
        1,
        0x0,
        ctypes.byref(tu_ptr),
    )
    if err != 0 or not tu_ptr.value:
        raise EncoderHygieneError(
            f"libclang failed to parse a synthetic wrapper #include-ing {header} "
            f"(CXErrorCode={err}); this must never be silently skipped, since a "
            "manifest-registered header this script cannot parse standalone is a "
            "header it cannot prove anything about."
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
        clang.lib.clang_disposeTranslationUnit(tu)
        raise EncoderHygieneError(
            f"Clang reported {len(fatal_diagnostics)} error diagnostic(s) parsing "
            f"{header} standalone (as its own translation unit, #include-d from a "
            "synthetic wrapper with its own target's real compile flags); a header "
            "that does not compile cleanly on its own cannot be trusted to have a "
            "correct AST, so this check refuses to proceed rather than silently "
            "scan a partial/error-recovery AST:\n"
            + "\n".join(f"  {d}" for d in fatal_diagnostics)
        )

    observed_file = clang.lib.clang_getFile(tu, header_abs.encode("utf-8"))
    if not observed_file:
        clang.lib.clang_disposeTranslationUnit(tu)
        raise EncoderHygieneError(
            f"clang_getFile() could not confirm that {header} was actually "
            "resolved/entered by its own wrapper translation unit's #include -- "
            "this manifest entry was NOT independently observed, which this "
            "script treats as a hard failure rather than silently skipping it."
        )

    return tu, wrapper_filename


def _validate_closure_rootedness(
    entries: Sequence[Path], expected_root: Path, kind_label: str
) -> frozenset[Path]:
    """Resolve every manifest entry (following any symlink) and hard-fail
    if any of them physically lives outside `expected_root`.

    This is what stops a same-named symlink smuggled directly into a
    manifest itself (e.g. a bogus "src/domain/SneakyAlias.h" that is
    really a symlink to "src/AuthModels.h", registered so it passes the
    on-disk header-inventory check) from silently widening a closure to
    include a file it does not actually, physically contain -- every
    closure-membership/inclusion-graph check below is keyed on exactly
    the resolved (realpath) set this function returns, never on the
    original, possibly-symlinked lexical manifest path.

    Raises EncoderHygieneError (never silently drops/renames an entry)
    if any manifest entry's real path escapes `expected_root`."""

    resolved: set[Path] = set()
    violations: list[str] = []
    for entry in entries:
        real = entry.resolve()
        if not real.is_relative_to(expected_root):
            violations.append(f"  {entry} resolves to {real}, which is not inside {expected_root}")
        resolved.add(real)
    if violations:
        raise EncoderHygieneError(
            f"{kind_label} manifest entries must each physically reside inside "
            f"{expected_root} once any symlink is followed -- found entries that "
            "do not (a symlink whose real target lies elsewhere is exactly the "
            "kind of same-named-but-different-file trick this check exists to "
            "catch):\n" + "\n".join(sorted(violations))
        )
    return frozenset(resolved)


def _audit_inclusion_graph(
    clang: _LibClang,
    tu: ctypes.c_void_p,
    header: Path,
    wrapper_filename: str,
    allowed_closure: frozenset[Path],
    repo_root: Path,
    external_roots: frozenset[Path],
) -> list[str]:
    """Ask libclang for the complete resolved inclusion graph of
    `header`'s own wrapper TU (via clang_getInclusions() -- see the
    _LibClang binding above and the module docstring) and return one
    violation description per project-owned file (i.e. one whose own
    resolved, symlink-followed real path lies inside this repository's
    tracked source tree AND is not a member of any registered
    `external_roots` -- see _external_roots() -- anything outside the
    repository entirely, such as a Qt/system/toolchain header, is
    always unconditionally external) it reaches that is not a member of
    `allowed_closure`.

    A review round demonstrated that an earlier revision blanket-exempted
    EVERYTHING resolving under this script's own dedicated Clang-toolchain
    build directory (by default `<repo_root>/build-encoder-hygiene`,
    physically NESTED inside the repository) as "external", reasoning
    that CMake's FetchContent (see CMakeLists.txt's QtKeychain
    declaration) downloads/builds genuinely external third-party source
    and generated build artifacts (e.g. qkeychain_export.h) directly
    under it. That blanket exemption also silently exempted any
    genuinely PROJECT-generated header/fragment placed anywhere else
    under that same build directory (e.g. a hypothetical
    `<build-dir>/generated/Lossy.inc` reached by a real header's
    #include) -- proven by a review round that planted exactly such a
    file with a lossy `QJsonObject` declaration and showed this check
    stayed green. `external_roots` is therefore now a small, EXPLICIT
    set of genuinely-external subtrees (currently: only
    `<clang-build-dir>/_deps`, where FetchContent vendors/builds
    qtkeychain's own real third-party source) -- everything else
    resolving inside the repository, including every OTHER path under
    the build directory (AUTOMOC's own internal per-target
    `*_autogen/` directories, this script's own `generated/*.txt`
    manifests, or any future project-generated header placed anywhere
    else under it), is audited exactly like any other project file: it
    must be a member of `allowed_closure` or this is a hard violation,
    never a silent skip.

    This is independent of however the #include that reached the
    forbidden file was spelled: bare, "../"-relative, absolute, through
    a symlink, or via a project-generated wrapper header all resolve to
    the same real file identity here, which is exactly what a
    compile-flag/include-path-based defense alone cannot guarantee (see
    module docstring)."""

    violations: list[str] = []
    included: list[Path] = []

    def visitor(included_file, _inclusion_stack, _include_len, _client_data) -> None:
        if not included_file:
            return
        name = clang.to_str(clang.lib.clang_getFileName(included_file))
        if name:
            included.append(Path(name))

    cb = clang._inclusion_visitor_func_type(visitor)
    clang.lib.clang_getInclusions(tu, cb, None)

    wrapper_basename = Path(wrapper_filename).name
    for included_path in included:
        # The wrapper's own synthetic "main file" is not a real
        # #include at all -- clang_getInclusions() reports it anyway
        # (with an empty inclusion stack), so it must be recognized and
        # skipped by exact name before any closure-membership check
        # (it would otherwise be misclassified as an unregistered
        # project-owned file, since it lives right next to the real
        # header on disk, lexically).
        if included_path.name == wrapper_basename:
            continue
        real = included_path.resolve()
        if not real.is_relative_to(repo_root):
            continue  # Outside the repo entirely: Qt/system/toolchain header, always external.
        if any(real.is_relative_to(root) for root in external_roots):
            continue  # Explicitly-registered external subtree (e.g. FetchContent-vendored source/build).
        if real not in allowed_closure:
            violations.append(
                f"  {header} transitively #includes {included_path} "
                f"(resolves to {real}), which is not part of the allowed "
                "header/fragment closure for this scan -- a forbidden "
                "cross-boundary dependency or an unregistered project file, "
                "regardless of how the #include itself was spelled"
            )

    return violations


def _external_roots(clang_build_dir: Path) -> frozenset[Path]:
    """The small, EXPLICIT set of subtrees this script treats as
    genuinely external (never subject to the domain/foundation
    dependency-direction closure check), independent of the blanket
    "anything under the build directory" exemption a review round
    demonstrated was unsound (see _audit_inclusion_graph()'s own doc
    comment for the exact bypass this replaces).

    Currently this is exactly one root: `<clang-build-dir>/_deps`, where
    CMake's FetchContent (see CMakeLists.txt's QtKeychain declaration)
    downloads and builds qtkeychain's own real, genuinely third-party
    source and generates its own build artifacts (e.g.
    qkeychain_export.h) -- confirmed directly against this project's own
    FetchContent_Declare() call, never assumed. Anything else resolving
    under the build directory (this script's own `generated/*.txt`
    manifests, AUTOMOC's per-target `*_autogen/` directories, or any
    future project-generated header placed anywhere else under it) is
    deliberately NOT included here, so it remains subject to the same
    closure-membership audit as any other project file."""

    return frozenset({(clang_build_dir / "_deps").resolve()})


def _scan_headers(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    headers: Sequence[Path],
    compile_args: list[str],
    sysroot_args: list[str],
    repo_root: Path,
    external_roots: frozenset[Path],
    allowed_closure: frozenset[Path],
    seen: set[tuple[str, int, str]],
    structural_violations: list[str],
) -> list[Finding]:
    """Independently parse every header/fragment in `headers` as its own
    synthetic wrapper translation unit (see _parse_header_as_own_tu()).

    For each one:
      - Audits its complete resolved inclusion graph against
        `allowed_closure` (see _audit_inclusion_graph()), appending any
        violation found to `structural_violations` -- a hard,
        never-allowlist-able failure (see run_check()).
      - Records a Finding for every public, QJson-family-returning,
        function-like declaration whose OWN resolved location is a
        member of `allowed_closure` -- not merely "== the header
        currently being probed": a header may legitimately #include
        another member of its own closure (e.g. Decks.h #include-ing
        ValueOrError.h), and that included file's declarations must
        still be attributed to their own true source file. `seen` is a
        single (resolved file, line, USR) dedup set shared across the
        *entire* run (both the domain and foundation passes), so a
        legitimately shared/cross-included file's declarations are
        recorded exactly once no matter how many headers in its own
        closure #include it -- never once per including header."""

    findings: list[Finding] = []

    def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        if kind in _FUNCTION_LIKE_KINDS:
            filename, line = clang.cursor_file_and_line(cursor)
            if filename is not None:
                real = Path(filename).resolve()
                if real in allowed_closure:
                    access = clang.lib.clang_getCXXAccessSpecifier(cursor)
                    if access in _PUBLIC_ACCESS_SPECIFIERS:
                        result_type = clang.lib.clang_getCursorResultType(cursor)
                        canonical = clang.lib.clang_getCanonicalType(result_type)
                        spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
                        if _is_qjson_family(spelling):
                            usr = clang.to_str(clang.lib.clang_getCursorUSR(cursor))
                            dedup_key = (str(real), line, usr)
                            if dedup_key not in seen:
                                seen.add(dedup_key)
                                display = clang.to_str(clang.lib.clang_getCursorDisplayName(cursor))
                                findings.append(
                                    Finding(
                                        file=real.relative_to(repo_root).as_posix(),
                                        line=line,
                                        display_name=display,
                                        canonical_return_type=spelling,
                                        usr=usr,
                                    )
                                )
            return 1  # CXChildVisit_Continue: do not descend into the body.
        return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

    visitor_cb = clang._visitor_func_type(visitor)

    for header in headers:
        tu, wrapper_filename = _parse_header_as_own_tu(clang, idx, header, compile_args, sysroot_args)
        structural_violations.extend(
            _audit_inclusion_graph(
                clang, tu, header, wrapper_filename, allowed_closure, repo_root, external_roots
            )
        )
        root = clang.lib.clang_getTranslationUnitCursor(tu)
        clang.lib.clang_visitChildren(root, visitor_cb, None)
        clang.lib.clang_disposeTranslationUnit(tu)

    return findings


def _find_compile_command_for_source(compile_commands: list[dict], source: Path) -> dict:
    """Look up `source`'s own EXACT compile_commands.json entry, matched
    by its resolved absolute path -- never a "representative" entry
    borrowed from some other file in the same target (see
    _representative_compile_args(), which remains correct for
    *headers*: a header has no compile_commands.json entry of its own at
    all, since it is never itself compiled as a translation unit by the
    real build). A review round demonstrated that never independently
    parsing each real .cpp with its own exact compile command left every
    production source file completely unaudited by this script's
    dependency-direction policy -- this is the lookup that closes that
    gap: a manifest-registered source with no matching compile command
    is a hard failure, never a silent skip."""

    resolved = source.resolve()
    for entry in compile_commands:
        if Path(entry["file"]).resolve() == resolved:
            return entry
    raise EncoderHygieneError(
        f"No compile_commands.json entry found for source {source} -- every "
        "manifest-registered source must have actually been compiled by the "
        "dedicated Clang build directory (see _configure_clang_build_dir()); "
        "a source manifest entry with no matching compile command is a hard "
        "failure, since it means either the manifest and the real build have "
        "silently drifted apart, or this source was never really built at all."
    )


def _parse_source_as_own_tu(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    source: Path,
    compile_commands: list[dict],
    sysroot_args: list[str],
) -> ctypes.c_void_p:
    """Parse a REAL production .cpp `source` directly, on disk, as its own
    translation unit -- using its OWN exact compile_commands.json entry
    (see _find_compile_command_for_source()), never flags borrowed from
    any other file. Unlike _parse_header_as_own_tu(), no synthetic
    wrapper/unsaved-file trick is needed here: a real source file already
    exists on disk and already has its own exact compile command, so it
    is handed to libclang exactly as the real build itself compiles it.

    This is the root fix for the coverage gap a review round
    demonstrated: this script's dependency-direction policy previously
    audited only headers/fragments (via their own synthetic wrapper
    TUs) -- a real, production .cpp could #include an
    absolute/"../"-relative/symlinked/generated forbidden header,
    inherit whatever lossy encoders it declares, and compile completely
    unaudited by this script. This function is what makes every real
    production source file's own, actual, resolved inclusion graph
    independently observable too, exactly like a header's (see
    _scan_sources()/_audit_inclusion_graph()).

    Returns the raw `CXTranslationUnit` pointer (caller must dispose
    it). Raises EncoderHygieneError on any parse failure or fatal
    diagnostic, for the identical reason _parse_header_as_own_tu() does:
    a source that does not compile cleanly cannot be trusted to have a
    correct AST, so this refuses to silently scan a partial/
    error-recovery one."""

    entry = _find_compile_command_for_source(compile_commands, source)
    compile_args = _sanitize_compile_args(entry["command"], entry["file"])
    source_abs = str(source.resolve())

    args_bytes = [a.encode("utf-8") for a in (compile_args + sysroot_args)]
    argv = (ctypes.c_char_p * len(args_bytes))(*args_bytes)

    tu_ptr = ctypes.c_void_p()
    err = clang.lib.clang_parseTranslationUnit2(
        idx,
        source_abs.encode("utf-8"),
        argv,
        len(args_bytes),
        None,
        0,
        0x0,
        ctypes.byref(tu_ptr),
    )
    if err != 0 or not tu_ptr.value:
        raise EncoderHygieneError(
            f"libclang failed to parse production source {source} directly, "
            f"using its own exact compile_commands.json entry (CXErrorCode="
            f"{err}); this must never be silently skipped, since a "
            "manifest-registered source this script cannot parse is a source "
            "it cannot prove anything about."
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
        clang.lib.clang_disposeTranslationUnit(tu)
        raise EncoderHygieneError(
            f"Clang reported {len(fatal_diagnostics)} error diagnostic(s) parsing "
            f"{source} directly with its own exact compile command; a source "
            "that does not compile cleanly cannot be trusted to have a correct "
            "AST, so this check refuses to proceed rather than silently scan a "
            "partial/error-recovery AST:\n" + "\n".join(f"  {d}" for d in fatal_diagnostics)
        )

    return tu


def _scan_sources(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    sources: Sequence[Path],
    compile_commands: list[dict],
    sysroot_args: list[str],
    repo_root: Path,
    external_roots: frozenset[Path],
    allowed_closure: frozenset[Path],
) -> list[str]:
    """Independently parse every REAL production .cpp in `sources` as its
    own translation unit (see _parse_source_as_own_tu()) -- each with its
    own exact compile_commands.json entry, never a borrowed/
    "representative" one -- and audit its complete resolved inclusion
    graph against `allowed_closure`, exactly like _scan_headers() already
    does for headers/fragments (see _audit_inclusion_graph(), reused
    unchanged here: a source's own file is skipped from the graph the
    identical way a header's own wrapper "main file" entry is, by
    passing the source's own path as the self-filtering
    `wrapper_filename` argument).

    Unlike _scan_headers(), this deliberately collects NO QJson-family
    Finding objects from a source's own declarations. Every one of this
    project's 14 allowlisted encoders is declared in a header but
    *defined out-of-line* in its own .cpp (e.g.
    src/domain/RawJson.cpp's `Value::toExactQJson()`,
    src/AuthModels.cpp's `AuthenticateRequest::toJson()`) -- naively
    collecting findings for declarations whose own resolved file is the
    source itself would misclassify every legitimate encoder's own
    out-of-line *definition* as an unrecognized new violation, since
    ALLOWLIST keys each entry by its *header's* repo-relative path, not
    its .cpp's (an out-of-line definition shares its declaration's USR
    but not its declaration's file). Any genuinely *new* QJson-family
    declaration would still have to be declared in some header to be
    part of this project's public API surface at all -- an
    out-of-line-only symbol with no header declaration has no way to be
    called from another translation unit -- so header/fragment scanning
    alone remains the correct, complete surface for Finding collection;
    this function exists purely to close the inclusion-graph/
    dependency-direction audit gap for real production sources a review
    round demonstrated was completely unaudited."""

    violations: list[str] = []
    for source in sources:
        tu = _parse_source_as_own_tu(clang, idx, source, compile_commands, sysroot_args)
        try:
            violations.extend(
                _audit_inclusion_graph(
                    clang, tu, source, str(source.resolve()), allowed_closure, repo_root, external_roots
                )
            )
        finally:
            clang.lib.clang_disposeTranslationUnit(tu)
    return violations


def _read_manifest(path: Path) -> list[Path]:
    if not path.is_file():
        raise EncoderHygieneError(
            f"Manifest {path} does not exist. Run `cmake` configure (see "
            "CMakeLists.txt's arkham_write_target_header_set_manifest() calls "
            "for domain/foundation *_headers.txt -- generated from the real "
            "arkham_domain_models/arkham_foundation targets' own live CMake "
            "HEADER_SETS metadata, not a hand-authored variable -- and the "
            "ARKHAM_DOMAIN_SOURCES/ARKHAM_FOUNDATION_SOURCES/"
            "ARKHAM_DOMAIN_FRAGMENTS/ARKHAM_FOUNDATION_FRAGMENTS-driven "
            "arkham_write_path_manifest() calls for *_sources.txt/*_fragments.txt) "
            "before running this script."
        )
    lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()]
    return [Path(line) for line in lines if line]


def _configure_clang_build_dir(repo_root: Path, build_dir: Path) -> None:
    """Configure (and build both the arkham_domain_models AND
    arkham_foundation targets in) a dedicated CMake build directory using
    Clang explicitly as the compiler, independent of whatever compiler
    this project's default/main build directory happens to use
    (ubuntu-latest's default is GCC, which exposes no libclang at all).
    This is what lets this script's compile_commands.json entries be
    handed to libclang with minimal, predictable sanitization (see
    _sanitize_compile_args()) rather than guessing which of an arbitrary
    other compiler's flags libclang would accept.

    Building arkham_foundation transitively builds arkham_domain_models
    too (it links it PUBLIC), but both are named explicitly so this
    step's intent -- "both production library targets, and therefore
    both header sets' manifests, are ready to scan" -- is not left
    implicit."""

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
        ["cmake", "--build", str(build_dir), "--target", "arkham_domain_models", "arkham_foundation"],
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

    # domain_headers.txt/foundation_headers.txt are generated by
    # arkham_write_target_header_set_manifest() (see cmake/PathManifest.cmake)
    # directly from the arkham_domain_models/arkham_foundation targets' own
    # live CMake HEADER_SETS/HEADER_SET_<name> metadata -- never from a
    # hand-authored CMake variable -- so a second/late FILE_SET call adding
    # headers to either target is automatically captured here with no
    # further change to this script. *_fragments.txt lists any registered
    # non-header project source fragments (.inc/.inl/.ipp/.tpp) that
    # contribute declarations via #include but are not headers in their own
    # right; both are empty today (no such file exists in this codebase yet)
    # but are read/validated/scanned identically to headers below, so a
    # future fragment is covered by construction rather than by a suffix
    # glob guess.
    generated_dir = clang_build_dir / "generated"
    domain_headers = _read_manifest(generated_dir / "domain_headers.txt")
    domain_sources = _read_manifest(generated_dir / "domain_sources.txt")
    domain_fragments = _read_manifest(generated_dir / "domain_fragments.txt")
    foundation_headers = _read_manifest(generated_dir / "foundation_headers.txt")
    foundation_sources = _read_manifest(generated_dir / "foundation_sources.txt")
    foundation_fragments = _read_manifest(generated_dir / "foundation_fragments.txt")

    # Every manifest entry must physically live inside the root it claims
    # to belong to (see _validate_closure_rootedness()) *before* it is
    # trusted as a member of any allowed-inclusion closure below -- this
    # is what stops a symlink smuggled directly into a manifest itself
    # (rather than merely #include-d from elsewhere) from silently
    # widening a closure.
    domain_root = (repo_root / "src" / "domain").resolve()
    foundation_root = (repo_root / "src").resolve()
    domain_closure = _validate_closure_rootedness(
        domain_headers + domain_fragments, domain_root, "Domain header/fragment"
    )
    foundation_only_closure = _validate_closure_rootedness(
        foundation_headers + foundation_fragments, foundation_root, "Foundation header/fragment"
    )
    # foundation -> domain is the allowed dependency direction
    # (arkham_foundation legitimately links arkham_domain_models); the
    # reverse, domain -> foundation, is exactly the forbidden direction a
    # review round demonstrated was not actually enforced (see module
    # docstring) -- hence domain's own allowed closure below deliberately
    # excludes foundation_only_closure entirely.
    foundation_closure = domain_closure | foundation_only_closure

    libclang_path = _find_libclang()
    clang = _LibClang(libclang_path)

    is_macos = platform.system() == "Darwin"
    sysroot_args = ["-isysroot", _macos_sdk_sysroot()] if is_macos else []

    domain_args = _representative_compile_args(compile_commands, domain_sources, "domain")
    foundation_args = _representative_compile_args(compile_commands, foundation_sources, "foundation")

    idx = clang.lib.clang_createIndex(0, 0)
    if not idx:
        raise EncoderHygieneError("clang_createIndex() failed")

    # A single dedup set shared across BOTH passes below: once
    # cross-closure #includes are permitted (a header may legitimately
    # #include another member of its own closure), the same real
    # declaration can be legitimately discovered while scanning more than
    # one wrapper TU, and must only ever be recorded/counted once overall.
    seen: set[tuple[str, int, str]] = set()
    structural_violations: list[str] = []
    clang_build_dir_resolved = clang_build_dir.resolve()
    external_roots = _external_roots(clang_build_dir_resolved)

    try:
        findings = _scan_headers(
            clang,
            idx,
            domain_headers + domain_fragments,
            domain_args,
            sysroot_args,
            repo_root,
            external_roots,
            domain_closure,
            seen,
            structural_violations,
        )
        findings += _scan_headers(
            clang,
            idx,
            foundation_headers + foundation_fragments,
            foundation_args,
            sysroot_args,
            repo_root,
            external_roots,
            foundation_closure,
            seen,
            structural_violations,
        )

        # See _scan_sources()'s own doc comment for why REAL production
        # .cpp files are audited for inclusion-graph/dependency-direction
        # violations only (never for new QJson-family Finding objects):
        # a review round demonstrated this script previously never
        # independently parsed a single real source file at all --
        # domain_sources.txt/foundation_sources.txt were read only to
        # borrow one "representative" compile-args list for scanning
        # *headers* -- so a production .cpp could #include an
        # absolute/"../"-relative/symlinked/generated forbidden header,
        # inherit whatever lossy encoders it declares, and compile with
        # this check staying green. Each source here is parsed with its
        # own exact compile_commands.json entry (never a borrowed one),
        # and both source manifests are themselves generated directly
        # from arkham_domain_models'/arkham_foundation's own live CMake
        # SOURCES metadata (see arkham_write_target_source_manifest() in
        # cmake/PathManifest.cmake) -- never a hand-authored variable --
        # so this scan can never silently miss a source added via a
        # later target_sources() call either.
        structural_violations.extend(
            _scan_sources(
                clang,
                idx,
                domain_sources,
                compile_commands,
                sysroot_args,
                repo_root,
                external_roots,
                domain_closure,
            )
        )
        structural_violations.extend(
            _scan_sources(
                clang,
                idx,
                foundation_sources,
                compile_commands,
                sysroot_args,
                repo_root,
                external_roots,
                foundation_closure,
            )
        )
    finally:
        clang.lib.clang_disposeIndex(idx)

    if structural_violations:
        # A hard, never-allowlist-able failure: no AllowlistEntry can ever
        # excuse a forbidden cross-boundary #include, unlike a QJson
        # finding, which the ALLOWLIST mechanism exists specifically to
        # cover for the tiny set of deliberately-approved adapters.
        raise EncoderHygieneError(
            "Domain/foundation dependency-direction boundary violated -- "
            "these are hard structural failures and can never be allowlisted "
            "(see this script's module docstring for why include-path/"
            "compile-flag narrowing alone cannot block every #include "
            "spelling, and why only inspecting the compiler's own resolved "
            "inclusion graph can):\n" + "\n".join(structural_violations)
        )

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
        "and builds arkham_domain_models/arkham_foundation in (default: "
        "<repo-root>/build-encoder-hygiene).",
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
        "deliberate, reviewed change to one of the 14 legitimate adapters/"
        "helpers/foundation encoders -- never as a way to silence a real "
        "violation.",
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

    counts: Counter[tuple[str, str]] = Counter(f.key() for f in findings)

    if args.list:
        for f in sorted(findings, key=lambda f: (f.file, f.line)):
            print(f"{classify(f, counts):9s} {f.file}:{f.line}  {f.display_name}")
            print(f"          canonical return type: {f.canonical_return_type}")
            print(f"          USR: {f.usr}")
            print(f"          occurrences of this (file, USR): {counts[f.key()]}")
        return 0

    violations = [f for f in findings if classify(f, counts) == "violation"]

    missing_or_wrong_count_entries = [
        entry for entry in ALLOWLIST if counts[entry.key()] != entry.expected_count
    ]

    if missing_or_wrong_count_entries:
        print(
            "error: the following allowlisted canonical adapter/decode-helper/"
            "foundation-encoder symbols were not found with their exact expected "
            "occurrence count -- the allowlist itself has gone stale (renamed/"
            "removed/duplicated?) and must be updated deliberately rather than "
            "silently left passing for the wrong reason:",
            file=sys.stderr,
        )
        for entry in missing_or_wrong_count_entries:
            found = counts[entry.key()]
            print(
                f"  {entry.file}: {entry.usr} "
                f"(expected {entry.expected_count}, found {found})",
                file=sys.stderr,
            )
        return 1

    if violations:
        # De-duplicate for reporting (a genuinely duplicated declaration
        # would otherwise print once per physical occurrence, which is
        # correct but noisy for a single root-cause fix).
        seen: set[tuple[str, int, str]] = set()
        print(
            f"error: {len(violations)} public QJson-returning declaration(s) are "
            "not in the tiny explicit allowlist (by exact file + USR + "
            "occurrence count):",
            file=sys.stderr,
        )
        for v in sorted(violations, key=lambda f: (f.file, f.line)):
            report_key = (v.file, v.line, v.usr)
            if report_key in seen:
                continue
            seen.add(report_key)
            print(f"  {v.file}:{v.line}  {v.display_name}", file=sys.stderr)
            print(f"      canonical return type: {v.canonical_return_type}", file=sys.stderr)
            print(f"      USR: {v.usr}", file=sys.stderr)
        return 1

    print(
        f"Encoder hygiene: {len(findings)} public QJson-returning declaration(s) "
        "found across the domain-model and foundation header/fragment sets, all "
        f"{len(ALLOWLIST)} allowlist entries accounted for at their exact "
        "expected occurrence count, zero violations, and every header/"
        "fragment's complete resolved inclusion graph stayed within its "
        "allowed domain/foundation dependency-direction closure."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
