#!/usr/bin/env python3

import argparse
import collections
import datetime as _datetime
import pathlib
import subprocess
import sys
from typing import Iterable, TextIO

import compare_rankwidth_backends
from render_scoreboard import (
    BRANCH_RANKWIDTH_SKIP_REASON_FIELDS,
    BRANCH_TREEWIDTH_SKIP_REASON_FIELDS,
    branch_dispatch_text,
    branch_skip_reason_text,
    cache_avoided_node_rate,
    cache_canonical_entry_rate,
    cache_hit_rate,
    comparison_key,
    component_kernel_text,
    comparison_speedup,
    format_ns,
    has_comparison_identity,
    labelled_path,
    markdown_escape,
    read_jsonl,
    rankwidth_kernel_text,
    record_qsop_mode,
    solver_config,
    summarize_native_comparison_records,
    summarize_solver_records,
    tier_sort_key,
)


SOURCE_ORDER = ("Internal corpus", "FeynmanDD", "MQT Bench", "PyZX")
SOURCE_URLS = {
    "Internal corpus": "tests/qasm_solver_corpus.json",
    "FeynmanDD": "https://github.com/cqs-thu/feynman-decision-diagram",
    "MQT Bench": "https://github.com/munich-quantum-toolkit/bench",
    "PyZX": "https://github.com/zxcalc/pyzx",
}
SOLVER_TIERS = ("0-32", "33-64", "65-128", "129-256", "257-512 sample")
NATIVE_TIERS = ("0-32", "33-64", "65-128", "129-256")

DEFAULT_SOLVER_ARTIFACTS = (
    ("0-32", "dlx4sop-tier-0-32-treewidth-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-rankwidth-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-branch-hybrid-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-wmc-amplitude-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-wmc-amp-soft-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-wmc-amp-block-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-wmc-residue-fourier-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-wmc-residue-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-treewidth-fresh.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-branch-hybrid-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-rankwidth-min-fill-cut-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-wmc-amplitude-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-wmc-amp-soft-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-wmc-amp-block-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-wmc-residue-fourier-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-wmc-residue-current.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-treewidth-fresh.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-branch-hybrid-fresh.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-rankwidth-min-fill-cut-current.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-wmc-amplitude-current.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-wmc-amp-soft-current.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-wmc-amp-block-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-branch-hybrid-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-treewidth-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-rankwidth-min-fill-cut-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-wmc-amplitude-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-wmc-amp-soft-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-wmc-amp-block-current.jsonl"),
    ("257-512 sample", "dlx4sop-tier-257-512-sample-treewidth-current.jsonl"),
    ("257-512 sample", "dlx4sop-tier-257-512-sample-wmc-amplitude-current.jsonl"),
    ("257-512 sample", "dlx4sop-tier-257-512-sample-wmc-amp-soft-current.jsonl"),
    ("257-512 sample", "dlx4sop-tier-257-512-sample-wmc-amp-block-current.jsonl"),
    # MQT Bench materialized corpus (tiers 33-64 and 65-128, GHZ/BV/QFT families)
    ("33-64", "mqt-bench-tier-33-64-treewidth-current.jsonl"),
    ("33-64", "mqt-bench-tier-33-64-branch-hybrid-current.jsonl"),
    ("65-128", "mqt-bench-tier-65-128-treewidth-current.jsonl"),
    ("65-128", "mqt-bench-tier-65-128-branch-hybrid-current.jsonl"),
)

DEFAULT_NATIVE_ARTIFACTS = tuple(
    (tier, f"dlx4sop-tier-{tier.replace(' ', '-')}-native-all-current.jsonl") for tier in NATIVE_TIERS
) + (
    # MQT Bench native simulator comparisons
    ("33-64", "mqt-bench-tier-33-64-native-current.jsonl"),
    ("65-128", "mqt-bench-tier-65-128-native-current.jsonl"),
)
# Note: NATIVE_TIERS now includes "0-32" — the tier-0-32 manifest boundaries are all
# ≤16 qubits, well within the native --max-qubits cap, so the small tier yields the
# most meaningful QSOP-vs-statevector comparison and must not be skipped.


def display_source(record: dict) -> str:
    source = str(record.get("source") or "unknown")
    return "Internal corpus" if source.lower() == "internal" else source


def record_identity(tier: str, record: dict) -> tuple:
    if has_comparison_identity(record):
        return (
            tier,
            display_source(record),
            str(record.get("source_relative_path") or ""),
            str(record.get("case") or ""),
            str(record.get("input") or ""),
            str(record.get("output") or ""),
        )
    return (
        tier,
        display_source(record),
        str(record.get("qasm_sha256") or ""),
        str(record.get("qsop_sha256") or ""),
        str(record.get("case") or ""),
        str(record.get("input") or ""),
        str(record.get("output") or ""),
    )


def mode_name(record: dict) -> str:
    return record_qsop_mode(record)


