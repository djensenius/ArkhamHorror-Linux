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
// domain-only-linked) CMake target -- see CMakeLists.txt's
// arkham-domain-foundation-isolation-probe target and the
// domain_foundation_isolation ctest case, which asserts the *build itself*
// fails, not merely that some separate lint/scan flags this file. If this
// file ever starts compiling, the include-root separation between domain
// and foundation has regressed and the ctest case (WILL_FAIL TRUE) turns
// that regression into an immediate, hard ctest failure.
#include "AuthModels.h" // foundation-only: must fail with a file-not-found
                        // compile error once arkham_domain_models's public
                        // include path no longer contains plain "src".
#include "Decks.h"      // domain: resolves fine via arkham_domain_models's
                        // own "src/domain" public include path.

namespace {

// Reproduces the reviewer's exact probe: a domain-local type publicly
// inheriting Arkham::AuthenticateRequest, reusing its inherited toJson().
struct LeakedDomainAuthenticateRequest : Arkham::AuthenticateRequest {};

} // namespace

int main() {
  LeakedDomainAuthenticateRequest leaked{{"probe@example.invalid", "x"}};
  return leaked.toJson().isEmpty() ? 1 : 0;
}
