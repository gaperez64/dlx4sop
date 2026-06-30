#!/usr/bin/env python3
"""Retune branch rankwidth-delegation policy on focused SOP corpora.

The output is an experiment artifact, not a canonical scoreboard input by itself. Use
rerun_branch_scoreboard.py with the selected profile to refresh the branch rows.
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[0]
sys.path.insert(0, str(TOOLS_DIR))

from bench_common import read_jsonl, write_jsonl_record  # noqa: E402
from branch_policy_profiles import PROFILES, profile_by_name, profile_dict  # noqa: E402


DEFAULT_SOP_SOLVE = (
    REPO_ROOT / "build-bench" / "sop-solve"
    if (REPO_ROOT / "build-bench" / "sop-solve").exists()
    else REPO_ROOT / "build" / "sop-solve"
)

RETUNE_DATASETS = (
    {
        "name": "local-core",
        "corpus": REPO_ROOT / "benchmarks" / "corpus" / "sop",
        "tiers": ("tier-1-8", "tier-9-16", "tier-17-32", "tier-33-64"),
        "max_vars": 128,
    },
    {
        "name": "mqt-large",
        "corpus": REPO_ROOT
        / "benchmarks"
        / "corpus"
        / "sop"
        / "materialized-external"
        / "mqt-bench",
        "tiers": ("tier-33-64", "tier-65-128"),
        "max_vars": 128,
    },
    {
        "name": "rankwidth-separation",
        "corpus": REPO_ROOT / "benchmarks" / "corpus" / "sop" / "synthetic" / "rankwidth",
        "tiers": ("tier-rankwidth",),
        "max_vars": 128,
    },
)


def run_dataset(
    *,
    profile_name: str,
    profile_args: list[str],
    dataset: dict[str, Any],
    sop_solve: pathlib.Path,
    timeout: float,
    memory_limit_mib: int | None,
    cgroup_memory_limit_mib: int | None,
) -> list[dict]:
    with tempfile.TemporaryDirectory(prefix="dlx4sop-branch-retune-") as tmp:
        output = pathlib.Path(tmp) / "records.jsonl"
        cmd = [
            sys.executable,
            str(TOOLS_DIR / "bench_sop_local.py"),
            "--sop-solve",
            str(sop_solve),
            "--corpus-dir",
            str(dataset["corpus"]),
            "--backend",
            "branch:auto",
            "--timeout",
            str(timeout),
            "--max-vars",
            str(dataset["max_vars"]),
            "--out",
            str(output),
            "--quiet",
            *profile_args,
        ]
        for tier in dataset["tiers"]:
            cmd.extend(["--tier", str(tier)])
        if cgroup_memory_limit_mib is not None:
            cmd.extend(["--cgroup-memory-limit-mib", str(cgroup_memory_limit_mib)])
        if memory_limit_mib is not None:
            cmd.extend(["--memory-limit-mib", str(memory_limit_mib)])
        print(f"+ {' '.join(cmd)}", file=sys.stderr)
        completed = subprocess.run(cmd, text=True)
        if completed.returncode not in (0, 1):
            raise RuntimeError(f"branch retune dataset failed: {' '.join(cmd)}")
        records = read_jsonl(output) if output.exists() else []
    for record in records:
        record["branch_policy_profile"] = profile_name
        record["branch_policy_args"] = list(profile_args)
        record["retune_dataset"] = dataset["name"]
    return records


def summarize(records: list[dict], timeout: float) -> dict[str, Any]:
    by_profile: dict[str, list[dict]] = collections.defaultdict(list)
    for record in records:
        by_profile[str(record.get("branch_policy_profile", ""))].append(record)

    timeout_penalty_ns = int(timeout * 1_000_000_000)
    rows = []
    for profile in PROFILES:
        profile_records = by_profile.get(profile.name, [])
        ok_records = [r for r in profile_records if r.get("status") == "ok"]
        penalty_ns = sum(
            timeout_penalty_ns for r in profile_records if r.get("status") != "ok"
        )
        solve_ns = sum(int(r.get("solve_elapsed_ns", r.get("elapsed_ns", 0))) for r in ok_records)
        stats = collections.Counter()
        for record in ok_records:
            for key in (
                "treewidth_delegations",
                "rankwidth_delegations",
                "branch_fallthroughs",
                "branch_rankwidth_skips",
                "branch_treewidth_skips",
            ):
                value = record.get("stats", {}).get(key, record.get(key, 0))
                if isinstance(value, int):
                    stats[key] += value
        rows.append(
            {
                "profile": profile.name,
                "description": profile.description,
                "records": len(profile_records),
                "ok": len(ok_records),
                "non_ok": len(profile_records) - len(ok_records),
                "solve_elapsed_ns": solve_ns,
                "score_ns": solve_ns + penalty_ns,
                "treewidth_delegations": stats["treewidth_delegations"],
                "rankwidth_delegations": stats["rankwidth_delegations"],
                "branch_fallthroughs": stats["branch_fallthroughs"],
                "branch_rankwidth_skips": stats["branch_rankwidth_skips"],
                "branch_treewidth_skips": stats["branch_treewidth_skips"],
            }
        )

    winner = min(rows, key=lambda row: (-row["ok"], row["score_ns"], row["profile"]))
    return {
        "schema": "branch_retune_summary_v1",
        "winner": winner["profile"],
        "winner_args": list(profile_by_name(winner["profile"]).args),
        "profiles": [profile_dict(profile) for profile in PROFILES],
        "rows": rows,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sop-solve", type=pathlib.Path, default=DEFAULT_SOP_SOLVE)
    parser.add_argument("--out", type=pathlib.Path, default=REPO_ROOT / "artifacts" / "full" / "branch-retune-current.jsonl")
    parser.add_argument(
        "--summary-out",
        type=pathlib.Path,
        default=REPO_ROOT / "artifacts" / "full" / "branch-retune-summary.json",
    )
    parser.add_argument("--timeout", type=float, default=30.0)
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
    parser.add_argument(
        "--profile",
        action="append",
        dest="profiles",
        help="restrict to a named profile; may be repeated",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.timeout <= 0:
        print("error: --timeout must be positive", file=sys.stderr)
        return 2
    if args.memory_limit_mib is not None and args.memory_limit_mib <= 0:
        print("error: --memory-limit-mib must be positive", file=sys.stderr)
        return 2
    if args.cgroup_memory_limit_mib is not None and args.cgroup_memory_limit_mib <= 0:
        print("error: --cgroup-memory-limit-mib must be positive", file=sys.stderr)
        return 2
    if not args.sop_solve.exists():
        print(f"error: sop-solve not found: {args.sop_solve}", file=sys.stderr)
        return 2

    selected = [profile_by_name(name) for name in args.profiles] if args.profiles else list(PROFILES)
    all_records: list[dict] = []
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as stream:
        for profile in selected:
            for dataset in RETUNE_DATASETS:
                if not pathlib.Path(dataset["corpus"]).exists():
                    print(f"warning: missing dataset {dataset['corpus']}", file=sys.stderr)
                    continue
                records = run_dataset(
                    profile_name=profile.name,
                    profile_args=list(profile.args),
                    dataset=dataset,
                    sop_solve=args.sop_solve,
                    timeout=args.timeout,
                    memory_limit_mib=args.memory_limit_mib,
                    cgroup_memory_limit_mib=args.cgroup_memory_limit_mib,
                )
                for record in records:
                    write_jsonl_record(stream, record)
                all_records.extend(records)

    summary = summarize(all_records, args.timeout)
    args.summary_out.parent.mkdir(parents=True, exist_ok=True)
    args.summary_out.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(
        f"winner: {summary['winner']} ({' '.join(str(a) for a in summary['winner_args'])})",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
