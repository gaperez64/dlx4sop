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

    components = subprocess.run(
        [str(exe), "--backend", "components", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if components.returncode != 0 or components.stdout != expected:
        raise AssertionError(f"large components CRT solve failed\n{components.stdout}\n{components.stderr}")

    default = subprocess.run(
        [str(exe), "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if default.returncode != 0 or default.stdout != expected:
        raise AssertionError(f"large default CRT solve failed\n{default.stdout}\n{default.stderr}")

    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if branch.returncode != 0 or branch.stdout != expected:
        raise AssertionError(f"large branch CRT solve failed\n{branch.stdout}\n{branch.stderr}")

    treewidth = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if treewidth.returncode != 0 or treewidth.stdout != expected:
        raise AssertionError(f"large treewidth CRT solve failed\n{treewidth.stdout}\n{treewidth.stderr}")

    brute_force = subprocess.run(
        [str(exe), "--backend", "brute-force", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if brute_force.returncode == 0 or "supports at most 62 variables" not in brute_force.stderr:
        raise AssertionError(
            f"brute force did not report the large-variable guard\n"
            f"{brute_force.stdout}\n{brute_force.stderr}"
        )

    labelled_qsop = "p qsop 8 64 1\nn 0\ncst 0\nq 0 1 3\n"
    labelled = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "64", "-"],
        input=labelled_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    labelled_expected = (
        "p qsop-result 8\n"
        "n 0\n"
        "counts 13835058055282163712 0 0 4611686018427387904 0 0 0 0\n"
    )
    if labelled.returncode != 0 or labelled.stdout != labelled_expected:
        raise AssertionError(f"large labelled rankwidth CRT solve failed\n{labelled.stdout}\n{labelled.stderr}")

    labelled_branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
        input=labelled_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if labelled_branch.returncode != 0 or labelled_branch.stdout != labelled_expected:
        raise AssertionError(
            f"large labelled branch CRT solve failed\n{labelled_branch.stdout}\n"
            f"{labelled_branch.stderr}"
        )


def run_branch_dp_handoff(exe: pathlib.Path) -> None:
    left_edges = [f"q {i} {i + 1} 8" for i in range(31)]
    right_edges = [f"q {i} {i + 1} 8" for i in range(32, 63)]
    edges = "\n".join(left_edges + right_edges)
    qsop = f"p qsop-sign 16 64 62\nn 0\ncst 3\n{edges}\n"
    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "64", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    treewidth = subprocess.run(
        [str(exe), "--backend", "treewidth", "--max-vars", "64", "-"],
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
        "treewidth_delegations: 1",
        "rankwidth_delegations: 0",
        "decomposition_width: 1",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(f"branch DP handoff stats failed\n{stats.stdout}\n{stats.stderr}")
    trace_phases = {line.split(",", 1)[0] for line in stats.stderr.splitlines()[1:] if line}
    expected_trace = {
        "branch.treewidth_order_probe",
        "branch.treewidth_delegate",
        "treewidth.initial_factors",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch DP handoff trace missing {expected_trace - trace_phases}\n{stats.stderr}"
        )


def run_branch_root_treewidth_trace(exe: pathlib.Path) -> None:
    edges = "\n".join(f"q {i} {i + 1} 8" for i in range(31))
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
        "treewidth_delegations: 1",
        "rankwidth_delegations: 0",
        "branch_rankwidth_skips: 1",
    }
    if stats.returncode != 0 or not all(part in stats.stdout for part in expected_stats):
        raise AssertionError(f"branch root treewidth stats failed\n{stats.stdout}\n{stats.stderr}")
    trace_phases = {line.split(",", 1)[0] for line in stats.stderr.splitlines()[1:] if line}
    expected_trace = {
        "branch.root_width_probe",
        "branch.treewidth_table_forecast",
        "branch.treewidth_join_pair_forecast",
        "branch.rankwidth_skip_treewidth_preferred",
        "branch.root_treewidth_delegate",
        "treewidth.initial_factors",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch root treewidth trace missing {expected_trace - trace_phases}\n{stats.stderr}"
        )


def run_branch_rankwidth_handoff(exe: pathlib.Path) -> None:
    edges = "\n".join(f"q {u} {v} 8" for u in range(20) for v in range(20, 40))
    qsop = f"p qsop-sign 16 40 400\nn 0\ncst 5\n{edges}\n"
    branch = subprocess.run(
        [str(exe), "--backend", "branch", "--max-vars", "40", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    rankwidth = subprocess.run(
        [str(exe), "--backend", "rankwidth", "--max-vars", "40", "-"],
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
        "branch.rankwidth_probe",
        "branch.rankwidth_support_probe",
        "branch.rankwidth_table_forecast",
        "branch.rankwidth_join_pair_forecast",
        "branch.treewidth_table_forecast",
        "branch.treewidth_join_pair_forecast",
        "branch.rankwidth_delegate",
    }
    if not expected_trace.issubset(trace_phases):
        raise AssertionError(
            f"branch rankwidth handoff trace missing {expected_trace - trace_phases}\n"
            f"{stats.stderr}"
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


def run_include_result_stats(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_labelled.qsop"
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
        "result_counts: 0 0 1 1 1 0 1 0",
    }
    if completed.returncode != 0 or not expected_result.issubset(set(completed.stdout.splitlines())):
        raise AssertionError(f"include-result stats failed\n{completed.stdout}\n{completed.stderr}")
    probability_lines = [
        line for line in completed.stdout.splitlines() if line.startswith("result_probability: ")
    ]
    if len(probability_lines) != 1:
        raise AssertionError(f"missing probability line\n{completed.stdout}\n{completed.stderr}")
    probability = float(probability_lines[0].split(": ", 1)[1])
    if abs(probability - 0.21338834764831843) > 1e-15:
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
        "p qsop 8 6 6\n"
        "n 0\n"
        "cst 0\n"
        "q 0 1 4\n"
        "q 1 2 4\n"
        "q 0 2 4\n"
        "q 3 4 4\n"
        "q 4 5 4\n"
        "q 3 5 4\n"
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
        ([str(exe), "--backend", "bad", str(qsop)], "unsupported backend"),
        ([str(exe), "--backend", "branch", "--max-vars", "0", str(qsop)], "residual branch solver refuses"),
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
        ([str(exe), "--treewidth-order"], "requires a value"),
        ([str(exe), "--treewidth-order", "bad", str(qsop)], "unsupported treewidth order"),
        ([str(exe), "--treewidth-order", "min-degree", str(qsop)], "requires --backend treewidth"),
        ([str(exe), "--branch-heuristic"], "requires a value"),
        ([str(exe), "--branch-heuristic", "rankwidth", str(qsop)], "unsupported branch heuristic"),
        ([str(exe), "--branch-heuristic", "cutrank", str(qsop)], "unsupported branch heuristic"),
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
        "rankwidth_support_width:",
        "rankwidth_labelled_width:",
        "table_entries:",
        "max_table_entries:",
        "signature_entries:",
        "max_signature_entries:",
        "join_pairs:",
        "join_signature_pairs:",
        "rankwidth_table_forecast:",
        "rankwidth_join_pair_forecast:",
        "rankwidth_labelled_exact_cuts:",
        "rankwidth_labelled_proxy_cuts:",
        "rankwidth_labelled_exact_assignments:",
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
    labelled_expected = subprocess.run(
        [str(exe), "--backend", "brute-force", str(labelled)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if labelled_expected.returncode != 0:
        raise AssertionError(f"labelled brute force solve failed\n{labelled_expected.stderr}")
    assert_rankwidth_matches(
        exe,
        labelled,
        labelled_expected.stdout,
        "--rankwidth-decomposition",
        str(labelled_decomposition),
    )
    assert_rankwidth_matches(exe, labelled, labelled_expected.stdout)
    assert_rankwidth_matches(exe, labelled, labelled_expected.stdout, "--rankwidth-generate", "balanced")
    assert_rankwidth_matches(exe, labelled, labelled_expected.stdout, "--rankwidth-generate", "min-fill-cut")

    labelled_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
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
    if (
        labelled_stats.returncode != 0
        or "rankwidth_mode: count-table" not in labelled_stats.stdout
        or "decomposition_width: 1" not in labelled_stats.stdout
        or "rankwidth_support_width: 1" not in labelled_stats.stdout
        or "rankwidth_labelled_width: 1" not in labelled_stats.stdout
        or "rankwidth_labelled_exact_cuts: 2" not in labelled_stats.stdout
        or "rankwidth_labelled_proxy_cuts: 0" not in labelled_stats.stdout
        or "join_signature_pairs: 4" not in labelled_stats.stdout
    ):
        raise AssertionError(f"labelled rankwidth stats failed\n{labelled_stats.stdout}\n{labelled_stats.stderr}")

    three_signature_qsop = "p qsop 4 3 2\nn 0\ncst 0\nq 0 2 1\nq 1 2 1\n"
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
        "rankwidth_support_width": 1,
        "rankwidth_labelled_width": 2,
        "max_table_entries": 3,
        "rankwidth_table_forecast": 12,
        "rankwidth_join_pair_forecast": 10,
        "rankwidth_labelled_exact_cuts": 4,
        "rankwidth_labelled_proxy_cuts": 0,
    }
    for key, value in expected_three_stats.items():
        if three_stats.get(key) != value:
            raise AssertionError(
                f"three-signature forecast mismatch for {key}: expected {value}, got "
                f"{three_stats.get(key)}\n{three_signature_stats.stdout}"
            )

    labelled_left_deep_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "left-deep",
            str(labelled),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    labelled_min_fill_cut_stats = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-generate",
            "min-fill-cut",
            str(labelled),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if labelled_left_deep_stats.returncode != 0 or labelled_min_fill_cut_stats.returncode != 0:
        raise AssertionError(
            f"labelled generated rankwidth stats failed\n"
            f"left-deep:\n{labelled_left_deep_stats.stdout}\n{labelled_left_deep_stats.stderr}\n"
            f"min-fill-cut:\n{labelled_min_fill_cut_stats.stdout}\n{labelled_min_fill_cut_stats.stderr}"
        )
    left_stats = parse_solver_stats(labelled_left_deep_stats.stdout)
    cut_stats = parse_solver_stats(labelled_min_fill_cut_stats.stdout)
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
                f"labelled min-fill-cut should use the left-deep fallback for {key}\n"
                f"left-deep:\n{labelled_left_deep_stats.stdout}\n"
                f"min-fill-cut:\n{labelled_min_fill_cut_stats.stdout}"
            )

    labelled_trace = subprocess.run(
        [
            str(exe),
            "--format",
            "stats",
            "--backend",
            "rankwidth",
            "--rankwidth-decomposition",
            str(labelled_decomposition),
            "--trace",
            "csv",
            str(labelled),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        labelled_trace.returncode != 0
        or "rankwidth.labelled_leaf" not in labelled_trace.stderr
        or "rankwidth.labelled_join_map" not in labelled_trace.stderr
        or "rankwidth.labelled_join" not in labelled_trace.stderr
    ):
        raise AssertionError(f"labelled rankwidth trace failed\n{labelled_trace.stdout}\n{labelled_trace.stderr}")

    bad_fourier = subprocess.run(
        [
            str(exe),
            "--backend",
            "rankwidth",
            "--rankwidth-mode",
            "fourier",
            "--rankwidth-decomposition",
            str(labelled_decomposition),
            str(labelled),
        ],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_fourier.returncode == 0 or "sign-only" not in bad_fourier.stderr:
        raise AssertionError(
            f"rankwidth Fourier accepted labelled QSOP\n{bad_fourier.stdout}\n{bad_fourier.stderr}"
        )


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

    for heuristic, reported in [
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
    for name in ["solve_single", "solve_labelled", "solve_disconnected"]:
        qsop = source_root / "tests" / "golden" / f"{name}.qsop"
        expected = subprocess.run(
            [str(exe), "--backend", "brute-force", str(qsop)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if expected.returncode != 0:
            raise AssertionError(f"{name}: brute-force solve failed\n{expected.stderr}")

        for order in ["min-fill", "min-degree", "min-fill-max-degree"]:
            completed = subprocess.run(
                [str(exe), "--backend", "treewidth", "--treewidth-order", order, str(qsop)],
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

    qsop = source_root / "tests" / "golden" / "solve_labelled.qsop"
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
