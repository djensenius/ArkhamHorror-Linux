#!/usr/bin/env python3
"""Round-4 review item 11 (PR #18 cumulative review): independently
verifies that every pinned source file in
contracts/asset-locale-digest-sources/ is byte-for-byte, mode-for-mode
identical to the real file the manifest (contracts/asset-locale-digest.json)
claims it was copied from, at the exact commit the manifest pins -- rather
than trusting the manifest's own self-reported sha256/sourceCommit fields,
which describe nothing but the manifest's and source file's mutual
self-consistency (an editor who changes a source file AND its accompanying
hash AND the sourceCommit label together leaves that self-consistency
check with nothing to catch).

This queries the GitHub Git Data API's recursive tree listing for the
pinned commit of the AUTHORITATIVE upstream repository -- whose identity
is a FIXED CONSTANT in this script (_EXPECTED_SOURCE_REPOSITORY), never
read from the manifest and trusted -- so a manifest cannot redirect
verification to an attacker-controlled fork by editing its own
provenance.sourceRepository field to name one. If the manifest's
sourceRepository does not match this fixed constant, verification fails
immediately without ever making a network request.

Round-6/7 review item 10: `sourceCommit` must be an exact, immutable
40-character lowercase hex SHA-1 object id -- never a mutable ref (a
branch/tag name, or an abbreviated/short SHA), which the GitHub API would
otherwise happily resolve to whatever commit currently sits there at
fetch time, silently verifying against a moving target rather than the
specific immutable commit this provenance record claims to attest to.
This is checked locally, before any network request, exactly like the
sourceRepository check above. As a second, belt-and-braces guard, this
also fetches the single-commit-object endpoint
(/repos/.../git/commits/<sha>) and requires its own `sha` field to equal
the requested identifier verbatim -- that endpoint can only ever resolve
an EXACT object id to itself or 404 (unlike the ref-resolving contents/
refs APIs), so this additionally protects against any future API
behavior change that might otherwise let a non-exact identifier slip
past the local regex check with a "helpful" fuzzy resolution.

For each of the five pinned locales, this:
  1. Locates the exact tree entry for frontend/src/digests/<web-locale>.json
     within the recursive tree listing at the pinned commit.
  2. Requires that entry's `mode` to be exactly "100644" (a regular,
     non-executable, non-symlink, non-submodule file) -- any other mode
     (e.g. "120000" for a symlink, "160000" for a submodule pointing
     somewhere else entirely, "100755" for an executable) is rejected.
  3. Requires that entry's `path` to be exactly the expected path (the
     tree listing is keyed by path already, but this re-confirms the
     match was not found via any fuzzy/prefix logic).
  4. Independently computes the git blob SHA-1 of the CHECKED-IN pinned
     source file's bytes (using git's own "blob <len>\\0<content>"
     hashing scheme) and requires it to equal the tree entry's own `sha`
     -- this is exactly what `git cat-file -p <sha>` would content-address,
     so an identical blob hash is git's own proof of byte-for-byte content
     equality, without this script needing to separately fetch and
     compare the full blob content over the network.

Any network failure, missing commit, missing blob, wrong mode, or hash
mismatch is a hard failure (non-zero exit) -- there is no fallback path
that can succeed without a genuine, verified network round trip against
the real upstream repository.

Usage:
    tools/verify_asset_locale_digest_provenance.py

Requires network access to https://api.github.com. Set GITHUB_TOKEN (or
GH_TOKEN) in the environment to authenticate the request and avoid the
much lower unauthenticated GitHub API rate limit -- CI already exposes
GITHUB_TOKEN by default for this purpose.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Callable

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTRACTS_DIR = REPO_ROOT / "contracts"
MANIFEST_JSON = CONTRACTS_DIR / "asset-locale-digest.json"
SOURCES_DIR = CONTRACTS_DIR / "asset-locale-digest-sources"

# Round-4 review item 11: fixed, hardcoded expected upstream identity --
# deliberately NEVER read from the manifest under verification, so the
# manifest itself cannot redirect this check to a different (potentially
# attacker-controlled) repository by editing its own
# provenance.sourceRepository field.
_EXPECTED_SOURCE_REPOSITORY = "https://github.com/halogenandtoast/ArkhamHorror"
_EXPECTED_OWNER = "halogenandtoast"
_EXPECTED_REPO = "ArkhamHorror"

# The real web client's per-locale digest files this project's pinned
# sources are copies of (see contracts/asset-locale-digest.json's
# provenance.sourceFileNote).
_UPSTREAM_DIGEST_PATH_TEMPLATE = "frontend/src/digests/{web_locale}.json"

_EXPECTED_REGULAR_FILE_MODE = "100644"

# Round-6/7 review item 10: sourceCommit must be an exact, immutable
# 40-character lowercase hex SHA-1 object id -- NEVER a mutable ref such
# as a branch name ("main"), tag, or abbreviated/short SHA. A mutable ref
# resolves to whatever commit currently sits at that ref at fetch time
# (a moving target an attacker who can push to the upstream repository,
# or simply time itself via an ordinary future upstream commit, could
# shift out from under this "pin"), which defeats the entire point of
# pinning a specific, immutable commit for provenance verification.
_FULL_LOWERCASE_SHA1_RE = re.compile(r"^[0-9a-f]{40}$")


class ProvenanceVerificationError(RuntimeError):
    """Raised for any provenance-verification failure -- network, missing
    commit/blob, wrong mode, or content-hash mismatch. There is
    deliberately no code path that swallows this and reports success."""


def _git_blob_sha1(data: bytes) -> str:
    """Computes the exact SHA-1 git itself would assign to `data` as a
    blob object -- "blob <len>\\0<content>", matching `git hash-object`
    -- so a match against a Git Data API tree entry's `sha` field is
    genuine proof of byte-for-byte content equality with whatever git
    object that commit's tree actually points at."""
    header = f"blob {len(data)}\0".encode("utf-8")
    return hashlib.sha1(header + data, usedforsecurity=False).hexdigest()


