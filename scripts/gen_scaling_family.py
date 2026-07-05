#!/usr/bin/env python3
"""Synthetic scaling family generator for the WMC-vs-solver crossover study.

Real benchmark families do not work here: the scalable MQT families (qaoa, qft,
vqe, qwalk, qpe) use continuous-angle gates (rzz/cp/ry) that the finite-modulus
QSOP importer rejects, while the importable families (ghz, bv, graphstate) are
Clifford and collapse to trivial QSOP treewidth. So we generate a synthetic
*phase-polynomial* (IQP-style) family whose QSOP treewidth grows with the qubit
count, using only discrete Clifford+T gates the importer accepts.

Construction for n qubits:
    H on every qubit
    m = round(density * n) random CCZ gates on distinct qubit triples
    a random T on roughly half the qubits
    H on every qubit
with a fixed-boundary 0^n -> 0^n amplitude. The CCZ gates create a degree-3
phase polynomial whose term-interaction graph has treewidth ~Theta(n) at fixed
density, so the treewidth DP cost (2^treewidth) explodes while #SAT/WMC (ganak)
degrades more gracefully — the regime where WMC overtakes the solver backends.

Usage:
    python3 scripts/gen_scaling_family.py --qubits 24 --seed 1 > circuit.qasm
    python3 scripts/gen_scaling_family.py --qubits 8,12,16,20 --seed 1 \
        --out-dir /tmp/scaling   # writes phase-n{N}-s{SEED}.qasm per size
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import random
import subprocess
import sys

SOURCE_NAME = "Synthetic"
SOURCE_URL = "scripts/gen_scaling_family.py"
FAMILY = "phase-poly"


def generate_qasm(n: int, seed: int, density: float) -> str:
    """Return OpenQASM 2.0 for one synthetic phase-polynomial circuit."""
    rng = random.Random(seed * 100003 + n)
    lines = ["OPENQASM 2.0;", 'include "qelib1.inc";', f"qreg q[{n}];"]
    lines += [f"h q[{i}];" for i in range(n)]
    num_ccz = max(1, round(density * n))
    for _ in range(num_ccz):
        a, b, c = rng.sample(range(n), 3)
        lines.append(f"ccz q[{a}],q[{b}],q[{c}];")
    for i in range(n):
        if rng.random() < 0.5:
            lines.append(f"t q[{i}];")
    lines += [f"h q[{i}];" for i in range(n)]
    return "\n".join(lines) + "\n"


def _parse_sizes(text: str) -> list[int]:
    return [int(tok) for tok in text.replace(" ", "").split(",") if tok]


def materialize(
    sizes: list[int],
    seeds: list[int],
    density: float,
    out_dir: pathlib.Path,
    qasm2sop: pathlib.Path,
) -> int:
    """Import each (size, seed) circuit into a committed .qsop + .meta.json sidecar.

    The corpus is laid out as out_dir/tier-scaling/<stem>.qsop so iter_qsop_corpus
    (scripts/bench_common.py) discovers it as a normal local corpus.
    """
    tier_dir = out_dir / "tier-scaling"
    tier_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for n in sizes:
        for seed in seeds:
            qasm = generate_qasm(n, seed, density)
            stem = f"phase-n{n:03d}-s{seed}"
            proc = subprocess.run(
                [str(qasm2sop), "/dev/stdin"],
                input=qasm, capture_output=True, text=True,
            )
            if proc.returncode != 0 or not proc.stdout.strip():
                print(f"warning: qasm2sop failed for {stem}: {proc.stderr.strip()}", file=sys.stderr)
                continue
            qsop = proc.stdout
            (tier_dir / f"{stem}.qsop").write_text(qsop, encoding="utf-8")
            nvars = 0
            header = qsop.splitlines()[0].split() if qsop.splitlines() else []
            if len(header) >= 4 and header[0] == "p":
                nvars = int(header[3])
            meta = {
                "source": SOURCE_NAME,
                "source_url": SOURCE_URL,
                "family": FAMILY,
                "qubits": n,
                "seed": seed,
                "density": density,
                "boundary_input": "0" * n,
                "boundary_output": "0" * n,
                "qasm_sha256": hashlib.sha256(qasm.encode()).hexdigest(),
                "qsop_sha256": hashlib.sha256(qsop.encode()).hexdigest(),
                "nvars": nvars,
                "qsop_mode": "sign",
                "benchmark_roles": ["scaling-study"],
                "solvable_without_external_tools": True,
            }
            (tier_dir / f"{stem}.meta.json").write_text(
                json.dumps(meta, indent=2) + "\n", encoding="utf-8"
            )
            written += 1
            print(f"materialized {stem} (qubits={n}, nvars={nvars})", file=sys.stderr)
    print(f"materialized {written} scaling instances under {tier_dir}", file=sys.stderr)
    return 0 if written else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--qubits", default="24",
                        help="qubit count, or comma-separated ladder (e.g. 8,12,16,20)")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--density", type=float, default=2.0,
                        help="CCZ gates per qubit (controls treewidth growth)")
    parser.add_argument("--out-dir", type=pathlib.Path, default=None,
                        help="write phase-n{N}-s{seed}.qasm per size here; "
                             "if omitted and a single size is given, writes QASM to stdout")
    parser.add_argument("--materialize-dir", type=pathlib.Path, default=None,
                        help="import each circuit into a committed .qsop + .meta.json corpus "
                             "under this directory (tier-scaling/ subdir)")
    parser.add_argument("--seeds", default=None,
                        help="comma-separated seeds for --materialize-dir (default: just --seed)")
    parser.add_argument("--qasm2sop", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent / "build" / "qasm2sop")
    args = parser.parse_args(argv)

    sizes = _parse_sizes(args.qubits)
    if not sizes:
        parser.error("--qubits must contain at least one size")

    if args.materialize_dir is not None:
        seeds = _parse_sizes(args.seeds) if args.seeds else [args.seed]
        return materialize(sizes, seeds, args.density, args.materialize_dir, args.qasm2sop)

    if args.out_dir is None:
        if len(sizes) != 1:
            parser.error("--out-dir is required when generating a ladder of sizes")
        sys.stdout.write(generate_qasm(sizes[0], args.seed, args.density))
        return 0

    args.out_dir.mkdir(parents=True, exist_ok=True)
    for n in sizes:
        path = args.out_dir / f"phase-n{n}-s{args.seed}.qasm"
        path.write_text(generate_qasm(n, args.seed, args.density), encoding="utf-8")
        print(f"wrote {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
