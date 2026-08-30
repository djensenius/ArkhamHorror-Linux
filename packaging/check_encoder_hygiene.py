#!/usr/bin/env python3
"""Prove, via real compiler-AST inspection (libclang), that no PUBLIC
declaration in this project's production headers or compiled target TUs
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
  - Every compile command owned by the application target, including
    src/main.cpp, AUTOMOC, QML type-registration/cache, and RCC-generated
    units, under the foundation/application closure. No app-local encoder
    exception exists.

Every compile-command target is reverse-inventoried against explicit CMake
target metadata. Unknown production targets fail; external, test, and
try-compile commands are excluded only by exact target records.

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
`#include "<that exact header, by absolute path>"`, once for every
distinct target/configuration compile context associated with its
manifested source set. Context identity comes from each command's CMake
object output, and its exact working directory plus command/arguments
representation is preserved.
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
resolved inclusion graph exactly like a header's.

A further review round demonstrated that this doc comment's earlier
claim -- "an out-of-line-only symbol has no way to be called from
another translation unit" -- was simply false: any TU can write its own
`extern` forward declaration of a namespace-scope, externally-linked
symbol and call it, whether or not that symbol also happens to have a
declaration in some header. Source scanning therefore now ALSO collects
QJson-family Findings for genuinely NEW, source-only declarations (see
_scan_sources()): for every function-like/constructor declaration found
in the source or a project header visible in that exact TU, this script asks libclang for that
declaration's *canonical* cursor (clang_getCanonicalCursor(), which
always resolves to the FIRST declaration of that entity anywhere in the
TU). An out-of-line definition is skipped only when the exact canonical
identity (resolved file, line, USR, signature, access, and shape) was
actually observed in unified seen data. A macro-conditional declaration
visible only under this source's preprocessor state is therefore
classified rather than skipped merely because its path is registered.
If the canonical cursor's own file is the .cpp itself,
this is a genuinely new declaration -- and it is recorded as a Finding
(which can never match any ALLOWLIST entry, since every entry is keyed
to a header path) if, and only if, it also has genuinely external
linkage (clang_getCursorLinkage() == CXLinkage_External): a `static`- or
anonymous-namespace-scoped helper has internal/unique-external linkage
and categorically cannot be referenced from another translation unit at
all, so it is correctly never flagged, no matter its own nominal access
specifier.

A further review round also demonstrated that this script's shape check
inspected only a declaration's own RESULT type, never (a) a non-const
QJson-family reference/pointer OUTPUT or INOUT parameter (e.g. a public/
friend `void encode(QJsonObject &out)`, or an equivalent constructor),
which is exactly as capable of smuggling a lossy value out of an
otherwise return-type-clean signature as a lossy return type is, nor (b)
encoder-shaped members made newly accessible purely through public/
protected inheritance or a using-declaration, with no new textual
declaration of their own at all (e.g. a new struct publicly deriving
from an already-allowlisted AuthenticateRequest, or privately deriving
it and using-declaring its toJson() back to public). This script now
also inspects every function-like/constructor declaration's own
parameters for a non-const QJson-family reference/pointer (see
_is_encoder_shaped()), and separately walks every class/struct/
class-template definition's own base-specifiers and using-declarations
(see _inherited_and_reexported_encoders()) to discover exactly this kind
of newly-exposed-without-a-new-declaration member, recursively through a
multi-level inheritance chain, attributing any resulting Finding to the
EXPOSING class's own file/line (which, keyed against ALLOWLIST_BY_KEY,
can never match the original declaring class's own allowlist entry, so
it fails closed as a violation with zero change to the allowlist
mechanism itself).


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
    functions, friend functions, conversion operators, function
    templates, constructors) whose *own* location (after macro
    expansion) is exactly the one header currently being probed, this
    script asks libclang for the *canonical* result type (i.e. with
    every typedef/using-alias/decltype/auto/template parameter already
    resolved to its underlying real type by the compiler itself) AND
    inspects every parameter recursively through canonical references,
    pointers, aliases, and template wrappers down to the ultimate
    QJson-family object's actual cv-qualification -- an output/inout parameter is
    exactly as capable of smuggling a lossy value out as a lossy return
    type is (see _is_encoder_shaped()) -- together with its USR (Unified
    Symbol Resolution -- a stable, fully qualified,
    signature-and-overload-aware identity Clang computes for every
    declaration; see https://clang.llvm.org/docs/USRs.html), access
    specifier, and exact source file.
  - For every class/struct/class-template definition and public/protected
    type alias whose own location
    is likewise exactly the header currently being probed, this script
    additionally walks its base-specifiers and using-declarations (see
    _inherited_and_reexported_encoders()) to discover any encoder-shaped
    member function made newly, transitively accessible through public/
    protected/dependent inheritance, an alias, or a using-declaration alone, with no new
    textual declaration of its own -- attributing the resulting Finding
    to the EXPOSING class's own file/line rather than the original
    declaring class's file, so it cannot masquerade as an
    already-audited, already-allowlisted symbol.
  - A declaration is a *violation* if its canonical return type (or, for
    an output/inout parameter, that parameter's own canonical pointee
    type) is in the QJson family (QJsonObject/QJsonArray/QJsonValue,
    with or without a reference/pointer/const qualifier) and its (file,
    USR) pair, counted by *exact occurrence count*, is not one of the
    ALLOWLIST entries below. There is no general "looks like a decode
    helper" heuristic
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
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Sequence


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


# Cursor kinds this script cares about (see clang-c/Index.h). Every value
# below was independently, empirically re-confirmed against this
# project's own pinned libclang via clang_getCursorKindSpelling() (which
# reports Clang's own name for a given integer kind) rather than trusted
# from memory/documentation alone -- a review round demonstrated exactly
# this kind of unverified-constant risk was real:
# _CXCursor_StructDecl was previously (wrongly) defined as 3, which is
# actually CXCursor_UnionDecl; the correct value is 2. That mistake had
# no observable effect before this round only because nothing in this
# script actually consumed _CXCursor_StructDecl until the inheritance-
# exposure walk added below started doing so.
_CXCursor_StructDecl = 2
_CXCursor_ClassDecl = 4
_CXCursor_ClassTemplate = 31
_CXCursor_FunctionDecl = 8
_CXCursor_ParmDecl = 10
_CXCursor_CXXMethod = 21
_CXCursor_Namespace = 22
_CXCursor_ConversionFunction = 26
_CXCursor_FunctionTemplate = 30
_CXCursor_Constructor = 24
_CXCursor_TypedefDecl = 20
_CXCursor_CXXBaseSpecifier = 44
_CXCursor_UsingDeclaration = 35
_CXCursor_TypeAliasDecl = 36
_CXCursor_OverloadedDeclRef = 49
_CXCursor_NoDeclFound = 71
_CXCursor_AnnotateAttr = 406
_CXCursor_FriendDecl = 603
_CXCursor_TemplateTypeParameter = 27

# The "shape-eligible" record/class-template kinds this script walks for
# base-specifier/using-declaration inheritance exposure (see
# _inherited_and_reexported_encoders()).
_RECORD_LIKE_KINDS = frozenset({_CXCursor_StructDecl, _CXCursor_ClassDecl, _CXCursor_ClassTemplate})

# Function-like declaration kinds whose own RESULT type is inspected for
# QJson-family shape (see _is_encoder_shaped()). Constructors are
# deliberately excluded here -- clang_getCursorResultType() has no
# meaningful "return type" concept for a constructor -- but ARE included
# in _OUTPARAM_CHECKED_KINDS below, since a constructor can still take a
# non-const QJson-family output/inout reference or pointer parameter
# exactly like an ordinary function can.
_FUNCTION_LIKE_KINDS = frozenset(
    {
        _CXCursor_FunctionDecl,
        _CXCursor_CXXMethod,
        _CXCursor_ConversionFunction,
        _CXCursor_FunctionTemplate,
    }
)

# Every declaration kind whose PARAMETERS are inspected for a non-const
# QJson-family output/inout reference or pointer (see
# _is_encoder_shaped()) -- a review round demonstrated a public/friend
# `void encode(QJsonObject &out)` (or an equivalent constructor) was a
# real, undetected bypass of the return-type-only check.
_OUTPARAM_CHECKED_KINDS = _FUNCTION_LIKE_KINDS | {_CXCursor_Constructor}

# CX_CXXAccessSpecifier (see clang-c/Index.h): 0 is "invalid" -- reported
# for cursors that are not class members at all (ordinary namespace-scope
# free functions, and friend declarations regardless of which access
# section they are textually written under -- both empirically
# confirmed), which are public by definition; 1 is explicitly public; 2
# is protected; 3 is private.
_CX_CXXInvalidAccessSpecifier = 0
_CX_CXXPublic = 1
_CX_CXXProtected = 2
_CX_CXXPrivate = 3
_PUBLIC_ACCESS_SPECIFIERS = frozenset({_CX_CXXInvalidAccessSpecifier, _CX_CXXPublic})

# A public OR protected base class/using-declaration still exposes its
# encoder-shaped members to the outside world (directly for a public
# base, or to any further subclass -- which can then re-expose it
# publicly with a single additional using-declaration or public
# inheritance step of its own -- for a protected one); only a PRIVATE
# base/using-declaration genuinely blocks further exposure. See
# _inherited_and_reexported_encoders().
_INHERITABLE_ACCESS_SPECIFIERS = frozenset({_CX_CXXPublic, _CX_CXXProtected})

# CXTypeKind values (see clang-c/Index.h) this script's output/inout
# parameter check needs to recognize a non-const reference/pointer,
# empirically re-confirmed the same way as the cursor kinds above.
_CXType_Pointer = 101
_CXType_LValueReference = 103
_CXType_RValueReference = 104
_CXType_FunctionNoProto = 110
_CXType_FunctionProto = 111
_CXType_MemberPointer = 117
_REFERENCE_OR_POINTER_TYPE_KINDS = frozenset(
    {_CXType_Pointer, _CXType_LValueReference, _CXType_RValueReference}
)
_FUNCTION_TYPE_KINDS = frozenset({_CXType_FunctionNoProto, _CXType_FunctionProto})

# CXLinkageKind values (see clang-c/Index.h): only a declaration with
# genuinely external linkage can be referenced (e.g. via an ad-hoc
# `extern` forward declaration) from another translation unit at all --
# Internal (an explicit `static`) and UniqueExternal (anonymous-
# namespace-scoped) declarations cannot be, no matter their own access
# specifier, and must not be misclassified as a new, externally-callable
# public-API bypass when found only in a source file with no header
# declaration (see _scan_sources()'s new declaration-classification
# logic, added to close exactly the source-only-declaration bypass a
# review round demonstrated).
_CXLinkage_Internal = 2
_CXLinkage_UniqueExternal = 3
_CXLinkage_External = 4



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

        # --- New bindings added to close the source-only-declaration,
        # output-parameter, and inheritance/using-declaration exposure
        # bypasses a review round demonstrated (see _is_encoder_shaped(),
        # _inherited_and_reexported_encoders(), and _scan_sources()'s new
        # declaration-classification logic). Every one of these was
        # empirically verified against this project's own pinned
        # libclang (parsing real, on-disk project sources/synthetic
        # fixtures and inspecting the exact cursors/types produced)
        # before being relied upon here, not merely assumed correct from
        # documentation.

        # clang_getCanonicalCursor(): resolves any declaration cursor to
        # the FIRST declaration of that same entity anywhere in the
        # translation unit -- for a member declared in a header and
        # defined out-of-line in a .cpp, this is always the header
        # declaration, regardless of which of the two cursors (the
        # header declaration or the .cpp's own out-of-line definition)
        # this is called on; for an entity with no earlier declaration
        # anywhere (a genuinely new, source-only declaration), this
        # resolves to itself. This is exactly the mechanism
        # _scan_sources() now uses to distinguish "this source cursor is
        # merely the out-of-line definition of an already
        # header-declared (and therefore already counted by
        # _scan_headers()) symbol" from "this is a brand new declaration
        # with no header declaration anywhere," without needing to
        # cross-reference the allowlist or any manifest by hand.
        lib.clang_getCanonicalCursor.restype = _CXCursor
        lib.clang_getCanonicalCursor.argtypes = [_CXCursor]

        # clang_getCursorLinkage(): see _CXLinkage_* above -- used to
        # exclude `static`/anonymous-namespace-scoped source-only
        # declarations (CXLinkage_Internal/CXLinkage_UniqueExternal) from
        # _scan_sources()'s new-declaration classification, since neither
        # can be referenced from another translation unit at all, unlike
        # an ordinary externally-linked (CXLinkage_External) one.
        lib.clang_getCursorLinkage.restype = ctypes.c_int
        lib.clang_getCursorLinkage.argtypes = [_CXCursor]

        # clang_getCursorType(): the (non-canonical) type of an arbitrary
        # cursor -- used on each function-like declaration's own
        # parameter (ParmDecl) cursors, obtained via
        # clang_Cursor_getArgument() below, to inspect their type for the
        # output-parameter check.
        lib.clang_getCursorType.restype = _CXType
        lib.clang_getCursorType.argtypes = [_CXCursor]

        lib.clang_Type_getNumTemplateArguments.restype = ctypes.c_int
        lib.clang_Type_getNumTemplateArguments.argtypes = [_CXType]
        lib.clang_Type_getTemplateArgumentAsType.restype = _CXType
        lib.clang_Type_getTemplateArgumentAsType.argtypes = [_CXType, ctypes.c_uint]
        lib.clang_getNumArgTypes.restype = ctypes.c_int
        lib.clang_getNumArgTypes.argtypes = [_CXType]
        lib.clang_getArgType.restype = _CXType
        lib.clang_getArgType.argtypes = [_CXType, ctypes.c_uint]
        lib.clang_getResultType.restype = _CXType
        lib.clang_getResultType.argtypes = [_CXType]

        # clang_Cursor_getNumArguments()/clang_Cursor_getArgument(): work
        # directly on a function-like DECLARATION cursor (not merely a
        # call-expression, which is the more commonly documented use),
        # empirically confirmed -- this is what makes each parameter's
        # own type independently inspectable for the output-parameter
        # check in _is_encoder_shaped(), without needing to parse the
        # cursor's own display name/spelling text.
        lib.clang_Cursor_getNumArguments.restype = ctypes.c_int
        lib.clang_Cursor_getNumArguments.argtypes = [_CXCursor]
        lib.clang_Cursor_getArgument.restype = _CXCursor
        lib.clang_Cursor_getArgument.argtypes = [_CXCursor, ctypes.c_uint]

        # clang_getPointeeType()/clang_isConstQualifiedType(): given a
        # reference or pointer CXType, resolve the type it refers to and
        # ask whether that pointee is const-qualified -- a const pointee
        # (`const QJsonObject &`/`const QJsonObject *`) is an ordinary
        # input parameter; a non-const one (`QJsonObject &`/
        # `QJsonObject *`) is a genuine output/inout parameter capable of
        # smuggling a lossy QJson-family value out of an otherwise
        # return-type-clean function, exactly like a lossy return type
        # would.
        lib.clang_getPointeeType.restype = _CXType
        lib.clang_getPointeeType.argtypes = [_CXType]
        lib.clang_isConstQualifiedType.restype = ctypes.c_uint
        lib.clang_isConstQualifiedType.argtypes = [_CXType]

        # clang_isCursorDefinition(): distinguishes a declaration-only
        # cursor from one that also carries a body/definition -- used
        # only for readability/defensive assertions around the
        # canonical-cursor logic above, not itself load-bearing for any
        # pass/fail decision.
        lib.clang_isCursorDefinition.restype = ctypes.c_uint
        lib.clang_isCursorDefinition.argtypes = [_CXCursor]

        # clang_getTypeDeclaration(): given a CXType naming a class/
        # struct (e.g. a base-specifier's own type), resolve the cursor
        # that actually declares/defines that class -- this is what lets
        # _inherited_and_reexported_encoders() walk into a base class's
        # own member declarations (which may live in an entirely
        # different header from the derived class currently being
        # scanned) to discover encoder-shaped members it makes
        # accessible to the derived class.
        lib.clang_getTypeDeclaration.restype = _CXCursor
        lib.clang_getTypeDeclaration.argtypes = [_CXType]
        lib.clang_getSpecializedCursorTemplate.restype = _CXCursor
        lib.clang_getSpecializedCursorTemplate.argtypes = [_CXCursor]

        # clang_getNumOverloadedDecls()/clang_getOverloadedDecl(): a
        # using-declaration's own OverloadedDeclRef child cursor (kind
        # _CXCursor_OverloadedDeclRef, empirically confirmed -- present
        # even when the using-declaration resolves to exactly one target,
        # not merely for genuine overload sets) is how libclang exposes
        # the actual declaration(s) a using-declaration re-exports;
        # clang_getCursorReferenced() on that child (or on the
        # using-declaration cursor itself) does NOT resolve to a useful
        # cursor for this case, empirically confirmed -- only this pair
        # does.
        lib.clang_getNumOverloadedDecls.restype = ctypes.c_uint
        lib.clang_getNumOverloadedDecls.argtypes = [_CXCursor]
        lib.clang_getOverloadedDecl.restype = _CXCursor
        lib.clang_getOverloadedDecl.argtypes = [_CXCursor, ctypes.c_uint]
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

        # clang_getLocation()/clang_Location_isInSystemHeader(): used
        # together by _audit_inclusion_graph() to ask the COMPILER
        # ITSELF (not a lexical "is this path outside the repo root?"
        # guess) whether an included file was reached via a genuine
        # system/toolchain include path (`-isystem`/`-iframework`/the
        # macOS `-isysroot` SDK root) -- this is real, authoritative
        # Clang classification, confirmed empirically against this
        # project's own real compile commands: Qt's own include
        # directories are passed via `-isystem`/`-iframework` (CMake
        # automatically marks an IMPORTED target's own
        # INTERFACE_INCLUDE_DIRECTORIES as SYSTEM for consuming
        # targets), so every Qt/macOS-SDK header is correctly classified
        # here, while qtkeychain's own FetchContent-vendored headers are
        # passed via an ordinary `-I` (confirmed directly against this
        # project's own real compile commands too) and are therefore
        # correctly NOT classified as a system header by Clang -- hence
        # `external_roots` (see _external_roots(), now itself sourced
        # from real FetchContent package metadata rather than a lexical
        # `_deps` guess) remains a necessary, SEPARATE classification for
        # genuinely-external dependency code that is not compiler-
        # system-classified. A review round demonstrated the previous
        # blanket "anything outside repo_root is external" rule
        # exempted ANY generated file placed outside the repository
        # entirely, with no real system/dependency justification at
        # all -- this pair of bindings is what replaces that lexical
        # guess with the compiler's own, authoritative answer.
        lib.clang_getLocation.restype = _CXSourceLocation
        lib.clang_getLocation.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint]
        lib.clang_Location_isInSystemHeader.restype = ctypes.c_uint
        lib.clang_Location_isInSystemHeader.argtypes = [_CXSourceLocation]

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
    # Normally the plain canonical return-type spelling; for an
    # output/inout-parameter violation (see _is_encoder_shaped()) this is
    # instead a human-readable description of the offending parameter
    # (still containing the actual QJson-family type name, so
    # classify()'s substring-based _is_qjson_family() check keeps working
    # unmodified either way).
    canonical_return_type: str
    usr: str

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


def _is_qjson_family(canonical_type_spelling: str) -> bool:
    return any(family in canonical_type_spelling for family in _QJSON_FAMILY) or _is_qvariant_json_container(
        canonical_type_spelling
    )


_MAX_TYPE_DEPTH = 32

_OUTPUT_CAPABLE_TEMPLATE_PREFIXES = (
    "std::shared_ptr<",
    "std::unique_ptr<",
    "std::weak_ptr<",
    "std::reference_wrapper<",
    "std::span<",
    "QSharedPointer<",
    "QWeakPointer<",
    "QPointer<",
    "QScopedPointer<",
    "QSharedDataPointer<",
    "QExplicitlySharedDataPointer<",
)
_CALLABLE_TEMPLATE_PREFIXES = (
    "std::function<",
    "std::move_only_function<",
)


def _normalized_type_spelling(spelling: str) -> str:
    return "".join(spelling.split()).removeprefix("const")


def _has_template_prefix(spelling: str, prefixes: Sequence[str]) -> bool:
    normalized = _normalized_type_spelling(spelling)
    return any(
        normalized.startswith(prefix)
        or normalized.startswith(prefix.rsplit("::", 1)[-1])
        for prefix in prefixes
    )


def _is_dependent_type(clang: "_LibClang", value_type: "_CXType") -> bool:
    canonical = clang.lib.clang_getCanonicalType(value_type)
    spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
    declaration = clang.lib.clang_getTypeDeclaration(canonical)
    return (
        "type-parameter-" in spelling
        or spelling in ("", "<dependent type>")
        or clang.lib.clang_getCursorKind(declaration)
        == _CXCursor_TemplateTypeParameter
    )


def _json_payload_path(
    clang: "_LibClang", value_type: "_CXType", depth: int = 0
) -> str | None:
    """Find JSON anywhere in a value graph, intentionally ignoring cv.
    Used only for callback/signal arguments whose direction is outward."""

    if depth > _MAX_TYPE_DEPTH:
        raise EncoderHygieneError(
            f"Callback JSON payload walk exceeded {_MAX_TYPE_DEPTH} levels"
        )
    canonical = clang.lib.clang_getCanonicalType(value_type)
    if canonical.kind in _REFERENCE_OR_POINTER_TYPE_KINDS or canonical.kind == _CXType_MemberPointer:
        pointee = clang.lib.clang_getPointeeType(canonical)
        if pointee.kind != 0:
            return _json_payload_path(clang, pointee, depth + 1)
    spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
    if _is_qjson_family(spelling):
        return spelling
    argument_count = clang.lib.clang_Type_getNumTemplateArguments(canonical)
    for index in range(max(argument_count, 0)):
        found = _json_payload_path(
            clang,
            clang.lib.clang_Type_getTemplateArgumentAsType(canonical, index),
            depth + 1,
        )
        if found is not None:
            return found
    return None


def _callback_json_path(
    clang: "_LibClang", value_type: "_CXType", depth: int = 0
) -> str | None:
    if depth > _MAX_TYPE_DEPTH:
        raise EncoderHygieneError(
            f"Callback type walk exceeded {_MAX_TYPE_DEPTH} levels"
        )
    original_spelling = clang.to_str(
        clang.lib.clang_getTypeSpelling(value_type)
    )
    canonical = clang.lib.clang_getCanonicalType(value_type)
    if canonical.kind in _REFERENCE_OR_POINTER_TYPE_KINDS or canonical.kind == _CXType_MemberPointer:
        pointee = clang.lib.clang_getPointeeType(canonical)
        if pointee.kind != 0:
            return _callback_json_path(clang, pointee, depth + 1)
        return None
    if canonical.kind in _FUNCTION_TYPE_KINDS:
        argument_count = clang.lib.clang_getNumArgTypes(canonical)
        for index in range(max(argument_count, 0)):
            found = _json_payload_path(
                clang, clang.lib.clang_getArgType(canonical, index), depth + 1
            )
            if found is not None:
                return found
        return None

    spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
    template_count = clang.lib.clang_Type_getNumTemplateArguments(canonical)
    if _has_template_prefix(
        spelling, _CALLABLE_TEMPLATE_PREFIXES
    ) or _has_template_prefix(
        original_spelling, _CALLABLE_TEMPLATE_PREFIXES
    ):
        if template_count < 1:
            return "QJsonObject-capable unresolved callable wrapper"
        callable_type = clang.lib.clang_Type_getTemplateArgumentAsType(canonical, 0)
        if _is_dependent_type(clang, callable_type):
            return "QJsonObject-capable unresolved callable wrapper"
        return _callback_json_path(clang, callable_type, depth + 1)

    # Optional/container/alias wrappers around a callable retain the
    # callback's outward direction.
    for index in range(max(template_count, 0)):
        found = _callback_json_path(
            clang,
            clang.lib.clang_Type_getTemplateArgumentAsType(canonical, index),
            depth + 1,
        )
        if found is not None:
            return found
    return None


def _cursor_has_annotation(
    clang: "_LibClang", cursor: "_CXCursor", annotation: str
) -> bool:
    found = False

    def visit(child: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        nonlocal found
        if (
            clang.lib.clang_getCursorKind(child) == _CXCursor_AnnotateAttr
            and clang.to_str(clang.lib.clang_getCursorDisplayName(child))
            == annotation
        ):
            found = True
        return 1

    callback = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(cursor, callback, None)
    return found


def _parameter_cursors(
    clang: "_LibClang", cursor: "_CXCursor"
) -> list[_CXCursor]:
    count = clang.lib.clang_Cursor_getNumArguments(cursor)
    if count >= 0:
        return [
            clang.lib.clang_Cursor_getArgument(cursor, index)
            for index in range(count)
        ]
    parameters: list[_CXCursor] = []

    def visit(child: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        if clang.lib.clang_getCursorKind(child) == _CXCursor_ParmDecl:
            parameters.append(child)
        return 1

    callback = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(cursor, callback, None)
    return parameters


def _mutable_json_path(
    clang: "_LibClang",
    value_type: "_CXType",
    *,
    directly_mutable: bool,
    depth: int = 0,
) -> str | None:
    """Return the ultimate mutable JSON-family type reachable through
    references, pointers, aliases, and template wrappers.

    Constness on a pointer object is intentionally not treated as
    constness on the object it points at. Thus ``QJsonObject * const &``
    is mutable output, while ``const QJsonObject * const &`` is input.
    For a template wrapper reached through a mutable reference/pointer,
    direct JSON-valued template arguments are mutable; pointer-valued
    arguments are followed even through a const wrapper because their
    pointee has independent cv-qualification."""

    if depth > _MAX_TYPE_DEPTH:
        raise EncoderHygieneError(
            f"JSON output-parameter type walk exceeded {_MAX_TYPE_DEPTH} levels"
        )

    original_spelling = clang.to_str(
        clang.lib.clang_getTypeSpelling(value_type)
    )
    canonical = clang.lib.clang_getCanonicalType(value_type)
    kind = canonical.kind
    if kind in _REFERENCE_OR_POINTER_TYPE_KINDS:
        pointee = clang.lib.clang_getPointeeType(canonical)
        if kind == _CXType_Pointer:
            # Top-level cv applies to the pointer, never its pointee.
            return _mutable_json_path(
                clang, pointee, directly_mutable=True, depth=depth + 1
            )
        return _mutable_json_path(
            clang,
            pointee,
            directly_mutable=not bool(clang.lib.clang_isConstQualifiedType(pointee)),
            depth=depth + 1,
        )

    spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical))
    template_arg_count = clang.lib.clang_Type_getNumTemplateArguments(canonical)

    # QVariantMap/List/Hash are themselves prohibited JSON containers
    # despite having template arguments in canonical form.
    if _is_qvariant_json_container(spelling):
        return spelling if directly_mutable and not clang.lib.clang_isConstQualifiedType(canonical) else None

    if template_arg_count < 0:
        if any(
            _re.fullmatch(rf"(?:const\s+)?{family}(?:\s+const)?", spelling.strip())
            for family in _QJSON_FAMILY
        ):
            if directly_mutable and not clang.lib.clang_isConstQualifiedType(canonical):
                return spelling
        return None

    if _has_template_prefix(
        spelling, _OUTPUT_CAPABLE_TEMPLATE_PREFIXES
    ) or _has_template_prefix(
        original_spelling, _OUTPUT_CAPABLE_TEMPLATE_PREFIXES
    ):
        if template_arg_count < 1:
            return "QJsonObject-capable unresolved output wrapper"
        pointee_argument = clang.lib.clang_Type_getTemplateArgumentAsType(
            canonical, 0
        )
        if _is_dependent_type(clang, pointee_argument):
            return "QJsonObject-capable unresolved output wrapper"
        return _mutable_json_path(
            clang, pointee_argument, directly_mutable=True, depth=depth + 1
        )

    wrapper_mutable = directly_mutable and not bool(
        clang.lib.clang_isConstQualifiedType(canonical)
    )
    for index in range(template_arg_count):
        argument = clang.lib.clang_Type_getTemplateArgumentAsType(canonical, index)
        if argument.kind == 0:
            continue
        argument_canonical = clang.lib.clang_getCanonicalType(argument)
        argument_is_indirect = argument_canonical.kind in _REFERENCE_OR_POINTER_TYPE_KINDS
        found = _mutable_json_path(
            clang,
            argument,
            directly_mutable=wrapper_mutable or argument_is_indirect,
            depth=depth + 1,
        )
        if found is not None:
            return found
    return None


def _is_encoder_shaped(clang: "_LibClang", cursor: "_CXCursor", kind: int) -> tuple[bool, str]:
    """True if this function-like/constructor declaration is
    "encoder-shaped": either its own canonical RESULT type (checked only
    for kinds in _FUNCTION_LIKE_KINDS -- a constructor has no meaningful
    return type) or any of its non-const-qualified reference/pointer
    PARAMETER types (checked for every kind in _OUTPARAM_CHECKED_KINDS)
    is in the QJson/QVariant-JSON-container family (see
    _is_qjson_family()).

    Returns (True, a human-readable description of the offending return
    type or parameter) when encoder-shaped, else (False, the plain
    canonical return-type spelling).

    A review round demonstrated the original return-type-only check
    missed a public/friend `void encode(QJsonObject &out)` (or an
    equivalent constructor writing through a non-const reference/pointer
    parameter) entirely: passing a non-const reference/pointer to a
    QJson-family type is exactly as capable of smuggling a lossy value
    out of an otherwise clean-looking signature as a lossy return type
    is. A CONST-qualified reference/pointer parameter (an ordinary INPUT
    parameter -- e.g. every JsonDecode.h decode helper takes one) must
    never be flagged, and is not: only a non-const pointee triggers this
    check."""

    result_type = clang.lib.clang_getCursorResultType(cursor)
    canonical_result = clang.lib.clang_getCanonicalType(result_type)
    result_spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical_result))
    if kind in _FUNCTION_LIKE_KINDS and _is_qjson_family(result_spelling):
        return True, result_spelling

    if kind in _OUTPARAM_CHECKED_KINDS:
        is_qt_signal = kind == _CXCursor_CXXMethod and _cursor_has_annotation(
            clang, cursor, "qt_signal"
        )
        for arg_index, parm_cursor in enumerate(_parameter_cursors(clang, cursor)):
            parm_type = clang.lib.clang_getCursorType(parm_cursor)
            canonical_parm = clang.lib.clang_getCanonicalType(parm_type)
            parm_spelling = clang.to_str(
                clang.lib.clang_getTypeSpelling(canonical_parm)
            )

            if is_qt_signal:
                signal_payload = _json_payload_path(clang, canonical_parm)
                if signal_payload is not None:
                    return (
                        True,
                        f"Qt signal output parameter #{arg_index}: "
                        f"{parm_spelling} (outbound {signal_payload})",
                    )

            callback_payload = _callback_json_path(clang, canonical_parm)
            if callback_payload is not None:
                return (
                    True,
                    f"callback output parameter #{arg_index}: "
                    f"{parm_spelling} (outbound {callback_payload})",
                )

            mutable_json = _mutable_json_path(
                clang,
                canonical_parm,
                directly_mutable=canonical_parm.kind
                in _REFERENCE_OR_POINTER_TYPE_KINDS,
            )
            if mutable_json is not None:
                return (
                    True,
                    "non-const output/inout parameter "
                    f"#{arg_index}: {parm_spelling} (mutable {mutable_json})",
                )

    return False, result_spelling


def _resolve_using_declaration_targets(clang: "_LibClang", using_cursor: "_CXCursor") -> list:
    """A using-declaration's actual re-exported target(s) are exposed by
    libclang only via its own OverloadedDeclRef child cursor's
    clang_getNumOverloadedDecls()/clang_getOverloadedDecl() pair --
    empirically confirmed present even when the using-declaration
    resolves to exactly one, non-overloaded target, not merely for a
    genuine overload set. clang_getCursorReferenced() on either the
    using-declaration cursor itself or its OverloadedDeclRef child does
    NOT resolve usefully for this case -- only this pair does."""

    targets: list = []

    def visit(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        if clang.lib.clang_getCursorKind(cursor) == _CXCursor_OverloadedDeclRef:
            count = clang.lib.clang_getNumOverloadedDecls(cursor)
            for i in range(count):
                targets.append(clang.lib.clang_getOverloadedDecl(cursor, i))
        return 2  # CXChildVisit_Recurse

    cb = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(using_cursor, cb, None)
    return targets


_MAX_INHERITANCE_DEPTH = 16


@dataclass(frozen=True)
class _Exposure:
    source_cursor: _CXCursor | None
    attribution_cursor: _CXCursor
    reason: str


def _record_member_candidates(
    clang: "_LibClang", record_cursor: "_CXCursor"
) -> list[_CXCursor]:
    members: list[_CXCursor] = []

    def visit(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if kind in _FUNCTION_LIKE_KINDS and access in _INHERITABLE_ACCESS_SPECIFIERS:
            members.append(cursor)
        return 1

    callback = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(record_cursor, callback, None)
    return members


def _alias_reexported_encoders(
    clang: "_LibClang", alias_cursor: "_CXCursor"
) -> list[_Exposure]:
    """Resolve public/protected aliases, including specializations of
    dependent wrapper templates, and attribute any exposed member back
    to the alias declaration."""

    alias_type = clang.lib.clang_getCanonicalType(
        clang.lib.clang_getCursorType(alias_cursor)
    )
    target_decl = clang.lib.clang_getTypeDeclaration(alias_type)
    if clang.lib.clang_getCursorKind(target_decl) in (0, _CXCursor_NoDeclFound):
        return []

    exposed = [
        _Exposure(member, alias_cursor, "public/protected type alias")
        for member in _record_member_candidates(clang, target_decl)
    ]

    primary = clang.lib.clang_getSpecializedCursorTemplate(target_decl)
    if clang.lib.clang_getCursorKind(primary) != _CXCursor_ClassTemplate:
        for inherited in _inherited_and_reexported_encoders(clang, target_decl):
            exposed.append(
                _Exposure(
                    inherited.source_cursor,
                    alias_cursor,
                    f"type alias of {inherited.reason}",
                )
            )
        return exposed

    argument_count = clang.lib.clang_Type_getNumTemplateArguments(alias_type)
    arguments = [
        clang.lib.clang_Type_getTemplateArgumentAsType(alias_type, index)
        for index in range(max(argument_count, 0))
    ]
    parameter_names: list[str] = []
    bases: list[_CXCursor] = []
    using_names: list[str] = []

    def visit_primary(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if kind == _CXCursor_TemplateTypeParameter:
            parameter_names.append(
                clang.to_str(clang.lib.clang_getCursorDisplayName(cursor))
            )
        elif kind == _CXCursor_CXXBaseSpecifier and access in _INHERITABLE_ACCESS_SPECIFIERS:
            bases.append(cursor)
        elif kind == _CXCursor_UsingDeclaration and access in _INHERITABLE_ACCESS_SPECIFIERS:
            using_names.append(
                clang.to_str(clang.lib.clang_getCursorDisplayName(cursor))
            )
        return 1

    callback = clang._visitor_func_type(visit_primary)
    clang.lib.clang_visitChildren(primary, callback, None)

    argument_records: list[_CXCursor] = []
    for argument in arguments:
        declaration = clang.lib.clang_getTypeDeclaration(
            clang.lib.clang_getCanonicalType(argument)
        )
        if clang.lib.clang_getCursorKind(declaration) not in (
            0,
            _CXCursor_NoDeclFound,
        ):
            argument_records.append(declaration)

    for base in bases:
        base_spelling = clang.to_str(
            clang.lib.clang_getTypeSpelling(
                clang.lib.clang_getCursorType(base)
            )
        )
        if base_spelling in parameter_names:
            index = parameter_names.index(base_spelling)
            if index >= len(arguments):
                exposed.append(
                    _Exposure(
                        None,
                        alias_cursor,
                        "unresolved public/protected alias template base",
                    )
                )
                continue
            base_decl = clang.lib.clang_getTypeDeclaration(
                clang.lib.clang_getCanonicalType(arguments[index])
            )
        else:
            # Concrete implementation bases of a class-template
            # specialization (e.g. libstdc++ shared_ptr internals) do not
            # receive one of this alias's type arguments directly. Walking
            # their unspecialized members would misattribute unrelated
            # dependent APIs to the alias.
            continue
        if clang.lib.clang_getCursorKind(base_decl) in (0, _CXCursor_NoDeclFound):
            exposed.append(
                _Exposure(
                    None,
                    alias_cursor,
                    "unresolved public/protected alias template base",
                )
            )
            continue
        exposed.extend(
            _Exposure(member, alias_cursor, "instantiated alias template base")
            for member in _record_member_candidates(clang, base_decl)
        )
        exposed.extend(
            _Exposure(
                inherited.source_cursor,
                alias_cursor,
                f"instantiated alias of {inherited.reason}",
            )
            for inherited in _inherited_and_reexported_encoders(clang, base_decl)
        )

    for using_name in using_names:
        simple_name = using_name.rsplit("::", 1)[-1]
        for argument_record in argument_records:
            for member in _record_member_candidates(clang, argument_record):
                member_name = clang.to_str(
                    clang.lib.clang_getCursorDisplayName(member)
                ).split("(", 1)[0]
                if member_name == simple_name:
                    exposed.append(
                        _Exposure(
                            member,
                            alias_cursor,
                            "instantiated dependent using-declaration alias",
                        )
                    )
    return exposed


def _inherited_and_reexported_encoders(
    clang: "_LibClang",
    class_cursor: "_CXCursor",
    depth: int = 0,
    visited: frozenset[str] = frozenset(),
) -> list[_Exposure]:
    """Walk `class_cursor`'s own direct children for:

      - a PUBLIC or PROTECTED base-specifier (see
        _INHERITABLE_ACCESS_SPECIFIERS): every one of that base class's
        own public/protected member functions becomes newly accessible
        through `class_cursor` itself (directly, for a public base; to
        any further subclass, for a protected one) with no new textual
        declaration inside `class_cursor` at all -- recursed
        transitively (with a depth guard against a pathological/cyclic
        hierarchy), so a multi-level inheritance chain is fully covered.
      - a PUBLIC or PROTECTED using-declaration (see
        _resolve_using_declaration_targets()): explicitly re-exports one
        or more inherited member(s) -- often from an otherwise PRIVATE
        base, which alone would have blocked exposure -- as new members
        of `class_cursor` itself.

    Returns a list of (source_cursor, attribution_cursor) pairs: for each
    newly-exposed member function found this way, `source_cursor` is the
    ORIGINAL member declaration (whose own shape/USR must still be
    checked by the caller), and `attribution_cursor` is the
    base-specifier or using-declaration cursor responsible for exposing
    it -- physically located inside `class_cursor`'s own body, in
    `class_cursor`'s own file, which is what lets the caller attribute a
    resulting Finding to the newly-exposing class/file rather than to
    the base's own original declaration file (where it may already be
    correctly allowlisted, entirely independently of this new exposure).

    A review round demonstrated this exact bypass: a new struct publicly
    inheriting from AuthenticateRequest (or privately inheriting it and
    using-declaring its toJson() back to public) exposes an
    already-allowlisted encoder as new, unaudited public API, entirely
    without writing any new textual declaration of its own."""

    if depth > _MAX_INHERITANCE_DEPTH:
        raise EncoderHygieneError(
            "Inheritance-exposure walk exceeded a sane recursion depth "
            f"({_MAX_INHERITANCE_DEPTH}) -- this is almost certainly a "
            "pathological/cyclic class hierarchy, not real production code."
        )

    class_usr = clang.to_str(clang.lib.clang_getCursorUSR(class_cursor))
    if class_usr and class_usr in visited:
        return []
    if class_usr:
        visited = visited | {class_usr}

    exposed: list[_Exposure] = []
    bases: list[_CXCursor] = []

    def visit(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if kind == _CXCursor_CXXBaseSpecifier and access in _INHERITABLE_ACCESS_SPECIFIERS:
            bases.append(cursor)
        elif kind == _CXCursor_UsingDeclaration and access in _INHERITABLE_ACCESS_SPECIFIERS:
            targets = _resolve_using_declaration_targets(clang, cursor)
            if targets:
                for target in targets:
                    exposed.append(
                        _Exposure(target, cursor, "using-declaration")
                    )
            else:
                exposed.append(
                    _Exposure(
                        None,
                        cursor,
                        "unresolved public/protected dependent using-declaration "
                        "capable of forwarding an encoder",
                    )
                )
        return 1  # CXChildVisit_Continue: never descend into member function bodies here.

    cb = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(class_cursor, cb, None)

    for base_specifier in bases:
        base_type = clang.lib.clang_getCursorType(base_specifier)
        base_decl = clang.lib.clang_getTypeDeclaration(base_type)
        base_kind = clang.lib.clang_getCursorKind(base_decl)
        if base_kind in (0, _CXCursor_TemplateTypeParameter, _CXCursor_NoDeclFound):
            exposed.append(
                _Exposure(
                    None,
                    base_specifier,
                    "unresolved public/protected dependent base capable of "
                    "inheriting an encoder",
                )
            )
            continue

        def visit_base_member(cursor: "_CXCursor", _parent: "_CXCursor", _client_data, _base=base_specifier) -> int:
            member_kind = clang.lib.clang_getCursorKind(cursor)
            member_access = clang.lib.clang_getCXXAccessSpecifier(cursor)
            if member_kind in _FUNCTION_LIKE_KINDS and member_access in _INHERITABLE_ACCESS_SPECIFIERS:
                exposed.append(_Exposure(cursor, _base, "base-class inheritance"))
            return 1  # CXChildVisit_Continue

        member_cb = clang._visitor_func_type(visit_base_member)
        clang.lib.clang_visitChildren(base_decl, member_cb, None)

        exposed.extend(
            _inherited_and_reexported_encoders(
                clang, base_decl, depth + 1, visited
            )
        )

    return exposed


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


def _sanitize_compile_args(command: str | Sequence[str], source_file: str) -> list[str]:
    """Turn one compile_commands.json entry's shell command string into the
    argv libclang's clang_parseTranslationUnit2() expects: drop the
    compiler executable itself (argv[0]), -o/-c/-arch <value>/-g (all
    irrelevant to AST-only parsing; some, like a stray -arch on a
    cross-compile entry, could even cause a spurious parse failure), and
    source-file token wherever it appears (passed to
    clang_parseTranslationUnit2() separately, not duplicated in argv)."""

    tokens = shlex.split(command) if isinstance(command, str) else list(command)
    if not tokens:
        raise EncoderHygieneError("compile_commands.json entry has an empty command/arguments array")
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
    cleaned = [token for token in cleaned if token != source_file]
    return cleaned


@dataclass(frozen=True)
class CompileContext:
    source: Path
    directory: Path
    arguments: tuple[str, ...]
    output: Path
    target: str
    configuration: str

    def identity(self) -> tuple[str, str, str, str, tuple[str, ...]]:
        return (
            str(self.source),
            str(self.output),
            self.target,
            self.configuration,
            self.arguments,
        )


def _entry_path(entry: dict, key: str, directory: Path) -> Path:
    value = entry.get(key)
    if not isinstance(value, str) or not value:
        raise EncoderHygieneError(
            f"compile_commands.json entry is missing non-empty {key!r}: {entry!r}"
        )
    path = Path(value)
    return (directory / path).resolve() if not path.is_absolute() else path.resolve()


def _compile_entry_tokens(entry: dict) -> list[str]:
    has_arguments = "arguments" in entry
    has_command = "command" in entry
    if has_arguments == has_command:
        raise EncoderHygieneError(
            "Each compile_commands.json entry must contain exactly one of "
            f"'arguments' or 'command', not both/neither: {entry!r}"
        )
    if has_arguments:
        arguments = entry["arguments"]
        if not isinstance(arguments, list) or not all(
            isinstance(argument, str) for argument in arguments
        ):
            raise EncoderHygieneError(
                f"compile command 'arguments' must be an array of strings: {entry!r}"
            )
        return list(arguments)
    command = entry["command"]
    if not isinstance(command, str):
        raise EncoderHygieneError(
            f"compile command 'command' must be a string: {entry!r}"
        )
    return shlex.split(command)


def _target_and_configuration_from_output(output: Path) -> tuple[str, str]:
    parts = output.parts
    candidates = [
        index
        for index, part in enumerate(parts[:-1])
        if part == "CMakeFiles"
        and index + 1 < len(parts)
        and parts[index + 1].endswith(".dir")
    ]
    if len(candidates) != 1:
        raise EncoderHygieneError(
            f"Compile-command output {output} does not carry one unambiguous "
            "CMake target object identity (expected exactly one "
            "CMakeFiles/<target>.dir component)"
        )
    index = candidates[0]
    target = parts[index + 1][: -len(".dir")]
    if not target:
        raise EncoderHygieneError(f"Compile-command output {output} has an empty target identity")
    relative_output = parts[index + 2 :]
    configurations = {
        "Debug",
        "Release",
        "RelWithDebInfo",
        "MinSizeRel",
    }
    configuration = relative_output[0] if relative_output and relative_output[0] in configurations else ""
    return target, configuration


def _validate_compile_contexts(contexts: Sequence[CompileContext]) -> None:
    identities = [context.identity() for context in contexts]
    if len(identities) != len(set(identities)):
        raise EncoderHygieneError(
            "Duplicate indistinguishable compile commands found; "
            "each target/configuration/object context must be unique"
        )
    outputs = [context.output for context in contexts]
    if len(outputs) != len(set(outputs)):
        raise EncoderHygieneError(
            f"Multiple compile commands claim the same object output: {outputs}"
        )
    target_configurations = [
        (context.source, context.target, context.configuration)
        for context in contexts
    ]
    if len(target_configurations) != len(set(target_configurations)):
        raise EncoderHygieneError(
            "Multiple unexplained object commands compile one source for the same "
            "target/configuration; ownership is ambiguous"
        )


def _all_compile_contexts(
    compile_commands: Sequence[dict],
) -> list[CompileContext]:
    contexts: list[CompileContext] = []
    for entry in compile_commands:
        directory_value = entry.get("directory")
        if not isinstance(directory_value, str) or not directory_value:
            raise EncoderHygieneError(
                f"compile_commands.json entry has no exact working directory: {entry!r}"
            )
        directory = Path(directory_value).resolve()
        source = _entry_path(entry, "file", directory)
        output = _entry_path(entry, "output", directory)
        target, configuration = _target_and_configuration_from_output(output)
        raw_tokens = _compile_entry_tokens(entry)
        arguments = tuple(_sanitize_compile_args(raw_tokens, entry["file"]))
        contexts.append(
            CompileContext(
                source=source,
                directory=directory,
                arguments=arguments,
                output=output,
                target=target,
                configuration=configuration,
            )
        )
    _validate_compile_contexts(contexts)
    return contexts


def _compile_contexts_for_source(
    compile_commands: Sequence[dict], source: Path
) -> list[CompileContext]:
    resolved = source.resolve()
    contexts = [
        context
        for context in _all_compile_contexts(compile_commands)
        if context.source == resolved
    ]
    if not contexts:
        raise EncoderHygieneError(
            f"No compile_commands.json entry found for source {source}; every "
            "manifested source must have at least one target-owned object command"
        )
    return contexts


def _macos_sdk_sysroot() -> str:
    return subprocess.check_output(
        ["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True
    ).strip()


def _header_compile_contexts(
    compile_commands: list[dict], sources: Sequence[Path], target_label: str
) -> list[CompileContext]:
    """Return every distinct target/configuration argument context for
    the manifested sources. Headers are independently parsed in all of
    them, so a consumer-specific define/include path or multi-config
    object command cannot be hidden by a first-path match."""

    if not sources:
        raise EncoderHygieneError(
            f"No {target_label} sources were listed in its manifest -- cannot "
            "derive target/configuration compile contexts for its headers."
        )
    contexts: list[CompileContext] = []
    for source in sources:
        contexts.extend(_compile_contexts_for_source(compile_commands, source))
    unique: dict[tuple, CompileContext] = {}
    for context in contexts:
        key = (
            context.target,
            context.configuration,
            context.directory,
            context.arguments,
        )
        unique.setdefault(key, context)
    if not unique:
        raise EncoderHygieneError(
            f"No target/configuration compile contexts found for {target_label} headers"
        )
    return list(unique.values())


@contextmanager
def _working_directory(directory: Path) -> Iterator[None]:
    previous = Path.cwd()
    try:
        os.chdir(directory)
        yield
    finally:
        os.chdir(previous)


def _parse_header_as_own_tu(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    header: Path,
    compile_args: list[str],
    sysroot_args: list[str],
    working_directory: Path | None = None,
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
    with _working_directory(working_directory or Path.cwd()):
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
        if not entry.is_file():
            violations.append(f"  {entry} is missing or is not a regular file")
            continue
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


def _physical_identity(path: Path) -> tuple[int, int] | None:
    try:
        stat = path.stat()
    except OSError:
        return None
    return stat.st_dev, stat.st_ino


@dataclass(frozen=True)
class OwnedPathPolicy:
    roots: frozenset[Path]
    physical_identities: frozenset[tuple[int, int]]

    def owns(self, lexical: Path, real: Path) -> bool:
        if any(
            lexical.is_relative_to(root) or real.is_relative_to(root)
            for root in self.roots
        ):
            return True
        identity = _physical_identity(real)
        return identity is not None and identity in self.physical_identities


def _owned_path_policy(
    repo_root: Path,
    generated_roots: frozenset[Path],
    generated_files: frozenset[Path] = frozenset(),
) -> OwnedPathPolicy:
    roots = frozenset({(repo_root / "src").resolve()}) | generated_roots
    identities: set[tuple[int, int]] = set()
    for root in roots:
        if not root.is_dir():
            raise EncoderHygieneError(f"Owned project/generated root does not exist: {root}")
        for path in root.rglob("*"):
            if path.is_file():
                identity = _physical_identity(path)
                if identity is not None:
                    identities.add(identity)
    for path in generated_files:
        identity = _physical_identity(path)
        if identity is None:
            raise EncoderHygieneError(
                f"Owned generated compile unit is missing: {path}"
            )
        identities.add(identity)
    return OwnedPathPolicy(roots=roots, physical_identities=frozenset(identities))


def _audit_inclusion_graph(
    clang: _LibClang,
    tu: ctypes.c_void_p,
    header: Path,
    wrapper_filename: str,
    allowed_closure: frozenset[Path],
    external_roots: frozenset[Path],
    owned_paths: OwnedPathPolicy | None = None,
    working_directory: Path | None = None,
) -> list[str]:
    """Ask libclang for the complete resolved inclusion graph of
    `header`'s own wrapper TU (via clang_getInclusions() -- see the
    _LibClang binding above and the module docstring) and return one
    violation description per project-owned file it reaches that is not
    a member of `allowed_closure`.

    A file is classified as genuinely EXTERNAL (and therefore exempt
    from the `allowed_closure` membership check entirely) if, and only
    if, EITHER:

      - `clang_Location_isInSystemHeader()` reports it was reached via a
        real compiler/toolchain system-include mechanism
        (`-isystem`/`-iframework`/the macOS SDK `-isysroot` root) --
        this is the COMPILER's own authoritative classification, not a
        lexical guess, and empirically confirmed (against this
        project's own real compile commands) to correctly cover every
        Qt header (CMake automatically marks an IMPORTED target's own
        include directories as SYSTEM for consumers) and every macOS
        SDK/libc++ header; or
      - its own resolved, symlink-followed real path lies inside one of
        the small, EXPLICIT `external_roots` (see _external_roots(),
        itself sourced from real CMake FetchContent package metadata,
        not a lexical `_deps` guess) -- needed because a
        FetchContent-vendored dependency's OWN headers (e.g.
        qtkeychain's) are, empirically confirmed, compiled via an
        ordinary `-I`, not `-isystem`, and are therefore NOT classified
        as a system header by Clang itself.

    EVERY other resolved file -- including one physically located
    outside this repository's own tracked source tree entirely, which a
    review round demonstrated an earlier revision blanket-exempted as
    always external purely by virtue of being outside `repo_root` -- is
    still audited exactly like any other project file: it must be a
    member of `allowed_closure` or this is a hard violation, never a
    silent skip. (In practice such a file categorically CANNOT be a
    member of `allowed_closure` at all, since every closure entry is
    independently required, by _validate_closure_rootedness(), to
    physically reside inside this repository's own src/domain or src/
    root -- so this now correctly, unconditionally fails closed instead
    of silently exempting it.)

    This is independent of however the #include that reached the
    file was spelled: bare, "../"-relative, absolute, through
    a symlink, or via a project-generated wrapper header all resolve to
    the same real file identity here, which is exactly what a
    compile-flag/include-path-based defense alone cannot guarantee (see
    module docstring)."""

    violations: list[str] = []
    included: list[tuple[Path, ctypes.c_void_p]] = []

    def visitor(included_file, _inclusion_stack, _include_len, _client_data) -> None:
        if not included_file:
            return
        name = clang.to_str(clang.lib.clang_getFileName(included_file))
        if name:
            included.append((Path(name), included_file))

    cb = clang._inclusion_visitor_func_type(visitor)
    clang.lib.clang_getInclusions(tu, cb, None)

    wrapper_real = Path(wrapper_filename).resolve()
    for included_path, included_file in included:
        lexical = (
            (working_directory / included_path).absolute()
            if working_directory is not None and not included_path.is_absolute()
            else included_path.absolute()
        )
        real = lexical.resolve()
        # The wrapper's own synthetic "main file" is not a real
        # #include at all -- clang_getInclusions() reports it anyway
        # (with an empty inclusion stack), so it must be recognized and
        # skipped by its EXACT resolved identity, never merely by
        # basename -- a review round demonstrated a basename-only
        # comparison cannot distinguish the wrapper's own synthetic file
        # from a second, differently-located real project file that
        # happens to share the same basename.
        if real == wrapper_real:
            continue
        if real in allowed_closure:
            continue
        # Project source and registered generated ownership always wins
        # over compiler/system and external-root markings. The physical
        # identity set also catches hardlink aliases placed under an
        # otherwise external/system include directory.
        if owned_paths is not None and owned_paths.owns(lexical, real):
            violations.append(
                f"  {header} transitively #includes {included_path} "
                f"(resolves to owned file {real}), which is not part of the "
                "allowed project/generated closure for this scan"
            )
            continue
        location = clang.lib.clang_getLocation(tu, included_file, 1, 1)
        if clang.lib.clang_Location_isInSystemHeader(location):
            continue  # A genuine compiler/system/toolchain header (see this function's own doc comment).
        if any(real.is_relative_to(root) for root in external_roots):
            continue  # Explicitly-registered external subtree (e.g. FetchContent-vendored source/build).
        violations.append(
            f"  {header} transitively #includes {included_path} "
            f"(resolves to {real}), which is not part of the allowed "
            "header/fragment closure for this scan -- a forbidden "
            "cross-boundary dependency or an unregistered project file, "
            "regardless of how the #include itself was spelled"
        )

    return violations


def _external_roots(clang_build_dir: Path) -> frozenset[Path]:
    """The small, EXPLICIT set of dependency subtrees this script treats as
    genuinely external (never subject to the domain/
    foundation dependency-direction closure check), independent of the
    blanket "anything under the build directory" (or, later, "anything
    outside repo_root") exemption two separate review rounds
    demonstrated was unsound (see _audit_inclusion_graph()'s own doc
    comment for the exact bypasses this replaces).

    Read from `<clang-build-dir>/generated/external_roots.txt`, which
    CMakeLists.txt populates from the real FetchContent
    `qtkeychain_SOURCE_DIR`/`qtkeychain_BINARY_DIR` metadata. AUTOGEN is
    deliberately absent: it is project-owned, enumerated against each
    target's AutogenInfo.json by _load_autogen_closures(), and audited."""

    manifest = clang_build_dir / "generated" / "external_roots.txt"
    return frozenset(root.resolve() for root in _read_manifest(manifest))


_CXX_GENERATED_SUFFIXES = frozenset(
    {".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp", ".c", ".cc", ".cpp", ".cxx", ".moc"}
)


@dataclass(frozen=True)
class AutogenClosure:
    target: str
    policy: str
    root: Path
    code_files: frozenset[Path]
    source_moc_owners: tuple[tuple[Path, Path], ...]


@dataclass(frozen=True)
class TargetPolicy:
    classification: str
    policy: str
    target: str
    target_type: str
    source_dir: Path
    binary_dir: Path


def _load_target_policies(clang_build_dir: Path) -> dict[str, TargetPolicy]:
    manifest = clang_build_dir / "generated" / "target_policy.txt"
    if not manifest.is_file():
        raise EncoderHygieneError(f"Target policy manifest is missing: {manifest}")
    policies: dict[str, TargetPolicy] = {}
    for line_number, raw_line in enumerate(
        manifest.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not raw_line:
            continue
        fields = raw_line.split("\t")
        if len(fields) != 6:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: expected "
                "CLASSIFICATION<TAB>POLICY<TAB>TARGET<TAB>TYPE<TAB>SOURCE_DIR<TAB>BINARY_DIR"
            )
        classification, policy, target, target_type, source_dir, binary_dir = fields
        if classification not in {"SCAN", "EXTERNAL", "TEST", "TRY_COMPILE"}:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: invalid target classification {classification!r}"
            )
        if classification == "SCAN" and policy not in {
            "domain",
            "foundation",
            "application",
        }:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: SCAN target has invalid policy {policy!r}"
            )
        if classification != "SCAN" and policy:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: excluded target must not carry policy {policy!r}"
            )
        if not target or target in policies or not target_type:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: empty/duplicate target identity"
            )
        source_path = Path(source_dir).resolve()
        binary_path = Path(binary_dir).resolve()
        if not source_path.is_dir() or not binary_path.is_dir():
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: target source/binary directory is missing"
            )
        policies[target] = TargetPolicy(
            classification=classification,
            policy=policy,
            target=target,
            target_type=target_type,
            source_dir=source_path,
            binary_dir=binary_path,
        )
    if not policies:
        raise EncoderHygieneError(f"Target policy manifest is empty: {manifest}")
    return policies


