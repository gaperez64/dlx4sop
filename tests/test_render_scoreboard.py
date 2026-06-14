#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_render_scoreboard.py RENDER_SCOREBOARD", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    report = {
        "source": "Synthetic",
        "source_url": "https://example.invalid/synthetic",
        "records": [
            {"source": "Synthetic", "source_url": "https://example.invalid/synthetic", "status": "ok", "mode": "sign", "max_imported_nvars": 4},
            {"source": "Synthetic", "source_url": "https://example.invalid/synthetic", "status": "too_many_vars", "mode": "labelled", "max_imported_nvars": 80},
        ],
    }
    solver = {
        "backend": "treewidth",
        "treewidth_order": "min-fill",
        "source": "Synthetic",
        "solve_elapsed_ns": 1234,
        "stats": {"decomposition_width": 2, "max_table_entries": 16, "join_pairs": 32},
        "treewidth_width": 2,
        "treewidth_max_table_entries": 16,
    }
    native = {
        "engine": "qiskit-statevector",
        "status": "ok",
        "elapsed_ns": 5678,
        "qubits": 3,
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        report_path = tmp_path / "report.json"
        solver_path = tmp_path / "solver.jsonl"
        native_path = tmp_path / "native.jsonl"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        solver_path.write_text(json.dumps(solver) + "\n", encoding="utf-8")
        native_path.write_text(json.dumps(native) + "\n", encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                "--import-report",
                str(report_path),
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
            raise AssertionError(f"scoreboard render failed:\n{completed.stdout}\n{completed.stderr}")
        for expected in (
            "## Import Coverage",
            "| Synthetic | https://example.invalid/synthetic | 2 | 1 | 0 | 1 | 0 |",
            "`treewidth --treewidth-order min-fill`",
            "`qiskit-statevector`",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
