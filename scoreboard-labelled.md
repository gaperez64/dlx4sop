# Scoreboard — labelled QSOPs

Last updated: 2026-06-21. Per-instance timeout: 30 s.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 188 | 24 | 6 | 18 | 86 | 54 / 86 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 4 | 0 | 0 | 2 | 0 | 2 |
| PyZX | https://github.com/zxcalc/pyzx | 126 | 2 | 12 | 32 | 38 | 42 |

Total current solved coverage: **318 fixed-boundary benchmark rows**.
The 257-512 exploratory sample contributes 98 solved rows out of 130 attempted under the current timeout cap.

## Survival Curves

Fraction of instances solved within a given wall-clock budget per backend. Higher and further left is better.

### FeynmanDD

![Survival curves — FeynmanDD](scoreboard-assets/labelled/survival-feynmandd.svg)

### MQT Bench (small, ≤32 qubits)

Pre-expansion set: circuits with at most 32 qubits. Native simulator runs are only tracked from tier 33-64 upward; this plot shows QSOP solver survival curves only.

![Survival curves — MQT Bench (0-32 tier)](scoreboard-assets/labelled/survival-mqt-bench.svg)

### MQT Bench (large, 34–128 qubits)

Expanded set: GHZ and BV circuits at 34–128 qubits. The native baseline is `qiskit-clifford` (stabilizer formalism, O(n²) memory) because statevector engines were killed or timed out at 34+ qubits (34-qubit statevector ≈ 272 GB). This plot is regenerated with the rest of the scoreboard when new QSOP and native artifacts are available.

![Survival curves — MQT Bench (33-64 and 65-128 tiers)](scoreboard-assets/labelled/survival-mqt-bench-large.svg)

### PyZX

![Survival curves — PyZX](scoreboard-assets/labelled/survival-pyzx.svg)

## Solver Time by Tier

Median solve time per tier, log scale. Only `ok` rows counted.

![Solver time by tier](scoreboard-assets/labelled/solver-time-by-tier.svg)

## Speedup vs Treewidth Baseline

Speedup of each backend relative to treewidth on matched pairs. Bars above 1.0x mean the backend is faster.

![Speedup vs treewidth](scoreboard-assets/labelled/solver-speedup-vs-treewidth.svg)

## Branch Dispatch

Fraction of branch-solver calls dispatched to treewidth sub-solver, rankwidth sub-solver, or pure-branch fallthrough per tier.

![Branch dispatch by tier](scoreboard-assets/labelled/branch-dispatch-by-tier.svg)

## WMC Solve Time Breakdown

Export time vs Ganak time per WMC encoding and tier.

![WMC time breakdown](scoreboard-assets/labelled/wmc-time-breakdown.svg)

## Internal Solver Configurations

Best configuration per tier at a glance.

| Tier | Configuration | Solved | Total solve time |
| --- | --- | ---: | ---: |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 26 / 26 | 12.8 ms |
| 0-32 | `branch --branch-heuristic split` | 26 / 26 | 17.2 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 26 / 26 | 18.2 ms |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 26 / 26 | 34.36 s |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 18 / 18 | 10.6 ms |
| 33-64 | `branch --branch-heuristic split` | 18 / 18 | 11.9 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 18 / 18 | 47.4 ms |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 18 / 18 | 1.61 s |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 18 / 18 | 1.61 s |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 18 / 18 | 1.77 s |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 18 / 18 | 7.00 s |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 18 / 18 | 181.66 s |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 52 / 52 | 83.6 ms |
| 65-128 | `branch --branch-heuristic split` | 52 / 52 | 118.5 ms |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 52 / 52 | 4.70 s |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 52 / 52 | 9.73 s |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 52 / 52 | 9.73 s |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 52 / 52 | 10.59 s |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 124 / 124 | 4.05 s |
| 129-256 | `branch --branch-heuristic split` | 124 / 124 | 4.91 s |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 124 / 124 | 89.29 s |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 124 / 124 | 89.97 s |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 124 / 124 | 98.35 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 98 / 130 | 1191.96 s |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 90 / 130 | 1762.81 s |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 91 / 130 | 1763.15 s |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 84 / 130 | 1874.09 s |

## Competitor Comparisons

Best native simulator per source and tier. Speedup = native time / QSOP time, so a value above 1 (**bold**) means QSOP is faster. Native runs only on boundaries it can fit under its qubit cap and finish in time; the **Matched / QSOP-solved** column shows on how many of the solver's rows that holds — a high speedup on a small matched set means QSOP also wins on coverage.

### FeynmanDD

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 11.8 ms | `mqt-ddsim-statevector` | 205.5 ms | **17.42x** | 24 / 24 |
| 33-64 | 3.8 ms | `pyzx-matrix` | 8.62 s | **2287.93x** | 6 / 6 |
| 65-128 | 33.3 ms | `pyzx-matrix` | 16.72 s | **501.65x** | 18 / 18 |
| 129-256 | 22.2 ms | `pyzx-matrix` | 17.76 s | **798.25x** | 6 / 86 |

### MQT Bench

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 65-128 | 3.5 ms | `pyzx-matrix` | 11.0 ms | **3.17x** | 2 / 2 |

### PyZX

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 948.1 us | `mqt-ddsim-statevector` | 17.6 ms | **18.58x** | 2 / 2 |
| 33-64 | 6.8 ms | `pyzx-matrix` | 115.1 ms | **16.96x** | 12 / 12 |
| 65-128 | 46.8 ms | `pyzx-matrix` | 26.41 s | **564.11x** | 32 / 32 |
| 129-256 | 118.0 ms | `pyzx-matrix` | 36.52 s | **309.58x** | 18 / 38 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `treewidth --treewidth-order min-fill-max-degree`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 98 / 130 rows solve under the current timeout cap.
Treewidth is the clean direct-DP baseline; hybrid branch is the best widened-tier configuration once component splitting and treewidth handoff trigger. Against native baselines, QSOP is consistently faster than the `pyzx-matrix` tool, while dense `aer-statevector` still wins on some low-width FeynmanDD rows. Labelled QSOPs have no native baseline: the simulators only evaluate sign boundaries where input equals output.
