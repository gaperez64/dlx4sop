#!/usr/bin/env python3
"""Freeze gauntlet QPY cases as post-import QSOP solver inputs.

The selection file is a tab-separated table with the columns ``category``,
``dataset``, and ``instance``.  Instances are resolved below
``DATASET/v1/payloads`` and imported through the exact same QPY -> QASM -> QSOP
route as ``.gauntlet/adapter.py`` and ``bench_gauntlet.py``.
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
from bench_gauntlet import Bench  # noqa: E402


def digest(path: pathlib.Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def git_revision(root: pathlib.Path) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=False,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip() if completed.returncode == 0 else "unknown"


def read_selection(path: pathlib.Path) -> list[dict[str, str]]:
    with path.open(newline="") as handle:
        rows = list(csv.DictReader(handle, delimiter="\t"))
    required = {"category", "dataset", "instance"}
    if not rows or not required.issubset(rows[0]):
        raise ValueError("selection must contain category, dataset, and instance columns")
    expanded: dict[tuple[str, str], dict[str, str]] = {}
    for row in rows:
        start = (row.get("start") or "").strip()
        end = (row.get("end") or "").strip()
        sizes: list[int | None]
        if start or end:
            if not start or not end or "{size}" not in row["instance"]:
                raise ValueError("ranged rows need start, end, and {size} in instance")
            sizes = list(range(int(start), int(end) + 1))
        else:
            sizes = [None]
        for size in sizes:
            instance = row["instance"].format(size=size) if size is not None else row["instance"]
            key = (row["dataset"], instance)
            if key in expanded:
                categories = set(expanded[key]["category"].split(","))
                categories.add(row["category"])
                expanded[key]["category"] = ",".join(sorted(categories))
            else:
                expanded[key] = {
                    "category": row["category"],
                    "dataset": row["dataset"],
                    "instance": instance,
                }
    return list(expanded.values())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("selection", type=pathlib.Path)
    parser.add_argument("--gauntlet-root", type=pathlib.Path, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--build", type=pathlib.Path, default=REPO_ROOT / "build-rel")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    selection = read_selection(args.selection)
    args.out.mkdir(parents=True, exist_ok=True)
    input_dir = args.out / "inputs"
    input_dir.mkdir(exist_ok=True)
    bench = Bench(
        args.build / "qasm2sop",
        args.build / "sop-stats",
        args.build / "sop-solve",
        args.timeout,
        [],
        12 << 30,
    )

    records: list[dict[str, object]] = []
    for row in selection:
        dataset = row["dataset"]
        instance = row["instance"]
        payload = args.gauntlet_root / "datasets" / dataset / "v1" / "payloads" / f"{instance}.qpy"
        if not payload.is_file():
            raise FileNotFoundError(payload)
        qasm, qubits = bench.translate(payload)
        qsop, mode, error_class, diagnostic = bench.import_qsop(qasm, "0" * qubits)
        relative = pathlib.Path("inputs") / f"{dataset}__{instance}.qsop"
        output = args.out / relative
        output.write_text(qsop)
        records.append(
            {
                "category": row["category"],
                "dataset": dataset,
                "instance": instance,
                "qubits": qubits,
                "payload": str(payload.relative_to(args.gauntlet_root)),
                "payload_sha256": digest(payload),
                "qsop": str(relative),
                "qsop_sha256": digest(output),
                "qsop_bytes": output.stat().st_size,
                "import_mode": mode,
                "exact_error_class": error_class,
                "exact_error": diagnostic,
            }
        )
        print(f"{dataset}/{instance}: {mode} -> {relative}", file=sys.stderr)

    manifest = {
        "schema": "dlx4sop_frozen_qsop_corpus_v1",
        "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "source": {
            "gauntlet_root": str(args.gauntlet_root.resolve()),
            "gauntlet_revision": git_revision(args.gauntlet_root),
            "dlx4sop_revision": git_revision(REPO_ROOT),
            "selection": str(args.selection),
            "importer": (
                "adapter.load_circuit -> qiskit.qasm2.dumps -> inline_simple_gates -> "
                "qasm2sop --input 0^n --output 0^n (exact, then --approx 1e-8 only for "
                "phase representability)"
            ),
            "command": " ".join(sys.argv),
        },
        "cases": records,
    }
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
