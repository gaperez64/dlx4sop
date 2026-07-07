#!/usr/bin/env python3
"""Rankwidth generator/mode family regression.

min-fill-search/count-table, best/count-table, and best/fourier must all
agree on the residue vector for the same instance -- a correctness
invariant, independent of which generator or solve mode is fastest.

Usage: python3 tests/test_rankwidth_family_backends.py <sop-solve>
"""

import pathlib
import subprocess
import sys
import tempfile

CONFIGS = [
    ("min-fill-search/count-table",
     ["--rankwidth-generate", "min-fill-search", "--rankwidth-mode", "count-table"]),
    ("best/count-table",
     ["--rankwidth-generate", "best", "--rankwidth-mode", "count-table"]),
    ("best/fourier",
     ["--rankwidth-generate", "best", "--rankwidth-mode", "fourier"]),
]


def _make_signed_path_qsop(nvars: int = 8, r: int = 8) -> str:
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 1"]
    for i in range(nvars):
        lines.append(f"u {i} {(i % (r - 1)) + 1}")
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def _make_signed_star_qsop(nvars: int = 8, r: int = 8) -> str:
    lines = [f"p qsop-sign {r} {nvars} {nvars - 1}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"u {i} {(i * 3 % (r - 1)) + 1}")
    for v in range(1, nvars):
        lines.append(f"e 0 {v}")
    return "\n".join(lines) + "\n"


def _make_cycle_qsop(nvars: int = 6, r: int = 8) -> str:
    lines = [f"p qsop-sign {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"e {i} {(i + 1) % nvars}")
    return "\n".join(lines) + "\n"


def _write_fixtures(tmp: pathlib.Path) -> list[pathlib.Path]:
    fixtures = [
        ("signed_path", _make_signed_path_qsop()),
        ("signed_star", _make_signed_star_qsop()),
        ("cycle", _make_cycle_qsop()),
    ]
    files = []
    for name, text in fixtures:
        path = tmp / f"{name}.qsop"
        path.write_text(text)
        files.append(path)
    return files


def _residue_vector(sop_solve, qsop, extra_args) -> str:
    cmd = [
        str(sop_solve),
        "--backend",
        "rankwidth",
        "--format",
        "residue-vector",
        "--max-vars",
        "64",
    ] + extra_args + [str(qsop)]
    result = subprocess.run(cmd, capture_output=True, timeout=30)
    if result.returncode != 0:
        raise AssertionError(
            f"{' '.join(extra_args)} failed on {qsop.name}: {result.stderr.decode()[:200]}"
        )
    return result.stdout.decode(errors="replace").strip()


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: test_rankwidth_family_backends.py <sop-solve>", file=sys.stderr)
        return 2
    sop_solve = pathlib.Path(sys.argv[1])
    if not sop_solve.exists():
        print(f"error: sop-solve not found at {sop_solve}", file=sys.stderr)
        return 2

    failed = []
    with tempfile.TemporaryDirectory() as td:
        tmp = pathlib.Path(td)
        files = _write_fixtures(tmp)
        for qsop in files:
            results = {}
            for label, extra_args in CONFIGS:
                try:
                    results[label] = _residue_vector(sop_solve, qsop, extra_args)
                except AssertionError as exc:
                    failed.append(str(exc))
            values = set(results.values())
            if len(values) > 1:
                failed.append(f"{qsop.name}: mismatched residue vectors across configs: {results}")

    if failed:
        for msg in failed:
            print(f"  FAIL: {msg}", file=sys.stderr)
        return 1
    print(f"{len(CONFIGS)} rankwidth configuration(s) agree across fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
