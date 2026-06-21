# Scoreboard — sign QSOPs

Last updated: 2026-06-21. Per-instance timeout: 30 s.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 338 | 80 | 28 | 166 | 50 | 14 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 170 | 56 | 72 | 42 | 0 | 0 |
| PyZX | https://github.com/zxcalc/pyzx | 206 | 44 | 20 | 36 | 48 | 58 |

Total current solved coverage: **746 fixed-boundary benchmark rows**.
The 257-512 exploratory sample contributes 72 solved rows out of 72 attempted under the current timeout cap.

## Survival Curves

Fraction of instances solved within a given wall-clock budget per backend. Higher and further left is better.

### FeynmanDD

![Survival curves — FeynmanDD](scoreboard-assets/sign/survival-feynmandd.svg)

### MQT Bench (small, ≤32 qubits)

Pre-expansion set: circuits with at most 32 qubits. Native simulator runs are only tracked from tier 33-64 upward; this plot shows QSOP solver survival curves only.

![Survival curves — MQT Bench (0-32 tier)](scoreboard-assets/sign/survival-mqt-bench.svg)

### MQT Bench (large, 34–128 qubits)

Expanded set: GHZ and BV circuits at 34–128 qubits. The native baseline is `qiskit-clifford` (stabilizer formalism, O(n²) memory) because statevector engines were killed or timed out at 34+ qubits (34-qubit statevector ≈ 272 GB). This plot is regenerated with the rest of the scoreboard when new QSOP and native artifacts are available.

![Survival curves — MQT Bench (33-64 and 65-128 tiers)](scoreboard-assets/sign/survival-mqt-bench-large.svg)

### PyZX

![Survival curves — PyZX](scoreboard-assets/sign/survival-pyzx.svg)

## Solver Time by Tier

Median solve time per tier, log scale. Only `ok` rows counted.

![Solver time by tier](scoreboard-assets/sign/solver-time-by-tier.svg)

## Speedup vs Treewidth Baseline

Speedup of each backend relative to treewidth on matched pairs. Bars above 1.0x mean the backend is faster.

![Speedup vs treewidth](scoreboard-assets/sign/solver-speedup-vs-treewidth.svg)

## Branch Dispatch

Fraction of branch-solver calls dispatched to treewidth sub-solver, rankwidth sub-solver, or pure-branch fallthrough per tier.

![Branch dispatch by tier](scoreboard-assets/sign/branch-dispatch-by-tier.svg)

## WMC Solve Time Breakdown

Export time vs Ganak time per WMC encoding and tier.

![WMC time breakdown](scoreboard-assets/sign/wmc-time-breakdown.svg)

## WMC vs Solver Scaling

Synthetic phase-polynomial circuits (committed under `benchmarks/corpus/sop/synthetic/scaling/`) whose QSOP treewidth grows with the qubit count. Real benchmark families cannot show this: the scalable MQT families use continuous-angle gates the finite-modulus importer rejects, and the importable ones are Clifford with trivial treewidth. As treewidth grows the branch backend collapses first. Across the sizes both solve, the treewidth DP stays ahead of ganak (WMC) — the DP's lead narrows as treewidth grows, so any crossover lies past the point where ganak itself stays tractable. Largest size solved under the current cap: branch 16q, treewidth 24q, ganak 20q.

![WMC vs solver scaling](scoreboard-assets/sign/wmc-vs-solver-scaling.svg)

## Internal Solver Configurations

Best configuration per tier at a glance.

