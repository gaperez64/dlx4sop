#!/usr/bin/env python3
"""MQT QSOP structure profiler.

Reads the materialized MQT QSOP corpus and produces per-row structure profiles.
Identifies high-width, high-expansion, and high-memory-risk MQT families.

Usage:
    python3 tools/profile_mqt_qsop.py \\
        --corpus-dir benchmarks/corpus/sop/materialized-external/mqt-bench \\
        --artifact-dir artifacts/mqt \\
        --sop-solve build/sop-solve
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

BACKENDS = ["treewidth", "branch:auto", "branch:no-rankwidth"]
BRANCH_CONFIGS = {
    "branch:auto": ["--backend", "branch", "--branch-heuristic", "split",
                    "--branch-rw-source", "auto", "--max-vars", "256"],
    "branch:no-rankwidth": ["--backend", "branch", "--branch-heuristic", "split",
                             "--branch-rw-source", "none", "--max-vars", "256"],
    "treewidth": ["--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
                  "--max-vars", "256"],
}


def _run_sop_solve(sop_solve: pathlib.Path, qsop_path: pathlib.Path,
                   args_extra: list[str], timeout: float) -> dict:
    try:
        result = subprocess.run(
            [str(sop_solve)] + args_extra + ["--format", "stats", str(qsop_path)],
            capture_output=True, timeout=timeout,
        )
        elapsed_ns = 0
        stats = {}
        for line in result.stdout.decode(errors="replace").splitlines():
            if "elapsed_ns:" in line:
                try:
                    elapsed_ns = int(line.split(":", 1)[1].strip())
                except ValueError:
                    pass
            if ":" in line:
                k, _, v = line.partition(":")
                k = k.strip().lower().replace(" ", "_").replace("-", "_")
                v = v.strip()
                try:
                    stats[k] = int(v)
                except ValueError:
                    stats[k] = v
        status = "ok" if result.returncode == 0 else "error"
        return {"status": status, "elapsed_ns": elapsed_ns, "stats": stats}
    except subprocess.TimeoutExpired:
        return {"status": "timeout", "elapsed_ns": 0, "stats": {}}
    except Exception as exc:
        return {"status": "error", "elapsed_ns": 0, "stats": {}, "error": str(exc)}


def profile_row(
    meta: dict,
    qsop_path: pathlib.Path,
    sop_solve: pathlib.Path,
    timeout: float,
) -> dict:
    row = {
        "mqt_family": meta.get("mqt_family", ""),
        "mqt_algorithm": meta.get("mqt_algorithm", ""),
        "mqt_qubits": meta.get("mqt_qubits", 0),
        "boundary_input": meta.get("boundary_input", ""),
        "boundary_output": meta.get("boundary_output", ""),
        "imported_nvars": meta.get("nvars", 0),
        "nedges": meta.get("nedges", 0),
        "r": meta.get("r", 8),
        "qsop_mode": meta.get("qsop_mode", ""),
        "treewidth_probe": meta.get("treewidth_probe", 0),
        "prefix_cut_rank": meta.get("prefix_cut_rank", 0),
        "component_count": meta.get("component_count", 1),
        "largest_component": meta.get("largest_component", 0),
        "qsop_path": str(qsop_path),
    }

    nvars = row["imported_nvars"]
    nqubits = row["mqt_qubits"]
    row["vars_per_qubit"] = (nvars / nqubits) if nqubits > 0 else 0
    row["edges_per_var"] = (row["nedges"] / nvars) if nvars > 0 else 0

    best_ns = None
    best_backend = None

    for backend, cfg in BRANCH_CONFIGS.items():
        result = _run_sop_solve(sop_solve, qsop_path, cfg, timeout)
        key_time = f"{backend.replace(':', '_').replace('-', '_')}_time"
        key_status = f"{backend.replace(':', '_').replace('-', '_')}_status"
        row[key_time] = result["elapsed_ns"]
        row[key_status] = result["status"]
        if result["status"] == "ok":
            if best_ns is None or result["elapsed_ns"] < best_ns:
                best_ns = result["elapsed_ns"]
                best_backend = backend

    row["best_backend"] = best_backend
    row["best_backend_time"] = best_ns or 0

    # Infer memory status from rankwidth stats
    stats_rw = row.get("rankwidth_status", "unknown")
    row["rankwidth_memory_status"] = stats_rw
    row["status"] = "ok" if any(
        row.get(f"{b.replace(':', '_').replace('-', '_')}_status") == "ok"
        for b in BRANCH_CONFIGS
    ) else "all-failed"

    return row


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus-dir", type=pathlib.Path,
                        default=REPO_ROOT / "benchmarks" / "corpus" / "sop" /
                                "materialized-external" / "mqt-bench")
    parser.add_argument("--artifact-dir", type=pathlib.Path,
                        default=REPO_ROOT / "artifacts" / "mqt")
    parser.add_argument("--sop-solve", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "sop-solve")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args(argv)

    if not args.sop_solve.exists():
        print(f"error: sop-solve binary not found at {args.sop_solve}", file=sys.stderr)
        return 1

    if not args.corpus_dir.exists():
        print(f"error: corpus dir not found: {args.corpus_dir}", file=sys.stderr)
        return 1

    args.artifact_dir.mkdir(parents=True, exist_ok=True)

    profile_rows = []
    for meta_path in sorted(args.corpus_dir.rglob("*.meta.json")):
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        qsop_path = meta_path.with_suffix("").with_suffix(".qsop")
        if not qsop_path.exists():
            qsop_path = pathlib.Path(meta.get("qsop_path", ""))
        if not qsop_path.exists():
            continue

        print(f"  profiling {qsop_path.name} ...", file=sys.stderr)
        row = profile_row(meta, qsop_path, args.sop_solve, args.timeout)
        profile_rows.append(row)

    profile_path = args.artifact_dir / "mqt-width-profile.jsonl"
    with open(profile_path, "w", encoding="utf-8") as f:
        for row in profile_rows:
            f.write(json.dumps(row) + "\n")

    # Build scaling table grouped by family
    import collections
    families: dict[str, list[dict]] = collections.defaultdict(list)
    for row in profile_rows:
        families[row["mqt_family"]].append(row)

    def _percentile(values: list[float], p: int) -> float:
        if not values:
            return 0.0
        values = sorted(values)
        idx = max(0, min(len(values) - 1, int(len(values) * p / 100)))
        return values[idx]

    scaling: list[dict] = []
    for family, rows in sorted(families.items()):
        qubits_list = [r["mqt_qubits"] for r in rows]
        nvars_list = [r["imported_nvars"] for r in rows]
        tw_list = [r["treewidth_probe"] for r in rows if r.get("treewidth_probe", 0) > 0]
        scaling.append({
            "family": family,
            "rows": len(rows),
            "qubits_p50": _percentile(qubits_list, 50),
            "qubits_p90": _percentile(qubits_list, 90),
            "qubits_max": max(qubits_list, default=0),
            "nvars_p50": _percentile(nvars_list, 50),
            "nvars_p90": _percentile(nvars_list, 90),
            "nvars_max": max(nvars_list, default=0),
            "treewidth_p50": _percentile(tw_list, 50),
            "treewidth_p90": _percentile(tw_list, 90),
            "treewidth_max": max(tw_list, default=0),
        })

    scaling_path = args.artifact_dir / "mqt-scaling-table.json"
    scaling_path.write_text(json.dumps(scaling, indent=2), encoding="utf-8")

    print(f"Profile: {len(profile_rows)} rows → {profile_path}", file=sys.stderr)
    print(f"Scaling table → {scaling_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
