#!/usr/bin/env python3
"""Local SOP corpus benchmark runner — no external tools required.

Runs committed .qsop files from the local corpus through sop-solve backends
and emits sop_bench_result_v2 JSONL records.

Usage:
    python3 tools/bench_sop_local.py \\
        --sop-solve build/sop-solve \\
        --corpus-dir benchmarks/corpus/sop \\
        --tier tier-17-32 \\
        --backend treewidth \\
        --backend rankwidth:from-treewidth \\
        --backend branch:auto \\
        --timeout 5 \\
        --out artifacts/local.jsonl

Backend variants:
    components, brute-force, treewidth
    rankwidth:from-treewidth, rankwidth:best, rankwidth:validate
    branch:auto, branch:native, branch:from-treewidth, branch:no-rankwidth
"""

from __future__ import annotations

import argparse
import cmath
import math
import pathlib
import sys
from typing import Any

from bench_common import (
    REPO_ROOT,
    DEFAULT_CORPUS,
    CommandResult,
    CorpusCase,
    counts_hash,
    iter_qsop_corpus,
    render_bar,
    run_command,
    tier_label,
    tier_sort_key,
    write_jsonl_record,
)

RANKWIDTH_KERNEL_TRACE_GROUPS = (
    (("rankwidth.join_map", "rankwidth.crt_join_map"), "rankwidth_join_map"),
    (("rankwidth.join", "rankwidth.crt_join"), "rankwidth_join"),
    (("rankwidth.fourier_join_map",), "rankwidth_fourier_join_map"),
    (("rankwidth.fourier_join",), "rankwidth_fourier_join"),
)

# ---------------------------------------------------------------------------
# Backend configurations
# ---------------------------------------------------------------------------

BACKEND_CONFIGS: dict[str, list[str]] = {
    "components": [
        "--backend", "components",
        "--max-vars", "22",
    ],
    "brute-force": [
        "--backend", "brute-force",
        "--max-vars", "22",
    ],
    "treewidth": [
        "--backend", "treewidth",
        "--treewidth-order", "min-fill-max-degree",
        "--max-vars", "256",
    ],
    "rankwidth:from-treewidth": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "from-treewidth",
        "--max-vars", "256",
    ],
    "rankwidth:best": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "best",
        "--max-vars", "256",
    ],
    "rankwidth:from-treewidth:fourier": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "from-treewidth",
        "--rankwidth-mode", "fourier",
        "--max-vars", "256",
    ],
    "rankwidth:best:fourier": [
        "--backend", "rankwidth",
        "--rankwidth-generate", "best",
        "--rankwidth-mode", "fourier",
        "--max-vars", "256",
    ],
    "branch:auto": [
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "auto",
        "--max-vars", "64",
    ],
    "branch:native": [
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "native",
        "--max-vars", "64",
    ],
    "branch:from-treewidth": [
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "from-treewidth",
        "--max-vars", "64",
    ],
    "branch:no-rankwidth": [
        "--backend", "branch",
        "--branch-heuristic", "split",
        "--branch-rw-source", "none",
        "--max-vars", "64",
    ],
}

# Hard var limits per backend family (gate before invoking sop-solve).
# The bare "rankwidth" and "branch" keys serve as family-prefix fallbacks in
# _backend_max_vars() for unknown variants (e.g. "rankwidth:new-mode" → 256).
BACKEND_MAX_VARS: dict[str, int] = {
    "components":         22,
    "brute-force":        22,
    "treewidth":         256,
    "rankwidth":         256,  # family fallback
    "rankwidth:from-treewidth": 256,
    "rankwidth:best":    256,
    "rankwidth:from-treewidth:fourier": 256,
    "rankwidth:best:fourier": 256,
    "rankwidth:validate": 256,
    "branch":             64,  # family fallback
    "branch:auto":        64,
    "branch:native":      64,
    "branch:from-treewidth": 64,
    "branch:no-rankwidth": 64,
}

DEFAULT_BACKENDS = [
    "treewidth",
    "rankwidth:from-treewidth",
    "rankwidth:best",
    "branch:from-treewidth",
    "branch:auto",
    "branch:no-rankwidth",
]


def _backend_config_key(backend: str) -> str:
    """Return the key in BACKEND_CONFIGS for a backend name (handles user overrides)."""
    return backend


