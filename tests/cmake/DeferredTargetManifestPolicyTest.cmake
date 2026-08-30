# Real, fail-before/pass-after CMake mutation test proving why
# arkham_write_target_header_set_manifest() and
# arkham_write_target_source_manifest() (both in cmake/PathManifest.cmake)
# MUST be invoked via cmake_language(DEFER DIRECTORY ... CALL ...), never
# directly/immediately -- see the doc comments above each of those
# functions for the full narrative -- and, alongside that, that both
# functions genuinely union PUBLIC/PRIVATE and INTERFACE-visibility
# FILE_SET/source registrations (reading every named header set's files
# via the single real `HEADER_SET_<name>` property regardless of which
# visibility it came from, since no separate `INTERFACE_HEADER_SET_<name>`
# property actually exists), and that
# arkham_append_target_autogen_manifest() correctly registers owned
# target/policy/AUTOGEN_BUILD_DIR metadata only for AUTOMOC targets.
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
#     review round reported as unobserved;
#   - a THIRD, INTERFACE-visibility FILE_SET (headers3/C.h) and
#     INTERFACE source (dummy3.cpp) -- InterfaceHeaderSetIsIncludedInManifestTest
#     below -- registered alongside the above, plus a locally-defined
#     REGRESSED reproduction of the real functions' pre-fix logic (run
#     side by side, never affecting the real manifests) so this test can
#     prove BOTH that the historical INTERFACE-visibility gaps were real
#     and reproducible (fail-before) and that the real, current functions
#     close them (pass-after), all without needing this file's own git
#     history;
#   - a SEPARATE `probe_target_automoc` OBJECT library with AUTOMOC
#     explicitly enabled (AutomocGeneratedCompilationUnitIsIncludedInManifestTest),
#     proving arkham_write_target_source_manifest() appends that
#     target's own AUTOMOC-generated mocs_compilation.cpp, and that
#     arkham_append_target_autogen_manifest() registers its AUTOGEN_BUILD_DIR
#     into an owned-target manifest -- alongside the same kind of
#     regressed pre-fix reproduction, and a negative control proving
#     arkham_append_target_autogen_manifest() appends NOTHING at all for
#     probe_target (AUTOMOC left OFF, the default).
#
# Expected, asserted outcomes:
#   - The two "_immediate" manifests must contain ONLY the first
#     header/source (headers1/A.h, dummy.cpp) -- proving the early-
#     snapshot bug is real and reproducible, not hypothetical.
#   - The two "_deferred" manifests must contain the first, second, AND
#     third (INTERFACE-visibility) header/source -- proving
#     cmake_language(DEFER) actually closes the "later target_sources()
#     call" gap AND that both PUBLIC/PRIVATE and INTERFACE visibilities
#     are unioned correctly.
#   - Each "_regressed_interface_bug" manifest (using the historical,
#     pre-fix logic) must be MISSING its own INTERFACE-only entry
#     (headers3/C.h / dummy3.cpp respectively) -- proving the gap this
#     test guards against was genuinely reproducible, not merely assumed.
#   - sources_automoc.txt must contain probe_target_automoc's own
#     mocs_compilation.cpp; sources_automoc_regressed.txt (the pre-AUTOMOC-
#     fix reproduction) must be missing it.
#   - external_roots_probe.txt must contain EXACTLY ONE line, matching
#     probe_target_automoc's own AUTOGEN_BUILD_DIR -- proving both that
#     the AUTOMOC-enabled target's root IS registered and that the
#     AUTOMOC-disabled target's call correctly appended nothing.
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
file(MAKE_DIRECTORY "${ARKHAM_SCRATCH_DIR}/headers3")

file(WRITE "${ARKHAM_SCRATCH_DIR}/headers1/A.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/headers2/B.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/headers3/C.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/dummy.cpp" "int arkham_deferred_manifest_probe_dummy() { return 0; }\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/dummy2.cpp" "int arkham_deferred_manifest_probe_dummy2() { return 1; }\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/dummy3.cpp" "int arkham_deferred_manifest_probe_dummy3() { return 2; }\n")

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

