cmake_minimum_required(VERSION 3.25)

project(ArkhamHorrorLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(QT_QML_GENERATE_QMLLS_INI ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Quick QuickControls2 WebSockets)

qt_standard_project_setup(REQUIRES 6.8)

# include(CTest) establishes the real BUILD_TESTING cache value (default ON)
# before the QtKeychain FetchContent block below temporarily forces it off,
# so the save/restore around that block is well-defined regardless of
# inclusion order.
include(CTest)

# --- QtKeychain (BSD-3-Clause; see third_party/qtkeychain/) ----------------
# Pinned to the exact 0.17.0 release commit for reproducible builds. Used by
# QtKeychainTokenStore for secure per-profile authentication-token storage
# (Secret Service/libsecret or KWallet on Linux). QtKeychain's own insecure
# plaintext fallback is never enabled by this project -- see
# QtKeychainJobFactory, which explicitly calls setInsecureFallback(false) on
# every job it creates.
#
# A small downstream hardening patch is applied on top of the pinned commit
# (the GIT_TAG below still names the exact unmodified upstream commit; the
# patch is a separate, reviewable diff applied after checkout, not a
# different pin). See
# third_party/qtkeychain/patches/0001-disable-insecure-kwallet-fallback-and-error-mapping.patch
# for the full rationale; in short, upstream's KWallet backend at this pin
# (a) unconditionally reads/migrates a legacy plaintext QSettings entry on
# open even when insecureFallback() is false, and (b) reports a failed final
# KWallet write/delete D-Bus reply as success. Both are closed by the patch.
include(FetchContent)
FetchContent_Declare(
    qtkeychain
    GIT_REPOSITORY https://github.com/frankosterfeld/qtkeychain.git
    GIT_TAG 875f77d9f61bd97fd84cca47ce3bc71186dfbd09 # v0.17.0
    UPDATE_DISCONNECTED TRUE # already pinned to an exact commit; skip the
        # update step on every reconfigure so the one-time PATCH_COMMAND
        # below is never re-run (and re-applied) against an already-patched
        # working tree.
    PATCH_COMMAND git apply --whitespace=error
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtkeychain/patches/0001-disable-insecure-kwallet-fallback-and-error-mapping.patch"
)

# Force QtKeychain's own build options before it configures: Qt 6 only, no
# CLI/QtQuick demo apps, no translations, and libsecret support enabled (the
# Linux/Secret Service backend). BUILD_TESTING is saved and forced off only
# for this subproject so QtKeychain's own autotest suite is never built,
# independent of this project's own BUILD_TESTING setting.
set(BUILD_WITH_QT5 OFF CACHE BOOL "" FORCE)
set(BUILD_TEST_APPLICATION OFF CACHE BOOL "" FORCE)
set(BUILD_QTQUICK_DEMO OFF CACHE BOOL "" FORCE)
set(BUILD_TRANSLATIONS OFF CACHE BOOL "" FORCE)
set(LIBSECRET_SUPPORT ON CACHE BOOL "" FORCE)
set(_arkham_saved_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(qtkeychain)
set(BUILD_TESTING "${_arkham_saved_build_testing}" CACHE BOOL "" FORCE)
unset(_arkham_saved_build_testing)

# packaging/check_encoder_hygiene.py's inclusion-graph audit (see that
# script's module docstring) must classify qtkeychain's own vendored/
# generated files as external (never subject to the domain/foundation
# dependency-direction closure check). A review round demonstrated an
# earlier revision derived that exemption from a hand-guessed, purely
# lexical `<clang-build-dir>/_deps` path -- unsound because nothing ties
# that exact subtree name to what FetchContent actually, truly used, and
# a differently-configured/future FetchContent layout could silently
# stop matching it. `qtkeychain_SOURCE_DIR`/`qtkeychain_BINARY_DIR` are
# themselves the real, authoritative CMake variables FetchContent_
# MakeAvailable() just populated above (see
# https://cmake.org/cmake/help/latest/module/FetchContent.html#command:fetchcontent_makeavailable) --
# this is genuine dependency-package metadata, not a guess -- so this
# manifest is generated directly from them instead. Written once, here,
# rather than per-target: qtkeychain is a shared external dependency of
# the whole project, not scoped to either arkham_domain_models or
# arkham_foundation individually.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/external_roots.txt"
    "${qtkeychain_SOURCE_DIR}\n${qtkeychain_BINARY_DIR}\n"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/autogen_targets.txt" "")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt" "")

# FetchContent builds qtkeychain via add_subdirectory(), so the namespaced
# alias normally created by the installed QtKeychainConfig.cmake does not
# exist; create it ourselves so downstream target_link_libraries() calls are
# identical to what a system-installed QtKeychain package would provide.
if(NOT TARGET Qt6Keychain::Qt6Keychain)
    add_library(Qt6Keychain::Qt6Keychain ALIAS qt6keychain)
endif()

# The strict value-semantic contract/domain-model layer (card catalog,
# decks, game list/lifecycle, validated identifiers, the raw JSON AST +
# its one canonical exact-QJson adapter family, and the shared JSON
# decode helpers/utilities they are built from) lives in its own
# `arkham_domain_models` library (see add_library() below), deliberately
# kept independent of Qt Gui/Network/Widgets/Keychain -- it links only
# Qt6::Core. This is a real, compiler-enforced dependency boundary (not a
# convention/comment): every other part of this codebase (networking,
# auth, session/profile management, keychain storage, input/controller
# UI glue) may depend on this domain-model layer, but nothing in this
# layer may ever depend back on any of that, so a future accidental bare
# `#include "NetworkAuthenticationClient.h"` from e.g. Decks.cpp would
# fail to compile (that header is not on this target's include path at
# all -- see target_include_directories() below, which is scoped to only
# src/domain, not shared build-wide with arkham_foundation's own "src"
# include root). tests/probes/DomainFoundationIsolationProbe.cpp and the
# domain_foundation_isolation ctest below compile exactly that bare-
# #include scenario (against AuthModels.h) and require it to fail with
# the expected file-not-found diagnostic. That compiled proof covers only
# the bare-#include spelling, though -- the *comprehensive*,
# spelling-independent guarantee (also catching "../"-relative, absolute,
# and symlinked #includes of a foundation-only header from within this
# layer) comes from packaging/check_encoder_hygiene.py's
# clang_getInclusions()-based resolved-inclusion-graph audit, which is
# what this codebase actually relies on to enforce this boundary; see
# that script's module docstring and the longer rationale comment further
# below, next to this target's own FILE_SET registration.
#
# This split is also what makes packaging/check_encoder_hygiene.py's AST
# scan well-scoped by *target/file-set membership*, not by a
# hand-maintained path list independently prone to drift: domain_headers.txt/
# foundation_headers.txt are generated by
# arkham_write_target_header_set_manifest() (cmake/PathManifest.cmake)
# directly from each target's own *live* CMake HEADER_SETS/
# HEADER_SET_<name> metadata below -- never re-derived from the
# ARKHAM_DOMAIN_HEADERS/ARKHAM_FOUNDATION_HEADERS variables themselves --
# so even a second/later target_sources(...FILE_SET...) call registering
# more headers on either target would automatically be captured, with no
# separate change needed here or in the Python script. domain_sources.txt/
# foundation_sources.txt (and the empty domain_fragments.txt/
# foundation_fragments.txt -- see ARKHAM_DOMAIN_FRAGMENTS/
# ARKHAM_FOUNDATION_FRAGMENTS below) remain plain arkham_write_path_manifest()
# writes, since sources/fragments have no FILE_SET-equivalent target
# metadata to source them from instead. The script independently,
# exhaustively parses exactly (and only) the public headers registered in
# each target's own HEADERS file set, so a pre-existing, unrelated header
# elsewhere in this codebase (e.g. AuthModels.h, which is *not* part of
# arkham_domain_models's file set) can never collide with -- or need to be
# carved out via a basename exception from -- the small set of legitimate
# QJson-returning symbols this domain layer itself declares (RawJson.h's
# three canonical toExactQJson*() adapters, and JsonDecode.h's
# decode-direction field/array/object helpers).
set(ARKHAM_DOMAIN_HEADERS
    src/domain/CardCatalog.h
    src/domain/ContractPin.h
    src/domain/ContractRevision.h
    src/domain/Decks.h
    src/domain/Games.h
    src/domain/Identifiers.h
    src/domain/JsonDecode.h
    src/domain/RawJson.h
    src/domain/ValueOrError.h
)

set(ARKHAM_DOMAIN_SOURCES
    src/domain/CardCatalog.cpp
    src/domain/ContractPin.cpp
    src/domain/ContractRevision.cpp
    src/domain/Decks.cpp
    src/domain/Games.cpp
    src/domain/Identifiers.cpp
    src/domain/JsonDecode.cpp
    src/domain/RawJson.cpp
)

# No .inc/.inl/.ipp/.tpp (or other non-header, non-source project file
# that can still be #include-d and contribute real declarations/macros)
# currently exists anywhere in this domain layer -- this list is
# deliberately empty today. It exists as real, exercised infrastructure
# (see arkham_check_domain_header_inventory()'s HEADER_SUFFIXES below,
# and packaging/check_encoder_hygiene.py's domain_fragments.txt manifest
# read/scan, which treats every entry here identically to a header: its
# own wrapper-TU parse, its own inclusion-graph audit, its own
# QJson-finding collection) so that if such a fragment is ever
# introduced, it is covered by construction rather than by a suffix-glob
# guess made after the fact -- a future PR only needs to add the file
# path here (and to disk with a registered suffix), never touch either
# the CMake inventory check or the Python scanner itself.
set(ARKHAM_DOMAIN_FRAGMENTS)

# Every other header this build compiles (networking, auth, session,
# keychain, input/controller UI glue, app composition) -- explicitly
# kept as a *separate* declared list from ARKHAM_DOMAIN_HEADERS above
# (rather than one combined list that the domain/non-domain split would
# then have to re-derive by filtering), so the FILE_SET registered on
# each of the two targets below is unambiguous, and so
# arkham_check_domain_header_inventory() below can still positively
# prove -- at CMake *configuration* time, not merely via a test's own
# runtime scan -- that their union, and only their union, matches every
# header that actually exists under src/ (in any nested subdirectory,
# under any of the several header suffixes it now globs), the same
# guarantee this project has relied on since the single-list version of
# this check was first introduced.
set(ARKHAM_FOUNDATION_HEADERS
    src/AppBootstrap.h
    src/AppSessionComposition.h
    src/AuthModels.h
    src/AuthTransportSecurity.h
    src/CompatibilityEvaluator.h
    src/ControllerGlyphs.h
    src/FocusController.h
    src/IAuthenticationClient.h
    src/ICapabilityProbe.h
    src/IKeychainJobFactory.h
    src/InputMapper.h
    src/InputRouter.h
    src/IProfileStore.h
    src/ITokenStore.h
    src/NetworkAuthenticationClient.h
    src/NetworkCapabilityProbe.h
    src/ProbeResult.h
    src/QSettingsProfileStore.h
    src/QtKeychainJobFactory.h
    src/QtKeychainTokenStore.h
    src/SemanticCommand.h
    src/ServerCapabilities.h
    src/ServerProfile.h
    src/SessionCoordinator.h
    src/TokenEnvelope.h
    src/TokenValidation.h
    src/UrlValidator.h
)

set(ARKHAM_FOUNDATION_SOURCES
    src/AppSessionComposition.cpp
    src/AuthModels.cpp
    src/AuthTransportSecurity.cpp
    src/CompatibilityEvaluator.cpp
    src/ControllerGlyphs.cpp
    src/FocusController.cpp
    src/InputMapper.cpp
    src/InputRouter.cpp
    src/NetworkAuthenticationClient.cpp
    src/NetworkCapabilityProbe.cpp
    src/QSettingsProfileStore.cpp
    src/QtKeychainJobFactory.cpp
    src/QtKeychainTokenStore.cpp
    src/SemanticCommand.cpp
    src/ServerCapabilities.cpp
    src/ServerProfile.cpp
    src/SessionCoordinator.cpp
    src/TokenEnvelope.cpp
    src/TokenValidation.cpp
    src/UrlValidator.cpp
)

# See ARKHAM_DOMAIN_FRAGMENTS above -- same rationale, for the
# foundation layer. Empty today; not currently exercised by any real
# file, but read/validated/scanned identically to
# ARKHAM_FOUNDATION_HEADERS by both the CMake inventory check and the
# Python AST policy scanner.
set(ARKHAM_FOUNDATION_FRAGMENTS)

# Fail configuration outright (not merely a test) if the same header were
# ever accidentally declared in both lists above -- e.g. a careless merge
# leaving a domain header duplicated into ARKHAM_FOUNDATION_HEADERS too --
# which would silently widen the domain target's own header file set out
# from under the AST policy scan's intended scope.
foreach(_arkham_dup_check_header IN LISTS ARKHAM_DOMAIN_HEADERS)
    list(FIND ARKHAM_FOUNDATION_HEADERS "${_arkham_dup_check_header}" _arkham_dup_check_index)
    if(NOT _arkham_dup_check_index EQUAL -1)
        message(FATAL_ERROR
            "${_arkham_dup_check_header} is declared in both ARKHAM_DOMAIN_HEADERS and ARKHAM_FOUNDATION_HEADERS; every header must belong to exactly one of the two.")
    endif()
endforeach()
unset(_arkham_dup_check_header)
unset(_arkham_dup_check_index)

# Structural enforcement (see cmake/DomainHeaderInventory.cmake): fails
# CMake *configuration* -- not merely a test, and not merely a source-text
# scan of already-known files -- the moment the union of
# ARKHAM_DOMAIN_HEADERS and ARKHAM_FOUNDATION_HEADERS above drifts from
# what actually exists under src/**, in either direction (a new/nested
# header of any registered suffix added to disk but never declared here,
# or a declared header deleted from disk). This is what makes "add a new
# nested header and never register it" fail the build itself, rather than
# relying on any runtime directory scan (recursive or not) that a test
# executable performs on its own -- see
# tests/cmake/DomainHeaderInventoryPolicyTest.cmake for the mutation
# tests proving this function itself behaves correctly on all three
# cases (match / undeclared-extra / declared-but-missing), and
# HEADER_SUFFIXES below for the full list of suffixes a header-like file
# anywhere under src/ is recognized under -- not just "*.h".
include(cmake/DomainHeaderInventory.cmake)
include(cmake/PathManifest.cmake)
arkham_append_encoder_hygiene_target(
    TARGET qt6keychain
    CLASSIFICATION EXTERNAL
    EXEMPT_REASON "FetchContent dependency; source and object paths must remain under registered qtkeychain roots"
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt"
)
arkham_check_domain_header_inventory(
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    HEADER_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
    HEADER_SUFFIXES h hpp hh hxx inc inl ipp tpp
    DECLARED_HEADERS ${ARKHAM_DOMAIN_HEADERS} ${ARKHAM_FOUNDATION_HEADERS} ${ARKHAM_DOMAIN_FRAGMENTS} ${ARKHAM_FOUNDATION_FRAGMENTS}
    OUT_ERROR _arkham_header_inventory_error
)
if(_arkham_header_inventory_error)
    message(FATAL_ERROR "${_arkham_header_inventory_error}")
endif()
unset(_arkham_header_inventory_error)

add_library(arkham_domain_models STATIC
    ${ARKHAM_DOMAIN_SOURCES}
)

# CMake's FILE_SET HEADERS mechanism (available since this project's
# already-declared cmake_minimum_required(VERSION 3.25)) is what makes
# this target's public header *set* an actual first-class piece of build
# metadata -- reflected in generated build system files, the target's own
# HEADER_SET_all/HEADER_DIRS_all properties, and any future `cmake
# --install`/export -- rather than merely "whatever happens to be
# reachable by #include from files listed as ordinary sources", which a
# text-glob-based inventory alone (see cmake/DomainHeaderInventory.cmake)
# cannot itself prove. BASE_DIRS matches this target's own
# target_include_directories() below, so consumers see the identical
# `#include "RawJson.h"`-style paths as before this split.
#
# BASE_DIRS is src/domain, NOT src -- domain headers physically live in
# their own subdirectory, and this target's public include path (see
# target_include_directories() below) is scoped to exactly that
# subdirectory too. This is a real, compiler-enforced boundary, not just
# a comment: a review round demonstrated that when both this target and
# arkham_foundation exposed the *same* public "src" include root, nothing
# actually stopped a domain header/source from directly
# `#include`-ing a foundation-only header such as AuthModels.h (e.g. to
# derive a domain-local type from AuthenticateRequest and reuse its
# inherited toJson() -- a lossy public encoder the domain policy scan
# never inspects, since AuthModels.h is intentionally out of its scope).
# With BASE_DIRS/the include path narrowed to src/domain only, a *bare*
# `#include "AuthModels.h"` cannot even find the file (a hard "file not
# found" compile error) -- see tests/probes/DomainFoundationIsolationProbe.cpp
# and the domain_foundation_isolation ctest below, which compiles exactly
# that scenario and requires it to fail with that exact diagnostic.
#
# Narrowing this include path alone does NOT block every #include
# spelling, though: a subsequent review round showed a domain
# header/source could still reach AuthModels.h via a *relative-parent*
# `#include "../AuthModels.h"` (quote-form #include always searches
# relative to the including file's own directory -- here, src/domain --
# *before* consulting any `-I`/BASE_DIRS path at all, so this narrowing
# has no effect on that spelling whatsoever), an absolute path, or a
# symlink placed inside src/domain/ pointing outside it. None of those
# are blockable by any include-path/compile-flag configuration, however
# it is arranged; only inspecting what the compiler's #include actually,
# truly resolved to can catch all of them uniformly, which is exactly
# what packaging/check_encoder_hygiene.py's clang_getInclusions()-based
# inclusion-graph audit (see that script's module docstring) does, and
# is therefore the actual, comprehensive enforcement of this boundary --
# this include-path narrowing and the compiled probe below remain in
# place as real, additional, defense-in-depth for the one spelling they
# do fully guarantee.
list(TRANSFORM ARKHAM_DOMAIN_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE _arkham_domain_headers_absolute)
target_sources(arkham_domain_models
    PUBLIC
        FILE_SET arkham_domain_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src/domain"
        FILES ${_arkham_domain_headers_absolute}
)
unset(_arkham_domain_headers_absolute)

target_include_directories(arkham_domain_models PUBLIC src/domain)
target_link_libraries(arkham_domain_models PUBLIC
    Qt6::Core
)
arkham_append_encoder_hygiene_target(
    TARGET arkham_domain_models
    CLASSIFICATION SCAN
    POLICY domain
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt"
)

# See packaging/check_encoder_hygiene.py: plain, one-path-per-line
# manifests (not JSON -- these paths are simple filesystem paths with no
# characters that would need escaping, so a text format avoids any
# CMake-side JSON-escaping subtlety) naming exactly the headers, sources,
# and fragments above, regenerated by CMake every time it reconfigures.
# domain_headers.txt/domain_sources.txt are written by
# arkham_write_target_header_set_manifest()/arkham_write_target_source_manifest()
# (see cmake/PathManifest.cmake) directly from arkham_domain_models's own
# live HEADER_SETS/SOURCES metadata -- never from the
# ARKHAM_DOMAIN_HEADERS/ARKHAM_DOMAIN_SOURCES variables -- so neither can
# ever silently miss a header/source added via some other/later
# target_sources() call on this target; domain_fragments.txt remains a
# plain arkham_write_path_manifest() write from its own variable (no
# FILE_SET-equivalent live metadata exists for non-header, non-source
# fragments in this project). This is the single source of truth the AST
# policy script's own scan is scoped to -- it never independently
# re-derives or hand-maintains any of these file lists, so they cannot
# silently drift from what this target actually compiles/exposes, the
# same guarantee arkham_check_domain_header_inventory() above already
# gives the *build* itself. The matching foundation_*.txt manifests are
# written just below, once ARKHAM_FOUNDATION_HEADERS/
# ARKHAM_FOUNDATION_SOURCES/ARKHAM_FOUNDATION_FRAGMENTS are populated, so
# the policy script can independently, exhaustively audit AuthModels.h's
# two legitimate non-domain encoders too -- not merely trust that nothing
# else in foundation needs auditing.
#
# Both manifest-writer calls are wrapped in cmake_language(DEFER DIRECTORY
# "${CMAKE_CURRENT_SOURCE_DIR}" CALL ...) -- see
# arkham_write_target_header_set_manifest()'s doc comment in
# cmake/PathManifest.cmake for why: each function's own
# get_target_property(... HEADER_SETS/SOURCES) read happens the moment
# the function body actually executes, so calling it directly/
# immediately right here would only see whichever target_sources() calls
# happen to textually precede this point -- a later FILE_SET/source
# registration anywhere else in this file would be silently invisible to
# both manifests. Deferring the whole call to end-of-directory-processing
# (this project has no add_subdirectory() calls of its own, so that is
# effectively "after every target_sources() call in this entire file")
# closes that gap for both header-set names and source lists alike. See
# tests/cmake/DeferredTargetManifestPolicyTest.cmake for the real,
# fail-before/pass-after CMake mutation test proving this.
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_write_target_header_set_manifest
        TARGET arkham_domain_models
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/domain_headers.txt"
)