def _parse_autogen_source_mocs(
    metadata: dict, metadata_path: Path, root: Path
) -> dict[Path, Path]:
    sources = metadata.get("SOURCES")
    if not isinstance(sources, list):
        raise EncoderHygieneError(f"{metadata_path}: SOURCES must be an array")
    source_modes: dict[Path, str] = {}
    for record in sources:
        if (
            not isinstance(record, list)
            or len(record) != 3
            or not isinstance(record[0], str)
            or not isinstance(record[1], str)
            or record[2] is not None
        ):
            raise EncoderHygieneError(
                f"Malformed SOURCES entry in {metadata_path}: {record!r}"
            )
        source = Path(record[0]).resolve()
        if source in source_modes or record[1] not in {"MU", "Mu"}:
            raise EncoderHygieneError(
                f"Duplicate/unsupported AUTOGEN source record in {metadata_path}: {record!r}"
            )
        source_modes[source] = record[1]

    owners: dict[Path, Path] = {}
    suffixes = [""]
    if metadata.get("MULTI_CONFIG") is True:
        suffixes = sorted(
            key.removeprefix("PARSE_CACHE_FILE_")
            for key, value in metadata.items()
            if key.startswith("PARSE_CACHE_FILE_")
            and isinstance(value, str)
            and Path(value).is_file()
        )
    if not suffixes:
        raise EncoderHygieneError(
            f"{metadata_path}: no built AUTOGEN configuration has a ParseCache"
        )

    for suffix in suffixes:
        key_suffix = f"_{suffix}" if suffix else ""
        parse_cache_value = metadata.get(f"PARSE_CACHE_FILE{key_suffix}")
        include_dir_value = metadata.get(f"INCLUDE_DIR{key_suffix}")
        if not isinstance(parse_cache_value, str) or not isinstance(include_dir_value, str):
            raise EncoderHygieneError(
                f"{metadata_path}: missing explicit ParseCache/include paths for {suffix or 'single-config'}"
            )
        parse_cache = Path(parse_cache_value).resolve()
        include_dir = Path(include_dir_value).resolve()
        if not parse_cache.is_file() or not include_dir.is_relative_to(root):
            raise EncoderHygieneError(
                f"{metadata_path}: source-MOC parse cache/include directory is missing or outside AUTOGEN"
            )

        mids: dict[Path, list[str]] = {source: [] for source in source_modes}
        current: Path | None = None
        lines = parse_cache.read_text(encoding="utf-8").splitlines()
        if not lines or lines[0] != "# Generated by CMake. Changes will be overwritten.":
            raise EncoderHygieneError(
                f"{parse_cache}: missing CMake ParseCache provenance header"
            )
        for line in lines[1:]:
            if line and not line.startswith(" "):
                current = Path(line).resolve()
                continue
            if line.startswith(" mid:"):
                if current not in mids:
                    raise EncoderHygieneError(
                        f"{parse_cache}: source-local MOC belongs to unregistered source {current}"
                    )
                mids[current].append(line[len(" mid:") :])

        for source in source_modes:
            includes = mids[source]
            for include in includes:
                if not include.endswith(".moc") or Path(include).is_absolute():
                    raise EncoderHygieneError(
                        f"{parse_cache}: invalid source-local MOC include {include!r}"
                    )
                output = (include_dir / include).resolve()
                if not output.is_relative_to(include_dir) or output in owners:
                    raise EncoderHygieneError(
                        f"{parse_cache}: escaping/duplicate source-local MOC output {output}"
                    )
                owners[output] = source
    return owners


