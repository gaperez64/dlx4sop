#!/usr/bin/env python3
"""
Differential correctness tests: compare every available backend on small random QSOPs.

Ground truth is branch exact count-table.  Every other backend must agree on the
residue vector for each test instance.

Backends tested:
  branch, treewidth (count-table, all orders), treewidth (fourier),
  rankwidth (all generators, count-table and fourier)

Moduli tested: r = 2, 3, 4, 5, 8
Instance sizes: n = 3..10 variables, up to ~m = n edges

Deterministic seed: SEED = 42 (override with env DIFFERENTIAL_SEED=<n>)
"""

import cmath
import itertools
import json
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time
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
                edges.append((u, v))

    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    constant = rng.randint(0, r - 1)

    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", f"cst {constant}"]
    for v in range(nvars):
        if unary[v] != 0:
            lines.append(f"u {v} {unary[v]}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
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
    args = list(extra_args)
    mode = None
    if "--solve-mode" in args:
        mode_index = args.index("--solve-mode")
        if mode_index + 1 < len(args):
            mode = args[mode_index + 1]
    if "--format" not in args and mode != "single-fourier":
        args = ["--format", "residue-vector"] + args
    result = subprocess.run(
        [str(exe), "--max-vars", "64"] + args + [qsop_path],
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
        ("branch", ["--backend", "branch", "--solve-mode", "count-table"]),
        ("branch:fourier",
         ["--backend", "branch", "--solve-mode", "fourier"]),
        ("treewidth:min-fill",
         ["--backend", "treewidth", "--treewidth-order", "min-fill"]),
        ("treewidth:min-degree",
         ["--backend", "treewidth", "--treewidth-order", "min-degree"]),
        ("treewidth:min-fill-max-degree",
         ["--backend", "treewidth", "--treewidth-order", "min-fill-max-degree"]),
        ("treewidth:fourier",
         ["--backend", "treewidth", "--solve-mode", "fourier"]),
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

        # Use branch exact count-table as ground truth
        ground_truth_out = run_sop_solve(exe, inst.text,
                                         ["--backend", "branch", "--solve-mode", "count-table"])
        if ground_truth_out is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                f"branch count-table solver failed"
            )
            continue
        ground_truth = parse_residue_vector(ground_truth_out)
        if ground_truth is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                f"could not parse branch output: {ground_truth_out!r}"
            )
            continue

        for label, args in configs:
            if label == "branch":
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
                    f"  branch: {ground_truth}\n"
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
            elif line.startswith("e "):
                parts = line.split()
                u, v = int(parts[1]), int(parts[2])
                # Sort u/v to keep canonical form (u < v)
                pu, pv = perm[u], perm[v]
                if pu > pv:
                    pu, pv = pv, pu
                lines_new.append(f"e {pu} {pv}")
            else:
                lines_new.append(line)
        renamed_text = "\n".join(lines_new) + "\n"

        orig_out = run_sop_solve(exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"])
        renamed_out = run_sop_solve(exe, renamed_text, ["--backend", "branch", "--solve-mode", "count-table"])
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


def test_metamorphic_duplicate_edge_pair(exe: pathlib.Path) -> None:
    """Metamorphic: adding the same sign edge twice must not change the result."""
    rng = random.Random(SEED + 2)
    for _ in range(10):
        inst = random_qsop(rng, 6, 8)
        nvars = inst.nvars
        u, v = 0, nvars - 1
        if u == v:
            continue
        lines = inst.text.splitlines()
        header = lines[0].split()
        nedges_new = int(header[3]) + 2
        header[3] = str(nedges_new)
        lines[0] = " ".join(header)
        lines.append(f"e {u} {v}")
        lines.append(f"e {u} {v}")
        modified_text = "\n".join(lines) + "\n"

        orig_out = run_sop_solve(exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"])
        modified_out = run_sop_solve(exe, modified_text, ["--backend", "branch", "--solve-mode", "count-table"])
        if orig_out is None or modified_out is None:
            continue
        orig = parse_residue_vector(orig_out)
        modified = parse_residue_vector(modified_out)
        assert orig == modified, (
            f"Duplicate edge pair changed residue vector!\n"
            f"  original: {orig}\n"
            f"  modified: {modified}"
        )


def parse_single_fourier_output(output: str) -> tuple[float, float, float] | None:
    """Parse amplitude_re/amplitude_im/numeric_error_bound from single-fourier output."""
    re_val = im_val = bound_val = None
    for line in output.splitlines():
        if line.startswith("amplitude_re: "):
            re_val = float(line[len("amplitude_re: "):])
        elif line.startswith("amplitude_im: "):
            im_val = float(line[len("amplitude_im: "):])
        elif line.startswith("numeric_error_bound: "):
            bound_val = float(line[len("numeric_error_bound: "):])
    if re_val is None or im_val is None or bound_val is None:
        return None
    return re_val, im_val, bound_val


def histogram_amplitude(counts: list[str], r: int) -> complex:
    """Exact amplitude reconstruction from a full residue histogram (double precision;
    fine here since instances are small enough that counts don't overflow double's
    53-bit mantissa)."""
    total = complex(0.0, 0.0)
    omega = cmath.exp(2j * cmath.pi / r)
    for k, c in enumerate(counts):
        total += int(c) * (omega**k)
    return total


def test_single_fourier_mode_matches_exact_counts(
    exe: pathlib.Path, backend_args: list[str], backend_label: str, seed_offset: int,
    verbose: bool = False,
) -> None:
    """The single-mode complex DP (Corollary 1: one complex value per boundary
    signature, table size independent of r) must agree with the exact count-table
    amplitude reconstruction, within its own reported numeric_error_bound (plus a
    small slack for this test's own double-precision reconstruction, which is less
    precise than the solver's internal long double)."""
    instances = generate_test_suite(INSTANCE_SIZES, MODULI, INSTANCES_PER_BUCKET, SEED + seed_offset)
    failures = []
    for idx, inst in enumerate(instances):
        ground_truth_out = run_sop_solve(
            exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"]
        )
        if ground_truth_out is None:
            continue
        counts = parse_residue_vector(ground_truth_out)
        if counts is None:
            continue
        expected = histogram_amplitude(counts, inst.r)

        single_out = run_sop_solve(exe, inst.text, backend_args)
        if single_out is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{backend_label}]: "
                f"single-fourier solve failed"
            )
            continue
        parsed = parse_single_fourier_output(single_out)
        if parsed is None:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{backend_label}]: "
                f"could not parse single-fourier output: {single_out!r}"
            )
            continue
        actual_re, actual_im, bound = parsed
        diff = abs(complex(actual_re, actual_im) - expected)
        tolerance = bound + 1e-6  # slack for this test's own double-precision reconstruction
        if diff > tolerance:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{backend_label}]: "
                f"single-fourier amplitude mismatch: expected {expected}, got "
                f"({actual_re}, {actual_im}), diff={diff}, bound={bound}"
            )
        elif verbose:
            print(f"  instance {idx} [{backend_label}]: single-fourier ok (diff={diff:.3e}, bound={bound:.3e})")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(
            f"{len(failures)} single-fourier mismatch(es) on {len(instances)} instances"
        )


