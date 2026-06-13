#!/usr/bin/env python3

import pathlib
import subprocess
import sys


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


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_solve.py SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_solve(exe, source_root, "solve_single")
    run_solve(exe, source_root, "solve_labelled")
    run_solve(exe, source_root, "solve_disconnected")
    run_max_vars_guard(exe, source_root)
    run_solver_stats(exe, source_root)
    run_cli_paths(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
