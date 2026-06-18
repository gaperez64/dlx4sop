#!/usr/bin/env python3
"""Scoreboard plot generator.

Reads scoreboard.json (the normalized intermediate) and emits SVG plots:

  scoreboard-assets/survival-feynmandd.svg
  scoreboard-assets/survival-mqt-bench.svg
  scoreboard-assets/survival-pyzx.svg
  scoreboard-assets/solver-time-by-tier.svg
  scoreboard-assets/solver-speedup-vs-treewidth.svg
  scoreboard-assets/branch-dispatch-by-tier.svg
  scoreboard-assets/branch-rankwidth-skip-reasons.svg
  scoreboard-assets/wmc-time-breakdown.svg

Falls back to reading raw JSONL artifacts if scoreboard.json is absent.

Usage:
    python3 tools/plot_scoreboard.py \\
        --scoreboard-json scoreboard.json \\
        --artifact-dir artifacts/full \\
        --output-dir scoreboard-assets
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

# Solver colors
SOLVER_COLORS: dict[str, str] = {
    "treewidth":         "#1f77b4",
    "branch:auto":       "#ff7f0e",
    "branch:no-rankwidth": "#2ca02c",
    "branch:from-treewidth": "#d62728",
    "rankwidth:from-treewidth": "#9467bd",
    "rankwidth:best":    "#8c564b",
    "best Ganak":        "#e377c2",
    "best native":       "#7f7f7f",
    "oracle best local": "#bcbd22",
}

# Source display names → slug
SOURCE_SLUGS: dict[str, str] = {
    "FeynmanDD": "feynmandd",
    "MQT Bench": "mqt-bench",
    "PyZX": "pyzx",
}

NS_BUDGETS = [
    1_000_000,        # 1 ms
    3_000_000,        # 3 ms
    10_000_000,       # 10 ms
    30_000_000,       # 30 ms
    100_000_000,      # 100 ms
    300_000_000,      # 300 ms
    1_000_000_000,    # 1 s
    3_000_000_000,    # 3 s
    10_000_000_000,   # 10 s
    30_000_000_000,   # 30 s
]

NS_LABELS = [
    "1ms", "3ms", "10ms", "30ms", "100ms", "300ms",
    "1s", "3s", "10s", "30s",
]


def _try_import_matplotlib() -> bool:
    try:
        import matplotlib  # noqa: F401
        return True
    except ImportError:
        return False


def _svg_text_escape(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ---------------------------------------------------------------------------
# Simple SVG writer (no matplotlib dependency)
# ---------------------------------------------------------------------------

class SVGDoc:
    def __init__(self, width: int = 800, height: int = 500) -> None:
        self.width = width
        self.height = height
        self._lines: list[str] = []
        self._lines.append(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}">'
        )
        self._lines.append('<style>text{font-family:sans-serif;font-size:11px;}</style>')

    def rect(self, x: float, y: float, w: float, h: float, fill: str = "#888",
             opacity: float = 1.0) -> None:
        self._lines.append(
            f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
            f'fill="{fill}" opacity="{opacity:.2f}"/>'
        )

    def line(self, x1: float, y1: float, x2: float, y2: float, stroke: str = "#333",
             width: float = 1.0, dash: str = "") -> None:
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        self._lines.append(
            f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
            f'stroke="{stroke}" stroke-width="{width:.1f}"{dash_attr}/>'
        )

    def polyline(self, points: list[tuple[float, float]], stroke: str = "#333",
                 width: float = 1.5, fill: str = "none", dash: str = "") -> None:
        pts = " ".join(f"{x:.1f},{y:.1f}" for x, y in points)
        dash_attr = f' stroke-dasharray="{dash}"' if dash else ""
        self._lines.append(
            f'<polyline points="{pts}" stroke="{stroke}" stroke-width="{width:.1f}" '
            f'fill="{fill}"{dash_attr}/>'
        )

    def text(self, x: float, y: float, content: str, anchor: str = "start",
             fill: str = "#333", size: int = 11) -> None:
        self._lines.append(
            f'<text x="{x:.1f}" y="{y:.1f}" text-anchor="{anchor}" '
            f'fill="{fill}" font-size="{size}">{_svg_text_escape(content)}</text>'
        )

    def close(self) -> str:
        self._lines.append("</svg>")
        return "\n".join(self._lines)


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_records_from_artifacts(artifact_dir: pathlib.Path) -> list[dict]:
    records = []
    for jf in sorted(artifact_dir.glob("**/*.jsonl")):
        try:
            with open(jf, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        records.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
        except OSError:
            pass
    return records


def _canonical_backend(rec: dict) -> str:
    backend = rec.get("backend", "")
    cfg = rec.get("backend_config", {})
    rw_source = cfg.get("branch_rw_source", "")
    if backend == "branch":
        mapping = {
            "auto": "branch:auto",
            "from-treewidth": "branch:from-treewidth",
            "native": "branch:native",
            "none": "branch:no-rankwidth",
            "both": "branch:auto",
        }
        return mapping.get(rw_source, "branch:auto")
    engine = rec.get("engine", "")
    if engine and not backend:
        return f"native:{engine}"
    return backend


def _best_native_curves(
    records: list[dict],
    source: str,
) -> dict[str, tuple[list[int], list[float]]]:
    """Build a single 'best native' survival curve taking the fastest native engine per instance."""
    # key: (case, boundary) → minimum elapsed_ns across all native engines
    INF = 10**18
    best_ns: dict[tuple, int] = {}
    found_any = False
    for r in records:
        rec_source = r.get("provenance", {}).get("source", r.get("source", ""))
        if rec_source != source:
            continue
        engine = r.get("engine", "")
        if not engine or r.get("backend"):
            continue
        found_any = True
        status = r.get("status", "")
        ns = int(r.get("elapsed_ns") or INF) if status == "ok" else INF
        key = (r.get("case", ""), r.get("boundary", ""))
        if key not in best_ns or ns < best_ns[key]:
            best_ns[key] = ns

    if not found_any or not best_ns:
        return {}

    sorted_ns = sorted(best_ns.values())
    total = len(sorted_ns)
    fracs = [sum(1 for ns in sorted_ns if ns <= b) / total for b in NS_BUDGETS]
    return {"best native": (list(NS_BUDGETS), fracs)}


# ---------------------------------------------------------------------------
# Survival / solved-by-budget plot
# ---------------------------------------------------------------------------

def _survival_curves(
    records: list[dict],
    source: str,
    backends: list[str],
) -> dict[str, tuple[list[int], list[float]]]:
    """Return {backend: (budgets_ns, fraction_solved)} for a given source."""
    by_backend: dict[str, list[dict]] = collections.defaultdict(list)
    for r in records:
        prov = r.get("provenance", {})
        rec_source = prov.get("source", r.get("source", ""))
        if rec_source != source:
            continue
        b = _canonical_backend(r)
        if b in backends:
            by_backend[b].append(r)

    curves: dict[str, tuple[list[int], list[float]]] = {}
    if "best native" in backends:
        curves.update(_best_native_curves(records, source))

    for backend, recs in by_backend.items():
        total = len(recs)
        if total == 0:
            continue
        # Sort by elapsed_ns; treat timeouts as infinity
        def _ns(r: dict) -> int:
            if r.get("status") != "ok":
                return 10**18
            return int(r.get("elapsed_ns", r.get("solve_elapsed_ns", 0)))
        sorted_ns = sorted(_ns(r) for r in recs)

        fracs = []
        for budget in NS_BUDGETS:
            solved = sum(1 for ns in sorted_ns if ns <= budget)
            fracs.append(solved / total)

        curves[backend] = (list(NS_BUDGETS), fracs)

    return curves


def plot_survival_svg(
    records: list[dict],
    source: str,
    output_path: pathlib.Path,
    backends: list[str] | None = None,
) -> None:
    if backends is None:
        backends = ["treewidth", "branch:auto", "branch:no-rankwidth",
                    "rankwidth:best", "rankwidth:from-treewidth", "best native"]

    curves = _survival_curves(records, source, backends)
    if not curves:
        return

    W, H = 720, 420
    PAD_L, PAD_R, PAD_T, PAD_B = 60, 160, 40, 50
    plot_w = W - PAD_L - PAD_R
    plot_h = H - PAD_T - PAD_B

    n_budgets = len(NS_BUDGETS)
    svg = SVGDoc(W, H)

    # Background
    svg.rect(PAD_L, PAD_T, plot_w, plot_h, fill="#f8f8f8")

    # Grid lines (y-axis)
    for frac in [0.0, 0.25, 0.5, 0.75, 1.0]:
        y = PAD_T + plot_h * (1.0 - frac)
        svg.line(PAD_L, y, PAD_L + plot_w, y, stroke="#ddd")
        svg.text(PAD_L - 5, y + 4, f"{frac:.0%}", anchor="end", fill="#666", size=10)

    # Grid lines (x-axis — log scale)
    for i, (ns, label) in enumerate(zip(NS_BUDGETS, NS_LABELS)):
        x = PAD_L + i * plot_w / (n_budgets - 1)
        svg.line(x, PAD_T, x, PAD_T + plot_h, stroke="#ddd")
        svg.text(x, PAD_T + plot_h + 15, label, anchor="middle", fill="#666", size=10)

    # Axes
    svg.line(PAD_L, PAD_T, PAD_L, PAD_T + plot_h, stroke="#333")
    svg.line(PAD_L, PAD_T + plot_h, PAD_L + plot_w, PAD_T + plot_h, stroke="#333")

    # Curves
    legend_y = PAD_T + 10
    for backend, (budgets, fracs) in curves.items():
        color = SOLVER_COLORS.get(backend, "#888")
        dash = "6,3" if "oracle" in backend else ""
        pts = [
            (PAD_L + i * plot_w / (n_budgets - 1), PAD_T + plot_h * (1.0 - f))
            for i, f in enumerate(fracs)
        ]
        svg.polyline(pts, stroke=color, width=2.0, dash=dash)

        final_frac = fracs[-1] if fracs else 0.0
        label = f"{backend} — {final_frac:.1%}"
        svg.rect(PAD_L + plot_w + 10, legend_y - 9, 12, 9, fill=color)
        svg.text(PAD_L + plot_w + 25, legend_y, label, fill="#333", size=10)
        legend_y += 18

    # Title
    svg.text(W // 2, 20, f"Solved fraction by time budget — {source}",
             anchor="middle", fill="#222", size=13)
    svg.text(W // 2, H - 8, "Time budget (log scale)", anchor="middle", fill="#555", size=10)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.close(), encoding="utf-8")
    print(f"  Wrote {output_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Solver time by tier (horizontal bars)
# ---------------------------------------------------------------------------

def plot_solver_time_by_tier_svg(
    records: list[dict],
    output_path: pathlib.Path,
    backends: list[str] | None = None,
) -> None:
    if backends is None:
        backends = ["treewidth", "branch:auto", "branch:no-rankwidth",
                    "rankwidth:from-treewidth", "rankwidth:best"]

    # Aggregate: {tier: {backend: total_ns}}
    import re
    tier_backend_ns: dict[str, dict[str, int]] = collections.defaultdict(
        lambda: collections.defaultdict(int)
    )
    tier_backend_n: dict[str, dict[str, int]] = collections.defaultdict(
        lambda: collections.defaultdict(int)
    )
    for r in records:
        if r.get("status") != "ok":
            continue
        b = _canonical_backend(r)
        if b not in backends:
            continue
        tier = r.get("tier", "")
        ns = int(r.get("elapsed_ns", r.get("solve_elapsed_ns", 0)))
        tier_backend_ns[tier][b] += ns
        tier_backend_n[tier][b] += 1

    def _tier_key(t: str) -> int:
        m = re.match(r"(\d+)", t)
        return int(m.group(1)) if m else 9999

    tiers = sorted(tier_backend_ns.keys(), key=_tier_key)
    if not tiers:
        return

    W, H = 720, max(300, 80 * len(tiers) + 60)
    PAD_L, PAD_R, PAD_T, PAD_B = 100, 160, 40, 40
    plot_w = W - PAD_L - PAD_R
    plot_h = H - PAD_T - PAD_B

    # Find max ns for log scale
    max_ns = max(
        (ns for tb in tier_backend_ns.values() for ns in tb.values()),
        default=1,
    )

    import math
    def _log_x(ns: int) -> float:
        if ns <= 0:
            return 0.0
        return math.log10(ns) / math.log10(max_ns) * plot_w

    row_h = plot_h / len(tiers)
    bar_h = row_h / (len(backends) + 1)

    svg = SVGDoc(W, H)
    svg.rect(PAD_L, PAD_T, plot_w, plot_h, fill="#f8f8f8")

    svg.text(W // 2, 22, "Solver time by tier (total, log scale)",
             anchor="middle", fill="#222", size=13)

    for ti, tier in enumerate(tiers):
        y_base = PAD_T + ti * row_h
        svg.text(PAD_L - 5, y_base + row_h / 2, tier, anchor="end", fill="#333", size=10)
        svg.line(PAD_L, y_base, PAD_L + plot_w, y_base, stroke="#ddd")

        for bi, backend in enumerate(backends):
            ns = tier_backend_ns[tier].get(backend, 0)
            if ns == 0:
                continue
            color = SOLVER_COLORS.get(backend, "#888")
            bar_x = PAD_L
            bar_y = y_base + bi * bar_h + 2
            bar_w = _log_x(ns)
            svg.rect(bar_x, bar_y, bar_w, bar_h - 2, fill=color, opacity=0.8)
            if bar_w > 30:
                ms = ns / 1_000_000
                label = f"{ms:.0f}ms" if ms >= 1 else f"{ns/1000:.0f}µs"
                svg.text(bar_x + bar_w + 3, bar_y + bar_h - 3, label, fill="#333", size=9)

    # Legend
    legend_y = PAD_T + 10
    for backend in backends:
        color = SOLVER_COLORS.get(backend, "#888")
        svg.rect(PAD_L + plot_w + 10, legend_y - 9, 12, 9, fill=color)
        svg.text(PAD_L + plot_w + 25, legend_y, backend, fill="#333", size=10)
        legend_y += 16

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.close(), encoding="utf-8")
    print(f"  Wrote {output_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Speedup vs treewidth
# ---------------------------------------------------------------------------

def plot_speedup_vs_treewidth_svg(
    records: list[dict],
    output_path: pathlib.Path,
    backends: list[str] | None = None,
) -> None:
    if backends is None:
        backends = ["branch:auto", "branch:no-rankwidth", "rankwidth:best"]

    import re
    # Aggregate per tier
    tier_ns: dict[str, dict[str, int]] = collections.defaultdict(lambda: collections.defaultdict(int))
    for r in records:
        if r.get("status") != "ok":
            continue
        b = _canonical_backend(r)
        tier = r.get("tier", "")
        ns = int(r.get("elapsed_ns", r.get("solve_elapsed_ns", 0)))
        tier_ns[tier][b] += ns

    def _tier_key(t: str) -> int:
        m = re.match(r"(\d+)", t)
        return int(m.group(1)) if m else 9999

    tiers = sorted(tier_ns.keys(), key=_tier_key)
    if not tiers:
        return

    W, H = 640, 380
    PAD_L, PAD_R, PAD_T, PAD_B = 60, 160, 40, 50
    plot_w = W - PAD_L - PAD_R
    plot_h = H - PAD_T - PAD_B
    n_tiers = len(tiers)

    svg = SVGDoc(W, H)
    svg.rect(PAD_L, PAD_T, plot_w, plot_h, fill="#f8f8f8")
    svg.text(W // 2, 22, "Speedup relative to treewidth (ratio > 1 = faster than treewidth)",
             anchor="middle", fill="#222", size=12)

    # Reference line at 1.0
    y_ref = PAD_T + plot_h / 2  # 1.0 in a 0.5–2.0 range
    svg.line(PAD_L, y_ref, PAD_L + plot_w, y_ref, stroke="#999", dash="4,2")
    svg.text(PAD_L - 5, y_ref + 4, "1.0×", anchor="end", fill="#666", size=10)

    ratio_min, ratio_max = 0.5, 2.0

    def _y(ratio: float) -> float:
        clamped = max(ratio_min, min(ratio_max, ratio))
        frac = (clamped - ratio_min) / (ratio_max - ratio_min)
        return PAD_T + plot_h * (1.0 - frac)

    # Y-axis labels
    for r_val in [0.5, 0.75, 1.0, 1.25, 1.5, 2.0]:
        y = _y(r_val)
        svg.text(PAD_L - 5, y + 4, f"{r_val:.2f}×", anchor="end", fill="#666", size=9)
        svg.line(PAD_L, y, PAD_L + plot_w, y, stroke="#eee")

    bar_group_w = plot_w / n_tiers
    bar_w = bar_group_w / (len(backends) + 1)

    for ti, tier in enumerate(tiers):
        tw_ns = tier_ns[tier].get("treewidth", 0)
        x_base = PAD_L + ti * bar_group_w

        svg.text(x_base + bar_group_w / 2, PAD_T + plot_h + 15, tier,
                 anchor="middle", fill="#444", size=10)

        if tw_ns == 0:
            continue
        for bi, backend in enumerate(backends):
            ns = tier_ns[tier].get(backend, 0)
            if ns == 0:
                continue
            ratio = tw_ns / ns  # > 1 means backend is faster
            color = SOLVER_COLORS.get(backend, "#888")
            x = x_base + bi * bar_w + 4
            y_top = _y(ratio)
            y_base_ref = _y(1.0)
            if ratio >= 1.0:
                svg.rect(x, y_top, bar_w - 2, y_base_ref - y_top, fill=color, opacity=0.8)
            else:
                svg.rect(x, y_base_ref, bar_w - 2, y_top - y_base_ref, fill=color, opacity=0.4)

    # Legend
    legend_y = PAD_T + 10
    for backend in backends:
        color = SOLVER_COLORS.get(backend, "#888")
        svg.rect(PAD_L + plot_w + 10, legend_y - 9, 12, 9, fill=color)
        svg.text(PAD_L + plot_w + 25, legend_y, backend, fill="#333", size=10)
        legend_y += 16

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.close(), encoding="utf-8")
    print(f"  Wrote {output_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Branch dispatch stacked bars
# ---------------------------------------------------------------------------

def plot_branch_dispatch_svg(
    records: list[dict],
    output_path: pathlib.Path,
) -> None:
    import re

    DISPATCH_FIELDS = [
        ("treewidth_delegations", "tw delegates", "#1f77b4"),
        ("rankwidth_delegations", "rw delegates", "#ff7f0e"),
        ("branch_fallthroughs", "fallthroughs", "#2ca02c"),
    ]

    tier_dispatch: dict[str, dict[str, int]] = collections.defaultdict(lambda: collections.defaultdict(int))
    for r in records:
        if _canonical_backend(r) != "branch:auto":
            continue
        if r.get("status") != "ok":
            continue
        tier = r.get("tier", "")
        stats = r.get("stats", {})
        for field, _, _ in DISPATCH_FIELDS:
            v = stats.get(field, 0)
            if isinstance(v, (int, float)):
                tier_dispatch[tier][field] += int(v)

    def _tier_key(t: str) -> int:
        m = re.match(r"(\d+)", t)
        return int(m.group(1)) if m else 9999

    tiers = sorted(tier_dispatch.keys(), key=_tier_key)
    if not tiers:
        return

    W, H = 560, 380
    PAD_L, PAD_R, PAD_T, PAD_B = 60, 160, 40, 50
    plot_w = W - PAD_L - PAD_R
    plot_h = H - PAD_T - PAD_B

    max_total = max(
        (sum(d.get(f, 0) for f, _, _ in DISPATCH_FIELDS) for d in tier_dispatch.values()),
        default=1,
    )

    bar_w = plot_w / len(tiers) - 8

    svg = SVGDoc(W, H)
    svg.rect(PAD_L, PAD_T, plot_w, plot_h, fill="#f8f8f8")
    svg.text(W // 2, 22, "Branch dispatch by tier (branch:auto)",
             anchor="middle", fill="#222", size=13)

    for ti, tier in enumerate(tiers):
        d = tier_dispatch[tier]
        x = PAD_L + ti * (plot_w / len(tiers)) + 4

        svg.text(x + bar_w / 2, PAD_T + plot_h + 15, tier,
                 anchor="middle", fill="#444", size=10)

        y_cur = PAD_T + plot_h
        for field, label, color in DISPATCH_FIELDS:
            count = d.get(field, 0)
            if count == 0 or max_total == 0:
                continue
            h = count / max_total * plot_h
            svg.rect(x, y_cur - h, bar_w, h, fill=color, opacity=0.85)
            y_cur -= h

    # Legend
    legend_y = PAD_T + 10
    for field, label, color in DISPATCH_FIELDS:
        svg.rect(PAD_L + plot_w + 10, legend_y - 9, 12, 9, fill=color)
        svg.text(PAD_L + plot_w + 25, legend_y, label, fill="#333", size=10)
        legend_y += 16

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.close(), encoding="utf-8")
    print(f"  Wrote {output_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# WMC time breakdown
# ---------------------------------------------------------------------------

def plot_wmc_time_svg(
    records: list[dict],
    output_path: pathlib.Path,
) -> None:
    import re

    wmc_records = [r for r in records if "ganak" in r.get("backend", "").lower()
                   or "wmc" in r.get("backend", "").lower()
                   or "sop2wmc" in str(r.get("backend_config", "")).lower()]

    if not wmc_records:
        return

    tier_data: dict[str, dict[str, int]] = collections.defaultdict(lambda: {"ganak_ns": 0, "export_ns": 0})
    for r in wmc_records:
        if r.get("status") not in ("ok", "timeout"):
            continue
        tier = r.get("tier", "")
        stats = r.get("stats", {})
        tier_data[tier]["ganak_ns"] += int(stats.get("ganak_elapsed_ns", 0))
        tier_data[tier]["export_ns"] += int(stats.get("export_elapsed_ns", 0))

    def _tier_key(t: str) -> int:
        m = re.match(r"(\d+)", t)
        return int(m.group(1)) if m else 9999

    tiers = sorted(tier_data.keys(), key=_tier_key)
    if not tiers:
        return

    W, H = 520, 360
    PAD_L, PAD_R, PAD_T, PAD_B = 60, 160, 40, 50
    plot_w = W - PAD_L - PAD_R
    plot_h = H - PAD_T - PAD_B

    max_ns = max(
        (d["ganak_ns"] + d["export_ns"] for d in tier_data.values()),
        default=1,
    )
    bar_w = plot_w / len(tiers) - 8

    svg = SVGDoc(W, H)
    svg.rect(PAD_L, PAD_T, plot_w, plot_h, fill="#f8f8f8")
    svg.text(W // 2, 22, "WMC time breakdown by tier", anchor="middle", fill="#222", size=13)

    for ti, tier in enumerate(tiers):
        d = tier_data[tier]
        x = PAD_L + ti * (plot_w / len(tiers)) + 4
        svg.text(x + bar_w / 2, PAD_T + plot_h + 15, tier,
                 anchor="middle", fill="#444", size=10)

        total = d["ganak_ns"] + d["export_ns"]
        if total == 0 or max_ns == 0:
            continue

        ganak_h = d["ganak_ns"] / max_ns * plot_h
        export_h = d["export_ns"] / max_ns * plot_h
        y_top = PAD_T + plot_h - ganak_h - export_h
        svg.rect(x, y_top, bar_w, export_h, fill="#17becf", opacity=0.85)
        svg.rect(x, y_top + export_h, bar_w, ganak_h, fill="#e377c2", opacity=0.85)

    # Legend
    legend_y = PAD_T + 10
    for label, color in [("Ganak", "#e377c2"), ("Export", "#17becf")]:
        svg.rect(PAD_L + plot_w + 10, legend_y - 9, 12, 9, fill=color)
        svg.text(PAD_L + plot_w + 25, legend_y, label, fill="#333", size=10)
        legend_y += 16

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(svg.close(), encoding="utf-8")
    print(f"  Wrote {output_path}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scoreboard-json", type=pathlib.Path,
                        default=REPO_ROOT / "scoreboard.json",
                        help="Normalized scoreboard JSON intermediate")
    parser.add_argument("--artifact-dir", type=pathlib.Path,
                        default=REPO_ROOT / "artifacts" / "full",
                        help="Fallback: raw JSONL artifact directory")
    parser.add_argument("--output-dir", type=pathlib.Path,
                        default=REPO_ROOT / "scoreboard-assets")
    parser.add_argument("--sources", action="append", dest="sources",
                        default=None, metavar="SOURCE",
                        help="Sources to plot survival curves for (default: all known)")
    args = parser.parse_args(argv)

    # Load records
    records: list[dict] = []
    if args.scoreboard_json.exists():
        data = json.loads(args.scoreboard_json.read_text(encoding="utf-8"))
        records = data.get("records", [])

    if not records and args.artifact_dir.exists():
        records = load_records_from_artifacts(args.artifact_dir)

    if not records:
        print("warning: no records found; plots will be empty", file=sys.stderr)

    sources = args.sources or list(SOURCE_SLUGS.keys())
    out = args.output_dir
    out.mkdir(parents=True, exist_ok=True)

    print("Generating scoreboard plots ...", file=sys.stderr)

    # Survival plots per source
    for source in sources:
        slug = SOURCE_SLUGS.get(source, source.lower().replace(" ", "-"))
        plot_survival_svg(records, source, out / f"survival-{slug}.svg")

    # Solver time by tier
    plot_solver_time_by_tier_svg(records, out / "solver-time-by-tier.svg")

    # Speedup vs treewidth
    plot_speedup_vs_treewidth_svg(records, out / "solver-speedup-vs-treewidth.svg")

    # Branch dispatch
    plot_branch_dispatch_svg(records, out / "branch-dispatch-by-tier.svg")

    # WMC time breakdown
    plot_wmc_time_svg(records, out / "wmc-time-breakdown.svg")

    print(f"Plots written to {out}/", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
