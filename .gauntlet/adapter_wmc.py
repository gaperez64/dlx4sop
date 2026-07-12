#!/usr/bin/env python3
"""qccq-gauntlet adapter for dlx4sop's sop2wmc + upstream ganak pipeline.

Speaks the inferq.zero-amplitude.v1 protocol (see qccq-gauntlet's
docs/adapter-protocol.md): for each request, computes <0^n|C|0^n> by piping
qasm2sop's output into `sop2wmc --encoding auto` (a single selected
weighted-CNF export with an embedded `amplitude_factor`), then into ganak's
`--mode 6` mpfr-complex model counter -- the same pipeline validated by
scripts/bench_wmc_ganak.py, whose parsing helpers this module reuses
directly. Frontend circuit handling (QPY loading, QASM conversion,
exact-then-approx qasm2sop import) is shared with adapter.py.

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
import pathlib
import subprocess
import sys
from decimal import Decimal, getcontext

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import bench_wmc_ganak as wmc  # noqa: E402
import build_external_qasm_manifest as manifest_tool  # noqa: E402
from adapter import (  # noqa: E402
    import_qsop,
    load_circuit,
    parse_norm_h,
    run_frontend,
)

SOP2WMC = REPO_ROOT / "build" / "sop2wmc"
GANAK = REPO_ROOT / ".gauntlet" / "ganak" / "ganak"

# Slightly above the harness's 120s per-case window so gauntlet, not an inner
# subprocess timeout, owns timeout classification during benchmark runs.
SUBPROCESS_TIMEOUT_S = 125.0


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


def solve(args: argparse.Namespace, qasm_text: str, nqubits: int) -> tuple[complex, dict]:
    zero = "0" * nqubits
    qsop_text, metrics = import_qsop(qasm_text, zero)
    norm_h = parse_norm_h(qsop_text)

    export_cmd = [str(SOP2WMC), "--encoding", args.encoding]
    if args.wmc_preprocess is not None:
        export_cmd += ["--wmc-preprocess", args.wmc_preprocess]
    if args.wmc_peel2_fill_budget is not None:
        export_cmd += ["--wmc-peel2-fill-budget", str(args.wmc_peel2_fill_budget)]
    if args.wmc_block_min_side is not None:
        export_cmd += ["--wmc-block-min-side", str(args.wmc_block_min_side)]
    if args.wmc_block_min_savings is not None:
        export_cmd += ["--wmc-block-min-savings", str(args.wmc_block_min_savings)]
    export_cmd.append("-")

    exported = subprocess.run(
        export_cmd,
        input=qsop_text,
        check=False,
        capture_output=True,
        text=True,
        timeout=args.ganak_timeout_seconds,
    )
    if exported.returncode != 0:
        raise RuntimeError(
            f"sop2wmc --encoding {args.encoding} failed: {exported.stderr.strip()}"
        )

    cnf_text = exported.stdout
    metrics.update(wmc.parse_wmc_metadata(cnf_text))
    metrics["wmc_requested_encoding"] = args.encoding
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
        timeout=args.ganak_timeout_seconds,
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
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--encoding",
        default="auto",
        choices=["auto", "amp-and", "amp-soft", "amp-block", "residue", "residue-fourier"],
    )
    parser.add_argument("--wmc-preprocess", choices=["none", "peel1", "peel2-safe"])
    parser.add_argument("--wmc-peel2-fill-budget", type=int)
    parser.add_argument("--wmc-block-min-side", type=int)
    parser.add_argument("--wmc-block-min-savings", type=int)
    parser.add_argument("--ganak-timeout-seconds", type=float, default=SUBPROCESS_TIMEOUT_S)
    args = parser.parse_args()

    if not SOP2WMC.is_file():
        raise SystemExit(f"missing build output: expected {SOP2WMC} (did bootstrap run?)")
    if not GANAK.is_file():
        raise SystemExit(f"missing ganak binary: expected {GANAK} (did bootstrap run?)")

    return run_frontend(lambda qasm_text, nqubits: solve(args, qasm_text, nqubits))


if __name__ == "__main__":
    sys.exit(main())
