#!/usr/bin/env python3
"""Recursively classify every bundled ELF shared library under an AppImage's
AppDir and require each to map to a known, audited, notice-bearing
third-party component (review round-4 item 12).

Previously, `packaging/lib/bundle_codec_notices.sh` and the corresponding CI
verification step in `.github/workflows/ci.yml` both worked "forwards": they
held a small handwritten table of exactly seven codec-library SONAME globs
(libavif and its six possible AV1 backends) and checked, for each one, "if
this specific name IS bundled, does it have a notice?". This can never catch
a library the table's author simply did not anticipate -- and the real
produced AppImage's `linuxdeploy`-resolved dependency closure turned out to
additionally include `libjpeg.so` (Qt's own `libqjpeg` plugin dependency) and
an entire family of `libabsl_*.so*` (Abseil) libraries, neither of which had
any notice at all.

This script instead works "backwards": it enumerates every `.so*` file
ACTUALLY present anywhere under the given AppDir root, and requires each one
to either
  1. be on the small, fixed ABI_ALLOWLIST of dynamic-loader/glibc-core
     libraries every glibc-based x86_64 Linux host is assumed to already
     provide (mirroring packaging/audit_dependency_closure.py's own
     allowlist; kept independently here so a change to one script's
     allowlist can never silently widen the other's), or
  2. match one of the ordered COMPONENT_PATTERNS regexes below, identifying
     which third_party/<component>/ notice directory documents it.

Any bundled library matching NEITHER is reported, by exact path, as
unmapped, and this script exits non-zero -- so adding a brand-new bundled
library this table has no entry for (whether by a future dependency change,
or a hostile/careless build-environment change) fails packaging loudly
rather than silently shipping an unattributed binary.

Modes:
  classify <lib_dir> [--json-out PATH]
      Prints "<component>\\t<path>" for every library requiring a notice
      (excluding ABI_ALLOWLIST-covered libraries, which need none). Exits 1
      (listing every unmapped library to stderr) if any bundled library
      cannot be classified. --json-out additionally writes a full
      machine-readable manifest/SBOM (every library's path, basename, and
      resolved component or allowlist status) to the given path.

  verify-notices <lib_dir> <third_party_root> <doc_root>
      Runs the same classification, then requires that for every distinct
      component actually found, every regular file present under
      <third_party_root>/<component>/ is also present, non-empty, and
      byte-for-byte identical (by sha256) under <doc_root>/<component>/ --
      i.e. that the AppImage's bundled notice content genuinely matches the
      checked-in source, not merely that some file with the same name
      exists. Intended to run against the REAL, extracted, final AppImage
      (after linuxdeploy + this repository's own notice-bundling step have
      both already run), not merely the pre-packaging AppDir, per review
      round-4 item 12's "verify content/checksum ... after final AppImage
      extraction, not before".

Usage:
    packaging/audit_codec_notices.py classify AppDir/usr/lib
    packaging/audit_codec_notices.py classify AppDir/usr/lib --json-out sbom.json
    packaging/audit_codec_notices.py verify-notices squashfs-root/usr/lib \\
        third_party squashfs-root/usr/share/doc/ArkhamHorror/third_party
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request
from pathlib import Path

# (SONAME) and build-id note patterns, in the same spirit as (and
# deliberately duplicated from, not imported from)
# audit_dependency_closure.py's own _NEEDED_RE/_RUNPATH_RE: each script
# keeps its own tiny, independently-reviewable readelf-output parser so a
# change to one script's regex can never silently affect the other's.
_SONAME_RE = re.compile(r"\(SONAME\)\s+Library soname:\s+\[(?P<name>[^\]]+)\]")
_BUILD_ID_RE = re.compile(r"Build ID:\s*(?P<id>[0-9a-fA-F]+)")

# Round-N+ review (HIGH, "canonical ELF identity misses load security/
# mapping ... e_entry ... pass"): parses `readelf -h`'s "Entry point
# address:" line -- the raw virtual address execution actually starts
# at. This is genuinely stable across a legitimate patchelf RUNPATH
# rewrite (the entry symbol itself, and the section containing it,
# never moves or is retargeted by that rewrite -- only `.dynstr`/
# `.dynamic`-adjacent metadata is ever relocated -- see
# _canonical_load_digest()'s own docstring), so silently omitting it
# left an attacker free to redirect execution to a completely different
# address (e.g. into attacker-controlled data treated as code) without
# changing anything else this digest previously authenticated.
_ENTRY_POINT_RE = re.compile(r"Entry point address:\s*(?P<address>0x[0-9a-fA-F]+)")

# Round-N+ review (HIGH, "canonical ELF identity misses ELF class/data/
# type/OSABI/machine"): parses the five identity-defining fields
# `readelf -h` reports before any program/section-header content at
# all -- EI_CLASS (32 vs 64-bit), EI_DATA (endianness), the object's
# own e_type (e.g. ET_DYN vs ET_EXEC), e_machine (target ISA -- an
# attacker substituting an entirely different-architecture object,
# e.g. swapping an x86-64 plugin for one that silently no-ops on
# load, or a malicious 32-bit shim, previously changed NONE of the
# section/segment-level digest fields below if the substitute
# coincidentally reused a similar section layout), and OS/ABI. None of
# these five fields is EVER rewritten by this project's own real
# packaging pipeline (a RUNPATH/RPATH edit or strip touches only
# section/segment content and the dynamic string table, never the
# fixed 16-byte e_ident/e_type/e_machine header prefix), so folding
# every one of them in unconditionally reopens no legitimate-repack
# false positive while closing a substitution class nothing else here
# could ever see.
_ELF_HEADER_CLASS_RE = re.compile(r"^\s*Class:\s*(?P<value>\S.*)$", re.MULTILINE)
_ELF_HEADER_DATA_RE = re.compile(r"^\s*Data:\s*(?P<value>\S.*)$", re.MULTILINE)
_ELF_HEADER_OSABI_RE = re.compile(r"^\s*OS/ABI:\s*(?P<value>\S.*)$", re.MULTILINE)
_ELF_HEADER_TYPE_RE = re.compile(r"^\s*Type:\s*(?P<value>\S.*)$", re.MULTILINE)
_ELF_HEADER_MACHINE_RE = re.compile(r"^\s*Machine:\s*(?P<value>\S.*)$", re.MULTILINE)

# Round-N+ review (HIGH, "Qt authenticity accepts matching build ID
# only; modified bytes can retain note. Build ID not signature."):
# parses `readelf -lW`'s one-line-per-entry program header format (see
# _canonical_load_digest()'s own docstring for the full rationale).
# The FLAGS column is a FIXED three-character field -- 'R' or ' ',
# then 'W' or ' ', then 'E' or ' ' -- never whitespace-collapsed by
# readelf itself, which is why this is matched as a literal
# three-character class rather than a naive whitespace-split (which
# would misalign every column after an entry with a missing permission
# bit, e.g. "R E" for read+execute-but-not-write).
_PROGRAM_HEADER_RE = re.compile(
    r"^\s*(?P<type>\S+)\s+"
    r"(?P<offset>0x[0-9a-fA-F]+)\s+"
    r"(?P<vaddr>0x[0-9a-fA-F]+)\s+"
    r"(?P<paddr>0x[0-9a-fA-F]+)\s+"
    r"(?P<filesz>0x[0-9a-fA-F]+)\s+"
    r"(?P<memsz>0x[0-9a-fA-F]+)\s+"
    r"(?P<flags>[R ][W ][E ])\s+"
    r"(?P<align>0x[0-9a-fA-F]+)\s*$",
    re.MULTILINE,
)

# Second-HIGH-round review ("Qt canonical digest hashes only PF_X
# PT_LOAD, missing behavior-bearing loaded data/program headers"):
# parses `readelf -SW`'s one-line-per-entry section header table, used
# by _canonical_load_digest() below to authenticate EVERY loaded
# (SHF_ALLOC) section's content and flags -- not merely the subset that
# happens to live in an executable (PF_X) PT_LOAD segment. The FLAGS
# column is a variable-length run of single-letter codes (e.g. "A",
# "AX", "WA", "AMS") with no fixed width, unlike the program-header
# table's FLAGS column, so it is matched as a greedy run of letters
# rather than a fixed-width class. The section [0] (always the
# reserved NULL entry) legitimately has an empty NAME and empty FLAGS,
# which this pattern tolerates (`\S*` / `[A-Za-z]*`, both zero-or-more).
_SECTION_HEADER_RE = re.compile(
    r"^\s*\[\s*\d+\]\s+"
    r"(?P<name>\S*)\s+"
    r"(?P<type>\S+)\s+"
    r"(?P<addr>[0-9a-fA-F]+)\s+"
    r"(?P<offset>[0-9a-fA-F]+)\s+"
    r"(?P<size>[0-9a-fA-F]+)\s+"
    r"(?P<entsize>[0-9a-fA-F]+)\s*"
    r"(?P<flags>[A-Za-z]*)\s+"
    r"(?P<link>\d+)\s+"
    r"(?P<info>\d+)\s+"
    r"(?P<align>\d+)\s*$",
    re.MULTILINE,
)

# Marks the start of readelf -lW's own "Section to Segment mapping"
# table, which lists -- in the SAME program-header index order as the
# "Program Headers" table above it in the same invocation's output --
# the whitespace-separated section names each program header physically
# contains. Used by _canonical_load_digest() to find, for each
# authenticated section, the FLAGS of whichever LOAD-type segment
# actually maps it into memory at runtime (the loader only ever
# consults program headers, never section headers, to decide page
# permissions) -- this is what lets a hostile PT_LOAD flag mutation
# (e.g. marking a segment containing .rodata executable) be detected
# even though it changes no section's own content or its own,
# independent sh_flags field.
_SECTION_TO_SEGMENT_HEADER_TEXT = "Section to Segment mapping"
_SECTION_TO_SEGMENT_LINE_RE = re.compile(
    r"^[ \t]*(?P<index>\d+)[ \t]*(?P<names>.*)$",
    re.MULTILINE,
)

# The exact, and ONLY, named sections this project's own real packaging
# pipeline (linuxdeploy's internal patchelf invocation, rewriting each
# bundled copy's RUNPATH after copying it out of the Qt SDK) is
# documented to ever legitimately alter the CONTENT of. `.dynstr` holds
# the actual RPATH/RUNPATH string bytes patchelf rewrites (and may need
# to grow, which this project's own empirical testing against a real
# patchelf 0.14.3 confirmed can relocate `.dynstr` -- together with
# whatever OTHER sections happened to be co-resident in its original
# PT_LOAD segment -- into a brand-new, appended PT_LOAD segment, while
# leaving every one of those OTHER sections' own content bytes
# unchanged); `.dynamic` holds the DT_RUNPATH/DT_RPATH tag's own string
# offset into `.dynstr`, which necessarily changes value whenever
# `.dynstr` is rewritten or relocated; `.interp` holds the interpreter
# path string, rewritten only if a packaging step ever invokes
# `patchelf --set-interpreter` (not currently done by this project's own
# pipeline, but excluded defensively since it is exactly the same class
# of documented, legitimate metadata rewrite). `.dynsym` is ALSO
# excluded from this raw pass -- NOT because patchelf rewrites its
# CONTENT directly, but because each Elf64_Sym entry's own st_shndx
# field is a raw SECTION-HEADER-TABLE INDEX, and this project's own
# empirical testing against a real patchelf 0.14.3 rpath rewrite
# confirmed that relocating/growing `.dynstr` can insert brand-new
# section-header-table entries earlier in the table, legitimately
# renumbering every OTHER section's index (and therefore every
# st_shndx value referencing one) even though the referenced section
# itself never moved or changed. `.dynsym`'s semantically meaningful
# content (each symbol's name/value/size/type/binding/visibility, and
# ITS OWN section by NAME rather than volatile index) is instead
# authenticated via _read_dynamic_symbols()'s decoded, index-independent
# view, exactly mirroring how _resolve_dynamic_tag_value() handles the
# same class of problem for `.dynamic`'s own address-valued tags.
# `.symtab` (the full, non-dynamic symbol table, present only on an
# UNSTRIPPED object -- this project's own actual bundled Qt plugins are
# always stripped release builds with no `.symtab` at all, but an
# ABI_ALLOWLIST-covered host system library or a distro-packaged
# component compared via package provenance may genuinely be unstripped)
# carries the EXACT SAME class of raw st_shndx section-index field as
# `.dynsym`, for exactly the same reason, and is excluded here for the
# same reason -- authenticated instead via _read_symtab_symbols()'s own
# decoded, index-independent view. `.strtab` (the plain string table
# `.symtab`'s own st_name fields index into, exactly analogous to how
# `.dynstr` relates to `.dynsym`) is excluded alongside it for the same
# "physical layout may legitimately shift, but decoded symbol NAMES are
# what is actually authenticated" reason as `.dynstr` above -- never
# because either table's real content is otherwise untrustworthy. No
# other section is excluded -- deliberately, so that any tampering with
# actually behavior-bearing content (`.text`, `.rodata`, `.data`,
# `.got`, `.init_array`, `.fini_array`, `.rela.*`, `.note.*`,
# `.eh_frame*`, `.gnu.hash`, `.gnu.version*`, and so on -- all
# empirically confirmed stable across this project's own real
# packaging pipeline's rpath rewrite) is still caught.
_CANONICAL_DIGEST_EXCLUDED_SECTIONS: frozenset[str] = frozenset(
    {".dynstr", ".dynamic", ".interp", ".dynsym", ".symtab", ".strtab"}
)

# Real cumulative-review regression, found only against this project's
# own actual bundled test fixtures once one grew large enough (a
# non-empty `.bss`) to push patchelf's rpath rewrite past the same
# relocation threshold already described above: `.gnu.hash` and every
# `.note.*` section are documented (see
# _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own docstring) to be swept,
# alongside `.dynstr`, into a brand-new PT_LOAD segment whenever that
# rewrite needs more room than the original slack allows -- and this
# project's own empirical testing found that the brand-new segment can
# legitimately have a DIFFERENT permission set than the ORIGINAL one,
# in EITHER direction: not only gaining a WRITE bit the section never
# had (already anticipated and tolerated by
# _canonical_load_digest()'s own inline comment), but also LOSING the
# EXECUTE bit it previously had (observed: `.gnu.hash`/`.note.*`
# moving from a "R E" segment to a "RW " one). Because a single
# standalone digest has no way to encode "only a GAIN is suspicious,
# a LOSS is fine" (there is no earlier/later ordering available to a
# hash function over one file in isolation -- doing so would require
# comparing two files' flags directly, which is not this function's
# job), the runtime-executable-bit check itself is excluded entirely
# for these two specific, already-established-as-relocatable
# categories of section, exactly mirroring how `.dynstr`/`.dynamic`/
# `.dynsym`/`.symtab`/`.strtab`'s own raw CONTENT is excluded above for
# the identical underlying "known to legitimately migrate during a
# real patchelf repack" reason. This does not weaken tamper detection
# for either section's own actual CONTENT (still fully hashed via the
# normal per-section pass) or for any OTHER section's executable-bit
# (a genuinely security-relevant section such as `.text`/`.plt`/
# `.init`/`.fini`/`.got`/`.data`/`.rodata` gaining or losing its
# executable status is still always caught) -- `.gnu.hash`/`.note.*`
# are pure, non-executed metadata that nothing ever jumps into by
# design, so which segment (and its incidental permission bits) they
# happen to be swept into carries no actual security meaning.
_SECTIONS_WITH_VOLATILE_SEGMENT_EXECUTABLE_BIT_NAMES: frozenset[str] = frozenset(
    {".gnu.hash"}
)
_SECTIONS_WITH_VOLATILE_SEGMENT_EXECUTABLE_BIT_PREFIXES: tuple[str, ...] = (".note.",)


def _has_volatile_segment_executable_bit(name: str) -> bool:
    """True for `.gnu.hash` and every `.note.*` section -- see
    _SECTIONS_WITH_VOLATILE_SEGMENT_EXECUTABLE_BIT_NAMES' own docstring
    for why these specific, already-documented-as-relocatable sections'
    runtime executable-bit is excluded from _canonical_load_digest()'s
    hash entirely, while their own content remains fully authenticated."""
    return name in _SECTIONS_WITH_VOLATILE_SEGMENT_EXECUTABLE_BIT_NAMES or name.startswith(
        _SECTIONS_WITH_VOLATILE_SEGMENT_EXECUTABLE_BIT_PREFIXES
    )

# `.dynamic`'s raw bytes are excluded wholesale above (its physical
# layout/ordering can legitimately shift alongside `.dynstr`'s own
# rewrite -- see _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own docstring),
# but that must not mean DT_NEEDED/DT_INIT/DT_FINI/DT_SONAME/DT_FLAGS
# and every other dynamic-linking directive go completely
# unauthenticated: every decoded dynamic tag (via readelf's own decoded,
# layout-independent view -- see _read_dynamic_tags()) is folded into
# _canonical_load_digest()'s own hash EXCEPT the tags that themselves
# carry, or directly measure, the RPATH/RUNPATH string patchelf is
# documented to legitimately rewrite: RPATH/RUNPATH themselves (the
# string content) and STRSZ (`.dynstr`'s own exact byte length, which
# this project's own empirical testing against a real patchelf 0.14.3
# rewrite confirmed changes value whenever the new RPATH/RUNPATH string
# is a different length than the one it replaced -- a direct,
# necessary, and therefore equally "documented legitimate rewrite"
# consequence of the same edit, not an independent authentication gap).
# A redirected DT_NEEDED SONAME, a hijacked DT_INIT/DT_FINI, or a
# widened DT_FLAGS/DT_FLAGS_1 bit is therefore still detected.
_DYNAMIC_TAGS_EXCLUDED_FROM_DIGEST: frozenset[str] = frozenset(
    {"RPATH", "RUNPATH", "STRSZ"}
)

_DYNAMIC_TAG_RE = re.compile(
    r"^\s*0x[0-9a-fA-F]+\s+\((?P<tag>[A-Za-z0-9_]+)\)\s+(?P<value>.*?)\s*$",
    re.MULTILINE,
)

# A dynamic tag's own decoded value is either meaningful, self-contained
# metadata (a needed library name, a flag word, a byte count -- stable
# across a legitimate repack) OR a bare virtual address INTO some other
# section (e.g. DT_INIT/DT_STRTAB/DT_GNU_HASH/DT_VERSYM/...). This
# project's own empirical testing against a real patchelf 0.14.3 rpath
# rewrite confirmed that growing `.dynstr` can relocate it -- together
# with whatever OTHER sections happened to be co-resident in its
# original segment (observed: `.gnu.hash`, every `.note.*`) -- into a
# brand-new PT_LOAD segment, which legitimately changes the RAW ADDRESS
# every dynamic tag pointing at any one of THOSE sections resolves to,
# even though the pointed-to section's own CONTENT never changed at
# all. A bare raw-address comparison would therefore false-positive on
# an entirely legitimate repack. _resolve_dynamic_tag_value() below
# solves this the same way _resolve_symbol_address() already does for
# symbol table entries (see that function's own docstring): by keying
# on the STABLE, content-addressed identity of the target SECTION'S
# NAME (invariant across relocation) PLUS the offset WITHIN that
# section (also invariant across relocation of the whole section --
# only the section's own base address moves, never a fixed value's
# position relative to it), rather than the volatile raw address.
#
# Third-HIGH-round review ("canonical ELF digest ... collapses dynamic
# pointer tags to section name, ignoring section-relative offset ...
# DT_INIT/FINI/INIT_ARRAY pointer changes within same section ... can
# pass with retained build ID"): the PREVIOUS version of this function
# returned only "-> <section name>", discarding the offset entirely.
# DT_INIT/DT_FINI point at a specific function's entry address, and
# DT_INIT_ARRAY/DT_FINI_ARRAY/DT_PREINIT_ARRAY point at the START of
# their respective array -- an attacker retargeting any of these to a
# DIFFERENT address that still happens to fall within the SAME section
# (e.g. redirecting DT_INIT a few bytes further into `.init`, or into
# attacker-controlled bytes elsewhere in `.text`) would previously have
# resolved to the identical "-> .init"/"-> .text" string and gone
# completely undetected. Preserving the offset closes this gap while
# remaining exactly as tolerant of legitimate section relocation as
# before, since the offset of a fixed logical target relative to its
# OWN section's start does not change merely because the whole section
# moves to a different base address.
_BARE_HEX_ADDRESS_RE = re.compile(r"^0x[0-9a-fA-F]+$")


def _resolve_dynamic_tag_value(value: str, sections: list[dict[str, str]]) -> str:
    """If `value` is a bare hex address (readelf's rendering for an
    address-valued dynamic tag), resolves it to
    "-> <section name>+0x<offset>" using whichever section's own
    [addr, addr + size) range contains it (or "-> <unmapped address>"
    if no section claims it, itself a meaningful, detectable
    difference) instead of the raw, potentially-legitimately-shifting
    address -- see this module's own comment above for why BOTH the
    section identity and the offset within it must be preserved. Any
    other, already-symbolic value (a library name, a flag word, a byte
    count) is returned completely unchanged."""
    if not _BARE_HEX_ADDRESS_RE.match(value):
        return value
    address = int(value, 16)
    for section in sections:
        if not section["name"] or "A" not in section["flags"]:
            continue
        start = int(section["addr"], 16)
        size = int(section["size"], 16)
        if size and start <= address < start + size:
            return f"-> {section['name']}+0x{address - start:x}"
    return "-> <unmapped address>"


def _read_dynamic_tags(path: Path) -> list[tuple[str, str]]:
    """Returns every (tag_name, decoded_value_text) pair from `path`'s own
    `.dynamic` section, in readelf's own decoded, human-readable form
    (e.g. ("NEEDED", "Shared library: [libc.so.6]")) -- deliberately the
    DECODED view, not raw section bytes, since a legitimate repack can
    freely reorder/relocate `.dynamic`'s own physical layout (see
    _CANONICAL_DIGEST_EXCLUDED_SECTIONS) without changing any tag's own
    logical meaning at all. Returns an empty list, never raises, for an
    object with no `.dynamic` section (e.g. a statically-linked
    executable) -- `readelf -d` itself exits 0 and merely reports there
    is none, which yields zero regex matches, not an error."""
    return [
        (match.group("tag"), match.group("value"))
        for match in _DYNAMIC_TAG_RE.finditer(_readelf(path, "-d", "-W"))
    ]


# readelf --dyn-syms -W's one-line-per-entry decoded dynamic symbol
# table format:
#   "     7: 0000000000001144    53 FUNC    GLOBAL DEFAULT   14 test_func"
#   "     1: 0000000000000000     0 FUNC    GLOBAL DEFAULT  UND malloc@GLIBC_2.2.5 (2)"
# The Ndx column is either a literal "UND"/"ABS"/"COM" keyword or a bare
# section-header-table index -- see _read_dynamic_symbols()'s own
# docstring for why the latter must be resolved to a stable section
# NAME rather than hashed as a raw, potentially-legitimately-shifting
# index. NAME may legitimately be empty (the reserved NUM 0 entry).
_DYNAMIC_SYMBOL_RE = re.compile(
    r"^[ \t]*\d+:[ \t]+(?P<value>[0-9a-fA-F]+)[ \t]+(?P<size>\d+)[ \t]+"
    r"(?P<type>\S+)[ \t]+(?P<bind>\S+)[ \t]+(?P<vis>\S+)[ \t]+"
    r"(?P<ndx>\S+)[ \t]*(?P<name>.*?)[ \t]*$",
    re.MULTILINE,
)


def _resolve_symbol_address(value: str, sections: list[dict[str, str]]) -> str:
    """Resolves a symbol table entry's own raw `st_value` field (a bare
    hex virtual address, e.g. readelf --syms's "0000000000020030"
    column -- always plain hex here, unlike a dynamic tag's decoded
    value, so no "0x" prefix is expected or required) to
    "-> <section name>+0x<offset>" using whichever loaded section's own
    [addr, addr + size) range contains it, or "-> <unmapped address>"
    if none does (itself a meaningful, detectable difference -- e.g. a
    genuinely undefined/zero-valued symbol, or a real hijack pointing
    outside any legitimate section). This is the SAME "resolve a
    volatile raw address to the STABLE section it falls within" idea
    _resolve_dynamic_tag_value() already applies to `.dynamic` tags --
    necessary here because this project's own empirical testing found
    that a SECTION-typed `.symtab`/`.dynsym` entry's value is exactly
    that section's own base address, which legitimately shifts whenever
    patchelf physically relocates that section (see
    _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own docstring) -- but, unlike a
    dynamic tag, the OFFSET *within* the section is preserved (not just
    the section identity), since a real FUNC/OBJECT symbol's value is a
    meaningful offset into a section that does NOT itself relocate
    (only special linker-synthesized SECTION/ABS symbols coincide with
    a section's own base address); collapsing to section-name alone
    would blind this digest to a symbol legitimately/illegitimately
    moving to a different offset within an otherwise-untouched
    section. Callers should NOT invoke this for a known
    linker-synthesized ABS alias symbol (see
    _LINKER_SYNTHESIZED_ABS_ALIAS_SYMBOL_NAMES) -- see that constant's
    own docstring for why such a symbol's address must be excluded
    entirely rather than resolved."""
    try:
        address = int(value, 16)
    except ValueError:
        return value
    for section in sections:
        if not section["name"] or "A" not in section["flags"]:
            continue
        start = int(section["addr"], 16)
        size = int(section["size"], 16)
        if size and start <= address < start + size:
            return f"-> {section['name']}+0x{address - start:x}"
    return "-> <unmapped address>"


# `_DYNAMIC` and `_GLOBAL_OFFSET_TABLE_` are linker-synthesized
# absolute-address aliases the link editor computes to equal
# `.dynamic`'s and `.got`'s own respective base addresses at LINK time.
# Depending on the exact binutils/ld/patchelf version involved, readelf
# may report either symbol's own Ndx column as the literal "ABS"
# keyword OR as a bare section-header-table index resolving to the
# very section it aliases (both forms observed empirically across
# different toolchain versions during this project's own testing) --
# the exclusion below therefore matches by NAME alone, regardless of
# which Ndx representation a given toolchain happens to produce. This
# project's own empirical testing against a real patchelf rpath
# rewrite that physically relocates `.dynamic` (see
# _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own docstring) found that
# patchelf updates every REAL runtime consumer of that address (the
# ELF program header's own PT_DYNAMIC entry, and every DT_* dynamic
# tag) but does NOT update these two symbol table entries' own stale
# `st_value` fields, which keep pointing at `.dynamic`'s/`.got`'s OLD,
# pre-relocation address -- a well-known, harmless patchelf quirk (no
# real ELF loader resolves "_DYNAMIC"/"_GLOBAL_OFFSET_TABLE_" by NAME
# at runtime; both are resolved structurally via AT_PHDR/PT_DYNAMIC and
# the GOT's own self-referential first entry respectively, so a stale
# symtab alias has zero runtime effect). Address-resolving these two
# symbols the same way as any other would false-positive on this
# entirely legitimate repack whenever `.dynamic`/`.got` happens to
# relocate; their value is therefore excluded from the digest entirely
# (mirroring how DT_STRSZ/RPATH/RUNPATH are excluded from
# _DYNAMIC_TAGS_EXCLUDED_FROM_DIGEST for the same class of reason) --
# every OTHER field of these two entries (size/type/bind/vis/ndx/name)
# remains fully authenticated.
_LINKER_SYNTHESIZED_ABS_ALIAS_SYMBOL_NAMES = frozenset(
    {"_DYNAMIC", "_GLOBAL_OFFSET_TABLE_"}
)


def _decode_symbol_table_entries(
    output: str, sections: list[dict[str, str]]
) -> list[tuple[str, str, str, str, str, str, str]]:
    """Shared decoding logic for both `.dynsym` (via _read_dynamic_
    symbols()) and `.symtab` (via _read_symtab_symbols()) -- see either
    caller's own docstring for why each entry's raw st_shndx section-
    header-table index must be resolved to a stable section NAME rather
    than hashed as a raw, potentially-legitimately-shifting index, and
    _resolve_symbol_address()'s own docstring for why each entry's raw
    st_value address field must likewise be resolved to a stable
    section-relative form. `_LINKER_SYNTHESIZED_ABS_ALIAS_SYMBOL_NAMES`
    members are special-cased: see that constant's own docstring."""
    results: list[tuple[str, str, str, str, str, str, str]] = []
    for match in _DYNAMIC_SYMBOL_RE.finditer(output):
        ndx = match.group("ndx")
        name = match.group("name")
        if ndx not in ("UND", "ABS", "COM"):
            try:
                index = int(ndx)
            except ValueError:
                ndx = f"<unparseable section index {ndx!r}>"
            else:
                ndx = (
                    sections[index]["name"]
                    if 0 <= index < len(sections)
                    else f"<unmapped section index {index}>"
                )
        if name in _LINKER_SYNTHESIZED_ABS_ALIAS_SYMBOL_NAMES:
            value = "<linker-synthesized ABS alias, address excluded>"
        else:
            value = _resolve_symbol_address(match.group("value"), sections)
        results.append(
            (
                value,
                match.group("size"),
                match.group("type"),
                match.group("bind"),
                match.group("vis"),
                ndx,
                name,
            )
        )
    return results


def _read_dynamic_symbols(
    path: Path, sections: list[dict[str, str]]
) -> list[tuple[str, str, str, str, str, str, str]]:
    """Returns every dynamic symbol table entry in `path` as a
    (value, size, type, bind, vis, resolved_ndx, name) tuple, in
    readelf's own decoded, human-readable form -- deliberately NOT
    `.dynsym`'s raw bytes (see _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own
    docstring for why each entry's raw st_shndx field is a
    section-header-table index that can legitimately be renumbered by
    an unrelated, legitimate `.dynstr` relocation). A literal "UND" /
    "ABS" / "COM" Ndx keyword is kept as-is (already a stable, symbolic
    value); a bare numeric index is resolved via `sections` to the
    actual section NAME it refers to (or "<unmapped section index>" if
    out of range, itself a meaningful, detectable difference) using the
    exact same name-based stability strategy _resolve_dynamic_tag_value()
    uses for address-valued dynamic tags. Returns an empty list, never
    raises, for an object with no dynamic symbol table at all."""
    try:
        output = _readelf(path, "--dyn-syms", "-W")
    except ElfIdentityError:
        return []
    return _decode_symbol_table_entries(output, sections)


# `readelf --syms -W` (unlike `--dyn-syms`) prints EVERY symbol table an
# object has, each preceded by its own "Symbol table '<name>' contains
# N entries:" header line -- an object with both `.dynsym` and
# `.symtab` (i.e. an unstripped shared object) therefore emits two
# separate such blocks in the same invocation's output. Matched here
# purely to locate the START of the `.symtab` block; the following
# entries are parsed by the exact same `_DYNAMIC_SYMBOL_RE` used for
# `--dyn-syms` output, since readelf renders both tables in an
# identical per-entry column format.
_SYMBOL_TABLE_HEADER_RE = re.compile(
    r"^Symbol table '(?P<name>[^']+)' contains \d+ entries:$",
    re.MULTILINE,
)


def _read_symtab_symbols(
    path: Path, sections: list[dict[str, str]]
) -> list[tuple[str, str, str, str, str, str, str]]:
    """Returns every entry of `path`'s own `.symtab` (the full,
    non-dynamic symbol table present only on an UNSTRIPPED object) as a
    (value, size, type, bind, vis, resolved_ndx, name) tuple, in
    readelf's own decoded, human-readable form -- deliberately NOT
    `.symtab`'s raw bytes (see _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own
    docstring: each entry's raw st_shndx field is EXACTLY the same
    class of legitimately-renumbered section-header-table index as
    `.dynsym`'s own, for the identical reason). Returns an empty list,
    never raises, for a stripped object with no `.symtab` at all (this
    project's own actually bundled Qt plugins are always stripped
    release builds; only an ABI_ALLOWLIST-covered host system library
    or an unstripped distro-packaged component compared via package
    provenance would ever genuinely have one) -- a missing `.symtab` is
    not itself an error, exactly like a missing `.dynamic` section on a
    statically-linked executable in _read_dynamic_tags()."""
    try:
        output = _readelf(path, "--syms", "-W")
    except ElfIdentityError:
        return []
    headers = list(_SYMBOL_TABLE_HEADER_RE.finditer(output))
    for index, header in enumerate(headers):
        if header.group("name") != ".symtab":
            continue
        block_start = header.end()
        block_end = (
            headers[index + 1].start() if index + 1 < len(headers) else len(output)
        )
        return _decode_symbol_table_entries(output[block_start:block_end], sections)
    return []





# Deliberately duplicated from (not imported from)
# audit_dependency_closure.py's own _ELF_MAGIC/_is_elf_file -- each script
# keeps its own tiny, independently-reviewable primitive so a change to
# one script's definition can never silently affect the other's, exactly
# matching the existing _SONAME_RE/_BUILD_ID_RE duplication rationale
# above. Used by find_bundled_libraries() (round-9+ review item 10) to
# discover every bundled ELF by its own magic bytes rather than by any
# filename convention.
_ELF_MAGIC = b"\x7fELF"


def _is_elf_file(path: Path) -> bool:
    """True if path's first bytes are the ELF magic number -- deliberately
    never inspects the extension/basename at all, so it correctly
    recognizes a real ELF object regardless of what it happens to be
    named (see find_bundled_libraries()'s own docstring for why this
    matters). Any I/O failure (permission, dangling symlink, deleted
    mid-walk) is treated as "not an ELF file" rather than a hard error --
    a file this script cannot even open is not a bundled ELF object this
    script could meaningfully classify or SBOM-inventory either way."""
    try:
        with path.open("rb") as handle:
            return handle.read(len(_ELF_MAGIC)) == _ELF_MAGIC
    except OSError:
        return False


class ElfIdentityError(RuntimeError):
    """Raised when `readelf` itself cannot be run (not installed, or the
    target is not a real ELF object) while computing a library's
    cryptographic/provenance identity for the SBOM or for the Qt
    build-id provenance check. Deliberately a hard, fail-closed error
    (mirroring audit_dependency_closure.py's ClosureAuditError) rather
    than a None-tolerant best-effort: an SBOM entry or a provenance
    decision silently missing this data would defeat the entire point
    of asking for it."""


def _readelf(path: Path, *flags: str) -> str:
    try:
        result = subprocess.run(
            ["readelf", *flags, str(path)],
            check=True,
            capture_output=True,
            text=True,
        )
    except FileNotFoundError as exc:
        raise ElfIdentityError(
            "readelf is required (part of binutils) but was not found on PATH."
        ) from exc
    except subprocess.CalledProcessError as exc:
        raise ElfIdentityError(
            f"readelf failed to parse {path} as an ELF object: {exc.stderr.strip()}"
        ) from exc
    return result.stdout


def _read_soname(path: Path) -> str | None:
    """This library's own DT_SONAME, if it declares one (most versioned
    shared objects do; a plugin loaded only by basename, e.g. a Qt
    plugin, typically does not) -- None, not a failure, when absent."""
    match = _SONAME_RE.search(_readelf(path, "-d", "-W"))
    return match.group("name") if match else None


def _read_build_id(path: Path) -> str | None:
    """This exact compiled object's own `.note.gnu.build-id`, a hex
    digest the linker embeds and which normal post-link binary-editing
    tools (patchelf's RUNPATH/interpreter rewriting, strip's debug-info
    removal, cp) do not alter, because it identifies the *compiled code*
    itself rather than any mutable container metadata.

    Round-N+ review (HIGH, "Build ID not signature"): a build-id match
    alone is deliberately NOT treated anywhere in this module any more as
    sufficient proof of authenticity (see
    _is_same_compiled_object_or_unwritten()'s own docstring, which now
    uses _canonical_load_digest() instead) -- it is merely a stored
    identifier the linker writes once at build time, never re-verified
    or invalidated by anything if the underlying bytes are edited
    afterward. A tool capable of patching PT_LOAD segment bytes in place
    can trivially leave an old, now-stale build-id note untouched, and
    nothing in the ELF format itself would ever notice or object. Still
    retained/exposed here for elf_identity()'s SBOM inventory, where it
    remains useful, honestly-labeled provenance metadata (alongside the
    real canonicalLoadDigest field), just never the basis for an
    authenticity decision by itself. Returns None, not a failure, if the
    object was linked without `--build-id` (rare on a modern distro
    toolchain, but not an error in itself)."""
    match = _BUILD_ID_RE.search(_readelf(path, "-n"))
    return match.group("id").lower() if match else None


def _read_entry_point(path: Path) -> str | None:
    """This exact compiled object's own ELF header e_entry field (the
    raw virtual address execution actually starts at), or None if
    `readelf -h`'s output cannot be matched at all (never expected for a
    real ELF object, but tolerated the same way a missing SONAME/
    build-id is). See _ENTRY_POINT_RE's own module-level comment for why
    this is safe to fold directly into _canonical_load_digest() without
    any of the offset/vaddr instability concerns that apply to PT_LOAD's
    own program-header fields."""
    match = _ENTRY_POINT_RE.search(_readelf(path, "-h"))
    return match.group("address").lower() if match else None


def _read_elf_header_identity(path: Path) -> dict[str, str] | None:
    """Round-N+ review (HIGH, "canonical ELF identity misses ELF class/
    data/type/OSABI/machine"): returns `path`'s own ELF header class
    (32/64-bit), data (endianness), OS/ABI, type (e.g. ET_DYN/ET_EXEC),
    and machine (target ISA) fields -- see _ELF_HEADER_CLASS_RE's own
    module-level comment for exactly why every one of these is safe to
    authenticate unconditionally. Returns None (never raises) only if
    `readelf -h`'s own output is missing ANY of the five fields --
    never expected for a real ELF object, but tolerated the identical
    way a missing SONAME/build-id is elsewhere in this module."""
    header_text = _readelf(path, "-h")
    fields = {
        "class": _ELF_HEADER_CLASS_RE.search(header_text),
        "data": _ELF_HEADER_DATA_RE.search(header_text),
        "osabi": _ELF_HEADER_OSABI_RE.search(header_text),
        "type": _ELF_HEADER_TYPE_RE.search(header_text),
        "machine": _ELF_HEADER_MACHINE_RE.search(header_text),
    }
    if any(match is None for match in fields.values()):
        return None
    return {name: match.group("value").strip() for name, match in fields.items() if match}


def _read_program_headers(path: Path) -> list[dict[str, str]]:
    """Returns every field (type, flags, filesz, memsz, align, vaddr,
    offset, paddr) for EVERY program header entry in `path`, in on-disk
    program-header-table order (index 0, 1, 2, ...) -- the same order
    readelf's own "Section to Segment mapping" table numbers its
    entries by, which is what lets _section_to_segment_load_flags()
    below correlate the two tables positionally. `flags` is the raw
    three-character "R"/"W"/"E" (or space) field, e.g. "R E", "RW ".

    Round-N+ review (HIGH, "canonical ELF identity still omits actual
    PT_LOAD mappings"): `offset` and `paddr`, though STILL not safe to
    hash as raw absolute values wholesale (this project's own empirical
    testing against a real patchelf 0.14.3 rewrite confirmed they can
    legitimately shift for segments whose content gets relocated into a
    newly-appended trailing run), are now retained here so
    _canonical_load_segment_records() can fold them into a CONTENT-
    stable, append-tolerant relative/bias representation for the
    non-trailing PT_LOAD prefix it authenticates. Keeping the raw
    parsed fields available here does not itself weaken anything:
    callers still choose which representation, if any, is safe to
    compare for their own purpose.

    Round-N+ review (HIGH, "canonical ELF identity misses load security/
    mapping ... load addresses/ranges/order/overlap pass"): `vaddr` IS
    now included -- unlike `offset`, it is only ever consulted by
    _canonical_load_digest() below to establish the real, load-time
    RELATIVE ORDER of PT_LOAD segments against one another (see
    _ordered_load_segment_flags()'s own docstring), never hashed as a
    raw absolute value itself, so a legitimately-shifted absolute
    virtual address from an appended relocation segment cannot itself
    change the digest -- only an actual reordering, insertion, removal,
    or permission change of a PT_LOAD segment relative to its siblings
    can. Raises ElfIdentityError (via _readelf()) if readelf itself
    cannot parse `path`'s program headers at all."""
    return [
        {
            "type": match.group("type"),
            "offset": match.group("offset"),
            "flags": match.group("flags"),
            "paddr": match.group("paddr"),
            "filesz": match.group("filesz"),
            "memsz": match.group("memsz"),
            "align": match.group("align"),
            "vaddr": match.group("vaddr"),
        }
        for match in _PROGRAM_HEADER_RE.finditer(_readelf(path, "-l", "-W"))
    ]


def _ordered_load_segments(path: Path) -> list[tuple[int, dict[str, str]]]:
    """Returns (program-header-table index, parsed_header_dict) for every
    PT_LOAD program header in `path`, ordered by ascending virtual
    address -- see _ordered_load_segment_flags()'s own docstring for the
    full ordering rationale (identical here; that function now simply
    projects the `flags` field from the fuller records this one
    preserves). The index mirrors _read_program_headers()'s own on-disk,
    pre-sort table order (the same indexing
    _section_to_segment_load_flags()/_load_segment_section_membership()
    already key by), so _canonical_ordered_load_segments() below can
    correlate a candidate trailing segment back to the sections it
    actually maps, rather than discarding the index and being left with
    only a flags string to judge a strip decision by."""
    try:
        headers = _read_program_headers(path)
    except ElfIdentityError:
        return []
    loads = [
        (index, header)
        for index, header in enumerate(headers)
        if header["type"] == "LOAD"
    ]
    loads.sort(key=lambda entry: int(entry[1]["vaddr"], 16))
    return loads


def _ordered_load_segment_flags(path: Path) -> list[str]:
    """Returns the R/W/E flags string (e.g. "R E", "RW ") of every
    PT_LOAD program header in `path`, ordered by ascending virtual
    address -- the real, load-time relative order the ELF spec itself
    requires PT_LOAD segments to already be non-overlapping and
    monotonic in (a well-formed object's loader silently relies on this
    invariant; this project's own build never intentionally produces a
    malformed one). A legitimate patchelf RUNPATH rewrite can shift
    every ABSOLUTE virtual address once a new segment is appended (this
    project's own empirical testing already established that for
    `_read_program_headers()`'s own excluded offset/paddr fields), but
    it does not insert a brand-new PT_LOAD segment ahead of or between
    existing ones, nor does it change any EXISTING PT_LOAD segment's own
    declared protection flags -- so this ordered, address-relative (not
    address-absolute) sequence is safe to fold directly into
    _canonical_load_digest(), closing the round-N+ review's "RX->RWX
    stays true and passes ... load addresses/ranges/order/overlap pass"
    finding at the SEGMENT granularity (in addition to the per-SECTION
    write-bit tracking _canonical_load_digest() also now does): an
    attacker flipping an existing PT_LOAD segment's own flags (e.g.
    granting it write permission it never had), reordering segments, or
    inserting/removing one entirely changes this sequence; a legitimate
    patchelf-appended trailing segment (whose own flags this project's
    own testing found are always a plain "RW ") does not retroactively
    change any EARLIER segment's own position in this ordering, only
    adds one more entry at the end. Delegates to _ordered_load_segments()
    above, discarding the program-header-table index that function
    additionally preserves for _canonical_load_segment_prefix()'s own
    use."""
    return [header["flags"] for _, header in _ordered_load_segments(path)]


# Round-N+ review (HIGH, "canonical ELF identity ... strips all
# trailing RW LOAD indiscriminately"): the PRIOR version of this
# function tolerated stripping ANY trailing run of exactly "RW "-flagged
# PT_LOAD segments purely by their own flags string, with no regard at
# all for WHAT BYTES that segment actually contains. Section headers
# are optional, loader-irrelevant metadata (the kernel/dynamic loader
# only ever consults program headers to map memory) -- so an attacker
# can freely append a brand-new PT_LOAD segment carrying entirely
# arbitrary, malicious content with NO corresponding section-header
# entry at all (fully legal ELF), and as long as its own flags happen
# to be the same "RW " triple a genuine patchelf append always carries
# (trivial for an attacker to also choose), the prior heuristic would
# have silently discarded it from the digest -- the exact "regular
# trailing data LOAD [segment] passes" finding. This version instead
# requires the candidate trailing segment's own section-to-segment
# mapping (_load_segment_section_membership()) to be BOTH non-empty AND
# entirely composed of section names already independently
# authenticated elsewhere in this digest (see this function's own
# in-body comment for exactly which names those are and why): a
# genuine patchelf append (whose relocated `.gnu.hash`/`.note.*`/
# `.dynstr` sections are always one of those already-authenticated
# names) keeps satisfying this and is still tolerated exactly as
# before, while an attacker's orphan, section-less (or fabricated-name,
# not-actually-authenticated) segment now fails this correlation check
# and remains folded into the digest, where its mere presence (an
# additional LOADSEG entry the reference object never had) is detected.
def _canonical_ordered_load_segments(path: Path) -> list[tuple[int, dict[str, str]]]:
    """Returns `_ordered_load_segments(path)` with any trailing run
    of "RW "-flagged segments removed ONLY while each one's own mapped
    sections (via `_load_segment_section_membership()`) are non-empty
    and every single one is a section name this digest already
    independently authenticates some other way -- the append-tolerant,
    but no longer purely flags-based, canonical PT_LOAD prefix actually
    folded into _canonical_load_digest() (see this function's own
    preceding module comment for the full rationale). Never raises;
    returns the full, unmodified ordered list if section headers or the
    "Section to Segment mapping" table cannot be read at all (a fail-
    closed default -- an unreadable/absent correlation source can never
    itself be used to justify tolerating a strip)."""
    ordered = list(_ordered_load_segments(path))
    if not ordered:
        return []
    try:
        sections = _read_section_headers(path)
    except ElfIdentityError:
        sections = []
    # Every section actually mapped into memory ("A"lloc flag set) is,
    # one way or another, already independently authenticated by
    # _canonical_load_digest(): either directly, via its own raw
    # content/type/declared-flags/runtime-permission-bit record in the
    # main per-section loop (every name NOT in
    # _CANONICAL_DIGEST_EXCLUDED_SECTIONS), or indirectly, via a
    # decoded, layout-independent view keyed by that SAME section name
    # (`.dynstr`/`.dynsym`/`.symtab`/`.strtab`/`.dynamic`/`.interp`,
    # each instead re-authenticated via _read_dynamic_tags()/
    # _read_dynamic_symbols()/_read_symtab_symbols() -- see
    # _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own docstring for exactly
    # why). A trailing segment whose OWN mapped names are entirely
    # drawn from this set therefore carries nothing this digest does
    # not already cover under that same name; a segment mapping even
    # one name outside it -- or, critically, mapping NO section at all
    # -- is never tolerated.
    allocated_names = {
        section["name"] for section in sections if "A" in section["flags"]
    }
    membership = _load_segment_section_membership(path)
    while ordered and ordered[-1][1]["flags"] == "RW ":
        index, _ = ordered[-1]
        names = membership.get(index)
        if not names or not all(name in allocated_names for name in names):
            break
        ordered.pop()
    return ordered


def _canonical_load_segment_prefix(path: Path) -> list[str]:
    """Flags-only projection of `_canonical_ordered_load_segments()` --
    retained as the small, focused helper existing callers/tests already
    use, while _canonical_load_segment_records() below reuses the SAME
    trailing-run detection to authenticate the fuller PT_LOAD topology
    fields the latest cumulative review required."""
    return [header["flags"] for _, header in _canonical_ordered_load_segments(path)]


def _canonical_load_segment_records(
    path: Path,
) -> list[tuple[str, str, str, str, str, str, str, str]]:
    """Round-N+ review (HIGH, "canonical ELF identity still omits actual
    PT_LOAD mappings"): returns a canonical, append-tolerant record for
    every PT_LOAD segment RETAINED by `_canonical_ordered_load_segments()`
    -- i.e. every non-trailing-appended "normal" segment, using the
    EXACT same membership-gated trailing-run detection this module
    already uses for `_canonical_load_segment_prefix()`, never a second,
    divergent heuristic.

    Each tuple is:
      (flags, filesz, memsz, align,
       deltaVaddrFromPrevious, deltaOffsetFromPrevious,
       offsetMinusVaddr, paddrMinusVaddr)

    Why these specific fields/representations:

      * flags/filesz/memsz/align are the segment's own security- and
        loader-meaningful declarations, preserved verbatim for every
        normal segment. Mutating any one changes the record directly.

      * `deltaVaddrFromPrevious` binds the REAL load-time topology
        (range spacing/order/overlap) without trusting raw absolute
        virtual addresses. A legitimate patchelf append can add new
        trailing PT_LOAD segments, but it does not insert a new normal
        segment ahead of or between the retained prefix; the relative
        spacing of that prefix is therefore stable, while a malicious
        overlap/re-gap mutation is not.

      * `deltaOffsetFromPrevious` does the same for on-disk PT_LOAD
        layout: a later cumulative review correctly pointed out that
        "same vaddr order, different file mapping topology" remained
        otherwise unbound. Again, relative spacing across the retained
        normal prefix is what survives a legitimate append, while a
        shifted/overlapping file mapping does not.

      * `offsetMinusVaddr` and `paddrMinusVaddr` are the content-stable
        bias terms that bind each segment's own file/load/physical
        relationship WITHOUT keying on raw absolute offset/paddr values.
        A legitimate whole-prefix relocation that preserves the ELF
        loader's own congruence rules keeps these biases unchanged; a
        malicious edit changing only offset, only paddr, or their
        relationship to the mapped address space does not.

    The first retained segment uses the literal "<first>" sentinel for
    both delta fields: there is, by definition, no previous retained
    segment to measure a gap from. That is sufficient because the
    per-segment bias terms above still bind the first segment's own
    relationship between file offset, physical address, and virtual
    address. Returns an empty list, never raises, when program headers
    cannot be parsed at all, mirroring `_ordered_load_segments()`."""
    records: list[tuple[str, str, str, str, str, str, str, str]] = []
    previous_vaddr: int | None = None
    previous_offset: int | None = None
    for _, header in _canonical_ordered_load_segments(path):
        vaddr = int(header["vaddr"], 16)
        offset = int(header["offset"], 16)
        paddr = int(header["paddr"], 16)
        if previous_vaddr is None or previous_offset is None:
            delta_vaddr = "<first>"
            delta_offset = "<first>"
        else:
            delta_vaddr = hex(vaddr - previous_vaddr)
            delta_offset = hex(offset - previous_offset)
        records.append(
            (
                header["flags"],
                header["filesz"],
                header["memsz"],
                header["align"],
                delta_vaddr,
                delta_offset,
                hex(offset - vaddr),
                hex(paddr - vaddr),
            )
        )
        previous_vaddr = vaddr
        previous_offset = offset
    return records


# Third-HIGH-round review ("... executable GNU_STACK, RELRO/TLS/load
# offsets/sizes can pass with retained build ID"): PT_GNU_STACK (whose
# own FLAGS field is the sole authoritative record of whether the
# process stack is mapped executable -- a real, security-relevant
# hardening toggle with no corresponding section at all),
# PT_GNU_RELRO (the byte range the dynamic loader mprotects read-only
# after relocation processing -- stripping or shrinking it is a real
# hardening downgrade), and PT_TLS (the thread-local-storage template's
# own size/alignment) are program-header-only state: none of them is
# correlated to any section by _section_to_segment_load_flags() (that
# function only ever looks at PT_LOAD entries), so NONE of their own
# flags/filesz/memsz/align were previously folded into
# _canonical_load_digest() at all -- an attacker could flip PT_GNU_STACK
# executable, shrink/delete PT_GNU_RELRO, or alter PT_TLS's own
# template size, and the previous digest would not change one bit.
#
# PT_LOAD, PT_DYNAMIC, and PT_NOTE are all deliberately excluded here,
# for DIFFERENT documented-legitimate-repack reasons empirically
# confirmed against a real patchelf 0.14.3 RUNPATH rewrite (this
# project's own testing, see this module's cumulative-review test
# suite): PT_LOAD's own per-segment filesz/memsz are NOT stable (moving
# `.gnu.hash`/`.note.*`/`.dynstr` out of an existing LOAD segment into
# a brand-new one legitimately shrinks the original segment's own
# filesz/memsz) -- that segment class is already correctly
# authenticated via the per-SECTION abstraction
# (_section_to_segment_load_flags() + per-section content hashing)
# rather than via any raw segment-level record, and must stay that way.
# PT_DYNAMIC's own filesz/memsz are ALSO not stable (a longer RPATH
# string can change the total number of Elf64_Dyn entries actually
# emitted, e.g. via padding/alignment, changing PT_DYNAMIC's own
# declared size by a whole entry even though every INDIVIDUAL decoded
# tag this project cares about is unchanged) -- `.dynamic`'s own
# decoded tag content is already fully authenticated via
# _read_dynamic_tags()'s own decoded, layout-independent view, so
# PT_DYNAMIC's raw program-header size is pure redundant bookkeeping
# for already-covered data, not an independent authentication gap.
#
# PT_NOTE's own SEGMENT COUNT/filesz/memsz are, for exactly the same
# reason, ALSO not stable: a real cumulative-review regression, found
# only against this project's own actual produced AppImage (this
# module's own prior synthetic fixtures had at most one real Qt-style
# extra allocated NOTE section co-resident with `.note.gnu.build-id`,
# never triggering this split) -- linuxdeploy-plugin-qt's genuine,
# unmodified-in-substance bundled Qt plugins/QML modules (e.g.
# plugins/imageformats/libqgif.so) legitimately had ONE PT_NOTE program
# header covering BOTH `.note.gnu.property` and `.note.qt.metadata`
# (plus a separate one for `.note.gnu.build-id`) before patchelf's
# RUNPATH rewrite, but THREE separate PT_NOTE program headers -- one
# per individual note section -- after it, once the rewrite's
# `.dynstr`-driven relocation (see the PT_LOAD comment above) moved
# those note sections into a brand-new, appended LOAD segment and the
# linker/patchelf re-grouped their own program-header coverage
# differently. Each individual note SECTION's own content, type, flags,
# and effective runtime executable-bit are already fully authenticated
# by the main per-section digest loop in _canonical_load_digest() (none
# of `.note.gnu.build-id`/`.note.gnu.property`/`.note.qt.metadata`/any
# other `.note.*` section is in `_CANONICAL_DIGEST_EXCLUDED_SECTIONS`),
# so PT_NOTE's own raw segment-level grouping/count/filesz/memsz is,
# exactly like PT_LOAD and PT_DYNAMIC above, pure redundant bookkeeping
# for already-covered data, not an independent authentication gap --
# and folding it in as if it WERE content-stable made every genuinely
# unmodified Qt plugin/QML module with more than one loaded NOTE
# section fail this digest comparison after every real patchelf run.
#
# Entries are returned SORTED by (type, flags, filesz, memsz, align)
# rather than raw on-disk program-header-table order: this project's
# own empirical testing found that a real patchelf 0.14.3 RUNPATH
# rewrite can legitimately REORDER the program-header table itself
# (e.g. swapping which of two GNU_STACK/GNU_RELRO-style segments
# appears first) with no security consequence at all, since the loader
# consults each entry by its own TYPE/semantics, never by table
# position. Sorting by full content tuple (rather than raw index)
# produces a canonical, order-independent representation of the exact
# same multiset of entries, so a genuine reorder cannot change the
# digest while any actual value change (flags/filesz/memsz/align)
# still does.
def _non_load_program_header_records(path: Path) -> list[tuple[str, str, str, str, str]]:
    """Returns (type, flags, filesz, memsz, align) for every program
    header entry in `path` whose type is none of "LOAD", "DYNAMIC", or
    "NOTE", sorted into a canonical, table-position-independent order
    (stable across a legitimate patchelf reordering of the table
    itself) -- see this function's own preceding module comment for
    exactly which security-relevant segments this closes
    (PT_GNU_STACK/PT_GNU_RELRO/PT_TLS) and why PT_LOAD/PT_DYNAMIC/
    PT_NOTE are all deliberately excluded (each is either already
    authenticated via a content-stable, section-keyed abstraction
    elsewhere in this module, or -- for PT_NOTE specifically -- its own
    raw segment grouping/count is empirically not stable under a real
    patchelf RUNPATH rewrite at all). Returns an empty list, never
    raises, if `path`'s program headers cannot be parsed at all
    (callers already tolerate this the same way they tolerate a
    missing SONAME/build-id)."""
    try:
        headers = _read_program_headers(path)
    except ElfIdentityError:
        return []
    return sorted(
        (header["type"], header["flags"], header["filesz"], header["memsz"], header["align"])
        for header in headers
        if header["type"] not in ("LOAD", "DYNAMIC", "NOTE")
    )



def _read_section_headers(path: Path) -> list[dict[str, str]]:
    """Returns every section header entry in `path` (name, type, flags,
    size, offset, and its own virtual address, each as a hex-string) in
    on-disk section-header-table order. The reserved index-0 NULL entry
    (empty name, empty flags) is included -- callers that only want
    genuinely loaded content filter by the presence of "A" in `flags`
    themselves, exactly as _canonical_load_digest() does."""
    sections: list[dict[str, str]] = []
    for match in _SECTION_HEADER_RE.finditer(_readelf(path, "-S", "-W")):
        sections.append(
            {
                "name": match.group("name"),
                "type": match.group("type"),
                "flags": match.group("flags"),
                "size": match.group("size"),
                "offset": match.group("offset"),
                "addr": match.group("addr"),
            }
        )
    return sections


def _section_to_segment_load_flags(path: Path) -> dict[str, str]:
    """Returns, for every section name readelf's own "Section to Segment
    mapping" table lists as belonging to at least one LOAD-type program
    header, that LOAD segment's own flags string (e.g. "R E", "RW ") --
    i.e. exactly the permissions the kernel/dynamic loader will actually
    map that section's bytes with at runtime, since the loader only ever
    consults program headers, never section headers, to decide page
    protection. A section named in more than one LOAD segment (not
    normally expected, but not itself malformed) uses the FIRST such
    segment's flags. Returns an empty mapping (never raises) if the
    marker text is absent or readelf cannot be run -- callers must
    already tolerate a missing/None flags lookup for any given section,
    exactly as they tolerate a missing build-id or SONAME."""
    try:
        combined = _readelf(path, "-l", "-W")
    except ElfIdentityError:
        return {}
    headers = _read_program_headers(path)
    marker = combined.find(_SECTION_TO_SEGMENT_HEADER_TEXT)
    if marker == -1:
        return {}
    mapping_block = combined[marker:]
    result: dict[str, str] = {}
    for match in _SECTION_TO_SEGMENT_LINE_RE.finditer(mapping_block):
        index = int(match.group("index"))
        if index >= len(headers):
            continue
        header_type, header_flags = headers[index]["type"], headers[index]["flags"]
        if header_type != "LOAD":
            continue
        for name in match.group("names").split():
            result.setdefault(name, header_flags)
    return result


def _load_segment_section_membership(path: Path) -> dict[int, list[str]]:
    """Returns, for every program-header-table index (the SAME on-disk,
    pre-sort indexing `_read_program_headers()`/
    `_section_to_segment_load_flags()` already use) whose own program-
    header type is "LOAD" and readelf's own "Section to Segment
    mapping" table lists at least one section name for, the sorted list
    of section names actually mapped into that segment.

    Used exclusively by `_canonical_load_segment_prefix()` to decide
    whether tolerating a candidate trailing "RW "-flagged PT_LOAD
    segment's append is structurally EVIDENCED (every section it maps
    is a name this module's own digest already independently
    authenticates one way or another) rather than merely flags-shaped
    -- closing the round-N+ review's "regular trailing data LOAD
    [segment] passes" finding: section headers are entirely optional,
    loader-irrelevant metadata (the kernel/dynamic loader only ever
    consults PROGRAM headers, never section headers, to map memory), so
    an attacker can legally append a brand-new PT_LOAD segment carrying
    arbitrary malicious bytes with NO corresponding section-header
    entry at all -- this function returns an EMPTY list (never a
    missing key) for such an orphan segment, which
    `_canonical_load_segment_prefix()` treats as conclusive proof that
    tolerating its removal is unevidenced. Returns an empty mapping
    (never raises) if the marker text is absent or readelf cannot be
    run at all -- callers must already tolerate this failure mode
    exactly as `_section_to_segment_load_flags()`'s own callers do."""
    try:
        combined = _readelf(path, "-l", "-W")
    except ElfIdentityError:
        return {}
    headers = _read_program_headers(path)
    marker = combined.find(_SECTION_TO_SEGMENT_HEADER_TEXT)
    if marker == -1:
        return {}
    mapping_block = combined[marker:]
    result: dict[int, list[str]] = {}
    for match in _SECTION_TO_SEGMENT_LINE_RE.finditer(mapping_block):
        index = int(match.group("index"))
        if index >= len(headers) or headers[index]["type"] != "LOAD":
            continue
        result[index] = sorted(match.group("names").split())
    return result


def _canonical_load_digest(path: Path) -> str | None:
    """Second-HIGH-round review ("Qt canonical digest hashes only PF_X
    PT_LOAD, missing behavior-bearing loaded data/program headers"):
    returns a SHA-256 digest computed over the content, ELF section
    type, ELF section flags, AND actual runtime (LOAD-segment) mapping
    flags of EVERY loaded (SHF_ALLOC) section in `path` -- i.e. every
    byte the dynamic loader will ever map into this object's own address
    space, not merely the subset that happens to live inside a segment
    already flagged executable -- excluding only the small, exactly
    documented set of sections this project's own real packaging
    pipeline (linuxdeploy's internal patchelf RUNPATH rewrite) is known
    to legitimately alter (see _CANONICAL_DIGEST_EXCLUDED_SECTIONS'
    own docstring).

    Round-N+ review (HIGH, "canonical ELF identity misses load security/
    mapping ... RX->RWX stays true and passes; e_entry, load addresses/
    ranges/order/overlap pass"), plus the later cumulative re-review
    (HIGH, "canonical ELF identity still omits actual PT_LOAD
    mappings"): also authenticates, in addition to every previously-
    covered field, (a) each retained section's own runtime WRITE bit,
    not merely its executable bit (closing an RX-segment-granted-
    write, i.e. "RX->RWX", downgrade that previously changed nothing
    this digest recorded), (b) the raw ELF header e_entry value
    (closing a redirected-execution-address tamper), and (c) the
    canonical, append-tolerant record of every RETAINED PT_LOAD
    segment's own flags/filesz/memsz/align plus relative mapping
    topology and stable offset/paddr bias terms (see
    _canonical_load_segment_records()'s own docstring for the full
    rationale) -- closing the remaining gap where a normal segment's
    own memsz/align/filesz/offset/paddr mapping could still mutate
    without changing any section-keyed record. Returns None if `path`
    cannot be parsed as an ELF object at all, or has no loaded section
    whatsoever (vanishingly rare for a real bundled
    library/plugin/executable, but not itself an error).

    This is deliberately NOT the previous, narrower "PF_X PT_LOAD bytes
    only" digest: that scope left every byte of `.rodata`, `.data`,
    `.got`, `.init_array`/`.fini_array`, relocation records, and ELF
    note sections (build-id, ABI-tag, gnu-property) completely
    unauthenticated, so a hostile rewrite of e.g. a jump table, a GOT
    entry, an `.init_array` constructor pointer, or read-only
    configuration/string data driving this object's own behavior would
    have gone entirely undetected as long as it avoided the specific
    byte ranges already flagged executable. It is also deliberately NOT
    a whole-file byte comparison (patchelf legitimately rewrites
    RUNPATH/RPATH string content in `.dynstr`/`.dynamic` and, whenever
    the new string is longer than the placeholder it replaces, this
    project's own empirical testing against a real patchelf 0.14.3
    confirmed that this can relocate `.dynstr` -- together with whatever
    OTHER sections happened to be co-resident in its original PT_LOAD
    segment -- into a brand-new, appended PT_LOAD segment entirely,
    changing raw file offsets and segment counts/topology even though no
    OTHER section's own content changed at all). Content is hashed
    keyed BY SECTION NAME (sorted, not by file offset or segment index)
    specifically so that this kind of legitimate, patchelf-driven
    relocation of surviving sections into a different segment/file
    position can never itself change the digest, while any actual
    content or permission tampering still does.

    Including each retained section's own ELF type/flags AND whether its
    actual runtime (LOAD-segment) mapping is ever executable (not merely
    its content bytes) is what additionally satisfies detecting a
    "segment header mutation" -- e.g. an attacker flipping a PT_LOAD
    program header's own PF_X bit to make a segment containing
    `.rodata` executable, without touching any section's content or its
    own, independent sh_flags field at all. Since the loader only ever
    consults program headers (never section headers) to set page
    permissions, this is the only signal that can catch that specific
    class of mutation. Deliberately only the EXECUTABLE bit, never the
    full R/W/E triple: this project's own empirical testing against a
    real patchelf 0.14.3 rpath rewrite confirmed that sections swept
    into a newly-appended relocation segment (e.g. `.gnu.hash`,
    `.note.*`) can legitimately gain a WRITE permission they never
    previously had, with no security consequence, whereas a section
    becoming newly EXECUTABLE that never was is the one direction of
    tampering that is always genuine and must always be detected.

    `.dynamic` and `.dynstr` are excluded from this raw, byte-level pass
    entirely (their physical layout can legitimately shift, exactly as
    described above) -- but that does not mean every OTHER dynamic-
    linking directive goes unauthenticated: every decoded `.dynamic` tag
    (DT_NEEDED, DT_INIT, DT_FINI, DT_SONAME, DT_FLAGS, and so on) EXCEPT
    the RPATH/RUNPATH tags themselves is separately folded into this
    same digest via _read_dynamic_tags()'s decoded, layout-independent
    view, so a redirected DT_NEEDED SONAME or a hijacked DT_INIT/DT_FINI
    function pointer is still detected. `.dynsym` is likewise excluded
    from the raw pass (see _CANONICAL_DIGEST_EXCLUDED_SECTIONS' own
    docstring for why its raw st_shndx fields are not stable across a
    legitimate repack) and is instead folded in via
    _read_dynamic_symbols()'s own decoded, index-independent view, so a
    hijacked/redirected dynamic symbol is still detected too. `.symtab`
    (present only on an unstripped object) is excluded and re-folded
    the identical way, via _read_symtab_symbols(), for the identical
    st_shndx-renumbering reason; `.strtab` is excluded alongside it
    exactly as `.dynstr` is excluded alongside `.dynsym`.

    A real cumulative-review regression, found only against this
    project's own actual produced AppImage (never reproduced by this
    module's own synthetic unit tests, which do not exercise a real
    patchelf rewrite large enough to trigger it): a SHT_NOBITS section
    (`.bss`/`.tbss`) has NO real file content at all -- its own
    sh_offset is simply wherever a subsequent section with real content
    would have started, not a pointer to reserved storage -- so its
    content is never actually read here (see the inline comment at the
    `.bss`-skipping `if` below for the full explanation); only its
    header identity (name/type/flags/effective runtime executable-bit/
    declared size) is authenticated, exactly as for every other
    section.

    Every dynamic-tag pointer value resolved by
    _resolve_dynamic_tag_value() (folded in below) preserves the
    OFFSET within its target section, not merely the section's own
    name -- see that function's own docstring for why a
    section-name-only resolution would miss a DT_INIT/DT_FINI/
    DT_INIT_ARRAY retarget that stays within the same section. Every
    non-LOAD program header (PT_GNU_STACK's own executable-stack
    permission, PT_GNU_RELRO's own protected-range size, PT_TLS's own
    template size/alignment, and so on -- see
    _non_load_program_header_records()'s own preceding module comment)
    is likewise folded in below, since none of these carries a
    corresponding section at all and was therefore previously
    completely unauthenticated.

    Round-N+ review (HIGH, "canonical ELF identity misses ELF class/
    data/type/OSABI/machine"): `path`'s own ELF header class/data/
    OS-ABI/type/machine fields (see _read_elf_header_identity()'s own
    docstring for why every one of these is safe to fold in
    unconditionally, with zero legitimate-repack false-positive risk)
    are now also authenticated, closing a whole-object substitution
    class (e.g. a different-architecture or different-object-type
    replacement that coincidentally reuses a similar section/segment
    layout) nothing else in this digest could ever see on its own."""
    try:
        header_identity = _read_elf_header_identity(path)
    except ElfIdentityError:
        return None
    if header_identity is None:
        return None
    try:
        sections = _read_section_headers(path)
    except ElfIdentityError:
        return None
    segment_flags = _section_to_segment_load_flags(path)
    loaded = [
        section
        for section in sections
        if "A" in section["flags"]
        and section["name"] not in _CANONICAL_DIGEST_EXCLUDED_SECTIONS
    ]
    if not loaded:
        return None
    digest = hashlib.sha256()
    for field_name in ("class", "data", "osabi", "type", "machine"):
        digest.update(f"ELFHDR\0{field_name}\0{header_identity[field_name]}\0".encode("utf-8"))
    try:
        with path.open("rb") as handle:
            for section in sorted(loaded, key=lambda entry: entry["name"]):
                name = section["name"]
                offset = int(section["offset"], 16)
                size = int(section["size"], 16)
                # Only the EXECUTABLE bit of the section's actual
                # runtime (LOAD-segment) mapping is authenticated here,
                # not the full R/W/E flag string -- see this function's
                # own docstring for why: this project's own empirical
                # testing against a real patchelf 0.14.3 rpath rewrite
                # confirmed that sections swept into a newly-appended
                # relocation segment (e.g. `.gnu.hash`, `.note.*`) can
                # legitimately gain a WRITE permission they never
                # previously had, with no security consequence (nothing
                # here executes, and W-only metadata is not the
                # kernel's actual W^X security boundary), whereas a
                # section becoming newly EXECUTABLE that never was is
                # the one direction of program-header tampering that is
                # always a genuine, always-detectable privilege change.
                # `.gnu.hash`/`.note.*` are excluded from even this
                # single-bit check -- see
                # _has_volatile_segment_executable_bit()'s own
                # docstring: the SAME relocation can legitimately swing
                # their executable bit in EITHER direction (including
                # LOSING it, not just gaining a write bit), which a
                # standalone per-file digest cannot distinguish from
                # genuine tampering.
                # Round-N+ review (HIGH, "canonical ELF identity misses
                # load security/mapping ... RX->RWX stays true and
                # passes"): the WRITE bit of the section's actual
                # runtime (LOAD-segment) mapping is now ALSO
                # authenticated here, not merely the executable bit --
                # closing the exact gap the review named: an attacker
                # granting WRITE permission to an already-executable
                # section (turning a legitimate RX mapping into a
                # dangerous RWX one) previously changed nothing this
                # digest recorded at all, since only the bare executable
                # boolean was ever folded in. `.gnu.hash`/`.note.*`
                # remain fully excluded from BOTH bits, for the
                # identical, already-documented reason
                # _has_volatile_segment_executable_bit()'s own docstring
                # gives: the same legitimate relocation that can swing
                # their executable bit in either direction can equally
                # swing their write bit, and neither one ever executes,
                # so W^X is not a real security boundary for them either
                # way.
                if _has_volatile_segment_executable_bit(name):
                    runtime_executable = "<volatile, excluded>"
                    runtime_writable = "<volatile, excluded>"
                else:
                    runtime_executable = "E" in segment_flags.get(name, "")
                    runtime_writable = "W" in segment_flags.get(name, "")
                header = (
                    f"{name}\0{section['type']}\0{section['flags']}\0"
                    f"{runtime_executable}\0{runtime_writable}\0{size}\0"
                ).encode("utf-8")
                digest.update(header)
                # A SHT_NOBITS section (`.bss`/`.tbss`) is, by the ELF
                # spec itself, defined to occupy NO file space at all:
                # its own sh_offset is merely wherever the next
                # sequential section WOULD have started had it had real
                # content, not a pointer to any actually-reserved
                # storage for it. Reading sh_size bytes from that offset
                # therefore does not read this section's own content at
                # all (there is none on disk to read) -- it reads
                # whatever unrelated bytes a LATER, real section happens
                # to occupy at that same file position. A real,
                # empirically confirmed cumulative-review regression:
                # this project's own actual produced AppImage's
                # linuxdeploy-patchelf run occasionally needs a full
                # section-table rewrite (observed for
                # plugins/tls/libqopensslbackend.so and
                # qml/.../FluentWinUI3/libqtquickcontrols2fluentwinui3
                # styleplugin.so specifically, both bundled genuinely
                # unmodified in substance), which moves several
                # sections earlier in the file to new positions after
                # `.bss` -- changing the accidental bytes previously
                # read at `.bss`'s own sh_offset even though `.bss`
                # itself, having no content, could not possibly have
                # changed. Every OTHER field of this section's own
                # identity (name/type/flags/effective runtime
                # executable-bit/declared size, all already folded into
                # `header` above) is still fully authenticated -- only
                # the meaningless, always-implicitly-zero-initialized
                # "content" is skipped, for NOBITS sections exclusively.
                if section["type"] != "NOBITS":
                    handle.seek(offset)
                    remaining = size
                    while remaining > 0:
                        chunk = handle.read(min(remaining, 1024 * 1024))
                        if not chunk:
                            break
                        digest.update(chunk)
                        remaining -= len(chunk)
    except OSError:
        return None
    try:
        dynamic_tags = _read_dynamic_tags(path)
    except ElfIdentityError:
        dynamic_tags = []
    resolved_tags = sorted(
        (tag, _resolve_dynamic_tag_value(value, sections))
        for tag, value in dynamic_tags
        if tag not in _DYNAMIC_TAGS_EXCLUDED_FROM_DIGEST
    )
    for tag, resolved_value in resolved_tags:
        digest.update(f"DYNAMIC\0{tag}\0{resolved_value}\0".encode("utf-8"))
    for symbol in sorted(_read_dynamic_symbols(path, sections)):
        digest.update(("DYNSYM\0" + "\0".join(symbol) + "\0").encode("utf-8"))
    for symbol in sorted(_read_symtab_symbols(path, sections)):
        digest.update(("SYMTAB\0" + "\0".join(symbol) + "\0").encode("utf-8"))
    # Third-HIGH-round review ("... executable GNU_STACK, RELRO/TLS/
    # load offsets/sizes can pass with retained build ID"): every
    # non-LOAD/DYNAMIC/NOTE program header (PT_GNU_STACK's own
    # executable-stack flag, PT_GNU_RELRO's own protected range size,
    # PT_TLS's own template size/alignment, and so on) is folded in
    # here -- see _non_load_program_header_records()'s own preceding
    # module comment for exactly why PT_LOAD/PT_DYNAMIC/PT_NOTE must
    # stay excluded from this raw record while every OTHER segment type
    # must not.
    for type_, flags, filesz, memsz, align in _non_load_program_header_records(path):
        digest.update(
            f"PHDR\0{type_}\0{flags}\0{filesz}\0{memsz}\0{align}\0".encode("utf-8")
        )
    # Round-N+ review (HIGH, "canonical ELF identity misses load
    # security/mapping ... e_entry ... load addresses/ranges/order/
    # overlap pass"), plus the later cumulative re-review ("actual
    # PT_LOAD mappings omitted"): the raw entry-point virtual address
    # (stable across a legitimate patchelf rewrite -- see
    # _read_entry_point()'s own docstring) closes a redirected-
    # execution gap nothing else here authenticated; the canonical,
    # append-tolerant PT_LOAD records (see
    # _canonical_load_segment_records()'s own docstring for exactly
    # why their relative/bias representation stays stable across a
    # legitimate trailing append while still detecting a mutated
    # normal segment's own memsz/align/filesz/offset/paddr/topology)
    # close the whole-segment reorder/insertion/removal/range/overlap/
    # permission-change gaps the per-section write/execute-bit check
    # above cannot, since that check is keyed by section name and says
    # nothing about the segment TOPOLOGY itself.
    entry_point = _read_entry_point(path)
    digest.update(f"ENTRY\0{entry_point}\0".encode("utf-8"))
    for record in _canonical_load_segment_records(path):
        digest.update(("LOADSEG\0" + "\0".join(record) + "\0").encode("utf-8"))
    return digest.hexdigest()


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def elf_identity(path: Path) -> dict[str, str | None]:
    """The full cryptographic/provenance identity of one bundled ELF
    object, for SBOM inventory purposes: its whole-file sha256 (detects
    ANY byte-level substitution of the final shipped artifact, however
    produced; always computable via pure Python, never fails), its own
    build-id (honestly-labeled provenance metadata only -- see
    _read_build_id()'s updated docstring for why this is NOT a proof of
    authenticity by itself), its own canonicalLoadDigest (the real
    cryptographic proof over every section actually mapped into memory
    at runtime, plus each section's own runtime permissions -- see
    _canonical_load_digest()'s own docstring for exactly what this
    covers and why it is stable across this project's own legitimate
    patchelf/strip repackaging while still detecting any real content or
    permission tampering, unlike a bare build-id), and its own DT_SONAME
    (its own declared logical library name/version, which for a
    versioned shared object may differ from its basename on disk).

    buildId/canonicalLoadDigest/soname are each independently None
    whenever they cannot be supplied -- whether because the object
    legitimately has none (not every object has a loaded/SHF_ALLOC
    section, not every plugin declares a SONAME, and a toolchain not
    passing `--build-id` produces no build-id note), because readelf
    itself is not installed, or because `path` is not parseable as an
    ELF object at all. This is deliberately never a hard failure: an
    SBOM inventory listing every final bundled ELF (this function's own
    purpose, per review directive: "never omit") must not itself go
    unproduced merely because one detail about one entry could not be
    determined in the current environment -- the sha256 field alone
    already gives every entry a verifiable, always-present identity, and
    a None field is visible, honestly-reported information in the
    manifest, not a silently dropped entry."""
    try:
        build_id = _read_build_id(path)
    except ElfIdentityError:
        build_id = None
    try:
        soname = _read_soname(path)
    except ElfIdentityError:
        soname = None
    return {
        "sha256": _sha256(path),
        "buildId": build_id,
        "canonicalLoadDigest": _canonical_load_digest(path),
        "soname": soname,
    }


# Mirrors packaging/audit_dependency_closure.py's own ABI_ALLOWLIST --
# duplicated (not imported) so a change to one script's allowlist can never
# silently widen the other's. These are assumed present on every
# glibc-based x86_64 Linux host an AppImage might run on, and therefore
# need no bundled notice even if a copy happens to be present in the AppDir.
ABI_ALLOWLIST: frozenset[str] = frozenset(
    {
        "ld-linux-x86-64.so.2",
        "linux-vdso.so.1",
        "libc.so.6",
        "libm.so.6",
        "libdl.so.2",
        "libpthread.so.0",
        "librt.so.1",
        "libresolv.so.2",
        "libutil.so.1",
    }
)

# Ordered (pattern, component) list. First match wins. Every component name
# here MUST have a corresponding third_party/<component>/ directory with at
# least one real, non-empty notice/license file, or verify-notices below (and
# bundle_codec_notices.sh, which consumes this same classification) fails.
#
# Library "families" that legitimately ship many similarly-prefixed shared
# objects (Abseil, Qt itself) are matched by prefix rather than an
# exhaustive fixed name list, since the exact member set/count varies by
# upstream version -- see third_party/abseil/NOTICE.md and
# third_party/qt/NOTICE.md for why.
COMPONENT_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (re.compile(r"^libavif\.so"), "libavif"),
    (re.compile(r"^libdav1d\.so"), "dav1d"),
    (re.compile(r"^libaom\.so"), "libaom"),
    (re.compile(r"^libgav1\.so"), "libgav1"),
    (re.compile(r"^librav1e\.so"), "rav1e"),
    (re.compile(r"^libSvtAv1.*\.so"), "svt-av1"),
    (re.compile(r"^libyuv\.so"), "libyuv"),
    # SharpYuv is libwebp's own standalone YUV<->RGB helper library
    # (a genuinely separate upstream project from Google's libyuv above,
    # despite the confusingly similar name) that libavif's bundled
    # closure transitively pulls in -- see third_party/sharpyuv/NOTICE.md.
    (re.compile(r"^libsharpyuv\.so"), "sharpyuv"),
    (re.compile(r"^libjpeg\.so"), "libjpeg"),
    # bzip2 is pulled in transitively by Qt/util-linux's own closure --
    # see third_party/bzip2/NOTICE.md.
    (re.compile(r"^libbz2\.so"), "bzip2"),
    (re.compile(r"^libabsl_.*\.so"), "abseil"),
    (re.compile(r"^libQt6.*\.so"), "qt"),
    # QtKeychain's dlopen()-only D-Bus Secret Service backend (see
    # build-appimage.sh's find_bundled_libsecret) and its own transitive
    # ELF-linked closure -- force-bundled explicitly (libsecret,
    # libgpg-error, libgcc_s, libstdc++, zlib) or pulled in automatically
    # by linuxdeploy's ldd-based resolution (libgcrypt, glib/gobject/gio,
    # libffi, pcre2, util-linux's libmount/libblkid/libuuid, libselinux,
    # liblzma). Every one of these was previously entirely unaudited by
    # this script's predecessor -- see the individual
    # third_party/<name>/NOTICE.md files for exactly why each is bundled.
    (re.compile(r"^libsecret-1\.so"), "libsecret"),
    (re.compile(r"^libgpg-error\.so"), "libgpg-error"),
    (re.compile(r"^libgcrypt\.so"), "libgcrypt"),
    (re.compile(r"^libffi\.so"), "libffi"),
    (re.compile(r"^lib(glib|gobject|gio|gmodule|gthread)-2\.0\.so"), "glib"),
    (re.compile(r"^libz\.so"), "zlib"),
    (re.compile(r"^libpcre2-.*\.so"), "pcre2"),
    (re.compile(r"^lib(mount|blkid|uuid)\.so"), "util-linux"),
    (re.compile(r"^libselinux\.so"), "libselinux"),
    (re.compile(r"^liblzma\.so"), "liblzma"),
    (re.compile(r"^lib(gcc_s|stdc\+\+)\.so"), "gcc-runtime"),
    # A second, larger wave of previously-unmapped libraries found by a
    # later cumulative review running this same classifier against the
    # real produced AppImage's full recursive `usr/` closure (not just
    # `usr/lib`, which the original round-4 item-12 work above happened
    # to be validated against): the xcb/X11/xkbcommon family Qt's xcb
    # platform plugin transitively needs, QtKeychain's own bundled
    # `libqt6keychain.so*` (previously only its *dependencies* -- e.g.
    # libsecret -- were mapped, never the library itself), and a further
    # round of glibc-adjacent system libraries (Kerberos/GSSAPI, ICU,
    # keyutils, systemd, lz4/zstd/brotli, legacy PCRE1, libpng, libcap,
    # D-Bus, libbsd/libmd) that distribution-specific glib/D-Bus/Qt
    # builds transitively pull in. See the individual
    # third_party/<name>/NOTICE.md files for exactly why each is bundled.
    (re.compile(r"^libXau\.so"), "libxau"),
    (re.compile(r"^libXdmcp\.so"), "libxdmcp"),
    # A later cumulative review found the single wildcard `libxcb.*\.so`
    # pattern previously here was factually wrong: it silently conflated
    # base libxcb (and the built-in protocol-extension libraries built
    # from libxcb's OWN single source repository, listed explicitly by
    # exact extension name below) with FIVE further `libxcb-*` libraries
    # that are each their own genuinely SEPARATE upstream git
    # repository/project (xcb-util, xcb-util-image, xcb-util-keysyms,
    # xcb-util-renderutil, xcb-util-wm) with their OWN, differently
    # dated copyright holders -- see third_party/xcb/NOTICE.md and the
    # sibling third_party/xcb-util*/NOTICE.md files for the exact
    # per-project copyright text this split now correctly attributes.
    # Deliberately listed as an explicit, closed set (never a `libxcb.*`
    # wildcard) so a hypothetical future `libxcb-something-new.so` this
    # list has no entry for is reported unmapped and fails packaging,
    # per this same review's "unknown binary must fail" requirement,
    # rather than being silently (and incorrectly) folded into "xcb".
    (re.compile(r"^libxcb\.so"), "xcb"),
    (
        re.compile(
            r"^libxcb-(glx|randr|render|shape|shm|sync|xfixes|xkb|dri2|dri3"
            r"|present|res|screensaver|xf86dri|xinerama|xtest|xv|xvmc)\.so"
        ),
        "xcb",
    ),
    (re.compile(r"^libxcb-util\.so"), "xcb-util"),
    (re.compile(r"^libxcb-image\.so"), "xcb-util-image"),
    (re.compile(r"^libxcb-keysyms\.so"), "xcb-util-keysyms"),
    (re.compile(r"^libxcb-render-util\.so"), "xcb-util-renderutil"),
    (re.compile(r"^libxcb-(icccm|ewmh)\.so"), "xcb-util-wm"),
    (re.compile(r"^libxcb-cursor\.so"), "xcb-util-cursor"),
    (re.compile(r"^libxkbcommon(-x11)?\.so"), "xkbcommon"),
    (re.compile(r"^libbrotli(common|dec|enc)\.so"), "brotli"),
    (re.compile(r"^libbsd\.so"), "libbsd"),
    (re.compile(r"^libmd\.so"), "libmd"),
    (re.compile(r"^libcap\.so"), "libcap"),
    (re.compile(r"^libdbus-1\.so"), "dbus"),
    (
        re.compile(r"^lib(krb5support|gssapi_krb5|k5crypto|krb5)\.so"),
        "krb5",
    ),
    # Round-9+ review item 11: libcom_err.so.2 was previously folded into
    # the "krb5" pattern/component above on the mistaken assumption that
    # it is part of MIT Kerberos 5. On the actual distribution this
    # project targets (Ubuntu 22.04 "Jammy"), libcom_err.so.2 is built
    # and shipped by the libcom-err2 binary package, whose SOURCE package
    # is e2fsprogs -- an entirely separate upstream project (the
    # ext2/3/4 filesystem utilities) with its own distinct package
    # name/version/copyright/license text, coincidentally also MIT-style
    # and also traceable to an MIT entity, but never the same license
    # text nor the same package as MIT Kerberos 5 itself. See
    # third_party/e2fsprogs/NOTICE.md for the full explanation and exact
    # package/version/license citation. Matched as its own, independent
    # pattern -- deliberately never folded back into the krb5 pattern
    # above -- so it always maps to its own correctly-attributed
    # component.
    (re.compile(r"^libcom_err\.so"), "e2fsprogs"),
    (re.compile(r"^libicu(data|i18n|uc)\.so"), "icu"),
    (re.compile(r"^libkeyutils\.so"), "libkeyutils"),
    (re.compile(r"^liblz4\.so"), "lz4"),
    (re.compile(r"^libpcre\.so"), "pcre"),
    (re.compile(r"^libpng(1[0-9])?\.so"), "libpng"),
    (re.compile(r"^libqt6keychain\.so"), "qtkeychain"),
    (re.compile(r"^libsystemd\.so"), "systemd"),
    (re.compile(r"^libzstd\.so"), "zstd"),
]

