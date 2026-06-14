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
  components, with CRT-backed large-count output.
- `brute-force`: enumerate all assignments for small oracle checks.
- `branch`: residual branch-and-sum with component splitting, cache stats, and
  CRT-backed large-count output.
- `rankwidth`: decomposition backend with sign/labelled count-table mode,
  CRT-backed large-count output, generated decompositions, and sign-only
  Fourier mode.
- `treewidth`: bucket-elimination backend with `min-fill|min-degree` orders and
  CRT-backed large-count output.

Current solver guidance:

- Use `components` as the default robust exact solver.
- Use `branch` as the main labelled/CRT baseline for connected instances and
  branch-cache experiments.
- Use `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table`
  as the strongest decomposition DP currently available when it accepts the
  instance; on the present smoke-scale corpora it reports smaller max tables
  than treewidth.
- Use `treewidth` for decomposition comparisons and tracing. `min-fill` and
  `min-degree` currently tie on table shape in the available benchmark pool.

Benchmark basis: `tools/bench_qasm_corpus.py` over the checked-in
`tests/qasm_solver_corpus.json` corpus (32 boundaries) and a larger comparison
pool (72 boundaries total) drawn from the checked-in cases plus supported
imports from [PyZX](https://github.com/zxcalc/pyzx) benchmark circuits and
[MQT Bench](https://github.com/munich-quantum-toolkit/bench). The FeynmanDD
scanner targets
[feynman-decision-diagram](https://github.com/cqs-thu/feynman-decision-diagram),
but that source does not yet contribute supported cases to the cited run. These
numbers are a tooling baseline, not a final performance ranking.

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
build/sop-solve --backend treewidth --treewidth-order min-degree --format stats tests/golden/solve_labelled.qsop
```

The `counts` line is a histogram over phase residues modulo `r`; the values are
ordinary assignment counts.

Import fixed-boundary OpenQASM:

```sh
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
```

Run benchmark summaries and build external corpus manifests:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend components --backend branch --trace --format summary
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --rankwidth-sweep --skip-unsupported --trace --format summary
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend treewidth --treewidth-order min-fill --treewidth-order min-degree --top-metric treewidth_max_table_entries --format summary
tools/build_external_qasm_manifest.py build/qasm2sop path/to/corpus --report corpus-report.json --output corpus.json
```

## Scope

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External formats stay at the tool boundary. New importer support should be
driven by real benchmark cases and covered by fixed-boundary amplitude checks.