def _backend_max_vars(backend: str) -> int:
    family = backend.split(":")[0]
    if backend in BACKEND_MAX_VARS:
        return BACKEND_MAX_VARS[backend]
    if family in BACKEND_MAX_VARS:
        return BACKEND_MAX_VARS[family]
    return 64


def run_backend(
    sop_solve: pathlib.Path,
    case: CorpusCase,
    backend: str,
    extra_args: list[str],
    timeout: float,
) -> dict:
    """Run sop-solve for a single (instance, backend) pair; return a result dict."""
    cfg_key = _backend_config_key(backend)
    if cfg_key not in BACKEND_CONFIGS:
        return {
            "status": "error",
            "elapsed_ns": 0,
            "stderr": f"unknown backend variant '{backend}'",
        }

    # Apply user extra_args after config (allows --max-vars override etc.)
    cmd = (
        [str(sop_solve)]
        + BACKEND_CONFIGS[cfg_key]
        + extra_args
        + ["--format", "stats", "--include-result", "--trace", "csv", str(case.qsop_path)]
    )
    result = run_command(cmd, timeout_seconds=timeout)

    if result.returncode == -1 and result.stderr == "timeout":
        return {"status": "timeout", "elapsed_ns": result.elapsed_ns}
    if result.returncode != 0:
        return {
            "status": "error",
            "elapsed_ns": result.elapsed_ns,
            "stderr": result.stderr[:300],
        }
    stats, result_hash, amplitude = parse_stats_result_and_amplitude(result.stdout)
    trace = parse_trace_csv(result.stderr)
    fields: dict[str, Any] = {}
    fields.update(backend_stat_aliases(stats))
    fields.update(trace_record_metrics(trace))
    fields.update(rankwidth_kernel_metrics(trace))
    return {
        "status": "ok",
        "elapsed_ns": result.elapsed_ns,
        "counts_hash": result_hash,
        "stats": stats,
        **amplitude,
        **fields,
    }


def amplitude_metrics(modulus: int, norm_h: int, counts: list[int]) -> dict[str, float]:
    omega = cmath.exp(2j * math.pi / modulus)
    if modulus % 2 == 0 and len(counts) == modulus:
        half = modulus // 2
        total = sum(
            (counts[residue] - counts[residue + half]) * (omega**residue)
            for residue in range(half)
        )
    else:
        total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    amplitude = total * (2.0 ** (-norm_h / 2.0))
    return {
        "amplitude_real": amplitude.real,
        "amplitude_imag": amplitude.imag,
    }


def parse_stats_result_and_amplitude(
    text: str,
) -> tuple[dict[str, int | str], str, dict[str, float]]:
    stats: dict[str, int | str] = {}
    result_counts = ""
    result_count_values: list[int] | None = None
    result_modulus: int | None = None
    result_norm_h: int | None = None
    string_keys = {
        "backend",
        "branch_heuristic",
        "rankwidth_mode",
        "rankwidth_fourier_kernel",
        "rankwidth_decomposition",
        "treewidth_order",
        "solve_mode",
        "solve_mode_kernel",
    }
    for line in text.splitlines():
        if ": " not in line:
            continue
        key, value = line.split(": ", 1)
        if key == "result_counts":
            result_counts = value
            result_count_values = [int(part) for part in value.split()]
            continue
        if key == "result_modulus":
            result_modulus = int(value)
            continue
        if key == "result_norm_h":
            result_norm_h = int(value)
            continue
        if key == "result_probability":
            continue
        stats[key] = value if key in string_keys else int(value)
    amplitude: dict[str, float] = {}
    if (
        result_modulus is not None
        and result_norm_h is not None
        and result_count_values is not None
    ):
        amplitude = amplitude_metrics(result_modulus, result_norm_h, result_count_values)
    result_hash = counts_hash(f"counts {result_counts}") if result_counts else ""
    return stats, result_hash, amplitude


def parse_trace_csv(text: str) -> dict[str, dict[str, int]]:
    rows = [line.strip() for line in text.splitlines() if line.strip()]
    if not rows or rows[0] != "phase,depth,items,elapsed_ns":
        return {}
    summary: dict[str, dict[str, int]] = {}
    for row in rows[1:]:
        parts = row.split(",", 3)
        if len(parts) != 4:
            continue
        phase, _depth, items, elapsed_ns = parts
        try:
            item_count = int(items)
            elapsed = int(elapsed_ns)
        except ValueError:
            continue
        entry = summary.setdefault(phase, {"events": 0, "items": 0, "max_items": 0, "elapsed_ns": 0})
        entry["events"] += 1
        entry["items"] += item_count
        entry["max_items"] = max(entry["max_items"], item_count)
        entry["elapsed_ns"] += elapsed
    return summary


