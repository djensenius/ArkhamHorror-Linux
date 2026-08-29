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

add_library(arkham_foundation STATIC
    src/AppCommand.cpp
    src/AppCommand.h
    src/AuthModels.cpp
    src/AuthModels.h
    src/AuthTransportSecurity.cpp
    src/AuthTransportSecurity.h
    src/CardCatalog.cpp
    src/CardCatalog.h
    src/CompatibilityEvaluator.cpp
    src/CompatibilityEvaluator.h
    src/ContractPin.cpp
    src/ContractPin.h
    src/ContractRevision.cpp
    src/ContractRevision.h
    src/Decks.cpp
    src/Decks.h
    src/Games.cpp
    src/Games.h
    src/IAuthenticationClient.h
    src/Identifiers.cpp
    src/Identifiers.h
    src/JsonDecode.cpp
    src/JsonDecode.h
    src/ICapabilityProbe.h
    src/IKeychainJobFactory.h
    src/IProfileStore.h
    src/ITokenStore.h
    src/NetworkAuthenticationClient.cpp
    src/NetworkAuthenticationClient.h
    src/NetworkCapabilityProbe.cpp
    src/NetworkCapabilityProbe.h
    src/ProbeResult.h
    src/QSettingsProfileStore.cpp
    src/QSettingsProfileStore.h
    src/QtKeychainJobFactory.cpp
    src/QtKeychainJobFactory.h
    src/QtKeychainTokenStore.cpp
    src/QtKeychainTokenStore.h
    src/ServerCapabilities.cpp
    src/ServerCapabilities.h
    src/ServerProfile.cpp
    src/ServerProfile.h
    src/UrlValidator.cpp
    src/UrlValidator.h
    src/ValueOrError.h
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC
    Qt6::Core
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
