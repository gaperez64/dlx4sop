# Scoreboard

Last updated: 2026-06-18.

This tracks progress toward a competitive exact strong simulator based on labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong simulation: import a static circuit into QSOP, solve the exact residue-count histogram, and compare with native simulators where possible.

## Benchmarks Used

Counts are fixed-boundary QSOP rows currently used in solver comparisons. The 257-512 column is an exploratory stratified sample and is shown as solved / attempted when timeouts remain.

| Source | Upstream | Total solved | 0-32 | 33-64 | 65-128 | 129-256 | 257-512 sample | QSOP modes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Internal corpus | tests/qasm_solver_corpus.json | 32 | 32 | 0 | 0 | 0 | 0 | sign 32 |
| FeynmanDD | https://github.com/cqs-thu/feynman-decision-diagram | 520 | 104 | 34 | 184 | 136 | 62 / 100 | labelled 182; sign 338 |
| MQT Bench | https://github.com/munich-quantum-toolkit/bench | 60 | 56 | 0 | 2 | 0 | 2 | labelled 4; sign 56 |
| PyZX | https://github.com/zxcalc/pyzx | 330 | 46 | 32 | 68 | 86 | 98 / 100 | labelled 126; sign 204 |

Total current solved coverage: 942 fixed-boundary benchmark rows.
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
| 0-32 | `treewidth --treewidth-order min-fill-max-degree` | 238 / 238 | 131.3 ms | tw width 2; max table 64; 17,128 join pairs |
| 0-32 | `branch --branch-heuristic split` | 238 / 238 | 209.0 ms | 10,191 nodes; cache hits=3,418, misses=6,773, hit rate=0.335; cache avoided nodes=6,934, rate=0.680; cache canonical hits=3,326; cache canonical lookups=7,835, stores=4,905; cache entries=140, canonical=83, canonical rate=0.593, slots=1,120; cache bytes key=14,157, counts=8,960, estimated=52,813; cache trace lookup=10,189 events/22.7 ms, store=6,771 events/17.1 ms; canonical cache trace lookup=7,835 events/21.3 ms, store=4,905 events/14.7 ms; max table 64; 480 join pairs; delegations tw=2, rw=0; branch dispatch splits=4,822/1.7 ms max components=7, root tw delegates=2/361.9 us max vars=32; branch policy fallthroughs=2,472, tw skips=0, rw skips=2; branch fallthrough max vars=30; rw skip reasons treewidth-preferred=2; max residual tw=2, cut-rank=0; branch table forecast rw=0, tw=64; branch join forecast rw=0, tw=256; branch root tw probe width=2, 2 events/137.4 us; max residual vars=32, components=7, largest=32 |
| 0-32 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 238 / 238 | 2.65 s | ganak 2.53 s + export 126.7 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding amp-block + ganak --mode 6` | 238 / 238 | 2.67 s | ganak 2.54 s + export 127.3 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding amplitude + ganak --mode 6` | 238 / 238 | 3.54 s | ganak 3.42 s + export 125.8 ms; 0 amplitude mismatches |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 238 / 238 | 4.71 s | rw width 12; rw labelled-cut-signature=12, support=12; max table 8,192; rw table forecast 32,768; rw join forecast 37,880; rw cut estimates exact=1,436, proxy=0, assignments=1,073,741,880; max signatures 4,096; 612,691 join pairs; rankwidth kernels map=1,683/1.57 s max items=8,192, join=1,683/2.99 s max items=8,192, labelled-map=718/4.7 ms max items=32, labelled=718/3.7 ms max items=48 |
| 0-32 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 238 / 238 | 11.55 s | ganak 11.42 s + export 134.5 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 238 / 238 | 571.89 s | ganak 570.43 s + export 1.47 s; 0 amplitude mismatches |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 66 / 66 | 63.3 ms | tw width 3; max table 128; 25,484 join pairs |
| 33-64 | `branch --branch-heuristic split` | 66 / 66 | 80.7 ms | 3,834 nodes; cache hits=1,556, misses=2,278, hit rate=0.406; cache avoided nodes=2,478, rate=0.646; cache canonical hits=1,514; cache canonical lookups=2,982, stores=1,554; cache entries=109, canonical=75, canonical rate=0.688, slots=1,744; cache bytes key=9,069, counts=13,952, estimated=37,667; cache trace lookup=3,802 events/4.1 ms, store=2,246 events/3.3 ms; canonical cache trace lookup=2,982 events/3.6 ms, store=1,554 events/2.4 ms; max table 128; 17,976 join pairs; delegations tw=42, rw=0; branch dispatch splits=1,665/590.3 us max components=9, tw delegates=10/2.7 ms max vars=53, root tw delegates=32/10.2 ms max vars=63; branch policy fallthroughs=802, tw skips=0, rw skips=42; branch fallthrough max vars=24; rw skip reasons treewidth-preferred=42; max residual tw=3, cut-rank=4; branch table forecast rw=0, tw=128; branch join forecast rw=0, tw=848; branch tw order width=3; branch root tw probe width=2, 32 events/6.5 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 66 / 66 | 553.8 ms | rw width 6; rw labelled-cut-signature=6, support=6; max table 512; rw table forecast 512; rw join forecast 4,506; rw cut estimates exact=1,676, proxy=0, assignments=2,305,843,009,213,694,070; max signatures 64; 283,446 join pairs; rankwidth kernels map=2,376/6.0 ms max items=128, join=2,376/23.8 ms max items=128, labelled-map=838/27.2 ms max items=128, labelled=838/31.3 ms max items=512 |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 66 / 66 | 3.12 s | ganak 3.08 s + export 45.2 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 66 / 66 | 3.18 s | ganak 3.13 s + export 50.3 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 66 / 66 | 4.10 s | ganak 4.05 s + export 46.2 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 66 / 66 | 14.45 s | ganak 14.39 s + export 61.8 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 45 / 66 | 2001.24 s | ganak 998.75 s + export 622.3 ms; 0 amplitude mismatches; 21 timeouts |
| 65-128 | `branch --branch-heuristic split` | 254 / 254 | 1.18 s | 2,532 nodes; cache hits=926, misses=1,606, hit rate=0.366; cache avoided nodes=940, rate=0.371; cache canonical hits=918; cache canonical lookups=1,936, stores=1,050; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=2,320 events/3.5 ms, store=1,394 events/3.2 ms; canonical cache trace lookup=1,936 events/3.3 ms, store=1,050 events/2.3 ms; max table 2,048; 479,788 join pairs; delegations tw=236, rw=0; branch dispatch splits=1,004/464.8 us max components=69, tw delegates=24/17.1 ms max vars=127, root tw delegates=212/414.1 ms max vars=128; branch policy fallthroughs=484, tw skips=0, rw skips=236; branch fallthrough max vars=18; rw skip reasons treewidth-preferred=236; max residual tw=7, cut-rank=11; branch table forecast rw=0, tw=2,048; branch join forecast rw=0, tw=25,088; branch tw order width=3; branch root tw probe width=7, 212 events/295.0 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 254 / 254 | 1.32 s | tw width 7; max table 2,048; 485,054 join pairs |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 254 / 254 | 38.03 s | ganak 37.78 s + export 254.1 ms; 0 amplitude mismatches |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 254 / 254 | 39.41 s | ganak 39.20 s + export 215.2 ms; 0 amplitude mismatches |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 254 / 254 | 43.29 s | ganak 43.07 s + export 214.2 ms; 0 amplitude mismatches |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 112 / 254 | 4335.63 s | rw width 10; rw labelled-cut-signature=10, support=10; max table 8,192; rw table forecast 16,384; rw join forecast 37,684; rw cut estimates exact=10,064, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 1,024; 4,458,301 join pairs; rankwidth kernels map=5,398/1.17 s max items=2,048, join=5,398/5.05 s max items=8,192, labelled-map=5,032/3.18 s max items=1,024, labelled=5,032/3.71 s max items=4,096; 142 timeouts |
| 129-256 | `branch --branch-heuristic split` | 222 / 222 | 17.42 s | 4,176 nodes; cache hits=1,472, misses=2,704, hit rate=0.352; cache avoided nodes=5,880, rate=1.408; cache canonical hits=1,368; cache canonical lookups=2,730, stores=1,424; cache entries=242, canonical=115, canonical rate=0.475, slots=1,936; cache bytes key=34,302, counts=15,488, estimated=79,486; cache trace lookup=4,010 events/10.0 ms, store=2,538 events/7.2 ms; canonical cache trace lookup=2,730 events/9.0 ms, store=1,424 events/4.3 ms; max table 262,144; 14,063,792 join pairs; delegations tw=228, rw=0; branch dispatch splits=1,977/1.3 ms max components=14, tw delegates=62/1.32 s max vars=211, root tw delegates=166/8.79 s max vars=249; branch policy fallthroughs=879, tw skips=0, rw skips=228; branch fallthrough max vars=29; rw skip reasons treewidth-preferred=228; max residual tw=14, cut-rank=18; branch table forecast rw=0, tw=262,144; branch join forecast rw=0, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 166 events/1.22 s; max residual vars=256, components=14, largest=249 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 222 / 222 | 18.37 s | tw width 14; max table 262,144; 14,077,088 join pairs |
| 129-256 | `sop2wmc --encoding amp-block + ganak --mode 6` | 222 / 222 | 109.70 s | ganak 109.31 s + export 389.7 ms; 0 amplitude mismatches |
| 129-256 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 222 / 222 | 110.15 s | ganak 109.91 s + export 243.2 ms; 0 amplitude mismatches |
| 129-256 | `sop2wmc --encoding amplitude + ganak --mode 6` | 222 / 222 | 119.38 s | ganak 119.13 s + export 255.8 ms; 0 amplitude mismatches |
| 257-512 sample | `treewidth --treewidth-order min-fill-max-degree` | 162 / 202 | 1760.96 s | tw width 19; max table 8,388,608; 275,157,818 join pairs; 40 timeouts |
| 257-512 sample | `sop2wmc --encoding amp-block + ganak --mode 6` | 158 / 202 | 1955.80 s | ganak 634.95 s + export 627.0 ms; 0 amplitude mismatches; 44 timeouts |
| 257-512 sample | `sop2wmc --encoding amp-soft + ganak --mode 6` | 158 / 202 | 1960.13 s | ganak 639.83 s + export 223.0 ms; 0 amplitude mismatches; 44 timeouts |
| 257-512 sample | `sop2wmc --encoding amplitude + ganak --mode 6` | 150 / 202 | 2076.36 s | ganak 516.03 s + export 222.9 ms; 0 amplitude mismatches; 52 timeouts |

