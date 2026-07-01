#!/usr/bin/env python3

import argparse
import collections
import json
import math
import pathlib
import sys
from typing import Iterable, TextIO

from bench_common import read_jsonl as _read_jsonl_common
from summarize_qasm_report import DEFAULT_TIERS, markdown_escape, summarize_reports


AMPLITUDE_ABS_TOL = 1e-6
BRANCH_TREEWIDTH_SKIP_REASON_FIELDS = (
    ("branch_treewidth_skip_width_events", "width"),
    ("branch_treewidth_skip_unavailable_events", "unavailable"),
    ("branch_treewidth_skip_order_width_events", "order-width"),
)
BRANCH_RANKWIDTH_SKIP_REASON_FIELDS = (
    ("branch_rankwidth_skip_treewidth_preferred_events", "treewidth-preferred"),
    ("branch_rankwidth_skip_prefix_proxy_events", "prefix-proxy"),
    ("branch_rankwidth_skip_policy_events", "policy"),
    ("branch_rankwidth_skip_width_events", "width"),
    ("branch_rankwidth_skip_table_forecast_events", "table-forecast"),
    ("branch_rankwidth_skip_join_pair_forecast_events", "join-pair-forecast"),
)
BRANCH_SKIP_REASON_FIELDS = tuple(
    field
    for field, _label in (
        *BRANCH_TREEWIDTH_SKIP_REASON_FIELDS,
        *BRANCH_RANKWIDTH_SKIP_REASON_FIELDS,
    )
)
BRANCH_DISPATCH_SUM_FIELDS = (
    "branch_component_split_events",
    "branch_component_split_elapsed_ns",
    "branch_treewidth_delegate_events",
    "branch_treewidth_delegate_elapsed_ns",
    "branch_root_treewidth_delegate_events",
    "branch_root_treewidth_delegate_elapsed_ns",
    "branch_rankwidth_delegate_events",
    "branch_rankwidth_delegate_elapsed_ns",
)
BRANCH_DISPATCH_MAX_FIELDS = (
    "branch_component_split_max_components",
    "branch_treewidth_delegate_max_vars",
    "branch_root_treewidth_delegate_max_vars",
    "branch_rankwidth_delegate_max_vars",
)
RANKWIDTH_KERNEL_PREFIXES = (
    "rankwidth_join_map",
    "rankwidth_join",
    "rankwidth_fourier_join_map",
    "rankwidth_fourier_join",
)
RANKWIDTH_KERNEL_SUM_FIELDS = tuple(
    field
    for prefix in RANKWIDTH_KERNEL_PREFIXES
    for field in (f"{prefix}_events", f"{prefix}_elapsed_ns")
)
RANKWIDTH_KERNEL_MAX_FIELDS = tuple(
    f"{prefix}_max_items" for prefix in RANKWIDTH_KERNEL_PREFIXES
)
COMPONENT_KERNEL_PREFIXES = (
    "components_convolution",
    "components_fourier_multiply",
    "branch_fourier_multiply",
)
COMPONENT_KERNEL_SUM_FIELDS = tuple(
    field
    for prefix in COMPONENT_KERNEL_PREFIXES
    for field in (f"{prefix}_events", f"{prefix}_elapsed_ns")
)

def read_jsonl(path: pathlib.Path) -> list[dict]:
    return _read_jsonl_common(path, strict=True)


def tier_path(text: str) -> tuple[str, pathlib.Path]:
    if "=" in text:
        label, path_text = text.split("=", 1)
        if not label:
            raise argparse.ArgumentTypeError("label before '=' must be non-empty")
        return label, pathlib.Path(path_text)
    path = pathlib.Path(text)
    return path.stem, path


def record_qsop_mode(record: dict) -> str:
    mode = record.get("qsop_mode")
    if mode == "sign":
        return mode
    if mode not in (None, ""):
        return "unknown"
    if record.get("input") is not None and record.get("output") is not None:
        return "sign"
    return "unknown"


def format_ns(ns: int) -> str:
    if ns >= 1_000_000_000:
        return f"{ns / 1_000_000_000:.2f} s"
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.1f} ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.1f} us"
    return f"{ns} ns"