def _default_fetch_json(url: str, token: str | None) -> dict:
    """Performs a real HTTPS GET against the GitHub API and returns the
    parsed JSON body. Raises ProvenanceVerificationError on ANY failure
    (network error, non-2xx status, invalid JSON) -- there is no
    fallback that can turn a failed fetch into a successful
    verification."""
    request = urllib.request.Request(
        url,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "ArkhamHorror-Linux-asset-locale-digest-provenance-check",
            **({"Authorization": f"Bearer {token}"} if token else {}),
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read()
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise ProvenanceVerificationError(
            f"network request to {url!r} failed: {exc}"
        ) from exc
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise ProvenanceVerificationError(
            f"response from {url!r} was not valid JSON: {exc}"
        ) from exc


def verify(
    manifest_path: Path = MANIFEST_JSON,
    sources_dir: Path = SOURCES_DIR,
    fetch_json: Callable[[str, str | None], dict] = _default_fetch_json,
    token: str | None = None,
) -> None:
    """Runs the full provenance verification described in this module's
    doc comment. Raises ProvenanceVerificationError on any failure;
    returns normally only when every pinned locale's source file has been
    positively, independently confirmed against the real upstream commit.
    """
    try:
        manifest_bytes = manifest_path.read_bytes()
    except OSError as exc:
        raise ProvenanceVerificationError(
            f"could not read manifest {manifest_path!r}: {exc}"
        ) from exc
    try:
        manifest = json.loads(manifest_bytes)
    except json.JSONDecodeError as exc:
        raise ProvenanceVerificationError(
            f"manifest {manifest_path!r} is not valid JSON: {exc}"
        ) from exc
    if not isinstance(manifest, dict):
        raise ProvenanceVerificationError(
            f"manifest {manifest_path!r} top-level JSON value must be an "
            f"object, got {type(manifest).__name__}"
        )
    provenance = manifest.get("provenance")
    if not isinstance(provenance, dict):
        raise ProvenanceVerificationError(
            f"manifest {manifest_path!r} is missing a 'provenance' object "
            f"(got {type(provenance).__name__ if provenance is not None else 'nothing'})"
        )

    declared_repository = provenance.get("sourceRepository")
    if declared_repository != _EXPECTED_SOURCE_REPOSITORY:
        # Deliberately fails BEFORE any network request: a manifest that
        # has already renamed its own claimed authority cannot be trusted
        # to tell this script where to go looking for "confirmation".
        raise ProvenanceVerificationError(
            f"manifest provenance.sourceRepository is {declared_repository!r}, "
            f"but this script only ever verifies against the fixed, "
            f"hardcoded authority {_EXPECTED_SOURCE_REPOSITORY!r} -- "
            "refusing to verify against a different repository identity "
            "the manifest itself named"
        )

    source_commit = provenance.get("sourceCommit")
    if not isinstance(source_commit, str) or not source_commit:
        raise ProvenanceVerificationError(
            "manifest provenance.sourceCommit is missing or not a string"
        )
    if not _FULL_LOWERCASE_SHA1_RE.fullmatch(source_commit):
        # Deliberately fails BEFORE any network request, same as the
        # sourceRepository check above: a mutable ref name (a branch,
        # tag, or abbreviated SHA) is never an acceptable "pin", since
        # the GitHub API would happily resolve it to whatever commit
        # currently sits there -- silently verifying against a moving
        # target rather than the specific immutable commit this
        # provenance record claims to attest to.
        raise ProvenanceVerificationError(
            f"manifest provenance.sourceCommit is {source_commit!r}, but "
            "must be an exact, immutable 40-character lowercase hex "
            "SHA-1 commit id -- a mutable ref (branch/tag name) or "
            "abbreviated SHA is never an acceptable pin"
        )

    commit_url = (
        f"https://api.github.com/repos/{_EXPECTED_OWNER}/{_EXPECTED_REPO}"
        f"/git/commits/{source_commit}"
    )
    commit_response = fetch_json(commit_url, token)
    resolved_commit_sha = commit_response.get("sha")
    if resolved_commit_sha != source_commit:
        # Belt-and-braces: the Git Data API's single-commit-object
        # endpoint (unlike the ref-resolving contents/refs APIs) can only
        # ever resolve an EXACT object id to itself or 404 -- so this
        # additionally guards against any future API behavior change that
        # might otherwise let a non-exact identifier slip through the
        # regex check above with a "helpful" fuzzy resolution.
        raise ProvenanceVerificationError(
            f"commit lookup for {source_commit!r} returned a different "
            f"sha {resolved_commit_sha!r} -- refusing to trust a "
            "non-exact commit resolution"
        )
    commit_tree = commit_response.get("tree")
    if not isinstance(commit_tree, dict) or not isinstance(
        commit_tree.get("sha"), str
    ):
        raise ProvenanceVerificationError(
            f"commit {source_commit!r} response had no usable tree sha -- "
            f"got keys {sorted(commit_response.keys())!r}"
        )

    tree_url = (
        f"https://api.github.com/repos/{_EXPECTED_OWNER}/{_EXPECTED_REPO}"
        f"/git/trees/{source_commit}?recursive=1"
    )
    tree_response = fetch_json(tree_url, token)
    if tree_response.get("truncated"):
        raise ProvenanceVerificationError(
            f"tree listing for commit {source_commit!r} was truncated by "
            "the GitHub API -- cannot exhaustively verify every entry"
        )
    tree_entries = tree_response.get("tree")
    if not isinstance(tree_entries, list):
        raise ProvenanceVerificationError(
            f"tree listing response for commit {source_commit!r} had no "
            "usable 'tree' array -- got keys "
            f"{sorted(tree_response.keys())!r}"
        )
    entries_by_path = {
        entry.get("path"): entry
        for entry in tree_entries
        if isinstance(entry, dict)
    }

    source_files = provenance["sourceFiles"]
    for web_locale, entry in sorted(source_files.items()):
        expected_upstream_path = _UPSTREAM_DIGEST_PATH_TEMPLATE.format(
            web_locale=web_locale
        )
        tree_entry = entries_by_path.get(expected_upstream_path)
        if tree_entry is None:
            raise ProvenanceVerificationError(
                f"commit {source_commit!r} of {_EXPECTED_SOURCE_REPOSITORY} "
                f"has no blob at {expected_upstream_path!r} -- the pinned "
                f"source for locale {web_locale!r} cannot be verified"
            )
        if tree_entry.get("path") != expected_upstream_path:
            raise ProvenanceVerificationError(
                f"tree entry for {web_locale!r} reports path "
                f"{tree_entry.get('path')!r}, expected exactly "
                f"{expected_upstream_path!r}"
            )
        actual_mode = tree_entry.get("mode")
        if actual_mode != _EXPECTED_REGULAR_FILE_MODE:
            raise ProvenanceVerificationError(
                f"{expected_upstream_path!r} at commit {source_commit!r} "
                f"has git mode {actual_mode!r}, expected exactly "
                f"{_EXPECTED_REGULAR_FILE_MODE!r} (a plain, non-executable, "
                "non-symlink, non-submodule regular file)"
            )
        upstream_blob_sha = tree_entry.get("sha")
        if not isinstance(upstream_blob_sha, str) or not upstream_blob_sha:
            raise ProvenanceVerificationError(
                f"tree entry for {expected_upstream_path!r} has no usable "
                "'sha' field"
            )

        pinned_path = sources_dir / f"{web_locale}.json"
        if not pinned_path.is_file():
            raise ProvenanceVerificationError(
                f"pinned source file {pinned_path} does not exist locally"
            )
        pinned_bytes = pinned_path.read_bytes()
        pinned_blob_sha = _git_blob_sha1(pinned_bytes)
        if pinned_blob_sha != upstream_blob_sha:
            raise ProvenanceVerificationError(
                f"locale {web_locale!r}: checked-in {pinned_path} has git "
                f"blob SHA-1 {pinned_blob_sha!r}, but commit "
                f"{source_commit!r} of {_EXPECTED_SOURCE_REPOSITORY} at "
                f"{expected_upstream_path!r} is blob {upstream_blob_sha!r} "
                "-- these must be byte-for-byte identical objects"
            )


def main(argv: list[str]) -> int:
    del argv  # no CLI arguments; behaviour is fixed and non-configurable
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    try:
        verify(token=token)
    except ProvenanceVerificationError as exc:
        print(f"asset locale digest provenance verification FAILED: {exc}", file=sys.stderr)
        return 1
    print(
        "asset locale digest provenance verification succeeded: every "
        "pinned source file byte-for-byte matches its declared upstream "
        "commit and path.",
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
