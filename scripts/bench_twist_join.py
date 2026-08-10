#!/usr/bin/env python3
"""Benchmark the twist-diagonalized join against the pairwise single-Fourier kernels.

The paper's cor:crossing-join bounds a branching join by O(2^{p+c}(p+c)) where p is the parent
cut-rank and c the crossing rank between the two children, against the |U|*|V| <= 4^k pairwise
scan. This script generates a family with both parameters under explicit control and measures
the separation.

Family(k, c) on 3k variables l_1..l_k, r_1..r_k, w_1..w_k:

  * edges l_i -- w_i and r_i -- w_i for every i, so each shore's cut toward the rest of the
    graph has rank k (2^k signatures per side) and the parent cut-rank is k;
  * edges l_i -- r_i for i <= c, so rank A[X_L, X_R] = c exactly.

The naive join therefore scans ~4^k pairs while the twist join pays ~2^{k+c}(k+c).

Usage:  bench_twist_join.py <sop-solve> <out.csv> [--kmax K] [--crossings 0,1,2,4] [--timeout S]
"""

import argparse
import csv
import pathlib
import subprocess
import sys
import time


def build_instance(k: int, c: int, modulus: int = 8) -> str:
    """Return QSOP text for Family(k, c). Variables: l_i = i, r_i = k + i, w_i = 2k + i."""
    nvars = 3 * k
    edges = []
    for i in range(k):
        left, right, witness = i, k + i, 2 * k + i
        edges.append((left, witness))
        edges.append((right, witness))
        if i < c:
            edges.append((left, right))
    lines = [f"p qsop-sign {modulus} {nvars} {len(edges)}", f"n {2 * k}", "cst 0"]
    # Unary phases: an odd coefficient on every witness keeps mode 1 non-degenerate.
    for v in range(nvars):
        lines.append(f"u {v} {(v * 3 + 1) % modulus}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def build_decomposition(k: int) -> str:
    """Explicit decomposition forcing the balanced k-vs-k join the family is designed around.

    Left shore l_1..l_k is contracted into one subtree, right shore r_1..r_k into another, and
    the two meet at a single join whose children each expose 2^k signatures; the witnesses
    w_1..w_k are then folded in one at a time above it. Left to its own devices the generator
    finds a cheaper tree that never realizes this join, which is exactly the shape the
    theorem is about.
    """
    nvars = 3 * k
    lines = []
    next_node = 0

    def leaf(var: int) -> int:
        nonlocal next_node
        node = next_node
        next_node += 1
        lines.append(f"l {node} {var}")
        return node

    def join(left: int, right: int) -> int:
        nonlocal next_node
        node = next_node
        next_node += 1
        lines.append(f"j {node} {left} {right}")
        return node

    left_root = leaf(0)
    for i in range(1, k):
        left_root = join(left_root, leaf(i))
    right_root = leaf(k)
    for i in range(1, k):
        right_root = join(right_root, leaf(k + i))
    root = join(left_root, right_root)
    for i in range(k):
        root = join(root, leaf(2 * k + i))
    header = f"p rwdec {nvars} {next_node} {root}"
    return "\n".join([header] + lines) + "\n"


def parse_stats(text: str) -> dict:
    stats = {}
    for line in text.splitlines():
        if ": " in line:
            key, _, value = line.partition(": ")
            stats[key.strip()] = value.strip()
    return stats


def run_case(
    exe: pathlib.Path,
    path: pathlib.Path,
    kernel: str,
    timeout: float,
    decomposition: pathlib.Path | None = None,
) -> dict:
    cmd = [
        str(exe),
        "--backend",
        "rankwidth",
        "--solve-mode",
        "single-fourier",
        "--format",
        "stats",
        "--max-vars",
        "256",
    ]
    if decomposition is not None:
        cmd += ["--rankwidth-decomposition", str(decomposition)]
    else:
        cmd += ["--rankwidth-generate", "min-fill-cut"]
    if kernel != "auto":
        cmd += ["--rankwidth-single-kernel", kernel]
    start = time.monotonic()
    try:
        proc = subprocess.run(cmd + [str(path)], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {"outcome": "timeout", "wall_s": timeout}
    wall = time.monotonic() - start
    if proc.returncode != 0:
        return {
            "outcome": "error",
            "wall_s": wall,
            "message": proc.stderr.decode(errors="replace").strip()[:200],
        }
    stats = parse_stats(proc.stdout.decode(errors="replace"))
    return {
        "outcome": "ok",
        "wall_s": wall,
        "amplitude_re": stats.get("amplitude_re", ""),
        "amplitude_im": stats.get("amplitude_im", ""),
        "join_pairs": stats.get("join_pairs", ""),
        "twist_join_events": stats.get("rankwidth_twist_join_events", ""),
        "streaming_join_events": stats.get("rankwidth_streaming_join_events", ""),
        "materialized_join_events": stats.get("rankwidth_materialized_join_events", ""),
        "dense_join_events": stats.get("rankwidth_dense_join_events", ""),
        "decomposition_width": stats.get("decomposition_width", ""),
        "max_signature_entries": stats.get("max_signature_entries", ""),
        "numeric_error_bound": stats.get("numeric_error_bound", ""),
    }


def amplitudes_agree(left: dict, right: dict) -> bool:
    return close_enough(left["amplitude_re"], right["amplitude_re"]) and close_enough(
        left["amplitude_im"], right["amplitude_im"]
    )


def close_enough(left: str, right: str, tol: float = 1e-6) -> bool:
    try:
        return abs(float(left) - float(right)) <= tol * (1.0 + abs(float(left)))
    except (TypeError, ValueError):
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("solver", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--kmin", type=int, default=3)
    parser.add_argument("--kmax", type=int, default=9)
    parser.add_argument("--crossings", default="0,1,2,4")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--kernels", default="streaming,materialized,twist,auto")
    parser.add_argument(
        "--reference-kernel",
        default="streaming",
        help="kernel whose amplitude every other kernel is checked against",
    )
    parser.add_argument("--workdir", type=pathlib.Path, default=None)
    parser.add_argument(
        "--generated-decomposition",
        action="store_true",
        help="let the solver pick a decomposition instead of the explicit balanced one",
    )
    args = parser.parse_args()

    crossings = [int(x) for x in args.crossings.split(",") if x.strip()]
    kernels = [x for x in args.kernels.split(",") if x.strip()]
    workdir = args.workdir or args.output.parent / "twist_bench_instances"
    workdir.mkdir(parents=True, exist_ok=True)

    rows = []
    mismatches = 0
    errors = 0
    unverified = 0
    for k in range(args.kmin, args.kmax + 1):
        for c in crossings:
            if c > k:
                continue
            path = workdir / f"family_k{k}_c{c}.qsop"
            path.write_text(build_instance(k, c))
            decomposition = None
            if not args.generated_decomposition:
                decomposition = workdir / f"family_k{k}_c{c}.rwdec"
                decomposition.write_text(build_decomposition(k))
            # The oracle is a named kernel, never "whichever ran first": once the pairwise
            # kernels start timing out at large k, the twist would otherwise validate itself.
            reference = None
            unchecked = []
            for kernel in kernels:
                result = run_case(args.solver, path, kernel, args.timeout, decomposition)
                row = {"k": k, "c": c, "nvars": 3 * k, "kernel": kernel}
                row.update(result)
                rows.append(row)
                if result["outcome"] == "error":
                    errors += 1
                    print(
                        f"ERROR k={k} c={c} {kernel}: {result.get('message', '')}",
                        file=sys.stderr,
                    )
                    continue
                if result["outcome"] != "ok":
                    continue
                if kernel == args.reference_kernel:
                    reference = result
                    for pending in unchecked:
                        if not amplitudes_agree(pending[1], reference):
                            mismatches += 1
                            print(f"MISMATCH k={k} c={c} {pending[0]} vs {kernel}", file=sys.stderr)
                    unchecked = []
                elif reference is None:
                    unchecked.append((kernel, result))
                elif not amplitudes_agree(result, reference):
                    mismatches += 1
                    print(
                        f"MISMATCH k={k} c={c} {kernel}: "
                        f"({result['amplitude_re']},{result['amplitude_im']}) vs "
                        f"({reference['amplitude_re']},{reference['amplitude_im']})",
                        file=sys.stderr,
                    )
            if reference is None and unchecked:
                unverified += len(unchecked)
                print(
                    f"note: k={k} c={c} unverified ({args.reference_kernel} did not finish)",
                    file=sys.stderr,
                )
                print(
                    f"k={k:2d} c={c} {kernel:13s} {result['outcome']:8s} "
                    f"{result['wall_s']:8.3f}s joins={result.get('join_pairs', '')}"
                )

    fields = sorted({key for row in rows for key in row})
    with args.output.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\nwrote {len(rows)} rows to {args.output}")
    if unverified:
        print(f"{unverified} runs unverified against {args.reference_kernel}", file=sys.stderr)
    if mismatches or errors:
        print(f"{mismatches} amplitude mismatches, {errors} solver errors", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
