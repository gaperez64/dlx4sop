#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run(cmd: list[str], input_text: str | None = None) -> str:
    completed = subprocess.run(
        cmd,
        input=input_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"command failed: {cmd}\n{completed.stderr}")
    return completed.stdout


def canonicalize(sop_check: pathlib.Path, text: str) -> str:
    return run([str(sop_check), "-"], text)


def solve(sop_solve: pathlib.Path, text: str, backend: str) -> str:
    return run([str(sop_solve), "--backend", backend, "--format", "residue-vector", "-"], text)


def parse_counts(result_text: str) -> tuple[int, list[int]]:
    modulus = None
    counts = None
    for line in result_text.splitlines():
        parts = line.split()
        if parts[:2] == ["p", "qsop-result"]:
            modulus = int(parts[2])
        if parts[:1] == ["counts"]:
            counts = [int(value) for value in parts[1:]]
    if modulus is None or counts is None:
        raise AssertionError(f"invalid result text:\n{result_text}")
    return modulus, counts


def rotated_counts(counts: list[int], shift: int) -> list[int]:
    out = [0 for _ in counts]
    for residue, count in enumerate(counts):
        out[(residue + shift) % len(counts)] += count
    return out


def test_canonicalization_idempotence(sop_check: pathlib.Path, source_root: pathlib.Path) -> None:
    for name in ["sign_raw.qsop"]:
        raw = (source_root / "tests" / "golden" / name).read_text()
        canonical = canonicalize(sop_check, raw)
        canonical_again = canonicalize(sop_check, canonical)
        if canonical_again != canonical:
            raise AssertionError(f"{name}: canonicalization is not idempotent")


def test_solver_agreement(
    sop_check: pathlib.Path, sop_solve: pathlib.Path, source_root: pathlib.Path
) -> None:
    raw = (source_root / "tests" / "golden" / "sign_raw.qsop").read_text()
    canonical = canonicalize(sop_check, raw)
    expected = solve(sop_solve, canonical, "branch")
    for backend in ["branch", "treewidth"]:
        actual = solve(sop_solve, raw, backend)
        if actual != expected:
            raise AssertionError(f"{backend}: raw and canonical solve results differ")


def test_constant_shift(sop_solve: pathlib.Path) -> None:
    base = """p qsop-sign 8 2 1
n 0
cst 0
u 0 1
u 1 2
e 0 1
"""
    shifted = base.replace("cst 0", "cst 3")

    base_modulus, base_counts = parse_counts(solve(sop_solve, base, "branch"))
    shifted_modulus, shifted_counts = parse_counts(solve(sop_solve, shifted, "branch"))
    if base_modulus != shifted_modulus:
        raise AssertionError("constant-shift solve changed modulus")
    if shifted_counts != rotated_counts(base_counts, 3):
        raise AssertionError("constant phase did not rotate residue counts")


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_sop_invariants.py SOP_CHECK SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2

    sop_check = pathlib.Path(sys.argv[1])
    sop_solve = pathlib.Path(sys.argv[2])
    source_root = pathlib.Path(sys.argv[3])
    test_canonicalization_idempotence(sop_check, source_root)
    test_solver_agreement(sop_check, sop_solve, source_root)
    test_constant_shift(sop_solve)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
