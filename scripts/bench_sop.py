#!/usr/bin/env python3
"""Run local SOP backend benchmarks without external tools.

Benchmarks sop-solve across tiers and backends using the local QSOP corpus
in benchmarks/corpus/sop/.  No Ganak, no MQT Bench, no native simulator.

Usage:
    python3 scripts/bench_sop.py [--sop-solve PATH] [--corpus-dir DIR]
        [--tier TIER] [--backend BACKEND] [--output JSONL]

Outputs one JSONL record per (instance, backend) pair to stdout or --output.
Each record includes: name, tier, nvars, nedges, r, backend, solve_elapsed_ns,
status, counts_hash.
"""

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop"
DEFAULT_SOP_SOLVE = REPO_ROOT / "build" / "sop-solve"

BACKENDS = ["components", "brute-force", "branch", "treewidth"]
BRANCH_HEURISTICS = ["split", "treewidth", "cutrank-proxy"]
BRANCH_RW_SOURCES = ["native", "from-treewidth"]

# Maximum variable count per backend to avoid timeouts.
BACKEND_MAX_VARS = {
    "brute-force": 22,
    "components":  22,
    "branch":      64,
    "treewidth":  256,
}


def load_meta(qsop_path: pathlib.Path) -> dict:
    meta_path = qsop_path.with_suffix(".meta.json")
    if meta_path.exists():
        with open(meta_path, encoding="utf-8") as f:
            return json.load(f)
    return {}


def counts_hash(output: str) -> str:
    counts_line = next((l for l in output.splitlines() if l.startswith("counts ")), "")
    return hashlib.sha1(counts_line.encode()).hexdigest()[:12]


def run_backend(sop_solve: pathlib.Path, qsop: pathlib.Path, backend: str,
                extra_args: list[str], timeout: float) -> dict:
    cmd = [str(sop_solve), "--backend", backend] + extra_args + [str(qsop)]
    t0 = time.monotonic_ns()
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        elapsed_ns = time.monotonic_ns() - t0
        if result.returncode == 0:
            return {"status": "ok", "elapsed_ns": elapsed_ns,
                    "counts_hash": counts_hash(result.stdout)}
        return {"status": "error", "elapsed_ns": elapsed_ns,
                "stderr": result.stderr[:200]}
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "elapsed_ns": int(timeout * 1e9)}


def bench_instance(sop_solve: pathlib.Path, qsop: pathlib.Path, meta: dict,
                   tier_name: str, backends: list[str], timeout: float) -> list[dict]:
    nvars = meta.get("nvars", 0)
    nedges = meta.get("nedges", 0)
    r = meta.get("r", 8)
    name = meta.get("name", qsop.stem)
    records = []

    for backend in backends:
        max_vars = BACKEND_MAX_VARS.get(backend, 64)
        if nvars > max_vars:
            continue
        result = run_backend(sop_solve, qsop, backend, [], timeout)
        record = {
            "name": name, "tier": tier_name, "nvars": nvars, "nedges": nedges,
            "r": r, "backend": backend,
            "solve_elapsed_ns": result.get("elapsed_ns", 0),
            "status": result["status"],
            "counts_hash": result.get("counts_hash", ""),
        }
        records.append(record)

    # Branch with from-treewidth rw_source.
    if "branch" in backends and nvars <= BACKEND_MAX_VARS["branch"]:
        result = run_backend(sop_solve, qsop, "branch",
                             ["--branch-rw-source", "from-treewidth"], timeout)
        record = {
            "name": name, "tier": tier_name, "nvars": nvars, "nedges": nedges,
            "r": r, "backend": "branch:from-treewidth",
            "solve_elapsed_ns": result.get("elapsed_ns", 0),
            "status": result["status"],
            "counts_hash": result.get("counts_hash", ""),
        }
        records.append(record)

    return records


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sop-solve", type=pathlib.Path, default=DEFAULT_SOP_SOLVE)
    parser.add_argument("--corpus-dir", type=pathlib.Path, default=DEFAULT_CORPUS)
    parser.add_argument("--tier", help="run only this tier (e.g. tier-1-8)")
    parser.add_argument("--backend", action="append", dest="backends",
                        help="backends to benchmark (default: all)")
    parser.add_argument("--timeout", type=float, default=10.0,
                        help="per-run timeout in seconds (default: 10)")
    parser.add_argument("--output", type=pathlib.Path, help="write JSONL to this file")
    args = parser.parse_args()

    if not args.sop_solve.exists():
        print(f"error: sop-solve not found at {args.sop_solve}", file=sys.stderr)
        print("  run: meson compile -C build", file=sys.stderr)
        return 1

    backends = args.backends or BACKENDS
    out_file = open(args.output, "w", encoding="utf-8") if args.output else sys.stdout

    try:
        tier_dirs = sorted(args.corpus_dir.iterdir())
        for tier_dir in tier_dirs:
            if not tier_dir.is_dir():
                continue
            if args.tier and tier_dir.name != args.tier:
                continue
            for qsop in sorted(tier_dir.glob("*.qsop")):
                meta = load_meta(qsop)
                records = bench_instance(args.sop_solve, qsop, meta, tier_dir.name,
                                         backends, args.timeout)
                for record in records:
                    print(json.dumps(record), file=out_file)
                    out_file.flush()
                    print(f"  {record['name']:50s} {record['backend']:25s}"
                          f" {record['status']:8s} {record['solve_elapsed_ns'] // 1_000_000}ms",
                          file=sys.stderr)
    finally:
        if args.output:
            out_file.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
