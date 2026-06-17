# Scoreboard

Last updated: 2026-06-17.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks Used

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 | sign 32 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 410 | 0 | 32 | 182 | 134 | 62 / 100 | labelled 154; sign 256 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 2 | 0 | 0 | 0 | 0 | 2 | labelled 2 |
| PyZX | https://github.com/zxcalc/pyzx | 286 | 0 | 32 | 70 | 86 | 98 / 100 | labelled 124; sign 162 |

Total current solved coverage: 730 fixed-boundary benchmark rows.
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
| 0-32 | `treewidth --treewidth-order min-fill` | 32 / 32 | 11.7 ms | tw width 1; max table 64; 486 join pairs |
| 0-32 | `branch --branch-heuristic split` | 32 / 32 | 12.5 ms | 193 nodes; cache hits=22, misses=171, hit rate=0.114; cache avoided nodes=26, rate=0.135; cache canonical hits=22; cache canonical lookups=124, stores=157; cache entries=15, canonical=14, canonical rate=0.933, slots=240; cache bytes key=624, counts=1,920, estimated=6,599; cache trace lookup=193 events/151.4 us, store=171 events/345.0 us; canonical cache trace lookup=124 events/144.8 us, store=157 events/334.5 us; 0 join pairs; delegations tw=0, rw=0; branch dispatch splits=79/21.4 us max components=3; branch policy fallthroughs=48, tw skips=0, rw skips=0; branch fallthrough max vars=7; max residual tw=0, cut-rank=0; max residual vars=10, components=3, largest=7 |
| 0-32 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 32 / 32 | 12.9 ms | rw width 2; rw labelled-cut-signature=2, support=2; max table 16; rw table forecast 32; rw join forecast 40; rw cut estimates exact=0, proxy=0, assignments=0; max signatures 4; 1,046 join pairs; rankwidth kernels map=107/87.4 us max items=8, join=107/89.1 us max items=16 |
| 0-32 | `sop2wmc --encoding amplitude + ganak --mode 6` | 238 / 238 | 2.98 s | ganak 2.88 s + export 104.3 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 238 / 238 | 483.66 s | ganak 482.48 s + export 1.17 s; 0 amplitude mismatches |
| 33-64 | `treewidth --treewidth-order min-fill` | 64 / 64 | 50.6 ms | tw width 3; max table 128; 23,576 join pairs |
| 33-64 | `branch --branch-heuristic split` | 64 / 64 | 67.8 ms | 3,832 nodes; cache hits=1,556, misses=2,276, hit rate=0.406; cache avoided nodes=2,478, rate=0.647; cache canonical hits=1,514; cache canonical lookups=2,982, stores=1,554; cache entries=109, canonical=75, canonical rate=0.688, slots=1,744; cache bytes key=9,069, counts=13,952, estimated=37,667; cache trace lookup=3,802 events/3.7 ms, store=2,246 events/2.9 ms; canonical cache trace lookup=2,982 events/3.2 ms, store=1,554 events/2.1 ms; max table 128; 16,036 join pairs; delegations tw=40, rw=0; branch dispatch splits=1,665/521.7 us max components=9, tw delegates=10/2.2 ms max vars=53, root tw delegates=30/6.7 ms max vars=63; branch policy fallthroughs=802, tw skips=0, rw skips=40; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=40; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 30 events/4.9 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 66 / 66 | 3.39 s | ganak 3.36 s + export 36.4 ms; 0 amplitude mismatches |
| 33-64 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 62 / 64 | 121.87 s | rw width 4; rw labelled-cut-signature=4, support=4; max table 128; rw table forecast 128; rw join forecast 530; rw cut estimates exact=1,436, proxy=0, assignments=18,014,398,509,482,088; max signatures 16; 209,614 join pairs; rankwidth kernels map=2,252/4.1 ms max items=128, join=2,252/17.0 ms max items=128, labelled-map=718/3.1 ms max items=16, labelled=718/2.2 ms max items=64; 2 timeouts |
| 65-128 | `branch --branch-heuristic split` | 252 / 252 | 1.02 s | 2,530 nodes; cache hits=926, misses=1,604, hit rate=0.366; cache avoided nodes=940, rate=0.372; cache canonical hits=918; cache canonical lookups=1,936, stores=1,050; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=2,320 events/3.0 ms, store=1,394 events/2.7 ms; canonical cache trace lookup=1,936 events/2.8 ms, store=1,050 events/2.0 ms; max table 2,048; 475,716 join pairs; delegations tw=234, rw=0; branch dispatch splits=1,004/399.0 us max components=69, tw delegates=24/14.5 ms max vars=127, root tw delegates=210/355.1 ms max vars=128; branch policy fallthroughs=484, tw skips=0, rw skips=234; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=234; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 210 events/255.4 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-fill` | 252 / 252 | 1.12 s | tw width 7; max table 2,048; 486,762 join pairs |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 254 / 254 | 37.28 s | ganak 37.10 s + export 178.1 ms; 0 amplitude mismatches |
| 65-128 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 52 / 252 | 216.97 s | rw width 6; rw labelled-cut-signature=6, support=6; max table 512; rw table forecast 512; rw join forecast 4,204; rw cut estimates exact=1,072, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 64; 636,857 join pairs; rankwidth kernels map=3,788/44.2 ms max items=1,024, join=3,788/144.8 ms max items=512, labelled-map=536/4.6 ms max items=16, labelled=536/3.1 ms max items=64; 200 timeouts |
| 129-256 | `branch --branch-heuristic split` | 220 / 220 | 15.38 s | 4,174 nodes; cache hits=1,472, misses=2,702, hit rate=0.353; cache avoided nodes=5,880, rate=1.409; cache canonical hits=1,368; cache canonical lookups=2,730, stores=1,424; cache entries=242, canonical=115, canonical rate=0.475, slots=1,936; cache bytes key=34,302, counts=15,488, estimated=79,486; cache trace lookup=4,010 events/9.1 ms, store=2,538 events/6.6 ms; canonical cache trace lookup=2,730 events/8.2 ms, store=1,424 events/3.9 ms; max table 262,144; 14,052,608 join pairs; delegations tw=226, rw=0; branch dispatch splits=1,977/1.2 ms max components=14, tw delegates=62/1.15 s max vars=211, root tw delegates=164/7.74 s max vars=249; branch policy fallthroughs=879, tw skips=0, rw skips=226; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=226; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 164 events/1.04 s; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill` | 220 / 220 | 16.52 s | tw width 14; max table 262,144; 13,129,376 join pairs |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 222 / 222 | 107.72 s | ganak 107.50 s + export 216.6 ms; 0 amplitude mismatches |
| 129-256 | `rankwidth --rankwidth-generate best --rankwidth-mode count-table` | 0 / 220 | 220.29 s | 220 timeouts |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 162 / 202 | 1723.14 s | tw width 19; max table 8,388,608; 275,157,818 join pairs; 40 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude error columns use completed rows where both sides recorded amplitudes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill` | `aer-statevector` | 8 / 32 | 6.9 ms | 31.4 ms | 4.56x | 8 | 0.136 | 1 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `mqt-ddsim-statevector` | 8 / 32 | 6.9 ms | 42.5 ms | 6.18x | 8 | 0 | 0 | 30 | 16 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `pyzx-matrix` | 8 / 32 | 6.9 ms | 7.97 s | 1158.64x | 8 | 1.69e-15 | 4.57e-15 | 30 | 10 | 10.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill` | `qiskit-statevector` | 8 / 32 | 6.9 ms | 8.5 ms | 1.23x | 8 | 4.15e-16 | 1.22e-15 | 30 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 68 / 182 | 212.3 ms | 643.9 ms | 3.03x | 68 | 0.019 | 1 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 68 / 182 | 212.3 ms | 20.14 s | 94.88x | 68 | 2.23e-17 | 1.07e-16 | 100 | 16 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 20 / 182 | 103.0 ms | 18.05 s | 175.23x | 20 | 2.43e-15 | 9.87e-15 | 100 | 10 | 10.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 68 / 182 | 212.3 ms | 3.65 s | 17.20x | 68 | 1.63e-16 | 2.22e-15 | 100 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 28 / 134 | 1.22 s | 255.6 ms | 0.21x | 28 | 0.0126 | 0.353 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 28 / 134 | 1.22 s | 109.7 ms | 0.09x | 28 | 5.47e-17 | 1.18e-15 | 80 | 16 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 6 / 134 | 33.9 ms | 16.30 s | 481.36x | 6 | 6.41e-15 | 1.58e-14 | 80 | 10 | 10.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 28 / 134 | 1.22 s | 145.7 ms | 0.12x | 28 | 7.16e-16 | 3.66e-15 | 80 | 16 | 10.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill`; 33-64: `treewidth --treewidth-order min-fill`; 65-128: `branch --branch-heuristic split`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 162 / 202 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
