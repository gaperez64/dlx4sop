#!/usr/bin/env python3
"""Run all corpus benchmarks and refresh the scoreboard.

Orchestrates solver, WMC, and native-simulator runs across all tiers, then
invokes refresh_scoreboard.py to regenerate scoreboard.md.  Heavy jobs
(large-tier rankwidth/branch) may take 30-60 minutes.

Usage (full rerun):
    python3 tools/run_corpus_benchmarks.py \\
        --artifact-dir /tmp/dlx4sop-artifacts \\
        --ganak /tmp/ganak/ganak \\
        --manifests benchmarks/manifests

    python3 tools/refresh_scoreboard.py \\
        --artifact-dir /tmp/dlx4sop-artifacts \\
        --allow-missing \\
        --output scoreboard.md
"""

import argparse
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
TOOLS_DIR = REPO_ROOT / "tools"

def _default_binary(name: str) -> pathlib.Path:
    """Prefer the optimized build-bench binary for benchmarking; fall back to the debug build."""
    optimized = REPO_ROOT / "build-bench" / name
    return optimized if optimized.exists() else REPO_ROOT / "build" / name


SOLVER_TIERS = ("0-32", "33-64", "65-128", "129-256", "257-512 sample")
NATIVE_TIERS = ("0-32", "33-64", "65-128", "129-256")
WMC_RESIDUE_TIERS = ("0-32", "33-64")
MQT_SOLVER_TIERS = ("33-64", "65-128")

SOLVER_TIMEOUT = {
    "0-32": 30,
    "33-64": 30,
    "65-128": 30,
    "129-256": 30,
    "257-512 sample": 30,
}
WMC_TIMEOUT = {
    "0-32": 30,
    "33-64": 30,
    "65-128": 30,
    "129-256": 30,
    "257-512 sample": 30,
}
WMC_SOP_SOLVE_TIMEOUT = {
    "0-32": 30,
    "33-64": 30,
    "65-128": 30,
    "129-256": 30,
    "257-512 sample": 30,
}
TIER_MAX_VARS = {
    "0-32": 32,
    "33-64": 64,
    "65-128": 128,
    "129-256": 256,
    "257-512 sample": 512,
}

# Map tier → list of (output_stem, bench_qasm_corpus.py extra args)
SOLVER_JOBS: dict[str, list[tuple[str, list[str]]]] = {
    "0-32": [
        ("dlx4sop-tier-0-32-treewidth-current", [
            "--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
        ]),
        ("dlx4sop-tier-0-32-rankwidth-current", [
            "--backend", "rankwidth",
        ]),
        ("dlx4sop-tier-0-32-branch-hybrid-current", [
            "--backend", "branch", "--branch-heuristic", "split",
        ]),
    ],
    "33-64": [
        ("dlx4sop-tier-33-64-treewidth-fresh", [
            "--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
        ]),
        ("dlx4sop-tier-33-64-branch-hybrid-current", [
            "--backend", "branch", "--branch-heuristic", "split",
        ]),
        ("dlx4sop-tier-33-64-rankwidth-min-fill-cut-current", [
            "--backend", "rankwidth",
            "--rankwidth-generate", "min-fill-cut",
            "--rankwidth-mode", "count-table",
        ]),
    ],
    "65-128": [
        ("dlx4sop-tier-65-128-treewidth-fresh", [
            "--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
        ]),
        ("dlx4sop-tier-65-128-branch-hybrid-fresh", [
            "--backend", "branch", "--branch-heuristic", "split",
        ]),
        ("dlx4sop-tier-65-128-rankwidth-min-fill-cut-current", [
            "--backend", "rankwidth",
            "--rankwidth-generate", "min-fill-cut",
            "--rankwidth-mode", "count-table",
        ]),
    ],
    "129-256": [
        ("dlx4sop-tier-129-256-treewidth-current", [
            "--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
        ]),
        ("dlx4sop-tier-129-256-branch-hybrid-current", [
            "--backend", "branch", "--branch-heuristic", "split",
        ]),
        ("dlx4sop-tier-129-256-rankwidth-min-fill-cut-current", [
            "--backend", "rankwidth",
            "--rankwidth-generate", "min-fill-cut",
            "--rankwidth-mode", "count-table",
        ]),
    ],
    "257-512 sample": [
        ("dlx4sop-tier-257-512-sample-treewidth-current", [
            "--backend", "treewidth", "--treewidth-order", "min-fill-max-degree",
        ]),
    ],
}


