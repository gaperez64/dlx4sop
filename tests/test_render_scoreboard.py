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
        "source_relative_path": "bell.qasm",
        "case": "bell",
        "input": "00",
        "output": "00",
        "solve_elapsed_ns": 1234,
        "amplitude_real": 1.0,
        "amplitude_imag": 0.0,
        "stats": {
            "decomposition_width": 2,
            "max_table_entries": 16,
            "join_pairs": 32,
            "search_nodes": 4,
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
        },
        "treewidth_width": 2,
        "treewidth_max_table_entries": 16,
        "rankwidth_table_forecast": 256,
        "rankwidth_join_pair_forecast": 96,
        "cache_lookup_events": 2,
        "cache_lookup_elapsed_ns": 2_000,
        "cache_canonical_lookup_events": 1,
        "cache_canonical_lookup_elapsed_ns": 800,
        "cache_store_events": 1,
        "cache_store_elapsed_ns": 3_000,
        "cache_canonical_store_events": 1,
        "cache_canonical_store_elapsed_ns": 900,
        "branch_rankwidth_table_forecast": 2,
        "branch_rankwidth_join_pair_forecast": 9,
        "branch_treewidth_table_forecast": 128,
        "branch_treewidth_join_pair_forecast": 64,
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
    }
    timeout_solver = {
        "backend": "branch",
        "branch_heuristic": "split",
        "source": "Synthetic",
        "status": "timeout",
        "solve_elapsed_ns": 2_000_000_000,
        "stats": {},
    }
    native = {
        "engine": "qiskit-statevector",
        "source": "Synthetic",
        "source_relative_path": "bell.qasm",
        "case": "bell",
        "input": "00",
        "output": "00",
        "status": "ok",
        "elapsed_ns": 5678,
        "amplitude_real": 1.0,
        "amplitude_imag": 0.0,
        "qubits": 3,
        "qubit_cap": 4,
        "timeout_seconds": 2.0,
        "memory_limit_mib": 512,
    }

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        report_path = tmp_path / "report.json"
        solver_path = tmp_path / "solver.jsonl"
        native_path = tmp_path / "native.jsonl"
        report_path.write_text(json.dumps(report), encoding="utf-8")
        solver_path.write_text(
            json.dumps(solver) + "\n" + json.dumps(timeout_solver) + "\n",
            encoding="utf-8",
        )
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
            "| synthetic | `branch --branch-heuristic split` | 0 / 1 | 2.00 s | 1 timeouts |",
            "| synthetic | `treewidth --treewidth-order min-fill` | 1 / 1 | 1.2 us | 4 nodes; cache hits=1, misses=2, hit rate=0.333, avoided nodes 1, avoided node rate 0.250, canonical hits 1, canonical lookups 3, canonical stores 2, entries 2, canonical entries 2, canonical entry rate 1.000, slots 16, key bytes 96, count bytes 64, estimated bytes 256; cache trace lookup=2 events/2.0 us, store=1 events/3.0 us; canonical cache trace lookup=1 events/800 ns, store=1 events/900 ns; component kernels convolution=1/85 ns, fourier=1/95 ns; tw width 2; max table 16; rw table forecast 256; rw join forecast 96; 32 join pairs; rankwidth kernels map=2/250 ns max items=20, join=2/500 ns max items=22, labelled-map=2/200 ns max items=11, labelled=2/440 ns max items=17, fourier-map=1/70 ns max items=7, fourier=1/80 ns max items=8, labelled-fourier-map=1/60 ns max items=6, labelled-fourier=1/75 ns max items=7; branch dispatch splits=3/5.0 us max components=2, tw delegates=1/7.0 us max vars=33, root tw delegates=1/3.0 us max vars=32, rw delegates=1/11.0 us max vars=40; branch policy fallthroughs=0, tw skips=2, rw skips=7; branch fallthrough max vars=19; tw skip reasons width=2; rw skip reasons table-forecast=3, join-pair-forecast=4; branch table forecast rw=2, tw=128; branch join forecast rw=9, tw=64; branch root tw probe width=6, 1 events/1.3 us |",
            "`qiskit-statevector`",
            "## Native Common-Row Comparison",
            "### Synthetic",
            "| synthetic | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 1 / 1 | 1.2 us | 5.7 us | 4.60x | 1 | 0 | 0 | 3 | 4 | 2.0 | 512 |  |",
        ):
            if expected not in completed.stdout:
                raise AssertionError(f"missing {expected!r} in:\n{completed.stdout}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
