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
- `qasm2sop`: import the supported static OpenQASM 2.0 subset into QSOP,
  including common Clifford/T gates, supported phase rotations, `u/u2/u3`,
  controlled phase/H/SX gates, `dcx`, `rxx/ryy/rzz`, `ccz/ccx/rccx/cswap`,
  and `iswap`.
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

## Solver Guide

- `components`: default robust exact solver.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  widened corpus tiers.
- `branch --branch-heuristic split`: current hybrid backend; it splits
  components and dispatches eligible residuals to treewidth or rankwidth.
- `rankwidth`: exact decomposition-DP backend with cut-rank diagnostics and
  count-table/Fourier modes; useful for comparison and targeted
  low-rank cases, but not the default corpus winner.
- `brute-force`: small-instance oracle.

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
build/sop-solve --backend treewidth --treewidth-order min-fill-max-degree tests/golden/solve_disconnected.qsop
build/sop-solve --format stats --include-result tests/golden/solve_single.qsop
build/sop-solve --format stats --backend treewidth --solve-mode fourier tests/golden/solve_disconnected.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm | build/sop-solve --format stats --include-probability -
build/sop2wmc --residue all tests/golden/solve_disconnected.qsop
build/sop2wmc --residue 2 -o residue2.cnf tests/golden/solve_disconnected.qsop && ganak residue2.cnf
build/sop2wmc --encoding amplitude tests/golden/solve_disconnected.qsop | ganak --mode 6 --verb 0 -
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

The WMC export reconstructs `amplitude = sum_k counts[k] * exp(2*pi*i*k/r)` and
`probability = |amplitude|^2 * 2^(-norm_h)` (the same convention as
`sop-solve --include-probability`) outside the counter; the metadata header in
each CNF block documents the variable map and the final accumulator bits.

## Benchmarks

Comparative benchmarking now lives entirely in
[qccq-gauntlet](https://qccq-cgd.pages.dev/), an external harness that runs
the dlx4sop backends and the `sop2wmc` + Ganak pipeline against shared
datasets/suites on a public leaderboard. `.gauntlet/adapter.py` and
`.gauntlet/adapter_wmc.py` are the two integration points gauntlet drives:
they import a circuit, run it through `qasm2sop`/`sop-solve` (or
`sop2wmc` + Ganak), and report back in the protocol gauntlet expects. Both
adapters import `build_external_qasm_manifest.py` / `bench_wmc_ganak.py`
from `scripts/` for OpenQASM munging and Ganak-output parsing.

Local microbenchmarking of the shared SIMD kernels (bitset popcount/xor and
f64 complex kernels) is still available:

```sh
meson setup build-bench --buildtype=release -Db_lto=true -Dc_args=-march=x86-64-v2
meson compile -C build-bench
build-bench/bench-kernels --quick
```

`-march=x86-64-v2` enables the hardware `popcnt` instruction (portable across x86 CPUs
since ~2009); without it `__builtin_popcount` falls back to a slow libgcc routine that
dominates ordering-heavy solves. On Apple Silicon (arm64) popcount is hardware by
default, so the `-Dc_args` is unnecessary there.
