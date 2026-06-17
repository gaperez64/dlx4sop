#!/usr/bin/env python3
"""Render compact scoreboard markdown from local and external bench JSONL.

Reads JSONL records produced by bench_sop.py (--local) and optionally
refresh_scoreboard.py (--external), and writes a scoreboard.md with:
  - compact bar/bucket local backend summary per tier
  - external source/tier comparison section (if --external provided)

Usage:
    python3 scripts/render_scoreboard.py \\
        --local results/local_backends.jsonl \\
        [--external results/scoreboard_refresh.jsonl] \\
        [--out scoreboard.md]
"""

import argparse
import datetime
import math
import pathlib
import sys
from typing import TextIO

# Add scripts/ to path so benchlib is importable.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from benchlib import (
    TIER_ORDER,
    bar_length,
    geomean,
    group_by,
    read_jsonl,
    render_bar,
    tier_label,
)

BACKEND_ORDER = [
    "components",
    "brute-force",
    "branch",
    "branch:from-treewidth",
    "treewidth",
    "rankwidth",
]

BACKEND_LABEL = {
    "components":           "components",
    "brute-force":          "brute-force",
    "branch":               "branch (native rw)",
    "branch:from-treewidth": "branch (from-tw)",
    "treewidth":            "treewidth",
    "rankwidth":            "rankwidth",
}


# ---------------------------------------------------------------------------
# Local backend section
# ---------------------------------------------------------------------------

def render_local_section(records: list[dict], out: TextIO) -> None:
    out.write("## Local sop-solve backends — geomean wall time by tier\n\n")

    by_tier = group_by(records, "tier")
    tiers = [t for t in TIER_ORDER if t in by_tier] + sorted(
        t for t in by_tier if t not in TIER_ORDER
    )

    if not tiers:
        out.write("_No local benchmark data._\n\n")
        return

    # Collect which backends appear across all tiers.
    all_backends: list[str] = []
    seen: set[str] = set()
    for bk in BACKEND_ORDER:
        for r in records:
            if r.get("backend") == bk and bk not in seen:
                all_backends.append(bk)
                seen.add(bk)
                break
    for r in records:
        bk = r.get("backend", "")
        if bk and bk not in seen:
            all_backends.append(bk)
            seen.add(bk)

    summary_rows: list[str] = []

    for tier in tiers:
        tier_recs = by_tier[tier]
        by_backend: dict[str, list[dict]] = {}
        for r in tier_recs:
            bk = r.get("backend", "unknown")
            by_backend.setdefault(bk, []).append(r)

        # Compute geomean elapsed for each backend (ok only).
        backend_gm: dict[str, float | None] = {}
        backend_counts: dict[str, dict[str, int]] = {}
        for bk, recs in by_backend.items():
            ok_ns = [r["solve_elapsed_ns"] for r in recs if r.get("status") == "ok"]
            n_timeout = sum(1 for r in recs if r.get("status") == "timeout")
            n_error = sum(1 for r in recs if r.get("status") == "error")
            backend_gm[bk] = geomean([v / 1e6 for v in ok_ns])  # ms
            backend_counts[bk] = {"ok": len(ok_ns), "timeout": n_timeout, "error": n_error}

        # Best geomean (lowest ms) to anchor bars.
        valid_gms = {bk: gm for bk, gm in backend_gm.items() if gm is not None}
        if not valid_gms:
            continue
        best_bk = min(valid_gms, key=lambda b: valid_gms[b])
        best_gm = valid_gms[best_bk]
        n_instances = len({r.get("name", "") for r in tier_recs})

        out.write(f"### Tier {tier_label(tier)}   (n={n_instances} instances, "
                  f"best={BACKEND_LABEL.get(best_bk, best_bk)} {best_gm:.3g} ms)\n\n")
        out.write("```\n")

        for bk in all_backends:
            if bk not in backend_gm:
                continue
            gm = backend_gm[bk]
            cnts = backend_counts.get(bk, {})
            label = BACKEND_LABEL.get(bk, bk)
            n_ok = cnts.get("ok", 0)
            n_to = cnts.get("timeout", 0)
            n_err = cnts.get("error", 0)
            if gm is not None:
                ratio = gm / best_gm
                bar = render_bar(ratio - 1)
                suffix = ""
                if n_to:
                    suffix += f"  (timeout {n_to})"
                if n_err:
                    suffix += f"  (error {n_err})"
                out.write(f"{label:<30s} {bar:<{40}s} {gm:8.3g} ms  {ratio:5.2f}x{suffix}\n")
            else:
                out.write(f"{label:<30s} {'—':<40s} n/a{' '*(10)}")
                if n_to:
                    out.write(f"  (timeout {n_to})")
                if n_err:
                    out.write(f"  (error {n_err})")
                out.write("\n")

        out.write("```\n\n")
        summary_rows.append(f"{tier_label(tier):12s} {BACKEND_LABEL.get(best_bk, best_bk)}")

    out.write("**Local backend winner by tier:**\n```\n")
    for row in summary_rows:
        out.write(row + "\n")
    out.write("```\n\n")


