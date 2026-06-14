#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_compare_native_solver_results.py COMPARE_TOOL", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    solver_records = [
        {
            "source": "Synthetic",
            "source_relative_path": "bell.qasm",
            "case": "bell",
            "input": "00",
            "output": "00",
            "backend": "treewidth",
            "treewidth_order": "min-fill-max-degree",
            "status": "ok",
            "solve_elapsed_ns": 100,
            "amplitude_real": 1.25,
            "amplitude_imag": 0.0,
        },
        {
            "source": "Synthetic",
            "source_relative_path": "large.qasm",
            "case": "large",
            "input": "0",
            "output": "0",
            "backend": "treewidth",
            "treewidth_order": "min-fill-max-degree",
            "status": "timeout",
            "solve_elapsed_ns": 5000,
        },
    ]
    native_records = [
        {
            "source": "Synthetic",
            "source_relative_path": "bell.qasm",
            "case": "bell",
            "input": "00",
            "output": "00",
            "engine": "aer-statevector",
            "status": "ok",
            "elapsed_ns": 400,
            "amplitude_real": 1.0,
            "amplitude_imag": 0.0,
            "qubits": 2,
            "qubit_cap": 16,
            "timeout_seconds": 2.0,
            "memory_limit_mib": 512,
        },
        {
            "source": "Synthetic",
            "source_relative_path": "large.qasm",
            "case": "large",
            "input": "0",
            "output": "0",
            "engine": "aer-statevector",
            "status": "ok",
            "elapsed_ns": 700,
            "qubits": 1,
            "qubit_cap": 16,
            "timeout_seconds": 2.0,
            "memory_limit_mib": 512,
        },
    ]

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        solver_path = tmp_path / "solver.jsonl"
        native_path = tmp_path / "native.jsonl"
        solver_path.write_text("\n".join(json.dumps(record) for record in solver_records) + "\n", encoding="utf-8")
        native_path.write_text("\n".join(json.dumps(record) for record in native_records) + "\n", encoding="utf-8")

        completed = subprocess.run(
            [
                str(tool),
                "--solver-jsonl",
                f"synthetic={solver_path}",
                "--native-jsonl",
                f"synthetic={native_path}",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"compare tool failed:\n{completed.stdout}\n{completed.stderr}")
        for expected in (
            "# Native Solver Comparison",
            "`treewidth --treewidth-order min-fill-max-degree`",
            "`aer-statevector`",
            "1 / 2",
            "4.00x",
            "1 | 1 | 0.25",
            "16",
            "512",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")

        completed = subprocess.run(
            [
                str(tool),
                "--solver-jsonl",
                f"synthetic={solver_path}",
                "--native-jsonl",
                f"synthetic={native_path}",
                "--format",
                "json",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"compare json failed:\n{completed.stdout}\n{completed.stderr}")
        rows = json.loads(completed.stdout)
        if (
            rows[0]["both_ok"] != 1
            or rows[0]["native_ok_solver_skip"] != 1
            or rows[0]["amplitude_checked"] != 1
            or rows[0]["amplitude_mismatches"] != 1
            or rows[0]["amplitude_max_abs_error"] != 0.25
        ):
            raise AssertionError(f"unexpected json rows:\n{completed.stdout}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