def trace_record_metrics(trace: dict[str, dict[str, int]]) -> dict[str, int | str]:
    total = sum(values["elapsed_ns"] for values in trace.values())
    if total <= 0:
        return {}
    phase, values = sorted(trace.items(), key=lambda item: (-item[1]["elapsed_ns"], item[0]))[0]
    elapsed = values["elapsed_ns"]
    return {
        "trace_elapsed_ns": total,
        "trace_top_phase": phase,
        "trace_top_elapsed_ns": elapsed,
        "trace_top_share_ppm": (elapsed * 1_000_000) // total,
    }


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


def backend_stat_aliases(stats: dict[str, int | str]) -> dict[str, int | str]:
    backend = str(stats.get("backend", ""))
    aliases: dict[str, int | str] = {}
    for key in ("rankwidth_mode", "rankwidth_fourier_kernel", "rankwidth_decomposition",
                "treewidth_order"):
        if key in stats:
            aliases[key] = stats[key]
    width = stats.get("decomposition_width")
    if isinstance(width, int):
        aliases["decomposition_width"] = width
        if backend == "rankwidth":
            aliases["rankwidth_width"] = width
        elif backend == "treewidth":
            aliases["treewidth_width"] = width
    table_entries = stats.get("table_entries")
    max_table_entries = stats.get("max_table_entries")
    if isinstance(table_entries, int):
        aliases["table_entries"] = table_entries
        if backend == "rankwidth":
            aliases["rankwidth_table_entries"] = table_entries
        elif backend == "treewidth":
            aliases["treewidth_table_entries"] = table_entries
    if isinstance(max_table_entries, int):
        aliases["max_table_entries"] = max_table_entries
        if backend == "rankwidth":
            aliases["rankwidth_max_table_entries"] = max_table_entries
        elif backend == "treewidth":
            aliases["treewidth_max_table_entries"] = max_table_entries
    for key in (
        "rankwidth_cutrank_width",
        "rankwidth_table_forecast",
        "rankwidth_join_pair_forecast",
        "rankwidth_dense_table_forecast",
        "rankwidth_dense_even_join_forecast",
        "signature_entries",
        "max_signature_entries",
        "join_pairs",
        "join_signature_pairs",
    ):
        value = stats.get(key)
        if isinstance(value, int):
            aliases[key] = value
    if backend == "rankwidth":
        if isinstance(stats.get("signature_entries"), int):
            aliases["rankwidth_signature_entries"] = stats["signature_entries"]
        if isinstance(stats.get("max_signature_entries"), int):
            aliases["rankwidth_max_signature_entries"] = stats["max_signature_entries"]
    return aliases


def bench_case(
    sop_solve: pathlib.Path,
    case: CorpusCase,
    backends: list[str],
    timeout: float,
    extra_args: list[str],
    max_vars_override: int | None = None,
) -> list[dict]:
    meta = case.meta
    nvars = meta.get("nvars", 0)
    nedges = meta.get("nedges", 0)
    r = meta.get("r", 8)
    skip_set = set(meta.get("skip_backends", []))
    known_hard = meta.get("known_hard", False)

    records = []
    for backend in backends:
        # Gate: skip if metadata says so (match exact name or family prefix)
        family = backend.split(":")[0]
        if backend in skip_set or family in skip_set:
            records.append(_make_record(
                case, backend, nvars, nedges, r,
                status="skipped",
                skip_reason=meta.get("skip_reason", "listed in skip_backends"),
            ))
            continue

        # Gate: skip if instance is too large for this backend. A CLI --max-vars override
        # raises the gate so large instances are attempted (and may time out) rather than
        # skipped — required for the scaling study.
        max_vars = _backend_max_vars(backend)
        if max_vars_override is not None:
            max_vars = max(max_vars, max_vars_override)
        if nvars > max_vars:
            reason = f"nvars={nvars} > max_vars={max_vars} for {backend}"
            if known_hard:
                reason = meta.get("skip_reason", reason)
            records.append(_make_record(
                case, backend, nvars, nedges, r,
                status="skipped",
                skip_reason=reason,
            ))
            continue

        result = run_backend(sop_solve, case, backend, extra_args, timeout)
        rec = _make_record(case, backend, nvars, nedges, r, **result)
        records.append(rec)

    return records


