#!/usr/bin/env python3
"""Prove, via real compiler-AST inspection (libclang), that no declaration,
type surface, or function-body construction in any production header/source
context semantically references a QJsonObject/QJsonArray/QJsonValue-family
type, except for a tiny, explicitly enumerated positive allowlist:

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
    arkham_foundation target). Its outbound exceptions remain exactly
    AuthModels.h's two request-body encoders; its inbound decoder
    signatures are separately pinned by exact identity/signature/access.
  - Every compile command owned by the application target, including
    src/main.cpp, AUTOMOC, QML type-registration/cache, and RCC-generated
    units, under the foundation/application closure. No app-local encoder
    exception exists.

Every compile-command target is reverse-inventoried against explicit CMake
target metadata. Unknown production targets fail; external, test, and
try-compile commands are excluded only by exact target records.
INTERFACE header contexts are derived from a CMake-evaluated link graph
generated separately for every compiled consumer and audited configuration.
Provider usage requirements are evaluated in the consumer's target-property
context. The graph crosses imported and exempt wrappers, resolves nested
generator expressions before Python reads it, applies transitive
INTERFACE_LINK_LIBRARIES_DIRECT injection followed by
INTERFACE_LINK_LIBRARIES_DIRECT_EXCLUDE precedence, and rejects any
unevaluated or unclassified result rather than guessing target names from
generator-expression text. A second structurally normalized compile graph
preserves nested COMPILE_ONLY payloads which intentionally disappear from
link-context evaluation. Its traversal starts from the final post-DIRECT,
post-DIRECT_EXCLUDE dependency set as well as genuine compile-only roots, so
an injected plugin's own compile-only usage cannot disappear by composition.

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
this is a genuinely new declaration and is recorded regardless of
external/internal/static/anonymous linkage or public/protected/private
access. Linkage and access are part of its exact allowance identity.

A later series of reviews demonstrated that output-channel inference is
itself an open-ended category error: pointer wrappers, arrays, callbacks,
functors, and dependent bases can all expose the same wire type through
different language machinery. The policy is therefore a closed positive
boundary, not an output classifier. Every production
function, constructor, conversion, alias, field, variable, and base is
recursively searched for any semantic reference to QJsonObject,
QJsonArray, QJsonValue, QJsonDocument, or QVariant JSON containers.
Constness and presumed direction do not exempt it. The only accepted
references are exact physical path/location + USR + full canonical
signature digest + access + linkage + occurrence count + owning
target/config observation entries below, covering the intentional inbound decoders
and the bounded RawJson/Auth adapters.

An even earlier version of this check (see git history:
tests/EncoderHygieneTests.cpp) was a purely source-text regex/parser,
which repeated review rounds proved could not keep up with an
open-ended set of textual evasions: `auto&` returns, `decltype(...)`
returns, type aliases, macro-defined return types from an
included/generated header, conversion operators, overloads/duplicate
identical class signatures colliding in a basename-keyed allowlist, and
comment/raw-string-literal desynchronization of the stripper. Every one
of those is a *textual* disguise for the exact same *semantic* fact --
"this declaration or expression's type, after full compiler resolution of
aliases/templates/decltype/auto/macros, is in the QJson family" -- which
only a real C++ compiler frontend can determine with certainty. This
script asks Clang to determine that fact directly, via libclang's AST
(https://clang.llvm.org/docs/LibClang.html), rather than re-implementing
an ever-growing fragment of C++ parsing by hand:

  - For every function-like declaration at every access/linkage (ordinary methods, free
    functions, friend functions, conversion operators, function
    templates, constructors) whose *own* location (after macro
    expansion) is exactly the one header currently being probed, this
    script asks libclang for the *canonical* result type (i.e. with
    every typedef/using-alias/decltype/auto/template parameter already
    resolved to its underlying real type by the compiler itself) AND
    inspects every parameter recursively through canonical references,
    arrays, functions, aliases, records, bases, and template
    instantiations, together with its USR (Unified
    Symbol Resolution -- a stable, fully qualified,
    signature-and-overload-aware identity Clang computes for every
    declaration; see https://clang.llvm.org/docs/USRs.html), access
    specifier, and exact source file.
  - For every class/struct/class-template definition and
    type alias whose own location
    is likewise exactly the header currently being probed, this script
    additionally walks its base-specifiers and using-declarations (see
    _inherited_and_reexported_encoders()) to discover any forbidden-type
    member function made newly, transitively accessible through public/
    protected/dependent inheritance, an alias, or a using-declaration alone, with no new
    textual declaration of its own -- attributing the resulting Finding
    to the EXPOSING class's own file/line rather than the original
    declaring class's file, so it cannot masquerade as an
    already-audited, already-allowlisted symbol.
  - For every production function body, this also audits local/static
    variables, local classes and lambdas, construction/call expressions,
    casts, initializers, and materialized temporaries. A void-returning
    submit function therefore cannot hide a QJsonDocument or QJsonObject
    construction merely because its outer declaration has no wire type.
    Lambda parameters and concrete function-template TypeRef arguments are
    independent surfaces; unresolved dependent body TypeRefs fail closed.
    Namespace-scope explicit instantiations/specializations are compiler-
    tokenized and matched to Clang's canonical specialization AST, even when
    the primary template is declared in an external/system header and
    libclang emits no project-owned cursor for the instantiation spelling.
    Source TUs are tokenized after preprocessing, so nested/argument macro
    expansions retain both their production invocation and macro-definition
    spelling locations; governed headers/fragments receive the identical
    per-context treatment. Matching specialization nodes are then traversed
    through their complete instantiated signature, body declarations,
    fields, bases, constructors, conversions, aliases, and members rather
    than accepting a safe-looking template argument list alone.
    Every numeric cursor kind used here is checked against the installed
    libclang's own clang_getCursorKindSpelling result at startup.
  - A declaration is a *violation* if any semantic type component is in
    the forbidden family and its exact file, USR, canonical semantic
    signature, access, and occurrence count are not one ALLOWLIST entry
    below. There is no general "looks like a decode
    helper" heuristic
    (e.g. "takes a QJson parameter, so it must be inbound-only") -- that
    itself would be a new textual/structural loophole (e.g. a lossy
    per-DTO `toJson(QJsonObject seed)` padded with an unused QJson-typed
    parameter purely to slip past such a rule). Every legitimate
    exception is named explicitly, by exact qualified USR/signature/access, exact expected
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
it). Repeats are deduplicated only within the same exact
target/configuration/TU observation. Physical occurrences and their
complete observation sets are aggregated afterward and pinned, so a
shared header's normal repeated observations remain explicit while no
additional target, configuration, TU, macro expansion, or relocated
declaration can collapse into a previously allowed occurrence.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import glob as globmod
import hashlib
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
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Iterator, Sequence


# --- The tiny, explicit allowlists ------------------------------------------
#
# Every entry names a source_file (a full path, relative to the repo root,
# with forward slashes -- portable across checkouts/CI runners, and never
# a bare basename), qualified USR, full instantiated signature, access/linkage,
# physical expansion+spelling identity, exact target/configuration/TU
# observation set, and expected physical occurrence count. A declaration is
# permitted only if every dimension matches.


def _stable_usr(usr: str) -> str:
    return usr.replace("@N@std@N@__1", "@N@std").replace(
        "@N@std@N@__cxx11", "@N@std"
    )


@dataclass(frozen=True)
class AllowlistEntry:
    file: str  # repo-root-relative, forward-slash path, e.g. "src/domain/RawJson.h"
    usr: str
    expected_count: int = 1
    semantic_signature: str | None = None
    access: int | None = None
    linkage: int | None = None
    full_signature_sha256: str | None = None
    physical_identity_sha256: str | None = None
    observation_set_sha256: str | None = None

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


# These pins cover dimensions which cannot be represented portably by a
# declaration USR alone. The first digest covers the complete physical
# spelling/expansion identity and exact target/configuration/TU observation set
# for every named ALLOWLIST declaration. The second covers every legitimate
# function-body occurrence (locals, temporaries, constructions, calls, lambdas,
# and local classes). Both are exact set pins: adding, removing, moving, or
# observing a surface in one more context changes the digest and fails closed.
_NAMED_ALLOWLIST_IDENTITY_SET_SHA256 = (
    "04096034a5fb3bb78d8587a5360fec919df7e2a5eaf948192b311cb94956f08d"
)
_LOCAL_WIRE_SURFACE_SET_SHA256 = (
    "c967eb44fdf62f07b8d9d0c9c6cf70bfcb9698d8acc3f7c86617a30f69c95128"
)


# src/domain/RawJson.h: the three canonical, production-bounded exact
# adapters. These are the *only* encoding-direction (domain data -> QJson)
# public conversions permitted anywhere in the domain-model header set.
_CANONICAL_ADAPTERS = (
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJson#1", semantic_signature="result=Arkham::ValueOrError<QJsonValue>", access=1),
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonObject#1", semantic_signature="result=Arkham::ValueOrError<QJsonObject>", access=1),
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonArray#1", semantic_signature="result=Arkham::ValueOrError<QJsonArray>", access=1),
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
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@N@detail@F@findField#&1$@S@QJsonObject#$@S@QLatin1String#", semantic_signature="result=std::optional<QJsonValue>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObject#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonObject>;param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArray#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonArray>;param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonObject>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonArray>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireRawField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonValue>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawArrayField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonValue>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalRawObjectField#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="result=Arkham::ValueOrError<QJsonValue>;param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@objectMembers#&1$@S@QJsonObject#", semantic_signature="result=QList<std::pair<QString, QJsonValue>>;param[0]=const QJsonObject &", access=0),
)

_INBOUND_DOMAIN_SURFACES = (
    AllowlistEntry("src/domain/CardCatalog.h", "c:@N@Arkham@S@SkillIcon@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/CardCatalog.h", "c:@N@Arkham@S@CardCost@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/CardCatalog.h", "c:@N@Arkham@S@GameValue@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/CardCatalog.h", "c:@N@Arkham@S@CardDef@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@ExternalDeckId@F@fromObject#&1$@S@QJsonObject#$@S@QStringView#S", semantic_signature="param[0]=const QJsonObject &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@DeckListInput@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@DeckList@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@Deck@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@CreateDeckRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@FetchDeckRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@DeckValidationError@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@DeckValidationResult@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Decks.h", "c:@N@Arkham@S@DeckOperationError@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.cpp", "c:@N@Arkham@F@decodeInvestigatorRefValue#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/Games.cpp", "c:@N@Arkham@F@decodeDeckListInputValue#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@InvestigatorSummary@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@ScenarioSummary@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@CampaignSummary@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@GameState@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@GameListRow@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@F@decodeGameList#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@CampaignOption@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@CampaignOptionRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@CampaignOrScenario@F@fromJson#&1$@S@QJsonObject#$@S@QStringView#S", semantic_signature="param[0]=const QJsonObject &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@CreateGameRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@ChooseDeckRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@S@ClaimSeatRequest@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Games.h", "c:@N@Arkham@F@decodeOpenSeats#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/Identifiers.h", "c:@N@Arkham@ST>1#T@TypedId@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Identifiers.h", "c:@N@Arkham@ST>1#T@NonEmptyString@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Identifiers.h", "c:@N@Arkham@S@CardCode@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/Identifiers.h", "c:@N@Arkham@S@CardName@F@fromJson#&1$@S@QJsonValue#$@S@QStringView#S", semantic_signature="param[0]=const QJsonValue &", access=1),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@typeName#&1$@S@QJsonValue#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@fieldPresence#&1$@S@QJsonObject#$@S@QLatin1String#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireStringValue#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireIntValue#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireBoolValue#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireString#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireInt#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireBool#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalString#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalInt#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalBool#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalNonNullString#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalNonNullInt#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@optionalNonNullBool#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireNullableString#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@requireNullableInt#&1$@S@QJsonObject#$@S@QLatin1String#$@S@QStringView#", semantic_signature="param[0]=const QJsonObject &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@decodeUuid#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@decodeNullableUuid#&1$@S@QJsonValue#$@S@QStringView#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/JsonDecode.h", "c:@N@Arkham@N@Json@F@toLosslessRaw#&1$@S@QJsonValue#", semantic_signature="param[0]=const QJsonValue &", access=0),
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@fromQJson#&1$@S@QJsonValue#S", semantic_signature="param[0]=const QJsonValue &", access=1),
)

_INTERNAL_DOMAIN_SURFACES = (
    AllowlistEntry("src/domain/RawJson.h", "c:@N@Arkham@N@Json@S@Value@F@toExactQJsonInner#&1$@N@Arkham@N@Json@S@ParseLimits#I#&K#1"),
    AllowlistEntry("src/domain/CardCatalog.cpp", "c:CardCatalog.cpp@N@Arkham@aN@F@decodeCardCodeValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/CardCatalog.cpp", "c:CardCatalog.cpp@N@Arkham@aN@F@decodeCardNameValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/CardCatalog.cpp", "c:CardCatalog.cpp@N@Arkham@aN@F@decodeSkillIconValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/CardCatalog.cpp", "c:CardCatalog.cpp@N@Arkham@aN@F@decodeCardCostValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/CardCatalog.cpp", "c:CardCatalog.cpp@N@Arkham@aN@F@decodeGameValueValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/Decks.cpp", "c:Decks.cpp@N@Arkham@aN@F@decodeInvestigatorRefValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/Decks.cpp", "c:Decks.cpp@N@Arkham@aN@F@decodeExternalDeckId#&1$@S@QJsonObject#$@S@QStringView#"),
    AllowlistEntry("src/domain/Decks.cpp", "c:Decks.cpp@N@Arkham@aN@F@decodeDeckListValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/Games.cpp", "c:Games.cpp@N@Arkham@aN@F@decodeGameStateValue#&1$@S@QJsonValue#$@S@QStringView#"),
    AllowlistEntry("src/domain/Games.cpp", "c:Games.cpp@N@Arkham@aN@F@decodeGameListRow#&1$@S@QJsonValue#$@S@QStringView#"),
)

DOMAIN_ALLOWLIST: tuple[AllowlistEntry, ...] = (
    _CANONICAL_ADAPTERS
    + _DECODE_HELPERS
    + _INBOUND_DOMAIN_SURFACES
    + _INTERNAL_DOMAIN_SURFACES
)

# src/AuthModels.h: the two legitimate foundation-layer request-body
# encoders. These deliberately live outside the domain-model header set
# (they carry secrets -- see AuthModels.h's own module comment on why
# they have no QDebug/toString either) and are the *only* QJson-returning
# public declarations permitted anywhere in the foundation header set.
FOUNDATION_ALLOWLIST: tuple[AllowlistEntry, ...] = (
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@AuthenticateRequest@F@toJson#1", semantic_signature="result=QJsonObject", access=1),
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@RegisterRequest@F@toJson#1", semantic_signature="result=QJsonObject", access=1),
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@AuthToken@F@fromJson#&1$@S@QJsonObject#S", semantic_signature="param[0]=const QJsonObject &", access=1),
    AllowlistEntry("src/AuthModels.h", "c:@N@Arkham@S@CurrentUser@F@fromJson#&1$@S@QJsonObject#S", semantic_signature="param[0]=const QJsonObject &", access=1),
    AllowlistEntry("src/ServerCapabilities.h", "c:@N@Arkham@S@ServerCapabilities@F@fromJson#&1$@S@QJsonObject#S", semantic_signature="param[0]=const QJsonObject &", access=1),
    AllowlistEntry("src/NetworkAuthenticationClient.h", "c:@N@Arkham@S@NetworkAuthenticationClient@F@issueTokenRequest#&1$@N@Arkham@S@ServerProfile#$@S@QStringView#&1$@S@QJsonObject#$@N@std@N@__1@S@function>#Fv(#$@N@Arkham@S@AuthResult>#$@N@Arkham@S@AuthToken)#"),
    AllowlistEntry("src/AuthModels.cpp", "c:AuthModels.cpp@N@Arkham@aN@F@jsonTypeName#&1$@S@QJsonValue#"),
    AllowlistEntry("src/AuthModels.cpp", "c:AuthModels.cpp@N@Arkham@aN@F@requireString#&1$@S@QJsonObject#$@S@QLatin1String#"),
    AllowlistEntry("src/AuthModels.cpp", "c:AuthModels.cpp@N@Arkham@aN@F@requireBool#&1$@S@QJsonObject#$@S@QLatin1String#"),
    AllowlistEntry("src/ServerCapabilities.cpp", "c:ServerCapabilities.cpp@N@Arkham@aN@F@jsonTypeName#&1$@S@QJsonValue#"),
    AllowlistEntry("src/ServerCapabilities.cpp", "c:ServerCapabilities.cpp@N@Arkham@aN@F@requireString#&1$@S@QJsonObject#$@S@QLatin1String#"),
    AllowlistEntry("src/QSettingsProfileStore.h", "c:@N@Arkham@S@QSettingsProfileStore@FI@m_ownedSettings"),
    AllowlistEntry("src/QSettingsProfileStore.h", "c:@N@Arkham@S@QSettingsProfileStore@FI@m_settings"),
)

ALLOWLIST: tuple[AllowlistEntry, ...] = DOMAIN_ALLOWLIST + FOUNDATION_ALLOWLIST

# SHA-256 of each entry's complete canonical declaration signature, in
# ALLOWLIST order. The signature includes result, every parameter,
# owner/type, static/member, cv/ref, variadic, exception, calling
# convention, ref qualifier, and the recursive forbidden-type proof path.
_FULL_SIGNATURE_SHA256 = (
    "99a59561baeaa62ba82ebafaf80a8a3dcc53f45bd34abc562350e448aa7be61d",
    "13bb4ed4a5f6e9e00e367ab417c646b5dd1ebd525cd6bef6581adf190e0b3312",
    "1e9ef271f67e8dd37f6f68151df8135e8b376ef995c0427336d27d591d64f748",
    "a5a6dcf0fbecd37f31da1ce28ba7a76c6f95ed2f1416dce4f9618d54cbce19fd",
    "de0f89fd01f7fe3aa6098efd3e742440231af2bddb8c4aa0ecc5a59438014d6d",
    "d654379d3da9c4a5a3c6aa650a72885730af4372fdf752c8435a43875331460f",
    "64876764a7ae875fcc23aa95f16f32f74a53e3619bd004b914a4d9942d35862f",
    "7afd07c61cfbaa4e4c5daae60b40d1b0ea1d6db516080db839d5ce126dd8f594",
    "08e00595f3d34bed12b77f808a153a3b3f2b1afc0f3cab535abb0e45fa61bb4f",
    "08e00595f3d34bed12b77f808a153a3b3f2b1afc0f3cab535abb0e45fa61bb4f",
    "08e00595f3d34bed12b77f808a153a3b3f2b1afc0f3cab535abb0e45fa61bb4f",
    "a8ef4111fce844fe7a38b85d7e1687a930c660791d2332e03f089f5f66bd81f7",
    "19552113caab7e22682b95bb7d25bfd8af5920fd129f8a7d43d5f9845432bdb8",
    "f24a0cf7292a965996aa9f863fb9298bace7b1677b0c8aa19c95eee83c5df35f",
    "88c0afddcf5adb32a2aa3563c13142735de8ccbe98d6b82a731c2f99befc3a00",
    "e6e824baef210f539225cbd946a04a581537ec50ab565e0ada89c73570b572ea",
    "ca36746d7320b6f1f1acc72cf2c8031fa3a2d8be51580146268510988676c6ae",
    "90f0547ebbb6d6cdaa99aa4371b3f2acc614fef755413fc5df2ab6536b7c9fca",
    "0470f1f8d0ac5e2b272dfec3b310e643c1739c6cf759f39250fdc9f9ed46f5a2",
    "36dce58dd6eb5c125fc8dfe340203ead2d9454620f97f0ffa5d0d339e3806a3f",
    "49f006cf3358b300141ed235601c7b5d5e3fb473dd722a48366ff21741e88f07",
    "04cc5684cb1b361211858efdd59b22865f3548d4738b9eda838f821a59af3b48",
    "8412f8cce63607383f9dea545d2acd821f205fff8b674460977c52f0da91d923",
    "db6193986aeeacbe9d4a226e65a9c16bb4b996d6510d5ffe6bdbff06c432935e",
    "6ea005d54adfbfa5962bd0ff876c7925d0dc5934e1b8c0eb2eaff5cfc21024ed",
    "fa8539aa6e17bd040e551b675fcc473464beae9e12e0ef16dddc0efb217c816d",
    "a12b65525c5671b1705143e1cf1493420d836681f2959fe52344854eb9eb940d",
    "8dbf9088d591d44f95db7dcc0ffe1200b808860e96112486c25cc98d33a72d7a",
    "2300469f61a57d33424aa1840831d33e01b724ba539c1799ac019879512f83d2",
    "d296108d1162449e25d0853d11326daafd82ca0be28bc90183c3e3bc123e6af9",
    "b74ac3b4d9b229babbae68a02805f725dd695f18d66568d34924bffaf175b1c7",
    "3f0611a2b2a389c392b2a849f128f11c5a2dc0582f739787280a5db4c0ab7f17",
    "5c26050edbcd97786d708163743f8b917bcf2dee91d40bc3644540a5416fcc41",
    "a263d4116d6251ff1b507d584dc68b1ff1a00260ed281d12090114c873601eb8",
    "8cbb5d3f23b3d56eab067bccdf54de48278f38e3646c47af8f4c206db8c74972",
    "02f107255bdb41a7d5a88cc10f6dbd688165d9c82fba66918e32e86e58f3675b",
    "ad97b9b0a7ba39dd244d630528dd85c130d96316692679242cbe49f5b742a0f1",
    "b3ce70313afab0067eb7e9c4961b6574d4057b0abc604a06c7576869ac571ce4",
    "e15d29f7e94439a7dee1a973ee10d0a4cbf4cce8ac5853d44b5f64e9cdd34bf3",
    "9ce7310597e3081990d2359d39bb1a1bbeab0a0878aed99218ed3ded813a987e",
    "0a94432e3b1b105eaf818aa4f58b27ba28ab4c8e364b969089c937bda50e8b04",
    "a2983b1b32ecbc2931890974b15313a3d3c4a5468a7aa92b528be3a4d344d19c",
    "505da9b1d5607d245ca03f4bb1a0656430b4253f75cced1add6e97144649a163",
    "c35a8813693950ae3e3e91115192f5ec408d8710432eb51373c1de25c99052d5",
    "f504ace951b113e52d3f417a9d3fc210c3c0d16ec2550aae03091f7ac9afa6fd",
    "e320575411954410fc8e96756e254d611b2454743f3965002ddc11a79597d44a",
    "ba879a1d82f8ba68e5f6d308c240bfe3481ebef7f59b6c35ae25fcc586422a58",
    "2df054407962f11b2161d9d7064bbe6e3997a7e74d4254c1fccaef64ef9b014a",
    "1fed010c66fcebc450463aae40c5540f8b6e2e45177cea024f429deb0ad95d6a",
    "0269f71e9ae0db50f6f9d7dc010567869b951e6df0542cbd6c5d186e10921bef",
    "2d8911a752c3b0e79c1ef6ec2fac4eda9350fe72facff6a61f390d988c09e742",
    "a517711e9d5bc6b6649b333d92a8fe30057414cf8ab0b6a178ab684883d2606d",
    "a96746cd3928b9bd415c4a22c08e61a8e3aea327d26137e6fde5b74fc6c492c0",
    "eb51cbe4afe9e3b4965318fefae70c5fb47fc257c8dcc93a01faf392033ad7e7",
    "9fa579b3be6c27dd05cbc6556e8b63cff65595256d95d9a31057a94908add8e7",
    "a96746cd3928b9bd415c4a22c08e61a8e3aea327d26137e6fde5b74fc6c492c0",
    "eb51cbe4afe9e3b4965318fefae70c5fb47fc257c8dcc93a01faf392033ad7e7",
    "9fa579b3be6c27dd05cbc6556e8b63cff65595256d95d9a31057a94908add8e7",
    "a96746cd3928b9bd415c4a22c08e61a8e3aea327d26137e6fde5b74fc6c492c0",
    "eb51cbe4afe9e3b4965318fefae70c5fb47fc257c8dcc93a01faf392033ad7e7",
    "f1581535f014bcfb6d944b7b7c54572c326d89850c75230dace99c51cf9f61e4",
    "55d0af07499d8ef65ce0cc145716b8df887ef8c6adaf4b866590e14be21558ee",
    "b73edf4e67c7736a0125a3d62661b9abc31c9149a15ec33bc4bd43cd34a6f6b3",
    "10c8ff5bd2546417e76a6bd0bd5d22fc4e87510c8f1b9f7f9056e1e2ccc6da67",
    "a1199b0b6bc57a654a0e3c2d012c0ab7343459dbee9e1ab75b9468aeedf94dfd",
    "8545422cef1a496ea73d0da7df72ff7d33b84dc05e85df3b9df6469a23b5d68b",
    "75ff78356166af00c87a2d7c08caf8ee425a55fccce65f1ecb457927417ef2bf",
    "5ed51fa0b97771442ee12cf218cbdcadbd8b6eefbdd9a1b34754943a23e6bd36",
    "d6c36fff70e010475edc3570576df4701d018faee308381909095175cf0eaaae",
    "b81f7c4d93f2720dbe4f73da2f9452e77a148355593e077dbfcc14a1b485705c",
    "4815f71a0df07dd5f442cac03003e5febc267e92861f3870afdf6bb290fbe977",
    "9b5e76650f84b5f9dc2b671b5cc6899b00b4c228036e290a8caf796239ae0fd7",
    "59f230fe73df99538be4b8ee196f714d7174cd5dc2a65b64c5d9a9c2dfce36f8",
    "b85306eb3eb9983b740375067983fa1a18a819eda634546d42e477e10834bc94",
    "64a12400fff6312005d0a774fe6fbaea5d3c1d83e3a175ac61de250a30e1eb79",
    "b218918c68e643c83f2bde7973a70c0a97b376048cfcbae4fdd516cc6d4ba780",
    "9c21915c6844a780a46eeeaf33cdf74c14a031591132b6863be0d3fa82ad97ef",
    "3bb547432865db8a9f5faf34cdb0244c222ca7b386bcb6b35720960141ba7751",
    "08abad487a757d408ff5922db45bb3b1267cbd23956b42fd2bb4a57acdc6d748",
    "bd3f6d06cb45ac45d847782e63c41c2de8765393d6071fe74f2017b38301206d",
    "727dd0b7bf98d07eede4442734b01bdc11c79aa86b50814c5efada0f9e30e0aa",
    "1f03517504bd23457781ad5518ddfee18b91ab604347b83152725df91e7d0cda",
    "2a210c5bf6ed6d11075a64e87ec73dc39fdb4e19e82bdf5869c6d68d888bf523",
    "591ddc9638979fd643c4d397efafd60d14d5121565052ca3a336dc324d9e2194",
    "1f03517504bd23457781ad5518ddfee18b91ab604347b83152725df91e7d0cda",
    "2a210c5bf6ed6d11075a64e87ec73dc39fdb4e19e82bdf5869c6d68d888bf523",
    "e7a01e79fd74a4ca8f491e3b22aee0dc3fecde9da2789f04b80ece4f7d656661",
    "60414f0a01d963397f22e2c56a3b1b5c7a34564a6c05442b08d6501645227724",
)

_PHYSICAL_OBSERVATION_SHA256 = tuple(
    tuple(line.split())
    for line in """
