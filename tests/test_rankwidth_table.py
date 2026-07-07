#!/usr/bin/env python3
"""Tests for the rankwidth count-table solver."""

import pathlib
import subprocess
import sys
import tempfile


def make_signed_path_qsop(nvars: int, r: int = 8) -> str:
    """Path graph with non-sign unary terms and signed quadratic edges."""
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 1"]
    for i in range(nvars):
        lines.append(f"u {i} {(i % (r - 1)) + 1}")
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def make_signed_star_qsop(nvars: int, r: int = 8) -> str:
    """Star graph (center=0) with non-sign unary terms and signed quadratic edges."""
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"u {i} {(i * 3 % (r - 1)) + 1}")
    for v in range(1, nvars):
        lines.append(f"e 0 {v}")
    return "\n".join(lines) + "\n"


def make_path_qsop(nvars: int, r: int = 8) -> str:
    """Return a QSOP string for a path graph with `nvars` nodes."""
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def make_sign_edge_path_qsop(nvars: int, r: int = 8) -> str:
    """Path graph with sign-edge coefficients (all edges = r/2, triggering the sign-edge path)."""
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def make_cycle_qsop(nvars: int, r: int = 8) -> str:
    """Return a QSOP string for a cycle with `nvars` nodes."""
    lines = [f"p qsop-sign {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"e {i} {(i + 1) % nvars}")
    return "\n".join(lines) + "\n"


def run_rankwidth(exe: pathlib.Path, qsop_text: str,
                  extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "rankwidth", "--format", "residue-vector", "--max-vars", "64"]
    if extra_args:
        args += extra_args
    args.append(qsop_path)
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def run_treewidth(exe: pathlib.Path, qsop_text: str,
                  extra_args: list[str] | None = None) -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "treewidth", "--format", "residue-vector", "--max-vars", "64"]
    if extra_args:
        args += extra_args
    args.append(qsop_path)
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def test_rankwidth_path_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Rankwidth must produce the same residue vector as treewidth on path graphs."""
    for nvars in range(3, 13):
        for r in [2, 4, 8]:
            qsop_text = make_path_qsop(nvars, r)
            r_tw = run_treewidth(exe, qsop_text)
            r_rw = run_rankwidth(exe, qsop_text)
            assert r_tw.returncode == 0, f"treewidth failed on path-{nvars} r={r}: {r_tw.stderr}"
            assert r_rw.returncode == 0, f"rankwidth failed on path-{nvars} r={r}: {r_rw.stderr}"
            assert r_tw.stdout == r_rw.stdout, (
                f"treewidth/rankwidth mismatch on path-{nvars} r={r}:\n"
                f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
            )


def test_rankwidth_cycle_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Rankwidth must produce the same residue vector as treewidth on cycle graphs."""
    for nvars in range(3, 10):
        for r in [4, 8]:
            qsop_text = make_cycle_qsop(nvars, r)
            r_tw = run_treewidth(exe, qsop_text)
            r_rw = run_rankwidth(exe, qsop_text)
            assert r_tw.returncode == 0, f"treewidth failed on cycle-{nvars} r={r}: {r_tw.stderr}"
            assert r_rw.returncode == 0, f"rankwidth failed on cycle-{nvars} r={r}: {r_rw.stderr}"
            assert r_tw.stdout == r_rw.stdout, (
                f"treewidth/rankwidth mismatch on cycle-{nvars} r={r}:\n"
                f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
            )


def test_rankwidth_table_unknown_option_rejected(exe: pathlib.Path) -> None:
    """--rankwidth-table must be rejected as an unknown option."""
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(make_path_qsop(4))
        qsop_path = f.name
    result = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64",
         "--rankwidth-table", "v2", qsop_path],
        check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    assert result.returncode == 2, (
        f"Expected exit 2 for unknown --rankwidth-table, got {result.returncode}"
    )


def test_rankwidth_signed_path_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Rankwidth must produce the same residue vector as treewidth on signed path graphs."""
    for nvars in range(3, 11):
        for r in [8, 16]:
            qsop_text = make_signed_path_qsop(nvars, r)
            r_tw = run_treewidth(exe, qsop_text)
            r_rw = run_rankwidth(exe, qsop_text)
            assert r_tw.returncode == 0, (
                f"treewidth failed on signed path-{nvars} r={r}: {r_tw.stderr}"
            )
            assert r_rw.returncode == 0, (
                f"rankwidth failed on signed path-{nvars} r={r}: {r_rw.stderr}"
            )
            assert r_tw.stdout == r_rw.stdout, (
                f"treewidth/rankwidth mismatch on signed path-{nvars} r={r}:\n"
                f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
            )


def test_rankwidth_signed_large_crt_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Signed CRT path (nvars >= 64) must agree with treewidth on a large path graph."""
    for nvars in [70, 100]:
        qsop_text = make_signed_path_qsop(nvars, r=8)
        r_tw = run_treewidth(exe, qsop_text, extra_args=["--max-vars", "256"])
        r_rw = run_rankwidth(exe, qsop_text, extra_args=["--max-vars", "256"])
        assert r_tw.returncode == 0, f"treewidth failed on signed path-{nvars}: {r_tw.stderr}"
        assert r_rw.returncode == 0, f"rankwidth failed on signed path-{nvars}: {r_rw.stderr}"
        assert r_tw.stdout == r_rw.stdout, (
            f"treewidth/rankwidth mismatch on signed path-{nvars}:\n"
            f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
        )


