# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). It provides small Unix-style tools to validate, inspect, import,
benchmark, and solve normalized quadratic SOP text files.

Design notes live in [ARCHITECTURE.md](ARCHITECTURE.md), future performance
notes in [ARCHITECTURE_SPEED_ANNEX.md](ARCHITECTURE_SPEED_ANNEX.md), and active
work in [tasks.md](tasks.md).

## Tools

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics as text or JSON.
- `sop-solve`: compute exact residue-count histograms or backend counters.
- `qasm2sop`: import the supported static OpenQASM 2.0 subset into QSOP.
- `tools/*.py`: benchmark runners, corpus scanners, and boundary translators.

Implemented solver backends:

- `components` (default): split connected components and cache repeated small
  components.
- `brute-force`: enumerate all assignments for small oracle checks.
- `branch`: residual branch-and-sum with component splitting and residual cache
  stats.
- `rankwidth`: sign-edge decomposition backend with count-table, CRT-backed
  large-count output, generated decompositions, and small-instance Fourier mode.

## Build

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Coverage is part of CI and must stay at or above 75% line coverage over
production `src` files:

```sh
tools/check-coverage.sh build-coverage
```

## QSOP Format

```text
p qsop <r> <variables> <quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0|1>
```

`e u v` is shorthand for a sign edge with coefficient `r/2`. Pins (`f`) are
applied during parsing, and canonical output uses dense variable IDs.

## Examples

Canonicalize, inspect, and solve:

```sh
build/sop-check tests/golden/labelled_raw.qsop
build/sop-stats --format json tests/golden/labelled_expected.qsop
build/sop-solve tests/golden/solve_labelled.qsop
build/sop-solve --backend branch --format stats tests/golden/solve_labelled.qsop
build/sop-solve --backend rankwidth --rankwidth-generate min-fill-cut tests/golden/solve_sign_path.qsop
```

The `counts` line is a histogram over phase residues modulo `r`; the values are
ordinary assignment counts.

Import fixed-boundary OpenQASM:

```sh
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
```

Run benchmark summaries:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend components --backend branch --trace --format summary
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --rankwidth-sweep --skip-unsupported --trace --format summary
```

## Scope

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External formats stay at the tool boundary. New importer support should be
driven by real benchmark cases and covered by fixed-boundary amplitude checks.