if(ARKHAM_ENCODER_HYGIENE_CONFIGURE)
    cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        CALL arkham_write_encoder_hygiene_target_universe
            OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_universe.txt"
            HEADER_INDEX_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_header_index.txt"
            HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/target_headers"
            SOURCE_INDEX_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_source_index.txt"
            SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/target_sources"
    )
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_write_target_source_manifest
        TARGET arkham_domain_models
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/domain_sources.txt"
)
# Register this target's AUTOGEN directory as owned generated metadata,
# never as an external/trusted root. The policy checker cross-checks its
# concrete C/C++ files against CMake's AutogenInfo.json and audits them
# through the generated compilation unit. Deferred because
# AUTOGEN_BUILD_DIR is finalized at end-of-directory processing.
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_append_target_autogen_manifest
        TARGET arkham_domain_models
        POLICY domain
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/autogen_targets.txt"
)
arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/domain_fragments.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_DOMAIN_FRAGMENTS}
)

add_library(arkham_foundation STATIC
    ${ARKHAM_FOUNDATION_SOURCES}
)

# See the arkham_domain_models FILE_SET above for why FILES is passed as
# this target's own absolute paths rather than the
# CMAKE_CURRENT_SOURCE_DIR-relative ARKHAM_FOUNDATION_HEADERS list
# directly.
list(TRANSFORM ARKHAM_FOUNDATION_HEADERS PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
    OUTPUT_VARIABLE _arkham_foundation_headers_absolute)
