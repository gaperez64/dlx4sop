# Scoreboard

Last updated: 2026-06-14.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, reconstruct the requested amplitude, and compare with native simulators where possible.

## Benchmarks Used

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 | sign 32 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 241 | 52 | 16 | 92 | 68 | 13 / 25 | labelled 75; sign 166 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 24 | 20 | 0 | 2 | 0 | 2 | labelled 4; sign 20 |
| PyZX | https://github.com/zxcalc/pyzx | 144 | 29 | 16 | 36 | 44 | 19 / 25 | labelled 48; sign 96 |

Total current solved coverage: 441 fixed-boundary benchmark rows.
The 257-512 exploratory sample contributes 34 solved rows out of 52 attempted under the current timeout cap.

## Internal Solver Configurations

Rows are grouped by imported-variable tier and sorted by total solve time. `Solved` is successful solver rows over attempted rows.

| Tier | Configuration | Solved | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 58.0 ms | tw width 2; max table 64; 12,692 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 66.9 ms | rw width 3; max table 48; max signatures 8; 43,702 join pairs |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 80.2 ms | 9,545 nodes; max table 64; 325 join pairs; delegations tw=1, rw=0 |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 26.3 ms | tw width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 27.0 ms | tw width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 27.3 ms | tw width 3; max table 128; 16,176 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 39.5 ms | 3,525 nodes; max table 128; 10,732 join pairs; delegations tw=20, rw=0 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 149.2 ms | rw width 6; max table 512; max signatures 64; 132,074 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 668.7 ms | tw width 7; max table 2,048; 289,681 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 669.4 ms | tw width 7; max table 2,048; 293,297 join pairs |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 676.5 ms | tw width 8; max table 4,096; 293,423 join pairs |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 1.52 s | 1,753 nodes; max table 2,048; 266,015 join pairs; delegations tw=109, rw=0 |
| 129-256 | `branch --branch-heuristic split` | 112 / 112 | 7.87 s | 6,566 nodes; max table 262,144; 7,037,488 join pairs; delegations tw=115, rw=0; branch policy fallthroughs=1,888, tw skips=0, rw skips=115; max residual tw=14, cut-rank=18; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 9.41 s | tw width 14; max table 262,144; 7,044,136 join pairs |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 34 / 52 | 72.04 s | tw width 11; max table 32,768; 513,669 join pairs; 18 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Until `sop2X` exporters exist, each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Refreshed native comparison artifacts also report amplitude checks, mismatches, and maximum absolute error whenever both sides recorded amplitudes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill` | `aer-statevector` | 4 / 16 | 3.8 ms | 8.7 ms | 2.30x | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `mqt-ddsim-statevector` | 4 / 16 | 3.8 ms | 14.8 ms | 3.89x | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `pyzx-matrix` | 4 / 16 | 3.8 ms | 4.04 s | 1065.59x | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 4 / 16 | 3.8 ms | 3.0 ms | 0.78x | 30 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 35 / 92 | 136.4 ms | 600.8 ms | 4.41x | 100 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 35 / 92 | 136.4 ms | 33.05 s | 242.32x | 100 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 10 / 92 | 60.1 ms | 8.00 s | 133.01x | 100 | 10 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 35 / 92 | 136.4 ms | 3.29 s | 24.16x | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 14 / 68 | 637.0 ms | 292.5 ms | 0.46x | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 14 / 68 | 637.0 ms | 82.9 ms | 0.13x | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 3 / 68 | 19.0 ms | 8.43 s | 443.45x | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 14 / 68 | 637.0 ms | 91.1 ms | 0.14x | 80 | 16 | 10.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 2 / 2 | 9.0 ms | 4.3 ms | 0.48x | 3 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 2 / 2 | 9.0 ms | 8.4 ms | 0.93x | 3 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 2 / 2 | 9.0 ms | 10.8 ms | 1.20x | 3 | 10 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 2 / 2 | 9.0 ms | 1.8 ms | 0.20x | 3 | 16 | 10.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill` | `aer-statevector` | 16 / 16 | 12.7 ms | 45.2 ms | 3.56x | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `mqt-ddsim-statevector` | 16 / 16 | 12.7 ms | 90.6 ms | 7.14x | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `pyzx-matrix` | 16 / 16 | 12.7 ms | 189.2 ms | 14.91x | 7 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 16 / 16 | 12.7 ms | 21.5 ms | 1.69x | 7 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 36 / 36 | 184.5 ms | 127.1 ms | 0.69x | 14 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 36 / 36 | 184.5 ms | 246.1 ms | 1.33x | 14 | 16 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 35 / 36 | 171.9 ms | 34.62 s | 201.33x | 14 | 10 | 10.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 36 / 36 | 184.5 ms | 103.4 ms | 0.56x | 14 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 38 / 44 | 1.04 s | 315.3 ms | 0.30x | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 38 / 44 | 1.04 s | 246.7 ms | 0.24x | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 20 / 44 | 331.8 ms | 37.57 s | 113.21x | 19 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 38 / 44 | 1.04 s | 342.7 ms | 0.33x | 19 | 16 | 10.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-degree`; 33-64: `treewidth --treewidth-order min-fill`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 34 / 52 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger, and the refreshed benchmark summaries now expose branch skip reasons plus split/delegate trace counts. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
