# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). The project goal is a competitive exact strong simulator using
QSOPs with fixed-boundary circuit amplitudes.


**Benchmarks:** dlx4sop's branch/treewidth/rankwidth solver backends and
the `sop2wmc` + Ganak weighted-model-counting pipeline (all described below)
are ranked on the
public [qccq-gauntlet leaderboard](https://qccq-cgd.pages.dev/).

## Tools

Prebuilt releases include the core command-line tools as `linux-x86_64` and
`macos-arm64` tarballs with SHA-256 sidecar files. The same tools are built
from source:

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics, with opt-in exact small-width
  support-graph diagnostics.
- `sop-solve`: solve exact residue-count histograms; stats mode can include
  the count vector used to reconstruct amplitudes and a convenience probability
  estimate via `--include-probability`.
- `qasm2sop`: import the supported OpenQASM 2.0 subset into QSOP,
  including common Clifford/T gates, supported phase rotations, `u/u2/u3`,
  controlled phase/H/SX gates, `dcx`, `rxx/ryy/rzz`, `ccz/ccx/rccx/cswap`,
  and `iswap`. Mid-circuit `measure` (with no classical feed-forward) and `reset`
  are lowered coherently — a measurement as the identity, a reset as a fresh `|0>`
  wire with the old value summed out. That equals a physical measure/reset exactly
  when the qubit is in a definite computational-basis state there (uncomputed or
  recycled ancillas, stabilizer syndromes — the alg85 regime, cross-checked against
  qiskit-aer); data-dependent `if` is still rejected.
- `sop2wmc`: export a QSOP to DIMACS CNF / WPCNF for external model counting.
  Five encodings are available via `--encoding <name>`:
  - `residue-accumulator` (alias `residue`, default): one DIMACS CNF per
    residue 0..r−1; plain #SAT each. Works with any integer counter (Ganak
    `--mode 0`, d4, sharpSAT). Requires r calls per instance.
  - `amp-and` (alias `amplitude`): single WPCNF with Tseitin AND auxiliaries
    carrying hard complex weights ω^b. All auxiliaries are circuit-determined;
    use `ganak --mode 6 --verb 0`. Multiply the raw WMC result by
    ω^constant (from the `c amplitude_factor` metadata line) to get the full
    amplitude.
  - `amp-soft`: single WPCNF with implication-only auxiliaries and soft weights
    ω^b − 1. Produces fewer clauses per edge than amp-and; Ganak integrates
    over underdetermined variables.
  - `residue-fourier`: r WPCNF blocks (one per Fourier exponent t) followed by
    an outer iDFT. Inner encoding selectable via
    `--wmc-fourier-inner (amp-and|amp-soft)`.
  - `amp-block`: single WPCNF; detects a uniform complete bipartite subgraph
    A×B in the edge set and encodes it with a mod-r adder counter per side plus
    Tseitin selector variables with hard weights ω^(c·a·b mod r). Non-block
    edges use amp-and. Block triggers when savings ≥ `--wmc-block-min-savings`
    and both sides ≥ `--wmc-block-min-side` (defaults 0 and 4); falls back to
    amp-soft output when no profitable block is found.
- `scripts/build_external_qasm_manifest.py` / `scripts/bench_wmc_ganak.py`:
  shared OpenQASM-munging and Ganak-output-parsing helpers imported directly
  by the `.gauntlet/` adapters that back the qccq-gauntlet registration; not
  standalone CLIs.

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External frameworks stay at corpus import and validation boundaries.

## Solver Guide

- `branch --solve-mode auto`: default recommended solver. It tries exact
  residue counting first and falls back to single-Fourier amplitude evaluation
  on safe exact-count refusals.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  developer/profiling runs.
- `rankwidth`: decomposition-DP backend with cut-rank diagnostics and
  count-table/Fourier modes; useful for comparison and targeted low-rank cases.

## QSOP Format

```text
p qsop-sign <r> <variables> <sign_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
f <vertex> <0 | 1>
```

