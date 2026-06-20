#!/usr/bin/env python3
"""Shared utilities for dlx4sop benchmark runners.

All benchmark runners should emit sop_bench_result_v2 records and use the
helpers here so that tools/render_scoreboard.py can consume any artifact.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import math
import pathlib
import subprocess
import time
from typing import IO, Iterable, Iterator

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_CORPUS = REPO_ROOT / "benchmarks" / "corpus" / "sop"

TIER_ORDER = ["tier-1-8", "tier-9-16", "tier-17-32", "tier-33-64"]


# ---------------------------------------------------------------------------
# Dataclasses
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class CommandResult:
    stdout: str
    stderr: str
    returncode: int
    elapsed_ns: int


@dataclasses.dataclass
class CorpusCase:
    instance_id: str
    tier: str
    qsop_path: pathlib.Path
    meta: dict


# ---------------------------------------------------------------------------
# JSONL I/O
# ---------------------------------------------------------------------------

def read_jsonl(path: pathlib.Path, strict: bool = False) -> list[dict]:
    """Read JSONL records. With strict=True, wrap a bad row in RuntimeError with the file:line."""
    records: list[dict] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError as exc:
            if strict:
                raise RuntimeError(f"{path}:{line_number}: invalid JSONL row") from exc
            raise
    return records


def write_jsonl_record(stream: IO[str], record: dict) -> None:
    stream.write(json.dumps(record) + "\n")
    stream.flush()


def case_qasm(case: dict) -> str:
    """Join a manifest case's qasm_lines into a QASM source string."""
    return "\n".join(case["qasm_lines"]) + "\n"


# ---------------------------------------------------------------------------
# Statistics
# ---------------------------------------------------------------------------

def geomean(values: Iterable[float]) -> float | None:
    pos = [v for v in values if v > 0]
    if not pos:
        return None
    return math.exp(sum(math.log(v) for v in pos) / len(pos))


# ---------------------------------------------------------------------------
# Bar rendering
# ---------------------------------------------------------------------------

def render_bar(ratio: float, width: int = 40, char: str = "█") -> str:
    if ratio <= 0:
        length = 1
    else:
        length = max(1, min(width, math.ceil(4 * math.log2(ratio + 1))))
    return char * length


# ---------------------------------------------------------------------------
# Tier helpers
# ---------------------------------------------------------------------------

def tier_sort_key(tier: str) -> tuple[int, str]:
    try:
        idx = TIER_ORDER.index(tier)
        return (idx, tier)
    except ValueError:
        return (len(TIER_ORDER), tier)


def tier_label(tier: str) -> str:
    return tier.replace("tier-", "").replace("-", "–")


# ---------------------------------------------------------------------------
# Command execution
# ---------------------------------------------------------------------------

def run_command(
    cmd: list[str],
    *,
    input_text: str | None = None,
    timeout_seconds: float | None = None,
) -> CommandResult:
    t0 = time.monotonic_ns()
    try:
        result = subprocess.run(
            cmd,
            input=input_text,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
        elapsed_ns = time.monotonic_ns() - t0
        return CommandResult(
            stdout=result.stdout,
            stderr=result.stderr,
            returncode=result.returncode,
            elapsed_ns=elapsed_ns,
        )
    except subprocess.TimeoutExpired:
        elapsed_ns = time.monotonic_ns() - t0
        return CommandResult(
            stdout="",
            stderr="timeout",
            returncode=-1,
            elapsed_ns=elapsed_ns,
        )


# ---------------------------------------------------------------------------
# Corpus helpers
# ---------------------------------------------------------------------------

def load_qsop_meta(qsop_path: pathlib.Path) -> dict:
    meta_path = qsop_path.with_suffix(".meta.json")
    if meta_path.exists():
        with open(meta_path, encoding="utf-8") as f:
            return json.load(f)
    return {}


def iter_qsop_corpus(
    corpus_dir: pathlib.Path,
    tiers: set[str] | None = None,
) -> Iterator[CorpusCase]:
    for tier_dir in sorted(corpus_dir.iterdir()):
        if not tier_dir.is_dir():
            continue
        if tiers and tier_dir.name not in tiers:
            continue
        for qsop_path in sorted(tier_dir.glob("*.qsop")):
            meta = load_qsop_meta(qsop_path)
            instance_id = meta.get("name", qsop_path.stem)
            meta.setdefault("tier", tier_dir.name)
            yield CorpusCase(
                instance_id=instance_id,
                tier=tier_dir.name,
                qsop_path=qsop_path,
                meta=meta,
            )


# ---------------------------------------------------------------------------
# Result helpers
# ---------------------------------------------------------------------------

def counts_hash(output: str) -> str:
    counts_line = next(
        (l for l in output.splitlines() if l.startswith("counts ")), ""
    )
    return hashlib.sha1(counts_line.encode()).hexdigest()[:12]


