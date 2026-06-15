#!/usr/bin/env python3

import argparse
import cmath
import csv
import hashlib
import json
import math
import pathlib
import subprocess
import sys
import time
from typing import TextIO


BACKENDS = ("components", "brute-force", "branch", "rankwidth", "treewidth")
DEFAULT_BACKENDS = ("components", "brute-force", "branch")
BRANCH_HEURISTICS = ("split", "treewidth", "cutrank-proxy")
RANKWIDTH_GENERATORS = ("left-deep", "balanced", "min-fill", "min-fill-cut")
RANKWIDTH_MODES = ("count-table", "fourier")
TREEWIDTH_ORDERS = ("min-fill", "min-degree", "min-fill-max-degree")
BRANCH_TREEWIDTH_SKIP_METRICS = {
    "branch.treewidth_skip_width": "branch_treewidth_skip_width_events",
    "branch.treewidth_skip_unavailable": "branch_treewidth_skip_unavailable_events",
    "branch.treewidth_skip_order_width": "branch_treewidth_skip_order_width_events",
}
BRANCH_RANKWIDTH_SKIP_METRICS = {
    "branch.rankwidth_skip_treewidth_preferred": "branch_rankwidth_skip_treewidth_preferred_events",
    "branch.rankwidth_skip_prefix_proxy": "branch_rankwidth_skip_prefix_proxy_events",
    "branch.rankwidth_skip_policy": "branch_rankwidth_skip_policy_events",
    "branch.rankwidth_skip_width": "branch_rankwidth_skip_width_events",
    "branch.rankwidth_skip_table_forecast": "branch_rankwidth_skip_table_forecast_events",
    "branch.rankwidth_skip_join_pair_forecast": "branch_rankwidth_skip_join_pair_forecast_events",
}
BRANCH_SKIP_REASON_FIELDS = tuple(BRANCH_TREEWIDTH_SKIP_METRICS.values()) + tuple(
    BRANCH_RANKWIDTH_SKIP_METRICS.values()
)
BRANCH_DISPATCH_TRACE_GROUPS = (
    (("branch.component_split",), "branch_component_split", "max_components"),
    (("branch.treewidth_delegate",), "branch_treewidth_delegate", "max_vars"),
    (("branch.root_treewidth_delegate",), "branch_root_treewidth_delegate", "max_vars"),
    (("branch.rankwidth_delegate",), "branch_rankwidth_delegate", "max_vars"),
)
BRANCH_DISPATCH_METRIC_FIELDS = tuple(
    field
    for _phases, prefix, max_key in BRANCH_DISPATCH_TRACE_GROUPS
    for field in (f"{prefix}_events", f"{prefix}_elapsed_ns", f"{prefix}_{max_key}")
)
RANKWIDTH_KERNEL_TRACE_GROUPS = (
    (("rankwidth.join_map", "rankwidth.crt_join_map"), "rankwidth_join_map"),
    (("rankwidth.join", "rankwidth.crt_join"), "rankwidth_join"),
    (
        ("rankwidth.labelled_join_map", "rankwidth.labelled_crt_join_map"),
        "rankwidth_labelled_join_map",
    ),
    (("rankwidth.labelled_join", "rankwidth.labelled_crt_join"), "rankwidth_labelled_join"),
    (("rankwidth.fourier_join_map",), "rankwidth_fourier_join_map"),
    (("rankwidth.fourier_join",), "rankwidth_fourier_join"),
)
RANKWIDTH_KERNEL_METRIC_FIELDS = tuple(
    field
    for _phases, prefix in RANKWIDTH_KERNEL_TRACE_GROUPS
    for field in (f"{prefix}_events", f"{prefix}_elapsed_ns", f"{prefix}_max_items")
)
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
    "trace_elapsed_ns",
    "trace_top_elapsed_ns",
    "trace_top_share_ppm",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "cache_avoided_nodes",
    "cache_canonical_hits",
    "cache_entries",
    "cache_canonical_entries",
    "cache_stored_residue_slots",
    "cache_hit_rate_ppm",
    "cache_avoided_node_rate_ppm",
    "cache_lookup_events",
    "cache_lookup_elapsed_ns",
    "cache_store_events",
    "cache_store_elapsed_ns",
    "branch_rankwidth_probe_events",
    "branch_rankwidth_probe_elapsed_ns",
    "branch_rankwidth_labelled_width",
    "branch_rankwidth_support_width",
    "branch_rankwidth_table_forecast",
    "branch_rankwidth_join_pair_forecast",
    "branch_treewidth_order_probe_events",
    "branch_treewidth_order_probe_elapsed_ns",
    "branch_treewidth_order_width",
    "branch_treewidth_table_forecast",
    "branch_treewidth_join_pair_forecast",
    "treewidth_multiply_events",
    "treewidth_multiply_elapsed_ns",
    "treewidth_sum_out_events",
    "treewidth_sum_out_elapsed_ns",
    *RANKWIDTH_KERNEL_METRIC_FIELDS,
    "branch_root_treewidth_delegate_events",
    "branch_root_treewidth_delegate_elapsed_ns",
    "branch_root_treewidth_delegate_max_vars",
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
    "treewidth_delegations",
    "rankwidth_delegations",
    "branch_fallthroughs",
    "branch_treewidth_skips",
    "branch_rankwidth_skips",
    *BRANCH_SKIP_REASON_FIELDS,
    *BRANCH_DISPATCH_METRIC_FIELDS,
    "max_residual_vars",
    "max_residual_edges",
    "max_residual_components",
    "max_residual_largest_component",
    "max_residual_min_fill_width",
    "max_residual_prefix_cut_rank",
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
    "status",
    "error",
    "qsop_mode",
    "r",
    "nvars",
    "nedges",
    "import_elapsed_ns",
    "solve_elapsed_ns",
    "trace_elapsed_ns",
    "trace_top_phase",
    "trace_top_elapsed_ns",
    "trace_top_share_ppm",
    "amplitude_real",
    "amplitude_imag",
    "search_nodes",
    "leaf_assignments",
    "cache_hits",
    "cache_misses",
    "cache_avoided_nodes",
    "cache_canonical_hits",
    "cache_entries",
    "cache_canonical_entries",
    "cache_stored_residue_slots",
    "cache_hit_rate_ppm",
    "cache_avoided_node_rate_ppm",
    "cache_lookup_events",
    "cache_lookup_elapsed_ns",
    "cache_store_events",
    "cache_store_elapsed_ns",
    "branch_rankwidth_probe_events",
    "branch_rankwidth_probe_elapsed_ns",
    "branch_rankwidth_labelled_width",
    "branch_rankwidth_support_width",
    "branch_rankwidth_table_forecast",
    "branch_rankwidth_join_pair_forecast",
    "branch_treewidth_order_probe_events",
    "branch_treewidth_order_probe_elapsed_ns",
    "branch_treewidth_order_width",
    "branch_treewidth_table_forecast",
    "branch_treewidth_join_pair_forecast",
    "treewidth_multiply_events",
    "treewidth_multiply_elapsed_ns",
    "treewidth_sum_out_events",
    "treewidth_sum_out_elapsed_ns",
    *RANKWIDTH_KERNEL_METRIC_FIELDS,
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
    "treewidth_delegations",
    "rankwidth_delegations",
    "branch_fallthroughs",
    "branch_treewidth_skips",
    "branch_rankwidth_skips",
    *BRANCH_SKIP_REASON_FIELDS,
    *BRANCH_DISPATCH_METRIC_FIELDS,
    "max_residual_vars",
    "max_residual_edges",
    "max_residual_components",
    "max_residual_largest_component",
    "max_residual_min_fill_width",
    "max_residual_prefix_cut_rank",
    "qasm_sha256",
    "qsop_sha256",
    "trace_summary",
]


