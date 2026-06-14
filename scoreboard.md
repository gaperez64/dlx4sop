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
reports. In the table below, `OK` means accepted by that capped import pass.
Rows above 32 are structurally importable cases that were rejected only by the
cap and then promoted into controlled widened manifests. A separate 129-256
promotion run emits 112 solver-ready rows from the same upstream sources.

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
claims for those tiers are based on heuristic support widths and solver table
sizes.

## Solver Results

Command shape:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve \
  --manifest CORPUS.json --backend treewidth --treewidth-order min-fill \
  --treewidth-order min-degree --treewidth-order min-fill-max-degree \
  --trace --format jsonl
```

`Solved / records` reports successful solver rows over attempted rows. Timeout
rows stay in the denominator. Rankwidth rows on 0-32 skip zero-variable
boundaries because the generated-decomposition backend currently requires at
least one variable. Branch rows include the hybrid policy: split disconnected
residuals first, probe a min-fill support-width upper bound, delegate low-width
residuals to treewidth, and use generated rankwidth only when a cheap linear
cut-rank proxy says rankwidth may be substantially smaller. Widened branch leaf
counts are mostly a saturation/work signal, not a useful exact aggregate.

| Tier | Backend/configuration | Solved / records | Total solve time | Key stats |
| --- | --- | ---: | ---: | --- |
| 0-32 | `treewidth --treewidth-order min-degree` | 133 / 133 | 58.0 ms | tw width 2; max table 64; 12,692 join pairs |
| 0-32 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 121 / 121 | 67.8 ms | rw width 3; max table 48; 43,702 join pairs; skips 12 zero-variable decomposition guards |
| 0-32 | `branch --branch-heuristic split` | 133 / 133 | 80.2 ms | 9,545 nodes; cache 0 / 9,545; delegations tw=1, rw=0 |
| 33-64 | `treewidth --treewidth-order min-fill` | 32 / 32 | 26.3 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-degree` | 32 / 32 | 27.0 ms | width 3; max table 128; 16,182 join pairs |
| 33-64 | `treewidth --treewidth-order min-fill-max-degree` | 32 / 32 | 27.3 ms | width 3; max table 128; 16,176 join pairs |
| 33-64 | `branch --branch-heuristic split` | 32 / 32 | 39.5 ms | 3,525 nodes; cache 0 / 3,525; delegations tw=20, rw=0 |
| 33-64 | `rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode count-table` | 32 / 32 | 124.1 ms | rw width 6; max table 512; 141,928 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill-max-degree` | 130 / 130 | 668.7 ms | width 7; max table 2048; 289,681 join pairs |
| 65-128 | `treewidth --treewidth-order min-fill` | 130 / 130 | 669.4 ms | width 7; max table 2048; 293,297 join pairs |
| 65-128 | `treewidth --treewidth-order min-degree` | 130 / 130 | 676.5 ms | width 8; max table 4096; 293,423 join pairs |
| 65-128 | `branch --branch-heuristic split` | 130 / 130 | 1.33 s | 1,753 nodes; cache 0 / 1,753; delegations tw=121, rw=0 |
| 129-256 | `treewidth --treewidth-order min-fill-max-degree` | 112 / 112 | 11.37 s | tw width 14; max table 262,144; 7,168,072 join pairs |

Current widened-tier signal: direct treewidth is still the best default solver.
Hybrid branch now completes the full 65-128 tier by handing most large residuals
to treewidth, but it is slower than calling treewidth directly. The full 129-256
treewidth row completes; a full branch row for that tier is not listed yet
because hard cases still fall through to branch search and need tighter
pre-solve policy before that is a useful headline number.

## Rankwidth Diagnostics

The rankwidth generator now compares the generated `min-fill-cut` decomposition
against a plain linear candidate and keeps the lower support-width candidate.
This is a support-graph proxy, not a labelled-width certificate, but it fixed
the known 33-64 labelled outlier and is also used as the branch rankwidth
handoff candidate.

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

Rankwidth is now available as a branch handoff when the treewidth bound is high
and a cheap linear cut-rank proxy is promising. The current QASM tiers mostly
trigger treewidth handoffs; synthetic complete-bipartite tests exercise the
rankwidth handoff path. The next rankwidth problem is better generated
decompositions and labelled-width estimates, not basic labelled support.

## Treewidth Trace Profile

Trace rows separate order construction from table joins. Percentages are over
traced solver phases, not total process time. On 33-64 and 65-128 the greedy
order pass dominates; on the larger 129-256 tier, dense multiplication becomes
the largest traced kernel because width reaches 14.

| Tier | Order | Total solve | Order phase | Multiply phase | Other traced phases |
| --- | --- | ---: | ---: | ---: | ---: |
| 33-64 | `min-fill` | 26.3 ms | 66.3% | 25.0% | 8.6% |
| 33-64 | `min-degree` | 27.0 ms | 66.9% | 24.7% | 8.4% |
| 33-64 | `min-fill-max-degree` | 27.3 ms | 66.2% | 25.1% | 8.7% |
| 65-128 | `min-fill-max-degree` | 668.7 ms | 71.4% | 24.9% | 3.7% |
| 65-128 | `min-fill` | 669.4 ms | 71.0% | 25.3% | 3.6% |
| 65-128 | `min-degree` | 676.5 ms | 70.8% | 25.5% | 3.6% |
| 129-256 | `min-fill-max-degree` | 11.37 s | 31.8% | 66.2% | 2.0% |

Treewidth optimization now has two regimes: cached or incremental order scoring
for small and medium tiers, and faster dense-table kernels for the 129-256 tier.
The current DP table behavior remains the correctness baseline.

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
Treewidth is the best current solver configuration and handles the promoted
0-32, 33-64, 65-128, and 129-256 tiers. Hybrid branch is now a decomposition
and DP-dispatch backend rather than a pure enumerator, but direct treewidth is
still faster on these tiers. Rankwidth handoff support exists and is tested;
the remaining rankwidth work is better decomposition generation and labelled
width prediction.
