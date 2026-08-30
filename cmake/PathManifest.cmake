# Provides arkham_write_path_manifest(), a small reusable CMake function
# that writes a plain, one-absolute-path-per-line text manifest (not
# JSON -- these are simple filesystem paths with no characters that would
# need escaping, so a text format avoids any CMake-side JSON-escaping
# subtlety) from a BASE_DIR-relative PATHS list, regenerated every time
# CMake reconfigures.
#
# This is the single source of truth packaging/check_encoder_hygiene.py's
# own AST scan is scoped to for every one of the four manifests
# CMakeLists.txt writes with it (domain headers/sources, foundation
# headers/sources): the script never independently re-derives or
# hand-maintains any of these file lists, so they cannot silently drift
# from what each target actually declares/compiles -- the same guarantee
# arkham_check_domain_header_inventory() (see DomainHeaderInventory.cmake)
# already gives the *build* itself.
#
# Arguments:
#   OUTPUT_FILE  Absolute path of the manifest file to write.
#   BASE_DIR     Directory each PATHS entry is joined against to produce
#                an absolute path in the written manifest (e.g.
#                CMAKE_CURRENT_SOURCE_DIR, so "src/domain/RawJson.h"
#                becomes an absolute path a Python script run from any
#                working directory can open directly).
#   PATHS        The BASE_DIR-relative paths to write, one per line.
function(arkham_write_path_manifest)
    set(oneValueArgs OUTPUT_FILE BASE_DIR)
    set(multiValueArgs PATHS)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_write_path_manifest: OUTPUT_FILE is required")
    endif()
    if(NOT ARG_BASE_DIR)
        message(FATAL_ERROR "arkham_write_path_manifest: BASE_DIR is required")
    endif()

    # file(WRITE) has created missing parent directories itself since
    # CMake 3.20 (below this project's 3.25 floor), but this
    # MAKE_DIRECTORY stays explicit so the file(WRITE) below never relies
    # on that implicit behavior.
    get_filename_component(_arkham_manifest_dir "${ARG_OUTPUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_arkham_manifest_dir}")
    unset(_arkham_manifest_dir)

    set(_arkham_manifest_lines "")
    foreach(_arkham_manifest_path IN LISTS ARG_PATHS)
        string(APPEND _arkham_manifest_lines "${ARG_BASE_DIR}/${_arkham_manifest_path}\n")
    endforeach()
    unset(_arkham_manifest_path)
    file(WRITE "${ARG_OUTPUT_FILE}" "${_arkham_manifest_lines}")
    unset(_arkham_manifest_lines)
endfunction()
