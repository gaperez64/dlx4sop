#!/usr/bin/env python3
"""Bounded-rankwidth synthetic QSOP family for rankwidth crossover experiments.

This materializes the graph family Gamma_{h,t} used in the rankwidth separation
argument: start with a complete binary tree of height h, replace every tree
vertex by a clique of t twin vertices, and replace every tree edge by the
complete bipartite graph between the two corresponding cliques.

The resulting sign-edge QSOP graph has a constant rankwidth bound from the
tree/twin construction, while it contains K_t and therefore has treewidth at
least t - 1. Varying h exercises linear-layout pressure; varying t exercises
the treewidth/contraction lower bound without changing the intended low-rank
structure.

Usage:
    python3 scripts/gen_rankwidth_family.py --height 3 --blowup 4 > instance.qsop
    python3 scripts/gen_rankwidth_family.py --heights 2,3 --blowups 2,4 \
        --materialize-dir benchmarks/corpus/sop/synthetic/rankwidth
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from collections.abc import Iterable

SOURCE_NAME = "Synthetic"
SOURCE_URL = "scripts/gen_rankwidth_family.py"
FAMILY = "binary-tree-clique-blowup"
TIER = "tier-rankwidth"


def _parse_int_list(text: str) -> list[int]:
    values = [int(tok) for tok in text.replace(" ", "").split(",") if tok]
    if not values:
        raise argparse.ArgumentTypeError("expected at least one integer")
    return values


def tree_vertex_count(height: int) -> int:
    return (1 << (height + 1)) - 1


def tree_edges(height: int) -> Iterable[tuple[int, int]]:
    n_tree = tree_vertex_count(height)
    for parent in range(n_tree):
        left = 2 * parent + 1
        right = left + 1
        if left < n_tree:
            yield parent, left
        if right < n_tree:
            yield parent, right


def vertex_id(tree_vertex: int, twin: int, blowup: int) -> int:
    return tree_vertex * blowup + twin


def edge_count(height: int, blowup: int) -> int:
    n_tree = tree_vertex_count(height)
    clique_edges = n_tree * blowup * (blowup - 1) // 2
    bipartite_edges = (n_tree - 1) * blowup * blowup
    return clique_edges + bipartite_edges


def iter_edges(height: int, blowup: int) -> Iterable[tuple[int, int]]:
    n_tree = tree_vertex_count(height)
    for base in range(n_tree):
        for i in range(blowup):
            for j in range(i + 1, blowup):
                yield vertex_id(base, i, blowup), vertex_id(base, j, blowup)
    for parent, child in tree_edges(height):
        for i in range(blowup):
            for j in range(blowup):
                yield vertex_id(parent, i, blowup), vertex_id(child, j, blowup)


def unary_value(index: int, mode: str) -> int:
    if mode == "none":
        return 0
    if mode == "alternating":
        return 1 if index % 2 == 0 else 3
    return 1


def generate_qsop(height: int, blowup: int, modulus: int, unary: str) -> str:
    nvars = tree_vertex_count(height) * blowup
    nedges = edge_count(height, blowup)
    lines = [f"p qsop-sign {modulus} {nvars} {nedges}", "n 0"]
    for v in range(nvars):
        value = unary_value(v, unary) % modulus
        if value:
            lines.append(f"u {v} {value}")
    for u, v in iter_edges(height, blowup):
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def instance_name(height: int, blowup: int, modulus: int, unary: str) -> str:
    return f"btclique-h{height:02d}-t{blowup:02d}-r{modulus}-{unary}"


def metadata(
    name: str,
    qsop: str,
    height: int,
    blowup: int,
    modulus: int,
    unary: str,
    relative_path: str,
) -> dict:
    n_tree = tree_vertex_count(height)
    return {
        "name": name,
        "source": SOURCE_NAME,
        "source_url": SOURCE_URL,
        "source_relative_path": relative_path,
        "family": FAMILY,
        "height": height,
        "blowup": blowup,
        "tree_vertices": n_tree,
        "r": modulus,
        "nvars": n_tree * blowup,
        "nedges": edge_count(height, blowup),
        "unary_pattern": unary,
        "qsop_mode": "sign",
        "theoretical_rankwidth": "O(1)",
        "treewidth_lower_bound": blowup - 1,
        "linear_rankwidth_pressure": "Omega(height)",
        "benchmark_roles": ["rankwidth-separation", "rq2"],
        "solvable_without_external_tools": True,
        "qsop_sha256": hashlib.sha256(qsop.encode()).hexdigest(),
    }


def materialize(
    heights: list[int],
    blowups: list[int],
    modulus: int,
    unary: str,
    out_dir: pathlib.Path,
) -> int:
    tier_dir = out_dir / TIER
    tier_dir.mkdir(parents=True, exist_ok=True)
    written = 0
    for height in heights:
        for blowup in blowups:
            qsop = generate_qsop(height, blowup, modulus, unary)
            name = instance_name(height, blowup, modulus, unary)
            qsop_path = tier_dir / f"{name}.qsop"
            meta_path = tier_dir / f"{name}.meta.json"
            relative_path = f"{TIER}/{qsop_path.name}"
            meta = metadata(name, qsop, height, blowup, modulus, unary, relative_path)
            qsop_path.write_text(qsop, encoding="utf-8")
            meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
            written += 1
            print(
                f"materialized {name} (nvars={meta['nvars']}, nedges={meta['nedges']})",
                file=sys.stderr,
            )
    write_manifest(out_dir)
    print(f"materialized {written} rankwidth instances under {tier_dir}", file=sys.stderr)
    return 0 if written else 1


def write_manifest(out_dir: pathlib.Path) -> None:
    tier_dir = out_dir / TIER
    rows = []
    for meta_path in sorted(tier_dir.glob("*.meta.json")):
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        qsop_name = f"{meta['name']}.qsop"
        rows.append(
            {
                "name": meta["name"],
                "tier": TIER,
                "path": f"{TIER}/{qsop_name}",
                "meta_path": f"{TIER}/{meta_path.name}",
                "family": meta["family"],
                "height": meta["height"],
                "blowup": meta["blowup"],
                "nvars": meta["nvars"],
                "nedges": meta["nedges"],
                "treewidth_lower_bound": meta["treewidth_lower_bound"],
                "theoretical_rankwidth": meta["theoretical_rankwidth"],
            }
        )
    manifest = out_dir / "manifest.jsonl"
    manifest.write_text("".join(json.dumps(row) + "\n" for row in rows), encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--height", type=int, default=3)
    parser.add_argument("--blowup", type=int, default=2)
    parser.add_argument("--heights", type=_parse_int_list, default=None)
    parser.add_argument("--blowups", type=_parse_int_list, default=None)
    parser.add_argument("--modulus", type=int, default=8)
    parser.add_argument("--unary", choices=("all-t", "alternating", "none"), default="all-t")
    parser.add_argument("--materialize-dir", type=pathlib.Path, default=None)
    args = parser.parse_args(argv)

    heights = args.heights or [args.height]
    blowups = args.blowups or [args.blowup]
    if any(h < 0 for h in heights):
        parser.error("heights must be non-negative")
    if any(t < 1 for t in blowups):
        parser.error("blowups must be positive")
    if args.modulus <= 0 or args.modulus % 2 != 0:
        parser.error("--modulus must be a positive even integer")

    if args.materialize_dir is not None:
        return materialize(heights, blowups, args.modulus, args.unary, args.materialize_dir)

    if len(heights) != 1 or len(blowups) != 1:
        parser.error("--materialize-dir is required for multiple heights or blowups")
    sys.stdout.write(generate_qsop(heights[0], blowups[0], args.modulus, args.unary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