class CommandTimeout(RuntimeError):
    def __init__(self, cmd: list[str], timeout_seconds: float, elapsed_ns: int):
        super().__init__(f"command timed out after {timeout_seconds:g}s: {cmd}")
        self.cmd = cmd
        self.timeout_seconds = timeout_seconds
        self.elapsed_ns = elapsed_ns


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


def run_command(
    cmd: list[str], *, input_text: str | None = None, timeout_seconds: float | None = None
) -> tuple[str, str, int]:
    start = time.perf_counter_ns()
    try:
        completed = subprocess.run(
            cmd,
            input=input_text,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_seconds,
        )
        elapsed = time.perf_counter_ns() - start
    except subprocess.TimeoutExpired as exc:
        elapsed = time.perf_counter_ns() - start
        raise CommandTimeout(cmd, timeout_seconds or 0.0, elapsed) from exc
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


def amplitude_metrics(modulus: int, norm_h: int, counts: list[int]) -> dict[str, float]:
    omega = cmath.exp(2j * math.pi / modulus)
    total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    amplitude = total * (2.0 ** (-norm_h / 2.0))
    return {
        "amplitude_real": amplitude.real,
        "amplitude_imag": amplitude.imag,
    }


def parse_stats_and_amplitude(text: str) -> tuple[dict[str, int | str], dict[str, float]]:
    stats: dict[str, int | str] = {}
    result_modulus: int | None = None
    result_norm_h: int | None = None
    result_counts: list[int] | None = None
    for line in text.splitlines():
        if not line:
            continue
        key, value = line.split(": ", 1)
        if key == "result_modulus":
            result_modulus = int(value)
            continue
        if key == "result_norm_h":
            result_norm_h = int(value)
            continue
        if key == "result_counts":
            result_counts = [int(part) for part in value.split()]
            continue
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
    metrics: dict[str, float] = {}
    if result_modulus is not None and result_norm_h is not None and result_counts is not None:
        metrics = amplitude_metrics(result_modulus, result_norm_h, result_counts)
    return stats, metrics


def parse_stats(text: str) -> dict[str, int | str]:
    return parse_stats_and_amplitude(text)[0]


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
        entry = summary.setdefault(phase, {"events": 0, "items": 0, "max_items": 0, "elapsed_ns": 0})
        item_count = int(items)
        entry["events"] += 1
        entry["items"] += item_count
        entry["max_items"] = max(entry["max_items"], item_count)
        entry["elapsed_ns"] += int(elapsed_ns)
    return summary


