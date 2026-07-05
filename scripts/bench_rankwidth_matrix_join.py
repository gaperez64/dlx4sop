#!/usr/bin/env python3
"""Benchmark the matrix-multiplication barrier construction for rankwidth joins.

This is the Proposition-1/Theorem-5 experiment from the rankwidth note. For
N = 2^m and k = 2m, it builds the twisted join

    H(a, c) = sum_{b,d} M(a,b) G(c,d) (-1)^(b.d)

where G is the normalized Walsh-Hadamard transform of a second matrix N'. The output is
exactly the matrix product M N'. The direct join has N^4 = 2^(2k) pair pressure, while
the dense route is matrix multiplication, classically N^3 = 2^(3k/2).
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import time

os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
os.environ.setdefault("MKL_NUM_THREADS", "1")
os.environ.setdefault("NUMEXPR_NUM_THREADS", "1")

import numpy as np  # noqa: E402

TOOLS_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[0]


def write_jsonl(stream, record: dict) -> None:
    stream.write(json.dumps(record) + "\n")
    stream.flush()


def ns_since(start: int) -> int:
    return time.perf_counter_ns() - start


def fwht_rows(values: np.ndarray) -> None:
    """In-place Walsh-Hadamard transform over the last axis."""
    width = values.shape[1]
    h = 1
    while h < width:
        for start in range(0, width, 2 * h):
            left = values[:, start : start + h].copy()
            right = values[:, start + h : start + 2 * h].copy()
            values[:, start : start + h] = left + right
            values[:, start + h : start + 2 * h] = left - right
        h *= 2


def make_sign_matrix(n: int) -> list[list[float]]:
    signs: list[list[float]] = []
    for b in range(n):
        row = []
        for d in range(n):
            row.append(-1.0 if ((b & d).bit_count() & 1) else 1.0)
        signs.append(row)
    return signs


def direct_twisted_join(m_matrix: np.ndarray, g_matrix: np.ndarray) -> np.ndarray:
    """Reference direct pair enumeration of the twisted join."""
    n = int(m_matrix.shape[0])
    signs = make_sign_matrix(n)
    out = np.zeros((n, n), dtype=np.float64)
    for a in range(n):
        for c in range(n):
            acc = 0.0
            g_row = g_matrix[c]
            for b in range(n):
                mb = float(m_matrix[a, b])
                if mb == 0.0:
                    continue
                sign_row = signs[b]
                for d in range(n):
                    acc += mb * float(g_row[d]) * sign_row[d]
            out[a, c] = acc
    return out


def run_case(m: int, seed: int, max_direct_m: int, dense_repeats: int) -> dict:
    n = 1 << m
    k = 2 * m
    rng = np.random.default_rng(seed + 1009 * m)
    left = rng.integers(0, 4, size=(n, n), dtype=np.int64).astype(np.float64)
    right_prime = rng.integers(0, 4, size=(n, n), dtype=np.int64).astype(np.float64)

    start = time.perf_counter_ns()
    transformed = right_prime.T.copy()
    fwht_rows(transformed)
    g_matrix = transformed / float(n)
    fwht_elapsed_ns = ns_since(start)

    dense_times: list[int] = []
    dense_output = None
    dense_output = left @ right_prime
    for _ in range(dense_repeats):
        start = time.perf_counter_ns()
        dense_output = left @ right_prime
        dense_times.append(ns_since(start))
    dense_elapsed_ns = min(dense_times)

    direct_status = "skipped"
    direct_elapsed_ns = 0
    verified = None
    if m <= max_direct_m:
        direct_status = "ok"
        start = time.perf_counter_ns()
        direct_output = direct_twisted_join(left, g_matrix)
        direct_elapsed_ns = ns_since(start)
        verified = bool(np.allclose(direct_output, dense_output, rtol=1e-9, atol=1e-7))

    speedup = None
    if direct_elapsed_ns and dense_elapsed_ns:
        speedup = direct_elapsed_ns / dense_elapsed_ns

    return {
        "schema": "rankwidth_matrix_join_v1",
        "experiment": "theorem5-matrix-join",
        "m": m,
        "rankwidth_k": k,
        "N": n,
        "direct_status": direct_status,
        "verified": verified,
        "direct_pair_count": n**4,
        "dense_classical_mul_count": n**3,
        "dense_theorem_exponent_with_classical_mm": "2^(3k/2)",
        "direct_exponent": "2^(2k)",
        "fwht_elapsed_ns": fwht_elapsed_ns,
        "dense_elapsed_ns": dense_elapsed_ns,
        "direct_elapsed_ns": direct_elapsed_ns,
        "direct_vs_dense_speedup": speedup,
        "dense_kernel": "numpy.matmul-float64",
        "seed": seed,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=pathlib.Path, default=REPO_ROOT / "artifacts" / "full" / "rankwidth-matrix-join-current.jsonl")
    parser.add_argument("--min-m", type=int, default=2)
    parser.add_argument("--max-m", type=int, default=10)
    parser.add_argument(
        "--max-direct-m",
        type=int,
        default=5,
        help="largest m for direct pair-enumeration timing; larger rows record dense timing only",
    )
    parser.add_argument("--dense-repeats", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1729)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.min_m < 1 or args.max_m < args.min_m:
        print("error: require 1 <= --min-m <= --max-m", file=sys.stderr)
        return 2
    if args.max_direct_m < 0:
        print("error: --max-direct-m must be non-negative", file=sys.stderr)
        return 2
    if args.dense_repeats <= 0:
        print("error: --dense-repeats must be positive", file=sys.stderr)
        return 2

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", encoding="utf-8") as stream:
        for m in range(args.min_m, args.max_m + 1):
            record = run_case(m, args.seed, args.max_direct_m, args.dense_repeats)
            write_jsonl(stream, record)
            dense_ms = record["dense_elapsed_ns"] / 1e6
            direct = record["direct_elapsed_ns"]
            direct_text = "skipped" if not direct else f"{direct / 1e6:.3f} ms"
            print(
                f"m={m} k={record['rankwidth_k']} N={record['N']}: "
                f"direct={direct_text}, dense={dense_ms:.3f} ms",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
