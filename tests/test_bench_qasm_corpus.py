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
                "branch.rankwidth_skip_join_pair_forecast,1,128,0",
                "branch.treewidth_skip_width,1,15,0",
                "branch.cache_lookup,0,4,50",
                "components.cache_lookup,0,2,60",
                "branch.cache_store,0,5,70",
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
        {"cache_hits": 2, "cache_misses": 1, "cache_avoided_nodes": 5, "search_nodes": 20},
        trace,
    )
    expected_cache = {
        "cache_hit_rate_ppm": 666666,
        "cache_avoided_node_rate_ppm": 250000,
        "cache_lookup_events": 2,
        "cache_lookup_elapsed_ns": 110,
        "cache_store_events": 1,
        "cache_store_elapsed_ns": 70,
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
    kernel_record = {"stats": {}, **rankwidth_kernel}
    if bench.record_rankwidth_kernel_elapsed_ns(kernel_record) != 1540:
        raise AssertionError(f"unexpected rankwidth kernel elapsed: {kernel_record}")

    if bench.trace_elapsed_ns(trace) != 5020:
        raise AssertionError(f"unexpected trace elapsed: {bench.trace_elapsed_ns(trace)}")
    expected_top = (
        "branch.rankwidth_delegate:1100:21.9%,"
        "branch.treewidth_delegate:700:13.9%,"
        "treewidth.sum_out:500:10.0%"
    )
    if bench.trace_top_phase_text(trace) != expected_top:
        raise AssertionError(f"unexpected trace top phases: {bench.trace_top_phase_text(trace)}")
    expected_trace_metrics = {
        "trace_elapsed_ns": 5020,
        "trace_top_phase": "branch.rankwidth_delegate",
        "trace_top_elapsed_ns": 1100,
        "trace_top_share_ppm": 219123,
    }
    if bench.trace_record_metrics(trace) != expected_trace_metrics:
        raise AssertionError(f"unexpected trace record metrics: {bench.trace_record_metrics(trace)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