def trace_summary_text(trace: dict[str, dict[str, int]]) -> str:
    return ";".join(
        f"{phase}:{values['events']}:{values['elapsed_ns']}" for phase, values in sorted(trace.items())
    )


def trace_elapsed_ns(trace: dict[str, dict[str, int]]) -> int:
    return sum(values["elapsed_ns"] for values in trace.values())


def trace_top_phase_text(trace: dict[str, dict[str, int]], limit: int = 3) -> str:
    total = trace_elapsed_ns(trace)
    if total <= 0 or limit <= 0:
        return ""
    ranked = sorted(
        trace.items(),
        key=lambda item: (-item[1]["elapsed_ns"], item[0]),
    )
    parts = []
    for phase, values in ranked[:limit]:
        elapsed = values["elapsed_ns"]
        parts.append(f"{phase}:{elapsed}:{(elapsed * 100.0 / total):.1f}%")
    return ",".join(parts)


def trace_record_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int | str]:
    total = trace_elapsed_ns(trace)
    if total <= 0:
        return {}
    phase, values = sorted(
        trace.items(),
        key=lambda item: (-item[1]["elapsed_ns"], item[0]),
    )[0]
    elapsed = values["elapsed_ns"]
    return {
        "trace_elapsed_ns": total,
        "trace_top_phase": phase,
        "trace_top_elapsed_ns": elapsed,
        "trace_top_share_ppm": (elapsed * 1_000_000) // total,
    }


def cache_record_metrics(
    stats: dict[str, int | str], trace: dict[str, dict[str, int]]
) -> dict[str, int]:
    metrics: dict[str, int] = {}
    hits = stats.get("cache_hits")
    misses = stats.get("cache_misses")
    if isinstance(hits, int) and isinstance(misses, int):
        total = hits + misses
        if total:
            metrics["cache_hit_rate_ppm"] = (hits * 1_000_000) // total
    avoided = stats.get("cache_avoided_nodes")
    search_nodes = stats.get("search_nodes")
    if isinstance(avoided, int) and isinstance(search_nodes, int) and search_nodes:
        metrics["cache_avoided_node_rate_ppm"] = (avoided * 1_000_000) // search_nodes
    for phase, events_key, elapsed_key in (
        ("cache_lookup", "cache_lookup_events", "cache_lookup_elapsed_ns"),
        ("cache_store", "cache_store_events", "cache_store_elapsed_ns"),
    ):
        matching = [
            values
            for event, values in trace.items()
            if event.endswith(f".{phase}") and event.rsplit(".", 1)[0] in {"branch", "components"}
        ]
        if matching:
            metrics[events_key] = sum(values["events"] for values in matching)
            metrics[elapsed_key] = sum(values["elapsed_ns"] for values in matching)
    return metrics


def branch_rankwidth_probe_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    labelled = trace.get("branch.rankwidth_probe")
    if labelled is not None:
        metrics["branch_rankwidth_probe_events"] = labelled["events"]
        metrics["branch_rankwidth_probe_elapsed_ns"] = labelled["elapsed_ns"]
        metrics["branch_rankwidth_labelled_width"] = labelled["max_items"]
    support = trace.get("branch.rankwidth_support_probe")
    if support is not None:
        metrics["branch_rankwidth_support_width"] = support["max_items"]
    table_forecast = trace.get("branch.rankwidth_table_forecast")
    if table_forecast is not None:
        metrics["branch_rankwidth_table_forecast"] = table_forecast["max_items"]
    join_forecast = trace.get("branch.rankwidth_join_pair_forecast")
    if join_forecast is not None:
        metrics["branch_rankwidth_join_pair_forecast"] = join_forecast["max_items"]
    return metrics


def branch_treewidth_probe_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    order = trace.get("branch.treewidth_order_probe")
    if order is not None:
        metrics["branch_treewidth_order_probe_events"] = order["events"]
        metrics["branch_treewidth_order_probe_elapsed_ns"] = order["elapsed_ns"]
        metrics["branch_treewidth_order_width"] = order["max_items"]
    table_forecast = trace.get("branch.treewidth_table_forecast")
    if table_forecast is not None:
        metrics["branch_treewidth_table_forecast"] = table_forecast["max_items"]
    join_forecast = trace.get("branch.treewidth_join_pair_forecast")
    if join_forecast is not None:
        metrics["branch_treewidth_join_pair_forecast"] = join_forecast["max_items"]
    return metrics


def treewidth_kernel_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    for phase, prefix in (
        ("treewidth.multiply", "treewidth_multiply"),
        ("treewidth.sum_out", "treewidth_sum_out"),
    ):
        values = trace.get(phase)
        if values is None:
            continue
        metrics[f"{prefix}_events"] = values["events"]
        metrics[f"{prefix}_elapsed_ns"] = values["elapsed_ns"]
    return metrics


def rankwidth_kernel_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    for phases, prefix in RANKWIDTH_KERNEL_TRACE_GROUPS:
        matching = [trace[phase] for phase in phases if phase in trace]
        if not matching:
            continue
        metrics[f"{prefix}_events"] = sum(values["events"] for values in matching)
        metrics[f"{prefix}_elapsed_ns"] = sum(values["elapsed_ns"] for values in matching)
        metrics[f"{prefix}_max_items"] = max(values["max_items"] for values in matching)
    return metrics


