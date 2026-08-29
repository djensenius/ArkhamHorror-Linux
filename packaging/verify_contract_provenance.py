#!/usr/bin/env python3
"""Prove every vendored contracts/ file this client's decoders are bound to
is byte-identical (and mode/type-identical) to the same path in the
*actual backend git history*, at the exact commit ContractPin.cpp pins to
-- not merely internally consistent with a SHA-256 recorded beside it, and
not merely present under some path that could be a symlink, a renamed
substitute, or something added and never actually vendored from upstream.

ContractDriftTests (see tests/ContractDriftTests.cpp) already re-hashes
every vendored file and compares it against the digest table in
src/ContractPin.cpp. That is real and useful (it catches accidental drift:
edit-without-re-hash), but it has two structural blind spots this script
closes:

  1. The digest table lives in the same repository, editable in the same
     commit, as the bytes it is meant to police, AND it is also the thing
     the previous version of this very script used to *discover which
     files to check* -- so a change that edited a vendored fixture, its
     digest, and (if this script trusted that table for enumeration too)
     the governed-file list, all together, could still pass every local
     check while having silently diverged from what the backend actually
     published. This script never reads which-files-to-check out of any
     local, mutable file: the governed path *set* is derived from (a) a
     short, explicit, human-reviewed ROOT_SCHEMAS list of which backend
     schemas this client's C++ types actually decode against (see the
     model-coverage section of the PR description), and (b) a recursive
     `$ref` (schema-to-schema) and fixture-to-schema (via manifest.json)
     dependency closure computed ENTIRELY by reading blobs out of the
     pinned backend commit's own git tree -- never a local file. A local
     schema edit that added, removed, or rewrote a `$ref` therefore cannot
     hide or fabricate a dependency from this script's point of view.

  2. A byte-for-byte match is not the whole story: a vendored path could
     be a symlink to some other file that happens to currently contain
     the right bytes (and would silently start pointing elsewhere on a
     future, unrelated commit), or could exist locally with an unexpected
     git blob mode (e.g. suddenly executable). This script rejects both:
     it checks the local path is a plain regular file (never a symlink,
     via Path.is_symlink()) and cross-checks the *backend's own* git blob
     mode/type for that path (via `git ls-tree`) is the plain, non-
     executable "100644 blob" every one of these JSON contract files is
     expected to be.

It also detects the opposite failure mode: an *extra*, ungoverned file
sitting in contracts/schemas/ or contracts/fixtures/ that this script's
computed governed set does not know about -- e.g. a new schema/fixture
added without registering it in ROOT_SCHEMAS/being reachable via the
`$ref`/manifest closure, which would otherwise let unverified content
accumulate silently beside the files that are actually checked.

This script fetches the exact pinned backend commit directly from the
backend's own git remote (djensenius/ArkhamHorror on GitHub, by default --
never a local developer checkout path, so this proof is reproducible
identically in CI and does not depend on whatever happens to be checked
out on this machine) and reads every governed file, and every fact used to
derive the governed set, from real git blobs at that commit. It never
executes anything fetched from the backend as code -- only ever parses it
as JSON/text.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import posixpath
import re
import stat
import subprocess
import sys
from pathlib import Path
from typing import Iterator, Sequence

DEFAULT_REMOTE = "https://github.com/djensenius/ArkhamHorror.git"

# Explicit, human-reviewed root list of backend contract schemas this
# client's C++ models are pinned to (catalog, decks, game-lifecycle,
# game-list, game-state -- see CardCatalog.h/Decks.h/Games.h). This is the
# ONE place a human reviewer must keep in sync with "what this client
# actually implements decoders for"; every other governed path below is
# discovered automatically from this root by walking the pinned BACKEND's
# own git tree, never a local file.
ROOT_SCHEMAS: tuple[str, ...] = (
    "contracts/schemas/catalog.schema.json",
    "contracts/schemas/decks.schema.json",
    "contracts/schemas/game-lifecycle.schema.json",
    "contracts/schemas/game-list.schema.json",
    "contracts/schemas/game-state.schema.json",
)

# contracts/manifest.json itself is a governed document (cross-checked for
# schemaRevision, and read -- from the backend, never locally -- to
# resolve which fixture maps to which schema below).
ROOT_DOCUMENTS: tuple[str, ...] = ("contracts/manifest.json",)

# fixtures/capabilities.json is consumed directly by this client
# (ServerCapabilities.cpp) without being schema-validated field-for-field;
# capabilities.schema.json itself is deliberately NOT in ROOT_SCHEMAS since
# no C++ type decodes strictly against it. It is still vendored/governed
# because contract-pin.json/schemaRevision consistency is cross-checked
# against it, so it is listed explicitly here rather than silently
# excluded.
ROOT_EXTRA_FIXTURES: tuple[str, ...] = ("contracts/fixtures/capabilities.json",)

# Directories scanned for *extra*, ungoverned files (see module docstring,
# blind spot 2's counterpart: files present locally that the computed
# governed set does not know about at all).
_SCANNED_DIRS: tuple[str, ...] = ("contracts/schemas", "contracts/fixtures")

_EXPECTED_BLOB_MODE = "100644"


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


class GitTree:
    """Reads file bytes and git blob mode/type out of a single pinned
    commit exclusively -- never a local working-tree file -- so both
    closure discovery (which files exist / what they `$ref`) and the
    final byte/mode comparison are rooted in the same untamperable source.
    """

    def blob_bytes(self, path: str) -> bytes:  # pragma: no cover - interface
        raise NotImplementedError

    def ls_tree(self, path: str) -> tuple[str, str] | None:  # pragma: no cover
        """Returns (mode, type) for `path` at this tree, e.g.
        ("100644", "blob"), or None if the path does not exist at all."""
        raise NotImplementedError


class RemoteGitTree(GitTree):
    """A GitTree backed by a real, shallow, single-commit fetch of the
    pinned backend commit from its real git remote."""

    def __init__(self, cache_dir: Path, remote: str, commit: str) -> None:
        self._cache_dir = cache_dir
        self._commit = commit
        cache_dir.mkdir(parents=True, exist_ok=True)
        if not (cache_dir / ".git").exists():
            _run(["git", "init", "-q", str(cache_dir)])
        # A shallow, single-commit fetch by exact SHA: the smallest amount
        # of backend history that still lets us read the real git tree at
        # the pinned commit, against GitHub's public smart-HTTP remote,
        # without ever cloning the whole backend repository and without
        # ever touching a local backend checkout path.
        _run(["git", "-C", str(cache_dir), "fetch", "--depth", "1", remote, commit])

    def blob_bytes(self, path: str) -> bytes:
        return _run(
            [
                "git",
                "-C",
                str(self._cache_dir),
                "cat-file",
                "-p",
                f"{self._commit}:{path}",
            ]
        ).stdout

    def ls_tree(self, path: str) -> tuple[str, str] | None:
        result = subprocess.run(
            ["git", "-C", str(self._cache_dir), "ls-tree", self._commit, "--", path],
            check=True,
            capture_output=True,
        )
        line = result.stdout.decode().strip()
        if not line:
            return None
        # Format: "<mode> <type> <sha>\t<path>"
        mode, obj_type, _rest = line.split(None, 2)
        return mode, obj_type


class RefEscapeError(RuntimeError):
    """A `$ref` (or manifest fixture/schema path) tried to name a path
    outside contracts/schemas/ -- via '..' traversal, an absolute path, or
    similar. Always treated as a hard failure, never silently resolved."""


def _resolve_schema_ref(schema_path: str, ref: str) -> str | None:
    """Resolves a JSON Schema `$ref` value to a contracts/-relative path,
    or None if it is a same-document fragment (`#/$defs/...`) naming no
    separate file. Raises RefEscapeError if the target would resolve
    outside contracts/schemas/ -- the only directory a schema-to-schema
    $ref is ever allowed to reach into."""
    file_part = ref.split("#", 1)[0]
    if not file_part:
        return None
    if file_part.startswith("/") or file_part.startswith("\\"):
        raise RefEscapeError(
            f"{schema_path}: $ref {ref!r} is an absolute path, not permitted"
        )
    resolved = (Path(schema_path).parent / file_part).as_posix()
    # Collapse any "./" segments explicitly via posixpath.normpath rather
    # than relying on Path's "/" join operator to have already done so:
    # this makes the "./nested.schema.json"-style refs actually present in
    # the real backend schemas (e.g. game-list.schema.json's
    # "$ref": "./game-state.schema.json") resolve deterministically
    # without depending on pathlib join internals. The ".." check just
    # below inspects the *raw* (pre-normpath) file_part's path parts
    # directly, so it independently catches any traversal attempt
    # regardless of its ordering relative to this normpath() call --
    # normpath() itself is only ever used to collapse a redundant "./"
    # for the containment check further down, and is never relied upon
    # to normalize away an attempted directory-traversal escape.
    normalized = posixpath.normpath(resolved)
    parts = Path(file_part).parts
    if ".." in parts:
        raise RefEscapeError(
            f"{schema_path}: $ref {ref!r} traverses outside its directory"
        )
    if not normalized.startswith("contracts/schemas/"):
        raise RefEscapeError(
            f"{schema_path}: $ref {ref!r} resolves to {normalized!r}, "
            "outside contracts/schemas/"
        )
    return normalized


def _iter_ref_values(node: object) -> Iterator[str]:
    """Recursively walks a *parsed* JSON Schema document and yields every
    string value bound to an actual `"$ref"` object key at any nesting
    depth. Unlike a text-level regex scan (which would match the four
    characters `"$ref"` anywhere a schema author happened to write them --
    for instance inside a `"description"` or `"examples"` string literal
    describing `$ref` itself), this only ever considers a real JSON object
    key, so it cannot produce a false-positive dependency from prose that
    merely mentions the syntax."""
    if isinstance(node, dict):
        for key, value in node.items():
            if key == "$ref" and isinstance(value, str):
                yield value
            else:
                yield from _iter_ref_values(value)
    elif isinstance(node, list):
        for item in node:
            yield from _iter_ref_values(item)


def compute_schema_closure(
    tree: GitTree, roots: Sequence[str]
) -> dict[str, bytes]:
    """Recursively discovers every schema file transitively `$ref`'d from
    `roots`, fetching each one's bytes from `tree` (the pinned BACKEND,
    never local disk) as it goes. Returns {contracts-relative path: bytes}.
    Raises if a referenced schema does not exist in the backend tree at
    all (an "omitted referenced schema" -- one this client's root list
    reaches via $ref but the backend never actually published, or one a
    ROOT_SCHEMAS entry itself misnamed)."""
    discovered: dict[str, bytes] = {}
    pending = list(roots)
    while pending:
        path = pending.pop()
        if path in discovered:
            continue
        info = tree.ls_tree(path)
        if info is None:
            raise RuntimeError(
                f"{path}: referenced schema does not exist in the pinned "
                "backend commit at all -- cannot vendor a file the "
                "backend never published at this SHA"
            )
        data = tree.blob_bytes(path)
        discovered[path] = data
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError as exc:
            # errors="replace" would silently substitute U+FFFD for
            # invalid bytes and keep scanning, which could mask corrupted
            # or non-UTF-8 schema bytes and compute an incorrect (missing
            # or corrupted) $ref closure -- weakening the exact provenance
            # guarantee this script exists to provide. Fail fast instead;
            # verify()'s caller already turns a RuntimeError here into a
            # clean failure description rather than an unhandled
            # traceback.
            raise RuntimeError(
                f"{path}: schema is not valid UTF-8 ({exc}); refusing to "
                "scan it for $ref"
            ) from exc
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError as exc:
            # A text-level regex scan for `"$ref"` would still "work" on
            # unparsable JSON, but could then just as easily match that
            # four-character sequence inside a string value the author
            # never intended as a real reference (a description or
            # example mentioning `$ref` syntax) -- or, on malformed JSON,
            # match a `$ref` key that isn't even reachable at runtime.
            # Parsing first and walking the real object graph via
            # _iter_ref_values() below closes both gaps at once; a schema
            # that isn't valid JSON at all is itself a hard failure, since
            # this client cannot decode against it either.
            raise RuntimeError(f"{path}: schema is not valid JSON ({exc})") from exc
        for ref in _iter_ref_values(parsed):
            target = _resolve_schema_ref(path, ref)
            if target is not None and target not in discovered:
                pending.append(target)
    return discovered


def _resolve_manifest_path(path: str) -> str:
    """Validates a contracts/manifest.json-declared path (schema or
    fixture) stays within contracts/ with no traversal, mirroring
    _resolve_schema_ref's protections for manifest-declared paths."""
    if path.startswith("/") or path.startswith("\\"):
        raise RefEscapeError(f"manifest.json: path {path!r} is absolute")
    if ".." in Path(path).parts:
        raise RefEscapeError(f"manifest.json: path {path!r} traverses directories")
    normalized = Path(path).as_posix()
    if not normalized.startswith("contracts/"):
        raise RefEscapeError(
            f"manifest.json: path {path!r} resolves outside contracts/"
        )
    return normalized


