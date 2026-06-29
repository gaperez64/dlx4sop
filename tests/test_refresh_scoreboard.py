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
        "qsop_mode": "sign",
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
        "branch_rankwidth_cutrank_width": 7,
        "branch_rankwidth_table_forecast": 2,
        "branch_rankwidth_join_pair_forecast": 9,
        "rankwidth_cutrank_width": 6,
        "rankwidth_table_forecast": 256,
        "rankwidth_join_pair_forecast": 96,
        "branch_treewidth_table_forecast": 128,
        "branch_treewidth_join_pair_forecast": 64,
        "branch_treewidth_order_width": 3,
        "branch_treewidth_skips": 2,
        "branch_rankwidth_skips": 7,
        "branch_fallthrough_trace_events": 1,
        "branch_fallthrough_max_vars": 19,
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
        "branch_root_width_probe_events": 1,
        "branch_root_width_probe_elapsed_ns": 1_300,
        "branch_root_width_probe_width": 6,
        "branch_rankwidth_delegate_events": 1,
        "branch_rankwidth_delegate_elapsed_ns": 11_000,
        "branch_rankwidth_delegate_max_vars": 40,
        "components_convolution_events": 1,
        "components_convolution_elapsed_ns": 85,
        "components_fourier_multiply_events": 1,
        "components_fourier_multiply_elapsed_ns": 95,
        "branch_fourier_multiply_events": 1,
        "branch_fourier_multiply_elapsed_ns": 105,
        "rankwidth_join_map_events": 2,
        "rankwidth_join_map_elapsed_ns": 250,
        "rankwidth_join_map_max_items": 20,
        "rankwidth_join_events": 2,
        "rankwidth_join_elapsed_ns": 500,
        "rankwidth_join_max_items": 22,
        "rankwidth_fourier_join_map_events": 1,
        "rankwidth_fourier_join_map_elapsed_ns": 70,
        "rankwidth_fourier_join_map_max_items": 7,
        "rankwidth_fourier_join_events": 1,
        "rankwidth_fourier_join_elapsed_ns": 80,
        "rankwidth_fourier_join_max_items": 8,
        "cache_lookup_events": 2,
        "cache_lookup_elapsed_ns": 2_000,
        "cache_canonical_lookup_events": 1,
        "cache_canonical_lookup_elapsed_ns": 800,
        "cache_store_events": 1,
        "cache_store_elapsed_ns": 3_000,
        "cache_canonical_store_events": 1,
        "cache_canonical_store_elapsed_ns": 900,
        "stats": {
            "search_nodes": 3,
            "cache_hits": 1,
            "cache_misses": 2,
            "cache_avoided_nodes": 1,
            "cache_canonical_hits": 1,
            "cache_canonical_lookups": 3,
            "cache_canonical_stores": 2,
            "cache_entries": 2,
            "cache_canonical_entries": 2,
            "cache_stored_residue_slots": 16,
            "cache_key_bytes": 96,
            "cache_count_bytes": 64,
            "cache_estimated_bytes": 256,
            "rankwidth_cutrank_width": 6,
            "rankwidth_table_forecast": 256,
            "rankwidth_join_pair_forecast": 96,
            "max_residual_min_fill_width": 5,
            "max_residual_prefix_cut_rank": 12,
        },
    }
    large_solver = {
        **fast_solver,
        "source_relative_path": "large.qasm",
        "case": "large",
        "input": "1",
        "output": "1",
        "solve_elapsed_ns": 2_000,
        "treewidth_width": 11,
        "treewidth_max_table_entries": 32_768,
        "stats": {
            "decomposition_width": 11,
            "max_table_entries": 32_768,
            "join_pairs": 512,
        },
    }
    large_timeout = {
        "backend": "treewidth",
        "treewidth_order": "min-fill-max-degree",
        "source": "Synthetic",
        "source_url": "https://example.invalid/synthetic",
        "source_relative_path": "timeout.qasm",
        "case": "timeout",
        "input": "0",
        "output": "0",
        "qsop_mode": "sign",
        "solve_elapsed_ns": 3_000_000_000,
        "status": "timeout",
        "stats": {},
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
    rankwidth_comparison = {
        **fast_solver,
        "backend": "rankwidth",
        "rankwidth_decomposition": "min-fill-cut",
        "rankwidth_mode": "count-table",
        "solve_elapsed_ns": 900,
        "rankwidth_width": 1,
        "rankwidth_max_table_entries": 4,
        "rankwidth_table_forecast": 4,
        "rankwidth_join_pair_forecast": 2,
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        solver_path = tmp_path / "solver.jsonl"
        large_path = tmp_path / "large.jsonl"
        native_path = tmp_path / "native.jsonl"
        rankwidth_comparison_path = tmp_path / "rankwidth-comparison.jsonl"
        output_path = tmp_path / "scoreboard.md"
        solver_path.write_text(
            json.dumps(fast_solver) + "\n" + json.dumps(slow_solver) + "\n",
            encoding="utf-8",
        )
        large_path.write_text(
            json.dumps(large_solver) + "\n" + json.dumps(large_timeout) + "\n",
            encoding="utf-8",
        )
        native_path.write_text(json.dumps(native) + "\n", encoding="utf-8")
        rankwidth_comparison_path.write_text(
            json.dumps(fast_solver) + "\n"
            + json.dumps(slow_solver) + "\n"
            + json.dumps(rankwidth_comparison) + "\n",
            encoding="utf-8",
        )
        completed = subprocess.run(
            [
                str(tool),
                "--no-default-artifacts",
                "--solver-jsonl",
                f"33-64={solver_path}",
                "--solver-jsonl",
                f"257-512 sample={large_path}",
                "--native-jsonl",
                f"33-64={native_path}",
                "--rankwidth-comparison-jsonl",
                f"33-64={rankwidth_comparison_path}",
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
            "# Scoreboard — sign QSOPs",
            "## Benchmarks",
            "| Synthetic | https://example.invalid/synthetic | 2 | 0 | 1 | 0 | 0 | 1 / 2 |",
            "## Survival Curves",
            "## WMC Solve Time Breakdown",
            "## Internal Solver Configurations",
            "`treewidth --treewidth-order min-fill`",
            "`branch --branch-heuristic split`",
            "| 33-64 | `treewidth --treewidth-order min-fill` | 1 / 1 | 1.0 us |",
            "| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 0 / 1 | 3.00 s |",
            "## Competitor Comparisons",
            "| 33-64 | 1.0 us | `qiskit-statevector` | 2.0 us | **2.00x** | 1 / 1 |",
            "## Current Takeaway",
        ):
            if expected not in output:
                raise AssertionError(f"missing {expected!r} in:\n{output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
