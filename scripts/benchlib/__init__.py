"""Shared utilities for local SOP corpus benchmarking scripts."""

from __future__ import annotations

import json
import math
import pathlib
from typing import Iterator

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop"

TIER_ORDER = ["tier-1-8", "tier-9-16", "tier-17-32", "tier-33-64"]


# ---------------------------------------------------------------------------
# Corpus loading
# ---------------------------------------------------------------------------

def iter_corpus(corpus_dir: pathlib.Path = DEFAULT_CORPUS,
                tiers: list[str] | None = None) -> Iterator[dict]:
    """Yield one metadata dict per QSOP instance in the corpus."""
    for tier_dir in sorted(corpus_dir.iterdir()):
        if not tier_dir.is_dir():
            continue
        if tiers and tier_dir.name not in tiers:
            continue
        for meta_path in sorted(tier_dir.glob("*.meta.json")):
            with open(meta_path, encoding="utf-8") as f:
                meta = json.load(f)
            meta.setdefault("tier", tier_dir.name)
            meta.setdefault("qsop_path", str(meta_path.with_suffix(".qsop")))
            yield meta


def load_manifest(corpus_dir: pathlib.Path = DEFAULT_CORPUS) -> list[dict]:
    """Load manifest.jsonl from the corpus directory."""
    manifest_path = corpus_dir / "manifest.jsonl"
    if not manifest_path.exists():
        return list(iter_corpus(corpus_dir))
    records = []
    with open(manifest_path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


# ---------------------------------------------------------------------------
# JSONL result loading
# ---------------------------------------------------------------------------

def read_jsonl(path: pathlib.Path) -> list[dict]:
    records = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                records.append(json.loads(line))
    return records


def iter_jsonl(path: pathlib.Path) -> Iterator[dict]:
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                yield json.loads(line)


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def geomean(values: list[float]) -> float | None:
    """Geometric mean of positive values; returns None for empty/all-zero."""
    pos = [v for v in values if v > 0]
    if not pos:
        return None
    return math.exp(sum(math.log(v) for v in pos) / len(pos))


def group_by(records: list[dict], key: str) -> dict[str, list[dict]]:
    result: dict[str, list[dict]] = {}
    for r in records:
        k = r.get(key, "")
        result.setdefault(k, []).append(r)
    return result


# ---------------------------------------------------------------------------
# Bar rendering
# ---------------------------------------------------------------------------

MAX_BAR = 40

def bar_length(ratio: float) -> int:
    """Log-bucketed bar length: clamp(1, MAX_BAR, ceil(4*log2(ratio+1)))."""
    if ratio <= 0:
        return 1
    return max(1, min(MAX_BAR, math.ceil(4 * math.log2(ratio + 1))))


def render_bar(ratio: float, char: str = "█") -> str:
    return char * bar_length(ratio)


# ---------------------------------------------------------------------------
# Tier helpers
# ---------------------------------------------------------------------------

def tier_label(tier_name: str) -> str:
    """Convert tier directory name to human-readable label."""
    return tier_name.replace("tier-", "").replace("-", "–")