def _load_autogen_closures(clang_build_dir: Path) -> dict[str, list[AutogenClosure]]:
    manifest = clang_build_dir / "generated" / "autogen_targets.txt"
    if not manifest.is_file():
        raise EncoderHygieneError(
            f"Owned AUTOGEN target manifest is missing: {manifest}"
        )

    closures: dict[str, list[AutogenClosure]] = {
        "domain": [],
        "foundation": [],
        "application": [],
    }
    seen_targets: set[str] = set()
    for line_number, raw_line in enumerate(
        manifest.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not raw_line.strip():
            continue
        fields = raw_line.split("\t")
        if len(fields) != 3:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: expected POLICY<TAB>TARGET<TAB>ROOT"
            )
        policy, target, root_text = fields
        if policy not in closures or not target or target in seen_targets:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: invalid/duplicate AUTOGEN ownership "
                f"record {raw_line!r}"
            )
        seen_targets.add(target)
        root = Path(root_text).resolve()
        metadata_path = (
            clang_build_dir
            / "CMakeFiles"
            / f"{target}_autogen.dir"
            / "AutogenInfo.json"
        )
        if not metadata_path.is_file():
            raise EncoderHygieneError(
                f"CMake AUTOGEN metadata for target {target!r} is missing: {metadata_path}"
            )
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise EncoderHygieneError(
                f"Could not read CMake AUTOGEN metadata {metadata_path}: {exc}"
            ) from exc
        metadata_root = Path(metadata.get("BUILD_DIR", "")).resolve()
        if metadata_root != root:
            raise EncoderHygieneError(
                f"AUTOGEN root mismatch for {target}: manifest={root}, "
                f"AutogenInfo.json={metadata_root}"
            )

        compilation_keys = ["MOC_COMPILATION_FILE"]
        if metadata.get("MULTI_CONFIG") is True:
            compilation_keys = sorted(
                key
                for key, value in metadata.items()
                if key.startswith("MOC_COMPILATION_FILE_")
                and isinstance(value, str)
                and Path(value).is_file()
            )
        compilations = {
            Path(metadata.get(key, "")).resolve() for key in compilation_keys
        }
        if not compilations or any(
            compilation.parent != root or not compilation.is_file()
            for compilation in compilations
        ):
            raise EncoderHygieneError(
                f"AUTOGEN compilation units for {target} are missing/outside "
                f"their owned root: {sorted(compilations)}"
            )
        source_moc_owners = _parse_autogen_source_mocs(
            metadata, metadata_path, root
        )
        expected_code = {*compilations, *source_moc_owners.keys()}
        header_mocs: set[Path] = set()
        for header_record in metadata.get("HEADERS", []):
            if not isinstance(header_record, list) or len(header_record) < 3:
                raise EncoderHygieneError(
                    f"Malformed HEADERS entry in {metadata_path}: {header_record!r}"
                )
            output = header_record[2]
            if isinstance(output, str) and output:
                generated = (root / output).resolve()
                if generated.is_file():
                    expected_code.add(generated)
                    header_mocs.add(generated)

        actual_code = {
            path.resolve()
            for path in root.rglob("*")
            if path.is_file()
            and path.suffix.lower() in _CXX_GENERATED_SUFFIXES
            and not _re.fullmatch(
                r"moc_predefs(?:_[A-Za-z0-9_]+)?\.h", path.name
            )
        }
        unexplained = actual_code - expected_code
        missing = expected_code - actual_code
        if unexplained or missing:
            details = [
                *(f"  unexplained generated C/C++ file: {path}" for path in sorted(unexplained)),
                *(f"  missing generated C/C++ file: {path}" for path in sorted(missing)),
            ]
            raise EncoderHygieneError(
                f"AUTOGEN closure for {target} does not exactly match CMake's "
                "AutogenInfo.json; arbitrary writable files under AUTOGEN are "
                "project-owned and never trusted as external:\n" + "\n".join(details)
            )

        for generated in expected_code - compilations:
            prefix = generated.read_text(encoding="utf-8", errors="replace")[:512]
            if "Meta object code from reading C++ file" not in prefix or "Qt Meta Object Compiler" not in prefix:
                raise EncoderHygieneError(
                    f"Expected CMake-declared MOC artifact lacks the genuine Qt "
                    f"moc provenance banner: {generated}"
                )
        compilation_texts = {
            compilation: compilation.read_text(encoding="utf-8", errors="replace")
            for compilation in compilations
        }
        for compilation, compilation_text in compilation_texts.items():
            if "This file is autogenerated. Changes will be overwritten." not in compilation_text[:256]:
                raise EncoderHygieneError(
                    f"AUTOGEN aggregation unit lacks CMake's generated provenance banner: "
                    f"{compilation}"
                )
        for generated in header_mocs:
            relative = generated.relative_to(root).as_posix()
            if not any(
                f'#include "{relative}"' in text
                for text in compilation_texts.values()
            ):
                raise EncoderHygieneError(
                    f"CMake-declared MOC artifact is not enumerated by the target's "
                    f"audited aggregation unit: {generated}"
                )

        closures[policy].append(
            AutogenClosure(
                target=target,
                policy=policy,
                root=root,
                code_files=frozenset(expected_code),
                source_moc_owners=tuple(
                    sorted(source_moc_owners.items(), key=lambda item: str(item[0]))
                ),
            )
        )
    return closures


