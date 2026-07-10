#!/usr/bin/env python3
"""qccq-gauntlet adapter for dlx4sop's branch backend.

Speaks the inferq.zero-amplitude.v1 protocol (see qccq-gauntlet's
docs/adapter-protocol.md): for each request, computes <0^n|C|0^n> by piping
qasm2sop's output into `sop-solve --format amplitude`. qasm2sop is tried
exactly first since that yields the
smallest, most precise modulus; --approx only kicks in if the circuit is not
exactly representable in the qsop-sign format.

single-fourier's reported amplitude is deliberately unnormalized (see
qsop_amplitude_t in qsop_solve.h) -- callers must scale it by
2**(-norm_h/2) themselves, where norm_h comes from the qsop file's own
header, not from sop-solve's output.
"""

from __future__ import annotations

import argparse
import cmath
import json
import pathlib
import subprocess
import sys
import warnings
from decimal import Decimal, getcontext

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
# a real registration needs.
APPROX_EPSILON = 1e-8

# Slightly above the harness's 120s per-case window so gauntlet, not an inner
# subprocess timeout, owns timeout classification during benchmark runs.
SUBPROCESS_TIMEOUT_S = 125.0


def forbidden_ops_present(circuit) -> set[str]:
    return {instr.operation.name for instr in circuit.data if instr.operation.name in FORBIDDEN_OPS}


def load_circuit(payload_path: str):
    from qiskit import qpy

    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        with open(payload_path, "rb") as handle:
            circuits = qpy.load(handle)
    circuit = circuits[0].copy()
    circuit.remove_final_measurements(inplace=True)
    # Reject only genuinely non-unitary ops (measure/reset/initialize/set_*). A dangling
    # classical register -- e.g. an MQT payload whose measurements were stripped upstream
    # but left the `creg` behind -- is left in place: qasm2sop ignores an inert `creg` and
    # still rejects a real `if(creg) gate` / `measure` / `reset`, so it is the backstop
    # that keeps dynamic circuits out while letting the inert register through. This
    # replaces an earlier circuit-level clbit strip that crashed on some payloads.
    forbidden = forbidden_ops_present(circuit)
    if forbidden:
        raise ValueError(f"non-unitary payload: forbidden ops {sorted(forbidden)}")
    return circuit


def _is_phase_representability_error(stderr: str) -> bool:
    """--approx only rescues circuits that fail the *exact* import because a phase/angle falls
    outside qasm2sop's finite grid (odd cp/cu1, non-pi/8 rz/rx/ry/u/p, non-sign quadratic). It
    does NOT add gate support or fix parse / non-unitary (creg/measure) rejections, so retrying
    those with --approx just burns a second qasm2sop run and yields a misleading combined error.
    Every exact-mode angle rejection names "angle" or "non-sign quadratic"; nothing else does."""
    return "angle" in stderr or "non-sign quadratic" in stderr


def import_qsop(qasm_text: str, zero: str) -> tuple[str, dict]:
    """Import qasm_text at the all-zero boundary. Tries an exact import first,
    falling back to --approx only if the exact failure is a phase/angle-representability
    issue (the only thing --approx can rescue). Shared by adapter.py and adapter_wmc.py."""
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
    if not _is_phase_representability_error(exact.stderr):
        raise RuntimeError(f"qasm2sop failed and is not phase-approximable: {exact_diagnostic!r}")

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


def parse_amplitude(sop_solve_output: str) -> tuple[Decimal, Decimal]:
    values: dict[str, str] = {}
    for line in sop_solve_output.splitlines():
        key, sep, value = line.partition(":")
        if sep:
            values[key.strip()] = value.strip()
    if "amplitude_re" not in values or "amplitude_im" not in values:
        raise RuntimeError(f"malformed sop-solve output:\n{sop_solve_output}")
    return Decimal(values["amplitude_re"]), Decimal(values["amplitude_im"])


def solve(qasm_text: str, nqubits: int) -> tuple[complex, dict]:
    zero = "0" * nqubits
    qsop_text, metrics = import_qsop(qasm_text, zero)
    norm_h = parse_norm_h(qsop_text)

    command = [
        str(SOP_SOLVE),
        "--format",
        "amplitude",
    ]
    command.append("-")

    completed = subprocess.run(
        command,
        input=qsop_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"sop-solve failed: {completed.stderr.strip()}")
    if "memory-skip" in completed.stderr:
        raise RuntimeError(f"sop-solve: {completed.stderr.strip()}")

    amp_re, amp_im = parse_amplitude(completed.stdout)
    metrics["norm_h"] = norm_h
    # sop-solve --format amplitude already reports the normalized amplitude,
    # amp * 2**(-norm_h/2), whose modulus is at most 1. It used to report the raw
    # sum-over-paths value, which scales like 2**(norm_h/2) and overflows a Python
    # float once norm_h passes ~2048 -- hence the arbitrary-precision rescale that
    # used to live here. The solver now carries the magnitude in a binary exponent
    # and normalizes internally, so there is nothing left to undo.
    return complex(float(amp_re), float(amp_im)), metrics


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend", default="branch", choices=["branch"], help=argparse.SUPPRESS)
    parser.parse_args()

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

    value, metrics = solve(qasm_text, nqubits)
    # OpenQASM 2 has no instruction for a circuit-level global phase, so it's lost
    # in the qasm2mod.dumps() round-trip above; qasm2sop/sop-solve then compute the
    # right amplitude *relative to that dropped phase*. Reapply it here -- it's a
    # property of the original circuit object, not something the qsop pipeline can
    # recover from QASM text alone.
    value *= cmath.exp(1j * float(circuit.global_phase))
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
