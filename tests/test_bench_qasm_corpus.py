#!/usr/bin/env python3

import importlib.util
import pathlib
import sys


def load_bench_tool(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("bench_qasm_corpus", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_bench_qasm_corpus.py BENCH_QASM_CORPUS", file=sys.stderr)
        return 2

    bench = load_bench_tool(pathlib.Path(sys.argv[1]))
    trace = bench.parse_trace_csv(
        "\n".join(
            [
                "phase,depth,items,elapsed_ns",
                "branch.component_split,0,2,100",
                "branch.component_split,1,3,200",
                "branch.treewidth_delegate,1,33,700",
                "branch.root_treewidth_delegate,0,32,300",
                "branch.root_width_probe,0,6,0",
                "branch.rankwidth_delegate,2,40,1100",
                "branch.fallthrough,2,19,0",
                "branch.rankwidth_skip_join_pair_forecast,1,128,0",
                "branch.treewidth_skip_width,1,15,0",
                "rankwidth.width_probe,0,5,10",
                "rankwidth.support_width_probe,0,4,0",
                "rankwidth.table_forecast,0,64,0",
                "rankwidth.join_pair_forecast,0,512,0",
                "branch.cache_lookup,0,4,50",
                "branch.cache_canonical_lookup,0,6,30",
                "components.cache_lookup,0,2,60",
                "branch.cache_store,0,5,70",
                "branch.cache_canonical_store,0,7,40",
                "treewidth.multiply,3,16,400",
                "treewidth.sum_out,2,8,500",
                "rankwidth.join_map,2,12,120",
                "rankwidth.crt_join_map,2,20,130",
                "rankwidth.join,2,18,240",
                "rankwidth.crt_join,2,22,260",
                "rankwidth.labelled_join_map,2,9,90",
                "rankwidth.labelled_crt_join_map,2,11,110",
                "rankwidth.labelled_join,2,14,210",
                "rankwidth.labelled_crt_join,2,17,230",
                "rankwidth.fourier_join_map,2,7,70",
                "rankwidth.fourier_join,2,8,80",
                "",
            ]
        )
    )

    cache = bench.cache_record_metrics(
        {
            "cache_hits": 2,
            "cache_misses": 1,
            "cache_avoided_nodes": 5,
            "search_nodes": 20,
            "cache_entries": 4,
            "cache_canonical_entries": 3,
        },
        trace,
    )
    expected_cache = {
        "cache_hit_rate_ppm": 666666,
        "cache_avoided_node_rate_ppm": 250000,
        "cache_canonical_entry_rate_ppm": 750000,
        "cache_lookup_events": 3,
        "cache_lookup_elapsed_ns": 140,
        "cache_canonical_lookup_events": 1,
        "cache_canonical_lookup_elapsed_ns": 30,
        "cache_store_events": 2,
        "cache_store_elapsed_ns": 110,
        "cache_canonical_store_events": 1,
        "cache_canonical_store_elapsed_ns": 40,
    }
    if cache != expected_cache:
        raise AssertionError(f"unexpected cache metrics: {cache}")

    skip = bench.branch_skip_reason_metrics(trace)
    expected_skip = {
        "branch_treewidth_skip_width_events": 1,
        "branch_rankwidth_skip_join_pair_forecast_events": 1,
    }
    if skip != expected_skip:
        raise AssertionError(f"unexpected skip metrics: {skip}")

    treewidth_probe = bench.branch_treewidth_probe_metrics(trace)
    expected_treewidth_probe = {
        "branch_root_width_probe_events": 1,
        "branch_root_width_probe_elapsed_ns": 0,
        "branch_root_width_probe_width": 6,
    }
    if treewidth_probe != expected_treewidth_probe:
        raise AssertionError(f"unexpected treewidth probe metrics: {treewidth_probe}")

    dispatch = bench.branch_dispatch_metrics(trace)
    expected_dispatch = {
        "branch_component_split_events": 2,
        "branch_component_split_elapsed_ns": 300,
        "branch_component_split_max_components": 3,
        "branch_treewidth_delegate_events": 1,
        "branch_treewidth_delegate_elapsed_ns": 700,
        "branch_treewidth_delegate_max_vars": 33,
        "branch_root_treewidth_delegate_events": 1,
        "branch_root_treewidth_delegate_elapsed_ns": 300,
        "branch_root_treewidth_delegate_max_vars": 32,
        "branch_rankwidth_delegate_events": 1,
        "branch_rankwidth_delegate_elapsed_ns": 1100,
        "branch_rankwidth_delegate_max_vars": 40,
    }
    if dispatch != expected_dispatch:
        raise AssertionError(f"unexpected dispatch metrics: {dispatch}")

    fallthrough = bench.branch_fallthrough_metrics(trace)
    expected_fallthrough = {
        "branch_fallthrough_trace_events": 1,
        "branch_fallthrough_max_vars": 19,
    }
    if fallthrough != expected_fallthrough:
        raise AssertionError(f"unexpected fallthrough metrics: {fallthrough}")

    kernel = bench.treewidth_kernel_metrics(trace)
    expected_kernel = {
        "treewidth_multiply_events": 1,
        "treewidth_multiply_elapsed_ns": 400,
        "treewidth_sum_out_events": 1,
        "treewidth_sum_out_elapsed_ns": 500,
    }
    if kernel != expected_kernel:
        raise AssertionError(f"unexpected treewidth kernel metrics: {kernel}")

    rankwidth_kernel = bench.rankwidth_kernel_metrics(trace)
    expected_rankwidth_kernel = {
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
    }
    if rankwidth_kernel != expected_rankwidth_kernel:
        raise AssertionError(f"unexpected rankwidth kernel metrics: {rankwidth_kernel}")
    rankwidth_probe = bench.rankwidth_probe_metrics(trace)
    expected_rankwidth_probe = {
        "rankwidth_width_probe_events": 1,
        "rankwidth_width_probe_elapsed_ns": 10,
        "rankwidth_width_probe_width": 5,
        "rankwidth_support_width_probe_width": 4,
        "rankwidth_trace_table_forecast": 64,
        "rankwidth_trace_join_pair_forecast": 512,
    }
    if rankwidth_probe != expected_rankwidth_probe:
        raise AssertionError(f"unexpected rankwidth probe metrics: {rankwidth_probe}")
    kernel_record = {"stats": {}, **rankwidth_kernel}
    if bench.record_rankwidth_kernel_elapsed_ns(kernel_record) != 1540:
        raise AssertionError(f"unexpected rankwidth kernel elapsed: {kernel_record}")

    if bench.trace_elapsed_ns(trace) != 5100:
        raise AssertionError(f"unexpected trace elapsed: {bench.trace_elapsed_ns(trace)}")
    expected_top = (
        "branch.rankwidth_delegate:1100:21.6%,"
        "branch.treewidth_delegate:700:13.7%,"
        "treewidth.sum_out:500:9.8%"
    )
    if bench.trace_top_phase_text(trace) != expected_top:
        raise AssertionError(f"unexpected trace top phases: {bench.trace_top_phase_text(trace)}")
    expected_trace_metrics = {
        "trace_elapsed_ns": 5100,
        "trace_top_phase": "branch.rankwidth_delegate",
        "trace_top_elapsed_ns": 1100,
        "trace_top_share_ppm": 215686,
    }
    if bench.trace_record_metrics(trace) != expected_trace_metrics:
        raise AssertionError(f"unexpected trace record metrics: {bench.trace_record_metrics(trace)}")

    if not bench.amplitude_metrics(8, 10, [1, 0, 0, 0, 0, 0, 0, 0]):
        raise AssertionError("small exact counts should emit amplitude metrics")
    parsed_stats, _parsed_amplitude = bench.parse_stats_and_amplitude(
        "\n".join(
            [
                "backend: treewidth",
                "solve_mode: fourier",
                "solve_mode_kernel: fourier",
                "treewidth_order: min-fill",
                "result_modulus: 8",
                "result_norm_h: 0",
                "result_counts: 1 0 0 0 0 0 0 0",
            ]
        )
    )
    if parsed_stats.get("solve_mode") != "fourier":
        raise AssertionError(f"missing solve_mode in parsed stats: {parsed_stats}")
    if parsed_stats.get("solve_mode_kernel") != "fourier":
        raise AssertionError(f"missing solve_mode_kernel in parsed stats: {parsed_stats}")
    large_counts = [
        81129638414606753753383043072000,
        81129638414606681695789005144064,
        81129638414606681695789005144064,
        81129638414606681695789005144064,
        81129638414606609638194967216128,
        81129638414606681695789005144064,
        81129638414606681695789005144064,
        81129638414606681695789005144064,
    ]
    large_amplitude = bench.amplitude_metrics(8, 114, large_counts)
    if abs(large_amplitude["amplitude_real"] - 1.0) > 1e-12 or abs(large_amplitude["amplitude_imag"]) > 1e-12:
        raise AssertionError(f"unexpected large-count amplitude: {large_amplitude}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
