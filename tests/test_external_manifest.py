#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
h q[0];
cx q[0], q[1];
"""


QC_SAMPLE = """.v a b
BEGIN
H a
cnot a b
END
"""


def run_manifest(builder: pathlib.Path, qasm2sop: pathlib.Path, *args: str) -> tuple[list[dict], str]:
    completed = subprocess.run(
        [str(builder), str(qasm2sop), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        raise AssertionError(f"manifest builder failed:\n{completed.stderr}")
    return json.loads(completed.stdout), completed.stderr


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_external_manifest.py BUILDER QASM2SOP QC2QASM", file=sys.stderr)
        return 2

    builder = pathlib.Path(sys.argv[1])
    qasm2sop = pathlib.Path(sys.argv[2])
    qc2qasm = pathlib.Path(sys.argv[3])

    help_result = subprocess.run(
        [str(builder), "--help"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if help_result.returncode != 0 or "external QASM/QC" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "bell.qasm").write_text(QASM_SAMPLE, encoding="utf-8")
        (root / "small.qc").write_text(QC_SAMPLE, encoding="utf-8")
        (root / "bad.qasm").write_text("OPENQASM 2.0;\nqreg q[1];\nu1(pi/3) q[0];\n", encoding="utf-8")

        qasm_cases, qasm_report = run_manifest(
            builder,
            qasm2sop,
            str(root),
            "--source-prefix",
            "smoke",
            "--boundaries",
            "zero-and-one",
        )
        if len(qasm_cases) != 1 or len(qasm_cases[0]["boundaries"]) != 2:
            raise AssertionError(f"unexpected QASM manifest:\n{qasm_cases}\n{qasm_report}")

        qc_cases, qc_report = run_manifest(
            builder,
            qasm2sop,
            str(root),
            "--source-prefix",
            "smoke",
            "--include-qc",
            "--qc2qasm",
            str(qc2qasm),
            "--max-cases",
            "2",
        )
        if len(qc_cases) != 2 or not any(case["name"].endswith("small") for case in qc_cases):
            raise AssertionError(f"unexpected QC manifest:\n{qc_cases}\n{qc_report}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
