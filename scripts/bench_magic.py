#!/usr/bin/env python3
"""Measure Section 5 magic counts over cached QSOPs and gauntlet QPY payloads.

QSOP inputs are passed directly to ``sop-stats --json``. QPY inputs use the same
adapter/lowering/import recipe as ``bench_gauntlet.py`` before collecting stats.
The summary is grouped by modulus and includes the width-bound subset used by
the QPF admission analysis (n >= 20 and m >= 2n).
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import statistics
import subprocess
import sys


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))


def percentile(values: list[int], numerator: int, denominator: int) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    index = ((len(ordered) - 1) * numerator + denominator - 1) // denominator
    return ordered[index]


def stats_for_qsop(sop_stats: pathlib.Path, path: pathlib.Path | None, text: str | None,
                   timeout: float) -> dict | None:
    command = [str(sop_stats), "--json", str(path) if path is not None else "-"]
    try:
        completed = subprocess.run(
            command,
            input=text,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return None
    if completed.returncode != 0:
        return None
    try:
        return json.loads(completed.stdout)
    except json.JSONDecodeError:
        return None


def collect(paths: list[pathlib.Path]) -> tuple[list[pathlib.Path], list[pathlib.Path]]:
    qsops: list[pathlib.Path] = []
    qpys: list[pathlib.Path] = []
    for path in paths:
        candidates = path.rglob("*") if path.is_dir() else [path]
        for candidate in candidates:
            if candidate.suffix == ".qsop":
                qsops.append(candidate)
            elif candidate.suffix == ".qpy":
                qpys.append(candidate)
    return sorted(qsops), sorted(qpys)


def print_group(label: str, rows: list[dict]) -> None:
    by_modulus: dict[int, list[dict]] = collections.defaultdict(list)
    for row in rows:
        by_modulus[int(row["modulus"])].append(row)
    print(label)
    print("modulus files tau=0 median p90 max width_bound width_bound_tau=0 band_12_60")
    for modulus, group in sorted(by_modulus.items()):
        tau = [int(row["magic_vertices_mode_1"]) for row in group]
        wide = [
            row for row in group
            if int(row["variables"]) >= 20
            and int(row["quadratic_terms"]) >= 2 * int(row["variables"])
        ]
        wide_zero = sum(int(row["magic_vertices_mode_1"]) == 0 for row in wide)
        band = sum(12 <= int(row["magic_vertices_mode_1"]) <= 60 for row in group)
        print(
            f"{modulus} {len(group)} {sum(value == 0 for value in tau)} "
            f"{int(statistics.median(tau))} {percentile(tau, 9, 10)} {max(tau, default=0)} "
            f"{len(wide)} {wide_zero} {band}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="*", type=pathlib.Path)
    parser.add_argument("--build", type=pathlib.Path, default=REPO_ROOT / "build")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--limit", type=int, default=0,
                        help="cap each input family for a quick audit (0 = all)")
    args = parser.parse_args()
    paths = args.paths or [pathlib.Path.home() / ".cache" / "dlx4tmp"]
    qsops, qpys = collect(paths)
    if args.limit > 0:
        qsops = qsops[:args.limit]
        qpys = qpys[:args.limit]

    sop_stats = args.build / "sop-stats"
    rows: list[dict] = []
    failures = 0
    for path in qsops:
        row = stats_for_qsop(sop_stats, path, None, args.timeout)
        if row is None:
            failures += 1
        else:
            rows.append(row)

    if qpys:
        import bench_gauntlet

        bench = bench_gauntlet.Bench(
            args.build / "qasm2sop",
            sop_stats,
            args.build / "sop-solve",
            args.timeout,
            [],
            0,
        )
        for path in qpys:
            try:
                qasm, qubits = bench.translate(path)
                qsop, _, _, _ = bench.import_qsop(qasm, "0" * qubits)
                row = stats_for_qsop(sop_stats, None, qsop, args.timeout)
            except Exception:  # one bad corpus item must not abort an audit
                row = None
            if row is None:
                failures += 1
            else:
                rows.append(row)

    print_group("magic audit", rows)
    print(f"processed={len(rows)} failed={failures}")
    return 0 if rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
