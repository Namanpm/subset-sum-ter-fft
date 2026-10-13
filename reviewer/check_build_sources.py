"""Recover the exact sources behind the frozen experiment builds.

    python reviewer/check_build_sources.py                  # check; seconds
    python reviewer/check_build_sources.py --extract DIR    # also write the recovered trees

Every campaign build was made from a working tree with uncommitted changes, so a git
revision alone does not identify its code. Each build instead recorded a source manifest
(SHA-256 and path of every file under the source directories) and the manifest's own
SHA-256 in build_freeze.json. For each of the three frozen builds this script:

  1. checks that source_manifest.txt still has the SHA-256 recorded in build_freeze.json;
  2. finds, for every manifest entry, a file whose bytes have exactly the recorded SHA-256,
     searching the current source directories and every blob reachable from a git commit
     (a blob also counts in its LF->CRLF checkout form, as Windows checkouts write it);
  3. checks that all 29 pinned frozen root-backend files occur in the manifest, comparing
     with line endings normalized as ter2_root/frozen_root_backends.sha256 does.

With --extract, recovered files are written to DIR/<build>/<manifest path>; every written
file then has exactly the manifest's SHA-256. Compiled __pycache__ entries are ignored.
Exit code 0 only if every manifest hash matches, all frozen files are present, and the only
unrecoverable entries are the known ones listed in EXPECTED_MISSING.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILDS = ("regime-20261001", "positive-20261003", "whole-root-20261003")
SOURCE_DIRS = ("src", "include", "tests", "bench", "ter2_root", "docs")
# Edited in place after the builds and never committed in their recorded state. The build
# configuration is recorded separately (build_freeze.json cmake_cache, *.vcxproj copies).
EXPECTED_MISSING = {
    "regime-20261001": {"CMakeLists.txt"},
    "positive-20261003": {"ter2_root/CMakeLists.txt"},
    "whole-root-20261003": {"ter2_root/CMakeLists.txt", "ter2_root/scripts/summarize_arwlf_whole_root.py"},
}


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def add_candidate(index: dict, data: bytes) -> None:
    index.setdefault(sha256(data), data)
    if b"\r" not in data:
        crlf = data.replace(b"\n", b"\r\n")
        index.setdefault(sha256(crlf), crlf)


def build_index() -> tuple[dict, dict]:
    tree, history = {}, {}
    for top in SOURCE_DIRS:
        for path in (REPO / top).rglob("*"):
            if path.is_file() and "__pycache__" not in path.parts:
                add_candidate(tree, path.read_bytes())
    add_candidate(tree, (REPO / "CMakeLists.txt").read_bytes())

    objects = subprocess.run(["git", "-C", str(REPO), "rev-list", "--objects", "--all"],
                             capture_output=True, text=True, check=True).stdout
    ids = "\n".join(line.split()[0] for line in objects.splitlines()) + "\n"
    batch = subprocess.run(["git", "-C", str(REPO), "cat-file", "--batch"],
                           input=ids.encode(), capture_output=True, check=True).stdout
    pos = 0
    while pos < len(batch):
        end = batch.index(b"\n", pos)
        _, kind, size = batch[pos:end].split()
        start, size = end + 1, int(size)
        if kind == b"blob":
            add_candidate(history, batch[start:start + size])
        pos = start + size + 1
    return tree, history


def read_manifest(path: Path) -> list[tuple[str, str]]:
    rows = []
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        if not line.strip():
            continue
        digest, name = line.split(None, 1)
        name = name.strip().replace("\\", "/").removeprefix("./")
        if "/__pycache__/" not in "/" + name:
            rows.append((digest.lower(), name))
    return rows


def frozen_pins() -> set[str]:
    pins = set()
    for line in (REPO / "ter2_root/frozen_root_backends.sha256").read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            pins.add(line.split()[0].lower())
    return pins


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    cli.add_argument("--extract", type=Path, help="write each recovered build tree under this directory")
    args = cli.parse_args()

    tree, history = build_index()
    pins = frozen_pins()
    failures = 0
    for build in BUILDS:
        folder = REPO / "results/arwlf" / build
        freeze = json.loads((folder / "build_freeze.json").read_text(encoding="utf-8-sig"))
        manifest = folder / "source_manifest.txt"
        problems = []
        if sha256(manifest.read_bytes()) != freeze["source_manifest_sha256"]:
            problems.append("source_manifest.txt does not match build_freeze.json")
        rows = read_manifest(manifest)

        from_tree = from_history = 0
        missing = set()
        normalized = set()
        for digest, name in rows:
            data = tree.get(digest)
            if data is not None:
                from_tree += 1
            else:
                data = history.get(digest)
                from_history += data is not None
            if data is None:
                missing.add(name)
                continue
            normalized.add(sha256(data.replace(b"\r\n", b"\n")))
            if args.extract:
                out = args.extract / build / name
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(data)

        frozen = len(pins & normalized)
        if frozen != len(pins):
            problems.append(f"only {frozen} of {len(pins)} pinned frozen files are in the manifest")
        if missing - EXPECTED_MISSING[build]:
            problems.append("unexpectedly unrecoverable: " + ", ".join(sorted(missing - EXPECTED_MISSING[build])))

        status = "FAIL" if problems else "PASS"
        print(f"{status}  {build} ({freeze['git_commit'][:8]}, dirty tree): "
              f"{len(rows) - len(missing)} of {len(rows)} source files recovered byte-exact "
              f"({from_tree} current tree, {from_history} git history); "
              f"{frozen}/{len(pins)} frozen backend files present")
        if missing:
            print("      not recoverable: " + ", ".join(sorted(missing)))
        for problem in problems:
            print("      " + problem)
        failures += bool(problems)
    if args.extract:
        print(f"Recovered trees written under {args.extract}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