MANDATORY_COMPONENTS: frozenset[str] = frozenset({"libavif"})

# Round-N+ review (MEDIUM, "libcom_err provenance/version inferred
# basename; CI apt unpinned, notice hardcodes Jammy 1.46.5-2ubuntu1.1
# while updates may .2"): rather than hardcoding one exact package
# VERSION string in a NOTICE.md file (which a routine, entirely
# legitimate Ubuntu security update to this project's own pinned
# `ubuntu-22.04` CI runner -- see .github/workflows/ci.yml's
# `appimage-smoke` job -- would silently make stale the moment a point
# release ships, with nothing to ever catch the drift), this maps each
# distro-packaged COMPONENT_PATTERNS component to the set of Debian
# SOURCE package name(s) it is expected to actually originate from on
# this project's real, pinned Ubuntu 22.04 build/audit environment.
# capture_package_provenance()/validate_component_package_provenance()
# below use this to authenticate, at real build/CI time, the ACTUAL
# `dpkg-query`-derived source package of the real system copy of each
# bundled distro library -- catching a wrong owner/version/component
# mapping outright -- while never needing this table itself edited
# merely because Ubuntu shipped a routine point-release version bump
# (source package NAMES are stable across such updates; only the
# version, which this table deliberately does not pin, changes).
#
# Every entry below was independently verified against a real,
# unmodified `ubuntu:22.04` container (matching the exact `ubuntu-22.04`
# CI runner image this project's own `appimage-smoke` job is pinned to)
# via `dpkg -S <realpath>` (falling back to the merged-/usr-stripped
# path when dpkg's own database predates the merge -- see
# _dpkg_owning_package()'s own docstring) followed by
# `dpkg-query -W -f='${source:Package}'`, for every actual bundled
# library this project's own real dependency closure pulls in --
# including libavif's own real, live ldd-resolved AV1 backend closure
# (dav1d/libgav1/libaom/libyuv/abseil), not a guessed/hypothetical one.
# Components with NO entry here (qt, qtkeychain -- never distro apt
# packages in this project's pipeline at all -- svt-av1/rav1e/
# sharpyuv, which the real, live Ubuntu 22.04 `libavif13` build does not
# even dynamically link, per real `ldd` output -- and icu, see
# _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE's own docstring for why)
# are simply never validated by this mechanism; that is a deliberate,
# honestly-scoped no-op for a component this table cannot make any true
# claim about, never a silent false negative for one it can.
COMPONENT_EXPECTED_SOURCE_PACKAGES: dict[str, frozenset[str]] = {
    "e2fsprogs": frozenset({"e2fsprogs"}),
    "krb5": frozenset({"krb5"}),
    "libsecret": frozenset({"libsecret"}),
    "libgpg-error": frozenset({"libgpg-error"}),
    "libgcrypt": frozenset({"libgcrypt20"}),
    "libffi": frozenset({"libffi"}),
    "glib": frozenset({"glib2.0"}),
    "zlib": frozenset({"zlib"}),
    "pcre2": frozenset({"pcre2"}),
    "util-linux": frozenset({"util-linux"}),
    "libselinux": frozenset({"libselinux"}),
    "liblzma": frozenset({"xz-utils"}),
    "libxau": frozenset({"libxau"}),
    "libxdmcp": frozenset({"libxdmcp"}),
    "xcb": frozenset({"libxcb"}),
    "xcb-util": frozenset({"xcb-util"}),
    "xcb-util-image": frozenset({"xcb-util-image"}),
    "xcb-util-keysyms": frozenset({"xcb-util-keysyms"}),
    "xcb-util-renderutil": frozenset({"xcb-util-renderutil"}),
    "xcb-util-wm": frozenset({"xcb-util-wm"}),
    "xcb-util-cursor": frozenset({"xcb-util-cursor"}),
    "xkbcommon": frozenset({"libxkbcommon"}),
    "brotli": frozenset({"brotli"}),
    "libbsd": frozenset({"libbsd"}),
    "libmd": frozenset({"libmd"}),
    "libcap": frozenset({"libcap2"}),
    "dbus": frozenset({"dbus"}),
    # "icu" is deliberately NOT listed here: see
    # _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE's own docstring below --
    # its real, verifiable provenance in this project's actual pipeline
    # is the Qt SDK's own bundled copy, authenticated by
    # bind_bundled_library_to_qt_sdk_provenance() instead, never a dpkg
    # package.
    "libkeyutils": frozenset({"keyutils"}),
    "lz4": frozenset({"lz4"}),
    "pcre": frozenset({"pcre3"}),
    "libpng": frozenset({"libpng1.6"}),
    "systemd": frozenset({"systemd"}),
    # Ubuntu 22.04 "Jammy"'s libzstd1 binary package is built from a
    # source package literally named "libzstd" (not "zstd" -- there is
    # no bare "zstd" source package in this release at all). Confirmed
    # via `dpkg -S`/`dpkg-query -W -f='${source:Package}'` against a
    # real, unmodified ubuntu:22.04 container.
    "zstd": frozenset({"libzstd"}),
    "libjpeg": frozenset({"libjpeg-turbo"}),
    "bzip2": frozenset({"bzip2"}),
    "libavif": frozenset({"libavif"}),
    "libaom": frozenset({"aom"}),
    "libyuv": frozenset({"libyuv"}),
    "dav1d": frozenset({"dav1d"}),
    "libgav1": frozenset({"libgav1"}),
    "abseil": frozenset({"abseil"}),
    # gcc-12 is Ubuntu 22.04 "Jammy"'s own current default GCC source
    # package name; kept as its own frozenset (rather than a bare
    # literal) so a deliberate future addition (e.g. if this project's
    # pinned CI image is ever intentionally moved to a newer Ubuntu with
    # a renamed gcc-NN source package) is a clearly-reviewable one-line
    # diff here, not a silent behavior change.
    "gcc-runtime": frozenset({"gcc-12"}),
}

