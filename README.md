# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). The project goal is a competitive exact strong simulator using
labelled quadratic SOPs with fixed-boundary circuit amplitudes as the current
benchmark contract.

Current corpus coverage, solver timings, and native simulator comparisons live
in [scoreboard.md](scoreboard.md).

## Build

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

CI enforces at least 75% line coverage over production `src` files:

```sh
tools/check-coverage.sh build-coverage
```

Release binaries are built by `.github/workflows/release-binaries.yml` when a
`v*` tag is pushed, or manually through GitHub Actions. The workflow compiles
and packages `qasm2sop`, `sop-check`, `sop-solve`, `sop-stats`, and `sop2wmc`
as `linux-x86_64` and `macos-arm64` tarballs with SHA-256 sidecar files.

## Tools

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics, with opt-in exact small-width
  support-graph diagnostics.
- `sop-solve`: solve exact residue-count histograms; stats mode can include
  the count vector used by benchmark tooling to reconstruct amplitudes and a
  convenience probability estimate via `--include-probability`.
- `qasm2sop`: import the supported static OpenQASM 2.0 subset into QSOP,
  including common Clifford/T gates, supported phase rotations, `u/u2/u3`,
  controlled phase/H/SX gates, `dcx`, `rxx/ryy/rzz`, `ccz/ccx/rccx/cswap`,
  and `iswap`.
- `sop2wmc`: export a QSOP to DIMACS CNF / WPCNF for external model counting.
  Two encodings are available:
  - `--encoding residue` (default): for each target residue `k`, models = `counts[k]`.
    Use with any integer `#SAT` counter (Ganak `--mode 0`, d4, sharpSAT). Requires
    `r` separate counter calls per instance.
  - `--encoding amplitude`: single WPCNF with complex literal weights ω^coeff.
    The weighted model count equals `sum_x omega^phase(x)` (without the constant
    factor); multiply by `omega^constant` (printed in the `c amplitude_factor`
    metadata line) to recover the full amplitude. Use `ganak --mode 6 --verb 0`.
    Typically 10–100× faster than the residue encoding for r ≥ 8.
- `tools/*.py`: benchmark runners, corpus scanners, and boundary translators.
  `tools/bench_wmc_ganak.py` drives `sop2wmc` + Ganak and cross-checks results
  against `sop-solve`; supports `--encoding (residue, amplitude, or both)`.

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External frameworks stay at the benchmark/import boundary.

## Solver Guide

- `components`: default robust exact solver.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  widened corpus tiers.
- `branch --branch-heuristic split`: current hybrid backend; it splits
  components and dispatches eligible residuals to treewidth or rankwidth.
- `rankwidth`: exact decomposition-DP backend with labelled cut-signature
  diagnostics and count-table/Fourier modes; useful for comparison and targeted
  low-rank cases, but not the default corpus winner.
- `brute-force`: small-instance oracle.

## QSOP Format

```text
p qsop <r> <variables> <quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0 | 1>
```

`e u v` is shorthand for a sign edge with coefficient `r/2`. Pins (`f`) are
applied during parsing, and canonical output uses dense variable IDs. Solver
`counts` are ordinary assignment counts bucketed by phase residue modulo `r`.

## Examples

```sh
build/sop-check tests/golden/labelled_raw.qsop
build/sop-stats --format json tests/golden/labelled_expected.qsop
build/sop-stats --exact-widths --exact-width-max-vars 12 tests/golden/solve_sign_path.qsop
build/sop-solve --backend treewidth --treewidth-order min-fill-max-degree tests/golden/solve_labelled.qsop
build/sop-solve --format stats --include-result tests/golden/solve_labelled.qsop
build/sop-solve --format stats --backend treewidth --solve-mode fourier tests/golden/solve_labelled.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm | build/sop-solve --format stats --include-probability -
build/sop2wmc --residue all tests/golden/solve_labelled.qsop
build/sop2wmc --residue 2 -o residue2.cnf tests/golden/solve_labelled.qsop && ganak residue2.cnf
build/sop2wmc --encoding amplitude tests/golden/solve_labelled.qsop | ganak --mode 6 --verb 0 -
```

The WMC export reconstructs `amplitude = sum_k counts[k] * exp(2*pi*i*k/r)` and
`probability = |amplitude|^2 * 2^(-norm_h)` (the same convention as
`sop-solve --include-probability`) outside the counter; the metadata header in
each CNF block documents the variable map and the final accumulator bits.

## Benchmarks

The public performance summary is [scoreboard.md](scoreboard.md). Refresh it
from generated JSONL artifacts with:

```sh
tools/refresh_scoreboard.py --artifact-dir /tmp --output scoreboard.md
```

For a fuller local run, include native simulator checks and the larger sample:

```sh
tools/refresh_scoreboard.py --artifact-dir /tmp --run-native --run-large-sample --output scoreboard.md
```

Useful benchmark helpers:

- `tools/bench_qasm_corpus.py`: run the QSOP importer and solver across a corpus.
- `tools/bench_qasm_native_simulator.py`: compare against supported native
  simulators.
- `tools/render_scoreboard.py`: render ad hoc reports when you already have
  JSONL inputs.

## Current Status

[scoreboard.md](scoreboard.md) tracks corpus coverage, solver timings, native
simulator comparisons, and the current recommended solver configuration for
each benchmark tier.
