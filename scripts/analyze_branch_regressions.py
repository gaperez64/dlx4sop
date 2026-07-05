#!/usr/bin/env python3
"""Branch regression analyzer.

Reads sop_bench_result_v2 JSONL artifacts and produces a Markdown/JSON
report that answers:

  - Is branch:auto slower than treewidth because it delegated to rankwidth?
  - Or because it probed rankwidth and skipped it?
  - Or because treewidth-order/root probes, cache, or fallthrough behavior changed?

Usage:
    python3 scripts/analyze_branch_regressions.py \\
        --artifact-dir artifacts/full \\
        --output artifacts/branch-regression-summary.md \\
        --json artifacts/branch-regression-summary.json
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import sys
from dataclasses import dataclass, field
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

VARIANTS = [
    "treewidth",
    "branch:auto",
    "branch:no-rankwidth",
    "branch:from-treewidth",
    "branch:native",
    "rankwidth:linear",
    "rankwidth:from-treewidth",
    "rankwidth:best",
]

# Tiers in display order
TIER_ORDER = ["0-32", "33-64", "65-128", "129-256", "257-512 sample"]


def _tier_sort_key(tier: str) -> int:
    m = re.match(r"(\d+)", tier)
    return int(m.group(1)) if m else 9999


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def _normalize_backend(record: dict) -> str:
    """Return a canonical backend variant name from a bench result record."""
    backend = record.get("backend", "")
    cfg = record.get("backend_config", {})
    rw_source = cfg.get("branch_rw_source", "")
    if backend == "branch":
        if rw_source in ("none", ""):
            if "no-rankwidth" in str(record.get("backend", "")):
                return "branch:no-rankwidth"
            # Treat missing rw_source as native
            return "branch:auto"
        mapping = {
            "auto": "branch:auto",
            "from-treewidth": "branch:from-treewidth",
            "native": "branch:native",
            "none": "branch:no-rankwidth",
            "both": "branch:auto",
        }
        return mapping.get(rw_source, f"branch:{rw_source}")
    return backend


def _tier_of(record: dict) -> str:
    tier = record.get("tier", "")
    # Normalize: "tier-17-32" → "0-32" style labels used in scoreboard
    # Keep raw tier string; caller can normalize.
    return tier


@dataclass
class InstanceRecord:
    instance_id: str
    tier: str
    backend: str
    elapsed_ns: int
    status: str
    stats: dict = field(default_factory=dict)


def load_jsonl_dir(artifact_dir: pathlib.Path) -> list[InstanceRecord]:
    records = []
    for jf in sorted(artifact_dir.glob("**/*.jsonl")):
        try:
            with open(jf, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    schema = obj.get("schema", "")
                    if schema not in ("sop_bench_result_v2", "sop_bench_result"):
                        continue
                    instance_id = obj.get("instance_id", obj.get("name", ""))
                    tier = obj.get("tier", "")
                    backend = obj.get("backend", "")
                    # Prefer canonical backend name from config
                    canonical = _normalize_backend(obj)
                    elapsed_ns = int(obj.get("elapsed_ns", obj.get("solve_elapsed_ns", 0)))
                    status = obj.get("status", "")
                    stats = obj.get("stats", {})
                    records.append(InstanceRecord(
                        instance_id=instance_id,
                        tier=tier,
                        backend=canonical,
                        elapsed_ns=elapsed_ns,
                        status=status,
                        stats=stats,
                    ))
        except OSError:
            pass
    return records


# ---------------------------------------------------------------------------
# Aggregation helpers
# ---------------------------------------------------------------------------

def group_by_tier_backend(
    records: list[InstanceRecord],
) -> dict[str, dict[str, list[InstanceRecord]]]:
    """Returns {tier: {backend: [records]}}."""
    result: dict[str, dict[str, list[InstanceRecord]]] = collections.defaultdict(
        lambda: collections.defaultdict(list)
    )
    for rec in records:
        result[rec.tier][rec.backend].append(rec)
    return result


def sum_elapsed(recs: list[InstanceRecord]) -> int:
    return sum(r.elapsed_ns for r in recs if r.status == "ok")


def count_ok(recs: list[InstanceRecord]) -> int:
    return sum(1 for r in recs if r.status == "ok")


def _stat_sum(recs: list[InstanceRecord], key: str) -> int:
    total = 0
    for r in recs:
        v = r.stats.get(key, 0)
        if isinstance(v, (int, float)):
            total += int(v)
    return total


def _ratio_str(numerator: int, denominator: int) -> str:
    if denominator == 0:
        return "N/A"
    return f"{numerator / denominator:.3f}x"


# ---------------------------------------------------------------------------
# Top offender analysis
# ---------------------------------------------------------------------------

def top_offenders_branch_vs_treewidth(
    records: list[InstanceRecord], top_n: int = 20
) -> list[dict]:
    """Top instances where branch:auto loses most time to treewidth."""
    by_instance: dict[str, dict[str, InstanceRecord]] = collections.defaultdict(dict)
    for r in records:
        by_instance[r.instance_id][r.backend] = r

    rows = []
    for iid, variants in by_instance.items():
        tw = variants.get("treewidth")
        br = variants.get("branch:auto")
        if tw is None or br is None:
            continue
        if tw.status != "ok" or br.status != "ok":
            continue
        if tw.elapsed_ns == 0:
            continue
        ratio = br.elapsed_ns / tw.elapsed_ns
        excess_ns = br.elapsed_ns - tw.elapsed_ns
        rw_delegations = br.stats.get("rankwidth_delegations", 0)
        rw_skips = br.stats.get("branch_rankwidth_skips", 0)
        rows.append({
            "instance_id": iid,
            "tier": br.tier,
            "ratio": ratio,
            "branch_ns": br.elapsed_ns,
            "treewidth_ns": tw.elapsed_ns,
            "excess_ns": excess_ns,
            "rw_delegations": rw_delegations,
            "rw_skips": rw_skips,
        })

    rows.sort(key=lambda r: r["excess_ns"], reverse=True)
    return rows[:top_n]


def top_offenders_branch_vs_no_rankwidth(
    records: list[InstanceRecord], top_n: int = 20
) -> list[dict]:
    """Top instances where branch:auto loses most to branch:no-rankwidth."""
    by_instance: dict[str, dict[str, InstanceRecord]] = collections.defaultdict(dict)
    for r in records:
        by_instance[r.instance_id][r.backend] = r

    rows = []
    for iid, variants in by_instance.items():
        nr = variants.get("branch:no-rankwidth")
        br = variants.get("branch:auto")
        if nr is None or br is None:
            continue
        if nr.status != "ok" or br.status != "ok":
            continue
        if nr.elapsed_ns == 0:
            continue
        ratio = br.elapsed_ns / nr.elapsed_ns
        excess_ns = br.elapsed_ns - nr.elapsed_ns
        rows.append({
            "instance_id": iid,
            "tier": br.tier,
            "ratio": ratio,
            "branch_ns": br.elapsed_ns,
            "no_rw_ns": nr.elapsed_ns,
            "excess_ns": excess_ns,
        })

    rows.sort(key=lambda r: r["excess_ns"], reverse=True)
    return rows[:top_n]


def top_rw_probe_overhead(
    records: list[InstanceRecord], top_n: int = 20
) -> list[dict]:
    """Top branch:auto instances with high rankwidth probe time but zero delegation."""
    rows = []
    for r in records:
        if r.backend != "branch:auto" or r.status != "ok":
            continue
        delegations = r.stats.get("rankwidth_delegations", 0)
        skips = r.stats.get("branch_rankwidth_skips", 0)
        if delegations > 0 or skips == 0:
            continue
        rows.append({
            "instance_id": r.instance_id,
            "tier": r.tier,
            "rw_skips": skips,
            "rw_delegations": delegations,
            "elapsed_ns": r.elapsed_ns,
        })

    rows.sort(key=lambda r: r["rw_skips"], reverse=True)
    return rows[:top_n]


# ---------------------------------------------------------------------------
# Per-tier aggregate table
# ---------------------------------------------------------------------------

def build_tier_summary(
    grouped: dict[str, dict[str, list[InstanceRecord]]],
) -> list[dict]:
    tiers = sorted(grouped.keys(), key=_tier_sort_key)
    rows = []
    for tier in tiers:
        backends = grouped[tier]

        tw_recs = backends.get("treewidth", [])
        br_recs = backends.get("branch:auto", [])
        nr_recs = backends.get("branch:no-rankwidth", [])
        bt_recs = backends.get("branch:from-treewidth", [])

        tw_ok = count_ok(tw_recs)
        br_ok = count_ok(br_recs)
        nr_ok = count_ok(nr_recs)

        tw_ns = sum_elapsed(tw_recs)
        br_ns = sum_elapsed(br_recs)
        nr_ns = sum_elapsed(nr_recs)

        rw_delegations = _stat_sum(br_recs, "rankwidth_delegations")
        rw_skips = _stat_sum(br_recs, "branch_rankwidth_skips")
        tw_delegations = _stat_sum(br_recs, "treewidth_delegations")
        fallthroughs = _stat_sum(br_recs, "branch_fallthroughs")

        rows.append({
            "tier": tier,
            "tw_ok": tw_ok,
            "tw_ns": tw_ns,
            "br_ok": br_ok,
            "br_ns": br_ns,
            "nr_ok": nr_ok,
            "nr_ns": nr_ns,
            "rw_delegations": rw_delegations,
            "rw_skips": rw_skips,
            "tw_delegations": tw_delegations,
            "fallthroughs": fallthroughs,
            "br_vs_tw_ratio": _ratio_str(br_ns, tw_ns),
            "br_vs_nr_ratio": _ratio_str(br_ns, nr_ns),
        })
    return rows


# ---------------------------------------------------------------------------
# Markdown rendering
# ---------------------------------------------------------------------------

def _ms(ns: int) -> str:
    if ns < 1_000_000:
        return f"{ns / 1_000:.1f} µs"
    if ns < 1_000_000_000:
        return f"{ns / 1_000_000:.2f} ms"
    return f"{ns / 1_000_000_000:.3f} s"


def render_markdown(
    tier_summary: list[dict],
    offenders_vs_tw: list[dict],
    offenders_vs_nr: list[dict],
    probe_overhead: list[dict],
) -> str:
    lines: list[str] = []
    lines.append("# Branch Regression Analysis\n")

    # Tier summary table
    lines.append("## Per-tier aggregate\n")
    lines.append("| Tier | tw solved | tw time | branch:auto time | branch:no-rw time | "
                 "br/tw ratio | br/nr ratio | rw delegations | rw skips | tw delegations |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for row in tier_summary:
        lines.append(
            f"| {row['tier']} | {row['tw_ok']} | {_ms(row['tw_ns'])} | "
            f"{_ms(row['br_ns'])} | {_ms(row['nr_ns'])} | "
            f"{row['br_vs_tw_ratio']} | {row['br_vs_nr_ratio']} | "
            f"{row['rw_delegations']} | {row['rw_skips']} | "
            f"{row['tw_delegations']} |"
        )
    lines.append("")

    # Diagnosis
    lines.append("## Diagnosis\n")
    for row in tier_summary:
        tier = row["tier"]
        delegations = row["rw_delegations"]
        skips = row["rw_skips"]
        if delegations == 0 and skips > 0:
            lines.append(
                f"- **{tier}**: branch:auto has rw_delegations=0 but rw_skips={skips}. "
                f"Overhead is rankwidth-probe cost (probe ran then skipped). "
                f"br/tw={row['br_vs_tw_ratio']}, br/nr={row['br_vs_nr_ratio']}."
            )
        elif delegations == 0 and skips == 0:
            lines.append(
                f"- **{tier}**: branch:auto has rw_delegations=0 and rw_skips=0. "
                f"Branch overhead likely from fallthrough/cache/tw-probe overhead. "
                f"br/tw={row['br_vs_tw_ratio']}."
            )
        else:
            lines.append(
                f"- **{tier}**: branch:auto has rw_delegations={delegations}. "
                f"Some overhead is from actual rankwidth solve work. "
                f"br/tw={row['br_vs_tw_ratio']}, br/nr={row['br_vs_nr_ratio']}."
            )
    lines.append("")

    # Top offenders vs treewidth
    if offenders_vs_tw:
        lines.append("## Top instances: branch:auto vs treewidth\n")
        lines.append("| Instance | Tier | branch:auto | treewidth | ratio | rw_del | rw_skips |")
        lines.append("|---|---|---:|---:|---:|---:|---:|")
        for row in offenders_vs_tw[:20]:
            lines.append(
                f"| {row['instance_id']} | {row['tier']} | {_ms(row['branch_ns'])} | "
                f"{_ms(row['treewidth_ns'])} | {row['ratio']:.3f}x | "
                f"{row['rw_delegations']} | {row['rw_skips']} |"
            )
        lines.append("")

    # Top offenders vs no-rankwidth
    if offenders_vs_nr:
        lines.append("## Top instances: branch:auto vs branch:no-rankwidth\n")
        lines.append("| Instance | Tier | branch:auto | no-rankwidth | ratio |")
        lines.append("|---|---|---:|---:|---:|")
        for row in offenders_vs_nr[:20]:
            lines.append(
                f"| {row['instance_id']} | {row['tier']} | {_ms(row['branch_ns'])} | "
                f"{_ms(row['no_rw_ns'])} | {row['ratio']:.3f}x |"
            )
        lines.append("")

    # Probe overhead
    if probe_overhead:
        lines.append("## Top instances: rankwidth probe overhead (zero delegations)\n")
        lines.append("| Instance | Tier | rw_skips | elapsed |")
        lines.append("|---|---|---:|---:|")
        for row in probe_overhead[:20]:
            lines.append(
                f"| {row['instance_id']} | {row['tier']} | "
                f"{row['rw_skips']} | {_ms(row['elapsed_ns'])} |"
            )
        lines.append("")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--artifact-dir", type=pathlib.Path,
                        default=REPO_ROOT / "artifacts" / "full",
                        help="Directory containing .jsonl artifact files")
    parser.add_argument("--local-jsonl", type=pathlib.Path, default=None,
                        help="Additional local JSONL file")
    parser.add_argument("--output", type=pathlib.Path,
                        default=REPO_ROOT / "artifacts" / "branch-regression-summary.md",
                        help="Output Markdown report path")
    parser.add_argument("--json", type=pathlib.Path, default=None,
                        help="Output JSON summary path")
    args = parser.parse_args(argv)

    print(f"Loading artifacts from {args.artifact_dir} ...", file=sys.stderr)
    records = load_jsonl_dir(args.artifact_dir)
    if args.local_jsonl and args.local_jsonl.exists():
        extra = load_jsonl_dir(args.local_jsonl.parent)
        records.extend(extra)

    if not records:
        print("warning: no records found", file=sys.stderr)
        return 1

    grouped = group_by_tier_backend(records)
    tier_summary = build_tier_summary(grouped)
    offenders_vs_tw = top_offenders_branch_vs_treewidth(records)
    offenders_vs_nr = top_offenders_branch_vs_no_rankwidth(records)
    probe_overhead = top_rw_probe_overhead(records)

    md = render_markdown(tier_summary, offenders_vs_tw, offenders_vs_nr, probe_overhead)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(md, encoding="utf-8")
    print(f"Report written to {args.output}", file=sys.stderr)

    if args.json:
        summary = {
            "tier_summary": tier_summary,
            "top_branch_vs_treewidth": offenders_vs_tw,
            "top_branch_vs_no_rankwidth": offenders_vs_nr,
            "top_probe_overhead": probe_overhead,
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
        print(f"JSON written to {args.json}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
