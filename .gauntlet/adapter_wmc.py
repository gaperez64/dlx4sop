#!/usr/bin/env python3
"""qccq-gauntlet adapter for dlx4sop's sop2wmc + upstream ganak pipeline.

Speaks the inferq.zero-amplitude.v1 protocol (see qccq-gauntlet's
docs/adapter-protocol.md): for each request, computes <0^n|C|0^n> by piping
qasm2sop's output into `sop2wmc --encoding amp-and` (a single weighted-CNF
export with an embedded `amplitude_factor`), then into ganak's
`--mode 6` mpfr-complex model counter -- the same pipeline validated by
scripts/bench_wmc_ganak.py, whose parsing helpers this module reuses
directly. Frontend circuit handling (QPY loading, clbit stripping, QASM
conversion, exact-then-approx qasm2sop import) is shared with adapter.py.

ganak's binary comes from upstream meelgroup/ganak's own release artifacts
(fetched and pinned by bootstrap-wmc.sh) rather than a source build: mode 6
complex counting is stock functionality, not specific to any fork, and the
upstream project explicitly recommends release binaries over building from
source (its CMake build pulls in cryptominisat5/arjun/approxmc via network
FetchContent and needs GMP+GMPXX already installed).
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
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bench_wmc_ganak as wmc  # noqa: E402
import build_external_qasm_manifest as manifest_tool  # noqa: E402
from adapter import (  # noqa: E402
    forbidden_ops_present,
    import_qsop,
    parse_norm_h,
    strip_unused_clbits,
)

SOP2WMC = REPO_ROOT / "build" / "sop2wmc"
GANAK = REPO_ROOT / ".gauntlet" / "ganak" / "ganak"

# amp-and: single WPCNF with Tseitin AND auxiliaries, one ganak --mode 6 call
# per case. Chosen over amp-soft/amp-block (untuned alternative encodings)
# and residue (r separate plain-#SAT calls plus an error-prone DFT
# recombination on our end -- see scripts/bench_wmc_ganak.py's
# counts_to_amplitude precision workarounds) as the simplest, most direct
# path to a single complex amplitude.
ENCODING = "amp-and"

# Comfortably under the harness's 120s per-case window (docs/operations.md),
# leaving headroom for QPY loading, QASM dumping, and two subprocess calls
# in the same budget.
SUBPROCESS_TIMEOUT_S = 100.0


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


def parse_ganak_complex_decimal(text: str) -> tuple[Decimal, Decimal]:
    """Like bench_wmc_ganak.parse_ganak_complex, but keeps full decimal precision.

    ganak's raw weighted count can reach ~1e300+ on deep circuits, well past a
    Python float's ~1.8e308 range and, short of outright overflow, past its
    ~15-17 significant digits of precision -- either way losing digits that
    matter once the count is scaled back down by 2**(-norm_h/2). Deferring the
    float conversion until after that scaling (see solve() below) avoids both.
    """
    for line in text.splitlines():
        stripped = line.strip()
        match = wmc.GANAK_COMPLEX_PATTERN.match(stripped)
        if match:
            return Decimal(match.group(1)), Decimal(match.group(2) or "0")
    raise ValueError(f"could not parse ganak complex output:\n{text}")


def solve(qasm_text: str, nqubits: int) -> tuple[complex, dict]:
    zero = "0" * nqubits
    qsop_text, metrics = import_qsop(qasm_text, zero)
    norm_h = parse_norm_h(qsop_text)

    exported = subprocess.run(
        [str(SOP2WMC), "--encoding", ENCODING, "-"],
        input=qsop_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if exported.returncode != 0:
        raise RuntimeError(f"sop2wmc --encoding {ENCODING} failed: {exported.stderr.strip()}")

    cnf_text = exported.stdout
    metrics.update(wmc.parse_wmc_metadata(cnf_text))
    metrics["norm_h"] = norm_h
    factor = wmc.parse_amplitude_factor(cnf_text)

    if factor == 0j:
        return 0j, metrics
    if wmc.is_zero_residual_wmc(metrics):
        return wmc.normalize_amplitude(factor, norm_h), metrics

    counted = subprocess.run(
        [str(GANAK), "--mode", "6", "--verb", "0", "-"],
        input=cnf_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_S,
    )
    if counted.returncode != 0:
        raise RuntimeError(f"ganak --mode 6 failed: {counted.stderr.strip()}")
    raw_re, raw_im = parse_ganak_complex_decimal(counted.stdout + counted.stderr)
    getcontext().prec = max(50, norm_h // 2 + 50)
    factor_re, factor_im = Decimal(factor.real), Decimal(factor.imag)
    amp_re = raw_re * factor_re - raw_im * factor_im
    amp_im = raw_re * factor_im + raw_im * factor_re
    scale = Decimal(2) ** (Decimal(-norm_h) / 2)
    return complex(float(amp_re * scale), float(amp_im * scale)), metrics


def main() -> int:
    argparse.ArgumentParser().parse_args()

    if not SOP2WMC.is_file():
        raise SystemExit(f"missing build output: expected {SOP2WMC} (did bootstrap run?)")
    if not GANAK.is_file():
        raise SystemExit(f"missing ganak binary: expected {GANAK} (did bootstrap run?)")

    print(json.dumps({"schema": "gauntlet.ready.v1"}), flush=True)
    request = json.load(sys.stdin)

    circuit = load_circuit(request["payload"])
    import qiskit.qasm2 as qasm2mod

    qasm_text = qasm2mod.dumps(circuit)
    qasm_text = manifest_tool.inline_simple_gates(qasm_text)
    nqubits = manifest_tool.qasm_qubits(qasm_text)

    value, metrics = solve(qasm_text, nqubits)
    # OpenQASM 2 has no instruction for a circuit-level global phase, so it's lost
    # in the qasm2mod.dumps() round-trip above; sop2wmc/ganak then compute the
    # right amplitude *relative to that dropped phase*. Reapply it here -- it's a
    # property of the original circuit object, not something the qsop pipeline can
    # recover from QASM text alone. See adapter.py's solve() for the same fix.
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
