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
    return value.replace("\\", "\\\\").replace('"', '\\"')


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
    rendered = render_header(source_bytes, data)

    if args.check:
        current = (
            GENERATED_HEADER.read_text(encoding="utf-8")
            if GENERATED_HEADER.exists()
            else ""
        )
        if current != rendered:
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
