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

MEASURED_QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
qreg q[2];
creg c[2];
h q[0];
cx q[0], q[1];
measure q -> c;
"""

SIMPLE_GATE_QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
gate makebell a,b {
h a;
cx a,b;
}
qreg q[2];
creg c[2];
makebell q[0],q[1];
measure q -> c;
"""

PARAM_GATE_QASM_SAMPLE = """OPENQASM 2.0;
include "qelib1.inc";
gate phasepair(theta) a,b {
u1(theta) a;
cp(theta) a,b;
}
qreg q[2];
h q;
phasepair(pi/4) q[0],q[1];
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
        (root / "measured.qasm").write_text(MEASURED_QASM_SAMPLE, encoding="utf-8")
        (root / "macro.qasm").write_text(SIMPLE_GATE_QASM_SAMPLE, encoding="utf-8")
        (root / "param.qasm").write_text(PARAM_GATE_QASM_SAMPLE, encoding="utf-8")
        (root / "small.qc").write_text(QC_SAMPLE, encoding="utf-8")
        (root / "bad.qasm").write_text("OPENQASM 2.0;\nqreg q[1];\nu1(pi/3) q[0];\n", encoding="utf-8")
        report_path = root / "report.json"

        qasm_cases, qasm_report = run_manifest(
            builder,
            qasm2sop,
            str(root),
            "--source-prefix",
            "smoke",
            "--boundaries",
            "zero-and-one",
            "--report",
            str(report_path),
        )
        if len(qasm_cases) != 1 or len(qasm_cases[0]["boundaries"]) != 2:
            raise AssertionError(f"unexpected QASM manifest:\n{qasm_cases}\n{qasm_report}")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if report["inputs"] != 5 or report["emitted"] != 1 or len(report["records"]) != 5:
            raise AssertionError(f"unexpected report shape:\n{report}")
        if (
            report["counts"].get("ok") != 1
            or report["counts"].get("unsupported_angle") != 1
            or report["counts"].get("unsupported_gate_definition") != 2
        ):
            raise AssertionError(f"unexpected report counts:\n{report}")
        ok_records = [record for record in report["records"] if record["status"] == "ok"]
        if len(ok_records) != 1 or ok_records[0].get("mode") != "sign":
            raise AssertionError(f"accepted record missing sign metadata:\n{report}")
        if len(ok_records[0].get("boundaries", [])) != 2:
            raise AssertionError(f"accepted record missing boundary metadata:\n{report}")

        stripped_cases, stripped_report = run_manifest(
            builder,
            qasm2sop,
            str(root),
            "--source-prefix",
            "smoke",
            "--strip-terminal-measurements",
        )
        measured = [case for case in stripped_cases if case["name"].endswith("measured")]
        if len(stripped_cases) != 2 or len(measured) != 1:
            raise AssertionError(f"unexpected stripped manifest:\n{stripped_cases}\n{stripped_report}")
        measured_qasm = "\n".join(measured[0]["qasm_lines"])
        if "creg" in measured_qasm or "measure" in measured_qasm:
            raise AssertionError(f"measured QASM was not stripped:\n{measured_qasm}")

        inlined_cases, inlined_report = run_manifest(
            builder,
            qasm2sop,
            str(root),
            "--source-prefix",
            "smoke",
            "--strip-terminal-measurements",
            "--inline-simple-gates",
        )
        macro = [case for case in inlined_cases if case["name"].endswith("macro")]
        param = [case for case in inlined_cases if case["name"].endswith("param")]
        if len(inlined_cases) != 4 or len(macro) != 1 or len(param) != 1:
            raise AssertionError(f"unexpected inlined manifest:\n{inlined_cases}\n{inlined_report}")
        macro_qasm = "\n".join(macro[0]["qasm_lines"])
        if "gate" in macro_qasm or "makebell" in macro_qasm or "measure" in macro_qasm:
            raise AssertionError(f"macro QASM was not inlined and stripped:\n{macro_qasm}")
        if "h q[0];" not in macro_qasm or "cx q[0],q[1];" not in macro_qasm:
            raise AssertionError(f"macro QASM missing expanded body:\n{macro_qasm}")
        param_qasm = "\n".join(param[0]["qasm_lines"])
        if "phasepair" in param_qasm or "theta" in param_qasm:
            raise AssertionError(f"parameterized macro was not inlined:\n{param_qasm}")
        if "u1(pi/4) q[0];" not in param_qasm or "cp(pi/4) q[0],q[1];" not in param_qasm:
            raise AssertionError(f"parameterized macro missing expanded body:\n{param_qasm}")

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