Quadratic terms are sign edges with implicit coefficient `r/2`;
duplicate sign edges cancel by parity. Pins (`f`) are applied during parsing,
and canonical output uses dense variable IDs. Solver
`counts` are ordinary assignment counts bucketed by phase residue modulo `r`.

## Examples

```sh
build/sop-check tests/golden/sign_raw.qsop
build/sop-stats --format json tests/golden/sign_expected.qsop
build/sop-stats --exact-widths --exact-width-max-vars 12 tests/golden/solve_sign_path.qsop
build/sop-solve tests/golden/solve_disconnected.qsop
build/sop-solve --format residue-vector tests/golden/solve_disconnected.qsop
build/sop-solve --format stats --include-result tests/golden/solve_single.qsop
build/sop-solve --format stats --backend treewidth --solve-mode fourier tests/golden/solve_disconnected.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm | build/sop-solve --format stats --include-probability -
build/sop2wmc --encoding auto --stats-only tests/golden/solve_disconnected.qsop
build/sop2wmc --residue all tests/golden/solve_disconnected.qsop
build/sop2wmc --residue 2 -o residue2.cnf tests/golden/solve_disconnected.qsop && ganak residue2.cnf
build/sop2wmc --encoding auto tests/golden/solve_disconnected.qsop | ganak --mode 6 --verb 0 -
```

### Approximate QASM imports

`qasm2sop` is exact by default: circuits with phases outside the supported
finite grid are rejected. Use `--approx X` to opt in to phase rounding, where
`X` is a positive additive amplitude error budget. Scientific notation is
accepted, for example:

```sh
build/qasm2sop --approx 1e-6 --input 0 --output 0 circuit.qasm
```

Approximate output includes comment lines recording the chosen modulus, rounded
phase count, and certified additive amplitude error bound.

The WMC export reconstructs `raw_amplitude = sum_k counts[k] * exp(2*pi*i*k/r)` and
`probability = |raw_amplitude|^2 * 2^(-norm_h)` (the same convention as
`sop-solve --include-probability`) outside the counter; the metadata header in
each CNF block documents the variable map and the final accumulator bits.

## Amplitudes are normalized

`sop-solve --format amplitude` reports the **normalized** amplitude
`raw_amplitude * 2^(-norm_h/2)`: the physical `<y|C|x>`, whose modulus is at most 1,
alongside the `norm_h` needed to recover the raw value. The raw sum-over-paths
amplitude grows like `2^nvars` so no fixed-exponent floating type can hold it, and
a naive `float(raw) * 2**(-norm_h/2)` silently yields `nan`. 

Internally the solvers
carry a mantissa and a separate binary exponent (`qsop_amplitude_t.scale_exp2`), which
also lets the branch backend multiply per-component amplitudes without any of them
overflowing on its own. Scaling by a power of two is exact, so nothing is lost.


## Build

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The default `build` directory is a debug build (`-O0`, assertions on), which is
appropriate for development and the test suite.

CI enforces at least 75% line coverage over production `src` files:

```sh
scripts/check-coverage.sh build-coverage
```

## Benchmarks

Comparative benchmarking can be found in
[qccq-gauntlet](https://qccq-cgd.pages.dev/), an external harness that runs
the dlx4sop backends and the `sop2wmc` + Ganak pipeline against shared
datasets/suites on a public leaderboard. `.gauntlet/adapter.py` and
`.gauntlet/adapter_wmc.py` are the two integration points gauntlet drives:
they import a circuit, run it through `qasm2sop`/`sop-solve` (or
`sop2wmc` + Ganak), and report back in the protocol gauntlet expects. Both
adapters import `build_external_qasm_manifest.py` / `bench_wmc_ganak.py`
from `scripts/` for OpenQASM munging and Ganak-output parsing.

## Solver internals

The branch backend delegates connected components to treewidth or rankwidth DP
according to a cost model. See [Solver internals](docs/solver-internals.md) for
the decision process, runtime tuning, calibration, and observability details;
`sop-solve --help-advanced` lists every advanced flag.
