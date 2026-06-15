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
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 52.0 ms | tw width 2; max table 64; 8,933 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 72.5 ms | rw width 3; max table 48; rw table forecast 0; rw join forecast 0; max signatures 8; 39,319 join pairs; rankwidth kernels map=884/770.9 us max items=32, join=884/1.2 ms max items=32, labelled-map=359/1.7 ms max items=64, labelled=359/2.0 ms max items=48 |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 87.5 ms | 5,233 nodes; cache hits=1,813, misses=3,420, hit rate=0.346; cache avoided nodes=3,858, rate=0.737; cache canonical hits=1,762; cache canonical lookups=4,023, stores=2,495; cache entries=131, canonical=81, canonical rate=0.618, slots=1,048; cache bytes key=13,498, counts=8,384, estimated=51,578; cache trace lookup=5,232 events/9.5 ms, store=3,419 events/7.1 ms; canonical cache trace lookup=4,023 events/9.0 ms, store=2,495 events/6.0 ms; max table 64; 240 join pairs; delegations tw=1, rw=0; branch dispatch splits=2,473/679.8 us max components=7, root tw delegates=1/152.0 us max vars=32; branch policy fallthroughs=1,260, tw skips=0, rw skips=1; branch fallthrough max vars=30; rw skip reasons treewidth-preferred=1; max residual tw=2, cut-rank=0; branch table forecast rw=0, tw=64; branch join forecast rw=0, tw=256; branch root tw probe width=2, 1 events/59.1 us; max residual vars=32, components=7, largest=32 |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 22.7 ms | tw width 3; max table 128; 11,788 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 25.0 ms | tw width 3; max table 128; 11,788 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 25.6 ms | tw width 3; max table 128; 11,772 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 30.9 ms | 1,891 nodes; cache hits=752, misses=1,139, hit rate=0.398; cache avoided nodes=1,262, rate=0.667; cache canonical hits=725; cache canonical lookups=1,460, stores=784; cache entries=107, canonical=75, canonical rate=0.701, slots=1,712; cache bytes key=8,600, counts=13,696, estimated=37,144; cache trace lookup=1,876 events/1.6 ms, store=1,124 events/1.3 ms; canonical cache trace lookup=1,460 events/1.4 ms, store=784 events/971.8 us; max table 128; 8,018 join pairs; delegations tw=20, rw=0; branch dispatch splits=820/232.1 us max components=9, tw delegates=5/1.0 ms max vars=53, root tw delegates=15/3.4 ms max vars=63; branch policy fallthroughs=399, tw skips=0, rw skips=20; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=20; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 15 events/2.4 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 145.8 ms | rw width 6; max table 512; rw table forecast 0; rw join forecast 0; max signatures 64; 133,010 join pairs; rankwidth kernels map=1,126/1.8 ms max items=128, join=1,126/11.0 ms max items=128, labelled-map=419/11.2 ms max items=128, labelled=419/15.2 ms max items=512 |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 472.0 ms | 1,265 nodes; cache hits=450, misses=815, hit rate=0.356; cache avoided nodes=478, rate=0.378; cache canonical hits=446; cache canonical lookups=962, stores=534; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=1,156 events/1.5 ms, store=706 events/1.4 ms; canonical cache trace lookup=962 events/1.4 ms, store=534 events/1.0 ms; max table 2,048; 246,870 join pairs; delegations tw=121, rw=0; branch dispatch splits=500/203.1 us max components=69, tw delegates=12/6.6 ms max vars=127, root tw delegates=109/162.4 ms max vars=128; branch policy fallthroughs=248, tw skips=0, rw skips=121; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=121; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 109 events/121.6 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 477.4 ms | tw width 8; max table 4,096; 252,717 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 530.8 ms | tw width 7; max table 2,048; 249,503 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 537.0 ms | tw width 7; max table 2,048; 252,189 join pairs |
| 129-256 | `branch --branch-heuristic split` | 112 / 112 | 7.39 s | 2,116 nodes; cache hits=785, misses=1,331, hit rate=0.371; cache avoided nodes=3,136, rate=1.482; cache canonical hits=737; cache canonical lookups=1,387, stores=678; cache entries=236, canonical=103, canonical rate=0.436, slots=1,888; cache bytes key=34,299, counts=15,104, estimated=79,099; cache trace lookup=2,032 events/4.0 ms, store=1,247 events/3.0 ms; canonical cache trace lookup=1,387 events/3.6 ms, store=678 events/1.8 ms; max table 262,144; 7,037,488 join pairs; delegations tw=115, rw=0; branch dispatch splits=1,002/536.2 us max components=14, tw delegates=31/567.7 ms max vars=211, root tw delegates=84/3.71 s max vars=249; branch policy fallthroughs=440, tw skips=0, rw skips=115; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=110, prefix-proxy=5; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 84 events/527.0 ms; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 8.17 s | tw width 14; max table 262,144; 7,044,136 join pairs |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 34 / 52 | 67.04 s | tw width 11; max table 32,768; 513,669 join pairs; 18 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Until `sop2X` exporters exist, each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude mismatch columns use completed rows where both sides recorded amplitudes. Rows with nonzero mismatches are correctness investigation targets, not accepted benchmark wins.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mismatches | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-degree` | `aer-statevector` | 4 / 16 | 2.9 ms | 8.0 ms | 2.76x | 2 | 0 | 4.45e-09 | 8.84e-09 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `mqt-ddsim-statevector` | 4 / 16 | 2.9 ms | 19.7 ms | 6.83x | 2 | 0 | 4.45e-09 | 8.84e-09 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `pyzx-matrix` | 4 / 16 | 2.9 ms | 3.94 s | 1363.30x | 2 | 0 | 4.45e-09 | 8.84e-09 | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `qiskit-statevector` | 4 / 16 | 2.9 ms | 3.1 ms | 1.07x | 2 | 0 | 4.45e-09 | 8.84e-09 | 30 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 35 / 92 | 95.2 ms | 4.44 s | 46.61x | 35 | 10 | 27.1 | 945 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 34 / 92 | 93.3 ms | 40.19 s | 430.95x | 34 | 12 | 27.9 | 945 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 11 / 92 | 46.8 ms | 22.67 s | 484.91x | 11 | 10 | 86.2 | 945 | 100 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 35 / 92 | 95.2 ms | 22.22 s | 233.36x | 35 | 10 | 27.1 | 945 | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 14 / 68 | 557.5 ms | 149.0 ms | 0.27x | 14 | 14 | 6.56e+13 | 7.35e+14 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 14 / 68 | 557.5 ms | 82.6 ms | 0.15x | 14 | 14 | 6.56e+13 | 7.35e+14 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 3 / 68 | 16.1 ms | 7.81 s | 483.49x | 3 | 3 | 1.85e+06 | 5.47e+06 | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 14 / 68 | 557.5 ms | 97.5 ms | 0.17x | 14 | 14 | 6.56e+13 | 7.35e+14 | 80 | 16 | 10.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mismatches | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 2 / 2 | 5.1 ms | 5.4 ms | 1.07x | 2 | 2 | 0.0144 | 0.0144 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 2 / 2 | 5.1 ms | 9.2 ms | 1.80x | 2 | 2 | 0.0144 | 0.0144 | 3 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 2 / 2 | 5.1 ms | 10.8 ms | 2.11x | 2 | 2 | 0.0144 | 0.0144 | 3 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 2 / 2 | 5.1 ms | 1.8 ms | 0.36x | 2 | 2 | 0.0144 | 0.0144 | 3 | 16 | 10.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mismatches | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-degree` | `aer-statevector` | 16 / 16 | 11.2 ms | 50.9 ms | 4.54x | 14 | 0 | 3.15e-09 | 1.94e-08 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `mqt-ddsim-statevector` | 16 / 16 | 11.2 ms | 106.3 ms | 9.49x | 14 | 0 | 3.15e-09 | 1.94e-08 | 7 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `pyzx-matrix` | 16 / 16 | 11.2 ms | 1.18 s | 105.15x | 14 | 0 | 3.15e-09 | 1.94e-08 | 7 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-degree` | `qiskit-statevector` | 16 / 16 | 11.2 ms | 21.6 ms | 1.92x | 14 | 0 | 3.15e-09 | 1.94e-08 | 7 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 36 / 36 | 178.9 ms | 151.1 ms | 0.84x | 35 | 33 | 35.9 | 945 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 36 / 36 | 178.9 ms | 266.0 ms | 1.49x | 35 | 33 | 35.9 | 945 | 14 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 35 / 36 | 153.9 ms | 46.10 s | 299.61x | 34 | 32 | 35.7 | 945 | 14 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 36 / 36 | 178.9 ms | 588.4 ms | 3.29x | 35 | 33 | 35.9 | 945 | 14 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 38 / 44 | 887.7 ms | 355.7 ms | 0.40x | 38 | 38 | 1.01e+19 | 3.85e+20 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 38 / 44 | 887.7 ms | 254.0 ms | 0.29x | 38 | 38 | 1.01e+19 | 3.85e+20 | 19 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 20 / 44 | 278.3 ms | 34.49 s | 123.95x | 20 | 20 | 1.93e+19 | 3.85e+20 | 19 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 38 / 44 | 887.7 ms | 353.1 ms | 0.40x | 38 | 38 | 1.01e+19 | 3.85e+20 | 19 | 16 | 10.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-degree`; 33-64: `treewidth --treewidth-order min-degree`; 65-128: `branch --branch-heuristic split`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 34 / 52 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
