#!/usr/bin/env python3
"""Treewidth backend scaling smoke test.

A large, low-treewidth synthetic instance (a ladder graph: two paths joined
by rungs, treewidth 2 regardless of size) must solve quickly via the
treewidth backend.

Usage: python3 tests/test_treewidth_scaling_smoke.py <sop-solve>
"""

import pathlib
import subprocess
import sys
import tempfile
import time

RUNGS = 150          # ladder rungs -> nvars = 2 * RUNGS
MODULUS = 8
TIMEOUT_S = 10.0


def _make_ladder_qsop(rungs: int, r: int) -> str:
    """Two length-`rungs` paths joined by rungs: treewidth 2 regardless of size."""
    nvars = 2 * rungs
    edges = []
    for i in range(rungs - 1):
        edges.append((i, i + 1))
        edges.append((rungs + i, rungs + i + 1))
    for i in range(rungs):
        edges.append((i, rungs + i))
    lines = [f"p qsop-sign {r} {nvars} {len(edges)}", "n 0", "cst 0"]
    for i in range(nvars):
        if i % 3 == 0:
            lines.append(f"u {i} {(i % (r - 1)) + 1}")
    for u, v in edges:
        lines.append(f"e {u} {v}")
    return "\n".join(lines) + "\n"


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_treewidth_scaling_smoke.py <sop-solve>", file=sys.stderr)
        return 2
    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: sop-solve not found at {sop_solve}", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as td:
        qsop = pathlib.Path(td) / "ladder.qsop"
        qsop.write_text(_make_ladder_qsop(RUNGS, MODULUS))

        cmd = [
            str(sop_solve), "--backend", "treewidth",
            "--max-vars", "4096", "--format", "stats", str(qsop),
        ]
        start = time.perf_counter()
        try:
            result = subprocess.run(cmd, capture_output=True, timeout=TIMEOUT_S)
        except subprocess.TimeoutExpired:
            print(
                f"FAIL: treewidth backend did not complete within {TIMEOUT_S}s "
                f"on a {2 * RUNGS}-variable ladder graph",
                file=sys.stderr,
            )
            return 1
        elapsed = time.perf_counter() - start

    if result.returncode != 0:
        print(
            f"FAIL: treewidth backend exited {result.returncode}: "
            f"{result.stderr.decode()[:300]}",
            file=sys.stderr,
        )
        return 1

    stdout = result.stdout.decode()
    width = None
    for line in stdout.splitlines():
        if line.startswith("decomposition_width:"):
            width = int(line.split(":", 1)[1].strip())
            break
    if width is None:
        print(f"FAIL: no decomposition_width in stats output:\n{stdout}", file=sys.stderr)
        return 1
    if width > 8:
        print(
            f"FAIL: decomposition_width {width} unexpectedly high for a ladder graph "
            f"(expected near 2-3)",
            file=sys.stderr,
        )
        return 1

    print(
        f"treewidth backend solved {2 * RUNGS}-variable ladder graph in "
        f"{elapsed:.2f}s (decomposition_width={width})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
