#!/usr/bin/env python3
"""Smoke-test bench_gauntlet's import-only control flow without requiring QPY input."""

from __future__ import annotations

import importlib.util
import pathlib
import sys


def load_module(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("bench_gauntlet", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} BENCH_GAUNTLET", file=sys.stderr)
        return 2

    module = load_module(pathlib.Path(sys.argv[1]))

    class ImportOnlyBench(module.Bench):
        def translate(self, payload: pathlib.Path) -> tuple[str, int]:
            return "OPENQASM 2.0;\nqreg q[2];\n", 2

        def import_qsop(self, qasm: str, zero: str,
                        exact_only: bool = False) -> tuple[str, str, str, str]:
            if zero != "00":
                raise AssertionError(f"unexpected boundary: {zero}")
            if not exact_only:
                raise AssertionError("import-only mode did not request exact-only classification")
            return "p qsop-sign 8 0 0\nn 0\ncst 0\n", "exact", "", ""

        def shape(self, qsop: str) -> dict:
            raise AssertionError("import-only mode called sop-stats")

        def solve(self, qsop: str) -> tuple[str, float, dict]:
            raise AssertionError("import-only mode called sop-solve")

    bench = ImportOnlyBench(pathlib.Path("qasm2sop"), pathlib.Path("sop-stats"),
                            pathlib.Path("sop-solve"), 1.0, [], 0, True)
    row = bench.case(pathlib.Path("alg85/v1/payloads/example.qpy"))
    expected = {
        "suite": "alg85",
        "instance": "example",
        "qubits": 2,
        "import_mode": "exact",
        "outcome": "imported",
        "wall_s": "",
    }
    for key, value in expected.items():
        if row[key] != value:
            raise AssertionError(f"expected {key}={value!r}, got {row[key]!r}")
    if not row["load_s"] or not row["import_s"]:
        raise AssertionError(f"missing import timings: {row}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
