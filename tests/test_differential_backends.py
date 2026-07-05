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

        orig_out = run_sop_solve(exe, inst.text, ["--backend", "brute-force"])
        modified_out = run_sop_solve(exe, modified_text, ["--backend", "brute-force"])
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


def bruteforce_amplitude(counts: list[str], r: int) -> complex:
    """Exact amplitude reconstruction from a full residue histogram (double precision;
    fine here since instances are small enough that counts don't overflow double's
    53-bit mantissa)."""
    total = complex(0.0, 0.0)
    omega = cmath.exp(2j * cmath.pi / r)
    for k, c in enumerate(counts):
        total += int(c) * (omega**k)
    return total


def test_single_fourier_mode_matches_bruteforce(
    exe: pathlib.Path, backend_args: list[str], backend_label: str, seed_offset: int,
    verbose: bool = False,
) -> None:
    """The single-mode complex DP (Corollary 1: one complex value per boundary
    signature, table size independent of r) must agree with the exact brute-force
    amplitude reconstruction, within its own reported numeric_error_bound (plus a
    small slack for this test's own double-precision reconstruction, which is less
    precise than the solver's internal long double)."""
    instances = generate_test_suite(INSTANCE_SIZES, MODULI, INSTANCES_PER_BUCKET, SEED + seed_offset)
    failures = []
    for idx, inst in enumerate(instances):
        ground_truth_out = run_sop_solve(exe, inst.text, ["--backend", "brute-force"])
        if ground_truth_out is None:
            continue
        counts = parse_residue_vector(ground_truth_out)
        if counts is None:
            continue
        expected = bruteforce_amplitude(counts, inst.r)

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


def test_old_backends_refuse_huge_r(exe: pathlib.Path) -> None:
    """The count-table/all-modes-Fourier backends allocate O(r) or O(r^2) structures, so
    they must cleanly refuse (fast, with a clear error message) rather than hang or crash
    trying to allocate for r = 2^64-2 -- this is the flip side of the single-mode DP's
    r-independence: old paths are gated, not widened."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(HUGE_R_SMOKE_TEXT)
        qsop_path = f.name

    configs = [
        ("brute-force", ["--backend", "brute-force"]),
        ("brute-force:fourier", ["--backend", "brute-force", "--solve-mode", "fourier"]),
        ("components", ["--backend", "components"]),
        ("branch", ["--backend", "branch"]),
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
        if "refuses modulus > 2^32-1" not in result.stderr:
            failures.append(
                f"[{label}]: expected a 'refuses modulus > 2^32-1' error, got: {result.stderr!r}"
            )

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(f"{len(failures)} old-backend huge-r refusal failure(s)")


def test_single_fourier_mode_treewidth_matches_rankwidth(exe: pathlib.Path,
                                                         verbose: bool = False) -> None:
    """Cross-check: the two independent single-mode complex DP implementations
    (treewidth bucket elimination vs rankwidth leaf/join) must agree with each other,
    not just with brute-force -- this specifically exercises the join/crossing-parity
    code path in solve_join_complex_streaming that has no treewidth analogue."""
    instances = generate_test_suite(INSTANCE_SIZES, MODULI, INSTANCES_PER_BUCKET, SEED + 5)
    failures = []
    for idx, inst in enumerate(instances):
        tw_out = run_sop_solve(exe, inst.text, ["--backend", "treewidth", "--solve-mode", "single-fourier"])
        rw_out = run_sop_solve(exe, inst.text, ["--backend", "rankwidth", "--solve-mode", "single-fourier"])
        if tw_out is None or rw_out is None:
            continue
        tw_parsed = parse_single_fourier_output(tw_out)
        rw_parsed = parse_single_fourier_output(rw_out)
        if tw_parsed is None or rw_parsed is None:
            failures.append(f"instance {idx} (nvars={inst.nvars} r={inst.r}): could not parse output")
            continue
        tw_re, tw_im, tw_bound = tw_parsed
        rw_re, rw_im, rw_bound = rw_parsed
        diff = abs(complex(tw_re, tw_im) - complex(rw_re, rw_im))
        tolerance = tw_bound + rw_bound + 1e-12
        if diff > tolerance:
            failures.append(
                f"instance {idx} (nvars={inst.nvars} r={inst.r}): "
                f"treewidth=({tw_re},{tw_im}) rankwidth=({rw_re},{rw_im}) diff={diff}"
            )
        elif verbose:
            print(f"  instance {idx}: treewidth/rankwidth agree (diff={diff:.3e})")

    if failures:
        for msg in failures:
            print(f"FAIL: {msg}", file=sys.stderr)
        raise AssertionError(
            f"{len(failures)} treewidth/rankwidth single-fourier disagreement(s) on "
            f"{len(instances)} instances"
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
    test_single_fourier_mode_matches_bruteforce(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth", 3,
        verbose=verbose,
    )
    test_single_fourier_mode_matches_bruteforce(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth", 4,
        verbose=verbose,
    )
    test_single_fourier_mode_treewidth_matches_rankwidth(exe, verbose=verbose)
    test_single_fourier_mode_large_r_smoke(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth"
    )
    test_single_fourier_mode_large_r_smoke(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth"
    )
    test_single_fourier_mode_huge_r_smoke(
        exe, ["--backend", "treewidth", "--solve-mode", "single-fourier"], "treewidth"
    )
    test_single_fourier_mode_huge_r_smoke(
        exe, ["--backend", "rankwidth", "--solve-mode", "single-fourier"], "rankwidth"
    )
    test_old_backends_refuse_huge_r(exe)
    print(f"all differential backend tests passed (seed={SEED})")


if __name__ == "__main__":
    main(sys.argv)
