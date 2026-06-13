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


def run_boundary_case(
    exe: pathlib.Path, source_root: pathlib.Path, name: str, options: list[str]
) -> None:
    qasm = source_root / "tests" / "golden" / f"{name}.qasm"
    expected = source_root / "tests" / "golden" / f"{name}.expected"
    completed = subprocess.run(
        [str(exe), *options, str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0 or completed.stdout != expected.read_text():
        raise AssertionError(
            f"{name}: unexpected boundary import\n{completed.stdout}\n{completed.stderr}"
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

    bad_phase = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[1];\nu1(pi/3) q[0];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if bad_phase.returncode == 0 or "unsupported u1 phase angle" not in bad_phase.stderr:
        raise AssertionError(f"unexpected bad phase result:\n{bad_phase.stderr}")

    mismatched_qregs = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg a[1];\nqreg b[2];\ncx a, b;\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if mismatched_qregs.returncode == 0 or "matching sizes" not in mismatched_qregs.stderr:
        raise AssertionError(f"unexpected mismatched qreg result:\n{mismatched_qregs.stderr}")

    bad_controlled_phase = subprocess.run(
        [str(exe), "-"],
        input="OPENQASM 2.0;\nqreg q[2];\ncu1(pi/3) q[0], q[1];\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        bad_controlled_phase.returncode == 0
        or "unsupported cu1 phase angle" not in bad_controlled_phase.stderr
    ):
        raise AssertionError(
            f"unexpected bad controlled phase result:\n{bad_controlled_phase.stderr}"
        )

    error_cases = [
        ([str(exe), "--bad"], "unknown option"),
        ([str(exe), "--input"], "missing value"),
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


def run_boundary_options(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    qasm = source_root / "tests" / "golden" / "qasm_h_boundary.qasm"
    expected = source_root / "tests" / "golden" / "qasm_h_boundary.expected"

    completed = subprocess.run(
        [str(exe), "--input", "1", "--output=1", str(qasm)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0 or completed.stdout != expected.read_text():
        raise AssertionError(f"unexpected boundary import:\n{completed.stdout}\n{completed.stderr}")

    zero_qasm = "OPENQASM 2.0;\nqreg q[1];\nid q[0];\n"
    zero_expected = "p qsop 8 1 0\nn 0\ncst 0\n\nu 0 4\n"
    zero_result = subprocess.run(
        [str(exe), "--input=0", "--output", "1", "-"],
        input=zero_qasm,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if zero_result.returncode != 0 or zero_result.stdout != zero_expected:
        raise AssertionError(f"unexpected zero-boundary import:\n{zero_result.stdout}\n{zero_result.stderr}")

    boundary_errors = [
        ([str(exe), "--input", "00", str(qasm)], "length 2 does not match 1"),
        ([str(exe), "--output", "2", str(qasm)], "must contain only 0 or 1"),
        ([str(exe), "--input", "0", "--input=0", str(qasm)], "duplicate --input"),
    ]
    for cmd, expected_error in boundary_errors:
        failed = subprocess.run(
            cmd,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if failed.returncode == 0 or expected_error not in failed.stderr:
            raise AssertionError(f"unexpected boundary error for {cmd}:\n{failed.stderr}")


def run_decomposed_gates(exe: pathlib.Path, source_root: pathlib.Path) -> None:
    cases = [
        ("qasm_x", ["--input", "0", "--output", "1"]),
        ("qasm_cx", ["--input", "10", "--output", "11"]),
        ("qasm_y", ["--input", "0", "--output", "1"]),
        ("qasm_cy", ["--input", "10", "--output", "11"]),
    ]
    for name, options in cases:
        run_boundary_case(exe, source_root, name, options)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm2sop.py QASM2SOP SOURCE_ROOT", file=sys.stderr)
        return 2

    exe = pathlib.Path(sys.argv[1])
    source_root = pathlib.Path(sys.argv[2])
    run_case(exe, source_root, "qasm_hth")
    run_case(exe, source_root, "qasm_cz")
    run_case(exe, source_root, "qasm_swap_id")
    run_case(exe, source_root, "qasm_u1")
    run_case(exe, source_root, "qasm_u1_negative")
    run_case(exe, source_root, "qasm_p")
    run_case(exe, source_root, "qasm_register_unary")
    run_boundary_case(
        exe, source_root, "qasm_register_cx", ["--input", "1100", "--output", "1111"]
    )
    run_boundary_case(exe, source_root, "qasm_cu1", ["--input", "11", "--output", "11"])
    run_boundary_case(
        exe, source_root, "qasm_named_cphase", ["--input", "11", "--output", "11"]
    )
    run_boundary_case(
        exe, source_root, "qasm_register_cp", ["--input", "1010", "--output", "1010"]
    )
    run_cli_paths(exe, source_root)
    run_boundary_options(exe, source_root)
    run_decomposed_gates(exe, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