def _qsop_mode(case: CorpusCase) -> str:
    mode = case.meta.get("qsop_mode")
    if mode == "sign":
        return "sign"
    if mode is not None:
        return "unknown"
    try:
        for line in case.qsop_path.read_text().splitlines():
            words = line.split()
            if words[:2] == ["p", "qsop-sign"]:
                return "sign"
            if words[:1] == ["p"]:
                return "unknown"
    except OSError:
        pass
    return "unknown"


def _make_record(
    case: CorpusCase,
    backend: str,
    nvars: int,
    nedges: int,
    r: int,
    *,
    status: str,
    elapsed_ns: int = 0,
    counts_hash: str = "",
    stderr: str = "",
    skip_reason: str = "",
    stats: dict[str, int | str] | None = None,
    **fields: Any,
) -> dict:
    meta = case.meta
    meta_source = meta.get("source", "local-synthetic")
    provenance = meta.get("provenance", {
        "kind": "generated",
        "generator": meta.get("generator", "build_sop_corpus.py:v1"),
        "source": meta_source,
        "source_url": meta.get("source_url"),
        "source_relative_path": None,
        "case": None,
        "boundary": None,
        "input": None,
        "output": None,
        "seed": meta.get("seed"),
    })
    # Boundary identity for native-comparison matching (render_scoreboard.comparison_key).
    # Materialized corpora (MQT, synthetic) carry the boundary + provenance in the meta sidecar;
    # construct the same (case, input, output) the native simulator records use.
    if "case" in meta:
        case_name = meta["case"]
    elif "mqt_algorithm" in meta:
        case_name = f"{meta.get('mqt_algorithm')}-{meta.get('mqt_qubits')}q-opt{meta.get('mqt_optimization_level')}"
    else:
        case_name = case.instance_id
    rec: dict = {
        "schema": "sop_bench_result_v2",
        "suite": "local-sop",
        "source": meta_source,
        "source_relative_path": meta.get("source_relative_path", ""),
        "case": case_name,
        "input": meta.get("boundary_input", ""),
        "output": meta.get("boundary_output", ""),
        "tier": case.tier,
        "instance_id": case.instance_id,
        "qsop_path": str(case.qsop_path),
        "qasm_path": None,
        "provenance": provenance,
        "runner_kind": "local",
        "runner_name": "sop-solve",
        "backend": backend,
        "backend_config": _backend_config_dict(backend),
        "qsop_mode": _qsop_mode(case),
        "status": status,
        "elapsed_ns": elapsed_ns,
        "solve_elapsed_ns": elapsed_ns,
        "import_elapsed_ns": 0,
        "nvars": nvars,
        "nedges": nedges,
        "r": r,
        "counts_hash": counts_hash,
        "amplitude_hash": "",
        "stats": stats or {},
    }
    if stderr:
        rec["error_detail"] = stderr
    if skip_reason:
        rec["skip_reason"] = skip_reason
    rec.update(fields)
    return rec


def _backend_config_dict(backend: str) -> dict:
    key = _backend_config_key(backend)
    cfg = BACKEND_CONFIGS.get(key, [])
    d: dict = {}
    it = iter(cfg)
    for tok in it:
        if tok.startswith("--"):
            k = tok.lstrip("--").replace("-", "_")
            try:
                v = next(it)
                d[k] = v
            except StopIteration:
                d[k] = True
    return d


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_progress(case: CorpusCase, backend: str, rec: dict) -> None:
    elapsed_ms = rec.get("elapsed_ns", 0) // 1_000_000
    status = rec.get("status", "?")
    skip_reason = rec.get("skip_reason", "")
    suffix = f" ({skip_reason})" if skip_reason else ""
    print(
        f"  {case.instance_id:52s} {backend:30s} {status:8s} {elapsed_ms:6d}ms{suffix}",
        file=sys.stderr,
    )


