# Scoreboard

Last updated: 2026-06-15.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks Used

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 | sign 32 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 244 | 52 | 16 | 92 | 68 | 16 / 25 | labelled 78; sign 166 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 24 | 20 | 0 | 2 | 0 | 2 | labelled 4; sign 20 |
| PyZX | https://github.com/zxcalc/pyzx | 147 | 29 | 16 | 36 | 44 | 22 / 25 | labelled 49; sign 98 |

Total current solved coverage: 447 fixed-boundary benchmark rows.
The 257-512 exploratory sample contributes 40 solved rows out of 52 attempted under the current timeout cap.

## Internal Solver Configurations

Rows are grouped by imported-variable tier and sorted by total solve time. `Solved` is successful solver rows over attempted rows.

| Tier | Configuration | Solved | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-fill` | 133 / 133 | 58.9 ms | tw width 2; max table 64; 8,933 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 133 / 133 | 86.0 ms | rw width 3; rw labelled-cut-signature=3, support=3; max table 48; rw table forecast 64; rw join forecast 324; rw cut estimates exact=718, proxy=0, assignments=1,073,741,880; max signatures 8; 42,519 join pairs; rankwidth kernels map=884/743.7 us max items=32, join=884/1.2 ms max items=32, labelled-map=359/1.6 ms max items=64, labelled=359/2.5 ms max items=48 |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 92.1 ms | 5,233 nodes; cache hits=1,813, misses=3,420, hit rate=0.346; cache avoided nodes=3,858, rate=0.737; cache canonical hits=1,762; cache canonical lookups=4,023, stores=2,495; cache entries=131, canonical=81, canonical rate=0.618, slots=1,048; cache bytes key=13,498, counts=8,384, estimated=51,578; cache trace lookup=5,232 events/9.5 ms, store=3,419 events/7.0 ms; canonical cache trace lookup=4,023 events/8.9 ms, store=2,495 events/6.1 ms; max table 64; 240 join pairs; delegations tw=1, rw=0; branch dispatch splits=2,473/694.2 us max components=7, root tw delegates=1/145.4 us max vars=32; branch policy fallthroughs=1,260, tw skips=0, rw skips=1; branch fallthrough max vars=30; rw skip reasons treewidth-preferred=1; max residual tw=2, cut-rank=0; branch table forecast rw=0, tw=64; branch join forecast rw=0, tw=256; branch root tw probe width=2, 1 events/56.1 us; max residual vars=32, components=7, largest=32 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 26.3 ms | tw width 3; max table 128; 11,772 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 32.6 ms | 1,891 nodes; cache hits=752, misses=1,139, hit rate=0.398; cache avoided nodes=1,262, rate=0.667; cache canonical hits=725; cache canonical lookups=1,460, stores=784; cache entries=107, canonical=75, canonical rate=0.701, slots=1,712; cache bytes key=8,600, counts=13,696, estimated=37,144; cache trace lookup=1,876 events/1.7 ms, store=1,124 events/1.4 ms; canonical cache trace lookup=1,460 events/1.5 ms, store=784 events/1.0 ms; max table 128; 8,018 join pairs; delegations tw=20, rw=0; branch dispatch splits=820/245.9 us max components=9, tw delegates=5/1.1 ms max vars=53, root tw delegates=15/3.5 ms max vars=63; branch policy fallthroughs=399, tw skips=0, rw skips=20; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=20; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 15 events/2.5 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 212.5 ms | rw width 6; rw labelled-cut-signature=6, support=6; max table 512; rw table forecast 512; rw join forecast 4,506; rw cut estimates exact=838, proxy=0, assignments=2,305,843,009,213,694,070; max signatures 64; 133,010 join pairs; rankwidth kernels map=1,126/1.8 ms max items=128, join=1,126/10.7 ms max items=128, labelled-map=419/10.9 ms max items=128, labelled=419/14.7 ms max items=512 |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 513.4 ms | 1,265 nodes; cache hits=450, misses=815, hit rate=0.356; cache avoided nodes=478, rate=0.378; cache canonical hits=446; cache canonical lookups=962, stores=534; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=1,156 events/1.5 ms, store=706 events/1.4 ms; canonical cache trace lookup=962 events/1.4 ms, store=534 events/997.7 us; max table 2,048; 246,870 join pairs; delegations tw=121, rw=0; branch dispatch splits=500/202.6 us max components=69, tw delegates=12/7.0 ms max vars=127, root tw delegates=109/184.4 ms max vars=128; branch policy fallthroughs=248, tw skips=0, rw skips=121; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=121; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 109 events/133.9 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 579.9 ms | tw width 7; max table 2,048; 249,503 join pairs |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 59 / 130 | 753.80 s | rw width 10; rw labelled-cut-signature=10, support=10; max table 8,192; rw table forecast 16,384; rw join forecast 37,684; rw cut estimates exact=5,448, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 1,024; 2,546,453 join pairs; rankwidth kernels map=2,826/466.5 ms max items=2,048, join=2,826/2.55 s max items=8,192, labelled-map=2,724/2.00 s max items=1,024, labelled=2,724/3.11 s max items=4,096; 71 timeouts |
| 129-256 | `branch --branch-heuristic split` | 112 / 112 | 7.99 s | 2,116 nodes; cache hits=785, misses=1,331, hit rate=0.371; cache avoided nodes=3,136, rate=1.482; cache canonical hits=737; cache canonical lookups=1,387, stores=678; cache entries=236, canonical=103, canonical rate=0.436, slots=1,888; cache bytes key=34,299, counts=15,104, estimated=79,099; cache trace lookup=2,032 events/4.5 ms, store=1,247 events/3.5 ms; canonical cache trace lookup=1,387 events/4.1 ms, store=678 events/2.0 ms; max table 262,144; 7,037,488 join pairs; delegations tw=115, rw=0; branch dispatch splits=1,002/631.3 us max components=14, tw delegates=31/582.4 ms max vars=211, root tw delegates=84/4.04 s max vars=249; branch policy fallthroughs=440, tw skips=0, rw skips=115; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=110, prefix-proxy=5; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 84 events/589.5 ms; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 8.76 s | tw width 14; max table 262,144; 7,044,136 join pairs |
| 129-256 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 49 / 112 | 748.69 s | rw width 9; rw labelled-cut-signature=9, support=9; max table 4,096; rw table forecast 4,096; rw join forecast 44,880; rw cut estimates exact=8,590, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 512; 6,394,034 join pairs; rankwidth kernels map=4,592/2.72 s max items=1,024, join=4,592/5.27 s max items=4,096, labelled-map=4,295/4.44 s max items=1,024, labelled=4,295/5.35 s max items=4,096; 63 timeouts |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 40 / 52 | 176.08 s | tw width 18; max table 4,194,304; 21,350,491 join pairs; 12 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Until `sop2X` exporters exist, each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude errors are reported for rows where both QSOP and the native tool completed.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 4 / 16 | 3.4 ms | 7.6 ms | 2.26x | 6.86e-16 | 1.02e-15 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 4 / 16 | 3.4 ms | 12.1 ms | 3.62x | 0 | 0 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 4 / 16 | 3.4 ms | 3.91 s | 1164.77x | 2.38e-15 | 4.57e-15 | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 4 / 16 | 3.4 ms | 3.3 ms | 0.98x | 5.68e-16 | 1.22e-15 | 30 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 35 / 92 | 107.4 ms | 3.98 s | 37.09x | 2.02e-16 | 2.21e-15 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 35 / 92 | 107.4 ms | 12.45 s | 115.86x | 1.95e-17 | 5.74e-17 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 10 / 92 | 47.2 ms | 13.22 s | 279.82x | 3.36e-15 | 9.87e-15 | 100 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 35 / 92 | 107.4 ms | 16.92 s | 157.56x | 2.18e-16 | 2.22e-15 | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 14 / 68 | 644.6 ms | 298.7 ms | 0.46x | 1.87e-15 | 3.79e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 14 / 68 | 644.6 ms | 53.8 ms | 0.08x | 8.4e-17 | 1.18e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 3 / 68 | 19.4 ms | 7.81 s | 403.40x | 1.15e-14 | 1.58e-14 | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 14 / 68 | 644.6 ms | 1.13 s | 1.75x | 1.26e-15 | 3.66e-15 | 80 | 16 | 10.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 2 / 2 | 5.9 ms | 4.9 ms | 0.84x | 4.33e-17 | 4.33e-17 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 2 / 2 | 5.9 ms | 7.3 ms | 1.24x | 0 | 0 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 2 / 2 | 5.9 ms | 11.6 ms | 1.97x | 1.3e-16 | 1.3e-16 | 3 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 2 / 2 | 5.9 ms | 1.6 ms | 0.28x | 0 | 0 | 3 | 16 | 10.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 16 / 16 | 13.8 ms | 56.3 ms | 4.07x | 6.08e-16 | 1.02e-15 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 16 / 16 | 13.8 ms | 73.8 ms | 5.34x | 0 | 0 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 16 / 16 | 13.8 ms | 1.06 s | 76.38x | 3.54e-15 | 6.82e-15 | 7 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 16 / 16 | 13.8 ms | 24.7 ms | 1.78x | 9.28e-16 | 1.78e-15 | 7 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 36 / 36 | 186.0 ms | 130.6 ms | 0.70x | 7.56e-16 | 2.21e-15 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 36 / 36 | 186.0 ms | 189.7 ms | 1.02x | 1.18e-16 | 1.18e-15 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 35 / 36 | 160.2 ms | 45.22 s | 282.33x | 7.5e-15 | 1.58e-14 | 14 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 36 / 36 | 186.0 ms | 527.8 ms | 2.84x | 1.54e-15 | 3.44e-15 | 14 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 38 / 44 | 973.8 ms | 1.54 s | 1.58x | 1.75e-15 | 4e-15 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 38 / 44 | 973.8 ms | 181.5 ms | 0.19x | 7.95e-17 | 1.18e-15 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 20 / 44 | 303.4 ms | 36.05 s | 118.84x | 2.08e-14 | 3.25e-14 | 19 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 38 / 44 | 973.8 ms | 5.03 s | 5.16x | 3.22e-15 | 6e-15 | 19 | 16 | 10.0 | 4096 |

## Current Takeaway

Current best internal choices are treewidth through 64 imported variables, hybrid branch from 65 to 256, and treewidth for the solved part of the exploratory 257-512 sample. Native tools remain useful comparison points, especially on smaller low-qubit cases, while QSOP is strongest on many fixed-boundary rows with larger imported variable counts.