def compute_governed_fixtures(tree: GitTree, schema_paths: set[str]) -> list[str]:
    """Reads contracts/manifest.json from the pinned BACKEND tree (never
    local) and returns every fixture path whose declared schema is in
    `schema_paths` -- i.e. every fixture this client's modeled schemas
    actually govern, discovered independently of anything checked into
    this repository."""
    manifest_path = "contracts/manifest.json"
    raw = tree.blob_bytes(manifest_path)
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        # Mirrors compute_schema_closure()'s strict decode: a corrupted or
        # unexpectedly non-UTF-8 manifest blob at the pinned backend
        # commit must fail deterministically here rather than raising an
        # unhandled traceback past verify()'s RuntimeError/RefEscapeError
        # handling.
        raise RuntimeError(
            f"{manifest_path}: manifest is not valid UTF-8 ({exc})"
        ) from exc
    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"{manifest_path}: manifest is not valid JSON ({exc})"
        ) from exc
    result = []
    for entry in manifest.get("fixtures", []):
        schema = _resolve_manifest_path(entry["schema"])
        path = _resolve_manifest_path(entry["path"])
        if schema in schema_paths:
            result.append(path)
    return result


def compute_governed_paths(tree: GitTree) -> set[str]:
    """The single source of truth for "every contracts/ path this client
    is bound to and must byte/mode-match the pinned backend on" -- built
    entirely from ROOT_SCHEMAS/ROOT_DOCUMENTS/ROOT_EXTRA_FIXTURES plus the
    backend-tree-derived closures above. Never consults any local,
    mutable file (e.g. src/ContractPin.cpp) to decide *which* files to
    check -- only used, elsewhere, to cross-check the digests recorded
    there once the set and bytes are already independently known."""
    schema_closure = compute_schema_closure(tree, ROOT_SCHEMAS)
    fixtures = compute_governed_fixtures(tree, set(schema_closure))
    return (
        set(schema_closure)
        | set(ROOT_DOCUMENTS)
        | set(ROOT_EXTRA_FIXTURES)
        | set(fixtures)
    )