def branch_skip_reason_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    for phase, key in BRANCH_TREEWIDTH_SKIP_METRICS.items():
        values = trace.get(phase)
        if values is not None:
            metrics[key] = values["events"]
    for phase, key in BRANCH_RANKWIDTH_SKIP_METRICS.items():
        values = trace.get(phase)
        if values is not None:
            metrics[key] = values["events"]
    return metrics


def branch_dispatch_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int]:
    metrics: dict[str, int] = {}
    for phases, prefix, max_key in BRANCH_DISPATCH_TRACE_GROUPS:
        matching = [trace[phase] for phase in phases if phase in trace]
        if not matching:
            continue
        metrics[f"{prefix}_events"] = sum(values["events"] for values in matching)
        metrics[f"{prefix}_elapsed_ns"] = sum(values["elapsed_ns"] for values in matching)
        metrics[f"{prefix}_{max_key}"] = max(values["max_items"] for values in matching)
    return metrics


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
        "max_residual_vars",
        "max_residual_edges",
        "max_residual_components",
        "max_residual_largest_component",
        "max_residual_min_fill_width",
        "max_residual_prefix_cut_rank",
        "cache_entries",
        "cache_canonical_entries",
        "cache_stored_residue_slots",
        "branch_rankwidth_labelled_width",
        "branch_rankwidth_support_width",
        "branch_rankwidth_table_forecast",
        "branch_rankwidth_join_pair_forecast",
        "branch_treewidth_order_width",
        "branch_treewidth_table_forecast",
        "branch_treewidth_join_pair_forecast",
        "branch_component_split_max_components",
        "branch_treewidth_delegate_max_vars",
        "branch_rankwidth_delegate_max_vars",
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
        for stat_key in (
            "cache_lookup_events",
            "cache_lookup_elapsed_ns",
            "cache_store_events",
            "cache_store_elapsed_ns",
            "branch_rankwidth_probe_events",
            "branch_rankwidth_probe_elapsed_ns",
            "branch_rankwidth_labelled_width",
            "branch_rankwidth_support_width",
            "branch_rankwidth_table_forecast",
            "branch_rankwidth_join_pair_forecast",
            "branch_treewidth_order_probe_events",
            "branch_treewidth_order_probe_elapsed_ns",
            "branch_treewidth_order_width",
            "branch_treewidth_table_forecast",
            "branch_treewidth_join_pair_forecast",
            "treewidth_multiply_events",
            "treewidth_multiply_elapsed_ns",
            "treewidth_sum_out_events",
            "treewidth_sum_out_elapsed_ns",
            *RANKWIDTH_KERNEL_METRIC_FIELDS,
            "branch_root_treewidth_delegate_events",
            "branch_root_treewidth_delegate_elapsed_ns",
            "branch_root_treewidth_delegate_max_vars",
            *BRANCH_SKIP_REASON_FIELDS,
            *BRANCH_DISPATCH_METRIC_FIELDS,
        ):
            add_stat(stats_total, stat_key, record.get(stat_key))

        trace_total = entry["trace"]
        for phase, values in record["trace"].items():
            phase_total = trace_total.setdefault(phase, {"events": 0, "items": 0, "max_items": 0, "elapsed_ns": 0})
            phase_total["events"] += values["events"]
            phase_total["items"] += values["items"]
            phase_total["max_items"] = max(phase_total["max_items"], values.get("max_items", 0))
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


def record_rankwidth_kernel_elapsed_ns(record: dict) -> int:
    return sum(
        record_metric(record, field)
        for field in RANKWIDTH_KERNEL_METRIC_FIELDS
        if field.endswith("_elapsed_ns")
    )