target_sources(arkham_foundation
    PUBLIC
        FILE_SET arkham_foundation_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
        FILES ${_arkham_foundation_headers_absolute}
)
unset(_arkham_foundation_headers_absolute)

cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_write_target_header_set_manifest
        TARGET arkham_foundation
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_headers.txt"
)
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_write_target_source_manifest
        TARGET arkham_foundation
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_sources.txt"
)
# See the matching arkham_domain_models call above.
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_append_target_autogen_manifest
        TARGET arkham_foundation
        POLICY foundation
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/autogen_targets.txt"
)
arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_fragments.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_FOUNDATION_FRAGMENTS}
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC
    arkham_domain_models
    Qt6::Gui
    Qt6::Network
    Qt6Keychain::Qt6Keychain
)
arkham_append_encoder_hygiene_target(
    TARGET arkham_foundation
    CLASSIFICATION SCAN
    POLICY foundation
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt"
)

qt_add_executable(arkham-horror
    src/main.cpp
)

qt_add_qml_module(arkham-horror
    URI ArkhamHorror
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

arkham_append_encoder_hygiene_target(
    TARGET arkham-horror
    CLASSIFICATION SCAN
    POLICY application
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt"
)
if(TARGET arkham-horror_tooling)
    arkham_append_encoder_hygiene_target(
        TARGET arkham-horror_tooling
        CLASSIFICATION SCAN
        POLICY application
        CONTEXT_TARGET arkham-horror
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/target_policy.txt"
    )
endif()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    CALL arkham_append_target_autogen_manifest
        TARGET arkham-horror
        POLICY application
        OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/autogen_targets.txt"
)

