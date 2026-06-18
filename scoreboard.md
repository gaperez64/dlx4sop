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
| 0-32 | `branch --branch-heuristic split` | 238 / 238 | 170.7 ms | 2,419 nodes; cache hits=526, misses=1,893, hit rate=0.217; cache avoided nodes=454, rate=0.188; cache canonical hits=526; cache canonical lookups=1,746, stores=1,517; cache entries=72, canonical=53, canonical rate=0.736, slots=576; cache bytes key=4,770, counts=4,608, estimated=24,199; cache trace lookup=2,397 events/3.2 ms, store=1,871 events/4.3 ms; canonical cache trace lookup=1,746 events/3.0 ms, store=1,517 events/3.7 ms; rw labelled-cut-signature=2, support=2; max table 64; rw table forecast 32; rw join forecast 180; rw cut estimates exact=0, proxy=0, assignments=0; 12,440 join pairs; delegations tw=58, rw=0; branch dispatch splits=1,086/423.5 us max components=7, tw delegates=36/4.8 ms max vars=28, root tw delegates=22/3.9 ms max vars=32; branch policy fallthroughs=588, tw skips=0, rw skips=58; branch fallthrough max vars=15; rw skip reasons treewidth-preferred=28, policy=14, table-forecast=16; max residual tw=2, cut-rank=6; branch rw probe labelled-cut-signature=2, support=2; branch table forecast rw=32, tw=64; branch join forecast rw=180, tw=256; branch tw order width=2; branch root tw probe width=2, 22 events/1.3 ms; max residual vars=32, components=7, largest=32 |
| 0-32 | `rankwidth --rankwidth-generate left-deep --rankwidth-mode count-table` | 238 / 238 | 760.2 ms | rw width 12; rw labelled-cut-signature=12, support=12; max table 8,192; rw table forecast 32,768; rw join forecast 37,880; rw cut estimates exact=1,436, proxy=0, assignments=1,073,741,880; max signatures 4,096; 612,691 join pairs |
| 0-32 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 238 / 238 | 2.65 s | ganak 2.53 s + export 126.7 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding amp-block + ganak --mode 6` | 238 / 238 | 2.67 s | ganak 2.54 s + export 127.3 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding amplitude + ganak --mode 6` | 238 / 238 | 3.54 s | ganak 3.42 s + export 125.8 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 238 / 238 | 11.55 s | ganak 11.42 s + export 134.5 ms; 0 amplitude mismatches |
| 0-32 | `sop2wmc --encoding residue + ganak --mode 0` | 238 / 238 | 571.89 s | ganak 570.43 s + export 1.47 s; 0 amplitude mismatches |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 66 / 66 | 63.3 ms | tw width 3; max table 128; 25,484 join pairs |
| 33-64 | `branch --branch-heuristic split` | 66 / 66 | 114.4 ms | 2,174 nodes; cache hits=712, misses=1,462, hit rate=0.328; cache avoided nodes=666, rate=0.306; cache canonical hits=712; cache canonical lookups=1,742, stores=1,116; cache entries=85, canonical=68, canonical rate=0.800, slots=768; cache bytes key=5,028, counts=6,144, estimated=25,316; cache trace lookup=2,142 events/2.6 ms, store=1,430 events/2.5 ms; canonical cache trace lookup=1,742 events/2.5 ms, store=1,116 events/2.0 ms; rw labelled-cut-signature=3, support=3; max table 128; rw table forecast 64; rw join forecast 408; rw cut estimates exact=0, proxy=0, assignments=0; 20,460 join pairs; delegations tw=60, rw=0; branch dispatch splits=943/372.7 us max components=9, tw delegates=28/5.0 ms max vars=53, root tw delegates=32/10.3 ms max vars=63; branch policy fallthroughs=488, tw skips=0, rw skips=60; branch fallthrough max vars=10; rw skip reasons treewidth-preferred=52, policy=6, table-forecast=2; max residual tw=3, cut-rank=9; branch rw probe labelled-cut-signature=3, support=3; branch table forecast rw=64, tw=128; branch join forecast rw=408, tw=848; branch tw order width=3; branch root tw probe width=2, 32 events/6.9 ms; max residual vars=63, components=9, largest=63 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 66 / 66 | 357.8 ms | rw width 6; rw labelled-cut-signature=6, support=6; max table 512; rw table forecast 512; rw join forecast 4,506; rw cut estimates exact=1,676, proxy=0, assignments=2,305,843,009,213,694,070; max signatures 64; 283,446 join pairs |
| 33-64 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 66 / 66 | 3.12 s | ganak 3.08 s + export 45.2 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding amp-block + ganak --mode 6` | 66 / 66 | 3.18 s | ganak 3.13 s + export 50.3 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding amplitude + ganak --mode 6` | 66 / 66 | 4.10 s | ganak 4.05 s + export 46.2 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding residue-fourier + ganak --mode 6` | 66 / 66 | 14.45 s | ganak 14.39 s + export 61.8 ms; 0 amplitude mismatches |
| 33-64 | `sop2wmc --encoding residue + ganak --mode 0` | 45 / 66 | 2001.24 s | ganak 998.75 s + export 622.3 ms; 0 amplitude mismatches; 21 timeouts |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 254 / 254 | 1.32 s | tw width 7; max table 2,048; 485,054 join pairs |
| 65-128 | `branch --branch-heuristic split` | 254 / 254 | 2.07 s | 2,094 nodes; cache hits=701, misses=1,393, hit rate=0.335; cache avoided nodes=640, rate=0.306; cache canonical hits=699; cache canonical lookups=1,582, stores=915; cache entries=213, canonical=182, canonical rate=0.854, slots=1,704; cache bytes key=11,658, counts=13,632, estimated=54,986; cache trace lookup=1,882 events/3.1 ms, store=1,181 events/3.0 ms; canonical cache trace lookup=1,582 events/3.0 ms, store=915 events/2.2 ms; rw labelled-cut-signature=3, support=3; max table 2,048; rw table forecast 64; rw join forecast 824; rw cut estimates exact=0, proxy=0, assignments=0; 480,448 join pairs; delegations tw=242, rw=0; branch dispatch splits=809/428.6 us max components=69, tw delegates=30/18.1 ms max vars=127, root tw delegates=212/430.1 ms max vars=128; branch policy fallthroughs=400, tw skips=0, rw skips=242; branch fallthrough max vars=10; rw skip reasons treewidth-preferred=238, policy=2, table-forecast=2; max residual tw=7, cut-rank=11; branch rw probe labelled-cut-signature=3, support=3; branch table forecast rw=64, tw=2,048; branch join forecast rw=824, tw=25,088; branch tw order width=3; branch root tw probe width=7, 212 events/310.2 ms; max residual vars=128, components=69, largest=128 |
| 65-128 | `sop2wmc --encoding amp-block + ganak --mode 6` | 254 / 254 | 38.03 s | ganak 37.78 s + export 254.1 ms; 0 amplitude mismatches |
| 65-128 | `sop2wmc --encoding amp-soft + ganak --mode 6` | 254 / 254 | 39.41 s | ganak 39.20 s + export 215.2 ms; 0 amplitude mismatches |
| 65-128 | `sop2wmc --encoding amplitude + ganak --mode 6` | 254 / 254 | 43.29 s | ganak 43.07 s + export 214.2 ms; 0 amplitude mismatches |
| 65-128 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 118 / 254 | 4100.45 s | rw width 12; rw labelled-cut-signature=12, support=12; max table 32,768; rw table forecast 32,768; rw join forecast 239,725; rw cut estimates exact=10,064, proxy=0, assignments=18,446,744,073,709,551,615; max signatures 4,096; 11,622,465 join pairs; 136 timeouts |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 222 / 222 | 18.37 s | tw width 14; max table 262,144; 14,077,088 join pairs |
| 129-256 | `branch --branch-heuristic split` | 222 / 222 | 24.08 s | 2,036 nodes; cache hits=497, misses=1,539, hit rate=0.244; cache avoided nodes=976, rate=0.479; cache canonical hits=461; cache canonical lookups=1,347, stores=923; cache entries=96, canonical=62, canonical rate=0.646, slots=768; cache bytes key=12,733, counts=6,144, estimated=33,706; cache trace lookup=1,870 events/3.7 ms, store=1,373 events/4.8 ms; canonical cache trace lookup=1,347 events/3.2 ms, store=923 events/2.5 ms; rw labelled-cut-signature=3, support=3; max table 262,144; rw table forecast 64; rw join forecast 1,808; rw cut estimates exact=0, proxy=0, assignments=0; 14,069,576 join pairs; delegations tw=266, rw=0; branch dispatch splits=953/1.0 ms max components=14, tw delegates=100/1.33 s max vars=211, root tw delegates=166/9.01 s max vars=249; branch policy fallthroughs=449, tw skips=0, rw skips=266; branch fallthrough max vars=15; rw skip reasons treewidth-preferred=214, policy=32, table-forecast=20; max residual tw=14, cut-rank=18; branch rw probe labelled-cut-signature=3, support=3; branch table forecast rw=64, tw=262,144; branch join forecast rw=1,808, tw=5,898,240; branch tw order width=14; branch root tw probe width=14, 166 events/1.30 s; max residual vars=256, components=14, largest=249 |
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
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 70 / 184 | 285.7 ms | 8.76 s | 30.67x | 70 | 0.0185 | 1 | 100 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 70 / 184 | 285.7 ms | 27.56 s | 96.45x | 70 | 2.1e-17 | 8.95e-17 | 100 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 22 / 184 | 128.6 ms | 48.68 s | 378.50x | 22 | 2.21e-15 | 9.87e-15 | 100 | 10 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 70 / 184 | 285.7 ms | 39.24 s | 137.35x | 70 | 1.59e-16 | 2.22e-15 | 100 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 28 / 136 | 617.1 ms | 854.6 ms | 1.38x | 28 | 0.0126 | 0.353 | 80 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 28 / 136 | 617.1 ms | 135.5 ms | 0.22x | 28 | 5.47e-17 | 1.18e-15 | 80 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 6 / 136 | 80.6 ms | 22.43 s | 278.22x | 6 | 6.41e-15 | 1.58e-14 | 80 | 10 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 28 / 136 | 617.1 ms | 4.78 s | 7.74x | 28 | 7.16e-16 | 3.66e-15 | 80 | 16 | 30.0 | 4096 |

### MQT Bench

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 2 / 2 | 9.3 ms | 5.8 ms | 0.62x | 2 | 0.354 | 0.707 | 3 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 2 / 2 | 9.3 ms | 6.4 ms | 0.69x | 2 | 0 | 0 | 3 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 2 / 2 | 9.3 ms | 12.2 ms | 1.31x | 2 | 1.08e-16 | 1.3e-16 | 3 | 10 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 2 / 2 | 9.3 ms | 1.9 ms | 0.21x | 2 | 0 | 0 | 3 | 16 | 30.0 | 4096 |

### PyZX

| Tier | QSOP configuration | Native engine | Both OK / matched | QSOP time | Native time | QSOP speedup | Amplitude checked | Mean amplitude error | Max amplitude error | Max boundary qubits | Qubit cap | Timeout | Memory cap |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 32 / 32 | 29.2 ms | 69.3 ms | 2.37x | 32 | 0.0938 | 1 | 7 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 32 / 32 | 29.2 ms | 116.1 ms | 3.97x | 32 | 0 | 0 | 7 | 16 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 32 / 32 | 29.2 ms | 2.13 s | 72.80x | 32 | 2.16e-15 | 6.82e-15 | 7 | 10 | 30.0 | 4096 |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 32 / 32 | 29.2 ms | 42.8 ms | 1.46x | 32 | 5.96e-16 | 1.89e-15 | 7 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 68 / 68 | 330.9 ms | 258.8 ms | 0.78x | 68 | 0.124 | 1 | 14 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 68 / 68 | 330.9 ms | 263.4 ms | 0.80x | 68 | 8.13e-17 | 1.18e-15 | 14 | 16 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 66 / 68 | 309.3 ms | 88.77 s | 286.97x | 66 | 5.16e-15 | 1.58e-14 | 14 | 10 | 30.0 | 4096 |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 68 / 68 | 330.9 ms | 1.09 s | 3.28x | 68 | 1.16e-15 | 3.44e-15 | 14 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `aer-statevector` | 76 / 86 | 2.68 s | 3.15 s | 1.18x | 76 | 0.132 | 1 | 19 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `mqt-ddsim-statevector` | 76 / 86 | 2.68 s | 381.4 ms | 0.14x | 76 | 5.6e-17 | 1.18e-15 | 19 | 16 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `pyzx-matrix` | 40 / 86 | 1.45 s | 119.25 s | 82.05x | 40 | 1.49e-14 | 3.25e-14 | 19 | 10 | 30.0 | 4096 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | `qiskit-statevector` | 76 / 86 | 2.68 s | 10.54 s | 3.93x | 76 | 2.28e-15 | 6e-15 | 19 | 16 | 30.0 | 4096 |

## Current Takeaway

Best current internal configurations by tier: 0-32: `treewidth --treewidth-order min-fill-max-degree`; 33-64: `treewidth --treewidth-order min-fill-max-degree`; 65-128: `treewidth --treewidth-order min-fill-max-degree`; 129-256: `treewidth --treewidth-order min-fill-max-degree`; 257-512 sample: `treewidth --treewidth-order min-fill-max-degree`.
The 257-512 stratified sample is not a full tier yet: 162 / 202 rows solve under the current timeout cap.
Treewidth remains the clean direct-DP baseline. Hybrid branch is the best current widened-tier configuration when component splitting and treewidth handoff trigger. Native comparisons are now capped and source-local; dense statevector tools can still win on low-qubit rows, while QSOP remains strong on many fixed-boundary rows with large imported variable counts.
