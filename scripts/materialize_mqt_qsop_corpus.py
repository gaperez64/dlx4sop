#!/usr/bin/env python3
"""Materialize MQT QSOP local corpus.

Converts selected MQT QASM manifest rows into committed QSOP files with
sidecar .meta.json files, so local tuning can run without MQT/Qiskit/native tools.

Usage:
    python3 scripts/materialize_mqt_qsop_corpus.py \\
        --manifest-dir benchmarks/manifests/mqt \\
        --output-dir benchmarks/corpus/sop/materialized-external/mqt-bench \\
        --qasm2sop build/qasm2sop \\
        --sop-solve build/sop-solve
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

TIER_DIRS = [
    "tier-0-32",
    "tier-33-64",
    "tier-65-128",
    "tier-129-256",
    "tier-257-512-sample",
]

# Map manifest tier key → output directory name
TIER_DIR_MAP: dict[str, str] = {
    "0-32":        "tier-0-32",
    "33-64":       "tier-33-64",
    "65-128":      "tier-65-128",
    "129-256":     "tier-129-256",
    "257-512-sample": "tier-257-512-sample",
}


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _probe_stats(qsop_path: pathlib.Path, sop_solve: pathlib.Path | None) -> dict:
    """Run sop-solve --format stats to get treewidth, cut-rank, etc."""
    if sop_solve is None or not sop_solve.exists():
        return {}
    try:
        result = subprocess.run(
            [str(sop_solve), "--backend", "treewidth", "--format", "stats", str(qsop_path)],
            capture_output=True,
            timeout=30.0,
        )
        if result.returncode != 0:
            return {}
        stats: dict = {}
        for line in result.stdout.decode(errors="replace").splitlines():
            if ":" in line:
                k, _, v = line.partition(":")
                k = k.strip().lower().replace(" ", "_")
                v = v.strip()
                try:
                    stats[k] = int(v)
                except ValueError:
                    stats[k] = v
        return stats
    except Exception:
        return {}


def _convert_qasm_to_qsop(
    qasm_bytes: bytes,
    boundary_input: str,
    boundary_output: str,
    qasm2sop: pathlib.Path,
    timeout: float = 60.0,
) -> bytes | None:
    """Run qasm2sop to produce a QSOP file; return bytes or None on failure."""
    try:
        result = subprocess.run(
            [
                str(qasm2sop),
                "--input", boundary_input,
                "--output", boundary_output,
                "-",
            ],
            input=qasm_bytes,
            capture_output=True,
            timeout=timeout,
        )
        if result.returncode != 0:
            return None
        return result.stdout
    except Exception:
        return None


def materialize_row(
    row: dict,
    output_tier_dir: pathlib.Path,
    qasm2sop: pathlib.Path,
    sop_solve: pathlib.Path | None,
    manifest_rows: list[dict],
    timeout: float = 60.0,
) -> bool:
    family = row.get("mqt_family", "unknown")
    nqubits = row.get("mqt_qubits", 0)
    opt_level = row.get("mqt_optimization_level", 0)
    qasm_sha256 = row.get("qasm_sha256", "")
    qasm_lines = row.get("qasm_lines", [])
    boundaries = row.get("boundaries", [])

    if not qasm_lines or not boundaries:
        return False

    qasm_bytes = "\n".join(qasm_lines).encode()
    if _sha256(qasm_bytes) != qasm_sha256:
        print(f"warning: QASM SHA256 mismatch for {family}-{nqubits}q-opt{opt_level}; proceeding", file=sys.stderr)

    output_tier_dir.mkdir(parents=True, exist_ok=True)

    materialized = 0
    for boundary_input, boundary_output in boundaries:
        stem = f"{family}-{nqubits}q-opt{opt_level}-{boundary_input[:8]}-{boundary_output[:8]}"
        qsop_path = output_tier_dir / f"{stem}.qsop"
        meta_path = output_tier_dir / f"{stem}.meta.json"

        qsop_bytes = _convert_qasm_to_qsop(qasm_bytes, boundary_input, boundary_output,
                                            qasm2sop, timeout=timeout)
        if qsop_bytes is None:
            continue

        qsop_path.write_bytes(qsop_bytes)
        qsop_sha256 = _sha256(qsop_bytes)

        # Probe treewidth stats if available
        stats = _probe_stats(qsop_path, sop_solve)
        nvars = row.get("max_imported_nvars", stats.get("nvars", 0))
        nedges = row.get("max_imported_edges", stats.get("nedges", 0))
        r = stats.get("r", 8)
        treewidth_probe = stats.get("max_treewidth_width", stats.get("tw_width", 0))
        prefix_cut_rank = stats.get("max_prefix_cut_rank", 0)

        meta = {
            "source": "MQT Bench",
            "source_url": row.get("source_url", "https://github.com/munich-quantum-toolkit/bench"),
            "mqt_family": family,
            "mqt_algorithm": family,
            "mqt_qubits": nqubits,
            "mqt_optimization_level": opt_level,
            "boundary_input": boundary_input,
            "boundary_output": boundary_output,
            "qasm_sha256": qasm_sha256,
            "qsop_sha256": qsop_sha256,
            "r": r,
            "nvars": nvars,
            "nedges": nedges,
            "qsop_mode": "sign",
            "treewidth_probe": treewidth_probe,
            "prefix_cut_rank": prefix_cut_rank,
            "component_count": stats.get("component_count", 1),
            "largest_component": stats.get("largest_component", nvars),
            "benchmark_roles": ["mqt-tuning", "external-refresh"],
            "solvable_without_external_tools": True,
        }
        meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

        manifest_rows.append({
            "qsop_path": str(qsop_path.relative_to(REPO_ROOT)),
            "meta_path": str(meta_path.relative_to(REPO_ROOT)),
            **meta,
        })
        materialized += 1

    return materialized > 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest-dir", type=pathlib.Path,
                        default=REPO_ROOT / "benchmarks" / "manifests" / "mqt")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=REPO_ROOT / "benchmarks" / "corpus" / "sop" /
                                "materialized-external" / "mqt-bench")
    parser.add_argument("--qasm2sop", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "qasm2sop")
    parser.add_argument("--sop-solve", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "sop-solve")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--max-rows", type=int, default=None,
                        help="Maximum total rows to materialize (for testing)")
    args = parser.parse_args(argv)

    if not args.qasm2sop.exists():
        print(f"error: qasm2sop binary not found at {args.qasm2sop}", file=sys.stderr)
        return 1

    manifest_rows: list[dict] = []
    total_materialized = 0

    for tier_key, tier_dir_name in TIER_DIR_MAP.items():
        manifest_file = args.manifest_dir / f"tier-{tier_key}.json"
        if not manifest_file.exists():
            manifest_file = args.manifest_dir / f"tier-{tier_key.replace(' ', '-')}.json"
        if not manifest_file.exists():
            continue

        rows = json.loads(manifest_file.read_text(encoding="utf-8"))
        output_tier_dir = args.output_dir / tier_dir_name

        print(f"Materializing {tier_key}: {len(rows)} rows ...", file=sys.stderr)
        for row in rows:
            if args.max_rows is not None and total_materialized >= args.max_rows:
                break
            ok = materialize_row(
                row, output_tier_dir, args.qasm2sop, args.sop_solve,
                manifest_rows, timeout=args.timeout,
            )
            if ok:
                total_materialized += 1

    manifest_path = args.output_dir / "manifest.jsonl"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with open(manifest_path, "w", encoding="utf-8") as f:
        for row in manifest_rows:
            f.write(json.dumps(row) + "\n")

    print(f"Materialized {total_materialized} rows → {args.output_dir}", file=sys.stderr)
    print(f"Manifest written to {manifest_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
