# Scoreboard — labelled QSOPs

Last updated: 2026-06-20. Per-instance timeout: 30 s.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 182 | 24 | 6 | 18 | 86 | 48 / 86 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 4 | 0 | 0 | 2 | 0 | 2 |
| PyZX | https://github.com/zxcalc/pyzx | 126 | 2 | 12 | 32 | 38 | 42 |

Total current solved coverage: **312 fixed-boundary benchmark rows**.
The 257-512 exploratory sample contributes 92 solved rows out of 130 attempted under the current timeout cap.

## Survival Curves

Fraction of instances solved within a given wall-clock budget per backend. Higher and further left is better.

### FeynmanDD

![Survival curves — FeynmanDD](scoreboard-assets/labelled/survival-feynmandd.svg)

### MQT Bench (small, ≤32 qubits)

Pre-expansion set: circuits with at most 32 qubits, compared against native statevector simulators. QSOP is not faster on this set — dense statevectors win at low qubit counts.

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
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 26 / 26 | 16.4 ms |
| 0-32 | `branch --branch-heuristic split` | 26 / 26 | 22.3 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 26 / 26 | 24.3 ms |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 26 / 26 | 37.01 s |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 18 / 18 | 14.3 ms |
| 33-64 | `branch --branch-heuristic split` | 18 / 18 | 20.4 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 18 / 18 | 119.1 ms |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 18 / 18 | 1.71 s |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 18 / 18 | 1.77 s |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 18 / 18 | 1.97 s |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 18 / 18 | 7.36 s |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 18 / 18 | 196.01 s |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 52 / 52 | 210.5 ms |
| 65-128 | `branch --branch-heuristic split` | 52 / 52 | 360.5 ms |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 52 / 52 | 10.34 s |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 52 / 52 | 10.46 s |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 52 / 52 | 10.82 s |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 52 / 52 | 11.83 s |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 124 / 124 | 13.92 s |
| 129-256 | `branch --branch-heuristic split` | 124 / 124 | 18.01 s |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 124 / 124 | 95.83 s |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 124 / 124 | 97.21 s |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 124 / 124 | 105.63 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 92 / 130 | 1561.11 s |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 92 / 130 | 1790.35 s |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 89 / 130 | 1799.82 s |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 82 / 130 | 1902.03 s |

## Competitor Comparisons

Best-performing native simulator per source and tier.

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `treewidth --treewidth-order min-fill-max-degree`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 92 / 130 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
