#!/usr/bin/env python3

import pathlib
import subprocess
import sys


QC_SAMPLE = """.v a b c d
.i a b
.o c d

BEGIN
H a b
tof a b
tof a b c
Z a b
Z a b c
Z a b c d
T* c
P* b
swap a d
END
"""

EXPECTED_QASM = """OPENQASM 2.0;
include "qelib1.inc";
qreg q[5];
h q[0];
h q[1];
cx q[0], q[1];
ccx q[0], q[1], q[2];
cz q[0], q[1];
ccz q[0], q[1], q[2];
h q[3];
ccx q[0], q[1], q[4];
ccx q[2], q[4], q[3];
ccx q[0], q[1], q[4];
h q[3];
tdg q[2];
sdg q[1];
swap q[0], q[3];
"""


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qc2qasm.py QC2QASM QASM2SOP", file=sys.stderr)
        return 2

    qc2qasm = pathlib.Path(sys.argv[1])
    qasm2sop = pathlib.Path(sys.argv[2])

    help_result = subprocess.run(
        [str(qc2qasm), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or ".qc" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    translated = subprocess.run(
        [str(qc2qasm), "-"],
        input=QC_SAMPLE,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if translated.returncode != 0 or translated.stdout != EXPECTED_QASM:
        raise AssertionError(
            f"unexpected translation\nstdout:\n{translated.stdout}\nstderr:\n{translated.stderr}"
        )

    imported = subprocess.run(
        [str(qasm2sop), "-"],
        input=translated.stdout,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if (
        imported.returncode == 0
        or "unsupported non-sign quadratic phase coefficient" not in imported.stderr
    ):
        raise AssertionError(f"translated QASM had unexpected import result:\n{imported.stderr}")

    invalid = subprocess.run(
        [str(qc2qasm), "-"],
        input=".v a\nBEGIN\nunknown a\nEND\n",
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if invalid.returncode == 0 or "unknown .qc gate" not in invalid.stderr:
        raise AssertionError(f"unexpected invalid result:\n{invalid.stderr}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
