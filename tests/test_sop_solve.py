#!/usr/bin/env python3

import math
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

    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", str(qsop)],
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

    branch_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "fourier",
            "--format",
            "residue-vector",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch_fourier.returncode != 0:
        raise AssertionError(f"{name}: branch Fourier backend failed\n{branch_fourier.stderr}")
    if branch_fourier.stdout != expected_text:
        raise AssertionError(
            f"{name}: branch Fourier residue-vector mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{branch_fourier.stdout}\n"
        )

    for rw_source in ("native", "from-treewidth", "both", "auto"):
        branch_rw = subprocess.run(
            [
                str(exe),
                "--backend",
                "branch",
                "--branch-rw-source",
                rw_source,
                "--format",
                "residue-vector",
                str(qsop),
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if branch_rw.returncode != 0:
            raise AssertionError(
                f"{name}: branch --branch-rw-source {rw_source} failed\n{branch_rw.stderr}"
            )
        if branch_rw.stdout != expected_text:
            raise AssertionError(
                f"{name}: branch --branch-rw-source {rw_source} mismatch\n"
                f"expected:\n{expected_text}\nactual:\n{branch_rw.stdout}\n"
            )

    branch_fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--solve-mode",
            "fourier",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        branch_fourier_stats.returncode != 0
        or "solve_mode: fourier" not in branch_fourier_stats.stdout
        or "solve_mode_kernel: hybrid-fourier" not in branch_fourier_stats.stdout
    ):
        raise AssertionError(
            f"{name}: branch Fourier stats failed\n"
            f"{branch_fourier_stats.stdout}\n{branch_fourier_stats.stderr}"
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
    if "branch single-fourier solver refuses 1 variables" not in completed.stderr:
        raise AssertionError(f"unexpected diagnostic:\n{completed.stderr}")


def run_large_rankwidth_crt(exe: pathlib.Path) -> None:
    qsop = "p qsop-sign 16 64 0\nn 0\ncst 0\n"
    completed = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64", "--format", "residue-vector", "-"],
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

    rankwidth_stats = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--max-vars",
            "64",
            "--format",
            "stats",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        rankwidth_stats.returncode != 0
        or "rankwidth.count_table_factorized" not in rankwidth_stats.stderr
        or "join_pairs: 0" not in rankwidth_stats.stdout
        or "rankwidth_join_pair_forecast: 0" not in rankwidth_stats.stdout
        or "max_table_entries: 16" not in rankwidth_stats.stdout
    ):
        raise AssertionError(
            f"large rankwidth count-table factorized stats failed\n"
            f"{rankwidth_stats.stdout}\n{rankwidth_stats.stderr}"
        )

    default = subprocess.run(
        [str(exe), "--max-vars", "64", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if default.returncode != 0 or default.stdout != expected:
        raise AssertionError(f"large default CRT solve failed\n{default.stdout}\n{default.stderr}")

    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode != 0 or branch.stdout != expected:
        raise AssertionError(f"large branch CRT solve failed\n{branch.stdout}\n{branch.stderr}")

    treewidth = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if treewidth.returncode != 0 or treewidth.stdout != expected:
        raise AssertionError(f"large treewidth CRT solve failed\n{treewidth.stdout}\n{treewidth.stderr}")

    treewidth_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "treewidth",
            "--solve-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "residue-vector",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if treewidth_fourier.returncode != 0 or treewidth_fourier.stdout != expected:
        raise AssertionError(
            f"large treewidth Fourier CRT solve failed\n"
            f"{treewidth_fourier.stdout}\n{treewidth_fourier.stderr}"
        )

    signed_qsop = "p qsop-sign 8 64 1\nn 0\ncst 0\ne 0 1\n"
    signed = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    signed_expected = (
        "p qsop-result 8\n"
        "n 0\n"
        "counts 13835058055282163712 0 0 0 4611686018427387904 0 0 0\n"
    )
    if signed.returncode != 0 or signed.stdout != signed_expected:
        raise AssertionError(f"large signed rankwidth CRT solve failed\n{signed.stdout}\n{signed.stderr}")

    signed_materialized = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--max-vars",
            "64",
            "--rankwidth-join-strategy",
            "materialized",
            "--format",
            "residue-vector",
            "-",
        ],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if signed_materialized.returncode != 0 or signed_materialized.stdout != signed_expected:
        raise AssertionError(
            f"large signed rankwidth materialized CRT solve failed\n"
            f"{signed_materialized.stdout}\n{signed_materialized.stderr}"
        )

    signed_branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if signed_branch.returncode != 0 or signed_branch.stdout != signed_expected:
        raise AssertionError(
            f"large signed branch CRT solve failed\n{signed_branch.stdout}\n"
            f"{signed_branch.stderr}"
        )

    signed_treewidth_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "treewidth",
            "--solve-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "residue-vector",
            "-",
        ],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        signed_treewidth_fourier.returncode != 0
        or signed_treewidth_fourier.stdout != signed_expected
    ):
        raise AssertionError(
            f"large signed treewidth Fourier CRT solve failed\n"
            f"{signed_treewidth_fourier.stdout}\n{signed_treewidth_fourier.stderr}"
        )

    rankwidth_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "residue-vector",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rankwidth_fourier.returncode != 0 or rankwidth_fourier.stdout != expected:
        raise AssertionError(
            f"large rankwidth Fourier CRT solve failed\n"
            f"{rankwidth_fourier.stdout}\n{rankwidth_fourier.stderr}"
        )

    rankwidth_fourier_stats = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "stats",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        rankwidth_fourier_stats.returncode != 0
        or "rankwidth.fourier_factorized" not in rankwidth_fourier_stats.stderr
        or "join_pairs: 0" not in rankwidth_fourier_stats.stdout
        or "rankwidth_join_pair_forecast: 0" not in rankwidth_fourier_stats.stdout
        or "max_table_entries: 16" not in rankwidth_fourier_stats.stdout
    ):
        raise AssertionError(
            f"large rankwidth Fourier factorized stats failed\n"
            f"{rankwidth_fourier_stats.stdout}\n{rankwidth_fourier_stats.stderr}"
        )

    signed_rankwidth_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "residue-vector",
            "-",
        ],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        signed_rankwidth_fourier.returncode != 0
        or signed_rankwidth_fourier.stdout != signed_expected
    ):
        raise AssertionError(
            f"large signed rankwidth Fourier CRT solve failed\n"
            f"{signed_rankwidth_fourier.stdout}\n{signed_rankwidth_fourier.stderr}"
        )


