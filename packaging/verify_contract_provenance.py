#!/usr/bin/env python3
"""Prove every vendored contracts/ file this client's decoders are bound to
is byte-identical to the same path in the *actual backend git history*, at
the exact commit ContractPin.cpp pins to -- not merely internally
consistent with a SHA-256 recorded beside it.

ContractDriftTests (see tests/ContractDriftTests.cpp) already re-hashes
every vendored file and compares it against the digest table in
src/ContractPin.cpp. That is a real and useful check, but it has one
structural blind spot: the recorded digest lives in the same repository,
editable in the same commit, as the bytes it is meant to police. A change
that edits contracts/fixtures/catalog.json *and* updates its digest in
ContractPin.cpp together would still pass ContractDriftTests, even though
the file may no longer match what the pinned backend commit actually
published. That test can catch accidental drift (edit-without-re-hash);
it cannot catch a deliberate or careless substitution that is internally
self-consistent.

This script closes that gap with an independent, external proof: it
fetches the exact pinned backend commit directly from the backend's own
git remote (djensenius/ArkhamHorror on GitHub, by default -- never a local
developer checkout path, so this proof is reproducible identically in CI
and does not depend on whatever happens to be checked out on this
machine) and byte-compares each governed file against the real git blob
at that commit. A mismatch here means the vendored file itself has
diverged from backend history, regardless of what digest sits beside it.

The governed path list is read directly out of src/ContractPin.cpp's
governedFixtureDigests() table (rather than being duplicated here) so
there is exactly one place that list can drift out of sync with what
ContractDriftTests actually verifies.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

DEFAULT_REMOTE = "https://github.com/djensenius/ArkhamHorror.git"

_GOVERNED_PATH_RE = re.compile(
    r'\{QStringLiteral\("([^"]+)"\),\s*QStringLiteral\(\s*((?:"[^"]*"\s*)+)\)\}',
    re.MULTILINE,
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def _adjacent_literal_concat(raw: str) -> str:
    """Joins the adjacent C++ string-literal pieces captured by
    _GOVERNED_PATH_RE's second group (ContractPin.cpp wraps each 64-char
    hex digest across two lines as two adjacent literals, which the C++
    preprocessor concatenates -- this replicates that concatenation)."""
    return "".join(piece[1:-1] for piece in re.findall(r'"[^"]*"', raw))


def _read_governed_digests(repo_root: Path) -> list[tuple[str, str]]:
    text = (repo_root / "src" / "ContractPin.cpp").read_text(encoding="utf-8")
    entries = [
        (path, _adjacent_literal_concat(digest_literal))
        for path, digest_literal in _GOVERNED_PATH_RE.findall(text)
    ]
    if not entries:
        raise RuntimeError(
            "found no governed digest entries in src/ContractPin.cpp -- "
            "did its table's formatting change?"
        )
    return entries


def _read_pinned_commit(repo_root: Path) -> str:
    pin = json.loads((repo_root / "contracts" / "contract-pin.json").read_text())
    commit = pin["backendCommit"]
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise RuntimeError(
            f"contracts/contract-pin.json's backendCommit {commit!r} is not "
            "a 40-character lowercase hex git commit SHA"
        )
    return commit


def _run(args: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(args, check=True, capture_output=True, **kwargs)


def _fetch_pinned_commit(cache_dir: Path, remote: str, commit: str) -> None:
    cache_dir.mkdir(parents=True, exist_ok=True)
    if not (cache_dir / ".git").exists():
        _run(["git", "init", "-q", str(cache_dir)])
    # A shallow, single-commit fetch by exact SHA: this is the smallest
    # possible amount of backend history that still lets us read the real
    # git blob at the pinned commit, and it works against GitHub's public
    # smart-HTTP remote without ever cloning the whole backend repository.
    _run(
        ["git", "-C", str(cache_dir), "fetch", "--depth", "1", remote, commit],
    )


def _blob_bytes(cache_dir: Path, commit: str, path: str) -> bytes:
    result = _run(
        ["git", "-C", str(cache_dir), "cat-file", "-p", f"{commit}:{path}"]
    )
    return result.stdout


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
    governed = _read_governed_digests(repo_root)
    commit = _read_pinned_commit(repo_root)
    cache_dir = (
        Path(args.cache_dir)
        if args.cache_dir
        else repo_root / "build" / "contract-provenance-cache"
    )

    print(f"Fetching pinned backend commit {commit} from {args.remote} ...")
    try:
        _fetch_pinned_commit(cache_dir, args.remote, commit)
    except subprocess.CalledProcessError as exc:
        sys.stderr.write(
            "error: could not fetch the pinned backend commit "
            f"{commit} from {args.remote}:\n{exc.stderr.decode(errors='replace')}\n"
        )
        return 2

    failures: list[str] = []
    for relative_path, recorded_digest in governed:
        backend_path = f"contracts/{relative_path}"
        vendored_path = repo_root / "contracts" / relative_path

        try:
            backend_bytes = _blob_bytes(cache_dir, commit, backend_path)
        except subprocess.CalledProcessError as exc:
            failures.append(
                f"{relative_path}: could not read {backend_path} at "
                f"{commit} from the backend remote: "
                f"{exc.stderr.decode(errors='replace').strip()}"
            )
            continue

        if not vendored_path.exists():
            failures.append(
                f"{relative_path}: vendored file is missing at "
                f"{vendored_path}"
            )
            continue
        vendored_bytes = vendored_path.read_bytes()

        backend_digest = hashlib.sha256(backend_bytes).hexdigest()
        vendored_digest = hashlib.sha256(vendored_bytes).hexdigest()

        if backend_bytes != vendored_bytes:
            failures.append(
                f"{relative_path}: vendored bytes do NOT match the real "
                f"backend blob at commit {commit} (backend sha256="
                f"{backend_digest}, vendored sha256={vendored_digest})"
            )
        elif backend_digest != recorded_digest:
            # Should be unreachable if the byte comparison above passed,
            # but checked explicitly so a hash-collision-shaped bug in this
            # script itself cannot silently report success.
            failures.append(
                f"{relative_path}: byte-identical to the backend blob, but "
                f"ContractPin.cpp's recorded digest {recorded_digest} does "
                f"not match the recomputed digest {backend_digest}"
            )
        else:
            print(f"  OK  {relative_path}  ({backend_digest})")

    if failures:
        sys.stderr.write(
            f"\ncontract provenance check FAILED for {len(failures)} of "
            f"{len(governed)} governed file(s):\n"
        )
        for failure in failures:
            sys.stderr.write(f"  - {failure}\n")
        return 1

    print(
        f"\nAll {len(governed)} governed contracts/ file(s) are byte-identical "
        f"to backend commit {commit}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