# Round-7 review (HIGH, "package versions are mutable/unpinned...pin
# package revision/snapshot or governed expectations"), and its own
# Round-N+ follow-up (HIGH, "`startswith(expected_version_prefix)` lets
# 1.46.50 satisfy 1.46.5; security revision drift... apt sources
# mutable/unversioned... Pin an immutable Ubuntu snapshot plus exact
# binary/source versions... Missing/wrong/same-name/revision drift
# fails"): the PREVIOUS design here
# (COMPONENT_EXPECTED_SOURCE_VERSION_PREFIX, pinning only the leading
# upstream-version PREFIX and matching via `str.startswith()`) had two
# concrete, exploitable gaps this table and its equality-only comparator
# (see validate_bundled_library_package_provenance()'s own use of
# `==`, never `startswith()`, below) both close:
#
#   1. A version-prefix COLLISION: `"1.46.50-1"[:len("1.46.5")]` also
#      equals the literal string "1.46.5", so `startswith("1.46.5")` was
#      satisfied by an entirely different, unreviewed upstream version
#      ("1.46.50") that merely happens to share the pinned prefix as a
#      literal substring -- never a real point release of "1.46.5" at
#      all. Pinning (and comparing) the COMPLETE, real
#      `dpkg-query`-reported version string (including its own
#      `debian_revision` suffix) makes this class of collision
#      structurally impossible: two distinct upstream/Debian-revision
#      combinations can never produce the identical complete string.
#
#   2. Silent SECURITY-REVISION DRIFT: the previous design deliberately
#      tolerated ANY `debian_revision`-only bump (e.g. "-2ubuntu1.1" ->
#      "-2ubuntu1.2") as "routine", which is exactly backwards for a
#      component whose provenance is meant to be a GOVERNED, reviewed
#      lock -- a live Ubuntu security update silently changing the
#      exact bytes this project ships underneath an assumed-safe prefix
#      is precisely the kind of drift a provenance pin exists to catch,
#      not exempt. Every value below is therefore now the COMPLETE
#      `dpkg-query --showformat='${Version}'` string, compared for
#      EXACT equality; ANY drift at all -- upstream or Debian-revision,
#      security patch or not -- is a real, reviewable diff that must be
#      deliberately re-pinned here, never silently absorbed. This IS
#      this project's own "equivalent governed lock verified before
#      packaging" (the review's own explicitly offered alternative to
#      an immutable archive-snapshot pin): the audit refuses to accept
#      ANY version this table was not deliberately updated to name,
#      regardless of which apt mirror/snapshot the installing host
#      happened to resolve it from.
#
# Third-HIGH-round review ("Other distro components accept mutable
# repo versions by source-package name ... Pin an immutable Ubuntu
# snapshot plus exact binary/source versions ... for EVERY distro
# input"): every entry below (not merely e2fsprogs, this table's
# original sole entry) was independently, freshly re-confirmed against
# a genuine, unmodified `ubuntu:22.04` container (with Jammy's
# `universe` component enabled, matching this project's own real CI
# `add-apt-repository universe`-equivalent package availability) --
# specifically:
#   docker run --rm ubuntu:22.04 bash -c '
#     apt-get update -qq &&
#     apt-get install -y -qq software-properties-common &&
#     add-apt-repository -y universe && apt-get update -qq &&
#     apt-get install -y --no-install-recommends \
#       <every COMPONENT_EXPECTED_SOURCE_PACKAGES binary package> &&
#     dpkg-query --showformat="${Package}\t${Version}\t${source:Package}\n" \
#       --show <every package>'
# -- the exact same base image this project's own real CI (see
# .github/workflows/ci.yml's `apt-get install ... libavif-dev
# libjpeg-turbo8-dev ...` steps, which install straight from Jammy's
# default `main`/`universe` archive with no PPA/backport of any kind)
# resolves these binary packages from. A component with no entry here
# (there are none remaining after this round) would be validated only
# by source-package name (COMPONENT_EXPECTED_SOURCE_PACKAGES), as
# before -- itself a deliberate, honestly-scoped no-op, never a silent
# false negative for one this table *does* cover. Re-confirm every
# value here the same way whenever this project's own pinned CI image
# tag is deliberately moved to a newer Ubuntu release, or whenever a
# deliberate, reviewed security-update re-pin is required.
COMPONENT_EXPECTED_SOURCE_VERSION: dict[str, str] = {
    "e2fsprogs": "1.46.5-2ubuntu1.2",
    "krb5": "1.19.2-2ubuntu0.8",
    "libsecret": "0.20.5-2",
    "libgpg-error": "1.43-3",
    "libgcrypt": "1.9.4-3ubuntu3.2",
    "libffi": "3.4.2-4",
    "glib": "2.72.4-0ubuntu2.9",
    "zlib": "1:1.2.11.dfsg-2ubuntu9.2",
    "pcre2": "10.39-3ubuntu0.1",
    "util-linux": "2.37.2-4ubuntu3.5",
    "libselinux": "3.3-1build2",
    "liblzma": "5.2.5-2ubuntu1.1",
    "libxau": "1:1.0.9-1build5",
    "libxdmcp": "1:1.1.3-0ubuntu5",
    "xcb": "1.14-3ubuntu3",
    "xcb-util": "0.4.0-1build2",
    "xcb-util-image": "0.4.0-2",
    "xcb-util-keysyms": "0.4.0-1build3",
    "xcb-util-renderutil": "0.3.9-1build3",
    "xcb-util-wm": "0.4.1-1.1build2",
    "xcb-util-cursor": "0.1.1-4ubuntu1",
    "xkbcommon": "1.4.0-1",
    "brotli": "1.0.9-2build6",
    "libbsd": "0.11.5-1",
    "libmd": "1.0.4-1build1",
    "libcap": "1:2.44-1ubuntu0.22.04.3",
    "dbus": "1.12.20-2ubuntu4.1",
    "libkeyutils": "1.6.1-2ubuntu3",
    "lz4": "1.9.3-2build2",
    "pcre": "2:8.39-13ubuntu0.22.04.1",
    "libpng": "1.6.37-3ubuntu0.6",
    "systemd": "249.11-0ubuntu3.22",
    "zstd": "1.4.8+dfsg-3build1",
    "libjpeg": "2.1.2-0ubuntu1",
    "bzip2": "1.0.8-5build1",
    "libavif": "0.9.3-3",
    "libaom": "3.3.0-1ubuntu0.1",
    "libyuv": "0.0~git20220104.b91df1a-2",
    "dav1d": "0.9.2-1",
    "libgav1": "0.17.0-1build1",
    "abseil": "0~20210324.2-2ubuntu0.3",
    "gcc-runtime": "12.3.0-1ubuntu1~22.04.3",
}

