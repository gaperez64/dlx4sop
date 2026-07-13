# Corpus-wide regression sweep (2026-07-13)

## Result

The default solver has no solved-set regression on the complete local QCCQ gauntlet corpus.
All 2,370 QPY payloads were measured at the same timeout, memory limit, job count, compiler,
and release/LTO settings.

| Metric | Baseline `504e223` | Current `72c2ba2` | Change |
|---|---:|---:|---:|
| Solved | 1,811 | 1,835 | +24 |
| Import failures | 44 | 44 | 0 |
| `max_fallback_vars` refusals | 394 | 394 | 0 |
| `max_vars` refusals | 89 | 89 | 0 |
| `no_delegate` refusals | 6 | 6 | 0 |
| OOM refusals | 24 | 0 | -24 |
| Timeouts | 2 | 2 | 0 |

There are zero solved-to-failed transitions, no new refusals, no new OOMs, and no new
timeouts. The two unchanged timeouts are
`mqt-easy/qwalk-noancilla_indep_qiskit_12` and
`mqt-easy/qwalk-noancilla_indep_qiskit_13`.

The 24 newly solved instances are the result-vector preflight cases in both `mqt-big` and
`mqt2040`: `qft` and `qftentangled` at sizes 28--30, `qpeexact` at sizes 29--31, and
`qpeinexact` at sizes 28--30.

## Solved count by suite

| Suite | Cases | Baseline solved | Current solved |
|---|---:|---:|---:|
| `alg85` | 154 | 154 | 154 |
| `inferq603` | 150 | 150 | 150 |
| `mqt-big` | 1,396 | 915 | 927 |
| `mqt-easy` | 271 | 269 | 269 |
| `mqt-fixed` | 33 | 33 | 33 |
| `mqt2040` | 258 | 182 | 194 |
| `supermarq` | 108 | 108 | 108 |
| **Total** | **2,370** | **1,811** | **1,835** |

## Timing on the previously solved set

All 1,811 baseline-solved cases remain solved.

| Metric | Baseline | Current | Change |
|---|---:|---:|---:|
| Total solver wall time | 1,217.347 s | 532.741 s | -56.2% |
| p50 | 0.004 s | 0.005 s | +0.001 s |
| p95 | 0.564 s | 0.410 s | -27.3% |
| Maximum | 76.140 s | 29.739 s | -60.9% |

The regression gate flags a case only when it slows by both more than 1 second and more than
25%. No case crosses that gate. The largest absolute slowdown is
`mqt-big/qpeinexact_indep_qiskit_27`, from 14.032 s to 14.859 s (+0.827 s, +5.9%).

## Protocol

- Corpus: every `*/v1/payloads/*.qpy` under the local QCCQ gauntlet datasets (2,370 cases).
- Runner: `scripts/bench_gauntlet.py`, using the gauntlet QPY-to-QASM front end.
- Solver command: default AUTO branch backend with stats output and result inclusion.
- Per-stage timeout: 120 seconds.
- Per-solver address-space limit: 12 GiB.
- Service memory limit: 14 GiB with swap disabled.
- Jobs: 1.
- Builds: Meson release with LTO enabled, GCC 15.2.1.
- CPU: Intel Core i7-11850H, 8 cores / 16 threads; AVX-512 runtime dispatch available.

The pre-plan source at `504e223` was rebuilt specifically for this comparison with the same
release and LTO settings as current. Its 42-test suite passed before measurement. The current
release suite also passes 42/42 after the sweep fix. A Clang ASan+UBSan build passes the same
42/42 tests with leak detection disabled because LeakSanitizer cannot operate under this
environment's ptrace restrictions.

The long run was resumed after terminal-session interruptions. The current result combines an
887-row flushed CSV prefix with an append-only 1,483-row service log. Validation found exactly
2,370 unique case keys, zero overlap, zero malformed records, zero missing corpus keys, and zero
extras. [`outcomes.csv`](outcomes.csv) is the normalized paired per-case outcome and timing table.

## Regression found and fixed

The new final JSONL summary initially serialized all 89 existing `max_vars` refusals as
`error:other_error`, even though stderr and solver behavior were unchanged. The run summary now
recognizes the stable `pass a larger --max-vars` diagnostic and emits
`status: refused, reason: max_vars`. A dedicated JSONL regression test and the real
`mqt-big/qnn_indep_qiskit_100` reproducer both confirm the corrected classification.
