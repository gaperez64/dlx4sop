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

## Tools

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics, with opt-in exact small-width
  support-graph diagnostics.
- `sop-solve`: solve exact residue-count histograms; stats mode can include
  the count vector used by benchmark tooling to reconstruct amplitudes.
- `qasm2sop`: import the supported static OpenQASM 2.0 subset into QSOP,
  including common Clifford/T gates, supported phase rotations, `u/u2/u3`,
  controlled phase/H/SX gates, `dcx`, `rxx/ryy/rzz`, `ccz/ccx/rccx/cswap`,
  and `iswap`.
- `tools/*.py`: benchmark runners, corpus scanners, and boundary translators.

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External frameworks stay at the benchmark/import boundary.

## Solver Guide

- `components`: default robust exact solver.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  widened corpus tiers.
- `branch --branch-heuristic split`: current hybrid backend; it splits
  components and dispatches low-width residuals to treewidth.
- `rankwidth`: decomposition-DP experiments; generated decompositions still need
  improvement.
- `brute-force`: small-instance oracle.

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
applied during parsing, and canonical output uses dense variable IDs. Solver
`counts` are ordinary assignment counts bucketed by phase residue modulo `r`.

## Examples

```sh
build/sop-check tests/golden/labelled_raw.qsop
build/sop-stats --format json tests/golden/labelled_expected.qsop
build/sop-stats --exact-widths --exact-width-max-vars 12 tests/golden/solve_sign_path.qsop
build/sop-solve --backend treewidth --treewidth-order min-fill-max-degree tests/golden/solve_labelled.qsop
build/sop-solve --format stats --include-result tests/golden/solve_labelled.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
```

Benchmark tables can be refreshed from generated JSONL and import reports:

```sh
tools/refresh_scoreboard.py --artifact-dir /tmp --output scoreboard.md
tools/refresh_scoreboard.py --artifact-dir /tmp --run-native --run-large-sample --output scoreboard.md
tools/render_scoreboard.py --import-report corpus-report.json --solver-jsonl tier=solver.jsonl --native-jsonl tier=native.jsonl
tools/compare_rankwidth_generators.py --rankwidth-jsonl 33-64=rankwidth-sweep.jsonl --qsop-mode labelled
tools/compare_native_solver_results.py --solver-jsonl tier=solver.jsonl --native-jsonl tier=native.jsonl
tools/bench_qasm_native_simulator.py corpus.json --engine all --max-qubits 16 --engine-qubit-cap pyzx-matrix=10 --timeout 10
```

`refresh_scoreboard.py` is the public scoreboard path; `render_scoreboard.py`
is the lower-level table renderer for ad hoc reports. Use
`bench_qasm_corpus.py --qsop-mode labelled --rankwidth-diagnostics` for bounded
labelled-rankwidth generator sweeps. Solver benchmark JSONL includes QSOP
amplitudes when stats are collected, and native comparison reports mark how
many common rows had amplitude checks, mismatches, mean absolute error, and
maximum absolute error. Rankwidth generator comparisons include kernel-time
winners alongside table and signature pressure. Branch benchmark summaries now
also surface skip reasons, fallthrough size, cache canonical-entry rate, cache
key/count/estimated bytes, root treewidth probes, component splits, and
DP-delegate trace counts so hybrid handoff decisions are visible without
opening the raw trace.
