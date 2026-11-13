#!/usr/bin/env python3
"""
verify-fw-sha256.py — fail-closed build-time sha256 check for RDNA3 firmware.

Reads one or more .bin files, computes their sha256, and compares against the
RDNA3 firmware table in src/rdna3/fw_table_rdna3.cpp (the in-repo authority
that the host test T4 also checks).  A clean checkout therefore never needs
evidence/ (which is not part of this repository).  Any mismatch (or missing
file / unregistered blob) produces a non-zero exit code.

Usage:
    python3 tools/verify-fw-sha256.py firmware/*.bin                  # in-repo table
    python3 tools/verify-fw-sha256.py --index <path> firmware/*.bin   # override
"""

import argparse
import hashlib
import os
import re
import sys
from pathlib import Path

# Regex matching INDEX M7 rows: | `amdgpu/<name>.bin` | <size> | `<sha256>` | ...
ROW_RE = re.compile(
    r"^\|\s*`(amdgpu/([^`]+\.bin))`\s*\|\s*(\d+)\s*\|\s*`([0-9a-f]{64})`\s*\|",
    re.M
)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1048576)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


# Regex matching one FwTable row in fw_table_rdna3.cpp:
#   { amdgpu::FwId::X, "name", "<sha256>", "<rev>", "amdgpu/name.bin", <size> },
ROWREPO_RE = re.compile(
    r"\{\s*amdgpu::FwId::[A-Za-z0-9_]+\s*,\s*\"([^\"]+)\"\s*,\s*"
    r"\"([0-9a-f]{64})\"\s*,\s*\"[^\"]*\"\s*,\s*\"amdgpu/([^\"]+\.bin)\"\s*,\s*(\d+)\s*\}",
    re.M
)


def repo_table() -> Path:
    root = Path(__file__).resolve().parent.parent   # tools -> navi48-bringup
    return root / "src" / "rdna3" / "fw_table_rdna3.cpp"


def parse_repo_table(path: Path) -> dict:
    """Return {basename: (size_bytes, sha256)} from the in-repo firmware table."""
    rows = {}
    for m in ROWREPO_RE.finditer(path.read_text()):
        basename = m.group(3)
        rows[basename] = (int(m.group(4)), m.group(2).lower())
    return rows


def find_index(explicit: str = None) -> Path:
    if explicit:
        p = Path(explicit)
        if p.is_file():
            return p
        raise FileNotFoundError(f"--index {explicit} not found")

    env = os.environ.get("N48_FW_INDEX")
    if env:
        p = Path(env)
        if p.is_file():
            return p

    here = Path(__file__).resolve()
    for parent in here.parents:
        cand = parent / "evidence" / "src" / "INDEX.md"
        if cand.is_file():
            return cand

    # Fallback relative to navi48-bringup root
    here = Path(__file__).resolve()
    root = here.parent.parent  # tools -> navi48-bringup
    cand = root / "../../../../evidence/src/INDEX.md"
    if cand.is_file():
        return cand.resolve()

    raise FileNotFoundError(
        "Cannot locate evidence/src/INDEX.md; set N48_FW_INDEX or pass --index"
    )


def parse_index(path: Path) -> dict:
    """Return {basename: (size_bytes, sha256)} for all amdgpu/ rows."""
    text = path.read_text()
    rows = {}
    for m in ROW_RE.finditer(text):
        full_path = m.group(1)          # "amdgpu/gc_11_0_1_imu.bin"
        basename = m.group(2)           # "gc_11_0_1_imu.bin"
        size = int(m.group(3))
        sha = m.group(4).lower()
        rows[basename] = (size, sha)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Fail-closed sha256 check for RDNA3 firmware blobs."
    )
    parser.add_argument("--index", help="path to evidence/src/INDEX.md")
    parser.add_argument("files", nargs="+", help="firmware .bin files")
    args = parser.parse_args()

    if args.index:
        try:
            index = parse_index(find_index(args.index))
        except FileNotFoundError as e:
            print(f"FAIL: {e}", file=sys.stderr)
            return 1
    else:
        table = repo_table()
        if not table.is_file():
            print(f"FAIL: {table} not found (in-repo firmware table)", file=sys.stderr)
            return 1
        index = parse_repo_table(table)
    failures = 0

    for f in args.files:
        path = Path(f)
        if not path.is_file():
            print(f"FAIL: {f}: file not found", file=sys.stderr)
            failures += 1
            continue

        basename = path.name
        file_size = path.stat().st_size

        record = index.get(basename)
        if record is None:
            print(f"FAIL: {basename}: not registered in the RDNA3 firmware table",
                  file=sys.stderr)
            failures += 1
            continue

        expected_size, expected_sha = record
        if file_size != expected_size:
            print(
                f"FAIL: {basename}: size mismatch "
                f"(file={file_size}, expected={expected_size})",
                file=sys.stderr,
            )
            failures += 1
            continue

        actual_sha = sha256_file(f)
        if actual_sha != expected_sha:
            print(
                f"FAIL: {basename}: sha256 mismatch\n"
                f"  actual:   {actual_sha}\n"
                f"  expected: {expected_sha}",
                file=sys.stderr,
            )
            failures += 1
            continue

        print(f"OK: {basename} ({file_size} bytes, sha256 matched)")

    if failures:
        print(f"FAILED: {failures}/{len(args.files)} blob(s) failed",
              file=sys.stderr)
        return 1

    print(f"All {len(args.files)} blobs verified against INDEX M7")
    return 0


if __name__ == "__main__":
    sys.exit(main())
