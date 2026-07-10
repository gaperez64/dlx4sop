#!/usr/bin/env python3
"""Benchmark the branch backend over qccq-gauntlet QPY datasets.

Like `scripts/bench_branch.py`, but reads the gauntlet's native `.qpy` payloads
through the *exact same front end* as `.gauntlet/adapter.py` -- the recipe that
produced the amplitudes the gauntlet ranks -- rather than re-deriving one from
`.qasm`. For each payload:

  * `adapter.load_circuit` (qiskit `qpy.load` -> `remove_final_measurements` ->
    strip inert clbits -> reject genuine non-unitary ops),
  * `qiskit.qasm2.dumps` + `build_external_qasm_manifest.inline_simple_gates`
    (the same OpenQASM 2 lowering the adapter feeds to qasm2sop),
  * `qasm2sop --input 0^n --output 0^n` (exact first, `--approx` only when the
    exact failure is a phase-representability one), then
  * `sop-solve --format stats --include-result` under a timeout, unless
    `--import-only` stops after qasm2sop for corpus auditing.

Emits one CSV row per instance: shape, `sop-stats` width diagnostics, per-stage
wall time (`load_s` qpy->qasm, `import_s` qasm2sop, `wall_s` the solve itself),
an outcome (`solved` / `refused:<reason>` / `timeout` / `killed:sig<n>` /
`import-failed`), and the branch search counters. This is the table a
branch-backend change is judged against: more `solved` without a `wall_s`
regression.

    scripts/bench_gauntlet.py --build build-rel \
        ../qccq-gauntlet/datasets/alg85 -o baseline_alg85.csv

The solve child is capped with RLIMIT_AS (default 12 GiB) so a runaway wide DP
fails its own allocation -- recorded as a refusal/kill -- instead of inviting
the kernel OOM killer on a shared machine. Keep --jobs 1 for anything you intend
to publish: a wide DP holds gigabytes, so parallel cases contend and OOM.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import csv
import importlib.util
import json
import pathlib
import resource
import subprocess
import sys
import time
import warnings

# qiskit's remove_final_measurements/qasm2 round-trip warns about layout on a few
# MQT payloads; it is benign and would otherwise repeat thousands of times.
warnings.simplefilter("ignore")

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import build_external_qasm_manifest as manifest_tool  # noqa: E402


def _load_adapter():
    """Import `.gauntlet/adapter.py` (a dotted-dir module) for its qpy recipe."""
    path = REPO_ROOT / ".gauntlet" / "adapter.py"
    spec = importlib.util.spec_from_file_location("gauntlet_adapter", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


adapter = _load_adapter()

# Matches .gauntlet/adapter.py: the only thing --approx can rescue is an angle
# outside qasm2sop's exact grid.
APPROX_EPSILON = 1e-8

# Same counters and shape keys bench_branch.py records, so the two tables line up
# column-for-column and a qasm baseline can be diffed against a qpy one.
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
    "decomposition_width",
    "rankwidth_cutrank_width",
    "rankwidth_table_forecast",
    "rankwidth_join_pair_forecast",
    "table_entries",
    "max_table_entries",
    "join_pairs",
    "simd_vectorized_ops",
    "simd_scalar_fallback_ops",
]

SHAPE_KEYS = [
    "modulus",
    "variables",
    "quadratic_terms",
    "components",
    "min_fill_width",
    "min_fill_dp_work",
    "prefix_cut_rank",
]

FIELDS = (
    ["suite", "instance", "qubits", "import_mode", "exact_error_class", "exact_error",
     "outcome", "load_s", "import_s", "wall_s"]
    + SHAPE_KEYS
    + STAT_KEYS
)


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


def run(cmd: list[str], *, stdin: str | None = None, timeout: float | None = None,
        preexec_fn=None):
    return subprocess.run(cmd, input=stdin, check=False, capture_output=True, text=True,
                          timeout=timeout, preexec_fn=preexec_fn)


def _address_space_limiter(limit_bytes: int):
    if limit_bytes <= 0:
        return None

    def _apply():
        resource.setrlimit(resource.RLIMIT_AS, (limit_bytes, limit_bytes))

    return _apply


class Bench:
    def __init__(self, qasm2sop: pathlib.Path, sop_stats: pathlib.Path, sop_solve: pathlib.Path,
                 timeout: float, solve_args: list[str], mem_limit_bytes: int,
                 import_only: bool = False):
        self.qasm2sop = str(qasm2sop)
        self.sop_stats = str(sop_stats)
        self.sop_solve = str(sop_solve)
        self.timeout = timeout
        self.solve_args = solve_args
        self.mem_limit_bytes = mem_limit_bytes
        self.import_only = import_only

    def translate(self, payload: pathlib.Path) -> tuple[str, int]:
        """The gauntlet's qpy recipe: adapter.load_circuit -> qasm2 dumps -> inline."""
        import qiskit.qasm2 as qasm2mod

        circuit = adapter.load_circuit(str(payload))
        qasm_text = qasm2mod.dumps(circuit)
        qasm_text = manifest_tool.inline_simple_gates(qasm_text)
        return qasm_text, manifest_tool.qasm_qubits(qasm_text)

    def import_qsop(self, qasm: str, zero: str,
                    exact_only: bool = False) -> tuple[str, str, str, str]:
        exact = run([self.qasm2sop, "--input", zero, "--output", zero, "-"], stdin=qasm,
                    timeout=self.timeout)
        if exact.returncode == 0:
            return exact.stdout, "exact", "", ""
        if "angle" not in exact.stderr and "non-sign quadratic" not in exact.stderr:
            raise RuntimeError(exact.stderr.strip())
        diagnostic = manifest_tool.diagnostic_from_exception(RuntimeError(exact.stderr))
        error_class = manifest_tool.classify_error(diagnostic)
        # Import-only audits need the exact/approx classification, not the approximate SOP itself.
        # Avoid materializing huge approximate QNN/QFT instances that will never be solved.
        if exact_only:
            return "", "approx", error_class, diagnostic
        approx = run(
            [self.qasm2sop, "--approx", repr(APPROX_EPSILON), "--input", zero, "--output", zero,
             "-"],
            stdin=qasm, timeout=self.timeout)
        if approx.returncode != 0:
            raise RuntimeError(approx.stderr.strip())
        return approx.stdout, "approx", error_class, diagnostic

    def shape(self, qsop: str) -> dict:
        # The width diagnostic (min-fill / cut-rank) can itself blow past the timeout on a wide
        # circuit; treat that like a missing shape rather than letting it abort the whole batch.
        try:
            completed = run([self.sop_stats, "--json", "-"], stdin=qsop, timeout=self.timeout)
        except subprocess.TimeoutExpired:
            return {}
        if completed.returncode != 0:
            return {}
        try:
            parsed = json.loads(completed.stdout)
        except json.JSONDecodeError:
            return {}
        return {key: parsed.get(key, "") for key in SHAPE_KEYS}

    def solve(self, qsop: str) -> tuple[str, float, dict]:
        command = [self.sop_solve, "--format", "stats", "--include-result", *self.solve_args, "-"]
        limiter = _address_space_limiter(self.mem_limit_bytes)
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

    def _case(self, path: pathlib.Path) -> dict:
        row = {field: "" for field in FIELDS}
        row["suite"] = path.parent.parent.parent.name if path.parent.name == "payloads" \
            else path.parent.name
        row["instance"] = path.stem

        load_started = time.monotonic()
        try:
            qasm, qubits = self.translate(path)
        except Exception as exc:  # qiskit / non-unitary rejection / parse
            row["outcome"] = "import-failed"
            row["import_mode"] = str(exc).splitlines()[0][:80] if str(exc) else repr(exc)
            row["load_s"] = f"{time.monotonic() - load_started:.3f}"
            return row
        row["load_s"] = f"{time.monotonic() - load_started:.3f}"
        row["qubits"] = qubits

        zero = "0" * qubits
        import_started = time.monotonic()
        try:
            (qsop, row["import_mode"], row["exact_error_class"],
             row["exact_error"]) = self.import_qsop(qasm, zero, self.import_only)
        except (RuntimeError, subprocess.TimeoutExpired) as exc:
            row["outcome"] = "import-failed"
            diagnostic = (manifest_tool.diagnostic_from_exception(exc)
                          if str(exc) else "timeout")
            row["import_mode"] = diagnostic[:80]
            row["exact_error"] = diagnostic
            row["exact_error_class"] = manifest_tool.classify_error(diagnostic)
            row["import_s"] = f"{time.monotonic() - import_started:.3f}"
            return row
        row["import_s"] = f"{time.monotonic() - import_started:.3f}"

        if self.import_only:
            row["outcome"] = "imported"
            return row

        row.update(self.shape(qsop))
        row["outcome"], wall, stats = self.solve(qsop)
        row["wall_s"] = f"{wall:.3f}"
        row.update(stats)
        return row

    def case(self, path: pathlib.Path) -> dict:
        # A batch of thousands must never die on one pathological instance: any escape from the
        # per-stage handling below lands here as an `error:` row so the sweep keeps going.
        try:
            return self._case(path)
        except Exception as exc:  # noqa: BLE001 -- deliberate batch-level backstop
            return {
                **{field: "" for field in FIELDS},
                "suite": path.parent.parent.parent.name
                if path.parent.name == "payloads" else path.parent.name,
                "instance": path.stem,
                "outcome": "error",
                "import_mode": f"{type(exc).__name__}: {exc}".splitlines()[0][:80],
            }


