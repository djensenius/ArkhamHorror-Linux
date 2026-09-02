#!/usr/bin/env bash
# Hermetic regression test for
# packaging/tests/avif_bundled_roundtrip_smoke.c's own encode-then-decode
# round trip logic, compiled and run directly against whatever libavif is
# available on this machine (no AppImage/container involved -- that
# end-to-end proof, run against the real packaged closure, lives in
# .github/workflows/ci.yml's "Verify the bundled libavif decodes AVIF
# images with no host codec fallback" step). This script only proves the
# smoke program itself is a correct, deterministic encode/decode round
# trip; it is not a substitute for that container-based no-host-fallback
# proof.
set -euo pipefail

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
  echo "SKIP: no C compiler (cc/gcc) available to build the AVIF smoke program."
  exit 0
fi
cc_bin="$(command -v cc || command -v gcc)"

if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists libavif; then
  echo "SKIP: pkg-config libavif not available on this machine."
  exit 0
fi

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

# shellcheck disable=SC2046
"$cc_bin" "$repo_root/packaging/tests/avif_bundled_roundtrip_smoke.c" \
  -o "$scratch/avif_bundled_roundtrip_smoke" \
  $(pkg-config --cflags --libs libavif)

output="$("$scratch/avif_bundled_roundtrip_smoke")"
echo "$output"
[[ "$output" == *"Encoded and decoded a 4x4 AVIF image"* ]] || {
  echo "Unexpected output from avif_bundled_roundtrip_smoke:" >&2
  echo "$output" >&2
  exit 1
}
echo "PASS: AVIF bundled round-trip smoke program encodes and decodes correctly."
