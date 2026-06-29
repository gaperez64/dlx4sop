#!/usr/bin/env python3
"""Tests for --rankwidth-generate min-fill-search (adjacent-swap local search)."""

import pathlib
import subprocess
import sys
import tempfile


def make_path_qsop(nvars: int, r: int = 8) -> str:
    nedges = nvars - 1
    lines = [f"p qsop-sign {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"e {i} {i + 1}")
    return "\n".join(lines) + "\n"


def make_cycle_qsop(nvars: int, r: int = 8) -> str:
    lines = [f"p qsop-sign {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"e {i} {(i + 1) % nvars}")
    return "\n".join(lines) + "\n"


def run_solver(exe: pathlib.Path, qsop_text: str, generator: str,
               fmt: str = "residue-vector") -> subprocess.CompletedProcess:
    with tempfile.NamedTemporaryFile(suffix=".qsop", mode="w", delete=False) as f:
        f.write(qsop_text)
        qsop_path = f.name
    args = [str(exe), "--backend", "rankwidth", "--max-vars", "64",
            "--rankwidth-generate", generator, "--format", fmt, qsop_path]
    return subprocess.run(args, check=False, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True)


def get_forecast(exe: pathlib.Path, qsop_text: str, gen: str) -> int:
    r = run_solver(exe, qsop_text, gen, fmt="stats")
    assert r.returncode == 0, f"{gen} stats failed: {r.stderr}"
    for line in r.stdout.splitlines():
        if line.startswith("rankwidth_table_forecast:"):
            return int(line.split(":")[1].strip())
    raise AssertionError(f"no rankwidth_table_forecast in stats output for {gen}")


def test_search_result_correct(exe: pathlib.Path) -> None:
    """min-fill-search must produce the same residue vector as left-deep on path graphs."""
    for nvars in range(3, 13):
        qsop_text = make_path_qsop(nvars)
        r_ref = run_solver(exe, qsop_text, "left-deep")
        r_search = run_solver(exe, qsop_text, "min-fill-search")
        assert r_ref.returncode == 0, f"left-deep failed (nvars={nvars}): {r_ref.stderr}"
        assert r_search.returncode == 0, f"min-fill-search failed (nvars={nvars}): {r_search.stderr}"
        # All generators must agree on the result
        assert r_ref.stdout == r_search.stdout, (
            f"search/left-deep mismatch (nvars={nvars}):\n"
            f"  left-deep: {r_ref.stdout!r}\n  search: {r_search.stdout!r}"
        )


def test_search_cycle_correct(exe: pathlib.Path) -> None:
    """min-fill-search must produce the same result as left-deep on cycle graphs."""
    for nvars in range(3, 9):
        qsop_text = make_cycle_qsop(nvars)
        r_ref = run_solver(exe, qsop_text, "left-deep")
        r_search = run_solver(exe, qsop_text, "min-fill-search")
        assert r_ref.returncode == 0
        assert r_search.returncode == 0, (
            f"min-fill-search failed (cycle nvars={nvars}): {r_search.stderr}"
        )
        assert r_ref.stdout == r_search.stdout, (
            f"search/left-deep mismatch (cycle nvars={nvars})"
        )


def test_search_forecast_not_worse_than_min_fill(exe: pathlib.Path) -> None:
    """min-fill-search forecast must be ≤ min-fill on path graphs (it starts from min-fill)."""
    for nvars in range(3, 12):
        qsop_text = make_path_qsop(nvars, 8)
        f_search = get_forecast(exe, qsop_text, "min-fill-search")
        f_mf = get_forecast(exe, qsop_text, "min-fill")
        assert f_search <= f_mf, (
            f"search forecast {f_search} > min-fill forecast {f_mf} (nvars={nvars})"
        )


def test_best_includes_search_candidate(exe: pathlib.Path) -> None:
    """best should benefit from min-fill-search: its forecast ≤ min-fill-search forecast."""
    for nvars in range(3, 10):
        qsop_text = make_path_qsop(nvars, 8)
        f_best = get_forecast(exe, qsop_text, "best")
        f_search = get_forecast(exe, qsop_text, "min-fill-search")
        assert f_best <= f_search, (
            f"best forecast {f_best} > min-fill-search forecast {f_search} (nvars={nvars})"
        )


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])

    test_search_result_correct(exe)
    test_search_cycle_correct(exe)
    test_search_forecast_not_worse_than_min_fill(exe)
    test_best_includes_search_candidate(exe)

    print("all rankwidth search tests passed")


if __name__ == "__main__":
    main(sys.argv)
