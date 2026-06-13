#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run_solve(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    qsop = source_root / "tests" / "golden" / f"{name}.qsop"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), "--format", "residue-vector", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: sop-solve failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: residue-vector mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )

    brute_force = subprocess.run(
        [str(exe), "--backend", "brute-force", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if brute_force.returncode != 0:
        raise AssertionError(f"{name}: brute-force backend failed\n{brute_force.stderr}")
    if brute_force.stdout != expected_text:
        raise AssertionError(
            f"{name}: brute-force residue-vector mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{brute_force.stdout}\n"
        )

    branch = subprocess.run(
        [str(exe), "--backend", "branch", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode != 0:
        raise AssertionError(f"{name}: branch backend failed\n{branch.stderr}")
    if branch.stdout != expected_text:
        raise AssertionError(
            f"{name}: branch residue-vector mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{branch.stdout}\n"
        )


def run_max_vars_guard(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_single.qsop"
    completed = subprocess.run(
        [str(exe), "--max-vars", "0", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0:
        raise AssertionError("max-vars guard unexpectedly allowed solve")
    if "brute-force solver refuses 1 variables" not in completed.stderr:
        raise AssertionError(f"unexpected diagnostic:\n{completed.stderr}")


def run_large_rankwidth_crt(exe: pathlib.Path) -> None:
    qsop = "p qsop-sign 16 64 0\nn 0\ncst 0\n"
    completed = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected = (
        "p qsop-result 16\n"
        "n 0\n"
        "counts 18446744073709551616 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
    )
    if completed.returncode != 0 or completed.stdout != expected:
        raise AssertionError(f"large rankwidth CRT solve failed\n{completed.stdout}\n{completed.stderr}")

    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode == 0 or "exceeds uint64 capacity" not in branch.stderr:
        raise AssertionError(f"branch did not report uint64 overflow\n{branch.stdout}\n{branch.stderr}")


def run_solver_stats(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    cases = [
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "branch",
                str(source_root / "tests" / "golden" / "solve_labelled.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_branch.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "branch",
                str(source_root / "tests" / "golden" / "solve_isolated_branch.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_isolated_branch.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "branch",
                str(source_root / "tests" / "golden" / "solve_branch_cache.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_branch_cache.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "components",
                str(source_root / "tests" / "golden" / "solve_disconnected.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_components.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "components",
                str(source_root / "tests" / "golden" / "solve_repeated_components.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_repeated_components.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "components",
                str(source_root / "tests" / "golden" / "solve_mirrored_components.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_mirrored_components.stats",
        ),
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "components",
                str(source_root / "tests" / "golden" / "solve_mirrored_path_components.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_mirrored_path_components.stats",
        ),
    ]

    for cmd, expected_path in cases:
        completed = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"solver stats failed for {cmd}\n{completed.stderr}")
        expected_text = expected_path.read_text()
        if completed.stdout != expected_text:
            raise AssertionError(
                f"solver stats mismatch for {cmd}\n"
                f"expected:\n{expected_text}\n"
                f"actual:\n{completed.stdout}\n"
            )


def run_cli_paths(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_single.qsop"
    expected = source_root / "tests" / "golden" / "solve_single.expected"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: sop-solve" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    stdin_result = subprocess.run(
        [str(exe), "-"],
        input=qsop.read_text(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if stdin_result.returncode != 0 or stdin_result.stdout != expected.read_text():
        raise AssertionError(f"unexpected stdin result:\n{stdin_result.stdout}\n{stdin_result.stderr}")

    error_cases = [
        ([str(exe), "--format"], "requires a value"),
        ([str(exe), "--format", "json", str(qsop)], "unsupported format"),
        ([str(exe), "--backend"], "requires a value"),
        ([str(exe), "--backend", "treewidth", str(qsop)], "unsupported backend"),
        ([str(exe), "--backend", "branch", "--max-vars", "0", str(qsop)], "residual branch solver refuses"),
        ([str(exe), "--rankwidth-decomposition"], "requires a path"),
        (
            [str(exe), "--rankwidth-decomposition", str(qsop), str(qsop)],
            "requires --backend rankwidth",
        ),
        ([str(exe), "--rankwidth-generate"], "requires a value"),
        ([str(exe), "--rankwidth-generate", "bad", str(qsop)], "unsupported rankwidth generator"),
        ([str(exe), "--rankwidth-generate", "linear", str(qsop)], "requires --backend rankwidth"),
        ([str(exe), "--rankwidth-mode"], "requires a value"),
        ([str(exe), "--rankwidth-mode", "bad", str(qsop)], "unsupported rankwidth mode"),
        ([str(exe), "--rankwidth-mode", "fourier", str(qsop)], "requires --backend rankwidth"),
        ([str(exe), "--branch-heuristic"], "requires a value"),
        ([str(exe), "--branch-heuristic", "rankwidth", str(qsop)], "unsupported branch heuristic"),
        ([str(exe), "--branch-heuristic", "treewidth", str(qsop)], "requires --backend branch"),
        ([str(exe), "--trace"], "requires a value"),
        ([str(exe), "--trace", "json", str(qsop)], "unsupported trace format"),
        ([str(exe), "--max-vars"], "requires a non-negative"),
        ([str(exe), "--max-vars", "-1", str(qsop)], "requires a non-negative"),
        ([str(exe), "--bad"], "unknown option"),
        ([str(exe), str(qsop), str(qsop)], "at most one input"),
        ([str(exe), str(source_root / "tests" / "golden" / "missing.qsop")], "No such file"),
    ]
    for cmd, expected_error in error_cases:
        completed = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode == 0 or expected_error not in completed.stderr:
            raise AssertionError(f"unexpected error result for {cmd}:\n{completed.stderr}")


def assert_rankwidth_matches(
    exe: pathlib.Path,
    qsop: pathlib.Path,
    expected_stdout: str,
    *extra_args: str,
) -> None:
    completed = subprocess.run(
        [str(exe), "--backend", "rankwidth", *extra_args, str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0 or completed.stdout != expected_stdout:
        raise AssertionError(
            f"rankwidth solve mismatch for {extra_args}\n{completed.stdout}\n{completed.stderr}"
        )


def assert_bad_rankwidth_decomposition(
    exe: pathlib.Path,
    qsop: pathlib.Path,
    directory: pathlib.Path,
    name: str,
    text: str,
    expected_error: str,
) -> None:
    path = directory / f"{name}.rwdec"
    path.write_text(text)
    completed = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(path),
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0 or expected_error not in completed.stderr:
        raise AssertionError(f"unexpected rankwidth validation result for {name}\n{completed.stderr}")


def run_rankwidth_backend(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_sign_path.qsop"
    decomposition = source_root / "tests" / "golden" / "solve_sign_path.rwdec"
    expected = subprocess.run(
        [str(exe), "--backend", "brute-force", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if expected.returncode != 0:
        raise AssertionError(f"brute force solve failed\n{expected.stderr}")

    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-decomposition",
        str(decomposition),
    )
    assert_rankwidth_matches(exe, qsop, expected.stdout)
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "balanced")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "min-fill")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "min-fill-cut")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-mode", "fourier")
    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-generate",
        "min-fill",
        "--rankwidth-mode",
        "fourier",
    )
    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-generate",
        "min-fill-cut",
        "--rankwidth-mode",
        "fourier",
    )

    stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(decomposition),
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_stats = {
        "backend: rankwidth",
        "rankwidth_mode: count-table",
        "rankwidth_decomposition: explicit",
        "decomposition_width: 1",
        "table_entries:",
        "max_table_entries:",
        "signature_entries:",
        "max_signature_entries:",
        "join_pairs:",
        "join_signature_pairs:",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(f"rankwidth stats failed\n{stats.stdout}\n{stats.stderr}")

    fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        fourier_stats.returncode != 0
        or "rankwidth_mode: fourier" not in fourier_stats.stdout
        or "rankwidth_decomposition: linear" not in fourier_stats.stdout
    ):
        raise AssertionError(f"rankwidth Fourier stats failed\n{fourier_stats.stdout}\n{fourier_stats.stderr}")

    cut_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "min-fill-cut",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        cut_stats.returncode != 0
        or "rankwidth_decomposition: min-fill-cut" not in cut_stats.stdout
    ):
        raise AssertionError(f"rankwidth min-fill-cut stats failed\n{cut_stats.stdout}\n{cut_stats.stderr}")

    traced = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(decomposition),
            "--trace",
            "csv",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        traced.returncode != 0
        or "rankwidth.leaf" not in traced.stderr
        or "rankwidth.join_map" not in traced.stderr
        or "rankwidth.join" not in traced.stderr
    ):
        raise AssertionError(f"rankwidth trace failed\n{traced.stdout}\n{traced.stderr}")

    fourier_traced = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--trace",
            "csv",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        fourier_traced.returncode != 0
        or "rankwidth.fourier_leaf" not in fourier_traced.stderr
        or "rankwidth.fourier_join_map" not in fourier_traced.stderr
        or "rankwidth.fourier_join" not in fourier_traced.stderr
    ):
        raise AssertionError(f"rankwidth Fourier trace failed\n{fourier_traced.stdout}\n{fourier_traced.stderr}")

    malformed = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(qsop),
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if malformed.returncode == 0 or str(qsop) not in malformed.stderr or "expected header" not in malformed.stderr:
        raise AssertionError(f"rankwidth malformed decomposition diagnostic failed\n{malformed.stderr}")

    combined = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(decomposition),
            "--rankwidth-generate",
            "balanced",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if combined.returncode == 0 or "cannot be combined" not in combined.stderr:
        raise AssertionError(f"rankwidth accepted explicit and generated decompositions\n{combined.stderr}")

    with tempfile.TemporaryDirectory() as tmp:
        directory = pathlib.Path(tmp)
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "duplicate_var",
            "p rwdec 3 5 4\nl 0 0\nl 1 0\nl 2 2\nj 3 0 1\nj 4 3 2\n",
            "more than once",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "missing_var",
            "p rwdec 3 3 2\nl 0 0\nl 1 1\nj 2 0 1\n",
            "root does not cover every variable",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "cycle",
            "p rwdec 3 5 4\nl 0 0\nl 1 1\nl 2 2\nj 3 0 4\nj 4 3 2\n",
            "contains a cycle",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "overlap",
            "p rwdec 3 5 4\nl 0 0\nl 1 1\nl 2 2\nj 3 0 1\nj 4 3 0\n",
            "children are not disjoint",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "undefined",
            "p rwdec 3 5 4\nl 0 0\nl 1 1\nl 2 2\nj 4 3 2\n",
            "references undefined node",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "bad_child",
            "p rwdec 3 4 3\nl 0 0\nl 1 1\nl 2 2\nj 3 0 5\n",
            "references node outside range",
        )
        assert_bad_rankwidth_decomposition(
            exe,
            qsop,
            directory,
            "mismatch",
            "p rwdec 4 1 0\nl 0 0\n",
            "variable count does not match QSOP",
        )

    labelled = source_root / "tests" / "golden" / "solve_labelled.qsop"
    labelled_decomposition = source_root / "tests" / "golden" / "solve_labelled.rwdec"
    bad = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(labelled_decomposition),
            str(labelled),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad.returncode == 0 or "sign-only" not in bad.stderr:
        raise AssertionError(f"rankwidth accepted labelled QSOP\n{bad.stdout}\n{bad.stderr}")


def run_branch_heuristics(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_labelled.qsop"
    expected = subprocess.run(
        [str(exe), "--backend", "branch", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if expected.returncode != 0:
        raise AssertionError(f"default branch solve failed\n{expected.stderr}")

    for heuristic in ["treewidth", "linear-rankwidth"]:
        completed = subprocess.run(
            [str(exe), "--backend", "branch", "--branch-heuristic", heuristic, str(qsop)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0 or completed.stdout != expected.stdout:
            raise AssertionError(
                f"{heuristic} branch solve mismatch\n{completed.stdout}\n{completed.stderr}"
            )

        stats = subprocess.run(
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "branch",
                "--branch-heuristic",
                heuristic,
                str(qsop),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if stats.returncode != 0 or f"branch_heuristic: {heuristic}" not in stats.stdout:
            raise AssertionError(f"{heuristic} stats missing heuristic\n{stats.stdout}\n{stats.stderr}")


def run_trace_csv(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_labelled.qsop"
    expected_stats = source_root / "tests" / "golden" / "solve_branch.stats"
    completed = subprocess.run(
        [str(exe), "--format", "stats", "--backend", "branch", "--trace", "csv", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"trace run failed\n{completed.stderr}")
    if completed.stdout != expected_stats.read_text():
        raise AssertionError(f"trace changed stats output\n{completed.stdout}")

    lines = [line for line in completed.stderr.splitlines() if line]
    if not lines or lines[0] != "phase,depth,items,elapsed_ns":
        raise AssertionError(f"missing trace CSV header:\n{completed.stderr}")
    rows = [line.split(",") for line in lines[1:]]
    phases = {row[0] for row in rows}
    expected_phases = {
        "branch.cache_lookup",
        "branch.component_split",
        "branch.select_variable",
        "branch.edge_free_leaf",
    }
    if not expected_phases.issubset(phases):
        raise AssertionError(f"missing trace phases {expected_phases - phases}:\n{completed.stderr}")
    for row in rows:
        if len(row) != 4:
            raise AssertionError(f"bad trace row: {row}")
        int(row[1])
        int(row[2])
        int(row[3])


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_solve.py SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_solve(exe, source_root, "solve_single")
    run_solve(exe, source_root, "solve_labelled")
    run_solve(exe, source_root, "solve_disconnected")
    run_solve(exe, source_root, "solve_mirrored_path_components")
    run_max_vars_guard(exe, source_root)
    run_large_rankwidth_crt(exe)
    run_solver_stats(exe, source_root)
    run_cli_paths(exe, source_root)
    run_rankwidth_backend(exe, source_root)
    run_branch_heuristics(exe, source_root)
    run_trace_csv(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
