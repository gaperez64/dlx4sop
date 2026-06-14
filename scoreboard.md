# Scoreboard

Last updated: 2026-06-14.

This tracks progress toward a competitive exact strong simulator based on
labelled quadratic SOPs. The current benchmark contract is fixed-boundary strong
simulation: import a static circuit into QSOP, solve the exact residue-count
histogram, and compare with native simulators where possible.

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
that cap and promoted into controlled widened tiers.

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

## Support Widths

`sop-stats --exact-widths` computes exact support-graph treewidth and GF(2)
rankwidth only under a small variable cap. These are support-graph certificates,
not labelled cut-signature-width certificates.

| Corpus tier | Exact cap | Exact coverage | Exact support treewidth | Exact support rankwidth | Heuristic notes |
| --- | ---: | ---: | --- | --- | --- |
| Checked-in corpus | 12 vars | 32 / 32 | `0:19, 1:13` | `0:19, 1:13` | max min-fill width 1 |
| 0-32 importer-fed pool | 16 vars | 100 / 133 | `0:63, 1:35, 2:2` | `0:63, 1:35, 2:2` | min-fill width `0:63, 1:54, 2:16` |
| 33-64 promoted tier | 16 vars | 0 / 32 | n/a | n/a | min-fill width `1:17, 2:14, 3:1` |
| 65-128 promoted tier | 16 vars | 0 / 130 | n/a | n/a | min-fill width `0:4, 1:6, 2:17, 3:17, 4:43, 5:24, 6:15, 7:4` |

The widened-tier exact-width rows are intentionally skipped by the cap. Current
claims there are about heuristic support widths and solver table sizes.

## Solver Results

Command shape:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve \
  --manifest CORPUS.json --backend treewidth --treewidth-order min-fill \
  --treewidth-order min-degree --treewidth-order min-fill-max-degree \
  --trace --format jsonl
```

`Solved / records` reports only successful solver rows as solved. Timeout rows
are kept in the denominator. The 65-128 branch and rankwidth rows below are the
first five promoted-tier boundaries with a 5s per-solve cap; the treewidth rows
are full-tier runs.

| Tier | Backend/configuration | Solved / records | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 51.7 ms | width 2; max table 64; 12,692 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 62.3 ms | width 3; max table 48; 43,702 join pairs; skips 12 zero-variable decomposition guards |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 79.0 ms | 9,781 nodes; 15,314 leaves; no cache hits |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 26.3 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 27.0 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 27.3 ms | width 3; max table 128; 16,176 join pairs |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 197.9 ms | width 6; max table 512; 239,018 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 668.7 ms | width 7; max table 2048; 289,681 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 669.4 ms | width 7; max table 2048; 293,297 join pairs |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 676.5 ms | width 8; max table 4096; 293,423 join pairs |
| 65-128 sample | `branch --branch-heuristic split` | 0 / 5 | 25.03 s | 5 timeouts |
| 65-128 sample | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 0 / 5 | 25.02 s | 5 timeouts |

Current widened-tier signal: treewidth is the only backend that completes the
full 65-128 promoted tier comfortably. Branch and generated rankwidth both time
out on the first five sign-only PyZX Toffoli-style boundaries under a 5s cap.

## Rankwidth Diagnostics

The rankwidth generator now compares the generated `min-fill-cut` decomposition
against a plain linear candidate on labelled instances and keeps the lower
support-width candidate. This is a proxy for labelled cost, not a labelled-width
certificate, but it fixed the known 33-64 labelled outlier.

FeynmanDD `random_10qubit_0` is labelled, with 61 imported variables and 69
quadratic terms.

| Backend/configuration | Solve time | Width/table | Main trace signal |
| --- | ---: | --- | --- |
| `treewidth --treewidth-order min-fill-max-degree` | about 1.1 ms | width 3; max table 128 | order construction dominates |
| `branch --branch-heuristic split` | about 8.9 ms | 2,367 nodes; no cache hits | variable selection dominates |
| `rankwidth --rankwidth-generate linear --rankwidth-mode count-table` | about 27 ms | width 6; max table 512 | labelled joins and join maps |
| `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | about 33 ms | width 6; max table 512 | labelled joins and join maps |
| `rankwidth --rankwidth-generate balanced --rankwidth-mode count-table` | about 2.7 s | width 7; max table 512 | labelled joins dominate |
| `rankwidth --rankwidth-generate min-fill --rankwidth-mode count-table` | >20 s cap | no completed row | generator/decomposition is bad for this case |

After this change, the immediate rankwidth problem is no longer the labelled
33-64 outlier. The remaining blocker is sign-heavy 65-128 Toffoli-style cases
where generated decompositions still create too much join work.

## Treewidth Trace Profile

Trace rows separate order construction from table joins. On the widened corpus,
treewidth solve time is dominated by greedy order construction, not by dense
factor multiplication.

| Tier | Order | Total solve | Order phase | Multiply phase | Other traced phases |
| --- | --- | ---: | ---: | ---: | ---: |
| 33-64 | `min-fill` | 26.3 ms | 66.3% | 25.0% | 8.6% |
| 33-64 | `min-degree` | 27.0 ms | 66.9% | 24.7% | 8.4% |
| 33-64 | `min-fill-max-degree` | 27.3 ms | 66.2% | 25.1% | 8.7% |
| 65-128 | `min-fill-max-degree` | 668.7 ms | 71.4% | 24.9% | 3.7% |
| 65-128 | `min-fill` | 669.4 ms | 71.0% | 25.3% | 3.6% |
| 65-128 | `min-degree` | 676.5 ms | 70.8% | 25.5% | 3.6% |

The next treewidth optimization target is therefore cached or incremental
ordering work, while keeping the current DP table behavior as the correctness
baseline.

## Native Simulator Results

These are native QASM-set comparisons, not `sop2X` exports. Dense statevector
backends use a 16-qubit cap; PyZX matrix mode uses a 10-qubit cap because it
materializes full matrices.

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
Treewidth is the best current solver configuration and handles the full 65-128
promoted tier. Branch needs a narrower role or stronger decomposition-style
splitting. Rankwidth now handles the known labelled 33-64 outlier, but needs
better generated decompositions for sign-heavy widened cases before it can be a
serious competitor.
