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
# layer may ever depend back on any of that, so a future accidental
# `#include "NetworkAuthenticationClient.h"` from e.g. Decks.cpp would
# fail to compile (that header is not on this target's include path at
# all -- see target_include_directories() below, which is scoped to only
# src/domain, not shared build-wide with arkham_foundation's own "src"
# include root). This is a verified, compiled guarantee, not merely an
# asserted one: tests/probes/DomainFoundationIsolationProbe.cpp and the
# domain_foundation_isolation ctest below reproduce the exact opposite
# scenario (a domain-linked translation unit #include-ing a
# foundation-only header, AuthModels.h) and require it to fail to
# compile.
#
# This split is also what makes packaging/check_encoder_hygiene.py's AST
# scan (see the domain_headers.txt/domain_sources.txt and
# foundation_headers.txt/foundation_sources.txt manifests
# arkham_write_path_manifest() -- cmake/PathManifest.cmake -- writes
# below) well-scoped by *target/file-set membership*, not by a
# hand-maintained path list independently prone to drift: it
# independently, exhaustively parses exactly (and only) the public
# headers registered in each target's own HEADERS file set, so a
# pre-existing, unrelated header elsewhere in this codebase (e.g.
# AuthModels.h, which is *not* part of arkham_domain_models's file set)
# can never collide with -- or need to be carved out via a basename
# exception from -- the small set of legitimate QJson-returning symbols
# this domain layer itself declares (RawJson.h's three canonical
# toExactQJson*() adapters, and JsonDecode.h's decode-direction
# field/array/object helpers).
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
arkham_check_domain_header_inventory(
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    HEADER_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
    HEADER_SUFFIXES h hpp hh hxx
    DECLARED_HEADERS ${ARKHAM_DOMAIN_HEADERS} ${ARKHAM_FOUNDATION_HEADERS}
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
# With BASE_DIRS/the include path narrowed to src/domain only, such an
# #include cannot even find the file (a hard "file not found" compile
# error), rather than merely being disallowed by convention. See
# tests/probes/DomainFoundationIsolationProbe.cpp and the
# domain_foundation_isolation ctest below, which compiles exactly that
# scenario and requires it to fail.
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

# See packaging/check_encoder_hygiene.py: a plain, one-path-per-line
# manifest (not JSON -- these paths are simple filesystem paths with no
# characters that would need escaping, so a text format avoids any
# CMake-side JSON-escaping subtlety) naming exactly the headers and
# sources above, regenerated by CMake every time it reconfigures, via
# arkham_write_path_manifest() (see cmake/PathManifest.cmake). This is
# the single source of truth the AST policy script's own scan is scoped
# to -- it never independently re-derives or hand-maintains this file
# list, so it cannot silently drift from what this target actually
# compiles, the same guarantee arkham_check_domain_header_inventory()
# above already gives the *build* itself. The matching foundation_*.txt
# manifests are written just below, once ARKHAM_FOUNDATION_HEADERS/
# ARKHAM_FOUNDATION_SOURCES are populated, so the policy script can
# independently, exhaustively audit AuthModels.h's two legitimate
# non-domain encoders too -- not merely trust that nothing else in
# foundation needs auditing.
arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/domain_headers.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_DOMAIN_HEADERS}
)
arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/domain_sources.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_DOMAIN_SOURCES}
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

arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_headers.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_FOUNDATION_HEADERS}
)
arkham_write_path_manifest(
    OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/generated/foundation_sources.txt"
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    PATHS ${ARKHAM_FOUNDATION_SOURCES}
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC
    arkham_domain_models
    Qt6::Gui
    Qt6::Network
    Qt6Keychain::Qt6Keychain
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

    # Compiled proof that arkham_domain_models cannot #include a
    # foundation-only header (see tests/probes/DomainFoundationIsolationProbe.cpp
    # for the exact reviewer-reported scenario this reproduces). This
    # target links ONLY arkham_domain_models -- never arkham_foundation --
    # and is EXCLUDE_FROM_ALL so its deliberately permanent compile failure
    # never breaks the normal `all`/default build or `cmake --build .`
    # without an explicit target. The ctest case below builds exactly this
    # one target on demand and is registered WILL_FAIL TRUE, so ctest
    # PASSES precisely when this probe FAILS to compile (proving
    # isolation) and FAILS if the probe ever unexpectedly starts
    # compiling (proving a regression of the include-root separation).
    add_executable(arkham-domain-foundation-isolation-probe
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
    set_tests_properties(domain_foundation_isolation PROPERTIES WILL_FAIL TRUE)

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