def _scan_headers(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    headers: Sequence[Path],
    compile_contexts: Sequence[CompileContext],
    sysroot_args: list[str],
    repo_root: Path,
    external_roots: frozenset[Path],
    allowed_closure: frozenset[Path],
    seen: set[tuple],
    structural_violations: list[str],
    owned_paths: OwnedPathPolicy | None = None,
) -> list[Finding]:
    """Independently parse every header/fragment in `headers` as its own
    synthetic wrapper translation unit (see _parse_header_as_own_tu()).

    For each one:
      - Audits its complete resolved inclusion graph against
        `allowed_closure` (see _audit_inclusion_graph()), appending any
        violation found to `structural_violations` -- a hard,
        never-allowlist-able failure (see run_check()).
      - Records a Finding for every public, encoder-shaped (see
        _is_encoder_shaped(): QJson-family return type OR non-const
        QJson-family output/inout parameter), function-like/constructor
        declaration whose OWN resolved location is a member of
        `allowed_closure` -- not merely "== the header currently being
        probed": a header may legitimately #include another member of
        its own closure (e.g. Decks.h #include-ing ValueOrError.h), and
        that included file's declarations must still be attributed to
        their own true source file.
      - For every class/struct/class-template DEFINITION whose own
        resolved location is a member of `allowed_closure`, additionally
        walks its base-class/using-declaration inheritance exposure (see
        _inherited_and_reexported_encoders()) and records a SEPARATE
        Finding -- attributed to the EXPOSING class's own file/line, not
        the original declaration's file -- for every encoder-shaped
        member function it newly makes accessible; this deliberately
        does not affect classify()/ALLOWLIST_BY_KEY, since the
        (exposing file, usr) key will never match an allowlist entry
        keyed to the original declaring file, closing the "derive from
        an already-allowlisted encoder type" bypass without any change
        to the allowlist mechanism itself.

    `seen` is a single exact canonical-identity set shared across the
    entire run. Own declarations include resolved file, line, USR, kind,
    signature, access, and shape; inherited/alias exposures additionally
    include their exposing declaration and reason. The differing keys guarantee
    the two kinds can never collide with each other even at the exact
    same nominal (file, line, usr), while still deduplicating repeats of
    the *same* kind (e.g. a legitimately shared/cross-included file's
    declarations, recorded exactly once no matter how many headers in
    its own closure #include it -- never once per including header)."""

    findings: list[Finding] = []

    def record_if_new(
        *,
        dedup_key: tuple,
        real: Path,
        line: int,
        display_name: str,
        shape_description: str,
        usr: str,
    ) -> None:
        if dedup_key in seen:
            return
        seen.add(dedup_key)
        findings.append(
            Finding(
                file=real.relative_to(repo_root).as_posix(),
                line=line,
                display_name=display_name,
                canonical_return_type=shape_description,
                usr=usr,
            )
        )

    def handle_own_declaration(cursor: _CXCursor, kind: int) -> None:
        canonical_cursor = clang.lib.clang_getCanonicalCursor(cursor)
        filename, line = clang.cursor_file_and_line(canonical_cursor)
        if filename is None:
            return
        real = Path(filename).resolve()
        if real not in allowed_closure:
            return
        access = clang.lib.clang_getCXXAccessSpecifier(canonical_cursor)
        if access not in _PUBLIC_ACCESS_SPECIFIERS:
            return
        is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
        if not is_shaped:
            return
        usr = clang.to_str(clang.lib.clang_getCursorUSR(canonical_cursor))
        signature_type = clang.lib.clang_getCanonicalType(
            clang.lib.clang_getCursorType(canonical_cursor)
        )
        signature = clang.to_str(clang.lib.clang_getTypeSpelling(signature_type))
        record_if_new(
            dedup_key=(
                str(real),
                line,
                usr,
                kind,
                signature,
                access,
                shape_description,
            ),
            real=real,
            line=line,
            display_name=clang.to_str(clang.lib.clang_getCursorDisplayName(cursor)),
            shape_description=shape_description,
            usr=usr,
        )

    def handle_inheritance_exposure(
        class_cursor: _CXCursor, exposures: Sequence[_Exposure] | None = None
    ) -> None:
        filename, _def_line = clang.cursor_file_and_line(class_cursor)
        if filename is None:
            return
        if Path(filename).resolve() not in allowed_closure:
            return
        class_access = clang.lib.clang_getCXXAccessSpecifier(class_cursor)
        if class_access not in (
            _CX_CXXInvalidAccessSpecifier,
            _CX_CXXPublic,
            _CX_CXXProtected,
        ):
            return
        class_usr = clang.to_str(clang.lib.clang_getCursorUSR(class_cursor))
        for exposure in (
            exposures
            if exposures is not None
            else _inherited_and_reexported_encoders(clang, class_cursor)
        ):
            attribution_filename, attribution_line = clang.cursor_file_and_line(
                exposure.attribution_cursor
            )
            if attribution_filename is None:
                continue
            attribution_real = Path(attribution_filename).resolve()
            if attribution_real not in allowed_closure:
                continue
            if exposure.source_cursor is None:
                usr = f"{class_usr}@dependent-exposure@{attribution_line}@{exposure.reason}"
                record_if_new(
                    dedup_key=(
                        str(attribution_real),
                        attribution_line,
                        usr,
                        "dependent-inheritance",
                        class_usr,
                    ),
                    real=attribution_real,
                    line=attribution_line,
                    display_name=exposure.reason,
                    shape_description=(
                        "unresolved exposure capable of returning/mutating QJsonObject"
                    ),
                    usr=usr,
                )
                continue
            source_kind = clang.lib.clang_getCursorKind(exposure.source_cursor)
            is_shaped, shape_description = _is_encoder_shaped(
                clang, exposure.source_cursor, source_kind
            )
            if not is_shaped:
                continue
            usr = clang.to_str(
                clang.lib.clang_getCursorUSR(exposure.source_cursor)
            )
            source_signature = clang.to_str(
                clang.lib.clang_getTypeSpelling(
                    clang.lib.clang_getCanonicalType(
                        clang.lib.clang_getCursorType(exposure.source_cursor)
                    )
                )
            )
            record_if_new(
                dedup_key=(
                    str(attribution_real),
                    attribution_line,
                    usr,
                    "inherited",
                    class_usr,
                    source_signature,
                    exposure.reason,
                ),
                real=attribution_real,
                line=attribution_line,
                display_name=(
                    f"{clang.to_str(clang.lib.clang_getCursorDisplayName(exposure.source_cursor))} "
                    "(exposed via inheritance/using-declaration)"
                ),
                shape_description=shape_description,
                usr=usr,
            )

    def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        if kind in (_CXCursor_TypeAliasDecl, _CXCursor_TypedefDecl):
            handle_inheritance_exposure(
                cursor, _alias_reexported_encoders(clang, cursor)
            )
            return 1
        if kind in _RECORD_LIKE_KINDS and clang.lib.clang_isCursorDefinition(cursor):
            handle_inheritance_exposure(cursor)
            return 2  # CXChildVisit_Recurse: still walk this class's own direct members normally.
        if kind in _OUTPARAM_CHECKED_KINDS:  # Superset of _FUNCTION_LIKE_KINDS, includes constructors.
            handle_own_declaration(cursor, kind)
            return 1  # CXChildVisit_Continue: do not descend into the body.
        return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

    visitor_cb = clang._visitor_func_type(visitor)

    if not compile_contexts:
        raise EncoderHygieneError("Header scan received no target/configuration compile contexts")
    for header in headers:
        for context in compile_contexts:
            tu, wrapper_filename = _parse_header_as_own_tu(
                clang,
                idx,
                header,
                list(context.arguments),
                sysroot_args,
                context.directory,
            )
            try:
                structural_violations.extend(
                    _audit_inclusion_graph(
                        clang,
                        tu,
                        header,
                        wrapper_filename,
                        allowed_closure,
                        external_roots,
                        owned_paths,
                        context.directory,
                    )
                )
                root = clang.lib.clang_getTranslationUnitCursor(tu)
                clang.lib.clang_visitChildren(root, visitor_cb, None)
            finally:
                clang.lib.clang_disposeTranslationUnit(tu)


    return findings


