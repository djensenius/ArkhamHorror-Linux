# Real, fail-before/pass-after CMake mutation test proving why
# arkham_write_target_header_set_manifest() and
# arkham_write_target_source_manifest() (both in cmake/PathManifest.cmake)
# MUST be invoked via cmake_language(DEFER DIRECTORY ... CALL ...), never
# directly/immediately -- see the doc comments above each of those
# functions for the full narrative.
#
# cmake_language(DEFER DIRECTORY <dir> CALL ...) requires <dir> to be a
# directory CMake is actually (or has already finished) processing as
# part of a real project() configure -- it errors with "is not known. It
# may not have been processed yet." when attempted from plain `cmake -P`
# script mode, which has no directory-processing/generate phase at all.
# This test therefore cannot use the same in-process `include()` +
# direct-call style as tests/cmake/DomainHeaderInventoryPolicyTest.cmake;
# instead, this driver script (itself run via `cmake -P`, see the
# cmake_configuration_policy ctest case in CMakeLists.txt) writes a tiny,
# disposable SCRATCH CMake project to its own separate scratch directory
# and configures it with a real, separate `cmake -S/-B` subprocess, then
# inspects the manifest files that scratch project's own CMakeLists.txt
# wrote.
#
# The scratch project's CMakeLists.txt (written out below, not committed
# separately) declares one OBJECT library target with:
#   - one FILE_SET (headers1/A.h) and one ordinary source (dummy.cpp)
#     registered BEFORE either manifest-writing call site;
#   - both arkham_write_target_header_set_manifest() and
#     arkham_write_target_source_manifest() invoked TWICE each: once
#     directly/immediately (the bug this test exists to prove), and once
#     wrapped in cmake_language(DEFER DIRECTORY ... CALL ...) (the fix);
#   - a SECOND FILE_SET (headers2/B.h) and a second ordinary source
#     (dummy2.cpp) registered AFTER both pairs of manifest-writing calls,
#     mirroring exactly the "a later target_sources() call" scenario a
#     review round reported as unobserved.
#
# Expected, asserted outcomes:
#   - The two "_immediate" manifests must contain ONLY the first
#     header/source (headers1/A.h, dummy.cpp) -- proving the early-
#     snapshot bug is real and reproducible, not hypothetical.
#   - The two "_deferred" manifests must contain BOTH the first and
#     second header/source -- proving cmake_language(DEFER) actually
#     closes the gap.
# A regression that made either function's real implementation stop
# using/needing DEFER (or a future edit that accidentally called it
# directly again in the real CMakeLists.txt) would not, by itself, be
# caught by this synthetic scratch project -- this test proves the
# *mechanism* cmake/PathManifest.cmake and CMakeLists.txt rely on
# actually behaves as documented, independent of this project's real
# src/ layout; the real CMakeLists.txt's own use of
# cmake_language(DEFER ...) around every one of these calls (see its
# comments) is what applies this guarantee to arkham_domain_models/
# arkham_foundation themselves.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ARKHAM_MODULE_DIR)
    message(FATAL_ERROR "ARKHAM_MODULE_DIR must be passed via -D (see CMakeLists.txt's add_test(NAME cmake_deferred_target_manifest_policy ...))")
endif()
if(NOT DEFINED ARKHAM_SCRATCH_DIR)
    message(FATAL_ERROR "ARKHAM_SCRATCH_DIR must be passed via -D (see CMakeLists.txt's add_test(NAME cmake_deferred_target_manifest_policy ...))")
endif()
if(NOT DEFINED CMAKE_COMMAND)
    message(FATAL_ERROR "CMAKE_COMMAND must be passed via -D so this driver script can spawn a real, separate `cmake -S/-B` configure of its scratch project")
endif()

# Start from a clean scratch directory every run so a previous run's
# leftovers (or a mid-run failure) can never mask a real regression.
if(EXISTS "${ARKHAM_SCRATCH_DIR}")
    file(REMOVE_RECURSE "${ARKHAM_SCRATCH_DIR}")
endif()
file(MAKE_DIRECTORY "${ARKHAM_SCRATCH_DIR}/headers1")
file(MAKE_DIRECTORY "${ARKHAM_SCRATCH_DIR}/headers2")

file(WRITE "${ARKHAM_SCRATCH_DIR}/headers1/A.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/headers2/B.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/dummy.cpp" "int arkham_deferred_manifest_probe_dummy() { return 0; }\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/dummy2.cpp" "int arkham_deferred_manifest_probe_dummy2() { return 1; }\n")

