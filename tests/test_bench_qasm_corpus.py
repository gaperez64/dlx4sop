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
                "branch.rankwidth_delegate,2,40,1100",
                "branch.rankwidth_skip_join_pair_forecast,1,128,0",
                "branch.treewidth_skip_width,1,15,0",
                "branch.cache_lookup,0,4,50",
                "components.cache_lookup,0,2,60",
                "branch.cache_store,0,5,70",
                "",
            ]
        )
    )

    cache = bench.cache_record_metrics({"cache_hits": 2, "cache_misses": 1}, trace)
    expected_cache = {
        "cache_hit_rate_ppm": 666666,
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

    dispatch = bench.branch_dispatch_metrics(trace)
    expected_dispatch = {
        "branch_component_split_events": 2,
        "branch_component_split_elapsed_ns": 300,
        "branch_component_split_max_components": 3,
        "branch_treewidth_delegate_events": 2,
        "branch_treewidth_delegate_elapsed_ns": 1000,
        "branch_treewidth_delegate_max_vars": 33,
        "branch_rankwidth_delegate_events": 1,
        "branch_rankwidth_delegate_elapsed_ns": 1100,
        "branch_rankwidth_delegate_max_vars": 40,
    }
    if dispatch != expected_dispatch:
        raise AssertionError(f"unexpected dispatch metrics: {dispatch}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
