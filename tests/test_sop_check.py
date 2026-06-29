#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile


def run_check(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    raw = source_root / "tests" / "golden" / f"{name}_raw.qsop"
    expected = source_root / "tests" / "golden" / f"{name}_expected.qsop"
    completed = subprocess.run(
        [str(exe), str(raw)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: sop-check failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: canonical output mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_q_rejected(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    invalid = source_root / "tests" / "golden" / "invalid_sign_coeff.qsop"
    completed = subprocess.run(
        [str(exe), str(invalid)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode == 0:
        raise AssertionError("q line unexpectedly parsed")
    if "quadratic coefficients are not supported" not in completed.stderr:
        raise AssertionError(f"unexpected diagnostic:\n{completed.stderr}")


def run_cli_paths(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    raw = source_root / "tests" / "golden" / "sign_raw.qsop"
    expected = source_root / "tests" / "golden" / "sign_expected.qsop"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: sop-check" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    quiet_result = subprocess.run(
        [str(exe), "--quiet", str(raw)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if quiet_result.returncode != 0 or quiet_result.stdout != "":
        raise AssertionError(f"unexpected --quiet result:\n{quiet_result.stdout}\n{quiet_result.stderr}")

    stdin_result = subprocess.run(
        [str(exe), "-"],
        input=raw.read_text(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if stdin_result.returncode != 0 or stdin_result.stdout != expected.read_text():
        raise AssertionError(f"unexpected stdin result:\n{stdin_result.stdout}\n{stdin_result.stderr}")

    with tempfile.TemporaryDirectory() as tmp:
        output = pathlib.Path(tmp) / "out.qsop"
        output_result = subprocess.run(
            [str(exe), "--output", str(output), str(raw)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if output_result.returncode != 0 or output_result.stdout != "":
            raise AssertionError(f"unexpected --output result:\n{output_result.stderr}")
        if output.read_text() != expected.read_text():
            raise AssertionError("unexpected --output file contents")

        bad_output = subprocess.run(
            [str(exe), "--output", str(pathlib.Path(tmp) / "missing" / "out.qsop"), str(raw)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if bad_output.returncode == 0:
            raise AssertionError("bad output path unexpectedly succeeded")

    error_cases = [
        ([str(exe), "--output"], "requires a path"),
        ([str(exe), "--bad"], "unknown option"),
        ([str(exe), str(raw), str(raw)], "at most one input"),
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
        print("usage: test_sop_check.py SOP_CHECK SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_check(exe, source_root, "sign")
    run_q_rejected(exe, source_root)
    run_cli_paths(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
