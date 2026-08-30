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

# The canonical, explicit list of every header in this project's own
# strict-domain-model foundation library. Kept as a `set()` variable
# (rather than inlined directly into add_library() below) so it can also
# be handed to arkham_check_domain_header_inventory() below and to the
# generated tests/DomainHeaderInventory.h.in header used by
# tests/EncoderHygieneTests.cpp -- a single source of truth for "every
# header this build compiles," rather than three independently
# hand-maintained lists that could silently drift apart.
set(ARKHAM_FOUNDATION_HEADERS
    src/AppBootstrap.h
    src/AppSessionComposition.h
    src/AuthModels.h
    src/AuthTransportSecurity.h
    src/CardCatalog.h
    src/CompatibilityEvaluator.h
    src/ContractPin.h
    src/ContractRevision.h
    src/ControllerGlyphs.h
    src/Decks.h
    src/FocusController.h
    src/Games.h
    src/IAuthenticationClient.h
    src/ICapabilityProbe.h
    src/Identifiers.h
    src/IKeychainJobFactory.h
    src/InputMapper.h
    src/InputRouter.h
    src/IProfileStore.h
    src/ITokenStore.h
    src/JsonDecode.h
    src/NetworkAuthenticationClient.h
    src/NetworkCapabilityProbe.h
    src/ProbeResult.h
    src/QSettingsProfileStore.h
    src/QtKeychainJobFactory.h
    src/QtKeychainTokenStore.h
    src/RawJson.h
    src/SemanticCommand.h
    src/ServerCapabilities.h
    src/ServerProfile.h
    src/SessionCoordinator.h
    src/TokenEnvelope.h
    src/TokenValidation.h
    src/UrlValidator.h
    src/ValueOrError.h
)

set(ARKHAM_FOUNDATION_SOURCES
    ${ARKHAM_FOUNDATION_HEADERS}
    src/AppSessionComposition.cpp
    src/AuthModels.cpp
    src/AuthTransportSecurity.cpp
    src/CardCatalog.cpp
    src/CompatibilityEvaluator.cpp
    src/ContractPin.cpp
    src/ContractRevision.cpp
    src/ControllerGlyphs.cpp
    src/Decks.cpp
    src/FocusController.cpp
    src/Games.cpp
    src/Identifiers.cpp
    src/JsonDecode.cpp
    src/InputMapper.cpp
    src/InputRouter.cpp
    src/NetworkAuthenticationClient.cpp
    src/NetworkCapabilityProbe.cpp
    src/QSettingsProfileStore.cpp
    src/QtKeychainJobFactory.cpp
    src/QtKeychainTokenStore.cpp
    src/RawJson.cpp
    src/SemanticCommand.cpp
    src/ServerCapabilities.cpp
    src/ServerProfile.cpp
    src/SessionCoordinator.cpp
    src/TokenEnvelope.cpp
    src/TokenValidation.cpp
    src/UrlValidator.cpp
)

# Structural enforcement (see cmake/DomainHeaderInventory.cmake): fails
# CMake *configuration* -- not merely a test, and not merely a source-text
# scan of already-known files -- the moment ARKHAM_FOUNDATION_HEADERS above
# drifts from what actually exists under src/**/*.h, in either direction
# (a new/nested header added to disk but never declared here, or a
# declared header deleted from disk). This is what makes "add a new nested
# header and never register it" fail the build itself, rather than relying
# on any runtime directory scan (recursive or not) that a test executable
# performs on its own -- see tests/cmake/DomainHeaderInventoryPolicyTest.cmake
# for the mutation tests proving this function itself behaves correctly on
# all three cases (match / undeclared-extra / declared-but-missing).
include(cmake/DomainHeaderInventory.cmake)
arkham_check_domain_header_inventory(
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    HEADER_GLOB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src"
    DECLARED_HEADERS ${ARKHAM_FOUNDATION_HEADERS}
    OUT_ERROR _arkham_header_inventory_error
)
if(_arkham_header_inventory_error)
    message(FATAL_ERROR "${_arkham_header_inventory_error}")
endif()
unset(_arkham_header_inventory_error)

add_library(arkham_foundation STATIC
    ${ARKHAM_FOUNDATION_SOURCES}
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC
    Qt6::Core
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

    # Single source of truth for tests/EncoderHygieneTests.cpp's header
    # inventory: rendered directly from ARKHAM_FOUNDATION_HEADERS (the
    # exact list add_library(arkham_foundation ...) above compiles, whose
    # completeness against the real src/ tree the FATAL_ERROR gate above
    # already verified at configure time -- including any header inside a
    # newly added nested subdirectory) via configure_file(), so the C++
    # scanner test can no longer independently drift from what the build
    # actually compiles, and gets full recursive-subdirectory coverage
    # "for free": there is no runtime directory walk left in the real
    # policy check for a nested/unregistered header to evade.
    set(ARKHAM_DOMAIN_HEADER_INVENTORY_QT_LITERALS "")
    foreach(_arkham_header IN LISTS ARKHAM_FOUNDATION_HEADERS)
        string(APPEND ARKHAM_DOMAIN_HEADER_INVENTORY_QT_LITERALS
            "      QStringLiteral(\"${CMAKE_CURRENT_SOURCE_DIR}/${_arkham_header}\"),\n")
    endforeach()
    configure_file(
        tests/DomainHeaderInventory.h.in
        "${CMAKE_CURRENT_BINARY_DIR}/generated/DomainHeaderInventory.h"
        @ONLY
    )
    unset(ARKHAM_DOMAIN_HEADER_INVENTORY_QT_LITERALS)

    qt_add_executable(arkham-encoder-hygiene-tests
        tests/EncoderHygieneTests.cpp
    )
    target_include_directories(arkham-encoder-hygiene-tests PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_link_libraries(arkham-encoder-hygiene-tests PRIVATE
        Qt6::Test
    )
    add_test(NAME encoder_hygiene COMMAND arkham-encoder-hygiene-tests)

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