| Tier | Configuration | Solved | Total solve time |
| --- | --- | ---: | ---: |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 212 / 212 | 90.2 ms |
| 0-32 | `branch --branch-heuristic split` | 212 / 212 | 111.2 ms |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 212 / 212 | 476.0 ms |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 212 / 212 | 496.53 s |
| 33-64 | `branch:auto` | 72 / 72 | 31.9 ms |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 48 / 48 | 32.6 ms |
| 33-64 | `treewidth --treewidth-order min-fill` | 72 / 72 | 35.9 ms |
| 33-64 | `branch --branch-heuristic split` | 48 / 48 | 43.5 ms |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 48 / 48 | 65.3 ms |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 48 / 48 | 2.56 s |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 48 / 48 | 2.57 s |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 48 / 48 | 3.51 s |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 48 / 48 | 13.75 s |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 27 / 48 | 1680.29 s |
| 65-128 | `branch:auto` | 42 / 42 | 18.5 ms |
| 65-128 | `treewidth --treewidth-order min-fill` | 42 / 42 | 40.5 ms |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 202 / 202 | 355.0 ms |
| 65-128 | `branch --branch-heuristic split` | 202 / 202 | 444.3 ms |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 202 / 202 | 37.00 s |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 202 / 202 | 37.24 s |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 202 / 202 | 41.36 s |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 114 / 202 | 3819.26 s |
| 129-256 | `branch --branch-heuristic split` | 98 / 98 | 725.3 ms |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 98 / 98 | 827.0 ms |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 98 / 98 | 37.03 s |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 98 / 98 | 37.11 s |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 98 / 98 | 42.09 s |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 72 / 72 | 65.57 s |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 68 / 72 | 215.33 s |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 68 / 72 | 215.92 s |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 68 / 72 | 216.87 s |

## Competitor Comparisons

Best native simulator per source and tier. Speedup = native time / QSOP time, so a value above 1 (**bold**) means QSOP is faster. Native runs only on boundaries it can fit under its qubit cap and finish in time; the **Matched / QSOP-solved** column shows on how many of the solver's rows that holds — a high speedup on a small matched set means QSOP also wins on coverage.

### Internal corpus

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 14.1 ms | `mqt-ddsim-statevector` | 293.4 ms | **20.84x** | 32 / 32 |

### FeynmanDD

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 34.0 ms | `mqt-ddsim-statevector` | 738.7 ms | **21.72x** | 80 / 80 |
| 33-64 | 3.0 ms | `pyzx-matrix` | 22.6 ms | **7.46x** | 4 / 28 |
| 65-128 | 8.9 ms | `pyzx-matrix` | 18.34 s | **2055.92x** | 4 / 166 |
| 129-256 | 22.3 ms | `qiskit-clifford` | 527.3 ms | **23.61x** | 2 / 50 |

### MQT Bench

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 22.7 ms | `pyzx-matrix` | 471.6 ms | **20.82x** | 56 / 56 |
| 33-64 | 31.9 ms | `qiskit-clifford` | 48.02 s | **1503.66x** | 72 / 72 |
| 65-128 | 15.5 ms | `qiskit-clifford` | 214.35 s | **13794.90x** | 36 / 42 |

### PyZX

| Tier | QSOP time | Best native | Native time | Best speedup | Matched / QSOP-solved |
| --- | ---: | --- | ---: | ---: | ---: |
| 0-32 | 17.5 ms | `mqt-ddsim-statevector` | 361.8 ms | **20.68x** | 44 / 44 |
| 33-64 | 13.5 ms | `pyzx-matrix` | 185.9 ms | **13.81x** | 20 / 20 |
| 65-128 | 54.0 ms | `pyzx-matrix` | 29.53 s | **546.70x** | 34 / 36 |
| 129-256 | 158.7 ms | `pyzx-matrix` | 4.68 s | **29.46x** | 18 / 48 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 72 / 72 rows solve under the current timeout cap.
Treewidth is the clean direct-DP baseline; hybrid branch is the best widened-tier configuration once component splitting and treewidth handoff trigger. Against native baselines, QSOP is consistently faster than the `pyzx-matrix` tool, while dense `aer-statevector` still wins on some low-width FeynmanDD rows. Labelled QSOPs have no native baseline: the simulators only evaluate sign boundaries where input equals output.