# ---------------------------------------------------------------------------
# External section
# ---------------------------------------------------------------------------

def render_external_section(records: list[dict], out: TextIO) -> None:
    out.write("## External comparisons — geomean wall time by source/tier\n\n")
    if not records:
        out.write("_No external benchmark data._\n\n")
        return

    by_source = group_by(records, "external_source")
    for source, src_recs in sorted(by_source.items()):
        out.write(f"### Source: {source}\n\n")
        by_tier = group_by(src_recs, "tier")
        tiers = [t for t in TIER_ORDER if t in by_tier] + sorted(
            t for t in by_tier if t not in TIER_ORDER
        )
        for tier in tiers:
            tier_recs = by_tier[tier]
            by_runner = group_by(tier_recs, "runner_kind")
            gms: dict[str, float | None] = {}
            for kind, recs in by_runner.items():
                ok_ms = [r.get("wall_ms", 0) for r in recs
                         if r.get("status") == "ok" and r.get("wall_ms", 0) > 0]
                gms[kind] = geomean(ok_ms)

            valid = {k: v for k, v in gms.items() if v is not None}
            if not valid:
                continue
            best_gm = min(valid.values())
            n_inst = len({r.get("instance_id", "") for r in tier_recs})
            out.write(f"Tier {tier_label(tier)}, n={n_inst}\n\n```\n")
            for kind in ("best-local", "native", "ganak"):
                if kind not in gms:
                    continue
                gm = gms[kind]
                runner_names = {r.get("runner_name", "") for r in by_runner.get(kind, [])}
                runner_str = ", ".join(sorted(runner_names))
                if gm is not None:
                    ratio = gm / best_gm
                    bar = render_bar(ratio - 1)
                    out.write(f"{kind+' ('+runner_str+')':<38s} {bar:<40s} {gm:8.3g} ms  {ratio:5.2f}x\n")
                else:
                    out.write(f"{kind+' ('+runner_str+')':<38s} {'—':<40s} n/a\n")
            out.write("```\n\n")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local", type=pathlib.Path,
                        help="JSONL from bench_sop.py")
    parser.add_argument("--external", type=pathlib.Path,
                        help="JSONL from refresh_scoreboard.py (optional)")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("scoreboard.md"),
                        help="output markdown file (default: scoreboard.md)")
    args = parser.parse_args()

    if not args.local and not args.external:
        print("error: at least one of --local or --external is required", file=sys.stderr)
        return 2

    local_records = read_jsonl(args.local) if args.local else []
    external_records = read_jsonl(args.external) if args.external else []

    now = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"# SOP Solver Scoreboard\n\n_Generated {now}_\n\n")
        if local_records:
            render_local_section(local_records, f)
        if external_records:
            render_external_section(external_records, f)

    print(f"wrote {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