def branch_skip_reason_parts(record: dict, fields: tuple[str, ...], prefix: str) -> list[str]:
    parts = []
    for field in fields:
        value = record_metric(record, field)
        if value == 0:
            continue
        label = field.removeprefix(prefix).removesuffix("_events").replace("_", "-")
        parts.append(f"{label}:{value}")
    return parts


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
        "timed_out_records": 0,
        "skipped_qsop_mode_records": 0,
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
        if args.qsop_mode != "all" and header["qsop_mode"] != args.qsop_mode:
            metadata["skipped_qsop_mode_records"] += 1
            continue
        for backend in backends:
            for config in iter_backend_configs(args, backend):
                cmd = [
                    str(args.sop_solve),
                    "--backend",
                    backend,
                    "--format",
                    "stats",
                    "--include-result",
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
                    stats_text, trace_text, solve_elapsed_ns = run_command(
                        cmd, input_text=qsop, timeout_seconds=args.solver_timeout
                    )
                except CommandTimeout as exc:
                    metadata["timed_out_records"] += 1
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
                            "treewidth_order": config["treewidth_order"],
                            "status": "timeout",
                            "error": str(exc),
                            **header,
                            "import_elapsed_ns": import_elapsed_ns,
                            "solve_elapsed_ns": exc.elapsed_ns,
                            "qasm_sha256": sha256_text(qasm),
                            "qsop_sha256": sha256_text(qsop),
                            "stats": {},
                            "trace": {},
                        }
                    )
                    continue
                except RuntimeError as exc:
                    if args.skip_unsupported and backend == "rankwidth" and is_skippable_rankwidth_error(exc):
                        metadata["skipped_rankwidth_records"] += 1
                        continue
                    raise
                stats, amplitude = parse_stats_and_amplitude(stats_text)
                aliases = backend_stat_aliases(backend, stats)
                trace = parse_trace_csv(trace_text) if args.trace else {}
                cache_metrics = cache_record_metrics(stats, trace)
                branch_probe_metrics = branch_rankwidth_probe_metrics(trace)
                treewidth_probe_metrics = branch_treewidth_probe_metrics(trace)
                kernel_metrics = treewidth_kernel_metrics(trace)
                rankwidth_metrics = rankwidth_kernel_metrics(trace)
                skip_reason_metrics = branch_skip_reason_metrics(trace)
                dispatch_metrics = branch_dispatch_metrics(trace)
                trace_metrics = trace_record_metrics(trace)
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
                        "status": "ok",
                        "error": "",
                        **header,
                        "import_elapsed_ns": import_elapsed_ns,
                        "solve_elapsed_ns": solve_elapsed_ns,
                        **amplitude,
                        "qasm_sha256": sha256_text(qasm),
                        "qsop_sha256": sha256_text(qsop),
                        "stats": stats,
                        **aliases,
                        **cache_metrics,
                        **branch_probe_metrics,
                        **treewidth_probe_metrics,
                        **kernel_metrics,
                        **rankwidth_metrics,
                        **skip_reason_metrics,
                        **dispatch_metrics,
                        **trace_metrics,
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
            "cache_avoided_nodes",
            "cache_canonical_hits",
            "cache_entries",
            "cache_canonical_entries",
            "cache_stored_residue_slots",
            "cache_hit_rate_ppm",
            "cache_avoided_node_rate_ppm",
            "cache_lookup_events",
            "cache_lookup_elapsed_ns",
            "cache_store_events",
            "cache_store_elapsed_ns",
            "branch_rankwidth_probe_events",
            "branch_rankwidth_probe_elapsed_ns",
            "branch_rankwidth_labelled_width",
            "branch_rankwidth_support_width",
            "branch_rankwidth_table_forecast",
            "branch_rankwidth_join_pair_forecast",
            "branch_treewidth_order_probe_events",
            "branch_treewidth_order_probe_elapsed_ns",
            "branch_treewidth_order_width",
            "branch_treewidth_table_forecast",
            "branch_treewidth_join_pair_forecast",
            "treewidth_multiply_events",
            "treewidth_multiply_elapsed_ns",
            "treewidth_sum_out_events",
            "treewidth_sum_out_elapsed_ns",
            *RANKWIDTH_KERNEL_METRIC_FIELDS,
            "branch_root_treewidth_delegate_events",
            "branch_root_treewidth_delegate_elapsed_ns",
            "branch_root_treewidth_delegate_max_vars",
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
            "treewidth_delegations",
            "rankwidth_delegations",
            "branch_fallthroughs",
            "branch_treewidth_skips",
            "branch_rankwidth_skips",
            *BRANCH_SKIP_REASON_FIELDS,
            *BRANCH_DISPATCH_METRIC_FIELDS,
            "max_residual_vars",
            "max_residual_edges",
            "max_residual_components",
            "max_residual_largest_component",
            "max_residual_min_fill_width",
            "max_residual_prefix_cut_rank",
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
                f"status={record.get('status', 'ok')} "
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
            if "cache_avoided_nodes" in stats:
                line += f" cache_avoided_nodes={stats['cache_avoided_nodes']}"
            if "cache_avoided_node_rate_ppm" in record:
                line += f" cache_avoided_node_rate_ppm={record['cache_avoided_node_rate_ppm']}"
            if "cache_canonical_hits" in stats:
                line += f" cache_canonical_hits={stats['cache_canonical_hits']}"
            if "cache_entries" in stats:
                line += (
                    f" cache_entries={stats['cache_entries']}"
                    f" cache_canonical_entries={stats.get('cache_canonical_entries', 0)}"
                    f" cache_slots={stats.get('cache_stored_residue_slots', 0)}"
                )
            if "cache_lookup_elapsed_ns" in record:
                line += f" cache_lookup_elapsed_ns={record['cache_lookup_elapsed_ns']}"
            if "cache_store_elapsed_ns" in record:
                line += f" cache_store_elapsed_ns={record['cache_store_elapsed_ns']}"
            if "branch_rankwidth_labelled_width" in record:
                line += (
                    f" branch_rankwidth=labelled:{record['branch_rankwidth_labelled_width']}"
                    f",support:{record.get('branch_rankwidth_support_width', 0)}"
                )
            if (
                "branch_rankwidth_table_forecast" in record
                or "branch_treewidth_table_forecast" in record
            ):
                line += (
                    f" branch_table_forecast=rw:{record.get('branch_rankwidth_table_forecast', 0)}"
                    f",tw:{record.get('branch_treewidth_table_forecast', 0)}"
                )
            if (
                "branch_rankwidth_join_pair_forecast" in record
                or "branch_treewidth_join_pair_forecast" in record
            ):
                line += (
                    " branch_join_pair_forecast="
                    f"rw:{record.get('branch_rankwidth_join_pair_forecast', 0)}"
                    f",tw:{record.get('branch_treewidth_join_pair_forecast', 0)}"
                )
            if "branch_treewidth_order_width" in record:
                line += f" branch_treewidth_order_width={record['branch_treewidth_order_width']}"
            if "treewidth_multiply_elapsed_ns" in record or "treewidth_sum_out_elapsed_ns" in record:
                line += (
                    " treewidth_kernels="
                    f"multiply:{record.get('treewidth_multiply_events', 0)}/"
                    f"{record.get('treewidth_multiply_elapsed_ns', 0)},"
                    f"sum:{record.get('treewidth_sum_out_events', 0)}/"
                    f"{record.get('treewidth_sum_out_elapsed_ns', 0)}"
                )
            if (
                "rankwidth_join_map_elapsed_ns" in record
                or "rankwidth_join_elapsed_ns" in record
                or "rankwidth_labelled_join_map_elapsed_ns" in record
                or "rankwidth_labelled_join_elapsed_ns" in record
                or "rankwidth_fourier_join_map_elapsed_ns" in record
                or "rankwidth_fourier_join_elapsed_ns" in record
            ):
                line += (
                    " rankwidth_kernels="
                    f"map:{record.get('rankwidth_join_map_events', 0)}/"
                    f"{record.get('rankwidth_join_map_elapsed_ns', 0)},"
                    f"join:{record.get('rankwidth_join_events', 0)}/"
                    f"{record.get('rankwidth_join_elapsed_ns', 0)},"
                    f"labelled_map:{record.get('rankwidth_labelled_join_map_events', 0)}/"
                    f"{record.get('rankwidth_labelled_join_map_elapsed_ns', 0)},"
                    f"labelled:{record.get('rankwidth_labelled_join_events', 0)}/"
                    f"{record.get('rankwidth_labelled_join_elapsed_ns', 0)},"
                    f"fourier_map:{record.get('rankwidth_fourier_join_map_events', 0)}/"
                    f"{record.get('rankwidth_fourier_join_map_elapsed_ns', 0)},"
                    f"fourier:{record.get('rankwidth_fourier_join_events', 0)}/"
                    f"{record.get('rankwidth_fourier_join_elapsed_ns', 0)}"
                )
            if "treewidth_delegations" in stats or "rankwidth_delegations" in stats:
                line += (
                    f" delegations={stats.get('treewidth_delegations', 0)}/"
                    f"{stats.get('rankwidth_delegations', 0)}"
                )
            if (
                record_has_metric(record, "branch_component_split_events")
                or record_has_metric(record, "branch_treewidth_delegate_events")
                or record_has_metric(record, "branch_root_treewidth_delegate_events")
                or record_has_metric(record, "branch_rankwidth_delegate_events")
            ):
                line += (
                    " branch_dispatch="
                    f"splits:{record_metric(record, 'branch_component_split_events')}"
                    f",tw:{record_metric(record, 'branch_treewidth_delegate_events')}"
                    f",root_tw:{record_metric(record, 'branch_root_treewidth_delegate_events')}"
                    f",rw:{record_metric(record, 'branch_rankwidth_delegate_events')}"
                )
                if record_has_metric(record, "branch_component_split_max_components"):
                    line += (
                        f",split_max_components:"
                        f"{record_metric(record, 'branch_component_split_max_components')}"
                    )
                if record_has_metric(record, "branch_treewidth_delegate_max_vars"):
                    line += (
                        f",tw_max_vars:"
                        f"{record_metric(record, 'branch_treewidth_delegate_max_vars')}"
                    )
                if record_has_metric(record, "branch_root_treewidth_delegate_max_vars"):
                    line += (
                        f",root_tw_max_vars:"
                        f"{record_metric(record, 'branch_root_treewidth_delegate_max_vars')}"
                    )
                if record_has_metric(record, "branch_rankwidth_delegate_max_vars"):
                    line += (
                        f",rw_max_vars:"
                        f"{record_metric(record, 'branch_rankwidth_delegate_max_vars')}"
                    )
            if (
                "branch_fallthroughs" in stats
                or "branch_treewidth_skips" in stats
                or "branch_rankwidth_skips" in stats
            ):
                line += (
                    f" branch_policy=fallthroughs:{stats.get('branch_fallthroughs', 0)}"
                    f",tw_skips:{stats.get('branch_treewidth_skips', 0)}"
                    f",rw_skips:{stats.get('branch_rankwidth_skips', 0)}"
                )
            treewidth_skip_reasons = branch_skip_reason_parts(
                record,
                tuple(BRANCH_TREEWIDTH_SKIP_METRICS.values()),
                "branch_treewidth_skip_",
            )
            if treewidth_skip_reasons:
                line += f" branch_treewidth_skip_reasons={','.join(treewidth_skip_reasons)}"
            rankwidth_skip_reasons = branch_skip_reason_parts(
                record,
                tuple(BRANCH_RANKWIDTH_SKIP_METRICS.values()),
                "branch_rankwidth_skip_",
            )
            if rankwidth_skip_reasons:
                line += f" branch_rankwidth_skip_reasons={','.join(rankwidth_skip_reasons)}"
            if "max_residual_min_fill_width" in stats or "max_residual_prefix_cut_rank" in stats:
                line += (
                    f" residual_widths=tw:{stats.get('max_residual_min_fill_width', 0)}"
                    f",cutrank:{stats.get('max_residual_prefix_cut_rank', 0)}"
                )
            trace_phase = dominant_trace_phase(record)
            if trace_phase:
                line += f" top_trace={trace_phase}"
            if record.get("status") != "ok" and record.get("error"):
                error = str(record["error"]).replace("\n", " ")
                if len(error) > 160:
                    error = error[:157] + "..."
                line += f" error={error}"
            print(line, file=file)


