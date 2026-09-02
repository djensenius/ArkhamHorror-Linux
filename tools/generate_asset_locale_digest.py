#!/usr/bin/env python3
"""Deterministically regenerate src/AssetLocaleDigestData.generated.h from
contracts/asset-locale-digest.json (a manifest: locale map, category-root
mapping, and pinned provenance for the five per-locale source files in
contracts/asset-locale-digest-sources/) plus those source files themselves.

Each contracts/asset-locale-digest-sources/<locale>.json file is an exact,
byte-for-byte copy of the real web client's own digest
(frontend/src/digests/<locale>.json at the pinned sourceCommit in the
manifest -- see that manifest's "provenance" note) -- a plain JSON array
of asset-relative path strings such as "cards/01001b.avif". This script:

  1. Verifies every source file's SHA-256 against the manifest's pinned
     hash (catching an accidental or malicious edit to a "pinned" file
     immediately, rather than silently regenerating stale/tampered data).
  2. Verifies the manifest's locale set is EXACTLY the five locales this
     issue's provenance covers (it/fr/es/ko/zh) -- no fewer, no more --
     and that the sourceFiles/localeMap/on-disk source-file set all agree
     with each other and with the source directory's actual contents (no
     unregistered extra source file, no missing declared one).
  3. Independently parses and normalises every path in every source file:
     splits "<root>/<artCode>.<ext>", maps <root> to a modeled
     AssetCategory via acceptedCategoryRoots (or verifies it is an
     explicitly-documented, deliberately-ignored root such Tarot card
     backs, which are not a modeled category at all), validates <ext>
     against that category's one fixed canonical extension, validates
     <artCode> against a strict allow-list grammar, and rejects
     duplicates. This never trusts the upstream data's shape blindly: an
     unmapped root that is not on the ignore-list, or any path that fails
     validation, is a hard generator error (data corruption or an
     unannounced upstream schema change), not a silent skip.

Usage:
    tools/generate_asset_locale_digest.py            # regenerate in place
    tools/generate_asset_locale_digest.py --check     # exit 1 if stale

tests/AssetLocaleDigestTests.cpp performs an independent, C++-only drift
check at test time (it recomputes the SHA-256 of every checked-in source
file and of the manifest itself, and compares them against the hashes
embedded in the generated header by this script), so CI catches drift even
without invoking Python -- this script's --check mode is a convenience for
local development, not what CI relies on.
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
CONTRACTS_DIR = REPO_ROOT / "contracts"
MANIFEST_JSON = CONTRACTS_DIR / "asset-locale-digest.json"
SOURCES_DIR = CONTRACTS_DIR / "asset-locale-digest-sources"
GENERATED_HEADER = REPO_ROOT / "src" / "AssetLocaleDigestData.generated.h"

# The exact, closed set of locales this issue's provenance covers. Neither
# more nor fewer: a manifest/source-directory mismatch against this set is
# a hard error, so an accidental extra or missing locale can never be
# silently regenerated into (or dropped from) the shipped digest.
_EXPECTED_LOCALES = frozenset({"ita", "fr", "es", "ko", "zh"})

# Round-4 review item 11: the exact, fixed ISO-locale -> web-locale map
# this issue's provenance covers (djensenius/ArkhamHorror-Linux#17 issue
# body: "Preserve the web locale mapping it -> ita, fr -> fr, es -> es,
# ko -> ko, zh -> zh."). The manifest's localeMap must equal this dict
# EXACTLY -- both keys and values, with no extra entry and no alias --
# not merely have a matching VALUE set (the older check below only
# compared `set(locale_map.values())`, which could not detect an extra
# ISO-locale KEY mapping to an otherwise-legitimate value, e.g. a
# malicious/accidental "it-IT": "ita" alias alongside the real "it": "ita"
# entry, or a key containing a path-traversal-shaped string).
_EXPECTED_LOCALE_MAP = {
    "it": "ita",
    "fr": "fr",
    "es": "es",
    "ko": "ko",
    "zh": "zh",
}

# Round-4 review item 11: every pinned source file's manifest-declared
# `path` must be EXACTLY this fixed, normalized, relative-to-contracts/
# path for its own locale -- never merely "some path that happens to
# exist" or "a path validated only by regex". This is checked with a
# literal equality comparison (not just "no '..' components"), so no
# path-normalization edge case (backslashes, a leading slash, a
# doubled/trailing slash, a differently-cased directory segment, a
# symlink component, etc.) can smuggle a source file's declared identity
# outside contracts/asset-locale-digest-sources/ or rename which locale
# it is read as.

# Strict allow-list grammar for a validated path's art-code segment
# (everything between the root and the extension): ASCII letters (either
# case -- the real upstream data genuinely contains uppercase variant
# suffixes, e.g. "cards/04242B.avif"), digits, '-', and '_'. This
# deliberately rejects '.', '/', and any other punctuation or control
# character outright.
_ART_CODE_RE = re.compile(r"^[0-9A-Za-z_-]+$")
_PATH_RE = re.compile(r"^([a-z]+)/([^/]+)\.([A-Za-z0-9]+)$")

# Canonical extension per modeled category token, mirroring
# AssetLocator::canonicalFormatFor()'s per-category fixed format -- a
# digest entry whose extension does not match its category's real
# extension indicates either a parsing bug in this script or a genuine
# upstream schema change, either of which must fail loudly rather than
# silently accepting the wrong extension for that category.
_CATEGORY_EXTENSION = {
    "card": "avif",
}


def _escape(value: str) -> str:
    # Escape backslashes and quotes first (order matters: backslash must
    # be escaped before the sequences that introduce a backslash for the
    # other control characters below), then every other control
    # character (any codepoint below U+0020, plus DEL/U+007F) as a
    # fixed-width 3-digit octal escape. Without this, a pinned source
    # value that ever contains a stray control character would silently
    # produce an invalid C++ string literal (or embed a literal control
    # byte) in the generated header.
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


def _load_manifest() -> dict:
    manifest = json.loads(MANIFEST_JSON.read_bytes())

    locale_map: dict[str, str] = manifest["localeMap"]
    # Round-4 review item 11: exact dict equality (keys AND values), not
    # merely a matching value set -- see _EXPECTED_LOCALE_MAP's comment.
    if locale_map != _EXPECTED_LOCALE_MAP:
        raise ValueError(
            "contracts/asset-locale-digest.json's localeMap "
            f"{locale_map!r} does not exactly match the fixed, pinned "
            f"locale map {_EXPECTED_LOCALE_MAP!r} required by "
            "djensenius/ArkhamHorror-Linux#17 -- no extra key/alias or "
            "differing value is permitted"
        )

    source_files: dict[str, dict] = manifest["provenance"]["sourceFiles"]
    if set(source_files.keys()) != _EXPECTED_LOCALES:
        raise ValueError(
            "contracts/asset-locale-digest.json's provenance.sourceFiles "
            f"key set {sorted(source_files.keys())!r} does not match the "
            f"exact expected set {sorted(_EXPECTED_LOCALES)!r}"
        )

    # Round-4 review item 11: each declared source path must be EXACTLY
    # the fixed "asset-locale-digest-sources/<locale>.json" this locale is
    # pinned to -- literal string equality, not merely "resolves inside
    # the directory" or "matches a regex" -- so a manifest cannot smuggle
    # a source file's declared identity to a different path (absolute,
    # containing "..", using a different locale's filename, backslashes,
    # a doubled/leading/trailing slash, etc.) while still passing a looser
    # containment check.
    for web_locale, entry in source_files.items():
        expected_path = f"asset-locale-digest-sources/{web_locale}.json"
        declared_path = entry.get("path")
        if declared_path != expected_path:
            raise ValueError(
                f"contracts/asset-locale-digest.json's provenance."
                f"sourceFiles[{web_locale!r}].path is {declared_path!r}, "
                f"but must be exactly {expected_path!r}"
            )

    on_disk = {p.stem for p in SOURCES_DIR.glob("*.json")}
    if on_disk != _EXPECTED_LOCALES:
        raise ValueError(
            f"{SOURCES_DIR} contains {sorted(on_disk)!r}, which does not "
            f"match the exact expected locale set "
            f"{sorted(_EXPECTED_LOCALES)!r} -- an unregistered source "
            "file was added or a declared one is missing"
        )

    return manifest


def _load_and_validate_source(
    web_locale: str,
    accepted_roots: dict[str, str],
    ignored_roots: set[str],
    source_files: dict[str, dict],
) -> list[tuple[str, str]]:
    """Returns a validated, deduplicated list of (category, artCode) pairs
    for `web_locale`, having verified the source file's pinned SHA-256 and
    every path's shape/grammar/extension first.
    """
    entry = source_files[web_locale]
    path = REPO_ROOT / "contracts" / entry["path"]
    raw_bytes = path.read_bytes()
    actual_sha256 = hashlib.sha256(raw_bytes).hexdigest()
    if actual_sha256 != entry["sha256"]:
        raise ValueError(
            f"{path} has SHA-256 {actual_sha256}, but the manifest pins "
            f"{entry['sha256']!r} -- this pinned source file must not be "
            "edited without updating its recorded hash (and re-verifying "
            "provenance against the real upstream commit)"
        )

    raw_paths = json.loads(raw_bytes)
    if not isinstance(raw_paths, list):
        raise ValueError(f"{path} must contain a JSON array of path strings")

    seen: set[str] = set()
    result: list[tuple[str, str]] = []
    for raw_path in raw_paths:
        if not isinstance(raw_path, str) or not raw_path:
            raise ValueError(f"{path}: non-string or empty path entry: {raw_path!r}")
        if raw_path in seen:
            raise ValueError(f"{path}: duplicate path entry: {raw_path!r}")
        seen.add(raw_path)

        match = _PATH_RE.match(raw_path)
        if not match:
            raise ValueError(f"{path}: path does not match expected shape: {raw_path!r}")
        root, art_code, ext = match.groups()

        if root in ignored_roots:
            continue
        category = accepted_roots.get(root)
        if category is None:
            raise ValueError(
                f"{path}: unmapped, non-ignored category root {root!r} in "
                f"path {raw_path!r} -- add it to acceptedCategoryRoots or "
                "ignoredCategoryRoots in contracts/asset-locale-digest.json "
                "as a conscious decision, do not silently drop it"
            )

        expected_ext = _CATEGORY_EXTENSION[category]
        if ext.lower() != expected_ext:
            raise ValueError(
                f"{path}: category {category!r} must use extension "
                f"{expected_ext!r}, got {ext!r} in path {raw_path!r}"
            )

        if not _ART_CODE_RE.match(art_code):
            raise ValueError(
                f"{path}: art code {art_code!r} in path {raw_path!r} fails "
                "the strict [0-9A-Za-z_-]+ grammar"
            )

        result.append((category, art_code))

    return result


def render_header(manifest: dict) -> str:
    locale_map: dict[str, str] = manifest["localeMap"]
    accepted_roots: dict[str, str] = manifest["acceptedCategoryRoots"]
    ignored_roots = set(manifest.get("ignoredCategoryRoots", {}).keys())
    source_files: dict[str, dict] = manifest["provenance"]["sourceFiles"]

    manifest_sha256 = hashlib.sha256(MANIFEST_JSON.read_bytes()).hexdigest()

    per_locale_entries: dict[str, list[tuple[str, str]]] = {}
    source_sha256: dict[str, str] = {}
    for web_locale in sorted(_EXPECTED_LOCALES):
        per_locale_entries[web_locale] = _load_and_validate_source(
            web_locale, accepted_roots, ignored_roots, source_files
        )
        source_sha256[web_locale] = source_files[web_locale]["sha256"]

    lines = []
    lines.append("// GENERATED FILE -- DO NOT EDIT BY HAND.")
    lines.append("//")
    lines.append(
        "// Produced by tools/generate_asset_locale_digest.py from "
        "contracts/asset-locale-digest.json and the pinned per-locale "
        "source files in contracts/asset-locale-digest-sources/."
    )
    lines.append(
        "// Re-run that script after editing either the manifest or a "
        "pinned source file; tests/AssetLocaleDigestTests.cpp fails the "
        "build if this file drifts from those sources' SHA-256 hashes."
    )
    lines.append("#pragma once")
    lines.append("")
    lines.append("namespace Arkham::AssetLocaleDigestData {")
    lines.append("")
    lines.append(
        f'inline constexpr char kManifestJsonSha256[] = "{manifest_sha256}";'
    )
    lines.append("")
    lines.append("struct SourceFileHash {")
    lines.append("  const char *webLocale;")
    lines.append("  const char *sha256;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr SourceFileHash kSourceFileHashes[] = {")
    for web_locale in sorted(_EXPECTED_LOCALES):
        lines.append(
            f'    {{"{_escape(web_locale)}", '
            f'"{_escape(source_sha256[web_locale])}"}},'
        )
    lines.append("};")
    lines.append("")
    lines.append("struct LocaleMapEntry {")
    lines.append("  const char *isoLocale;")
    lines.append("  const char *webLocale;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr LocaleMapEntry kLocaleMap[] = {")
    # Sort by ISO locale key rather than relying on dict/JSON insertion
    # order: a semantically-equivalent reordering of localeMap in the
    # manifest (which JSON/dict iteration preserves as-is) would
    # otherwise produce a different generated header for identical
    # data, breaking the determinism this generator is required to
    # guarantee.
    for locale in sorted(locale_map):
        mapped = locale_map[locale]
        lines.append(f'    {{"{_escape(locale)}", "{_escape(mapped)}"}},')
    lines.append("};")
    lines.append("")
    # DigestEntry is keyed by the exact, fully-resolved art code (the
    # SAME string AssetLocator::resolveArtCodeForSide() computes for a
    # given identifier+side) rather than a separately decomposed
    # (identifier, side) pair. The raw upstream data is a flat list of
    # already-resolved final path segments (e.g. "01001b.avif",
    # "01514_Mutated19.avif") with no side annotation of its own, and a
    # generic reverse-parse from art code back to (identifier, side)
    # would be genuinely ambiguous (e.g. a trailing "b" could mean
    # AssetSide::Back OR the generic ResolvedFront rule's output) --
    # keying by the resolved art code string sidesteps that ambiguity
    # entirely, matching upstream's own model exactly.
    lines.append("struct DigestEntry {")
    lines.append("  const char *webLocale;")
    lines.append("  const char *category;")
    lines.append("  const char *artCode;")
    lines.append("};")
    lines.append("")
    lines.append("inline constexpr DigestEntry kEntries[] = {")
    for web_locale in sorted(_EXPECTED_LOCALES):
        for category, art_code in per_locale_entries[web_locale]:
            lines.append(
                "    {"
                f'"{_escape(web_locale)}", '
                f'"{_escape(category)}", '
                f'"{_escape(art_code)}"'
                "},"
            )
    lines.append("};")
    lines.append("")
    lines.append("} // namespace Arkham::AssetLocaleDigestData")
    lines.append("")
    return "\n".join(lines)


def _clang_format(rendered: str) -> tuple[str, bool]:
    """Runs the rendered header text through clang-format so this script's
    own output is byte-identical to what `mise run format:check`
    validates against src/*.h -- without this, every fresh regeneration
    would need a manual `clang-format -i` pass, and --check would keep
    reporting the checked-in (already clang-formatted) header as "stale"
    purely due to line-wrapping differences, never actual data drift.
    Falls back to returning the unformatted text (rather than raising)
    if clang-format isn't on PATH -- or is on PATH but fails to run to
    completion (a broken install, missing shared libs, a crash, etc.):
    plain (non---check) generation mode still writes SOME output rather
    than aborting outright, and CI's separate format:check job
    independently enforces formatting regardless. --check itself, on the
    other hand, now fails closed whenever `formatted_ok` comes back False
    (see main()) rather than trusting a weaker comparison that cannot be
    made meaningful without a real formatter run -- see review round-3
    item 16.

    Returns (text, formatted_ok): `formatted_ok` is True only when
    clang-format actually ran to completion and its output is being
    returned -- NOT merely when the binary was found on PATH. Callers
    that need to know whether `text` is trustworthy for a full-text
    staleness comparison must check `formatted_ok`, not
    `shutil.which("clang-format")`, since a present-but-broken install
    would otherwise silently make that comparison meaningless.
    """
    clang_format = shutil.which("clang-format")
    if clang_format is None:
        return rendered, False
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
        return rendered, False
    return result.stdout, True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if the generated header is stale instead of "
        "rewriting it.",
    )
    args = parser.parse_args(argv)

    manifest = _load_manifest()
    rendered, formatted_ok = _clang_format(render_header(manifest))

    if args.check:
        current = (
            GENERATED_HEADER.read_text(encoding="utf-8")
            if GENERATED_HEADER.exists()
            else ""
        )
        # Review round-3 item 16: a hash-only comparison when
        # clang-format is unavailable can be fooled by a HAND-EDITED
        # generated header whose embedded kManifestJsonSha256/
        # kSourceFileHashes comments were left untouched but whose actual
        # kEntries[] rows were mutated (e.g. a single locale's artCode
        # silently changed) -- those embedded hashes describe the INPUT
        # (the manifest + source files), never the OUTPUT rows this
        # script itself renders, so comparing them alone proves nothing
        # about whether the checked-in file's rows still match what
        # render_header() actually produces. Rather than trust that
        # weaker signal, --check now FAILS CLOSED whenever clang-format
        # did not actually run to completion (`formatted_ok` is False):
        # CI's format job and `mise run setup:macos` both always install
        # clang-format, so this path is never exercised there in
        # practice, and a local run missing it is directed to install it
        # rather than silently trusting a comparison that cannot detect
        # this exact class of tampering.
        if not formatted_ok:
            print(
                "clang-format is required for --check (it was not found on "
                "PATH, or was found but failed to run to completion) -- "
                "install it (see `mise run setup:macos`, or `apt-get "
                "install clang-format` as CI does) and re-run --check. A "
                "hash-only fallback comparison cannot detect a hand-edited "
                "generated header whose embedded hashes were left stale "
                "while its actual data rows were mutated, so this script "
                "no longer attempts one.",
                file=sys.stderr,
            )
            return 1
        is_stale = current != rendered
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