def collect(paths: list[pathlib.Path]) -> list[pathlib.Path]:
    found: list[pathlib.Path] = []
    for path in paths:
        if path.is_dir():
            found.extend(sorted(path.rglob("*.qpy")))
        else:
            found.append(path)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", type=pathlib.Path,
                        help="directories of .qpy (searched recursively) or individual files")
    parser.add_argument("-o", "--out", type=pathlib.Path, help="CSV destination (default: stdout)")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="per-subprocess timeout in seconds (default: the gauntlet's 120)")
    parser.add_argument("--jobs", type=int, default=1,
                        help="run this many cases in parallel. Only for triage: a wide DP holds "
                             "gigabytes, so parallel cases contend for bandwidth and can OOM. Any "
                             "number you intend to publish wants --jobs 1")
    parser.add_argument("--build", type=pathlib.Path, default=REPO_ROOT / "build-rel",
                        help="build directory to shell out to (default: build-rel, the release "
                             "tree; timing off a debug build is not representative)")
    parser.add_argument("--mem-max-gib", type=float, default=12.0,
                        help="RLIMIT_AS cap on the solve child, in GiB (default 12; 0 disables). A "
                             "runaway DP then fails its own allocation instead of OOM-killing the "
                             "machine")
    parser.add_argument("--solve-arg", action="append", default=[], dest="solve_args",
                        help="extra flag forwarded to sop-solve; repeatable")
    parser.add_argument("--import-only", action="store_true",
                        help="stop after qasm2sop; skip shape diagnostics and solving")
    args = parser.parse_args()

    mem_limit_bytes = int(args.mem_max_gib * (1 << 30))
    bench = Bench(args.build / "qasm2sop", args.build / "sop-stats", args.build / "sop-solve",
                  args.timeout, args.solve_args, mem_limit_bytes, args.import_only)
    cases = collect(args.paths)
    if not cases:
        print("no .qpy found", file=sys.stderr)
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
                          f"load={row['load_s']}s import={row['import_s']}s solve={row['wall_s']}s",
                          file=sys.stderr)
    finally:
        if args.out:
            handle.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