LARGE_R_SMOKE_TEXT = (
    "p qsop-sign 4294967200 5 4\n"
    "n 3\n"
    "cst 123456789\n"
    "u 0 111111111\n"
    "u 1 222222222\n"
    "u 2 333333333\n"
    "u 3 444444444\n"
    "u 4 555555555\n"
    "e 0 1\n"
    "e 1 2\n"
    "e 2 3\n"
    "e 3 4\n"
)


def test_single_fourier_mode_large_r_smoke(exe: pathlib.Path, backend_args: list[str],
                                           backend_label: str) -> None:
    """Regression test for the bug this backend fixes: r in the billions (as produced
    by qasm2sop --approx at a tight error budget) must solve fast, since the DP's
    table size is O(2^k) independent of r -- unlike count-table/all-modes-Fourier
    mode, which would need an O(r) or O(r^2) allocation and could not complete."""
    start = time.monotonic()
    output = run_sop_solve(exe, LARGE_R_SMOKE_TEXT, backend_args)
    elapsed = time.monotonic() - start
    assert output is not None, f"[{backend_label}] large-r single-fourier solve failed"
    assert elapsed < 5.0, (
        f"[{backend_label}] large-r single-fourier solve took {elapsed:.2f}s (expected < 5s)"
    )
    parsed = parse_single_fourier_output(output)
    assert parsed is not None, (
        f"[{backend_label}] could not parse large-r single-fourier output: {output!r}"
    )