def _parse_source_as_own_tu(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    context: CompileContext,
    sysroot_args: list[str],
) -> ctypes.c_void_p:
    """Parse a REAL production .cpp `source` directly, on disk, as its own
    translation unit using one exact target/configuration object context
    returned by _compile_contexts_for_source(), never flags borrowed
    from any other file. Unlike _parse_header_as_own_tu(), no synthetic
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

    source = context.source
    compile_args = list(context.arguments)
    source_abs = str(source)

    args_bytes = [a.encode("utf-8") for a in (compile_args + sysroot_args)]
    argv = (ctypes.c_char_p * len(args_bytes))(*args_bytes)

    tu_ptr = ctypes.c_void_p()
    with _working_directory(context.directory):
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
            f"using exact target/config context {context.target}/"
            f"{context.configuration or '<single-config>'} from {context.directory} "
            f"(CXErrorCode="
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
    seen: set[tuple],
    owned_paths: OwnedPathPolicy | None = None,
    target: str | None = None,
    contexts: Sequence[CompileContext] | None = None,
) -> tuple[list[str], list[Finding]]:
    """Independently parse every REAL production .cpp in `sources` as its
    own translation unit (see _parse_source_as_own_tu()) in every exact
    target/configuration object command matching its physical path --
    never a first-match or borrowed context -- and:

      - audit its complete resolved inclusion graph against
        `allowed_closure`, exactly like _scan_headers() already does for
        headers/fragments (see _audit_inclusion_graph(), reused
        unchanged here: a source's own file is skipped from the graph
        the identical way a header's own wrapper "main file" entry is,
        by passing the source's own path as the self-filtering
        `wrapper_filename` argument);
      - collect Findings for source-only external declarations and for
        project-header declarations visible only in this TU's exact
        preprocessor context,
        such as a namespace-scope `QJsonObject encodeDeck(const
        DeckList&)` written directly in a .cpp with no header
        declaration at all, which a review round demonstrated compiles
        and remains callable from another translation unit via an
        ad-hoc `extern` forward declaration despite this script
        previously collecting zero findings from source scanning.

    An out-of-line definition of an already observed header declaration (e.g.
    src/domain/RawJson.cpp's `Value::toExactQJson()`, or
    src/AuthModels.cpp's `AuthenticateRequest::toJson()`) is correctly
    not re-flagged: it is skipped only when its exact canonical identity
    (resolved path, line, USR, signature, access, and shape) is already
    in unified `seen` data. Header-path membership alone never suppresses
    a macro-conditional declaration absent from standalone wrappers. Only
    when the canonical cursor's own file is the .cpp itself (no earlier
    declaration anywhere) -- and that declaration has genuinely EXTERNAL
    linkage (clang_getCursorLinkage() == CXLinkage_External; a `static`-
    or anonymous-namespace-scoped helper has internal/unique-external
    linkage and categorically cannot be referenced from another
    translation unit, so is correctly never flagged) -- is a new Finding
    recorded, keyed by the *source*'s own repo-relative path (which can
    never match any ALLOWLIST entry, since every entry is keyed to a
    header path), so it always, correctly, classifies as a violation.

    `seen` is the same whole-run canonical-identity set _scan_headers()
    uses (resolved file, line, USR, kind, signature, access, and shape):
    a source-only declaration's key uses the source's own path, so it cannot
    collide with a header-scan entry, but a source scanned more than
    once (impossible in this script's normal flow, since each manifest
    entry is scanned exactly once, but kept for defensive consistency
    with _scan_headers()) would still be deduplicated correctly."""

    violations: list[str] = []
    findings: list[Finding] = []

    def finding_path(real: Path) -> str:
        try:
            return real.relative_to(repo_root).as_posix()
        except ValueError:
            return real.as_posix()

    def make_visitor(source_real: Path):
        def record_own(cursor: _CXCursor, kind: int) -> None:
            canonical = clang.lib.clang_getCanonicalCursor(cursor)
            canonical_filename, canonical_line = clang.cursor_file_and_line(canonical)
            if canonical_filename is None:
                return
            canonical_real = Path(canonical_filename).resolve()
            if canonical_real not in allowed_closure and canonical_real != source_real:
                return

            access = clang.lib.clang_getCXXAccessSpecifier(canonical)
            if access not in _PUBLIC_ACCESS_SPECIFIERS:
                return
            if canonical_real == source_real:
                linkage = clang.lib.clang_getCursorLinkage(cursor)
                if linkage != _CXLinkage_External:
                    return

            is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
            if not is_shaped:
                return
            usr = clang.to_str(clang.lib.clang_getCursorUSR(canonical))
            signature = clang.to_str(
                clang.lib.clang_getTypeSpelling(
                    clang.lib.clang_getCanonicalType(
                        clang.lib.clang_getCursorType(canonical)
                    )
                )
            )
            dedup_key = (
                str(canonical_real),
                canonical_line,
                usr,
                kind,
                signature,
                access,
                shape_description,
            )
            if dedup_key in seen:
                return
            seen.add(dedup_key)
            findings.append(
                Finding(
                    file=finding_path(canonical_real),
                    line=canonical_line,
                    display_name=clang.to_str(
                        clang.lib.clang_getCursorDisplayName(cursor)
                    ),
                    canonical_return_type=shape_description,
                    usr=usr,
                )
            )

        def record_inheritance(
            class_cursor: _CXCursor,
            exposures: Sequence[_Exposure] | None = None,
        ) -> None:
            class_filename, _class_line = clang.cursor_file_and_line(class_cursor)
            if class_filename is None:
                return
            class_real = Path(class_filename).resolve()
            if class_real not in allowed_closure and class_real != source_real:
                return
            class_access = clang.lib.clang_getCXXAccessSpecifier(class_cursor)
            if class_access not in (
                _CX_CXXInvalidAccessSpecifier,
                _CX_CXXPublic,
                _CX_CXXProtected,
            ):
                return
            class_usr = clang.to_str(clang.lib.clang_getCursorUSR(class_cursor))
            for exposure in (
                exposures
                if exposures is not None
                else _inherited_and_reexported_encoders(clang, class_cursor)
            ):
                attribution_filename, attribution_line = clang.cursor_file_and_line(
                    exposure.attribution_cursor
                )
                if attribution_filename is None:
                    continue
                attribution_real = Path(attribution_filename).resolve()
                if attribution_real not in allowed_closure and attribution_real != source_real:
                    continue
                if exposure.source_cursor is None:
                    usr = (
                        f"{class_usr}@dependent-exposure@{attribution_line}@"
                        f"{exposure.reason}"
                    )
                    shape_description = (
                        "unresolved exposure capable of returning/mutating QJsonObject"
                    )
                    display_name = exposure.reason
                    source_signature = ""
                else:
                    source_kind = clang.lib.clang_getCursorKind(
                        exposure.source_cursor
                    )
                    is_shaped, shape_description = _is_encoder_shaped(
                        clang, exposure.source_cursor, source_kind
                    )
                    if not is_shaped:
                        continue
                    usr = clang.to_str(
                        clang.lib.clang_getCursorUSR(exposure.source_cursor)
                    )
                    display_name = (
                        clang.to_str(
                            clang.lib.clang_getCursorDisplayName(
                                exposure.source_cursor
                            )
                        )
                        + " (exposed via inheritance/using-declaration)"
                    )
                    source_signature = clang.to_str(
                        clang.lib.clang_getTypeSpelling(
                            clang.lib.clang_getCanonicalType(
                                clang.lib.clang_getCursorType(
                                    exposure.source_cursor
                                )
                            )
                        )
                    )
                dedup_key = (
                    str(attribution_real),
                    attribution_line,
                    usr,
                    "inherited",
                    class_usr,
                    source_signature,
                    exposure.reason,
                )
                if dedup_key in seen:
                    continue
                seen.add(dedup_key)
                findings.append(
                    Finding(
                        file=finding_path(attribution_real),
                        line=attribution_line,
                        display_name=display_name,
                        canonical_return_type=shape_description,
                        usr=usr,
                    )
                )

        def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
            kind = clang.lib.clang_getCursorKind(cursor)
            filename, _line = clang.cursor_file_and_line(cursor)
            if filename is not None:
                cursor_real = Path(filename).resolve()
                if cursor_real != source_real and cursor_real not in allowed_closure:
                    # System/external declarations are not policy-owned. In
                    # contrast, project headers in allowed_closure MUST be
                    # traversed in this exact source TU: source-local defines
                    # can reveal declarations absent from standalone wrappers.
                    return 1

            if kind in (_CXCursor_TypeAliasDecl, _CXCursor_TypedefDecl):
                record_inheritance(
                    cursor, _alias_reexported_encoders(clang, cursor)
                )
                return 1
            if kind in _RECORD_LIKE_KINDS and clang.lib.clang_isCursorDefinition(cursor):
                record_inheritance(cursor)
                return 2
            if kind not in _OUTPARAM_CHECKED_KINDS:
                return 2

            canonical = clang.lib.clang_getCanonicalCursor(cursor)
            canonical_filename, _canonical_line = clang.cursor_file_and_line(canonical)
            if canonical_filename is None:
                return 1  # CXChildVisit_Continue
            canonical_real = Path(canonical_filename).resolve()
            if canonical_real == source_real or canonical_real in allowed_closure:
                record_own(cursor, kind)
            return 1

        return visitor

    all_contexts = (
        list(contexts)
        if contexts is not None
        else _all_compile_contexts(compile_commands)
    )
    contexts_by_source: dict[Path, list[CompileContext]] = {}
    for context in all_contexts:
        if target is None or context.target == target:
            contexts_by_source.setdefault(context.source, []).append(context)

    for source in sources:
        source_real = source.resolve()
        source_contexts = contexts_by_source.get(source_real, [])
        if not source_contexts:
            raise EncoderHygieneError(
                f"No exact compile context for source {source} in "
                f"target {target or '<any registered target>'}"
            )
        for context in source_contexts:
            tu = _parse_source_as_own_tu(clang, idx, context, sysroot_args)
            try:
                violations.extend(
                    _audit_inclusion_graph(
                        clang,
                        tu,
                        source,
                        str(source.resolve()),
                        allowed_closure,
                        external_roots,
                        owned_paths,
                        context.directory,
                    )
                )
                root = clang.lib.clang_getTranslationUnitCursor(tu)
                visitor_cb = clang._visitor_func_type(make_visitor(source.resolve()))
                clang.lib.clang_visitChildren(root, visitor_cb, None)
            finally:
                clang.lib.clang_disposeTranslationUnit(tu)
    return violations, findings


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
    entries = [Path(line) for line in lines if line]
    if len(entries) != len(set(entries)):
        raise EncoderHygieneError(
            f"Manifest {path} contains duplicate path entries; ownership must be "
            "unambiguous rather than silently deduplicated"
        )
    return entries


def _canonical_source_manifest(
    entries: Sequence[Path], label: str
) -> list[Path]:
    canonical: list[Path] = []
    seen_paths: set[Path] = set()
    seen_identities: set[tuple[int, int]] = set()
    for entry in entries:
        if not entry.is_file():
            raise EncoderHygieneError(
                f"{label} source manifest entry is missing/not regular: {entry}"
            )
        real = entry.resolve()
        identity = _physical_identity(real)
        if (
            real in seen_paths
            or identity is None
            or identity in seen_identities
        ):
            raise EncoderHygieneError(
                f"{label} source manifest contains an aliased/duplicate physical "
                f"source identity: {entry} -> {real}"
            )
        seen_paths.add(real)
        seen_identities.add(identity)
        canonical.append(real)
    return canonical


def _validate_target_inventory(
    policies: dict[str, TargetPolicy],
    contexts: Sequence[CompileContext],
    external_roots: frozenset[Path],
) -> None:
    contexts_by_target: dict[str, list[CompileContext]] = {}
    for context in contexts:
        contexts_by_target.setdefault(context.target, []).append(context)

    unknown = sorted(set(contexts_by_target) - set(policies))
    stale = sorted(set(policies) - set(contexts_by_target))
    if unknown or stale:
        details = [
            *(f"  unowned compile-command target: {target}" for target in unknown),
            *(f"  registered target has no compile command: {target}" for target in stale),
        ]
        raise EncoderHygieneError(
            "Production compile-command reverse inventory is incomplete:\n"
            + "\n".join(details)
        )

    for target, policy in policies.items():
        target_contexts = contexts_by_target[target]
        if policy.classification == "EXTERNAL":
            for context in target_contexts:
                if not (
                    any(
                        context.source.is_relative_to(root)
                        for root in external_roots
                    )
                    and any(
                        context.output.is_relative_to(root)
                        for root in external_roots
                    )
                ):
                    raise EncoderHygieneError(
                        f"Target {target!r} is marked EXTERNAL but compile context "
                        f"is not physically inside registered dependency roots: "
                        f"{context.source} -> {context.output}"
                    )


def _configure_clang_build_dir(repo_root: Path, build_dir: Path) -> None:
    """Configure and build every CMake-metadata-registered production
    SCAN target in a dedicated CMake build directory using
    Clang explicitly as the compiler, independent of whatever compiler
    this project's default/main build directory happens to use
    (ubuntu-latest's default is GCC, which exposes no libclang at all).
    This is what lets this script's compile_commands.json entries be
    handed to libclang with minimal, predictable sanitization (see
    _sanitize_compile_args()) rather than guessing which of an arbitrary
    other compiler's flags libclang would accept.

    The target list comes from generated target_policy.txt rather than a
    hard-coded pair, so application/generated production units cannot be
    omitted when a target is added."""

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
    target_policies = _load_target_policies(build_dir)
    scan_targets = [
        target
        for target, policy in target_policies.items()
        if policy.classification == "SCAN"
    ]
    if not scan_targets:
        raise EncoderHygieneError(
            "CMake target policy metadata contains no production SCAN target"
        )
    subprocess.run(
        ["cmake", "--build", str(build_dir), "--target", *scan_targets],
        check=True,
        cwd=repo_root,
    )


def run_check(repo_root: Path, clang_build_dir: Path, skip_configure: bool) -> list[Finding]:
    if not skip_configure:
        _configure_clang_build_dir(repo_root, clang_build_dir)

    compile_commands_path = clang_build_dir / "compile_commands.json"
    if not compile_commands_path.is_file():
        raise EncoderHygieneError(f"{compile_commands_path} does not exist after configuring")
    try:
        compile_commands = json.loads(
            compile_commands_path.read_text(encoding="utf-8")
        )
    except (OSError, json.JSONDecodeError) as exc:
        raise EncoderHygieneError(
            f"Could not read {compile_commands_path}: {exc}"
        ) from exc
    if not isinstance(compile_commands, list) or not all(
        isinstance(entry, dict) for entry in compile_commands
    ):
        raise EncoderHygieneError(
            f"{compile_commands_path} must contain an array of compile-command objects"
        )

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
    domain_sources = _canonical_source_manifest(
        _read_manifest(generated_dir / "domain_sources.txt"), "Domain"
    )
    domain_fragments = _read_manifest(generated_dir / "domain_fragments.txt")
    foundation_headers = _read_manifest(generated_dir / "foundation_headers.txt")
    foundation_sources = _canonical_source_manifest(
        _read_manifest(generated_dir / "foundation_sources.txt"), "Foundation"
    )
    foundation_fragments = _read_manifest(generated_dir / "foundation_fragments.txt")

    all_contexts = _all_compile_contexts(compile_commands)
    target_policies = _load_target_policies(clang_build_dir)
    external_roots = _external_roots(clang_build_dir.resolve())
    _validate_target_inventory(target_policies, all_contexts, external_roots)
    contexts_by_target: dict[str, list[CompileContext]] = {}
    for context in all_contexts:
        contexts_by_target.setdefault(context.target, []).append(context)

    all_compiled_sources = {context.source for context in all_contexts}
    for label, sources in (
        ("Domain", domain_sources),
        ("Foundation", foundation_sources),
    ):
        stale_sources = sorted(set(sources) - all_compiled_sources)
        if stale_sources:
            raise EncoderHygieneError(
                f"{label} source manifest has no exact target/config compile context:\n"
                + "\n".join(f"  {source}" for source in stale_sources)
            )
    source_owner_policy = {
        **{source: "foundation" for source in foundation_sources},
        **{source: "domain" for source in domain_sources},
    }

    # Every manifest entry must physically live inside the root it claims
    # to belong to (see _validate_closure_rootedness()) *before* it is
    # trusted as a member of any allowed-inclusion closure below -- this
    # is what stops a symlink smuggled directly into a manifest itself
    # (rather than merely #include-d from elsewhere) from silently
    # widening a closure.
    domain_root = (repo_root / "src" / "domain").resolve()
    foundation_root = (repo_root / "src").resolve()
    domain_source_closure = _validate_closure_rootedness(
        domain_headers + domain_fragments, domain_root, "Domain header/fragment"
    )
    foundation_source_closure = _validate_closure_rootedness(
        foundation_headers + foundation_fragments, foundation_root, "Foundation header/fragment"
    )
    autogen_closures = _load_autogen_closures(clang_build_dir)
    autogen_by_target: dict[str, AutogenClosure] = {}
    for closures in autogen_closures.values():
        for closure in closures:
            target_policy = target_policies.get(closure.target)
            if (
                target_policy is None
                or target_policy.classification != "SCAN"
                or target_policy.policy != closure.policy
                or closure.target in autogen_by_target
            ):
                raise EncoderHygieneError(
                    f"AUTOGEN target {closure.target!r} has missing/mismatched "
                    "production target ownership metadata"
                )
            for generated, owner in closure.source_moc_owners:
                if not any(
                    context.source == owner
                    for context in contexts_by_target[closure.target]
                ):
                    raise EncoderHygieneError(
                        f"Source-local MOC {generated} owner {owner} has no exact "
                        f"compile context in target {closure.target}"
                    )
            autogen_by_target[closure.target] = closure

    domain_closure = domain_source_closure
    # foundation -> domain is the allowed dependency direction
    # (arkham_foundation legitimately links arkham_domain_models); the
    # reverse, domain -> foundation, is exactly the forbidden direction a
    # review round demonstrated was not actually enforced (see module
    # docstring) -- hence domain's own allowed closure below deliberately
    # excludes foundation_only_closure entirely.
    foundation_closure = (
        domain_source_closure
        | foundation_source_closure
    )

    libclang_path = _find_libclang()
    clang = _LibClang(libclang_path)

    is_macos = platform.system() == "Darwin"
    sysroot_args = ["-isysroot", _macos_sdk_sysroot()] if is_macos else []

    policy_contexts: dict[str, list[CompileContext]] = {}
    for policy in ("domain", "foundation", "application"):
        candidates = [
            context
            for target, target_policy in target_policies.items()
            if target_policy.classification == "SCAN"
            for context in contexts_by_target[target]
            if (
                target_policy.policy == policy
                or source_owner_policy.get(context.source) == policy
            )
        ]
        unique: dict[tuple, CompileContext] = {}
        for context in candidates:
            unique.setdefault(
                (
                    context.target,
                    context.configuration,
                    context.directory,
                    context.arguments,
                ),
                context,
            )
        policy_contexts[policy] = list(unique.values())
    if not policy_contexts["domain"] or not policy_contexts["foundation"]:
        raise EncoderHygieneError(
            "Target policy metadata must provide domain and foundation scan contexts"
        )

    idx = clang.lib.clang_createIndex(0, 0)
    if not idx:
        raise EncoderHygieneError("clang_createIndex() failed")

    # A single dedup set shared across BOTH passes below: once
    # cross-closure #includes are permitted (a header may legitimately
    # #include another member of its own closure), the same real
    # declaration can be legitimately discovered while scanning more than
    # one wrapper TU, and must only ever be recorded/counted once overall.
    seen: set[tuple] = set()
    structural_violations: list[str] = []
    generated_roots = frozenset(
        closure.root
        for policy_closures in autogen_closures.values()
        for closure in policy_closures
    )
    generated_compile_units = frozenset(
        context.source
        for context in all_contexts
        if target_policies[context.target].classification == "SCAN"
        and not context.source.is_relative_to((repo_root / "src").resolve())
    )
    owned_paths = _owned_path_policy(
        repo_root, generated_roots, generated_compile_units
    )

    try:
        findings = _scan_headers(
            clang,
            idx,
            domain_headers + domain_fragments,
            policy_contexts["domain"],
            sysroot_args,
            repo_root,
            external_roots,
            domain_source_closure,
            seen,
            structural_violations,
            owned_paths,
        )
        findings += _scan_headers(
            clang,
            idx,
            foundation_headers + foundation_fragments,
            policy_contexts["foundation"],
            sysroot_args,
            repo_root,
            external_roots,
            foundation_closure,
            seen,
            structural_violations,
            owned_paths,
        )

        base_closures = {
            "domain": domain_source_closure,
            "foundation": foundation_closure,
            "application": foundation_closure,
        }
        for target, target_policy in target_policies.items():
            if target_policy.classification != "SCAN":
                continue
            target_contexts = contexts_by_target[target]
            target_sources = sorted({context.source for context in target_contexts}, key=str)
            for context in target_contexts:
                if not context.output.is_relative_to(target_policy.binary_dir):
                    raise EncoderHygieneError(
                        f"Target {target} object output escapes its CMake BINARY_DIR: "
                        f"{context.output}"
                    )
                if not context.source.is_relative_to((repo_root / "src").resolve()) and not (
                    context.source.is_relative_to(target_policy.binary_dir)
                    or context.source.is_relative_to(target_policy.source_dir)
                ):
                    raise EncoderHygieneError(
                        f"Target {target} source is neither repo-owned nor generated "
                        f"under its exact CMake source/binary ownership: {context.source}"
                    )
            target_generated = autogen_by_target.get(target)
            effective_groups: dict[str, list[Path]] = {}
            for source in target_sources:
                effective_policy = source_owner_policy.get(
                    source, target_policy.policy
                )
                effective_groups.setdefault(effective_policy, []).append(source)
            for effective_policy, group_sources in effective_groups.items():
                allowed_closure = (
                    base_closures[effective_policy]
                    | frozenset(group_sources)
                    | (
                        target_generated.code_files
                        if target_generated is not None
                        else frozenset()
                    )
                )
                target_violations, target_findings = _scan_sources(
                    clang,
                    idx,
                    group_sources,
                    compile_commands,
                    sysroot_args,
                    repo_root,
                    external_roots,
                    allowed_closure,
                    seen,
                    owned_paths,
                    target=target,
                    contexts=all_contexts,
                )
                structural_violations.extend(target_violations)
                findings += target_findings
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
        "and builds every registered production SCAN target in (default: "
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
        "found across all explicitly-owned production targets, all "
        f"{len(ALLOWLIST)} allowlist entries accounted for at their exact "
        "expected occurrence count, zero violations, and every header, fragment, "
        "source target/configuration, and owned AUTOGEN unit stayed within its "
        "allowed domain/foundation dependency-direction closure."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