def record_label(record: dict) -> str:
    return f"{record['source']}:{record['case']} {record['boundary']}"


def metric_value(record: dict, metric: str) -> int | None:
    value = record.get(metric)
    if isinstance(value, int):
        return value
    value = record["stats"].get(metric)
    return value if isinstance(value, int) else None


def write_timeout_overview(records: list[dict], args: argparse.Namespace, file: TextIO) -> None:
    timeouts = [record for record in records if record.get("status") == "timeout"]
    if not timeouts:
        return

    print("timeouts:", file=file)
    for key in sorted({record_summary_key(record) for record in timeouts}):
        selected = [record for record in timeouts if record_summary_key(record) == key]
        for line in format_summary_key(key):
            print(f"  {line}", file=file)
        print(f"    records: {len(selected)}", file=file)
        print(f"    elapsed_ns: {sum(int(record['solve_elapsed_ns']) for record in selected)}", file=file)
        sources: dict[str, int] = {}
        for record in selected:
            source = record.get("source") or "unknown"
            sources[source] = sources.get(source, 0) + 1
        for source in sorted(sources):
            print(f"    source[{source}]: {sources[source]}", file=file)

    if args.timeout_top == 0:
        return
    print("top_timeout_records:", file=file)
    ranked = sorted(
        timeouts,
        key=lambda record: (
            int(record.get("solve_elapsed_ns") or 0),
            int(record.get("nvars") or 0),
            record.get("source") or "",
            record.get("case") or "",
        ),
        reverse=True,
    )
    for record in ranked[: args.timeout_top]:
        error = str(record.get("error") or "").replace("\n", " ")
        if len(error) > 160:
            error = error[:157] + "..."
        print(
            f"  {record['source']}:{record['case']} {record['boundary']} "
            f"backend={record['backend']} nvars={record['nvars']} nedges={record['nedges']} "
            f"elapsed_ns={record['solve_elapsed_ns']} error={error}",
            file=file,
        )