## Competitor Comparisons

These compare the best current QSOP configuration for each tier against native QASM baselines on common rows. Each native tool is compared only on the QASM rows from that source that it can parse and fit under its cap. Speedup is native elapsed time divided by QSOP solve time, so values above `1.00x` mean QSOP is faster. Amplitude error columns use completed rows where both sides recorded amplitudes.

### FeynmanDD

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 10 / 34 | 11.8 ms | 60.2 ms | 5.11x | 10 | 0.109 | 1 | 30 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 10 / 34 | 11.8 ms | 66.2 ms | 5.62x | 10 | 0 | 0 | 30 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 10 / 34 | 11.8 ms | 9.69 s | 822.57x | 10 | 1.37e-15 | 4.57e-15 | 30 | 10 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 10 / 34 | 11.8 ms | 16.0 ms | 1.36x | 10 | 3.47e-16 | 1.22e-15 | 30 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 70 / 184 | 252.2 ms | 8.76 s | 34.75x | 70 | 0.0185 | 1 | 100 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 70 / 184 | 252.2 ms | 27.56 s | 109.28x | 70 | 2.1e-17 | 8.95e-17 | 100 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 22 / 184 | 124.3 ms | 48.68 s | 391.64x | 22 | 2.21e-15 | 9.87e-15 | 100 | 10 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 70 / 184 | 252.2 ms | 39.24 s | 155.62x | 70 | 1.59e-16 | 2.22e-15 | 100 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 28 / 136 | 1.33 s | 854.6 ms | 0.64x | 28 | 0.0126 | 0.353 | 80 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 28 / 136 | 1.33 s | 135.5 ms | 0.10x | 28 | 5.47e-17 | 1.18e-15 | 80 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 6 / 136 | 38.8 ms | 22.43 s | 577.70x | 6 | 6.41e-15 | 1.58e-14 | 80 | 10 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 28 / 136 | 1.33 s | 4.78 s | 3.58x | 28 | 7.16e-16 | 3.66e-15 | 80 | 16 | 30.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 2 / 2 | 7.0 ms | 5.8 ms | 0.82x | 2 | 0.354 | 0.707 | 3 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 2 / 2 | 7.0 ms | 6.4 ms | 0.91x | 2 | 0 | 0 | 3 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 2 / 2 | 7.0 ms | 12.2 ms | 1.73x | 2 | 1.08e-16 | 1.3e-16 | 3 | 10 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 2 / 2 | 7.0 ms | 1.9 ms | 0.28x | 2 | 0 | 0 | 3 | 16 | 30.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 32 / 32 | 29.2 ms | 69.3 ms | 2.37x | 32 | 0.0938 | 1 | 7 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 32 / 32 | 29.2 ms | 116.1 ms | 3.97x | 32 | 0 | 0 | 7 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 32 / 32 | 29.2 ms | 2.13 s | 72.80x | 32 | 2.16e-15 | 6.82e-15 | 7 | 10 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 32 / 32 | 29.2 ms | 42.8 ms | 1.46x | 32 | 5.96e-16 | 1.89e-15 | 7 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `aer-statevector` | 68 / 68 | 430.9 ms | 258.8 ms | 0.60x | 68 | 0.124 | 1 | 14 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 68 / 68 | 430.9 ms | 263.4 ms | 0.61x | 68 | 8.13e-17 | 1.18e-15 | 14 | 16 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `pyzx-matrix` | 66 / 68 | 366.3 ms | 88.77 s | 242.31x | 66 | 5.16e-15 | 1.58e-14 | 14 | 10 | 30.0 | 4096 |
| 65-128 | `branch --branch-heuristic split` | `qiskit-statevector` | 68 / 68 | 430.9 ms | 1.09 s | 2.52x | 68 | 1.16e-15 | 3.44e-15 | 14 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `aer-statevector` | 76 / 86 | 2.16 s | 3.15 s | 1.46x | 76 | 0.132 | 1 | 19 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `mqt-ddsim-statevector` | 76 / 86 | 2.16 s | 381.4 ms | 0.18x | 76 | 5.6e-17 | 1.18e-15 | 19 | 16 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `pyzx-matrix` | 40 / 86 | 675.5 ms | 119.25 s | 176.53x | 40 | 1.49e-14 | 3.25e-14 | 19 | 10 | 30.0 | 4096 |
| 129-256 | `branch --branch-heuristic split` | `qiskit-statevector` | 76 / 86 | 2.16 s | 10.54 s | 4.89x | 76 | 2.28e-15 | 6e-15 | 19 | 16 | 30.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `branch --branch-heuristic split`; 129-256: `branch --branch-heuristic split`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 162 / 202 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
