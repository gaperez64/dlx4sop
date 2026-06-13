#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_solve(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    qsop = source_root / "tests" / "golden" / f"{name}.qsop"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), "--format", "residue-vector", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: sop-solve failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: residue-vector mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_max_vars_guard(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_single.qsop"
    completed = subprocess.run(
        [str(exe), "--max-vars", "0", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0:
        raise AssertionError("max-vars guard unexpectedly allowed solve")
    if "brute-force solver refuses 1 variables" not in completed.stderr:
        raise AssertionError(f"unexpected diagnostic:\n{completed.stderr}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_solve.py SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_solve(exe, source_root, "solve_single")
    run_solve(exe, source_root, "solve_labelled")
    run_max_vars_guard(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