def tier_slug(tier: str) -> str:
    return tier.replace(" ", "-")


def manifest_path(manifests_dir: pathlib.Path, tier: str) -> pathlib.Path:
    return manifests_dir / f"dlx4sop-tier-{tier_slug(tier)}-manifest.json"


def run_to_jsonl(cmd: list, output: pathlib.Path, verbose: bool) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    if verbose:
        print(f"+ {' '.join(str(a) for a in cmd)} > {output}", file=sys.stderr)
    with output.open("w", encoding="utf-8") as stream:
        result = subprocess.run(
            [str(a) for a in cmd],
            stdout=stream,
            stderr=None if verbose else subprocess.PIPE,
            text=True,
        )
    if result.returncode != 0:
        message = result.stderr.strip() if result.stderr else ""
        raise RuntimeError(
            f"command exited {result.returncode}: {' '.join(str(a) for a in cmd)}"
            + (f"\n{message}" if message else "")
        )


def run_solver_jobs(
    args: argparse.Namespace,
    manifests_dir: pathlib.Path,
    artifact_dir: pathlib.Path,
) -> None:
    bench = TOOLS_DIR / "bench_qasm_corpus.py"
    qasm2sop = args.qasm2sop
    sop_solve = args.sop_solve

    for tier in SOLVER_TIERS:
        mf = manifest_path(manifests_dir, tier)
        if not mf.exists():
            print(f"warning: manifest missing for {tier}: {mf}", file=sys.stderr)
            continue
        timeout = str(args.timeout if args.timeout is not None else SOLVER_TIMEOUT.get(tier, 60))
        max_vars = str(TIER_MAX_VARS.get(tier, 512))
        for stem, extra_args in SOLVER_JOBS.get(tier, []):
            output = artifact_dir / f"{stem}.jsonl"
            cmd = [
                sys.executable, str(bench),
                str(qasm2sop), str(sop_solve),
                "--manifest", str(mf),
                "--solver-timeout", timeout,
                "--max-vars", max_vars,
                "--trace",
                "--format", "jsonl",
                *extra_args,
            ]
            print(f"\n--- solver: {tier} / {stem} ---", file=sys.stderr)
            run_to_jsonl(cmd, output, args.verbose)


def run_wmc_jobs(
    args: argparse.Namespace,
    manifests_dir: pathlib.Path,
    artifact_dir: pathlib.Path,
) -> None:
    bench = TOOLS_DIR / "bench_wmc_ganak.py"
    ganak = args.ganak

    for tier in SOLVER_TIERS:
        mf = manifest_path(manifests_dir, tier)
        if not mf.exists():
            print(f"warning: manifest missing for {tier}: {mf}", file=sys.stderr)
            continue
        slug = tier_slug(tier)
        timeout = str(args.timeout if args.timeout is not None else WMC_TIMEOUT.get(tier, 30))
        sop_timeout = str(args.timeout if args.timeout is not None else WMC_SOP_SOLVE_TIMEOUT.get(tier, 30))
        max_vars = TIER_MAX_VARS.get(tier, 512)
        base_cmd = [
            sys.executable, str(bench),
            "--qasm2sop", str(args.qasm2sop),
            "--sop2wmc", str(args.sop2wmc),
            "--sop-solve", str(args.sop_solve),
            "--ganak", str(ganak),
            "--manifest", str(mf),
            "--format", "jsonl",
            "--ganak-timeout", timeout,
            "--sop-solve-backend", "treewidth",
            "--sop-solve-max-vars", str(max_vars),
            "--sop-solve-timeout", sop_timeout,
        ]
        # amp-and (amplitude): all tiers
        amp_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-amplitude-current.jsonl"
        print(f"\n--- WMC amp-and: {tier} ---", file=sys.stderr)
        run_to_jsonl([*base_cmd, "--encoding", "amplitude"], amp_output, args.verbose)

        # amp-soft: all tiers
        amp_soft_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-amp-soft-current.jsonl"
        print(f"\n--- WMC amp-soft: {tier} ---", file=sys.stderr)
        run_to_jsonl([*base_cmd, "--encoding", "amp-soft"], amp_soft_output, args.verbose)

        # amp-block: all tiers
        amp_block_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-amp-block-current.jsonl"
        print(f"\n--- WMC amp-block: {tier} ---", file=sys.stderr)
        run_to_jsonl([*base_cmd, "--encoding", "amp-block"], amp_block_output, args.verbose)

        # residue-fourier: small tiers only (r calls per instance, expensive)
        if tier in WMC_RESIDUE_TIERS:
            res_fourier_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-residue-fourier-current.jsonl"
            print(f"\n--- WMC residue-fourier: {tier} ---", file=sys.stderr)
            run_to_jsonl([*base_cmd, "--encoding", "residue-fourier"], res_fourier_output, args.verbose)

        # residue (plain #SAT): small tiers only
        if tier in WMC_RESIDUE_TIERS:
            res_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-residue-current.jsonl"
            print(f"\n--- WMC residue: {tier} ---", file=sys.stderr)
            run_to_jsonl([*base_cmd, "--encoding", "residue"], res_output, args.verbose)


