#!/usr/bin/env python3
"""Focused solver-path smoke tests for CI coverage.

These cases keep the existing correctness surface small while forcing code paths
that normal golden tests intentionally avoid: alternate treewidth orders,
treewidth Fourier factors, rankwidth-from-treewidth generation, and the
rankwidth single-Fourier join kernels.
"""

from __future__ import annotations

import math
import pathlib
import subprocess
import sys


SYNTHETIC_QSOP = """\
p qsop-sign 8 8 10
n 0
cst 1
u 0 1
u 1 2
u 2 3
u 3 4
u 4 5
u 5 6
u 6 7
e 0 1
e 1 2
e 2 3
e 3 4
e 4 5
e 5 6
e 6 7
e 0 3
e 2 6
e 1 5
"""

NO_EDGE_QSOP = """\
p qsop-sign 8 6 0
n 0
cst 3
u 0 1
u 1 2
u 2 3
u 3 4
u 4 5
u 5 6
"""


def run_solver(exe: pathlib.Path, args: list[str], qsop: str = SYNTHETIC_QSOP) -> str:
    result = subprocess.run(
        [str(exe), *args, "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"sop-solve {' '.join(args)} failed with {result.returncode}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result.stdout


def fields(text: str) -> dict[str, str]:
    parsed: dict[str, str] = {}
    for line in text.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            parsed[key.strip()] = value.strip()
    return parsed


def amplitude(text: str) -> complex:
    parsed = fields(text)
    return complex(float(parsed["amplitude_re"]), float(parsed["amplitude_im"]))


def assert_close(left: complex, right: complex, label: str) -> None:
    if not (
        math.isclose(left.real, right.real, rel_tol=1e-9, abs_tol=1e-6)
        and math.isclose(left.imag, right.imag, rel_tol=1e-9, abs_tol=1e-6)
    ):
        raise AssertionError(f"{label}: {left!r} != {right!r}")


def require_fields(text: str, required: set[str]) -> dict[str, str]:
    parsed = fields(text)
    missing = required.difference(parsed)
    if missing:
        raise AssertionError(f"missing fields {sorted(missing)} in:\n{text}")
    return parsed


def check_treewidth_modes(exe: pathlib.Path) -> complex:
    count_stats = run_solver(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "count-table",
            "--treewidth-order",
            "min-degree",
            "--format",
            "stats",
            "--include-result",
            "--include-probability",
        ],
    )
    count_fields = require_fields(
        count_stats,
        {
            "backend",
            "solve_mode",
            "treewidth_order",
            "decomposition_width",
            "result_counts",
            "result_probability",
        },
    )
    if count_fields["backend"] != "treewidth" or count_fields["solve_mode"] != "count-table":
        raise AssertionError(count_stats)

    fourier_stats = run_solver(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "fourier",
            "--treewidth-order",
            "min-fill-max-degree",
            "--format",
            "stats",
            "--include-result",
        ],
    )
    fourier_fields = require_fields(
        fourier_stats,
        {"backend", "solve_mode", "treewidth_order", "result_counts", "join_pairs"},
    )
    if fourier_fields["result_counts"] != count_fields["result_counts"]:
        raise AssertionError(f"treewidth Fourier/count-table mismatch:\n{fourier_stats}\n{count_stats}")

    ref_stats = run_solver(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "long-double",
            "--treewidth-order",
            "min-degree",
            "--format",
            "stats",
        ],
    )
    ref_fields = require_fields(
        ref_stats,
        {"single_mode_precision", "treewidth_order", "numeric_error_bound", "join_pairs"},
    )
    if ref_fields["single_mode_precision"] != "long-double":
        raise AssertionError(ref_stats)
    ref_amp = amplitude(ref_stats)

    double_stats = run_solver(
        exe,
        [
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--treewidth-order",
            "min-fill-max-degree",
            "--format",
            "stats",
        ],
    )
    double_fields = require_fields(
        double_stats,
        {"single_mode_precision", "treewidth_single_complex_kernel", "simd_scalar_fallback_ops"},
    )
    if double_fields["treewidth_single_complex_kernel"] != "2":
        raise AssertionError(double_stats)
    assert_close(amplitude(double_stats), ref_amp, "treewidth double single-Fourier")
    return ref_amp


def check_rankwidth_modes(exe: pathlib.Path, reference: complex) -> None:
    count_stats = run_solver(
        exe,
        [
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "from-treewidth",
            "--rankwidth-mode",
            "count-table",
            "--format",
            "stats",
            "--max-vars",
            "64",
        ],
    )
    count_fields = require_fields(
        count_stats,
        {
            "rankwidth_decomposition",
            "rankwidth_transition_bytes",
            "rankwidth_materialized_join_events",
            "rankwidth_table_assignment_bytes",
        },
    )
    if count_fields["rankwidth_decomposition"] != "from-treewidth":
        raise AssertionError(count_stats)

    single_cases = [
        (
            "rankwidth long-double materialized",
            [
                "--rankwidth-generate",
                "from-treewidth",
                "--single-mode-precision",
                "long-double",
                "--rankwidth-single-kernel",
                "materialized",
            ],
            {"rankwidth_single_complex_kernel": "1", "rankwidth_materialized_join_events": None},
        ),
        (
            "rankwidth double streaming",
            [
                "--rankwidth-generate",
                "min-fill-cut",
                "--single-mode-precision",
                "double",
                "--rankwidth-single-kernel",
                "streaming",
            ],
            {"rankwidth_single_complex_kernel": "2", "rankwidth_streaming_join_events": None},
        ),
        (
            "rankwidth double dense",
            [
                "--rankwidth-generate",
                "min-fill-cut",
                "--single-mode-precision",
                "double",
                "--rankwidth-single-kernel",
                "dense",
            ],
            {"rankwidth_single_complex_kernel": "2", "rankwidth_dense_join_events": None},
        ),
    ]
    for label, extra, expected in single_cases:
        stats = run_solver(
            exe,
            [
                "--backend",
                "rankwidth",
                "--solve-mode",
                "single-fourier",
                "--format",
                "stats",
                "--max-vars",
                "64",
                *extra,
            ],
        )
        parsed = require_fields(
            stats,
            {
                "rankwidth_single_complex_kernel",
                "rankwidth_decomposition",
                "signature_entries",
                "max_signature_entries",
                *expected.keys(),
            },
        )
        for key, value in expected.items():
            if value is not None and parsed[key] != value:
                raise AssertionError(f"{label}: expected {key}={value}, got:\n{stats}")
            if value is None and int(parsed[key]) <= 0:
                raise AssertionError(f"{label}: expected positive {key}, got:\n{stats}")
        assert_close(amplitude(stats), reference, label)

    no_edge = run_solver(
        exe,
        [
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "balanced",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "--format",
            "stats",
            "--max-vars",
            "64",
        ],
        NO_EDGE_QSOP,
    )
    no_edge_fields = require_fields(
        no_edge,
        {"rankwidth_single_complex_kernel", "simd_scalar_fallback_ops", "numeric_error_bound"},
    )
    if no_edge_fields["rankwidth_single_complex_kernel"] != "2":
        raise AssertionError(no_edge)


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} <sop-solve>", file=sys.stderr)
        return 2
    exe = pathlib.Path(argv[1])
    if not exe.exists():
        print(f"error: sop-solve not found at {exe}", file=sys.stderr)
        return 2

    reference = check_treewidth_modes(exe)
    check_rankwidth_modes(exe, reference)
    print("solver coverage-path smoke tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