def write_rankwidth_diagnostics(records: list[dict], file: TextIO) -> None:
    rankwidth = [record for record in records if record.get("backend") == "rankwidth"]
    if not rankwidth:
        return

    print("rankwidth_generator_diagnostics:", file=file)
    for key in sorted({record_summary_key(record) for record in rankwidth}):
        selected = [record for record in rankwidth if record_summary_key(record) == key]
        ok = [record for record in selected if record.get("status", "ok") == "ok"]
        timeouts = [record for record in selected if record.get("status") == "timeout"]
        for line in format_summary_key(key):
            print(f"  {line}", file=file)
        print(f"    records: {len(selected)}", file=file)
        print(f"    solved_records: {len(ok)}", file=file)
        print(f"    timed_out_records: {len(timeouts)}", file=file)
        print(f"    solve_elapsed_ns: {sum(int(record['solve_elapsed_ns']) for record in selected)}", file=file)
        for label, metric in (
            ("max_width", "rankwidth_width"),
            ("max_table_entries", "rankwidth_max_table_entries"),
            ("max_signature_entries", "rankwidth_max_signature_entries"),
            ("join_pairs", "join_pairs"),
            ("join_signature_pairs", "join_signature_pairs"),
        ):
            values = [metric_value(record, metric) for record in ok]
            values = [value for value in values if value is not None]
            if not values:
                continue
            value = sum(values) if label in {"join_pairs", "join_signature_pairs"} else max(values)
            print(f"    {label}: {value}", file=file)
        kernel_elapsed = sum(record_rankwidth_kernel_elapsed_ns(record) for record in ok)
        if kernel_elapsed:
            print(f"    rankwidth_kernel_elapsed_ns: {kernel_elapsed}", file=file)
        if ok:
            slowest = max(ok, key=lambda record: int(record["solve_elapsed_ns"]))
            print(
                f"    slowest_ok: {slowest['source']}:{slowest['case']} {slowest['boundary']} "
                f"elapsed_ns={slowest['solve_elapsed_ns']} "
                f"nvars={slowest['nvars']} nedges={slowest['nedges']}",
                file=file,
            )


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
        ("largest_residual_min_fill_width", "max_residual_min_fill_width"),
        ("largest_residual_prefix_cut_rank", "max_residual_prefix_cut_rank"),
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
    solved = [record for record in records if record.get("status", "ok") == "ok"]
    print(f"records: {len(records)}", file=file)
    print(f"case_boundaries: {metadata['case_boundaries']}", file=file)
    print(f"solved_records: {len(solved)}", file=file)
    print(f"skipped_qsop_mode_records: {metadata['skipped_qsop_mode_records']}", file=file)
    print(f"skipped_rankwidth_records: {metadata['skipped_rankwidth_records']}", file=file)
    print(f"timed_out_records: {metadata['timed_out_records']}", file=file)
    print(f"imported_sign: {metadata['imported_sign']}", file=file)
    print(f"imported_labelled: {metadata['imported_labelled']}", file=file)
    source_boundaries = metadata.get("source_boundaries", {})
    if source_boundaries:
        print("sources:", file=file)
        for source in sorted(source_boundaries):
            print(f"  {source}: boundaries={source_boundaries[source]}", file=file)
    write_timeout_overview(records, args, file)
    write_rankwidth_diagnostics(records, file)
    write_largest_overview(records, file)
    summary = summarize_records(solved)
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
            "cache_avoided_nodes",
            "cache_canonical_hits",
            "cache_entries",
            "cache_canonical_entries",
            "cache_stored_residue_slots",
            "cache_lookup_events",
            "cache_lookup_elapsed_ns",
            "cache_store_events",
            "cache_store_elapsed_ns",
            "branch_rankwidth_probe_events",
            "branch_rankwidth_probe_elapsed_ns",
            "branch_rankwidth_labelled_width",
            "branch_rankwidth_support_width",
            "branch_rankwidth_table_forecast",
            "branch_rankwidth_join_pair_forecast",
            "branch_treewidth_order_probe_events",
            "branch_treewidth_order_probe_elapsed_ns",
            "branch_treewidth_order_width",
            "branch_treewidth_table_forecast",
            "branch_treewidth_join_pair_forecast",
            "treewidth_multiply_events",
            "treewidth_multiply_elapsed_ns",
            "treewidth_sum_out_events",
            "treewidth_sum_out_elapsed_ns",
            *RANKWIDTH_KERNEL_METRIC_FIELDS,
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
            "treewidth_delegations",
            "rankwidth_delegations",
            "branch_fallthroughs",
            "branch_treewidth_skips",
            "branch_rankwidth_skips",
            *BRANCH_SKIP_REASON_FIELDS,
            *BRANCH_DISPATCH_METRIC_FIELDS,
            "max_residual_vars",
            "max_residual_edges",
            "max_residual_components",
            "max_residual_largest_component",
            "max_residual_min_fill_width",
            "max_residual_prefix_cut_rank",
        ):
            if key in stats:
                print(f"  {key}: {stats[key]}", file=file)
        if "cache_hits" in stats or "cache_misses" in stats:
            print(f"  cache_hit_rate: {cache_hit_rate(stats)}", file=file)

        trace = entry["trace"]
        if trace:
            print(f"  trace_elapsed_ns: {trace_elapsed_ns(trace)}", file=file)
            top_phases = trace_top_phase_text(trace)
            if top_phases:
                print(f"  trace_top: {top_phases}", file=file)
            print("  trace:", file=file)
            for phase in sorted(trace):
                values = trace[phase]
                print(
                    f"    {phase}: events={values['events']} "
                    f"items={values['items']} max_items={values.get('max_items', 0)} "
                    f"elapsed_ns={values['elapsed_ns']}",
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
    parser.add_argument(
        "--qsop-mode",
        choices=("all", "sign", "labelled"),
        default="all",
        help="Only solve imported QSOP rows with this mode. Imports still count in summary metadata.",
    )
    parser.add_argument("--solver-timeout", type=float, help="Per-solve timeout in seconds.")
    parser.add_argument("--trace", action="store_true", help="Collect and summarize sop-solve CSV trace rows.")
    parser.add_argument(
        "--timeout-top",
        type=int,
        default=5,
        help="With --format summary, print this many timed-out case-boundary rows.",
    )
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
        "--rankwidth-diagnostics",
        action="store_true",
        help="Convenience mode: rankwidth count-table sweep with trace and timeout-aware summary rows.",
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
    if args.solver_timeout is not None and args.solver_timeout <= 0.0:
        parser.error("--solver-timeout must be positive")
    if args.top < 0:
        parser.error("--top must be non-negative")
    if args.timeout_top < 0:
        parser.error("--timeout-top must be non-negative")
    if args.rankwidth_diagnostics:
        args.backends = ["rankwidth"]
        args.rankwidth_generators = list(RANKWIDTH_GENERATORS)
        args.rankwidth_modes = ["count-table"]
        args.skip_unsupported = True
        args.trace = True
    elif args.rankwidth_sweep:
        args.rankwidth_generators = list(RANKWIDTH_GENERATORS)
        args.rankwidth_modes = list(RANKWIDTH_MODES)
    else:
        args.rankwidth_generators = args.rankwidth_generators or ["left-deep"]
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
