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
resolved inclusion graph exactly like a header's.

A further review round demonstrated that this doc comment's earlier
claim -- "an out-of-line-only symbol has no way to be called from
another translation unit" -- was simply false: any TU can write its own
`extern` forward declaration of a namespace-scope, externally-linked
symbol and call it, whether or not that symbol also happens to have a
declaration in some header. Source scanning therefore now ALSO collects
QJson-family Findings for genuinely NEW, source-only declarations (see
_scan_sources()): for every function-like/constructor declaration found
directly in a real .cpp, this script asks libclang for that
declaration's *canonical* cursor (clang_getCanonicalCursor(), which
always resolves to the FIRST declaration of that entity anywhere in the
TU). If the canonical cursor's own file is a header already covered by
the header scan, the .cpp cursor is merely that header declaration's
out-of-line *definition* and is silently skipped here (it was already
counted, correctly, while scanning its header). If the canonical
cursor's own file is the .cpp itself (no earlier declaration anywhere),
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
    inspects every one of its parameters (constructors included) for a
    non-const-qualified reference/pointer whose own canonical pointee
    type is likewise QJson-family -- an output/inout parameter is
    exactly as capable of smuggling a lossy value out as a lossy return
    type is (see _is_encoder_shaped()) -- together with its USR (Unified
    Symbol Resolution -- a stable, fully qualified,
    signature-and-overload-aware identity Clang computes for every
    declaration; see https://clang.llvm.org/docs/USRs.html), access
    specifier, and exact source file.
  - For every class/struct/class-template DEFINITION whose own location
    is likewise exactly the header currently being probed, this script
    additionally walks its base-specifiers and using-declarations (see
    _inherited_and_reexported_encoders()) to discover any encoder-shaped
    member function made newly, transitively accessible through public/
    protected inheritance or a using-declaration alone, with no new
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
_CXCursor_CXXMethod = 21
_CXCursor_Namespace = 22
_CXCursor_ConversionFunction = 26
_CXCursor_FunctionTemplate = 30
_CXCursor_Constructor = 24
_CXCursor_CXXBaseSpecifier = 44
_CXCursor_UsingDeclaration = 35
_CXCursor_OverloadedDeclRef = 49
_CXCursor_FriendDecl = 603

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
_REFERENCE_OR_POINTER_TYPE_KINDS = frozenset(
    {_CXType_Pointer, _CXType_LValueReference, _CXType_RValueReference}
)

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
        num_args = clang.lib.clang_Cursor_getNumArguments(cursor)
        for arg_index in range(max(num_args, 0)):
            parm_cursor = clang.lib.clang_Cursor_getArgument(cursor, arg_index)
            parm_type = clang.lib.clang_getCursorType(parm_cursor)
            canonical_parm = clang.lib.clang_getCanonicalType(parm_type)
            if canonical_parm.kind not in _REFERENCE_OR_POINTER_TYPE_KINDS:
                continue
            pointee = clang.lib.clang_getPointeeType(canonical_parm)
            if clang.lib.clang_isConstQualifiedType(pointee):
                continue  # A const reference/pointer is an ordinary input parameter, never flagged.
            canonical_pointee = clang.lib.clang_getCanonicalType(pointee)
            pointee_spelling = clang.to_str(clang.lib.clang_getTypeSpelling(canonical_pointee))
            if _is_qjson_family(pointee_spelling):
                qualifier = "&" if canonical_parm.kind != _CXType_Pointer else "*"
                return (
                    True,
                    f"non-const output/inout parameter #{arg_index}: {pointee_spelling} {qualifier}",
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


def _inherited_and_reexported_encoders(
    clang: "_LibClang", class_cursor: "_CXCursor", depth: int = 0
) -> list:
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

    exposed: list = []
    bases: list = []

    def visit(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if kind == _CXCursor_CXXBaseSpecifier and access in _INHERITABLE_ACCESS_SPECIFIERS:
            bases.append(cursor)
        elif kind == _CXCursor_UsingDeclaration and access in _INHERITABLE_ACCESS_SPECIFIERS:
            for target in _resolve_using_declaration_targets(clang, cursor):
                exposed.append((target, cursor))
        return 1  # CXChildVisit_Continue: never descend into member function bodies here.

    cb = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(class_cursor, cb, None)

    for base_specifier in bases:
        base_type = clang.lib.clang_getCursorType(base_specifier)
        base_decl = clang.lib.clang_getTypeDeclaration(base_type)

        def visit_base_member(cursor: "_CXCursor", _parent: "_CXCursor", _client_data, _base=base_specifier) -> int:
            member_kind = clang.lib.clang_getCursorKind(cursor)
            member_access = clang.lib.clang_getCXXAccessSpecifier(cursor)
            if member_kind in _FUNCTION_LIKE_KINDS and member_access in _INHERITABLE_ACCESS_SPECIFIERS:
                exposed.append((cursor, _base))
            return 1  # CXChildVisit_Continue

        member_cb = clang._visitor_func_type(visit_base_member)
        clang.lib.clang_visitChildren(base_decl, member_cb, None)

        exposed.extend(_inherited_and_reexported_encoders(clang, base_decl, depth + 1))

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
    external_roots: frozenset[Path],
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
        real = included_path.resolve()
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
        location = clang.lib.clang_getLocation(tu, included_file, 1, 1)
        if clang.lib.clang_Location_isInSystemHeader(location):
            continue  # A genuine compiler/system/toolchain header (see this function's own doc comment).
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
    genuinely external/trusted-generated (never subject to the domain/
    foundation dependency-direction closure check), independent of the
    blanket "anything under the build directory" (or, later, "anything
    outside repo_root") exemption two separate review rounds
    demonstrated was unsound (see _audit_inclusion_graph()'s own doc
    comment for the exact bypasses this replaces).

    Read from `<clang-build-dir>/generated/external_roots.txt`, which
    CMakeLists.txt/cmake/PathManifest.cmake populate from two distinct,
    always CMake-metadata-derived (never hand-authored/lexically
    guessed) sources:

      - The REAL, FetchContent-populated `qtkeychain_SOURCE_DIR`/
        `qtkeychain_BINARY_DIR` variables (written eagerly, right after
        `FetchContent_MakeAvailable(qtkeychain)`) -- genuine third-party
        dependency package metadata, replacing a previous
        `<clang-build-dir>/_deps` lexical guess that happened to work
        only because this project currently has exactly one FetchContent
        dependency at exactly that conventional location.
      - Each production target's own AUTOMOC `AUTOGEN_BUILD_DIR` (see
        arkham_append_target_autogen_root() in cmake/PathManifest.cmake,
        appended once both targets are fully configured) -- needed
        because a real build showed that AUTOMOC's own generated
        `mocs_compilation.cpp` (already independently scanned as a
        SOURCE) transitively `#include`s per-class `moc_*.cpp`
        fragments physically written under that directory, which are
        mechanically generated in full by Qt's `moc` tool directly from
        an already-audited Q_OBJECT/Q_GADGET header and can never
        introduce a public API surface of their own, so requiring them
        to have their own manifest entry would be auditing generated
        boilerplate that cannot possibly differ from what its source
        header already declares.

    Either way, this stays correct automatically if qtkeychain's own
    FetchContent declaration changes, a new dependency is added, or
    AUTOGEN_BUILD_DIR's own CMake default ever changes, with no change
    needed to this script."""

    manifest = clang_build_dir / "generated" / "external_roots.txt"
    return frozenset(root.resolve() for root in _read_manifest(manifest))


def _scan_headers(
    clang: _LibClang,
    idx: ctypes.c_void_p,
    headers: Sequence[Path],
    compile_args: list[str],
    sysroot_args: list[str],
    repo_root: Path,
    external_roots: frozenset[Path],
    allowed_closure: frozenset[Path],
    seen: set[tuple],
    structural_violations: list[str],
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

    `seen` is a single dedup set shared across the *entire* run (both
    the domain and foundation passes, and both header and source scans):
    each entry is either a 3-tuple `(resolved file, line, USR)` for an
    own-declaration Finding, or a 4-tuple
    `(resolved file, line, USR, "inherited")` for an inheritance/using-
    declaration-exposure Finding -- the differing tuple shapes guarantee
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
        filename, line = clang.cursor_file_and_line(cursor)
        if filename is None:
            return
        real = Path(filename).resolve()
        if real not in allowed_closure:
            return
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if access not in _PUBLIC_ACCESS_SPECIFIERS:
            return
        is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
        if not is_shaped:
            return
        usr = clang.to_str(clang.lib.clang_getCursorUSR(cursor))
        record_if_new(
            dedup_key=(str(real), line, usr),
            real=real,
            line=line,
            display_name=clang.to_str(clang.lib.clang_getCursorDisplayName(cursor)),
            shape_description=shape_description,
            usr=usr,
        )

    def handle_inheritance_exposure(class_cursor: _CXCursor) -> None:
        filename, _def_line = clang.cursor_file_and_line(class_cursor)
        if filename is None:
            return
        if Path(filename).resolve() not in allowed_closure:
            return
        for source_cursor, attribution_cursor in _inherited_and_reexported_encoders(clang, class_cursor):
            source_kind = clang.lib.clang_getCursorKind(source_cursor)
            is_shaped, shape_description = _is_encoder_shaped(clang, source_cursor, source_kind)
            if not is_shaped:
                continue
            attribution_filename, attribution_line = clang.cursor_file_and_line(attribution_cursor)
            if attribution_filename is None:
                continue
            attribution_real = Path(attribution_filename).resolve()
            if attribution_real not in allowed_closure:
                continue
            usr = clang.to_str(clang.lib.clang_getCursorUSR(source_cursor))
            record_if_new(
                dedup_key=(str(attribution_real), attribution_line, usr, "inherited"),
                real=attribution_real,
                line=attribution_line,
                display_name=(
                    f"{clang.to_str(clang.lib.clang_getCursorDisplayName(source_cursor))} "
                    "(exposed via inheritance/using-declaration)"
                ),
                shape_description=shape_description,
                usr=usr,
            )

    def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        if kind in _RECORD_LIKE_KINDS and clang.lib.clang_isCursorDefinition(cursor):
            handle_inheritance_exposure(cursor)
            return 2  # CXChildVisit_Recurse: still walk this class's own direct members normally.
        if kind in _OUTPARAM_CHECKED_KINDS:  # Superset of _FUNCTION_LIKE_KINDS, includes constructors.
            handle_own_declaration(cursor, kind)
            return 1  # CXChildVisit_Continue: do not descend into the body.
        return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

    visitor_cb = clang._visitor_func_type(visitor)

    for header in headers:
        tu, wrapper_filename = _parse_header_as_own_tu(clang, idx, header, compile_args, sysroot_args)
        structural_violations.extend(
            _audit_inclusion_graph(clang, tu, header, wrapper_filename, allowed_closure, external_roots)
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
    seen: set[tuple],
) -> tuple[list[str], list[Finding]]:
    """Independently parse every REAL production .cpp in `sources` as its
    own translation unit (see _parse_source_as_own_tu()) -- each with its
    own exact compile_commands.json entry, never a borrowed/
    "representative" one -- and:

      - audit its complete resolved inclusion graph against
        `allowed_closure`, exactly like _scan_headers() already does for
        headers/fragments (see _audit_inclusion_graph(), reused
        unchanged here: a source's own file is skipped from the graph
        the identical way a header's own wrapper "main file" entry is,
        by passing the source's own path as the self-filtering
        `wrapper_filename` argument);
      - collect a Finding for every genuinely NEW, source-only,
        externally-linked, encoder-shaped declaration this source
        introduces -- i.e. one with no earlier declaration anywhere,
        such as a namespace-scope `QJsonObject encodeDeck(const
        DeckList&)` written directly in a .cpp with no header
        declaration at all, which a review round demonstrated compiles
        and remains callable from another translation unit via an
        ad-hoc `extern` forward declaration despite this script
        previously collecting zero findings from source scanning.

    An out-of-line DEFINITION of an already header-declared symbol (e.g.
    src/domain/RawJson.cpp's `Value::toExactQJson()`, or
    src/AuthModels.cpp's `AuthenticateRequest::toJson()`) is correctly
    NOT re-flagged here: for each function-like/constructor cursor found
    in this source, clang_getCanonicalCursor() is used to resolve it to
    its own FIRST declaration anywhere in the TU. When that canonical
    cursor's own file is a header already covered by _scan_headers()
    (i.e. a member of `allowed_closure`), this cursor is merely that
    header declaration's out-of-line definition, already correctly
    counted while scanning its header, and is silently skipped. Only
    when the canonical cursor's own file is the .cpp itself (no earlier
    declaration anywhere) -- and that declaration has genuinely EXTERNAL
    linkage (clang_getCursorLinkage() == CXLinkage_External; a `static`-
    or anonymous-namespace-scoped helper has internal/unique-external
    linkage and categorically cannot be referenced from another
    translation unit, so is correctly never flagged) -- is a new Finding
    recorded, keyed by the *source*'s own repo-relative path (which can
    never match any ALLOWLIST entry, since every entry is keyed to a
    header path), so it always, correctly, classifies as a violation.

    `seen` is the SAME whole-run dedup set _scan_headers() uses (3-tuple
    `(resolved file, line, USR)` entries): a source-only declaration's
    dedup key uses the source's own resolved path, so it can never
    collide with a header-scan entry, but a source scanned more than
    once (impossible in this script's normal flow, since each manifest
    entry is scanned exactly once, but kept for defensive consistency
    with _scan_headers()) would still be deduplicated correctly."""

    violations: list[str] = []
    findings: list[Finding] = []

    def make_visitor(source_real: Path):
        def visitor(cursor: _CXCursor, _parent: _CXCursor, _client_data) -> int:
            kind = clang.lib.clang_getCursorKind(cursor)
            filename, _line = clang.cursor_file_and_line(cursor)
            if filename is not None and Path(filename).resolve() != source_real:
                # This cursor (and everything beneath it) belongs entirely to a
                # different, transitively-#include-d file -- e.g. a project header
                # already independently covered by _scan_headers(), or a Qt/system
                # header entirely out of scope here. Declarations reached only by
                # recursing into another file's own AST must never be attributed to
                # THIS source, so this subtree is not descended into at all.
                return 1  # CXChildVisit_Continue

            if kind in _RECORD_LIKE_KINDS:
                return 2  # CXChildVisit_Recurse: a source-defined class/struct is not itself
                # flagged here (constructing new *types* in a .cpp is not how this bypass
                # works; only a source-only *function*/constructor declaration is), but its
                # own members, physically written in this same source, must still be
                # visited normally.
            if kind not in _OUTPARAM_CHECKED_KINDS:  # Superset of _FUNCTION_LIKE_KINDS, includes constructors.
                return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

            canonical = clang.lib.clang_getCanonicalCursor(cursor)
            canonical_filename, canonical_line = clang.cursor_file_and_line(canonical)
            if canonical_filename is None:
                return 1  # CXChildVisit_Continue: no location at all (e.g. built-in); nothing to check.
            canonical_real = Path(canonical_filename).resolve()
            if canonical_real in allowed_closure:
                return 1  # Already declared (and already counted) in a scanned header; this source
                # cursor is merely that declaration's out-of-line definition.
            if not canonical_real.is_relative_to(repo_root):
                return 1  # The canonical declaration belongs to an entirely external (e.g. Qt/
                # system) header -- this is a specialization/instantiation of pre-existing
                # external API, not a new source-only project declaration.

            linkage = clang.lib.clang_getCursorLinkage(cursor)
            if linkage != _CXLinkage_External:
                return 1  # `static`/anonymous-namespace-scoped: cannot be referenced from another TU.

            access = clang.lib.clang_getCXXAccessSpecifier(cursor)
            if access not in _PUBLIC_ACCESS_SPECIFIERS:
                return 1  # A private/protected member of a class defined only in this source is not
                # reachable from outside that class regardless of its own linkage.

            is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
            if not is_shaped:
                return 1

            usr = clang.to_str(clang.lib.clang_getCursorUSR(cursor))
            dedup_key = (str(canonical_real), canonical_line, usr)
            if dedup_key not in seen:
                seen.add(dedup_key)
                findings.append(
                    Finding(
                        file=canonical_real.relative_to(repo_root).as_posix(),
                        line=canonical_line,
                        display_name=clang.to_str(clang.lib.clang_getCursorDisplayName(cursor)),
                        canonical_return_type=shape_description,
                        usr=usr,
                    )
                )
            return 1

        return visitor

    for source in sources:
        tu = _parse_source_as_own_tu(clang, idx, source, compile_commands, sysroot_args)
        try:
            violations.extend(
                _audit_inclusion_graph(
                    clang, tu, source, str(source.resolve()), allowed_closure, external_roots
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

        # A review round demonstrated this script previously never
        # independently parsed a single real source file at all --
        # domain_sources.txt/foundation_sources.txt were read only to
        # borrow one "representative" compile-args list for scanning
        # *headers* -- so a production .cpp could #include an
        # absolute/"../"-relative/symlinked/generated forbidden header,
        # inherit whatever lossy encoders it declares, and compile with
        # this check staying green. A further review round demonstrated
        # that even after independent source parsing was added, it only
        # ever checked the inclusion graph, never collecting new
        # QJson-family Finding objects at all -- a genuinely new,
        # source-only, externally-linked encoder declaration (with no
        # header declaration anywhere) passed unaudited (see
        # _scan_sources()'s own doc comment for exactly how this is now
        # closed via clang_getCanonicalCursor()/clang_getCursorLinkage()).
        # Each source here is parsed with its own exact
        # compile_commands.json entry (never a borrowed one), and both
        # source manifests are themselves generated directly from
        # arkham_domain_models'/arkham_foundation's own live CMake
        # SOURCES/INTERFACE_SOURCES (+ AUTOMOC-generated
        # mocs_compilation.cpp, when applicable) metadata (see
        # arkham_write_target_source_manifest() in
        # cmake/PathManifest.cmake) -- never a hand-authored variable --
        # so this scan can never silently miss a source added via a
        # later target_sources() call, an INTERFACE_SOURCES entry, or a
        # Qt AUTOMOC-generated compilation unit either.
        domain_source_violations, domain_source_findings = _scan_sources(
            clang,
            idx,
            domain_sources,
            compile_commands,
            sysroot_args,
            repo_root,
            external_roots,
            domain_closure,
            seen,
        )
        structural_violations.extend(domain_source_violations)
        findings += domain_source_findings

        foundation_source_violations, foundation_source_findings = _scan_sources(
            clang,
            idx,
            foundation_sources,
            compile_commands,
            sysroot_args,
            repo_root,
            external_roots,
            foundation_closure,
            seen,
        )
        structural_violations.extend(foundation_source_violations)
        findings += foundation_source_findings
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