def run_native_jobs(
    args: argparse.Namespace,
    manifests_dir: pathlib.Path,
    artifact_dir: pathlib.Path,
) -> None:
    bench = TOOLS_DIR / "bench_qasm_native_simulator.py"

    for tier in NATIVE_TIERS:
        mf = manifest_path(manifests_dir, tier)
        if not mf.exists():
            print(f"warning: manifest missing for {tier}: {mf}", file=sys.stderr)
            continue
        slug = tier_slug(tier)
        output = artifact_dir / f"dlx4sop-tier-{slug}-native-all-current.jsonl"
        cmd = [
            sys.executable, str(bench),
            str(mf),
            "--engine", "all",
            "--max-qubits", str(args.native_max_qubits),
            "--engine-qubit-cap", f"pyzx-matrix={args.pyzx_matrix_max_qubits}",
            # qiskit-clifford is stabilizer-based (O(n^2) memory): let it run far beyond the
            # dense-statevector cap so large Clifford circuits still get a native baseline.
            "--engine-qubit-cap", f"qiskit-clifford={args.clifford_max_qubits}",
            "--timeout", str(args.timeout if args.timeout is not None else args.native_timeout),
            "--memory-limit-mib", str(args.memory_limit_mib),
            "--skip-unsupported",
            "--format", "jsonl",
        ]
        print(f"\n--- native: {tier} ---", file=sys.stderr)
        run_to_jsonl(cmd, output, args.verbose)


def run_mqt_solver_jobs(
    args: argparse.Namespace,
    artifact_dir: pathlib.Path,
) -> None:
    """Run sop-solve on the MQT materialized QSOP corpus (bench_sop_local.py per tier)."""
    bench = TOOLS_DIR / "bench_sop_local.py"
    sop_solve = args.sop_solve
    mqt_root = REPO_ROOT / "benchmarks" / "corpus" / "sop" / "materialized-external" / "mqt-bench"
    mqt_jobs = [
        ("treewidth", ["--backend", "treewidth"]),
        ("branch-hybrid", ["--backend", "branch:auto"]),
    ]
    if not mqt_root.exists():
        print(f"warning: MQT corpus root missing: {mqt_root}", file=sys.stderr)
        return
    for tier in MQT_SOLVER_TIERS:
        tier_dir = mqt_root / f"tier-{tier}"
        if not tier_dir.exists():
            print(f"warning: MQT corpus tier dir missing: {tier_dir}", file=sys.stderr)
            continue
        timeout = str(args.timeout if args.timeout is not None else SOLVER_TIMEOUT.get(tier, 30))
        max_vars = str(TIER_MAX_VARS.get(tier, 128))
        for backend_name, extra_args in mqt_jobs:
            slug = tier.replace(" ", "-")
            stem = f"mqt-bench-tier-{slug}-{backend_name}-current"
            output = artifact_dir / f"{stem}.jsonl"
            cmd = [
                sys.executable, str(bench),
                "--sop-solve", str(sop_solve),
                "--corpus-dir", str(mqt_root),
                "--tier", f"tier-{tier}",
                "--timeout", timeout,
                "--max-vars", max_vars,
                "--out", str(output),
                *extra_args,
            ]
            print(f"\n--- MQT solver: {tier} / {backend_name} ---", file=sys.stderr)
            result = subprocess.run([str(a) for a in cmd])
            if result.returncode not in (0, 1):
                print(f"warning: MQT solver job exited {result.returncode}", file=sys.stderr)


