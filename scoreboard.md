# Scoreboard — sign QSOPs

Last updated: 2026-06-29. Per-instance timeout: 30 s.

This tracks progress toward a competitive exact strong simulator based on signed quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Internal corpus | tests/qasm_solver_corpus.json | 14 | 14 | 0 | 0 | 0 | 0 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 288 | 42 | 24 | 166 | 42 | 14 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 146 | 32 | 72 | 42 | 0 | 0 |
| PyZX | https://github.com/zxcalc/pyzx | 180 | 44 | 14 | 36 | 40 | 46 |

Total current solved coverage: **628 fixed-boundary benchmark rows**.
The 257-512 exploratory sample contributes 60 solved rows out of 60 attempted under the current timeout cap.

## Survival Curves

Fraction of instances solved within a given wall-clock budget per backend. Higher and further left is better.

### FeynmanDD

![Survival curves — FeynmanDD](scoreboard-assets/survival-feynmandd.svg)

### MQT Bench (small, ≤32 qubits)

Pre-expansion set: circuits with at most 32 qubits, compared against the best native simulator that fits each boundary under its qubit cap.

![Survival curves — MQT Bench (0-32 tier)](scoreboard-assets/survival-mqt-bench.svg)

### MQT Bench (large, 34–128 qubits)

Expanded set: GHZ and BV circuits at 34–128 qubits. The native baseline is `qiskit-clifford` (stabilizer formalism, O(n²) memory) because statevector engines were killed or timed out at 34+ qubits (34-qubit statevector ≈ 272 GB). This plot is regenerated with the rest of the scoreboard when new QSOP and native artifacts are available.

![Survival curves — MQT Bench (33-64 and 65-128 tiers)](scoreboard-assets/survival-mqt-bench-large.svg)

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

## WMC vs Solver Scaling

Synthetic phase-polynomial circuits (committed under `benchmarks/corpus/sop/synthetic/scaling/`) whose QSOP treewidth grows with the qubit count. Real benchmark families cannot show this: the scalable MQT families use continuous-angle gates the finite-modulus importer rejects, and the importable ones are Clifford with trivial treewidth. As treewidth grows the branch backend collapses first. Across the sizes both solve, the treewidth DP stays ahead of ganak (WMC) — the DP's lead narrows as treewidth grows, so any crossover lies past the point where ganak itself stays tractable. Largest size solved under the current cap: branch 16q, treewidth 24q, ganak 20q.

![WMC vs solver scaling](scoreboard-assets/wmc-vs-solver-scaling.svg)

## Internal Solver Configurations

Best configuration per tier at a glance.

| Tier | Configuration | Solved | Total solve time |
| --- | --- | ---: | ---: |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 132 / 132 | 53.0 ms |
| 0-32 | `branch --branch-heuristic split` | 132 / 132 | 59.5 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 132 / 132 | 426.6 ms |
| 0-32 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 121 / 132 | 2.71 s |
| 0-32 | `sop2wmc --encoding amp-block + ganak --mode 6` | 121 / 132 | 2.72 s |
| 0-32 | `sop2wmc --encoding amplitude + ganak --mode 6` | 121 / 132 | 3.03 s |
| 0-32 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 121 / 132 | 11.18 s |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 132 / 132 | 470.75 s |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 38 / 38 | 21.9 ms |
| 33-64 | `branch:auto` | 72 / 72 | 27.7 ms |
| 33-64 | `branch --branch-heuristic split` | 38 / 38 | 31.1 ms |
| 33-64 | `treewidth --treewidth-order min-fill` | 72 / 72 | 34.2 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 38 / 38 | 47.1 ms |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 38 / 38 | 1.88 s |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 38 / 38 | 1.89 s |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 38 / 38 | 2.61 s |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 38 / 38 | 10.37 s |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 17 / 38 | 1478.01 s |
| 65-128 | `branch:auto` | 42 / 42 | 16.6 ms |
| 65-128 | `treewidth --treewidth-order min-fill` | 42 / 42 | 36.1 ms |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 202 / 202 | 348.2 ms |
| 65-128 | `branch --branch-heuristic split` | 202 / 202 | 435.0 ms |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 202 / 202 | 38.52 s |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 202 / 202 | 39.24 s |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 202 / 202 | 44.48 s |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 114 / 202 | 2119.62 s |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 82 / 82 | 599.3 ms |
| 129-256 | `branch --branch-heuristic split` | 82 / 82 | 605.2 ms |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 82 / 82 | 35.17 s |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 82 / 82 | 35.60 s |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 82 / 82 | 38.46 s |
| 129-256 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 40 / 82 | 1285.11 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 60 / 60 | 65.13 s |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 56 / 60 | 214.83 s |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 56 / 60 | 215.48 s |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 56 / 60 | 216.41 s |

## Competitor Comparisons

Best native simulator per source and tier. Speedup = native time / QSOP time, so a value above 1 (**bold**) means QSOP is faster. Native runs only on boundaries it can fit under its qubit cap and finish in time; the **Matched / QSOP-solved** column shows on how many of the solver's rows that holds — a high speedup on a small matched set means QSOP also wins on coverage.

### Internal corpus

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 5.7 ms | `mqt-ddsim-statevector` | 132.9 ms | **23.36x** | 14 / 14 |

### FeynmanDD

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 17.1 ms | `qiskit-statevector` | 1.14 s | **66.71x** | 42 / 42 |
| 65-128 | 8.0 ms | `pyzx-matrix` | 19.39 s | **2428.69x** | 4 / 166 |
| 129-256 | 21.4 ms | `qiskit-clifford` | 562.2 ms | **26.31x** | 2 / 42 |

### MQT Bench

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 12.2 ms | `pyzx-matrix` | 608.1 ms | **50.05x** | 32 / 32 |
| 33-64 | 27.7 ms | `qiskit-clifford` | 50.32 s | **1814.47x** | 72 / 72 |
| 65-128 | 12.8 ms | `qiskit-clifford` | 205.57 s | **16017.23x** | 34 / 42 |

### PyZX

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 17.7 ms | `mqt-ddsim-statevector` | 366.8 ms | **20.71x** | 44 / 44 |
| 33-64 | 8.0 ms | `pyzx-matrix` | 284.0 ms | **35.28x** | 14 / 14 |
| 65-128 | 51.5 ms | `pyzx-matrix` | 27.58 s | **535.55x** | 34 / 36 |
| 129-256 | 212.7 ms | `pyzx-matrix` | 40.35 s | **189.70x** | 22 / 40 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `treewidth --treewidth-order min-fill-max-degree`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 column is an exploratory stratified sample (60 rows), not the full tier; all solve under the current timeout cap.
Treewidth is the clean direct-DP baseline; hybrid branch is the best widened-tier configuration once component splitting and treewidth handoff trigger. Against native baselines, QSOP is consistently faster than the `pyzx-matrix` tool, while dense `aer-statevector` still wins on some low-width FeynmanDD rows.
