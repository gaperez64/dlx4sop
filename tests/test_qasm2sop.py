#!/usr/bin/env python3

import pathlib
import subprocess
import sys


def run_case(exe: pathlib.Path, source_root: pathlib.Path, name: str) -> None:
    qasm = source_root / "tests" / "golden" / f"{name}.qasm"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"{name}: qasm2sop failed\n{completed.stderr}")
    expected_text = expected.read_text()
    if completed.stdout != expected_text:
        raise AssertionError(
            f"{name}: imported QSOP mismatch\n"
            f"expected:\n{expected_text}\n"
            f"actual:\n{completed.stdout}\n"
        )


def run_cli_paths(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qasm = source_root / "tests" / "golden" / "qasm_hth.qasm"
    expected = source_root / "tests" / "golden" / "qasm_hth.expected"

    help_result = subprocess.run(
        [str(exe), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "usage: qasm2sop" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    stdin_result = subprocess.run(
        [str(exe), "-"],
        input=qasm.read_text(),
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if stdin_result.returncode != 0 or stdin_result.stdout != expected.read_text():
        raise AssertionError(f"unexpected stdin result:\n{stdin_result.stdout}\n{stdin_result.stderr}")

    unsupported = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nmeasure q[0] -> c[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if unsupported.returncode == 0 or "dynamic or classical" not in unsupported.stderr:
        raise AssertionError(f"unexpected unsupported result:\n{unsupported.stderr}")

    error_cases = [
        ([str(exe), "--bad"], "unknown option"),
        ([str(exe), str(qasm), str(qasm)], "at most one input"),
        ([str(exe), str(source_root / "tests" / "golden" / "missing.qasm")], "No such file"),
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
        print("usage: test_qasm2sop.py QASM2SOP SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_case(exe, source_root, "qasm_hth")
    run_case(exe, source_root, "qasm_cz")
    run_cli_paths(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
