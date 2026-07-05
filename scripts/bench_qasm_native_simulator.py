#!/usr/bin/env python3

import argparse
import contextlib
import csv
import json
import pathlib
import signal
import sys
import time
from typing import TextIO

from bench_common import case_qasm
from qasm_native_compat import pyzx_state_index, qasm_with_native_compat_definitions


ENGINES = (
    "qiskit-statevector",
    "aer-statevector",
    "pyzx-matrix",
    "mqt-ddsim-statevector",
    "qiskit-clifford",
)

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
    "qubit_cap",
    "timeout_seconds",
    "memory_limit_mib",
    "cgroup_memory_limit_mib",
    "elapsed_ns",
    "amplitude_real",
    "amplitude_imag",
    "status",
    "error",
]


class NativeTimeout(RuntimeError):
    pass


@contextlib.contextmanager
def native_timeout(seconds: float | None):
    if seconds is None:
        yield
        return

    def handle_timeout(_signum, _frame):
        raise NativeTimeout(f"native simulator timed out after {seconds:g}s")

    old_handler = signal.signal(signal.SIGALRM, handle_timeout)
    signal.setitimer(signal.ITIMER_REAL, seconds)
    try:
        yield
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old_handler)


def apply_memory_limit(memory_limit_mib: int | None) -> None:
    if memory_limit_mib is None:
        return
    try:
        import resource
    except ImportError as exc:
        raise RuntimeError("--memory-limit-mib requires the resource module") from exc

    limit = memory_limit_mib * 1024 * 1024
    _soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    if hard != resource.RLIM_INFINITY and limit > hard:
        raise RuntimeError(
            f"--memory-limit-mib {memory_limit_mib} exceeds existing hard RLIMIT_AS"
        )
    resource.setrlimit(resource.RLIMIT_AS, (limit, hard))


def state_index(bits: str) -> int:
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def qiskit_circuit_from_qasm(qasm: str, *, decompose: bool = False):
    from qiskit import QuantumCircuit

    circuit = QuantumCircuit.from_qasm_str(qasm_with_native_compat_definitions(qasm))
    return circuit.decompose(reps=1) if decompose else circuit


def qiskit_statevector_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        from qiskit.quantum_info import Statevector
    except ImportError as exc:
        raise RuntimeError("qiskit is not installed") from exc

    circuit = qiskit_circuit_from_qasm(qasm)
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    state = Statevector(initial).evolve(circuit)
    return complex(state.data[state_index(output_bits)]), circuit.num_qubits


def aer_statevector_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        from qiskit.quantum_info import Statevector
        from qiskit_aer import AerSimulator
    except ImportError as exc:
        raise RuntimeError("qiskit-aer is not installed") from exc

    circuit = qiskit_circuit_from_qasm(qasm, decompose=True)
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    circuit.save_statevector()
    result = AerSimulator(method="statevector").run(
        circuit,
        initial_statevector=Statevector(initial),
    ).result()
    state = result.get_statevector(circuit)
    return complex(state.data[state_index(output_bits)]), circuit.num_qubits


def pyzx_matrix_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        import pyzx as zx
    except ImportError as exc:
        raise RuntimeError("pyzx is not installed") from exc

    circuit = zx.Circuit.from_qasm(qasm_with_native_compat_definitions(qasm))
    matrix = circuit.to_matrix()
    return (
        complex(matrix[pyzx_state_index(output_bits, circuit.qubits), pyzx_state_index(input_bits, circuit.qubits)]),
        circuit.qubits,
    )