# Deliberately x86_64-only (this project's whole AppImage target ABI --
# see ABI_ALLOWLIST/audit_dependency_closure.py's own X86_64 assumption)
# and deliberately NOT including any bundled AppDir path -- these are
# the standard locations a real Debian/Ubuntu system installs its own
# unmodified system libraries, searched here specifically to find the
# ORIGINAL, dpkg-owned system copy of a library this project's build
# process separately *bundles a copy of* into the AppDir, so that copy's
# real upstream package/version/source provenance can be authenticated
# against the live system dpkg database -- the bundled AppDir copy
# itself is never dpkg-owned (it is linuxdeploy's own plain file copy,
# possibly patchelf-rewritten) and therefore could never itself be
# looked up this way.
_SYSTEM_LIBRARY_SEARCH_DIRS: tuple[Path, ...] = (
    Path("/usr/lib/x86_64-linux-gnu"),
    Path("/lib/x86_64-linux-gnu"),
    Path("/usr/lib"),
    Path("/lib"),
)


# New review item ("ICU library package-provenance mismatch", found only
# once the AppImage-smoke job's "Verify every bundled ELF library ships
# its required license notice" step -- fixed to be reachable at all by
# two entirely unrelated earlier commits this same round -- finally ran
# to completion for the first time, against the real produced AppImage,
# on the real pinned `ubuntu-22.04` CI runner): "icu" is structurally
# different from every other COMPONENT_EXPECTED_SOURCE_PACKAGES entry.
# Every one of those was independently verified (see that table's own
# docstring) to be a real, ldd-resolved dependency this project's build
# picks up from the *live build host's own installed distro package* --
# but ICU is not: Qt's official prebuilt Linux SDK bundles its OWN copy
# of ICU directly inside `$QT_ROOT_DIR/lib/` (confirmed via a real CI
# "Package" step log: "Deploying shared library
# .../Qt/6.11.1/gcc_64/lib/libicudata.so.73", immediately followed by
# "WARNING: Could not find copyright files for file
# .../libicudata.so.73 using dpkg-query" -- linuxdeploy itself already
# knows this file is not dpkg-owned), at Qt's own pinned ICU version --
# specifically so Qt's `QLocale`/`QCollator` support never has a
# fragile runtime ABI dependency on whatever ICU version (if any) a
# given end-user's distro happens to ship. This project's own pinned
# `ubuntu-22.04` "Jammy" `appimage-smoke` runner ships ICU 70
# (`libicu70`), never the newer ICU 73 that Qt 6.11.1 bundles --
# there is structurally no dpkg-owned `libicudata.so.73` (etc.) this
# runner could ever have installed, at any point release, for
# capture_package_provenance()/bind_bundled_library_to_system_provenance()
# to compare against. Requiring dpkg provenance for "icu" can therefore
# never succeed on the very host `--require-package-provenance` is
# pinned to run on -- a previously-undiscovered latent bug, not a
# regression introduced by removing it here.
#
# ICU's *bundled-content* provenance is instead authenticated the exact
# same way this project already authenticates core Qt libraries and
# Qt's own plugins/QML modules (see _is_real_core_qt_library()/
# _is_real_qt_plugin_or_qml_module()): by proving, via
# _is_same_compiled_object_or_unwritten(), that the bundled file is the
# same compiled object as the real Qt SDK's own copy at the identical
# `lib/<basename>` relative path -- see
# bind_bundled_library_to_qt_sdk_provenance()/
# validate_bundled_library_qt_sdk_provenance() below. This is strictly
# MORE precise than the dpkg cross-check for this one component (it
# compares against the exact upstream artifact this project's own build
# genuinely obtained the file from, not merely "a same-named file some
# host happens to have installed"), so no security coverage is lost by
# excluding "icu" from COMPONENT_EXPECTED_SOURCE_PACKAGES above.
#
# Round-N+ review ("qtSdkProvenance included only ICU, not core
# Qt/plugins using same classification"): core Qt libraries and Qt's
# own plugins/QML modules (classify_path()'s "qt" component) were
# ALREADY authenticated against this exact same real Qt SDK reference
# copy at classification time (_is_real_core_qt_library()/
# _is_real_qt_plugin_or_qml_module(), above) -- classify_path() refuses
# to ever return "qt" for a file that fails that proof. But that proof
# was previously discarded immediately after use: only a bare boolean
# ever survived past classification, never surfaced as a structured,
# reviewable SBOM/provenance record the way "icu"'s dedicated
# qtSdkProvenance field already was. "qt" is added here so every
# "qt"-classified bundled file gets the identical structured
# qtSdkProvenance record (reference path/sha256/canonical digest) "icu"
# already did -- both are, correctly, never checked against
# COMPONENT_EXPECTED_SOURCE_PACKAGES/dpkg (see that table's own
# docstring: "qt" has never had an entry there either, for the same
# underlying reason -- neither ever originates from a dpkg-owned distro
# package in this project's real pipeline).
_COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE: frozenset[str] = frozenset({"icu", "qt"})


# The classic top-level directories that are, on every modern (usr-
# merged) Debian/Ubuntu release, pure symlinks into the equivalent
# `/usr/...` directory (`/lib -> usr/lib`, `/bin -> usr/bin`, etc.) --
# used by _dpkg_owning_package() below to generate the "add a `/usr`
# prefix" candidate for a path that does not already have one.
_USR_MERGE_TOP_LEVEL_DIRS: tuple[str, ...] = ("/lib", "/lib32", "/lib64", "/libx32", "/bin", "/sbin")


def _dpkg_owning_package(path: Path) -> str | None:
    """The name (with any `:architecture` qualifier stripped) of the
    dpkg-installed binary package that owns the real, existing file at
    `path` -- or None if `dpkg` itself is not installed (this function
    is only ever meaningfully exercised on a real Debian/Ubuntu system;
    a macOS/non-Debian development machine or a component that
    genuinely is not distro-packaged both fall through to this same,
    deliberately non-fatal None), or if no installed package owns this
    exact path.

    Round-N+ review (`appimage-smoke` regression: "16/176 resolved
    dependencies" captured, i.e. this project's own real distro
    packages -- libxau6, libglib2.0-0, libxcb1, ... -- were being
    reported as NOT dpkg-owned on a genuine, unmodified `ubuntu-22.04`
    CI runner): this function previously ONLY ever tried stripping a
    leading `/usr` prefix as a fallback, on the theory that dpkg's
    database always records the OLDER, pre-usr-merge form
    (`/lib/x86_64-linux-gnu/...`) while `ldd` reports the merged,
    canonical `/usr/lib/x86_64-linux-gnu/...` form. Directly reproducing
    this against a real, freshly `apt-get install`ed `ubuntu:22.04`
    container proved the OPPOSITE is what actually happens for the vast
    majority of real packages on this exact, currently pinned base
    image: `ldd` itself reports paths in the `/lib/x86_64-linux-gnu/...`
    compatibility-symlink form (that is literally the path baked into
    `/etc/ld.so.cache` by `ldconfig`), while dpkg's own package-contents
    database records the CANONICAL, merged `/usr/lib/x86_64-linux-gnu/
    ...` form for those same packages -- e.g. `dpkg -S
    /usr/lib/x86_64-linux-gnu/libXau.so.6` succeeds while `dpkg -S
    /lib/x86_64-linux-gnu/libXau.so.6` (the exact form `ldd` reports)
    fails outright. Ubuntu's usr-merge migration has happened
    incrementally, package by package, over several release cycles, so
    a small minority of packages' contents databases may still only
    know the OLDER pre-merge form (this project's own earlier testing
    against `libz1g` observed exactly that) -- there is no single
    universally correct direction. Rather than continuing to guess a
    single fixed direction, this function now tries EVERY plausible
    variant symmetrically: `path` exactly as given; with any leading
    `/usr` prefix stripped (covers dpkg recording the older pre-merge
    form for an already-canonical input); and, if `path` is not already
    `/usr`-prefixed but starts with one of the classic usr-merged
    top-level compatibility symlinks (`/lib`, `/bin`, `/sbin`, ...),
    with an explicit `/usr` prefix ADDED (covers dpkg recording the
    canonical merged form for an `ldd`-reported pre-merge-style input --
    the common case this regression surfaced). The first candidate any
    installed package actually owns wins; every one of these merely
    represents the same on-disk file (`/lib` is always purely a symlink
    to `usr/lib` on any of these systems), never a different file."""
    if shutil.which("dpkg") is None:
        return None
    candidates: list[Path] = [path]
    path_str = str(path)
    if path_str.startswith("/usr/"):
        stripped = path_str.removeprefix("/usr")
        if stripped and Path(stripped) not in candidates:
            candidates.append(Path(stripped))
    else:
        for merged_dir in _USR_MERGE_TOP_LEVEL_DIRS:
            if path_str == merged_dir or path_str.startswith(merged_dir + "/"):
                prefixed = Path("/usr" + path_str)
                if prefixed not in candidates:
                    candidates.append(prefixed)
                break
    for candidate in candidates:
        try:
            result = subprocess.run(
                ["dpkg", "-S", str(candidate)],
                capture_output=True,
                text=True,
            )
        except OSError:
            return None
        if result.returncode != 0 or not result.stdout.strip():
            continue
        owner = result.stdout.strip().splitlines()[0].split(":", 1)[0]
        return owner.split(":")[0]
    return None


