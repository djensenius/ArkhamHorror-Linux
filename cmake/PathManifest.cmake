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
# A THIRD review round showed this function still only reads HEADER_SETS
# (the names of this target's own PUBLIC/PRIVATE FILE_SET-registered
# header sets), never INTERFACE_HEADER_SETS (the analogous property for
# INTERFACE-visibility FILE_SET registrations -- see
# https://cmake.org/cmake/help/latest/prop_tgt/INTERFACE_HEADER_SETS.html).
# Neither of this project's two targets currently registers an INTERFACE
# header set, but nothing about this function's own logic actually
# depended on that being true -- an INTERFACE FILE_SET added later would
# have silently produced a header this manifest, and therefore the AST
# policy script's scan, never saw at all. This function now unions BOTH
# properties' NAME lists into one deduplicated list, then reads every
# name's own file list from the SAME `HEADER_SET_<name>` property --
# real, empirical testing against this project's own pinned CMake
# 4.4.3 (`cmake --help-property-list`, and a real scratch-project
# generator-expression probe) showed there is no separate
# `INTERFACE_HEADER_SET_<name>` property at all: an earlier revision of
# this function assumed one existed and read it for INTERFACE-visibility
# names specifically, which always silently generated an EMPTY result
# (not an error), so an INTERFACE-only FILE_SET's headers were being
# unconditionally DROPPED from this manifest the whole time, exactly
# opposite of the fix this comment originally claimed. `HEADER_SET_<name>`
# alone (see
# https://cmake.org/cmake/help/latest/prop_tgt/HEADER_SET_NAME.html,
# which documents no INTERFACE-prefixed counterpart) correctly resolves
# for a name regardless of which list (HEADER_SETS or
# INTERFACE_HEADER_SETS) it came from. See
# tests/cmake/DeferredTargetManifestPolicyTest.cmake's
# InterfaceHeaderSetIsIncludedInManifestTest for the real, fail-before/
# pass-after CMake mutation test proving this.
#
# A LATER review round showed this alone is still not sufficient:
# HEADER_SETS itself (see the doc comment inside the function body
# below) is read via a plain, immediate get_target_property() call --
# `file(GENERATE)` only defers *evaluating the per-name file-list
# generator expressions built from that already-captured names list*, it
# does not retroactively discover a brand-new file-set *name* that a
# later `target_sources(...FILE_SET <even-later-name>...)` call registers
# after this function has already been invoked once. The names list is
# captured too early, at ordinary configure-time call order, regardless
# of file(GENERATE)'s own separate generate-time deferral for the
# *values* of already-known names. Callers of this function (see
# CMakeLists.txt) MUST therefore invoke it via
# `cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL
# arkham_write_target_header_set_manifest ...)`, never directly -- this
# defers the *entire call*, including its internal
# get_target_property(... HEADER_SETS) read, until CMake finishes
# processing the current directory scope (i.e. after every
# target_sources() call anywhere later in the same CMakeLists.txt has
# already executed), so a file set registered after the textual point
# this function is invoked from is still captured. See
# tests/cmake/DeferredTargetManifestPolicyTest.cmake for the real,
# fail-before/pass-after CMake mutation test proving both the deferred
# and (for contrast) non-deferred call behavior.
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
    # generator-expression-valued property -- so a get_target_property()
    # call here sees every name registered on this target *as of the
    # moment this function body actually executes*, which is why this
    # function must only ever be invoked through the
    # cmake_language(DEFER ...) wrapper documented above: called
    # directly/immediately, this would only see whichever FILE_SET
    # registrations happened to precede the call site textually, exactly
    # the bug a review round demonstrated.
    get_target_property(_arkham_thsm_public_names ${ARG_TARGET} HEADER_SETS)
    get_target_property(_arkham_thsm_interface_names ${ARG_TARGET} INTERFACE_HEADER_SETS)
    if(NOT _arkham_thsm_public_names AND NOT _arkham_thsm_interface_names)
        message(FATAL_ERROR
            "arkham_write_target_header_set_manifest: target '${ARG_TARGET}' has no "
            "HEADERS-type FILE_SET registered at all (neither PUBLIC/PRIVATE nor "
            "INTERFACE) -- nothing for the encoder-hygiene AST scanner to audit for "
            "this target, which is almost certainly a build misconfiguration rather "
            "than an intentionally headerless target.")
    endif()
    if(NOT _arkham_thsm_public_names)
        set(_arkham_thsm_public_names "")
    endif()
    if(NOT _arkham_thsm_interface_names)
        set(_arkham_thsm_interface_names "")
    endif()

    # A real, empirically-verified-against-this-project's-own-pinned-
    # CMake-4.4.3 `cmake --help-property-list` run showed there is NO
    # such property as `INTERFACE_HEADER_SET_<name>` at all -- an earlier
    # revision of this function assumed one existed (by false analogy
    # with INTERFACE_SOURCES/SOURCES being genuinely distinct properties
    # below) and read it for every INTERFACE-visibility name, which
    # always silently resolves to an EMPTY generator-expression result,
    # not an error -- so an INTERFACE-only FILE_SET's headers were
    # SILENTLY DROPPED from this manifest entirely despite
    # INTERFACE_HEADER_SETS correctly listing the set's *name*. The one
    # real, universal property for ANY named header set's own file
    # list, regardless of whether that name came from HEADER_SETS or
    # INTERFACE_HEADER_SETS, is `HEADER_SET_<name>` (confirmed directly:
    # `$<TARGET_PROPERTY:tgt,HEADER_SET_someInterfaceSetName>` correctly
    # resolves to that set's files even when the set itself was
    # registered via `target_sources(tgt INTERFACE FILE_SET
    # someInterfaceSetName ...)` -- see
    # https://cmake.org/cmake/help/latest/prop_tgt/HEADER_SET_NAME.html,
    # which documents no INTERFACE-prefixed counterpart). Both name
    # lists are therefore combined into ONE deduplicated list here and
    # every name (whatever its originating visibility) is read via the
    # SAME `HEADER_SET_<name>` property below. See
    # tests/cmake/DeferredTargetManifestPolicyTest.cmake's
    # InterfaceHeaderSetIsIncludedInManifestTest for the real,
    # fail-before/pass-after mutation test proving both that this
    # specific bug was real and reproducible, and that this fix closes
    # it.
    set(_arkham_thsm_all_names "")
    list(APPEND _arkham_thsm_all_names ${_arkham_thsm_public_names})
    list(APPEND _arkham_thsm_all_names ${_arkham_thsm_interface_names})
    if(_arkham_thsm_all_names)
        list(REMOVE_DUPLICATES _arkham_thsm_all_names)
    endif()
    unset(_arkham_thsm_public_names)
    unset(_arkham_thsm_interface_names)

    # Build ONE combined generator expression naming every one of this
    # target's header-set names' own file lists, joined with real
    # newlines:
    # $<JOIN:$<TARGET_PROPERTY:tgt,HEADER_SET_name1>;$<TARGET_PROPERTY:tgt,HEADER_SET_name2>;...,\n>
    # Nested generator expressions evaluate inside-out, so each
    # HEADER_SET_<name> resolves to that set's own semicolon-separated
    # absolute file list first, and the outer $<JOIN:...> then flattens
    # the whole thing (however many names there turn out to be, now or
    # after some future second FILE_SET is added, of either visibility)
    # into one newline-per-path manifest -- exactly the format
    # packaging/check_encoder_hygiene.py's _read_manifest() already
    # expects, with zero changes needed there.
    set(_arkham_thsm_pieces "")
    foreach(_arkham_thsm_name IN LISTS _arkham_thsm_all_names)
        list(APPEND _arkham_thsm_pieces "$<TARGET_PROPERTY:${ARG_TARGET},HEADER_SET_${_arkham_thsm_name}>")
    endforeach()
    unset(_arkham_thsm_name)
    unset(_arkham_thsm_all_names)

    file(GENERATE OUTPUT "${ARG_OUTPUT_FILE}"
        CONTENT "$<JOIN:${_arkham_thsm_pieces},\n>\n"
    )
    unset(_arkham_thsm_pieces)
