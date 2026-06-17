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

SOLVER_TIERS = ("0-32", "33-64", "65-128", "129-256", "257-512 sample")
NATIVE_TIERS = ("33-64", "65-128", "129-256")
WMC_RESIDUE_TIERS = ("0-32", "33-64")

SOLVER_TIMEOUT = {
    "0-32": 30,
    "33-64": 60,
    "65-128": 300,
    "129-256": 750,
    "257-512 sample": 30,
}
WMC_TIMEOUT = {
    "0-32": 30,
    "33-64": 60,
    "65-128": 120,
    "129-256": 300,
    "257-512 sample": 60,
}
WMC_SOP_SOLVE_TIMEOUT = {
    "0-32": 30,
    "33-64": 60,
    "65-128": 120,
    "129-256": 300,
    "257-512 sample": 60,
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


def run_to_jsonl(cmd: list, output: pathlib.Path, verbose: bool) -> bool:
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
    if result.returncode not in (0, 1):
        print(
            f"warning: command exited {result.returncode}: {' '.join(str(a) for a in cmd)}",
            file=sys.stderr,
        )
        return False
    return True


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
        # Amplitude: all tiers
        amp_output = artifact_dir / f"dlx4sop-tier-{slug}-wmc-amplitude-current.jsonl"
        print(f"\n--- WMC amplitude: {tier} ---", file=sys.stderr)
        run_to_jsonl([*base_cmd, "--encoding", "amplitude"], amp_output, args.verbose)

        # Residue: only small tiers
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
            "--timeout", str(args.timeout if args.timeout is not None else args.native_timeout),
            "--memory-limit-mib", str(args.memory_limit_mib),
            "--skip-unsupported",
            "--format", "jsonl",
        ]
        print(f"\n--- native: {tier} ---", file=sys.stderr)
        run_to_jsonl(cmd, output, args.verbose)


def run_scoreboard(args: argparse.Namespace, artifact_dir: pathlib.Path) -> None:
    refresh = TOOLS_DIR / "refresh_scoreboard.py"
    cmd = [
        sys.executable, str(refresh),
        "--artifact-dir", str(artifact_dir),
        "--allow-missing",
        "--output", str(args.output),
    ]
    print(f"\n--- refresh scoreboard → {args.output} ---", file=sys.stderr)
    result = subprocess.run([str(a) for a in cmd])
    if result.returncode != 0:
        print("warning: refresh_scoreboard.py exited non-zero", file=sys.stderr)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifact-dir", type=pathlib.Path, default=pathlib.Path("/tmp/dlx4sop-artifacts"))
    parser.add_argument("--manifests", type=pathlib.Path, default=REPO_ROOT / "benchmarks" / "manifests")
    parser.add_argument("--ganak", type=pathlib.Path, default=pathlib.Path("/tmp/ganak/ganak"))
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=REPO_ROOT / "build" / "qasm2sop")
    parser.add_argument("--sop2wmc", type=pathlib.Path, default=REPO_ROOT / "build" / "sop2wmc")
    parser.add_argument("--sop-solve", type=pathlib.Path, default=REPO_ROOT / "build" / "sop-solve")
    parser.add_argument("--output", type=pathlib.Path, default=REPO_ROOT / "scoreboard.md")
    parser.add_argument("--skip-solver", action="store_true", help="skip solver backend jobs")
    parser.add_argument("--skip-wmc", action="store_true", help="skip WMC jobs")
    parser.add_argument("--skip-native", action="store_true", help="skip native simulator jobs")
    parser.add_argument("--skip-scoreboard", action="store_true", help="skip scoreboard refresh")
    parser.add_argument("--timeout", type=float, default=None,
                        help="override all per-tier timeouts (solver, WMC, native) with this value")
    parser.add_argument("--native-max-qubits", type=int, default=16)
    parser.add_argument("--pyzx-matrix-max-qubits", type=int, default=10)
    parser.add_argument("--native-timeout", type=float, default=10.0)
    parser.add_argument("--memory-limit-mib", type=int, default=4096)
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    artifact_dir = args.artifact_dir
    artifact_dir.mkdir(parents=True, exist_ok=True)
    manifests_dir = args.manifests

    if not args.skip_solver:
        run_solver_jobs(args, manifests_dir, artifact_dir)
    if not args.skip_wmc:
        run_wmc_jobs(args, manifests_dir, artifact_dir)
    if not args.skip_native:
        run_native_jobs(args, manifests_dir, artifact_dir)
    if not args.skip_scoreboard:
        run_scoreboard(args, artifact_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
