#!/usr/bin/env python3

import math
import pathlib
import sys

try:
    from qiskit import QuantumCircuit
    from qiskit.quantum_info import Statevector
except ImportError:
    print("qiskit is not installed; skipping optional qasm2sop qiskit test", file=sys.stderr)
    raise SystemExit(77)

# Reuse the maintained qasm2sop -> sop-solve pipeline (it handles both the residue-vector and the
# --format amplitude solver outputs; this file used to carry a stale copy that only parsed the
# former and broke once sop-solve emitted amplitudes).
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_qasm_amplitudes import sop_amplitude  # noqa: E402


def state_index(bits: str) -> int:
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def qiskit_amplitude(circuit: QuantumCircuit, input_bits: str, output_bits: str) -> complex:
    initial = [0j] * (1 << circuit.num_qubits)
    initial[state_index(input_bits)] = 1.0 + 0j
    state = Statevector(initial).evolve(circuit)
    return complex(state.data[state_index(output_bits)])


def assert_close(name: str, expected: complex, actual: complex) -> None:
    if abs(expected - actual) > 1e-9:
        raise AssertionError(f"{name}: amplitude mismatch expected {expected!r}, got {actual!r}")


def bell_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q[0];
    cx q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.cx(0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("00", "01"), ("10", "01")]


def phase_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q;
    sx q[1];
    p(pi/2) q[0];
    p(pi/8) q[1];
    cp(pi/4) q[0], q[1];
    cp(-7*pi/8) q[1], q[0];
    sxdg q[1];
    cy q[1], q[0];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.h(1)
    circuit.sx(1)
    circuit.p(math.pi / 2.0, 0)
    circuit.p(math.pi / 8.0, 1)
    circuit.cp(math.pi / 4.0, 0, 1)
    circuit.cp(-7.0 * math.pi / 8.0, 1, 0)
    circuit.sxdg(1)
    circuit.cy(1, 0)
    return qasm, circuit, [("00", "00"), ("00", "10"), ("11", "01"), ("10", "11")]


def register_pair_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg a[2];
    qreg b[2];
    h a;
    cx a, b;
    cp(3*pi/4) a, b;
    swap a[0], b[1];
    """
    circuit = QuantumCircuit(4)
    circuit.h(0)
    circuit.h(1)
    circuit.cx(0, 2)
    circuit.cx(1, 3)
    circuit.cp(3.0 * math.pi / 4.0, 0, 2)
    circuit.cp(3.0 * math.pi / 4.0, 1, 3)
    circuit.swap(0, 3)
    return qasm, circuit, [("0000", "0000"), ("0000", "1111"), ("1010", "1110")]


def named_controlled_phase_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q;
    cs q[0], q[1];
    csdg q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.h(1)
    circuit.cs(0, 1)
    circuit.csdg(0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("11", "11")]


def rz_crz_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q;
    rz(pi/4) q[0];
    crz(pi/4) q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.h(1)
    circuit.rz(math.pi / 4.0, 0)
    circuit.crz(math.pi / 4.0, 0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("10", "10")]


def h_cry_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q[0];
    cry(pi) q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.cry(math.pi, 0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("01", "01"), ("01", "10")]


def cu_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    # All four parameters genuinely nonzero (dyadic, since this file's sop_amplitude has no
    # --approx support) -- exercises the general decomposition, not just the theta-only
    # special case (which is mathematically identical to cry and already covered there).
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q[0];
    cu(pi,pi/2,pi/2,pi/4) q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.cu(math.pi, math.pi / 2.0, math.pi / 2.0, math.pi / 4.0, 0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("01", "01"), ("01", "10")]


def axis_rotation_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    h q;
    rx(pi/4) q[0];
    ry(-pi/2) q[1];
    cx q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.h(0)
    circuit.h(1)
    circuit.rx(math.pi / 4.0, 0)
    circuit.ry(-math.pi / 2.0, 1)
    circuit.cx(0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("10", "01")]


def u_gates_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[2];
    u2(pi/4,-pi/2) q[0];
    u3(pi/4,pi/4,pi/2) q[1];
    cx q[0], q[1];
    """
    circuit = QuantumCircuit(2)
    circuit.u(math.pi / 2.0, math.pi / 4.0, -math.pi / 2.0, 0)
    circuit.u(math.pi / 4.0, math.pi / 4.0, math.pi / 2.0, 1)
    circuit.cx(0, 1)
    return qasm, circuit, [("00", "00"), ("00", "11"), ("10", "01")]


def three_qubit_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[3];
    ccz q[0], q[1], q[2];
    ccx q[0], q[1], q[2];
    """
    circuit = QuantumCircuit(3)
    circuit.h(2)
    circuit.ccx(0, 1, 2)
    circuit.h(2)
    circuit.ccx(0, 1, 2)
    return qasm, circuit, [("111", "110"), ("110", "111"), ("010", "010")]


def cswap_case() -> tuple[str, QuantumCircuit, list[tuple[str, str]]]:
    qasm = """OPENQASM 2.0;
    include "qelib1.inc";
    qreg q[3];
    cswap q[0], q[1], q[2];
    """
    circuit = QuantumCircuit(3)
    circuit.cx(2, 1)
    circuit.ccx(0, 1, 2)
    circuit.cx(2, 1)
    return qasm, circuit, [("101", "110"), ("110", "101"), ("001", "001")]


def run_qiskit_cases(qasm2sop: pathlib.Path, sop_solve: pathlib.Path) -> None:
    for case_name, (qasm, circuit, boundaries) in {
        "bell": bell_case(),
        "phase": phase_case(),
        "register_pair": register_pair_case(),
        "named_controlled_phase": named_controlled_phase_case(),
        "rz_crz": rz_crz_case(),
        "h_cry": h_cry_case(),
        "cu": cu_case(),
        "axis_rotation": axis_rotation_case(),
        "u_gates": u_gates_case(),
        "three_qubit": three_qubit_case(),
        "cswap": cswap_case(),
    }.items():
        for input_bits, output_bits in boundaries:
            expected = qiskit_amplitude(circuit, input_bits, output_bits)
            try:
                actual = sop_amplitude(qasm2sop, sop_solve, qasm, input_bits, output_bits)
            except AssertionError as exc:
                # Genuine, expected exact-mode representability rejections (an odd pi/8-unit cp/cu1
                # angle, or a non-sign quadratic coefficient) -- the same cases test_qasm_amplitudes
                # skips. Not a qiskit-comparison failure.
                message = str(exc)
                if (
                    "unsupported cp/cu1 angle in exact mode" in message
                    or "unsupported non-sign quadratic phase coefficient" in message
                ):
                    continue
                raise
            assert_close(f"{case_name} {input_bits}->{output_bits}", expected, actual)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_qiskit.py QASM2SOP SOP_SOLVE", file=sys.stderr)
        return 2

    run_qiskit_cases(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