endfunction()

# Provides arkham_write_target_source_manifest(), the SOURCES-property
# analog of arkham_write_target_header_set_manifest() above: writes a
# one-absolute-path-per-line manifest derived directly from a target's
# own live SOURCES/SOURCE_DIR target properties, rather than from a
# hand-authored ARKHAM_DOMAIN_SOURCES/ARKHAM_FOUNDATION_SOURCES `set()`
# variable. A review round demonstrated the domain/foundation dependency-
# direction boundary was audited for headers/fragments only -- every real
# production .cpp compiled straight past packaging/check_encoder_hygiene.py's
# AST scan entirely, so a source file could #include an absolute/"../"/
# symlinked/generated forbidden header, inherit a lossy encoder from it,
# and compile cleanly while the policy stayed green (see run_check()'s
# module docstring). Closing that gap requires the Python script to parse
# every REAL .cpp with its own exact compile_commands.json entry (see
# _scan_sources()); this manifest is what tells it, authoritatively, the
# complete list of sources to do that for -- sourced from the same live
# target metadata the build itself compiles, so it can never silently
# miss a source added via a second/later target_sources() call the way a
# hand-maintained variable could.
#
# Unlike HEADER_SET_<name>, plain SOURCES is not itself a per-config/
# generator-expression-only-resolvable property in the way that would
# require file(GENERATE) to read safely -- get_target_property(<tgt>
# SOURCES) returns an ordinary, immediately-usable list of
# SOURCE_DIR-relative paths once queried. The SAME early-snapshot hazard
# arkham_write_target_header_set_manifest() has for HEADER_SETS still
# applies here, though: calling this function BEFORE a later
# target_sources(<tgt> PRIVATE ...) call registers more sources would
# silently omit them. Callers MUST therefore invoke this function via the
# identical cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
# CALL arkham_write_target_source_manifest ...) wrapper documented above
# arkham_write_target_header_set_manifest() -- see
# tests/cmake/DeferredTargetManifestPolicyTest.cmake for the matching
# fail-before/pass-after mutation coverage.
#
# A THIRD review round showed two further gaps here:
#
#   - Only plain SOURCES was read, never INTERFACE_SOURCES (the
#     analogous property for sources a target exposes to its consumers
#     without compiling itself -- see
#     https://cmake.org/cmake/help/latest/prop_tgt/INTERFACE_SOURCES.html).
#     Neither of this project's two targets currently registers any
#     INTERFACE sources, but exactly like INTERFACE_HEADER_SETS above,
#     nothing about this function's logic actually depended on that
#     staying true. Both properties' lists are now unioned (in that
#     order, with SOURCE_DIR-relative entries resolved against this
#     target's own SOURCE_DIR either way) before being written out.
#   - This project's global `set(CMAKE_AUTOMOC ON)` (see CMakeLists.txt)
#     means both arkham_domain_models and arkham_foundation each get a
#     real, separately-compiled AUTOMOC-generated `mocs_compilation.cpp`
#     translation unit -- confirmed directly against this project's own
#     dedicated Clang build directory's compile_commands.json, which
#     lists exactly one such entry per target, at CMake's own stable,
#     documented default location
#     (https://cmake.org/cmake/help/latest/prop_tgt/AUTOGEN_BUILD_DIR.html):
#     `<AUTOGEN_BUILD_DIR>/mocs_compilation.cpp`, where AUTOGEN_BUILD_DIR
#     itself defaults to `<dir-matching-CMAKE_CURRENT_BINARY_DIR>/
#     <target-name>_autogen` when never explicitly overridden. This file
#     is neither in SOURCES nor INTERFACE_SOURCES (AUTOMOC's generated
#     compilation unit is not a property CMake exposes through either
#     one), so it was previously invisible to this manifest -- and
#     therefore to packaging/check_encoder_hygiene.py's dependency-
#     direction/inclusion-graph audit -- entirely, even though it is
#     genuinely compiled as part of the target and does appear in the
#     real compile_commands.json (proven directly: this project has no
#     Q_OBJECT/Q_GADGET types in either target today, so
#     arkham_domain_models's own mocs_compilation.cpp is presently a
#     trivial "no moc" stub, but arkham_foundation's is not -- it
#     #includes several real per-class moc_*.cpp files, e.g. for
#     SessionCoordinator/InputRouter/NetworkAuthenticationClient). This
#     function now appends that target's own computed
#     mocs_compilation.cpp path whenever CMake's own AUTOMOC target
#     property is enabled for it, so a future Q_OBJECT/Q_GADGET type
#     added to either target's generated moc output is audited exactly
#     like any other compiled translation unit, never silently skipped.
#     packaging/check_encoder_hygiene.py independently requires every
#     target/configuration object command for each manifested physical
#     source, keyed by compile_commands.json's CMakeFiles/<target>.dir
#     output identity. Missing, ambiguous, duplicate, or targetless
#     entries fail closed, and each entry's own working directory plus
#     command/arguments representation is used verbatim after dropping
#     compile-only flags. AUTOGEN_BUILD_DIR is separately registered as
#     owned generated metadata and cross-checked against the target's
#     AutogenInfo.json; it is never an external/trusted subtree. See
#     tests/cmake/
#     DeferredTargetManifestPolicyTest.cmake's
#     AutomocGeneratedCompilationUnitIsIncludedInManifestTest for the
#     real, fail-before/pass-after CMake mutation test proving this
#     specific fix.)
#
# Arguments:
#   TARGET       The target whose SOURCES this manifest is derived from.
#   OUTPUT_FILE  Absolute path of the manifest file to write.
function(arkham_write_target_source_manifest)
    set(oneValueArgs TARGET OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "arkham_write_target_source_manifest: TARGET is required")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_write_target_source_manifest: OUTPUT_FILE is required")
    endif()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "arkham_write_target_source_manifest: no such target '${ARG_TARGET}'")
    endif()

    get_filename_component(_arkham_tsm_dir "${ARG_OUTPUT_FILE}" DIRECTORY)
    file(MAKE_DIRECTORY "${_arkham_tsm_dir}")
    unset(_arkham_tsm_dir)

    get_target_property(_arkham_tsm_source_dir ${ARG_TARGET} SOURCE_DIR)
    get_target_property(_arkham_tsm_own_sources ${ARG_TARGET} SOURCES)
    get_target_property(_arkham_tsm_interface_sources ${ARG_TARGET} INTERFACE_SOURCES)
    if(NOT _arkham_tsm_own_sources AND NOT _arkham_tsm_interface_sources)
        message(FATAL_ERROR
            "arkham_write_target_source_manifest: target '${ARG_TARGET}' has no "
            "SOURCES or INTERFACE_SOURCES at all -- nothing for the encoder-hygiene "
            "AST scanner to audit for this target, which is almost certainly a build "
            "misconfiguration rather than an intentionally sourceless target.")
    endif()
    if(NOT _arkham_tsm_own_sources)
        set(_arkham_tsm_own_sources "")
    endif()
    if(NOT _arkham_tsm_interface_sources)
        set(_arkham_tsm_interface_sources "")
    endif()
    # Deliberately UNQUOTED expansions below: list(APPEND var "") always
    # adds one literal empty-string element (an unwanted stray manifest
    # line resolving to just this target's own SOURCE_DIR), whereas an
    # unquoted expansion of an empty-string variable supplies zero
    # arguments, correctly appending nothing when a property was never
    # set at all.
    set(_arkham_tsm_sources "")
    list(APPEND _arkham_tsm_sources ${_arkham_tsm_own_sources})
    list(APPEND _arkham_tsm_sources ${_arkham_tsm_interface_sources})
    unset(_arkham_tsm_own_sources)
    unset(_arkham_tsm_interface_sources)

    # PUBLIC sources occur in both SOURCES and INTERFACE_SOURCES. Resolve
    # every entry to one normalized physical path and deduplicate that
    # combined universe before writing; target/config command ownership
    # remains separate in target_policy.txt/compile_commands.json.
    set(_arkham_tsm_normalized_sources "")
    foreach(_arkham_tsm_path IN LISTS _arkham_tsm_sources)
        if(_arkham_tsm_path MATCHES "\\$<")
            message(FATAL_ERROR
                "arkham_write_target_source_manifest: generator-expression source "
                "'${_arkham_tsm_path}' cannot be assigned one fail-closed physical identity")
        endif()
        cmake_path(IS_RELATIVE _arkham_tsm_path _arkham_tsm_is_relative)
        if(_arkham_tsm_is_relative)
            cmake_path(ABSOLUTE_PATH _arkham_tsm_path
                BASE_DIRECTORY "${_arkham_tsm_source_dir}"
                NORMALIZE
                OUTPUT_VARIABLE _arkham_tsm_absolute)
        else()
            cmake_path(NORMAL_PATH _arkham_tsm_path
                OUTPUT_VARIABLE _arkham_tsm_absolute)
        endif()
        file(REAL_PATH "${_arkham_tsm_absolute}" _arkham_tsm_real)
        list(APPEND _arkham_tsm_normalized_sources "${_arkham_tsm_real}")
    endforeach()
    unset(_arkham_tsm_path)
    unset(_arkham_tsm_is_relative)
    unset(_arkham_tsm_absolute)
    unset(_arkham_tsm_real)
    unset(_arkham_tsm_sources)
    unset(_arkham_tsm_source_dir)
    list(REMOVE_DUPLICATES _arkham_tsm_normalized_sources)
    list(SORT _arkham_tsm_normalized_sources)

    set(_arkham_tsm_lines "")
    foreach(_arkham_tsm_path IN LISTS _arkham_tsm_normalized_sources)
        string(APPEND _arkham_tsm_lines "${_arkham_tsm_path}\n")
    endforeach()
    unset(_arkham_tsm_path)
    unset(_arkham_tsm_normalized_sources)

    # AUTOMOC's own generated aggregation translation unit is compiled as
    # part of this target (see the doc comment above this function) but
    # is exposed through neither SOURCES nor INTERFACE_SOURCES -- append
    # its own, CMake-documented default path directly whenever AUTOMOC is
    # enabled for this target, so it is never silently missing from this
    # manifest (and therefore from the AST policy scan) the way a review
    # round demonstrated it previously was.
    get_target_property(_arkham_tsm_automoc ${ARG_TARGET} AUTOMOC)
    if(_arkham_tsm_automoc)
        get_target_property(_arkham_tsm_autogen_dir ${ARG_TARGET} AUTOGEN_BUILD_DIR)
        if(NOT _arkham_tsm_autogen_dir)
            get_target_property(_arkham_tsm_binary_dir ${ARG_TARGET} BINARY_DIR)
            set(_arkham_tsm_autogen_dir "${_arkham_tsm_binary_dir}/${ARG_TARGET}_autogen")
            unset(_arkham_tsm_binary_dir)
        endif()
        string(APPEND _arkham_tsm_lines "${_arkham_tsm_autogen_dir}/mocs_compilation.cpp\n")
        unset(_arkham_tsm_autogen_dir)
    endif()
    unset(_arkham_tsm_automoc)

    file(WRITE "${ARG_OUTPUT_FILE}" "${_arkham_tsm_lines}")
    unset(_arkham_tsm_lines)