def find_local_extra_files(repo_root: Path, governed: set[str]) -> list[str]:
    """Scans _SCANNED_DIRS for files not present in `governed` -- an
    ungoverned schema/fixture added locally without being reachable from
    ROOT_SCHEMAS's $ref/manifest closure."""
    extras: list[str] = []
    for scanned_dir in _SCANNED_DIRS:
        directory = repo_root / scanned_dir
        if not directory.is_dir():
            continue
        for child in sorted(directory.rglob("*")):
            if child.is_dir():
                continue
            relative = child.relative_to(repo_root).as_posix()
            if relative not in governed:
                extras.append(relative)
    return extras


def verify(
    tree: GitTree, repo_root: Path, governed_digests: dict[str, str] | None = None
) -> list[str]:
    """Runs the full provenance check against `tree` (the pinned backend)
    and `repo_root` (the local working tree), returning a list of
    human-readable failure descriptions (empty if everything checks out).
    `governed_digests` is an optional {contracts-relative path: sha256} map
    (e.g. cross-checked against src/ContractPin.cpp) used only as a
    secondary, offline-friendly belt-and-suspenders check -- never as the
    source of which paths to verify."""
    failures: list[str] = []

    try:
        governed = compute_governed_paths(tree)
    except (RuntimeError, RefEscapeError) as exc:
        return [str(exc)]

    for relative_path in sorted(governed):
        info = tree.ls_tree(relative_path)
        if info is None:
            failures.append(
                f"{relative_path}: backend tree has no such path (should be "
                "unreachable -- closure discovery already checked this)"
            )
            continue
        mode, obj_type = info
        if obj_type != "blob" or mode != _EXPECTED_BLOB_MODE:
            failures.append(
                f"{relative_path}: backend git object is mode={mode} "
                f"type={obj_type}, expected a plain non-executable blob "
                f"({_EXPECTED_BLOB_MODE})"
            )
            continue

        backend_bytes = tree.blob_bytes(relative_path)
        local_path = repo_root / relative_path

        if not local_path.exists() and not local_path.is_symlink():
            failures.append(f"{relative_path}: vendored file is missing")
            continue
        if local_path.is_symlink():
            failures.append(
                f"{relative_path}: vendored path is a symlink, not a plain "
                "regular file -- rejected even if its target's bytes "
                "currently match"
            )
            continue
        local_stat = local_path.lstat()
        if not stat.S_ISREG(local_stat.st_mode):
            failures.append(
                f"{relative_path}: vendored path is not a regular file "
                f"(st_mode={oct(local_stat.st_mode)})"
            )
            continue
        # _EXPECTED_BLOB_MODE ("100644") is a plain, non-executable blob;
        # the backend mode/type check above only inspects the pinned git
        # tree, so a locally-chmod'd +x contracts/*.json would otherwise
        # slip through byte-identical even though it no longer matches the
        # "mode/type-identical to the backend" guarantee this script
        # advertises. Reject any of the local owner/group/other execute
        # bits explicitly.
        if local_stat.st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH):
            failures.append(
                f"{relative_path}: vendored file is locally executable "
                f"(st_mode={oct(stat.S_IMODE(local_stat.st_mode))}), but the "
                f"pinned backend blob is a non-executable "
                f"{_EXPECTED_BLOB_MODE} file"
            )
            continue

        local_bytes = local_path.read_bytes()
        if local_bytes != backend_bytes:
            failures.append(
                f"{relative_path}: vendored bytes do NOT match the real "
                f"backend blob (backend sha256="
                f"{hashlib.sha256(backend_bytes).hexdigest()}, vendored "
                f"sha256={hashlib.sha256(local_bytes).hexdigest()})"
            )
            continue

        if governed_digests is not None and relative_path in governed_digests:
            recomputed = hashlib.sha256(backend_bytes).hexdigest()
            recorded = governed_digests[relative_path]
            if recomputed != recorded:
                failures.append(
                    f"{relative_path}: byte-identical to the pinned backend "
                    f"blob, but src/ContractPin.cpp's recorded digest "
                    f"{recorded} does not match the recomputed digest "
                    f"{recomputed} -- ContractPin.cpp is stale"
                )

    for extra in find_local_extra_files(repo_root, governed):
        failures.append(
            f"{extra}: present locally under a governed directory but is "
            "not reachable from ROOT_SCHEMAS's $ref/manifest closure -- "
            "either register it (add to ROOT_SCHEMAS/ROOT_EXTRA_FIXTURES, "
            "or ensure a governed schema $ref's it) or remove it"
        )

    return failures