def tier_sort_key(tier: str) -> tuple[int, str]:
    for index, (name, _minimum, _maximum) in enumerate(DEFAULT_TIERS):
        if tier == name:
            return index, tier
    return len(DEFAULT_TIERS), tier


def solver_config(record: dict) -> str:
    backend = record.get("backend", "")
    solve_mode = record.get("solve_mode")
    solve_mode_suffix = (
        f" --solve-mode {solve_mode}"
        if isinstance(solve_mode, str) and solve_mode and solve_mode != "count-table"
        else ""
    )
    if backend == "wmc":
        enc = record.get("wmc_encoding", "amplitude")
        mode = 0 if enc == "residue" else 6
        return f"sop2wmc --encoding {enc} + ganak --mode {mode}"
    if backend == "branch":
        return f"branch --branch-heuristic {record.get('branch_heuristic') or 'split'}{solve_mode_suffix}"
    if backend == "rankwidth":
        generator = record.get("rankwidth_decomposition") or "left-deep"
        mode = record.get("rankwidth_mode") or "count-table"
        if generator == "min-fill-search" and mode == "count-table":
            return "rankwidth:linear"
        return (
            f"rankwidth --rankwidth-generate {generator} "
            f"--rankwidth-mode {mode}"
        )
    if backend == "treewidth":
        return f"treewidth --treewidth-order {record.get('treewidth_order') or 'min-fill'}{solve_mode_suffix}"
    return str(backend) + solve_mode_suffix


def stat_value(record: dict, key: str) -> int | None:
    value = record.get(key)
    if isinstance(value, int):
        if key == "leaf_assignments" and value == (1 << 64) - 1:
            return None
        return value
    stats = record.get("stats", {})
    value = stats.get(key) if isinstance(stats, dict) else None
    if key == "leaf_assignments" and value == (1 << 64) - 1:
        return None
    return value if isinstance(value, int) else None


def amplitude_value(record: dict) -> complex | None:
    real = record.get("amplitude_real")
    imag = record.get("amplitude_imag")
    if isinstance(real, (int, float)) and isinstance(imag, (int, float)):
        return complex(float(real), float(imag))
    return None


def add_sum(counter: dict[str, int], key: str, value: int | None) -> None:
    if isinstance(value, int):
        counter[key] = counter.get(key, 0) + value


def add_max(counter: dict[str, int], key: str, value: int | None) -> None:
    if isinstance(value, int):
        counter[key] = max(counter.get(key, 0), value)


def branch_skip_reason_text(stats: dict[str, int], fields: tuple[tuple[str, str], ...], value_formatter=str) -> str:
    parts = []
    for field, label in fields:
        value = stats.get(field, 0)
        if isinstance(value, int) and value:
            parts.append(f"{label}={value_formatter(value)}")
    return ", ".join(parts)


def branch_dispatch_text(stats: dict[str, int], value_formatter=str) -> str:
    parts = []
    split_events = stats.get("branch_component_split_events", 0)
    if isinstance(split_events, int) and split_events:
        parts.append(
            f"splits={value_formatter(split_events)}/"
            f"{format_ns(stats.get('branch_component_split_elapsed_ns', 0))} "
            f"max components={value_formatter(stats.get('branch_component_split_max_components', 0))}"
        )
    treewidth_events = stats.get("branch_treewidth_delegate_events", 0)
    if isinstance(treewidth_events, int) and treewidth_events:
        parts.append(
            f"tw delegates={value_formatter(treewidth_events)}/"
            f"{format_ns(stats.get('branch_treewidth_delegate_elapsed_ns', 0))} "
            f"max vars={value_formatter(stats.get('branch_treewidth_delegate_max_vars', 0))}"
        )
    root_treewidth_events = stats.get("branch_root_treewidth_delegate_events", 0)
    if isinstance(root_treewidth_events, int) and root_treewidth_events:
        parts.append(
            f"root tw delegates={value_formatter(root_treewidth_events)}/"
            f"{format_ns(stats.get('branch_root_treewidth_delegate_elapsed_ns', 0))} "
            f"max vars={value_formatter(stats.get('branch_root_treewidth_delegate_max_vars', 0))}"
        )
    rankwidth_events = stats.get("branch_rankwidth_delegate_events", 0)
    if isinstance(rankwidth_events, int) and rankwidth_events:
        parts.append(
            f"rw delegates={value_formatter(rankwidth_events)}/"
            f"{format_ns(stats.get('branch_rankwidth_delegate_elapsed_ns', 0))} "
            f"max vars={value_formatter(stats.get('branch_rankwidth_delegate_max_vars', 0))}"
        )
    return ", ".join(parts)