def qiskit_clifford_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    """Compute amplitude for Clifford circuits using stabilizer simulation (O(n²) memory).

    Only valid when the amplitude is real and non-negative (sign-edge boundaries for
    Clifford circuits such as GHZ and BV). Returns sqrt(probability) as the real part.
    Fails with RuntimeError for non-Clifford circuits.
    """
    try:
        from qiskit import QuantumCircuit
        from qiskit.quantum_info import Clifford, StabilizerState
    except ImportError as exc:
        raise RuntimeError("qiskit is not installed") from exc

    base = qiskit_circuit_from_qasm(qasm)
    if len(input_bits) > base.num_qubits:
        raise RuntimeError(f"input boundary uses {len(input_bits)} bits for {base.num_qubits} qubits")

    prepared = QuantumCircuit(base.num_qubits)
    for qubit, bit in enumerate(input_bits):
        if bit == "1":
            prepared.x(qubit)
    prepared.compose(base, inplace=True)

    try:
        clf = Clifford(prepared)
    except Exception as exc:
        raise RuntimeError(f"circuit is not a Clifford circuit: {exc}") from exc

    stab = StabilizerState(clf)
    prob = stab.probabilities_dict().get(output_bits, 0.0)
    import math
    amp = math.sqrt(max(prob, 0.0))
    return complex(amp, 0.0), base.num_qubits


def mqt_ddsim_statevector_amplitude(qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    try:
        from qiskit import QuantumCircuit
        from mqt.ddsim import DDSIMProvider
    except ImportError as exc:
        raise RuntimeError("mqt.ddsim is not installed") from exc

    circuit = qiskit_circuit_from_qasm(qasm)
    prepared = QuantumCircuit(circuit.num_qubits)
    if len(input_bits) > circuit.num_qubits:
        raise RuntimeError(f"input boundary uses {len(input_bits)} bits for {circuit.num_qubits} qubits")
    for qubit, bit in enumerate(input_bits):
        if bit == "1":
            prepared.x(qubit)
    prepared.compose(circuit, inplace=True)
    backend = DDSIMProvider().get_backend("statevector_simulator")
    try:
        result = backend.run(prepared, shots=0).result()
    except Exception as exc:
        if isinstance(exc, NativeTimeout):
            raise
        decomposed = QuantumCircuit(circuit.num_qubits)
        for qubit, bit in enumerate(input_bits):
            if bit == "1":
                decomposed.x(qubit)
        decomposed.compose(circuit.decompose(reps=1), inplace=True)
        result = backend.run(decomposed, shots=0).result()
    state = result.data(0)["statevector"]
    return complex(state[state_index(output_bits)]), circuit.num_qubits


def native_amplitude(engine: str, qasm: str, input_bits: str, output_bits: str) -> tuple[complex, int]:
    if engine == "qiskit-statevector":
        return qiskit_statevector_amplitude(qasm, input_bits, output_bits)
    if engine == "aer-statevector":
        return aer_statevector_amplitude(qasm, input_bits, output_bits)
    if engine == "pyzx-matrix":
        return pyzx_matrix_amplitude(qasm, input_bits, output_bits)
    if engine == "mqt-ddsim-statevector":
        return mqt_ddsim_statevector_amplitude(qasm, input_bits, output_bits)
    if engine == "qiskit-clifford":
        return qiskit_clifford_amplitude(qasm, input_bits, output_bits)
    raise AssertionError(f"unhandled engine {engine}")


def engine_import_error(engine: str) -> str:
    try:
        if engine in ("qiskit-statevector", "aer-statevector", "mqt-ddsim-statevector"):
            import qiskit  # noqa: F401
        if engine == "aer-statevector":
            import qiskit_aer  # noqa: F401
        if engine == "pyzx-matrix":
            import pyzx  # noqa: F401
        if engine == "mqt-ddsim-statevector":
            import mqt.ddsim  # noqa: F401
        if engine == "qiskit-clifford":
            import qiskit  # noqa: F401
    except ImportError as exc:
        return str(exc)
    return ""


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
        "case": case.get("name") or (
            f"{case.get('mqt_algorithm', 'unknown')}-"
            f"{case.get('mqt_qubits', '?')}q-"
            f"opt{case.get('mqt_optimization_level', '?')}"
        ),
        "source": case.get("source", "internal"),
        "source_url": case.get("source_url", ""),
        "source_relative_path": case.get("source_relative_path", ""),
        "boundary": boundary_index,
        "input": input_bits,
        "output": output_bits,
        "engine": engine,
        "qubits": qubits,
        "qubit_cap": None,
        "timeout_seconds": None,
        "memory_limit_mib": None,
        "elapsed_ns": elapsed_ns,
        "amplitude_real": None if amplitude is None else amplitude.real,
        "amplitude_imag": None if amplitude is None else amplitude.imag,
        "status": status,
        "error": error or "",
    }


