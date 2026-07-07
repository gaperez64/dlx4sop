#!/usr/bin/env python3
"""Smoke test for the branch/treewidth benchmark harness."""

import csv
import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_bench_branch_treewidth.py BENCH_SCRIPT SOP_SOLVE SOURCE_ROOT",
              file=sys.stderr)
        return 2
    bench_script = pathlib.Path(sys.argv[1])
    sop_solve = pathlib.Path(sys.argv[2])
    source_root = pathlib.Path(sys.argv[3])
    qsop = source_root / "tests" / "golden" / "solve_single.qsop"

    result = subprocess.run(
        [
            sys.executable,
            str(bench_script),
            "--solver",
            str(sop_solve),
            "--timeout",
            "10",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=25,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"benchmark harness failed\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )

    rows = list(csv.DictReader(result.stdout.splitlines()))
    if len(rows) != 2:
        raise AssertionError(f"expected two benchmark rows, got {len(rows)}: {rows}")
    backends = {row["backend"] for row in rows}
    if backends != {"branch", "treewidth"}:
        raise AssertionError(f"unexpected backend rows: {backends}")
    for row in rows:
        if row["status"] != "ok":
            raise AssertionError(f"benchmark row did not complete: {row}")
        if float(row["elapsed_s"]) < 0.0:
            raise AssertionError(f"bad elapsed time: {row}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
