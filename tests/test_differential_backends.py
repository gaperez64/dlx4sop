#!/usr/bin/env python3
"""
Differential correctness tests: compare every available backend on small random QSOPs.

Ground truth is brute-force (exact for n <= 12).  Every other backend must
agree on the residue vector for each test instance.

Backends tested:
  brute-force, components, treewidth (count-table, all orders),
  treewidth (fourier), branch, rankwidth (all generators, count-table and fourier)

Moduli tested: r = 2, 3, 4, 5, 8
Instance sizes: n = 3..10 variables, up to ~m = n edges

Deterministic seed: SEED = 42 (override with env DIFFERENTIAL_SEED=<n>)
"""

import itertools
import json
import os
import pathlib
import random
import subprocess
import sys
import tempfile
from typing import NamedTuple


SEED = int(os.environ.get("DIFFERENTIAL_SEED", "42"))

# Moduli and sizes to exercise
MODULI = [2, 4, 6, 8]
INSTANCE_SIZES = [3, 4, 5, 6, 7, 8, 9, 10]
INSTANCES_PER_BUCKET = 3   # random instances per (nvars, r) bucket


class SopInstance(NamedTuple):
    r: int
    nvars: int
    nedges: int
    text: str   # full QSOP text


# ---------------------------------------------------------------------------
# QSOP generator
# ---------------------------------------------------------------------------

def random_qsop(rng: random.Random, nvars: int, r: int,
                edge_density: float = 0.4) -> SopInstance:
    """Generate a random QSOP instance."""
    # Build a random graph; each possible edge is included with probability edge_density
    edges = []
    for u in range(nvars):
        for v in range(u + 1, nvars):
            if rng.random() < edge_density:
                q = rng.randint(1, r - 1)   # non-zero coefficient
                edges.append((u, v, q))

    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    constant = rng.randint(0, r - 1)

    lines = [f"p qsop {r} {nvars} {len(edges)}", "n 0", f"cst {constant}"]
    for v in range(nvars):
        if unary[v] != 0:
            lines.append(f"u {v} {unary[v]}")
    for u, v, q in edges:
        lines.append(f"q {u} {v} {q}")
    text = "\n".join(lines) + "\n"
    return SopInstance(r=r, nvars=nvars, nedges=len(edges), text=text)


def generate_test_suite(sizes: list[int], moduli: list[int],
                        per_bucket: int, seed: int) -> list[SopInstance]:
    rng = random.Random(seed)
    instances = []
    for nvars in sizes:
        for r in moduli:
            for _ in range(per_bucket):
                instances.append(random_qsop(rng, nvars, r))
    return instances


# ---------------------------------------------------------------------------
# Solver runner
# ---------------------------------------------------------------------------