def rankwidth_kernel_text(stats: dict[str, int], value_formatter=str) -> str:
    parts = []
    for prefix, label in (
        ("rankwidth_join_map", "map"),
        ("rankwidth_join", "join"),
        ("rankwidth_fourier_join_map", "fourier-map"),
        ("rankwidth_fourier_join", "fourier"),
    ):
        events = stats.get(f"{prefix}_events", 0)
        elapsed = stats.get(f"{prefix}_elapsed_ns", 0)
        max_items = stats.get(f"{prefix}_max_items", 0)
        if isinstance(events, int) and (events or elapsed or max_items):
            parts.append(
                f"{label}={value_formatter(events)}/{format_ns(elapsed)} "
                f"max items={value_formatter(max_items)}"
            )
    return ", ".join(parts)


def component_kernel_text(stats: dict[str, int], value_formatter=str) -> str:
    parts = []
    for prefix, label in (
        ("components_convolution", "convolution"),
        ("components_fourier_multiply", "fourier"),
        ("branch_fourier_multiply", "branch-fourier"),
    ):
        events = stats.get(f"{prefix}_events", 0)
        elapsed = stats.get(f"{prefix}_elapsed_ns", 0)
        if isinstance(events, int) and (events or elapsed):
            parts.append(f"{label}={value_formatter(events)}/{format_ns(elapsed)}")
    return ", ".join(parts)


def cache_hit_rate(stats: dict[str, int]) -> str:
    hits = stats.get("cache_hits", 0)
    misses = stats.get("cache_misses", 0)
    total = hits + misses
    if total == 0:
        return "n/a"
    return f"{hits / total:.3f}"


def cache_avoided_node_rate(stats: dict[str, int]) -> str:
    nodes = stats.get("search_nodes", 0)
    if nodes == 0:
        return "n/a"
    return f"{stats.get('cache_avoided_nodes', 0) / nodes:.3f}"


def cache_canonical_entry_rate(stats: dict[str, int]) -> str:
    entries = stats.get("cache_entries", 0)
    if entries == 0:
        return "n/a"
    return f"{stats.get('cache_canonical_entries', 0) / entries:.3f}"


