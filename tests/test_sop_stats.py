#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_stats(
    exe: pathlib.Path,
    source_root: pathlib.Path,
    args: list[str],
    expected_name: str,
) -> None:
    qsop = source_root / "tests" / "golden" / "sign_raw.qsop"
    expected = source_root / "tests" / "golden" / expected_name
    completed = subprocess.run(
        [str(exe), *args, str(qsop)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"sop-stats failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"stats output mismatch for {expected_name}\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_cli_paths(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "sign_raw.qsop"
    expected = source_root / "tests" / "golden" / "stats_sign.text"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: sop-stats" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    version_result = subprocess.run(
        [str(exe), "--version"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if version_result.returncode != 0 or version_result.stdout != "sop-stats 0.3\n":
        raise AssertionError(
            f"unexpected --version result:\n{version_result.stdout}\n{version_result.stderr}"
        )

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
        ([str(exe), "--format", "xml", str(qsop)], "unsupported format"),
        ([str(exe), "--exact-width-max-vars"], "requires a value"),
        ([str(exe), "--exact-width-max-vars", "bad"], "must be a non-negative integer"),
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

    # Stdin-based parse error cases: covers qsop_parse.c lines 585, 607-608, 649-650.
    parse_error_cases = [
        # Pre-header non-p directive → "appears before p header" (line 585)
        ("n 0\n", "appears before p header"),
        # Unknown directive after valid header → "unknown directive" (lines 607-608)
        ("p qsop-sign 8 1 0\nXXX 0\n", "unknown directive"),
        # Empty file (no p header at all) → "missing p header" (lines 649-650)
        ("", "missing p header"),
    ]
    for bad_input, expected_error in parse_error_cases:
        completed = subprocess.run(
            [str(exe), "-"],
            input=bad_input,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode == 0 or expected_error not in completed.stderr:
            raise AssertionError(
                f"expected parse error '{expected_error}' not found:\n{completed.stderr}"
            )


def run_large_width_diagnostics(exe: pathlib.Path) -> None:
    nvars = 66
    lines = ["p qsop-sign 8 66 65", "n 0", "cst 0", ""]
    lines.extend(f"e {v} {v + 1}" for v in range(nvars - 1))
    qsop = "\n".join(lines) + "\n"
    completed = subprocess.run(
        [str(exe), "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_parts = {
        "variables: 66",
        "quadratic_terms: 65",
        "components: 1",
        "max_degree: 2",
        "width_diagnostics: available",
        "min_fill_width: 1",
        "min_fill_edges: 0",
        "prefix_cut_rank: 1",
    }
    if completed.returncode != 0 or not expected_parts.issubset(set(completed.stdout.splitlines())):
        raise AssertionError(f"large width diagnostics failed\n{completed.stdout}\n{completed.stderr}")

    json_result = subprocess.run(
        [str(exe), "--format", "json", "-"],
        input=qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        json_result.returncode != 0
        or '"variables":66' not in json_result.stdout
        or '"width_diagnostics_available":true' not in json_result.stdout
        or '"prefix_cut_rank":1' not in json_result.stdout
    ):
        raise AssertionError(f"large JSON width diagnostics failed\n{json_result.stdout}\n{json_result.stderr}")


def run_exact_widths(exe: pathlib.Path) -> None:
    path_qsop = """p qsop-sign 8 4 3
n 0
cst 0

e 0 1
e 1 2
e 2 3
"""
    completed = subprocess.run(
        [str(exe), "--exact-widths", "-"],
        input=path_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    expected_parts = {
        "exact_widths: available",
        "exact_width_max_vars: 12",
        "exact_treewidth: 1",
        "exact_rankwidth: 1",
    }
    if completed.returncode != 0 or not expected_parts.issubset(set(completed.stdout.splitlines())):
        raise AssertionError(f"exact path widths failed\n{completed.stdout}\n{completed.stderr}")

    triangle_qsop = """p qsop-sign 8 3 3
n 0
cst 0

e 0 1
e 1 2
e 0 2
"""
    json_result = subprocess.run(
        [str(exe), "--format", "json", "--exact-widths", "--exact-width-max-vars", "3", "-"],
        input=triangle_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        json_result.returncode != 0
        or '"exact_widths_available":true' not in json_result.stdout
        or '"exact_width_max_vars":3' not in json_result.stdout
        or '"exact_treewidth":2' not in json_result.stdout
        or '"exact_rankwidth":1' not in json_result.stdout
    ):
        raise AssertionError(f"exact triangle JSON widths failed\n{json_result.stdout}\n{json_result.stderr}")

    skipped = subprocess.run(
        [str(exe), "--exact-widths", "--exact-width-max-vars", "3", "-"],
        input=path_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        skipped.returncode != 0
        or "exact_widths: skipped" not in skipped.stdout
        or "exact_treewidth:" in skipped.stdout
        or "exact_rankwidth:" in skipped.stdout
    ):
        raise AssertionError(f"exact width cap skip failed\n{skipped.stdout}\n{skipped.stderr}")

    # K_3+pendant (K_3 triangle with one extra leaf attached to vertex 0): min_degree=1 < best_width=2
    # → exact_treewidth_search does NOT prune at line 365-368 → enters the elimination loop body
    # (lines 370-415 in qsop_stats.c).
    k3_pendant = """p qsop-sign 8 4 4
n 0
cst 0

e 0 1
e 1 2
e 0 2
e 0 3
"""
    pendant_result = subprocess.run(
        [str(exe), "--exact-widths", "-"],
        input=k3_pendant,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        pendant_result.returncode != 0
        or "exact_treewidth: 2" not in pendant_result.stdout
        or "exact_rankwidth:" not in pendant_result.stdout
    ):
        raise AssertionError(f"K_3+pendant exact widths failed\n{pendant_result.stdout}\n{pendant_result.stderr}")

    # Single-variable instance: nvars=1 → qsop_compute_exact_treewidth returns 0 early (line 422)
    # and qsop_compute_exact_rankwidth returns true with *out=0 (lines 438-439).
    one_var = "p qsop-sign 8 1 0\nn 0\ncst 0\n"
    one_var_result = subprocess.run(
        [str(exe), "--exact-widths", "-"],
        input=one_var,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        one_var_result.returncode != 0
        or "exact_treewidth: 0" not in one_var_result.stdout
        or "exact_rankwidth: 0" not in one_var_result.stdout
    ):
        raise AssertionError(f"1-var exact widths failed\n{one_var_result.stdout}\n{one_var_result.stderr}")

    # --exact-width-max-vars 20 exceeds QSOP_EXACT_WIDTH_HARD_MAX=16 → value is clamped to 16
    # (line 505 in qsop_stats.c).
    capped = subprocess.run(
        [str(exe), "--exact-widths", "--exact-width-max-vars", "20", "-"],
        input=path_qsop,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if capped.returncode != 0 or "exact_width_max_vars: 16" not in capped.stdout:
        raise AssertionError(f"--exact-width-max-vars 20 clamp failed\n{capped.stdout}\n{capped.stderr}")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_sop_stats.py SOP_STATS SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_stats(exe, source_root, [], "stats_sign.text")
    run_stats(exe, source_root, ["--json"], "stats_sign.json")
    run_stats(exe, source_root, ["--format", "json"], "stats_sign.json")
    run_stats(exe, source_root, ["--format", "text"], "stats_sign.text")
    run_cli_paths(exe, source_root)
    run_large_width_diagnostics(exe)
    run_exact_widths(exe)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