def _dpkg_package_metadata(package: str) -> tuple[str, str] | None:
    """(installed version, source package name) for an installed dpkg
    binary `package` name, or None if `dpkg-query` is unavailable or the
    package is not actually installed (a defensive case that should
    never occur given this is only ever called with a package name
    `_dpkg_owning_package()` itself just reported as installed, but
    handled explicitly rather than assumed)."""
    if shutil.which("dpkg-query") is None:
        return None
    try:
        result = subprocess.run(
            [
                "dpkg-query",
                "--showformat=${Version}\t${source:Package}",
                "--show",
                package,
            ],
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    if result.returncode != 0 or "\t" not in result.stdout:
        return None
    version, source_package = result.stdout.split("\t", 1)
    if not version or not source_package:
        return None
    return version, source_package


def _dpkg_package_architecture(package: str) -> str | None:
    """Round-N+ review (HIGH, "distro provenance collapses identity ...
    arch stripped"): the dpkg-recorded Debian architecture (e.g.
    "amd64") of an installed binary `package` -- `_dpkg_owning_package()`
    above deliberately discards any `:architecture` qualifier `dpkg -S`
    itself reports (needed there only to normalize a multi-arch-
    qualified package NAME back into a plain one `dpkg-query --show`
    accepts), which previously meant no caller anywhere in this module
    ever recorded or verified a captured library's own real
    architecture at all -- a substituted i386/foreign-arch package
    sharing an otherwise-identical name/version string would have
    silently "matched". Returns None (never raises) under the exact
    same unavailability conditions `_dpkg_package_metadata()` does."""
    if shutil.which("dpkg-query") is None:
        return None
    try:
        result = subprocess.run(
            ["dpkg-query", "--showformat=${Architecture}", "--show", package],
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    architecture = result.stdout.strip()
    if result.returncode != 0 or not architecture:
        return None
    return architecture


# This project's own real, pinned CI/packaging pipeline only ever
# builds and packages for one Debian architecture -- see
# .github/workflows/ci.yml's `appimage-smoke` job, which runs entirely
# on a real, unmodified `ubuntu-22.04` x86_64 GitHub-hosted runner with
# no cross-architecture/multilib step anywhere in the pipeline. Any
# OTHER dpkg-reported architecture for a component this project expects
# to be a real distro package is therefore always a genuine anomaly
# (a foreign-arch/multilib substitute, or a misconfigured/cross-built
# host), never a legitimate variant this project's own pipeline could
# knowingly produce.
EXPECTED_DISTRO_PACKAGE_ARCHITECTURE: str = "amd64"


# Debian/Ubuntu top-level directories that "usrmerge" replaces with a
# symlink into the corresponding /usr/... directory. A real package's
# own .md5sums payload may legitimately record either the pre-merge
# form (most existing packages, built before their distro merged /usr)
# or the post-merge form (packages built directly targeting a merged
# root) for the exact same real, installed file -- both are honest,
# unmodified dpkg-recorded paths for the identical bytes.
_USRMERGE_TOP_LEVEL_DIRS: tuple[str, ...] = (
    "bin",
    "sbin",
    "lib",
    "lib32",
    "lib64",
    "libx32",
)


def _merged_usr_relative_path_candidates(path: Path) -> frozenset[str]:
    """Round-N+ Docker/real-dpkg validation fix: returns every root-
    relative path form (no leading "/") that a real Debian/Ubuntu
    package could honestly have recorded in its own .md5sums database
    for the exact same real file at `path` -- both the pre-merge
    ("lib/x86_64-linux-gnu/libz.so.1.2.11") and post-merge
    ("usr/lib/x86_64-linux-gnu/libz.so.1.2.11") spellings -- so a
    caller need not know or guess which form this particular package
    happened to use at build time. `path` should already be resolved
    (symlinks followed) by the caller; this function itself performs
    no filesystem access and never raises."""
    relative = str(path).lstrip("/")
    candidates = {relative}
    parts = relative.split("/", 1)
    if len(parts) == 2:
        head, rest = parts
        if head == "usr" and "/" in rest:
            merged_head, merged_rest = rest.split("/", 1)
            if merged_head in _USRMERGE_TOP_LEVEL_DIRS:
                candidates.add(f"{merged_head}/{merged_rest}")
        elif head in _USRMERGE_TOP_LEVEL_DIRS:
            candidates.add(f"usr/{relative}")
    return frozenset(candidates)


# A module-level constant (rather than an inline literal) purely so
# tests can redirect it to a real, temporary directory containing a
# hand-crafted .md5sums file -- proving _dpkg_recorded_file_md5()'s own
# usrmerge path-matching logic against real file content, without
# needing a real dpkg database or mocking the global Path class.
_DPKG_INFO_DIR = Path("/var/lib/dpkg/info")


def _dpkg_recorded_file_md5(package: str, path: Path) -> str | None:
    """Round-N+ review (HIGH, "distro provenance ... substituted self-
    consistent bytes pass" / "governed pre-copy SHA from checked-in/
    pinned lock"): returns the per-file MD5 checksum dpkg ITSELF
    recorded for `path` at install time, read directly from
    `package`'s own `/var/lib/dpkg/info/<package>[:architecture].md5sums`
    database file -- the exact same install-time integrity record
    `dpkg -V`/`debsums` themselves rely on. This is deliberately used
    instead of hand-maintaining and perpetually re-pinning a separate,
    project-owned SHA256 table keyed by component: unlike
    COMPONENT_EXPECTED_SOURCE_VERSION's own version-string pin (which
    is EXPECTED to change on every deliberate, reviewed distro release
    bump), a per-file content checksum changes in lockstep with that
    same version string for any real, unmodified dpkg-managed file --
    so authenticating against dpkg's OWN already-governed, install-time
    record closes "substituted self-consistent bytes pass" (a file
    whose content was tampered with LOCALLY after installation, while
    its package/version metadata was left untouched) without
    introducing a second, independently-maintained pin this project
    would have to keep re-synchronized with the first by hand on every
    routine update -- exactly the "equivalent governed lock verified
    before packaging" alternative the review itself allows. Returns
    None (never raises) if `package` has no md5sums database at all (a
    package legitimately installed without per-file checksums -- e.g.
    one containing only conffiles, or dpkg itself unavailable), or if
    `path` has no matching entry within it; a caller must already treat
    None as an honest "cannot verify here", never itself a failure,
    exactly as every other provenance mechanism in this module does.

    Docker/ubuntu:22.04 validation of this exact function (real dpkg,
    real installed zlib1g) surfaced a genuine gap that would otherwise
    silently degrade EVERY file under a merged-/usr top-level directory
    to "unavailable": modern Debian/Ubuntu ship with `/bin`, `/sbin`,
    `/lib`, `/lib32`, `/lib64`, and `/libx32` as symlinks into the
    corresponding `/usr/...` directory ("usrmerge"), but each package's
    own `.md5sums` payload still records file paths exactly as they
    were laid out in the underlying `.deb` archive at build time --
    which, for the vast majority of real Debian/Ubuntu packages
    (including zlib1g itself), is still the pre-merge `lib/...` form
    even though `Path.resolve()` on the live filesystem returns the
    merged `usr/lib/...` form. Matching only the resolved form here
    therefore silently failed to find a real, correct entry for nearly
    every real system library on a real Ubuntu host, always reporting
    "unavailable" instead of ever truly verifying real file integrity.
    _merged_usr_relative_path_candidates() below tries every path form
    a real package could legitimately have recorded."""
    info_dir = _DPKG_INFO_DIR
    candidates = [info_dir / f"{package}.md5sums"]
    try:
        candidates.extend(sorted(info_dir.glob(f"{package}:*.md5sums")))
    except OSError:
        pass
    try:
        resolved = path.resolve()
    except OSError:
        resolved = path
    relative_candidates = _merged_usr_relative_path_candidates(resolved)
    for candidate in candidates:
        try:
            text = candidate.read_text()
        except OSError:
            continue
        for line in text.splitlines():
            if "  " not in line:
                continue
            checksum, recorded_path = line.split("  ", 1)
            if recorded_path.strip() in relative_candidates:
                return checksum.strip().lower()
    return None


def _dpkg_full_provenance_record(path: Path) -> dict[str, str] | None:
    """The SINGLE, shared implementation of "everything this module can
    honestly claim, from dpkg alone, about the real file at `path`" --
    package name, version, source package, architecture, and an
    install-time file-integrity verification against dpkg's own
    recorded per-file checksum (see _dpkg_recorded_file_md5()'s own
    docstring) -- or None if dpkg is unavailable or `path` is not owned
    by any installed package at all.

    Round-N+ review ("arch stripped" / "substituted self-consistent
    bytes pass"): capture_package_provenance(), capture_distro_source_
    provenance(), and bind_bundled_library_to_system_provenance()
    previously each independently called _dpkg_owning_package() and
    _dpkg_package_metadata() themselves, so a future fix to one (e.g.
    the architecture/integrity fields this same round adds) risked
    landing in only some of them. Every one of those three call sites
    now calls this single function instead -- a simplification, not an
    additional layer -- so architecture and integrity verification are
    available to every provenance record this module ever produces, by
    construction, with no call site able to silently fall behind."""
    package = _dpkg_owning_package(path)
    if package is None:
        return None
    metadata = _dpkg_package_metadata(package)
    if metadata is None:
        return None
    version, source_package = metadata
    record: dict[str, str] = {
        "package": package,
        "version": version,
        "sourcePackage": source_package,
    }
    architecture = _dpkg_package_architecture(package)
    if architecture is not None:
        record["architecture"] = architecture
    recorded_md5 = _dpkg_recorded_file_md5(package, path)
    if recorded_md5 is None:
        record["dpkgFileIntegrity"] = "unavailable"
    else:
        try:
            actual_md5 = hashlib.md5(path.read_bytes()).hexdigest()
        except OSError:
            record["dpkgFileIntegrity"] = "unavailable"
        else:
            record["dpkgRecordedMd5"] = recorded_md5
            record["dpkgActualMd5"] = actual_md5
            record["dpkgFileIntegrity"] = (
                "verified" if actual_md5 == recorded_md5 else "mismatch"
            )
    return record


def capture_package_provenance(basename: str) -> dict[str, str] | None:
    """Round-N+ review (MEDIUM, package provenance): finds the real,
    currently-installed system copy of a library sharing `basename`
    (searched across _SYSTEM_LIBRARY_SEARCH_DIRS, symlinks resolved) and
    returns its dpkg-derived {"package", "version", "sourcePackage"}
    identity, or None the moment any step of that is unavailable --
    dpkg not installed, no real system library of this basename exists
    on the current host at all (a genuinely project-vendored/
    self-built codec library, or simply a non-Debian development
    machine), or the found file is not actually dpkg-owned. None is
    never itself an error here: it is the correct, honest answer
    whenever this specific, real-system cross-check cannot meaningfully
    be performed in the current environment, and every caller (both the
    CLI's provenance validation and the SBOM inventory below) treats it
    as such -- only a REAL, resolved provenance record that disagrees
    with COMPONENT_EXPECTED_SOURCE_PACKAGES is ever treated as a
    failure."""
    for search_dir in _SYSTEM_LIBRARY_SEARCH_DIRS:
        candidate = search_dir / basename
        if not candidate.exists():
            continue
        resolved = candidate.resolve()
        record = _dpkg_full_provenance_record(resolved)
        if record is None:
            continue
        return record
    return None


def validate_component_package_provenance(
    component: str, provenance: dict[str, str]
) -> str | None:
    """Returns a descriptive problem string if `provenance` (as returned
    by capture_package_provenance()) actually disagrees with
    COMPONENT_EXPECTED_SOURCE_PACKAGES' expectation for `component`, or
    None if it agrees, or None if `component` simply has no entry in
    that table at all (nothing this project can currently verify -- see
    COMPONENT_EXPECTED_SOURCE_PACKAGES' own docstring for exactly which
    components that is and why, deliberately never conflated with an
    actual, checkable mismatch)."""
    expected = COMPONENT_EXPECTED_SOURCE_PACKAGES.get(component)
    if expected is None:
        return None
    actual = provenance["sourcePackage"]
    if actual in expected:
        return None
    return (
        f"component {component!r} claims Debian source package "
        f"{sorted(expected)!r}, but the real installed system library's "
        f"own dpkg-derived source package is {actual!r} (binary package "
        f"{provenance['package']!r} version {provenance['version']!r})"
    )


def bind_bundled_library_to_system_provenance(
    bundled_path: Path,
) -> dict[str, object]:
    """Round-7 review (HIGH, "distro provenance looks up an unrelated
    host file by basename and is not cryptographically bound to the
    bundled input/final ELF"): capture_package_provenance() above only
    ever proves a claim about *some same-basename file the current host
    happens to have installed* -- it never actually inspects
    `bundled_path` (the real file this project bundled into the AppDir)
    at all, so a bundled library that has been substituted, downgraded,
    or otherwise diverged from any real dpkg-owned system copy would
    still silently "match" merely because a same-named file exists
    somewhere in _SYSTEM_LIBRARY_SEARCH_DIRS.

    This function closes that gap by additionally comparing
    `bundled_path`'s own _canonical_load_digest() (see that function's
    own docstring: every loaded section's content, every decoded
    dynamic-linking directive, and the executable-ness of every
    section's actual runtime segment mapping -- i.e. everything
    behavior-relevant, excluding only the exact fields patchelf's own
    RUNPATH rewrite is documented to legitimately alter) against the
    resolved system copy's own digest, so a claimed provenance can only
    ever be trusted when it is cryptographically proven to be that
    exact package's own build output, not merely a coincidentally
    same-named file.

    Returns a dict with a "status" key, one of:
      - "dpkg_unavailable": `dpkg`/`dpkg-query` are not installed on this
        host at all (a non-Debian development machine); no claim about
        distro package provenance can be made here, so this is a
        legitimate, honest no-op -- never itself a failure.
      - "not_found": dpkg IS available (a real Debian/Ubuntu-family
        host) but no file sharing `bundled_path`'s basename exists under
        any _SYSTEM_LIBRARY_SEARCH_DIRS entry at all -- on a host where
        distro package provenance IS checkable, an expected distro
        component with no findable system copy is itself a real
        problem, never silently accepted.
      - "not_dpkg_owned": a same-basename system file was found, but it
        is not owned by any installed dpkg package.
      - "content_mismatch": a same-basename, dpkg-owned system file was
        found, but its _canonical_load_digest() does not match
        `bundled_path`'s own -- the bundled file cannot be proven to
        actually be that package's own build output at all (this is the
        exact "unrelated host file"/substitution scenario the review
        finding is about). Carries "systemPath"/"systemSha256"/
        "systemCanonicalLoadDigest" alongside "package"/"version"/
        "sourcePackage" so the exact disagreement is independently
        reconstructable from the SBOM alone, without re-deriving
        anything by basename after the fact (Third-HIGH-round review,
        "Generated SBOM must allow independent reconstruction; do not
        re-look up by basename after validation").
      - "matched": a same-basename, dpkg-owned system file was found
        AND its content is cryptographically proven identical (modulo
        only patchelf's documented RUNPATH-adjacent rewrites) to
        `bundled_path` -- "package"/"version"/"sourcePackage" keys are
        present, exactly as capture_package_provenance() itself
        returns, plus "systemPath"/"systemSha256"/
        "bundledCanonicalLoadDigest" (the exact validated input
        identity: the real system file this bundled copy was proven to
        match, its whole-file sha256, and the shared canonical load
        digest both files agree on) -- again for full independent SBOM
        reconstruction -- so this dict can be passed directly to
        validate_component_package_provenance() for the source-package
        name check."""
    if shutil.which("dpkg") is None or shutil.which("dpkg-query") is None:
        return {"status": "dpkg_unavailable"}
    basename = bundled_path.name
    try:
        bundled_resolved = bundled_path.resolve()
    except OSError:
        bundled_resolved = bundled_path
    found_any_system_copy = False
    found_any_dpkg_owned_copy = False
    for search_dir in _SYSTEM_LIBRARY_SEARCH_DIRS:
        candidate = search_dir / basename
        if not candidate.exists():
            continue
        resolved = candidate.resolve()
        if resolved == bundled_resolved:
            # This project's bundled AppDir copy must never itself live
            # under one of the real system search directories; treat it
            # as "no real system copy found" rather than trivially
            # "matching itself".
            continue
        found_any_system_copy = True
        record = _dpkg_full_provenance_record(resolved)
        if record is None:
            continue
        found_any_dpkg_owned_copy = True
        bundled_digest = _canonical_load_digest(bundled_path)
        system_digest = _canonical_load_digest(resolved)
        if (
            bundled_digest is None
            or system_digest is None
            or bundled_digest != system_digest
        ):
            return {
                "status": "content_mismatch",
                **record,
                "systemPath": str(resolved),
                "systemSha256": _sha256(resolved),
                "systemCanonicalLoadDigest": system_digest,
            }
        return {
            "status": "matched",
            **record,
            "systemPath": str(resolved),
            "systemSha256": _sha256(resolved),
            "bundledCanonicalLoadDigest": bundled_digest,
        }
    if found_any_dpkg_owned_copy:
        # Unreachable in practice (a dpkg-owned copy found by the loop
        # above always returns before falling through), kept only so
        # this function's control flow is exhaustive/explicit rather
        # than relying on an implicit fallthrough.
        return {"status": "not_dpkg_owned"}
    if found_any_system_copy:
        return {"status": "not_dpkg_owned"}
    return {"status": "not_found"}


# Round-N+ review (HIGH, "distro provenance post-hoc/unpinned: after
# packaging it searches fixed system dirs by basename, not exact
# linuxdeploy-selected pre-copy file"): _SYSTEM_LIBRARY_SEARCH_DIRS-based
# lookup (bind_bundled_library_to_system_provenance() above) always
# performs its own INDEPENDENT directory search, AFTER packaging has
# already finished, picking the FIRST same-basename file it happens to
# find across a fixed, hardcoded directory list -- on a host with more
# than one same-named candidate (multilib, a manually installed
# alternate, or simply a different search order than the real dynamic
# loader/linuxdeploy itself used), this can silently authenticate
# against a COMPLETELY DIFFERENT file than the one linuxdeploy's own
# ldd-based dependency resolution actually copied bytes from.
#
# `resolve_ldd_dependencies()`/`capture_distro_source_provenance()`
# below close this by capturing provenance the SAME way, at the SAME
# real resolution mechanism (the actual dynamic loader, via `ldd`), and
# at the SAME time linuxdeploy performs its own copy -- i.e. run BEFORE
# packaging, directly against this project's own first-party, not-yet-
# bundled executable/Qt plugins (see packaging/build-appimage.sh's own
# "Capture distro provenance" step, which invokes the new
# `capture-distro-provenance` CLI subcommand immediately after this
# project's own build but strictly before linuxdeploy's first
# invocation) -- never a basename re-discovered afterward under an
# independently maintained, potentially stale/wrong directory list.
def resolve_ldd_dependencies(elf_path: Path) -> dict[str, Path]:
    """Runs the real `ldd` against a genuine, not-yet-packaged ELF
    executable or plugin and returns {soname: resolved_absolute_path}
    for every dynamically resolved dependency it reports -- i.e. the
    EXACT file the real dynamic loader (never an independently
    reimplemented directory search) selects for `elf_path`, on this
    exact host, at this exact moment: the identical resolution
    linuxdeploy's own automatic bundling performs internally when
    deciding what to copy, since both consult the same `ld.so` cache/
    search rules. Entries `ldd` reports as "not found" (a dependency
    this exact host cannot itself resolve at all) are omitted -- a
    caller requiring full coverage of every expected component enforces
    that separately, exactly as every other provenance mechanism in
    this module already does. Returns an empty dict (never raises) if
    `ldd` is not installed, `elf_path` cannot be inspected as a real
    ELF at all, or a subprocess-level error occurs invoking it."""
    if shutil.which("ldd") is None:
        return {}
    try:
        result = subprocess.run(
            ["ldd", str(elf_path)], capture_output=True, text=True
        )
    except OSError:
        return {}
    resolved: dict[str, Path] = {}
    for line in result.stdout.splitlines():
        match = _LDD_DEPENDENCY_LINE_RE.match(line)
        if not match:
            continue
        target = match.group("target")
        if target is None or target == "not found":
            continue
        # Deliberately NOT canonicalized via Path.resolve(): `target` is
        # already the exact path the real dynamic loader itself
        # resolved this soname to (the identical resolution linuxdeploy
        # performs) -- further canonicalizing it here could silently
        # substitute a DIFFERENT (if content-identical) path on a
        # merged-/usr host where e.g. /lib is itself a directory
        # symlink to /usr/lib, which would make this capture no longer
        # describe the literal path the loader/linuxdeploy actually
        # used.
        resolved[match.group("soname")] = Path(target)
    return resolved


# `ldd` prints one line per dependency in one of two shapes:
#   "\tlibfoo.so.1 => /lib/x86_64-linux-gnu/libfoo.so.1 (0x00007f...)"
#   "\tlinux-vdso.so.1 (0x00007ffee...)"                 (no "=>" target
#                                                          -- the kernel
#                                                          VDSO, never a
#                                                          real file)
#   "\tlibbar.so => not found"
# Only the first shape (a real "=>" resolution to an actual path) is
# useful here; the regex's own `target` group is optional so lines
# without one (the VDSO shape) simply fail to match `resolve_ldd_
# dependencies()`'s own filter above, exactly like an explicit "not
# found" target does.
_LDD_DEPENDENCY_LINE_RE = re.compile(
    r"^\s*(?P<soname>\S+)\s+=>\s+(?P<target>\S+|not found)(?:\s+\(0x[0-9a-fA-F]+\))?\s*$"
)


def capture_distro_source_provenance(
    resolved_dependencies: dict[str, list[Path]],
) -> dict[str, list[dict[str, str]]]:
    """For every {soname: [resolved_absolute_path, ...]} pair (as
    assembled by cmd_capture_distro_provenance() across every first-
    party executable/Qt plugin/QML module this project's build is about
    to bundle FROM), returns {soname: [{"path", "sha256", "package",
    "version", "sourcePackage", "architecture", "dpkgFileIntegrity",
    ...}, ...]} -- one capture record per DISTINCT real resolved path,
    never collapsed to a single "winner" -- capturing each candidate's
    real identity at the EXACT moment and EXACT path the real loader
    (and therefore linuxdeploy) actually copies it from, never a
    basename re-discovered in a fixed, hardcoded directory list after
    packaging has already finished (see this function's own preceding
    module comment).

    Round-N+ review (HIGH, "distro provenance collapses identity ...
    keyed SONAME/basename overwrites different requester resolutions"):
    two different first-party requesters CAN legitimately resolve the
    identical soname to two DIFFERENT real files (a private/conflicting
    RPATH, or a force-bundled explicit input whose own real system
    identity differs from what some unrelated requester's dependency
    resolution happens to report) -- collapsing to a single {soname:
    record} entry (the old shape) always silently discards every
    candidate but the last one `dict.update()`/iteration order happened
    to pick, so bind_bundled_library_to_captured_provenance() could only
    ever compare the bundled file against ONE of the real candidates,
    with no way to even detect the other was ever thrown away. This
    function's list-valued result preserves every one, so the binding
    step downstream can honestly check the bundled file against ALL of
    them.

    An soname/path whose dpkg ownership/version cannot be determined at
    all (dpkg missing, the resolved path no longer exists, or the file
    genuinely is not dpkg-owned -- e.g. a project-vendored library
    `ldd` also happens to resolve) is simply omitted from that soname's
    candidate list, exactly the same honest, non-fatal degrade every
    other provenance mechanism in this module already uses; a caller
    that requires full coverage enforces that itself (see
    cmd_capture_distro_provenance()'s own reporting). A soname with zero
    survivors is a key mapped to an empty list, never omitted entirely
    -- so downstream code can still distinguish "we tried, but nothing
    was dpkg-provable" from "this soname was never even a candidate"."""
    captured: dict[str, list[dict[str, str]]] = {}
    for soname, resolved_paths in resolved_dependencies.items():
        candidates: list[dict[str, str]] = []
        for resolved_path in resolved_paths:
            if not resolved_path.is_file():
                continue
            record = _dpkg_full_provenance_record(resolved_path)
            if record is None:
                continue
            candidates.append(
                {
                    "path": str(resolved_path),
                    "sha256": _sha256(resolved_path),
                    **record,
                }
            )
        captured[soname] = candidates
    return captured


def _nofollow_open_flags() -> int:
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    return flags


def _capture_immutable_staged_file(
    source_path: Path, staging_dir: Path
) -> dict[str, str] | None:
    """Round-N+ review (HIGH, "hash/copy from same nofollow descriptor
    or immutable staging object"): resolves `source_path` to its real
    on-disk file once, opens THAT exact inode with O_NOFOLLOW, then
    hashes and copies bytes from that SAME open descriptor into
    `staging_dir` before anything else in this module ever trusts the
    path again.

    The resulting staged file is this module's immutable, post-capture
    source of truth: bind_bundled_library_to_captured_provenance() below
    compares final bundled bytes against this staged object, never by
    re-opening the original system-library path by name after capture.
    Returns None (never raises) if the source cannot be resolved/opened
    safely."""
    try:
        resolved = source_path.resolve(strict=True)
    except OSError:
        return None
    try:
        fd = os.open(resolved, _nofollow_open_flags())
    except OSError:
        return None
    staging_dir.mkdir(parents=True, exist_ok=True)
    partial_path = staging_dir / f"{resolved.name}.partial"
    sha256 = hashlib.sha256()
    actual_md5 = hashlib.md5()
    try:
        with os.fdopen(fd, "rb", closefd=True) as source_handle, partial_path.open(
            "wb"
        ) as staged_handle:
            while True:
                chunk = source_handle.read(1024 * 1024)
                if not chunk:
                    break
                sha256.update(chunk)
                actual_md5.update(chunk)
                staged_handle.write(chunk)
    except OSError:
        try:
            partial_path.unlink()
        except OSError:
            pass
        return None
    sha256_hex = sha256.hexdigest()
    final_path = staging_dir / f"{sha256_hex}--{resolved.name}"
    try:
        if final_path.exists():
            partial_path.unlink()
        else:
            partial_path.rename(final_path)
            final_path.chmod(0o444)
    except OSError:
        try:
            partial_path.unlink()
        except OSError:
            pass
        return None
    return {
        "sourceRealPath": str(resolved),
        "stagedPath": str(final_path),
        "sha256": sha256_hex,
        "actualMd5": actual_md5.hexdigest(),
    }


def _capture_distro_manifest_entry(
    loader_path: Path, bundled_path: str, requester: Path, needed: str, staging_dir: Path
) -> dict[str, object] | None:
    """Captures one exact requester+DT_NEEDED edge's immutable distro
    provenance record. `loader_path` is the literal path ldd reported
    for that edge; `bundled_path` is the exact AppDir-relative
    destination linuxdeploy will copy it to (`usr/lib/<basename>` in
    this project's own real packaging pipeline)."""
    staged = _capture_immutable_staged_file(loader_path, staging_dir)
    if staged is None:
        return None
    source_real_path = Path(staged["sourceRealPath"])
    package = _dpkg_owning_package(source_real_path)
    if package is None:
        return None
    metadata = _dpkg_package_metadata(package)
    if metadata is None:
        return None
    version, source_package = metadata
    entry: dict[str, object] = {
        "bundledPath": bundled_path,
        "needed": needed,
        "loaderPath": str(loader_path),
        "sourceRealPath": staged["sourceRealPath"],
        "stagedPath": staged["stagedPath"],
        "sha256": staged["sha256"],
        "package": package,
        "version": version,
        "sourcePackage": source_package,
        "requesterEdges": [
            {"requester": str(requester), "needed": needed, "loaderPath": str(loader_path)}
        ],
    }
    architecture = _dpkg_package_architecture(package)
    if architecture is not None:
        entry["architecture"] = architecture
    recorded_md5 = _dpkg_recorded_file_md5(package, source_real_path)
    if recorded_md5 is None:
        entry["dpkgFileIntegrity"] = "unavailable"
    else:
        entry["dpkgRecordedMd5"] = recorded_md5
        entry["dpkgActualMd5"] = staged["actualMd5"]
        entry["dpkgFileIntegrity"] = (
            "verified"
            if staged["actualMd5"] == recorded_md5
            else "mismatch"
        )
    entry["canonicalLoadDigest"] = _canonical_load_digest(Path(staged["stagedPath"]))
    return entry


def _bundled_library_destination_path(needed: str) -> str:
    return f"usr/lib/{Path(needed).name}"


def build_distro_provenance_manifest(
    dependency_edges: list[tuple[Path, str, Path]], staging_dir: Path
) -> tuple[dict[str, object], list[str]]:
    """Builds the authoritative capture-before-packaging distro
    provenance manifest used by cmd_capture_distro_provenance() and, in
    turn, cmd_classify()'s provenance-required mode.

    Each input tuple is (requester_path, needed_soname, loader_resolved_
    path). The returned manifest is keyed by EXACT final bundled path,
    never by basename or SONAME alone:

      {"formatVersion": 2, "bundledPaths": {"usr/lib/libfoo.so.1": {...}}}

    Round-N+ review (HIGH, "keyed SONAME/basename overwrites different
    requester resolutions" / "same basename force bundle"): if two
    distinct real source objects would land at the SAME bundled path but
    differ in sourceRealPath/sha256, that is reported as a conflict
    rather than letting one silently overwrite the other."""
    bundled_paths: dict[str, dict[str, object]] = {}
    conflicts: list[str] = []
    for requester, needed, loader_path in dependency_edges:
        entry = _capture_distro_manifest_entry(
            loader_path,
            _bundled_library_destination_path(needed),
            requester,
            needed,
            staging_dir,
        )
        if entry is None:
            continue
        bundled_path = str(entry["bundledPath"])
        existing = bundled_paths.get(bundled_path)
        if existing is None:
            bundled_paths[bundled_path] = entry
            continue
        if (
            existing["sha256"] == entry["sha256"]
            and existing["sourceRealPath"] == entry["sourceRealPath"]
        ):
            existing_edges = existing.setdefault("requesterEdges", [])
            assert isinstance(existing_edges, list)
            for new_edge in entry["requesterEdges"]:  # type: ignore[index]
                if new_edge not in existing_edges:
                    existing_edges.append(new_edge)
            continue
        conflicts.append(
            f"{bundled_path}: conflicting captured sources "
            f"{existing['sourceRealPath']!r} ({existing['sha256']!r}) vs "
            f"{entry['sourceRealPath']!r} ({entry['sha256']!r})"
        )
    return {"formatVersion": 2, "bundledPaths": bundled_paths}, conflicts


def bind_bundled_library_to_captured_provenance(
    bundled_path: Path,
    manifest: dict[str, object],
    bundled_relative_path: str | None = None,
) -> dict[str, object]:
    """The capture-before-packaging replacement for
    bind_bundled_library_to_system_provenance() above (see this
    function's own preceding module comment for the full "searches
    fixed system dirs by basename, not exact linuxdeploy-selected
    pre-copy file" rationale): `manifest` is the JSON object captured
    BEFORE packaging by build_distro_provenance_manifest()/the
    `capture-distro-provenance` CLI subcommand below. This function
    performs NO independent system search of its own at all -- it looks
    the FINAL bundled file up by its exact AppDir-relative destination
    path (e.g. "usr/lib/libfoo.so.1"), never by basename/SONAME alone.

    Round-N+ review (HIGH, "distro provenance collapses identity ...
    keyed SONAME/basename overwrites different requester resolutions" /
    "same basename force bundle"): the authoritative capture step now
    resolves any many-edges-to-one-destination case UP FRONT. Either
    every requester+DT_NEEDED edge bound to this exact bundled path
    agreed on the SAME immutable staged source object (and is merged
    into one manifest entry carrying every requester edge), or capture
    failed with an explicit conflict before packaging was trusted at
    all. The bind step therefore checks exactly ONE staged object per
    bundled destination, not "any same-basename candidate that happens
    to match".

    Returns a dict with the same "status" contract as
    bind_bundled_library_to_system_provenance() ("matched"/
    "content_mismatch"/"not_found"; there is no "dpkg_unavailable"/
    "not_dpkg_owned" distinction here since the manifest itself is only
    ever produced on a real, dpkg-equipped build host -- an empty/absent
    manifest entry means capture itself could not prove anything about
    this component, which every caller already treats identically to
    "not found" either way).

    Round-N+ review ("changed file after capture"): the staged source
    object's own sha256 is re-verified here too. The original system
    path is deliberately never re-opened after capture; only the
    immutable staged object is trusted."""
    if manifest.get("formatVersion") != 2:
        legacy_candidates = manifest.get(bundled_path.name)
        if not isinstance(legacy_candidates, list):
            return {"status": "not_found"}
        candidates: list[dict[str, str]] = legacy_candidates
        if not candidates:
            return {"status": "not_found"}
        bundled_digest = _canonical_load_digest(bundled_path)
        mismatches: list[dict[str, object]] = []
        for entry in candidates:
            captured_path = Path(entry["path"])
            if not captured_path.is_file():
                mismatches.append(
                    {**entry, "problem": "captured system file no longer exists"}
                )
                continue
            current_sha256 = _sha256(captured_path)
            if current_sha256 != entry["sha256"]:
                mismatches.append(
                    {
                        **entry,
                        "systemPath": str(captured_path),
                        "systemSha256": current_sha256,
                        "systemCanonicalLoadDigest": _canonical_load_digest(captured_path),
                        "problem": (
                            "the captured system file's own bytes changed on "
                            "disk between provenance-capture time and this "
                            f"classify invocation (recorded sha256 "
                            f"{entry['sha256']!r}, now {current_sha256!r})"
                        ),
                    }
                )
                continue
            system_digest = _canonical_load_digest(captured_path)
            if (
                bundled_digest is None
                or system_digest is None
                or bundled_digest != system_digest
            ):
                mismatches.append(
                    {
                        **entry,
                        "systemPath": str(captured_path),
                        "systemSha256": current_sha256,
                        "systemCanonicalLoadDigest": system_digest,
                    }
                )
                continue
            return {
                "status": "matched",
                **entry,
                "systemPath": str(captured_path),
                "systemSha256": current_sha256,
                "bundledCanonicalLoadDigest": bundled_digest,
            }
        return {
            "status": "content_mismatch",
            "candidateCount": len(candidates),
            "mismatches": mismatches,
            **mismatches[0],
        }
    manifest_entries = manifest.get("bundledPaths")
    if not isinstance(manifest_entries, dict):
        return {"status": "not_found"}
    manifest_key = bundled_relative_path or _bundled_library_destination_path(
        bundled_path.name
    )
    entry = manifest_entries.get(manifest_key)
    if not isinstance(entry, dict):
        return {"status": "not_found"}
    staged_path = Path(str(entry["stagedPath"]))
    if not staged_path.is_file():
        return {
            "status": "content_mismatch",
            **entry,
            "systemPath": str(entry["sourceRealPath"]),
            "systemSha256": None,
            "problem": "captured staged source file no longer exists",
        }
    bundled_digest = _canonical_load_digest(bundled_path)
    staged_sha256 = _sha256(staged_path)
    if staged_sha256 != entry["sha256"]:
        return {
            "status": "content_mismatch",
            **entry,
            "systemPath": str(entry["sourceRealPath"]),
            "systemSha256": staged_sha256,
            "systemCanonicalLoadDigest": _canonical_load_digest(staged_path),
            "problem": (
                "the immutable staged source object's own bytes changed on disk "
                "after capture"
            ),
        }
    staged_digest = entry.get("canonicalLoadDigest")
    if bundled_digest is not None and staged_digest is not None:
        matched = bundled_digest == staged_digest
    else:
        matched = _sha256(bundled_path) == staged_sha256
    result: dict[str, object] = {
        "status": "matched" if matched else "content_mismatch",
        **entry,
        "systemPath": str(entry["sourceRealPath"]),
        "systemSha256": staged_sha256,
        "systemCanonicalLoadDigest": staged_digest,
    }
    if bundled_digest is not None:
        result["bundledCanonicalLoadDigest"] = bundled_digest
    return result


def load_distro_provenance_manifest(path: Path) -> dict[str, object]:
    """Loads a JSON manifest written by cmd_capture_distro_provenance()
    (or capture_distro_source_provenance() directly). Supports the
    current exact-destination keyed formatVersion 2 manifest and, for
    backward-compatible local/unit-test callers only, the older
    soname-keyed list-of-candidates shape. Raises ValueError with a
    descriptive message on any structural problem."""
    try:
        raw = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read distro provenance manifest {path}: {error}") from error
    if not isinstance(raw, dict):
        raise ValueError(
            f"malformed distro provenance manifest {path}: expected a JSON object"
        )
    if raw.get("formatVersion") == 2:
        bundled_paths = raw.get("bundledPaths")
        if isinstance(bundled_paths, dict) and all(
            isinstance(key, str) and isinstance(value, dict)
            for key, value in bundled_paths.items()
        ):
            return raw
        raise ValueError(
            f"malformed distro provenance manifest {path}: formatVersion 2 "
            "requires a 'bundledPaths' object keyed by exact bundled destination"
        )
    if not all(
        isinstance(value, list) and all(isinstance(item, dict) for item in value)
        for value in raw.values()
    ):
        raise ValueError(
            f"malformed distro provenance manifest {path}: expected a JSON "
            "object whose every value is a JSON array of objects (one per "
            "distinct real candidate resolution)"
        )
    return raw


def validate_bundled_library_package_provenance(
    component: str, binding: dict[str, object], require_provenance: bool = False
) -> str | None:
    """Returns a descriptive problem string if `binding` (as returned by
    bind_bundled_library_to_system_provenance()) fails to prove
    `component`'s expected distro-package provenance, or None if it
    either agrees or `component` has no entry in
    COMPONENT_EXPECTED_SOURCE_PACKAGES at all (nothing this project
    currently claims to verify for it -- see that table's own
    docstring).

    Round-7 review ("require it for every distro component" /
    "missing provenance passes" / "pin package revision/snapshot or
    governed expectations"): unlike the older
    validate_component_package_provenance() (which only ever judges a
    provenance record that was already successfully captured, and is
    silently never called at all when capture returns None), this
    function is handed EVERY outcome bind_bundled_library_to_system_provenance()
    can produce, including the ones that used to be silently skipped.

    "content_mismatch" -- a same-basename, dpkg-owned system file was
    actually found, but its content provably differs from the bundled
    file -- is ALWAYS a real, reported failure, unconditionally: this is
    exactly the "unrelated host file"/substitution scenario the review
    finding is centrally about, and is never a legitimate "just isn't
    installed here" case the way "not_found"/"not_dpkg_owned" can
    honestly be on an incomplete host.

    "not_found"/"not_dpkg_owned" are only elevated to real failures when
    `require_provenance` is True -- this is the "governed expectations"
    half of the finding: this project's own pinned `ubuntu-22.04`
    `appimage-smoke` CI job (see .github/workflows/ci.yml, which passes
    `--require-package-provenance` explicitly) runs against the REAL,
    final, ldd-closure-resolved AppImage, where by construction every
    bundled distro library's real system counterpart genuinely IS
    installed and dpkg-owned on that exact same runner -- there,
    "missing" is unambiguously suspicious and must fail. A merely
    dpkg-equipped host that does not happen to have every expected
    package actually apt-installed (a partial local development
    container, or this project's own basename-only classification unit
    tests using fake fixture files) is explicitly NOT asserted to have
    genuinely reviewed, governed grounds to claim non-installation is
    itself wrong -- so it remains a legitimate skip there, exactly
    matching this project's own long-standing degrade-gracefully
    policy (see COMPONENT_EXPECTED_SOURCE_PACKAGES' own docstring)."""
    status = binding["status"]
    if status == "dpkg_unavailable":
        return None
    if component not in COMPONENT_EXPECTED_SOURCE_PACKAGES:
        return None
    if status == "not_found":
        if not require_provenance:
            return None
        return (
            f"component {component!r} expects a real, dpkg-owned system "
            "library sharing its bundled basename to be findable under "
            "_SYSTEM_LIBRARY_SEARCH_DIRS on this (real Debian/Ubuntu, "
            "dpkg-equipped, --require-package-provenance) host, but none "
            "exists -- an expected distro component's provenance must be "
            "provable here, never silently skipped"
        )
    if status == "not_dpkg_owned":
        if not require_provenance:
            return None
        return (
            f"component {component!r}'s same-basename system file exists on "
            "this host but is not owned by any installed dpkg package -- "
            "its provenance cannot be authenticated"
        )
    if status == "content_mismatch":
        return (
            f"component {component!r}'s bundled library does not "
            "byte-for-byte match (via _canonical_load_digest(), which "
            "already tolerates every field patchelf's own documented "
            "RUNPATH rewrite may alter) the real dpkg-owned system copy "
            f"of the same basename ({binding.get('sourcePackage')!r} "
            f"{binding.get('version')!r}) -- the bundled file cannot be "
            "proven to actually be that package's own build output at all"
        )
    # Round-N+ review (HIGH, "distro provenance collapses identity ...
    # arch stripped"): the reference system copy's own dpkg-recorded
    # architecture must agree with this project's single, real,
    # pinned CI target (EXPECTED_DISTRO_PACKAGE_ARCHITECTURE's own
    # docstring) -- a foreign-arch/multilib substitute sharing an
    # otherwise-identical name/version string is a real, reported
    # failure, never silently accepted. Absent ("architecture" key
    # missing, e.g. dpkg-query itself unavailable for this specific
    # lookup) is treated exactly like every other "cannot verify here"
    # case in this module: honestly skipped, never conflated with a
    # proven mismatch.
    actual_architecture = binding.get("architecture")
    if (
        actual_architecture is not None
        and actual_architecture != EXPECTED_DISTRO_PACKAGE_ARCHITECTURE
    ):
        return (
            f"component {component!r}'s real dpkg-owned system reference "
            f"copy is architecture {actual_architecture!r}, but this "
            f"project's own pinned CI/packaging target is exactly "
            f"{EXPECTED_DISTRO_PACKAGE_ARCHITECTURE!r} -- a foreign-"
            "arch/multilib substitute cannot be trusted as this "
            "component's real build output"
        )
    # Round-N+ review (HIGH, "substituted self-consistent bytes pass"):
    # the reference system copy's OWN content must still agree with
    # dpkg's own recorded install-time per-file checksum -- see
    # _dpkg_recorded_file_md5()'s own docstring for exactly why this is
    # the "equivalent governed lock" this project relies on instead of
    # a separately hand-maintained SHA256 pin table. "mismatch" means
    # the reference file this whole comparison is built on has itself
    # been tampered with locally since dpkg installed it, and can never
    # be trusted as real proof of anything, regardless of what its
    # digest happens to otherwise agree with.
    if binding.get("dpkgFileIntegrity") == "mismatch":
        return (
            f"component {component!r}'s real dpkg-owned system reference "
            f"copy at {binding.get('systemPath')!r} does not match dpkg's "
            "own recorded install-time checksum for that exact file -- it "
            "has been modified on disk since installation and can no "
            "longer be trusted as real, unmodified package build output "
            "to validate the bundled copy against"
        )
    name_problem = validate_component_package_provenance(component, binding)  # type: ignore[arg-type]
    if name_problem is not None:
        return name_problem
    # Round-N+ review (HIGH, "`startswith(expected_version_prefix)` lets
    # 1.46.50 satisfy 1.46.5; security revision drift"): EXACT equality
    # against the COMPLETE, real `dpkg-query`-reported version string --
    # see COMPONENT_EXPECTED_SOURCE_VERSION's own docstring for exactly
    # why a prefix/`startswith()` comparison is structurally unsound
    # (both the literal-substring collision and the silently-tolerated
    # security-revision-drift gap it names). Any drift at all --
    # upstream or Debian-revision, security patch or not -- is now a
    # real, reported failure that must be deliberately reviewed and
    # re-pinned here, never silently absorbed.
    expected_version = COMPONENT_EXPECTED_SOURCE_VERSION.get(component)
    if expected_version is not None:
        actual_version = str(binding["version"])
        if actual_version != expected_version:
            return (
                f"component {component!r} expects its pinned "
                f"{binding['sourcePackage']!r} version to be EXACTLY "
                f"{expected_version!r}, but the real installed system "
                f"package version is {actual_version!r} -- ANY drift "
                "(a genuine upstream revision, a routine point release, "
                "or a security-update Debian-revision bump alike) must "
                "be deliberately reviewed and re-pinned in "
                "COMPONENT_EXPECTED_SOURCE_VERSION, never silently "
                "absorbed"
            )
    return None


# This project's own real, pinned CI Qt toolchain -- see
# packaging/qt_sdk_lock.json and .github/workflows/ci.yml. The lock file
# records both the deliberate Qt release pin and the exact upstream
# Updates.xml metadata digest the workflow now verifies BEFORE
# install-qt-action runs, closing the "same version string, different
# upstream SDK contents" gap a bare sdkVersion check alone cannot.
_QT_SDK_LOCK_PATH: Path = Path(__file__).with_name("qt_sdk_lock.json")


@functools.lru_cache(maxsize=1)
def _load_qt_sdk_lock() -> dict[str, str]:
    """Loads the checked-in Qt SDK lock file governing this repository's
    accepted Qt toolchain identity. A malformed/missing lock file is a
    real configuration error, never silently treated as "no pin"."""
    try:
        raw = json.loads(_QT_SDK_LOCK_PATH.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read Qt SDK lock {_QT_SDK_LOCK_PATH}: {error}") from error
    expected_keys = {"sdkVersion", "updatesXmlUrl", "updatesXmlSha256"}
    if not isinstance(raw, dict) or set(raw) != expected_keys or not all(
        isinstance(raw[key], str) and raw[key] for key in expected_keys
    ):
        raise ValueError(
            f"malformed Qt SDK lock {_QT_SDK_LOCK_PATH}: expected exactly "
            "{'sdkVersion','updatesXmlUrl','updatesXmlSha256'} string keys"
        )
    return {
        "sdkVersion": raw["sdkVersion"],
        "updatesXmlUrl": raw["updatesXmlUrl"],
        "updatesXmlSha256": raw["updatesXmlSha256"],
    }


def _qt_sdk_source_provenance() -> dict[str, str]:
    """Stable, serializable sub-object shared by every Qt/ICU provenance
    binding, recording the checked-in upstream Qt SDK identity this
    repository actually governs."""
    lock = _load_qt_sdk_lock()
    return {
        "sdkVersion": lock["sdkVersion"],
        "updatesXmlUrl": lock["updatesXmlUrl"],
        "updatesXmlSha256": lock["updatesXmlSha256"],
        "verification": "sha256(Updates.xml) pinned in packaging/qt_sdk_lock.json",
    }


# Kept as a small convenience alias for existing callers/tests and for
# parity with COMPONENT_EXPECTED_SOURCE_VERSION's distro-package pin.
EXPECTED_QT_SDK_VERSION: str = _load_qt_sdk_lock()["sdkVersion"]


def verify_qt_sdk_lock(
    qt_sdk_lock: dict[str, str] | None = None,
) -> dict[str, str]:
    """Fetches the pinned upstream Qt Updates.xml metadata and verifies
    its sha256 against this repository's checked-in lock. Intended for
    CI/workflow use BEFORE install-qt-action consumes the SDK."""
    lock = qt_sdk_lock or _load_qt_sdk_lock()
    with urllib.request.urlopen(lock["updatesXmlUrl"]) as response:
        payload = response.read()
    actual_sha256 = hashlib.sha256(payload).hexdigest()
    return {
        "sdkVersion": lock["sdkVersion"],
        "updatesXmlUrl": lock["updatesXmlUrl"],
        "expectedSha256": lock["updatesXmlSha256"],
        "actualSha256": actual_sha256,
        "status": "matched"
        if actual_sha256 == lock["updatesXmlSha256"]
        else "content_mismatch",
    }


def bind_bundled_library_to_qt_sdk_provenance(
    bundled_path: Path,
    qt_reference_dir: Path | None,
    sdk_version: str | None = None,
    sdk_source_provenance: dict[str, str] | None = None,
) -> dict[str, object]:
    """Companion to bind_bundled_library_to_system_provenance() for
    components in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE (see that
    constant's own docstring) whose real, verifiable upstream origin in
    this project's actual pipeline is the Qt SDK's own bundled copy --
    never a dpkg-owned distro package. Proves `bundled_path` is the
    exact same compiled object as the real Qt SDK's own file at the
    identical relative path resolved by
    _resolve_qt_sdk_reference_candidate() (plugin subdirectory, "qml"
    sub-path, or the `lib/<basename>` fallback used by core Qt
    libraries and ICU alike), using the identical
    _is_same_compiled_object_or_unwritten() check this module already
    uses for core Qt libraries (_is_real_core_qt_library()) and Qt's own
    plugins/QML modules (_is_real_qt_plugin_or_qml_module()).

    Round-N+ review ("Qt SDK binding only status/referencePath; no SDK
    archive/manifest identity/reference SHA/canonical digest/transform
    evidence"): every returned binding now carries the SAME checked-in,
    immutable sdkSourceProvenance sub-object (version + exact
    Updates.xml URL + pinned sha256) shared across every Qt library,
    plugin, QML module, and ICU entry in this run, PLUS the reference
    copy's own sha256/_canonical_load_digest() and the final bundled
    file's own sha256/_canonical_load_digest(). The binding object
    itself is therefore the single, complete per-artifact provenance
    receipt later serialized unchanged into the SBOM.

    Returns a dict with a "status" key, one of:
      - "qt_reference_dir_unavailable": no --qt-reference-dir was
        supplied at all; no claim can be made here -- a legitimate,
        honest no-op, exactly mirroring
        bind_bundled_library_to_system_provenance()'s own
        "dpkg_unavailable" status.
      - "not_found": a qt_reference_dir WAS supplied, but no file exists
        at the identical relative path under it -- on a host where this
        check IS performable, an expected Qt-SDK-bundled component with
        no findable reference copy is itself a real problem, never
        silently accepted.
      - "content_mismatch": a reference copy was found at that path,
        but it is not proven identical to `bundled_path` -- the exact
        "substituted/tampered bundled file" scenario this check exists
        to catch.
      - "matched": a reference copy was found AND proven, by
        _is_same_compiled_object_or_unwritten(), to be the same
        compiled object as `bundled_path`.

    Every result also carries "allowedTransform" -- a fixed, explicit,
    self-describing statement of the ONE tolerated transformation this
    binding's own digest comparison already accepts (Round-N+ review,
    "no explicit 'allowed transform' evidence field ... only implicit
    via digest matching") -- rather than leaving that fact only
    inferable by reading _canonical_load_digest()'s own source."""
    sdk_source_provenance = sdk_source_provenance or _qt_sdk_source_provenance()
    allowed_transform = (
        "patchelf RUNPATH/RPATH rewrite only (see _canonical_load_digest()"
        " for the exact set of fields this tolerates)"
    )
    if qt_reference_dir is None:
        return {
            "status": "qt_reference_dir_unavailable",
            "sdkSourceProvenance": sdk_source_provenance,
            "allowedTransform": allowed_transform,
        }
    candidate = _resolve_qt_sdk_reference_candidate(bundled_path, qt_reference_dir)
    reference_path = str(candidate)
    if not candidate.is_file():
        result: dict[str, object] = {
            "status": "not_found",
            "referencePath": reference_path,
            "sdkSourceProvenance": sdk_source_provenance,
            "allowedTransform": allowed_transform,
        }
        if sdk_version is not None:
            result["sdkVersion"] = sdk_version
        return result
    # Each digest below is computed exactly once, here, and reused both
    # to derive "matched"/"content_mismatch" AND as this result's own
    # recorded evidence -- deliberately never re-hashing the same file
    # a second time the way calling the separate, boolean-only
    # _is_same_compiled_object_or_unwritten() helper here (in addition
    # to recording these fields) would have required.
    reference_sha256 = _sha256(candidate)
    reference_digest = _canonical_load_digest(candidate)
    result: dict[str, object] = {
        "sdkSourceProvenance": sdk_source_provenance,
        "referencePath": reference_path,
        "referenceSha256": reference_sha256,
        "referenceCanonicalLoadDigest": reference_digest,
        "allowedTransform": allowed_transform,
    }
    if not bundled_path.is_file():
        # Mirrors _is_same_compiled_object_or_unwritten()'s own
        # "unwritten bundled file" allowance -- see that helper's
        # docstring for why this is not a security-relevant weakening
        # of the real, on-disk code path (find_bundled_libraries()
        # always yields a real, existing file in production; only this
        # module's own pure path-resolution unit tests exercise a
        # bundled_path that was never materialized on disk).
        matched = True
    else:
        bundled_sha256 = _sha256(bundled_path)
        bundled_digest = _canonical_load_digest(bundled_path)
        result["bundledSha256"] = bundled_sha256
        result["bundledCanonicalLoadDigest"] = bundled_digest
        if reference_digest is not None and bundled_digest is not None:
            matched = reference_digest == bundled_digest
        else:
            matched = reference_sha256 == bundled_sha256
    result["status"] = "matched" if matched else "content_mismatch"
    if sdk_version is not None:
        result["sdkVersion"] = sdk_version
    return result


def validate_bundled_library_qt_sdk_provenance(
    component: str, binding: dict[str, object], require_provenance: bool = False
) -> str | None:
    """Companion to validate_bundled_library_package_provenance() for
    components authenticated via
    bind_bundled_library_to_qt_sdk_provenance() above, mirroring its
    require_provenance semantics: "content_mismatch" is always a hard
    failure; "not_found" is a hard failure only when `require_provenance`
    is True (this project's own pinned `ubuntu-22.04` `appimage-smoke` CI
    job passes --require-package-provenance and runs against the real,
    final AppImage with --qt-reference-dir always supplied, where by
    construction the real Qt SDK reference copy genuinely exists);
    without it, "not_found" remains a legitimate skip on a host that
    never supplied --qt-reference-dir at all (this module's own
    basename-only classification unit tests, or a caller with no Qt SDK
    reference available).

    Round-N+ review (MEDIUM/HIGH, "require_provenance accepts
    qt_reference_dir_unavailable"): unlike "not_found" above,
    "qt_reference_dir_unavailable" previously returned None
    UNCONDITIONALLY, before any require_provenance branching at all --
    meaning even this project's own pinned, --require-package-
    provenance CI run would silently pass if `--qt-reference-dir` were
    ever accidentally omitted from that exact invocation, rather than
    hard-failing the way an equivalently-missing distro-provenance
    manifest already does (see cmd_classify()'s own
    `require_package_provenance and distro_provenance_manifest is None`
    check). It is now a hard failure under `require_provenance` too,
    exactly mirroring "not_found"'s own semantics.

    Round-N+ review (HIGH, "version-only/self-attested"): whenever
    `binding` carries sdkSourceProvenance at all, it is compared against
    this repository's own checked-in Qt lock exactly -- version, source
    URL, and pinned metadata sha256 alike. When `require_provenance` is
    True, a missing sdkVersion or sdkSourceProvenance is itself also a
    hard failure."""
    status = binding["status"]
    if status == "qt_reference_dir_unavailable":
        if not require_provenance:
            return None
        return (
            f"component {component!r} requires --require-package-"
            "provenance's Qt SDK reference check to actually run, but "
            "no --qt-reference-dir was supplied to this invocation at "
            "all -- provenance must be provable here, never silently "
            "skipped merely because the flag proving it was omitted"
        )
    if status == "not_found":
        if not require_provenance:
            return None
        return (
            f"component {component!r} expects a real Qt SDK reference "
            f"copy ({binding.get('referencePath')!r}) proving this "
            "file's genuine Qt-SDK-bundled origin, but none exists -- "
            "its provenance must be provable here, never silently "
            "skipped"
        )
    if status == "content_mismatch":
        return (
            f"component {component!r}'s bundled library does not "
            "byte-for-byte match (via _canonical_load_digest(), which "
            "already tolerates every field patchelf's own documented "
            "RUNPATH rewrite may alter) the real Qt SDK's own reference "
            f"copy at {binding.get('referencePath')!r} -- the bundled "
            "file cannot be proven to actually be that SDK's own build "
            "output at all"
        )
    expected_sdk_source_provenance = _qt_sdk_source_provenance()
    actual_sdk_source_provenance = binding.get("sdkSourceProvenance")
    if actual_sdk_source_provenance is None:
        if require_provenance:
            return (
                f"component {component!r} requires a complete Qt SDK provenance "
                "binding, but its serialized binding omitted "
                "'sdkSourceProvenance' entirely"
            )
    elif actual_sdk_source_provenance != expected_sdk_source_provenance:
        return (
            f"component {component!r}'s serialized Qt SDK source provenance "
            f"{actual_sdk_source_provenance!r} does not EXACTLY match this "
            f"repository's checked-in lock {expected_sdk_source_provenance!r}"
        )
    sdk_version = binding.get("sdkVersion")
    if sdk_version is not None and sdk_version != EXPECTED_QT_SDK_VERSION:
        return (
            f"component {component!r}'s bundled library claims Qt SDK "
            f"version {sdk_version!r}, but this project's own governed "
            f"pin is EXACTLY {EXPECTED_QT_SDK_VERSION!r} -- ANY drift "
            "must be deliberately reviewed and re-pinned in "
            "EXPECTED_QT_SDK_VERSION, never silently absorbed"
        )
    if (
        sdk_version is not None
        and actual_sdk_source_provenance is not None
        and sdk_version != actual_sdk_source_provenance.get("sdkVersion")
    ):
        return (
            f"component {component!r}'s binding claims runtime Qt SDK version "
            f"{sdk_version!r}, but its serialized sdkSourceProvenance pin says "
            f"{actual_sdk_source_provenance.get('sdkVersion')!r}"
        )
    if sdk_version is None and require_provenance:
        return (
            f"component {component!r} requires --require-package-"
            "provenance's Qt SDK version check to actually run, but no "
            "--qt-sdk-version was supplied to this invocation at all -- "
            "the governed EXPECTED_QT_SDK_VERSION pin can only protect a "
            "run that actually supplies something to check it against"
        )
    return None



def compute_qt_sdk_bindings(
    by_component: dict[str, list[Path]],
    qt_reference_dir: Path | None,
    sdk_version: str | None = None,
) -> dict[Path, dict[str, object]]:
    """Computes bind_bundled_library_to_qt_sdk_provenance() EXACTLY ONCE
    per bundled file in `by_component` whose component is in
    _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE ("icu" and "qt"),
    returning a dict keyed by each file's own Path.

    Round-N+ review ("build_sbom_inventory binds separately before
    cmd_classify later rebind/validate, so serialized object not
    necessarily exact validated proof" / "produce ONE immutable
    validation-binding object per artifact used both for acceptance and
    SBOM ... Remove second lookup/rebind ... Ensure reference mutation/
    race impossible (open descriptor/hash once or verify identity)"):
    prior to this, build_sbom_inventory() and cmd_classify()'s own
    provenance-validation loop each independently called
    bind_bundled_library_to_qt_sdk_provenance() a SECOND time for every
    qt/icu file -- meaning the object actually serialized into the SBOM
    was never provably the exact same computation cmd_classify() used
    to decide pass/fail; a change to either the bundled file or the
    real Qt SDK reference tree occurring between those two independent
    calls (however narrow a window) could make the SBOM's own recorded
    "qtSdkProvenance" diverge from what was actually validated.

    cmd_classify() calls this function exactly once, immediately after
    classify_all(), and passes the SAME resulting dict into BOTH
    build_sbom_inventory() (to populate each entry's "qtSdkProvenance"
    field) and its own provenance-validation loop -- so both the
    accepted/rejected decision and the serialized SBOM record are
    provably the one and only binding ever computed for that file this
    invocation, closing the reference-mutation/race window entirely (a
    file changing after this function returns can no longer affect
    validation OR the SBOM differently, since both already read from
    the same already-computed dict)."""
    bindings: dict[Path, dict[str, object]] = {}
    sdk_source_provenance = _qt_sdk_source_provenance()
    for component, paths in by_component.items():
        if component not in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE:
            continue
        for path in paths:
            bindings[path] = bind_bundled_library_to_qt_sdk_provenance(
                path, qt_reference_dir, sdk_version, sdk_source_provenance
            )
    return bindings


# Qt ships its own plugins (image format decoders, platform integrations,
# TLS backends, SQL drivers, etc.) under a fixed, well-documented set of
# subdirectory names -- see Qt's own "Deploying Qt Plugins" documentation
# -- regardless of each plugin's own basename (e.g. imageformats/libqjpeg.so,
# platforms/libqxcb.so, generic/libqoffscreen.so; this project's own
# build-appimage.sh explicitly force-includes "libqoffscreen.so" via
# EXTRA_PLATFORM_PLUGINS and passes `--plugin qt` to linuxdeploy). None of
# these basenames match the `libQt6.*` prefix pattern above, so without
# this directory-based fallback every single bundled Qt plugin would be
# reported unmapped the first time this classifier ran against a real
# AppImage -- they are still an inseparable, officially-distributed part
# of the same Qt project documented in third_party/qt/NOTICE.md, not a
# distinct third-party dependency requiring its own notice directory.
#
# IMPORTANT: a directory name being one of these alone is NOT sufficient
# to classify a file as "qt" -- see classify_path()'s qt_reference_dir
# parameter. A later cumulative review correctly found that "any .so
# found inside a directory with one of these names is Qt" is fail-open:
# an attacker (or a broken build step) could drop an arbitrary,
# unaudited `.so` directly into e.g. `usr/lib/plugins/platforms/` and
# have it silently accepted as legitimate, notice-covered Qt content.
# classify_path() now additionally requires that a file with the exact
# same relative sub-path genuinely exists under the real Qt SDK
# installation used for this build (passed in as qt_reference_dir, e.g.
# `$QT_ROOT_DIR` as exported by jurplel/install-qt-action) before ever
# returning "qt" for a directory-matched file; without a reference
# directory, directory-based Qt-plugin/QML classification is refused
# entirely (fails closed) rather than trusting the path alone.
QT_PLUGIN_DIRECTORIES: frozenset[str] = frozenset(
    {
        "imageformats",
        "platforms",
        "platforminputcontexts",
        "platformthemes",
        "generic",
        "iconengines",
        "styles",
        "sqldrivers",
        "tls",
        "networkinformation",
        "wayland-decoration-client",
        "wayland-graphics-integration-client",
        "wayland-shell-integration",
        "xcbglintegrations",
        "egldeviceintegrations",
        "printsupport",
    }
)

# Qt Quick/QML modules (Controls styles, Layouts, LocalStorage, Particles,
# Shapes, Window, ...) are deployed by linuxdeploy's Qt plugin under a
# fixed top-level "qml" directory name (e.g.
# usr/qml/QtQuick/Controls/libqtquickcontrols2plugin.so,
# usr/qml/QtQml/Models/libmodelsplugin.so) -- a second real, previously
# unanticipated case of "official Qt content whose basename never matches
# libQt6.*", exactly analogous to QT_PLUGIN_DIRECTORIES above but for
# QML rather than C++ plugins. Every one of these was found unmapped by a
# later cumulative review the first time this classifier ran against the
# real produced AppImage's full `usr/` tree (previously only validated
# against `usr/lib`). Matched as a whole path *component* named "qml"
# appearing anywhere in the path (not just as the immediate parent, since
# real QML modules nest arbitrarily deep, e.g.
# usr/qml/QtQuick/Controls/Basic/impl/...) -- never a bare substring
# test, so a hypothetical unrelated "libqmlfoo.so" is never
# misclassified purely by name. As with QT_PLUGIN_DIRECTORIES, the "qml"
# directory name alone is not sufficient; see classify_path()'s
# qt_reference_dir parameter.
QT_QML_ROOT_DIRNAME: str = "qml"


def classify(basename: str) -> str | None:
    """Returns the component name a bundled library's basename belongs to,
    or None if it matches neither the ABI allowlist nor any known
    component -- callers distinguish "allowlisted, no notice needed" from
    "unmapped, must fail" by separately checking membership in
    ABI_ALLOWLIST themselves (this function alone cannot distinguish the
    two None-returning cases, deliberately keeping it a single simple
    lookup with no hidden allowlist special-casing to audit)."""
    for pattern, component in COMPONENT_PATTERNS:
        if pattern.match(basename):
            return component
    return None


def _resolve_qt_sdk_reference_candidate(path: Path, qt_reference_dir: Path) -> Path:
    """Single, shared implementation of "where would the real Qt SDK's
    own copy of this exact bundled file live" -- used by BOTH
    classification (_is_real_qt_plugin_or_qml_module()/
    _is_real_core_qt_library(), via classify_path()) AND
    bind_bundled_library_to_qt_sdk_provenance() (the richer SBOM/
    provenance binding consumed by cmd_classify()/build_sbom_inventory()).

    Round-N+ review ("qtSdkProvenance included only ICU, not core
    Qt/plugins using same classification"): prior to this,
    bind_bundled_library_to_qt_sdk_provenance() unconditionally assumed
    the ICU-only `lib/<basename>` shape, which is wrong for a real Qt
    plugin (`plugins/<subdir>/<basename>`) or QML module (anywhere
    beneath a literal `qml` path component) -- extending qtSdkProvenance
    to those components required a single, correct, shape-aware
    resolver reused everywhere, rather than duplicating (and risking
    divergence between) this logic across classification and provenance
    binding.

    Three shapes, identical to classify_path()'s own directory-based
    dispatch:
      - a Qt plugin (path's immediate parent directory name is one of
        QT_PLUGIN_DIRECTORIES): qt_reference_dir/plugins/<dirname>/<basename>
      - a Qt QML module (a literal "qml" path component appears
        anywhere in path's parts): qt_reference_dir/qml/<sub-path
        beneath that "qml" component>
      - otherwise (a core Qt library such as libQt6Core.so.6, OR ICU,
        which Qt's own SDK bundles directly alongside its core
        libraries -- see _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE's
        own docstring): qt_reference_dir/lib/<basename>, the original,
        still-correct fallback shape for both of those cases."""
    if path.parent.name in QT_PLUGIN_DIRECTORIES:
        return qt_reference_dir / "plugins" / path.parent.name / path.name
    if QT_QML_ROOT_DIRNAME in path.parts:
        qml_index = path.parts.index(QT_QML_ROOT_DIRNAME)
        sub_path = Path(*path.parts[qml_index + 1 :])
        return qt_reference_dir / QT_QML_ROOT_DIRNAME / sub_path
    return qt_reference_dir / "lib" / path.name


def _is_real_qt_plugin_or_qml_module(path: Path, qt_reference_dir: Path) -> bool:
    """Returns True only if a file with the SAME relative sub-path (plugin
    subdirectory + basename, or the full path beneath the "qml" root)
    genuinely exists under qt_reference_dir -- the real Qt SDK
    installation actually used to build this project (e.g. `$QT_ROOT_DIR`
    as exported by jurplel/install-qt-action in CI) -- AND, whenever a
    real bundled file is actually being classified (see the path.is_file()
    note below), that final bundled file is provably the SAME compiled
    object as that reference copy, not merely a same-named/same-path
    substitute.

    A cumulative review correctly found that same-relative-path existence
    ALONE is still fail-open: an attacker (or a broken build step) could
    replace the genuine Qt plugin at that exact path with an arbitrary
    binary and this check would previously still accept it, since it
    never actually inspected the bundled file's own content. Bytes alone
    cannot be the sole proof either, in the other direction:
    linuxdeploy's patchelf step legitimately rewrites the bundled copy's
    RUNPATH/interpreter after copying it out of the SDK, so a genuine,
    entirely unmodified-in-substance Qt plugin will NOT be byte-identical
    to the pre-patchelf reference copy.

    Round-N+ review (HIGH, "Qt authenticity accepts matching build ID
    only; modified bytes can retain note. Build ID not signature." then
    "Qt canonical digest hashes only PF_X PT_LOAD, missing
    behavior-bearing loaded data/program headers"): the correct, precise
    signal is each object's own canonical loaded-content digest (see
    _canonical_load_digest()'s own docstring) -- a real cryptographic
    digest over every section the loader ever actually maps into memory
    (not merely the subset already flagged executable), plus each
    surviving section's own runtime mapping permissions -- which
    patchelf/strip provably never modify -- NOT a bare `.note.gnu.
    build-id` match, which is merely a stored identifier nothing ever
    re-verifies against the file's current bytes: a tool capable of
    patching loaded code or data in place could trivially leave an old,
    correct-looking build-id note untouched, and a build-id-only check
    would have silently accepted the substitution. If either copy has no
    loaded (SHF_ALLOC) section at all (vanishingly rare for a real
    bundled library/plugin, but not itself an error), this falls back to
    requiring the two files be fully byte-identical -- deliberately the
    strict, fail-closed choice for that rarer case, rather than silently
    trusting the path alone.

    `path.is_file()` is checked before performing this content
    verification at all: production callers (classify_all(), via
    find_bundled_libraries()'s real filesystem walk) always pass a real,
    existing bundled file, so this content check is always actually
    exercised where it matters. A caller exercising only the pure
    subpath-resolution logic against a path that was never materialized
    on disk (as this module's own unit tests do, deliberately without
    needing a real ELF toolchain for that narrower purpose) has, by
    construction, no bundled bytes to substitute an attack into in the
    first place -- there is nothing for a content check to meaningfully
    protect against for a file that was never written -- so this is not
    a security-relevant weakening of the real, on-disk code path."""
    if path.parent.name not in QT_PLUGIN_DIRECTORIES and QT_QML_ROOT_DIRNAME not in path.parts:
        return False
    candidate = _resolve_qt_sdk_reference_candidate(path, qt_reference_dir)
    return _is_same_compiled_object_or_unwritten(candidate, path)


def _is_same_compiled_object_or_unwritten(candidate: Path, path: Path) -> bool:
    """Shared same-compiled-object proof used by both
    _is_real_qt_plugin_or_qml_module() (Qt plugins/QML modules) and
    _is_real_core_qt_library() (core libQt6*.so* libraries): see the
    former's own docstring for why a genuine cryptographic digest over
    each object's own canonical loaded-content (falling back to a
    full byte comparison only when neither has any loaded section) is
    the correct signal, rather than a bare path/name match, a bare
    `.note.gnu.build-id` match (not a signature -- see
    _read_build_id()'s updated docstring), or a bare byte-identical
    comparison alone. `path.is_file()` is checked first for exactly the
    same reason documented there -- production callers always pass a
    real, existing bundled file; this module's own pure-logic unit
    tests (deliberately not needing a real ELF toolchain) may not."""
    if not candidate.is_file():
        return False
    if not path.is_file():
        return True

    reference_digest = _canonical_load_digest(candidate)
    bundled_digest = _canonical_load_digest(path)
    if reference_digest is not None and bundled_digest is not None:
        return reference_digest == bundled_digest
    return _sha256(candidate) == _sha256(path)


# Round-9+ review item 10 ("core Qt classified by basename only and
# unauthenticated"): the core Qt shared libraries themselves (e.g.
# libQt6Core.so.6) -- as opposed to Qt's own plugins/QML modules, which
# already require the _is_real_qt_plugin_or_qml_module() authentication
# above -- were previously classified purely by classify()'s unauthenticated
# `^libQt6.*\.so` basename pattern, with no verification against the real
# Qt SDK at all. A hostile or substituted file merely NAMED e.g.
# "libQt6Backdoor.so.6" or a byte-swapped "libQt6Core.so.6" would
# previously be silently accepted as genuine, notice-covered Qt content.
_CORE_QT_LIBRARY_RE = re.compile(r"^libQt6.*\.so")


def _is_real_core_qt_library(path: Path, qt_reference_dir: Path) -> bool:
    """True only if path's basename matches the core Qt library naming
    convention AND a file at the identical relative path
    (qt_reference_dir/lib/<basename>) genuinely exists and is proven, by
    the same build-id-or-byte-content check used for Qt plugins/QML
    modules, to be the SAME compiled object. See
    _is_real_qt_plugin_or_qml_module()'s docstring for the full
    build-id/patchelf rationale, which applies identically here."""
    if not _CORE_QT_LIBRARY_RE.match(path.name):
        return False
    candidate = _resolve_qt_sdk_reference_candidate(path, qt_reference_dir)
    return _is_same_compiled_object_or_unwritten(candidate, path)


def classify_path(path: Path, qt_reference_dir: Path | None = None) -> str | None:
    """Like classify(), but additionally resolves any library located
    directly inside one of Qt's own standardized plugin subdirectories
    (QT_PLUGIN_DIRECTORIES), anywhere beneath a literal "qml" path
    component (QT_QML_ROOT_DIRNAME), or matching the core Qt library
    naming convention (libQt6*.so*), to the "qt" component -- but, unlike
    an earlier version of this function, ONLY if qt_reference_dir is
    supplied AND the bundled file is verified, by build-id (or, absent
    one, full byte content), to be the SAME compiled object as the file
    at the identical relative sub-path under it (see
    _is_real_qt_plugin_or_qml_module()'s docstring for why this
    authoritative cross-check is required, not merely the directory name,
    basename pattern, or path existence alone). If qt_reference_dir is
    None, directory/basename-based Qt classification is refused entirely
    (returns None, i.e. unmapped, rather than trusting the path or name)
    -- callers that omit it therefore fail closed rather than silently
    reintroducing the fail-open behavior a cumulative review found and
    required be fixed.

    Round-9+ review item 10: a file whose basename merely happens to
    match the libQt6*.so* naming convention (e.g. a hostile
    "libQt6Backdoor.so.6", or a substituted "libQt6Core.so.6" with
    different bytes/build-id) is deliberately NEVER classified as "qt"
    via classify()'s own plain, unauthenticated COMPONENT_PATTERNS
    lookup below whenever qt_reference_dir is available -- only a file
    proven (by _is_real_core_qt_library()) to be the same compiled
    object as the genuine file at the same relative path under the real
    Qt SDK is accepted. classify()'s own libQt6 pattern remains reachable
    only as an explicit, intentionally-lenient fallback for callers that
    never had a Qt SDK reference available in the first place (e.g. this
    module's own basename-only unit tests, or a caller auditing a lib
    directory produced by a build this script has no Qt SDK access for)."""
    if qt_reference_dir is not None:
        if _is_real_qt_plugin_or_qml_module(path, qt_reference_dir):
            return "qt"
        if _CORE_QT_LIBRARY_RE.match(path.name):
            return "qt" if _is_real_core_qt_library(path, qt_reference_dir) else None
    return classify(path.name)


# Round-9+ review item 10 ("rglob *.so* omits main executable, helper
# ELFs, AppRun; ... explicitly classify first-party executables"): this
# project's own compiled artifacts that a real produced AppImage bundles
# alongside its third-party dependencies -- the main application
# executable and linuxdeploy's generated AppRun launcher -- are neither
# a third-party COMPONENT_PATTERNS match nor covered by ABI_ALLOWLIST
# (which is exclusively for host-provided system libraries). They need
# no third-party notice (this project owns their copyright itself), but
# must never be silently absent from the SBOM/audit, nor be reported as
# an unmapped/unknown bundled binary (which would fail packaging).
#
# This module is invoked with two different, both legitimate, values of
# `lib_dir`: packaging/lib/bundle_codec_notices.sh (the pre-packaging
# AppDir build step) scans only "$app_dir/usr", while the final CI
# verify-notices step scans the FULL extracted AppImage root (so it can
# also discover AppRun, which lives as a sibling of usr/, not beneath
# it). The main executable's relative path is therefore either
# "usr/bin/arkham-horror" or "bin/arkham-horror" depending on which root
# was scanned.
#
# A real produced AppImage's linuxdeploy-plugin-qt run also emits a
# second, distinct file at the AppDir root: it deploys AppRun as a
# symlink to usr/bin/arkham-horror during the first ("populate")
# linuxdeploy invocation, then, on the second ("package") invocation
# (needed for --plugin qt's apprun-hooks mechanism), detects that
# existing AppRun, renames it to "AppRun.wrapped", and writes its own
# generated launcher stub as the new "AppRun" (which execs
# AppRun.wrapped after running environment-setup hooks such as the Qt
# plugin's). AppRun.wrapped is therefore always the SAME symlink to this
# project's own executable under one more linuxdeploy-generated alias,
# not a distinct third-party artifact -- observed only by actually
# running the CI packaging workflow, not by this test suite alone,
# since neither a local macOS build nor the earlier fixtures exercise
# linuxdeploy's own two-invocation renaming behavior.
#
# Expressed below as a closed set of exact relative-path SUFFIXES
# (matched against the tail of path.relative_to(lib_dir).parts, never a
# basename-only match): a hostile file placed at some OTHER path merely
# sharing one of these basenames -- e.g.
# "usr/lib/plugins/generic/arkham-horror" -- still does not match any
# suffix and must still be classified normally (and fail if unmapped).
FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES: frozenset[tuple[str, ...]] = frozenset(
    {
        ("usr", "bin", "arkham-horror"),
        ("bin", "arkham-horror"),
        ("AppRun",),
        ("AppRun.wrapped",),
    }
)


def _is_first_party_executable(path: Path, lib_dir: Path) -> bool:
    """True only if path's relative-to-lib_dir path components end with
    one of FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES exactly -- see
    that constant's docstring for why a suffix match (rather than a
    single fixed full relative path) is required here, and why it is
    still not a basename-only match."""
    relative_parts = path.relative_to(lib_dir).parts
    for suffix in FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES:
        if len(relative_parts) >= len(suffix) and relative_parts[-len(suffix) :] == suffix:
            return True
    return False


def find_bundled_libraries(lib_dir: Path) -> list[Path]:
    """Every real ELF object (regular file, or symlink resolving to one)
    found anywhere under lib_dir, recursively, discovered by inspecting
    each candidate file's own leading magic bytes -- covers usr/lib,
    per-arch subdirectories, and plugin directories (e.g.
    imageformats/) alike, since linuxdeploy's exact placement is an
    implementation detail this must not hard-code one particular layout
    for (mirroring audit_dependency_closure.py's own doc-comment
    reasoning).

    Round-9+ review item 10 ("rglob *.so* omits main executable, helper
    ELFs, AppRun"): this previously globbed only for `*.so*` basenames,
    which structurally can never discover a bundled ELF whose own
    basename simply does not happen to contain ".so" -- this project's
    own main application executable (usr/bin/arkham-horror) and
    linuxdeploy's generated AppRun launcher are exactly such files, and
    were previously entirely invisible to this script: never classified,
    never notice-checked, and never even listed in the SBOM inventory.
    Every regular file (following symlinks) anywhere under lib_dir is
    now inspected by its own actual magic bytes instead, so discovery no
    longer depends on any filename convention a bundled ELF is not
    actually obligated to follow -- this also naturally discovers any
    other unanticipated "helper" ELF (a small bundled tool/launcher with
    no ".so" in its name at all) the same way."""
    return sorted(p for p in lib_dir.rglob("*") if p.is_file() and _is_elf_file(p))


def classify_all(
    lib_dir: Path,
    qt_reference_dir: Path | None = None,
) -> tuple[dict[str, list[Path]], list[Path]]:
    """Returns (component -> [paths requiring that component's notice],
    unmapped_paths). ABI_ALLOWLIST-covered libraries and this project's
    own first-party executables (see
    FIRST_PARTY_EXECUTABLE_RELATIVE_PATH_SUFFIXES) are excluded from both
    (neither needs a third-party notice, and neither is a failure).
    qt_reference_dir is forwarded to classify_path() -- see its
    docstring; omitting it means every directory/basename-matched Qt
    plugin/QML module/core library is reported unmapped."""
    by_component: dict[str, list[Path]] = {}
    unmapped: list[Path] = []
    for path in find_bundled_libraries(lib_dir):
        basename = path.name
        if basename in ABI_ALLOWLIST:
            continue
        if _is_first_party_executable(path, lib_dir):
            continue
        component = classify_path(path, qt_reference_dir)
        if component is None:
            unmapped.append(path)
            continue
        by_component.setdefault(component, []).append(path)
    return by_component, unmapped


def build_sbom_inventory(
    lib_dir: Path,
    qt_reference_dir: Path | None = None,
    distro_provenance_manifest: dict[str, object] | None = None,
    qt_sdk_bindings: dict[Path, dict[str, object]] | None = None,
    qt_sdk_version: str | None = None,
) -> list[dict[str, object]]:
    """Every real bundled ELF found under lib_dir, with NO exclusions --
    unlike classify_all() above (whose whole purpose is deciding what
    needs a *notice*, and therefore deliberately excludes
    ABI_ALLOWLIST-covered and first-party-executable libraries as "needs
    nothing"), a review finding specifically required that the
    SBOM/manifest inventory itself never silently omit a bundled file
    merely because it happens to be allowlisted or first-party: each
    classification is itself a meaningful, auditable fact ("this file is
    trusted to be host-provided, not bundled for its own component
    notice", or "this file is this project's own compiled artifact, not
    a third-party dependency at all") that a complete inventory must
    still record, not a reason to leave the file invisible.

    Each entry: path (relative to lib_dir), basename, classification
    ("allowlisted", "first-party", a COMPONENT_PATTERNS/Qt component
    name, or "unmapped"), elf_identity()'s sha256/buildId/
    canonicalLoadDigest/soname fields, packageProvenance (see
    bind_bundled_library_to_system_provenance()'s own docstring --
    Third-HIGH-round review, "SBOM calls legacy basename capture
    instead of validated binding": this field used to be populated via
    the older capture_package_provenance(basename), which -- exactly as
    that function's own docstring already warns -- only ever proves a
    claim about *some same-basename file the current host happens to
    have installed*, never actually inspecting `path` (the real bundled
    file THIS entry describes) at all; a substituted/downgraded bundled
    library could therefore have "matched" a same-named-but-unrelated
    system file with no real cryptographic connection between the two
    whatsoever. Using the full, content-bound
    bind_bundled_library_to_system_provenance() binding here instead
    means the SBOM's own packageProvenance field for every entry is
    ALREADY the validated result -- "status": "matched" (with
    "package"/"version"/"sourcePackage"/"systemPath"/"systemSha256"/
    "bundledCanonicalLoadDigest"), "content_mismatch" (a same-basename
    system file exists but is cryptographically proven to be a
    DIFFERENT build than this bundled file, plus its own
    "systemPath"/"systemSha256"/"systemCanonicalLoadDigest" for
    independent reconstruction), "not_dpkg_owned", "not_found", or
    "dpkg_unavailable") -- never a bare, unvalidated basename re-lookup
    a consumer would have to independently re-verify), and -- for
    classification in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE
    ("qt" and "icu", neither of which has a legitimate dpkg-owned
    counterpart in this project's pipeline for the bundled files this
    check governs; see that constant's own docstring) -- a
    qtSdkProvenance field recording
    bind_bundled_library_to_qt_sdk_provenance()'s own result instead, so
    this entry's real, verifiable upstream origin is never silently
    represented as merely "packageProvenance: null" with no explanation
    -- every final bundled ELF, fully identified, with an explicit,
    reviewable disposition; nothing bundled is ever left out of this
    list.

    Round-N+ review (HIGH, "distro provenance post-hoc/unpinned ...
    capture exact loader/copy source BEFORE packaging ... No basename
    re-discovery"): when `distro_provenance_manifest` is supplied (the
    JSON object produced by capture_distro_source_provenance() BEFORE
    packaging -- see packaging/build-appimage.sh's own "Capture distro
    provenance" step), packageProvenance is populated via
    bind_bundled_library_to_captured_provenance() instead -- which
    performs NO independent system search of its own at all, only a
    direct manifest lookup -- so a real, governed build/CI invocation
    never falls back to the older, basename-search-based
    bind_bundled_library_to_system_provenance(). Omitting it (the
    default) preserves that older, best-effort behavior for a
    non-governed/local/partial-host invocation that has no manifest to
    supply at all.

    Round-N+ review ("qtSdkProvenance included only ICU, not core
    Qt/plugins using same classification" / "Remove second lookup/
    rebind ... Ensure reference mutation/race impossible"): when
    `qt_sdk_bindings` is supplied (the dict produced by
    compute_qt_sdk_bindings(), computed ONCE by cmd_classify()
    immediately after classify_all() and shared with its own
    provenance-validation loop), each qt/icu entry's "qtSdkProvenance"
    is looked up directly from it -- never recomputed here -- so the
    exact object serialized into the SBOM is provably the same one
    cmd_classify() itself validated, not a second, independent,
    possibly time-of-check/time-of-use-divergent computation. Omitting
    it (the default, e.g. this module's own direct unit-test callers)
    falls back to computing it inline via
    bind_bundled_library_to_qt_sdk_provenance() for backward
    compatibility with callers that only ever need the SBOM in
    isolation. `qt_sdk_version` is forwarded to that inline fallback
    call only (bindings already present in a supplied `qt_sdk_bindings`
    dict already carry whatever "sdkVersion" compute_qt_sdk_bindings()
    was invoked with)."""
    entries: list[dict[str, object]] = []
    for path in find_bundled_libraries(lib_dir):
        basename = path.name
        relative_path = str(path.relative_to(lib_dir))
        if basename in ABI_ALLOWLIST:
            classification = "allowlisted"
        elif _is_first_party_executable(path, lib_dir):
            classification = "first-party"
        else:
            component = classify_path(path, qt_reference_dir)
            classification = component if component is not None else "unmapped"
        entry: dict[str, object] = {
            "path": relative_path,
            "basename": basename,
            "classification": classification,
        }
        entry.update(elf_identity(path))
        if distro_provenance_manifest is not None:
            entry["packageProvenance"] = bind_bundled_library_to_captured_provenance(
                path, distro_provenance_manifest, relative_path
            )
        else:
            entry["packageProvenance"] = bind_bundled_library_to_system_provenance(path)
        if classification in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE:
            if qt_sdk_bindings is not None and path in qt_sdk_bindings:
                entry["qtSdkProvenance"] = qt_sdk_bindings[path]
            else:
                entry["qtSdkProvenance"] = bind_bundled_library_to_qt_sdk_provenance(
                    path, qt_reference_dir, qt_sdk_version
                )
        entries.append(entry)
    entries.sort(key=lambda e: str(e["path"]))
    return entries


def cmd_capture_distro_provenance(args: argparse.Namespace) -> int:
    """Handler for the `capture-distro-provenance` CLI subcommand: runs
    resolve_ldd_dependencies() against every ELF path in args.elf_paths
    (this project's own pre-packaging, first-party executable and/or Qt
    plugins), collects EVERY distinct real resolved path any requester
    reports for a given soname (Round-N+ review, HIGH, "distro
    provenance collapses identity ... keyed SONAME/basename overwrites
    different requester resolutions": two different first-party
    requesters CAN legitimately resolve the same soname to two
    DIFFERENT real files -- a private/conflicting RPATH, or a force-
    bundled explicit input whose own real identity differs from what an
    unrelated requester's dependency resolution reports -- so this no
    longer collapses to a single "winner" the way a plain dict merge
    would), captures full provenance for every distinct candidate via
    capture_distro_source_provenance(), and writes the result as JSON to
    args.output -- the manifest later consumed by `classify
    --distro-provenance-manifest`.

    `appimage-smoke` regression ("component 'libsecret' expects a real,
    dpkg-owned system library ... but none exists"): resolve_ldd_
    dependencies(elf_path) only ever reports elf_path's OWN
    dependencies -- the sonames elf_path itself DT_NEEDED-links against
    -- never elf_path itself. build-appimage.sh's own "Capture distro
    provenance" step passes several force-bundled libraries (libsecret,
    libgpg-error, libgcc_s, libstdc++, zlib, libcom_err -- see that
    script's own comments on why linuxdeploy's automatic ldd-based
    bundling cannot discover them itself) as direct `elf_paths`
    specifically so THEIR OWN provenance gets captured; but unless some
    OTHER scanned ELF file also happens to independently DT_NEEDED-link
    the exact same soname (which the reviewed Qt-plugin/executable
    closure does for some of these but not reliably all -- libsecret-
    1.so.0 in particular is only ever dlopen()'d, never DT_NEEDED-
    linked, by anything this project bundles), that library's own
    soname never appears as a *value* in resolve_ldd_dependencies()'s
    output at all, so it was silently absent from the manifest entirely
    -- a real gap the older, pre-manifest basename-directory-search
    fallback (bind_bundled_library_to_system_provenance()) never had,
    since it searched the system for the bundled basename directly
    rather than depending on some other file happening to reference it.
    Every explicitly given `elf_path` is therefore ALSO captured as its
    own {soname: path} candidate (soname = elf_path.name, the exact real
    file linuxdeploy's own --library flag copies verbatim into the
    AppImage under that same basename) in addition to its resolved
    dependencies -- added as one MORE distinct candidate for that
    soname (deduplicated only if it is the exact same resolved path as
    one already collected), never overwriting any dependency-derived
    candidate the way the old single-winner merge did (both may
    describe the identical real system file in practice, in which case
    they collapse to one candidate via that exact-path dedup; but when
    they genuinely differ, BOTH survive and are independently checked
    downstream). This is a pure superset: first-party inputs with no
    real dpkg owner (arkham-horror itself, Qt's own plugin ELF files)
    simply fail capture_distro_source_provenance()'s own dpkg-ownership
    check and are omitted from the result exactly as before, so this
    cannot spuriously "prove" ownership of anything that truly has
    none."""
    dependency_edges: list[tuple[Path, str, Path]] = []

    missing_inputs: list[Path] = []
    for elf_path in args.elf_paths:
        if not elf_path.is_file():
            missing_inputs.append(elf_path)
            continue
        for soname, resolved_path in resolve_ldd_dependencies(elf_path).items():
            dependency_edges.append((elf_path, soname, resolved_path))
    if missing_inputs:
        print(
            "audit_codec_notices: capture-distro-provenance input(s) not "
            f"found: {[str(p) for p in missing_inputs]!r}",
            file=sys.stderr,
        )
        return 2
    if not dependency_edges:
        print(
            "audit_codec_notices: capture-distro-provenance resolved zero "
            "dependencies across all given ELF path(s) -- is `ldd` "
            "installed, and are the given paths real, dynamically linked "
            "ELF files?",
            file=sys.stderr,
        )
        return 1
    # Applied AFTER (and deliberately not counted towards) the "resolved
    # zero dependencies" `ldd`-sanity check above -- that check exists to
    # catch a broken/missing `ldd` or non-dynamic inputs, which self-
    # entries (always present for any real, existing elf_path) would
    # otherwise permanently mask.
    for elf_path in args.elf_paths:
        dependency_edges.append((elf_path, elf_path.name, elf_path))
    staging_dir = getattr(args, "staging_dir", None) or (
        args.output.parent / ".distro-provenance-stage"
    )
    manifest, conflicts = build_distro_provenance_manifest(dependency_edges, staging_dir)
    if conflicts:
        print(
            "audit_codec_notices: conflicting distro provenance would map "
            "different real source objects to the same bundled destination:",
            file=sys.stderr,
        )
        for conflict in conflicts:
            print(f"  {conflict}", file=sys.stderr)
        return 1
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    captured_candidates = len(manifest["bundledPaths"])  # type: ignore[index]
    print(
        f"audit_codec_notices: captured distro provenance for "
        f"{captured_candidates} exact bundled destination"
        f"{'s' if captured_candidates != 1 else ''} to {args.output}"
    )
    return 0


def cmd_classify(args: argparse.Namespace) -> int:
    lib_dir: Path = args.lib_dir
    if not lib_dir.is_dir():
        print(f"Not a directory: {lib_dir}", file=sys.stderr)
        return 2
    qt_reference_dir: Path | None = args.qt_reference_dir
    if qt_reference_dir is not None and not qt_reference_dir.is_dir():
        print(f"Not a directory: {qt_reference_dir}", file=sys.stderr)
        return 2

    require_package_provenance: bool = getattr(
        args, "require_package_provenance", False
    )
    distro_provenance_manifest_path: Path | None = getattr(
        args, "distro_provenance_manifest", None
    )
    distro_provenance_manifest: dict[str, object] | None = None
    if distro_provenance_manifest_path is not None:
        try:
            distro_provenance_manifest = load_distro_provenance_manifest(
                distro_provenance_manifest_path
            )
        except ValueError as error:
            print(f"audit_codec_notices: {error}", file=sys.stderr)
            return 2
    qt_sdk_version: str | None = getattr(args, "qt_sdk_version", None)

    by_component, unmapped = classify_all(lib_dir, qt_reference_dir)

    # Round-N+ review ("build_sbom_inventory binds separately before
    # cmd_classify later rebind/validate ... Remove second lookup/
    # rebind"): compute every qt/icu file's Qt-SDK-provenance binding
    # EXACTLY ONCE here, immediately after classify_all(), then share
    # this SAME dict with both build_sbom_inventory() (for the "inventory"
    # JSON output below) and this function's own provenance-validation
    # loop further down -- see compute_qt_sdk_bindings()'s own docstring
    # for why this closes the reference-mutation/race window a second,
    # independent recomputation in each of those two places previously
    # left open.
    qt_sdk_bindings = compute_qt_sdk_bindings(by_component, qt_reference_dir, qt_sdk_version)

    if args.json_out is not None:
        manifest = {
            "libDir": str(lib_dir),
            "qtReferenceDir": str(qt_reference_dir) if qt_reference_dir is not None else None,
            "components": {
                component: [str(p.relative_to(lib_dir)) for p in paths]
                for component, paths in sorted(by_component.items())
            },
            "unmapped": [str(p.relative_to(lib_dir)) for p in unmapped],
            # Full inventory of every bundled ELF (including allowlisted
            # ones -- see build_sbom_inventory()'s own docstring for why
            # those must never be silently omitted here) with
            # cryptographic/provenance identity (sha256/build-id/SONAME),
            # per review directive: a true SBOM must let a consumer
            # answer "what, exactly, did we ship" for every single
            # bundled file, not only the subset requiring a notice.
            "inventory": build_sbom_inventory(
                lib_dir,
                qt_reference_dir,
                distro_provenance_manifest,
                qt_sdk_bindings,
            ),
        }
        args.json_out.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")

    missing_mandatory = MANDATORY_COMPONENTS - by_component.keys()
    if missing_mandatory:
        print(
            "audit_codec_notices: mandatory component(s) "
            f"{sorted(missing_mandatory)!r} were not found bundled under "
            f"{lib_dir}.",
            file=sys.stderr,
        )
        return 1

    if unmapped:
        print(
            f"audit_codec_notices: {len(unmapped)} bundled librar"
            f"{'y is' if len(unmapped) == 1 else 'ies are'} not mapped to any "
            "known third-party component -- add a COMPONENT_PATTERNS entry "
            "(and a matching third_party/<component>/ notice directory) in "
            "packaging/audit_codec_notices.py for:",
            file=sys.stderr,
        )
        for path in unmapped:
            print(f"  {path}", file=sys.stderr)
        return 1

    # Round-7 review (HIGH, "distro provenance looks up an unrelated
    # host file by basename and is not cryptographically bound to the
    # bundled input/final ELF; missing provenance passes; package
    # versions are mutable/unpinned"): cross-check every classified
    # distro-packaged component's ACTUAL BUNDLED FILE -- not merely a
    # same-named file found anywhere on the host -- against a real,
    # dpkg-owned system copy, requiring the two to be
    # cryptographically proven identical (via
    # bind_bundled_library_to_system_provenance()'s own
    # _canonical_load_digest() comparison) before trusting that system
    # copy's dpkg-derived source package/version at all. On a real,
    # dpkg-equipped host (this project's own pinned `ubuntu-22.04`
    # `appimage-smoke` CI job, in particular) an expected distro
    # component's provenance can no longer silently pass merely because
    # it was never found, never dpkg-owned, or diverged in content --
    # see validate_bundled_library_package_provenance()'s own
    # docstring for exactly which outcomes that now covers. A content
    # mismatch is always a hard failure, everywhere; "not found"/"not
    # dpkg-owned" are additionally hard failures only when
    # --require-package-provenance is passed (this project's own pinned
    # `ubuntu-22.04` `appimage-smoke` CI job -- see
    # .github/workflows/ci.yml -- passes it explicitly, since that job
    # runs against the real, final AppImage where every expected distro
    # library's system counterpart genuinely IS installed); without it,
    # it remains a harmless, skipped no-op on a merely dpkg-equipped but
    # incomplete host (a partial local dev container, or this file's own
    # basename-only classification unit tests).
    #
    # Round-N+ review (HIGH, "No basename re-discovery"): when a real
    # distro_provenance_manifest was captured BEFORE packaging (see
    # this function's own earlier --require-package-provenance/
    # --distro-provenance-manifest handling above), every distro
    # component's binding below is looked up directly in that manifest
    # via bind_bundled_library_to_captured_provenance() -- never
    # re-discovered afterward by an independent basename search.
    #
    # When require_package_provenance is set but no manifest was
    # supplied, an authoritative check would otherwise silently fall
    # back to the after-the-fact basename search below for any REAL
    # distro-packaged component present (never for Qt-SDK-provenance
    # components such as "icu", which never go through that fallback at
    # all) -- reopening exactly the defect the manifest mechanism
    # exists to close. Fail closed with an explicit configuration error
    # instead, but only when a real distro component that would
    # actually need it is present at all, so --require-package-
    # provenance remains usable on its own for an icu-only/Qt-SDK-only
    # classification.
    if require_package_provenance and distro_provenance_manifest is None:
        distro_components_present = sorted(
            component
            for component in by_component
            if component not in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE
        )
        if distro_components_present:
            print(
                "audit_codec_notices: --require-package-provenance requires "
                "--distro-provenance-manifest (a manifest captured BEFORE "
                "packaging via the capture-distro-provenance subcommand) "
                f"for real distro component(s) {distro_components_present!r} "
                "-- falling back to an after-the-fact basename search is "
                "exactly the defect this flag combination exists to "
                "prevent.",
                file=sys.stderr,
            )
            return 2

    provenance_problems: list[str] = []
    for component, paths in sorted(by_component.items()):
        for path in paths:
            # New review item ("ICU library package-provenance
            # mismatch"): components in
            # _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE ("icu" and
            # "qt") never originate from a dpkg-owned distro package in
            # this project's actual pipeline -- see that constant's own
            # docstring -- so their provenance is authenticated against
            # the real Qt SDK reference copy instead of the host's dpkg
            # database. Round-N+ review ("Remove second lookup/
            # rebind"): reuses the SAME qt_sdk_bindings dict computed
            # once above (via compute_qt_sdk_bindings(), immediately
            # after classify_all()) rather than calling
            # bind_bundled_library_to_qt_sdk_provenance() again here --
            # see compute_qt_sdk_bindings()'s own docstring.
            if component in _COMPONENTS_WITH_QT_SDK_BUNDLED_PROVENANCE:
                qt_sdk_binding = qt_sdk_bindings[path]
                problem = validate_bundled_library_qt_sdk_provenance(
                    component, qt_sdk_binding, require_package_provenance
                )
            elif distro_provenance_manifest is not None:
                binding = bind_bundled_library_to_captured_provenance(
                    path, distro_provenance_manifest, str(path.relative_to(lib_dir))
                )
                problem = validate_bundled_library_package_provenance(
                    component, binding, require_package_provenance
                )
            else:
                binding = bind_bundled_library_to_system_provenance(path)
                problem = validate_bundled_library_package_provenance(
                    component, binding, require_package_provenance
                )
            if problem is not None:
                provenance_problems.append(f"{path.relative_to(lib_dir)}: {problem}")
    if provenance_problems:
        print(
            "audit_codec_notices: bundled component/real-system "
            "package-provenance mismatch (see "
            "COMPONENT_EXPECTED_SOURCE_PACKAGES in "
            "packaging/audit_codec_notices.py):",
            file=sys.stderr,
        )
        for problem in provenance_problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    for component, paths in sorted(by_component.items()):
        for path in paths:
            print(f"{component}\t{path}")
    return 0


def cmd_verify_notices(args: argparse.Namespace) -> int:
    lib_dir: Path = args.lib_dir
    third_party_root: Path = args.third_party_root
    doc_root: Path = args.doc_root
    qt_reference_dir: Path | None = args.qt_reference_dir

    if not lib_dir.is_dir():
        print(f"Not a directory: {lib_dir}", file=sys.stderr)
        return 2
    if qt_reference_dir is not None and not qt_reference_dir.is_dir():
        print(f"Not a directory: {qt_reference_dir}", file=sys.stderr)
        return 2

    by_component, unmapped = classify_all(lib_dir, qt_reference_dir)

    missing_mandatory = MANDATORY_COMPONENTS - by_component.keys()
    if missing_mandatory:
        print(
            "audit_codec_notices: mandatory component(s) "
            f"{sorted(missing_mandatory)!r} were not found bundled under "
            f"{lib_dir}.",
            file=sys.stderr,
        )
        return 1

    if unmapped:
        print(
            f"audit_codec_notices: {len(unmapped)} bundled librar"
            f"{'y is' if len(unmapped) == 1 else 'ies are'} not mapped to any "
            "known third-party component:",
            file=sys.stderr,
        )
        for path in unmapped:
            print(f"  {path}", file=sys.stderr)
        return 1

    fail = False
    for component in sorted(by_component):
        source_dir = third_party_root / component
        if not source_dir.is_dir():
            print(
                f"audit_codec_notices: no checked-in notice source directory "
                f"at {source_dir} for bundled component '{component}'.",
                file=sys.stderr,
            )
            fail = True
            continue
        source_files = [f for f in source_dir.iterdir() if f.is_file()]
        if not source_files:
            print(
                f"audit_codec_notices: {source_dir} has no notice files for "
                f"'{component}'.",
                file=sys.stderr,
            )
            fail = True
            continue
        bundled_dir = doc_root / component
        for source_file in source_files:
            bundled_file = bundled_dir / source_file.name
            if not bundled_file.is_file() or bundled_file.stat().st_size == 0:
                print(
                    f"audit_codec_notices: bundled '{component}' is missing "
                    f"its required non-empty '{source_file.name}' at "
                    f"{bundled_file}.",
                    file=sys.stderr,
                )
                fail = True
                continue
            source_hash = _sha256(source_file)
            bundled_hash = _sha256(bundled_file)
            if source_hash != bundled_hash:
                print(
                    f"audit_codec_notices: bundled '{component}/"
                    f"{source_file.name}' (sha256 {bundled_hash}) does not "
                    f"match the checked-in source (sha256 {source_hash}) -- "
                    "notice content has drifted from what this repository "
                    "actually ships.",
                    file=sys.stderr,
                )
                fail = True

    if fail:
        return 1

    print(
        f"Every bundled component ({', '.join(sorted(by_component))}) has a "
        "checksum-verified, non-empty required license notice."
    )
    return 0


def cmd_verify_qt_sdk_lock(_args: argparse.Namespace) -> int:
    try:
        result = verify_qt_sdk_lock()
    except Exception as error:
        print(f"audit_codec_notices: failed to verify Qt SDK lock: {error}", file=sys.stderr)
        return 1
    if result["status"] != "matched":
        print(
            "audit_codec_notices: pinned Qt SDK metadata digest mismatch: "
            f"{result['updatesXmlUrl']} expected {result['expectedSha256']!r} "
            f"but got {result['actualSha256']!r}",
            file=sys.stderr,
        )
        return 1
    print(
        "audit_codec_notices: verified pinned Qt SDK metadata "
        f"{result['updatesXmlUrl']} sha256={result['actualSha256']}"
    )
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    qt_reference_dir_help = (
        "path to the real Qt SDK installation actually used to build this "
        "project (e.g. $QT_ROOT_DIR as exported by jurplel/install-qt-action "
        "in CI). Required to classify any bundled library found inside a "
        "Qt plugin subdirectory or beneath a 'qml' directory as the 'qt' "
        "component -- see classify_path()'s docstring. Omitting this means "
        "such libraries are reported unmapped (fail closed) rather than "
        "trusted by directory name alone."
    )

    classify_parser = subparsers.add_parser("classify")
    classify_parser.add_argument("lib_dir", type=Path)
    classify_parser.add_argument("--json-out", type=Path, default=None)
    classify_parser.add_argument("--qt-reference-dir", type=Path, default=None, help=qt_reference_dir_help)
    classify_parser.add_argument(
        "--require-package-provenance",
        action="store_true",
        help=(
            "Round-7 review (\"missing provenance passes\" / \"governed "
            "expectations\"): fail closed if an expected distro "
            "component's real dpkg-owned system counterpart cannot be "
            "found or is not dpkg-owned at all (in addition to the "
            "always-enforced content-mismatch check), instead of "
            "silently skipping it. Intended for a real, dpkg-equipped "
            "host where every expected distro library's system "
            "counterpart is genuinely guaranteed to be installed (this "
            "project's own pinned ubuntu-22.04 appimage-smoke CI job in "
            "particular); never pass this on a partial/incomplete host. "
            "Requires --distro-provenance-manifest (see its own help) -- "
            "an after-the-fact basename search is never accepted as "
            "authoritative provenance."
        ),
    )
    classify_parser.add_argument(
        "--distro-provenance-manifest",
        type=Path,
        default=None,
        help=(
            "Round-N+ review (HIGH, \"distro provenance post-hoc/"
            "unpinned ... capture exact loader/copy source BEFORE "
            "packaging ... No basename re-discovery\"): path to the JSON "
            "manifest produced by the capture-distro-provenance "
            "subcommand BEFORE packaging (see packaging/build-"
            "appimage.sh's own \"Capture distro provenance\" step). When "
            "supplied, every distro component's package provenance is "
            "looked up directly in this manifest (bind_bundled_library_"
            "to_captured_provenance()) instead of an independent, "
            "after-the-fact basename search across fixed system "
            "directories."
        ),
    )
    classify_parser.add_argument(
        "--qt-sdk-version",
        type=str,
        default=None,
        help=(
            "Round-N+ review (\"include exact Qt SDK release/archive/"
            "checksum\"): this project's own pinned Qt release (e.g. "
            "the exact $QT_VERSION jurplel/install-qt-action installed "
            "-- see .github/workflows/ci.yml), recorded verbatim as "
            "\"sdkVersion\" in every qt/icu inventory entry's "
            "qtSdkProvenance -- see "
            "bind_bundled_library_to_qt_sdk_provenance()'s own docstring "
            "for why this is the most precise upstream-SDK-release "
            "identity this mechanism can honestly claim."
        ),
    )
    classify_parser.set_defaults(func=cmd_classify)

    capture_provenance_parser = subparsers.add_parser(
        "capture-distro-provenance",
        help=(
            "Captures real distro-package provenance (path/sha256/"
            "package/version/sourcePackage) for every dynamically "
            "resolved dependency of one or more pre-packaging, "
            "first-party ELF files (this project's own compiled "
            "executable and/or Qt plugins BEFORE linuxdeploy copies/"
            "patches anything), using the real dynamic loader's own "
            "resolution (via `ldd`) -- never an independent post-hoc "
            "directory search. Must be run BEFORE packaging; see "
            "packaging/build-appimage.sh's own \"Capture distro "
            "provenance\" step."
        ),
    )
    capture_provenance_parser.add_argument(
        "elf_paths",
        type=Path,
        nargs="+",
        help="One or more pre-packaging, first-party ELF executables/plugins to resolve dependencies from.",
    )
    capture_provenance_parser.add_argument("--output", type=Path, required=True)
    capture_provenance_parser.add_argument(
        "--staging-dir",
        type=Path,
        default=None,
        help=(
            "Directory where immutable same-open-descriptor staged copies of "
            "captured distro source files are stored. Defaults to a sibling "
            ".distro-provenance-stage directory next to --output."
        ),
    )
    capture_provenance_parser.set_defaults(func=cmd_capture_distro_provenance)

    verify_qt_sdk_lock_parser = subparsers.add_parser(
        "verify-qt-sdk-lock",
        help=(
            "Fetches the checked-in pinned Qt Updates.xml metadata URL and "
            "verifies its sha256 against packaging/qt_sdk_lock.json. Intended "
            "for CI use before install-qt-action."
        ),
    )
    verify_qt_sdk_lock_parser.set_defaults(func=cmd_verify_qt_sdk_lock)

    verify_parser = subparsers.add_parser("verify-notices")
    verify_parser.add_argument("lib_dir", type=Path)
    verify_parser.add_argument("third_party_root", type=Path)
    verify_parser.add_argument("doc_root", type=Path)
    verify_parser.add_argument("--qt-reference-dir", type=Path, default=None, help=qt_reference_dir_help)
    verify_parser.set_defaults(func=cmd_verify_notices)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
