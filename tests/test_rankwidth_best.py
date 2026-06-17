#!/usr/bin/env python3
"""Tests for --rankwidth-generate best (picks the lowest-forecast decomposition)."""

import pathlib
import subprocess
import sys
import tempfile


def make_path_qsop(nvars: int, r: int = 8) -> str:
    nedges = nvars - 1
    lines = [f"p qsop {r} {nvars} {nedges}", "n 0", "cst 0"]
    for i in range(nedges):
        lines.append(f"q {i} {i + 1} 1")
    return "\n".join(lines) + "\n"


def make_cycle_qsop(nvars: int, r: int = 8) -> str:
    lines = [f"p qsop {r} {nvars} {nvars}", "n 0", "cst 0"]
    for i in range(nvars):
        lines.append(f"q {i} {(i + 1) % nvars} 1")
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


def test_best_agrees_with_an_individual_generator(exe: pathlib.Path) -> None:
    """best must produce a residue vector that equals one of the 4 individual generators."""
    base_generators = ["left-deep", "balanced", "min-fill", "min-fill-cut"]
    for nvars in range(3, 11):
        for r in [4, 8]:
            qsop_text = make_path_qsop(nvars, r)
            r_best = run_solver(exe, qsop_text, "best")
            assert r_best.returncode == 0, (
                f"best failed (nvars={nvars} r={r}): {r_best.stderr}"
            )
            individual_results = {
                gen: run_solver(exe, qsop_text, gen).stdout
                for gen in base_generators
            }
            assert r_best.stdout in individual_results.values(), (
                f"best output not equal to any individual generator "
                f"(nvars={nvars} r={r}):\n"
                f"  best: {r_best.stdout!r}\n"
                f"  individual: {individual_results}"
            )


def test_best_cycle_agrees_with_individual(exe: pathlib.Path) -> None:
    """best agrees with one of the individual generators on cycle graphs."""
    base_generators = ["left-deep", "balanced", "min-fill", "min-fill-cut"]
    for nvars in range(3, 8):
        qsop_text = make_cycle_qsop(nvars, 8)
        r_best = run_solver(exe, qsop_text, "best")
        assert r_best.returncode == 0, (
            f"best failed (cycle nvars={nvars}): {r_best.stderr}"
        )
        individual_results = {
            gen: run_solver(exe, qsop_text, gen).stdout
            for gen in base_generators
        }
        assert r_best.stdout in individual_results.values(), (
            f"best not equal to any individual generator (cycle nvars={nvars}):\n"
            f"  best: {r_best.stdout!r}"
        )


def test_best_forecast_not_worse_than_individuals(exe: pathlib.Path) -> None:
    """best's max_table_entries forecast must be ≤ each individual generator's forecast."""
    base_generators = ["left-deep", "balanced", "min-fill", "min-fill-cut"]

    def get_forecast(exe: pathlib.Path, qsop_text: str, gen: str) -> int:
        r = run_solver(exe, qsop_text, gen, fmt="stats")
        assert r.returncode == 0, f"{gen} stats failed: {r.stderr}"
        for line in r.stdout.splitlines():
            if line.startswith("rankwidth_table_forecast:"):
                return int(line.split(":")[1].strip())
        raise AssertionError(f"no rankwidth_table_forecast in stats output for {gen}")

    for nvars in range(3, 10):
        qsop_text = make_path_qsop(nvars, 8)
        best_forecast = get_forecast(exe, qsop_text, "best")
        for gen in base_generators:
            ind_forecast = get_forecast(exe, qsop_text, gen)
            assert best_forecast <= ind_forecast, (
                f"best forecast {best_forecast} > {gen} forecast {ind_forecast} "
                f"(nvars={nvars})"
            )


def main(argv: list[str]) -> None:
    if len(argv) < 3:
        print(f"usage: {argv[0]} <sop-solve> <source-root>")
        sys.exit(2)
    exe = pathlib.Path(argv[1])

    test_best_agrees_with_an_individual_generator(exe)
    test_best_cycle_agrees_with_individual(exe)
    test_best_forecast_not_worse_than_individuals(exe)

    print("all rankwidth best tests passed")


if __name__ == "__main__":
    main(sys.argv)
