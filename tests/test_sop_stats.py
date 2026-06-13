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
    qsop = source_root / "tests" / "golden" / "labelled_raw.qsop"
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
    qsop = source_root / "tests" / "golden" / "labelled_raw.qsop"
    expected = source_root / "tests" / "golden" / "stats_labelled.text"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: sop-stats" not in help_result.stdout:
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
        ([str(exe), "--format", "xml", str(qsop)], "unsupported format"),
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
        print("usage: test_sop_stats.py SOP_STATS SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_stats(exe, source_root, [], "stats_labelled.text")
    run_stats(exe, source_root, ["--json"], "stats_labelled.json")
    run_stats(exe, source_root, ["--format", "json"], "stats_labelled.json")
    run_stats(exe, source_root, ["--format", "text"], "stats_labelled.text")
    run_cli_paths(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
