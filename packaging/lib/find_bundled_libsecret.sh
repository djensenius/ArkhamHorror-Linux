# shellcheck shell=bash
#
# find_bundled_libsecret [search_root...]
#
# Prints the resolved path to a system libsecret-1.so.0 to stdout (empty
# output if none is found), for build-appimage.sh to bundle into the
# AppImage via linuxdeploy --library. Never aborts the caller's script
# under `set -euo pipefail`, even when `ldconfig` is missing/erroring or
# the `find` fallback matches nothing:
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
find_bundled_libsecret() {
  local search_roots=("$@")
  if [[ ${#search_roots[@]} -eq 0 ]]; then
    search_roots=(/usr/lib /usr/lib/x86_64-linux-gnu /lib)
  fi

  local candidate=""

  if command -v ldconfig >/dev/null 2>&1; then
    local ldconfig_output
    ldconfig_output="$(ldconfig -p 2>/dev/null || true)"
    candidate="$(printf '%s\n' "$ldconfig_output" \
      | awk '/libsecret-1\.so\.0/ {print $NF; exit}' || true)"
  fi

  if [[ -z "$candidate" ]]; then
    candidate="$(find "${search_roots[@]}" -maxdepth 3 \
      -name 'libsecret-1.so.0*' -print -quit 2>/dev/null || true)"
  fi

  printf '%s' "$candidate"
}