endfunction()

# Register an AUTOMOC target's generated directory as project-owned
# metadata. It is deliberately not an external/trusted root:
# check_encoder_hygiene.py cross-checks every C/C++ artifact beneath it
# against CMake's target-specific AutogenInfo.json, audits the generated
# compilation unit with its exact object compile command, and allows
# only the concrete moc outputs CMake enumerates there.
function(arkham_append_target_autogen_manifest)
    set(oneValueArgs TARGET POLICY OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "arkham_append_target_autogen_manifest: TARGET is required")
    endif()
    if(NOT ARG_POLICY MATCHES "^(domain|foundation|application)$")
        message(FATAL_ERROR "arkham_append_target_autogen_manifest: POLICY must be domain, foundation, or application")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_append_target_autogen_manifest: OUTPUT_FILE is required")
    endif()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "arkham_append_target_autogen_manifest: no such target '${ARG_TARGET}'")
    endif()

    get_target_property(_arkham_tam_automoc ${ARG_TARGET} AUTOMOC)
    if(_arkham_tam_automoc)
        get_target_property(_arkham_tam_autogen_dir ${ARG_TARGET} AUTOGEN_BUILD_DIR)
        if(NOT _arkham_tam_autogen_dir)
            get_target_property(_arkham_tam_binary_dir ${ARG_TARGET} BINARY_DIR)
            set(_arkham_tam_autogen_dir "${_arkham_tam_binary_dir}/${ARG_TARGET}_autogen")
            unset(_arkham_tam_binary_dir)
        endif()
        file(APPEND "${ARG_OUTPUT_FILE}"
            "${ARG_POLICY}\t${ARG_TARGET}\t${_arkham_tam_autogen_dir}\n")
        unset(_arkham_tam_autogen_dir)
    endif()
    unset(_arkham_tam_automoc)