def run_mqt_native_jobs(
    args: argparse.Namespace,
    manifests_dir: pathlib.Path,
    artifact_dir: pathlib.Path,
) -> None:
    """Run native simulators on MQT QASM manifests."""
    bench = TOOLS_DIR / "bench_qasm_native_simulator.py"
    mqt_manifests = manifests_dir / "mqt"

    for tier in MQT_SOLVER_TIERS:
        slug = tier.replace(" ", "-")
        mf = mqt_manifests / f"tier-{slug}.json"
        if not mf.exists():
            print(f"warning: MQT native manifest missing: {mf}", file=sys.stderr)
            continue
        output = artifact_dir / f"mqt-bench-tier-{slug}-native-current.jsonl"
        cmd = [
            sys.executable, str(bench),
            str(mf),
            "--engine", "all",
            "--max-qubits", str(args.native_max_qubits),
            "--engine-qubit-cap", f"pyzx-matrix={args.pyzx_matrix_max_qubits}",
            # Stabilizer simulation scales to the large (34-128q) MQT circuits the dense
            # statevector engines cannot reach, so clifford is the native baseline here.
            "--engine-qubit-cap", f"qiskit-clifford={args.clifford_max_qubits}",
            "--timeout", str(args.timeout if args.timeout is not None else args.native_timeout),
            "--memory-limit-mib", str(args.memory_limit_mib),
            "--skip-unsupported",
            "--format", "jsonl",
        ]
        print(f"\n--- MQT native: {tier} ---", file=sys.stderr)
        run_to_jsonl(cmd, output, args.verbose)


SCALING_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop" / "synthetic" / "scaling"
RANKWIDTH_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop" / "synthetic" / "rankwidth"


def run_scaling_study(args: argparse.Namespace, artifact_dir: pathlib.Path) -> None:
    """WMC-vs-solver crossover study on the committed synthetic phase-polynomial corpus.

    Runs treewidth + branch (sop-solve) and ganak (sop2wmc + ganak) over the same
    high-treewidth instances so the scoreboard can plot where WMC overtakes the solver
    backends. Uses a high --max-vars so the large instances are attempted (and time out)
    rather than refused.
    """
    if not SCALING_CORPUS.exists():
        print(f"warning: scaling corpus missing: {SCALING_CORPUS}", file=sys.stderr)
        return
    timeout = str(args.scaling_timeout)
    sop_local = TOOLS_DIR / "bench_sop_local.py"
    for backend_arg, label in (("treewidth", "treewidth"), ("branch:auto", "branch")):
        output = artifact_dir / f"scaling-{label}-current.jsonl"
        cmd = [
            sys.executable, str(sop_local),
            "--sop-solve", str(args.sop_solve),
            "--corpus-dir", str(SCALING_CORPUS),
            "--tier", "tier-scaling",
            "--backend", backend_arg,
            "--timeout", timeout,
            "--max-vars", "4096",
            "--out", str(output),
        ]
        print(f"\n--- scaling solver: {label} ---", file=sys.stderr)
        result = subprocess.run([str(a) for a in cmd])
        if result.returncode not in (0, 1):
            print(f"warning: scaling solver {label} exited {result.returncode}", file=sys.stderr)

    instances = sorted((SCALING_CORPUS / "tier-scaling").glob("*.qsop"))
    if instances:
        wmc_out = artifact_dir / "scaling-wmc-current.jsonl"
        cmd = [
            sys.executable, str(TOOLS_DIR / "bench_wmc_ganak.py"),
            "--ganak", str(args.ganak), "--sop2wmc", str(args.sop2wmc),
            "--ganak-timeout", timeout, "--format", "jsonl",
            "--encoding", "amp-soft",
            *[str(p) for p in instances],
        ]
        print("\n--- scaling WMC: ganak ---", file=sys.stderr)
        run_to_jsonl(cmd, wmc_out, args.verbose)


def run_rankwidth_separation_study(args: argparse.Namespace, artifact_dir: pathlib.Path) -> None:
    """Targeted RQ2 study on the bounded-rankwidth clique-blowup tree family."""
    if not RANKWIDTH_CORPUS.exists():
        print(f"warning: rankwidth corpus missing: {RANKWIDTH_CORPUS}", file=sys.stderr)
        return
    output = artifact_dir / "rankwidth-separation-current.jsonl"
    sop_local = TOOLS_DIR / "bench_sop_local.py"
    cmd = [
        sys.executable, str(sop_local),
        "--sop-solve", str(args.sop_solve),
        "--corpus-dir", str(RANKWIDTH_CORPUS),
        "--tier", "tier-rankwidth",
        "--backend", "treewidth",
        "--backend", "rankwidth:best",
        "--backend", "rankwidth:best:fourier",
        "--timeout", str(args.rankwidth_study_timeout),
        "--max-vars", "128",
        "--out", str(output),
    ]
    print("\n--- rankwidth separation study ---", file=sys.stderr)
    result = subprocess.run([str(a) for a in cmd])
    if result.returncode not in (0, 1):
        print(f"warning: rankwidth separation study exited {result.returncode}", file=sys.stderr)


