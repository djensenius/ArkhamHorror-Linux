cmake_minimum_required(VERSION 3.25)

project(ArkhamHorrorLinux VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(QT_QML_GENERATE_QMLLS_INI ON)

# Cumulative-review finding (PR #18, exact head 4a47ea34): the previous
# AssetJpegDecoder.cpp kept its libjpeg cinfo/error-manager state as plain
# automatic (stack) locals mutated after the setjmp() checkpoint --
# formally undefined behavior per [csetjmp.syn], even though every
# compiler actually observed happened to preserve their values across
# longjmp() regardless (see AssetJpegDecoder.cpp's own restructuring to
# heap-allocated, unchanged-pointer-reached state, which is what actually
# fixes this at the root). This option lets CI additionally PROVE the fix
# under Clang/GCC's UndefinedBehaviorSanitizer in an optimized
# (RelWithDebInfo/Release) build -- exactly the configuration most likely
# to have actually miscompiled the previous code, since only an optimizer
# aggressive enough to register-allocate/reorder around setjmp()'s own
# (deliberately limited) guarantees could turn this from "happens to
# work" into an observable failure. Default OFF: this is an opt-in
# diagnostic configuration, never the default developer/CI build (which
# would be slower and would reject some otherwise-fine third-party code
# this project does not control, e.g. inside Qt itself, if ever linked
# statically).
option(ARKHAM_ENABLE_UBSAN
      "Build with -fsanitize=undefined (Clang/GCC only; opt-in diagnostic build, never the default)"
      OFF)
if(ARKHAM_ENABLE_UBSAN)
  if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    message(FATAL_ERROR "ARKHAM_ENABLE_UBSAN requires Clang or GCC")
  endif()
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Quick QuickControls2 WebSockets)

qt_standard_project_setup(REQUIRES 6.8)

# --- libavif (BSD-2-Clause; https://github.com/AOMediaCodec/libavif) -------
# Review item 4 (PR #18 cumulative review): Qt has no official AVIF plugin
# (upstream qtimageformats does not ship one), and card art defaults to
# AVIF -- a permanent AssetErrorCode::UnsupportedCodec for every card asset
# is not acceptable. AssetAvifDecoder.cpp decodes AVIF directly against
# libavif's C API instead, so this is a REQUIRED build dependency (not an
# optional, best-effort one like WrapVulkanHeaders below) -- a configure
# that cannot find libavif fails loudly rather than silently shipping a
# build with no AVIF support at all. See mise.toml's setup:macos task and
# .github/workflows/ci.yml for how each environment installs it, and
# packaging/build-appimage.sh for how its own shared-library closure is
# bundled into the AppImage.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAVIF REQUIRED IMPORTED_TARGET libavif)

# --- libjpeg / libjpeg-turbo (IJG/BSD-style; https://libjpeg-turbo.org/) ---
# Review item 2 (PR #18 cumulative review at 14cf8de6): the previous JPEG
# decode path went through QImageReader (Qt's bundled qjpeg plugin), with
# a separate ScopedJpegDecodeWarningDetector installing a PROCESS-GLOBAL
# Qt message handler to observe whether libjpeg had to recover from
# corrupt/incomplete entropy-coded scan data -- unsafe under any
# concurrent/reentrant decode (two overlapping instances corrupt each
# other's shared static state and can self-recursively reinstall their own
# handler). AssetJpegDecoder.cpp fixes this at the root by decoding JPEG
# directly against libjpeg's own C API instead, with a per-call (never
# static/shared) custom jpeg_error_mgr, exactly mirroring how
# AssetAvifDecoder.cpp already decodes AVIF directly against libavif. This
# is therefore a REQUIRED build dependency (not optional), like libavif
# above. See mise.toml's setup:macos task and .github/workflows/ci.yml for
# how each environment installs it.
pkg_check_modules(LIBJPEG REQUIRED IMPORTED_TARGET libjpeg)

