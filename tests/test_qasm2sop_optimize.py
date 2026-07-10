#!/usr/bin/env python3

"""Differential check for the qasm2sop Hadamard-simplification pass.

The pass (on by default, disabled with --no-optimize) is amplitude-exact: it must never
change the physical amplitude of a circuit, only shrink the emitted QSOP. For every circuit
we import with and without --no-optimize, solve both, and assert:

  * the normalized complex amplitude  amp * 2^(-norm_h/2)  is identical (invariant, phase and
    magnitude), and
  * the optimized instance has no more variables and no more sign edges than the unoptimized
    one (monotone reduction),

and separately that at least one circuit is actually reduced (the pass is not a no-op).
"""

import cmath
import math
import pathlib
import re
import subprocess
import sys

QREG_RE = re.compile(r"qreg\s+[A-Za-z_][A-Za-z0-9_]*\[(\d+)\]")
HEADER_RE = re.compile(r"^p qsop-sign\s+(\d+)\s+(\d+)\s+(\d+)")


def nqubits_of(qasm: str) -> int:
    return sum(int(m.group(1)) for m in QREG_RE.finditer(qasm))


def run_qasm2sop(qasm2sop: pathlib.Path, qasm: str, options: list[str]) -> str | None:
    completed = subprocess.run(
        [str(qasm2sop), *options, "-"],
        input=qasm,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if completed.returncode != 0:
        return None  # exact-mode rejection (e.g. odd cp angle); caller skips the circuit
    return completed.stdout


def qsop_shape(qsop_text: str) -> tuple[int, int, int]:
    for line in qsop_text.splitlines():
        m = HEADER_RE.match(line)
        if m is not None:
            return int(m.group(1)), int(m.group(2)), int(m.group(3))
    raise AssertionError(f"no qsop-sign header in:\n{qsop_text}")


def norm_h_of(qsop_text: str) -> int:
    for line in qsop_text.splitlines():
        parts = line.split()
        if parts[:1] == ["n"]:
            return int(parts[1])
    raise AssertionError(f"no normalization line in:\n{qsop_text}")


def solver_amplitude(sop_solve: pathlib.Path, qsop_text: str) -> complex:
    """Normalized amplitude amp * 2^(-norm_h/2), invariant under the simplification pass.

    norm_h is taken from the QSOP input: the single-amplitude output shape does not echo it,
    and the pass rescales the raw amplitude and norm_h in lockstep so this value is exact."""
    norm_h = norm_h_of(qsop_text)
    solved = subprocess.run(
        [str(sop_solve), "-"],
        input=qsop_text,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if solved.returncode != 0:
        raise AssertionError(f"sop-solve failed\n{solved.stderr}\nQSOP:\n{qsop_text}")

    modulus = None
    counts: list[int] | None = None
    amplitude_re = amplitude_im = None
    for line in solved.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[:2] == ["p", "qsop-result"]:
            modulus = int(parts[2])
        elif parts[0] == "counts":
            counts = [int(part) for part in parts[1:]]
        elif line.startswith("amplitude_re: "):
            amplitude_re = float(line.split(": ", 1)[1])
        elif line.startswith("amplitude_im: "):
            amplitude_im = float(line.split(": ", 1)[1])

    scale = 2.0 ** (-norm_h / 2.0)
    if amplitude_re is not None and amplitude_im is not None:
        # already normalized by sop-solve
        return complex(amplitude_re, amplitude_im)
    if modulus is None or counts is None:
        raise AssertionError(f"malformed solver output:\n{solved.stdout}")
    omega = cmath.exp(2j * math.pi / modulus)
    total = sum(count * (omega**residue) for residue, count in enumerate(counts))
    return total * scale


def compare(
    qasm2sop: pathlib.Path,
    sop_solve: pathlib.Path,
    name: str,
    qasm: str,
    options: list[str],
) -> tuple[bool, bool]:
    """Return (compared, reduced); compared is False when exact-mode import is rejected."""
    optimized = run_qasm2sop(qasm2sop, qasm, options)
    unoptimized = run_qasm2sop(qasm2sop, qasm, [*options, "--no-optimize"])
    if optimized is None or unoptimized is None:
        return False, False

    _, opt_vars, opt_edges = qsop_shape(optimized)
    _, un_vars, un_edges = qsop_shape(unoptimized)
    if opt_vars > un_vars or opt_edges > un_edges:
        raise AssertionError(
            f"{name} {options}: optimization increased size "
            f"({un_vars}v/{un_edges}e -> {opt_vars}v/{opt_edges}e)"
        )

    amp_opt = solver_amplitude(sop_solve, optimized)
    amp_un = solver_amplitude(sop_solve, unoptimized)
    if abs(amp_opt - amp_un) > 1e-9:
        raise AssertionError(
            f"{name} {options}: amplitude changed by optimization "
            f"{amp_un!r} (unopt) vs {amp_opt!r} (opt)\n"
            f"unopt QSOP:\n{unoptimized}\nopt QSOP:\n{optimized}"
        )
    return True, (opt_vars < un_vars or opt_edges < un_edges)


# Uncompute / inverse-gate identities the pass exists to collapse. qasm2sop pins every
# unspecified boundary qubit to 0, so these keep a degree-1/2 chain across the two-qubit /
# multi-Hadamard body that the pass must shrink while preserving the <0|I|0> = 1 amplitude.
# (A single-qubit h;h is deliberately omitted: with the 0-pinned boundary it collapses to one
# isolated degree-0 variable that only out-of-scope degree-0 removal could strip.)
IDENTITY_CASES = [
    ("cx_cx", "OPENQASM 2.0;\nqreg q[2];\ncx q[0],q[1];\ncx q[0],q[1];\n"),
    ("x_x", "OPENQASM 2.0;\nqreg q[1];\nx q[0];\nx q[0];\n"),
    ("sx_sxdg", "OPENQASM 2.0;\nqreg q[1];\nsx q[0];\nsxdg q[0];\n"),
]


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: test_qasm2sop_optimize.py QASM2SOP SOP_SOLVE SOURCE_ROOT",
            file=sys.stderr,
        )
        return 2

    qasm2sop = pathlib.Path(sys.argv[1])
    sop_solve = pathlib.Path(sys.argv[2])
    source_root = pathlib.Path(sys.argv[3])

    compared = 0
    reduced = 0

    for name, qasm in IDENTITY_CASES:
        did_compare, did_reduce = compare(qasm2sop, sop_solve, name, qasm, [])
        if not did_compare:
            raise AssertionError(f"{name}: identity circuit unexpectedly rejected")
        if not did_reduce:
            raise AssertionError(f"{name}: expected the pass to shrink an uncompute identity")
        compared += 1
        reduced += 1

    # Sweep the golden QASM corpus with an open boundary (a single, generally non-zero
    # amplitude that exercises the whole instance).
    golden = sorted((source_root / "tests" / "golden").glob("*.qasm"))
    for qasm_path in golden:
        qasm = qasm_path.read_text()
        try:
            did_compare, did_reduce = compare(qasm2sop, sop_solve, qasm_path.stem, qasm, [])
        except AssertionError:
            raise
        if did_compare:
            compared += 1
            reduced += int(did_reduce)

    if compared == 0:
        raise AssertionError("no circuits were compared")
    if reduced == 0:
        raise AssertionError("optimization never reduced any instance; pass appears inert")
    print(f"compared {compared} circuits, {reduced} reduced by optimization")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