def summarize_solver_records(named_records: Iterable[tuple[str, list[dict]]]) -> list[dict]:
    grouped: dict[tuple[str, str], dict] = {}
    for tier, records in named_records:
        for record in records:
            key = (tier, solver_config(record))
            entry = grouped.setdefault(
                key,
                {
                    "tier": tier,
                    "config": key[1],
                    "records": 0,
                    "ok": 0,
                    "timeouts": 0,
                    "memouts": 0,
                    "errors": 0,
                    "elapsed_ns": 0,
                    "sources": collections.Counter(),
                    "stats": {},
                },
            )
            entry["records"] += 1
            status = record.get("status", "ok")
            if status == "ok":
                entry["ok"] += 1
            elif status == "timeout":
                entry["timeouts"] += 1
            elif status == "memout":
                entry["memouts"] += 1
            else:
                entry["errors"] += 1
            entry["elapsed_ns"] += int(record.get("solve_elapsed_ns") or 0)
            entry["sources"][record.get("source") or "unknown"] += 1
            if status != "ok":
                continue
            stats = entry["stats"]
            for stat in (
                "search_nodes",
                "leaf_assignments",
                "cache_hits",
                "cache_misses",
                "cache_avoided_nodes",
                "cache_canonical_hits",
                "cache_canonical_lookups",
                "cache_canonical_stores",
                "components",
            ):
                add_sum(stats, stat, stat_value(record, stat))
            for stat in (
                "wmc_export_elapsed_ns",
                "wmc_ganak_elapsed_ns",
            ):
                add_sum(stats, stat, stat_value(record, stat))
            # WMC mismatches live at top level under "mismatches".
            # -1 means "check skipped" (no reference or precision limited); don't count those.
            mm = stat_value(record, "mismatches")
            if mm is not None and mm > 0:
                add_sum(stats, "wmc_mismatches", mm)
            for stat in (
                "treewidth_delegations",
                "rankwidth_delegations",
                "branch_fallthroughs",
                "branch_fallthrough_trace_events",
                "branch_treewidth_skips",
                "branch_rankwidth_skips",
                "cache_lookup_events",
                "cache_lookup_elapsed_ns",
                "cache_canonical_lookup_events",
                "cache_canonical_lookup_elapsed_ns",
                "cache_store_events",
                "cache_store_elapsed_ns",
                "cache_canonical_store_events",
                "cache_canonical_store_elapsed_ns",
                "branch_rankwidth_probe_events",
                "branch_rankwidth_probe_elapsed_ns",
                "rankwidth_width_probe_events",
                "rankwidth_width_probe_elapsed_ns",
                "branch_treewidth_order_probe_events",
                "branch_treewidth_order_probe_elapsed_ns",
                "branch_root_width_probe_events",
                "branch_root_width_probe_elapsed_ns",
                "rankwidth_linear_transition_events",
                *COMPONENT_KERNEL_SUM_FIELDS,
                *RANKWIDTH_KERNEL_SUM_FIELDS,
                *BRANCH_SKIP_REASON_FIELDS,
                *BRANCH_DISPATCH_SUM_FIELDS,
            ):
                add_sum(stats, stat, stat_value(record, stat))
            for stat in (
                "rankwidth_width",
                "rankwidth_cutrank_width",
                "rankwidth_max_table_entries",
                "rankwidth_table_forecast",
                "rankwidth_join_pair_forecast",
                "rankwidth_dense_table_forecast",
                "rankwidth_dense_even_join_forecast",
                "rankwidth_max_signature_entries",
                "cache_entries",
                "cache_canonical_entries",
                "cache_stored_residue_slots",
                "cache_key_bytes",
                "cache_count_bytes",
                "cache_estimated_bytes",
                "treewidth_width",
                "treewidth_max_table_entries",
                "decomposition_width",
                "max_table_entries",
                "max_signature_entries",
                "max_residual_vars",
                "max_residual_edges",
                "max_residual_components",
                "max_residual_largest_component",
                "max_residual_min_fill_width",
                "max_residual_prefix_cut_rank",
                "branch_rankwidth_cutrank_width",
                "rankwidth_width_probe_width",
                "rankwidth_cutrank_width_probe_width",
                "rankwidth_trace_table_forecast",
                "rankwidth_trace_join_pair_forecast",
                "branch_fallthrough_max_vars",
                "branch_rankwidth_table_forecast",
                "branch_rankwidth_join_pair_forecast",
                "branch_treewidth_order_width",
                "branch_root_width_probe_width",
                "branch_treewidth_table_forecast",
                "branch_treewidth_join_pair_forecast",
                *RANKWIDTH_KERNEL_MAX_FIELDS,
                *BRANCH_DISPATCH_MAX_FIELDS,
            ):
                add_max(stats, stat, stat_value(record, stat))
            for stat in (
                "join_pairs",
                "join_signature_pairs",
            ):
                add_sum(stats, stat, stat_value(record, stat))
    return [grouped[key] for key in sorted(grouped)]


def cap_value(record: dict, key: str) -> str:
    if key not in record:
        return "not recorded"
    value = record.get(key)
    return "none" if value is None else str(value)


def memory_cap_value(record: dict) -> str:
    cgroup = record.get("cgroup_memory_limit_mib")
    if cgroup is not None:
        return f"cgroup {cgroup}"
    value = record.get("memory_limit_mib")
    return "none" if value is None else f"rlimit {value}"


def has_comparison_identity(record: dict) -> bool:
    return bool(record.get("case") and record.get("input") is not None and record.get("output") is not None)


def comparison_key(record: dict) -> tuple[str, str, str, str, str]:
    return (
        str(record.get("source") or "unknown"),
        str(record.get("source_relative_path") or ""),
        str(record.get("case") or ""),
        str(record.get("input") or ""),
        str(record.get("output") or ""),
    )