def run_auto_amplitude_handles_large_count_strings(exe: pathlib.Path) -> None:
    qsop = "p qsop-sign 8 70 0\nn 70\ncst 0\nu 0 4\n"
    completed = subprocess.run(
        [str(exe), "--format", "amplitude", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected = {
        "solve_mode: auto",
        "solve_mode_kernel: count-table",
        "amplitude_re: 0",
        "amplitude_im: 0",
    }
    if completed.returncode != 0 or not expected.issubset(set(completed.stdout.splitlines())):
        raise AssertionError(
            f"auto amplitude large-count conversion failed\n{completed.stdout}\n{completed.stderr}"
        )


def run_branch_dp_handoff(exe: pathlib.Path) -> None:
    left_edges = [f"e {i} {i + 1}" for i in range(31)]
    right_edges = [f"e {i} {i + 1}" for i in range(32, 63)]
    edges = "\n".join(left_edges + right_edges)
    qsop = f"p qsop-sign 16 64 62\nn 0\ncst 3\n{edges}\n"
    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    treewidth = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode != 0 or treewidth.returncode != 0 or branch.stdout != treewidth.stdout:
        raise AssertionError(
            f"branch DP handoff mismatch\nbranch:\n{branch.stdout}\n{branch.stderr}\n"
            f"treewidth:\n{treewidth.stdout}\n{treewidth.stderr}"
        )

    stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--max-vars",
            "64",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_stats = {
        "backend: branch",
        "solve_mode_kernel: single-fourier",
        "treewidth_delegations: 1",
        "rankwidth_delegations: 0",
        "decomposition_width: 1",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(f"branch DP handoff stats failed\n{stats.stdout}\n{stats.stderr}")
    trace_phases = {line.split(",", 1)[0] for line in stats.stderr.splitlines()[1:] if line}
    # --single-mode-precision auto resolves to the f64 tables: they are the only precision with
    # SIMD kernels, and long double is opt-in now that the DP carries a binary exponent for range.
    expected_trace = {
        "treewidth.single_mode_initial_factors_f64",
        "treewidth.single_mode_multiply_f64",
        "treewidth.single_mode_sum_out_f64",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch DP handoff trace missing {expected_trace - trace_phases}\n{stats.stderr}"
        )


def run_branch_root_treewidth_trace(exe: pathlib.Path) -> None:
    edges = "\n".join(f"e {i} {i + 1}" for i in range(31))
    qsop = f"p qsop-sign 16 32 31\nn 0\ncst 3\n{edges}\n"
    stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--max-vars",
            "32",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_stats = {
        "backend: branch",
        "solve_mode_kernel: single-fourier",
        "treewidth_delegations: 1",
        "rankwidth_delegations: 0",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(f"branch root treewidth stats failed\n{stats.stdout}\n{stats.stderr}")
    trace_phases = {line.split(",", 1)[0] for line in stats.stderr.splitlines()[1:] if line}
    # --single-mode-precision auto resolves to the f64 tables: they are the only precision with
    # SIMD kernels, and long double is opt-in now that the DP carries a binary exponent for range.
    expected_trace = {
        "treewidth.single_mode_initial_factors_f64",
        "treewidth.single_mode_multiply_f64",
        "treewidth.single_mode_sum_out_f64",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch root treewidth trace missing {expected_trace - trace_phases}\n{stats.stderr}"
        )

    fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--max-vars",
            "32",
            "--solve-mode",
            "fourier",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_fourier_stats = {
        "backend: branch",
        "solve_mode: fourier",
        "solve_mode_kernel: hybrid-fourier",
        "treewidth_delegations: 1",
    }
    if fourier_stats.returncode != 0 or not all(
        part in fourier_stats.stdout for part in expected_fourier_stats
    ):
        raise AssertionError(
            f"branch root treewidth Fourier stats failed\n"
            f"{fourier_stats.stdout}\n{fourier_stats.stderr}"
        )
    fourier_trace_phases = {
        line.split(",", 1)[0] for line in fourier_stats.stderr.splitlines()[1:] if line
    }
    expected_fourier_trace = {
        "branch.root_treewidth_delegate",
        "treewidth.fourier_initial_factors",
        "treewidth.fourier_multiply",
        "treewidth.fourier_sum_out",
    }
    if not expected_fourier_trace.issubset(fourier_trace_phases):
        raise AssertionError(
            f"branch root treewidth Fourier trace missing "
            f"{expected_fourier_trace - fourier_trace_phases}\n{fourier_stats.stderr}"
        )


def run_branch_rankwidth_handoff(exe: pathlib.Path) -> None:
    edges = "\n".join(f"e {u} {v}" for u in range(20) for v in range(20, 40))
    qsop = f"p qsop-sign 16 40 400\nn 0\ncst 5\n{edges}\n"
    branch = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--branch-rw-source",
            "auto",
            "--max-vars",
            "40",
            "--format",
            "residue-vector",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    rankwidth = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "40", "--format", "residue-vector", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode != 0 or rankwidth.returncode != 0 or branch.stdout != rankwidth.stdout:
        raise AssertionError(
            f"branch rankwidth handoff mismatch\nbranch:\n{branch.stdout}\n{branch.stderr}\n"
            f"rankwidth:\n{rankwidth.stdout}\n{rankwidth.stderr}"
        )

    stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--branch-rw-source",
            "auto",
            "--max-vars",
            "40",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_stats = {
        "backend: branch",
        "solve_mode_kernel: single-fourier",
        "treewidth_delegations: 0",
        "rankwidth_delegations: 1",
        "decomposition_width: 1",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(
            f"branch rankwidth handoff stats failed\n{stats.stdout}\n{stats.stderr}"
        )
    trace_phases = {line.split(",", 1)[0] for line in stats.stderr.splitlines()[1:] if line}
    expected_trace = {
        "branch.single.component_split",
        "rankwidth.width_probe",
        "rankwidth.cutrank_width_probe",
        "rankwidth.table_forecast",
        "rankwidth.join_pair_forecast",
        "rankwidth.single_mode_join_f64",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch rankwidth handoff trace missing {expected_trace - trace_phases}\n"
            f"{stats.stderr}"
        )

    fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--branch-rw-source",
            "auto",
            "--max-vars",
            "40",
            "--solve-mode",
            "fourier",
            "--trace",
            "csv",
            "-",
        ],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_fourier_stats = {
        "backend: branch",
        "solve_mode: fourier",
        "solve_mode_kernel: hybrid-fourier",
        "rankwidth_delegations: 1",
    }
    if fourier_stats.returncode != 0 or not all(
        part in fourier_stats.stdout for part in expected_fourier_stats
    ):
        raise AssertionError(
            f"branch rankwidth Fourier handoff stats failed\n"
            f"{fourier_stats.stdout}\n{fourier_stats.stderr}"
        )
    fourier_trace_phases = {
        line.split(",", 1)[0] for line in fourier_stats.stderr.splitlines()[1:] if line
    }
    expected_fourier_trace = {
        "branch.rankwidth_delegate",
        "rankwidth.fourier_leaf",
        "rankwidth.fourier_join_map",
        "rankwidth.fourier_join",
        "rankwidth.fourier_even_closed_form",
    }
    if not expected_fourier_trace.issubset(fourier_trace_phases):
        raise AssertionError(
            f"branch rankwidth Fourier handoff trace missing "
            f"{expected_fourier_trace - fourier_trace_phases}\n{fourier_stats.stderr}"
        )

    signed_edges = "\n".join(f"e {u} {v}" for u in range(16) for v in range(16, 32))
    signed_qsop = f"p qsop-sign 8 32 256\nn 0\ncst 5\n{signed_edges}\n"
    signed_fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--branch-rw-source",
            "auto",
            "--max-vars",
            "32",
            "--solve-mode",
            "fourier",
            "--trace",
            "csv",
            "-",
        ],
        input=signed_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_signed_stats = {
        "backend: branch",
        "solve_mode: fourier",
        "rankwidth_delegations: 1",
        "rankwidth_cutrank_width: 1",
    }
    if signed_fourier_stats.returncode != 0 or not all(
        part in signed_fourier_stats.stdout for part in expected_signed_stats
    ):
        raise AssertionError(
            f"branch signed rankwidth Fourier handoff stats failed\n"
            f"{signed_fourier_stats.stdout}\n{signed_fourier_stats.stderr}"
        )
    signed_trace_phases = {
        line.split(",", 1)[0]
        for line in signed_fourier_stats.stderr.splitlines()[1:]
        if line
    }
    expected_signed_trace = {
        "branch.rankwidth_delegate",
        "rankwidth.fourier_leaf",
        "rankwidth.fourier_join_map",
        "rankwidth.fourier_join",
        "rankwidth.fourier_even_closed_form",
    }
    if not expected_signed_trace.issubset(signed_trace_phases):
        raise AssertionError(
            f"branch signed rankwidth Fourier trace missing "
            f"{expected_signed_trace - signed_trace_phases}\n"
            f"{signed_fourier_stats.stderr}"
        )


def run_solver_stats(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    cases = [
        (
            [
                str(exe),
                "--format",
                "stats",
                "--backend",
                "branch",
                "--solve-mode",
                "count-table",
                str(source_root / "tests" / "golden" / "solve_signed_edge.qsop"),
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
                "--solve-mode",
                "count-table",
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
                "--solve-mode",
                "count-table",
                str(source_root / "tests" / "golden" / "solve_branch_cache.qsop"),
            ],
            source_root / "tests" / "golden" / "solve_branch_cache.stats",
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


def run_include_result_stats(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_signed_edge.qsop"
    completed = subprocess.run(
        [str(exe), "--format", "stats", "--include-result", "--include-probability", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_result = {
        "result_modulus: 8",
        "result_norm_h: 4",
        "result_counts: 0 0 1 0 2 0 1 0",
    }
    if completed.returncode != 0 or not expected_result.issubset(set(completed.stdout.splitlines())):
        raise AssertionError(f"include-result stats failed\n{completed.stdout}\n{completed.stderr}")
    probability_lines = [
        line for line in completed.stdout.splitlines() if line.startswith("result_probability: ")
    ]
    if len(probability_lines) != 1:
        raise AssertionError(f"missing probability line\n{completed.stdout}\n{completed.stderr}")
    probability = float(probability_lines[0].split(": ", 1)[1])
    if abs(probability - 0.25) > 1e-15:
        raise AssertionError(f"unexpected probability {probability}\n{completed.stdout}")

    invalid = subprocess.run(
        [str(exe), "--include-result", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if invalid.returncode == 0 or "--include-result requires --format stats" not in invalid.stderr:
        raise AssertionError(f"include-result guard failed\n{invalid.stdout}\n{invalid.stderr}")

    invalid_probability = subprocess.run(
        [str(exe), "--include-probability", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        invalid_probability.returncode == 0
        or "--include-probability requires --format stats" not in invalid_probability.stderr
    ):
        raise AssertionError(
            f"include-probability guard failed\n{invalid_probability.stdout}\n{invalid_probability.stderr}"
        )


def parse_solver_stats(text: str) -> dict[str, int | str]:
    stats: dict[str, int | str] = {}
    for line in text.splitlines():
        key, value = line.split(": ", 1)
        try:
            stats[key] = int(value)
        except ValueError:
            stats[key] = value
    return stats


def run_branch_component_cache(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    for name in (
        "solve_repeated_components",
        "solve_mirrored_components",
        "solve_mirrored_path_components",
    ):
        qsop = source_root / "tests" / "golden" / f"{name}.qsop"
        completed = subprocess.run(
            [str(exe), "--format", "stats", "--backend", "branch", str(qsop)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"{name}: branch cache stats failed\n{completed.stderr}")
        stats = parse_solver_stats(completed.stdout)
        if stats["cache_hits"] < 1:
            raise AssertionError(f"{name}: expected a shared branch component cache hit")
        if stats["cache_hits"] + stats["cache_misses"] != stats["search_nodes"]:
            raise AssertionError(f"{name}: branch cache hits + misses do not match search nodes")
        if stats.get("cache_canonical_hits", 0) < 1:
            raise AssertionError(f"{name}: expected a canonical branch cache hit")
        if stats.get("cache_canonical_lookups", 0) < stats.get("cache_canonical_hits", 0):
            raise AssertionError(f"{name}: canonical lookups should cover canonical hits")
        if stats.get("cache_canonical_stores", 0) < 1:
            raise AssertionError(f"{name}: expected canonical branch cache stores")
        if stats.get("cache_canonical_entries", 0) < 1:
            raise AssertionError(f"{name}: expected canonical branch cache entries")
        if stats.get("cache_estimated_bytes", 0) <= 0:
            raise AssertionError(f"{name}: expected branch cache byte accounting")
        if stats.get("cache_estimated_bytes", 0) < stats.get("cache_count_bytes", 0):
            raise AssertionError(f"{name}: cache estimate is smaller than count storage")

    repeated_triangle = (
        "p qsop-sign 8 6 6\n"
        "n 0\n"
        "cst 0\n"
        "e 0 1\n"
        "e 1 2\n"
        "e 0 2\n"
        "e 3 4\n"
        "e 4 5\n"
        "e 3 5\n"
    )
    completed = subprocess.run(
        [str(exe), "--format", "stats", "--backend", "branch", "-"],
        input=repeated_triangle,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"repeated triangle branch cache stats failed\n{completed.stderr}")
    stats = parse_solver_stats(completed.stdout)
    if stats.get("cache_avoided_nodes", 0) < 1:
        raise AssertionError("repeated triangle: expected branch cache to report avoided nodes")
    if stats.get("cache_key_bytes", 0) <= 0 or stats.get("cache_count_bytes", 0) <= 0:
        raise AssertionError("repeated triangle: expected branch cache key/count bytes")


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

    version_result = subprocess.run(
        [str(exe), "--version"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if version_result.returncode != 0 or version_result.stdout != "sop-solve 0.4\n":
        raise AssertionError(
            f"unexpected --version result:\n{version_result.stdout}\n{version_result.stderr}"
        )

    stdin_result = subprocess.run(
        [str(exe), "--format", "residue-vector", "-"],
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
        ([str(exe), "--backend", "bad", str(qsop)], "unsupported backend"),
        ([str(exe), "--backend", "branch", "--max-vars", "0", str(qsop)], "branch single-fourier solver refuses"),
        ([str(exe), "--rankwidth-decomposition"], "requires a path"),
        (
            [str(exe), "--rankwidth-decomposition", str(qsop), str(qsop)],
            "requires --backend rankwidth",
        ),
        ([str(exe), "--rankwidth-generate"], "requires a value"),
        ([str(exe), "--rankwidth-generate", "bad", str(qsop)], "unsupported rankwidth generator"),
        ([str(exe), "--rankwidth-generate", "left-deep", str(qsop)], "requires --backend rankwidth"),
        ([str(exe), "--backend", "rankwidth", "--rankwidth-generate", "path", str(qsop)], "unsupported rankwidth generator"),
        ([str(exe), "--rankwidth-mode"], "requires a value"),
        ([str(exe), "--rankwidth-mode", "bad", str(qsop)], "unsupported rankwidth mode"),
        ([str(exe), "--rankwidth-mode", "fourier", str(qsop)], "requires --backend rankwidth"),
        ([str(exe), "--solve-mode"], "requires a value"),
        ([str(exe), "--solve-mode", "bad", str(qsop)], "unsupported solve mode"),
        (
            [
                str(exe),
                "--backend",
                "rankwidth",
                "--solve-mode",
                "fourier",
                "--rankwidth-mode",
                "count-table",
                str(qsop),
            ],
            "conflicts with --rankwidth-mode",
        ),
        ([str(exe), "--treewidth-order"], "requires a value"),
        ([str(exe), "--treewidth-order", "bad", str(qsop)], "unsupported treewidth order"),
        ([str(exe), "--treewidth-order", "min-degree", str(qsop)], "requires --backend treewidth"),
        ([str(exe), "--branch-heuristic"], "requires a value"),
        ([str(exe), "--branch-heuristic", "rankwidth", str(qsop)], "unsupported branch heuristic"),
        ([str(exe), "--branch-heuristic", "cutrank", str(qsop)], "unsupported branch heuristic"),
        (
            [str(exe), "--backend", "treewidth", "--branch-heuristic", "treewidth", str(qsop)],
            "requires --backend branch",
        ),
        ([str(exe), "--trace"], "requires a value"),
        ([str(exe), "--trace", "json", str(qsop)], "unsupported trace format"),
        ([str(exe), "--max-vars"], "requires a non-negative"),
        ([str(exe), "--max-vars", "-1", str(qsop)], "must be a non-negative"),
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
        [str(exe), "--backend", "rankwidth", "--format", "residue-vector", *extra_args, str(qsop)],
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
        [str(exe), "--backend", "branch", "--format", "residue-vector", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if expected.returncode != 0:
        raise AssertionError(f"branch oracle solve failed\n{expected.stderr}")

    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-decomposition",
        str(decomposition),
    )
    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-decomposition",
        str(decomposition),
        "--rankwidth-join-strategy",
        "materialized",
    )
    assert_rankwidth_matches(
        exe,
        qsop,
        expected.stdout,
        "--rankwidth-decomposition",
        str(decomposition),
        "--rankwidth-join-strategy",
        "streaming",
    )
    assert_rankwidth_matches(exe, qsop, expected.stdout)
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "balanced")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "min-fill")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "min-fill-cut")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-generate", "from-treewidth")
    assert_rankwidth_matches(exe, qsop, expected.stdout, "--rankwidth-mode", "fourier")

    disconnected = """p qsop-sign 8 4 0
n 4
cst 0
u 0 1
u 1 2
u 2 3
u 3 4
"""
    with tempfile.TemporaryDirectory() as tmp:
        disconnected_qsop = pathlib.Path(tmp) / "disconnected.qsop"
        disconnected_qsop.write_text(disconnected)
        disconnected_expected = subprocess.run(
            [str(exe), "--backend", "branch", "--format", "residue-vector", str(disconnected_qsop)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if disconnected_expected.returncode != 0:
            raise AssertionError(f"branch disconnected solve failed\n{disconnected_expected.stderr}")
        assert_rankwidth_matches(
            exe,
            disconnected_qsop,
            disconnected_expected.stdout,
            "--rankwidth-generate",
            "from-treewidth",
        )

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
        "rankwidth_cutrank_width:",
        "table_entries:",
        "max_table_entries:",
        "signature_entries:",
        "max_signature_entries:",
        "join_pairs:",
        "join_signature_pairs:",
        "rankwidth_table_forecast:",
        "rankwidth_join_pair_forecast:",
        "rankwidth_linear_transition_events:",
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
        or "rankwidth_decomposition: left-deep" not in fourier_stats.stdout
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

    zero_var_qsop = "p qsop-sign 8 0 0\nn 4\ncst 3\n"
    zero_var_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--include-result",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "min-fill-cut",
            "-",
        ],
        input=zero_var_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_zero_var_stats = {
        "backend: rankwidth",
        "rankwidth_mode: count-table",
        "rankwidth_decomposition: min-fill-cut",
        "decomposition_width: 0",
        "rankwidth_cutrank_width: 0",
        "table_entries: 1",
        "max_table_entries: 1",
        "signature_entries: 1",
        "max_signature_entries: 1",
        "join_pairs: 0",
        "join_signature_pairs: 0",
        "rankwidth_table_forecast: 1",
        "rankwidth_join_pair_forecast: 0",
        "result_counts: 0 0 0 1 0 0 0 0",
    }
    if zero_var_stats.returncode != 0 or not all(
        part in zero_var_stats.stdout for part in expected_zero_var_stats
    ):
        raise AssertionError(
            f"zero-variable rankwidth stats failed\n{zero_var_stats.stdout}\n{zero_var_stats.stderr}"
        )

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
        or "rankwidth.linear_dp" not in traced.stderr
        or "rankwidth_linear_transition_events:" not in traced.stdout
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
        or "rankwidth.fourier_even_closed_form" not in fourier_traced.stderr
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

    signed = source_root / "tests" / "golden" / "solve_signed_edge.qsop"
    signed_decomposition = source_root / "tests" / "golden" / "solve_signed_edge.rwdec"
    signed_expected = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", str(signed)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if signed_expected.returncode != 0:
        raise AssertionError(f"signed branch oracle solve failed\n{signed_expected.stderr}")
    assert_rankwidth_matches(
        exe,
        signed,
        signed_expected.stdout,
        "--rankwidth-decomposition",
        str(signed_decomposition),
    )
    assert_rankwidth_matches(
        exe,
        signed,
        signed_expected.stdout,
        "--rankwidth-decomposition",
        str(signed_decomposition),
        "--rankwidth-join-strategy",
        "materialized",
    )
    assert_rankwidth_matches(
        exe,
        signed,
        signed_expected.stdout,
        "--rankwidth-decomposition",
        str(signed_decomposition),
        "--rankwidth-join-strategy",
        "streaming",
    )
    assert_rankwidth_matches(
        exe,
        signed,
        signed_expected.stdout,
        "--rankwidth-mode",
        "fourier",
        "--rankwidth-decomposition",
        str(signed_decomposition),
    )
    assert_rankwidth_matches(exe, signed, signed_expected.stdout)
    assert_rankwidth_matches(exe, signed, signed_expected.stdout, "--rankwidth-generate", "balanced")
    assert_rankwidth_matches(exe, signed, signed_expected.stdout, "--rankwidth-generate", "min-fill-cut")

    signed_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(signed_decomposition),
            str(signed),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        signed_stats.returncode != 0
        or "rankwidth_mode: count-table" not in signed_stats.stdout
        or "decomposition_width: 1" not in signed_stats.stdout
        or "rankwidth_cutrank_width: 1" not in signed_stats.stdout
        or "rankwidth_linear_transition_events: 6" not in signed_stats.stdout
    ):
        raise AssertionError(f"signed rankwidth stats failed\n{signed_stats.stdout}\n{signed_stats.stderr}")

    three_signature_qsop = "p qsop-sign 4 3 2\nn 0\ncst 0\ne 0 2\ne 1 2\n"
    three_signature_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            "-",
        ],
        input=three_signature_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if three_signature_stats.returncode != 0:
        raise AssertionError(
            f"three-signature rankwidth stats failed\n"
            f"{three_signature_stats.stdout}\n{three_signature_stats.stderr}"
        )
    three_stats = parse_solver_stats(three_signature_stats.stdout)
    expected_three_stats = {
        "rankwidth_cutrank_width": 1,
        "max_table_entries": 2,
        "rankwidth_table_forecast": 8,
        "rankwidth_join_pair_forecast": 8,
    }
    for key, value in expected_three_stats.items():
        if three_stats.get(key) != value:
            raise AssertionError(
                f"three-signature forecast mismatch for {key}: expected {value}, got "
                f"{three_stats.get(key)}\n{three_signature_stats.stdout}"
            )

    signed_left_deep_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            str(signed),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    signed_min_fill_cut_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "min-fill-cut",
            str(signed),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if signed_left_deep_stats.returncode != 0 or signed_min_fill_cut_stats.returncode != 0:
        raise AssertionError(
            f"signed generated rankwidth stats failed\n"
            f"left-deep:\n{signed_left_deep_stats.stdout}\n{signed_left_deep_stats.stderr}\n"
            f"min-fill-cut:\n{signed_min_fill_cut_stats.stdout}\n{signed_min_fill_cut_stats.stderr}"
        )
    left_stats = parse_solver_stats(signed_left_deep_stats.stdout)
    cut_stats = parse_solver_stats(signed_min_fill_cut_stats.stdout)
    for key in (
        "decomposition_width",
        "table_entries",
        "max_table_entries",
        "signature_entries",
        "max_signature_entries",
        "join_pairs",
        "join_signature_pairs",
        "rankwidth_table_forecast",
        "rankwidth_join_pair_forecast",
    ):
        if left_stats[key] != cut_stats[key]:
            raise AssertionError(
                f"signed min-fill-cut should use the left-deep fallback for {key}\n"
                f"left-deep:\n{signed_left_deep_stats.stdout}\n"
                f"min-fill-cut:\n{signed_min_fill_cut_stats.stdout}"
            )

    signed_trace = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(signed_decomposition),
            "--trace",
            "csv",
            str(signed),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        signed_trace.returncode != 0
        or "rankwidth.linear_dp" not in signed_trace.stderr
        or "rankwidth_linear_transition_events: 6" not in signed_trace.stdout
    ):
        raise AssertionError(f"signed rankwidth trace failed\n{signed_trace.stdout}\n{signed_trace.stderr}")

    high_signature_linear = """p qsop-sign 8 14 21
n 0
cst 0
e 0 4
e 0 8
e 0 13
e 1 4
e 1 9
e 2 3
e 2 10
e 2 13
e 3 8
e 3 11
e 3 12
e 4 5
e 4 7
e 4 11
e 4 12
e 5 7
e 6 13
e 7 8
e 7 9
e 7 13
e 10 11
"""
    high_sig_expected = subprocess.run(
        [str(exe), "--backend", "branch", "--format", "residue-vector", "--max-vars", "16", "-"],
        input=high_signature_linear,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    high_sig_rankwidth = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            "--max-vars",
            "32",
            "--format",
            "residue-vector",
            "-",
        ],
        input=high_signature_linear,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        high_sig_expected.returncode != 0
        or high_sig_rankwidth.returncode != 0
        or high_sig_rankwidth.stdout != high_sig_expected.stdout
    ):
        raise AssertionError(
            f"high-signature linear rankwidth mismatch\n"
            f"rankwidth:\n{high_sig_rankwidth.stdout}\n{high_sig_rankwidth.stderr}\n"
            f"branch oracle:\n{high_sig_expected.stdout}\n{high_sig_expected.stderr}"
        )
    high_sig_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            "--max-vars",
            "32",
            "-",
        ],
        input=high_signature_linear,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    parsed_high_sig = parse_solver_stats(high_sig_stats.stdout)
    if (
        high_sig_stats.returncode != 0
        or parsed_high_sig.get("max_signature_entries", 0) <= 8
        or parsed_high_sig.get("rankwidth_linear_transition_events", 0) == 0
    ):
        raise AssertionError(
            f"high-signature linear rankwidth stats failed\n"
            f"{high_sig_stats.stdout}\n{high_sig_stats.stderr}"
        )

    path70 = "p qsop-sign 8 70 69\nn 0\ncst 0\n" + "".join(
        f"e {i} {i + 1}\n" for i in range(69)
    )
    path70_rankwidth = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            "--max-vars",
            "128",
            "-",
        ],
        input=path70,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    path70_treewidth = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "128", "-"],
        input=path70,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        path70_rankwidth.returncode != 0
        or path70_treewidth.returncode != 0
        or path70_rankwidth.stdout != path70_treewidth.stdout
    ):
        raise AssertionError(
            f"linear rankwidth CRT path mismatch\n"
            f"rankwidth:\n{path70_rankwidth.stdout}\n{path70_rankwidth.stderr}\n"
            f"treewidth:\n{path70_treewidth.stdout}\n{path70_treewidth.stderr}"
        )

    signed_fourier_trace = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--rankwidth-decomposition",
            str(signed_decomposition),
            "--trace",
            "csv",
            str(signed),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        signed_fourier_trace.returncode != 0
        or "rankwidth.fourier_leaf" not in signed_fourier_trace.stderr
        or "rankwidth.fourier_join_map" not in signed_fourier_trace.stderr
        or "rankwidth.fourier_join" not in signed_fourier_trace.stderr
        or "rankwidth.fourier_even_closed_form" not in signed_fourier_trace.stderr
    ):
        raise AssertionError(
            f"signed rankwidth Fourier trace failed\n"
            f"{signed_fourier_trace.stdout}\n{signed_fourier_trace.stderr}"
        )


def run_branch_heuristics(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_signed_edge.qsop"
    expected = subprocess.run(
        [str(exe), "--backend", "branch", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if expected.returncode != 0:
        raise AssertionError(f"default branch solve failed\n{expected.stderr}")

    for heuristic, reported in [
        ("delegation-depth", "delegation-depth"),
        ("treewidth", "treewidth"),
        ("cutrank-proxy", "cutrank-proxy"),
    ]:
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
        if stats.returncode != 0 or f"branch_heuristic: {reported}" not in stats.stdout:
            raise AssertionError(f"{heuristic} stats missing heuristic\n{stats.stdout}\n{stats.stderr}")


def run_treewidth_backend(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    for name in ["solve_single", "solve_signed_edge", "solve_disconnected"]:
        qsop = source_root / "tests" / "golden" / f"{name}.qsop"
        expected = subprocess.run(
            [str(exe), "--backend", "branch", "--format", "residue-vector", str(qsop)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if expected.returncode != 0:
            raise AssertionError(f"{name}: branch oracle solve failed\n{expected.stderr}")

        for order in ["min-fill", "min-degree", "min-fill-max-degree"]:
            completed = subprocess.run(
                [
                    str(exe),
                    "--backend",
                    "treewidth",
                    "--treewidth-order",
                    order,
                    "--format",
                    "residue-vector",
                    str(qsop),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if completed.returncode != 0 or completed.stdout != expected.stdout:
                raise AssertionError(
                    f"{name}: treewidth {order} solve mismatch\n{completed.stdout}\n"
                    f"{completed.stderr}"
                )
            fourier_completed = subprocess.run(
                [
                    str(exe),
                    "--backend",
                    "treewidth",
                    "--treewidth-order",
                    order,
                    "--solve-mode",
                    "fourier",
                    "--format",
                    "residue-vector",
                    str(qsop),
                ],
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            if fourier_completed.returncode != 0 or fourier_completed.stdout != expected.stdout:
                raise AssertionError(
                    f"{name}: treewidth Fourier {order} solve mismatch\n"
                    f"{fourier_completed.stdout}\n{fourier_completed.stderr}"
                )

    qsop = source_root / "tests" / "golden" / "solve_signed_edge.qsop"
    stats = subprocess.run(
        [str(exe), "--format", "stats", "--backend", "treewidth", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        stats.returncode != 0
        or "backend: treewidth" not in stats.stdout
        or "treewidth_order: min-fill" not in stats.stdout
        or "decomposition_width:" not in stats.stdout
        or "max_table_entries:" not in stats.stdout
    ):
        raise AssertionError(f"treewidth stats failed\n{stats.stdout}\n{stats.stderr}")

    fourier_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "treewidth",
            "--solve-mode",
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
        or "solve_mode: fourier" not in fourier_stats.stdout
        or "solve_mode_kernel: fourier" not in fourier_stats.stdout
        or "treewidth_order: min-fill" not in fourier_stats.stdout
    ):
        raise AssertionError(
            f"treewidth Fourier stats failed\n"
            f"{fourier_stats.stdout}\n{fourier_stats.stderr}"
        )

    min_degree_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "treewidth",
            "--treewidth-order",
            "min-degree",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if min_degree_stats.returncode != 0 or "treewidth_order: min-degree" not in min_degree_stats.stdout:
        raise AssertionError(
            f"treewidth min-degree stats failed\n{min_degree_stats.stdout}\n{min_degree_stats.stderr}"
        )

    max_degree_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "treewidth",
            "--treewidth-order",
            "min-fill-max-degree",
            str(qsop),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        max_degree_stats.returncode != 0
        or "treewidth_order: min-fill-max-degree" not in max_degree_stats.stdout
    ):
        raise AssertionError(
            f"treewidth min-fill-max-degree stats failed\n"
            f"{max_degree_stats.stdout}\n{max_degree_stats.stderr}"
        )

    guarded = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "1", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if guarded.returncode == 0 or "treewidth backend refuses" not in guarded.stderr:
        raise AssertionError(
            f"treewidth max-vars guard did not trigger\n{guarded.stdout}\n{guarded.stderr}"
        )

    traced = subprocess.run(
        [str(exe), "--format", "stats", "--backend", "treewidth", "--trace", "csv", str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_phases = {
        "treewidth.initial_factors",
        "treewidth.min_fill_order",
        "treewidth.multiply",
        "treewidth.sum_out",
    }
    if traced.returncode != 0 or "backend: treewidth" not in traced.stdout:
        raise AssertionError(f"treewidth trace run failed\n{traced.stdout}\n{traced.stderr}")
    lines = [line for line in traced.stderr.splitlines() if line]
    if not lines or lines[0] != "phase,depth,items,elapsed_ns":
        raise AssertionError(f"treewidth trace missing CSV header\n{traced.stderr}")
    phases = {line.split(",", 1)[0] for line in lines[1:]}
    if not expected_phases.issubset(phases):
        raise AssertionError(f"treewidth trace missing phases {expected_phases - phases}\n{traced.stderr}")

def run_trace_csv(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_signed_edge.qsop"
    expected_stats = source_root / "tests" / "golden" / "solve_branch.stats"
    completed = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--solve-mode",
            "count-table",
            "--trace",
            "csv",
            str(qsop),
        ],
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
        "branch.component_split",
        "branch.select_variable",
        "branch.edge_free_leaf",
    }
    if not expected_phases.issubset(phases):
        raise AssertionError(f"missing trace phases {expected_phases - phases}:\n{completed.stderr}")
    if not ({"branch.cache_lookup", "branch.cache_canonical_lookup"} & phases):
        raise AssertionError(f"missing cache lookup trace phase:\n{completed.stderr}")
    if not ({"branch.cache_store", "branch.cache_canonical_store"} & phases):
        raise AssertionError(f"missing cache store trace phase:\n{completed.stderr}")
    for row in rows:
        if len(row) != 4:
            raise AssertionError(f"bad trace row: {row}")
        int(row[1])
        int(row[2])
        int(row[3])

    disconnected = source_root / "tests" / "golden" / "solve_disconnected.qsop"
    fourier = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "branch",
            "--solve-mode",
            "fourier",
            "--trace",
            "csv",
            str(disconnected),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        fourier.returncode != 0
        or "solve_mode_kernel: hybrid-fourier" not in fourier.stdout
        or "branch.fourier_multiply" not in fourier.stderr
    ):
        raise AssertionError(
            f"branch Fourier component trace missing\n{fourier.stdout}\n{fourier.stderr}"
        )


def _path_qsop(nvars: int, r: int) -> str:
    edges = "\n".join(f"e {i} {i + 1}" for i in range(nvars - 1))
    return f"p qsop-sign {r} {nvars} {nvars - 1}\nn 0\ncst 0\n{edges}\n"


def _amplitude_fields(output: str) -> tuple[str | None, str | None]:
    values: dict[str, str] = {}
    for line in output.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            values[key.strip()] = value.strip()
    return values.get("amplitude_re"), values.get("amplitude_im")


def _amplitudes_close(left: str, right: str, *, rel: float = 1e-9, abs_tol: float = 1e-6) -> bool:
    left_re, left_im = _amplitude_fields(left)
    right_re, right_im = _amplitude_fields(right)
    if left_re is None or left_im is None or right_re is None or right_im is None:
        return False
    left_value = (float(left_re), float(left_im))
    right_value = (float(right_re), float(right_im))
    error = math.hypot(left_value[0] - right_value[0], left_value[1] - right_value[1])
    scale = max(1.0, math.hypot(*left_value), math.hypot(*right_value))
    return (
        error <= abs_tol + rel * scale
    )


def run_branch_large_from_treewidth(exe: pathlib.Path) -> None:
    # P_20: nvars=20 >= BRANCH_TREEWIDTH_DELEGATE_MIN_VARS=16 → enters branch_try_dp_delegate.
    # from-treewidth source sets rw_uses_from_treewidth=true → order cache MISS path
    # (lines 1416-1420, 1448-1453, 1460-1480 in branch.c).
    p20 = _path_qsop(20, 8)
    native = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "32", "-"],
        input=p20, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    ft = subprocess.run(
        [str(exe), "--backend", "branch", "--branch-rw-source", "from-treewidth",
         "--max-vars", "32", "-"],
        input=p20, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if (
        native.returncode != 0
        or ft.returncode != 0
        or not _amplitudes_close(ft.stdout, native.stdout)
    ):
        raise AssertionError(
            f"branch from-treewidth P_20 mismatch\nnative: {native.stdout}{native.stderr}\n"
            f"from-treewidth: {ft.stdout}{ft.stderr}"
        )

    # Asymmetric 2×P_20: component A has a unary term (u 0 1), component B is a plain P_20.
    # The two components share the same adjacency fingerprint (both re-index to edges 0-1..18-19)
    # but have different residual-cache keys (different unary[0]).  Component A goes through
    # branch_try_dp_delegate, misses the order cache, and inserts its order.  Component B also
    # misses the residual cache (different unary) → hits branch_try_dp_delegate → order cache
    # HIT → covers branch_order_cache_lookup success body (lines 225-231) and the cache-hit
    # branch of branch_try_dp_delegate (lines 1456-1457).
    # With --stats-jsonl the treewidth-delegation recording path fires for each component
    # (lines 1672-1678 in branch.c).
    asym_two_paths = (
        f"p qsop-sign 8 40 38\nn 0\ncst 0\nu 0 1\n"
        + "\n".join(f"e {i} {i + 1}" for i in range(19))
        + "\n"
        + "\n".join(f"e {20 + i} {20 + i + 1}" for i in range(19))
        + "\n"
    )
    with tempfile.TemporaryDirectory() as td:
        sink = pathlib.Path(td) / "asym_sink.jsonl"
        asym_ft = subprocess.run(
            [str(exe), "--backend", "branch", "--branch-rw-source", "from-treewidth",
             "--max-vars", "64", "--stats-jsonl", str(sink), "-"],
            input=asym_two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        )
        asym_ref = subprocess.run(
            [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
            input=asym_two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        )
    if (
        asym_ft.returncode != 0
        or asym_ref.returncode != 0
        or not _amplitudes_close(asym_ft.stdout, asym_ref.stdout)
    ):
        raise AssertionError(
            f"branch from-treewidth asymmetric 2×P_20 mismatch\n"
            f"from-treewidth: {asym_ft.stdout}{asym_ft.stderr}\n"
            f"ref: {asym_ref.stdout}{asym_ref.stderr}"
        )

    # K_16 complete graph (r=2, all edges weight 1 = r/2, cutrank_width=1).
    # min_fill_width=15 > BRANCH_TREEWIDTH_DELEGATE_MAX_WIDTH=14 AND rw_uses_from_treewidth=true
    # → use_stats_order_for_rankwidth=true → covers lines 1460-1463 (wide path in branch_try_dp_delegate).
    k16_edges = "\n".join(
        f"e {u} {v}" for u in range(16) for v in range(u + 1, 16)
    )
    k16 = f"p qsop-sign 2 16 120\nn 0\ncst 0\n{k16_edges}\n"
    k16_ft = subprocess.run(
        [str(exe), "--backend", "branch", "--branch-rw-source", "from-treewidth",
         "--max-vars", "32", "-"],
        input=k16, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    k16_ref = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "32", "-"],
        input=k16, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if (
        k16_ft.returncode != 0
        or k16_ref.returncode != 0
        or not _amplitudes_close(k16_ft.stdout, k16_ref.stdout)
    ):
        raise AssertionError(
            f"branch from-treewidth K_16 mismatch\n"
            f"from-treewidth: {k16_ft.stdout}{k16_ft.stderr}\n"
            f"ref: {k16_ref.stdout}{k16_ref.stderr}"
        )

    # 2×P_20 symmetric (both components identical): exercise the "both" rw_source code path.
    two_paths = (
        f"p qsop-sign 8 40 38\nn 0\ncst 0\n"
        + "\n".join(f"e {i} {i + 1}" for i in range(19))
        + "\n"
        + "\n".join(f"e {20 + i} {20 + i + 1}" for i in range(19))
        + "\n"
    )
    for rw_source in ("from-treewidth", "both"):
        ft2 = subprocess.run(
            [str(exe), "--backend", "branch", "--branch-rw-source", rw_source,
             "--max-vars", "64", "-"],
            input=two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        ref2 = subprocess.run(
            [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
            input=two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if (
            ft2.returncode != 0
            or ref2.returncode != 0
            or not _amplitudes_close(ft2.stdout, ref2.stdout)
        ):
            raise AssertionError(
                f"branch {rw_source} 2×P_20 mismatch\n{ft2.stdout}{ft2.stderr}\n"
                f"ref: {ref2.stdout}{ref2.stderr}"
            )


def run_branch_large_fourier(exe: pathlib.Path) -> None:
    # P_20 with --solve-mode fourier triggers use_fourier=true inside branch_try_dp_delegate
    # when treewidth delegation fires (min_fill=2 <= 14), covering line 1618 in branch.c.
    p20 = _path_qsop(20, 8)
    fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "fourier",
            "--max-vars",
            "32",
            "--format",
            "residue-vector",
            "-",
        ],
        input=p20, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    ct = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "32", "--format", "residue-vector", "-"],
        input=p20, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if fourier.returncode != 0 or ct.returncode != 0 or fourier.stdout != ct.stdout:
        raise AssertionError(
            f"branch Fourier P_20 mismatch\nfourier: {fourier.stdout}{fourier.stderr}\n"
            f"count-table: {ct.stdout}{ct.stderr}"
        )

    # 2×P_20 disconnected with Fourier: branch_solve_residual_split_components runs with
    # use_fourier=true, covering the NTT prime/root setup and per-component convolution
    # (lines 1853-1875, 1883-1890, 1920-1932 in branch.c).
    two_paths = (
        f"p qsop-sign 8 40 38\nn 0\ncst 0\n"
        + "\n".join(f"e {i} {i + 1}" for i in range(19))
        + "\n"
        + "\n".join(f"e {20 + i} {20 + i + 1}" for i in range(19))
        + "\n"
    )
    fourier2 = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "fourier",
            "--max-vars",
            "64",
            "--format",
            "residue-vector",
            "-",
        ],
        input=two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    ct2 = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "--format", "residue-vector", "-"],
        input=two_paths, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if fourier2.returncode != 0 or ct2.returncode != 0 or fourier2.stdout != ct2.stdout:
        raise AssertionError(
            f"branch Fourier 2×P_20 mismatch\nfourier: {fourier2.stdout}{fourier2.stderr}\n"
            f"count-table: {ct2.stdout}{ct2.stderr}"
        )


def run_branch_stats_sink(exe: pathlib.Path) -> None:
    # K_{20,20}: rankwidth wins → recording after rankwidth delegation (lines 1537-1543).
    knn_edges = "\n".join(f"e {u} {v}" for u in range(20) for v in range(20, 40))
    knn = f"p qsop-sign 16 40 400\nn 0\ncst 5\n{knn_edges}\n"
    # P_20: treewidth wins → recording after treewidth delegation (lines 1672-1678).
    p20 = _path_qsop(20, 8)

    with tempfile.TemporaryDirectory() as td:
        sink = pathlib.Path(td) / "sink.jsonl"
        for label, qsop_text, max_vars in [("K_{20,20}", knn, "40"), ("P_20", p20, "32")]:
            result = subprocess.run(
                [str(exe), "--backend", "branch", "--max-vars", max_vars,
                 "--stats-jsonl", str(sink), "-"],
                input=qsop_text, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            if result.returncode != 0:
                raise AssertionError(
                    f"branch stats-sink {label} failed\n{result.stderr}"
                )
            if not sink.exists() or sink.stat().st_size == 0:
                raise AssertionError(
                    f"branch stats-sink {label}: expected JSONL output in {sink}"
                )
            sink.unlink()


def run_rankwidth_memory_budget(exe: pathlib.Path) -> None:
    """D1: --rankwidth-memory-budget-mib and --rankwidth-memory-policy option parsing."""
    p5 = _path_qsop(5, 8)

    # Large budget: solve proceeds normally.
    ok = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "16",
         "--rankwidth-memory-budget-mib", "1000", "--rankwidth-memory-policy", "skip",
         "--format", "stats", "-"],
        input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if ok.returncode != 0:
        raise AssertionError(f"rankwidth with large budget failed\n{ok.stderr}")
    if "backend: rankwidth" not in ok.stdout:
        raise AssertionError(f"expected rankwidth stats output\n{ok.stdout}")

    # Hard-error policy with large budget: solve still succeeds.
    ok2 = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "16",
         "--rankwidth-memory-budget-mib", "1000", "--rankwidth-memory-policy", "hard-error",
         "--format", "stats", "-"],
        input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if ok2.returncode != 0:
        raise AssertionError(f"rankwidth hard-error with large budget failed\n{ok2.stderr}")

    # Fallback policy with large budget: solve succeeds.
    ok3 = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "16",
         "--rankwidth-memory-budget-mib", "1000", "--rankwidth-memory-policy", "fallback",
         "--format", "stats", "-"],
        input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if ok3.returncode != 0:
        raise AssertionError(f"rankwidth fallback with large budget failed\n{ok3.stderr}")

    # Invalid policy value must be rejected (exit 2).
    bad_pol = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "16",
         "--rankwidth-memory-policy", "bad-value", "-"],
        input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if bad_pol.returncode != 2:
        raise AssertionError(
            f"expected exit 2 for invalid --rankwidth-memory-policy, got {bad_pol.returncode}"
        )

    # Invalid budget value must be rejected (exit 2).
    bad_bud = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "16",
         "--rankwidth-memory-budget-mib", "notanumber", "-"],
        input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
    )
    if bad_bud.returncode != 2:
        raise AssertionError(
            f"expected exit 2 for invalid --rankwidth-memory-budget-mib, got {bad_bud.returncode}"
        )


def run_branch_policy_arg_validation(exe: pathlib.Path) -> None:
    """Phase 4C: branch policy numeric flags must reject invalid values (exit 2)."""
    p5 = _path_qsop(5, 8)

    invalid_cases = [
        ("--branch-rw-min-treewidth-width", "notanint"),
        ("--branch-rw-min-treewidth-forecast", "notanint"),
        ("--branch-rw-min-residual-vars", "notanint"),
        ("--branch-rw-low-rank-bypass", "notanint"),
        ("--branch-rw-min-speedup", "notadouble"),
        ("--branch-rw-fixed-overhead-ns", "notanint"),
        ("--branch-tw-fixed-overhead-ns", "notanint"),
        ("--branch-rw-memory-penalty-ns", "notanint"),
        ("--rankwidth-memory-budget-bytes", "notanint"),
        ("--rankwidth-materialize-join-max-pairs", "notanint"),
    ]
    for flag, bad_value in invalid_cases:
        result = subprocess.run(
            [str(exe), "--backend", "branch", flag, bad_value, "-"],
            input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if result.returncode != 2:
            raise AssertionError(
                f"expected exit 2 for {flag} {bad_value!r}, got {result.returncode}"
            )
        if "error:" not in result.stderr:
            raise AssertionError(
                f"{flag}: expected error message on stderr, got: {result.stderr!r}"
            )

    # Negative values for uint flags must also be rejected.
    neg_cases = [
        "--branch-rw-min-treewidth-width",
        "--branch-rw-min-residual-vars",
        "--branch-rw-fixed-overhead-ns",
        "--branch-tw-fixed-overhead-ns",
    ]
    for flag in neg_cases:
        result = subprocess.run(
            [str(exe), "--backend", "branch", flag, "-1", "-"],
            input=p5, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if result.returncode != 2:
            raise AssertionError(
                f"expected exit 2 for {flag} -1, got {result.returncode}"
            )


def run_kernel_diagnostics(exe: pathlib.Path) -> None:
    auto = subprocess.run(
        [str(exe), "--print-kernels"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_auto = {
        "single_mode_precision=auto",
    }
    if auto.returncode != 0 or not expected_auto.issubset(set(auto.stdout.splitlines())):
        raise AssertionError(f"--print-kernels failed\n{auto.stdout}\n{auto.stderr}")
    # simd_compiled is the set of kernels linked into the binary; simd_kernel is the one this CPU
    # actually selected at run time. On a fat build they differ (e.g. compiled avx512,avx2,scalar
    # but running avx2), so the stats assertions below must key off the active kernel.
    compiled_simd = next(
        (
            line.split("=", 1)[1]
            for line in auto.stdout.splitlines()
            if line.startswith("simd_compiled=")
        ),
        None,
    )
    if compiled_simd is None:
        raise AssertionError(f"--print-kernels missing simd_compiled\n{auto.stdout}")
    active_simd = next(
        (
            line.split("=", 1)[1]
            for line in auto.stdout.splitlines()
            if line.startswith("simd_kernel=")
        ),
        None,
    )
    if active_simd is None:
        raise AssertionError(f"--print-kernels missing simd_kernel\n{auto.stdout}")
    if active_simd not in compiled_simd.split(","):
        raise AssertionError(
            f"active simd kernel {active_simd!r} is not among compiled {compiled_simd!r}"
        )
    if not any(line.startswith("bitset_popcount_kernel=") for line in auto.stdout.splitlines()):
        raise AssertionError(f"--print-kernels missing bitset_popcount_kernel\n{auto.stdout}")

    simd_arg = subprocess.run(
        [str(exe), "--simd", "scalar", "--print-kernels"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if simd_arg.returncode != 2 or "unknown option '--simd'" not in simd_arg.stderr:
        raise AssertionError(f"--simd should not be accepted\n{simd_arg.stdout}\n{simd_arg.stderr}")

    precision_without_mode = subprocess.run(
        [str(exe), "--backend", "treewidth", "--single-mode-precision", "double", "-"],
        input=_path_qsop(3, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        precision_without_mode.returncode != 2
        or "--single-mode-precision requires --solve-mode single-fourier or auto"
        not in precision_without_mode.stderr
    ):
        raise AssertionError(
            f"--single-mode-precision guard failed\n"
            f"{precision_without_mode.stdout}\n{precision_without_mode.stderr}"
        )

    treewidth_double = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "treewidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "-",
        ],
        input=_path_qsop(3, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_double_stats = {
        "single_mode_precision: double",
        f"simd_kernel: {active_simd}",
        f"bitset_kernel: {active_simd}",
        "treewidth_single_complex_kernel: 2",
    }
    if (
        treewidth_double.returncode != 0
        or not expected_double_stats.issubset(set(treewidth_double.stdout.splitlines()))
    ):
        raise AssertionError(
            f"treewidth double precision solve failed\n"
            f"{treewidth_double.stdout}\n{treewidth_double.stderr}"
        )

    rankwidth_double = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--single-mode-precision",
            "double",
            "-",
        ],
        input=_path_qsop(3, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_rankwidth_double_stats = {
        "single_mode_precision: double",
        f"simd_kernel: {active_simd}",
        f"bitset_kernel: {active_simd}",
        "rankwidth_single_complex_kernel: 2",
    }
    if (
        rankwidth_double.returncode != 0
        or not expected_rankwidth_double_stats.issubset(set(rankwidth_double.stdout.splitlines()))
    ):
        raise AssertionError(
            f"rankwidth double precision solve failed\n"
            f"{rankwidth_double.stdout}\n{rankwidth_double.stderr}"
        )
    rankwidth_double_stats = parse_solver_stats(rankwidth_double.stdout)
    if rankwidth_double_stats.get("simd_scalar_fallback_ops", 0) <= 0:
        raise AssertionError(
            f"rankwidth double precision should report scalar fallback SIMD work\n"
            f"{rankwidth_double.stdout}"
        )
    if rankwidth_double_stats.get("simd_vectorized_ops", 0) != 0:
        raise AssertionError(
            f"rankwidth double precision unexpectedly reported vectorized ops\n"
            f"{rankwidth_double.stdout}"
        )

    rankwidth_materialized = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "materialized",
            "-",
        ],
        input=_path_qsop(5, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rankwidth_materialized.returncode != 0:
        raise AssertionError(
            f"rankwidth materialized single-mode failed\n"
            f"{rankwidth_materialized.stdout}\n{rankwidth_materialized.stderr}"
        )
    materialized_stats = parse_solver_stats(rankwidth_materialized.stdout)
    if materialized_stats.get("rankwidth_materialized_join_events", 0) < 1:
        raise AssertionError(f"expected materialized join events\n{rankwidth_materialized.stdout}")
    if materialized_stats.get("rankwidth_transition_bytes", 0) <= 0:
        raise AssertionError(f"expected materialized transition bytes\n{rankwidth_materialized.stdout}")

    rankwidth_streaming = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "streaming",
            "-",
        ],
        input=_path_qsop(5, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rankwidth_streaming.returncode != 0:
        raise AssertionError(
            f"rankwidth streaming single-mode failed\n"
            f"{rankwidth_streaming.stdout}\n{rankwidth_streaming.stderr}"
        )
    streaming_stats = parse_solver_stats(rankwidth_streaming.stdout)
    if streaming_stats.get("rankwidth_streaming_join_events", 0) < 1:
        raise AssertionError(f"expected streaming join events\n{rankwidth_streaming.stdout}")

    rankwidth_dense = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--solve-mode",
            "single-fourier",
            "--rankwidth-single-kernel",
            "dense",
            "-",
        ],
        input=_path_qsop(5, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if rankwidth_dense.returncode != 0:
        raise AssertionError(
            f"rankwidth dense single-mode failed\n"
            f"{rankwidth_dense.stdout}\n{rankwidth_dense.stderr}"
        )
    dense_stats = parse_solver_stats(rankwidth_dense.stdout)
    if dense_stats.get("rankwidth_dense_join_events", 0) < 1:
        raise AssertionError(f"expected dense join events\n{rankwidth_dense.stdout}")

    branch_kernel = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "single-fourier",
            "--branch-single-kernel",
            "scalar",
            "--branch-single-precision",
            "double",
            "-",
        ],
        input=_path_qsop(4, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch_kernel.returncode != 0 or "mode: single-fourier" not in branch_kernel.stdout:
        raise AssertionError(
            f"branch single-mode kernel options failed\n"
            f"{branch_kernel.stdout}\n{branch_kernel.stderr}"
        )

    bad_branch_numeric = subprocess.run(
        [
            str(exe),
            "--backend",
            "branch",
            "--solve-mode",
            "single-fourier",
            "--branch-single-max-search-nodes",
            "notanint",
            "-",
        ],
        input=_path_qsop(3, 8),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_branch_numeric.returncode != 2 or "branch-single-max-search-nodes" not in bad_branch_numeric.stderr:
        raise AssertionError(
            f"branch single-mode numeric guard failed\n"
            f"{bad_branch_numeric.stdout}\n{bad_branch_numeric.stderr}"
        )


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_solve.py SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_solve(exe, source_root, "solve_single")
    run_solve(exe, source_root, "solve_signed_edge")
    run_solve(exe, source_root, "solve_disconnected")
    run_solve(exe, source_root, "solve_mirrored_path_components")
    run_max_vars_guard(exe, source_root)
    run_large_rankwidth_crt(exe)
    run_auto_amplitude_handles_large_count_strings(exe)
    run_branch_dp_handoff(exe)
    run_branch_root_treewidth_trace(exe)
    run_branch_rankwidth_handoff(exe)
    run_solver_stats(exe, source_root)
    run_include_result_stats(exe, source_root)
    run_branch_component_cache(exe, source_root)
    run_cli_paths(exe, source_root)
    run_rankwidth_backend(exe, source_root)
    run_branch_heuristics(exe, source_root)
    run_treewidth_backend(exe, source_root)
    run_trace_csv(exe, source_root)
    run_branch_large_from_treewidth(exe)
    run_branch_large_fourier(exe)
    run_branch_stats_sink(exe)
    run_rankwidth_memory_budget(exe)
    run_branch_policy_arg_validation(exe)
    run_kernel_diagnostics(exe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