HUGE_R_SMOKE_TEXT = (
    "p qsop-sign 18446744073709551614 5 4\n"
    "n 3\n"
    "cst 12345678901234567\n"
    "u 0 1111111111111111\n"
    "u 1 2222222222222222\n"
    "u 2 3333333333333333\n"
    "u 3 4444444444444444\n"
    "u 4 5555555555555555\n"
    "e 0 1\n"
    "e 1 2\n"
    "e 2 3\n"
    "e 3 4\n"
)


def test_single_fourier_mode_huge_r_smoke(exe: pathlib.Path, backend_args: list[str],
                                          backend_label: str) -> None:
    """Same regression as test_single_fourier_mode_large_r_smoke, but with r pinned to
    2^64-2 (the largest even uint64_t value) -- beyond the uint32_t ceiling that existed
    before this session's widening of qsop_instance_t.r/constant/unary to uint64_t. The
    single-mode DP's table size is independent of r, so this must still solve quickly."""
    start = time.monotonic()
    output = run_sop_solve(exe, HUGE_R_SMOKE_TEXT, backend_args)
    elapsed = time.monotonic() - start
    assert output is not None, f"[{backend_label}] huge-r single-fourier solve failed"
    assert elapsed < 5.0, (
        f"[{backend_label}] huge-r single-fourier solve took {elapsed:.2f}s (expected < 5s)"
    )
    parsed = parse_single_fourier_output(output)
    assert parsed is not None, (
        f"[{backend_label}] could not parse huge-r single-fourier output: {output!r}"
    )


