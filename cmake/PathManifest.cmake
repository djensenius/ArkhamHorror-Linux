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

# Provides arkham_write_target_header_set_manifest(), which writes the
# same kind of one-absolute-path-per-line manifest as
# arkham_write_path_manifest() above, but sourced from a target's actual,
# live CMake FILE_SET metadata (the HEADER_SETS / HEADER_SET_<NAME>
# target properties CMake itself maintains -- see
# https://cmake.org/cmake/help/latest/prop_tgt/HEADER_SETS.html) instead
# of a hand-authored PATHS list.
#
# A review round demonstrated that packaging/check_encoder_hygiene.py's
# header manifests, when generated from ARKHAM_DOMAIN_HEADERS/
# ARKHAM_FOUNDATION_HEADERS (plain hand-maintained `set()` variables)
# rather than from the actual target, could silently miss a header
# registered via a *second*, independently-added `target_sources(...
# FILE_SET <other-name> TYPE HEADERS ...)` call on the same target --
# nothing re-derived the manifest from what the target *actually*
# exposes as public headers, so a second/late file set (or, in
# principle, one added by different/generated CMake code entirely) would
# never reach the AST scanner at all. This function closes that gap
# structurally: HEADER_SETS enumerates *every* named header-type file
# set currently registered on the target (however many there are, added
# however many separate target_sources() calls it took), and
# HEADER_SET_<name> resolves to that one named set's exact file list;
# `file(GENERATE)` (not `file(WRITE)`) is required here because these
# properties -- like most target properties -- are only guaranteed
# fully, finally populated at CMake *generate* time, once every
# target_sources() call anywhere in the whole project for this target
# has been processed, regardless of where in CMakeLists.txt this
# function itself happens to be called from.
#
# Arguments:
#   TARGET       The target whose HEADER_SETS this manifest is derived
#                from (e.g. arkham_domain_models, arkham_foundation).
#   OUTPUT_FILE  Absolute path of the manifest file to write.
function(arkham_write_target_header_set_manifest)
    set(oneValueArgs TARGET OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "arkham_write_target_header_set_manifest: TARGET is required")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_write_target_header_set_manifest: OUTPUT_FILE is required")
    endif()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "arkham_write_target_header_set_manifest: no such target '${ARG_TARGET}'")
    endif()

    get_filename_component(_arkham_thsm_dir "${ARG_OUTPUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_arkham_thsm_dir}")
    unset(_arkham_thsm_dir)

    # HEADER_SETS itself (unlike HEADER_SET_<NAME>) is a plain,
    # immediately-available list of file-set *names* -- not a
    # generator-expression-valued property -- so it can be read directly
    # here at configure time via get_target_property(), as long as every
    # target_sources(...FILE_SET...) call for this target earlier in
    # this same configure run has already executed (true for both of
    # this project's targets: their FILE_SET registrations happen
    # immediately after their add_library() calls, well before this
    # function is ever invoked on them).
    get_target_property(_arkham_thsm_names ${ARG_TARGET} HEADER_SETS)
    if(NOT _arkham_thsm_names)
        message(FATAL_ERROR
            "arkham_write_target_header_set_manifest: target '${ARG_TARGET}' has no "
            "HEADERS-type FILE_SET registered at all -- nothing for the encoder-hygiene "
            "AST scanner to audit for this target, which is almost certainly a build "
            "misconfiguration rather than an intentionally headerless target.")
    endif()

    # Build ONE combined generator expression naming every one of this
    # target's header-set names' own file lists, joined with real
    # newlines: $<JOIN:$<TARGET_PROPERTY:tgt,HEADER_SET_name1>;$<TARGET_PROPERTY:tgt,HEADER_SET_name2>;...,\n>
    # Nested generator expressions evaluate inside-out, so each
    # HEADER_SET_<name> resolves to that set's own semicolon-separated
    # absolute file list first, and the outer $<JOIN:...> then flattens
    # the whole thing (however many names there turn out to be, now or
    # after some future second FILE_SET is added) into one newline-per-path manifest --
    # exactly the format packaging/check_encoder_hygiene.py's
    # _read_manifest() already expects, with zero changes needed there.
    set(_arkham_thsm_pieces "")
    foreach(_arkham_thsm_name IN LISTS _arkham_thsm_names)
        list(APPEND _arkham_thsm_pieces "$<TARGET_PROPERTY:${ARG_TARGET},HEADER_SET_${_arkham_thsm_name}>")
    endforeach()
    unset(_arkham_thsm_name)
    unset(_arkham_thsm_names)

    file(GENERATE OUTPUT "${ARG_OUTPUT_FILE}"
        CONTENT "$<JOIN:${_arkham_thsm_pieces},\n>\n"
    )
    unset(_arkham_thsm_pieces)
endfunction()