endfunction()

# Record every target that may contribute a compile_commands.json entry.
# SCAN targets receive a domain/foundation/application closure; EXTERNAL,
# TEST, and TRY_COMPILE exclusions are exact target metadata, never
# inferred from a path or target-name pattern.
function(arkham_append_encoder_hygiene_target)
    set(oneValueArgs TARGET CLASSIFICATION POLICY CONTEXT_TARGET EXEMPT_REASON OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TARGET OR NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "arkham_append_encoder_hygiene_target: TARGET must name an existing target")
    endif()
    if(NOT ARG_CLASSIFICATION MATCHES "^(SCAN|EXTERNAL|TEST|TRY_COMPILE)$")
        message(FATAL_ERROR "arkham_append_encoder_hygiene_target: invalid CLASSIFICATION '${ARG_CLASSIFICATION}'")
    endif()
    if(ARG_CLASSIFICATION STREQUAL "SCAN")
        if(NOT ARG_POLICY MATCHES "^(domain|foundation|application)$")
            message(FATAL_ERROR "arkham_append_encoder_hygiene_target: SCAN requires domain, foundation, or application POLICY")
        endif()
        if(NOT ARG_CONTEXT_TARGET)
            set(ARG_CONTEXT_TARGET "${ARG_TARGET}")
        endif()
        if(NOT TARGET ${ARG_CONTEXT_TARGET})
            message(FATAL_ERROR "arkham_append_encoder_hygiene_target: CONTEXT_TARGET must exist")
        endif()
        get_target_property(_arkham_eht_type ${ARG_TARGET} TYPE)
        if(_arkham_eht_type STREQUAL "INTERFACE_LIBRARY" AND ARG_CONTEXT_TARGET STREQUAL ARG_TARGET)
            message(FATAL_ERROR "arkham_append_encoder_hygiene_target: INTERFACE target requires an explicit compiled CONTEXT_TARGET")
        endif()
    elseif(ARG_POLICY OR ARG_CONTEXT_TARGET)
        message(FATAL_ERROR "arkham_append_encoder_hygiene_target: exempt targets must not specify POLICY/CONTEXT_TARGET")
    elseif(NOT ARG_EXEMPT_REASON)
        message(FATAL_ERROR "arkham_append_encoder_hygiene_target: exempt target requires EXEMPT_REASON")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_append_encoder_hygiene_target: OUTPUT_FILE is required")
    endif()

    get_target_property(_arkham_eht_type ${ARG_TARGET} TYPE)
    get_target_property(_arkham_eht_source_dir ${ARG_TARGET} SOURCE_DIR)
    get_target_property(_arkham_eht_binary_dir ${ARG_TARGET} BINARY_DIR)
    file(APPEND "${ARG_OUTPUT_FILE}"
        "${ARG_CLASSIFICATION}\t${ARG_POLICY}\t${ARG_TARGET}\t${_arkham_eht_type}\t${_arkham_eht_source_dir}\t${_arkham_eht_binary_dir}\t${ARG_CONTEXT_TARGET}\t${ARG_EXEMPT_REASON}\n")
    set_property(TARGET ${ARG_TARGET} PROPERTY ARKHAM_ENCODER_HYGIENE_CLASSIFICATION "${ARG_CLASSIFICATION}")
    set_property(TARGET ${ARG_TARGET} PROPERTY ARKHAM_ENCODER_HYGIENE_POLICY "${ARG_POLICY}")
    set_property(TARGET ${ARG_TARGET} PROPERTY ARKHAM_ENCODER_HYGIENE_CONTEXT_TARGET "${ARG_CONTEXT_TARGET}")
    set_property(TARGET ${ARG_TARGET} PROPERTY ARKHAM_ENCODER_HYGIENE_EXEMPT_REASON "${ARG_EXEMPT_REASON}")
    unset(_arkham_eht_type)
    unset(_arkham_eht_source_dir)
    unset(_arkham_eht_binary_dir)