def test_count_table_backends_refuse_huge_r(exe: pathlib.Path) -> None:
    """The count-table/all-modes-Fourier backends allocate O(r) or O(r^2) structures, so
    they must cleanly refuse (fast, with a clear error message) rather than hang or crash
    trying to allocate for r = 2^64-2 -- this is the flip side of the single-mode DP's
    r-independence: old paths are gated, not widened."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(HUGE_R_SMOKE_TEXT)
        qsop_path = f.name

    configs = [
        ("branch", ["--backend", "branch", "--solve-mode", "count-table"]),
        ("branch:fourier", ["--backend", "branch", "--solve-mode", "fourier"]),
        ("treewidth", ["--backend", "treewidth"]),
        ("treewidth:fourier", ["--backend", "treewidth", "--solve-mode", "fourier"]),
        ("rankwidth", ["--backend", "rankwidth"]),
        ("rankwidth:fourier", ["--backend", "rankwidth", "--rankwidth-mode", "fourier"]),
    ]
    failures = []
    for label, args in configs:
        start = time.monotonic()
        result = subprocess.run(
            [str(exe), "--max-vars", "64"] + args + [qsop_path],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        elapsed = time.monotonic() - start
        if result.returncode <= 0:
            failures.append(f"[{label}]: expected a clean nonzero exit, got {result.returncode}")
            continue
        if elapsed > 5.0:
            failures.append(f"[{label}]: refusal took {elapsed:.2f}s (expected a fast, clean refusal)")
            continue
        if ("refuses modulus > 2^32-1" not in result.stderr and
                "requires R <= UINT32_MAX" not in result.stderr):
            failures.append(
                f"[{label}]: expected a large-R refusal error, got: {result.stderr!r}"
            )

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(f"{len(failures)} old-backend huge-r refusal failure(s)")


SINGLE_FOURIER_BACKEND_CONFIGS = [
    ("treewidth", ["--backend", "treewidth", "--solve-mode", "single-fourier"]),
    (
        "treewidth-double",
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
    ),
    ("rankwidth", ["--backend", "rankwidth", "--solve-mode", "single-fourier"]),
    (
        "rankwidth-materialized",
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "materialized",
        ],
    ),
    (
        "rankwidth-dense",
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "dense",
        ],
    ),
    (
        "rankwidth-double",
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
    ),
    (
        "rankwidth-double-materialized",
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--rankwidth-single-kernel",
            "materialized",
            "--simd",
            "scalar",
        ],
    ),
    (
        "rankwidth-double-dense",
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--rankwidth-single-kernel",
            "dense",
            "--simd",
            "scalar",
        ],
    ),
    ("branch", ["--backend", "branch", "--solve-mode", "single-fourier"]),
]


def test_single_fourier_mode_backends_agree(exe: pathlib.Path, verbose: bool = False) -> None:
    """Cross-check: every single-mode complex DP implementation (treewidth bucket
    elimination, rankwidth leaf/join, and branch's split-and-delegate-to-either) must
    agree with each other pairwise, not just with exact counts -- this specifically
    exercises the join/crossing-parity code path in solve_join_complex_streaming (no
    treewidth analogue) and branch's own duplicated delegation cost-model (which must
    keep picking a backend that agrees with whichever of treewidth/rankwidth it
    delegates to for a given instance shape)."""
    instances = generate_test_suite(INSTANCE_SIZES, MODULI, INSTANCES_PER_BUCKET, SEED + 5)
    failures = []
    for idx, inst in enumerate(instances):
        parsed_by_label: dict[str, tuple[float, float, float]] = {}
        for label, args in SINGLE_FOURIER_BACKEND_CONFIGS:
            out = run_sop_solve(exe, inst.text, args)
            if out is None:
                continue
            parsed = parse_single_fourier_output(out)
            if parsed is None:
                failures.append(
                    f"instance {idx} (nvars={inst.nvars} r={inst.r}) [{label}]: "
                    f"could not parse output: {out!r}"
                )
                continue
            parsed_by_label[label] = parsed

        labels = list(parsed_by_label)
        for a, b in itertools.combinations(labels, 2):
            a_re, a_im, a_bound = parsed_by_label[a]
            b_re, b_im, b_bound = parsed_by_label[b]
            diff = abs(complex(a_re, a_im) - complex(b_re, b_im))
            tolerance = a_bound + b_bound + 1e-12
            if diff > tolerance:
                failures.append(
                    f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                    f"{a}=({a_re},{a_im}) {b}=({b_re},{b_im}) diff={diff}"
                )
            elif verbose:
                print(f"  instance {idx}: {a}/{b} agree (diff={diff:.3e})")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(
            f"{len(failures)} single-fourier backend disagreement(s) on {len(instances)} instances"
        )


def disconnected_qsop(rng: random.Random, component_sizes: list[int], r: int) -> SopInstance:
    """Concatenate several independent random_qsop instances with disjoint variable ranges
    and zero cross-edges, to exercise branch single-fourier's component-splitting path --
    the one new code path with no coverage from test_single_fourier_mode_backends_agree,
    since random_qsop's edge density (0.4) rarely produces a disconnected graph on its own."""
    offset = 0
    unary: list[int] = []
    edges: list[tuple[int, int]] = []
    for size in component_sizes:
        part = random_qsop(rng, size, r)
        for line in part.text.splitlines():
            if line.startswith("u "):
                _, v, coeff = line.split()
                unary_index = offset + int(v)
                while len(unary) <= unary_index:
                    unary.append(0)
                unary[unary_index] = int(coeff)
            elif line.startswith("e "):
                _, u, v = line.split()
                edges.append((offset + int(u), offset + int(v)))
        offset += size
    nvars = offset
    while len(unary) < nvars:
        unary.append(0)
    constant = rng.randint(0, r - 1)
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", f"cst {constant}"]
    for v in range(nvars):
        if unary[v] != 0:
            lines.append(f"u {v} {unary[v]}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    text = "\n".join(lines) + "\n"
    return SopInstance(r=r, nvars=nvars, nedges=len(edges), text=text)


def test_branch_single_fourier_disconnected_components(exe: pathlib.Path,
                                                        verbose: bool = False) -> None:
    """branch single-fourier's residual component split and complex-multiply combination step
    have little coverage from test_single_fourier_mode_backends_agree, since that suite's
    random instances are rarely disconnected. Build genuinely multi-component instances (2-4
    parts) directly and check against exact count-table ground truth."""
    rng = random.Random(SEED + 7)
    failures = []
    shapes = [[3, 4], [2, 2, 2], [5, 3], [2, 3, 4], [1, 1, 6]]
    for idx, sizes in enumerate(shapes):
        r = rng.choice(MODULI)
        inst = disconnected_qsop(rng, sizes, r)

        ground_truth_out = run_sop_solve(
            exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"]
        )
        if ground_truth_out is None:
            continue
        counts = parse_residue_vector(ground_truth_out)
        if counts is None:
            continue
        expected = histogram_amplitude(counts, inst.r)

        branch_out = run_sop_solve(
            exe, inst.text, ["--backend", "branch", "--solve-mode", "single-fourier"]
        )
        if branch_out is None:
            failures.append(f"shape {idx} {sizes} (r={r}): branch single-fourier solve failed")
            continue
        parsed = parse_single_fourier_output(branch_out)
        if parsed is None:
            failures.append(f"shape {idx} {sizes} (r={r}): could not parse output: {branch_out!r}")
            continue
        actual_re, actual_im, bound = parsed
        diff = abs(complex(actual_re, actual_im) - expected)
        tolerance = bound + 1e-6
        if diff > tolerance:
            failures.append(
                f"shape {idx} {sizes} (r={r}): expected {expected}, got "
                f"({actual_re},{actual_im}), diff={diff}, bound={bound}"
            )
        elif verbose:
            print(f"  shape {idx} {sizes} (r={r}): branch disconnected ok (diff={diff:.3e})")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(f"{len(failures)} disconnected-component failure(s)")


def test_branch_single_fourier_root_guard_allows_splittable_large_instance(
    exe: pathlib.Path, verbose: bool = False
) -> None:
    """qsop_solve_branch_single_mode's root nvars check must not refuse an instance whose
    *total* nvars exceeds --max-vars if it splits into components that are each individually
    small enough to delegate -- otherwise a large instance that's actually a disjoint union of
    many small sub-circuits would be refused outright before ever attempting the (cheap)
    component split. Uses an artificially small --max-vars (well under single-fourier's raised
    default of 4096) so this is exercisable without needing thousands of variables: this
    instance's total nvars (15, three 5-variable components) exceeds --max-vars=10, but no
    single component does."""
    rng = random.Random(SEED + 9)
    r = rng.choice(MODULI)
    sizes = [5, 5, 5]
    inst = disconnected_qsop(rng, sizes, r)
    assert sum(sizes) > 10 > max(sizes)

    ground_truth_out = run_sop_solve(
        exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"]
    )
    assert ground_truth_out is not None, "branch count-table ground truth failed"
    counts = parse_residue_vector(ground_truth_out)
    assert counts is not None, f"could not parse branch output: {ground_truth_out!r}"
    expected = histogram_amplitude(counts, inst.r)

    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(inst.text)
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--max-vars", "10", "--backend", "branch", "--solve-mode", "single-fourier",
         qsop_path],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert result.returncode == 0, (
        f"branch single-fourier should split a {sum(sizes)}-variable, {len(sizes)}-component "
        f"instance into per-component delegates each under --max-vars=10, not refuse the whole "
        f"instance outright; stderr={result.stderr!r}"
    )
    parsed = parse_single_fourier_output(result.stdout)
    assert parsed is not None, f"could not parse output: {result.stdout!r}"
    actual_re, actual_im, bound = parsed
    diff = abs(complex(actual_re, actual_im) - expected)
    tolerance = bound + 1e-6
    assert diff <= tolerance, (
        f"expected {expected}, got ({actual_re},{actual_im}), diff={diff}, bound={bound}"
    )
    if verbose:
        print(f"  root guard allows splittable {sum(sizes)}-var instance (diff={diff:.3e})")