def run_scoreboard(args: argparse.Namespace, artifact_dir: pathlib.Path) -> None:
    bench = TOOLS_DIR / "bench.py"
    timeout_s = int(args.timeout) if args.timeout else 30
    cmd = [
        sys.executable, str(bench),
        "render",
        "--artifact-dir", str(artifact_dir),
        "--view", "full",
        "--timeout-note", f"{timeout_s} s",
    ]
    print("\n--- render full scoreboard ---", file=sys.stderr)
    result = subprocess.run([str(a) for a in cmd])
    if result.returncode != 0:
        print("warning: bench.py render exited non-zero", file=sys.stderr)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifact-dir", type=pathlib.Path, default=pathlib.Path("/tmp/dlx4sop-artifacts"))
    parser.add_argument("--manifests", type=pathlib.Path, default=REPO_ROOT / "benchmarks" / "manifests")
    parser.add_argument("--ganak", type=pathlib.Path, default=pathlib.Path("/tmp/ganak/ganak"))
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=_default_binary("qasm2sop"))
    parser.add_argument("--sop2wmc", type=pathlib.Path, default=_default_binary("sop2wmc"))
    parser.add_argument("--sop-solve", type=pathlib.Path, default=_default_binary("sop-solve"))
    parser.add_argument("--output", type=pathlib.Path, default=REPO_ROOT / "scoreboard.md")
    parser.add_argument("--skip-solver", action="store_true", help="skip solver backend jobs")
    parser.add_argument("--skip-wmc", action="store_true", help="skip WMC jobs")
    parser.add_argument("--skip-native", action="store_true", help="skip native simulator jobs")
    parser.add_argument("--skip-scoreboard", action="store_true", help="skip scoreboard refresh")
    parser.add_argument("--timeout", type=float, default=None,
                        help="override all per-tier timeouts (solver, WMC, native) with this value")
    parser.add_argument("--native-max-qubits", type=int, default=16)
    parser.add_argument("--pyzx-matrix-max-qubits", type=int, default=10)
    parser.add_argument("--clifford-max-qubits", type=int, default=128,
                        help="qubit cap for the stabilizer (qiskit-clifford) engine; it scales "
                             "far past the dense-statevector cap")
    parser.add_argument("--native-timeout", type=float, default=10.0)
    parser.add_argument("--memory-limit-mib", type=int, default=4096)
    parser.add_argument("--skip-scaling", action="store_true",
                        help="skip the WMC-vs-solver scaling study on the committed synthetic corpus")
    parser.add_argument("--skip-rankwidth-study", action="store_true",
                        help="skip the bounded-rankwidth synthetic separation study")
    parser.add_argument("--scaling-timeout", type=float, default=30.0,
                        help="per-instance timeout (s) for the scaling study backends "
                             "(CI uses a small cap)")
    parser.add_argument("--rankwidth-study-timeout", type=float, default=30.0,
                        help="per-instance timeout (s) for the bounded-rankwidth study")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    artifact_dir = args.artifact_dir
    artifact_dir.mkdir(parents=True, exist_ok=True)
    manifests_dir = args.manifests

    if "build-bench" not in str(args.sop_solve):
        print("warning: sop-solve is not from an optimized build (build-bench/); timings will "
              "be slow. Build it with: meson setup build-bench --buildtype=release -Db_lto=true",
              file=sys.stderr)

    if not args.skip_solver:
        run_solver_jobs(args, manifests_dir, artifact_dir)
        run_mqt_solver_jobs(args, artifact_dir)
    if not args.skip_wmc:
        run_wmc_jobs(args, manifests_dir, artifact_dir)
    if not args.skip_native:
        run_native_jobs(args, manifests_dir, artifact_dir)
        run_mqt_native_jobs(args, manifests_dir, artifact_dir)
    if not args.skip_scaling:
        run_scaling_study(args, artifact_dir)
    if not args.skip_rankwidth_study:
        run_rankwidth_separation_study(args, artifact_dir)
    if not args.skip_scoreboard:
        run_scoreboard(args, artifact_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
