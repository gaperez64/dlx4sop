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
import pathlib
import sys

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
    cmd = [str(sop_solve)] + BACKEND_CONFIGS[cfg_key] + extra_args + [str(case.qsop_path)]
    result = run_command(cmd, timeout_seconds=timeout)

    if result.returncode == -1 and result.stderr == "timeout":
        return {"status": "timeout", "elapsed_ns": result.elapsed_ns}
    if result.returncode != 0:
        return {
            "status": "error",
            "elapsed_ns": result.elapsed_ns,
            "stderr": result.stderr[:300],
        }
    return {
        "status": "ok",
        "elapsed_ns": result.elapsed_ns,
        "counts_hash": counts_hash(result.stdout),
    }


def bench_case(
    sop_solve: pathlib.Path,
    case: CorpusCase,
    backends: list[str],
    timeout: float,
    extra_args: list[str],
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

        # Gate: skip if instance is too large for this backend
        max_vars = _backend_max_vars(backend)
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
) -> dict:
    meta = case.meta
    provenance = meta.get("provenance", {
        "kind": "generated",
        "generator": meta.get("generator", "build_sop_corpus.py:v1"),
        "source": "local-synthetic",
        "source_url": None,
        "source_relative_path": None,
        "case": None,
        "boundary": None,
        "input": None,
        "output": None,
        "seed": meta.get("seed"),
    })
    rec: dict = {
        "schema": "sop_bench_result_v2",
        "suite": "local-sop",
        "source": "local-synthetic",
        "tier": case.tier,
        "instance_id": case.instance_id,
        "qsop_path": str(case.qsop_path),
        "qasm_path": None,
        "provenance": provenance,
        "runner_kind": "local",
        "runner_name": "sop-solve",
        "backend": backend,
        "backend_config": _backend_config_dict(backend),
        "status": status,
        "elapsed_ns": elapsed_ns,
        "solve_elapsed_ns": elapsed_ns,
        "import_elapsed_ns": 0,
        "nvars": nvars,
        "nedges": nedges,
        "r": r,
        "counts_hash": counts_hash,
        "amplitude_hash": "",
        "stats": {},
    }
    if stderr:
        rec["error_detail"] = stderr
    if skip_reason:
        rec["skip_reason"] = skip_reason
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
                    args.sop_solve, case, backends, args.timeout, extra_args
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
