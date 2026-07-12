#!/usr/bin/env python3
"""Benchmark the branch backend over a directory of OpenQASM circuits.

Reproduces what `.gauntlet/adapter.py` does to each circuit -- strip the
non-unitary tail, import with `qasm2sop --input 0^n --output 0^n` (exact first,
`--approx` only when the exact failure is a phase-representability one) -- and
then runs `sop-solve --format stats --include-result` under a timeout, so the
solve mode picked is the same one the gauntlet's amplitude run would pick.

Emits one CSV row per instance: instance shape, the width diagnostics from
`sop-stats`, wall time, an outcome (`solved` / `refused:<reason>` / `timeout` /
`import-failed`), and the branch search counters. This is the table every stage
of a branch-backend change is judged against; keep the baseline around.

    scripts/bench_branch.py ../qccq-gauntlet/datasets/mqt-easy -o baseline.csv
    scripts/bench_branch.py --timeout 120 ../qccq-gauntlet/datasets/mqt2040

The solve child is capped with RLIMIT_AS (default 12 GiB) so a runaway wide DP
fails its own allocation -- recorded as a refusal/kill -- instead of inviting
the kernel OOM killer on a shared machine. Keep --jobs 1 for anything you intend
to publish: a wide DP holds gigabytes, so parallel cases contend and OOM.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import json
import pathlib
import re
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import build_external_qasm_manifest as manifest_tool  # noqa: E402
import bench_common  # noqa: E402

STRIP_RE = re.compile(r"^\s*(measure|creg|barrier)\b")

# Same counters and shape keys bench_gauntlet.py records, so the two tables line up
# column-for-column and a qasm baseline can be diffed against a qpy one.
STAT_KEYS = bench_common.STAT_KEYS
SHAPE_KEYS = bench_common.SHAPE_KEYS

FIELDS = ["suite", "instance", "qubits", "import_mode", "outcome", "wall_s"] + SHAPE_KEYS + STAT_KEYS

refusal_reason = bench_common.refusal_reason
run = bench_common.run


def strip_non_unitary(qasm: str) -> str:
    return "\n".join(line for line in qasm.splitlines() if not STRIP_RE.match(line)) + "\n"


class Bench:
    def __init__(self, qasm2sop: pathlib.Path, sop_stats: pathlib.Path, sop_solve: pathlib.Path,
                 timeout: float, solve_args: list[str], mem_limit_bytes: int = 0):
        self.qasm2sop = str(qasm2sop)
        self.sop_stats = str(sop_stats)
        self.sop_solve = str(sop_solve)
        self.timeout = timeout
        self.solve_args = solve_args
        self.mem_limit_bytes = mem_limit_bytes

    def import_qsop(self, qasm: str, zero: str) -> tuple[str, str]:
        qsop, mode, _error_class, _diagnostic = bench_common.import_qsop(
            self.qasm2sop, qasm, zero, self.timeout, manifest_tool)
        return qsop, mode

    def shape(self, qsop: str) -> dict:
        completed = run([self.sop_stats, "--json", "-"], stdin=qsop, timeout=self.timeout)
        if completed.returncode != 0:
            return {}
        parsed = json.loads(completed.stdout)
        return {key: parsed.get(key, "") for key in SHAPE_KEYS}

    def solve(self, qsop: str) -> tuple[str, float, dict]:
        command = [self.sop_solve, "--format", "stats", "--include-result", *self.solve_args, "-"]
        limiter = bench_common.address_space_limiter(self.mem_limit_bytes)
        started = time.monotonic()
        try:
            completed = run(command, stdin=qsop, timeout=self.timeout, preexec_fn=limiter)
        except subprocess.TimeoutExpired:
            return "timeout", self.timeout, {}
        elapsed = time.monotonic() - started
        if completed.returncode < 0:
            # Killed by a signal, with no diagnostic of its own. Almost always the OOM killer or the
            # RLIMIT_AS backstop on a wide DP -- do not report it as a solver refusal.
            return f"killed:sig{-completed.returncode}", elapsed, {}
        if completed.returncode != 0:
            return f"refused:{refusal_reason(completed.stderr)}", elapsed, {}

        values: dict[str, str] = {}
        for line in completed.stdout.splitlines():
            key, sep, value = line.partition(":")
            if sep:
                values[key.strip()] = value.strip()
        return "solved", elapsed, {key: values.get(key, "") for key in STAT_KEYS}

    def case(self, path: pathlib.Path) -> dict:
        row = {field: "" for field in FIELDS}
        row["suite"] = path.parent.name
        row["instance"] = path.stem
        qasm = strip_non_unitary(path.read_text())
        try:
            row["qubits"] = manifest_tool.qasm_qubits(qasm)
        except ValueError as exc:
            row["outcome"] = "import-failed"
            row["import_mode"] = str(exc)[:80]
            return row

        zero = "0" * int(row["qubits"])
        try:
            qsop, row["import_mode"] = self.import_qsop(qasm, zero)
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            row["outcome"] = "import-failed"
            row["import_mode"] = str(exc).splitlines()[0][:80] if str(exc) else "timeout"
            return row

        row.update(self.shape(qsop))
        row["outcome"], wall, stats = self.solve(qsop)
        row["wall_s"] = f"{wall:.3f}"
        row.update(stats)
        return row


def collect(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in paths:
        if path.is_dir():
            found.extend(sorted(path.rglob("*.qasm")))
        else:
            found.append(path)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=pathlib.Path,
                        help="directories of .qasm (searched recursively) or individual files")
    parser.add_argument("-o", "--out", type=pathlib.Path, help="CSV destination (default: stdout)")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="per-subprocess timeout in seconds (default: the gauntlet's 120)")
    parser.add_argument("--jobs", type=int, default=1,
                        help="run this many cases in parallel. Only for triage: a wide DP holds "
                             "gigabytes, so parallel cases contend for bandwidth and can OOM, "
                             "which lands in the CSV as a refusal or a timeout that a serial run "
                             "solves. Any number you intend to publish wants --jobs 1")
    parser.add_argument("--build", type=pathlib.Path, default=REPO_ROOT / "build")
    parser.add_argument("--solve-arg", action="append", default=[], dest="solve_args",
                        help="extra flag forwarded to sop-solve; repeatable")
    parser.add_argument("--mem-max-gib", type=float, default=12.0,
                        help="RLIMIT_AS cap on the solve child, in GiB (default 12; 0 disables). A "
                             "runaway DP then fails its own allocation instead of OOM-killing the "
                             "machine")
    args = parser.parse_args()

    mem_limit_bytes = int(args.mem_max_gib * (1 << 30))
    bench = Bench(args.build / "qasm2sop", args.build / "sop-stats", args.build / "sop-solve",
                  args.timeout, args.solve_args, mem_limit_bytes)
    cases = collect(args.paths)
    if not cases:
        print("no .qasm found", file=sys.stderr)
        return 1

    handle = args.out.open("w", newline="") if args.out else sys.stdout
    writer = csv.DictWriter(handle, fieldnames=FIELDS)
    writer.writeheader()
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for row in pool.map(bench.case, cases):
                writer.writerow(row)
                handle.flush()
                if args.out:
                    print(f"{row['suite']}/{row['instance']}: {row['outcome']} "
                          f"{row['wall_s']}s", file=sys.stderr)
    finally:
        if args.out:
            handle.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