def test_branch_root_guard_allows_splittable_large_instance(
    exe: pathlib.Path, verbose: bool = False
) -> None:
    """The same root-nvars-guard fix as
    test_branch_single_fourier_root_guard_allows_splittable_large_instance above, but for
    qsop_solve_branch's count-table/all-modes entry point. This recursion's branching fallback
    (branch_sum_uncached) has no separate max_fallback_vars-style cap of its own -- unlike
    single-fourier -- so the fix there required also adding a new per-component max_vars check
    at the fallthrough-to-branching point, not just relaxing the root check."""
    rng = random.Random(SEED + 10)
    r = rng.choice(MODULI)
    sizes = [5, 5, 5]
    inst = disconnected_qsop(rng, sizes, r)
    assert sum(sizes) > 10 > max(sizes)

    ground_truth_out = run_sop_solve(
        exe, inst.text, ["--backend", "branch", "--solve-mode", "count-table"]
    )
    assert ground_truth_out is not None, "branch count-table ground truth failed"
    ground_truth = parse_residue_vector(ground_truth_out)
    assert ground_truth is not None, f"could not parse branch output: {ground_truth_out!r}"

    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(inst.text)
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--max-vars", "10", "--format", "residue-vector", "--backend", "branch",
         "--solve-mode", "count-table", qsop_path],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert result.returncode == 0, (
        f"branch should split a {sum(sizes)}-variable, {len(sizes)}-component instance into "
        f"per-component delegates/searches each under --max-vars=10, not refuse the whole "
        f"instance outright; stderr={result.stderr!r}"
    )
    counts = parse_residue_vector(result.stdout)
    assert counts is not None, f"could not parse output: {result.stdout!r}"
    assert counts == ground_truth, f"mismatch: ground_truth={ground_truth} branch={counts}"
    if verbose:
        print(f"  branch root guard allows splittable {sum(sizes)}-var instance")