0f9a250d54072a631f584497af47a1038f17c6093992cc7e7631c541dcbcea5f 0f673fbae863919c482461ceb3d3332f6a9967000442f12bd6b87f2259352a6e
cdbff74369e44fff3ddbddeed0f85ba3b80b4b59be7370a31af5355e1cf6479d 0f673fbae863919c482461ceb3d3332f6a9967000442f12bd6b87f2259352a6e
681299b767722ac879debc916fb6068ebc6c373256cbe18d787419c9f5b88f94 0f673fbae863919c482461ceb3d3332f6a9967000442f12bd6b87f2259352a6e
c4789eabd7ace68d6262342757d177e3c77a96984d173eac8352c0ffdedcc579 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
a635722642ed0a31f22b01eecac20a4dbe863a5978402e4f87b4b2600be8ca4b 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
79c713fa15a817c2e50ce9ce0e0b809721ec63820a6f66e4e3fd39909f042c8d 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
d62ff777e4a5036b702c06824eb4fbb35e56e1bc057d1911f2f477f881400fb4 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
5f6cf275875f399304fec0768f2d1b25f22569cdb1c332cf17a7a9ba779583a4 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
4ad79d25162f858499b5637eb06056648da636974c0a31991a8b0cc3c645107d 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
59f378b7bafae4de6820349244e94a43469d217893178fdb280e8595c0e6a54b 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
62b8fd1351ed85165ab92cad5bc1396379bc5c782a7a77530c2af328c5378ab3 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
0174addc8027543e83a84d139b84aa1ab1a8dccb29e47535d44afbb26387191a 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
30a2e94d4421c9a297515de91b8bcc4473e516e9a61892a1aa115ef296ee1c16 9cdcb9f5ca677fe9d05e2c7393a7e3f44b58b0e0131a4dc1d6f9e8318491659e
f33b3caa8432562b090da560d20cf4393ed9151cebfc68132e86386786309fc8 9cdcb9f5ca677fe9d05e2c7393a7e3f44b58b0e0131a4dc1d6f9e8318491659e
b7977b2ef586291a5293cc7496dc2c911aafac8bc6efcdb8ebe0145473463040 9cdcb9f5ca677fe9d05e2c7393a7e3f44b58b0e0131a4dc1d6f9e8318491659e
abf07117d48d970609516dcb657baa19819d455c3df383625a0e32e420f85f8f 9cdcb9f5ca677fe9d05e2c7393a7e3f44b58b0e0131a4dc1d6f9e8318491659e
ef34f7e6e201b5c5d50f88955d38c6a3c29f9fa96395f749be28f32e9f5eeeac 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
2f23012d64db5f51f89752e0437c0c2153c0e4b8fe4edbd9fed9101039c198f3 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
ccb8b9f7d02a4d8f8b6609fec5216409136322e8550e25f623be124afbe60fe7 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
0a6a94ada05c72fda395de1826c1d194fc3d1630a6211896e99d8e5ba6175b18 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
9bf307c78c1f62e1b04d6e09c9272f3554b8b6a24125ec137445e18bbd51824a 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
f273bd242ed2d1d6f67853a64e45e5a195bbd8a0f8b402dc30d13bfab49e8379 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
53c7234764508625691428a44da6349b581541981e0f238e5419d55bd0f10fa5 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
a8b6a6680dba76bd42b7fd879c6a04a58239c78a89be31aac5f39700d238cc7b 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
663c988c42c31f21c4a8908ca945b326ff109913f9abd614fdc129c969ffeccc 6441d16fdc8f38459586d6355f5f7084a018f861c018d74eda533cd97a7afac0
719519684dd10328e24e96c22ffaf4a0b78466b1813f0c93fd1cd995575bfbbb 1cae59a1be2fc73fe9f2bf70fd58013deb4c95f5e2ba56d04c571d26efe621a6
064b4fb5b46b0afa2a61c2f88c4eae79f8377b1b2f3f93c71a903ff4fa386061 1cae59a1be2fc73fe9f2bf70fd58013deb4c95f5e2ba56d04c571d26efe621a6
70d534023e0e26f86a708e8fa1a2c54457e69a7038a43a2fcc953600a295304a 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
05025d4987e4dfcd16ff25fd2c5280aa268e425b05d9ba358b0404a8772fb1a5 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
469e037555f0ff4761243f681ec5470406b53d901383827c9fd2fe97c268cf91 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
5ddf83c7ff4f89334c6be8e9d2d8ce110f6780c5176a723d0d98b13d8306e515 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
e69bedcb308ec05850ca6ecd1415bbea9ed0e85779c3c44c61d1a16a963992bd 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
1b153695d808371b811f3a29f76311945dcc7f7161c799f135fb07b6b9d17efb 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
d170a49b9d14b87be8dcc229efc69a6b1c471bd38615c34cd5e0e298d151840e 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
08b769347a52a73fb0658d2f4d47652fba438e498438f8a6fc7d364b07c1de98 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
979bbf03f024ecf3c957c9477536aaec65295b3427a14f6c3c7933b39cc1dc73 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
071c8481518f8a63b74be79c729b48feb84a16ba7deb6a19313d7f138192bb69 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
b159fdd212cb3b870cc93ab07879ca50932d1e9556085d9e2a38aed4dec1536a 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
7225be5df6d435766732ef46fe0dae33cc142faa56bae0c35c55ff68bdffeda9 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
815b0e6a55e0d2c1ea56bf54218617b938de92309e60ae325b87e5d5f68f7aa9 59b696445cc66278605c797280ddadfd88e7e26e7ac03bd7aace6ffb3e3b76f2
fa348fb4146f1fc1c2e77a66a80e046c13de54461ef7e0cc30662da441ba3b7d a37c696d473335edc16dae41ea9c1c73d53d12b438e0d7c2464c56a9b85cb7b3
904bc92d1664d4b5c14f49cd7a7c1eda30c0efbb08329aef71f88e7f7f5b6085 a37c696d473335edc16dae41ea9c1c73d53d12b438e0d7c2464c56a9b85cb7b3
b9f7df8156378c4ed65f3d26155baf4177de2373e1004b5e92b1f65a386bbab8 a37c696d473335edc16dae41ea9c1c73d53d12b438e0d7c2464c56a9b85cb7b3
b625bef0bcbb5409dd1f3e5d8e6e9bf0ed65540a8c9653bb4273c6a873c086b9 a37c696d473335edc16dae41ea9c1c73d53d12b438e0d7c2464c56a9b85cb7b3
0f9dc24c3de062dee2ed0b986f0c07419f388a6fe6b1becf58895b48ce33ab21 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
b781abe6216bcb1baf78271d2f486908e2a240745263acd659662403c5d50426 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
ae5f06fbdb5216d4873f9022ba5a3b7e21d7255827922f765104101cfe37316e 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
255fa2834c7a3810b7a75ff66635016222e420f46823691bc211468473a15145 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
b89dc874df7c8cd6e852ce23570c727249db048c9ac4ad793a1d7b992cca8471 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
d9ac5412890e04ace216ee6fcb9d958346253d38e6e79991d75b65d4dd491ae1 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
02c6973a54953bddaa000c0ec0b9df83ebc3fd366a3978a1f1c74250beb70271 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
a6e060b1cff34d60af461e86000b6b786496c1ca3b33d5c2710c23a4f9fce88c 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
abf401ea67ef4b9a43bfdeffd96d590b159577347ff15140ef2e70cf3e878ccd 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
c04c07d09324f5e301e672540207c3ff3b91ca0c0fbb95c52dbf74d134265650 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
932ea2f132e021af995b5fa03717ed3ae9184478f100727e7c26713d9e39dc68 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
326cd32ee7dc2e1af2d4c30c258db6d8a75be1634735da3ee7d819b5e77d7ff6 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
98cc81db6a278d24796649234359e7932a814d9ebe13bd64d91914216d3bfcd2 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
c2473a0cb31796b0e86289f4e490cb9affeca51df3914c877f364fa0b3eb418c 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
a2268f0c77130928455f47dd7ef1e37a01b73bd6ee76fa27549c7f83945e27b1 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
a6f41581cf26c898a60754087a98410dfb57d7ddf8fe1b957a1ffa82f741ac8f 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
401319ba8bd89f67ff207e6e6b92b9869a160f6d86437da362798a6fb581f6d6 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
d908bd16f2065fe43c3b3976b7efcd9116b9cdf00c6e6348a20177a27a4d0f38 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
eacb2134cbe11a53e7858ba68050fde5f968336e91a684a19a0ddd11dd82451c 6c0dfd805c9328173130e7b7ac5738f389aa2c941aa17759553ecca83e941d2f
3942edd96506c5e6ec900274791103aaea9ee5faebbed594b7aed6682f4791aa 0f673fbae863919c482461ceb3d3332f6a9967000442f12bd6b87f2259352a6e
32efdc4e247c758d4ba0e770d0a033e638fe14813d27bd8f3b9d358c1a2b4517 0f673fbae863919c482461ceb3d3332f6a9967000442f12bd6b87f2259352a6e
25de225f7baf1251c7ffa8253ece769761c9b3313eb4da002c9f67340cd46836 14f249ad9129a178ba7dffd159c5002e8933a02dcbfe46688f2c86f9b057803b
3df277b5a4e13a38046fdd4347813c88584b25f91e134489cd76f69892439df1 14f249ad9129a178ba7dffd159c5002e8933a02dcbfe46688f2c86f9b057803b
32b0f84519f812b908465f22ef9223750575ed726b3fef296c8a192028005f2b 14f249ad9129a178ba7dffd159c5002e8933a02dcbfe46688f2c86f9b057803b
58394953e8d88279eeb13706ee222330b5403d5bfdd0ea35e43533a632b4e7a7 14f249ad9129a178ba7dffd159c5002e8933a02dcbfe46688f2c86f9b057803b
aebf3376475743c02078ced70b19f25937e2381b88d020d31bce2529ee79f0eb 14f249ad9129a178ba7dffd159c5002e8933a02dcbfe46688f2c86f9b057803b
5f97b70c2a3570ec95a04eeda835ed72972f33e77d811ee09f848a5aad79625f cdad59efb3e3d0d00f5f47300abba9585670411a257dd7810f8b71f878fac327
85e4c8af04afa9b18bff66f30e0d1789f610f9be54e15359dce62e4f974b423f cdad59efb3e3d0d00f5f47300abba9585670411a257dd7810f8b71f878fac327
f081ca18d38279284e1b4cebd1504bef2899f77325bef19edbce106132c4d085 cdad59efb3e3d0d00f5f47300abba9585670411a257dd7810f8b71f878fac327
33fb66634ba898886d2c7dacf11f749c790a20825f9454f0772f11f518574ce8 1cae59a1be2fc73fe9f2bf70fd58013deb4c95f5e2ba56d04c571d26efe621a6
9067d3080b73d4a4c8e043d39b8fdfd40cc81656e08b5ab9c4903a1a4cd034ce 1cae59a1be2fc73fe9f2bf70fd58013deb4c95f5e2ba56d04c571d26efe621a6
13a1d0645938d20e013f6d13cde3df84665ff102ee09cd98b1d32483e5c1a3c8 98a651773ce7497f6057daabb0a8962c5c24958a5ffe035b21789329a48dc7d5
0ee2bb9601578c93e7630124828c655e87fecc232ae088cd78f84800db3d3136 98a651773ce7497f6057daabb0a8962c5c24958a5ffe035b21789329a48dc7d5
3d35b2a36befbc2018f061dbc9ee23c19ac4dd7c61440895c5376304bae144e1 98a651773ce7497f6057daabb0a8962c5c24958a5ffe035b21789329a48dc7d5
ddc69cb5bac98d4727abae17b83eb9c203c5076a1977e97b861b1b1c6758ef58 98a651773ce7497f6057daabb0a8962c5c24958a5ffe035b21789329a48dc7d5
f7fc661844b5ec0fb986ac1c9193490e2a10630cad0019c29f264b294843781b c09eb256b6b547c58e218da8f9f61ed70ca8ea4aeee84b3e4a4ecc3b7c0c87f8
c8555a4172cda924837f6c8b76e469e482040e30010ad5e76c2e64ddb6a69c0e 6bd84007c0af0e12a60735d5904034e6b62b8e17dd8ee20635b770230b726a5d
15aa03f86d12f797fb71a92a3aaa7b46f2f1a6ca99f48a9481e2c221ecb5669f 55de7050178e6e231d8d1d392544bf7d2b1e8a36f7793d4a49211bb7a751b41a
9926bef10b71a8393a7bf0a913479ed9073a019d65675cbe82b237ddf4b1a5e6 55de7050178e6e231d8d1d392544bf7d2b1e8a36f7793d4a49211bb7a751b41a
ec56b7c7ad3f5982b76809c86e85a17013da4eeb3890556f2ff99af14826e470 55de7050178e6e231d8d1d392544bf7d2b1e8a36f7793d4a49211bb7a751b41a
2e46f13b51e1d1551a2cef60c981b211252a5cdcdb9b6081bf28a108b97b6d61 cd0ba248f2c5452c7b2d6d3ac7cd86591b4d7312a12eb8db47e2083cee24ef82
2a200459170400e5b742b54a63afc712217799c78e01e0bd89c117381d935cd0 cd0ba248f2c5452c7b2d6d3ac7cd86591b4d7312a12eb8db47e2083cee24ef82
4e4b1430daf5e6db73dc55f0f51a64a9565c10643b3fd8cfde808e835a7935b8 a4ca8f78d8f7af72acdb25bddb2d763b6e114a31a434137b7db99770ce193ad6
c558a96d46f1905479c52e098e469d463f91ce7039743ac120dbdf336046c826 a4ca8f78d8f7af72acdb25bddb2d763b6e114a31a434137b7db99770ce193ad6
""".splitlines()
    if line
)

if len(_FULL_SIGNATURE_SHA256) != len(ALLOWLIST):
    raise AssertionError("full declaration-signature pin count does not match ALLOWLIST")
if len(_PHYSICAL_OBSERVATION_SHA256) != len(ALLOWLIST):
    raise AssertionError("physical/observation pin count does not match ALLOWLIST")

ALLOWLIST = tuple(
    replace(
        entry,
        usr=_stable_usr(entry.usr),
        access=(
            entry.access
            if entry.access is not None
            else 3
            if "toExactQJsonInner" in entry.usr
            or "issueTokenRequest" in entry.usr
            or "QSettingsProfileStore@FI@" in entry.usr
            else 0
        ),
        linkage=2 if "@aN@" in entry.usr else 4,
        full_signature_sha256=signature_hash,
        physical_identity_sha256=physical_hash,
        observation_set_sha256=observation_hash,
    )
    for entry, signature_hash, (physical_hash, observation_hash) in zip(
        ALLOWLIST,
        _FULL_SIGNATURE_SHA256,
        _PHYSICAL_OBSERVATION_SHA256,
        strict=True,
    )
)
DOMAIN_ALLOWLIST = ALLOWLIST[: len(DOMAIN_ALLOWLIST)]
FOUNDATION_ALLOWLIST = ALLOWLIST[len(DOMAIN_ALLOWLIST) :]
ALLOWLIST_BY_KEY: dict[tuple[str, str], AllowlistEntry] = {e.key(): e for e in ALLOWLIST}
ALLOWLIST_USRS = frozenset(entry.usr for entry in ALLOWLIST)
# Exact semantic stop-points whose internals are governed separately:
# RawJson::Value's two QJson bridges are explicit declaration allowances,
# while bare QVariant is intentionally not a forbidden JSON container
# (only its statically JSON-shaped Map/List/Hash forms are).
SAFE_OPAQUE_RECORD_USRS = frozenset(
    {
        "c:@N@Arkham@N@Json@S@Value",
        "c:@S@QVariant",
    }
)

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
_CXCursor_UnionDecl = 3
_CXCursor_ClassDecl = 4
_CXCursor_FieldDecl = 6
_CXCursor_VarDecl = 9
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
_CXCursor_TypeRef = 43
_CXCursor_OverloadedDeclRef = 49
_CXCursor_NoDeclFound = 71
_CXCursor_CallExpr = 103
_CXCursor_CStyleCastExpr = 117
_CXCursor_CompoundLiteralExpr = 118
_CXCursor_InitListExpr = 119
_CXCursor_CXXStaticCastExpr = 124
_CXCursor_CXXDynamicCastExpr = 125
_CXCursor_CXXReinterpretCastExpr = 126
_CXCursor_CXXConstCastExpr = 127
_CXCursor_CXXFunctionalCastExpr = 128
_CXCursor_CXXNewExpr = 134
_CXCursor_LambdaExpr = 144
_CXCursor_ObjCBoolLiteralExpr = 145
_CXCursor_TranslationUnit = 350
_CXCursor_FriendDecl = 603
_CXCursor_TemplateTypeParameter = 27

_EXPECTED_CURSOR_KIND_SPELLINGS = {
    _CXCursor_StructDecl: "StructDecl",
    _CXCursor_UnionDecl: "UnionDecl",
    _CXCursor_ClassDecl: "ClassDecl",
    _CXCursor_FieldDecl: "FieldDecl",
    _CXCursor_FunctionDecl: "FunctionDecl",
    _CXCursor_VarDecl: "VarDecl",
    _CXCursor_ParmDecl: "ParmDecl",
    _CXCursor_TypedefDecl: "TypedefDecl",
    _CXCursor_CXXMethod: "CXXMethod",
    _CXCursor_Namespace: "Namespace",
    _CXCursor_Constructor: "CXXConstructor",
    _CXCursor_ConversionFunction: "CXXConversion",
    _CXCursor_TemplateTypeParameter: "TemplateTypeParameter",
    _CXCursor_FunctionTemplate: "FunctionTemplate",
    _CXCursor_ClassTemplate: "ClassTemplate",
    _CXCursor_UsingDeclaration: "UsingDeclaration",
    _CXCursor_TypeAliasDecl: "TypeAliasDecl",
    _CXCursor_TypeRef: "TypeRef",
    _CXCursor_CXXBaseSpecifier: "C++ base class specifier",
    _CXCursor_OverloadedDeclRef: "OverloadedDeclRef",
    _CXCursor_NoDeclFound: "NoDeclFound",
    _CXCursor_CallExpr: "CallExpr",
    _CXCursor_CStyleCastExpr: "CStyleCastExpr",
    _CXCursor_CompoundLiteralExpr: "CompoundLiteralExpr",
    _CXCursor_InitListExpr: "InitListExpr",
    _CXCursor_CXXStaticCastExpr: "CXXStaticCastExpr",
    _CXCursor_CXXDynamicCastExpr: "CXXDynamicCastExpr",
    _CXCursor_CXXReinterpretCastExpr: "CXXReinterpretCastExpr",
    _CXCursor_CXXConstCastExpr: "CXXConstCastExpr",
    _CXCursor_CXXFunctionalCastExpr: "CXXFunctionalCastExpr",
    _CXCursor_CXXNewExpr: "CXXNewExpr",
    _CXCursor_LambdaExpr: "LambdaExpr",
    _CXCursor_ObjCBoolLiteralExpr: "ObjCBoolLiteralExpr",
    _CXCursor_TranslationUnit: "TranslationUnit",
    _CXCursor_FriendDecl: "FriendDecl",
}

# The "shape-eligible" record/class-template kinds this script walks for
# base-specifier/using-declaration inheritance exposure (see
# _inherited_and_reexported_encoders()).
_RECORD_LIKE_KINDS = frozenset(
    {
        _CXCursor_StructDecl,
        _CXCursor_UnionDecl,
        _CXCursor_ClassDecl,
        _CXCursor_ClassTemplate,
    }
)

# Function-like declaration kinds with a meaningful result type.
_FUNCTION_LIKE_KINDS = frozenset(
    {
        _CXCursor_FunctionDecl,
        _CXCursor_CXXMethod,
        _CXCursor_ConversionFunction,
        _CXCursor_FunctionTemplate,
    }
)

# Every function/constructor signature subject to the closed forbidden-
# wire-type boundary. Constructors have parameters but no result.
_SIGNATURE_DECL_KINDS = _FUNCTION_LIKE_KINDS | {_CXCursor_Constructor}

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
_TYPE_SURFACE_KINDS = frozenset(
    {
        _CXCursor_TypeAliasDecl,
        _CXCursor_TypedefDecl,
        _CXCursor_CXXBaseSpecifier,
        _CXCursor_FieldDecl,
        _CXCursor_VarDecl,
        _CXCursor_ParmDecl,
        _CXCursor_TypeRef,
    }
)
# Expression kinds that can introduce a typed construction independently of a
# VarDecl. These are the stable CXCursor values for calls/constructor
# temporaries, C/compound/list initialization, every C++ named/functional cast,
# new expressions, and lambdas. Decl/member references do not need a second
# finding: their declaring local/field is already audited, while calls and
# temporaries may have no declaration at all.
_EXPRESSION_SURFACE_KINDS = frozenset(
    {
        _CXCursor_CallExpr,
        _CXCursor_CStyleCastExpr,
        _CXCursor_CompoundLiteralExpr,
        _CXCursor_InitListExpr,
        _CXCursor_CXXStaticCastExpr,
        _CXCursor_CXXDynamicCastExpr,
        _CXCursor_CXXReinterpretCastExpr,
        _CXCursor_CXXConstCastExpr,
        _CXCursor_CXXFunctionalCastExpr,
        _CXCursor_CXXNewExpr,
        _CXCursor_LambdaExpr,
    }
)

# A public OR protected base class/using-declaration still exposes its
# encoder-shaped members to the outside world (directly for a public
# base, or to any further subclass -- which can then re-expose it
# publicly with a single additional using-declaration or public
# inheritance step of its own -- for a protected one); only a PRIVATE
# base/using-declaration genuinely blocks further exposure. See
# _inherited_and_reexported_encoders().
_INHERITABLE_ACCESS_SPECIFIERS = frozenset({_CX_CXXPublic, _CX_CXXProtected})

# CXTypeKind values used by the closed recursive semantic type graph.
_CXType_Pointer = 101
_CXType_LValueReference = 103
_CXType_RValueReference = 104
_CXType_FunctionNoProto = 110
_CXType_FunctionProto = 111
_CXType_MemberPointer = 117
_CXType_ConstantArray = 112
_CXType_IncompleteArray = 114
_CXType_VariableArray = 115
_CXType_DependentSizedArray = 116
_REFERENCE_OR_POINTER_TYPE_KINDS = frozenset(
    {_CXType_Pointer, _CXType_LValueReference, _CXType_RValueReference}
)
_FUNCTION_TYPE_KINDS = frozenset({_CXType_FunctionNoProto, _CXType_FunctionProto})
_ARRAY_TYPE_KINDS = frozenset(
    {
        _CXType_ConstantArray,
        _CXType_IncompleteArray,
        _CXType_VariableArray,
        _CXType_DependentSizedArray,
    }
)
SUPPORTED_PRODUCTION_CONFIGS = ("Debug", "Release", "RelWithDebInfo")

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
        self.forbidden_type_cache: dict[
            tuple[int, str, bool, int, int], str | None
        ] = {}
        self.forbidden_type_active: set[
            tuple[int, str, bool, int, int]
        ] = set()
        self.forbidden_type_blockers: dict[
            tuple[int, str, bool, int, int],
            set[tuple[int, str, bool, int, int]],
        ] = {}
        self.type_graph_errors: list[str] = []
        lib = self.lib

        lib.clang_getCString.restype = ctypes.c_char_p
        lib.clang_getCString.argtypes = [_CXString]
        lib.clang_disposeString.argtypes = [_CXString]
        try:
            lib.clang_getCursorKindSpelling.restype = _CXString
            lib.clang_getCursorKindSpelling.argtypes = [ctypes.c_uint]
        except AttributeError as exc:
            raise EncoderHygieneError(
                "Installed libclang does not expose "
                "clang_getCursorKindSpelling; cursor-kind policy cannot be "
                "verified and therefore fails closed"
            ) from exc

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
        lib.clang_getArrayElementType.restype = _CXType
        lib.clang_getArrayElementType.argtypes = [_CXType]

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

        # Resolve pointees while recursively inventorying semantic type
        # references. Constness does not exempt a declaration; legitimate
        # inbound signatures are exact allowlist entries.
        lib.clang_getPointeeType.restype = _CXType
        lib.clang_getPointeeType.argtypes = [_CXType]

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
        lib.clang_getCursorReferenced.restype = _CXCursor
        lib.clang_getCursorReferenced.argtypes = [_CXCursor]
        lib.clang_Cursor_getNumTemplateArguments.restype = ctypes.c_int
        lib.clang_Cursor_getNumTemplateArguments.argtypes = [_CXCursor]
        lib.clang_Cursor_getTemplateArgumentType.restype = _CXType
        lib.clang_Cursor_getTemplateArgumentType.argtypes = [
            _CXCursor,
            ctypes.c_uint,
        ]
        lib.clang_getCursorSemanticParent.restype = _CXCursor
        lib.clang_getCursorSemanticParent.argtypes = [_CXCursor]
        lib.clang_getCXXAccessSpecifier.restype = ctypes.c_int
        lib.clang_getCXXAccessSpecifier.argtypes = [_CXCursor]
        lib.clang_getCursorResultType.restype = _CXType
        lib.clang_getCursorResultType.argtypes = [_CXCursor]
        lib.clang_getCanonicalType.restype = _CXType
        lib.clang_getCanonicalType.argtypes = [_CXType]
        lib.clang_getTypeSpelling.restype = _CXString
        lib.clang_getTypeSpelling.argtypes = [_CXType]
        lib.clang_Cursor_isVariadic.restype = ctypes.c_uint
        lib.clang_Cursor_isVariadic.argtypes = [_CXCursor]
        lib.clang_getCursorExceptionSpecificationType.restype = ctypes.c_int
        lib.clang_getCursorExceptionSpecificationType.argtypes = [_CXCursor]
        lib.clang_getFunctionTypeCallingConv.restype = ctypes.c_int
        lib.clang_getFunctionTypeCallingConv.argtypes = [_CXType]
        lib.clang_Type_getCXXRefQualifier.restype = ctypes.c_int
        lib.clang_Type_getCXXRefQualifier.argtypes = [_CXType]
        lib.clang_CXXMethod_isStatic.restype = ctypes.c_uint
        lib.clang_CXXMethod_isStatic.argtypes = [_CXCursor]

        lib.clang_getCursorLocation.restype = _CXSourceLocation
        lib.clang_getCursorLocation.argtypes = [_CXCursor]
        lib.clang_getExpansionLocation.argtypes = [
            _CXSourceLocation,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
            ctypes.POINTER(ctypes.c_uint),
        ]
        lib.clang_getSpellingLocation.argtypes = [
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
        self._verify_cursor_kind_constants()

    def _verify_cursor_kind_constants(self) -> None:
        mismatches: list[str] = []
        for value, expected in _EXPECTED_CURSOR_KIND_SPELLINGS.items():
            actual = self.to_str(
                self.lib.clang_getCursorKindSpelling(value)
            )
            if actual != expected:
                mismatches.append(
                    f"{value}: expected {expected!r}, installed libclang "
                    f"reports {actual!r}"
                )
        if mismatches:
            raise EncoderHygieneError(
                "Installed libclang cursor-kind ABI does not match the "
                "closed policy inventory:\n"
                + "\n".join(f"  {mismatch}" for mismatch in mismatches)
            )

    def to_str(self, cxstr: _CXString) -> str:
        raw = self.lib.clang_getCString(cxstr)
        result = raw.decode("utf-8", "replace") if raw else ""
        self.lib.clang_disposeString(cxstr)
        return result

    def cursor_file_and_line(self, cursor: _CXCursor) -> tuple[str | None, int]:
        filename, line, _offset, _spelling_file, _spelling_offset = (
            self.cursor_location_identity(cursor)
        )
        return filename, line

    def cursor_location_identity(
        self, cursor: _CXCursor
    ) -> tuple[str | None, int, int, str | None, int]:
        loc = self.lib.clang_getCursorLocation(cursor)
        file_ptr = ctypes.c_void_p()
        line = ctypes.c_uint()
        col = ctypes.c_uint()
        offset = ctypes.c_uint()
        self.lib.clang_getExpansionLocation(
            loc, ctypes.byref(file_ptr), ctypes.byref(line), ctypes.byref(col), ctypes.byref(offset)
        )
        if not file_ptr.value:
            return None, 0, 0, None, 0
        expansion_file = self.to_str(self.lib.clang_getFileName(file_ptr))
        spelling_file_ptr = ctypes.c_void_p()
        spelling_line = ctypes.c_uint()
        spelling_col = ctypes.c_uint()
        spelling_offset = ctypes.c_uint()
        self.lib.clang_getSpellingLocation(
            loc,
            ctypes.byref(spelling_file_ptr),
            ctypes.byref(spelling_line),
            ctypes.byref(spelling_col),
            ctypes.byref(spelling_offset),
        )
        spelling_file = (
            self.to_str(self.lib.clang_getFileName(spelling_file_ptr))
            if spelling_file_ptr.value
            else None
        )
        return (
            expansion_file,
            line.value,
            offset.value,
            spelling_file,
            spelling_offset.value,
        )


@dataclass(frozen=True)
class Finding:
    file: str  # repo-root-relative, forward-slash path, e.g. "src/domain/RawJson.h"
    line: int
    display_name: str
    # Stable semantic fingerprint of every forbidden result/parameter or
    # type-surface reference on this declaration.
    canonical_return_type: str
    usr: str
    access: int = 0
    linkage: int = 0
    offset: int = 0
    spelling_file: str = ""
    spelling_offset: int = 0
    observation_context: str = ""
    physical_identity_sha256: str = ""
    observation_set_sha256: str = ""
    exact_set_allowed: bool = False
    body_surface: bool = False

    def key(self) -> tuple[str, str]:
        return (self.file, self.usr)


def _identity_set_digest(entries: Sequence[Finding]) -> str:
    payload = [
        {
            "file": finding.file,
            "usr": finding.usr,
            "full_signature": hashlib.sha256(
                finding.canonical_return_type.encode("utf-8")
            ).hexdigest(),
            "access": finding.access,
            "linkage": finding.linkage,
            "body_surface": finding.body_surface,
            "physical_identity": finding.physical_identity_sha256,
            "observation_set": finding.observation_set_sha256,
        }
        for finding in entries
    ]
    return hashlib.sha256(
        json.dumps(
            sorted(
                payload,
                key=lambda item: (
                    item["file"],
                    item["usr"],
                    item["physical_identity"],
                    item["observation_set"],
                ),
            ),
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    ).hexdigest()


def _cursor_is_function_local(
    clang: "_LibClang", cursor: "_CXCursor"
) -> bool:
    parent = clang.lib.clang_getCursorSemanticParent(cursor)
    previous: tuple[int, int, int, int, int] | None = None
    for _ in range(32):
        kind = clang.lib.clang_getCursorKind(parent)
        if kind in _SIGNATURE_DECL_KINDS or kind == _CXCursor_LambdaExpr:
            return True
        if kind in (_CXCursor_NoDeclFound, _CXCursor_TranslationUnit):
            return False
        identity = (
            parent.kind,
            parent.xdata,
            int(parent.data[0] or 0),
            int(parent.data[1] or 0),
            int(parent.data[2] or 0),
        )
        if identity == previous:
            return False
        previous = identity
        parent = clang.lib.clang_getCursorSemanticParent(parent)
    return False


def _type_surface_is_body(
    clang: "_LibClang",
    cursor: "_CXCursor",
    kind: int,
    parent: "_CXCursor",
) -> bool:
    if kind in _EXPRESSION_SURFACE_KINDS:
        return True
    parent_kind = clang.lib.clang_getCursorKind(parent)
    if kind == _CXCursor_ParmDecl:
        owner = clang.lib.clang_getCursorSemanticParent(cursor)
        return (
            parent_kind == _CXCursor_LambdaExpr
            or _cursor_is_function_local(clang, owner)
        )
    if kind == _CXCursor_TypeRef and parent_kind == _CXCursor_ParmDecl:
        owner = clang.lib.clang_getCursorSemanticParent(parent)
        return _cursor_is_function_local(clang, owner)
    return (
        parent_kind == _CXCursor_LambdaExpr
        or _cursor_is_function_local(clang, parent)
        or _cursor_is_function_local(clang, cursor)
    )


def _is_unresolved_dependent_type_ref(
    clang: "_LibClang", cursor: "_CXCursor", kind: int
) -> bool:
    if kind != _CXCursor_TypeRef:
        return False
    referenced = clang.lib.clang_getCursorReferenced(cursor)
    return (
        clang.lib.clang_getCursorKind(referenced)
        == _CXCursor_TemplateTypeParameter
    )


def _type_ref_is_completely_covered_by_parent(
    clang: "_LibClang", cursor: "_CXCursor", parent: "_CXCursor"
) -> bool:
    """Suppress only a TypeRef already represented by its immediate complete
    declaration surface. References below initializer/reference expressions,
    template defaults, or specialization uses remain independent findings."""

    if clang.lib.clang_getCursorKind(cursor) != _CXCursor_TypeRef:
        return False
    parent_kind = clang.lib.clang_getCursorKind(parent)
    if parent_kind == _CXCursor_TypeRef:
        # A nested-name qualifier (QSettings in QSettings::Status) is not the
        # selected type. The outer TypeRef is visited independently with the
        # complete selected canonical type.
        return True
    if parent_kind == _CXCursor_UsingDeclaration:
        # Every using-declaration is resolved separately by
        # handle_inheritance_exposure(), including overload sets and access.
        return True
    if parent_kind in {
        _CXCursor_ParmDecl,
        _CXCursor_TypeAliasDecl,
        _CXCursor_TypedefDecl,
        _CXCursor_CXXBaseSpecifier,
        _CXCursor_FieldDecl,
        _CXCursor_VarDecl,
    }:
        # Each of these cursors is independently inspected with its complete
        # selected canonical type. This also suppresses nested-name qualifiers
        # such as QSettings in a safe QSettings::Status parameter.
        return True
    if parent_kind in _SIGNATURE_DECL_KINDS:
        return _is_encoder_shaped(clang, parent, parent_kind)[0]
    return False


def _cursor_template_argument_fingerprint(
    clang: "_LibClang", cursor: "_CXCursor", kind: int
) -> str | None:
    if kind != _CXCursor_CallExpr:
        return None
    referenced = clang.lib.clang_getCursorReferenced(cursor)
    count = clang.lib.clang_Cursor_getNumTemplateArguments(referenced)
    for index in range(max(count, 0)):
        argument = clang.lib.clang_Cursor_getTemplateArgumentType(
            referenced, index
        )
        if argument.kind == 0:
            continue
        forbidden = _forbidden_type_fingerprint(
            clang, argument, traverse_records=False
        )
        if forbidden:
            return f"template-arg[{index}]:{forbidden}"
    return None


@dataclass(frozen=True)
class _ExplicitTemplateCandidate:
    line: int
    offset: int
    spelling_file: Path
    spelling_offset: int
    statement: str
    names_and_args: tuple[tuple[str, tuple[str, ...]], ...]


_EXPLICIT_TEMPLATE_START = _re.compile(
    r"(?m)^[ \t]*(?:extern[ \t]+template(?![ \t]*<)[ \t]+|"
    r"template[ \t]*<[ \t]*>|template(?![ \t]*<)[ \t]+)"
)
_EXPLICIT_INSTANTIATION_START = _re.compile(
    r"(?m)^[ \t]*(?:extern[ \t]+)?template(?![ \t]*<)[ \t]+"
)


def _split_template_arguments(value: str) -> tuple[str, ...]:
    arguments: list[str] = []
    start = 0
    depth = 0
    for index, character in enumerate(value):
        if character in "<([{":
            depth += 1
        elif character in ">)]}":
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append(value[start:index].strip())
            start = index + 1
    arguments.append(value[start:].strip())
    return tuple(argument for argument in arguments if argument)


def _template_names_and_arguments(
    statement: str,
) -> tuple[tuple[str, tuple[str, ...]], ...]:
    result: list[tuple[str, tuple[str, ...]]] = []
    for match in _re.finditer(r"([A-Za-z_]\w*)[ \t]*<", statement):
        name = match.group(1)
        if name == "template":
            continue
        begin = match.end()
        depth = 1
        index = begin
        while index < len(statement) and depth:
            if statement[index] == "<":
                depth += 1
            elif statement[index] == ">":
                depth -= 1
            index += 1
        if depth:
            continue
        result.append(
            (
                name,
                _split_template_arguments(statement[begin : index - 1]),
            )
        )
    return tuple(result)


def _explicit_template_candidates(
    path: Path,
    arguments: Sequence[str],
    directory: Path,
    always_tokenize: bool,
) -> list[_ExplicitTemplateCandidate]:
    text = path.read_text(encoding="utf-8")
    compiler = os.environ.get("ARKHAM_CLANGXX", "clang++")
    if _EXPLICIT_TEMPLATE_START.search(text) is None:
        if not always_tokenize:
            return []
        preprocess_result = subprocess.run(
            [
                compiler,
                *arguments,
                "-E",
                "-fkeep-system-includes",
                "-x",
                "c++",
                str(path),
            ],
            cwd=directory,
            capture_output=True,
            text=True,
        )
        if preprocess_result.returncode != 0:
            raise EncoderHygieneError(
                f"Compiler preprocessing failed while discovering macro "
                f"template instantiations in {path}:\n"
                f"{preprocess_result.stderr}"
            )
        active_file: Path | None = None
        active_text: list[str] = []
        for output_line in preprocess_result.stdout.splitlines():
            marker = _re.fullmatch(r'#\s+\d+\s+"([^"]+)".*', output_line)
            if marker:
                marker_path = Path(marker.group(1))
                if not marker_path.is_absolute():
                    marker_path = directory / marker_path
                active_file = marker_path.resolve()
                continue
            if active_file == path.resolve():
                active_text.append(output_line)
        if (
            _EXPLICIT_INSTANTIATION_START.search(
                "\n".join(active_text)
            )
            is None
        ):
            return []
    token_command = [
        compiler,
        *arguments,
        "-E",
        "-Xclang",
        "-dump-tokens",
        "-x",
        "c++",
        str(path),
    ]
    token_result = subprocess.run(
        token_command,
        cwd=directory,
        capture_output=True,
        text=True,
    )
    if token_result.returncode != 0:
        raise EncoderHygieneError(
            f"Compiler tokenization failed while resolving explicit template "
            f"instantiations in {path}:\n{token_result.stderr}"
        )
    tokens: list[tuple[str, int, int, Path, int, int]] = []
    for output_line in token_result.stderr.splitlines():
        location_match = _re.search(
            r"Loc=<(.+?):(\d+):(\d+)"
            r"(?: <Spelling=(.+?):(\d+):(\d+)>)?>$",
            output_line,
        )
        first_quote = output_line.find("'")
        second_quote = output_line.find("'", first_quote + 1)
        if (
            location_match is None
            or first_quote < 0
            or second_quote < 0
        ):
            continue
        location_path = Path(location_match.group(1))
        if not location_path.is_absolute():
            location_path = directory / location_path
        if location_path.resolve() != path.resolve():
            continue
        spelling_path = (
            Path(location_match.group(4))
            if location_match.group(4)
            else location_path
        )
        if not spelling_path.is_absolute():
            spelling_path = directory / spelling_path
        tokens.append(
            (
                output_line[first_quote + 1 : second_quote],
                int(location_match.group(2)),
                int(location_match.group(3)),
                spelling_path.resolve(),
                int(location_match.group(5) or location_match.group(2)),
                int(location_match.group(6) or location_match.group(3)),
            )
        )

    line_starts = [0]
    for match in _re.finditer("\n", text):
        line_starts.append(match.end())
    candidates: list[_ExplicitTemplateCandidate] = []
    for token_index, (
        token,
        line,
        column,
        spelling_path,
        spelling_line,
        spelling_column,
    ) in enumerate(tokens):
        if token != "template":
            continue
        next_index = token_index + 1
        if next_index < len(tokens) and tokens[next_index][0] == "<":
            # `template<class T>` is a primary declaration. Only the empty
            # `template<>` form is an explicit specialization.
            if (
                next_index + 1 >= len(tokens)
                or tokens[next_index + 1][0] != ">"
            ):
                continue
        statement_tokens: list[str] = []
        angle_depth = 0
        paren_depth = 0
        for (
            current,
            _token_line,
            _token_column,
            _spelling_path,
            _spelling_line,
            _spelling_column,
        ) in tokens[token_index:]:
            statement_tokens.append(current)
            if current == "<":
                angle_depth += 1
            elif current == ">" and angle_depth:
                angle_depth -= 1
            elif current == "(":
                paren_depth += 1
            elif current == ")" and paren_depth:
                paren_depth -= 1
            elif current in {";", "{"} and angle_depth == 0 and paren_depth == 0:
                break
        statement = " ".join(statement_tokens)
        names_and_args = _template_names_and_arguments(statement)
        if not names_and_args:
            raise EncoderHygieneError(
                f"Could not resolve explicit template declaration at "
                f"{path}:{line}"
            )
        character_offset = line_starts[line - 1] + column - 1
        byte_offset = len(text[:character_offset].encode("utf-8"))
        if spelling_path.is_file():
            spelling_text = spelling_path.read_text(encoding="utf-8")
            spelling_line_starts = [0]
            for spelling_match in _re.finditer("\n", spelling_text):
                spelling_line_starts.append(spelling_match.end())
            if spelling_line > len(spelling_line_starts):
                raise EncoderHygieneError(
                    f"Compiler reported invalid macro spelling line "
                    f"{spelling_path}:{spelling_line}"
                )
            spelling_character_offset = (
                spelling_line_starts[spelling_line - 1]
                + spelling_column
                - 1
            )
            spelling_byte_offset = len(
                spelling_text[:spelling_character_offset].encode("utf-8")
            )
        else:
            # Command-line/builtin macro spellings have no physical file.
            # Preserve their compiler-reported line/column as a stable packed
            # identity rather than dropping the spelling side.
            spelling_byte_offset = (spelling_line << 32) | spelling_column
        candidates.append(
            _ExplicitTemplateCandidate(
                line=line,
                offset=byte_offset,
                spelling_file=spelling_path,
                spelling_offset=spelling_byte_offset,
                statement=statement,
                names_and_args=names_and_args,
            )
        )
    return candidates


def _normalized_template_spelling(value: str) -> str:
    return _re.sub(r"\b(?:class|struct|union|enum)\s+", "", value).replace(
        " ", ""
    )


def _collect_explicit_template_findings(
    path: Path,
    arguments: Sequence[str],
    directory: Path,
    repo_root: Path,
    observation_context: str,
    always_tokenize: bool = False,
) -> list[Finding]:
    candidates = _explicit_template_candidates(
        path, arguments, directory, always_tokenize
    )
    if not candidates:
        return []
    compiler = os.environ.get("ARKHAM_CLANGXX", "clang++")
    command = [
        compiler,
        *arguments,
        "-Xclang",
        "-ast-dump=json",
        "-fsyntax-only",
        "-x",
        "c++",
        str(path),
    ]
    completed = subprocess.run(
        command,
        cwd=directory,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise EncoderHygieneError(
            f"Compiler AST dump failed while resolving explicit template "
            f"instantiations in {path}:\n{completed.stderr}"
        )
    try:
        root = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise EncoderHygieneError(
            f"Compiler returned malformed AST JSON for {path}: {exc}"
        ) from exc

    specializations: dict[
        str,
        list[tuple[tuple[str, ...], tuple[str, ...], dict]],
    ] = {}
    type_aliases: dict[str, str] = {}
    record_nodes: dict[str, list[dict]] = {}

    def visit(node: object) -> None:
        if not isinstance(node, dict):
            return
        name = node.get("name")
        inner = node.get("inner")
        type_info = node.get("type")
        if (
            node.get("kind")
            in {
                "CXXRecordDecl",
                "RecordDecl",
                "ClassTemplateSpecializationDecl",
                "ClassTemplatePartialSpecializationDecl",
            }
            and isinstance(name, str)
        ):
            record_nodes.setdefault(name, []).append(node)
        if (
            node.get("kind") in {"TypeAliasDecl", "TypedefDecl"}
            and isinstance(name, str)
            and isinstance(type_info, dict)
        ):
            canonical_alias = type_info.get(
                "desugaredQualType", type_info.get("qualType")
            )
            if isinstance(canonical_alias, str):
                type_aliases[name] = canonical_alias
        if isinstance(name, str) and isinstance(inner, list):
            arguments_found: list[str] = []
            canonical_found: list[str] = []
            for child in inner:
                if not isinstance(child, dict) or child.get("kind") != "TemplateArgument":
                    continue
                type_info = child.get("type")
                if not isinstance(type_info, dict):
                    continue
                written = type_info.get("qualType")
                canonical = type_info.get("desugaredQualType", written)
                if isinstance(written, str) and isinstance(canonical, str):
                    arguments_found.append(written)
                    canonical_found.append(canonical)
            if arguments_found:
                specializations.setdefault(name, []).append(
                    (
                        tuple(arguments_found),
                        tuple(canonical_found),
                        node,
                    )
                )
        if isinstance(inner, list):
            for child in inner:
                visit(child)

    visit(root)

    def instantiated_surfaces(
        node: dict,
        visited: frozenset[str] = frozenset(),
        depth: int = 0,
    ) -> set[str]:
        if depth > _MAX_TYPE_DEPTH:
            raise EncoderHygieneError(
                "Explicit specialization AST graph exceeded the configured "
                f"{_MAX_TYPE_DEPTH}-level complexity bound in {path}"
            )
        node_id = str(node.get("id", id(node)))
        if node_id in visited:
            return set()
        nested_visited = visited | {node_id}
        forbidden: set[str] = set()

        def inspect_type(type_info: object) -> None:
            if not isinstance(type_info, dict):
                return
            spellings = {
                value
                for key in ("qualType", "desugaredQualType")
                if isinstance((value := type_info.get(key)), str)
            }
            for spelling in spellings:
                if _is_qjson_family(spelling):
                    forbidden.add(spelling)
                for referenced_name in _re.findall(
                    r"[A-Za-z_]\w*", spelling
                ):
                    alias_target = type_aliases.get(referenced_name)
                    if (
                        alias_target is not None
                        and _is_qjson_family(alias_target)
                    ):
                        forbidden.add(alias_target)
                    for record in record_nodes.get(referenced_name, ()):
                        forbidden.update(
                            instantiated_surfaces(
                                record,
                                nested_visited,
                                depth + 1,
                            )
                        )

        def inspect(value: object) -> None:
            if isinstance(value, dict):
                inspect_type(value.get("type"))
                for nested in value.values():
                    inspect(nested)
            elif isinstance(value, list):
                for nested in value:
                    inspect(nested)

        inspect(node)
        return forbidden

    findings: list[Finding] = []
    try:
        relative = path.resolve().relative_to(repo_root).as_posix()
    except ValueError:
        relative = path.resolve().as_posix()
    for candidate in candidates:
        matches: list[tuple[str, tuple[str, ...], dict]] = []
        for name, provided_arguments in candidate.names_and_args:
            normalized_provided = tuple(
                _normalized_template_spelling(argument)
                for argument in provided_arguments
            )
            for written, canonical, node in specializations.get(name, ()):
                if len(written) < len(normalized_provided):
                    continue
                if all(
                    {
                        expected,
                        _normalized_template_spelling(
                            type_aliases.get(
                                provided_arguments[index].split("::")[-1],
                                provided_arguments[index],
                            )
                        ),
                    }
                    & {
                        _normalized_template_spelling(written[index]),
                        _normalized_template_spelling(canonical[index]),
                    }
                    for index, expected in enumerate(normalized_provided)
                ):
                    matches.append((name, canonical, node))
        if not matches:
            raise EncoderHygieneError(
                f"Compiler AST did not expose the explicit template "
                f"specialization spelled at {path}:{candidate.line}: "
                f"{candidate.statement}"
            )
        forbidden = [
            f"{name}:{argument}"
            for name, arguments_found, _node in matches
            for argument in arguments_found
            if _is_qjson_family(argument)
        ]
        forbidden.extend(
            f"{name}:instantiated-surface:{surface}"
            for name, _arguments_found, node in matches
            for surface in instantiated_surfaces(node)
        )
        if not forbidden:
            continue
        identity = ",".join(sorted(set(forbidden)))
        try:
            spelling_file = candidate.spelling_file.relative_to(
                repo_root
            ).as_posix()
        except ValueError:
            spelling_file = candidate.spelling_file.as_posix()
        findings.append(
            Finding(
                file=relative,
                line=candidate.line,
                display_name=candidate.statement,
                canonical_return_type=(
                    "kind=explicit-template-instantiation;"
                    f"forbidden=[{identity}]"
                ),
                usr=(
                    f"c:@explicit-template@{candidate.offset}@"
                    f"{hashlib.sha256(candidate.statement.encode('utf-8')).hexdigest()}"
                ),
                access=_CX_CXXInvalidAccessSpecifier,
                linkage=_CXLinkage_External,
                offset=candidate.offset,
                spelling_file=spelling_file,
                spelling_offset=candidate.spelling_offset,
                observation_context=observation_context,
                body_surface=False,
            )
        )
    return findings


def _is_qjson_family(canonical_type_spelling: str) -> bool:
    return any(family in canonical_type_spelling for family in _QJSON_FAMILY) or _is_qvariant_json_container(
        canonical_type_spelling
    )


_MAX_TYPE_DEPTH = 128

def _resolve_substituted_type(
    clang: "_LibClang",
    value_type: "_CXType",
    substitutions: dict[str, _CXType],
) -> "_CXType":
    spellings = {
        clang.to_str(clang.lib.clang_getTypeSpelling(value_type)),
        clang.to_str(
            clang.lib.clang_getTypeSpelling(
                clang.lib.clang_getCanonicalType(value_type)
            )
        ),
    }
    for name, replacement in substitutions.items():
        if name in spellings:
            return replacement
    replacements = list(substitutions.values())
    for spelling in spellings:
        match = _re.fullmatch(r"type-parameter-\d+-(\d+)", spelling)
        if match and int(match.group(1)) < len(replacements):
            return replacements[int(match.group(1))]
    return value_type


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


def _stable_type_spelling(clang: "_LibClang", value_type: "_CXType") -> str:
    spelling = clang.to_str(
        clang.lib.clang_getTypeSpelling(
            clang.lib.clang_getCanonicalType(value_type)
        )
    )
    spelling = spelling.replace("std::__1::", "std::").replace(
        "std::__cxx11::", "std::"
    )
    spelling = _re.sub(
        r"\(lambda at (?:.*[/\\])?([^/\\():]+:\d+:\d+)\)",
        r"(lambda at \1)",
        spelling,
    )
    return " ".join(spelling.split())


def _forbidden_type_fingerprint(
    clang: "_LibClang",
    value_type: "_CXType",
    depth: int = 0,
    visited_records: frozenset[str] = frozenset(),
    traverse_records: bool = True,
) -> str | None:
    """Return the canonical outer type whenever any recursively reachable
    semantic component references a forbidden Qt wire container."""

    if depth > _MAX_TYPE_DEPTH:
        message = (
            f"semantic type graph exceeded {_MAX_TYPE_DEPTH} levels at "
            f"{_stable_type_spelling(clang, value_type)}"
        )
        clang.type_graph_errors.append(message)
        return "unresolved-complexity=>QJsonObject"
    canonical = clang.lib.clang_getCanonicalType(value_type)
    spelling = _stable_type_spelling(clang, canonical)
    cache_key = (
        canonical.kind,
        spelling,
        traverse_records,
        int(canonical.data[0] or 0),
        int(canonical.data[1] or 0),
    )
    if cache_key in clang.forbidden_type_cache:
        cached = clang.forbidden_type_cache[cache_key]
        blockers = clang.forbidden_type_blockers.get(cache_key, set())
        if cached is not None or not any(
            clang.forbidden_type_cache.get(blocker) is not None
            for blocker in blockers
        ):
            return cached
        del clang.forbidden_type_cache[cache_key]
        clang.forbidden_type_blockers.pop(cache_key, None)
    if cache_key in clang.forbidden_type_active:
        # Record the cycle edge on every active caller. Negative results may
        # still be cached for performance, but are invalidated if the cut
        # dependency is subsequently proven to reach a forbidden type.
        for active_key in clang.forbidden_type_active:
            clang.forbidden_type_blockers.setdefault(active_key, set()).add(
                cache_key
            )
        return None
    clang.forbidden_type_active.add(cache_key)

    def finish(result: str | None) -> str | None:
        clang.forbidden_type_active.discard(cache_key)
        clang.forbidden_type_cache[cache_key] = result
        if result is not None:
            clang.forbidden_type_blockers.pop(cache_key, None)
        return result

    if _is_qjson_family(spelling):
        return finish(spelling)
    if canonical.kind in _REFERENCE_OR_POINTER_TYPE_KINDS or canonical.kind == _CXType_MemberPointer:
        pointee = clang.lib.clang_getPointeeType(canonical)
        nested = (
            _forbidden_type_fingerprint(
                clang, pointee, depth + 1, visited_records, traverse_records
            )
            if pointee.kind != 0
            else None
        )
        if nested:
            return finish(f"{spelling}=>{nested}")
    if canonical.kind in _ARRAY_TYPE_KINDS:
        nested = _forbidden_type_fingerprint(
            clang,
            clang.lib.clang_getArrayElementType(canonical),
            depth + 1,
            visited_records,
            traverse_records,
        )
        if nested:
            return finish(f"{spelling}=>{nested}")
    if canonical.kind in _FUNCTION_TYPE_KINDS:
        result = clang.lib.clang_getResultType(canonical)
        nested = (
            _forbidden_type_fingerprint(
                clang, result, depth + 1, visited_records, traverse_records
            )
            if result.kind != 0
            else None
        )
        if nested:
            return finish(f"{spelling}=>{nested}")
        for index in range(max(clang.lib.clang_getNumArgTypes(canonical), 0)):
            nested = _forbidden_type_fingerprint(
                clang,
                clang.lib.clang_getArgType(canonical, index),
                depth + 1,
                visited_records,
                traverse_records,
            )
            if nested:
                return finish(f"{spelling}=>{nested}")
    template_count = clang.lib.clang_Type_getNumTemplateArguments(canonical)
    for index in range(max(template_count, 0)):
        argument = clang.lib.clang_Type_getTemplateArgumentAsType(
            canonical, index
        )
        nested = (
            _forbidden_type_fingerprint(
                clang, argument, depth + 1, visited_records, traverse_records
            )
            if argument.kind != 0
            else None
        )
        if nested:
            return finish(f"{spelling}=>{nested}")
    declaration = clang.lib.clang_getTypeDeclaration(canonical)
    if traverse_records and clang.lib.clang_getCursorKind(declaration) in _RECORD_LIKE_KINDS:
        record_usr = clang.to_str(clang.lib.clang_getCursorUSR(declaration))
        if record_usr in SAFE_OPAQUE_RECORD_USRS:
            return finish(None)
        if not record_usr or record_usr not in visited_records:
            nested_visited = (
                visited_records | {record_usr}
                if record_usr
                else visited_records
            )
            found: str | None = None
            def visit(
                cursor: "_CXCursor", _parent: "_CXCursor", _client_data
            ) -> int:
                nonlocal found
                kind = clang.lib.clang_getCursorKind(cursor)
                cursor_usr = _stable_usr(
                    clang.to_str(clang.lib.clang_getCursorUSR(cursor))
                )
                if cursor_usr in ALLOWLIST_USRS:
                    return 1
                nested_types: list[_CXType] = []
                nested_traverse_records = True
                if kind in _SIGNATURE_DECL_KINDS:
                    if kind in _FUNCTION_LIKE_KINDS:
                        nested_types.append(
                            clang.lib.clang_getCursorResultType(cursor)
                        )
                    nested_types.extend(
                        clang.lib.clang_getCursorType(parameter)
                        for parameter in _parameter_cursors(clang, cursor)
                    )
                elif kind in _TYPE_SURFACE_KINDS or kind in _RECORD_LIKE_KINDS:
                    nested_types.append(clang.lib.clang_getCursorType(cursor))
                for nested_type in nested_types:
                    nested = _forbidden_type_fingerprint(
                        clang,
                        nested_type,
                        depth + 1,
                        nested_visited,
                        nested_traverse_records,
                    )
                    if nested:
                        found = nested
                        return 0
                return 1

            callback = clang._visitor_func_type(visit)
            clang.lib.clang_visitChildren(declaration, callback, None)
            if found:
                return finish(f"{spelling}=>{found}")
    return finish(None)


def _is_encoder_shaped(clang: "_LibClang", cursor: "_CXCursor", kind: int) -> tuple[bool, str]:
    """Closed positive boundary: report every forbidden Qt wire type
    referenced by a production function signature, without inferring
    direction, mutability, ownership, or callback behavior."""

    has_forbidden = False
    forbidden_references: list[str] = []
    parameters = _parameter_cursors(clang, cursor)
    result_spelling = "<constructor>"
    if kind in _FUNCTION_LIKE_KINDS:
        result = clang.lib.clang_getCursorResultType(cursor)
        result_spelling = _stable_type_spelling(clang, result)
        result_forbidden = _forbidden_type_fingerprint(clang, result)
        if result_forbidden is not None:
            forbidden_references.append(f"result:{result_forbidden}")
            has_forbidden = True
    parameter_spellings: list[str] = []
    for parameter in parameters:
        parameter_type = clang.lib.clang_getCursorType(parameter)
        parameter_spellings.append(
            _stable_type_spelling(clang, parameter_type)
        )
        parameter_forbidden = _forbidden_type_fingerprint(
            clang, parameter_type
        )
        if parameter_forbidden is not None:
            forbidden_references.append(
                f"param[{len(parameter_spellings) - 1}]:{parameter_forbidden}"
            )
            has_forbidden = True
    cursor_type = clang.lib.clang_getCanonicalType(
        clang.lib.clang_getCursorType(cursor)
    )
    semantic_parent = clang.lib.clang_getCursorSemanticParent(cursor)
    owner_usr = _stable_usr(
        clang.to_str(clang.lib.clang_getCursorUSR(semantic_parent))
    )
    full_signature = (
        f"kind={kind};owner={owner_usr};type={_stable_type_spelling(clang, cursor_type)};"
        f"result={result_spelling};params=[{'|'.join(parameter_spellings)}];"
        f"static={int(kind == _CXCursor_CXXMethod and bool(clang.lib.clang_CXXMethod_isStatic(cursor)))};"
        f"variadic={int(bool(clang.lib.clang_Cursor_isVariadic(cursor)))};"
        f"exception={clang.lib.clang_getCursorExceptionSpecificationType(cursor)};"
        f"calling_conv={clang.lib.clang_getFunctionTypeCallingConv(cursor_type)};"
        f"ref_qualifier={clang.lib.clang_Type_getCXXRefQualifier(cursor_type)};"
        f"forbidden=[{'|'.join(forbidden_references)}]"
    )
    return has_forbidden, full_signature


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


def _cursor_is_system_header(clang: "_LibClang", cursor: "_CXCursor") -> bool:
    location = clang.lib.clang_getCursorLocation(cursor)
    return bool(clang.lib.clang_Location_isInSystemHeader(location))


def _effective_template_base_exposures(
    clang: "_LibClang",
    value_type: "_CXType",
    attribution_cursor: "_CXCursor",
    substitutions: dict[str, _CXType],
    visited: frozenset[tuple[str, tuple[str, ...]]] = frozenset(),
    depth: int = 0,
) -> list[_Exposure]:
    if depth > _MAX_INHERITANCE_DEPTH:
        raise EncoderHygieneError(
            "Alias effective-base walk exceeded its recursion bound"
        )
    resolved_type = _resolve_substituted_type(clang, value_type, substitutions)
    canonical = clang.lib.clang_getCanonicalType(resolved_type)
    declaration = clang.lib.clang_getTypeDeclaration(canonical)
    if clang.lib.clang_getCursorKind(declaration) in (0, _CXCursor_NoDeclFound):
        return [
            _Exposure(
                None,
                attribution_cursor,
                "unresolved public/protected effective alias base",
            )
        ]

    primary = clang.lib.clang_getSpecializedCursorTemplate(declaration)
    record = (
        primary
        if clang.lib.clang_getCursorKind(primary) == _CXCursor_ClassTemplate
        else declaration
    )
    local_substitutions = dict(substitutions)
    parameter_names: list[str] = []
    if record is primary:
        def collect_parameter(
            cursor: "_CXCursor", _parent: "_CXCursor", _client_data
        ) -> int:
            if clang.lib.clang_getCursorKind(cursor) == _CXCursor_TemplateTypeParameter:
                parameter_names.append(
                    clang.to_str(clang.lib.clang_getCursorDisplayName(cursor))
                )
            return 1

        callback = clang._visitor_func_type(collect_parameter)
        clang.lib.clang_visitChildren(record, callback, None)
        argument_count = clang.lib.clang_Type_getNumTemplateArguments(canonical)
        for index, name in enumerate(parameter_names):
            if index >= max(argument_count, 0):
                break
            argument = clang.lib.clang_Type_getTemplateArgumentAsType(
                canonical, index
            )
            local_substitutions[name] = _resolve_substituted_type(
                clang, argument, substitutions
            )

    record_usr = clang.to_str(clang.lib.clang_getCursorUSR(record))
    substitution_key = tuple(
        sorted(
            clang.to_str(
                clang.lib.clang_getTypeSpelling(
                    clang.lib.clang_getCanonicalType(value)
                )
            )
            for value in local_substitutions.values()
        )
    )
    key = (record_usr, substitution_key)
    if record_usr and key in visited:
        return []
    if record_usr:
        visited = visited | {key}

    system_record = _cursor_is_system_header(clang, record)
    exposed: list[_Exposure] = []
    for member in _record_member_candidates(clang, record):
        if system_record:
            member_kind = clang.lib.clang_getCursorKind(member)
            shaped, description = _is_encoder_shaped(
                clang, member, member_kind
            )
            if not shaped or "unresolved" in description:
                continue
        exposed.append(
            _Exposure(member, attribution_cursor, "effective concrete template base")
        )

    bases: list[_CXType] = []
    unresolved_using = False

    def visit(cursor: "_CXCursor", _parent: "_CXCursor", _client_data) -> int:
        nonlocal unresolved_using
        kind = clang.lib.clang_getCursorKind(cursor)
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        if (
            kind == _CXCursor_CXXBaseSpecifier
            and access in _INHERITABLE_ACCESS_SPECIFIERS
        ):
            bases.append(clang.lib.clang_getCursorType(cursor))
        elif (
            kind == _CXCursor_UsingDeclaration
            and access in _INHERITABLE_ACCESS_SPECIFIERS
        ):
            targets = _resolve_using_declaration_targets(clang, cursor)
            if targets:
                exposed.extend(
                    _Exposure(
                        target,
                        attribution_cursor,
                        "effective template using-declaration",
                    )
                    for target in targets
                )
            else:
                unresolved_using = True
        return 1

    callback = clang._visitor_func_type(visit)
    clang.lib.clang_visitChildren(record, callback, None)
    if unresolved_using and not system_record:
        exposed.append(
            _Exposure(
                None,
                attribution_cursor,
                "unresolved public/protected effective template using-declaration",
            )
        )
    for base in bases:
        nested = _effective_template_base_exposures(
            clang,
            base,
            attribution_cursor,
            local_substitutions,
            visited,
            depth + 1,
        )
        if system_record:
            nested = [
                exposure
                for exposure in nested
                if exposure.source_cursor is not None
                and _is_encoder_shaped(
                    clang,
                    exposure.source_cursor,
                    clang.lib.clang_getCursorKind(exposure.source_cursor),
                )[0]
            ]
        exposed.extend(nested)
    return exposed


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

    exposed: list[_Exposure] = []
    target_is_system = _cursor_is_system_header(clang, target_decl)
    for member in _record_member_candidates(clang, target_decl):
        if target_is_system:
            shaped, description = _is_encoder_shaped(
                clang, member, clang.lib.clang_getCursorKind(member)
            )
            if not shaped or "unresolved" in description:
                continue
        exposed.append(
            _Exposure(member, alias_cursor, "public/protected type alias")
        )

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

    substitutions = {
        name: argument
        for name, argument in zip(parameter_names, arguments, strict=False)
    }
    for base in bases:
        exposed.extend(
            _effective_template_base_exposures(
                clang,
                clang.lib.clang_getCursorType(base),
                alias_cursor,
                substitutions,
            )
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
    (including every declaration with no forbidden type reference, which this script
    never even constructs a Finding for -- see `_collect_findings()` --
    but classify() stays total/defensive regardless).

    `counts` is optional so this function stays usable (and directly
    unit-testable) for a single ad-hoc Finding with no surrounding
    dataset; run_check()'s real pass-through always supplies it, since
    exact-count enforcement is the whole point of AllowlistEntry."""

    if not _is_qjson_family(finding.canonical_return_type):
        return "allowed"
    if finding.exact_set_allowed:
        return "allowed"
    entry = ALLOWLIST_BY_KEY.get(finding.key())
    if entry is None:
        return "violation"
    if (
        entry.full_signature_sha256 is None
        or entry.full_signature_sha256
        != hashlib.sha256(
            finding.canonical_return_type.encode("utf-8")
        ).hexdigest()
        or entry.access is None
        or entry.access != finding.access
        or entry.linkage is None
        or entry.linkage != finding.linkage
        or (
            bool(finding.physical_identity_sha256)
            and entry.physical_identity_sha256
            != finding.physical_identity_sha256
        )
        or (
            bool(finding.observation_set_sha256)
            and entry.observation_set_sha256
            != finding.observation_set_sha256
        )
    ):
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
    context_target: str
    exempt_reason: str


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
        if len(fields) != 8:
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: expected "
                "CLASSIFICATION<TAB>POLICY<TAB>TARGET<TAB>TYPE<TAB>SOURCE_DIR"
                "<TAB>BINARY_DIR<TAB>CONTEXT_TARGET<TAB>EXEMPT_REASON"
            )
        (
            classification,
            policy,
            target,
            target_type,
            source_dir,
            binary_dir,
            context_target,
            exempt_reason,
        ) = fields
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
        if classification == "SCAN" and (not context_target or exempt_reason):
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: SCAN target needs context and no exemption"
            )
        if classification != "SCAN" and (context_target or not exempt_reason):
            raise EncoderHygieneError(
                f"{manifest}:{line_number}: exempt target needs a machine-readable reason"
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
            context_target=context_target,
            exempt_reason=exempt_reason,
        )
    if not policies:
        raise EncoderHygieneError(f"Target policy manifest is empty: {manifest}")
    return policies


