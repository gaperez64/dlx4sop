# Scoreboard

Last updated: 2026-06-17.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks Used

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | tests/qasm_solver_corpus.json | 64 | 64 | 0 | 0 | 0 | 0 | sign 64 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 520 | 104 | 34 | 184 | 136 | 62 / 100 | labelled 182; sign 338 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 60 | 56 | 0 | 2 | 0 | 2 | labelled 4; sign 56 |
| PyZX | https://github.com/zxcalc/pyzx | 478 | 46 | 64 | 98 / 138 | 172 | 98 / 100 | labelled 182; sign 296 |

Total current solved coverage: 1122 fixed-boundary benchmark rows.
The 257-512 exploratory sample contributes 162 solved rows out of 202 attempted under the current timeout cap.

## 257-512 Sample Stratification

Rows with treewidth width at most 19 are the current low-width promotion candidates; timeouts remain the separate high-width residue.
| Bucket | Rows | Solved | Timeouts | Max width | Max table |
| --- | ---: | ---: | ---: | ---: | ---: |
| Solved, width <= 19 | 162 | 162 | 0 | 19 | 8388608 |
| Timeouts | 40 | 0 | 40 | 0 | 0 |

## Internal Solver Configurations

Rows are grouped by imported-variable tier and sorted by total solve time. `Solved` is successful solver rows over attempted rows.