def default_solver_jsonl(artifact_dir: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    return [(tier, artifact_dir / filename) for tier, filename in DEFAULT_SOLVER_ARTIFACTS]


def default_native_jsonl(artifact_dir: pathlib.Path) -> list[tuple[str, pathlib.Path]]:
    return [(tier, artifact_dir / filename) for tier, filename in DEFAULT_NATIVE_ARTIFACTS]


def read_named_jsonl(
    named_paths: Iterable[tuple[str, pathlib.Path]], allow_missing: bool
) -> list[tuple[str, list[dict]]]:
    records = []
    for label, path in named_paths:
        if not path.exists():
            if allow_missing:
                print(f"warning: missing artifact {path}", file=sys.stderr)
                continue
            raise RuntimeError(f"missing artifact {path}")
        records.append((label, read_jsonl(path)))
    return records


def source_sort_key(source: str) -> tuple[int, str]:
    if source in SOURCE_ORDER:
        return (SOURCE_ORDER.index(source), source)
    return (len(SOURCE_ORDER), source)


def coverage_by_source(named_records: list[tuple[str, list[dict]]]) -> dict[str, dict]:
    rows: dict[str, dict] = {}
    for tier, records in named_records:
        for record in records:
            source = display_source(record)
            entry = rows.setdefault(
                source,
                {
                    "source": source,
                    "url": record.get("source_url") or SOURCE_URLS.get(source, ""),
                    "attempted": collections.defaultdict(set),
                    "solved": collections.defaultdict(set),
                    "modes": collections.defaultdict(set),
                },
            )
            if not entry["url"] and record.get("source_url"):
                entry["url"] = record["source_url"]
            identity = record_identity(tier, record)
            entry["attempted"][tier].add(identity)
            if record.get("status", "ok") == "ok":
                entry["solved"][tier].add(identity)
                entry["modes"][mode_name(record)].add(identity)
    return rows


def mode_summary(entry: dict) -> str:
    parts = []
    for mode in sorted(entry["modes"]):
        if entry["modes"][mode]:
            parts.append(f"{mode} {len(entry['modes'][mode])}")
    return "; ".join(parts)


def format_count(value: int) -> str:
    return f"{value:,}"


def public_key_stats(stats: dict[str, int]) -> str:
    parts = []
    if "wmc_export_elapsed_ns" in stats or "wmc_ganak_elapsed_ns" in stats:
        parts.append(
            f"ganak {format_ns(stats.get('wmc_ganak_elapsed_ns', 0))} + "
            f"export {format_ns(stats.get('wmc_export_elapsed_ns', 0))}; "
            f"{stats.get('wmc_mismatches', 0)} amplitude mismatches"
        )
        return "; ".join(parts)
    if "search_nodes" in stats:
        parts.append(f"{format_count(stats['search_nodes'])} nodes")
    if "cache_hits" in stats or "cache_misses" in stats:
        parts.append(
            f"cache hits={format_count(stats.get('cache_hits', 0))}, "
            f"misses={format_count(stats.get('cache_misses', 0))}, "
            f"hit rate={cache_hit_rate(stats)}"
        )
    if stats.get("cache_avoided_nodes", 0):
        parts.append(
            f"cache avoided nodes={format_count(stats['cache_avoided_nodes'])}, "
            f"rate={cache_avoided_node_rate(stats)}"
        )
    if stats.get("cache_canonical_hits", 0):
        parts.append(f"cache canonical hits={format_count(stats['cache_canonical_hits'])}")
    if stats.get("cache_canonical_lookups", 0) or stats.get("cache_canonical_stores", 0):
        parts.append(
            f"cache canonical lookups={format_count(stats.get('cache_canonical_lookups', 0))}, "
            f"stores={format_count(stats.get('cache_canonical_stores', 0))}"
        )
    if "cache_entries" in stats:
        parts.append(
            f"cache entries={format_count(stats['cache_entries'])}, "
            f"canonical={format_count(stats.get('cache_canonical_entries', 0))}, "
            f"canonical rate={cache_canonical_entry_rate(stats)}, "
            f"slots={format_count(stats.get('cache_stored_residue_slots', 0))}"
        )
    if stats.get("cache_estimated_bytes", 0):
        parts.append(
            f"cache bytes key={format_count(stats.get('cache_key_bytes', 0))}, "
            f"counts={format_count(stats.get('cache_count_bytes', 0))}, "
            f"estimated={format_count(stats['cache_estimated_bytes'])}"
        )
    if "cache_lookup_elapsed_ns" in stats or "cache_store_elapsed_ns" in stats:
        parts.append(
            f"cache trace lookup={format_count(stats.get('cache_lookup_events', 0))} events/"
            f"{format_ns(stats.get('cache_lookup_elapsed_ns', 0))}, "
            f"store={format_count(stats.get('cache_store_events', 0))} events/"
            f"{format_ns(stats.get('cache_store_elapsed_ns', 0))}"
        )
    if "cache_canonical_lookup_elapsed_ns" in stats or "cache_canonical_store_elapsed_ns" in stats:
        parts.append(
            "canonical cache trace "
            f"lookup={format_count(stats.get('cache_canonical_lookup_events', 0))} events/"
            f"{format_ns(stats.get('cache_canonical_lookup_elapsed_ns', 0))}, "
            f"store={format_count(stats.get('cache_canonical_store_events', 0))} events/"
            f"{format_ns(stats.get('cache_canonical_store_elapsed_ns', 0))}"
        )
    component_kernel = component_kernel_text(stats, format_count)
    if component_kernel:
        parts.append(f"component kernels {component_kernel}")
    if "rankwidth_width" in stats:
        parts.append(f"rw width {stats['rankwidth_width']}")
    if "rankwidth_labelled_width" in stats or "rankwidth_support_width" in stats:
        parts.append(
            f"rw labelled-cut-signature={format_count(stats.get('rankwidth_labelled_width', 0))}, "
            f"support={format_count(stats.get('rankwidth_support_width', 0))}"
        )
    if "treewidth_width" in stats:
        parts.append(f"tw width {stats['treewidth_width']}")
    table = max(
        stats.get("rankwidth_max_table_entries", 0),
        stats.get("treewidth_max_table_entries", 0),
        stats.get("max_table_entries", 0),
    )
    if table:
        parts.append(f"max table {format_count(table)}")
    if "rankwidth_table_forecast" in stats:
        parts.append(f"rw table forecast {format_count(stats['rankwidth_table_forecast'])}")
    if "rankwidth_join_pair_forecast" in stats:
        parts.append(f"rw join forecast {format_count(stats['rankwidth_join_pair_forecast'])}")
    if "rankwidth_labelled_exact_cuts" in stats or "rankwidth_labelled_proxy_cuts" in stats:
        parts.append(
            f"rw cut estimates exact={format_count(stats.get('rankwidth_labelled_exact_cuts', 0))}, "
            f"proxy={format_count(stats.get('rankwidth_labelled_proxy_cuts', 0))}, "
            f"assignments={format_count(stats.get('rankwidth_labelled_exact_assignments', 0))}"
        )
    signatures = max(
        stats.get("rankwidth_max_signature_entries", 0),
        stats.get("max_signature_entries", 0),
    )
    if signatures:
        parts.append(f"max signatures {format_count(signatures)}")
    if "join_pairs" in stats:
        parts.append(f"{format_count(stats['join_pairs'])} join pairs")
    kernel_text = rankwidth_kernel_text(stats, format_count)
    if kernel_text:
        parts.append(f"rankwidth kernels {kernel_text}")
    if "treewidth_delegations" in stats or "rankwidth_delegations" in stats:
        parts.append(
            f"delegations tw={format_count(stats.get('treewidth_delegations', 0))}, "
            f"rw={format_count(stats.get('rankwidth_delegations', 0))}"
        )
    dispatch = branch_dispatch_text(stats, format_count)
    if dispatch:
        parts.append(f"branch dispatch {dispatch}")
    if (
        "branch_fallthroughs" in stats
        or "branch_treewidth_skips" in stats
        or "branch_rankwidth_skips" in stats
    ):
        parts.append(
            f"branch policy fallthroughs={format_count(stats.get('branch_fallthroughs', 0))}, "
            f"tw skips={format_count(stats.get('branch_treewidth_skips', 0))}, "
            f"rw skips={format_count(stats.get('branch_rankwidth_skips', 0))}"
        )
        if "branch_fallthrough_max_vars" in stats:
            parts.append(
                f"branch fallthrough max vars={format_count(stats['branch_fallthrough_max_vars'])}"
            )
        treewidth_skip_reasons = branch_skip_reason_text(
            stats,
            BRANCH_TREEWIDTH_SKIP_REASON_FIELDS,
            format_count,
        )
        if treewidth_skip_reasons:
            parts.append(f"tw skip reasons {treewidth_skip_reasons}")
        rankwidth_skip_reasons = branch_skip_reason_text(
            stats,
            BRANCH_RANKWIDTH_SKIP_REASON_FIELDS,
            format_count,
        )
        if rankwidth_skip_reasons:
            parts.append(f"rw skip reasons {rankwidth_skip_reasons}")
    if "max_residual_min_fill_width" in stats or "max_residual_prefix_cut_rank" in stats:
        parts.append(
            f"max residual tw={stats.get('max_residual_min_fill_width', 0)}, "
            f"cut-rank={stats.get('max_residual_prefix_cut_rank', 0)}"
        )
    if "branch_rankwidth_labelled_width" in stats or "branch_rankwidth_support_width" in stats:
        parts.append(
            f"branch rw probe labelled-cut-signature={stats.get('branch_rankwidth_labelled_width', 0)}, "
            f"support={stats.get('branch_rankwidth_support_width', 0)}"
        )
    if "branch_rankwidth_table_forecast" in stats or "branch_treewidth_table_forecast" in stats:
        parts.append(
            "branch table forecast "
            f"rw={format_count(stats.get('branch_rankwidth_table_forecast', 0))}, "
            f"tw={format_count(stats.get('branch_treewidth_table_forecast', 0))}"
        )
    if "branch_rankwidth_join_pair_forecast" in stats or "branch_treewidth_join_pair_forecast" in stats:
        parts.append(
            "branch join forecast "
            f"rw={format_count(stats.get('branch_rankwidth_join_pair_forecast', 0))}, "
            f"tw={format_count(stats.get('branch_treewidth_join_pair_forecast', 0))}"
        )
    if "branch_treewidth_order_width" in stats:
        parts.append(f"branch tw order width={stats['branch_treewidth_order_width']}")
    if "branch_root_width_probe_width" in stats:
        parts.append(
            f"branch root tw probe width={stats['branch_root_width_probe_width']}, "
            f"{format_count(stats.get('branch_root_width_probe_events', 0))} events/"
            f"{format_ns(stats.get('branch_root_width_probe_elapsed_ns', 0))}"
        )
    if "max_residual_vars" in stats or "max_residual_components" in stats:
        parts.append(
            f"max residual vars={format_count(stats.get('max_residual_vars', 0))}, "
            f"components={format_count(stats.get('max_residual_components', 0))}, "
            f"largest={format_count(stats.get('max_residual_largest_component', 0))}"
        )
    return "; ".join(parts)


def public_solver_status_stats(row: dict) -> str:
    parts = [public_key_stats(row["stats"])]
    if row.get("timeouts"):
        parts.append(f"{row['timeouts']} timeouts")
    if row.get("errors"):
        parts.append(f"{row['errors']} errors")
    return "; ".join(part for part in parts if part)


def tier_cell(entry: dict, tier: str) -> str:
    solved = len(entry["solved"].get(tier, ()))
    attempted = len(entry["attempted"].get(tier, ()))
    if attempted > solved:
        return f"{solved} / {attempted}"
    return str(solved)


def stratify_large_sample(records: list[dict]) -> tuple[int, list[dict]]:
    threshold = 0
    for record in records:
        if record.get("status") == "ok" and isinstance(record.get("treewidth_width"), int):
            threshold = max(threshold, int(record["treewidth_width"]))
    low_bucket = f"Solved, width <= {threshold}"
    high_bucket = f"Solved, width > {threshold}"
    buckets = {
        low_bucket: {"rows": 0, "solved": 0, "timeouts": 0, "max_width": 0, "max_table": 0},
        high_bucket: {"rows": 0, "solved": 0, "timeouts": 0, "max_width": 0, "max_table": 0},
        "Timeouts": {"rows": 0, "solved": 0, "timeouts": 0, "max_width": 0, "max_table": 0},
    }
    for record in records:
        status = str(record.get("status") or "ok")
        width = record.get("treewidth_width")
        table = record.get("treewidth_max_table_entries")
        if status == "timeout":
            bucket = buckets["Timeouts"]
        elif isinstance(width, int) and width <= threshold:
            bucket = buckets[low_bucket]
        else:
            bucket = buckets[high_bucket]
        bucket["rows"] += 1
        if status == "ok":
            bucket["solved"] += 1
        elif status == "timeout":
            bucket["timeouts"] += 1
        if isinstance(width, int):
            bucket["max_width"] = max(bucket["max_width"], width)
        if isinstance(table, int):
            bucket["max_table"] = max(bucket["max_table"], table)
    return threshold, [
        {"bucket": bucket, **values}
        for bucket, values in buckets.items()
        if values["rows"] > 0
    ]


def best_solver_configs(named_records: list[tuple[str, list[dict]]]) -> dict[str, str]:
    non_wmc = [
        (tier, [r for r in records if r.get("backend") != "wmc"])
        for tier, records in named_records
    ]
    rows = summarize_solver_records(non_wmc)
    best: dict[str, tuple[int, str]] = {}
    for row in rows:
        if row["ok"] == 0:
            continue
        current = best.get(row["tier"])
        score = (row["elapsed_ns"], row["config"])
        if current is None or score < current:
            best[row["tier"]] = score
    return {tier: config for tier, (_elapsed, config) in best.items()}


def write_benchmark_table(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    non_wmc = [
        (tier, [r for r in records if r.get("backend") != "wmc"])
        for tier, records in named_records
    ]
    rows = coverage_by_source(non_wmc)
    print("## Benchmarks Used\n", file=file)
    print(
        "Counts are fixed-boundary QSOP rows currently used in solver comparisons. "
        "The 257-512 column is an exploratory stratified sample and is shown as "
        "solved / attempted when timeouts remain.\n",
        file=file,
    )
    print(
        "| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | "
        "257-512 sample | QSOP modes |",
        file=file,
    )
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |", file=file)
    total_solved = 0
    total_attempted_large = 0
    total_solved_large = 0
    for source in sorted(rows, key=source_sort_key):
        entry = rows[source]
        solved = len(set().union(*entry["solved"].values())) if entry["solved"] else 0
        total_solved += solved
        total_attempted_large += len(entry["attempted"].get("257-512 sample", ()))
        total_solved_large += len(entry["solved"].get("257-512 sample", ()))
        print(
            f"| {markdown_escape(source)} | {markdown_escape(entry['url'] or SOURCE_URLS.get(source, ''))} | "
            f"{solved} | "
            f"{tier_cell(entry, '0-32')} | {tier_cell(entry, '33-64')} | "
            f"{tier_cell(entry, '65-128')} | {tier_cell(entry, '129-256')} | "
            f"{tier_cell(entry, '257-512 sample')} | {markdown_escape(mode_summary(entry))} |",
            file=file,
        )
    print(
        f"\nTotal current solved coverage: {total_solved} fixed-boundary benchmark rows.",
        file=file,
    )
    if total_attempted_large:
        print(
            f"The 257-512 exploratory sample contributes {total_solved_large} solved rows "
            f"out of {total_attempted_large} attempted under the current timeout cap.",
            file=file,
        )
    sample_rows = [record for tier, records in non_wmc for record in records if tier == "257-512 sample"]
    if sample_rows:
        print("\n## 257-512 Sample Stratification\n", file=file)
        threshold, rows = stratify_large_sample(sample_rows)
        print(
            f"Rows with treewidth width at most {threshold} are the current low-width promotion "
            f"candidates; timeouts remain the separate high-width residue.",
            file=file,
        )
        print("| Bucket | Rows | Solved | Timeouts | Max width | Max table |", file=file)
        print("| --- | ---: | ---: | ---: | ---: | ---: |", file=file)
        for row in rows:
            print(
                f"| {markdown_escape(row['bucket'])} | {row['rows']} | {row['solved']} | "
                f"{row['timeouts']} | {row['max_width']} | {row['max_table']} |",
                file=file,
            )


def write_solver_table(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    rows = summarize_solver_records(named_records)
    if not rows:
        return
    print("\n## Internal Solver Configurations\n", file=file)
    print(
        "Rows are grouped by imported-variable tier and sorted by total solve time. "
        "`Solved` is successful solver rows over attempted rows.\n",
        file=file,
    )
    print("| Tier | Configuration | Solved | Total solve time | Key stats |", file=file)
    print("| --- | --- | ---: | ---: | --- |", file=file)
    for row in sorted(rows, key=lambda item: (tier_sort_key(item["tier"]), item["elapsed_ns"], item["config"])):
        print(
            f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['config'])}` | "
            f"{row['ok']} / {row['records']} | {format_ns(row['elapsed_ns'])} | "
            f"{markdown_escape(public_solver_status_stats(row))} |",
            file=file,
        )


def write_competitor_tables(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    file: TextIO,
) -> None:
    # Pass all solver configs (see write_condensed_competitor_table): MQT-bench configs would
    # otherwise displace the dlx4sop configs that share boundaries with the native runs.
    rows = summarize_native_comparison_records(solver_records, native_records)
    if not rows:
        return
    print("\n## Competitor Comparisons\n", file=file)
    print(
        "These compare each current QSOP configuration against native QASM baselines on "
        "common rows, keeping the best speedup per source and tier. Each native tool is "
        "compared only on the QASM rows from that source that it can parse and fit under "
        "its cap. Speedup is native elapsed time divided by QSOP solve time, so values above "
        "`1.00x` mean QSOP is faster. Amplitude error columns use completed rows "
        "where both sides recorded amplitudes.\n",
        file=file,
    )
    for source in sorted({row["source"] for row in rows}, key=source_sort_key):
        print(f"### {markdown_escape(source)}\n", file=file)
        print(
            "| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | "
            "Native time | QSOP speedup | Amplitude checked | Mean amplitude error | "
            "Max amplitude error | "
            "Max boundary qubits | Qubit cap | Timeout | Memory cap |",
            file=file,
        )
        print(
            "| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
            file=file,
        )
        source_rows = [row for row in rows if row["source"] == source]
        source_rows.sort(key=lambda item: (tier_sort_key(item["tier"]), item["engine"]))
        for row in source_rows:
            qubit_cap = row["qubit_caps"].most_common(1)[0][0] if row["qubit_caps"] else "not recorded"
            timeout = row["timeouts"].most_common(1)[0][0] if row["timeouts"] else "not recorded"
            memory_cap = row["memory_caps"].most_common(1)[0][0] if row["memory_caps"] else "not recorded"
            mean_error = (
                row["amplitude_abs_error_sum"] / row["amplitude_checked"]
                if row["amplitude_checked"]
                else 0.0
            )
            print(
                f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['config'])}` | "
                f"`{markdown_escape(row['engine'])}` | {row['both_ok']} / {row['matched']} | "
                f"{format_ns(row['solver_elapsed_ns'])} | {format_ns(row['native_elapsed_ns'])} | "
                f"{comparison_speedup(row['native_elapsed_ns'], row['solver_elapsed_ns'])} | "
                f"{row['amplitude_checked']} | {mean_error:.3g} | {row['amplitude_max_abs_error']:.3g} | "
                f"{row['max_boundary_qubits']} | {markdown_escape(qubit_cap)} | "
                f"{markdown_escape(timeout)} | {markdown_escape(memory_cap)} |",
                file=file,
            )
        print("", file=file)


def write_takeaway(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    # Exclude the external MQT-bench corpus when picking the "best internal configuration":
    # its tiny GHZ/BV circuits solve in microseconds and would otherwise report a misleading
    # per-tier winner that does not reflect the dlx4sop corpus.
    internal_records = [
        (tier, [r for r in records if r.get("source") != "MQT Bench"])
        for tier, records in named_records
    ]
    best = best_solver_configs(internal_records)
    best_parts = [f"{tier}: `{best[tier]}`" for tier in SOLVER_TIERS if tier in best]
    non_wmc = [(tier, [r for r in records if r.get("backend") != "wmc"]) for tier, records in named_records]
    sample_rows = [record for tier, records in non_wmc for record in records if tier == "257-512 sample"]
    sample_ok = sum(1 for record in sample_rows if record.get("status", "ok") == "ok")
    sample_total = len(sample_rows)
    print("## Current Takeaway\n", file=file)
    if best_parts:
        print("Best current internal configurations by tier: " + "; ".join(best_parts) + ".", file=file)
    if sample_total:
        print(
            f"The 257-512 stratified sample is not a full tier yet: "
            f"{sample_ok} / {sample_total} rows solve under the current timeout cap.",
            file=file,
        )
    print(
        "Treewidth is the clean direct-DP baseline; hybrid branch is the best widened-tier "
        "configuration once component splitting and treewidth handoff trigger. Against native "
        "baselines, QSOP is consistently faster than the `pyzx-matrix` tool, while dense "
        "`aer-statevector` still wins on some low-width FeynmanDD rows.",
        file=file,
    )


def write_condensed_solver_table(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    rows = summarize_solver_records(named_records)
    print("## Internal Solver Configurations\n", file=file)
    print("Best configuration per tier at a glance.\n", file=file)
    print("| Tier | Configuration | Solved | Total solve time |", file=file)
    print("| --- | --- | ---: | ---: |", file=file)
    for row in sorted(rows, key=lambda item: (tier_sort_key(item["tier"]), item["elapsed_ns"], item["config"])):
        print(
            f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['config'])}` | "
            f"{row['ok']} / {row['records']} | {format_ns(row['elapsed_ns'])} |",
            file=file,
        )


def write_condensed_competitor_table(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    file: TextIO,
) -> None:
    # Pass all solver configs (not a single global best-per-tier): MQT-bench configs solve
    # tiny circuits in microseconds and would otherwise win the per-tier pick and displace the
    # dlx4sop configs that actually share boundaries with the native runs. The grouping below
    # already selects the best speedup per (source, tier).
    rows = summarize_native_comparison_records(solver_records, native_records)
    print("\n## Competitor Comparisons\n", file=file)
    comparison_rows = [row for row in rows if row["both_ok"] > 0 and row["solver_elapsed_ns"] > 0]
    if not comparison_rows:
        print(
            "No native baseline available for this mode under the current qubit caps and "
            "timeout — no boundary was solved by both a native simulator and the solver.\n",
            file=file,
        )
        return
    # Distinct solver-solved boundaries per (source, tier) — the denominator for coverage.
    qsop_solved: dict[tuple[str, str], set] = {}
    for tier, records in solver_records:
        for r in records:
            if r.get("status", "ok") == "ok" and has_comparison_identity(r):
                key = (str(r.get("source") or "unknown"), tier)
                qsop_solved.setdefault(key, set()).add(comparison_key(r))
    print(
        "Best native simulator per source and tier. Speedup = native time / QSOP time, so a "
        "value above 1 (**bold**) means QSOP is faster. Native runs only on boundaries it can "
        "fit under its qubit cap and finish in time; the **Matched / QSOP-solved** column shows "
        "on how many of the solver's rows that holds — a high speedup on a small matched set "
        "means QSOP also wins on coverage.\n",
        file=file,
    )
    for source in sorted({row["source"] for row in rows}, key=source_sort_key):
        print(f"### {markdown_escape(source)}\n", file=file)
        print("| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |", file=file)
        print("| --- | ---: | --- | ---: | ---: | ---: |", file=file)
        source_rows = [row for row in rows if row["source"] == source and row["both_ok"] > 0]
        by_tier: dict[str, dict] = {}
        for row in source_rows:
            if row["solver_elapsed_ns"] <= 0:
                continue
            tier = row["tier"]
            speedup_val = row["native_elapsed_ns"] / row["solver_elapsed_ns"]
            if tier not in by_tier or speedup_val > by_tier[tier]["_speedup"]:
                by_tier[tier] = {**row, "_speedup": speedup_val}
        for tier in sorted(by_tier, key=tier_sort_key):
            row = by_tier[tier]
            sp = row["_speedup"]
            speedup_str = f"**{sp:.2f}x**" if sp >= 1 else f"{sp:.2f}x"
            solved_total = len(qsop_solved.get((source, tier), ()))
            coverage = f"{row['both_ok']} / {solved_total}" if solved_total else str(row["both_ok"])
            print(
                f"| {markdown_escape(tier)} | {format_ns(row['solver_elapsed_ns'])} | "
                f"`{markdown_escape(row['engine'])}` | {format_ns(row['native_elapsed_ns'])} | "
                f"{speedup_str} | {coverage} |",
                file=file,
            )
        print("", file=file)


def _scaling_largest_solved(artifact_dir: pathlib.Path, stem: str, time_ns: str, time_ms: str) -> int | None:
    """Largest qubit count solved (status ok) for a scaling artifact."""
    import json
    import re
    path = artifact_dir / stem
    best = None
    if not path.exists():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if r.get("status") != "ok":
            continue
        name = str(r.get("instance_id") or r.get("instance") or r.get("case") or "")
        m = re.search(r"n0*(\d+)", name)
        if m:
            q = int(m.group(1))
            best = q if best is None else max(best, q)
    return best


def _scaling_median_by_qubits(artifact_dir: pathlib.Path, stem: str,
                              ns_key: str, ms_key: str) -> dict[int, float]:
    """Median ok solve time (ms) per qubit count for a scaling artifact."""
    import json
    import re
    import statistics
    per_q: dict[int, list[float]] = {}
    path = artifact_dir / stem
    if not path.exists():
        return {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        if r.get("status") != "ok":
            continue
        name = str(r.get("instance_id") or r.get("instance") or r.get("case") or "")
        m = re.search(r"n0*(\d+)", name)
        if not m:
            continue
        ms = r.get(ms_key)
        if ms is None and r.get(ns_key) is not None:
            ms = r[ns_key] / 1e6
        if ms is not None:
            per_q.setdefault(int(m.group(1)), []).append(float(ms))
    return {q: statistics.median(v) for q, v in per_q.items()}


def write_scaling_section(artifact_dir: pathlib.Path | None, assets_subdir: str, file: TextIO) -> None:
    """WMC-vs-solver scaling study section (synthetic phase-polynomial family)."""
    print("## WMC vs Solver Scaling\n", file=file)
    caption = (
        "Synthetic phase-polynomial circuits (committed under "
        "`benchmarks/corpus/sop/synthetic/scaling/`) whose QSOP treewidth grows with the "
        "qubit count. Real benchmark families cannot show this: the scalable MQT families use "
        "continuous-angle gates the finite-modulus importer rejects, and the importable ones "
        "are Clifford with trivial treewidth. As treewidth grows the branch backend collapses "
        "first."
    )
    if artifact_dir is not None:
        tw_by_q = _scaling_median_by_qubits(artifact_dir, "scaling-treewidth-current.jsonl",
                                            "solve_elapsed_ns", "solve_ms")
        gk_by_q = _scaling_median_by_qubits(artifact_dir, "scaling-wmc-current.jsonl",
                                            "wmc_ganak_elapsed_ns", "ganak_ms")
        shared = sorted(set(tw_by_q) & set(gk_by_q))
        win = next((q for q in shared if gk_by_q[q] < tw_by_q[q]), None)
        if win is not None:
            caption += (f" ganak (WMC) overtakes the treewidth DP at {win} qubits, before "
                        "both reach the timeout wall.")
        elif shared:
            caption += (" Across the sizes both solve, the treewidth DP stays ahead of ganak "
                        "(WMC) — the DP's lead narrows as treewidth grows, so any crossover lies "
                        "past the point where ganak itself stays tractable.")
    if artifact_dir is not None:
        tw = _scaling_largest_solved(artifact_dir, "scaling-treewidth-current.jsonl",
                                     "solve_elapsed_ns", "solve_ms")
        br = _scaling_largest_solved(artifact_dir, "scaling-branch-current.jsonl",
                                     "solve_elapsed_ns", "solve_ms")
        gk = _scaling_largest_solved(artifact_dir, "scaling-wmc-current.jsonl",
                                     "wmc_ganak_elapsed_ns", "ganak_ms")
        parts = [f"{lbl} {v}q" for lbl, v in (("branch", br), ("treewidth", tw), ("ganak", gk))
                 if v is not None]
        if parts:
            caption += (" Largest size solved under the current cap: "
                        + ", ".join(parts) + ".")
    print(caption + "\n", file=file)
    print(f"![WMC vs solver scaling]({assets_subdir}/wmc-vs-solver-scaling.svg)\n", file=file)


def write_mode_scoreboard(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    *,
    mode: str,
    assets_subdir: str,
    file: TextIO,
    timeout_note: str = "",
    artifact_dir: pathlib.Path | None = None,
) -> None:
    filtered_solver = [
        (tier, [r for r in records if record_qsop_mode(r) == mode])
        for tier, records in solver_records
    ]
    filtered_solver = [(t, rs) for t, rs in filtered_solver if rs]
    # Native records are NOT mode-filtered: qsop_mode is a structural property of the QSOP
    # file (it has a `q` line), whereas a native record only knows its boundary, so deriving
    # mode from input!=output disagrees with the solver's classification. The competitor table
    # matches native to the mode-filtered solver records by exact boundary identity and inherits
    # the solver's mode, which is the authoritative one.

    today = _datetime.date.today().isoformat()
    print(f"# Scoreboard — {mode} QSOPs\n", file=file)
    updated_line = f"Last updated: {today}."
    if timeout_note:
        updated_line += f" Per-instance timeout: {timeout_note}."
    print(updated_line + "\n", file=file)
    print(
        "This tracks progress toward a competitive exact strong simulator based on "
        "labelled quadratic SOPs. The current benchmark contract is fixed-boundary "
        "strong simulation: import a static circuit into QSOP, solve the exact "
        "residue-count histogram, and compare with native simulators where possible.\n",
        file=file,
    )

    non_wmc = [(t, [r for r in rs if r.get("backend") != "wmc"]) for t, rs in filtered_solver]
    non_wmc = [(t, rs) for t, rs in non_wmc if rs]
    rows = coverage_by_source(non_wmc)
    print("## Benchmarks\n", file=file)
    print(
        "Counts are fixed-boundary QSOP rows currently used in solver comparisons. "
        "The 257-512 column is an exploratory stratified sample and is shown as "
        "solved / attempted when timeouts remain.\n",
        file=file,
    )
    print(
        "| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |",
        file=file,
    )
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    total_solved = 0
    total_attempted_large = 0
    total_solved_large = 0
    for source in sorted(rows, key=source_sort_key):
        entry = rows[source]
        solved = len(set().union(*entry["solved"].values())) if entry["solved"] else 0
        total_solved += solved
        total_attempted_large += len(entry["attempted"].get("257-512 sample", ()))
        total_solved_large += len(entry["solved"].get("257-512 sample", ()))
        print(
            f"| {markdown_escape(source)} | {markdown_escape(entry['url'] or SOURCE_URLS.get(source, ''))} | "
            f"{solved} | "
            f"{tier_cell(entry, '0-32')} | {tier_cell(entry, '33-64')} | "
            f"{tier_cell(entry, '65-128')} | {tier_cell(entry, '129-256')} | "
            f"{tier_cell(entry, '257-512 sample')} |",
            file=file,
        )
    print(
        f"\nTotal current solved coverage: **{total_solved} fixed-boundary benchmark rows**.",
        file=file,
    )
    if total_attempted_large:
        print(
            f"The 257-512 exploratory sample contributes {total_solved_large} solved rows "
            f"out of {total_attempted_large} attempted under the current timeout cap.",
            file=file,
        )

    print("\n## Survival Curves\n", file=file)
    print(
        "Fraction of instances solved within a given wall-clock budget per backend. "
        "Higher and further left is better.\n",
        file=file,
    )
    print("### FeynmanDD\n", file=file)
    print(f"![Survival curves — FeynmanDD]({assets_subdir}/survival-feynmandd.svg)\n", file=file)
    print("### MQT Bench (small, ≤32 qubits)\n", file=file)
    print(
        "Pre-expansion set: circuits with at most 32 qubits, compared against the best native "
        "simulator that fits each boundary under its qubit cap.\n",
        file=file,
    )
    print(f"![Survival curves — MQT Bench (0-32 tier)]({assets_subdir}/survival-mqt-bench.svg)\n", file=file)
    print("### MQT Bench (large, 34–128 qubits)\n", file=file)
    print(
        "Expanded set: GHZ and BV circuits at 34–128 qubits. The native baseline is "
        "`qiskit-clifford` (stabilizer formalism, O(n²) memory) because statevector engines "
        "were killed or timed out at 34+ qubits (34-qubit statevector ≈ 272 GB). This plot "
        "is regenerated with the rest of the scoreboard when new QSOP and native artifacts "
        "are available.\n",
        file=file,
    )
    print(
        f"![Survival curves — MQT Bench (33-64 and 65-128 tiers)]"
        f"({assets_subdir}/survival-mqt-bench-large.svg)\n",
        file=file,
    )
    print("### PyZX\n", file=file)
    print(f"![Survival curves — PyZX]({assets_subdir}/survival-pyzx.svg)\n", file=file)

    print("## Solver Time by Tier\n", file=file)
    print("Median solve time per tier, log scale. Only `ok` rows counted.\n", file=file)
    print(f"![Solver time by tier]({assets_subdir}/solver-time-by-tier.svg)\n", file=file)

    print("## Speedup vs Treewidth Baseline\n", file=file)
    print(
        "Speedup of each backend relative to treewidth on matched pairs. "
        "Bars above 1.0x mean the backend is faster.\n",
        file=file,
    )
    print(f"![Speedup vs treewidth]({assets_subdir}/solver-speedup-vs-treewidth.svg)\n", file=file)

    print("## Branch Dispatch\n", file=file)
    print(
        "Fraction of branch-solver calls dispatched to treewidth sub-solver, rankwidth "
        "sub-solver, or pure-branch fallthrough per tier.\n",
        file=file,
    )
    print(f"![Branch dispatch by tier]({assets_subdir}/branch-dispatch-by-tier.svg)\n", file=file)

    print("## WMC Solve Time Breakdown\n", file=file)
    print("Export time vs Ganak time per WMC encoding and tier.\n", file=file)
    print(f"![WMC time breakdown]({assets_subdir}/wmc-time-breakdown.svg)\n", file=file)

    # Scaling study is mode-agnostic (synthetic sign family); show it on the sign scoreboard.
    if mode == "sign":
        write_scaling_section(artifact_dir, assets_subdir, file)

    write_condensed_solver_table(filtered_solver, file)
    write_condensed_competitor_table(filtered_solver, native_records, file)
    write_takeaway(filtered_solver, file)


def write_index(solver_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    today = _datetime.date.today().isoformat()
    print("# Scoreboard\n", file=file)
    print(f"Last updated: {today}.\n", file=file)
    print(
        "This tracks progress toward a competitive exact strong simulator based on "
        "labelled quadratic SOPs. The current benchmark contract is fixed-boundary "
        "strong simulation: import a static circuit into QSOP, solve the exact "
        "residue-count histogram, and compare with native simulators where possible.\n",
        file=file,
    )

    non_wmc = [
        (tier, [r for r in records if r.get("backend") != "wmc"])
        for tier, records in solver_records
    ]
    non_wmc = [(t, rs) for t, rs in non_wmc if rs]
    rows = coverage_by_source(non_wmc)

    total_solved = sum(
        len(set().union(*entry["solved"].values())) if entry["solved"] else 0
        for entry in rows.values()
    )
    print(f"Total current solved coverage: **{total_solved} fixed-boundary benchmark rows**.\n", file=file)

    print("| Source | Total solved | Sign | Labelled |", file=file)
    print("| --- | ---: | ---: | ---: |", file=file)
    for source in sorted(rows, key=source_sort_key):
        entry = rows[source]
        solved = len(set().union(*entry["solved"].values())) if entry["solved"] else 0
        sign_count = len(entry["modes"].get("sign", set()))
        labelled_count = len(entry["modes"].get("labelled", set()))
        print(
            f"| {markdown_escape(source)} | {solved} | {sign_count} | {labelled_count} |",
            file=file,
        )

    print("", file=file)
    print("- [Sign QSOP scoreboard](scoreboard-sign.md)", file=file)
    print("- [Labelled QSOP scoreboard](scoreboard-labelled.md)", file=file)


def write_scoreboard(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    file: TextIO,
    rankwidth_comparison_records: list[dict] | None = None,
    rankwidth_comparison_top: int = 10,
) -> None:
    today = _datetime.date.today().isoformat()
    print("# Scoreboard\n", file=file)
    print(f"Last updated: {today}.\n", file=file)
    print(
        "This tracks progress toward a competitive exact strong simulator based on "
        "labelled quadratic SOPs. The current benchmark contract is fixed-boundary "
        "strong simulation: import a static circuit into QSOP, solve the exact "
        "residue-count histogram, and compare with native simulators where possible.\n",
        file=file,
    )
    write_benchmark_table(solver_records, file)
    write_solver_table(solver_records, file)
    if rankwidth_comparison_records:
        print("", file=file)
        compare_rankwidth_backends.write_markdown(
            rankwidth_comparison_records,
            "all",
            rankwidth_comparison_top,
            file,
            heading_level=2,
        )
    write_competitor_tables(solver_records, native_records, file)
    write_takeaway(solver_records, file)


def read_rankwidth_comparison_records(
    named_paths: Iterable[tuple[str, pathlib.Path]], allow_missing: bool
) -> list[dict]:
    records = []
    for tier, path in named_paths:
        if not path.exists():
            if allow_missing:
                print(f"warning: missing artifact {path}", file=sys.stderr)
                continue
            raise RuntimeError(f"missing artifact {path}")
        for record in read_jsonl(path):
            copied = dict(record)
            copied["_tier"] = tier
            records.append(copied)
    return records


def run_to_jsonl(command: list[str], output: pathlib.Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(command, stdout=stream, stderr=subprocess.PIPE, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed with status {completed.returncode}: {' '.join(command)}\n"
            f"{completed.stderr}"
        )


def run_selected_jobs(args: argparse.Namespace) -> None:
    repo = pathlib.Path(__file__).resolve().parents[1]
    artifact_dir = args.artifact_dir
    if args.run_native:
        native_tool = repo / "tools" / "bench_qasm_native_simulator.py"
        for tier in NATIVE_TIERS:
            manifest = artifact_dir / f"dlx4sop-tier-{tier}-manifest.json"
            if not manifest.exists():
                if args.allow_missing:
                    print(f"warning: missing manifest {manifest}", file=sys.stderr)
                    continue
                raise RuntimeError(f"missing manifest {manifest}")
            output = artifact_dir / f"dlx4sop-tier-{tier}-native-all-current.jsonl"
            command = [
                str(native_tool),
                str(manifest),
                "--engine",
                "all",
                "--max-qubits",
                str(args.native_max_qubits),
                "--engine-qubit-cap",
                f"pyzx-matrix={args.pyzx_matrix_max_qubits}",
                "--timeout",
                str(args.timeout),
                "--memory-limit-mib",
                str(args.memory_limit_mib),
                "--skip-unsupported",
                "--format",
                "jsonl",
            ]
            run_to_jsonl(command, output)
    if args.run_large_sample:
        manifest = artifact_dir / "dlx4sop-tier-257-512-sample-manifest.json"
        if not manifest.exists():
            if args.allow_missing:
                print(f"warning: missing manifest {manifest}", file=sys.stderr)
                return
            raise RuntimeError(f"missing manifest {manifest}")
        output = artifact_dir / "dlx4sop-tier-257-512-sample-treewidth-current.jsonl"
        command = [
            str(repo / "tools" / "bench_qasm_corpus.py"),
            str(args.qasm2sop),
            str(args.sop_solve),
            "--manifest",
            str(manifest),
            "--backend",
            "treewidth",
            "--treewidth-order",
            "min-fill-max-degree",
            "--max-vars",
            "512",
            "--solver-timeout",
            str(args.large_sample_timeout),
            "--trace",
            "--format",
            "jsonl",
        ]
        run_to_jsonl(command, output)


def _write_scoreboard_json(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    path: pathlib.Path,
    *,
    mode: str = "all",
) -> None:
    """Write normalized scoreboard intermediate JSON for plot generation."""
    import json as _json
    import datetime as _dt

    if mode != "all":
        solver_records = [
            (tier, [r for r in records if record_qsop_mode(r) == mode])
            for tier, records in solver_records
        ]
        solver_records = [(t, rs) for t, rs in solver_records if rs]

    tiers = list(SOLVER_TIERS)
    solver_summary = []
    summarized = summarize_solver_records(solver_records)
    for entry in summarized:
        config = entry.get("config", "")
        backend_field = entry.get("config", "").split(" ")[0] if config else ""
        solver_summary.append({
            "tier": entry.get("tier", ""),
            "config": config,
            "backend": backend_field,
            "solved": entry.get("ok", 0),
            "attempted": entry.get("records", 0),
            "elapsed_ns": entry.get("elapsed_ns", 0),
            "sources": dict(entry.get("sources", {})),
            "stats": entry.get("stats", {}),
        })

    out = {
        "generated_at": _dt.datetime.now().isoformat(),
        "tiers": tiers,
        "solver_summary": solver_summary,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_json.dumps(out, indent=2), encoding="utf-8")
    print(f"scoreboard JSON written to {path}", file=sys.stderr)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Refresh the public benchmark scoreboard.")
    parser.add_argument("--artifact-dir", type=pathlib.Path, default=pathlib.Path("/tmp"))
    parser.add_argument("--solver-jsonl", action="append", type=labelled_path, default=[], metavar="TIER=PATH")
    parser.add_argument("--native-jsonl", action="append", type=labelled_path, default=[], metavar="TIER=PATH")
    parser.add_argument(
        "--rankwidth-comparison-jsonl",
        action="append",
        type=labelled_path,
        default=[],
        metavar="TIER=PATH",
        help="JSONL produced by bench_qasm_corpus.py --rankwidth-comparison.",
    )
    parser.add_argument("--no-default-artifacts", action="store_true")
    parser.add_argument("--allow-missing", action="store_true")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("scoreboard.md"))
    parser.add_argument("--json", type=pathlib.Path, default=None, dest="json_out",
                        help="Write normalized scoreboard intermediate JSON to this path")
    parser.add_argument(
        "--qsop-mode",
        choices=("sign", "labelled"),
        default=None,
        help="Filter records to this QSOP mode and emit a mode scoreboard",
    )
    parser.add_argument(
        "--assets-subdir",
        type=str,
        default=None,
        help="Subdirectory prefix for SVG image paths (e.g. scoreboard-assets/sign)",
    )
    parser.add_argument(
        "--index",
        action="store_true",
        help="Emit the combined index scoreboard instead of a mode scoreboard",
    )
    parser.add_argument(
        "--rankwidth-comparison-output",
        type=pathlib.Path,
        help="Optional markdown report comparing rankwidth against treewidth and branch.",
    )
    parser.add_argument(
        "--rankwidth-comparison-qsop-mode",
        choices=("all", "sign", "labelled"),
        default="all",
    )
    parser.add_argument("--rankwidth-comparison-top", type=int, default=10)
    parser.add_argument("--run-native", action="store_true", help="rerun capped native simulator jobs")
    parser.add_argument("--run-large-sample", action="store_true", help="rerun the 257-512 treewidth sample")
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=pathlib.Path("build/qasm2sop"))
    parser.add_argument("--sop-solve", type=pathlib.Path, default=pathlib.Path("build/sop-solve"))
    parser.add_argument("--native-max-qubits", type=int, default=16)
    parser.add_argument("--pyzx-matrix-max-qubits", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--memory-limit-mib", type=int, default=4096)
    parser.add_argument("--large-sample-timeout", type=float, default=3.0)
    parser.add_argument("--timeout-note", default="", metavar="TEXT",
                        help="note the per-instance timeout in the mode scoreboard header (e.g. '120 s')")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.rankwidth_comparison_top < 0:
        print("error: --rankwidth-comparison-top must be non-negative", file=sys.stderr)
        return 2
    try:
        run_selected_jobs(args)
        solver_paths = [] if args.no_default_artifacts else default_solver_jsonl(args.artifact_dir)
        native_paths = [] if args.no_default_artifacts else default_native_jsonl(args.artifact_dir)
        solver_paths.extend(args.solver_jsonl)
        native_paths.extend(args.native_jsonl)
        solver_records = read_named_jsonl(solver_paths, args.allow_missing)
        native_records = read_named_jsonl(native_paths, args.allow_missing)

        if args.index:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("w", encoding="utf-8") as output:
                write_index(solver_records, output)
            return 0

        if args.qsop_mode is not None:
            assets_subdir = args.assets_subdir or f"scoreboard-assets/{args.qsop_mode}"
            args.output.parent.mkdir(parents=True, exist_ok=True)
            with args.output.open("w", encoding="utf-8") as output:
                write_mode_scoreboard(
                    solver_records,
                    native_records,
                    mode=args.qsop_mode,
                    assets_subdir=assets_subdir,
                    file=output,
                    timeout_note=getattr(args, "timeout_note", ""),
                    artifact_dir=args.artifact_dir,
                )
            if args.json_out is not None:
                _write_scoreboard_json(
                    solver_records, native_records, args.json_out, mode=args.qsop_mode
                )
            return 0

        rankwidth_records = read_rankwidth_comparison_records(
            args.rankwidth_comparison_jsonl,
            args.allow_missing,
        )
        if args.rankwidth_comparison_qsop_mode != "all":
            rankwidth_records = [
                record
                for record in rankwidth_records
                if record.get("qsop_mode") == args.rankwidth_comparison_qsop_mode
            ]
        with args.output.open("w", encoding="utf-8") as output:
            write_scoreboard(
                solver_records,
                native_records,
                output,
                rankwidth_comparison_records=rankwidth_records,
                rankwidth_comparison_top=args.rankwidth_comparison_top,
            )
        if args.json_out is not None:
            _write_scoreboard_json(solver_records, native_records, args.json_out)
        if args.rankwidth_comparison_output is not None:
            with args.rankwidth_comparison_output.open("w", encoding="utf-8") as output:
                compare_rankwidth_backends.write_markdown(
                    rankwidth_records,
                    "rankwidth:best:count-table",
                    args.rankwidth_comparison_top,
                    output,
                )
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