# ---------------------------------------------------------------------------
# Optional, secondary cross-check against src/ContractPin.cpp's own digest
# table. This NEVER decides which paths are governed (see verify()'s
# docstring) -- it only flags a stale/forgotten digest update once the
# governed set and correct bytes are already known independently.
# ---------------------------------------------------------------------------

_GOVERNED_DIGEST_ENTRY_RE = re.compile(
    r'\{QStringLiteral\("([^"]+)"\),\s*QStringLiteral\(\s*((?:"[^"]*"\s*)+)\)\}',
    re.MULTILINE,
)


def _adjacent_literal_concat(raw: str) -> str:
    """Joins the adjacent C++ string-literal pieces captured by
    _GOVERNED_DIGEST_ENTRY_RE's second group (ContractPin.cpp wraps each
    64-char hex digest across two lines as two adjacent literals, which
    the C++ preprocessor concatenates -- this replicates that)."""
    return "".join(piece[1:-1] for piece in re.findall(r'"[^"]*"', raw))


def read_contract_pin_digests(repo_root: Path) -> dict[str, str]:
    text = (repo_root / "src" / "ContractPin.cpp").read_text(encoding="utf-8")
    return {
        f"contracts/{path}": _adjacent_literal_concat(digest_literal)
        for path, digest_literal in _GOVERNED_DIGEST_ENTRY_RE.findall(text)
    }


