#!/usr/bin/env python3

import argparse
import csv
import hashlib
import json
import pathlib
import subprocess
import sys
import time
from typing import TextIO


BACKENDS = ("components", "brute-force", "branch", "rankwidth", "treewidth")
DEFAULT_BACKENDS = ("components", "brute-force", "branch")
BRANCH_HEURISTICS = ("split", "treewidth", "cutrank-proxy")
RANKWIDTH_GENERATORS = ("linear", "balanced", "min-fill", "min-fill-cut")
RANKWIDTH_MODES = ("count-table", "fourier")
TREEWIDTH_ORDERS = ("min-fill", "min-degree", "min-fill-max-degree")
BACKEND_ALIAS_METRICS = (
    "rankwidth_width",
    "treewidth_width",
    "rankwidth_table_entries",
    "rankwidth_max_table_entries",
    "treewidth_table_entries",
    "treewidth_max_table_entries",
    "rankwidth_signature_entries",
    "rankwidth_max_signature_entries",
)
TOP_METRICS = (
    "solve_elapsed_ns",
    "import_elapsed_ns",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "components",
    "decomposition_width",
    "rankwidth_width",
    "treewidth_width",
    "table_entries",
    "max_table_entries",
    "rankwidth_table_entries",
    "rankwidth_max_table_entries",
    "treewidth_table_entries",
    "treewidth_max_table_entries",
    "signature_entries",
    "max_signature_entries",
    "rankwidth_signature_entries",
    "rankwidth_max_signature_entries",
    "join_pairs",
    "join_signature_pairs",
)
CSV_FIELDS = [
    "case",
    "source",
    "source_url",
    "source_relative_path",
    "boundary",
    "input",
    "output",
    "backend",
    "branch_heuristic",
    "rankwidth_mode",
    "rankwidth_decomposition",
    "treewidth_order",
    "qsop_mode",
    "r",
    "nvars",
    "nedges",
    "import_elapsed_ns",
    "solve_elapsed_ns",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "components",
    "decomposition_width",
    "rankwidth_width",
    "treewidth_width",
    "table_entries",
    "max_table_entries",
    "rankwidth_table_entries",
    "rankwidth_max_table_entries",
    "treewidth_table_entries",
    "treewidth_max_table_entries",
    "signature_entries",
    "max_signature_entries",
    "rankwidth_signature_entries",
    "rankwidth_max_signature_entries",
    "join_pairs",
    "join_signature_pairs",
    "qasm_sha256",
    "qsop_sha256",
    "trace_summary",
]


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def run_command(cmd: list[str], *, input_text: str | None = None) -> tuple[str, str, int]:
    start = time.perf_counter_ns()
    completed = subprocess.run(
        cmd,
        input=input_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.perf_counter_ns() - start
    if completed.returncode != 0:
        raise RuntimeError(f"command failed: {cmd}\n{completed.stderr}")
    return completed.stdout, completed.stderr, elapsed


def is_skippable_rankwidth_error(error: Exception) -> bool:
    text = str(error)
    return (
        "sign-only" in text
        or "requires at least one variable" in text
        or "could not find a 64-bit NTT prime" in text
    )


def load_cases(path: pathlib.Path) -> list[dict]:
    return json.loads(path.read_text())


def case_qasm(case: dict) -> str:
    return "\n".join(case["qasm_lines"]) + "\n"


def qsop_header(qsop: str) -> dict[str, int | str]:
    header: dict[str, int | str] | None = None
    mode = "sign"
    for line in qsop.splitlines():
        parts = line.split()
        if len(parts) == 5 and parts[:2] == ["p", "qsop"]:
            header = {
                "r": int(parts[2]),
                "nvars": int(parts[3]),
                "nedges": int(parts[4]),
            }
            continue
        if parts and parts[0] == "q":
            mode = "labelled"
    if header is None:
        raise RuntimeError(f"missing QSOP header:\n{qsop}")
    header["qsop_mode"] = mode
    return header


def parse_stats(text: str) -> dict[str, int | str]:
    stats: dict[str, int | str] = {}
    for line in text.splitlines():
        if not line:
            continue
        key, value = line.split(": ", 1)
        stats[key] = (
            value
            if key
            in {
                "backend",
                "branch_heuristic",
                "rankwidth_mode",
                "rankwidth_decomposition",
                "treewidth_order",
            }
            else int(value)
        )
    return stats


def backend_stat_aliases(backend: str, stats: dict[str, int | str]) -> dict[str, int | str]:
    aliases: dict[str, int | str] = {}
    if backend == "rankwidth":
        mapping = {
            "decomposition_width": "rankwidth_width",
            "table_entries": "rankwidth_table_entries",
            "max_table_entries": "rankwidth_max_table_entries",
            "signature_entries": "rankwidth_signature_entries",
            "max_signature_entries": "rankwidth_max_signature_entries",
        }
    elif backend == "treewidth":
        mapping = {
            "decomposition_width": "treewidth_width",
            "table_entries": "treewidth_table_entries",
            "max_table_entries": "treewidth_max_table_entries",
        }
    else:
        return aliases

    for source, target in mapping.items():
        value = stats.get(source)
        if isinstance(value, int):
            aliases[target] = value
    return aliases


def parse_trace_csv(text: str) -> dict[str, dict[str, int]]:
    rows = [line for line in text.splitlines() if line]
    if not rows:
        return {}
    if rows[0] != "phase,depth,items,elapsed_ns":
        raise RuntimeError(f"unexpected trace header:\n{text}")

    summary: dict[str, dict[str, int]] = {}
    for row in rows[1:]:
        phase, _depth, items, elapsed_ns = row.split(",", 3)
        entry = summary.setdefault(phase, {"events": 0, "items": 0, "elapsed_ns": 0})
        entry["events"] += 1
        entry["items"] += int(items)
        entry["elapsed_ns"] += int(elapsed_ns)
    return summary


def trace_summary_text(trace: dict[str, dict[str, int]]) -> str:
    return ";".join(
        f"{phase}:{values['events']}:{values['elapsed_ns']}" for phase, values in sorted(trace.items())
    )


def add_counter(total: dict[str, int], key: str, value: int | str | None) -> None:
    if isinstance(value, int):
        total[key] = total.get(key, 0) + value


def add_stat(total: dict[str, int], key: str, value: int | str | None) -> None:
    if not isinstance(value, int):
        return
    if key in {
        "decomposition_width",
        "rankwidth_width",
        "treewidth_width",
        "max_table_entries",
        "rankwidth_max_table_entries",
        "treewidth_max_table_entries",
        "max_signature_entries",
        "rankwidth_max_signature_entries",
    }:
        total[key] = max(total.get(key, 0), value)
    else:
        add_counter(total, key, value)


def record_summary_key(record: dict) -> tuple[str, str, str, str]:
    return (
        record["backend"],
        record["branch_heuristic"],
        f"{record['rankwidth_decomposition']}:{record['rankwidth_mode']}",
        record["treewidth_order"],
    )


def format_summary_key(key: tuple[str, str, str, str]) -> list[str]:
    backend, branch_heuristic, rankwidth_config, treewidth_order = key
    lines = [f"backend: {backend}"]
    if backend == "branch" and branch_heuristic:
        lines.append(f"  branch_heuristic: {branch_heuristic}")
    if backend == "rankwidth" and rankwidth_config != ":":
        decomposition, mode = rankwidth_config.split(":", 1)
        lines.append(f"  rankwidth_decomposition: {decomposition}")
        lines.append(f"  rankwidth_mode: {mode}")
    if backend == "treewidth" and treewidth_order:
        lines.append(f"  treewidth_order: {treewidth_order}")
    return lines


def summarize_records(records: list[dict]) -> dict[tuple[str, str, str, str], dict]:
    summary: dict[tuple[str, str, str, str], dict] = {}
    for record in records:
        key = record_summary_key(record)
        entry = summary.setdefault(
            key,
            {
                "records": 0,
                "solve_elapsed_ns": 0,
                "import_elapsed_ns": 0,
                "stats": {},
                "trace": {},
            },
        )
        entry["records"] += 1
        entry["solve_elapsed_ns"] += record["solve_elapsed_ns"]
        entry["import_elapsed_ns"] += record["import_elapsed_ns"]

        stats_total = entry["stats"]
        for stat_key, value in record["stats"].items():
            add_stat(stats_total, stat_key, value)
        for stat_key in BACKEND_ALIAS_METRICS:
            add_stat(stats_total, stat_key, record.get(stat_key))

        trace_total = entry["trace"]
        for phase, values in record["trace"].items():
            phase_total = trace_total.setdefault(phase, {"events": 0, "items": 0, "elapsed_ns": 0})
            phase_total["events"] += values["events"]
            phase_total["items"] += values["items"]
            phase_total["elapsed_ns"] += values["elapsed_ns"]
    return summary


def cache_hit_rate(stats: dict[str, int]) -> str:
    hits = stats.get("cache_hits", 0)
    misses = stats.get("cache_misses", 0)
    total = hits + misses
    if total == 0:
        return "n/a"
    return f"{hits / total:.3f}"


def record_metric(record: dict, metric: str) -> int:
    value = record.get(metric)
    if isinstance(value, int):
        return value
    value = record["stats"].get(metric)
    return value if isinstance(value, int) else 0


def record_has_metric(record: dict, metric: str) -> bool:
    return isinstance(record.get(metric), int) or isinstance(record["stats"].get(metric), int)


def dominant_trace_phase(record: dict) -> str:
    trace = record["trace"]
    if not trace:
        return ""
    phase, values = max(trace.items(), key=lambda item: (item[1]["elapsed_ns"], item[0]))
    return (
        f"{phase}:events={values['events']}:items={values['items']}:"
        f"elapsed_ns={values['elapsed_ns']}"
    )


def iter_case_boundaries(cases: list[dict], limit: int | None):
    seen = 0
    for case in cases:
        qasm = case_qasm(case)
        for input_bits, output_bits in case["boundaries"]:
            if limit is not None and seen >= limit:
                return
            seen += 1
            yield case, qasm, input_bits, output_bits


def iter_backend_configs(args: argparse.Namespace, backend: str):
    if backend == "rankwidth":
        for generator in args.rankwidth_generators:
            for mode in args.rankwidth_modes:
                yield {
                    "rankwidth_generate": generator,
                    "rankwidth_mode": mode,
                    "branch_heuristic": "",
                    "treewidth_order": "",
                }
        return
    if backend == "treewidth":
        for order in args.treewidth_orders:
            yield {
                "rankwidth_generate": "",
                "rankwidth_mode": "",
                "branch_heuristic": "",
                "treewidth_order": order,
            }
        return

    yield {
        "rankwidth_generate": "",
        "rankwidth_mode": "",
        "branch_heuristic": args.branch_heuristic if backend == "branch" else "",
        "treewidth_order": "",
    }


def benchmark(args: argparse.Namespace) -> tuple[list[dict], dict]:
    cases = load_cases(args.manifest)
    backends = args.backends or list(DEFAULT_BACKENDS)
    records: list[dict] = []
    metadata = {
        "case_boundaries": 0,
        "imported_sign": 0,
        "imported_labelled": 0,
        "skipped_rankwidth_records": 0,
        "source_boundaries": {},
    }

    for case_data, qasm, input_bits, output_bits in iter_case_boundaries(cases, args.limit):
        case_name = case_data["name"]
        source = case_data.get("source", "internal")
        source_url = case_data.get("source_url", "")
        source_relative_path = case_data.get("source_relative_path", "")
        qsop, _stderr, import_elapsed_ns = run_command(
            [str(args.qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
            input_text=qasm,
        )
        header = qsop_header(qsop)
        metadata["case_boundaries"] += 1
        metadata["source_boundaries"][source] = metadata["source_boundaries"].get(source, 0) + 1
        if header["qsop_mode"] == "labelled":
            metadata["imported_labelled"] += 1
        else:
            metadata["imported_sign"] += 1
        for backend in backends:
            for config in iter_backend_configs(args, backend):
                cmd = [
                    str(args.sop_solve),
                    "--backend",
                    backend,
                    "--format",
                    "stats",
                    "--max-vars",
                    str(args.max_vars),
                ]
                if args.trace:
                    cmd += ["--trace", "csv"]
                if backend == "branch" and config["branch_heuristic"] != "split":
                    cmd += ["--branch-heuristic", config["branch_heuristic"]]
                if backend == "rankwidth":
                    cmd += [
                        "--rankwidth-generate",
                        config["rankwidth_generate"],
                        "--rankwidth-mode",
                        config["rankwidth_mode"],
                    ]
                if backend == "treewidth":
                    cmd += ["--treewidth-order", config["treewidth_order"]]
                cmd.append("-")
                try:
                    stats_text, trace_text, solve_elapsed_ns = run_command(cmd, input_text=qsop)
                except RuntimeError as exc:
                    if args.skip_unsupported and backend == "rankwidth" and is_skippable_rankwidth_error(exc):
                        metadata["skipped_rankwidth_records"] += 1
                        continue
                    raise
                stats = parse_stats(stats_text)
                aliases = backend_stat_aliases(backend, stats)
                trace = parse_trace_csv(trace_text) if args.trace else {}
                records.append(
                    {
                        "case": case_name,
                        "source": source,
                        "source_url": source_url,
                        "source_relative_path": source_relative_path,
                        "boundary": f"{input_bits}->{output_bits}",
                        "input": input_bits,
                        "output": output_bits,
                        "backend": backend,
                        "branch_heuristic": config["branch_heuristic"],
                        "rankwidth_mode": config["rankwidth_mode"],
                        "rankwidth_decomposition": config["rankwidth_generate"],
                        "treewidth_order": str(stats.get("treewidth_order", config["treewidth_order"])),
                        **header,
                        "import_elapsed_ns": import_elapsed_ns,
                        "solve_elapsed_ns": solve_elapsed_ns,
                        "qasm_sha256": sha256_text(qasm),
                        "qsop_sha256": sha256_text(qsop),
                        "stats": stats,
                        **aliases,
                        "trace": trace,
                    }
                )
    return records, metadata


def write_jsonl(records: list[dict], file: TextIO) -> None:
    for record in records:
        print(json.dumps(record, sort_keys=True, separators=(",", ":")), file=file)


def write_csv(records: list[dict], file: TextIO) -> None:
    writer = csv.DictWriter(file, fieldnames=CSV_FIELDS)
    writer.writeheader()
    for record in records:
        stats = record["stats"]
        row = {field: record.get(field, "") for field in CSV_FIELDS}
        for key in (
            "search_nodes",
            "leaf_assignments",
            "cache_hits",
            "cache_misses",
            "components",
            "decomposition_width",
            "rankwidth_width",
            "treewidth_width",
            "table_entries",
            "max_table_entries",
            "rankwidth_table_entries",
            "rankwidth_max_table_entries",
            "treewidth_table_entries",
            "treewidth_max_table_entries",
            "signature_entries",
            "max_signature_entries",
            "rankwidth_signature_entries",
            "rankwidth_max_signature_entries",
            "join_pairs",
            "join_signature_pairs",
        ):
            row[key] = record.get(key, stats.get(key, ""))
        row["trace_summary"] = trace_summary_text(record["trace"])
        writer.writerow(row)


def write_top_records(records: list[dict], args: argparse.Namespace, file: TextIO) -> None:
    if args.top == 0:
        return

    print(f"top_records_by_{args.top_metric}:", file=file)
    for key in sorted({record_summary_key(record) for record in records}):
        backend_records = [
            record
            for record in records
            if record_summary_key(record) == key and record_has_metric(record, args.top_metric)
        ]
        ranked = sorted(
            backend_records,
            key=lambda record: (
                record_metric(record, args.top_metric),
                record["solve_elapsed_ns"],
                record["case"],
                record["boundary"],
            ),
            reverse=True,
        )
        print(f"  {format_summary_key(key)[0]}", file=file)
        for line in format_summary_key(key)[1:]:
            print(f"  {line}", file=file)
        if not ranked:
            print("    no records report this metric", file=file)
            continue
        for record in ranked[: args.top]:
            stats = record["stats"]
            line = (
                f"    {record['source']}:{record['case']} {record['boundary']} "
                f"value={record_metric(record, args.top_metric)} "
                f"nvars={record['nvars']} nedges={record['nedges']} "
                f"solve_elapsed_ns={record['solve_elapsed_ns']}"
            )
            if "search_nodes" in stats:
                line += f" search_nodes={stats['search_nodes']}"
            if "leaf_assignments" in stats:
                line += f" leaf_assignments={stats['leaf_assignments']}"
            if "components" in stats:
                line += f" components={stats['components']}"
            if "decomposition_width" in stats:
                line += f" width={stats['decomposition_width']}"
            if "max_table_entries" in stats:
                line += f" max_table={stats['max_table_entries']}"
            if "max_signature_entries" in stats:
                line += f" max_signatures={stats['max_signature_entries']}"
            if "cache_hits" in stats or "cache_misses" in stats:
                line += f" cache={stats.get('cache_hits', 0)}/{stats.get('cache_misses', 0)}"
            trace_phase = dominant_trace_phase(record)
            if trace_phase:
                line += f" top_trace={trace_phase}"
            print(line, file=file)


def record_label(record: dict) -> str:
    return f"{record['source']}:{record['case']} {record['boundary']}"


def metric_value(record: dict, metric: str) -> int | None:
    value = record.get(metric)
    if isinstance(value, int):
        return value
    value = record["stats"].get(metric)
    return value if isinstance(value, int) else None


def write_largest_overview(records: list[dict], file: TextIO) -> None:
    if not records:
        return
    metrics = [
        ("largest_nvars", "nvars"),
        ("largest_nedges", "nedges"),
        ("slowest_solve", "solve_elapsed_ns"),
        ("largest_decomposition_width", "decomposition_width"),
        ("largest_decomposition_table", "max_table_entries"),
        ("largest_rankwidth_width", "rankwidth_width"),
        ("largest_rankwidth_table", "rankwidth_max_table_entries"),
        ("largest_treewidth_width", "treewidth_width"),
        ("largest_treewidth_table", "treewidth_max_table_entries"),
    ]
    for label, metric in metrics:
        candidates = [record for record in records if metric_value(record, metric) is not None]
        if not candidates:
            continue
        record = max(
            candidates,
            key=lambda item: (
                metric_value(item, metric) or 0,
                item["solve_elapsed_ns"],
                item["case"],
                item["boundary"],
            ),
        )
        print(
            f"{label}: {record_label(record)} backend={record['backend']} "
            f"value={metric_value(record, metric)} nvars={record['nvars']} "
            f"nedges={record['nedges']} mode={record['qsop_mode']}",
            file=file,
        )


def write_summary(records: list[dict], metadata: dict, args: argparse.Namespace, file: TextIO) -> None:
    print(f"records: {len(records)}", file=file)
    print(f"case_boundaries: {metadata['case_boundaries']}", file=file)
    print(f"solved_records: {len(records)}", file=file)
    print(f"skipped_rankwidth_records: {metadata['skipped_rankwidth_records']}", file=file)
    print(f"imported_sign: {metadata['imported_sign']}", file=file)
    print(f"imported_labelled: {metadata['imported_labelled']}", file=file)
    source_boundaries = metadata.get("source_boundaries", {})
    if source_boundaries:
        print("sources:", file=file)
        for source in sorted(source_boundaries):
            print(f"  {source}: boundaries={source_boundaries[source]}", file=file)
    write_largest_overview(records, file)
    summary = summarize_records(records)
    for key in sorted(summary):
        entry = summary[key]
        stats = entry["stats"]
        for line in format_summary_key(key):
            print(line, file=file)
        print(f"  records: {entry['records']}", file=file)
        print(f"  import_elapsed_ns: {entry['import_elapsed_ns']}", file=file)
        print(f"  solve_elapsed_ns: {entry['solve_elapsed_ns']}", file=file)
        for key in (
            "search_nodes",
            "leaf_assignments",
            "components",
            "cache_hits",
            "cache_misses",
            "decomposition_width",
            "rankwidth_width",
            "treewidth_width",
            "table_entries",
            "max_table_entries",
            "rankwidth_table_entries",
            "rankwidth_max_table_entries",
            "treewidth_table_entries",
            "treewidth_max_table_entries",
            "signature_entries",
            "max_signature_entries",
            "rankwidth_signature_entries",
            "rankwidth_max_signature_entries",
            "join_pairs",
            "join_signature_pairs",
        ):
            if key in stats:
                print(f"  {key}: {stats[key]}", file=file)
        if "cache_hits" in stats or "cache_misses" in stats:
            print(f"  cache_hit_rate: {cache_hit_rate(stats)}", file=file)

        trace = entry["trace"]
        if trace:
            print("  trace:", file=file)
            for phase in sorted(trace):
                values = trace[phase]
                print(
                    f"    {phase}: events={values['events']} "
                    f"items={values['items']} elapsed_ns={values['elapsed_ns']}",
                    file=file,
                )
    write_top_records(records, args, file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    source_root = pathlib.Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description="Benchmark the QASM solver corpus.")
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("sop_solve", type=pathlib.Path)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=source_root / "tests" / "qasm_solver_corpus.json",
        help="QASM corpus manifest.",
    )
    parser.add_argument(
        "--backend",
        dest="backends",
        action="append",
        choices=BACKENDS,
        help="Backend to benchmark. May be repeated; defaults to all backends.",
    )
    parser.add_argument("--format", choices=("jsonl", "csv", "summary"), default="jsonl")
    parser.add_argument("--limit", type=int, help="Limit case-boundary pairs before backend expansion.")
    parser.add_argument("--max-vars", type=int, default=24, help="Pass-through solver variable guard.")
    parser.add_argument("--trace", action="store_true", help="Collect and summarize sop-solve CSV trace rows.")
    parser.add_argument(
        "--skip-unsupported",
        action="store_true",
        help="Skip rankwidth records rejected by the current sign-only backend.",
    )
    parser.add_argument(
        "--branch-heuristic",
        choices=BRANCH_HEURISTICS,
        default="split",
        help="Variable-choice heuristic used by the branch backend.",
    )
    parser.add_argument(
        "--rankwidth-generate",
        dest="rankwidth_generators",
        action="append",
        choices=RANKWIDTH_GENERATORS,
        help="Generated decomposition used by the rankwidth backend. May be repeated.",
    )
    parser.add_argument(
        "--rankwidth-mode",
        dest="rankwidth_modes",
        action="append",
        choices=RANKWIDTH_MODES,
        help="Solve mode used by the rankwidth backend. May be repeated.",
    )
    parser.add_argument(
        "--rankwidth-sweep",
        action="store_true",
        help="Benchmark all rankwidth generator and solve-mode combinations.",
    )
    parser.add_argument(
        "--treewidth-order",
        dest="treewidth_orders",
        action="append",
        choices=TREEWIDTH_ORDERS,
        help="Treewidth elimination order. May be repeated.",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=0,
        help="With --format summary, print this many largest case-boundary records per backend.",
    )
    parser.add_argument(
        "--top-metric",
        choices=TOP_METRICS,
        default="solve_elapsed_ns",
        help="Metric used by --top ranking.",
    )
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_vars < 0:
        parser.error("--max-vars must be non-negative")
    if args.top < 0:
        parser.error("--top must be non-negative")
    if args.rankwidth_sweep:
        args.rankwidth_generators = list(RANKWIDTH_GENERATORS)
        args.rankwidth_modes = list(RANKWIDTH_MODES)
    else:
        args.rankwidth_generators = args.rankwidth_generators or ["linear"]
        args.rankwidth_modes = args.rankwidth_modes or ["count-table"]
    args.treewidth_orders = args.treewidth_orders or ["min-fill"]
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        records, metadata = benchmark(args)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.format == "jsonl":
        write_jsonl(records, sys.stdout)
    elif args.format == "csv":
        write_csv(records, sys.stdout)
    else:
        write_summary(records, metadata, args, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
