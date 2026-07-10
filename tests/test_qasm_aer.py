#!/usr/bin/env python3

"""Physical cross-check: qiskit-aer (matrix-product-state) vs dlx4sop on measure/reset circuits.

qasm2sop lowers a no-feed-forward `measure` as a coherent no-op and `reset` as a fresh |0> wire
with the pre-reset value free-summed. That coherent convention equals a *physical* simulation
exactly when the measured/reset qubit is in a definite computational-basis state at that point --
the regime real ancilla-recycle / error-correction circuits (and the whole alg85 corpus) live in.

This test builds deterministic circuits in that regime, simulates them physically with aer-MPS
(which treats measure/reset as genuine collapse/reset), and asserts the amplitude matches
dlx4sop's coherent result. A determinism guard (running aer twice) rejects any case that strays
out of the coherent=physical regime, so the test can never silently pass on a bad case.
"""

import pathlib
import sys

try:
    import numpy as np
    from qiskit import QuantumCircuit, qasm2
    from qiskit_aer import AerSimulator
except ImportError:
    print("qiskit-aer is not installed; skipping optional qasm2sop aer test", file=sys.stderr)
    raise SystemExit(77)

# Reuse the qasm2sop -> sop-solve amplitude pipeline.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from test_qasm_amplitudes import sop_amplitude  # noqa: E402

SIM = AerSimulator(method="matrix_product_state")


def state_index(bits: str) -> int:
    # little-endian, matching sop_amplitude / qiskit statevector ordering
    return sum((1 << i) for i, bit in enumerate(bits) if bit == "1")


def aer_statevector(circuit: QuantumCircuit) -> np.ndarray:
    qc = circuit.copy()
    qc.save_statevector()
    result = SIM.run(qc, shots=1).result()
    return np.asarray(result.get_statevector())


def recycle_phase() -> tuple[str, QuantumCircuit, list[str]]:
    # Data qubit picks up a real phase (i) and returns to |0>; a separate ancilla is driven to |1>,
    # measured, then reset back to |0> -- a genuine |1>->|0> reset on a definite-state qubit. The
    # nonzero output is |00> with amplitude i, exercising a nontrivial *complex* amplitude.
    qc = QuantumCircuit(2, 1)
    qc.x(0)
    qc.s(0)
    qc.x(0)  # q0 = i|0>
    qc.x(1)  # ancilla q1 = |1>
    qc.measure(1, 0)  # definite outcome 1, coherent no-op
    qc.reset(1)  # |1> -> |0>
    return qasm2.dumps(qc), qc, ["00", "01", "10", "11"]


def measure_midcircuit() -> tuple[str, QuantumCircuit, list[str]]:
    # A mid-circuit measurement (no feed-forward) on a definite-state qubit, followed by a gate that
    # uses that qubit -- checks the measurement is a true no-op.
    qc = QuantumCircuit(2, 1)
    qc.x(0)  # q0 = |1>
    qc.measure(0, 0)  # definite 1, no-op
    qc.cx(0, 1)  # q1 = 1
    return qasm2.dumps(qc), qc, ["00", "01", "10", "11"]


def bitflip_code_no_error() -> tuple[str, QuantumCircuit, list[str]]:
    # 3-qubit bit-flip code, no error injected: encode |0_L>, extract both parity syndromes into two
    # ancillas (deterministically 0), measure + reset the ancillas, decode. Ends in |00000> with
    # amplitude 1. Exercises several measure+reset on deterministically-zero ancillas.
    qc = QuantumCircuit(5, 2)  # q0..q2 data, q3/q4 syndrome ancillas
    qc.cx(0, 1)
    qc.cx(0, 2)  # encode (data is |0>, stays |000>)
    qc.cx(0, 3)
    qc.cx(1, 3)  # parity(q0,q1) -> q3
    qc.cx(1, 4)
    qc.cx(2, 4)  # parity(q1,q2) -> q4
    qc.measure(3, 0)
    qc.measure(4, 1)
    qc.reset(3)
    qc.reset(4)
    qc.cx(0, 2)
    qc.cx(0, 1)  # decode
    return qasm2.dumps(qc), qc, ["00000", "00001", "00010"]


def run_aer_cases(qasm2sop: pathlib.Path, sop_solve: pathlib.Path) -> None:
    cases = {
        "recycle_phase": recycle_phase(),
        "measure_midcircuit": measure_midcircuit(),
        "bitflip_code_no_error": bitflip_code_no_error(),
    }
    checks = 0
    for name, (qasm, circuit, outputs) in cases.items():
        sv1 = aer_statevector(circuit)
        sv2 = aer_statevector(circuit)
        if not np.allclose(sv1, sv2, atol=1e-9):
            raise AssertionError(
                f"{name}: aer statevector is non-deterministic -- case is outside the "
                f"coherent=physical regime and cannot cross-check"
            )
        zero = "0" * circuit.num_qubits
        for output_bits in outputs:
            physical = complex(sv1[state_index(output_bits)])
            coherent = sop_amplitude(qasm2sop, sop_solve, qasm, zero, output_bits)
            if abs(physical - coherent) > 1e-6:
                raise AssertionError(
                    f"{name} ->{output_bits}: aer(physical)={physical!r} "
                    f"dlx4sop(coherent)={coherent!r}"
                )
            checks += 1
    if checks == 0:
        raise AssertionError("no aer cross-check cases were exercised")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: test_qasm_aer.py QASM2SOP SOP_SOLVE", file=sys.stderr)
        return 2
    run_aer_cases(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
