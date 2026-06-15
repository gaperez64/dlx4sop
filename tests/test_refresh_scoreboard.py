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
        "amplitude_real": 1.0,
        "amplitude_imag": 0.0,
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
        "branch_rankwidth_labelled_width": 4,
        "branch_rankwidth_support_width": 7,
        "branch_rankwidth_table_forecast": 2,
        "branch_rankwidth_join_pair_forecast": 9,
        "branch_treewidth_table_forecast": 128,
        "branch_treewidth_join_pair_forecast": 64,
        "branch_treewidth_order_width": 3,
        "branch_treewidth_skips": 2,
        "branch_rankwidth_skips": 7,
        "branch_treewidth_skip_width_events": 2,
        "branch_rankwidth_skip_table_forecast_events": 3,
        "branch_rankwidth_skip_join_pair_forecast_events": 4,
        "branch_component_split_events": 3,
        "branch_component_split_elapsed_ns": 5_000,
        "branch_component_split_max_components": 2,
        "branch_treewidth_delegate_events": 1,
        "branch_treewidth_delegate_elapsed_ns": 7_000,
        "branch_treewidth_delegate_max_vars": 33,
        "branch_root_treewidth_delegate_events": 1,
        "branch_root_treewidth_delegate_elapsed_ns": 3_000,
        "branch_root_treewidth_delegate_max_vars": 32,
        "branch_rankwidth_delegate_events": 1,
        "branch_rankwidth_delegate_elapsed_ns": 11_000,
        "branch_rankwidth_delegate_max_vars": 40,
        "cache_lookup_events": 2,
        "cache_lookup_elapsed_ns": 2_000,
        "cache_store_events": 1,
        "cache_store_elapsed_ns": 3_000,
        "stats": {
            "search_nodes": 3,
            "cache_hits": 1,
            "cache_misses": 2,
            "cache_entries": 2,
            "cache_stored_residue_slots": 16,
        },
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
        "amplitude_real": 1.0,
        "amplitude_imag": 0.0,
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
            "cache hits=1, misses=2, hit rate=0.333",
            "cache entries=2, slots=16",
            "cache trace lookup=2 events/2.0 us, store=1 events/3.0 us",
            "branch dispatch splits=3/5.0 us max components=2, tw delegates=1/7.0 us max vars=33, root tw delegates=1/3.0 us max vars=32, rw delegates=1/11.0 us max vars=40",
            "branch policy fallthroughs=0, tw skips=2, rw skips=7",
            "tw skip reasons width=2",
            "rw skip reasons table-forecast=3, join-pair-forecast=4",
            "branch rw probe labelled-cut-signature=4, support=7",
            "branch table forecast rw=2, tw=128",
            "branch join forecast rw=9, tw=64",
            "branch tw order width=3",
            "## Competitor Comparisons",
            "| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 1 / 1 | 1.0 us | 2.0 us | 2.00x | 1 | 0 | 0 | 1 | 16 | 10.0 | 4096 |",
            "## Current Takeaway",
        ):
            if expected not in output:
                raise AssertionError(f"missing {expected!r} in:\n{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
