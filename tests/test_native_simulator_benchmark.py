#!/usr/bin/env python3

import json
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_native_simulator_benchmark.py NATIVE_BENCH", file=sys.stderr)
        return 2

    tool = pathlib.Path(sys.argv[1])
    manifest = [
        {
            "name": "two_qubit",
            "qasm_lines": [
                "OPENQASM 2.0;",
                'include "qelib1.inc";',
                "qreg q[2];",
                "h q[0];",
                "cx q[0],q[1];",
            ],
            "boundaries": [["00", "00"]],
        }
    ]

    with tempfile.TemporaryDirectory() as tmp:
        manifest_path = pathlib.Path(tmp) / "manifest.json"
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                str(manifest_path),
                "--max-qubits",
                "1",
                "--skip-unsupported",
                "--format",
                "summary",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"native benchmark skip failed:\n{completed.stdout}\n{completed.stderr}")
        if "records: 1" not in completed.stdout or "skipped: 1" not in completed.stdout:
            raise AssertionError(f"unexpected skip summary:\n{completed.stdout}")

        try:
            import qiskit  # noqa: F401
        except ImportError:
            return 0

        manifest = [
            {
                "name": "native_compat",
                "qasm_lines": [
                    "OPENQASM 2.0;",
                    'include "qelib1.inc";',
                    "qreg q[3];",
                    "h q[0];",
                    "iswap q[0],q[1];",
                    "ccz q[0],q[1],q[2];",
                ],
                "boundaries": [["000", "000"]],
            }
        ]
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        completed = subprocess.run(
            [
                str(tool),
                str(manifest_path),
                "--engine",
                "qiskit-statevector",
                "--skip-unsupported",
                "--format",
                "summary",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise AssertionError(f"native compatibility benchmark failed:\n{completed.stdout}\n{completed.stderr}")
        if "engine: qiskit-statevector" not in completed.stdout or "ok: 1" not in completed.stdout:
            raise AssertionError(f"unexpected native compatibility summary:\n{completed.stdout}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
