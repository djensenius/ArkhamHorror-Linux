# shellcheck shell=bash
#
# find_bundled_library <name-glob> [search_root...]
#
# Prints the resolved path to a system shared library matching name-glob
# (e.g. 'libsecret-1.so.0*') to stdout (empty output if none is found).
# Used by build-appimage.sh to locate libraries that must be force-bundled
# into the AppImage via linuxdeploy --library because they are either
# dlopen()-only (never an ELF DT_NEEDED dependency linuxdeploy's automatic
# ldd-based bundling would follow, e.g. libsecret-1) or a transitive
# dependency linuxdeploy's own blacklist excludes from automatic bundling
# by default (e.g. libgpg-error, required by bundled libgcrypt). Never
# aborts the caller's script under `set -euo pipefail`, even when
# `ldconfig` is missing/erroring or the `find` fallback matches nothing:
#
#   - `ldconfig -p` is only invoked when `ldconfig` exists on PATH, and its
#     output is captured on its own line before being parsed with `awk` as
#     a second, independent step. Evaluating "ldconfig -p | awk ..." as a
#     single pipeline would, under pipefail, take ldconfig's (non-zero)
#     exit status as the pipeline's status whenever ldconfig itself fails
#     or is a stub/wrapper that still lets awk exit 0 -- which would abort
#     the whole packaging script under `set -e`, before this function's
#     `find`-based fallback ever ran.
#   - The `find` fallback uses `-print -quit` (a GNU find extension) so
#     find itself stops after the first match, rather than piping into
#     `head -n1`: once `head` has its first line it exits, and a
#     still-writing `find` on the other end of that pipe would receive
#     SIGPIPE and terminate with a non-zero/signalled status -- which,
#     again under pipefail, would abort the script even though `head`
#     already captured the fallback's correct answer.
#   - Both stages are additionally suffixed with `|| true` so an
#     unrelated, non-fatal error (e.g. a permission-denied subdirectory
#     under `find`) is treated as "no match from this stage", not a fatal
#     script error. The caller is still responsible for treating a
#     completely empty result as fatal -- this function only guarantees it
#     will not raise `set -e` itself; it does not weaken the caller's own
#     "found nothing at all" validation.
#
# search_root defaults to the real system library directories `find`
# would search on a Debian/Ubuntu-based AppImage builder; tests pass a
# fake temporary root instead so this function is exercised without
# touching real host paths.
find_bundled_library() {
  local name_glob="$1"
  shift
  local search_roots=("$@")
  if [[ ${#search_roots[@]} -eq 0 ]]; then
    search_roots=(/usr/lib /usr/lib/x86_64-linux-gnu /lib)
  fi

  # Every caller's glob ends in '*' (e.g. 'libsecret-1.so.0*', to also match
  # versioned filenames like libsecret-1.so.0.0.0). Strip that trailing '*'
  # to get the fixed prefix ldconfig -p's first column (the SONAME/filename)
  # must start with; ldconfig -p only ever lists concrete filenames, so a
  # plain prefix check is both correct and simpler/safer here than trying to
  # translate a shell glob into an awk/POSIX regex (in which a literal '.'
  # and a bare trailing '*' mean something different from glob syntax).
  local name_prefix="${name_glob%\*}"

  local candidate=""

  if command -v ldconfig >/dev/null 2>&1; then
    local ldconfig_output
    ldconfig_output="$(ldconfig -p 2>/dev/null || true)"
    candidate="$(printf '%s\n' "$ldconfig_output" \
      | awk -v prefix="$name_prefix" 'index($1, prefix) == 1 {print $NF; exit}' || true)"
  fi

  if [[ -z "$candidate" ]]; then
    candidate="$(find "${search_roots[@]}" -maxdepth 3 \
      -name "$name_glob" -print -quit 2>/dev/null || true)"
  fi

  printf '%s' "$candidate"
}

# find_bundled_libsecret [search_root...]
#
# Thin wrapper over find_bundled_library for libsecret-1.so.0 specifically
# (kept as its own named function for existing callers/tests).
find_bundled_libsecret() {
  find_bundled_library 'libsecret-1.so.0*' "$@"
}

# find_bundled_libgpgerror [search_root...]
#
# Thin wrapper over find_bundled_library for libgpg-error.so.0. QtKeychain's
# Secret Service/libsecret backend transitively depends on libgcrypt, which
# depends on libgpg-error -- but linuxdeploy's own default blacklist
# excludes libgpg-error from automatic bundling (it is treated as a "core"
# system library on the assumption a suitable copy is always present on the
# target host), which is false for a portable AppImage that must run on
# arbitrary distros. Force-bundling it the same way as libsecret closes
# that gap.
find_bundled_libgpgerror() {
  find_bundled_library 'libgpg-error.so.0*' "$@"
}

# find_bundled_libgccs [search_root...]
#
# Thin wrapper over find_bundled_library for libgcc_s.so.1. Like
# libgpg-error above, linuxdeploy's own default blacklist excludes
# libgcc_s (and libstdc++, below) from automatic bundling on the
# assumption a compatible copy is always already present on the target
# host -- but libgcc_s/libstdc++'s C++ ABI is not guaranteed compatible
# across distros/ages the way glibc's C ABI is (this is a well-known
# class of AppImage portability bug: an AppImage built against a newer
# GCC failing on an older host with "version `GCC_x.y` not found" or
# similar), which is exactly the failure mode a portable AppImage
# targeting arbitrary/older host distros (including SteamOS) must avoid.
# Force-bundling it here, rather than relying on ABI_ALLOWLIST to excuse
# it, keeps the recursive closure audit's own allowlist narrowly limited
# to the dynamic loader and true core-glibc libraries only.
find_bundled_libgccs() {
  find_bundled_library 'libgcc_s.so.1*' "$@"
}

# find_bundled_libstdcxx [search_root...]
#
# Thin wrapper over find_bundled_library for libstdc++.so.6. See
# find_bundled_libgccs() above for why this must be force-bundled rather
# than allowlisted: C++ standard library ABI compatibility across distros
# is not guaranteed the same way glibc's C ABI is.
find_bundled_libstdcxx() {
  find_bundled_library 'libstdc++.so.6*' "$@"
}

# find_bundled_libz [search_root...]
#
# Thin wrapper over find_bundled_library for libz.so.1 (zlib). Required
# transitively by both bundled Qt (libQt6Core.so.6) and bundled libsecret's
# own closure (libgio-2.0.so.0), but -- like libgcc_s/libstdc++ above --
# excluded from linuxdeploy's automatic bundling by its own default
# blacklist, again on the (for a portable AppImage, incorrect) assumption
# that a compatible system copy is always already present.
find_bundled_libz() {
  find_bundled_library 'libz.so.1*' "$@"
}