def read_pinned_commit(repo_root: Path) -> str:
    pin = json.loads(
        (repo_root / "contracts" / "contract-pin.json").read_text(encoding="utf-8")
    )
    commit = pin["backendCommit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise RuntimeError(
            f"contracts/contract-pin.json's backendCommit {commit!r} is not "
            "a 40-character lowercase hex git commit SHA"
        )
    return commit


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--remote",
        default=DEFAULT_REMOTE,
        help=(
            "Backend git remote to fetch the pinned commit from "
            f"(default: {DEFAULT_REMOTE}). Intentionally never defaults to "
            "a local filesystem path, so this check proves the same thing "
            "in CI as it does anywhere else."
        ),
    )
    parser.add_argument(
        "--cache-dir",
        default=None,
        help=(
            "Directory to fetch the pinned backend commit into (default: "
            "<repo>/build/contract-provenance-cache). Reused across runs "
            "to avoid re-fetching identical bytes every invocation."
        ),
    )
    args = parser.parse_args()

    repo_root = _repo_root()
    commit = read_pinned_commit(repo_root)
    cache_dir = (
        Path(args.cache_dir)
        if args.cache_dir
        else repo_root / "build" / "contract-provenance-cache"
    )

    print(f"Fetching pinned backend commit {commit} from {args.remote} ...")
    try:
        tree = RemoteGitTree(cache_dir, args.remote, commit)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            "error: could not fetch the pinned backend commit "
            f"{commit} from {args.remote}:\n"
            f"{exc.stderr.decode(errors='replace')}\n"
        )
        return 2

    try:
        governed_digests = read_contract_pin_digests(repo_root)
    except FileNotFoundError:
        governed_digests = None

    failures = verify(tree, repo_root, governed_digests)

    if failures:
        sys.stderr.write(
            f"\ncontract provenance check FAILED with {len(failures)} "
            "problem(s):\n"
        )
        for failure in failures:
            sys.stderr.write(f"  - {failure}\n")
        return 1

    governed = compute_governed_paths(tree)
    print(
        f"\nAll {len(governed)} governed contracts/ file(s) are byte- and "
        f"mode-identical to backend commit {commit}, with no unregistered "
        "extra files present."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