def annotate_limits(record: dict, args: argparse.Namespace) -> dict:
    record["qubit_cap"] = effective_max_qubits(args, str(record["engine"]))
    record["timeout_seconds"] = args.timeout
    record["memory_limit_mib"] = args.memory_limit_mib
    record["cgroup_memory_limit_mib"] = args.cgroup_memory_limit_mib
    return record


def effective_max_qubits(args: argparse.Namespace, engine: str) -> int | None:
    return args.engine_qubit_caps.get(engine, args.max_qubits)


def benchmark(args: argparse.Namespace) -> list[dict]:
    cases = json.loads(args.manifest.read_text(encoding="utf-8"))
    records = []
    engines = list(ENGINES) if args.engine == "all" else [args.engine]
    for engine in engines:
        dependency_error = engine_import_error(engine)
        max_qubits = effective_max_qubits(args, engine)
        for case, boundary_index, (input_bits, output_bits) in boundary_records(cases, args.limit):
            qasm = case_qasm(case)
            boundary_qubits = max(len(input_bits), len(output_bits))
            if dependency_error:
                records.append(
                    annotate_limits(
                        record_case(
                            case,
                            boundary_index,
                            input_bits,
                            output_bits,
                            engine,
                            "skipped",
                            qubits=boundary_qubits,
                            error=dependency_error,
                        ),
                        args,
                    )
                )
                continue
            if max_qubits is not None and boundary_qubits > max_qubits:
                records.append(
                    annotate_limits(
                        record_case(
                            case,
                            boundary_index,
                            input_bits,
                            output_bits,
                            engine,
                            "skipped",
                            qubits=boundary_qubits,
                            error=f"boundary uses {boundary_qubits} qubits above qubit cap {max_qubits}",
                        ),
                        args,
                    )
                )
                continue
            try:
                start = time.perf_counter_ns()
                with native_timeout(args.timeout):
                    amplitude, qubits = native_amplitude(engine, qasm, input_bits, output_bits)
                elapsed_ns = time.perf_counter_ns() - start
            except Exception as exc:
                if not args.skip_unsupported:
                    raise
                records.append(
                    annotate_limits(
                        record_case(
                            case,
                            boundary_index,
                            input_bits,
                            output_bits,
                            engine,
                            "skipped",
                            error=str(exc).strip().splitlines()[-1] if str(exc).strip() else repr(exc),
                        ),
                        args,
                    )
                )
                continue
            records.append(
                annotate_limits(
                    record_case(
                        case,
                        boundary_index,
                        input_bits,
                        output_bits,
                        engine,
                        "ok",
                        elapsed_ns=elapsed_ns,
                        amplitude=amplitude,
                        qubits=qubits,
                    ),
                    args,
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
    print(f"records: {len(records)}", file=file)
    limit_rows = {
        (
            record.get("qubit_cap"),
            record.get("timeout_seconds"),
            record.get("memory_limit_mib"),
            record.get("cgroup_memory_limit_mib"),
        )
        for record in records
    }
    if len(limit_rows) == 1:
        qubit_cap, timeout_seconds, memory_limit_mib, cgroup_memory_limit_mib = next(iter(limit_rows))
        print(f"qubit_cap: {qubit_cap if qubit_cap is not None else 'none'}", file=file)
        print(
            f"timeout_seconds: {timeout_seconds if timeout_seconds is not None else 'none'}",
            file=file,
        )
        print(
            f"memory_limit_mib: {memory_limit_mib if memory_limit_mib is not None else 'none'}",
            file=file,
        )
        print(
            "cgroup_memory_limit_mib: "
            f"{cgroup_memory_limit_mib if cgroup_memory_limit_mib is not None else 'none'}",
            file=file,
        )
    for engine in sorted({record["engine"] for record in records}):
        engine_records = [record for record in records if record["engine"] == engine]
        ok = [record for record in engine_records if record["status"] == "ok"]
        skipped = [record for record in engine_records if record["status"] != "ok"]
        total_elapsed = sum(int(record["elapsed_ns"] or 0) for record in ok)
        max_elapsed = max((int(record["elapsed_ns"] or 0) for record in ok), default=0)
        max_qubits = max((int(record["qubits"] or 0) for record in ok), default=0)
        print(f"engine: {engine}", file=file)
        print(f"  records: {len(engine_records)}", file=file)
        print(f"  ok: {len(ok)}", file=file)
        print(f"  skipped: {len(skipped)}", file=file)
        print(f"  total_elapsed_ns: {total_elapsed}", file=file)
        print(f"  max_elapsed_ns: {max_elapsed}", file=file)
        print(f"  max_qubits: {max_qubits}", file=file)


def parse_engine_qubit_cap(text: str) -> tuple[str, int]:
    if "=" not in text:
        raise argparse.ArgumentTypeError("engine cap must use ENGINE=N")
    engine, value_text = text.split("=", 1)
    if engine not in ENGINES:
        raise argparse.ArgumentTypeError(f"unsupported engine {engine!r}")
    try:
        value = int(value_text)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("engine qubit cap must be an integer") from exc
    if value < 0:
        raise argparse.ArgumentTypeError("engine qubit cap must be non-negative")
    return engine, value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Benchmark native statevector simulators on QASM manifest fixed-boundary amplitudes."
    )
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--engine", choices=ENGINES + ("all",), default="qiskit-statevector")
    parser.add_argument("--format", choices=("jsonl", "csv", "summary"), default="jsonl")
    parser.add_argument("--limit", type=int)
    parser.add_argument("--max-qubits", type=int, help="skip boundaries above this dense-state qubit count")
    parser.add_argument(
        "--engine-qubit-cap",
        action="append",
        type=parse_engine_qubit_cap,
        default=[],
        metavar="ENGINE=N",
        help="override --max-qubits for one engine; may be repeated",
    )
    parser.add_argument("--timeout", type=float, help="per-boundary native simulator timeout in seconds")
    parser.add_argument("--memory-limit-mib", type=int, help="process address-space cap for native simulator runs")
    parser.add_argument("--cgroup-memory-limit-mib", type=int,
                        help="cgroup physical-memory cap applied by the orchestrator and recorded in output rows")
    parser.add_argument("--skip-unsupported", action="store_true")
    args = parser.parse_args(argv)
    if args.limit is not None and args.limit < 0:
        parser.error("--limit must be non-negative")
    if args.max_qubits is not None and args.max_qubits < 0:
        parser.error("--max-qubits must be non-negative")
    args.engine_qubit_caps = {}
    for engine, cap in args.engine_qubit_cap:
        if engine in args.engine_qubit_caps:
            parser.error(f"duplicate --engine-qubit-cap for {engine}")
        args.engine_qubit_caps[engine] = cap
    if args.timeout is not None and args.timeout <= 0:
        parser.error("--timeout must be positive")
    if args.memory_limit_mib is not None and args.memory_limit_mib <= 0:
        parser.error("--memory-limit-mib must be positive")
    if args.cgroup_memory_limit_mib is not None and args.cgroup_memory_limit_mib <= 0:
        parser.error("--cgroup-memory-limit-mib must be positive")
    return args


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    apply_memory_limit(args.memory_limit_mib)
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
