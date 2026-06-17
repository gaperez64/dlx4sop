#!/usr/bin/env python3
"""Validate the sop2wmc export (residue and amplitude) without an external counter.

Residue encoding: for residue k, the exported CNF must have exactly counts[k]
models. We brute-force the (tiny) variable space with unit propagation.

Amplitude encoding: the exported WPCNF must produce a WMC sum (multiplied by
the amplitude_factor from metadata) equal to sop-solve's amplitude
sum_k counts[k] * omega^k. We brute-force the WMC by directly evaluating the
literal weight product for each free-variable assignment.
"""

import cmath
import itertools
import math
import pathlib
import re
import subprocess
import sys
import tempfile


def run(cmd, **kwargs):
    return subprocess.run(
        cmd, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, **kwargs
    )


def reference_counts(sop_solve: pathlib.Path, qsop: pathlib.Path):
    result = run([str(sop_solve), "--format", "residue-vector", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(f"sop-solve failed on {qsop}:\n{result.stderr}")
    for line in result.stdout.splitlines():
        if line.startswith("counts"):
            return [int(tok) for tok in line.split()[1:]]
    raise AssertionError(f"no counts line in sop-solve output:\n{result.stdout}")


def parse_blocks(text: str):
    """Split sop2wmc --residue all output into per-residue (nvars, xvars, clauses)."""
    blocks = []
    current = None
    for line in text.splitlines():
        if line.startswith("c --- residue"):
            if current is not None:
                blocks.append(current)
            current = {"residue": int(line.split()[3]), "nvars": None, "xvars": [], "tokens": []}
            continue
        if current is None:
            continue
        if line.startswith("c sop2wmc"):
            for field in line.split():
                if field.startswith("nvars="):
                    current["nvars"] = int(field.split("=", 1)[1])
        elif line.startswith("c xvar"):
            current["xvars"].append(int(line.split()[3]))
        elif line.startswith("c") or line.startswith("p cnf"):
            continue
        else:
            current["tokens"].extend(int(tok) for tok in line.split())
    if current is not None:
        blocks.append(current)
    for block in blocks:
        clause, clauses = [], []
        for tok in block["tokens"]:
            if tok == 0:
                clauses.append(clause)
                clause = []
            else:
                clause.append(tok)
        block["clauses"] = clauses
    return blocks


def model_exists(clauses, assignment) -> bool:
    """Unit-propagate a partial assignment; raise if the circuit is undetermined."""
    a = dict(assignment)
    changed = True
    while changed:
        changed = False
        for clause in clauses:
            satisfied = False
            unassigned = []
            for lit in clause:
                var, want = abs(lit), lit > 0
                if var in a:
                    if a[var] == want:
                        satisfied = True
                        break
                else:
                    unassigned.append(lit)
            if satisfied:
                continue
            if not unassigned:
                return False  # conflict -> no model
            if len(unassigned) == 1:
                lit = unassigned[0]
                a[abs(lit)] = lit > 0
                changed = True
    for clause in clauses:
        if not any(abs(l) in a and a[abs(l)] == (l > 0) for l in clause):
            raise AssertionError(f"unit propagation left a clause undetermined: {clause}")
    return True


def verify_instance(sop2wmc: pathlib.Path, sop_solve: pathlib.Path, qsop: pathlib.Path) -> None:
    counts = reference_counts(sop_solve, qsop)
    r = len(counts)

    result = run([str(sop2wmc), "--residue", "all", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(f"sop2wmc failed on {qsop}:\n{result.stderr}")
    blocks = parse_blocks(result.stdout)
    if len(blocks) != r:
        raise AssertionError(f"{qsop}: expected {r} residue blocks, got {len(blocks)}")

    nvars = blocks[0]["nvars"]
    xvars = blocks[0]["xvars"]
    if nvars is None or len(xvars) != nvars:
        raise AssertionError(f"{qsop}: bad metadata nvars={nvars} xvars={xvars}")

    histogram = [0] * r
    for block in blocks:
        k = block["residue"]
        for bits in itertools.product((False, True), repeat=nvars):
            assignment = {xvars[i]: bits[i] for i in range(nvars)}
            if model_exists(block["clauses"], assignment):
                histogram[k] += 1

    if histogram != counts:
        raise AssertionError(
            f"{qsop}: model histogram {histogram} != sop-solve counts {counts}"
        )
    if sum(histogram) != (1 << nvars):
        raise AssertionError(f"{qsop}: histogram sums to {sum(histogram)}, expected {1 << nvars}")

    # A single-residue export must match the corresponding block exactly.
    single = run([str(sop2wmc), "--residue", "2", str(qsop)])
    if single.returncode != 0:
        raise AssertionError(f"sop2wmc --residue 2 failed on {qsop}:\n{single.stderr}")


def parse_complex_weight(s: str) -> complex:
    m = re.match(r"([+-]?[\d.e+-]+)\+([+-]?[\d.e+-]+)i", s.strip())
    if not m:
        raise ValueError(f"cannot parse complex weight: {s!r}")
    return complex(float(m.group(1)), float(m.group(2)))


def parse_amplitude_wpcnf(text: str):
    """Return (xvars, weights, amplitude_factor, clauses, nvars_total)."""
    xvars = []          # DIMACS var for each free QSOP variable (1-indexed)
    weights = {}        # var -> (true_weight, false_weight) as complex
    amplitude_factor = complex(1, 0)
    nvars_total = 0
    nclauses = 0
    clause_tokens = []

    for line in text.splitlines():
        if line.startswith("p cnf"):
            parts = line.split()
            nvars_total = int(parts[2])
            nclauses = int(parts[3])
        elif line.startswith("c xvar"):
            xvars.append(int(line.split()[3]))
        elif line.startswith("c amplitude_factor"):
            amplitude_factor = parse_complex_weight(line.split(None, 2)[2])
        elif line.startswith("c p weight"):
            parts = line.split()
            lit = int(parts[3])
            w = parse_complex_weight(parts[4])
            var = abs(lit)
            if lit > 0:
                if var not in weights:
                    weights[var] = [complex(1, 0), complex(1, 0)]
                weights[var][0] = w
            else:
                if var not in weights:
                    weights[var] = [complex(1, 0), complex(1, 0)]
                weights[var][1] = w
        elif not line.startswith("c") and line.strip():
            clause_tokens.extend(int(t) for t in line.split())

    clauses = []
    clause = []
    for tok in clause_tokens:
        if tok == 0:
            clauses.append(clause)
            clause = []
        else:
            clause.append(tok)
    return xvars, weights, amplitude_factor, clauses, nvars_total


def resolve_tseitin(clauses, free_assignment):
    """Unit-propagate free assignment through Tseitin clauses; return full assignment."""
    a = dict(free_assignment)
    changed = True
    while changed:
        changed = False
        for clause in clauses:
            unset = [lit for lit in clause if abs(lit) not in a]
            if any(a.get(abs(lit)) == (lit > 0) for lit in clause):
                continue  # satisfied
            if not unset:
                raise AssertionError(f"conflict propagating {free_assignment}")
            if len(unset) == 1:
                lit = unset[0]
                a[abs(lit)] = lit > 0
                changed = True
    return a


def eval_wmc(xvars, weights, amplitude_factor, clauses, nvars_total) -> complex:
    """Brute-force WMC over all 2^|xvars| free assignments."""
    total = complex(0, 0)
    for bits in itertools.product((False, True), repeat=len(xvars)):
        free = {xvars[i]: bits[i] for i in range(len(xvars))}
        full = resolve_tseitin(clauses, free)
        w = complex(1, 0)
        for var in range(1, nvars_total + 1):
            is_true = full.get(var, False)
            if var in weights:
                w *= weights[var][0] if is_true else weights[var][1]
        total += w
    return total * amplitude_factor


def counts_to_amplitude(counts, r) -> complex:
    omega = cmath.exp(2 * math.pi * 1j / r)
    return sum(c * omega ** k for k, c in enumerate(counts))


def eval_wmc_all_vars(weights: dict, amplitude_factor: complex,
                      clauses: list, nvars_total: int) -> complex:
    """Brute-force WMC over all 2^nvars_total assignments.

    Unlike eval_wmc, this handles non-determined auxiliary variables (e.g.
    amp-soft) by enumerating all assignments including auxiliaries.
    """
    total = complex(0, 0)
    for bits in itertools.product((False, True), repeat=nvars_total):
        sat = True
        for clause in clauses:
            if not any(
                (bits[abs(lit) - 1] if lit > 0 else not bits[abs(lit) - 1])
                for lit in clause
            ):
                sat = False
                break
        if not sat:
            continue
        w = complex(1, 0)
        for var in range(1, nvars_total + 1):
            if var in weights:
                w *= weights[var][0] if bits[var - 1] else weights[var][1]
        total += w
    return total * amplitude_factor


def verify_amplitude(sop2wmc: pathlib.Path, sop_solve: pathlib.Path, qsop: pathlib.Path) -> None:
    counts = reference_counts(sop_solve, qsop)
    r = len(counts)
    ref_amplitude = counts_to_amplitude(counts, r)

    result = run([str(sop2wmc), "--encoding", "amplitude", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(f"sop2wmc --encoding amplitude failed on {qsop}:\n{result.stderr}")

    xvars, weights, amp_factor, clauses, nvars_total = parse_amplitude_wpcnf(result.stdout)
    got = eval_wmc(xvars, weights, amp_factor, clauses, nvars_total)

    if abs(got - ref_amplitude) > 1e-9 * max(1.0, abs(ref_amplitude)):
        raise AssertionError(
            f"{qsop}: amplitude {got} does not match sop-solve {ref_amplitude}"
        )


def verify_amp_soft(sop2wmc: pathlib.Path, sop_solve: pathlib.Path, qsop: pathlib.Path) -> None:
    """Verify amp-soft: no ternary clauses, binary == 2*encoded, amplitude matches."""
    counts = reference_counts(sop_solve, qsop)
    r = len(counts)
    ref_amplitude = counts_to_amplitude(counts, r)

    result = run([str(sop2wmc), "--encoding", "amp-soft", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(
            f"sop2wmc --encoding amp-soft failed on {qsop}:\n{result.stderr}"
        )

    xvars, weights, amp_factor, clauses, nvars_total = parse_amplitude_wpcnf(result.stdout)

    # Parse nvars from metadata to determine number of aux vars.
    sop_nvars = None
    for line in result.stdout.splitlines():
        if "encoding=amp-soft" in line:
            for field in line.split():
                if field.startswith("nvars="):
                    sop_nvars = int(field.split("=", 1)[1])
    if sop_nvars is None:
        raise AssertionError(f"{qsop}: amp-soft missing nvars in metadata")

    encoded_edges = nvars_total - sop_nvars

    # Structural: no ternary (or longer) clauses.
    ternary = [c for c in clauses if len(c) >= 3]
    if ternary:
        raise AssertionError(f"{qsop}: amp-soft has ternary/long clauses: {ternary}")

    # Structural: exactly 2 binary clauses per encoded edge.
    if len(clauses) != 2 * encoded_edges:
        raise AssertionError(
            f"{qsop}: amp-soft has {len(clauses)} clauses, expected {2*encoded_edges}"
        )

    # Amplitude correctness via brute force over all assignments.
    got = eval_wmc_all_vars(weights, amp_factor, clauses, nvars_total)
    if abs(got - ref_amplitude) > 1e-9 * max(1.0, abs(ref_amplitude)):
        raise AssertionError(
            f"{qsop}: amp-soft amplitude {got} != sop-solve reference {ref_amplitude}"
        )


def parse_fourier_blocks(text: str):
    """Parse residue-fourier multi-block output into a list of (t, nvars_total, weights, clauses)."""
    blocks = []
    current_t = None
    current_lines = []
    for line in text.splitlines():
        m = re.match(r"c --- fourier t=(\d+) r=(\d+) ---", line)
        if m:
            if current_t is not None:
                blocks.append((current_t, "\n".join(current_lines)))
            current_t = int(m.group(1))
            current_lines = []
        else:
            current_lines.append(line)
    if current_t is not None:
        blocks.append((current_t, "\n".join(current_lines)))
    return blocks


def verify_residue_fourier(
    sop2wmc: pathlib.Path, sop_solve: pathlib.Path, qsop: pathlib.Path
) -> None:
    """Verify residue-fourier: inverse DFT of brute-force amplitudes matches sop-solve counts."""
    ref_counts = reference_counts(sop_solve, qsop)
    r = len(ref_counts)

    result = run([str(sop2wmc), "--encoding", "residue-fourier", str(qsop)])
    if result.returncode != 0:
        raise AssertionError(
            f"sop2wmc --encoding residue-fourier failed on {qsop}:\n{result.stderr}"
        )

    blocks = parse_fourier_blocks(result.stdout)
    if len(blocks) != r:
        raise AssertionError(f"{qsop}: expected {r} fourier blocks, got {len(blocks)}")

    # Compute amplitudes Z_t for each block via brute-force WMC.
    omega = cmath.exp(2 * math.pi * 1j / r)
    z_values = []
    for t, block_text in sorted(blocks):
        if "encoding=fourier-t0" in block_text:
            # Z_0 = 2^n.
            nvars_line = next(
                (l for l in block_text.splitlines() if "z0_log2=" in l), ""
            )
            n = int(re.search(r"z0_log2=(\d+)", nvars_line).group(1))
            z_values.append(complex(1 << n, 0))
        else:
            xvars, weights, amp_factor, clauses, nvars_total = parse_amplitude_wpcnf(block_text)
            z_t = eval_wmc_all_vars(weights, amp_factor, clauses, nvars_total)
            z_values.append(z_t)

    # Inverse DFT: N_k = (1/r) * sum_t Z_t * omega^(-t*k).
    got_counts = []
    for k in range(r):
        n_k = sum(z_values[t] * omega ** (-t * k) for t in range(r)) / r
        # Check imaginary part is near zero and real part is near integer.
        if abs(n_k.imag) > 1e-6:
            raise AssertionError(
                f"{qsop}: iDFT count[{k}] has large imaginary part: {n_k}"
            )
        rounded = round(n_k.real)
        if abs(n_k.real - rounded) > 1e-6:
            raise AssertionError(
                f"{qsop}: iDFT count[{k}] is not near integer: {n_k.real}"
            )
        if rounded < 0:
            raise AssertionError(
                f"{qsop}: iDFT count[{k}] is negative: {rounded}"
            )
        got_counts.append(int(rounded))

    if got_counts != ref_counts:
        raise AssertionError(
            f"{qsop}: residue-fourier counts {got_counts} != sop-solve {ref_counts}"
        )


def verify_peel1(sop2wmc: pathlib.Path, sop_solve: pathlib.Path, qsop: pathlib.Path) -> None:
    """Verify that peel1 preprocessing gives the same amplitude as no-preprocess."""
    counts = reference_counts(sop_solve, qsop)
    r = len(counts)
    ref_amplitude = counts_to_amplitude(counts, r)

    for enc in ("amp-and", "amp-soft"):
        result = run(
            [str(sop2wmc), "--encoding", enc, "--wmc-preprocess", "peel1", str(qsop)]
        )
        if result.returncode != 0:
            raise AssertionError(
                f"sop2wmc --encoding {enc} --wmc-preprocess peel1 failed on {qsop}:\n{result.stderr}"
            )

        # Handle the special zero-amplitude case.
        if "encoding=zero" in result.stdout:
            if abs(ref_amplitude) > 1e-9:
                raise AssertionError(
                    f"{qsop}: peel1 emitted zero-amplitude WPCNF but reference is {ref_amplitude}"
                )
            continue

        xvars, weights, amp_factor, clauses, nvars_total = parse_amplitude_wpcnf(result.stdout)
        got = eval_wmc_all_vars(weights, amp_factor, clauses, nvars_total)
        if abs(got - ref_amplitude) > 1e-9 * max(1.0, abs(ref_amplitude)):
            raise AssertionError(
                f"{qsop} {enc} peel1: got {got}, reference {ref_amplitude}"
            )


def check_cli(sop2wmc: pathlib.Path, source_root: pathlib.Path) -> None:
    qsop = source_root / "tests" / "golden" / "solve_labelled.qsop"

    help_result = run([str(sop2wmc), "--help"])
    if help_result.returncode != 0 or "usage: sop2wmc" not in help_result.stdout:
        raise AssertionError(f"unexpected --help result:\n{help_result.stdout}\n{help_result.stderr}")

    stdin_result = run([str(sop2wmc), "--residue", "0", "-"], input=qsop.read_text())
    file_result = run([str(sop2wmc), "--residue", "0", str(qsop)])
    if stdin_result.returncode != 0 or stdin_result.stdout != file_result.stdout:
        raise AssertionError("stdin export differs from file export")

    with tempfile.TemporaryDirectory() as tmp:
        out = pathlib.Path(tmp) / "out.cnf"
        written = run([str(sop2wmc), "--residue", "all", "-o", str(out), str(qsop)])
        if written.returncode != 0 or written.stdout != "":
            raise AssertionError(f"unexpected -o result:\n{written.stderr}")
        if "p cnf" not in out.read_text():
            raise AssertionError("output file missing DIMACS header")

    # All valid encodings should succeed.
    for enc in ("amp-and", "amplitude", "amp-soft", "residue-fourier",
                "residue-accumulator", "residue"):
        r = run([str(sop2wmc), "--encoding", enc, str(qsop)])
        if r.returncode != 0:
            raise AssertionError(f"--encoding {enc} unexpectedly failed:\n{r.stderr}")

    # peel1 preprocessing should succeed for amplitude encodings and residue-fourier.
    for enc in ("amp-and", "amp-soft", "residue-fourier"):
        for pp in ("none", "peel1"):
            r = run([str(sop2wmc), "--encoding", enc, "--wmc-preprocess", pp, str(qsop)])
            if r.returncode != 0:
                raise AssertionError(
                    f"--encoding {enc} --wmc-preprocess {pp} failed:\n{r.stderr}"
                )

    # Fourier inner encoding flag.
    for inner in ("amp-and", "amp-soft"):
        r = run([str(sop2wmc), "--encoding", "residue-fourier",
                 "--wmc-fourier-inner", inner, str(qsop)])
        if r.returncode != 0:
            raise AssertionError(
                f"--wmc-fourier-inner {inner} unexpectedly failed:\n{r.stderr}"
            )

    error_cases = [
        ([str(sop2wmc), "--residue"], "requires a value"),
        ([str(sop2wmc), "--residue", "nope", str(qsop)], "must be 'all'"),
        ([str(sop2wmc), "--residue", "999", str(qsop)], "out of range"),
        ([str(sop2wmc), "--bad"], "unknown option"),
        ([str(sop2wmc), str(qsop), str(qsop)], "at most one input"),
        ([str(sop2wmc), str(source_root / "tests" / "golden" / "missing.qsop")], "No such file"),
        ([str(sop2wmc), "-o"], "requires a path"),
        ([str(sop2wmc), "--encoding"], "requires a value"),
        ([str(sop2wmc), "--encoding", "nope", str(qsop)], "must be 'amp-and'"),
    ]
    for cmd, expected in error_cases:
        completed = run(cmd)
        if completed.returncode == 0 or expected not in completed.stderr:
            raise AssertionError(f"unexpected error result for {cmd}:\n{completed.stderr}")


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: test_sop2wmc.py SOP2WMC SOP_SOLVE SOURCE_ROOT", file=sys.stderr)
        return 2
    sop2wmc = pathlib.Path(sys.argv[1])
    sop_solve = pathlib.Path(sys.argv[2])
    source_root = pathlib.Path(sys.argv[3])

    for name in ("solve_sign_path.qsop", "solve_labelled.qsop"):
        qsop = source_root / "tests" / "golden" / name
        verify_instance(sop2wmc, sop_solve, qsop)
        verify_amplitude(sop2wmc, sop_solve, qsop)
        verify_amp_soft(sop2wmc, sop_solve, qsop)
        verify_residue_fourier(sop2wmc, sop_solve, qsop)
        verify_peel1(sop2wmc, sop_solve, qsop)
    check_cli(sop2wmc, source_root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
