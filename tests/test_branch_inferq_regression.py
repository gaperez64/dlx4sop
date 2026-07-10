#!/usr/bin/env python3
"""Regression for InferQ603 472592...d9c561's branch cost-model false negative."""

from __future__ import annotations

import math
import pathlib
import subprocess
import sys


def run(exe: pathlib.Path, fixture: pathlib.Path, args: list[str]) -> dict[str, str]:
    completed = subprocess.run(
        [str(exe), *args, str(fixture)],
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"sop-solve {' '.join(args)} failed with {completed.returncode}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    values = {}
    for line in completed.stdout.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            values[key.strip()] = value.strip()
    return values


def amplitude(stats: dict[str, str]) -> tuple[float, float, float]:
    return (
        float(stats["amplitude_re"]),
        float(stats["amplitude_im"]),
        float(stats["numeric_error_bound"]),
    )


def assert_within_bounds(name: str, lhs: dict[str, str], rhs: dict[str, str]) -> None:
    lhs_re, lhs_im, lhs_bound = amplitude(lhs)
    rhs_re, rhs_im, rhs_bound = amplitude(rhs)
    error = math.hypot(lhs_re - rhs_re, lhs_im - rhs_im)
    bound = lhs_bound + rhs_bound
    if error > bound:
        raise AssertionError(f"{name}: amplitude distance {error:.3g} exceeds bound {bound:.3g}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_branch_inferq_regression.py <sop-solve> <source-root>", file=sys.stderr)
        return 2
    exe = pathlib.Path(sys.argv[1])
    fixture = pathlib.Path(sys.argv[2]) / "tests" / "golden" / "inferq_472592.qsop"

    branch = run(exe, fixture, ["--format", "stats", "--include-result"])
    expected = {
        "backend": "branch",
        "solve_mode_kernel": "single-fourier",
        "rankwidth_delegations": "1",
        "treewidth_delegations": "0",
        "rankwidth_cutrank_width": "10",
    }
    for key, value in expected.items():
        if branch.get(key) != value:
            raise AssertionError(f"branch {key}: expected {value!r}, got {branch.get(key)!r}")
    bitset_kernel = branch.get("bitset_kernel")
    vectorized_ops = int(branch.get("simd_vectorized_ops", "0"))
    scalar_fallback_ops = int(branch.get("simd_scalar_fallback_ops", "0"))
    if bitset_kernel == "neon":
        # The fixture occupies two 64-bit words, exactly NEON's minimum vector width.
        if vectorized_ops <= 0:
            raise AssertionError("branch did not report the expected NEON bitset vector work")
    elif bitset_kernel in {"scalar", "avx2", "avx512"}:
        # Two words are shorter than AVX2's four lanes and AVX-512's eight lanes.
        if vectorized_ops != 0 or scalar_fallback_ops <= 0:
            raise AssertionError(
                f"branch reported unexpected {bitset_kernel} SIMD counters: "
                f"vectorized={vectorized_ops}, scalar_fallback={scalar_fallback_ops}"
            )
    else:
        raise AssertionError(f"branch reported unknown bitset kernel {bitset_kernel!r}")
    for key in ("rankwidth_table_forecast", "rankwidth_join_pair_forecast", "join_pairs"):
        if int(branch.get(key, "0")) <= 0:
            raise AssertionError(f"branch did not report {key}")

    rankwidth = run(
        exe,
        fixture,
        [
            "--format", "stats", "--backend", "rankwidth", "--rankwidth-generate",
            "from-treewidth", "--solve-mode", "single-fourier", "--max-vars", "2000",
        ],
    )
    treewidth = run(
        exe,
        fixture,
        [
            "--format", "stats", "--backend", "treewidth", "--solve-mode",
            "single-fourier", "--max-vars", "2000",
        ],
    )
    assert_within_bounds("branch vs direct rankwidth", branch, rankwidth)
    assert_within_bounds("branch vs direct treewidth", branch, treewidth)
    print("InferQ branch delegation regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