# --- zlib (zlib License; https://zlib.net/) --------------------------------
# Cumulative-review finding (PR #18, exact head 4a47ea34): a CRC-valid PNG
# IDAT run can contain a complete, valid image zlib stream followed by
# extra bytes or an entirely second zlib stream -- libpng only warns about
# this (and Qt's qpng plugin surfaces no way to observe that warning), so
# QImageReader/QImage happily decodes and this project would happily cache
# the result. AssetPngValidator.cpp closes this by concatenating every
# IDAT chunk's payload (already gathered while walking the chunk sequence
# for pngChunksAreStrictlyValid()) and inflating it directly against
# zlib's own C API, requiring the decompressed byte count to exactly equal
# the expected (width/height/bit-depth/colour-type-derived) scanline byte
# count, Z_STREAM_END to be reached, and every input byte to be consumed --
# rejecting both truncated streams and any trailing/second-stream bytes.
# Qt's own qUncompress()/qCompress() are NOT usable for this: they require
# Qt's own wire format (a 4-byte big-endian uncompressed-length prefix
# ahead of the zlib stream), not the bare zlib stream PNG's IDAT chunks
# actually contain, so the real zlib C API is called directly here, exactly
# mirroring how AssetAvifDecoder.cpp/AssetJpegDecoder.cpp call libavif/
# libjpeg directly rather than going through Qt's own image plugins.
find_package(ZLIB REQUIRED)


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
    src/AppBootstrap.h
    src/AppSessionComposition.cpp
    src/AppSessionComposition.h
    src/AssetAvifDecoder.cpp
    src/AssetAvifDecoder.h
    src/AssetCache.cpp
    src/AssetCache.h
    src/AssetImageRequest.cpp
    src/AssetImageRequest.h
    src/AssetJpegDecoder.cpp
    src/AssetJpegDecoder.h
    src/AssetLocaleDigest.cpp
    src/AssetLocaleDigest.h
    src/AssetLocator.cpp
    src/AssetLocator.h
    src/AssetNetworkFetcher.cpp
    src/AssetNetworkFetcher.h
    src/AssetPngValidator.cpp
    src/AssetPngValidator.h
    src/AssetRequestCoordinator.cpp
    src/AssetRequestCoordinator.h
    src/AssetTypes.cpp
    src/AssetTypes.h
    src/AuthModels.cpp
    src/AuthModels.h
    src/AuthTransportSecurity.cpp
    src/AuthTransportSecurity.h
    src/CompatibilityEvaluator.cpp
    src/CompatibilityEvaluator.h
    src/ContractPin.cpp
    src/ContractPin.h
    src/ContractRevision.cpp
    src/ContractRevision.h
    src/ControllerGlyphs.cpp
    src/ControllerGlyphs.h
    src/FocusController.cpp
    src/FocusController.h
    src/IAuthenticationClient.h
    src/ICapabilityProbe.h
    src/IKeychainJobFactory.h
    src/InputMapper.cpp
    src/InputMapper.h
    src/InputRouter.cpp
    src/InputRouter.h
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
    src/SemanticCommand.cpp
    src/SemanticCommand.h
    src/ServerCapabilities.cpp
    src/ServerCapabilities.h
    src/ServerProfile.cpp
    src/ServerProfile.h
    src/SessionCoordinator.cpp
    src/SessionCoordinator.h
    src/TokenEnvelope.cpp
    src/TokenEnvelope.h
    src/TokenValidation.cpp
    src/TokenValidation.h
    src/UrlValidator.cpp
    src/UrlValidator.h
    src/ValueOrError.h
)

target_include_directories(arkham_foundation PUBLIC src)
target_link_libraries(arkham_foundation PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6Keychain::Qt6Keychain
    PkgConfig::LIBAVIF
    PkgConfig::LIBJPEG
    ZLIB::ZLIB
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

    qt_add_executable(arkham-asset-tests
        tests/AssetCacheTests.cpp
        tests/AssetCacheTests.h
        tests/AssetFetchUrlTestSupport.h
        tests/AssetImageRequestTests.cpp
        tests/AssetImageRequestTests.h
        tests/AssetLocaleDigestTests.cpp
        tests/AssetLocaleDigestTests.h
        tests/AssetLocatorTests.cpp
        tests/AssetLocatorTests.h
        tests/AssetNetworkFetcherTests.cpp
        tests/AssetNetworkFetcherTests.h
        tests/AssetRequestCoordinatorTests.cpp
        tests/AssetRequestCoordinatorTests.h
        tests/AssetTestsMain.cpp
        tests/MockHttpServer.cpp
        tests/MockHttpServer.h
    )
    target_compile_definitions(arkham-asset-tests PRIVATE
        ARKHAM_TEST_CONTRACTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/contracts"
    )
    target_link_libraries(arkham-asset-tests PRIVATE
        arkham_foundation
        Qt6::Network
        Qt6::Test
    )
    add_test(NAME asset COMMAND arkham-asset-tests)

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