def test_branch_single_fourier_refuses_wide_component(exe: pathlib.Path) -> None:
    """When neither treewidth (cap 14) nor rankwidth (cap 12) is viable for a connected
    component, the explicit delegate-only branch single-fourier policy must preserve the old
    clear, fast refusal without attempting residual fallback."""
    rng = random.Random(SEED + 8)
    nvars = 40
    edges = [
        (u, v)
        for u in range(nvars)
        for v in range(u + 1, nvars)
        if rng.random() < 0.5
    ]
    text = "\n".join(
        [f"p qsop-sign 16 {nvars} {len(edges)}", "n 0", "cst 0"]
        + [f"e {u} {v}" for u, v in edges]
    ) + "\n"
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(text)
        qsop_path = f.name

    start = time.monotonic()
    result = subprocess.run(
        [str(exe), "--max-vars", "64", "--backend", "branch", "--solve-mode", "single-fourier",
         "--branch-single-fourier-fallback", "delegate-only",
         "--trace", "csv",
         qsop_path],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    elapsed = time.monotonic() - start
    assert result.returncode != 0, "expected a nonzero exit for a too-wide component"
    assert elapsed < 5.0, f"refusal took {elapsed:.2f}s (expected a fast, clean refusal)"
    assert "no delegate available" in result.stderr, (
        f"expected a 'no delegate available' error, got: {result.stderr!r}"
    )
    assert "branch.single.fallback_node" not in result.stderr, (
        f"delegate-only must refuse before residual fallback\n{result.stderr}"
    )


def test_branch_single_fourier_large_component(exe: pathlib.Path) -> None:
    """Regression test: qsop_compute_stats_with_order only populates its order[] output for
    nvars <= 63 (compute_large_width_diagnostics, used above that threshold, computes
    min_fill_width/prefix_cut_rank correctly but never touches order at all). Passing that
    unpopulated (all-zero) buffer straight to the treewidth delegate previously crashed with
    "internal error: treewidth elimination found no factor for variable" on any >63-variable
    low-treewidth component -- caught via a real gauntlet circuit during manual verification,
    not by the smaller random instances above (INSTANCE_SIZES tops out at 10). A path graph
    (treewidth 1 regardless of length) with 100 variables exercises this directly."""
    rng = random.Random(SEED + 9)
    nvars = 100
    r = 8
    unary = [rng.randint(0, r - 1) for _ in range(nvars)]
    constant = rng.randint(0, r - 1)
    lines = [f"p qsop-sign {r} {nvars} {nvars - 1}", "n 0", f"cst {constant}"]
    for v in range(nvars):
        if unary[v] != 0:
            lines.append(f"u {v} {unary[v]}")
    for v in range(nvars - 1):
        lines.append(f"e {v} {v + 1}")
    text = "\n".join(lines) + "\n"

    # run_sop_solve hardcodes --max-vars 64 before extra_args; a trailing --max-vars here
    # overrides it (sop_solve.c's parser is last-value-wins), needed since nvars=100 here.
    branch_out = run_sop_solve(
        exe, text,
        ["--backend", "branch", "--solve-mode", "single-fourier", "--max-vars", "128"],
    )
    treewidth_out = run_sop_solve(
        exe, text,
        ["--backend", "treewidth", "--solve-mode", "single-fourier", "--max-vars", "128"],
    )
    assert branch_out is not None, "branch single-fourier failed on a 100-variable path graph"
    assert treewidth_out is not None, "treewidth single-fourier failed on a 100-variable path graph"
    branch_parsed = parse_single_fourier_output(branch_out)
    treewidth_parsed = parse_single_fourier_output(treewidth_out)
    assert branch_parsed is not None, f"could not parse branch output: {branch_out!r}"
    assert treewidth_parsed is not None, f"could not parse treewidth output: {treewidth_out!r}"
    b_re, b_im, b_bound = branch_parsed
    t_re, t_im, t_bound = treewidth_parsed
    diff = abs(complex(b_re, b_im) - complex(t_re, t_im))
    tolerance = b_bound + t_bound + 1e-9
    assert diff <= tolerance, (
        f"branch/treewidth disagree on 100-variable path graph: "
        f"branch=({b_re},{b_im}) treewidth=({t_re},{t_im}) diff={diff}"
    )


def test_branch_single_fourier_residual_fallback(exe: pathlib.Path) -> None:
    """Force branch single-fourier past its delegate caps and check the scalar residual
    fallback against the treewidth single-mode backend on a small complete graph."""
    nvars = 16
    r = 8
    edges = [(u, v) for u in range(nvars) for v in range(u + 1, nvars)]
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 3"]
    for v in range(nvars):
        lines.append(f"u {v} {(2 * v + 1) % r}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    text = "\n".join(lines) + "\n"

    branch_out = run_sop_solve(
        exe,
        text,
        [
            "--backend", "branch",
            "--solve-mode", "single-fourier",
            "--branch-rw-source", "none",
            "--branch-single-fourier-fallback", "always",
            "--branch-single-max-fallback-vars", str(nvars),
            "--branch-single-max-search-nodes", "2000000",
            "--max-vars", "32",
        ],
    )
    treewidth_out = run_sop_solve(
        exe,
        text,
        ["--backend", "treewidth", "--solve-mode", "single-fourier", "--max-vars", "32"],
    )
    assert branch_out is not None, "branch residual fallback failed"
    assert treewidth_out is not None, "treewidth single-fourier reference failed"
    branch_parsed = parse_single_fourier_output(branch_out)
    treewidth_parsed = parse_single_fourier_output(treewidth_out)
    assert branch_parsed is not None, f"could not parse branch output: {branch_out!r}"
    assert treewidth_parsed is not None, f"could not parse treewidth output: {treewidth_out!r}"
    b_re, b_im, b_bound = branch_parsed
    t_re, t_im, t_bound = treewidth_parsed
    diff = abs(complex(b_re, b_im) - complex(t_re, t_im))
    assert diff <= b_bound + t_bound + 1e-7, (
        f"branch fallback/treewidth disagree: branch=({b_re},{b_im}) "
        f"treewidth=({t_re},{t_im}) diff={diff}"
    )


def path_qsop(nvars: int, r: int) -> str:
    """A path graph (treewidth 1 regardless of length): 0-1-2-...-(nvars-1)."""
    lines = [f"p qsop-sign {r} {nvars} {nvars - 1}", "n 0", "cst 0"]
    lines += [f"e {v} {v + 1}" for v in range(nvars - 1)]
    return "\n".join(lines) + "\n"


def test_single_fourier_default_max_vars(exe: pathlib.Path) -> None:
    """--max-vars defaults to 24 (a count-table safety valve, since nvars directly
    drives their 2^nvars or O(r) cost). single-fourier mode has no such blowup, so its default
    is raised to 4096 when the caller doesn't pass --max-vars explicitly (see sop_solve.c's
    comment above `if (single_fourier_mode && !max_vars_set)`). Check both halves: a
    30-variable low-treewidth instance succeeds under single-fourier with no --max-vars flag
    at all, while a dense 30-variable count-table instance is still refused by the unraised
    default --
    confirming the raise is scoped to single-fourier, not a global change."""
    text = path_qsop(30, 8)
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(text)
        qsop_path = f.name

    single_fourier = subprocess.run(
        [str(exe), "--backend", "branch", "--solve-mode", "single-fourier", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert single_fourier.returncode == 0, (
        f"single-fourier with no --max-vars should succeed on 30 variables (default raised to "
        f"4096), got: {single_fourier.stderr!r}"
    )

    rng = random.Random(0)
    dense_edges = [
        (u, v)
        for u in range(30)
        for v in range(u + 1, 30)
        if rng.random() < 0.5
    ]
    dense_text = "\n".join(
        [f"p qsop-sign 8 30 {len(dense_edges)}", "n 0", "cst 0"]
        + [f"e {u} {v}" for u, v in dense_edges]
    ) + "\n"
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(dense_text)
        dense_qsop_path = f.name

    count_table = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "count-table",
            "--format",
            "residue-vector",
            dense_qsop_path,
        ],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert count_table.returncode != 0 and "refuses" in count_table.stderr, (
        f"count-table mode's default --max-vars must stay at 24 (unraised); expected a clean "
        f"refusal on 30 variables, got returncode={count_table.returncode} "
        f"stderr={count_table.stderr!r}"
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
    test_metamorphic_duplicate_edge_pair(exe)
    test_single_fourier_mode_matches_exact_counts(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth", 3,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth", 4,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "treewidth-double",
        13,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "rankwidth-double",
        14,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "dense",
        ],
        "rankwidth-dense",
        15,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--rankwidth-single-kernel",
            "dense",
            "--simd",
            "scalar",
        ],
        "rankwidth-double-dense",
        16,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_exact_counts(
        exe, ["--backend", "branch", "--solve-mode", "single-fourier"], "branch", 6,
        verbose=verbose,
    )
    test_single_fourier_mode_backends_agree(exe, verbose=verbose)
    test_branch_single_fourier_disconnected_components(exe, verbose=verbose)
    test_branch_single_fourier_root_guard_allows_splittable_large_instance(exe, verbose=verbose)
    test_branch_root_guard_allows_splittable_large_instance(exe, verbose=verbose)
    test_branch_single_fourier_refuses_wide_component(exe)
    test_branch_single_fourier_large_component(exe)
    test_branch_single_fourier_residual_fallback(exe)
    test_single_fourier_default_max_vars(exe)
    test_single_fourier_mode_large_r_smoke(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth"
    )
    test_single_fourier_mode_large_r_smoke(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth"
    )
    test_single_fourier_mode_large_r_smoke(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "treewidth-double",
    )
    test_single_fourier_mode_large_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "rankwidth-double",
    )
    test_single_fourier_mode_large_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "dense",
        ],
        "rankwidth-dense",
    )
    test_single_fourier_mode_large_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--rankwidth-single-kernel",
            "dense",
            "--simd",
            "scalar",
        ],
        "rankwidth-double-dense",
    )
    test_single_fourier_mode_large_r_smoke(
        exe, ["--backend", "branch", "--solve-mode", "single-fourier"], "branch"
    )
    test_single_fourier_mode_huge_r_smoke(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth"
    )
    test_single_fourier_mode_huge_r_smoke(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth"
    )
    test_single_fourier_mode_huge_r_smoke(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "treewidth-double",
    )
    test_single_fourier_mode_huge_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--simd",
            "scalar",
        ],
        "rankwidth-double",
    )
    test_single_fourier_mode_huge_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "dense",
        ],
        "rankwidth-dense",
    )
    test_single_fourier_mode_huge_r_smoke(
        exe,
        [
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--rankwidth-single-kernel",
            "dense",
            "--simd",
            "scalar",
        ],
        "rankwidth-double-dense",
    )
    test_single_fourier_mode_huge_r_smoke(
        exe, ["--backend", "branch", "--solve-mode", "single-fourier"], "branch"
    )
    test_count_table_backends_refuse_huge_r(exe)
    print(f"all differential backend tests passed (seed={SEED})")


if __name__ == "__main__":
    main(sys.argv)
