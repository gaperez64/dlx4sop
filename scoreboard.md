# Scoreboard — sign QSOPs

Last updated: 2026-06-30. Per-instance timeout: 30 s.

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

Synthetic phase-polynomial circuits (committed under `benchmarks/corpus/sop/synthetic/scaling/`) whose QSOP treewidth grows with the qubit count. Real benchmark families cannot show this: the scalable MQT families use continuous-angle gates the finite-modulus importer rejects, and the importable ones are Clifford with trivial treewidth. As treewidth grows the branch backend collapses first. Across the sizes both solve, the treewidth DP stays ahead of ganak (WMC) — the DP's lead narrows as treewidth grows, so any crossover lies past the point where ganak itself stays tractable. Largest size solved under the current cap: branch 16q, treewidth 24q, ganak 24q.

![WMC vs solver scaling](scoreboard-assets/wmc-vs-solver-scaling.svg)

## Rankwidth Separation Study

Synthetic sign-edge QSOPs under `benchmarks/corpus/sop/synthetic/rankwidth/` instantiate the binary-tree clique-blowup family from the rankwidth note. The intended signal is not broad corpus coverage: it is whether the rankwidth backend stays at constant cutrank while treewidth tables grow with the clique blow-up parameter.

| Config | OK / rows | Time | Kernel time | Max width | Max table | Forecast pressure |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `rankwidth:best:count-table` | 7 / 7 | 46.8 ms | 435.2 us | 1 | 16 | 112 |
| `rankwidth:best:fourier` | 7 / 7 | 46.3 ms | 153.8 us | 1 | 8 | 112 |
| `rankwidth:best:fourier:dense-reference` | 7 / 7 | 46.9 ms | 204.7 us | 1 | 8 | 112 |
| `rankwidth:from-treewidth:count-table` | 7 / 7 | 62.0 ms | 558.6 us | 2 | 32 | 224 |
| `rankwidth:from-treewidth:fourier` | 7 / 7 | 34.0 ms | 210.6 us | 2 | 16 | 224 |
| `rankwidth:from-treewidth:fourier:dense-reference` | 7 / 7 | 33.4 ms | 266.1 us | 2 | 16 | 224 |
| `treewidth:min-fill-max-degree` | 6 / 7 | 252.1 ms | 0 ns | 15 | 524,288 | 0 |

`rankwidth:best:fourier` was faster than treewidth on 3 / 6 common solved rows, with lower/equal table size on 6 / 6 rows. This is the targeted RQ2 check: the table pressure separates even when total wall time still includes rank-decomposition and prototype overhead.

## Internal Solver Configurations

Best configuration per tier at a glance.

| Tier | Configuration | Solved | Total solve time |
| --- | --- | ---: | ---: |
| 0-32 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 132 / 132 | 76.7 ms |
| 0-32 | `sop2wmc --encoding amp-block + ganak --mode 6` | 132 / 132 | 81.0 ms |
| 0-32 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 132 / 132 | 81.3 ms |
| 0-32 | `sop2wmc --encoding amplitude + ganak --mode 6` | 132 / 132 | 87.8 ms |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 132 / 132 | 143.8 ms |
| 0-32 | `branch --branch-heuristic split` | 132 / 132 | 144.7 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 132 / 132 | 459.1 ms |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 132 / 132 | 466.66 s |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 38 / 38 | 27.7 ms |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 38 / 38 | 29.5 ms |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 38 / 38 | 30.0 ms |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 38 / 38 | 30.2 ms |
| 33-64 | `branch --branch-heuristic split` | 38 / 38 | 55.3 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 38 / 38 | 68.5 ms |
| 33-64 | `branch:auto` | 72 / 72 | 75.8 ms |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 110 / 110 | 129.9 ms |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 17 / 38 | 1462.60 s |
| 65-128 | `branch:auto` | 42 / 42 | 45.3 ms |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 244 / 244 | 521.2 ms |
| 65-128 | `branch --branch-heuristic split` | 202 / 202 | 533.1 ms |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 202 / 202 | 11.80 s |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 202 / 202 | 13.39 s |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 202 / 202 | 13.64 s |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 114 / 202 | 295.32 s |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 82 / 82 | 604.1 ms |
| 129-256 | `branch --branch-heuristic split` | 82 / 82 | 610.0 ms |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 82 / 82 | 11.10 s |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 82 / 82 | 11.39 s |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 82 / 82 | 12.65 s |
| 129-256 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 40 / 82 | 148.91 s |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 60 / 60 | 16.16 s |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 60 / 60 | 16.94 s |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 60 / 60 | 18.13 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 60 / 60 | 63.14 s |

## Competitor Comparisons

Best native simulator per source and tier. Speedup = native time / QSOP time, so a value above 1 (**bold**) means QSOP is faster. Native runs only on boundaries it can fit under its qubit cap and finish in time; the **Matched / QSOP-solved** column shows on how many of the solver's rows that holds — a high speedup on a small matched set means QSOP also wins on coverage.

### Internal corpus

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 5.4 ms | `mqt-ddsim-statevector` | 126.4 ms | **23.49x** | 14 / 14 |

### FeynmanDD

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 43.8 ms | `qiskit-statevector` | 653.0 ms | **14.92x** | 42 / 42 |
| 65-128 | 10.6 ms | `pyzx-matrix` | 17.45 s | **1652.52x** | 4 / 166 |
| 129-256 | 9.8 ms | `qiskit-clifford` | 557.3 ms | **56.88x** | 2 / 42 |

### MQT Bench

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 11.4 ms | `pyzx-matrix` | 450.2 ms | **39.51x** | 32 / 32 |
| 33-64 | 75.8 ms | `qiskit-clifford` | 47.50 s | **626.91x** | 72 / 72 |
| 65-128 | 38.6 ms | `qiskit-clifford` | 211.12 s | **5473.79x** | 36 / 42 |

### PyZX

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 16.2 ms | `mqt-ddsim-statevector` | 342.7 ms | **21.18x** | 44 / 44 |
| 33-64 | 17.1 ms | `pyzx-matrix` | 238.0 ms | **13.92x** | 14 / 14 |
| 65-128 | 72.0 ms | `pyzx-matrix` | 25.20 s | **350.14x** | 34 / 36 |
| 129-256 | 211.1 ms | `pyzx-matrix` | 34.35 s | **162.72x** | 22 / 40 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `treewidth --treewidth-order min-fill-max-degree`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 column is an exploratory stratified sample (60 rows), not the full tier; all solve under the current timeout cap.
Treewidth is the clean direct-DP baseline; hybrid branch is the best widened-tier configuration once component splitting and treewidth handoff trigger. Against native baselines, QSOP is consistently faster than the `pyzx-matrix` tool, while dense `aer-statevector` still wins on some low-width FeynmanDD rows.