# --- INTERFACE-visibility FILE_SET/source: proves both that
# INTERFACE_HEADER_SETS/INTERFACE_SOURCES are unioned in at all, and
# that each named set's files are read back via the one REAL universal
# `HEADER_SET_<name>` property (empirically confirmed via
# `cmake --help-property-list` against this project's own pinned CMake
# 4.4.3 and a real generator-expression probe: there is no separate
# `INTERFACE_HEADER_SET_<name>` property at all) rather than a
# plausible-sounding but nonexistent INTERFACE-prefixed one. ---
target_sources(probe_target INTERFACE
    FILE_SET headers3 TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/headers3/C.h"
)
target_sources(probe_target INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/dummy3.cpp")

# A minimal, deliberately-REGRESSED reproduction of the historical,
# pre-fix arkham_write_target_header_set_manifest() logic -- assumes a
# separate `INTERFACE_HEADER_SET_<name>` property exists and reads THAT
# for INTERFACE-visibility names, which always silently contributes
# nothing (not an error) -- run side by side with the real function
# (never substituting for it) so this test can prove the historical gap
# was genuinely reproducible, not merely asserted.
function(regressed_header_manifest_pre_interface_fix)
    set(oneValueArgs TARGET OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})
    get_target_property(_names ${ARG_TARGET} HEADER_SETS)
    get_target_property(_iface_names ${ARG_TARGET} INTERFACE_HEADER_SETS)
    if(NOT _names)
        set(_names "")
    endif()
    if(NOT _iface_names)
        set(_iface_names "")
    endif()
    set(_pieces "")
    foreach(_n IN LISTS _names)
        list(APPEND _pieces "$<TARGET_PROPERTY:${ARG_TARGET},HEADER_SET_${_n}>")
    endforeach()
    foreach(_n IN LISTS _iface_names)
        list(APPEND _pieces "$<TARGET_PROPERTY:${ARG_TARGET},INTERFACE_HEADER_SET_${_n}>")
    endforeach()
    file(GENERATE OUTPUT "${ARG_OUTPUT_FILE}" CONTENT "$<JOIN:${_pieces},\n>\n")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    regressed_header_manifest_pre_interface_fix
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/manifest_regressed_interface_bug.txt"
)

# A minimal, deliberately-REGRESSED reproduction of the historical,
# pre-fix arkham_write_target_source_manifest() logic (plain SOURCES
# only -- no INTERFACE_SOURCES union, no AUTOMOC mocs_compilation.cpp
# append at all). Reused below for BOTH probe_target (proving the
# INTERFACE_SOURCES gap) and probe_target_automoc (proving the AUTOMOC
# gap), since both historical bugs share the exact same "just read
# plain SOURCES" shape.
function(regressed_source_manifest_sources_property_only)
    set(oneValueArgs TARGET OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})
    get_target_property(_srcdir ${ARG_TARGET} SOURCE_DIR)
    get_target_property(_srcs ${ARG_TARGET} SOURCES)
    if(NOT _srcs)
        set(_srcs "")
    endif()
    set(_lines "")
    foreach(_s IN LISTS _srcs)
        cmake_path(IS_RELATIVE _s _is_rel)
        if(_is_rel)
            string(APPEND _lines "${_srcdir}/${_s}\n")
        else()
            string(APPEND _lines "${_s}\n")
        endif()
    endforeach()
    file(WRITE "${ARG_OUTPUT_FILE}" "${_lines}")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    regressed_source_manifest_sources_property_only
    TARGET probe_target
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/sources_regressed_interface_bug.txt"
)

# --- AUTOMOC-generated compilation-unit registration: a separate probe
# target, so its Qt-less "AUTOMOC disabled" configure-time author
# warning (expected and harmless in this Qt-free scratch project --
# confirmed empirically that CMake still leaves the AUTOMOC target
# property itself reporting ON, and AUTOGEN_BUILD_DIR still resolves to
# its normal default, even though no Qt was found to actually run moc)
# cannot be confused with the FILE_SET/source scenarios above. ---
add_library(probe_target_automoc OBJECT dummy.cpp)
set_target_properties(probe_target_automoc PROPERTIES AUTOMOC ON)
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    arkham_write_target_source_manifest
    TARGET probe_target_automoc
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/sources_automoc.txt"
)
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    regressed_source_manifest_sources_property_only
    TARGET probe_target_automoc
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/sources_automoc_regressed.txt"
)