| Tier | Configuration | Solved | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 32 / 32 | 13.7 ms | rw width 2; rw labelled-cut-signature=2, support=2; max table 16; rw table forecast 32; rw join forecast 40; rw cut estimates exact=0, proxy=0, assignments=0; max signatures 4; 1,046 join pairs; rankwidth kernels map=107/83.4 us max items=8, join=107/88.8 us max items=16 |
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 238 / 238 | 106.9 ms | tw width 2; max table 64; 17,128 join pairs |
| 0-32 | `branch --branch-heuristic split` | 238 / 238 | 177.8 ms | 10,191 nodes; cache hits=3,418, misses=6,773, hit rate=0.335; cache avoided nodes=6,934, rate=0.680; cache canonical hits=3,326; cache canonical lookups=7,835, stores=4,905; cache entries=140, canonical=83, canonical rate=0.593, slots=1,120; cache bytes key=14,157, counts=8,960, estimated=52,813; cache trace lookup=10,189 events/17.5 ms, store=6,771 events/13.0 ms; canonical cache trace lookup=7,835 events/16.4 ms, store=4,905 events/11.1 ms; max table 64; 480 join pairs; delegations tw=2, rw=0; branch dispatch splits=4,822/1.3 ms max components=7, root tw delegates=2/278.4 us max vars=32; branch policy fallthroughs=2,472, tw skips=0, rw skips=2; branch fallthrough max vars=30; rw skip reasons treewidth-preferred=2; max residual tw=2, cut-rank=0; branch table forecast rw=0, tw=64; branch join forecast rw=0, tw=256; branch root tw probe width=2, 2 events/109.2 us; max residual vars=32, components=7, largest=32 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 66 / 66 | 56.2 ms | tw width 3; max table 128; 25,484 join pairs |
| 33-64 | `branch --branch-heuristic split` | 66 / 66 | 66.7 ms | 3,834 nodes; cache hits=1,556, misses=2,278, hit rate=0.406; cache avoided nodes=2,478, rate=0.646; cache canonical hits=1,514; cache canonical lookups=2,982, stores=1,554; cache entries=109, canonical=75, canonical rate=0.688, slots=1,744; cache bytes key=9,069, counts=13,952, estimated=37,667; cache trace lookup=3,802 events/3.4 ms, store=2,246 events/2.8 ms; canonical cache trace lookup=2,982 events/3.0 ms, store=1,554 events/2.0 ms; max table 128; 17,976 join pairs; delegations tw=42, rw=0; branch dispatch splits=1,665/477.6 us max components=9, tw delegates=10/2.1 ms max vars=53, root tw delegates=32/7.9 ms max vars=63; branch policy fallthroughs=802, tw skips=0, rw skips=42; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=42; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 32 events/5.1 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 62 / 64 | 122.28 s | rw width 4; rw labelled-cut-signature=4, support=4; max table 128; rw table forecast 128; rw join forecast 530; rw cut estimates exact=1,436, proxy=0, assignments=18,014,398,509,482,088; max signatures 16; 209,614 join pairs; rankwidth kernels map=2,252/5.0 ms max items=128, join=2,252/21.2 ms max items=128, labelled-map=718/3.7 ms max items=16, labelled=718/2.7 ms max items=64; 2 timeouts |
| 65-128 | `branch --branch-heuristic split` | 254 / 254 | 912.0 ms | 2,532 nodes; cache hits=926, misses=1,606, hit rate=0.366; cache avoided nodes=940, rate=0.371; cache canonical hits=918; cache canonical lookups=1,936, stores=1,050; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=2,320 events/2.6 ms, store=1,394 events/2.6 ms; canonical cache trace lookup=1,936 events/2.5 ms, store=1,050 events/1.7 ms; max table 2,048; 479,788 join pairs; delegations tw=236, rw=0; branch dispatch splits=1,004/360.9 us max components=69, tw delegates=24/12.8 ms max vars=127, root tw delegates=212/311.6 ms max vars=128; branch policy fallthroughs=484, tw skips=0, rw skips=236; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=236; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 212 events/226.8 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 254 / 254 | 1.02 s | tw width 7; max table 2,048; 485,054 join pairs |
| 65-128 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 52 / 252 | 217.09 s | rw width 6; rw labelled-cut-signature=6, support=6; max table 512; rw table forecast 512; rw join forecast 4,204; rw cut estimates exact=1,072, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 64; 636,857 join pairs; rankwidth kernels map=3,788/45.4 ms max items=1,024, join=3,788/149.6 ms max items=512, labelled-map=536/4.5 ms max items=16, labelled=536/3.2 ms max items=64; 200 timeouts |
| 129-256 | `branch --branch-heuristic split` | 220 / 220 | 14.33 s | 4,174 nodes; cache hits=1,472, misses=2,702, hit rate=0.353; cache avoided nodes=5,880, rate=1.409; cache canonical hits=1,368; cache canonical lookups=2,730, stores=1,424; cache entries=242, canonical=115, canonical rate=0.475, slots=1,936; cache bytes key=34,302, counts=15,488, estimated=79,486; cache trace lookup=4,010 events/8.2 ms, store=2,538 events/5.9 ms; canonical cache trace lookup=2,730 events/7.4 ms, store=1,424 events/3.5 ms; max table 262,144; 14,052,608 join pairs; delegations tw=226, rw=0; branch dispatch splits=1,977/1.1 ms max components=14, tw delegates=62/1.08 s max vars=211, root tw delegates=164/7.24 s max vars=249; branch policy fallthroughs=879, tw skips=0, rw skips=226; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=216, prefix-proxy=10; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 164 events/989.9 ms; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 222 / 222 | 16.46 s | tw width 14; max table 262,144; 14,077,088 join pairs |
| 129-256 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 0 / 220 | 220.31 s | 220 timeouts |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 162 / 202 | 1723.14 s | tw width 19; max table 8,388,608; 275,157,818 join pairs; 40 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Until `sop2X` exporters exist, each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude error columns use completed rows where both sides recorded amplitudes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 10 / 34 | 10.4 ms | 37.9 ms | 3.65x | 10 | 0.109 | 1 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 10 / 34 | 10.4 ms | 49.6 ms | 4.77x | 10 | 0 | 0 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 10 / 34 | 10.4 ms | 7.98 s | 768.24x | 10 | 1.37e-15 | 4.57e-15 | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 10 / 34 | 10.4 ms | 12.7 ms | 1.22x | 10 | 3.47e-16 | 1.22e-15 | 30 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 70 / 184 | 193.8 ms | 651.1 ms | 3.36x | 70 | 0.0185 | 1 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 70 / 184 | 193.8 ms | 20.15 s | 103.97x | 70 | 2.16e-17 | 1.07e-16 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 22 / 184 | 93.7 ms | 30.62 s | 326.87x | 22 | 2.21e-15 | 9.87e-15 | 100 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 70 / 184 | 193.8 ms | 3.65 s | 18.86x | 70 | 1.59e-16 | 2.22e-15 | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 28 / 134 | 1.10 s | 255.6 ms | 0.23x | 28 | 0.0126 | 0.353 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 28 / 134 | 1.10 s | 109.7 ms | 0.10x | 28 | 5.47e-17 | 1.18e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 6 / 134 | 32.1 ms | 16.30 s | 507.17x | 6 | 6.41e-15 | 1.58e-14 | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 28 / 134 | 1.10 s | 145.7 ms | 0.13x | 28 | 7.16e-16 | 3.66e-15 | 80 | 16 | 10.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 2 / 2 | 5.2 ms | 4.3 ms | 0.84x | 2 | 0.354 | 0.707 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 2 / 2 | 5.2 ms | 5.5 ms | 1.05x | 2 | 0 | 0 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 2 / 2 | 5.2 ms | 10.8 ms | 2.08x | 2 | 1.08e-16 | 1.3e-16 | 3 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 2 / 2 | 5.2 ms | 1.7 ms | 0.32x | 2 | 0 | 0 | 3 | 16 | 10.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 32 / 32 | 24.9 ms | 53.8 ms | 2.16x | 32 | 0.0938 | 1 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 32 / 32 | 24.9 ms | 94.8 ms | 3.80x | 32 | 0 | 0 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 32 / 32 | 24.9 ms | 343.9 ms | 13.79x | 32 | 2.16e-15 | 6.82e-15 | 7 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 32 / 32 | 24.9 ms | 34.3 ms | 1.37x | 32 | 5.96e-16 | 1.89e-15 | 7 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 68 / 68 | 335.2 ms | 199.7 ms | 0.60x | 68 | 0.124 | 1 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 68 / 68 | 335.2 ms | 214.9 ms | 0.64x | 68 | 8.13e-17 | 1.18e-15 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 66 / 68 | 283.7 ms | 50.52 s | 178.11x | 66 | 5.16e-15 | 1.58e-14 | 14 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 68 / 68 | 335.2 ms | 180.7 ms | 0.54x | 68 | 1.16e-15 | 3.44e-15 | 14 | 16 | 10.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `rankwidth --rankwidth-generate best --rankwidth-mode count-table`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `branch --branch-heuristic split`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 162 / 202 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
