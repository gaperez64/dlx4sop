#!/usr/bin/env python3
"""Build or refresh the local synthetic QSOP corpus.

Generates deterministic synthetic QSOP instances for each tier and writes
them to benchmarks/corpus/sop/ with .meta.json provenance sidecar files.

Usage:
    python3 scripts/build_sop_corpus.py [--output-dir DIR]

The generated instances are small enough to solve with every backend and
cover moduli r=8, r=16 so that WMC encodings can be tested without Ganak.
"""

import argparse
import json
import pathlib
import random
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = REPO_ROOT / "benchmarks" / "corpus" / "sop"

TIERS = [
    {"name": "tier-1-8",   "min_vars": 1,  "max_vars": 8,  "r_choices": [8],  "count": 6},
    {"name": "tier-9-16",  "min_vars": 9,  "max_vars": 16, "r_choices": [8, 16], "count": 4},
    {"name": "tier-17-32", "min_vars": 17, "max_vars": 32, "r_choices": [8, 16], "count": 4},
    {"name": "tier-33-64", "min_vars": 33, "max_vars": 64, "r_choices": [8],  "count": 3},
]

# Provenance template for generated instances.
GENERATOR = "build_sop_corpus.py:v1"


def write_qsop(path: pathlib.Path, r: int, norm_h: int,
               nvars: int, constant: int, unary: list[int], edges: list[tuple[int, int, int]]) -> None:
    with open(path, "w", encoding="utf-8") as f:
        # Header: p qsop <r> <nvars> <nedges>
        f.write(f"p qsop {r} {nvars} {len(edges)}\n")
        # Normalization constant (norm_h).
        f.write(f"n {norm_h}\n")
        if constant:
            f.write(f"cst {constant}\n")
        for v, c in enumerate(unary):
            if c:
                f.write(f"u {v} {c}\n")
        for u, v, q in edges:
            f.write(f"q {u} {v} {q}\n")


def write_meta(path: pathlib.Path, name: str, r: int, nvars: int, nedges: int,
               description: str) -> None:
    meta = {
        "name": name,
        "generator": GENERATOR,
        "r": r,
        "nvars": nvars,
        "nedges": nedges,
        "description": description,
        "solvable_without_external_tools": True,
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")


def gen_path_graph(rng: random.Random, nvars: int, r: int) -> dict:
    """Path graph x0-x1-...-x(n-1) with random labels."""
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    edges = [(i, i + 1, rng.randint(1, r - 1)) for i in range(nvars - 1)]
    return {"unary": unary, "edges": edges, "constant": rng.randint(0, r - 1)}


def gen_cycle_graph(rng: random.Random, nvars: int, r: int) -> dict:
    """Cycle graph with random labels."""
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    edges = [(i, (i + 1) % nvars, rng.randint(1, r - 1)) for i in range(nvars)]
    return {"unary": unary, "edges": edges, "constant": 0}


def gen_random_sparse(rng: random.Random, nvars: int, r: int, edge_prob: float = 0.15) -> dict:
    """Random sparse graph."""
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    edges = []
    for u in range(nvars):
        for v in range(u + 1, nvars):
            if rng.random() < edge_prob:
                edges.append((u, v, rng.randint(1, r - 1)))
    return {"unary": unary, "edges": edges, "constant": rng.randint(0, r - 1)}


def gen_star_graph(rng: random.Random, nvars: int, r: int) -> dict:
    """Star graph: center=0 connected to all others."""
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    edges = [(0, v, rng.randint(1, r - 1)) for v in range(1, nvars)]
    return {"unary": unary, "edges": edges, "constant": 0}


GENERATORS = {
    "path": gen_path_graph,
    "cycle": gen_cycle_graph,
    "sparse": gen_random_sparse,
    "star": gen_star_graph,
}

SHAPES = ["path", "cycle", "sparse", "star", "path", "sparse"]


def build_tier(tier: dict, output_dir: pathlib.Path, seed: int) -> list[str]:
    rng = random.Random(seed)
    tier_dir = output_dir / tier["name"]
    tier_dir.mkdir(parents=True, exist_ok=True)

    written = []
    min_v, max_v = tier["min_vars"], tier["max_vars"]
    for idx in range(tier["count"]):
        r = tier["r_choices"][idx % len(tier["r_choices"])]
        shape = SHAPES[idx % len(SHAPES)]
        nvars = min_v + rng.randint(0, max_v - min_v)
        norm_h = nvars + 2
        gen = GENERATORS[shape]
        data = gen(rng, nvars, r)
        edges = data["edges"]
        name = f"{tier['name']}-{shape}-n{nvars}-r{r}-{idx:02d}"
        qsop_path = tier_dir / f"{name}.qsop"
        meta_path = tier_dir / f"{name}.meta.json"
        write_qsop(qsop_path, r, norm_h, nvars, data["constant"], data["unary"], edges)
        write_meta(meta_path, name, r, nvars, len(edges),
                   f"Synthetic {shape} graph, {nvars} vars, r={r}, seed={seed}+{idx}")
        written.append(str(qsop_path.relative_to(output_dir.parent.parent)))
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=pathlib.Path, default=DEFAULT_OUTPUT,
                        help="root of the QSOP corpus (default: benchmarks/corpus/sop)")
    parser.add_argument("--seed", type=int, default=42,
                        help="RNG seed for reproducible generation (default: 42)")
    args = parser.parse_args()

    all_written = []
    for i, tier in enumerate(TIERS):
        written = build_tier(tier, args.output_dir, args.seed + i * 1000)
        all_written.extend(written)
        print(f"[{tier['name']}] wrote {len(written)} QSOP files", file=sys.stderr)

    print(f"corpus: {len(all_written)} files total", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