# Owned AUTOGEN metadata starts empty; the AUTOMOC target contributes
# exactly one policy/target/root record and the non-AUTOMOC target none.
file(WRITE "${CMAKE_BINARY_DIR}/autogen_targets_probe.txt" "")
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    arkham_append_target_autogen_manifest
    TARGET probe_target_automoc
    POLICY domain
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/autogen_targets_probe.txt"
)
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
    arkham_append_target_autogen_manifest
    TARGET probe_target
    POLICY domain
    OUTPUT_FILE "${CMAKE_BINARY_DIR}/autogen_targets_probe.txt"
)
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
# contain headers1/A.h, headers2/B.h (PUBLIC/PRIVATE visibility), AND
# headers3/C.h (INTERFACE visibility). ---
list(FILTER _headers_deferred INCLUDE REGEX "headers1/A\\.h$|headers2/B\\.h$|headers3/C\\.h$")
list(LENGTH _headers_deferred _headers_deferred_count)
if(NOT _headers_deferred_count EQUAL 3)
    message(FATAL_ERROR
        "Case 2 (deferred header-set manifest) expected exactly headers1/A.h, headers2/B.h, and headers3/C.h (3 entries) but found ${_headers_deferred_count}: ${_headers_deferred}")
endif()
message(STATUS "Case 2 (deferred header-set manifest correctly contains headers1/A.h, headers2/B.h, and headers3/C.h) passed.")

# --- Case 3 (fail-before, sources): the immediate source manifest must
# be missing the later-registered dummy2.cpp. ---
list(FILTER _sources_immediate INCLUDE REGEX "dummy2\\.cpp$")
if(NOT "${_sources_immediate}" STREQUAL "")
    message(FATAL_ERROR
        "Case 3 (immediate source manifest) expected dummy2.cpp to be MISSING (proving the early-SOURCES-snapshot bug), but it was present.")
endif()
message(STATUS "Case 3 (immediate source manifest correctly omits the later-registered dummy2.cpp) passed.")

# --- Case 4 (pass-after, sources): the deferred source manifest must
# contain dummy.cpp, dummy2.cpp (plain SOURCES), AND dummy3.cpp
# (INTERFACE_SOURCES). ---
list(FILTER _sources_deferred INCLUDE REGEX "/dummy\\.cpp$|dummy2\\.cpp$|dummy3\\.cpp$")
list(LENGTH _sources_deferred _sources_deferred_count)
if(NOT _sources_deferred_count EQUAL 3)
    message(FATAL_ERROR
        "Case 4 (deferred source manifest) expected exactly dummy.cpp, dummy2.cpp, and dummy3.cpp (3 entries) but found ${_sources_deferred_count}: ${_sources_deferred}")
endif()
message(STATUS "Case 4 (deferred source manifest correctly contains dummy.cpp, dummy2.cpp, and dummy3.cpp) passed.")

# --- InterfaceHeaderSetIsIncludedInManifestTest: Case 5 (fail-before)
# proves the historical "assumed INTERFACE_HEADER_SET_<name> property"
# bug was real -- the regressed reproduction's manifest must be MISSING
# the INTERFACE-only headers3/C.h entirely (it may still legitimately
# contain headers1/A.h and headers2/B.h, since those go through the
# correctly-named HEADER_SET_<name> property in both the real and
# regressed functions alike; only the INTERFACE-visibility entry is
# expected to differ). ---
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/manifest_regressed_interface_bug.txt" _headers_regressed)
list(FILTER _headers_regressed INCLUDE REGEX "headers3/C\\.h$")
if(NOT "${_headers_regressed}" STREQUAL "")
    message(FATAL_ERROR
        "Case 5 (InterfaceHeaderSetIsIncludedInManifestTest, fail-before) expected the regressed pre-fix reproduction (which reads a nonexistent INTERFACE_HEADER_SET_<name> property) to be MISSING headers3/C.h, but it was present. Either CMake started supporting that property, or the regressed reproduction itself no longer matches the historical bug -- re-examine the scenario.")
endif()
message(STATUS "Case 5 (InterfaceHeaderSetIsIncludedInManifestTest, fail-before: regressed pre-fix header manifest correctly omits the INTERFACE-only headers3/C.h) passed.")
# Case 5's pass-after half is Case 2 above: the REAL, current
# arkham_write_target_header_set_manifest() (manifest_deferred.txt)
# already asserted to contain headers3/C.h alongside headers1/A.h and
# headers2/B.h.
message(STATUS "Case 5 (InterfaceHeaderSetIsIncludedInManifestTest, pass-after: see Case 2) passed.")

