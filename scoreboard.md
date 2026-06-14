# Scoreboard

Last updated: 2026-06-14.

This tracks progress toward a competitive exact strong simulator based on
labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong
simulation: import a static circuit into QSOP, solve the exact residue-count
histogram, and compare with native simulators where possible.

## Benchmarks Used

The scoreboard reports fixed-boundary benchmark rows currently used in solver
comparisons. External sources are cited by upstream repository.

| Source | Upstream | Total | 0-32 | 33-64 | 65-128 | 129-256 | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | `tests/qasm_solver_corpus.json` | 32 | 32 | 0 | 0 | 0 | sign 32 |
| FeynmanDD | <https://github.com/cqs-thu/feynman-decision-diagram> | 228 | 52 | 16 | 92 | 68 | sign 161; labelled 67 |
| MQT Bench | <https://github.com/munich-quantum-toolkit/bench> | 22 | 20 | 0 | 2 | 0 | sign 20; labelled 2 |
| PyZX | <https://github.com/zxcalc/pyzx> | 125 | 29 | 16 | 36 | 44 | sign 81; labelled 44 |

Total current solver scoreboard coverage: 407 fixed-boundary benchmark rows.

## Internal Solver Configurations

Rows are sorted within each imported-variable tier by total solve time. `Solved`
is successful solver rows over attempted rows.

| Tier | Configuration | Solved | Total solve time | Width/table | Notes |
| --- | --- | ---: | ---: | --- | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 58.0 ms | tw 2; max table 64 | 12,692 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 66.9 ms | rw 3; max table 48 | 12 zero-variable rows skipped by generated-decomposition guard |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 80.2 ms | width 2; max table 64 | 9,545 nodes; treewidth delegations 1 |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 26.3 ms | tw 3; max table 128 | 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 27.0 ms | tw 3; max table 128 | 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 27.3 ms | tw 3; max table 128 | 16,176 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 39.5 ms | width 3; max table 128 | 3,525 nodes; treewidth delegations 20 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 130.9 ms | rw 6; max table 512 | 141,928 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 668.7 ms | tw 7; max table 2,048 | 289,681 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 669.4 ms | tw 7; max table 2,048 | 293,297 join pairs |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 676.5 ms | tw 8; max table 4,096 | 293,423 join pairs |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 1.33 s | width 7; max table 2,048 | 1,753 nodes; treewidth delegations 121 |
| 129-256 | `branch --branch-heuristic split` | 112 / 112 | 7.87 s | residual tw 14; max table 262,144 | 6,566 nodes; treewidth delegations 115 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 9.41 s | tw 14; max table 262,144 | 7,044,136 join pairs |

Best current internal configurations by tier: treewidth `min-degree` for 0-32,
treewidth `min-fill` for 33-64, treewidth `min-fill-max-degree` for 65-128,
and hybrid branch for 129-256.

## Competitor Comparisons

These compare QSOP against native QASM baselines on common fixed-boundary rows.
Until `sop2X` exporters exist, each native tool is compared only on the QASM
rows from the corresponding source that it can parse and fit under its cap.
Speedup is native elapsed time divided by QSOP solve time over rows where both
completed. Values above `1.00x` mean QSOP is faster.

The QSOP configuration is the best current internal configuration for the tier
with native data: treewidth `min-fill` on 33-64 and treewidth
`min-fill-max-degree` on 65-128. Historical native rows did not record timeout
or memory caps; the harness now supports those fields for future refreshes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Native cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 4 / 16 | 3.8 ms | 4.1 ms | 1.07x | 30 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `aer-statevector` | 4 / 16 | 3.8 ms | 9.4 ms | 2.48x | 30 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `mqt-ddsim-statevector` | 4 / 16 | 3.8 ms | 72.1 ms | 19.00x | 30 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `pyzx-matrix` | 4 / 16 | 3.8 ms | 4.77 s | 1256.94x | 30 | 10 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 35 / 92 | 136.4 ms | 51.45 s | 377.23x | 100 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 35 / 92 | 136.4 ms | 18.48 s | 135.52x | 100 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 35 / 92 | 136.4 ms | 109.45 s | 802.50x | 100 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 11 / 92 | 63.2 ms | 24.63 s | 389.48x | 100 | 10 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Native cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 2 / 2 | 9.0 ms | 2.5 ms | 0.28x | 3 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 2 / 2 | 9.0 ms | 28.7 ms | 3.20x | 3 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 2 / 2 | 9.0 ms | 21.7 ms | 2.41x | 3 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 2 / 2 | 9.0 ms | 11.0 ms | 1.22x | 3 | 10 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Native cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 16 / 16 | 12.7 ms | 24.9 ms | 1.96x | 7 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `aer-statevector` | 16 / 16 | 12.7 ms | 70.6 ms | 5.56x | 7 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `mqt-ddsim-statevector` | 16 / 16 | 12.7 ms | 319.8 ms | 25.20x | 7 | 16 |
| 33-64 | `treewidth --treewidth-order min-fill` | `pyzx-matrix` | 16 / 16 | 12.7 ms | 440.4 ms | 34.71x | 7 | 10 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 36 / 36 | 184.5 ms | 1.56 s | 8.46x | 14 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 36 / 36 | 184.5 ms | 194.8 ms | 1.06x | 14 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 36 / 36 | 184.5 ms | 2.01 s | 10.90x | 14 | 16 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 35 / 36 | 171.9 ms | 81.61 s | 474.67x | 14 | 10 |

## Current Takeaway

Treewidth is the clean direct-DP baseline and handles all promoted tiers through
129-256 variables. Hybrid branch is now the fastest 129-256 configuration after
root treewidth order reuse. Against dense native baselines, QSOP is usually
faster on the current external common rows, but Qiskit is faster on the tiny MQT
Bench subset. The next comparison refresh should regenerate native rows with
explicit timeout and memory caps and then extend feasible native comparisons
beyond 65-128.
