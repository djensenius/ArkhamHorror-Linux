# Provides arkham_check_domain_header_inventory(), a small reusable CMake
# function that recursively globs every "*.h" file under a directory and
# reports (via an out-parameter, see below -- never by calling
# message(FATAL_ERROR ...) itself) whether that recursive glob disagrees,
# in either direction, with an explicitly declared list of headers.
#
# This is what makes a newly added header -- including one placed in a
# brand-new nested subdirectory that no source-text scanner ever visits --
# fail CMake *configuration* outright the moment it is added to disk but
# not registered, or vice versa (declared but deleted from disk), rather
# than relying on any test executable's own runtime directory walk. See
# CMakeLists.txt for the FATAL_ERROR gate that calls this function against
# the real src/ tree before add_library(arkham_foundation ...) is even
# invoked, and tests/cmake/DomainHeaderInventoryPolicyTest.cmake for the
# `cmake -P`-driven ctest case that exercises this function's three
# possible outcomes (exact match / undeclared extra / declared-but-missing)
# against a synthetic scratch tree, independent of the real src/ layout.
#
# This function itself never calls message(FATAL_ERROR ...) on a
# mismatch -- only on genuinely missing/misused arguments -- so it stays
# independently unit-testable via `cmake -P`: the caller decides what to
# do with a non-empty OUT_ERROR.
#
# Arguments:
#   BASE_DIR         Directory that DECLARED_HEADERS entries and the
#                     glob's discovered paths are both made relative to
#                     (e.g. the project root, so "src/Foo.h" is what both
#                     sides compare against).
#   HEADER_GLOB_DIR   Directory recursively searched for "*.h" files
#                     (e.g. "${BASE_DIR}/src"). CONFIGURE_DEPENDS is
#                     passed to file(GLOB_RECURSE ...) so adding or
#                     removing a header re-triggers CMake's own
#                     configure-time re-check on the next build, without
#                     requiring a fully manual reconfigure -- except
#                     under `cmake -P` script mode (CONFIGURE_DEPENDS is
#                     rejected there outright), which this function
#                     detects via CMAKE_SCRIPT_MODE_FILE and silently
#                     omits, since a one-shot script has no later
#                     "configure" to re-trigger anyway.
#   DECLARED_HEADERS  The explicit list every header is expected to
#                     appear in (e.g. CMakeLists.txt's own
#                     ARKHAM_FOUNDATION_HEADERS), as BASE_DIR-relative
#                     paths using forward slashes.
#   OUT_ERROR         Name of a variable set (in the caller's scope) to
#                     an empty string on success, or a human-readable,
#                     newline-separated description of every mismatch
#                     found (each naming the exact offending path) on
#                     failure.
function(arkham_check_domain_header_inventory)
    set(oneValueArgs BASE_DIR HEADER_GLOB_DIR OUT_ERROR)
    set(multiValueArgs DECLARED_HEADERS)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_BASE_DIR)
        message(FATAL_ERROR "arkham_check_domain_header_inventory: BASE_DIR is required")
    endif()
    if(NOT ARG_HEADER_GLOB_DIR)
        message(FATAL_ERROR "arkham_check_domain_header_inventory: HEADER_GLOB_DIR is required")
    endif()
    if(NOT ARG_OUT_ERROR)
        message(FATAL_ERROR "arkham_check_domain_header_inventory: OUT_ERROR is required")
    endif()

    # CONFIGURE_DEPENDS makes a normal `cmake` project configure re-run
    # automatically the next time this glob's result would change (i.e.
    # a header is added/removed under HEADER_GLOB_DIR), without the
    # caller having to manually reconfigure. It is rejected outright,
    # though, by `cmake -P` script mode (also known as "find package"
    # mode in some diagnostics) -- see
    # tests/cmake/DomainHeaderInventoryPolicyTest.cmake, which calls this
    # very function that way to exercise it against a synthetic scratch
    # tree independent of the real src/ layout. CMAKE_SCRIPT_MODE_FILE is
    # only ever non-empty while running under `cmake -P`, so it is the
    # correct switch here rather than duplicating this whole function.
    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB_RECURSE _arkham_discovered_headers
            LIST_DIRECTORIES false
            RELATIVE "${ARG_BASE_DIR}"
            "${ARG_HEADER_GLOB_DIR}/*.h"
        )
    else()
        file(GLOB_RECURSE _arkham_discovered_headers
            LIST_DIRECTORIES false
            RELATIVE "${ARG_BASE_DIR}"
            CONFIGURE_DEPENDS
            "${ARG_HEADER_GLOB_DIR}/*.h"
        )
    endif()
    list(SORT _arkham_discovered_headers)

    set(_arkham_declared_headers ${ARG_DECLARED_HEADERS})
    list(SORT _arkham_declared_headers)

    set(_arkham_errors "")

    foreach(_arkham_path IN LISTS _arkham_discovered_headers)
        list(FIND _arkham_declared_headers "${_arkham_path}" _arkham_index)
        if(_arkham_index EQUAL -1)
            string(APPEND _arkham_errors
                "  on disk but not declared in DECLARED_HEADERS: ${_arkham_path}\n")
        endif()
    endforeach()

    foreach(_arkham_path IN LISTS _arkham_declared_headers)
        list(FIND _arkham_discovered_headers "${_arkham_path}" _arkham_index)
        if(_arkham_index EQUAL -1)
            string(APPEND _arkham_errors
                "  declared in DECLARED_HEADERS but missing from disk: ${_arkham_path}\n")
        endif()
    endforeach()

    if(_arkham_errors)
        set("${ARG_OUT_ERROR}"
            "Domain header inventory drift detected between the recursive glob of \"${ARG_HEADER_GLOB_DIR}/*.h\" and the explicitly declared header list passed as DECLARED_HEADERS (see CMakeLists.txt's ARKHAM_FOUNDATION_HEADERS). Every header under this directory -- including one in any newly added nested subdirectory -- must be explicitly registered there; add or remove the entries named below to resolve:\n${_arkham_errors}"
            PARENT_SCOPE)
    else()
        set("${ARG_OUT_ERROR}" "" PARENT_SCOPE)
    endif()
endfunction()