file(WRITE "${ARKHAM_SCRATCH_DIR}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.25)
project(ArkhamDeferredManifestProbe CXX)

include("${ARKHAM_MODULE_DIR}/PathManifest.cmake")

add_library(probe_target OBJECT dummy.cpp)
target_sources(probe_target PUBLIC
    FILE_SET headers1 TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/headers1/A.h"
)

# --- The bug: called directly/immediately, both functions only ever see
# whatever was registered on probe_target *before* this textual point. --
arkham_write_target_header_set_manifest(
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/manifest_immediate.txt"
)
arkham_write_target_source_manifest(
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/sources_immediate.txt"
)

# --- The fix: deferred to end-of-directory-processing, both functions
# see every FILE_SET/source registered anywhere in this file, regardless
# of call-site order. ---
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    arkham_write_target_header_set_manifest
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/manifest_deferred.txt"
)
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    arkham_write_target_source_manifest
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/sources_deferred.txt"
)

# --- Registered AFTER every manifest-writing call site above, exactly
# mirroring the reviewer-reported "a later target_sources() call" gap. --
target_sources(probe_target PUBLIC
    FILE_SET headers2 TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/headers2/B.h"
)
target_sources(probe_target PRIVATE dummy2.cpp)
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${ARKHAM_SCRATCH_DIR}"
        -B "${ARKHAM_SCRATCH_DIR}/build"
        "-DARKHAM_MODULE_DIR=${ARKHAM_MODULE_DIR}"
    RESULT_VARIABLE _arkham_dtm_configure_result
    OUTPUT_VARIABLE _arkham_dtm_configure_output
    ERROR_VARIABLE _arkham_dtm_configure_error
)
if(NOT _arkham_dtm_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Scratch project configure failed (exit ${_arkham_dtm_configure_result}):\n"
        "--- stdout ---\n${_arkham_dtm_configure_output}\n"
        "--- stderr ---\n${_arkham_dtm_configure_error}")
endif()

function(_arkham_dtm_read_manifest_lines path out_var)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Expected manifest file was never written: ${path}")
    endif()
    file(STRINGS "${path}" _lines)
    set(${out_var} "${_lines}" PARENT_SCOPE)
endfunction()

_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/manifest_immediate.txt" _headers_immediate)
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/manifest_deferred.txt" _headers_deferred)
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/sources_immediate.txt" _sources_immediate)
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/sources_deferred.txt" _sources_deferred)

# --- Case 1 (fail-before, headers): the immediate header-set manifest
# must be missing the later-registered headers2/B.h -- proving the bug
# is real, not merely theoretical. ---
list(FILTER _headers_immediate INCLUDE REGEX "headers2/B\\.h$")
if(NOT "${_headers_immediate}" STREQUAL "")
    message(FATAL_ERROR
        "Case 1 (immediate header-set manifest) expected headers2/B.h to be MISSING (proving the early-HEADER_SETS-snapshot bug), but it was present. Either the bug no longer reproduces this way, or arkham_write_target_header_set_manifest() itself changed -- re-examine the scenario.")
endif()
message(STATUS "Case 1 (immediate header-set manifest correctly omits the later-registered headers2/B.h) passed.")

# --- Case 2 (pass-after, headers): the deferred header-set manifest must
# contain BOTH headers1/A.h and headers2/B.h. ---
list(FILTER _headers_deferred INCLUDE REGEX "headers1/A\\.h$|headers2/B\\.h$")
list(LENGTH _headers_deferred _headers_deferred_count)
if(NOT _headers_deferred_count EQUAL 2)
    message(FATAL_ERROR
        "Case 2 (deferred header-set manifest) expected exactly headers1/A.h and headers2/B.h (2 entries) but found ${_headers_deferred_count}: ${_headers_deferred}")
endif()
message(STATUS "Case 2 (deferred header-set manifest correctly contains both headers1/A.h and headers2/B.h) passed.")

# --- Case 3 (fail-before, sources): the immediate source manifest must
# be missing the later-registered dummy2.cpp. ---
list(FILTER _sources_immediate INCLUDE REGEX "dummy2\\.cpp$")
if(NOT "${_sources_immediate}" STREQUAL "")
    message(FATAL_ERROR
        "Case 3 (immediate source manifest) expected dummy2.cpp to be MISSING (proving the early-SOURCES-snapshot bug), but it was present.")
endif()
message(STATUS "Case 3 (immediate source manifest correctly omits the later-registered dummy2.cpp) passed.")

# --- Case 4 (pass-after, sources): the deferred source manifest must
# contain BOTH dummy.cpp and dummy2.cpp. ---
list(FILTER _sources_deferred INCLUDE REGEX "dummy\\.cpp$|dummy2\\.cpp$")
list(LENGTH _sources_deferred _sources_deferred_count)
if(NOT _sources_deferred_count EQUAL 2)
    message(FATAL_ERROR
        "Case 4 (deferred source manifest) expected exactly dummy.cpp and dummy2.cpp (2 entries) but found ${_sources_deferred_count}: ${_sources_deferred}")
endif()
message(STATUS "Case 4 (deferred source manifest correctly contains both dummy.cpp and dummy2.cpp) passed.")

message(STATUS "DeferredTargetManifestPolicyTest: all 4 cases passed.")