def comparison_speedup(native_ns: int, solver_ns: int) -> str:
    if native_ns <= 0 or solver_ns <= 0:
        return "n/a"
    return f"{native_ns / solver_ns:.2f}x"


def summarize_native_comparison_records(
    solver_records: Iterable[tuple[str, list[dict]]],
    native_records: Iterable[tuple[str, list[dict]]],
) -> list[dict]:
    native_by_tier_and_key: dict[tuple[str, tuple[str, str, str, str, str]], list[dict]] = {}
    for tier, records in native_records:
        for record in records:
            if has_comparison_identity(record):
                native_by_tier_and_key.setdefault((tier, comparison_key(record)), []).append(record)

    grouped: dict[tuple[str, str, str], dict] = {}
    for tier, records in solver_records:
        for solver in records:
            if not has_comparison_identity(solver):
                continue
            matches = native_by_tier_and_key.get((tier, comparison_key(solver)), [])
            if not matches:
                continue
            config = solver_config(solver)
            solver_ok = solver.get("status", "ok") == "ok"
            solver_elapsed_ns = int(solver.get("solve_elapsed_ns") or 0)
            source = comparison_key(solver)[0]
            for native in matches:
                engine = native.get("engine") or "unknown"
                key = (source, tier, config, engine)
                entry = grouped.setdefault(
                    key,
                    {
                        "source": source,
                        "tier": tier,
                        "config": config,
                        "engine": engine,
                        "matched": 0,
                        "both_ok": 0,
                        "solver_elapsed_ns": 0,
                        "native_elapsed_ns": 0,
                        "amplitude_checked": 0,
                        "amplitude_mismatches": 0,
                        "amplitude_abs_error_sum": 0.0,
                        "amplitude_max_abs_error": 0.0,
                        "max_boundary_qubits": 0,
                        "qubit_caps": collections.Counter(),
                        "timeouts": collections.Counter(),
                        "memory_caps": collections.Counter(),
                        "errors": collections.Counter(),
                    },
                )
                entry["matched"] += 1
                entry["qubit_caps"][cap_value(native, "qubit_cap")] += 1
                entry["timeouts"][cap_value(native, "timeout_seconds")] += 1
                entry["memory_caps"][memory_cap_value(native)] += 1
                if isinstance(native.get("qubits"), int):
                    entry["max_boundary_qubits"] = max(
                        entry["max_boundary_qubits"], int(native["qubits"])
                    )
                native_ok = native.get("status") == "ok"
                if solver_ok and native_ok:
                    entry["both_ok"] += 1
                    entry["solver_elapsed_ns"] += solver_elapsed_ns
                    entry["native_elapsed_ns"] += int(native.get("elapsed_ns") or 0)
                    solver_amplitude = amplitude_value(solver)
                    native_amplitude = amplitude_value(native)
                    if solver_amplitude is not None and native_amplitude is not None:
                        entry["amplitude_checked"] += 1
                        error = abs(solver_amplitude - native_amplitude)
                        entry["amplitude_abs_error_sum"] += error
                        entry["amplitude_max_abs_error"] = max(
                            entry["amplitude_max_abs_error"], error
                        )
                        if error > AMPLITUDE_ABS_TOL:
                            entry["amplitude_mismatches"] += 1
                elif native.get("error"):
                    entry["errors"][str(native["error"])] += 1
    return [grouped[key] for key in sorted(grouped)]


