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
#     packaging/check_encoder_hygiene.py's own
#     _find_compile_command_for_source() independently enforces the
#     other half of this: every source this manifest lists (including
#     this appended mocs_compilation.cpp entry) MUST have its own exact
#     compile_commands.json entry, or the scan hard-fails outright --
#     this manifest can never silently list a source Ninja/Make itself
#     never actually compiled. (compile_commands.json entries carry no
#     per-target attribution field a generator-agnostic reverse check
#     could key on -- e.g. the Ninja generator this project's own
#     packaging/check_encoder_hygiene.py build uses gives every entry the
#     SAME top-level build "directory" regardless of which target owns
#     it -- so the manifest-to-target-SOURCES direction covered by
#     arkham_write_target_source_manifest() itself, plus this function's
#     analogous AUTOGEN_BUILD_DIR/trusted-root registration for the one
#     concrete generated-TU gap a real build actually exposed, is what
#     this project actually enforces; see tests/cmake/
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

    # FILE_SET-registered headers do NOT appear in a target's plain
    # SOURCES property (a separate CMake property/concept entirely) --
    # this list is exactly the ordinary (non-FILE_SET) sources passed to
    # add_library()/target_sources(), i.e. real .cpp translation units,
    # each SOURCE_DIR-relative (never absolute) as CMake itself stores
    # them for a target defined via add_library(... ${SOURCES_VAR}) in
    # this same directory scope.
    set(_arkham_tsm_lines "")
    foreach(_arkham_tsm_path IN LISTS _arkham_tsm_sources)
        cmake_path(IS_RELATIVE _arkham_tsm_path _arkham_tsm_is_relative)
        if(_arkham_tsm_is_relative)
            string(APPEND _arkham_tsm_lines "${_arkham_tsm_source_dir}/${_arkham_tsm_path}\n")
        else()
            string(APPEND _arkham_tsm_lines "${_arkham_tsm_path}\n")
        endif()
    endforeach()
    unset(_arkham_tsm_path)
    unset(_arkham_tsm_is_relative)
    unset(_arkham_tsm_sources)
    unset(_arkham_tsm_source_dir)

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

# A real, running build subsequently showed that once
# arkham_write_target_source_manifest() (above) started registering each
# target's own AUTOMOC-generated `mocs_compilation.cpp` as a genuine,
# independently-scanned SOURCE, that translation unit's OWN transitive
# `#include`s of the individual per-Q_OBJECT-class `moc_*.cpp` fragments
# AUTOMOC generates (e.g. `moc_FocusController.cpp`, physically written
# under that same target's AUTOGEN_BUILD_DIR) were then, correctly,
# reached by packaging/check_encoder_hygiene.py's inclusion-graph audit
# for the first time -- and, having no manifest entry of their own,
# failed as unregistered project files. These fragments are not
# hand-authored project code at all: each one is mechanically generated,
# in full, by Qt's `moc` tool directly from an already
# FILE_SET-registered, already independently AST-scanned Q_OBJECT/
# Q_GADGET header, and can only ever reference members that header
# itself already declares -- moc cannot invent a new public API surface
# of its own. This function registers a target's own AUTOGEN_BUILD_DIR
# (computed identically to arkham_write_target_source_manifest()'s own
# mocs_compilation.cpp path logic above) as a trusted, GENERATED root in
# the same manifest packaging/check_encoder_hygiene.py's
# `_external_roots()` reads for genuinely-external FetchContent
# dependencies (see the `generated/external_roots.txt` writer in
# CMakeLists.txt) -- appended via file(APPEND ...), never overwriting
# the FetchContent-derived lines already written there. This is real,
# CMake-derived target metadata (AUTOMOC/AUTOGEN_BUILD_DIR properties),
# never a lexical "build directory" guess, so it stays correct
# automatically if CMake's own AUTOGEN_BUILD_DIR default ever changes or
# is explicitly overridden for either target.
#
# Arguments:
#   TARGET       The target whose AUTOGEN_BUILD_DIR (if AUTOMOC-enabled)
#                is registered.
#   OUTPUT_FILE  Absolute path of the (already-existing) external-roots
#                manifest file to append to.
function(arkham_append_target_autogen_root)
    set(oneValueArgs TARGET OUTPUT_FILE)
    cmake_parse_arguments(ARG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "arkham_append_target_autogen_root: TARGET is required")
    endif()
    if(NOT ARG_OUTPUT_FILE)
        message(FATAL_ERROR "arkham_append_target_autogen_root: OUTPUT_FILE is required")
    endif()
    if(NOT TARGET ${ARG_TARGET})
        message(FATAL_ERROR "arkham_append_target_autogen_root: no such target '${ARG_TARGET}'")
    endif()

    get_target_property(_arkham_tar_automoc ${ARG_TARGET} AUTOMOC)
    if(_arkham_tar_automoc)
        get_target_property(_arkham_tar_autogen_dir ${ARG_TARGET} AUTOGEN_BUILD_DIR)
        if(NOT _arkham_tar_autogen_dir)
            get_target_property(_arkham_tar_binary_dir ${ARG_TARGET} BINARY_DIR)
            set(_arkham_tar_autogen_dir "${_arkham_tar_binary_dir}/${ARG_TARGET}_autogen")
            unset(_arkham_tar_binary_dir)
        endif()
        file(APPEND "${ARG_OUTPUT_FILE}" "${_arkham_tar_autogen_dir}\n")
        unset(_arkham_tar_autogen_dir)
    endif()
    unset(_arkham_tar_automoc)
endfunction()
