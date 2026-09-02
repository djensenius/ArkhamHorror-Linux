// Deliberately-broken compile-time probe: this file must NEVER compile.
//
// A review round on this project reproduced a real bypass of the
// domain/foundation split by compiling a translation unit that included a
// domain header (Decks.h) alongside a foundation-only header (AuthModels.h),
// derived a domain-local type from Arkham::AuthenticateRequest, and called
// its *inherited* toJson() -- a lossy public QJson encoder the
// domain-scoped encoder-hygiene AST policy never inspects, since
// AuthModels.h is intentionally out of that policy's scope (it belongs to
// FOUNDATION_ALLOWLIST, audited separately -- see
// packaging/check_encoder_hygiene.py).
//
// That bypass was possible only because arkham_domain_models and
// arkham_foundation both exposed the identical public "src" include root,
// so "#include "AuthModels.h"" resolved even though this translation unit
// links only arkham_domain_models (see the isolation_probe_target below).
//
// This source file is compiled into a real (EXCLUDE_FROM_ALL,
// domain-only-linked) CMake OBJECT library -- see CMakeLists.txt's
// arkham-domain-foundation-isolation-probe target and the
// domain_foundation_isolation ctest case, which asserts the *build itself*
// fails with the exact expected file-not-found diagnostic (via
// PASS_REGULAR_EXPRESSION -- see CMakeLists.txt for why WILL_FAIL alone
// was insufficient: a later review round showed it could pass for the
// WRONG reason, e.g. a link error, if this were compiled as a linked
// executable instead), not merely that some separate lint/scan flags this
// file. If this file ever starts compiling cleanly, the include-root
// separation between domain and foundation has regressed and the ctest
// case turns that regression into an immediate, hard ctest failure.
//
// This is deliberately compile-only (an OBJECT library, never linked) and
// makes no out-of-line call into AuthModels.cpp: an earlier version of
// this probe was a linked *executable* calling
// LeakedDomainAuthenticateRequest::toJson() (inherited, out-of-line) --
// which meant that if the include-path regression this probe exists to
// detect were reintroduced, the build would still fail, but via an
// unrelated LINK error (undefined reference to AuthModels.cpp's out-of-
// line toJson(), since only arkham_domain_models is linked) rather than
// the intended COMPILE "file not found" error, silently defeating the
// probe's actual purpose. Type formation alone (below) needs no such
// call, so a regression here can only be masked by ALSO reintroducing
// AuthModels.cpp into this target's link -- not a plausible accident.
//
// This one permanent, compiled scenario deliberately covers only the
// *bare* #include "AuthModels.h" spelling (the one narrowing this
// target's own public include path to just src/domain, see
// CMakeLists.txt, actually blocks). Other spellings that bypass
// include-path narrowing entirely regardless of how it is configured --
// "../AuthModels.h" (quote-form #include always searches relative to the
// including file's own directory first), an absolute path, or a symlink
// placed inside src/domain/ pointing outside it -- are structurally
// caught instead by packaging/check_encoder_hygiene.py's
// clang_getInclusions()-based inclusion-graph audit, which inspects what
// the compiler's #include actually, truly resolved to rather than
// depending on any particular spelling or include-path configuration;
// see that script's module docstring.
#include "AuthModels.h" // foundation-only: must fail with a file-not-found
                        // compile error since arkham_domain_models's public
                        // include path is scoped to only src/domain (never
                        // plain "src").

namespace {

// Reproduces the reviewer's exact probe: a domain-local type publicly
// inheriting Arkham::AuthenticateRequest. Deliberately does NOT call
// toJson() (or anything else out-of-line) -- see the rationale above for
// why this probe must never depend on the linker to detect a regression.
struct LeakedDomainAuthenticateRequest : Arkham::AuthenticateRequest {};

// Mere type formation: proves the type is complete and constructible
// (i.e. that AuthModels.h was actually reachable and usable, not merely
// forward-declared/half-parsed) without invoking the linker at all --
// this function is never called from anywhere, and this whole file is
// compiled only as an OBJECT library member (see CMakeLists.txt), which
// never invokes the linker in the first place.
[[maybe_unused]] void formTypeOnly() {
  LeakedDomainAuthenticateRequest leaked{{"probe@example.invalid", "x"}};
  (void)leaked;
}

} // namespace
