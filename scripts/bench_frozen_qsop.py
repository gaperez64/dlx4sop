#!/usr/bin/env python3
"""Benchmark frozen QSOP inputs without rerunning the circuit importer."""

from __future__ import annotations

import argparse
import csv
import json
import pathlib
import subprocess
import sys
import tempfile
import time
from typing import TextIO

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import bench_common  # noqa: E402


FIELDS = [
    "instance",
    "outcome",
    "reason",
    "wall_s",
    "exit_code",
    "diagnostic",
] + bench_common.STAT_KEYS


def solve(path: pathlib.Path, executable: pathlib.Path, timeout: float, mem_bytes: int,
          solve_args: list[str], jsonl_out: TextIO | None = None) -> dict[str, object]:
    row: dict[str, object] = {key: "" for key in FIELDS}
    row["instance"] = path.stem
    started = time.monotonic()
    final: dict[str, object] = {}
    with tempfile.TemporaryDirectory(prefix="dlx4sop-frozen-") as tmp:
        jsonl = pathlib.Path(tmp) / "run.jsonl"
        command = [
            str(executable),
            "--backend", "branch",
            "--solve-mode", "auto",
            "--format", "stats",
            "--stats-jsonl", str(jsonl),
            *solve_args,
            str(path),
        ]
        try:
            completed = subprocess.run(
                command,
                check=False,
                capture_output=True,
                text=True,
                timeout=timeout,
                preexec_fn=bench_common.address_space_limiter(mem_bytes),
            )
        except subprocess.TimeoutExpired:
            row["outcome"] = "timeout"
            row["reason"] = "timeout"
            row["wall_s"] = f"{timeout:.3f}"
            return row
        row["wall_s"] = f"{time.monotonic() - started:.3f}"
        row["exit_code"] = completed.returncode
        if jsonl.exists():
            for line in jsonl.read_text().splitlines():
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if jsonl_out is not None:
                    record["benchmark_instance"] = path.stem
                    jsonl_out.write(json.dumps(record, separators=(",", ":")) + "\n")
                    jsonl_out.flush()
                if record.get("schema") == "sop_solve_run_stats_v1":
                    final = record

        for key in bench_common.STAT_KEYS:
            row[key] = final.get(key, "")
        row["diagnostic"] = final.get("diagnostic", "")
        if completed.returncode == 0:
            row["outcome"] = "solved"
            row["reason"] = "none"
        elif completed.returncode < 0:
            row["outcome"] = f"killed:sig{-completed.returncode}"
            row["reason"] = "killed"
        elif final:
            row["outcome"] = final.get("status", "error")
            row["reason"] = final.get("reason", "other_error")
        else:
            row["outcome"] = "error"
            row["reason"] = bench_common.refusal_reason(completed.stderr)
            row["diagnostic"] = completed.stderr.strip()
    return row


def collect(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in paths:
        if path.is_dir():
            found.extend(sorted(path.rglob("*.qsop")))
        else:
            found.append(path)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=pathlib.Path)
    parser.add_argument("-o", "--out", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, default=REPO_ROOT / "build-rel")
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument("--mem-max-gib", type=float, default=12.0)
    parser.add_argument("--solve-arg", action="append", default=[])
    parser.add_argument("--jsonl-out", type=pathlib.Path,
                        help="optional aggregate copy of all per-run JSONL records")
    parser.add_argument("--resume", action="store_true",
                        help="append only instances not already present in the output CSV")
    args = parser.parse_args()

    cases = collect(args.paths)
    if not cases:
        print("no .qsop found", file=sys.stderr)
        return 1
    args.out.parent.mkdir(parents=True, exist_ok=True)
    completed: set[str] = set()
    append = args.resume and args.out.exists()
    if append:
        with args.out.open(newline="") as existing:
            completed = {row["instance"] for row in csv.DictReader(existing)}
        cases = [path for path in cases if path.stem not in completed]
    jsonl_mode = "a" if append else "w"
    jsonl_handle = args.jsonl_out.open(jsonl_mode) if args.jsonl_out else None
    with args.out.open("a" if append else "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        if not append:
            writer.writeheader()
        try:
            for path in cases:
                row = solve(
                    path,
                    args.build / "sop-solve",
                    args.timeout,
                    int(args.mem_max_gib * (1 << 30)),
                    args.solve_arg,
                    jsonl_handle,
                )
                writer.writerow(row)
                handle.flush()
                print(f"{path.stem}: {row['outcome']}:{row['reason']} {row['wall_s']}s", file=sys.stderr)
        finally:
            if jsonl_handle is not None:
                jsonl_handle.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
