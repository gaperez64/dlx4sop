#!/usr/bin/env python3
"""Measure the rankwidth join cost features on the controlled twist family.

The generated family and decomposition are the same as bench_twist_join.py. Every
solve runs in its own 8 GiB, no-swap user service unless --no-cgroup is given. The
CSV contains the largest realized join from each trace. The JSON summary reports
robust nanoseconds-per-unit estimates without consulting a real benchmark case.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time

from bench_twist_join import build_decomposition, build_instance


KERNELS = ("streaming", "materialized", "dense", "twist")
TRACE_PREFIX = "rankwidth.single_join."
FIELDS = (
    "k",
    "c",
    "repetition",
    "kernel",
    "outcome",
    "wall_s",
    "node",
    "actual_ns",
    "selected_kernel",
    "left_entries",
    "right_entries",
    "parent_entries_forecast",
    "parent_entries_realized",
    "left_dim",
    "right_dim",
    "parent_dim",
    "crossing_rank",
    "dense_pairs",
    "twist_ops",
    "predicted_ns",
    "predicted_pairwise_ns",
    "predicted_twist_ns",
    "predicted_workspace_bytes",
    "amplitude_re",
    "amplitude_im",
    "exit_code",
    "stderr",
)


def parse_stats(text: str) -> dict[str, str]:
    stats: dict[str, str] = {}
    for line in text.splitlines():
        key, separator, value = line.partition(": ")
        if separator:
            stats[key] = value
    return stats


def parse_trace(text: str) -> dict[int, dict[str, int]]:
    nodes: dict[int, dict[str, int]] = {}
    for line in text.splitlines():
        fields = line.split(",")
        if len(fields) != 4 or not fields[0].startswith("rankwidth."):
            continue
        try:
            node = int(fields[1])
            items = int(fields[2])
            elapsed_ns = int(fields[3])
        except ValueError:
            continue
        phase = fields[0]
        record = nodes.setdefault(node, {})
        if phase.startswith(TRACE_PREFIX):
            record[phase.removeprefix(TRACE_PREFIX)] = items
        elif phase == "rankwidth.single_mode_join_f64":
            record["actual_ns"] = elapsed_ns
    return nodes


def largest_join(trace: dict[int, dict[str, int]]) -> tuple[int, dict[str, int]] | None:
    candidates = [
        (node, record)
        for node, record in trace.items()
        if "left_entries" in record and "right_entries" in record and "actual_ns" in record
    ]
    if not candidates:
        return None
    return max(
        candidates,
        key=lambda item: (
            item[1]["left_entries"] * item[1]["right_entries"],
            item[1]["actual_ns"],
        ),
    )


def service_command(command: list[str], unit: str, memory: str) -> list[str]:
    return [
        "systemd-run",
        "--user",
        "--quiet",
        "--pipe",
        "--wait",
        "--collect",
        f"--unit={unit}",
        "-p",
        f"MemoryMax={memory}",
        "-p",
        "MemorySwapMax=0",
        "-p",
        "OOMPolicy=continue",
        "--",
        *command,
    ]


def run_one(
    solver: pathlib.Path,
    instance: pathlib.Path,
    decomposition: pathlib.Path,
    kernel: str,
    timeout: float,
    unit: str,
    memory: str,
    use_cgroup: bool,
) -> dict[str, object]:
    solve = [
        str(solver),
        "--backend",
        "rankwidth",
        "--solve-mode",
        "single-fourier",
        "--single-mode-precision",
        "double",
        "--rankwidth-decomposition",
        str(decomposition),
        "--rankwidth-single-kernel",
        kernel,
        "--rankwidth-materialize-join-max-pairs",
        "4194304",
        "--max-vars",
        "256",
        "--format",
        "stats",
        "--trace",
        "csv",
        str(instance),
    ]
    command = service_command(solve, unit, memory) if use_cgroup else solve
    started = time.monotonic()
    try:
        process = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        if use_cgroup:
            subprocess.run(
                ["systemctl", "--user", "stop", unit],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
        return {
            "outcome": "timeout",
            "wall_s": f"{time.monotonic() - started:.9f}",
            "exit_code": "",
            "stderr": str(error)[:300],
        }
    wall = time.monotonic() - started
    if process.returncode != 0:
        return {
            "outcome": "error",
            "wall_s": f"{wall:.9f}",
            "exit_code": process.returncode,
            "stderr": process.stderr.replace("\n", " ")[:300],
        }
    selected = largest_join(parse_trace(process.stderr))
    if selected is None:
        return {
            "outcome": "missing-trace",
            "wall_s": f"{wall:.9f}",
            "exit_code": process.returncode,
            "stderr": process.stderr.replace("\n", " ")[:300],
        }
    node, profile = selected
    stats = parse_stats(process.stdout)
    return {
        "outcome": "ok",
        "wall_s": f"{wall:.9f}",
        "node": node,
        **profile,
        "amplitude_re": stats.get("amplitude_re", ""),
        "amplitude_im": stats.get("amplitude_im", ""),
        "exit_code": process.returncode,
        "stderr": "",
    }


def quantile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return math.nan
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] * (upper - position) + ordered[upper] * (position - lower)


def summarize(rows: list[dict[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {"source": "controlled synthetic family only", "kernels": {}}
    kernels = result["kernels"]
    assert isinstance(kernels, dict)
    for kernel in KERNELS:
        selected = [row for row in rows if row["kernel"] == kernel and row["outcome"] == "ok"]
        ratios: list[float] = []
        for row in selected:
            if kernel == "twist":
                units = int(row.get("twist_ops", 0))
            elif kernel == "dense":
                units = int(row.get("dense_pairs", 0))
            else:
                units = int(row.get("left_entries", 0)) * int(row.get("right_entries", 0))
            if units > 0:
                ratios.append(int(row["actual_ns"]) / units)
        kernels[kernel] = {
            "rows": len(selected),
            "ns_per_primary_unit_median": statistics.median(ratios) if ratios else None,
            "ns_per_primary_unit_q75": quantile(ratios, 0.75) if ratios else None,
            "ns_per_primary_unit_min": min(ratios) if ratios else None,
            "ns_per_primary_unit_max": max(ratios) if ratios else None,
        }

    mismatches: list[dict[str, object]] = []
    grouped: dict[tuple[int, int, int], list[dict[str, object]]] = {}
    for row in rows:
        grouped.setdefault((int(row["k"]), int(row["c"]), int(row["repetition"])), []).append(row)
    for key, group in grouped.items():
        reference = next(
            (row for row in group if row["kernel"] == "streaming" and row["outcome"] == "ok"),
            None,
        )
        if reference is None:
            continue
        for row in group:
            if row["outcome"] != "ok":
                continue
            delta = max(
                abs(float(row[field]) - float(reference[field]))
                for field in ("amplitude_re", "amplitude_im")
            )
            if delta > 1e-6 * (1.0 + abs(float(reference["amplitude_re"]))):
                mismatches.append({"case": key, "kernel": row["kernel"], "max_delta": delta})
    result["amplitude_mismatches"] = mismatches
    result["outcomes"] = {
        outcome: sum(row["outcome"] == outcome for row in rows)
        for outcome in sorted({str(row["outcome"]) for row in rows})
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("solver", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--kmin", type=int, default=6)
    parser.add_argument("--kmax", type=int, default=10)
    parser.add_argument("--crossings", default="0,1,2,4")
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--no-cgroup", action="store_true")
    args = parser.parse_args()

    crossings = [int(value) for value in args.crossings.split(",") if value.strip()]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict[str, object]] = []
    with tempfile.TemporaryDirectory(prefix="dlx4sop-rw-cost-") as directory:
        work = pathlib.Path(directory)
        total = sum(c <= k for k in range(args.kmin, args.kmax + 1) for c in crossings)
        total *= args.repetitions * len(KERNELS)
        sequence = 0
        for k in range(args.kmin, args.kmax + 1):
            for c in crossings:
                if c > k:
                    continue
                instance = work / f"family_k{k}_c{c}.qsop"
                decomposition = work / f"family_k{k}_c{c}.rwdec"
                instance.write_text(build_instance(k, c))
                decomposition.write_text(build_decomposition(k))
                for repetition in range(1, args.repetitions + 1):
                    for kernel in KERNELS:
                        sequence += 1
                        unit = f"dlx4sop-rw-cost-{os.getpid()}-{sequence:04d}"
                        measured = run_one(
                            args.solver.resolve(),
                            instance,
                            decomposition,
                            kernel,
                            args.timeout,
                            unit,
                            args.memory,
                            not args.no_cgroup,
                        )
                        row = {field: "" for field in FIELDS}
                        row.update(k=k, c=c, repetition=repetition, kernel=kernel)
                        row.update(measured)
                        rows.append(row)
                        print(
                            f"[{sequence:03d}/{total}] k={k} c={c} {kernel}: "
                            f"{measured['outcome']} {measured['wall_s']}s",
                            file=sys.stderr,
                            flush=True,
                        )

    with args.output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    summary = summarize(rows)
    summary_path = args.output.with_suffix(".json")
    summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    print(summary_path)
    return 1 if summary["amplitude_mismatches"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
