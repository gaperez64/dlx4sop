# Scoreboard

Last updated: 2026-06-14.

This scoreboard tracks progress toward a competitive SOP-based strong simulator.
The current benchmark target is exact fixed-boundary strong simulation: import a
static circuit into a labelled quadratic SOP, solve the resulting residue-count
histogram exactly, and compare amplitudes against independent simulators where
available.

## Corpus Sources

External sources are cited by upstream repository. Generated manifests and
reports are local benchmark artifacts, not citation targets.

Use `tools/summarize_qasm_report.py` on the generated per-source import reports
to reproduce tier tables from structured data. The default tiers are `0-32`,
`33-64`, `65-128`, `129-256`, and `257+` imported SOP variables.

| Source | Upstream | Inputs scanned | Emitted at 32 vars | Too large at 32 vars | Other unsupported |
| --- | --- | ---: | ---: | ---: | ---: |
| Internal corpus | `tests/qasm_solver_corpus.json` | 12 cases | 12 cases | 0 | 0 |
| PyZX | <https://github.com/zxcalc/pyzx> | 344 | 29 | 292 | 23 |
| MQT Bench | <https://github.com/munich-quantum-toolkit/bench> | 32 | 20 | 3 | 9 |
| FeynmanDD | <https://github.com/cqs-thu/feynman-decision-diagram> | 425 | 52 | 361 | 12 |
| Total |  | 813 | 113 cases | 656 | 44 |

The 32-variable emitted pool has 133 fixed-boundary amplitudes: 32 internal, 29
PyZX, 20 MQT Bench, and 52 FeynmanDD. It contains 120 sign-only boundaries and
13 labelled boundaries. The largest emitted cases currently have 32 imported SOP
variables and 32 quadratic terms.

There are many importable but filtered cases above the current 32-variable
scoreboard cap: 292 PyZX cases, 361 FeynmanDD cases, and 3 MQT Bench cases. That
is the next source of larger benchmark instances.

## Current dlx4sop Results

Benchmark command shape:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve \
  --backend branch --backend rankwidth --backend treewidth \
  --rankwidth-generate min-fill-cut --rankwidth-mode count-table \
  --treewidth-order min-fill --treewidth-order min-degree \
  --treewidth-order min-fill-max-degree --trace --format summary
```

| Backend/configuration | Boundaries solved | Total solve time | Key stats |
| --- | ---: | ---: | --- |
| `branch --branch-heuristic split` | 133 / 133 | 81.1 ms | 9,781 nodes; 15,314 leaf assignments; cache 0 / 9,781 |
| `branch --branch-heuristic treewidth` | 133 / 133 | 677.0 ms | 91,543 nodes; cache 42,736 / 48,807; hit rate 0.467 |
| `branch --branch-heuristic cutrank-proxy` | 133 / 133 | 14.48 s | 4,007,973 nodes; cache 62,992 / 3,944,981 |
| `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 133 | 70.5 ms | max width 3; max table 48; max signatures 8 |
| `treewidth --treewidth-order min-degree` | 133 / 133 | 56.9 ms | max width 2; max table 64; 12,692 join pairs |
| `treewidth --treewidth-order min-fill` | 133 / 133 | 59.4 ms | max width 2; max table 64; 12,692 join pairs |
| `treewidth --treewidth-order min-fill-max-degree` | 133 / 133 | 58.2 ms | max width 2; max table 64; 12,690 join pairs |

The current `rankwidth` row skips 12 zero-variable boundary reductions because
the generated decomposition path refuses empty decompositions. Those are a
benchmark/tooling guard issue, not hard instances.

The `components` backend was also run on the subset with at most 16 imported SOP
variables: 100 / 100 boundaries solved in 52.9 ms, with 190 components and a
component-cache hit rate of 0.379. It remains useful for disconnected small
components but is not the current full-pool baseline.

## Heuristic Note

`cutrank-proxy` is not the exact graph parameter linear rankwidth, and it is not
the rankwidth DP solver. It is a branch variable-ordering heuristic. At each
residual state, it scores candidate variables using a local GF(2) cut-rank proxy
between the candidate's active neighbors and the remaining active graph, then
branches on the smallest score with the normal split heuristic as a tie-break.
It does not compute, optimize, or certify a global linear layout. On the current
pool it is too expensive relative to the benefit, so `split` remains the branch
baseline.

## Independent Checks

| Checker | Coverage so far | Status |
| --- | ---: | --- |
| Built-in exact agreement tests | CI-sized golden and corpus tests | Passing |
| Qiskit optional manifest check | 97 checked, 3 skipped by Qiskit parser support | Passing checked cases |
| Qiskit simulator speed baseline | Not run | Pending |
| Aer speed baseline | Not run | Pending |
| MQT simulator baseline | Not run | Pending |
| ZX-calculus simulator baselines | Not run | Pending |

The skipped Qiskit manifest cases use gates this local Qiskit OpenQASM parser did
not define from the source text, such as `iswap` and `ccz`.

Until `sop2X` exporters exist, competitor speed comparisons should be native-set
comparisons: run `dlx4sop` on the source cases it can import, and run each
external simulator on that simulator's native benchmark format. For QASM-backed
sets, `tools/bench_qasm_native_simulator.py` records Qiskit Statevector or Aer
fixed-boundary amplitude timings directly from manifest QASM.

## Current Takeaway

The current source-attributed pool is good enough to exercise importer plumbing,
labelled SOP support, treewidth/rankwidth tracing, and branch cache behavior. It
is not yet hard enough to judge competitiveness: decomposition widths are still
small, and the most important next move is to promote larger importable cases
above the 32-variable cap into a controlled benchmark tier.
