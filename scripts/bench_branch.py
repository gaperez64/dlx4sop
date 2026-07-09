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

# Matches .gauntlet/adapter.py: the only thing --approx can rescue is an angle
# outside qasm2sop's exact grid.
APPROX_EPSILON = 1e-8

STRIP_RE = re.compile(r"^\s*(measure|creg|barrier)\b")

# Counters worth a column. `search_nodes` and the cache pair say whether the
# recursion was entered at all and whether the memo is working; the rest say
# which sub-solver actually did the work.
STAT_KEYS = [
    "solve_mode_kernel",
    "search_nodes",
    "cache_hits",
    "cache_misses",
    "cache_entries",
    "cache_estimated_bytes",
    "treewidth_delegations",
    "rankwidth_delegations",
    "branch_fallthroughs",
    "branch_treewidth_skips",
    "branch_rankwidth_skips",
    "branch_propagations",
    "branch_zero_prunes",
    "branch_width_probes",
    "branch_probe_skips",
    "branch_cutset_size",
    "max_residual_vars",
    "max_residual_min_fill_width",
    "max_residual_prefix_cut_rank",
]

SHAPE_KEYS = [
    "modulus",
    "variables",
    "quadratic_terms",
    "components",
    "min_fill_width",
    "prefix_cut_rank",
]

FIELDS = ["suite", "instance", "qubits", "import_mode", "outcome", "wall_s"] + SHAPE_KEYS + STAT_KEYS


def strip_non_unitary(qasm: str) -> str:
    return "\n".join(line for line in qasm.splitlines() if not STRIP_RE.match(line)) + "\n"


def refusal_reason(stderr: str) -> str:
    """A stable slug per refusal, so rows stay comparable across stages."""
    table = [
        ("fallback refused component", "max_fallback_vars"),
        ("search-node cap exceeded", "max_search_nodes"),
        ("recursion-depth cap", "max_recursion_depth"),
        ("cutset budget", "cutset_budget"),
        ("no delegate available", "no_delegate"),
        ("exceeds", "root_sanity"),
        ("pass a larger --max-vars", "max_vars"),
        ("memory-skip", "memory_skip"),
        ("out of memory", "oom"),
    ]
    for needle, slug in table:
        if needle in stderr:
            return slug
    return "other"


def run(cmd: list[str], *, stdin: str | None = None, timeout: float | None = None):
    return subprocess.run(
        cmd, input=stdin, check=False, capture_output=True, text=True, timeout=timeout
    )


class Bench:
    def __init__(self, qasm2sop: pathlib.Path, sop_stats: pathlib.Path, sop_solve: pathlib.Path,
                 timeout: float, solve_args: list[str]):
        self.qasm2sop = str(qasm2sop)
        self.sop_stats = str(sop_stats)
        self.sop_solve = str(sop_solve)
        self.timeout = timeout
        self.solve_args = solve_args

    def import_qsop(self, qasm: str, zero: str) -> tuple[str, str]:
        exact = run([self.qasm2sop, "--input", zero, "--output", zero, "-"], stdin=qasm,
                    timeout=self.timeout)
        if exact.returncode == 0:
            return exact.stdout, "exact"
        if "angle" not in exact.stderr and "non-sign quadratic" not in exact.stderr:
            raise RuntimeError(exact.stderr.strip())
        approx = run(
            [self.qasm2sop, "--approx", repr(APPROX_EPSILON), "--input", zero, "--output", zero,
             "-"],
            stdin=qasm, timeout=self.timeout)
        if approx.returncode != 0:
            raise RuntimeError(approx.stderr.strip())
        return approx.stdout, "approx"

    def shape(self, qsop: str) -> dict:
        completed = run([self.sop_stats, "--json", "-"], stdin=qsop, timeout=self.timeout)
        if completed.returncode != 0:
            return {}
        parsed = json.loads(completed.stdout)
        return {key: parsed.get(key, "") for key in SHAPE_KEYS}

    def solve(self, qsop: str) -> tuple[str, float, dict]:
        command = [self.sop_solve, "--format", "stats", "--include-result", *self.solve_args, "-"]
        started = time.monotonic()
        try:
            completed = run(command, stdin=qsop, timeout=self.timeout)
        except subprocess.TimeoutExpired:
            return "timeout", self.timeout, {}
        elapsed = time.monotonic() - started
        if completed.returncode < 0:
            # Killed by a signal, with no diagnostic of its own. Almost always the OOM killer on a
            # wide DP -- do not report it as a solver refusal.
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
    args = parser.parse_args()

    bench = Bench(args.build / "qasm2sop", args.build / "sop-stats", args.build / "sop-solve",
                  args.timeout, args.solve_args)
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
