# Scoreboard

Last updated: 2026-06-14.

This tracks progress toward a competitive exact strong simulator based on
labelled quadratic SOPs. The current benchmark contract is fixed-boundary
strong simulation: import a static circuit into QSOP, solve the exact
residue-count histogram, and compare with native simulators where possible.

## Corpus

External sources are cited by upstream repository. Generated manifests and
reports are local benchmark artifacts, not citation targets.

| Source | Upstream | Inputs | Imported at 0-32 vars | Too large at 32 vars | Other unsupported |
| --- | --- | ---: | ---: | ---: | ---: |
| Internal corpus | `tests/qasm_solver_corpus.json` | 12 | 12 | 0 | 0 |
| FeynmanDD | <https://github.com/cqs-thu/feynman-decision-diagram> | 425 | 52 | 361 | 12 |
| MQT Bench | <https://github.com/munich-quantum-toolkit/bench> | 32 | 20 | 3 | 9 |
| PyZX | <https://github.com/zxcalc/pyzx> | 344 | 29 | 292 | 23 |

External import classification by imported QSOP variables, from 32-variable-cap
reports. Rows above 32 are structurally importable cases that were rejected by
that cap and later promoted into controlled widened tiers.

| Tier | Records | OK | Too large | Other unsupported | Sign | Labelled |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0-32 | 101 | 101 | 0 | 0 | 88 | 13 |
| 33-64 | 32 | 0 | 32 | 0 | 23 | 9 |
| 65-128 | 130 | 0 | 130 | 0 | 102 | 28 |
| 129-256 | 112 | 0 | 112 | 0 | 49 | 63 |
| 257+ | 382 | 0 | 382 | 0 | 179 | 203 |
| Unknown unsupported | 44 | 0 | 0 | 44 | 0 | 0 |

Use `tools/summarize_qasm_report.py` or `tools/render_scoreboard.py` on
generated import reports and benchmark JSONL to refresh these tables.

## Solver Results

Command shape:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve \
  --manifest CORPUS.json --backend treewidth --treewidth-order min-fill \
  --treewidth-order min-degree --treewidth-order min-fill-max-degree \
  --trace --format jsonl
```

| Tier | Backend/configuration | Solved records | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 | 56.9 ms | width 2; max table 64 |
| 0-32 | `branch --branch-heuristic split` | 133 | 81.1 ms | 9,781 nodes; no cache hits |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 | 70.5 ms | skips 12 zero-variable decomposition guards |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 | 31.1 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 | 31.8 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 | 32.0 ms | width 3; max table 128; 16,176 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 | 99.0 ms | 19,629 nodes; 32,678 leaves; no cache hits |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 | 1.88 s | width 7; max table 1024; 386,804 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 | 682.4 ms | width 7; max table 2048; 289,681 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 | 686.4 ms | width 7; max table 2048; 293,297 join pairs |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 | 689.0 ms | width 8; max table 4096; 293,423 join pairs |

Current widened-tier signal: treewidth is the best full-pool backend. Branch
split did not complete the first 65-128 case within a 20 s cap. Rankwidth
`min-fill-cut` did not complete the first 65-128 case within a 20 s cap, while
the rankwidth `linear` generator solves that same sign-only case in about 48 ms.

## Rankwidth Generator Diagnostic

FeynmanDD `random_10qubit_0` is the 33-64 tier case that previously dominated
rankwidth time. It is labelled, with 61 imported variables and 69 quadratic
terms.

| Backend/configuration | Solve time | Width/table | Main trace signal |
| --- | ---: | --- | --- |
| `treewidth --treewidth-order min-fill-max-degree` | 1.14 ms | width 3; max table 128 | order construction dominates |
| `branch --branch-heuristic split` | 8.86 ms | 2,367 nodes; no cache hits | variable selection dominates |
| `rankwidth --rankwidth-generate linear --rankwidth-mode count-table` | 26.9 ms | width 6; max table 512 | labelled joins and join maps |
| `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 1.72 s | width 7; max table 1024 | labelled joins dominate |
| `rankwidth --rankwidth-generate balanced --rankwidth-mode count-table` | 2.74 s | width 7; max table 512 | labelled joins dominate |
| `rankwidth --rankwidth-generate min-fill --rankwidth-mode count-table` | >20 s cap | no completed row | generator/decomposition is bad for this case |

The immediate rankwidth problem is decomposition quality and labelled join
cost, not basic labelled-DP correctness.

## Native Simulator Results

These are native QASM-set comparisons, not `sop2X` exports. The native runner
adds compatibility definitions for QASM gates such as `ccz` and `iswap` when
the simulator parser needs them. Dense statevector backends use a 16-qubit cap;
PyZX matrix mode uses a 10-qubit cap because it materializes full matrices.
The Qiskit manifest correctness checker now covers the 20 / 32 promoted-tier
boundaries under the 16-qubit cap with no parser skips; the other 12 are qubit
cap skips.

| Tier | Engine | OK / records | Total elapsed | Max qubits | Main skip reason |
| --- | --- | ---: | ---: | ---: | --- |
| 33-64 | `qiskit-statevector` | 20 / 32 | 29.0 ms | 10 | qubit cap |
| 33-64 | `aer-statevector` | 20 / 32 | 80.0 ms | 10 | qubit cap |
| 33-64 | `mqt-ddsim-statevector` | 20 / 32 | 391.9 ms | 10 | qubit cap |
| 33-64 | `pyzx-matrix` | 20 / 32 | 5.21 s | 10 | qubit cap |
| 65-128 | `aer-statevector` | 73 / 130 | 18.71 s | 16 | qubit cap |
| 65-128 | `qiskit-statevector` | 73 / 130 | 53.01 s | 16 | qubit cap |
| 65-128 | `mqt-ddsim-statevector` | 73 / 130 | 111.48 s | 16 | qubit cap |
| 65-128 | `pyzx-matrix` | 48 / 130 | 106.26 s | 10 | qubit cap |

## Current Takeaway

The importer-fed corpus is now large enough to expose backend separation.
Treewidth handles the 65-128 promoted tier cleanly. Branch needs either much
better decomposition-style splitting or a narrower role. Rankwidth needs better
generated decompositions for labelled cases before it can be a serious
competitor on the widened corpus.