target_link_libraries(arkham-horror PRIVATE
    arkham_foundation
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::WebSockets
)

install(TARGETS arkham-horror
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    qt_add_executable(arkham-foundation-tests
        tests/FoundationTests.cpp
    )
    target_link_libraries(arkham-foundation-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME foundation COMMAND arkham-foundation-tests)

    qt_add_executable(arkham-contract-tests
        tests/ContractTests.cpp
    )
    target_compile_definitions(arkham-contract-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-contract-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME contract COMMAND arkham-contract-tests)

    qt_add_executable(arkham-contract-drift-tests
        tests/ContractDriftTests.cpp
    )
    target_compile_definitions(arkham-contract-drift-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-contract-drift-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME contract_drift COMMAND arkham-contract-drift-tests)

    qt_add_executable(arkham-card-catalog-tests
        tests/CardCatalogTests.cpp
    )
    target_compile_definitions(arkham-card-catalog-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-card-catalog-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME card_catalog COMMAND arkham-card-catalog-tests)

    qt_add_executable(arkham-decks-tests
        tests/DecksTests.cpp
    )
    target_compile_definitions(arkham-decks-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-decks-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME decks COMMAND arkham-decks-tests)

    qt_add_executable(arkham-games-tests
        tests/GamesTests.cpp
    )
    target_compile_definitions(arkham-games-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-games-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME games COMMAND arkham-games-tests)

    qt_add_executable(arkham-raw-json-tests
        tests/RawJsonTests.cpp
    )
    target_link_libraries(arkham-raw-json-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME raw_json COMMAND arkham-raw-json-tests)

    # Encoder hygiene (the structural proof that no public domain-model
    # header exposes a lossy QJson-returning encoder outside the tiny
    # canonical adapter/decode-helper allowlist) is no longer a compiled
    # C++ source-text scanner test here. It is now a compiler-AST-based
    # Python tool -- see packaging/check_encoder_hygiene.py -- run via the
    # `mise run contracts:check-encoder-hygiene` /
    # `mise run contracts:test-encoder-hygiene-script` tasks and a
    # dedicated "Encoder hygiene" CI job (.github/workflows/ci.yml),
    # exactly mirroring how packaging/verify_contract_provenance.py's own
    # live-network check and offline unit tests are run outside ctest.
    # This keeps it decoupled from this project's own default compiler
    # (which may not be Clang/expose libclang), and from this ctest binary
    # dir's own compile_commands.json (the tool configures its own
    # dedicated Clang-toolchain build directory internally). See that
    # script's module docstring for the full architecture rationale.

    # A dedicated CMake-script ctest case (run via `cmake -P`, not
    # compiled into any C++ test binary) directly proving
    # arkham_check_domain_header_inventory() itself -- the function whose
    # FATAL_ERROR gate above is what actually makes a new/nested/
    # unregistered header fail CMake configuration -- behaves correctly
    # on all three of its possible outcomes, exercised against a
    # synthetic scratch tree independent of this project's real src/
    # layout. See tests/cmake/DomainHeaderInventoryPolicyTest.cmake.
    add_test(NAME cmake_header_inventory_policy
        COMMAND ${CMAKE_COMMAND}
            -DARKHAM_MODULE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake
            -DARKHAM_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/domain_header_inventory_policy_scratch
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/DomainHeaderInventoryPolicyTest.cmake
    )

    # A dedicated CMake-script ctest case proving why
    # arkham_write_target_header_set_manifest() and
    # arkham_write_target_source_manifest() (cmake/PathManifest.cmake)
    # MUST be invoked through cmake_language(DEFER DIRECTORY ... CALL
    # ...) -- as every real call site below does -- rather than directly.
    # cmake_language(DEFER) requires a real project()-configured
    # directory scope (it errors in plain `cmake -P` script mode), so
    # this driver script itself spawns a genuine, separate `cmake -S/-B`
    # configure of a tiny disposable scratch project rather than
    # `include()`-ing PathManifest.cmake directly the way
    # DomainHeaderInventoryPolicyTest.cmake does; see that script's own
    # top-of-file comment for the full fail-before/pass-after narrative.
    add_test(NAME cmake_deferred_target_manifest_policy
        COMMAND ${CMAKE_COMMAND}
            -DARKHAM_MODULE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake
            -DARKHAM_SCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/deferred_target_manifest_policy_scratch
            -DCMAKE_COMMAND=${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/DeferredTargetManifestPolicyTest.cmake
    )

    # Compiled proof that arkham_domain_models cannot #include a
    # foundation-only header (see tests/probes/DomainFoundationIsolationProbe.cpp
    # for the exact reviewer-reported scenario this reproduces, and for why
    # it deliberately covers only the bare-#include spelling -- other
    # spellings are the Python AST inclusion-graph audit's job, not this
    # compiled canary's). This target links ONLY arkham_domain_models --
    # never arkham_foundation -- and is EXCLUDE_FROM_ALL so its
    # deliberately permanent compile failure never breaks the normal
    # `all`/default build or `cmake --build .` without an explicit target.
    #
    # This is an OBJECT library, not an executable: an earlier revision
    # linked an executable and called the leaked type's inherited,
    # out-of-line toJson(), so if the include-path regression this probe
    # exists to detect were ever reintroduced, the build would still fail
    # overall -- but via an unrelated LINK error (undefined reference,
    # since only arkham_domain_models is linked, never AuthModels.cpp)
    # rather than the intended COMPILE "file not found" error, which a
    # review round showed meant a plain WILL_FAIL ctest could "pass" for
    # entirely the wrong reason and would not actually catch the
    # regression it claimed to. An OBJECT library never invokes the
    # linker at all (`cmake --build --target <objlib>` only compiles),
    # and the probe source itself makes no out-of-line call (see its own
    # comments), so the only possible failure reason left is the compiler
    # itself failing to resolve the #include.
    #
    # The ctest case below builds exactly this one target on demand and
    # is registered with PASS_REGULAR_EXPRESSION (not WILL_FAIL): ctest
    # PASSES only when the build's own output actually contains the
    # specific "AuthModels.h" file-not-found diagnostic (tolerant of both
    # Clang's and GCC's differing wording, since this project's default
    # `mise run test`/CI build may use either -- see the regex comment
    # below), regardless of the process's exit code, which is precisely
    # what proves the *correct* failure reason rather than merely *some*
    # failure.
    add_library(arkham-domain-foundation-isolation-probe OBJECT
        EXCLUDE_FROM_ALL
        tests/probes/DomainFoundationIsolationProbe.cpp
    )
    target_link_libraries(arkham-domain-foundation-isolation-probe PRIVATE
        arkham_domain_models
    )
    add_test(NAME domain_foundation_isolation
        COMMAND ${CMAKE_COMMAND}
            --build ${CMAKE_BINARY_DIR}
            --target arkham-domain-foundation-isolation-probe
            --config $<CONFIG>
    )
    # Matches both Clang's (`'AuthModels.h' file not found`) and GCC's
    # (`AuthModels.h: No such file or directory`) diagnostic wording for
    # exactly this missing header -- this project's default `mise run
    # test`/CI "Unit tests"/"Build" jobs use ubuntu-latest's default
    # compiler (GCC, no explicit CXX override), while local
    # development/AppleClang produces the other wording; the dedicated
    # "Encoder hygiene" CI job explicitly installs/uses Clang. CMake's
    # ctest regex engine supports `|` alternation exactly like this.
    set_tests_properties(domain_foundation_isolation PROPERTIES
        PASS_REGULAR_EXPRESSION "AuthModels\\.h[^\\n]*(file not found|No such file or directory)"
    )

    qt_add_executable(arkham-network-tests
        tests/NetworkTests.cpp
    )
    target_compile_definitions(arkham-network-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-network-tests PRIVATE
        arkham_foundation
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME network COMMAND arkham-network-tests)

    qt_add_executable(arkham-token-store-tests
        tests/TokenStoreTests.cpp
    )
    target_link_libraries(arkham-token-store-tests PRIVATE
        arkham_foundation
        Qt6Keychain::Qt6Keychain
        Qt6::Test
    )
    add_test(NAME token_store COMMAND arkham-token-store-tests)

    qt_add_executable(arkham-auth-client-tests
        tests/AuthClientTests.cpp
    )
    target_link_libraries(arkham-auth-client-tests PRIVATE
        arkham_foundation
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME auth_client COMMAND arkham-auth-client-tests)

    qt_add_executable(arkham-session-coordinator-tests
        tests/SessionCoordinatorTests.cpp
    )
    target_link_libraries(arkham-session-coordinator-tests PRIVATE
        arkham_foundation
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME session_coordinator COMMAND arkham-session-coordinator-tests)

    qt_add_executable(arkham-app-bootstrap-tests
        tests/AppBootstrapTests.cpp
    )
    target_link_libraries(arkham-app-bootstrap-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME app_bootstrap COMMAND arkham-app-bootstrap-tests)

    qt_add_executable(arkham-semantic-command-tests
        tests/SemanticCommandTests.cpp
    )
    target_link_libraries(arkham-semantic-command-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME semantic_command COMMAND arkham-semantic-command-tests)

    qt_add_executable(arkham-focus-controller-tests
        tests/FocusControllerTests.cpp
    )
    target_link_libraries(arkham-focus-controller-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME focus_controller COMMAND arkham-focus-controller-tests)

    qt_add_executable(arkham-input-mapper-tests
        tests/InputMapperTests.cpp
    )
    target_link_libraries(arkham-input-mapper-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME input_mapper COMMAND arkham-input-mapper-tests)

    qt_add_executable(arkham-input-router-tests
        tests/InputRouterTests.cpp
    )
    target_link_libraries(arkham-input-router-tests PRIVATE
        arkham_foundation
        Qt6::Gui
        Qt6::Test
    )
    add_test(NAME input_router COMMAND arkham-input-router-tests)

    qt_add_executable(arkham-controller-glyph-tests
        tests/ControllerGlyphTests.cpp
    )
    target_link_libraries(arkham-controller-glyph-tests PRIVATE
        arkham_foundation
        Qt6::Test
    )
    add_test(NAME controller_glyph COMMAND arkham-controller-glyph-tests)

    qt_add_executable(arkham-semantic-input-fixture-tests
        tests/SemanticInputFixtureTests.cpp
    )
    target_compile_definitions(arkham-semantic-input-fixture-tests PRIVATE
        ARKHAM_TEST_QML_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml"
    )
    target_link_libraries(arkham-semantic-input-fixture-tests PRIVATE
        arkham_foundation
        Qt6::Gui
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Test
    )
    add_test(NAME semantic_input_fixture COMMAND arkham-semantic-input-fixture-tests)

    # QtKeychain's kwallet5 backend (the two downstream hardening patches'
    # target) only exists in keychain_unix.cpp, compiled solely for
    # Unix-like, non-Apple platforms, and this test requires a session D-Bus
    # bus, so it is Linux-only and run under dbus-run-session.
    if(UNIX AND NOT APPLE)
        find_package(Qt6 6.8 REQUIRED COMPONENTS DBus)
        qt_add_executable(arkham-kwallet-hardening-tests
            tests/QtKeychainKWalletHardeningTests.cpp
        )
        target_link_libraries(arkham-kwallet-hardening-tests PRIVATE
            arkham_foundation
            Qt6Keychain::Qt6Keychain
            Qt6::DBus
            Qt6::Test
        )
        find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
        if(DBUS_RUN_SESSION_EXECUTABLE)
            add_test(NAME kwallet_hardening
                COMMAND "${DBUS_RUN_SESSION_EXECUTABLE}" --
                        "$<TARGET_FILE:arkham-kwallet-hardening-tests>")
        else()
            message(WARNING
                "dbus-run-session not found; the kwallet_hardening test "
                "will be built but not registered with CTest. Install the "
                "'dbus' package to enable it (expected to be present in CI).")
        endif()
    endif()
endif()
