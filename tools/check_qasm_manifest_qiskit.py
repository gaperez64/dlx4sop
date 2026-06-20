#!/usr/bin/env python3

import argparse
import cmath
import json
import math
import pathlib
import subprocess
import sys

from bench_common import case_qasm
from qasm_native_compat import qasm_with_native_compat_definitions

try:
    from qiskit import QuantumCircuit
    from qiskit.quantum_info import Statevector
except ImportError:
    print("qiskit is not installed; skipping manifest comparison", file=sys.stderr)
    raise SystemExit(77)


def state_index(bits: str) -> int:
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def parse_solver_amplitude(output: str) -> complex:
    modulus = None
    norm_h = None
    counts = None
    for line in output.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[:2] == ["p", "qsop-result"]:
            modulus = int(parts[2])
        elif parts[0] == "n":
            norm_h = int(parts[1])
        elif parts[0] == "counts":
            counts = [int(part) for part in parts[1:]]
    if modulus is None or norm_h is None or counts is None:
        raise RuntimeError(f"malformed solver output:\n{output}")

    omega = cmath.exp(2j * math.pi / modulus)
    total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    return total * (2.0 ** (-norm_h / 2.0))


def sop_amplitude(
    qasm2sop: pathlib.Path,
    sop_solve: pathlib.Path,
    solver_args: list[str],
    qasm: str,
    input_bits: str,
    output_bits: str,
) -> complex:
    imported = subprocess.run(
        [str(qasm2sop), "--input", input_bits, "--output", output_bits, "-"],
        input=qasm,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if imported.returncode != 0:
        raise RuntimeError(imported.stderr.strip())

    solved = subprocess.run(
        [str(sop_solve), *solver_args, "-"],
        input=imported.stdout,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if solved.returncode != 0:
        raise RuntimeError(f"{solved.stderr.strip()}\nQSOP:\n{imported.stdout}")
    return parse_solver_amplitude(solved.stdout)


def qiskit_amplitude(qasm: str, input_bits: str, output_bits: str) -> complex:
    circuit = QuantumCircuit.from_qasm_str(qasm_with_native_compat_definitions(qasm))
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    state = Statevector(initial).evolve(circuit)
    return complex(state.data[state_index(output_bits)])


def compare_manifest(args: argparse.Namespace) -> int:
    cases = json.loads(args.manifest.read_text(encoding="utf-8"))
    checked = 0
    skipped_qiskit = 0
    skipped_qubit_cap = 0
    for case in cases:
        qasm = case_qasm(case)
        for input_bits, output_bits in case["boundaries"]:
            if args.limit is not None and checked >= args.limit:
                print(f"checked: {checked}")
                print(f"skipped_qiskit_unsupported: {skipped_qiskit}")
                print(f"skipped_qubit_cap: {skipped_qubit_cap}")
                return 0
            label = f"{case['name']} {input_bits}->{output_bits}"
            boundary_qubits = max(len(input_bits), len(output_bits))
            if args.max_qubits is not None and boundary_qubits > args.max_qubits:
                skipped_qubit_cap += 1
                print(
                    f"skip: {label}: boundary uses {boundary_qubits} qubits above --max-qubits {args.max_qubits}",
                    file=sys.stderr,
                )
                continue
            try:
                expected = qiskit_amplitude(qasm, input_bits, output_bits)
            except Exception as exc:
                if not args.skip_qiskit_unsupported:
                    raise
                skipped_qiskit += 1
                print(f"skip: {label}: {exc}", file=sys.stderr)
                continue
            actual = sop_amplitude(args.qasm2sop, args.sop_solve, args.solver_args, qasm, input_bits, output_bits)
            if abs(expected - actual) > args.tolerance:
                print(
                    f"mismatch: {label}: expected {expected!r}, got {actual!r}",
                    file=sys.stderr,
                )
                return 1
            checked += 1
    print(f"checked: {checked}")
    print(f"skipped_qiskit_unsupported: {skipped_qiskit}")
    print(f"skipped_qubit_cap: {skipped_qubit_cap}")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare a QASM manifest against Qiskit amplitudes.")
    parser.add_argument("qasm2sop", type=pathlib.Path)
    parser.add_argument("sop_solve", type=pathlib.Path)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--limit", type=int)
    parser.add_argument("--max-qubits", type=int, help="skip dense Qiskit comparisons above this boundary width")
    parser.add_argument("--skip-qiskit-unsupported", action="store_true")
    parser.add_argument(
        "--solver-backend",
        choices=("components", "brute-force", "branch", "rankwidth", "treewidth"),
        help="sop-solve backend used for the imported QSOP.",
    )
    parser.add_argument("--solver-max-vars", type=int, help="pass --max-vars to sop-solve")
    parser.add_argument("--tolerance", type=float, default=1e-9)
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_qubits is not None and args.max_qubits < 0:
        parser.error("--max-qubits must be non-negative")
    if args.tolerance <= 0:
        parser.error("--tolerance must be positive")
    if args.solver_max_vars is not None and args.solver_max_vars < 0:
        parser.error("--solver-max-vars must be non-negative")
    args.solver_args = []
    if args.solver_backend is not None:
        args.solver_args += ["--backend", args.solver_backend]
    if args.solver_max_vars is not None:
        args.solver_args += ["--max-vars", str(args.solver_max_vars)]
    return args


def main(argv: list[str]) -> int:
    return compare_manifest(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