def _load_evaluated_link_contexts(
    repo_root: Path,
    clang_build_dir: Path,
    policies: dict[str, TargetPolicy],
    contexts_by_target: dict[str, list[CompileContext]],
    audited_configs: Sequence[str],
) -> dict[str, tuple[tuple[str, str], ...]]:
    generated = clang_build_dir / "generated"
    graph_root = generated / "target_link_graph"
    graph_index_path = generated / "target_link_graph_index.txt"
    alias_path = generated / "target_link_aliases.txt"
    external_path = generated / "target_link_external_items.txt"
    if (
        not graph_root.is_dir()
        or not graph_index_path.is_file()
        or not alias_path.is_file()
        or not external_path.is_file()
    ):
        raise EncoderHygieneError(
            "Per-configuration CMake-evaluated target link graph metadata "
            "is missing"
        )
    indexed_files: dict[tuple[str, str], str] = {}
    for line_number, line in enumerate(
        graph_index_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        fields = line.split("\t")
        if (
            len(fields) != 3
            or not all(fields)
            or (fields[0], fields[1]) in indexed_files
            or not _re.fullmatch(r"[0-9a-f]{64}\.txt", fields[2])
        ):
            raise EncoderHygieneError(
                f"{graph_index_path}:{line_number}: malformed/duplicate "
                "evaluated graph index record"
            )
        indexed_files[(fields[0], fields[1])] = fields[2]
    if not indexed_files:
        raise EncoderHygieneError(
            f"Evaluated target graph index is empty: {graph_index_path}"
        )
    if len(set(indexed_files.values())) != len(indexed_files):
        raise EncoderHygieneError(
            f"Evaluated target graph index aliases two consumer/provider "
            f"records to one file: {graph_index_path}"
        )
    actual_configs = {
        path.name for path in graph_root.iterdir() if path.is_dir()
    }
    if actual_configs != set(audited_configs):
        raise EncoderHygieneError(
            "Evaluated link graph configuration set differs from audited "
            f"configurations: graph={sorted(actual_configs)}, "
            f"audited={sorted(audited_configs)}"
        )

    aliases: dict[str, str] = {}
    for line_number, line in enumerate(
        alias_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 2 or not all(fields):
            raise EncoderHygieneError(
                f"{alias_path}:{line_number}: malformed target alias"
            )
        alias, canonical = fields
        previous = aliases.setdefault(alias, canonical)
        if previous != canonical:
            raise EncoderHygieneError(
                f"{alias_path}:{line_number}: ambiguous target alias {alias}"
            )
    external_items = {
        line
        for line in external_path.read_text(encoding="utf-8").splitlines()
        if line
    }

    field_names = {
        "consumer",
        "target",
        "imported",
        "links",
        "interface",
        "compile_links",
        "compile_interface",
        "direct",
        "exclude",
    }
    raw_graphs: dict[
        str, dict[str, dict[str, dict[str, tuple[str, ...]]]]
    ] = {}
    expected_nodes: set[str] | None = None
    imported_nodes: dict[str, bool] = {}
    expected_consumers = {
        target
        for target, policy in policies.items()
        if policy.classification == "SCAN"
        and policy.target_type != "INTERFACE_LIBRARY"
    }
    indexed_consumers = {consumer for consumer, _target in indexed_files}
    if indexed_consumers != expected_consumers:
        raise EncoderHygieneError(
            "Evaluated link graph consumer roots differ from compiled SCAN "
            f"targets: indexed={sorted(indexed_consumers)}, "
            f"expected={sorted(expected_consumers)}"
        )
    for configuration in audited_configs:
        config_dir = graph_root / configuration
        consumer_graphs: dict[
            str, dict[str, dict[str, tuple[str, ...]]]
        ] = {consumer: {} for consumer in expected_consumers}
        actual_files = {path.name for path in config_dir.glob("*.txt")}
        if actual_files != set(indexed_files.values()):
            raise EncoderHygieneError(
                f"Evaluated link graph files for {configuration} differ from "
                f"the exact index: missing="
                f"{sorted(set(indexed_files.values()) - actual_files)}, "
                f"extra={sorted(actual_files - set(indexed_files.values()))}"
            )
        for (indexed_consumer, indexed_target), filename in sorted(
            indexed_files.items()
        ):
            path = config_dir / filename
            fields: dict[str, str] = {}
            for line_number, line in enumerate(
                path.read_text(encoding="utf-8").splitlines(), start=1
            ):
                key, separator, value = line.partition("\t")
                if (
                    not separator
                    or key not in field_names
                    or key in fields
                ):
                    raise EncoderHygieneError(
                        f"{path}:{line_number}: malformed evaluated link record"
                    )
                fields[key] = value
            if set(fields) != field_names:
                raise EncoderHygieneError(
                    f"{path}: incomplete evaluated link record"
                )
            consumer = fields["consumer"]
            target = fields["target"]
            if (
                consumer != indexed_consumer
                or consumer not in consumer_graphs
                or not target
                or target != indexed_target
                or (
                    consumer in consumer_graphs
                    and target in consumer_graphs[consumer]
                )
            ):
                raise EncoderHygieneError(
                    f"{path}: mismatched/duplicate evaluated consumer/target "
                    "identity"
                )
            if fields["imported"] not in {"0", "1"}:
                raise EncoderHygieneError(
                    f"{path}: invalid imported-target marker"
                )
            imported = fields["imported"] == "1"
            previous_imported = imported_nodes.setdefault(target, imported)
            if previous_imported != imported:
                raise EncoderHygieneError(
                    f"{path}: inconsistent imported-target marker"
                )
            consumer_graphs[consumer][target] = {
                field: tuple(
                    edge for edge in fields[field].split("|") if edge
                )
                for field in (
                    "links",
                    "interface",
                    "compile_links",
                    "compile_interface",
                    "direct",
                    "exclude",
                )
            }
        node_sets = {
            frozenset(graph) for graph in consumer_graphs.values()
        }
        if len(node_sets) != 1:
            raise EncoderHygieneError(
                f"Evaluated link graph providers differ by consumer in "
                f"{configuration}"
            )
        nodes = set(next(iter(node_sets)))
        if expected_nodes is None:
            expected_nodes = nodes
        elif nodes != expected_nodes:
            raise EncoderHygieneError(
                "Evaluated link graph target universe differs by "
                f"configuration {configuration}: expected="
                f"{sorted(expected_nodes)}, actual={sorted(nodes)}"
            )
        raw_graphs[configuration] = consumer_graphs

    if expected_nodes is None:
        raise EncoderHygieneError(
            "Evaluated link graph has no target universe"
        )
    if not set(policies).issubset(expected_nodes):
        raise EncoderHygieneError(
            "Evaluated link graph omits classified targets: "
            f"{sorted(set(policies) - expected_nodes)}"
        )
    for alias, canonical in aliases.items():
        if canonical not in expected_nodes or alias in expected_nodes:
            raise EncoderHygieneError(
                f"Invalid evaluated target alias {alias} -> {canonical}"
            )

    graphs: dict[
        str,
        dict[str, dict[str, dict[str, frozenset[str]]]],
    ] = {}
    link_only = _re.compile(r"^\$<LINK_ONLY:([^$<>]+)>$")
    for configuration, raw_consumer_graphs in raw_graphs.items():
        consumer_graphs: dict[
            str, dict[str, dict[str, frozenset[str]]]
        ] = {}
        for consumer, raw_graph in raw_consumer_graphs.items():
            graph: dict[str, dict[str, frozenset[str]]] = {}
            for target, raw_fields in raw_graph.items():
                resolved_fields: dict[str, frozenset[str]] = {}
                for field, raw_edges in raw_fields.items():
                    resolved: set[str] = set()
                    for raw_edge in raw_edges:
                        edge = raw_edge
                        if edge.startswith("::@") or edge in {
                            "debug",
                            "optimized",
                            "general",
                        }:
                            continue
                        match = link_only.fullmatch(edge)
                        if match:
                            edge = match.group(1)
                        if "$<" in edge:
                            raise EncoderHygieneError(
                                f"Target {target} consumer {consumer} "
                                f"configuration {configuration} has an "
                                f"unevaluated {field} link edge: {edge}"
                            )
                        edge = aliases.get(edge, edge)
                        if edge in expected_nodes:
                            resolved.add(edge)
                        elif (
                            imported_nodes[target]
                            and Path(edge).is_absolute()
                            and not Path(edge).resolve().is_relative_to(
                                repo_root
                            )
                        ):
                            continue
                        elif edge not in external_items:
                            raise EncoderHygieneError(
                                f"Target {target} consumer {consumer} "
                                f"configuration {configuration} has an "
                                "unclassified evaluated link edge (generator "
                                f"expression result is not enumerable): {edge}"
                            )
                    resolved_fields[field] = frozenset(resolved)
                graph[target] = resolved_fields
            consumer_graphs[consumer] = graph
        graphs[configuration] = consumer_graphs

    def transitive_usage(
        graph: dict[str, dict[str, frozenset[str]]],
        roots: set[str],
        edge_field: str = "interface",
    ) -> set[str]:
        reached: set[str] = set()
        pending = list(roots)
        while pending:
            current = pending.pop()
            if current in reached:
                continue
            reached.add(current)
            pending.extend(graph[current][edge_field] - reached)
        return reached

    def consumer_link_closure(
        graph: dict[str, dict[str, frozenset[str]]],
        consumer: str,
    ) -> set[str]:
        initial = set(graph[consumer]["links"])
        effective_direct = set(initial)
        while True:
            usage = transitive_usage(graph, effective_direct)
            injected = set().union(
                *(graph[target]["direct"] for target in usage),
                set(),
            )
            expanded = effective_direct | injected
            if expanded == effective_direct:
                break
            effective_direct = expanded
        usage = transitive_usage(graph, effective_direct)
        excluded = set().union(
            *(graph[target]["exclude"] for target in usage),
            set(),
        )
        effective_direct -= excluded
        link_usage = transitive_usage(graph, effective_direct)
        compile_only_roots = (
            set(graph[consumer]["compile_links"])
            - set(graph[consumer]["links"])
        )
        compile_usage = transitive_usage(
            graph,
            effective_direct | compile_only_roots,
            "compile_interface",
        )
        return link_usage | compile_usage

    compile_config_by_audited = {
        configuration: (
            configuration if len(audited_configs) > 1 else ""
        )
        for configuration in audited_configs
    }
    result: dict[str, tuple[tuple[str, str], ...]] = {}
    compiled_roots = [
        target
        for target, policy in policies.items()
        if policy.classification == "SCAN"
        and policy.target_type != "INTERFACE_LIBRARY"
    ]
    for target, policy in policies.items():
        if policy.classification != "SCAN":
            continue
        refs: set[tuple[str, str]] = set()
        for configuration in audited_configs:
            compile_configuration = compile_config_by_audited[configuration]
            consumer_graphs = graphs[configuration]
            for root in compiled_roots:
                if not any(
                    context.configuration == compile_configuration
                    for context in contexts_by_target.get(root, ())
                ):
                    continue
                if (
                    policy.target_type != "INTERFACE_LIBRARY"
                    and root == target
                ) or (
                    policy.target_type == "INTERFACE_LIBRARY"
                    and target
                    in consumer_link_closure(
                        consumer_graphs[root], root
                    )
                ):
                    refs.add((root, compile_configuration))
            if policy.target_type == "INTERFACE_LIBRARY" and (
                policy.context_target,
                compile_configuration,
            ) not in refs:
                raise EncoderHygieneError(
                    f"Interface target {target} context "
                    f"{policy.context_target} does not consume it in evaluated "
                    f"configuration {configuration}"
                )
        if not refs:
            raise EncoderHygieneError(
                f"SCAN target {target} has no evaluated compile contexts"
            )
        result[target] = tuple(sorted(refs))
    return result


def _load_target_header_manifests(
    repo_root: Path,
    clang_build_dir: Path,
    policies: dict[str, TargetPolicy],
    contexts: Sequence[CompileContext],
    audited_configs: Sequence[str],
) -> tuple[
    dict[str, list[Path]],
    dict[str, tuple[tuple[str, str], ...]],
]:
    universe_path = clang_build_dir / "generated" / "target_universe.txt"
    index_path = clang_build_dir / "generated" / "target_header_index.txt"
    if not universe_path.is_file() or not index_path.is_file():
        raise EncoderHygieneError(
            "CMake target universe/header index metadata is missing"
        )

    universe: dict[str, tuple[str, str]] = {}
    for line_number, line in enumerate(
        universe_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 3 or fields[0] in universe:
            raise EncoderHygieneError(
                f"{universe_path}:{line_number}: malformed/duplicate target"
            )
        universe[fields[0]] = (fields[1], fields[2])
    if set(universe) != set(policies):
        raise EncoderHygieneError(
            "CMake target universe and explicit target policy records differ: "
            f"universe-only={sorted(set(universe) - set(policies))}, "
            f"policy-only={sorted(set(policies) - set(universe))}"
        )
    for target, policy in policies.items():
        if universe[target] != (policy.target_type, policy.classification):
            raise EncoderHygieneError(
                f"Target universe metadata mismatch for {target}"
            )

    contexts_by_target: dict[str, list[CompileContext]] = {}
    for context in contexts:
        contexts_by_target.setdefault(context.target, []).append(context)
    evaluated_contexts = _load_evaluated_link_contexts(
        repo_root,
        clang_build_dir,
        policies,
        contexts_by_target,
        audited_configs,
    )

    manifests: dict[str, list[Path]] = {}
    manifest_contexts: dict[str, tuple[tuple[str, str], ...]] = {}
    for line_number, line in enumerate(
        index_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise EncoderHygieneError(
                f"{index_path}:{line_number}: malformed header index record"
            )
        target, policy_name, context_target, manifest_text = fields
        declared_context_targets = tuple(
            context for context in context_target.split(",") if context
        )
        policy = policies.get(target)
        if (
            policy is None
            or policy.classification != "SCAN"
            or policy.policy != policy_name
            or declared_context_targets != (policy.context_target,)
            or target in manifests
        ):
            raise EncoderHygieneError(
                f"{index_path}:{line_number}: mismatched/duplicate header ownership"
            )
        context_refs = evaluated_contexts.get(target, ())
        for actual_context, actual_config in context_refs:
            if actual_context not in contexts_by_target:
                raise EncoderHygieneError(
                    f"Target {target} has no deterministic compiled header context "
                    f"from {actual_context}"
                )
            context_policy = policies.get(actual_context)
            if context_policy is None or context_policy.classification != "SCAN":
                raise EncoderHygieneError(
                    f"Target {target} header context {actual_context} is not a "
                    "production SCAN target"
                )
            if not any(
                context.configuration == actual_config
                for context in contexts_by_target[actual_context]
            ):
                raise EncoderHygieneError(
                    f"Target {target} has no exact {actual_config or '<single>'} "
                    f"compile context from {actual_context}"
                )
        entries = _read_manifest(Path(manifest_text))
        canonical: list[Path] = []
        identities: set[tuple[int, int]] = set()
        for entry in entries:
            if not entry.is_file():
                raise EncoderHygieneError(
                    f"Target {target} header is missing/not regular: {entry}"
                )
            real = entry.resolve()
            identity = _physical_identity(real)
            if identity is None or identity in identities:
                raise EncoderHygieneError(
                    f"Target {target} has duplicate/aliased header identity: {entry}"
                )
            if not (
                real.is_relative_to((repo_root / "src").resolve())
                or real.is_relative_to(policy.binary_dir)
                or real.is_relative_to(policy.source_dir)
            ):
                raise EncoderHygieneError(
                    f"Target {target} header escapes CMake ownership: {real}"
                )
            identities.add(identity)
            canonical.append(real)
        manifests[target] = canonical
        manifest_contexts[target] = context_refs

    expected_scan = {
        target
        for target, policy in policies.items()
        if policy.classification == "SCAN"
    }
    if set(manifests) != expected_scan:
        raise EncoderHygieneError(
            "Target header index is incomplete: "
            f"missing={sorted(expected_scan - set(manifests))}, "
            f"extra={sorted(set(manifests) - expected_scan)}"
        )
    return manifests, manifest_contexts


def _load_target_source_manifests(
    repo_root: Path,
    clang_build_dir: Path,
    policies: dict[str, TargetPolicy],
) -> dict[str, list[Path]]:
    index_path = clang_build_dir / "generated" / "target_source_index.txt"
    if not index_path.is_file():
        raise EncoderHygieneError(f"Target source index is missing: {index_path}")
    manifests: dict[str, list[Path]] = {}
    for line_number, line in enumerate(
        index_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        if not line:
            continue
        fields = line.split("\t")
        if len(fields) != 4:
            raise EncoderHygieneError(
                f"{index_path}:{line_number}: malformed source index record"
            )
        target, policy_name, context_target, manifest_text = fields
        context_targets = tuple(
            context for context in context_target.split(",") if context
        )
        policy = policies.get(target)
        if (
            policy is None
            or policy.classification != "SCAN"
            or policy.policy != policy_name
            or policy.context_target not in context_targets
            or target in manifests
        ):
            raise EncoderHygieneError(
                f"{index_path}:{line_number}: mismatched/duplicate source ownership"
            )
        entries = _read_manifest(Path(manifest_text))
        canonical: list[Path] = []
        identities: set[tuple[int, int]] = set()
        for entry in entries:
            if not entry.is_file():
                raise EncoderHygieneError(
                    f"Target {target} source is missing/not regular: {entry}"
                )
            real = entry.resolve()
            identity = _physical_identity(real)
            if identity is None or identity in identities:
                raise EncoderHygieneError(
                    f"Target {target} has duplicate/aliased source identity: {entry}"
                )
            if not (
                real.is_relative_to((repo_root / "src").resolve())
                or real.is_relative_to(policy.binary_dir)
                or real.is_relative_to(policy.source_dir)
            ):
                raise EncoderHygieneError(
                    f"Target {target} source escapes CMake ownership: {real}"
                )
            identities.add(identity)
            canonical.append(real)
        manifests[target] = canonical
    expected = {
        target
        for target, policy in policies.items()
        if policy.classification == "SCAN"
    }
    if set(manifests) != expected:
        raise EncoderHygieneError(
            "Target source index is incomplete: "
            f"missing={sorted(expected - set(manifests))}, "
            f"extra={sorted(set(manifests) - expected)}"
        )
    return manifests


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
        header_output_roots = [root]
        if metadata.get("MULTI_CONFIG") is True:
            header_output_roots = [
                Path(value).resolve()
                for key, value in metadata.items()
                if key.startswith("INCLUDE_DIR_")
                and isinstance(value, str)
                and Path(value).is_dir()
            ]
        for header_record in metadata.get("HEADERS", []):
            if not isinstance(header_record, list) or len(header_record) < 3:
                raise EncoderHygieneError(
                    f"Malformed HEADERS entry in {metadata_path}: {header_record!r}"
                )
            output = header_record[2]
            if isinstance(output, str) and output:
                for output_root in header_output_roots:
                    generated = (output_root / output).resolve()
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
            include_relative = next(
                (
                    generated.relative_to(output_root).as_posix()
                    for output_root in header_output_roots
                    if generated.is_relative_to(output_root)
                ),
                relative,
            )
            if not any(
                f'#include "{relative}"' in text
                or f"#include <{include_relative}>" in text
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
      - Records a Finding for every function-like or constructor signature,
        at every access/linkage, that semantically references a forbidden
        Qt wire container anywhere in its recursive type graph
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

    `seen` is shared across the run but every key begins with the exact
    target/configuration/TU observation context. Own declarations include
    resolved expansion/spelling identity, USR, kind, signature, access,
    and shape; inherited/alias exposures additionally include their
    exposing declaration and reason. Thus only duplicate visits within
    one observation collapse. Physical occurrences and the complete set
    of contexts which observed each one remain available for the final
    exact-set allowance check."""

    findings: list[Finding] = []
    active_observation_context = ""

    def policy_path(path: Path) -> str:
        real = path.resolve()
        return (
            real.relative_to(repo_root).as_posix()
            if real.is_relative_to(repo_root)
            else real.as_posix()
        )

    def record_if_new(
        *,
        dedup_key: tuple,
        real: Path,
        line: int,
        display_name: str,
        shape_description: str,
        usr: str,
        access: int,
        linkage: int,
        offset: int,
        spelling_file: str,
        spelling_offset: int,
        body_surface: bool = False,
    ) -> None:
        observation_key = (active_observation_context, *dedup_key)
        if observation_key in seen:
            return
        seen.add(observation_key)
        findings.append(
            Finding(
                file=policy_path(real),
                line=line,
                display_name=display_name,
                canonical_return_type=shape_description,
                usr=usr,
                access=access,
                linkage=linkage,
                offset=offset,
                spelling_file=spelling_file,
                spelling_offset=spelling_offset,
                observation_context=active_observation_context,
                body_surface=body_surface,
            )
        )

    def handle_own_declaration(cursor: _CXCursor, kind: int) -> bool:
        canonical_cursor = clang.lib.clang_getCanonicalCursor(cursor)
        cursor_filename, _cursor_line = clang.cursor_file_and_line(cursor)
        canonical_filename, _canonical_line = clang.cursor_file_and_line(
            canonical_cursor
        )
        physical_cursor = (
            canonical_cursor
            if clang.lib.clang_isCursorDefinition(cursor)
            and cursor_filename != canonical_filename
            else cursor
        )
        (
            filename,
            line,
            offset,
            spelling_filename,
            spelling_offset,
        ) = clang.cursor_location_identity(physical_cursor)
        if filename is None:
            return False
        real = Path(filename).resolve()
        if real not in allowed_closure:
            return False
        access = clang.lib.clang_getCXXAccessSpecifier(canonical_cursor)
        is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
        if not is_shaped:
            return False
        usr = _stable_usr(
            clang.to_str(clang.lib.clang_getCursorUSR(canonical_cursor))
        )
        linkage = clang.lib.clang_getCursorLinkage(canonical_cursor)
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
            access=access,
            linkage=linkage,
            offset=offset,
            spelling_file=(
                policy_path(Path(spelling_filename))
                if spelling_filename
                else ""
            ),
            spelling_offset=spelling_offset,
            body_surface=_cursor_is_function_local(clang, cursor),
        )
        return True

    def handle_type_surface(
        cursor: _CXCursor, kind: int, parent: _CXCursor
    ) -> None:
        (
            filename,
            line,
            offset,
            spelling_filename,
            spelling_offset,
        ) = clang.cursor_location_identity(cursor)
        if filename is None:
            return
        real = Path(filename).resolve()
        if real not in allowed_closure:
            return
        body_surface = _type_surface_is_body(
            clang, cursor, kind, parent
        )
        if kind == _CXCursor_ParmDecl and not body_surface:
            # Namespace/record signatures and aliases are already audited as
            # complete outer declarations. ParmDecl is independently needed
            # only inside lambdas/local declarations.
            return
        if (
            kind == _CXCursor_TypeRef
            and not body_surface
            and _type_ref_is_completely_covered_by_parent(
                clang, cursor, parent
            )
        ):
            return
        access = clang.lib.clang_getCXXAccessSpecifier(cursor)
        cursor_type = clang.lib.clang_getCursorType(cursor)
        forbidden_path = _forbidden_type_fingerprint(
            clang,
            cursor_type,
            traverse_records=kind != _CXCursor_TypeRef,
        )
        if forbidden_path is None:
            forbidden_path = _cursor_template_argument_fingerprint(
                clang, cursor, kind
            )
        if (
            forbidden_path is None
            and body_surface
            and _is_unresolved_dependent_type_ref(clang, cursor, kind)
        ):
            forbidden_path = (
                "unresolved-dependent:"
                f"{_stable_type_spelling(clang, cursor_type)}"
                "=>QJsonObject"
            )
        if forbidden_path is None:
            return
        usr = _stable_usr(
            clang.to_str(clang.lib.clang_getCursorUSR(cursor))
        )
        linkage = clang.lib.clang_getCursorLinkage(cursor)
        if not usr:
            owner_usr = _stable_usr(
                clang.to_str(clang.lib.clang_getCursorUSR(parent))
            )
            usr = f"{owner_usr}@type-surface@{kind}@{line}@{offset}"
        owner_usr = _stable_usr(
            clang.to_str(clang.lib.clang_getCursorUSR(parent))
        )
        description = (
            f"kind={kind};owner={owner_usr};"
            f"type={_stable_type_spelling(clang, cursor_type)};"
            f"forbidden={forbidden_path}"
        )
        record_if_new(
            dedup_key=(str(real), line, usr, kind, description, access),
            real=real,
            line=line,
            display_name=clang.to_str(
                clang.lib.clang_getCursorDisplayName(cursor)
            ),
            shape_description=description,
            usr=usr,
            access=access,
            linkage=linkage,
            offset=offset,
            spelling_file=(
                policy_path(Path(spelling_filename))
                if spelling_filename
                else ""
            ),
            spelling_offset=spelling_offset,
            body_surface=body_surface,
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
                    access=class_access,
                    linkage=clang.lib.clang_getCursorLinkage(class_cursor),
                    offset=0,
                    spelling_file=policy_path(attribution_real),
                    spelling_offset=0,
                    body_surface=_cursor_is_function_local(
                        clang, class_cursor
                    ),
                )
                continue
            source_kind = clang.lib.clang_getCursorKind(exposure.source_cursor)
            is_shaped, shape_description = _is_encoder_shaped(
                clang, exposure.source_cursor, source_kind
            )
            if not is_shaped:
                continue
            usr = _stable_usr(
                clang.to_str(
                    clang.lib.clang_getCursorUSR(exposure.source_cursor)
                )
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
                access=class_access,
                linkage=clang.lib.clang_getCursorLinkage(class_cursor),
                offset=0,
                spelling_file=policy_path(attribution_real),
                spelling_offset=0,
                body_surface=_cursor_is_function_local(
                    clang, class_cursor
                ),
            )

    def visitor(cursor: _CXCursor, parent: _CXCursor, _client_data) -> int:
        kind = clang.lib.clang_getCursorKind(cursor)
        if kind in (_CXCursor_TypeAliasDecl, _CXCursor_TypedefDecl):
            handle_type_surface(cursor, kind, parent)
            handle_inheritance_exposure(
                cursor, _alias_reexported_encoders(clang, cursor)
            )
            return 1
        if kind in _RECORD_LIKE_KINDS and clang.lib.clang_isCursorDefinition(cursor):
            handle_inheritance_exposure(cursor)
            return 2  # CXChildVisit_Recurse: still walk this class's own direct members normally.
        if kind in _SIGNATURE_DECL_KINDS:
            handle_own_declaration(cursor, kind)
            return 2
        if kind in _TYPE_SURFACE_KINDS:
            handle_type_surface(cursor, kind, parent)
            return 2
        if kind in _EXPRESSION_SURFACE_KINDS:
            handle_type_surface(cursor, kind, parent)
            return 2
        return 2  # CXChildVisit_Recurse: keep looking for nested declarations.

    visitor_cb = clang._visitor_func_type(visitor)

    if not compile_contexts:
        raise EncoderHygieneError("Header scan received no target/configuration compile contexts")
    for header in headers:
        for context in compile_contexts:
            active_observation_context = (
                f"{context.target}|{context.configuration or '<single>'}|"
                f"header:{policy_path(header)}"
            )
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
                for finding in _collect_explicit_template_findings(
                    header,
                    (*context.arguments, *sysroot_args),
                    context.directory,
                    repo_root,
                    active_observation_context,
                    always_tokenize=True,
                ):
                    explicit_key = (
                        active_observation_context,
                        finding.file,
                        finding.offset,
                        finding.usr,
                        finding.canonical_return_type,
                    )
                    if explicit_key not in seen:
                        seen.add(explicit_key)
                        findings.append(finding)
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
      - collect Findings for source-only declarations at every access and
        linkage, for project-header declarations visible only in the exact
        source preprocessor context, and for every forbidden-typed local,
        local class/field, lambda body, call, construction, initializer,
        cast, new expression, and temporary in every production body. This
        includes internal/static/anonymous/private code: wire construction is
        not made safe by being uncallable from another translation unit;
      - retain each exact target/configuration/TU observation separately for
        the final physical occurrence and bidirectional observation-set pins.

    Exact source-context discovery also includes project-header declarations
        visible only in this TU's exact
        preprocessor context,
        such as a namespace-scope `QJsonObject encodeDeck(const
        DeckList&)` written directly in a .cpp with no header
        declaration at all, which a review round demonstrated compiles
        and remains callable from another translation unit via an
        ad-hoc `extern` forward declaration despite this script
        previously collecting zero findings from source scanning.

    An out-of-line definition is attributed to its exact canonical declaration
    identity rather than counted as another physical declaration, but the
    source-TU observation remains explicit and its body is still traversed.
    Header-path membership alone never suppresses a macro-conditional
    declaration absent from standalone wrappers. `seen` includes the exact
    observation context, so only repeated AST visits within one context
    deduplicate."""

    violations: list[str] = []
    findings: list[Finding] = []
    active_observation_context = ""

    def finding_path(real: Path) -> str:
        try:
            return real.relative_to(repo_root).as_posix()
        except ValueError:
            return real.as_posix()

    def make_visitor(source_real: Path):
        def record_own(cursor: _CXCursor, kind: int) -> bool:
            canonical = clang.lib.clang_getCanonicalCursor(cursor)
            cursor_filename, _cursor_line = clang.cursor_file_and_line(cursor)
            canonical_filename, _canonical_line = clang.cursor_file_and_line(
                canonical
            )
            physical_cursor = (
                canonical
                if clang.lib.clang_isCursorDefinition(cursor)
                and cursor_filename != canonical_filename
                else cursor
            )
            (
                physical_filename,
                canonical_line,
                offset,
                spelling_filename,
                spelling_offset,
            ) = clang.cursor_location_identity(physical_cursor)
            canonical_filename = physical_filename
            if canonical_filename is None:
                return False
            canonical_real = Path(canonical_filename).resolve()
            if canonical_real not in allowed_closure and canonical_real != source_real:
                return False

            access = clang.lib.clang_getCXXAccessSpecifier(canonical)
            linkage = clang.lib.clang_getCursorLinkage(canonical)
            is_shaped, shape_description = _is_encoder_shaped(clang, cursor, kind)
            if not is_shaped:
                return False
            usr = _stable_usr(
                clang.to_str(clang.lib.clang_getCursorUSR(canonical))
            )
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
            observation_key = (active_observation_context, *dedup_key)
            if observation_key in seen:
                return True
            seen.add(observation_key)
            findings.append(
                Finding(
                    file=finding_path(canonical_real),
                    line=canonical_line,
                    display_name=clang.to_str(
                        clang.lib.clang_getCursorDisplayName(cursor)
                    ),
                    canonical_return_type=shape_description,
                    usr=usr,
                    access=access,
                    linkage=linkage,
                    offset=offset,
                    spelling_file=(
                        finding_path(Path(spelling_filename).resolve())
                        if spelling_filename
                        else ""
                    ),
                    spelling_offset=spelling_offset,
                    observation_context=active_observation_context,
                    body_surface=_cursor_is_function_local(clang, cursor),
                )
            )
            return True

        def record_type_surface(
            cursor: _CXCursor, kind: int, parent: _CXCursor
        ) -> None:
            (
                filename,
                line,
                offset,
                spelling_filename,
                spelling_offset,
            ) = clang.cursor_location_identity(cursor)
            if filename is None:
                return
            real = Path(filename).resolve()
            if real not in allowed_closure and real != source_real:
                return
            body_surface = _type_surface_is_body(
                clang, cursor, kind, parent
            )
            if kind == _CXCursor_ParmDecl and not body_surface:
                return
            if (
                kind == _CXCursor_TypeRef
                and not body_surface
                and _type_ref_is_completely_covered_by_parent(
                    clang, cursor, parent
                )
            ):
                return
            access = clang.lib.clang_getCXXAccessSpecifier(cursor)
            linkage = clang.lib.clang_getCursorLinkage(cursor)
            cursor_type = clang.lib.clang_getCursorType(cursor)
            forbidden_path = _forbidden_type_fingerprint(
                clang,
                cursor_type,
                traverse_records=kind != _CXCursor_TypeRef,
            )
            if forbidden_path is None:
                forbidden_path = _cursor_template_argument_fingerprint(
                    clang, cursor, kind
                )
            if (
                forbidden_path is None
                and body_surface
                and _is_unresolved_dependent_type_ref(
                    clang, cursor, kind
                )
            ):
                forbidden_path = (
                    "unresolved-dependent:"
                    f"{_stable_type_spelling(clang, cursor_type)}"
                    "=>QJsonObject"
                )
            if forbidden_path is None:
                return
            usr = _stable_usr(
                clang.to_str(clang.lib.clang_getCursorUSR(cursor))
            )
            if not usr:
                owner_usr = _stable_usr(
                    clang.to_str(clang.lib.clang_getCursorUSR(parent))
                )
                usr = f"{owner_usr}@type-surface@{kind}@{line}@{offset}"
            owner_usr = _stable_usr(
                clang.to_str(clang.lib.clang_getCursorUSR(parent))
            )
            description = (
                f"kind={kind};owner={owner_usr};"
                f"type={_stable_type_spelling(clang, cursor_type)};"
                f"forbidden={forbidden_path}"
            )
            dedup_key = (str(real), line, usr, kind, description, access)
            observation_key = (active_observation_context, *dedup_key)
            if observation_key in seen:
                return
            seen.add(observation_key)
            findings.append(
                Finding(
                    file=finding_path(real),
                    line=line,
                    display_name=clang.to_str(
                        clang.lib.clang_getCursorDisplayName(cursor)
                    ),
                    canonical_return_type=description,
                    usr=usr,
                    access=access,
                    linkage=linkage,
                    offset=offset,
                    spelling_file=(
                        finding_path(Path(spelling_filename).resolve())
                        if spelling_filename
                        else ""
                    ),
                    spelling_offset=spelling_offset,
                    observation_context=active_observation_context,
                    body_surface=body_surface,
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
                    usr = _stable_usr(
                        clang.to_str(
                            clang.lib.clang_getCursorUSR(
                                exposure.source_cursor
                            )
                        )
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
                observation_key = (active_observation_context, *dedup_key)
                if observation_key in seen:
                    continue
                seen.add(observation_key)
                findings.append(
                    Finding(
                        file=finding_path(attribution_real),
                        line=attribution_line,
                        display_name=display_name,
                        canonical_return_type=shape_description,
                        usr=usr,
                        access=class_access,
                        linkage=clang.lib.clang_getCursorLinkage(class_cursor),
                        observation_context=active_observation_context,
                        body_surface=_cursor_is_function_local(
                            clang, class_cursor
                        ),
                    )
                )

        def visitor(cursor: _CXCursor, parent: _CXCursor, _client_data) -> int:
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
                record_type_surface(cursor, kind, parent)
                record_inheritance(
                    cursor, _alias_reexported_encoders(clang, cursor)
                )
                return 1
            if kind in _RECORD_LIKE_KINDS and clang.lib.clang_isCursorDefinition(cursor):
                record_inheritance(cursor)
                return 2
            if kind not in _SIGNATURE_DECL_KINDS:
                if kind in _TYPE_SURFACE_KINDS:
                    record_type_surface(cursor, kind, parent)
                    return 2
                if kind in _EXPRESSION_SURFACE_KINDS:
                    record_type_surface(cursor, kind, parent)
                    return 2
                return 2

            canonical = clang.lib.clang_getCanonicalCursor(cursor)
            canonical_filename, _canonical_line = clang.cursor_file_and_line(canonical)
            if canonical_filename is None:
                return 1  # CXChildVisit_Continue
            canonical_real = Path(canonical_filename).resolve()
            if canonical_real == source_real or canonical_real in allowed_closure:
                record_own(cursor, kind)
            return 2

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
            active_observation_context = (
                f"{context.target}|{context.configuration or '<single>'}|"
                f"source:{finding_path(source_real)}"
            )
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
                for finding in _collect_explicit_template_findings(
                    source,
                    (*context.arguments, *sysroot_args),
                    context.directory,
                    repo_root,
                    active_observation_context,
                    always_tokenize=True,
                ):
                    explicit_key = (
                        active_observation_context,
                        finding.file,
                        finding.offset,
                        finding.usr,
                        finding.canonical_return_type,
                    )
                    if explicit_key not in seen:
                        seen.add(explicit_key)
                        findings.append(finding)
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
    stale = sorted(
        target
        for target in set(policies) - set(contexts_by_target)
        if policies[target].target_type != "INTERFACE_LIBRARY"
    )
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
        target_contexts = contexts_by_target.get(target, [])
        if not target_contexts:
            continue
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
        "Ninja Multi-Config",
        f"-DCMAKE_CXX_COMPILER={clangxx}",
        f"-DCMAKE_CONFIGURATION_TYPES={';'.join(SUPPORTED_PRODUCTION_CONFIGS)}",
        "-DBUILD_TESTING=OFF",
        "-DARKHAM_ENCODER_HYGIENE_CONFIGURE=ON",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    ]
    ninja = shutil.which("ninja")
    if ninja is None and shutil.which("mise"):
        ninja = subprocess.check_output(
            ["mise", "which", "ninja"], text=True
        ).strip()
    if not ninja:
        raise EncoderHygieneError(
            "Ninja is required for the audited multi-config matrix"
        )
    configure_cmd.append(f"-DCMAKE_MAKE_PROGRAM={ninja}")
    if qt_prefix:
        configure_cmd.append(f"-DCMAKE_PREFIX_PATH={qt_prefix}")

    subprocess.run(configure_cmd, check=True, cwd=repo_root)
    target_policies = _load_target_policies(build_dir)
    scan_targets = [
        target
        for target, policy in target_policies.items()
        if policy.classification == "SCAN"
        and policy.target_type != "INTERFACE_LIBRARY"
    ]
    if not scan_targets:
        raise EncoderHygieneError(
            "CMake target policy metadata contains no production SCAN target"
        )
    for configuration in SUPPORTED_PRODUCTION_CONFIGS:
        subprocess.run(
            [
                "cmake",
                "--build",
                str(build_dir),
                "--config",
                configuration,
                "--target",
                *scan_targets,
            ],
            check=True,
            cwd=repo_root,
        )


def run_check(
    repo_root: Path,
    clang_build_dir: Path,
    skip_configure: bool,
    enforce_identity_pins: bool = False,
) -> list[Finding]:
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
    audited_configs_path = generated_dir / "audited_configs.txt"
    audited_configs = tuple(
        line.strip()
        for line in audited_configs_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    )
    if not audited_configs:
        raise EncoderHygieneError(
            f"Audited production configuration manifest is empty: {audited_configs_path}"
        )
    target_policies = _load_target_policies(clang_build_dir)
    external_roots = _external_roots(clang_build_dir.resolve())
    _validate_target_inventory(target_policies, all_contexts, external_roots)
    target_headers, target_header_contexts = _load_target_header_manifests(
        repo_root,
        clang_build_dir,
        target_policies,
        all_contexts,
        audited_configs,
    )
    target_source_manifests = _load_target_source_manifests(
        repo_root, clang_build_dir, target_policies
    )
    contexts_by_target: dict[str, list[CompileContext]] = {}
    for context in all_contexts:
        contexts_by_target.setdefault(context.target, []).append(context)
    for target, policy in target_policies.items():
        if policy.classification != "SCAN" or policy.target_type == "INTERFACE_LIBRARY":
            continue
        actual_configs = {
            context.configuration
            for context in contexts_by_target.get(target, [])
        }
        expected_configs = (
            set(audited_configs)
            if len(audited_configs) > 1
            else {""}
        )
        if actual_configs != expected_configs:
            raise EncoderHygieneError(
                f"Target {target} configuration coverage mismatch: "
                f"expected={sorted(expected_configs)}, actual={sorted(actual_configs)}"
            )

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
    policy_rank = {"domain": 0, "foundation": 1, "application": 2}
    for target, sources in target_source_manifests.items():
        target_policy_name = target_policies[target].policy
        for source in sources:
            if source.suffix.lower() not in {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}:
                continue
            existing = source_owner_policy.get(source)
            if existing is None or policy_rank[target_policy_name] < policy_rank[existing]:
                source_owner_policy[source] = target_policy_name

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

    cxx_source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm"}
    all_manifested_code = {
        source
        for sources in target_source_manifests.values()
        for source in sources
        if source.suffix.lower() in cxx_source_suffixes
    }
    for target, policy in target_policies.items():
        if policy.classification != "SCAN":
            continue
        manifested_code = {
            source
            for source in target_source_manifests[target]
            if source.suffix.lower() in cxx_source_suffixes
        }
        generated_code = (
            autogen_by_target[target].code_files
            if target in autogen_by_target
            else frozenset()
        )
        unexplained_contexts = [
            context.source
            for context in contexts_by_target.get(target, [])
            if context.source not in all_manifested_code
            and context.source not in generated_code
        ]
        if unexplained_contexts:
            raise EncoderHygieneError(
                f"Target {target} compile commands contain sources absent from "
                "its complete CMake SOURCES/INTERFACE_SOURCES/AUTOGEN metadata:\n"
                + "\n".join(f"  {source}" for source in unexplained_contexts)
            )

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
    headers_by_policy = {
        policy: frozenset(
            header
            for target, headers in target_headers.items()
            if target_policies[target].policy == policy
            for header in headers
        )
        for policy in ("domain", "foundation", "application")
    }
    if not domain_source_closure.issubset(headers_by_policy["domain"]) or not foundation_source_closure.issubset(
        headers_by_policy["domain"] | headers_by_policy["foundation"]
    ):
        raise EncoderHygieneError(
            "Legacy domain/foundation closure manifests are not fully owned by "
            "the closed-world per-target header universe"
        )
    header_closures = {
        "domain": headers_by_policy["domain"] | frozenset(domain_fragments),
        "foundation": (
            headers_by_policy["domain"]
            | headers_by_policy["foundation"]
            | frozenset(domain_fragments)
            | frozenset(foundation_fragments)
        ),
        "application": (
            headers_by_policy["domain"]
            | headers_by_policy["foundation"]
            | headers_by_policy["application"]
            | frozenset(domain_fragments)
            | frozenset(foundation_fragments)
        ),
    }

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
            for context in contexts_by_target.get(target, [])
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

    # One dedup set is shared across both passes, but keys include the exact
    # target/configuration/TU context. Duplicate AST visits in one observation
    # collapse; observations from another wrapper, source, target, or config
    # remain explicit for the final bidirectional observation-set pin.
    seen: set[tuple] = set()
    structural_violations: list[str] = []
    generated_roots = frozenset(
        closure.root
        for policy_closures in autogen_closures.values()
        for closure in policy_closures
    )
    generated_compile_units = frozenset(
        [
            context.source
            for context in all_contexts
            if target_policies[context.target].classification == "SCAN"
            and not context.source.is_relative_to((repo_root / "src").resolve())
        ]
        + [
            source
            for target, sources in target_source_manifests.items()
            for source in sources
            if target_policies[target].classification == "SCAN"
            and source.suffix.lower() in cxx_source_suffixes
            and not source.is_relative_to((repo_root / "src").resolve())
        ]
    )
    generated_target_headers = frozenset(
        header
        for headers in target_headers.values()
        for header in headers
        if not header.is_relative_to((repo_root / "src").resolve())
    )
    owned_paths = _owned_path_policy(
        repo_root,
        generated_roots,
        generated_compile_units | generated_target_headers,
    )

    try:
        findings: list[Finding] = []
        for target, headers in target_headers.items():
            target_policy = target_policies[target]
            context_candidates = [
                context
                for context_target, context_configuration in target_header_contexts[target]
                for context in contexts_by_target[context_target]
                if context.configuration == context_configuration
            ]
            unique_contexts: dict[tuple, CompileContext] = {}
            for context in context_candidates:
                unique_contexts.setdefault(
                    (
                        context.target,
                        context.configuration,
                        context.directory,
                        context.arguments,
                    ),
                    context,
                )
            target_generated = autogen_by_target.get(target)
            allowed_headers = (
                header_closures[target_policy.policy]
                | (
                    target_generated.code_files
                    if target_generated is not None
                    else frozenset()
                )
            )
            findings += _scan_headers(
                clang,
                idx,
                headers,
                list(unique_contexts.values()),
                sysroot_args,
                repo_root,
                external_roots,
                allowed_headers,
                seen,
                structural_violations,
                owned_paths,
            )
        findings += _scan_headers(
            clang,
            idx,
            domain_fragments,
            policy_contexts["domain"],
            sysroot_args,
            repo_root,
            external_roots,
            header_closures["domain"],
            seen,
            structural_violations,
            owned_paths,
        )
        findings += _scan_headers(
            clang,
            idx,
            foundation_fragments,
            policy_contexts["foundation"],
            sysroot_args,
            repo_root,
            external_roots,
            header_closures["foundation"],
            seen,
            structural_violations,
            owned_paths,
        )

        base_closures = {
            "domain": header_closures["domain"],
            "foundation": header_closures["foundation"],
            "application": header_closures["application"],
        }
        for target, target_policy in target_policies.items():
            if target_policy.classification != "SCAN":
                continue
            target_contexts = contexts_by_target.get(target, [])
            compiled_sources = {context.source for context in target_contexts}
            manifested_code = {
                source
                for source in target_source_manifests[target]
                if source.suffix.lower() in cxx_source_suffixes
            }
            uncompiled_sources = manifested_code - compiled_sources
            scan_contexts = list(all_contexts)
            if uncompiled_sources:
                for (
                    context_target,
                    context_configuration,
                ) in target_header_contexts[target]:
                    for fallback in contexts_by_target[context_target]:
                        if fallback.configuration != context_configuration:
                            continue
                        scan_contexts.extend(
                            CompileContext(
                                source=source,
                                directory=fallback.directory,
                                arguments=fallback.arguments,
                                output=fallback.output,
                                target=target,
                                configuration=fallback.configuration,
                            )
                            for source in uncompiled_sources
                        )
            target_sources = sorted(
                compiled_sources | uncompiled_sources, key=str
            )
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
                    contexts=scan_contexts,
                )
                structural_violations.extend(target_violations)
                findings += target_findings
    finally:
        clang.lib.clang_disposeIndex(idx)

    if clang.type_graph_errors:
        raise EncoderHygieneError(
            "Semantic type graph could not be closed within the configured "
            "complexity bound:\n"
            + "\n".join(sorted(set(clang.type_graph_errors)))
        )

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

    physical_findings: dict[tuple, Finding] = {}
    observed_contexts: dict[tuple, set[str]] = {}
    for finding in findings:
        physical_key = (
            finding.file,
            finding.line,
            finding.offset,
            finding.spelling_file,
            finding.spelling_offset,
            finding.usr,
            finding.canonical_return_type,
            finding.access,
            finding.linkage,
            finding.body_surface,
        )
        physical_findings.setdefault(physical_key, finding)
        observed_contexts.setdefault(physical_key, set()).add(
            finding.observation_context
        )

    expected_contexts_by_file: dict[str, set[tuple[str, str]]] = {}
    for target, headers in target_headers.items():
        contexts = [
            context
            for context_target, context_configuration in target_header_contexts[target]
            for context in contexts_by_target[context_target]
            if context.configuration == context_configuration
        ]
        for header in headers:
            relative = (
                header.relative_to(repo_root).as_posix()
                if header.is_relative_to(repo_root)
                else header.as_posix()
            )
            expected_contexts_by_file.setdefault(relative, set()).update(
                (
                    context.target,
                    context.configuration or "<single>",
                )
                for context in contexts
            )
    for target, sources in target_source_manifests.items():
        for source in sources:
            if source.suffix.lower() not in cxx_source_suffixes:
                continue
            relative = (
                source.relative_to(repo_root).as_posix()
                if source.is_relative_to(repo_root)
                else source.as_posix()
            )
            source_contexts = [
                context
                for context in all_contexts
                if context.source == source
            ]
            if not source_contexts:
                source_contexts = [
                    context
                    for context_target, context_configuration in target_header_contexts[target]
                    for context in contexts_by_target[context_target]
                    if context.configuration == context_configuration
                ]
            expected_contexts_by_file.setdefault(relative, set()).update(
                (
                    context.target,
                    context.configuration or "<single>",
                )
                for context in source_contexts
            )

    missing_observations: list[str] = []
    for physical_key, finding in physical_findings.items():
        if finding.key() not in ALLOWLIST_BY_KEY:
            continue
        required = expected_contexts_by_file.get(finding.file, set())
        observed_pairs = {
            (parts[0], parts[1])
            for context in observed_contexts.get(physical_key, set())
            if len(parts := context.split("|", 2)) >= 2
        }
        missing = required - observed_pairs
        if missing:
            missing_observations.append(
                f"  {finding.file}:{finding.line} {finding.usr}: "
                f"missing target/config observations {sorted(missing)}"
            )
    if missing_observations:
        raise EncoderHygieneError(
            "Forbidden-type declaration observation matrix is incomplete:\n"
            + "\n".join(missing_observations)
        )

    pinned_findings: list[Finding] = []
    for physical_key, finding in physical_findings.items():
        physical_payload = json.dumps(
            {
                "file": finding.file,
                "line": finding.line,
                "offset": finding.offset,
                "spelling_file": finding.spelling_file,
                "spelling_offset": finding.spelling_offset,
                "usr": finding.usr,
            },
            sort_keys=True,
            separators=(",", ":"),
        )
        observation_payload = json.dumps(
            sorted(observed_contexts.get(physical_key, set())),
            separators=(",", ":"),
        )
        pinned_findings.append(
            replace(
                finding,
                physical_identity_sha256=hashlib.sha256(
                    physical_payload.encode("utf-8")
                ).hexdigest(),
                observation_set_sha256=hashlib.sha256(
                    observation_payload.encode("utf-8")
                ).hexdigest(),
            )
        )

    named_findings = [
        finding
        for finding in pinned_findings
        if finding.key() in ALLOWLIST_BY_KEY
    ]
    body_findings = [
        finding
        for finding in pinned_findings
        if finding.key() not in ALLOWLIST_BY_KEY and finding.body_surface
    ]
    named_digest = _identity_set_digest(named_findings)
    local_digest = _identity_set_digest(body_findings)
    identity_errors: list[str] = []
    if named_digest != _NAMED_ALLOWLIST_IDENTITY_SET_SHA256:
        identity_errors.append(
            "named allowlist physical/observation set changed "
            f"(expected {_NAMED_ALLOWLIST_IDENTITY_SET_SHA256}, found "
            f"{named_digest}; {len(named_findings)} physical occurrences)"
        )
    if local_digest != _LOCAL_WIRE_SURFACE_SET_SHA256:
        identity_errors.append(
            "function-body wire surface set changed "
            f"(expected {_LOCAL_WIRE_SURFACE_SET_SHA256}, found "
            f"{local_digest}; {len(body_findings)} physical occurrences)"
        )
    if enforce_identity_pins and identity_errors:
        raise EncoderHygieneError(
            "Exact forbidden-type physical identity/observation allowance "
            "mismatch. A declaration, macro expansion, local construction, or "
            "target/configuration/TU observation was added, removed, or moved; "
            "review the full production surface before deliberately repinning:\n"
            + "\n".join(f"  {error}" for error in identity_errors)
        )
    if enforce_identity_pins:
        return [
            replace(finding, exact_set_allowed=True)
            if finding.key() not in ALLOWLIST_BY_KEY and finding.body_surface
            else finding
            for finding in pinned_findings
        ]
    return pinned_findings


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
        "<repo-root>/build-encoder-hygiene-matrix).",
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
        help="Print every forbidden Qt wire-type surface found (with its "
        "exact USR and classification) and exit, without applying pass/fail "
        "policy. Intended for maintainers updating ALLOWLIST after a "
        "deliberate, reviewed change to the exact positive surface allowlist "
        "-- never as a way to silence a real "
        "violation.",
    )
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    clang_build_dir = (
        args.clang_build_dir
        or (repo_root / "build-encoder-hygiene-matrix")
    ).resolve()

    try:
        findings = run_check(
            repo_root,
            clang_build_dir,
            args.skip_configure,
            enforce_identity_pins=not args.list,
        )
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
            f"error: {len(violations)} forbidden Qt wire-type surface(s) are "
            "not in the exact positive allowlist (file + USR + canonical "
            "semantic signature + access + occurrence count):",
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
        f"Encoder hygiene: {len(findings)} exact forbidden-type surface(s) "
        "accounted for across all explicitly-owned production targets, all "
        f"{len(ALLOWLIST)} allowlist entries accounted for at their exact "
        "expected occurrence count, zero violations, and every header, fragment, "
        "source target/configuration, and owned AUTOGEN unit stayed within its "
        "allowed domain/foundation dependency-direction closure."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
