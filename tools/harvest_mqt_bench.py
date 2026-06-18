#!/usr/bin/env python3
"""MQT Bench harvester.

Generate or discover MQT Bench OpenQASM 2 circuits, probe qasm2sop
importability, select candidate fixed-boundary rows, and emit tiered
QASM manifests.

Requires mqt.bench and qiskit if --mqt-source is not given and MQT Bench
is not installed.  The committed output artifacts are QASM manifests and
sidecar metadata — no runtime MQT dependency in C tools.

Usage:
    python3 tools/harvest_mqt_bench.py \\
        --output-dir benchmarks/manifests/mqt \\
        --max-per-family 20 \\
        --seed 1234 \\
        --target-tier 33-64 \\
        --target-tier 65-128 \\
        --target-tier 129-256

Optional (faster / dependency-free import probing):
    --qasm2sop build/qasm2sop
    --mqt-source /path/to/mqt/bench/checkout
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import random
import subprocess
import sys
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCE_URL = "https://github.com/munich-quantum-toolkit/bench"

# Map tier name → (min_vars, max_vars) bounds on imported QSOP variable count.
TIER_BOUNDS: dict[str, tuple[int, int]] = {
    "0-32":        (0,   32),
    "33-64":       (33,  64),
    "65-128":      (65, 128),
    "129-256":    (129, 256),
    "257-512-sample": (257, 512),
}

DEFAULT_BENCHMARKS = [
    "ghz", "qft", "qftentangled", "bv", "qwalk",
    "qpeexact", "qpeinexact", "wstate", "graphstate",
    "dj", "hhl", "vqe", "qaoa",
]


# ---------------------------------------------------------------------------
# MQT loading helpers (optional dependency)
# ---------------------------------------------------------------------------

class MqtUnavailable(RuntimeError):
    pass


def _load_mqt(source: pathlib.Path | None) -> Any:
    if source is not None:
        pkg_src = source / "src"
        sys.path.insert(0, str(pkg_src if pkg_src.exists() else source))
    try:
        from mqt.bench import BenchmarkLevel, get_benchmark
        from mqt.bench.benchmarks import get_available_benchmark_names
        from qiskit.qasm2 import dumps as qasm2_dumps
        return {
            "BenchmarkLevel": BenchmarkLevel,
            "get_benchmark": get_benchmark,
            "get_available_benchmark_names": get_available_benchmark_names,
            "qasm2_dumps": qasm2_dumps,
        }
    except ModuleNotFoundError as exc:
        raise MqtUnavailable(
            "harvest_mqt_bench.py requires mqt.bench and qiskit; "
            "install them or pass --mqt-source /path/to/checkout"
        ) from exc


def _qasm_bytes(qc: Any, mod: dict) -> bytes:
    return mod["qasm2_dumps"](qc).encode()


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


# ---------------------------------------------------------------------------
# QASM import probe via qasm2sop binary
# ---------------------------------------------------------------------------

def _probe_import(
    qasm_bytes: bytes,
    qasm2sop: pathlib.Path,
    sop_stats: pathlib.Path | None = None,
    timeout: float = 30.0,
) -> dict:
    """Return {'status': 'ok'|'error', 'nvars': N, 'nedges': N, 'reason': '...'}."""
    try:
        # Step 1: convert QASM → QSOP text (piped to stdout)
        p_import = subprocess.run(
            [str(qasm2sop), "-"],
            input=qasm_bytes,
            capture_output=True,
            timeout=timeout,
        )
        if p_import.returncode != 0:
            reason = p_import.stderr.decode(errors="replace").strip()[:200]
            return {"status": "error", "reason": reason, "nvars": 0, "nedges": 0}

        # Step 2: pipe QSOP text into sop-stats to get variable/edge counts
        sop_stats_bin = sop_stats or (qasm2sop.parent / "sop-stats")
        p_stats = subprocess.run(
            [str(sop_stats_bin), "-"],
            input=p_import.stdout,
            capture_output=True,
            timeout=timeout,
        )
        if p_stats.returncode != 0:
            reason = p_stats.stderr.decode(errors="replace").strip()[:200]
            return {"status": "error", "reason": reason, "nvars": 0, "nedges": 0}

        stdout = p_stats.stdout.decode(errors="replace")
        nvars = 0
        nedges = 0
        for line in stdout.splitlines():
            if line.startswith("variables:"):
                nvars = int(line.split(":")[1].strip())
            elif line.startswith("quadratic_terms:"):
                nedges = int(line.split(":")[1].strip())
        return {"status": "ok", "nvars": nvars, "nedges": nedges}
    except subprocess.TimeoutExpired:
        return {"status": "error", "reason": "import-timeout", "nvars": 0, "nedges": 0}
    except Exception as exc:
        return {"status": "error", "reason": str(exc)[:100], "nvars": 0, "nedges": 0}


def _tier_for_nvars(nvars: int) -> str | None:
    for tier, (lo, hi) in TIER_BOUNDS.items():
        if lo <= nvars <= hi:
            return tier
    return None


# ---------------------------------------------------------------------------
# Boundary helpers
# ---------------------------------------------------------------------------

def _zero_boundary(nqubits: int) -> str:
    return "0" * nqubits


def _one_boundary(nqubits: int) -> str:
    return "1" * nqubits


# ---------------------------------------------------------------------------
# Harvesting
# ---------------------------------------------------------------------------

def harvest(
    mqt_source: pathlib.Path | None,
    families: list[str],
    sizes: list[int],
    opt_levels: list[int],
    target_tiers: set[str],
    max_per_family: int,
    qasm2sop: pathlib.Path | None,
    sop_stats: pathlib.Path | None,
    seed: int,
    verbose: bool,
) -> tuple[dict[str, list[dict]], list[dict]]:
    """Return (tiered_candidates, unsupported_rows)."""
    rng = random.Random(seed)

    try:
        mod = _load_mqt(mqt_source)
    except MqtUnavailable as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)

    BenchmarkLevel = mod["BenchmarkLevel"]
    get_benchmark = mod["get_benchmark"]
    get_available_benchmark_names = mod["get_available_benchmark_names"]
    qasm2_dumps = mod["qasm2_dumps"]

    if not families:
        try:
            families = list(get_available_benchmark_names())
        except Exception:
            families = DEFAULT_BENCHMARKS

    tiered: dict[str, list[dict]] = {tier: [] for tier in TIER_BOUNDS}
    unsupported: list[dict] = []
    family_tier_counts: dict[tuple[str, str], int] = collections.Counter() if False else {}

    import collections
    family_tier_counts = collections.Counter()

    for family in families:
        for nqubits in sizes:
            for opt_level in opt_levels:
                # Check per-family-per-tier cap
                skip = False
                for tier in target_tiers:
                    if family_tier_counts.get((family, tier), 0) >= max_per_family:
                        skip = True
                        break
                # We don't know tier yet; check after import
                try:
                    level = (BenchmarkLevel.Indep if hasattr(BenchmarkLevel, "Indep")
                             else BenchmarkLevel.INDEP)
                    qc = get_benchmark(family, level, nqubits, opt_level=opt_level)
                except Exception as exc:
                    if verbose:
                        print(f"  skip {family}/{nqubits}/opt{opt_level}: {exc}", file=sys.stderr)
                    continue

                try:
                    qasm_bytes = _qasm_bytes(qc, mod)
                except Exception as exc:
                    unsupported.append({
                        "source": "MQT Bench",
                        "source_url": SOURCE_URL,
                        "family": family,
                        "qubits": nqubits,
                        "opt_level": opt_level,
                        "reason": f"qasm-export: {exc}",
                        "qasm_sha256": "",
                    })
                    continue

                qasm_sha256 = _sha256(qasm_bytes)
                qasm_lines = qasm_bytes.decode(errors="replace").splitlines()

                import_result: dict = {"status": "error", "reason": "no-qasm2sop", "nvars": 0, "nedges": 0}
                if qasm2sop is not None and qasm2sop.exists():
                    import_result = _probe_import(qasm_bytes, qasm2sop, sop_stats=sop_stats)
                else:
                    # Estimate nvars from qubit count (rough: 1–4x expansion factor)
                    import_result = {"status": "estimated", "nvars": nqubits * 2, "nedges": nqubits * 4}

                nvars = import_result.get("nvars", 0)
                tier = _tier_for_nvars(nvars)

                if import_result["status"] == "error":
                    unsupported.append({
                        "source": "MQT Bench",
                        "source_url": SOURCE_URL,
                        "family": family,
                        "qubits": nqubits,
                        "opt_level": opt_level,
                        "reason": import_result.get("reason", "import-failed"),
                        "qasm_sha256": qasm_sha256,
                    })
                    continue

                if tier is None or tier not in target_tiers:
                    # Wrong tier or outside any tier
                    continue

                if family_tier_counts.get((family, tier), 0) >= max_per_family:
                    continue

                boundaries = [
                    [_zero_boundary(nqubits), _zero_boundary(nqubits)],
                    [_one_boundary(nqubits), _one_boundary(nqubits)],
                ]

                row: dict = {
                    "source": "MQT Bench",
                    "source_url": SOURCE_URL,
                    "mqt_family": family,
                    "mqt_algorithm": family,
                    "mqt_qubits": nqubits,
                    "mqt_optimization_level": opt_level,
                    "qasm_sha256": qasm_sha256,
                    "qasm_lines": qasm_lines,
                    "boundaries": boundaries,
                    "max_imported_nvars": nvars,
                    "max_imported_edges": import_result.get("nedges", 0),
                    "import_status": import_result["status"],
                    "tier": tier,
                }
                tiered[tier].append(row)
                family_tier_counts[(family, tier)] = family_tier_counts.get((family, tier), 0) + 1

                if verbose:
                    print(f"  + {family}/{nqubits}/opt{opt_level} → {tier} nvars={nvars}",
                          file=sys.stderr)

    return tiered, unsupported


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=REPO_ROOT / "benchmarks" / "manifests" / "mqt")
    parser.add_argument("--max-per-family", type=int, default=20)
    parser.add_argument("--seed", type=int, default=1234)
    parser.add_argument("--target-tier", action="append", dest="target_tiers",
                        default=None, metavar="TIER",
                        help="Tier(s) to target (e.g. 33-64). Repeat for multiple.")
    parser.add_argument("--family", action="append", dest="families",
                        default=None, metavar="FAMILY",
                        help="MQT family to include. Repeat for multiple.")
    parser.add_argument("--size", action="append", dest="sizes", type=int,
                        default=None, metavar="N",
                        help="Qubit sizes to probe. Repeat for multiple.")
    parser.add_argument("--opt-level", action="append", dest="opt_levels", type=int,
                        default=None, metavar="N",
                        help="MQT optimization levels. Default: 1 2")
    parser.add_argument("--mqt-source", type=pathlib.Path, default=None,
                        help="Path to MQT Bench source checkout")
    parser.add_argument("--qasm2sop", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "qasm2sop",
                        help="Path to qasm2sop binary for import probing")
    parser.add_argument("--sop-stats", type=pathlib.Path, default=None,
                        help="Path to sop-stats binary (default: <qasm2sop-dir>/sop-stats)")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    target_tiers: set[str] = set(args.target_tiers or list(TIER_BOUNDS.keys()))
    families: list[str] = args.families or []
    sizes: list[int] = args.sizes or list(range(2, 65))
    opt_levels: list[int] = args.opt_levels or [1, 2]

    print(f"Harvesting MQT Bench circuits → {args.output_dir}", file=sys.stderr)
    print(f"  target tiers: {sorted(target_tiers)}", file=sys.stderr)
    print(f"  sizes: {min(sizes)}–{max(sizes)}, opt_levels: {opt_levels}", file=sys.stderr)

    tiered, unsupported = harvest(
        mqt_source=args.mqt_source,
        families=families,
        sizes=sizes,
        opt_levels=opt_levels,
        target_tiers=target_tiers,
        max_per_family=args.max_per_family,
        qasm2sop=args.qasm2sop,
        sop_stats=args.sop_stats,
        seed=args.seed,
        verbose=args.verbose,
    )

    args.output_dir.mkdir(parents=True, exist_ok=True)

    total = 0
    for tier, rows in tiered.items():
        safe_tier = tier.replace(" ", "-")
        out_path = args.output_dir / f"tier-{safe_tier}.json"
        out_path.write_text(json.dumps(rows, indent=2), encoding="utf-8")
        print(f"  {tier}: {len(rows)} rows → {out_path}", file=sys.stderr)
        total += len(rows)

    # Unsupported log
    unsup_path = args.output_dir / "unsupported.jsonl"
    with open(unsup_path, "w", encoding="utf-8") as f:
        for row in unsupported:
            f.write(json.dumps(row) + "\n")
    print(f"  unsupported: {len(unsupported)} rows → {unsup_path}", file=sys.stderr)

    # Harvest summary
    summary = {
        "seed": args.seed,
        "target_tiers": sorted(target_tiers),
        "sizes": sizes,
        "opt_levels": opt_levels,
        "total_candidates": total,
        "unsupported_count": len(unsupported),
        "per_tier": {tier: len(rows) for tier, rows in tiered.items()},
    }
    summary_path = args.output_dir / "harvest-summary.json"
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Harvest summary: {total} total candidates, {len(unsupported)} unsupported",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
