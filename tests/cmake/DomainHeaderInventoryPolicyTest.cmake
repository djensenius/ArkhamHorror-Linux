# Exercises arkham_check_domain_header_inventory() (cmake/DomainHeaderInventory.cmake)
# against a synthetic, disposable scratch directory tree -- entirely
# separate from this project's real src/ tree -- proving all three of its
# possible outcomes:
#   1. An exact match between DECLARED_HEADERS and what is actually on
#      disk (including a header inside a nested subdirectory) produces an
#      empty OUT_ERROR.
#   2. A header added to disk (inside that same nested subdirectory) but
#      never added to DECLARED_HEADERS produces a non-empty OUT_ERROR
#      naming that exact file.
#   3. A header present in DECLARED_HEADERS but deleted from disk produces
#      a non-empty OUT_ERROR naming that exact file.
#
# Run via `cmake -P` as its own ctest case (see CMakeLists.txt's
# add_test(NAME cmake_header_inventory_policy ...)) rather than compiled
# into any C++ test binary, since the behavior under test -- recursive
# glob-vs-declared-list drift detection -- is itself a piece of CMake
# configuration-time logic, not C++ code; this is the direct proof that a
# brand-new nested/unregistered header fails *before* add_library() is
# ever reached, independent of the real src/ layout.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ARKHAM_MODULE_DIR)
    message(FATAL_ERROR "ARKHAM_MODULE_DIR must be passed via -D (see CMakeLists.txt's add_test(NAME cmake_header_inventory_policy ...))")
endif()
if(NOT DEFINED ARKHAM_SCRATCH_DIR)
    message(FATAL_ERROR "ARKHAM_SCRATCH_DIR must be passed via -D (see CMakeLists.txt's add_test(NAME cmake_header_inventory_policy ...))")
endif()

include("${ARKHAM_MODULE_DIR}/DomainHeaderInventory.cmake")

# Start from a clean scratch directory every run so a previous run's
# leftovers (or a mid-run failure) can never mask a real regression.
if(EXISTS "${ARKHAM_SCRATCH_DIR}")
    file(REMOVE_RECURSE "${ARKHAM_SCRATCH_DIR}")
endif()
file(MAKE_DIRECTORY "${ARKHAM_SCRATCH_DIR}/src/nested")

file(WRITE "${ARKHAM_SCRATCH_DIR}/src/Top.h" "// scratch header, not part of the real project\n")
file(WRITE "${ARKHAM_SCRATCH_DIR}/src/nested/Nested.h" "// scratch nested header, not part of the real project\n")

set(_declared_headers
    "src/Top.h"
    "src/nested/Nested.h"
)

# --- Case 1: exact match (including the nested header) -> empty error -----
arkham_check_domain_header_inventory(
    BASE_DIR "${ARKHAM_SCRATCH_DIR}"
    HEADER_GLOB_DIR "${ARKHAM_SCRATCH_DIR}/src"
    DECLARED_HEADERS ${_declared_headers}
    OUT_ERROR _error_case1
)
if(NOT "${_error_case1}" STREQUAL "")
    message(FATAL_ERROR
        "Case 1 (exact match) expected an empty OUT_ERROR but got:\n${_error_case1}")
endif()
message(STATUS "Case 1 (exact match, including a nested header) passed: no error reported, as expected.")

# --- Case 2: undeclared new nested header on disk -> non-empty error ------
file(WRITE "${ARKHAM_SCRATCH_DIR}/src/nested/Undeclared.h" "// undeclared nested header\n")
arkham_check_domain_header_inventory(
    BASE_DIR "${ARKHAM_SCRATCH_DIR}"
    HEADER_GLOB_DIR "${ARKHAM_SCRATCH_DIR}/src"
    DECLARED_HEADERS ${_declared_headers}
    OUT_ERROR _error_case2
)
if("${_error_case2}" STREQUAL "")
    message(FATAL_ERROR
        "Case 2 (undeclared nested header) expected a non-empty OUT_ERROR but got none -- this is exactly the 'new nested/unregistered header must fail configuration' requirement, so an empty result here is itself the regression.")
endif()
if(NOT _error_case2 MATCHES "src/nested/Undeclared\\.h")
    message(FATAL_ERROR
        "Case 2 (undeclared nested header) error did not name the offending file src/nested/Undeclared.h:\n${_error_case2}")
endif()
message(STATUS "Case 2 (undeclared nested header) passed: error named src/nested/Undeclared.h, as expected.")
file(REMOVE "${ARKHAM_SCRATCH_DIR}/src/nested/Undeclared.h")

# --- Case 3: declared header missing from disk -> non-empty error ---------
file(REMOVE "${ARKHAM_SCRATCH_DIR}/src/nested/Nested.h")
arkham_check_domain_header_inventory(
    BASE_DIR "${ARKHAM_SCRATCH_DIR}"
    HEADER_GLOB_DIR "${ARKHAM_SCRATCH_DIR}/src"
    DECLARED_HEADERS ${_declared_headers}
    OUT_ERROR _error_case3
)
if("${_error_case3}" STREQUAL "")
    message(FATAL_ERROR
        "Case 3 (declared-but-missing header) expected a non-empty OUT_ERROR but got none.")
endif()
if(NOT _error_case3 MATCHES "src/nested/Nested\\.h")
    message(FATAL_ERROR
        "Case 3 (declared-but-missing header) error did not name the offending file src/nested/Nested.h:\n${_error_case3}")
endif()
message(STATUS "Case 3 (declared-but-missing header) passed: error named src/nested/Nested.h, as expected.")

# Clean up after ourselves so repeated local `ctest` runs (not just CI's
# single fresh checkout) never accumulate stale scratch state.
file(REMOVE_RECURSE "${ARKHAM_SCRATCH_DIR}")

message(STATUS "DomainHeaderInventoryPolicyTest: all cases passed.")
