#!/usr/bin/env python3
"""Rerun canonical branch artifacts with a selected branch policy profile."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[0]
sys.path.insert(0, str(TOOLS_DIR))

from branch_policy_profiles import profile_args, profile_by_name  # noqa: E402


DEFAULT_QASM2SOP = (
    REPO_ROOT / "build-bench" / "qasm2sop"
    if (REPO_ROOT / "build-bench" / "qasm2sop").exists()
    else REPO_ROOT / "build" / "qasm2sop"
)
DEFAULT_SOP_SOLVE = (
    REPO_ROOT / "build-bench" / "sop-solve"
    if (REPO_ROOT / "build-bench" / "sop-solve").exists()
    else REPO_ROOT / "build" / "sop-solve"
)

QASM_BRANCH_JOBS = (
    ("0-32", "dlx4sop-tier-0-32-branch-hybrid-current.jsonl", 32),
    ("33-64", "dlx4sop-tier-33-64-branch-hybrid-current.jsonl", 64),
    ("65-128", "dlx4sop-tier-65-128-branch-hybrid-fresh.jsonl", 128),
    ("129-256", "dlx4sop-tier-129-256-branch-hybrid-current.jsonl", 256),
)

MQT_BRANCH_JOBS = (
    ("33-64", "mqt-bench-tier-33-64-branch-hybrid-current.jsonl", 64),
    ("65-128", "mqt-bench-tier-65-128-branch-hybrid-current.jsonl", 128),
)


def run_stdout_to_file(cmd: list[str], output: pathlib.Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    print(f"+ {' '.join(cmd)} > {output}", file=sys.stderr)
    with output.open("w", encoding="utf-8") as stream:
        completed = subprocess.run(cmd, stdout=stream, text=True)
    if completed.returncode != 0:
        raise RuntimeError(f"command exited {completed.returncode}: {' '.join(cmd)}")


def run_cmd(cmd: list[str]) -> None:
    print(f"+ {' '.join(cmd)}", file=sys.stderr)
    completed = subprocess.run(cmd, text=True)
    if completed.returncode not in (0, 1):
        raise RuntimeError(f"command exited {completed.returncode}: {' '.join(cmd)}")


def profile_from_summary(path: pathlib.Path) -> str:
    data = json.loads(path.read_text(encoding="utf-8"))
    winner = data.get("winner")
    if not isinstance(winner, str) or not winner:
        raise RuntimeError(f"{path} does not contain a winner profile")
    return winner


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-dir", type=pathlib.Path, default=REPO_ROOT / "artifacts" / "full")
    parser.add_argument("--manifests", type=pathlib.Path, default=REPO_ROOT / "benchmarks" / "manifests")
    parser.add_argument("--profile", help="branch policy profile to use")
    parser.add_argument(
        "--summary",
        type=pathlib.Path,
        default=REPO_ROOT / "artifacts" / "full" / "branch-retune-summary.json",
        help="retune summary used when --profile is omitted",
    )
    parser.add_argument("--qasm2sop", type=pathlib.Path, default=DEFAULT_QASM2SOP)
    parser.add_argument("--sop-solve", type=pathlib.Path, default=DEFAULT_SOP_SOLVE)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--scaling-timeout", type=float, default=30.0)
    parser.add_argument(
        "--memory-limit-mib",
        type=int,
        default=None,
        help="per-solve address-space cap; lower overhead than systemd-run scopes",
    )
    parser.add_argument(
        "--cgroup-memory-limit-mib",
        type=int,
        default=None,
        help="per-solve cgroup physical-memory cap; set only when systemd-run overhead is acceptable",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    profile_name = args.profile or profile_from_summary(args.summary)
    profile = profile_by_name(profile_name)
    branch_args = profile_args(profile.name)
    if args.timeout <= 0 or args.scaling_timeout <= 0:
        print("error: timeouts must be positive", file=sys.stderr)
        return 2
    if args.memory_limit_mib is not None and args.memory_limit_mib <= 0:
        print("error: --memory-limit-mib must be positive", file=sys.stderr)
        return 2
    if args.cgroup_memory_limit_mib is not None and args.cgroup_memory_limit_mib <= 0:
        print("error: --cgroup-memory-limit-mib must be positive", file=sys.stderr)
        return 2
    for path in (args.qasm2sop, args.sop_solve):
        if not path.exists():
            print(f"error: missing binary: {path}", file=sys.stderr)
            return 2

    qasm_bench = TOOLS_DIR / "bench_qasm_corpus.py"
    local_bench = TOOLS_DIR / "bench_sop_local.py"

    for tier, artifact_name, max_vars in QASM_BRANCH_JOBS:
        manifest = args.manifests / f"dlx4sop-tier-{tier}-manifest.json"
        if not manifest.exists():
            print(f"warning: missing manifest {manifest}", file=sys.stderr)
            continue
        output = args.artifact_dir / artifact_name
        cmd = [
            sys.executable,
            str(qasm_bench),
            str(args.qasm2sop),
            str(args.sop_solve),
            "--manifest",
            str(manifest),
            "--solver-timeout",
            str(args.timeout),
            "--max-vars",
            str(max_vars),
            "--trace",
            "--format",
            "jsonl",
            "--backend",
            "branch",
            "--branch-heuristic",
            "split",
            *branch_args,
        ]
        if args.cgroup_memory_limit_mib is not None:
            cmd.extend(["--cgroup-memory-limit-mib", str(args.cgroup_memory_limit_mib)])
        if args.memory_limit_mib is not None:
            cmd.extend(["--memory-limit-mib", str(args.memory_limit_mib)])
        run_stdout_to_file(cmd, output)

    mqt_root = (
        REPO_ROOT / "benchmarks" / "corpus" / "sop" / "materialized-external" / "mqt-bench"
    )
    for tier, artifact_name, max_vars in MQT_BRANCH_JOBS:
        tier_dir = mqt_root / f"tier-{tier}"
        if not tier_dir.exists():
            print(f"warning: missing MQT tier {tier_dir}", file=sys.stderr)
            continue
        output = args.artifact_dir / artifact_name
        cmd = [
            sys.executable,
            str(local_bench),
            "--sop-solve",
            str(args.sop_solve),
            "--corpus-dir",
            str(mqt_root),
            "--tier",
            f"tier-{tier}",
            "--backend",
            "branch:auto",
            "--timeout",
            str(args.timeout),
            "--max-vars",
            str(max_vars),
            "--out",
            str(output),
            "--quiet",
            *branch_args,
        ]
        if args.cgroup_memory_limit_mib is not None:
            cmd.extend(["--cgroup-memory-limit-mib", str(args.cgroup_memory_limit_mib)])
        if args.memory_limit_mib is not None:
            cmd.extend(["--memory-limit-mib", str(args.memory_limit_mib)])
        run_cmd(cmd)

    scaling_root = REPO_ROOT / "benchmarks" / "corpus" / "sop" / "synthetic" / "scaling"
    if scaling_root.exists():
        output = args.artifact_dir / "scaling-branch-current.jsonl"
        cmd = [
            sys.executable,
            str(local_bench),
            "--sop-solve",
            str(args.sop_solve),
            "--corpus-dir",
            str(scaling_root),
            "--tier",
            "tier-scaling",
            "--backend",
            "branch:auto",
            "--timeout",
            str(args.scaling_timeout),
            "--max-vars",
            "4096",
            "--out",
            str(output),
            "--quiet",
            *branch_args,
        ]
        if args.cgroup_memory_limit_mib is not None:
            cmd.extend(["--cgroup-memory-limit-mib", str(args.cgroup_memory_limit_mib)])
        if args.memory_limit_mib is not None:
            cmd.extend(["--memory-limit-mib", str(args.memory_limit_mib)])
        run_cmd(cmd)

    print(f"refreshed branch artifacts with profile {profile.name}: {profile.description}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
