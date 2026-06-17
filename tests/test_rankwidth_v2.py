#!/usr/bin/env python3
"""Tests for --rankwidth-table v2 (sorted-entry join optimisation)."""

import pathlib
import subprocess
import sys
import tempfile


def make_path_qsop(nvars: int, r: int = 8) -> str:
    """Return a QSOP string for a path graph with `nvars` nodes."""
    nedges = nvars - 1
    lines = [f"p qsop {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"q {i} {i + 1} 1")
    return "\n".join(lines) + "\n"


def make_cycle_qsop(nvars: int, r: int = 8) -> str:
    """Return a QSOP string for a cycle with `nvars` nodes."""
    lines = [f"p qsop {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"q {i} {(i + 1) % nvars} 1")
    return "\n".join(lines) + "\n"


def run_rankwidth(exe: pathlib.Path, qsop_text: str,
                  table_version: str, extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "rankwidth", "--max-vars", "64",
            "--rankwidth-table", table_version, qsop_path]
    if extra_args:
        args += extra_args
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def test_v2_path_agrees_with_v1(exe: pathlib.Path) -> None:
    """v2 must produce the same residue vector as v1 on path graphs."""
    for nvars in range(3, 13):
        for r in [2, 4, 8]:
            qsop_text = make_path_qsop(nvars, r)
            r1 = run_rankwidth(exe, qsop_text, "v1")
            r2 = run_rankwidth(exe, qsop_text, "v2")
            assert r1.returncode == 0, f"v1 failed on path-{nvars} r={r}: {r1.stderr}"
            assert r2.returncode == 0, f"v2 failed on path-{nvars} r={r}: {r2.stderr}"
            assert r1.stdout == r2.stdout, (
                f"v1/v2 mismatch on path-{nvars} r={r}:\n"
                f"  v1: {r1.stdout!r}\n  v2: {r2.stdout!r}"
            )


def test_v2_cycle_agrees_with_v1(exe: pathlib.Path) -> None:
    """v2 must produce the same residue vector as v1 on cycle graphs."""
    for nvars in range(3, 10):
        for r in [4, 8]:
            qsop_text = make_cycle_qsop(nvars, r)
            r1 = run_rankwidth(exe, qsop_text, "v1")
            r2 = run_rankwidth(exe, qsop_text, "v2")
            assert r1.returncode == 0, f"v1 failed on cycle-{nvars} r={r}: {r1.stderr}"
            assert r2.returncode == 0, f"v2 failed on cycle-{nvars} r={r}: {r2.stderr}"
            assert r1.stdout == r2.stdout, (
                f"v1/v2 mismatch on cycle-{nvars} r={r}:\n"
                f"  v1: {r1.stdout!r}\n  v2: {r2.stdout!r}"
            )


def test_v2_requires_rankwidth_backend(exe: pathlib.Path) -> None:
    """--rankwidth-table v2 without --backend rankwidth must be rejected."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(4))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "branch", "--rankwidth-table", "v2", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode != 0, "--rankwidth-table v2 with --backend branch should fail"
    assert "rankwidth" in result.stderr, (
        f"Expected error mentioning rankwidth, got: {result.stderr!r}"
    )


def test_v1_explicit_matches_default(exe: pathlib.Path) -> None:
    """Explicitly passing --rankwidth-table v1 must produce the same result as omitting it."""
    qsop_text = make_path_qsop(8, 8)
    r1 = run_rankwidth(exe, qsop_text, "v1")
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    default_result = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert r1.returncode == 0
    assert default_result.returncode == 0
    assert r1.stdout == default_result.stdout, "v1 explicit != v1 default"


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])

    test_v2_path_agrees_with_v1(exe)
    test_v2_cycle_agrees_with_v1(exe)
    test_v2_requires_rankwidth_backend(exe)
    test_v1_explicit_matches_default(exe)

    print("all rankwidth v2 tests passed")


if __name__ == "__main__":
    main(sys.argv)