# --- Case 6 (fail-before): the regressed pre-fix source-manifest
# reproduction (plain SOURCES only) must be MISSING the INTERFACE-only
# dummy3.cpp. ---
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/sources_regressed_interface_bug.txt" _sources_regressed)
list(FILTER _sources_regressed INCLUDE REGEX "dummy3\\.cpp$")
if(NOT "${_sources_regressed}" STREQUAL "")
    message(FATAL_ERROR
        "Case 6 (fail-before) expected the regressed pre-fix source-manifest reproduction (plain SOURCES only) to be MISSING dummy3.cpp, but it was present.")
endif()
message(STATUS "Case 6 (fail-before: regressed pre-fix source manifest correctly omits the INTERFACE-only dummy3.cpp) passed.")
# Case 6's pass-after half is Case 4 above.
message(STATUS "Case 6 (pass-after: see Case 4) passed.")

# --- AutomocGeneratedCompilationUnitIsIncludedInManifestTest: Case 7
# (pass-after) proves the REAL arkham_write_target_source_manifest()
# appends probe_target_automoc's own AUTOMOC-generated
# mocs_compilation.cpp. ---
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/sources_automoc.txt" _sources_automoc)
list(FILTER _sources_automoc INCLUDE REGEX "probe_target_automoc_autogen/mocs_compilation\\.cpp$")
list(LENGTH _sources_automoc _sources_automoc_count)
if(NOT _sources_automoc_count EQUAL 1)
    message(FATAL_ERROR
        "Case 7 (AutomocGeneratedCompilationUnitIsIncludedInManifestTest, pass-after) expected exactly one probe_target_automoc_autogen/mocs_compilation.cpp entry in sources_automoc.txt but found ${_sources_automoc_count}: ${_sources_automoc}")
endif()
message(STATUS "Case 7 (AutomocGeneratedCompilationUnitIsIncludedInManifestTest, pass-after: real source manifest contains probe_target_automoc's own mocs_compilation.cpp) passed.")

# --- Case 8 (fail-before): the regressed pre-AUTOMOC-fix reproduction
# (plain SOURCES only, same function reused from Case 6) must be
# MISSING mocs_compilation.cpp entirely, proving the historical
# "AUTOMOC's own generated TU has no SOURCES/INTERFACE_SOURCES entry at
# all" gap was real. ---
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/sources_automoc_regressed.txt" _sources_automoc_regressed)
list(FILTER _sources_automoc_regressed INCLUDE REGEX "mocs_compilation\\.cpp$")
if(NOT "${_sources_automoc_regressed}" STREQUAL "")
    message(FATAL_ERROR
        "Case 8 (AutomocGeneratedCompilationUnitIsIncludedInManifestTest, fail-before) expected the regressed pre-fix reproduction to be MISSING any mocs_compilation.cpp entry, but found: ${_sources_automoc_regressed}")
endif()
message(STATUS "Case 8 (AutomocGeneratedCompilationUnitIsIncludedInManifestTest, fail-before: regressed pre-fix source manifest correctly omits mocs_compilation.cpp) passed.")

# --- Case 9: arkham_append_target_autogen_manifest() must append EXACTLY
# ONE line overall to the owned AUTOGEN manifest -- proving
# both that probe_target_automoc's (AUTOMOC ON) own AUTOGEN_BUILD_DIR IS
# registered, and that probe_target's (AUTOMOC left OFF, the default)
# call correctly appended NOTHING at all, i.e. the function's own
# AUTOMOC guard is real and not a no-op that always appends regardless
# of whether AUTOMOC is actually enabled. ---
_arkham_dtm_read_manifest_lines("${ARKHAM_SCRATCH_DIR}/build/autogen_targets_probe.txt" _autogen_targets_probe)
list(LENGTH _autogen_targets_probe _autogen_targets_probe_count)
if(NOT _autogen_targets_probe_count EQUAL 1)
    message(FATAL_ERROR
        "Case 9 expected exactly one owned AUTOGEN record but found ${_autogen_targets_probe_count}: ${_autogen_targets_probe}")
endif()
list(GET _autogen_targets_probe 0 _autogen_targets_probe_only_line)
if(NOT _autogen_targets_probe_only_line MATCHES "^domain[\t]probe_target_automoc[\t].*probe_target_automoc_autogen$")
    message(FATAL_ERROR
        "Case 9 expected domain/target/root AUTOGEN metadata, found: ${_autogen_targets_probe_only_line}")
endif()
message(STATUS "Case 9 (owned AUTOGEN metadata registers only the AUTOMOC-enabled target) passed.")

message(STATUS "DeferredTargetManifestPolicyTest: all 9 cases passed.")
