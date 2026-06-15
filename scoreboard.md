# Scoreboard

Last updated: 2026-06-15.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

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

## 257-512 Sample Stratification

Rows with treewidth width at most 11 are the current low-width promotion candidates; timeouts remain the separate high-width residue.
| Bucket | Rows | Solved | Timeouts | Max width | Max table |
| --- | ---: | ---: | ---: | ---: | ---: |
| Solved, width <= 11 | 34 | 34 | 0 | 11 | 32768 |
| Timeouts | 18 | 0 | 18 | 0 | 0 |

## Internal Solver Configurations

Rows are grouped by imported-variable tier and sorted by total solve time. `Solved` is successful solver rows over attempted rows.

| Tier | Configuration | Solved | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 52.2 ms | tw width 2; max table 64; 8,933 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 72.9 ms | rw width 3; max table 48; rw table forecast 0; rw join forecast 0; max signatures 8; 39,319 join pairs; rankwidth kernels map=884/789.3 us max items=32, join=884/1.2 ms max items=32, labelled-map=359/1.7 ms max items=64, labelled=359/2.1 ms max items=48 |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 85.6 ms | 5,233 nodes; cache hits=1,813, misses=3,420, hit rate=0.346; cache avoided nodes=3,858, rate=0.737; cache canonical hits=1,762; cache canonical lookups=4,023, stores=2,495; cache entries=131, canonical=81, canonical rate=0.618, slots=1,048; cache bytes key=13,498, counts=8,384, estimated=51,578; cache trace lookup=5,232 events/9.4 ms, store=3,419 events/7.1 ms; canonical cache trace lookup=4,023 events/8.9 ms, store=2,495 events/6.1 ms; max table 64; 240 join pairs; delegations tw=1, rw=0; branch dispatch splits=2,473/666.1 us max components=7, root tw delegates=1/153.5 us max vars=32; branch policy fallthroughs=1,260, tw skips=0, rw skips=1; branch fallthrough max vars=30; rw skip reasons treewidth-preferred=1; max residual tw=2, cut-rank=0; branch table forecast rw=0, tw=64; branch join forecast rw=0, tw=256; branch root tw probe width=2, 1 events/57.5 us; max residual vars=32, components=7, largest=32 |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 23.3 ms | tw width 3; max table 128; 11,788 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 24.9 ms | tw width 3; max table 128; 11,772 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 25.5 ms | tw width 3; max table 128; 11,788 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 32.7 ms | 1,891 nodes; cache hits=752, misses=1,139, hit rate=0.398; cache avoided nodes=1,262, rate=0.667; cache canonical hits=725; cache canonical lookups=1,460, stores=784; cache entries=107, canonical=75, canonical rate=0.701, slots=1,712; cache bytes key=8,600, counts=13,696, estimated=37,144; cache trace lookup=1,876 events/1.7 ms, store=1,124 events/1.4 ms; canonical cache trace lookup=1,460 events/1.5 ms, store=784 events/1.0 ms; max table 128; 8,018 join pairs; delegations tw=20, rw=0; branch dispatch splits=820/253.6 us max components=9, tw delegates=5/1.0 ms max vars=53, root tw delegates=15/3.5 ms max vars=63; branch policy fallthroughs=399, tw skips=0, rw skips=20; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=20; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 15 events/2.5 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 147.5 ms | rw width 6; max table 512; rw table forecast 0; rw join forecast 0; max signatures 64; 133,010 join pairs; rankwidth kernels map=1,126/1.8 ms max items=128, join=1,126/10.7 ms max items=128, labelled-map=419/10.8 ms max items=128, labelled=419/14.5 ms max items=512 |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 475.3 ms | 1,265 nodes; cache hits=450, misses=815, hit rate=0.356; cache avoided nodes=478, rate=0.378; cache canonical hits=446; cache canonical lookups=962, stores=534; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=1,156 events/1.3 ms, store=706 events/1.2 ms; canonical cache trace lookup=962 events/1.3 ms, store=534 events/909.0 us; max table 2,048; 246,870 join pairs; delegations tw=121, rw=0; branch dispatch splits=500/185.5 us max components=69, tw delegates=12/6.6 ms max vars=127, root tw delegates=109/165.6 ms max vars=128; branch policy fallthroughs=248, tw skips=0, rw skips=121; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=121; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 109 events/124.2 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 479.4 ms | tw width 8; max table 4,096; 252,717 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 529.0 ms | tw width 7; max table 2,048; 249,503 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 537.2 ms | tw width 7; max table 2,048; 252,189 join pairs |
| 129-256 | `branch --branch-heuristic split` | 112 / 112 | 7.55 s | 2,116 nodes; cache hits=785, misses=1,331, hit rate=0.371; cache avoided nodes=3,136, rate=1.482; cache canonical hits=737; cache canonical lookups=1,387, stores=678; cache entries=236, canonical=103, canonical rate=0.436, slots=1,888; cache bytes key=34,299, counts=15,104, estimated=79,099; cache trace lookup=2,032 events/4.1 ms, store=1,247 events/3.1 ms; canonical cache trace lookup=1,387 events/3.7 ms, store=678 events/1.9 ms; max table 262,144; 7,037,488 join pairs; delegations tw=115, rw=0; branch dispatch splits=1,002/561.4 us max components=14, tw delegates=31/571.2 ms max vars=211, root tw delegates=84/3.82 s max vars=249; branch policy fallthroughs=440, tw skips=0, rw skips=115; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=110, prefix-proxy=5; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 84 events/533.1 ms; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 8.20 s | tw width 14; max table 262,144; 7,044,136 join pairs |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 34 / 52 | 67.17 s | tw width 11; max table 32,768; 513,669 join pairs; 18 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Until `sop2X` exporters exist, each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude error columns use completed rows where both sides recorded amplitudes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-degree` | `aer-statevector` | 4 / 16 | 3.0 ms | 7.4 ms | 2.45x | 4 | 6.86e-16 | 1.02e-15 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `mqt-ddsim-statevector` | 4 / 16 | 3.0 ms | 14.2 ms | 4.72x | 4 | 0 | 0 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `pyzx-matrix` | 4 / 16 | 3.0 ms | 3.77 s | 1252.48x | 4 | 2.38e-15 | 4.57e-15 | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `qiskit-statevector` | 4 / 16 | 3.0 ms | 3.0 ms | 0.99x | 4 | 5.68e-16 | 1.22e-15 | 30 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 35 / 92 | 99.1 ms | 459.4 ms | 4.64x | 35 | 2.02e-16 | 2.21e-15 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 35 / 92 | 99.1 ms | 13.68 s | 138.07x | 35 | 1.87e-17 | 5.74e-17 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 11 / 92 | 47.8 ms | 17.03 s | 356.13x | 11 | 3.08e-15 | 9.87e-15 | 100 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 35 / 92 | 99.1 ms | 2.69 s | 27.11x | 35 | 2.18e-16 | 2.22e-15 | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 14 / 68 | 587.4 ms | 137.9 ms | 0.23x | 14 | 1.87e-15 | 3.79e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 14 / 68 | 587.4 ms | 44.4 ms | 0.08x | 14 | 8.4e-17 | 1.18e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 3 / 68 | 16.4 ms | 7.74 s | 473.39x | 3 | 1.15e-14 | 1.58e-14 | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 14 / 68 | 587.4 ms | 76.2 ms | 0.13x | 14 | 1.26e-15 | 3.66e-15 | 80 | 16 | 10.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 2 / 2 | 5.3 ms | 4.5 ms | 0.85x | 2 | 4.33e-17 | 4.33e-17 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 2 / 2 | 5.3 ms | 6.9 ms | 1.31x | 2 | 0 | 0 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 2 / 2 | 5.3 ms | 10.5 ms | 1.99x | 2 | 1.3e-16 | 1.3e-16 | 3 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 2 / 2 | 5.3 ms | 1.8 ms | 0.34x | 2 | 0 | 0 | 3 | 16 | 10.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-degree` | `aer-statevector` | 16 / 16 | 11.3 ms | 45.8 ms | 4.04x | 16 | 6.08e-16 | 1.02e-15 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `mqt-ddsim-statevector` | 16 / 16 | 11.3 ms | 79.9 ms | 7.06x | 16 | 0 | 0 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `pyzx-matrix` | 16 / 16 | 11.3 ms | 152.1 ms | 13.42x | 16 | 3.54e-15 | 6.82e-15 | 7 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `qiskit-statevector` | 16 / 16 | 11.3 ms | 21.1 ms | 1.86x | 16 | 9.28e-16 | 1.78e-15 | 7 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 36 / 36 | 174.4 ms | 123.9 ms | 0.71x | 36 | 7.56e-16 | 2.21e-15 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 36 / 36 | 174.4 ms | 168.2 ms | 0.96x | 36 | 1.18e-16 | 1.18e-15 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 35 / 36 | 147.9 ms | 30.62 s | 207.02x | 35 | 7.5e-15 | 1.58e-14 | 14 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 36 / 36 | 174.4 ms | 106.1 ms | 0.61x | 36 | 1.54e-15 | 3.44e-15 | 14 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 38 / 44 | 898.9 ms | 318.0 ms | 0.35x | 38 | 1.75e-15 | 4e-15 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 38 / 44 | 898.9 ms | 185.6 ms | 0.21x | 38 | 7.95e-17 | 1.18e-15 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 20 / 44 | 281.0 ms | 34.24 s | 121.86x | 20 | 2.08e-14 | 3.25e-14 | 19 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 38 / 44 | 898.9 ms | 296.4 ms | 0.33x | 38 | 3.22e-15 | 6e-15 | 19 | 16 | 10.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-degree`; 33-64: `treewidth --treewidth-order min-degree`; 65-128: `branch --branch-heuristic split`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 34 / 52 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