def test_rankwidth_signed_star_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Rankwidth must produce the same residue vector as treewidth on signed star graphs."""
    for nvars in range(3, 9):
        for r in [8]:
            qsop_text = make_signed_star_qsop(nvars, r)
            r_tw = run_treewidth(exe, qsop_text)
            r_rw = run_rankwidth(exe, qsop_text)
            assert r_tw.returncode == 0, (
                f"treewidth failed on signed star-{nvars} r={r}: {r_tw.stderr}"
            )
            assert r_rw.returncode == 0, (
                f"rankwidth failed on signed star-{nvars} r={r}: {r_rw.stderr}"
            )
            assert r_tw.stdout == r_rw.stdout, (
                f"treewidth/rankwidth mismatch on signed star-{nvars} r={r}:\n"
                f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
            )


def test_rankwidth_sign_edge_large_crt_agrees_with_treewidth(exe: pathlib.Path) -> None:
    """Sign-edge CRT path (nvars >= 64) must agree with treewidth on large sign-edge paths."""
    for nvars in [70, 100]:
        for r in [4, 8]:
            qsop_text = make_sign_edge_path_qsop(nvars, r=r)
            r_tw = run_treewidth(exe, qsop_text, extra_args=["--max-vars", "256"])
            r_rw = run_rankwidth(exe, qsop_text, extra_args=["--max-vars", "256"])
            assert r_tw.returncode == 0, (
                f"treewidth failed on sign-edge path-{nvars} r={r}: {r_tw.stderr}"
            )
            assert r_rw.returncode == 0, (
                f"rankwidth failed on sign-edge path-{nvars} r={r}: {r_rw.stderr}"
            )
            assert r_tw.stdout == r_rw.stdout, (
                f"treewidth/rankwidth mismatch on sign-edge path-{nvars} r={r}:\n"
                f"  tw: {r_tw.stdout!r}\n  rw: {r_rw.stdout!r}"
            )


def test_fourier_count_table_agree_sign_edge(exe: pathlib.Path) -> None:
    """--solve-mode fourier and --solve-mode count-table must agree on sign-edge SOPs."""
    for nvars in range(3, 10):
        for r in [2, 4, 8]:
            qsop_text = make_sign_edge_path_qsop(nvars, r)
            with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
                f.write(qsop_text)
                qsop_path = f.name
            r_ct = subprocess.run(
                [str(exe), "--backend", "rankwidth", "--max-vars", "64",
                 "--format", "residue-vector", "--solve-mode", "count-table", qsop_path],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            r_ft = subprocess.run(
                [str(exe), "--backend", "rankwidth", "--max-vars", "64",
                 "--format", "residue-vector", "--solve-mode", "fourier", qsop_path],
                check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            assert r_ct.returncode == 0, (
                f"count-table failed on path-{nvars} r={r}: {r_ct.stderr}"
            )
            assert r_ft.returncode == 0, (
                f"fourier failed on path-{nvars} r={r}: {r_ft.stderr}"
            )
            assert r_ct.stdout == r_ft.stdout, (
                f"fourier/count-table mismatch on path-{nvars} r={r}:\n"
                f"  count-table: {r_ct.stdout!r}\n  fourier: {r_ft.stdout!r}"
            )


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])

    test_rankwidth_path_agrees_with_treewidth(exe)
    test_rankwidth_cycle_agrees_with_treewidth(exe)
    test_rankwidth_sign_edge_large_crt_agrees_with_treewidth(exe)
    test_fourier_count_table_agree_sign_edge(exe)
    test_rankwidth_signed_path_agrees_with_treewidth(exe)
    test_rankwidth_signed_large_crt_agrees_with_treewidth(exe)
    test_rankwidth_signed_star_agrees_with_treewidth(exe)
    test_rankwidth_table_unknown_option_rejected(exe)

    print("all rankwidth table tests passed")


if __name__ == "__main__":
    main(sys.argv)
