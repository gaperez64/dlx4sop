# Scoreboard

Last updated: 2026-06-19.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 520 | 104 | 34 | 184 | 136 | 62 / 100 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 114 | 0 | 72 | 42 | 0 | 0 |
| PyZX | https://github.com/zxcalc/pyzx | 330 | 46 | 32 | 68 | 86 | 98 / 100 |

Total current solved coverage: **996 fixed-boundary benchmark rows**.
The 257-512 exploratory sample contributes 162 solved rows out of 202 attempted under the current timeout cap.

## Survival Curves

Fraction of instances solved within a given wall-clock budget per backend. Higher and further left is better.

### FeynmanDD

![Survival curves — FeynmanDD](scoreboard-assets/survival-feynmandd.svg)

### MQT Bench

![Survival curves — MQT Bench](scoreboard-assets/survival-mqt-bench.svg)

### PyZX

![Survival curves — PyZX](scoreboard-assets/survival-pyzx.svg)

## Solver Time by Tier

Median solve time per tier, log scale. Only `ok` rows counted.

![Solver time by tier](scoreboard-assets/solver-time-by-tier.svg)

## Speedup vs Treewidth Baseline

Speedup of each backend relative to treewidth on matched pairs. Bars above 1.0x mean the backend is faster.

![Speedup vs treewidth](scoreboard-assets/solver-speedup-vs-treewidth.svg)

## Branch Dispatch

Fraction of branch-solver calls dispatched to treewidth sub-solver, rankwidth sub-solver, or pure-branch fallthrough per tier.

![Branch dispatch by tier](scoreboard-assets/branch-dispatch-by-tier.svg)

## WMC Solve Time Breakdown

Export time vs Ganak time per WMC encoding and tier.

![WMC time breakdown](scoreboard-assets/wmc-time-breakdown.svg)

## Internal Solver Configurations

Best configuration per tier at a glance. See [scoreboard-details.md](scoreboard-details.md) for full verbose stats.

| Tier | Configuration | Solved | Total solve time |
| --- | --- | ---: | ---: |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 238 / 238 | 131.3 ms |
| 0-32 | `branch --branch-heuristic split` | 238 / 238 | 170.7 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep` | 238 / 238 | 760.2 ms |
| 0-32 | `sop2wmc amp-soft + ganak` | 238 / 238 | 2.65 s |
| 0-32 | `sop2wmc residue + ganak` | 238 / 238 | 571.89 s |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 66 / 66 | 63.3 ms |
| 33-64 | `branch --branch-heuristic split` | 66 / 66 | 114.4 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut` | 66 / 66 | 311 ms |
| 33-64 | `sop2wmc amp-soft + ganak` | 66 / 66 | 3.12 s |
| 33-64 | `sop2wmc residue + ganak` | 45 / 66 | 2001.24 s |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 254 / 254 | 1.32 s |
| 65-128 | `branch --branch-heuristic split` | 254 / 254 | 2.07 s |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut` | 118 / 254 | 4102 s |
| 65-128 | `sop2wmc amp-block + ganak` | 254 / 254 | 38.03 s |
| 129-256 | `rankwidth --rankwidth-generate min-fill-cut` | 92 / 222 | 304 s |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 222 / 222 | 18.37 s |
| 129-256 | `branch --branch-heuristic split` | 222 / 222 | 24.08 s |
| 129-256 | `sop2wmc amp-block + ganak` | 222 / 222 | 109.70 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 162 / 202 | 1760.96 s |
| 257-512 sample | `sop2wmc amp-block + ganak` | 158 / 202 | 1955.80 s |

## Competitor Comparisons

Best-performing native simulator per source and tier. See [scoreboard-details.md](scoreboard-details.md) for the full per-engine breakdown.

### FeynmanDD

| Tier | QSOP time | Best native | Native time | Best speedup |
| --- | ---: | --- | ---: | ---: |
| 33-64 | 11.8 ms | `pyzx-matrix` | 9.69 s | **822.57x** |
| 65-128 | 285.7 ms | `pyzx-matrix` | 48.68 s | **378.50x** |
| 129-256 | 617.1 ms | `pyzx-matrix` | 22.43 s | **278.22x** |

### MQT Bench

GHZ and BV circuits (34–104 qubits). Native baseline: `qiskit-clifford` stabilizer simulation (O(n²) memory, exact amplitudes for Clifford circuits). Statevector engines (`mqt-ddsim-statevector`, `aer-statevector`, `qiskit-statevector`) were all killed or timed out — a 34-qubit statevector needs ~272 GB — so `qiskit-clifford` is the only viable native comparison.

| Tier | QSOP time | Best native | Native time | Best speedup |
| --- | ---: | --- | ---: | ---: |
| 33-64 | 33.9 ms | `qiskit-clifford` | 44.80 s | **1323x** |
| 65-128 | 19.5 ms | `qiskit-clifford` | 249.58 s | **12794x** |

### PyZX

| Tier | QSOP time | Best native | Native time | Best speedup |
| --- | ---: | --- | ---: | ---: |
| 33-64 | 29.2 ms | `pyzx-matrix` | 2.13 s | **72.80x** |
| 65-128 | 330.9 ms | `pyzx-matrix` | 88.77 s | **286.97x** |
| 129-256 | 2.68 s | `pyzx-matrix` | 119.25 s | **82.05x** |

## Current Takeaway

Best current internal configurations by tier: all tiers use `treewidth --treewidth-order min-fill-max-degree` as the direct DP baseline; `branch --branch-heuristic split` is the best widened-tier configuration when component splitting and treewidth handoff trigger (close to treewidth speed, wider coverage). Rankwidth wins on structured instances where its width is significantly smaller than treewidth width.

The 257-512 sample (162 / 202 rows solved) is exploratory: dense statevector native tools can win on low-qubit rows, but QSOP remains strong on fixed-boundary rows with large imported variable counts where statevector simulation is infeasible.

---

*Full verbose stats and per-engine competitor tables: [scoreboard-details.md](scoreboard-details.md)*
