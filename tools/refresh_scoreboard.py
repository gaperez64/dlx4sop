#!/usr/bin/env python3

import argparse
import collections
import datetime as _datetime
import pathlib
import subprocess
import sys
from typing import Iterable, TextIO

from render_scoreboard import (
    comparison_speedup,
    format_ns,
    has_comparison_identity,
    labelled_path,
    markdown_escape,
    read_jsonl,
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
NATIVE_TIERS = ("33-64", "65-128", "129-256")

DEFAULT_SOLVER_ARTIFACTS = (
    ("0-32", "dlx4sop-tier-0-32-treewidth-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-rankwidth-current.jsonl"),
    ("0-32", "dlx4sop-tier-0-32-branch-hybrid-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-treewidth-fresh.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-branch-hybrid-current.jsonl"),
    ("33-64", "dlx4sop-tier-33-64-rankwidth-min-fill-cut-current.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-treewidth-fresh.jsonl"),
    ("65-128", "dlx4sop-tier-65-128-branch-hybrid-fresh.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-branch-hybrid-current.jsonl"),
    ("129-256", "dlx4sop-tier-129-256-treewidth-current.jsonl"),
    ("257-512 sample", "dlx4sop-tier-257-512-sample-treewidth-current.jsonl"),
)

DEFAULT_NATIVE_ARTIFACTS = tuple(
    (tier, f"dlx4sop-tier-{tier}-native-all-current.jsonl") for tier in NATIVE_TIERS
)


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
    return str(record.get("qsop_mode") or record.get("mode") or "unknown")


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
    if "search_nodes" in stats:
        parts.append(f"{format_count(stats['search_nodes'])} nodes")
    if stats.get("cache_avoided_nodes", 0):
        parts.append(f"cache avoided nodes={format_count(stats['cache_avoided_nodes'])}")
    if "rankwidth_width" in stats:
        parts.append(f"rw width {stats['rankwidth_width']}")
    if "treewidth_width" in stats:
        parts.append(f"tw width {stats['treewidth_width']}")
    table = max(
        stats.get("rankwidth_max_table_entries", 0),
        stats.get("treewidth_max_table_entries", 0),
        stats.get("max_table_entries", 0),
    )
    if table:
        parts.append(f"max table {format_count(table)}")
    signatures = max(
        stats.get("rankwidth_max_signature_entries", 0),
        stats.get("max_signature_entries", 0),
    )
    if signatures:
        parts.append(f"max signatures {format_count(signatures)}")
    if "join_pairs" in stats:
        parts.append(f"{format_count(stats['join_pairs'])} join pairs")
    if "treewidth_delegations" in stats or "rankwidth_delegations" in stats:
        parts.append(
            f"delegations tw={format_count(stats.get('treewidth_delegations', 0))}, "
            f"rw={format_count(stats.get('rankwidth_delegations', 0))}"
        )
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


def best_solver_configs(named_records: list[tuple[str, list[dict]]]) -> dict[str, str]:
    rows = summarize_solver_records(named_records)
    best: dict[str, tuple[int, str]] = {}
    for row in rows:
        if row["ok"] == 0:
            continue
        current = best.get(row["tier"])
        score = (row["elapsed_ns"], row["config"])
        if current is None or score < current:
            best[row["tier"]] = score
    return {tier: config for tier, (_elapsed, config) in best.items()}


def filter_records_by_config(
    named_records: list[tuple[str, list[dict]]], selected: dict[str, str]
) -> list[tuple[str, list[dict]]]:
    filtered = []
    for tier, records in named_records:
        wanted = selected.get(tier)
        if wanted is None:
            continue
        kept = [record for record in records if solver_config(record) == wanted]
        if kept:
            filtered.append((tier, kept))
    return filtered


def write_benchmark_table(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    rows = coverage_by_source(named_records)
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
    best_records = filter_records_by_config(solver_records, best_solver_configs(solver_records))
    rows = summarize_native_comparison_records(best_records, native_records)
    if not rows:
        return
    print("\n## Competitor Comparisons\n", file=file)
    print(
        "These compare the best current QSOP configuration for each tier against "
        "native QASM baselines on common rows. Until `sop2X` exporters exist, each "
        "native tool is compared only on the QASM rows from that source that it can "
        "parse and fit under its cap. Speedup is native elapsed time divided by "
        "QSOP solve time, so values above `1.00x` mean QSOP is faster.\n",
        file=file,
    )
    for source in sorted({row["source"] for row in rows}, key=source_sort_key):
        print(f"### {markdown_escape(source)}\n", file=file)
        print(
            "| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | "
            "Native time | QSOP speedup | Max boundary qubits | Qubit cap | Timeout | Memory cap |",
            file=file,
        )
        print("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
        source_rows = [row for row in rows if row["source"] == source]
        source_rows.sort(key=lambda item: (tier_sort_key(item["tier"]), item["engine"]))
        for row in source_rows:
            qubit_cap = row["qubit_caps"].most_common(1)[0][0] if row["qubit_caps"] else "not recorded"
            timeout = row["timeouts"].most_common(1)[0][0] if row["timeouts"] else "not recorded"
            memory_cap = row["memory_caps"].most_common(1)[0][0] if row["memory_caps"] else "not recorded"
            print(
                f"| {markdown_escape(row['tier'])} | `{markdown_escape(row['config'])}` | "
                f"`{markdown_escape(row['engine'])}` | {row['both_ok']} / {row['matched']} | "
                f"{format_ns(row['solver_elapsed_ns'])} | {format_ns(row['native_elapsed_ns'])} | "
                f"{comparison_speedup(row['native_elapsed_ns'], row['solver_elapsed_ns'])} | "
                f"{row['max_boundary_qubits']} | {markdown_escape(qubit_cap)} | "
                f"{markdown_escape(timeout)} | {markdown_escape(memory_cap)} |",
                file=file,
            )
        print("", file=file)


def write_takeaway(named_records: list[tuple[str, list[dict]]], file: TextIO) -> None:
    best = best_solver_configs(named_records)
    best_parts = [f"{tier}: `{best[tier]}`" for tier in SOLVER_TIERS if tier in best]
    sample_rows = [record for tier, records in named_records for record in records if tier == "257-512 sample"]
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
        "Treewidth remains the clean direct-DP baseline. Hybrid branch is the best "
        "current widened-tier configuration when component splitting and treewidth "
        "handoff trigger. Native comparisons are now capped and source-local; dense "
        "statevector tools can still win on low-qubit rows, while QSOP remains strong "
        "on many fixed-boundary rows with large imported variable counts.",
        file=file,
    )


def write_scoreboard(
    solver_records: list[tuple[str, list[dict]]],
    native_records: list[tuple[str, list[dict]]],
    file: TextIO,
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
    write_competitor_tables(solver_records, native_records, file)
    write_takeaway(solver_records, file)


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


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Refresh the public benchmark scoreboard.")
    parser.add_argument("--artifact-dir", type=pathlib.Path, default=pathlib.Path("/tmp"))
    parser.add_argument("--solver-jsonl", action="append", type=labelled_path, default=[], metavar="TIER=PATH")
    parser.add_argument("--native-jsonl", action="append", type=labelled_path, default=[], metavar="TIER=PATH")
    parser.add_argument("--no-default-artifacts", action="store_true")
    parser.add_argument("--allow-missing", action="store_true")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("scoreboard.md"))
    parser.add_argument("--run-native", action="store_true", help="rerun capped native simulator jobs")
    parser.add_argument("--run-large-sample", action="store_true", help="rerun the 257-512 treewidth sample")
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=pathlib.Path("build/qasm2sop"))
    parser.add_argument("--sop-solve", type=pathlib.Path, default=pathlib.Path("build/sop-solve"))
    parser.add_argument("--native-max-qubits", type=int, default=16)
    parser.add_argument("--pyzx-matrix-max-qubits", type=int, default=10)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--memory-limit-mib", type=int, default=4096)
    parser.add_argument("--large-sample-timeout", type=float, default=3.0)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        run_selected_jobs(args)
        solver_paths = [] if args.no_default_artifacts else default_solver_jsonl(args.artifact_dir)
        native_paths = [] if args.no_default_artifacts else default_native_jsonl(args.artifact_dir)
        solver_paths.extend(args.solver_jsonl)
        native_paths.extend(args.native_jsonl)
        solver_records = read_named_jsonl(solver_paths, args.allow_missing)
        native_records = read_named_jsonl(native_paths, args.allow_missing)
        with args.output.open("w", encoding="utf-8") as output:
            write_scoreboard(solver_records, native_records, output)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