def summarize(all_records: list[dict]) -> None:
    """Print a brief geomean summary per (tier, backend) to stderr."""
    import collections
    import math

    buckets: dict[tuple[str, str], list[int]] = collections.defaultdict(list)
    for rec in all_records:
        if rec.get("status") == "ok":
            buckets[(rec["tier"], rec["backend"])].append(rec["elapsed_ns"])

    tiers = sorted({r["tier"] for r in all_records}, key=lambda t: tier_sort_key(t))
    backends = list(dict.fromkeys(r["backend"] for r in all_records))

    print("\n--- Summary (geomean wall time) ---", file=sys.stderr)
    for tier in tiers:
        ok_by_backend = {b: buckets[(tier, b)] for b in backends if buckets[(tier, b)]}
        if not ok_by_backend:
            continue
        best_ns = min(
            math.exp(sum(math.log(v) for v in ns) / len(ns))
            for ns in ok_by_backend.values()
        )
        print(f"\nTier {tier_label(tier)}:", file=sys.stderr)
        for bk in backends:
            ns_list = buckets[(tier, bk)]
            if not ns_list:
                n_skip = sum(
                    1 for r in all_records
                    if r["tier"] == tier and r["backend"] == bk
                    and r.get("status") == "skipped"
                )
                n_err = sum(
                    1 for r in all_records
                    if r["tier"] == tier and r["backend"] == bk
                    and r.get("status") in ("error", "timeout")
                )
                note = f"skip={n_skip}" if n_skip else f"err/to={n_err}" if n_err else "—"
                print(f"  {bk:35s} {note}", file=sys.stderr)
                continue
            gm_ns = math.exp(sum(math.log(v) for v in ns_list) / len(ns_list))
            gm_ms = gm_ns / 1e6
            ratio = gm_ns / best_ns
            bar = render_bar(ratio - 1, width=30)
            print(f"  {bk:35s} {bar:<32s} {gm_ms:7.3f} ms  {ratio:.2f}x",
                  file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sop-solve",
        type=pathlib.Path,
        default=REPO_ROOT / "build" / "sop-solve",
    )
    parser.add_argument(
        "--corpus-dir",
        type=pathlib.Path,
        action="append",
        dest="corpus_dirs",
        metavar="DIR",
        help="corpus root (may be repeated for multiple roots)",
    )
    parser.add_argument(
        "--tier",
        action="append",
        dest="tiers",
        metavar="TIER",
        help="restrict to this tier (may be repeated)",
    )
    parser.add_argument(
        "--backend",
        action="append",
        dest="backends",
        metavar="BACKEND",
        help="backend variant to benchmark (may be repeated; default: all defaults)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="per-run timeout in seconds (default: 10)",
    )
    parser.add_argument(
        "--max-vars",
        type=int,
        default=None,
        help="override --max-vars for all backends",
    )
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=None,
        help="write JSONL to this file (default: stdout)",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="suppress per-run progress output",
    )
    args = parser.parse_args()

    if not args.sop_solve.exists():
        print(f"error: sop-solve not found at {args.sop_solve}", file=sys.stderr)
        print("  run: meson compile -C build", file=sys.stderr)
        return 1

    corpus_dirs: list[pathlib.Path] = args.corpus_dirs or [DEFAULT_CORPUS]
    backends: list[str] = args.backends or DEFAULT_BACKENDS
    tiers_filter: set[str] | None = set(args.tiers) if args.tiers else None
    extra_args: list[str] = []
    if args.max_vars is not None:
        extra_args = ["--max-vars", str(args.max_vars)]

    # Validate backends
    unknown = [b for b in backends if b not in BACKEND_CONFIGS]
    if unknown:
        print(f"error: unknown backend(s): {unknown}", file=sys.stderr)
        print(f"  known: {sorted(BACKEND_CONFIGS)}", file=sys.stderr)
        return 1

    out_stream = open(args.out, "w", encoding="utf-8") if args.out else sys.stdout
    all_records: list[dict] = []

    try:
        for corpus_dir in corpus_dirs:
            if not corpus_dir.exists():
                print(f"warning: corpus dir not found: {corpus_dir}", file=sys.stderr)
                continue
            for case in iter_qsop_corpus(corpus_dir, tiers=tiers_filter):
                records = bench_case(
                    args.sop_solve, case, backends, args.timeout, extra_args,
                    max_vars_override=args.max_vars,
                )
                for rec in records:
                    write_jsonl_record(out_stream, rec)
                    all_records.append(rec)
                    if not args.quiet:
                        print_progress(case, rec["backend"], rec)
    finally:
        if args.out:
            out_stream.close()

    if not args.quiet:
        summarize(all_records)

    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
    sys.exit(main())
