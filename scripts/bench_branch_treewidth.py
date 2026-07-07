#!/usr/bin/env python3
"""Sequential branch-vs-treewidth timing harness for QSOP files.

This is intentionally small and local-corpus agnostic. It runs sop-solve once
with the default branch backend and once with the standalone treewidth backend,
captures wall time plus selected --format stats fields, and emits CSV rows that
can be joined with external manifest/case-id metadata.
"""

from __future__ import annotations

import argparse
import csv
import pathlib
import subprocess
import sys
import time
from collections.abc import Iterable


STAT_FIELDS = [
    "solve_mode",
    "solve_mode_kernel",
    "simd_kernel",
    "single_mode_precision",
    "treewidth_single_complex_kernel",
    "rankwidth_single_complex_kernel",
    "bitset_kernel",
    "simd_vectorized_ops",
    "simd_scalar_fallback_ops",
    "treewidth_delegations",
    "rankwidth_delegations",
    "branch_fallthroughs",
    "branch_treewidth_skips",
    "branch_rankwidth_skips",
    "max_residual_vars",
    "max_residual_edges",
    "max_residual_min_fill_width",
    "decomposition_width",
    "table_entries",
    "max_table_entries",
    "join_pairs",
    "signature_entries",
    "max_signature_entries",
    "rankwidth_transition_bytes",
    "rankwidth_dense_join_events",
    "rankwidth_materialized_join_events",
    "rankwidth_streaming_join_events",
]

CSV_FIELDS = [
    "case",
    "path",
    "backend",
    "status",
    "elapsed_s",
    "returncode",
    "timeout_s",
    "memory_max",
    *STAT_FIELDS,
    "stderr_excerpt",
]


def parse_stats(text: str) -> dict[str, str]:
    stats: dict[str, str] = {}
    for line in text.splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        stats[key.strip()] = value.strip()
    return stats


def read_list_file(path: pathlib.Path) -> list[pathlib.Path]:
    entries: list[pathlib.Path] = []
    base = path.parent
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        entry = pathlib.Path(line)
        entries.append(entry if entry.is_absolute() else base / entry)
    return entries


def collect_inputs(files: Iterable[str], list_files: Iterable[str]) -> list[pathlib.Path]:
    paths: list[pathlib.Path] = []
    for list_file in list_files:
        paths.extend(read_list_file(pathlib.Path(list_file)))
    paths.extend(pathlib.Path(file) for file in files)
    return paths


def solver_command(solver: pathlib.Path, backend: str, qsop: pathlib.Path) -> list[str]:
    command = [str(solver), "--format", "stats"]
    if backend == "treewidth":
        command.extend(["--backend", "treewidth"])
    command.append(str(qsop))
    return command


def command_with_limits(command: list[str], timeout_s: float, memory_max: str | None,
                        memory_swap_max: str | None) -> list[str]:
    if memory_max is None:
        return command
    limited = [
        "systemd-run",
        "--user",
        "--scope",
        "--quiet",
        "-p",
        f"MemoryMax={memory_max}",
    ]
    if memory_swap_max is not None:
        limited.extend(["-p", f"MemorySwapMax={memory_swap_max}"])
    limited.extend(["timeout", f"{timeout_s:g}s"])
    limited.extend(command)
    return limited


def run_one(solver: pathlib.Path, qsop: pathlib.Path, backend: str, timeout_s: float,
            memory_max: str | None, memory_swap_max: str | None) -> dict[str, str]:
    command = solver_command(solver, backend, qsop)
    limited_command = command_with_limits(command, timeout_s, memory_max, memory_swap_max)
    start = time.perf_counter()
    try:
        completed = subprocess.run(
            limited_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout_s + 5.0 if memory_max is not None else timeout_s,
            check=False,
        )
        elapsed = time.perf_counter() - start
        timed_out = completed.returncode == 124
        status = "timeout" if timed_out else ("ok" if completed.returncode == 0 else "error")
        stats = parse_stats(completed.stdout)
        row = {
            "case": qsop.stem,
            "path": str(qsop),
            "backend": backend,
            "status": status,
            "elapsed_s": f"{elapsed:.6f}",
            "returncode": str(completed.returncode),
            "timeout_s": f"{timeout_s:g}",
            "memory_max": memory_max or "",
            "stderr_excerpt": completed.stderr.replace("\n", " ")[:400],
        }
        for field in STAT_FIELDS:
            row[field] = stats.get(field, "")
        return row
    except subprocess.TimeoutExpired as exc:
        elapsed = time.perf_counter() - start
        stderr = exc.stderr if isinstance(exc.stderr, str) else ""
        row = {
            "case": qsop.stem,
            "path": str(qsop),
            "backend": backend,
            "status": "timeout",
            "elapsed_s": f"{elapsed:.6f}",
            "returncode": "",
            "timeout_s": f"{timeout_s:g}",
            "memory_max": memory_max or "",
            "stderr_excerpt": stderr.replace("\n", " ")[:400],
        }
        for field in STAT_FIELDS:
            row[field] = ""
        return row


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run sop-solve branch and treewidth stats sequentially and emit CSV.",
    )
    parser.add_argument("files", nargs="*", help="QSOP files to benchmark")
    parser.add_argument("--list", action="append", default=[], help="file containing QSOP paths")
    parser.add_argument("--solver", default="build/sop-solve", help="path to sop-solve")
    parser.add_argument("--timeout", type=float, default=120.0, help="per-run timeout in seconds")
    parser.add_argument("--memory-max", help="optional systemd MemoryMax value, e.g. 8G")
    parser.add_argument(
        "--memory-swap-max",
        default="0",
        help="optional systemd MemorySwapMax value when --memory-max is set",
    )
    parser.add_argument("--output", help="CSV output path; defaults to stdout")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    solver = pathlib.Path(args.solver)
    inputs = collect_inputs(args.files, args.list)
    if not inputs:
        print("error: provide at least one QSOP file or --list", file=sys.stderr)
        return 2
    if not solver.exists():
        print(f"error: solver not found: {solver}", file=sys.stderr)
        return 2

    out_file = open(args.output, "w", newline="") if args.output else sys.stdout
    try:
        writer = csv.DictWriter(out_file, fieldnames=CSV_FIELDS)
        writer.writeheader()
        for qsop in inputs:
            for backend in ("branch", "treewidth"):
                writer.writerow(
                    run_one(
                        solver,
                        qsop,
                        backend,
                        args.timeout,
                        args.memory_max,
                        args.memory_swap_max if args.memory_max is not None else None,
                    )
                )
                out_file.flush()
    finally:
        if args.output:
            out_file.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