endfunction()

function(_arkham_collect_buildsystem_targets directory out_var)
    get_property(_arkham_cbt_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_arkham_cbt_subdirs DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    set(_arkham_cbt_all ${_arkham_cbt_targets})
    foreach(_arkham_cbt_subdir IN LISTS _arkham_cbt_subdirs)
        _arkham_collect_buildsystem_targets("${_arkham_cbt_subdir}" _arkham_cbt_nested)
        list(APPEND _arkham_cbt_all ${_arkham_cbt_nested})
    endforeach()
    list(REMOVE_DUPLICATES _arkham_cbt_all)
    set(${out_var} "${_arkham_cbt_all}" PARENT_SCOPE)
endfunction()

# At end-of-directory processing, prove every non-imported C++ target in
# the complete directory tree has an explicit SCAN/EXEMPT record and emit
# each SCAN target's own complete late/named/INTERFACE header universe.
function(arkham_write_encoder_hygiene_target_universe)
    set(oneValueArgs OUTPUT_FILE HEADER_INDEX_FILE HEADER_DIR SOURCE_INDEX_FILE SOURCE_DIR)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})
    if(NOT ARG_OUTPUT_FILE OR NOT ARG_HEADER_INDEX_FILE OR NOT ARG_HEADER_DIR
            OR NOT ARG_SOURCE_INDEX_FILE OR NOT ARG_SOURCE_DIR)
        message(FATAL_ERROR "arkham_write_encoder_hygiene_target_universe: all output arguments are required")
    endif()
    file(MAKE_DIRECTORY "${ARG_HEADER_DIR}")
    file(MAKE_DIRECTORY "${ARG_SOURCE_DIR}")
    _arkham_collect_buildsystem_targets("${CMAKE_SOURCE_DIR}" _arkham_ehu_targets)
    list(SORT _arkham_ehu_targets)
    set(_arkham_ehu_universe "")
    set(_arkham_ehu_index "")
    set(_arkham_ehu_source_index "")
    foreach(_arkham_ehu_target IN LISTS _arkham_ehu_targets)
        get_target_property(_arkham_ehu_imported ${_arkham_ehu_target} IMPORTED)
        get_target_property(_arkham_ehu_type ${_arkham_ehu_target} TYPE)
        if(_arkham_ehu_imported OR _arkham_ehu_type STREQUAL "UTILITY")
            continue()
        endif()
        if(NOT _arkham_ehu_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY|INTERFACE_LIBRARY)$")
            continue()
        endif()
        get_target_property(_arkham_ehu_class ${_arkham_ehu_target} ARKHAM_ENCODER_HYGIENE_CLASSIFICATION)
        if(NOT _arkham_ehu_class)
            message(FATAL_ERROR
                "Encoder-hygiene target universe: non-imported target '${_arkham_ehu_target}' "
                "(${_arkham_ehu_type}) has no explicit SCAN/EXEMPT classification")
        endif()
        string(APPEND _arkham_ehu_universe
            "${_arkham_ehu_target}\t${_arkham_ehu_type}\t${_arkham_ehu_class}\n")
        if(NOT _arkham_ehu_class STREQUAL "SCAN")
            continue()
        endif()

        string(SHA256 _arkham_ehu_hash "${_arkham_ehu_target}")
        string(SUBSTRING "${_arkham_ehu_hash}" 0 16 _arkham_ehu_short_hash)
        set(_arkham_ehu_header_file
            "${ARG_HEADER_DIR}/${_arkham_ehu_short_hash}.txt")
        set(_arkham_ehu_source_file
            "${ARG_SOURCE_DIR}/${_arkham_ehu_short_hash}.txt")
        get_target_property(_arkham_ehu_context ${_arkham_ehu_target} ARKHAM_ENCODER_HYGIENE_CONTEXT_TARGET)
        get_target_property(_arkham_ehu_policy ${_arkham_ehu_target} ARKHAM_ENCODER_HYGIENE_POLICY)

        get_target_property(_arkham_ehu_names ${_arkham_ehu_target} HEADER_SETS)
        get_target_property(_arkham_ehu_interface_names ${_arkham_ehu_target} INTERFACE_HEADER_SETS)
        if(NOT _arkham_ehu_names)
            set(_arkham_ehu_names "")
        endif()
        if(_arkham_ehu_interface_names)
            list(APPEND _arkham_ehu_names ${_arkham_ehu_interface_names})
        endif()
        list(REMOVE_DUPLICATES _arkham_ehu_names)
        set(_arkham_ehu_header_pieces "")
        foreach(_arkham_ehu_name IN LISTS _arkham_ehu_names)
            list(APPEND _arkham_ehu_header_pieces
                "$<TARGET_PROPERTY:${_arkham_ehu_target},HEADER_SET_${_arkham_ehu_name}>")
        endforeach()

        get_target_property(_arkham_ehu_source_dir ${_arkham_ehu_target} SOURCE_DIR)
        get_target_property(_arkham_ehu_sources ${_arkham_ehu_target} SOURCES)
        get_target_property(_arkham_ehu_interface_sources ${_arkham_ehu_target} INTERFACE_SOURCES)
        if(NOT _arkham_ehu_sources)
            set(_arkham_ehu_sources "")
        endif()
        if(_arkham_ehu_interface_sources)
            list(APPEND _arkham_ehu_sources ${_arkham_ehu_interface_sources})
        endif()
        foreach(_arkham_ehu_source IN LISTS _arkham_ehu_sources)
            if(_arkham_ehu_source MATCHES "\\.(h|hh|hpp|hxx|inc|inl|ipp|tpp)$")
                cmake_path(IS_RELATIVE _arkham_ehu_source _arkham_ehu_relative)
                if(_arkham_ehu_relative)
                    cmake_path(ABSOLUTE_PATH _arkham_ehu_source
                        BASE_DIRECTORY "${_arkham_ehu_source_dir}"
                        NORMALIZE OUTPUT_VARIABLE _arkham_ehu_source)
                endif()
                list(APPEND _arkham_ehu_header_pieces "${_arkham_ehu_source}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _arkham_ehu_header_pieces)
        file(GENERATE OUTPUT "${_arkham_ehu_header_file}"
            CONTENT "$<JOIN:${_arkham_ehu_header_pieces},\n>\n"
            TARGET ${_arkham_ehu_target})
        set(_arkham_ehu_source_pieces "")
        foreach(_arkham_ehu_source IN LISTS _arkham_ehu_sources)
            if(_arkham_ehu_source MATCHES "\\$<")
                list(APPEND _arkham_ehu_source_pieces "${_arkham_ehu_source}")
            else()
                cmake_path(IS_RELATIVE _arkham_ehu_source _arkham_ehu_relative)
                if(_arkham_ehu_relative)
                    cmake_path(ABSOLUTE_PATH _arkham_ehu_source
                        BASE_DIRECTORY "${_arkham_ehu_source_dir}"
                        NORMALIZE OUTPUT_VARIABLE _arkham_ehu_source)
                endif()
                list(APPEND _arkham_ehu_source_pieces "${_arkham_ehu_source}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES _arkham_ehu_source_pieces)
        file(GENERATE OUTPUT "${_arkham_ehu_source_file}"
            CONTENT "$<JOIN:${_arkham_ehu_source_pieces},\n>\n"
            TARGET ${_arkham_ehu_target})
        string(APPEND _arkham_ehu_index
            "${_arkham_ehu_target}\t${_arkham_ehu_policy}\t${_arkham_ehu_context}\t${_arkham_ehu_header_file}\n")
        string(APPEND _arkham_ehu_source_index
            "${_arkham_ehu_target}\t${_arkham_ehu_policy}\t${_arkham_ehu_context}\t${_arkham_ehu_source_file}\n")
    endforeach()
    file(WRITE "${ARG_OUTPUT_FILE}" "${_arkham_ehu_universe}")
    file(WRITE "${ARG_HEADER_INDEX_FILE}" "${_arkham_ehu_index}")
    file(WRITE "${ARG_SOURCE_INDEX_FILE}" "${_arkham_ehu_source_index}")
endfunction()