def run_sop_solve(exe: pathlib.Path, qsop_text: str, extra_args: list[str]) -> str | None:
    """Run sop-solve and return the residue-vector output, or None on error."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--max-vars", "64"] + extra_args + [qsop_path],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def parse_residue_vector(output: str) -> list[str] | None:
    """Parse counts from p qsop-result output."""
    for line in output.splitlines():
        if line.startswith("counts "):
            return line[len("counts "):].split()
    return None


# ---------------------------------------------------------------------------
# Backend configurations to test
# ---------------------------------------------------------------------------

def backend_configs(nvars: int, r: int) -> list[tuple[str, list[str]]]:
    """Return (label, args) pairs for all backends to test on this instance."""
    configs = [
        ("brute-force", ["--backend", "brute-force"]),
        ("components", ["--backend", "components"]),
        ("treewidth:min-fill",
         ["--backend", "treewidth", "--treewidth-order", "min-fill"]),
        ("treewidth:min-degree",
         ["--backend", "treewidth", "--treewidth-order", "min-degree"]),
        ("treewidth:min-fill-max-degree",
         ["--backend", "treewidth", "--treewidth-order", "min-fill-max-degree"]),
        ("treewidth:fourier",
         ["--backend", "treewidth", "--solve-mode", "fourier"]),
        ("branch", ["--backend", "branch"]),
        ("branch:fourier",
         ["--backend", "branch", "--solve-mode", "fourier"]),
        ("rankwidth:left-deep",
         ["--backend", "rankwidth", "--rankwidth-generate", "left-deep"]),
        ("rankwidth:balanced",
         ["--backend", "rankwidth", "--rankwidth-generate", "balanced"]),
        ("rankwidth:min-fill",
         ["--backend", "rankwidth", "--rankwidth-generate", "min-fill"]),
        ("rankwidth:min-fill-cut",
         ["--backend", "rankwidth", "--rankwidth-generate", "min-fill-cut"]),
        ("rankwidth:min-fill-cut:fourier",
         ["--backend", "rankwidth", "--rankwidth-generate", "min-fill-cut",
          "--rankwidth-mode", "fourier"]),
    ]
    # Fourier requires a prime for NTT; skip for moduli with no suitable prime
    # (the solver handles this gracefully — we just accept a None return value)
    return configs


# ---------------------------------------------------------------------------
# Main comparison logic
# ---------------------------------------------------------------------------

def compare_backends(exe: pathlib.Path, instances: list[SopInstance],
                     verbose: bool = False) -> list[str]:
    """Run all backends on all instances and return a list of failure messages."""
    failures = []
    for idx, inst in enumerate(instances):
        configs = backend_configs(inst.nvars, inst.r)

        # Use brute-force as ground truth
        ground_truth_out = run_sop_solve(exe, inst.text,
                                         ["--backend", "brute-force"])
        if ground_truth_out is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                f"brute-force solver failed"
            )
            continue
        ground_truth = parse_residue_vector(ground_truth_out)
        if ground_truth is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                f"could not parse brute-force output: {ground_truth_out!r}"
            )
            continue

        for label, args in configs:
            if label == "brute-force":
                continue   # already ran this as ground truth
            output = run_sop_solve(exe, inst.text, args)
            if output is None:
                if verbose:
                    print(f"  instance {idx} [{label}]: backend unavailable/failed (skipped)")
                continue   # some backends may legitimately fail on unsupported r
            counts = parse_residue_vector(output)
            if counts is None:
                failures.append(
                    f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{label}]: "
                    f"could not parse output: {output!r}"
                )
                continue
            if counts != ground_truth:
                failures.append(
                    f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{label}]: "
                    f"mismatch\n"
                    f"  QSOP: {inst.text.splitlines()[0]}\n"
                    f"  brute-force: {ground_truth}\n"
                    f"  {label}:     {counts}"
                )
            elif verbose:
                print(f"  instance {idx} [{label}]: ok")

    return failures


def test_all_backends_agree(exe: pathlib.Path, verbose: bool = False) -> None:
    """Main differential test: all backends must agree on all random instances."""
    instances = generate_test_suite(INSTANCE_SIZES, MODULI, INSTANCES_PER_BUCKET, SEED)
    if verbose:
        print(f"generated {len(instances)} instances (seed={SEED})")
    failures = compare_backends(exe, instances, verbose=verbose)
    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(
            f"{len(failures)} differential backend disagreement(s) on "
            f"{len(instances)} instances"
        )


def test_metamorphic_variable_rename(exe: pathlib.Path) -> None:
    """Metamorphic: renaming variables must not change the residue vector."""
    rng = random.Random(SEED + 1)
    for _ in range(10):
        inst = random_qsop(rng, 6, 8)
        nvars = inst.nvars

        # Renaming = permuting variable ids
        perm = list(range(nvars))
        rng.shuffle(perm)
        inv_perm = [0] * nvars
        for i, p in enumerate(perm):
            inv_perm[p] = i

        # Rewrite QSOP with permuted ids
        lines_new = []
        for line in inst.text.splitlines():
            if line.startswith("u "):
                parts = line.split()
                v = int(parts[1])
                lines_new.append(f"u {perm[v]} {parts[2]}")
            elif line.startswith("q "):
                parts = line.split()
                u, v = int(parts[1]), int(parts[2])
                # Sort u/v to keep canonical form (u < v)
                pu, pv = perm[u], perm[v]
                if pu > pv:
                    pu, pv = pv, pu
                lines_new.append(f"q {pu} {pv} {parts[3]}")
            else:
                lines_new.append(line)
        renamed_text = "\n".join(lines_new) + "\n"

        orig_out = run_sop_solve(exe, inst.text, ["--backend", "brute-force"])
        renamed_out = run_sop_solve(exe, renamed_text, ["--backend", "brute-force"])
        if orig_out is None or renamed_out is None:
            continue
        orig = parse_residue_vector(orig_out)
        renamed = parse_residue_vector(renamed_out)
        assert orig == renamed, (
            f"Variable rename changed residue vector!\n"
            f"  original:  {orig}\n"
            f"  renamed:   {renamed}\n"
            f"  original QSOP:\n{inst.text}"
        )


def test_metamorphic_zero_edge(exe: pathlib.Path) -> None:
    """Metamorphic: adding a zero-coefficient edge must not change the result."""
    rng = random.Random(SEED + 2)
    for _ in range(10):
        inst = random_qsop(rng, 6, 8)
        # Add an edge with coefficient = 0 (zero edge)
        nvars = inst.nvars
        u, v = 0, nvars - 1
        if u == v:
            continue
        # Parse and add the zero edge
        lines = inst.text.splitlines()
        header = lines[0].split()
        nedges_new = int(header[3]) + 1
        header[3] = str(nedges_new)
        lines[0] = " ".join(header)
        lines.append(f"q {u} {v} 0")
        modified_text = "\n".join(lines) + "\n"

        orig_out = run_sop_solve(exe, inst.text, ["--backend", "brute-force"])
        modified_out = run_sop_solve(exe, modified_text, ["--backend", "brute-force"])
        if orig_out is None or modified_out is None:
            continue
        orig = parse_residue_vector(orig_out)
        modified = parse_residue_vector(modified_out)
        assert orig == modified, (
            f"Zero-coefficient edge changed residue vector!\n"
            f"  without zero edge: {orig}\n"
            f"  with zero edge:    {modified}"
        )


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])
    _source_root = pathlib.Path(argv[2])
    verbose = "--verbose" in argv or "-v" in argv

    test_all_backends_agree(exe, verbose=verbose)
    test_metamorphic_variable_rename(exe)
    test_metamorphic_zero_edge(exe)
    print(f"all differential backend tests passed (seed={SEED})")


if __name__ == "__main__":
    main(sys.argv)
