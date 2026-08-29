#!/usr/bin/env python3
"""Deterministically regenerate src/AssetLocaleDigestData.generated.h from
contracts/asset-locale-digest.json.

This script is the single source of truth for turning the pinned,
human-reviewed JSON digest (see that file's own "provenance" note for what
it currently covers and why) into the compact C++ lookup table
AssetLocaleDigest.cpp consults at runtime. It is intentionally *generated*
data, kept in its own file separate from handwritten sources, per
djensenius/ArkhamHorror-Linux#17's requirement to keep generated lookup
output identifiable and separate from the handwritten diff.

Usage:
    tools/generate_asset_locale_digest.py            # regenerate in place
    tools/generate_asset_locale_digest.py --check     # exit 1 if stale

tests/AssetLocaleDigestTests.cpp performs an independent, C++-only drift
check at test time (it recomputes the SHA-256 of the checked-in JSON and
compares it against the hash embedded in the generated header by this
script), so CI catches drift even without invoking Python -- this script's
--check mode is a convenience for local development, not what CI relies on.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_JSON = REPO_ROOT / "contracts" / "asset-locale-digest.json"
GENERATED_HEADER = REPO_ROOT / "src" / "AssetLocaleDigestData.generated.h"

_VALID_CATEGORIES = {
    "card",
    "investigator_portrait",
    "chaos_token",
    "set_icon",
    "campaign_box",
    "slot_icon",
    "homebrew_card",
    "homebrew_set",
    "homebrew_box",
}
_VALID_SIDES = {"front", "back", "alternate_front", "resolved_front", "mutated_front"}


def _escape(value: str) -> str:
    # Escape backslashes and quotes first (order matters: backslash must
    # be escaped before the sequences that introduce a backslash for the
    # other control characters below), then every other control
    # character (any codepoint below U+0020, plus DEL/U+007F) as a
    # fixed-width 3-digit octal escape. Without this, a pinned JSON value
    # that ever contains a stray control character -- even one this
    # digest's data is not expected to contain, such as a form feed or a
    # raw NUL -- would silently produce an invalid C++ string literal (or
    # embed a literal control byte) in the generated header.
    #
    # Octal (not hex) is used deliberately: a C++ "\xHH" escape has NO
    # fixed width and greedily consumes every following hex digit
    # character (0-9a-fA-F) in the same string literal, so "\x01" next to
    # an ordinary hex-looking character like "a" or "3" would silently
    # become part of the escape and corrupt the following character. A
    # "\ooo" octal escape is capped at exactly 3 digits by the C++ grammar,
    # so emitting exactly 3 digits every time is always unambiguous
    # regardless of what character follows.
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    named = {
        "\n": "\\n",
        "\r": "\\r",
        "\t": "\\t",
        "\b": "\\b",
        "\f": "\\f",
        "\v": "\\v",
    }
    result = []
    for ch in escaped:
        codepoint = ord(ch)
        if ch in named:
            result.append(named[ch])
        elif codepoint < 0x20 or codepoint == 0x7F:
            result.append(f"\\{codepoint:03o}")
        else:
            result.append(ch)
    return "".join(result)


def render_header(source_bytes: bytes, data: dict) -> str:
    digest_hex = hashlib.sha256(source_bytes).hexdigest()

    locale_map = data["localeMap"]
    entries = data["entries"]

    for locale, mapped in locale_map.items():
        if not locale or not mapped:
            raise ValueError(f"empty locale mapping entry: {locale!r} -> {mapped!r}")

    for entry in entries:
        if entry["category"] not in _VALID_CATEGORIES:
            raise ValueError(f"unknown category in digest entry: {entry!r}")
        if entry["side"] not in _VALID_SIDES:
            raise ValueError(f"unknown side in digest entry: {entry!r}")
        if not entry["locale"] or not entry["identifier"]:
            raise ValueError(f"empty locale/identifier in digest entry: {entry!r}")

    lines = []
    lines.append("// GENERATED FILE -- DO NOT EDIT BY HAND.")
    lines.append("//")
    lines.append(
        "// Produced by tools/generate_asset_locale_digest.py from "
        "contracts/asset-locale-digest.json."
    )
    lines.append(
        "// Re-run that script after editing the JSON source; "
        "tests/AssetLocaleDigestTests.cpp fails the build if this file "
        "drifts from the JSON source's SHA-256."
    )
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace Arkham::AssetLocaleDigestData {")
    lines.append("")
    lines.append(
        f'inline constexpr char kSourceJsonSha256[] = "{digest_hex}";'
    )
    lines.append("")
    lines.append("struct LocaleMapEntry {")
    lines.append("  const char *isoLocale;")
    lines.append("  const char *webLocale;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr LocaleMapEntry kLocaleMap[] = {")
    for locale, mapped in locale_map.items():
        lines.append(f'    {{"{_escape(locale)}", "{_escape(mapped)}"}},')
    lines.append("};")
    lines.append("")
    lines.append("struct DigestEntry {")
    lines.append("  const char *webLocale;")
    lines.append("  const char *category;")
    lines.append("  const char *identifier;")
    lines.append("  const char *side;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr DigestEntry kEntries[] = {")
    for entry in entries:
        lines.append(
            "    {"
            f'"{_escape(entry["locale"])}", '
            f'"{_escape(entry["category"])}", '
            f'"{_escape(entry["identifier"])}", '
            f'"{_escape(entry["side"])}"'
            "},"
        )
    lines.append("};")
    lines.append("")
    lines.append("} // namespace Arkham::AssetLocaleDigestData")
    lines.append("")
    return "\n".join(lines)


def _clang_format(rendered: str) -> str:
    """Runs the rendered header text through clang-format so this script's
    own output is byte-identical to what `mise run format:check`
    validates against src/*.h -- without this, every fresh regeneration
    would need a manual `clang-format -i` pass, and --check would keep
    reporting the checked-in (already clang-formatted) header as "stale"
    purely due to line-wrapping differences, never actual data drift.
    Falls back to the unformatted text (rather than failing outright) if
    clang-format isn't on PATH, since --check's real drift signal is the
    embedded SHA-256, and CI's separate format:check job independently
    enforces formatting regardless.
    """
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        return rendered
    try:
        result = subprocess.run(
            [clang_format, "--assume-filename=AssetLocaleDigestData.generated.h"],
            input=rendered,
            capture_output=True,
            text=True,
            check=True,
        )
    except (subprocess.CalledProcessError, OSError):
        # clang-format was found on PATH but failed to run to completion
        # (a broken install, missing shared libs, a crash, etc.) -- honour
        # the same fallback-to-unformatted-output promise as the "not on
        # PATH at all" case above rather than letting the script abort.
        return rendered
    return result.stdout


def _extract_embedded_sha256(header_text: str) -> str | None:
    """Extracts the kSourceJsonSha256 hex string embedded in an already
    generated header, or None if the header is missing/malformed. Used by
    --check to validate drift by hash alone when clang-format isn't
    available to make a full-text comparison meaningful (see
    _clang_format's docstring): a checked-in header need only be
    formatting-different, not data-different, to make a raw text compare
    report a false positive.
    """
    match = re.search(
        r'kSourceJsonSha256\[\]\s*=\s*"([0-9a-f]{64})"', header_text
    )
    return match.group(1) if match else None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if the generated header is stale instead of "
        "rewriting it.",
    )
    args = parser.parse_args(argv)

    source_bytes = SOURCE_JSON.read_bytes()
    data = json.loads(source_bytes)
    expected_sha256 = hashlib.sha256(source_bytes).hexdigest()
    clang_format_available = shutil.which("clang-format") is not None
    rendered = _clang_format(render_header(source_bytes, data))

    if args.check:
        current = (
            GENERATED_HEADER.read_text(encoding="utf-8")
            if GENERATED_HEADER.exists()
            else ""
        )
        if clang_format_available:
            # The full-text comparison is meaningful here: `rendered` was
            # itself just clang-formatted above, so any difference from
            # `current` (which is expected to already be clang-formatted,
            # being checked in) reflects real drift -- either data drift
            # or a stale formatting pass -- not merely this run's
            # clang-format availability.
            is_stale = current != rendered
        else:
            # Without clang-format, `rendered`'s formatting cannot be
            # trusted to match the checked-in header's -- comparing full
            # text here would report the header as stale purely because
            # this particular run lacks clang-format, even when the data
            # itself (the actual drift signal) is unchanged. Fall back to
            # comparing only the embedded SHA-256 of the JSON source
            # against a fresh hash of it: that is the same drift check
            # tests/AssetLocaleDigestTests.cpp performs independently in
            # C++, and is unaffected by formatting-tool availability.
            embedded = _extract_embedded_sha256(current)
            is_stale = embedded != expected_sha256
        if is_stale:
            print(
                f"{GENERATED_HEADER} is stale; run "
                "tools/generate_asset_locale_digest.py to regenerate it.",
                file=sys.stderr,
            )
            return 1
        print("Generated asset locale digest header is up to date.")
        return 0

    GENERATED_HEADER.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"Wrote {GENERATED_HEADER}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
