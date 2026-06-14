#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_refresh_scoreboard.py REFRESH_SCOREBOARD", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    fast_solver = {
        "backend": "treewidth",
        "treewidth_order": "min-fill",
        "source": "Synthetic",
        "source_url": "https://example.invalid/synthetic",
        "source_relative_path": "bell.qasm",
        "case": "bell",
        "input": "0",
        "output": "0",
        "qsop_mode": "labelled",
        "solve_elapsed_ns": 1_000,
        "status": "ok",
        "treewidth_width": 1,
        "treewidth_max_table_entries": 8,
        "stats": {"decomposition_width": 1, "max_table_entries": 8},
    }
    slow_solver = {
        **fast_solver,
        "backend": "branch",
        "branch_heuristic": "split",
        "solve_elapsed_ns": 10_000,
        "stats": {"search_nodes": 3},
    }
    native = {
        "engine": "qiskit-statevector",
        "source": "Synthetic",
        "source_relative_path": "bell.qasm",
        "case": "bell",
        "input": "0",
        "output": "0",
        "status": "ok",
        "elapsed_ns": 2_000,
        "qubits": 1,
        "qubit_cap": 16,
        "timeout_seconds": 10.0,
        "memory_limit_mib": 4096,
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        solver_path = tmp_path / "solver.jsonl"
        native_path = tmp_path / "native.jsonl"
        output_path = tmp_path / "scoreboard.md"
        solver_path.write_text(
            json.dumps(fast_solver) + "\n" + json.dumps(slow_solver) + "\n",
            encoding="utf-8",
        )
        native_path.write_text(json.dumps(native) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                "--no-default-artifacts",
                "--solver-jsonl",
                f"33-64={solver_path}",
                "--native-jsonl",
                f"33-64={native_path}",
                "--output",
                str(output_path),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"scoreboard refresh failed:\n{completed.stdout}\n{completed.stderr}")
        output = output_path.read_text(encoding="utf-8")
        for expected in (
            "## Benchmarks Used",
            "| Synthetic | https://example.invalid/synthetic | 1 | 0 | 1 | 0 | 0 | 0 | labelled 1 |",
            "## Internal Solver Configurations",
            "`treewidth --treewidth-order min-fill`",
            "`branch --branch-heuristic split`",
            "## Competitor Comparisons",
            "| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 1 / 1 | 1.0 us | 2.0 us | 2.00x | 1 | 16 | 10.0 | 4096 |",
            "## Current Takeaway",
        ):
            if expected not in output:
                raise AssertionError(f"missing {expected!r} in:\n{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
