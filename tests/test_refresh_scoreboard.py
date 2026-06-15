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
        "rankwidth_support_width": 6,
        "rankwidth_labelled_width": 4,
        "rankwidth_table_forecast": 256,
        "rankwidth_join_pair_forecast": 96,
        "rankwidth_labelled_exact_cuts": 5,
        "rankwidth_labelled_proxy_cuts": 1,
        "rankwidth_labelled_exact_assignments": 64,
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
        "rankwidth_join_map_events": 2,
        "rankwidth_join_map_elapsed_ns": 250,
        "rankwidth_join_map_max_items": 20,
        "rankwidth_join_events": 2,
        "rankwidth_join_elapsed_ns": 500,
        "rankwidth_join_max_items": 22,
        "rankwidth_labelled_join_map_events": 2,
        "rankwidth_labelled_join_map_elapsed_ns": 200,
        "rankwidth_labelled_join_map_max_items": 11,
        "rankwidth_labelled_join_events": 2,
        "rankwidth_labelled_join_elapsed_ns": 440,
        "rankwidth_labelled_join_max_items": 17,
        "rankwidth_fourier_join_map_events": 1,
        "rankwidth_fourier_join_map_elapsed_ns": 70,
        "rankwidth_fourier_join_map_max_items": 7,
        "rankwidth_fourier_join_events": 1,
        "rankwidth_fourier_join_elapsed_ns": 80,
        "rankwidth_fourier_join_max_items": 8,
        "rankwidth_labelled_fourier_join_map_events": 1,
        "rankwidth_labelled_fourier_join_map_elapsed_ns": 60,
        "rankwidth_labelled_fourier_join_map_max_items": 6,
        "rankwidth_labelled_fourier_join_events": 1,
        "rankwidth_labelled_fourier_join_elapsed_ns": 75,
        "rankwidth_labelled_fourier_join_max_items": 7,
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
            "rankwidth_support_width": 6,
            "rankwidth_labelled_width": 4,
            "rankwidth_table_forecast": 256,
            "rankwidth_join_pair_forecast": 96,
            "rankwidth_labelled_exact_cuts": 5,
            "rankwidth_labelled_proxy_cuts": 1,
            "rankwidth_labelled_exact_assignments": 64,
            "max_residual_min_fill_width": 5,
            "max_residual_linear_cut_rank": 12,
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
        "qsop_mode": "labelled",
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
        rankwidth_comparison_output = tmp_path / "rankwidth-backends.md"
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
                "--rankwidth-comparison-output",
                str(rankwidth_comparison_output),
                "--rankwidth-comparison-qsop-mode",
                "labelled",
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
            "| Synthetic | https://example.invalid/synthetic | 2 | 0 | 1 | 0 | 0 | 1 / 2 | labelled 2 |",
            "## 257-512 Sample Stratification",
            "| Solved, width <= 11 | 1 | 1 | 0 | 11 | 32768 |",
            "| Timeouts | 1 | 0 | 1 | 0 | 0 |",
            "## Internal Solver Configurations",
            "`treewidth --treewidth-order min-fill`",
            "`branch --branch-heuristic split`",
            "cache hits=1, misses=2, hit rate=0.333",
            "cache avoided nodes=1, rate=0.333",
            "cache canonical hits=1",
            "cache canonical lookups=3, stores=2",
            "cache entries=2, canonical=2, canonical rate=1.000, slots=16",
            "cache bytes key=96, counts=64, estimated=256",
            "cache trace lookup=2 events/2.0 us, store=1 events/3.0 us",
            "canonical cache trace lookup=1 events/800 ns, store=1 events/900 ns",
            "rw table forecast 256",
            "rw join forecast 96",
            "rw labelled-cut-signature=4, support=6",
            "rw cut estimates exact=5, proxy=1, assignments=64",
            "rankwidth kernels map=2/250 ns max items=20, join=2/500 ns max items=22, labelled-map=2/200 ns max items=11, labelled=2/440 ns max items=17, fourier-map=1/70 ns max items=7, fourier=1/80 ns max items=8",
            "branch dispatch splits=3/5.0 us max components=2, tw delegates=1/7.0 us max vars=33, root tw delegates=1/3.0 us max vars=32, rw delegates=1/11.0 us max vars=40",
            "branch policy fallthroughs=0, tw skips=2, rw skips=7",
            "branch fallthrough max vars=19",
            "tw skip reasons width=2",
            "rw skip reasons table-forecast=3, join-pair-forecast=4",
            "branch rw probe labelled-cut-signature=4, support=7",
            "branch table forecast rw=2, tw=128",
            "branch join forecast rw=9, tw=64",
            "branch tw order width=3",
            "branch root tw probe width=6, 1 events/1.3 us",
            "max residual tw=5, cut-rank=12",
            "## Competitor Comparisons",
            "| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 1 / 1 | 1.0 us | 2.0 us | 2.00x | 1 | 0 | 0 | 1 | 16 | 10.0 | 4096 |",
            "## Current Takeaway",
        ):
            if expected not in output:
                raise AssertionError(f"missing {expected!r} in:\n{output}")
        rankwidth_output = rankwidth_comparison_output.read_text(encoding="utf-8")
        for expected in (
            "# Rankwidth Backend Comparison",
            "`rankwidth:min-fill-cut:count-table` | 1 / 1 | 900 ns | 1 | 4 | 4 | 2",
            "| 33-64 | labelled | 1 | 1 | 0 | 900 ns | 1.0 us | -100 ns | 1 / 1 | 1 / 0 / 0 | 1 / 0 / 0 | 1 / 1 | -100 ns / -9.1 us | 0 |",
            "Synthetic:bell 0->0",
        ):
            if expected not in rankwidth_output:
                raise AssertionError(f"missing {expected!r} in:\n{rankwidth_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