def write_import_tables(report_paths: list[pathlib.Path], file: TextIO) -> None:
    summary = summarize_reports(report_paths, list(DEFAULT_TIERS))
    print("## Import Coverage\n", file=file)
    print("| Source | Upstream | Inputs | OK | Below min | Too large | Other unsupported |", file=file)
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["sources"]:
        print(
            f"| {markdown_escape(row['source'])} | {markdown_escape(row.get('source_url') or '')} | "
            f"{row['inputs']} | {row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | "
            f"{row['unsupported']} |",
            file=file,
        )
    print("\n| Tier | Imported variables | Records | OK | Below min | Too large | Other unsupported |", file=file)
    print("| --- | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in summary["tier_summary"]:
        print(
            f"| {markdown_escape(row['tier'])} | {markdown_escape(row['range'])} | {row['records']} | "
            f"{row['ok']} | {row['below_min_vars']} | {row['too_many_vars']} | {row['unsupported']} |",
            file=file,
        )


def write_mqt_scaling_table(scaling_table_path: pathlib.Path | None, file: TextIO) -> None:
    """Render MQT detailed scaling table from mqt-scaling-table.json."""
    if scaling_table_path is None or not scaling_table_path.exists():
        return
    try:
        scaling = json.loads(scaling_table_path.read_text(encoding="utf-8"))
    except Exception as exc:
        print(f"> warning: could not read MQT scaling table: {exc}\n", file=file)
        return
    if not scaling:
        return
    print("\n## MQT Bench Scaling by Family\n", file=file)
    print(
        "| Family | Mode | Rows | Qubits p50 | Qubits max "
        "| Vars p50 | Vars max | TW p50 | TW max | Cut-rank p50 | Cut-rank max |",
        file=file,
    )
    print("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)
    for row in scaling:
        sign = row.get("sign_count", 0)
        mode_str = "sign" if sign > 0 or row.get("qsop_mode") == "sign" else "unknown"
        print(
            f"| {markdown_escape(row.get('family', ''))} "
            f"| {markdown_escape(mode_str)} "
            f"| {row.get('rows', 0)} "
            f"| {int(row.get('qubits_p50', 0))} "
            f"| {int(row.get('qubits_max', 0))} "
            f"| {int(row.get('nvars_p50', 0))} "
            f"| {int(row.get('nvars_max', 0))} "
            f"| {int(row.get('treewidth_p50', 0))} "
            f"| {int(row.get('treewidth_max', 0))} "
            f"| {int(row.get('cut_rank_p50', 0))} "
            f"| {int(row.get('cut_rank_max', 0))} |",
            file=file,
        )


def write_mqt_manifest_notice(manifest_dir: pathlib.Path | None, file: TextIO) -> None:
    """Emit a notice when MQT manifests are absent or empty."""
    if manifest_dir is None:
        return
    tier_jsons = list(manifest_dir.glob("tier-*.json")) if manifest_dir.exists() else []
    if tier_jsons:
        return
    print("\n## MQT Bench Data\n", file=file)
    print("> **MQT Bench manifests not found.** "
          "No MQT QSOP rows are available for this scoreboard.\n", file=file)
    print("To populate MQT data, run:\n", file=file)
    print("```sh", file=file)
    print("# Step 1 — harvest QASM manifests from MQT Bench (requires mqt-bench + qiskit):", file=file)
    print("python3 tools/bench.py harvest-mqt \\", file=file)
    print("    --manifest-dir benchmarks/manifests/mqt \\", file=file)
    print("    --qasm2sop build/qasm2sop \\", file=file)
    print("    --sop-stats build/sop-stats", file=file)
    print("", file=file)
    print("# Step 2 — materialize QASM manifests to local QSOP corpus:", file=file)
    print("python3 tools/bench.py materialize-mqt \\", file=file)
    print("    --manifest-dir benchmarks/manifests/mqt \\", file=file)
    print("    --corpus-dir benchmarks/corpus/sop/materialized-external/mqt-bench \\", file=file)
    print("    --qasm2sop build/qasm2sop \\", file=file)
    print("    --sop-solve build/sop-solve", file=file)
    print("", file=file)
    print("# Step 3 — profile corpus structure:", file=file)
    print("python3 tools/bench.py profile-mqt \\", file=file)
    print("    --corpus-dir benchmarks/corpus/sop/materialized-external/mqt-bench \\", file=file)
    print("    --artifact-dir artifacts/mqt \\", file=file)
    print("    --sop-solve build/sop-solve", file=file)
    print("```", file=file)


def write_mqt_tuning_summary(records: list[dict], file: TextIO) -> None:
    """Render a compact MQT tuning summary from sop_bench_result_v2 records."""
    print("# MQT Tuning Summary\n", file=file)
    if not records:
        print("No MQT tuning records found.\n", file=file)
        return
    write_local_backend_summary(records, file)


def write_local_backend_summary(records: list[dict], file: TextIO) -> None:
    """Render a compact local backend summary from sop_bench_result_v2 records."""
    by_tier_backend: dict[tuple[str, str], dict] = {}
    for r in records:
        tier = r.get("tier", "")
        backend = r.get("backend", "")
        key = (tier, backend)
        entry = by_tier_backend.setdefault(key, {
            "tier": tier,
            "backend": backend,
            "solved": 0,
            "skipped": 0,
            "timeout": 0,
            "error": 0,
            "total_ns": 0,
            "elapsed_values": [],
            "sign": 0,
        })
        status = r.get("status", "")
        if status == "ok":
            entry["solved"] += 1
            ns = int(r.get("elapsed_ns") or r.get("solve_elapsed_ns") or 0)
            entry["total_ns"] += ns
            if ns > 0:
                entry["elapsed_values"].append(ns)
        elif status == "skipped":
            entry["skipped"] += 1
        elif status == "timeout":
            entry["timeout"] += 1
        else:
            entry["error"] += 1
        qsop_mode = r.get("qsop_mode", "")
        if qsop_mode == "sign":
            entry["sign"] += 1

    if not by_tier_backend:
        return

    print("## Local sop-solve backends\n", file=file)

    for tier in sorted({k[0] for k in by_tier_backend}, key=tier_sort_key):
        tier_entries = {k[1]: v for k, v in by_tier_backend.items() if k[0] == tier}
        ok_ns = [v["total_ns"] for v in tier_entries.values() if v["solved"] > 0 and v["total_ns"] > 0]
        fastest_ns = min(ok_ns) if ok_ns else 0

        print(f"### {tier}\n", file=file)
        print("| Backend | Solved | Skipped | Timeout | Error | Total time | Geomean | Ratio | Signed rows |", file=file)
        print("| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |", file=file)

        for backend in sorted(tier_entries):
            v = tier_entries[backend]
            vals = v["elapsed_values"]
            geomean_ns = (
                int(math.exp(sum(math.log(x) for x in vals) / len(vals))) if vals else None
            )
            ratio_str = (
                f"{v['total_ns'] / fastest_ns:.2f}x"
                if fastest_ns > 0 and v["total_ns"] > 0
                else "—"
            )
            mode_str = f"{v['sign']}"
            print(
                f"| `{backend}` | {v['solved']} | {v['skipped']} | {v['timeout']} | {v['error']}"
                f" | {format_ns(v['total_ns'])}"
                f" | {format_ns(geomean_ns) if geomean_ns is not None else '—'}"
                f" | {ratio_str} | {mode_str} |",
                file=file,
            )
        print("", file=file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Render scoreboard Markdown tables from structured benchmark outputs.")
    parser.add_argument("--import-report", action="append", type=pathlib.Path, default=[])
    parser.add_argument("--local-jsonl", action="append", type=pathlib.Path, default=[], metavar="PATH",
                        help="Local sop_bench_result_v2 JSONL file(s) for local backend summary")
    parser.add_argument("--mqt-tuning-jsonl", type=pathlib.Path, default=None, metavar="PATH",
                        help="MQT tuning JSONL file for MQT tuning summary")
    parser.add_argument("--mqt-manifest-dir", type=pathlib.Path, default=None,
                        help="MQT manifest directory; if empty/missing, an MQT notice is emitted")
    parser.add_argument("--mqt-scaling-table", type=pathlib.Path, default=None,
                        help="Path to mqt-scaling-table.json from profile-mqt")
    parser.add_argument("--output", type=pathlib.Path, default=None,
                        help="Write output to this file instead of stdout")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    out_file = None
    try:
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            out_file = open(args.output, "w", encoding="utf-8")
        out = out_file if out_file is not None else sys.stdout

        if args.mqt_tuning_jsonl:
            mqt_records = read_jsonl(args.mqt_tuning_jsonl)
            write_mqt_tuning_summary(mqt_records, out)
        if args.local_jsonl:
            local_records: list[dict] = []
            for path in args.local_jsonl:
                local_records.extend(read_jsonl(path))
            write_local_backend_summary(local_records, out)
        if args.import_report:
            write_import_tables(args.import_report, out)
        write_mqt_scaling_table(args.mqt_scaling_table, out)
        write_mqt_manifest_notice(args.mqt_manifest_dir, out)
    except (RuntimeError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        if out_file is not None:
            out_file.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
