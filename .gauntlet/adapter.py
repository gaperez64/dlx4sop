#!/usr/bin/env python3
"""qccq-gauntlet adapter for dlx4sop's branch/treewidth/rankwidth backends.

Speaks the inferq.zero-amplitude.v1 protocol (see qccq-gauntlet's
docs/adapter-protocol.md): for each request, computes <0^n|C|0^n> by piping
qasm2sop's output into `sop-solve --solve-mode single-fourier`, the mode that
keeps working when qasm2sop --approx produces a modulus too large for the
default count-table mode's O(r) tables. qasm2sop is tried exactly first
since that yields the smallest, most precise modulus; --approx only kicks in
if the circuit is not exactly representable in the qsop-sign format.

single-fourier's reported amplitude is deliberately unnormalized (see
qsop_amplitude_t in qsop_solve.h) -- callers must scale it by
2**(-norm_h/2) themselves, where norm_h comes from the qsop file's own
header, not from sop-solve's output.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import warnings

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import build_external_qasm_manifest as manifest_tool  # noqa: E402

QASM2SOP = REPO_ROOT / "build" / "qasm2sop"
SOP_SOLVE = REPO_ROOT / "build" / "sop-solve"

# Non-unitary operations the protocol requires rejecting outright (final
# measurements are stripped separately, before this check runs).
FORBIDDEN_OPS = {"measure", "reset", "initialize", "set_density_matrix", "set_statevector"}

# Matches the gauntlet's own zero-amplitude tolerance (1e-8 absolute+relative
# in every real suite bar "smoke"), so the modulus this buys is exactly what
# a real registration needs -- see scripts/scan_gauntlet_coverage.py.
APPROX_EPSILON = 1e-8

# Comfortably under the harness's 120s per-case window (docs/operations.md),
# leaving headroom for QPY loading and QASM dumping in the same budget.
SUBPROCESS_TIMEOUT_S = 100.0


def forbidden_ops_present(circuit) -> set[str]:
    return {instr.operation.name for instr in circuit.data if instr.operation.name in FORBIDDEN_OPS}


def strip_unused_clbits(circuit):
    """Drop classical registers no instruction actually touches.

    Several MQT-derived QPY payloads carry a dangling `creg` declaration left
    over from textual measurement-stripping upstream (the source QASM's
    `measure`/`if` lines were removed but the `creg` line was not). qasm2sop's
    parser hard-rejects any `creg`/`measure` line as a dynamic/classical
    feature, even when the register is provably inert, so this preprocessing
    is required to avoid a false "non-unitary" rejection.
    """
    if circuit.num_clbits == 0:
        return circuit
    live = any(
        len(instr.clbits) > 0 or getattr(instr.operation, "condition", None) is not None
        for instr in circuit.data
    )
    if live:
        return circuit
    from qiskit import QuantumCircuit

    stripped = QuantumCircuit(*circuit.qregs)
    for instr in circuit.data:
        stripped.append(instr.operation, instr.qubits, [])
    return stripped


def load_circuit(payload_path: str):
    from qiskit import qpy

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with open(payload_path, "rb") as handle:
            circuits = qpy.load(handle)
    circuit = circuits[0].copy()
    circuit.remove_final_measurements(inplace=True)
    circuit = strip_unused_clbits(circuit)
    forbidden = forbidden_ops_present(circuit)
    if forbidden or circuit.num_clbits:
        raise ValueError(
            f"non-unitary payload: forbidden ops {sorted(forbidden)}, clbits={circuit.num_clbits}"
        )
    return circuit


def import_qsop(qasm_text: str, zero: str) -> tuple[str, dict]:
    """Import qasm_text at the all-zero boundary. Tries an exact import first,
    falling back to --approx only if qasm2sop cannot represent it exactly."""
    exact = subprocess.run(
        [str(QASM2SOP), "--input", zero, "--output", zero, "-"],
        input=qasm_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if exact.returncode == 0:
        return exact.stdout, {"import_mode": "exact"}
    exact_diagnostic = manifest_tool.diagnostic_from_exception(RuntimeError(exact.stderr))

    approx = subprocess.run(
        [str(QASM2SOP), "--approx", repr(APPROX_EPSILON), "--input", zero, "--output", zero, "-"],
        input=qasm_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if approx.returncode != 0:
        approx_diagnostic = manifest_tool.diagnostic_from_exception(RuntimeError(approx.stderr))
        raise RuntimeError(
            f"qasm2sop failed both exactly and with --approx {APPROX_EPSILON:g}: "
            f"exact={exact_diagnostic!r} approx={approx_diagnostic!r}"
        )
    return approx.stdout, {"import_mode": "approx", "approx_epsilon": APPROX_EPSILON}


def parse_norm_h(qsop_text: str) -> int:
    for line in qsop_text.splitlines():
        parts = line.split()
        if parts[:1] == ["n"]:
            return int(parts[1])
    raise RuntimeError(f"qsop output has no 'n' (norm_h) header line:\n{qsop_text}")


def parse_amplitude(sop_solve_output: str) -> complex:
    values: dict[str, str] = {}
    for line in sop_solve_output.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            values[key.strip()] = value.strip()
    if "amplitude_re" not in values or "amplitude_im" not in values:
        raise RuntimeError(f"malformed sop-solve output:\n{sop_solve_output}")
    return complex(float(values["amplitude_re"]), float(values["amplitude_im"]))


def solve(backend: str, qasm_text: str, nqubits: int) -> tuple[complex, dict]:
    zero = "0" * nqubits
    qsop_text, metrics = import_qsop(qasm_text, zero)
    norm_h = parse_norm_h(qsop_text)

    completed = subprocess.run(
        [str(SOP_SOLVE), "--backend", backend, "--solve-mode", "single-fourier", "-"],
        input=qsop_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"sop-solve --backend {backend} failed: {completed.stderr.strip()}")

    raw = parse_amplitude(completed.stdout)
    metrics["norm_h"] = norm_h
    return raw * (2.0 ** (-norm_h / 2.0)), metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", required=True, choices=["branch", "treewidth", "rankwidth"])
    args = parser.parse_args()

    if not QASM2SOP.is_file() or not SOP_SOLVE.is_file():
        raise SystemExit(
            f"missing build output: expected {QASM2SOP} and {SOP_SOLVE} (did bootstrap run?)"
        )

    print(json.dumps({"schema": "gauntlet.ready.v1"}), flush=True)
    request = json.load(sys.stdin)

    circuit = load_circuit(request["payload"])
    import qiskit.qasm2 as qasm2mod

    qasm_text = qasm2mod.dumps(circuit)
    qasm_text = manifest_tool.inline_simple_gates(qasm_text)
    nqubits = manifest_tool.qasm_qubits(qasm_text)

    value, metrics = solve(args.backend, qasm_text, nqubits)
    print(
        json.dumps(
            {
                "schema": "gauntlet.result.v1",
                "value": {"re": value.real, "im": value.imag},
                "metrics": metrics,
            }
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
