# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). The project goal is a competitive exact strong simulator using
labelled quadratic SOPs with fixed-boundary circuit amplitudes as the current
benchmark contract.

Current corpus coverage, solver timings, and native simulator comparisons live
in [scoreboard.md](scoreboard.md) (index), [scoreboard-sign.md](scoreboard-sign.md),
and [scoreboard-labelled.md](scoreboard-labelled.md).

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
- `tools/*.py`: benchmark runners, corpus scanners, and boundary translators.
  `tools/bench.py` is the unified benchmark entry point (see **Benchmarks**
  below). `tools/bench_wmc_ganak.py` drives `sop2wmc` + Ganak and cross-checks
  results against `sop-solve`; all five current encodings are supported.

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

The public performance summary is split into two mode-specific scoreboards plus
an index:

- [scoreboard.md](scoreboard.md) — index with combined totals and links
- [scoreboard-sign.md](scoreboard-sign.md) — sign QSOP benchmarks
- [scoreboard-labelled.md](scoreboard-labelled.md) — labelled QSOP benchmarks

`tools/bench.py` is the unified benchmark entry point. It requires only the
built binaries and the committed QSOP corpus for local runs; external tools
(Ganak, native simulators, QASM manifests) are only needed for full refreshes.

### Local backend tuning (no external tools)

```sh
meson compile -C build
python3 tools/bench.py local \
    --tier tier-17-32 \
    --backend treewidth \
    --backend rankwidth:from-treewidth \
    --backend branch:auto \
    --timeout 5 \
    --out artifacts/local/tier-17-32.jsonl
python3 tools/bench.py render \
    --artifact-dir artifacts/local \
    --view local \
    --output /tmp/local-scoreboard.md
```

Available backend variants for `--backend`:

```text
treewidth
rankwidth:from-treewidth
rankwidth:best        (best decomposition strategy)
rankwidth:validate    (cross-check two solve paths for consistency)
branch:auto
branch:from-treewidth
branch:native
branch:no-rankwidth   (control: branch without rankwidth delegation)
```

### Full scoreboard refresh (requires Ganak + native simulators)

```sh
python3 tools/run_corpus_benchmarks.py \
    --artifact-dir artifacts/full \
    --ganak /path/to/ganak
```

`run_corpus_benchmarks.py` is the single orchestrator. It runs solver, WMC (Ganak),
and native-simulator jobs for all tiers (including the MQT Bench large tiers), then
renders:
1. `scoreboard-sign.md` + `scoreboard-assets/sign/` SVGs (sign QSOPs only)
2. `scoreboard-labelled.md` + `scoreboard-assets/labelled/` SVGs (labelled QSOPs only)
3. `scoreboard.md` — combined index linking to both mode scoreboards

`bench.py full` is a thin alias for the same pipeline. Pass `--skip-wmc`,
`--skip-native`, `--skip-solver`, or `--skip-scoreboard` to run a subset, and
`--timeout N` to override the default 30 s per-instance timeout for all jobs.

### Render from existing artifacts

```sh
python3 tools/bench.py render \
    --artifact-dir /tmp/dlx4sop-artifacts \
    --view full
```

This regenerates both mode scoreboards, their SVG plots, and the index from
existing JSONL artifacts without re-running any experiments.

### Other benchmark tools

- `tools/bench_qasm_corpus.py`: run the QSOP importer and solver across a manifest.
- `tools/bench_wmc_ganak.py`: drive `sop2wmc` + Ganak and cross-check against `sop-solve`.
- `tools/bench_qasm_native_simulator.py`: compare against supported native simulators.
- `tools/render_scoreboard.py`: render ad hoc reports (local backend summaries, MQT tuning).
- `tools/run_corpus_benchmarks.py`: the single full-pipeline orchestrator (`bench.py full` is an alias).

## Current Status

[scoreboard.md](scoreboard.md) is the index with combined coverage totals and
links to the per-mode scoreboards: [scoreboard-sign.md](scoreboard-sign.md) and
[scoreboard-labelled.md](scoreboard-labelled.md). Each tracks corpus coverage,
solver timings, and the recommended solver configuration per tier.

Headline finding: on sign QSOPs, the solver beats the `pyzx-matrix` baseline
across tiers while dense `aer-statevector` still wins some low-width rows.
Labelled QSOPs have no native baseline — statevector simulators only evaluate
sign boundaries (input == output).
