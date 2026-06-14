#!/usr/bin/env python3

import argparse
import csv
import json
import pathlib
import sys
import time
from typing import TextIO


CSV_FIELDS = [
    "case",
    "source",
    "source_url",
    "source_relative_path",
    "boundary",
    "input",
    "output",
    "engine",
    "qubits",
    "elapsed_ns",
    "amplitude_real",
    "amplitude_imag",
    "status",
    "error",
]


def case_qasm(case: dict) -> str:
    return "\n".join(case["qasm_lines"]) + "\n"


def state_index(bits: str) -> int:
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def qiskit_statevector_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        from qiskit import QuantumCircuit
        from qiskit.quantum_info import Statevector
    except ImportError as exc:
        raise RuntimeError("qiskit is not installed") from exc

    circuit = QuantumCircuit.from_qasm_str(qasm)
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    state = Statevector(initial).evolve(circuit)
    return complex(state.data[state_index(output_bits)]), circuit.num_qubits


def aer_statevector_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        from qiskit import QuantumCircuit
        from qiskit.quantum_info import Statevector
        from qiskit_aer import AerSimulator
    except ImportError as exc:
        raise RuntimeError("qiskit-aer is not installed") from exc

    circuit = QuantumCircuit.from_qasm_str(qasm)
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    circuit.save_statevector()
    result = AerSimulator(method="statevector").run(
        circuit,
        initial_statevector=Statevector(initial),
    ).result()
    state = result.get_statevector(circuit)
    return complex(state.data[state_index(output_bits)]), circuit.num_qubits


def native_amplitude(engine: str, qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    if engine == "qiskit-statevector":
        return qiskit_statevector_amplitude(qasm, input_bits, output_bits)
    if engine == "aer-statevector":
        return aer_statevector_amplitude(qasm, input_bits, output_bits)
    raise AssertionError(f"unhandled engine {engine}")


def boundary_records(cases: list[dict], limit: int | None):
    emitted = 0
    for case in cases:
        for boundary_index, boundary in enumerate(case["boundaries"]):
            if limit is not None and emitted >= limit:
                return
            emitted += 1
            yield case, boundary_index, boundary


def record_case(
    case: dict,
    boundary_index: int,
    input_bits: str,
    output_bits: str,
    engine: str,
    status: str,
    elapsed_ns: int | None = None,
    amplitude: complex | None = None,
    qubits: int | None = None,
    error: str | None = None,
) -> dict:
    return {
        "case": case["name"],
        "source": case.get("source", "internal"),
        "source_url": case.get("source_url", ""),
        "source_relative_path": case.get("source_relative_path", ""),
        "boundary": boundary_index,
        "input": input_bits,
        "output": output_bits,
        "engine": engine,
        "qubits": qubits,
        "elapsed_ns": elapsed_ns,
        "amplitude_real": None if amplitude is None else amplitude.real,
        "amplitude_imag": None if amplitude is None else amplitude.imag,
        "status": status,
        "error": error or "",
    }


def benchmark(args: argparse.Namespace) -> list[dict]:
    cases = json.loads(args.manifest.read_text(encoding="utf-8"))
    records = []
    for case, boundary_index, (input_bits, output_bits) in boundary_records(cases, args.limit):
        qasm = case_qasm(case)
        boundary_qubits = max(len(input_bits), len(output_bits))
        if args.max_qubits is not None and boundary_qubits > args.max_qubits:
            records.append(
                record_case(
                    case,
                    boundary_index,
                    input_bits,
                    output_bits,
                    args.engine,
                    "skipped",
                    qubits=boundary_qubits,
                    error=f"boundary uses {boundary_qubits} qubits above --max-qubits {args.max_qubits}",
                )
            )
            continue
        try:
            start = time.perf_counter_ns()
            amplitude, qubits = native_amplitude(args.engine, qasm, input_bits, output_bits)
            elapsed_ns = time.perf_counter_ns() - start
        except Exception as exc:
            if not args.skip_unsupported:
                raise
            records.append(
                record_case(
                    case,
                    boundary_index,
                    input_bits,
                    output_bits,
                    args.engine,
                    "skipped",
                    error=str(exc).strip().splitlines()[-1] if str(exc).strip() else repr(exc),
                )
            )
            continue
        records.append(
            record_case(
                case,
                boundary_index,
                input_bits,
                output_bits,
                args.engine,
                "ok",
                elapsed_ns=elapsed_ns,
                amplitude=amplitude,
                qubits=qubits,
            )
        )
    return records


def write_jsonl(records: list[dict], file: TextIO) -> None:
    for record in records:
        print(json.dumps(record, sort_keys=True), file=file)


def write_csv(records: list[dict], file: TextIO) -> None:
    writer = csv.DictWriter(file, fieldnames=CSV_FIELDS, extrasaction="ignore")
    writer.writeheader()
    for record in records:
        writer.writerow(record)


def write_summary(records: list[dict], file: TextIO) -> None:
    ok = [record for record in records if record["status"] == "ok"]
    skipped = [record for record in records if record["status"] != "ok"]
    total_elapsed = sum(int(record["elapsed_ns"] or 0) for record in ok)
    max_elapsed = max((int(record["elapsed_ns"] or 0) for record in ok), default=0)
    max_qubits = max((int(record["qubits"] or 0) for record in ok), default=0)
    print(f"records: {len(records)}", file=file)
    print(f"ok: {len(ok)}", file=file)
    print(f"skipped: {len(skipped)}", file=file)
    print(f"total_elapsed_ns: {total_elapsed}", file=file)
    print(f"max_elapsed_ns: {max_elapsed}", file=file)
    print(f"max_qubits: {max_qubits}", file=file)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark native statevector simulators on QASM manifest fixed-boundary amplitudes."
    )
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--engine", choices=("qiskit-statevector", "aer-statevector"), default="qiskit-statevector")
    parser.add_argument("--format", choices=("jsonl", "csv", "summary"), default="jsonl")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--max-qubits", type=int, help="skip boundaries above this dense-state qubit count")
    parser.add_argument("--skip-unsupported", action="store_true")
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_qubits is not None and args.max_qubits < 0:
        parser.error("--max-qubits must be non-negative")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    records = benchmark(args)
    if args.format == "jsonl":
        write_jsonl(records, sys.stdout)
    elif args.format == "csv":
        write_csv(records, sys.stdout)
    else:
        write_summary(records, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
