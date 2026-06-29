# dlx4sop Optimization Sprint

## Context

The sign-edge-only refactor makes the internal QSOP invariant simple:

```text
P(x) = c + sum_v a_v x_v + (r/2) sum_{uv in E} x_u x_v mod r
```

Unary labels remain arbitrary residues, but every quadratic term is an implicit sign edge.
This removes the pre-refactor mode split and lets the solver, WMC exporter, and benchmark
tooling specialize for one representation.

The newer WMC/rank-width note adds a concrete algorithmic target: Fourier-mode
rank-width joins can be accelerated by dense algebraic kernels. The matrix-multiplication
result is not a generic `2^k` black-box improvement. It says the current direct twisted
join has a real matrix-multiplication barrier, but dense Fourier joins can improve the
crossover when rank-width grows.

## Optimization Targets

### 1. Make Fourier Rank-Width Joins Streaming First

Current Fourier rank-width joins materialize a full join map for `left->len * right->len`
signature pairs in `src/solve/rankwidth.c`. This can consume large memory before the
actual join and is the same class of failure as the 65-128 rankwidth OOM rows observed
during scoreboard refresh.

Implement a streaming or CSR Fourier transition path analogous to the sign-edge
count-table CSR path:

- compute parent signature and residue shift on demand or into compact transition rows;
- avoid storing representative assignments for every pair;
- preserve trace counters for `rankwidth.fourier_join_map` and `rankwidth.fourier_join`;
- emit `status: error` rows rather than aborting benchmark batches when a solver refuses
  or runs out of memory.

This is the lowest-risk optimization because it keeps the same arithmetic and semantics.

### 2. Add a Dense Fourier Join Mode

Add a new experimental solve mode, e.g. `rankwidth:fourier-dense`, behind an explicit
CLI flag or `--rankwidth-mode`.

Implementation phases:

1. Build explicit GF(2) bases for left, right, and parent signature spaces.
2. Pack signatures into dense coordinates `0..2^k-1`.
3. Keep the existing pairwise Fourier join as the reference path.
4. For even Fourier modes, use XOR convolution over the parent signature space.
5. For odd Fourier modes, implement a dense blocked twisted-join kernel.
6. Add a validator mode that compares dense and reference joins on small instances.

The even-mode path is the first actionable matrix-adjacent target: the bilinear twist
vanishes, so Walsh-Hadamard convolution should be much simpler than the full
Bravyi-Fattal-Gottesman-style normal form.

### 3. Make SIMD Useful After Layout Changes

SIMD is likely useful only after dense data layout exists.

Current sparse tables are entry-oriented:

```text
(signature, residue, count)
```

Dense Fourier kernels should use contiguous arrays:

```text
values[mode][dense_signature]
```

or blocked variants that keep each vector lane on adjacent modular-prime values.

SIMD candidates:

- modular add/mul in blocked dense Fourier joins;
- FWHT butterfly kernels for even Fourier modes;
- popcount/parity helpers for sign-edge transition construction;
- treewidth Fourier factor multiplication when scopes are dense and projection maps are hot.

Do not start with SIMD intrinsics in the current sparse join loops. The memory access
pattern is the limiting factor there.

### 4. Tighten Forecasts and Dispatch

The paper's useful crossover heuristics are:

- direct rank-width is promising roughly when `2 * rw <= tw`;
- dense Fourier improves the target toward `(omegaMM / 2) * rw <= tw`;
- broad corpora where `rw` and `tw` are close should not be expected to favor rank-width.

Add benchmark-visible diagnostics:

- `rankwidth_cutrank_width`;
- `treewidth_width` or table forecast for the same row;
- direct join-pair forecast;
- dense Fourier forecast;
- selected rankwidth kernel;
- solver refusal reason.

Use these to make branch handoff and scoreboard summaries explain why rankwidth was or
was not attempted.

## Experiment Plan

### RQ1: Corpus Coverage

Keep rejected imports visible. The sign-edge-only importer should reject unsupported
non-sign quadratic phases rather than approximate them.

Metrics:

- attempted boundaries;
- imported sign-edge QSOPs;
- skipped import records;
- source/tier coverage after boundary pinning and reduction.

### RQ2: Native SOP Backends and Rank-Width Fit

Add a targeted bounded-rank-width synthetic corpus based on the paper's Corollary 3.

Parameters:

- tree height `h`;
- clique size or blow-up parameter `t`;
- fixed finite modulus, preferably `r = 8` initially;
- all-zero fixed-boundary amplitudes where applicable.

Record:

- `nvars`, `nedges`;
- cutrank width;
- treewidth forecast;
- rankwidth table and join forecasts;
- backend status/time for treewidth, branch, rankwidth count-table, rankwidth Fourier,
  and rankwidth Fourier dense.

Goal:

Test whether rankwidth becomes the practical winner on instances designed to isolate the
low-rank regime, not just on broad imported corpora.

### RQ3: External Baselines

Keep native simulator comparisons as sanity checks, not a single ranking.

For new targeted rankwidth families, external baselines may be less meaningful than
internal exact comparisons. Still record:

- unsupported native rows;
- memory/time caps;
- exact agreement where native tools can run.

### RQ4: WMC Scaling

Continue treating WMC as complementary evidence.

Near-term WMC experiments:

- compare `amp-soft` and optimized `amp-block` on sign-edge block-heavy instances;
- record Ganak `ok`, `timeout`, and `error` statuses explicitly;
- add block-structure summaries: number of detected complete bipartite sign blocks,
  auxiliary variables saved, clauses saved, and Ganak time.

The current conclusion to test is conditional: WMC outlives branch on some hard synthetic
regimes, but current Ganak encodings do not beat native treewidth on solved frontier or
time-to-solution.

## Suggested Sprint Order

1. Done: finish current scoreboard refresh and keep the generated status/error rows.
2. Done: preserve solver status/error rows in the scoreboard data path so OOM/refusal rows
   remain visible.
3. Done: implement streaming Fourier rankwidth joins.
4. Done: add the bounded-rank-width synthetic corpus generator and manifest.
5. Done: add a targeted rankwidth crossover benchmark path.
6. Later: implement dense even-mode Fourier joins with FWHT.
7. Later: add dense odd-mode blocked twisted joins.
8. Later: add SIMD only after dense layout and scalar dense kernels are correct.

## Sprint Log

### 2026-06-29: Streaming Fourier Join

Implemented the first kernel optimization in `src/solve/rankwidth.c`:

- Fourier rankwidth joins no longer allocate a full `rw_join_map_t` for
  `left->len * right->len` signature pairs.
- Parent signatures and sign-edge residue shifts are computed on demand with the same
  transition helper used by the count-table sign-edge path.
- The parent Fourier table now has an open-addressing signature index, replacing the
  old linear scan in `fourier_table_signature_index`.
- Existing trace labels are preserved: `rankwidth.fourier_join_map` now records the
  pair forecast, and `rankwidth.fourier_join` records the streamed join execution.

Validation:

- `ninja -C build`
- `python3 tests/test_sop_solve.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_bench_qasm_corpus.py tools/bench_qasm_corpus.py`
- `meson test -C build --print-errorlogs`

### 2026-06-29: Bounded-Rankwidth Experiment Path

Added the Corollary 3-style synthetic family:

- New generator: `tools/gen_rankwidth_family.py`.
- New committed corpus: `benchmarks/corpus/sop/synthetic/rankwidth/`.
- Corpus shape: complete binary tree vertices blown up to twin cliques, with complete
  bipartite sign edges across tree edges.
- Manifest: `benchmarks/corpus/sop/synthetic/rankwidth/manifest.jsonl`.
- Benchmark path: `tools/run_corpus_benchmarks.py` now emits
  `rankwidth-separation-current.jsonl`.
- Scoreboard path: `tools/refresh_scoreboard.py` now renders a text summary when that
  artifact exists.

Initial smoke signal with a 5s cap:

- `btclique-h01-t16-r8-all-t` timed out in treewidth but solved in both rankwidth modes.
- All 7 committed rows solved in `rankwidth:best` and `rankwidth:best:fourier`.
- The rankwidth summaries stayed at cutrank width 1 with max table 16 on the smoke run.

Validation:

- `python3 tests/test_gen_rankwidth_family.py tools/gen_rankwidth_family.py`
- `meson test -C build 'rankwidth family generator smoke' 'rankwidth family benchmark smoke' --print-errorlogs`
- `python3 tools/run_corpus_benchmarks.py --skip-solver --skip-wmc --skip-native --skip-scaling --skip-scoreboard --artifact-dir /tmp/dlx4sop-rw-study-smoke --sop-solve build/sop-solve --rankwidth-study-timeout 5`

### 2026-06-29: Sign-Edge Fourier Twist Specialization

Optimized the streaming Fourier join for sign-edge QSOPs:

- Removed root-power lookup and a modular multiply from the Fourier join twist.
- The sign-edge quadratic phase contributes `+1` on even Fourier modes and a sign flip
  on odd modes when the cross parity is one.
- This is the scalar specialization that should precede SIMD: the mode loop now has
  simpler contiguous arithmetic and no root-power table dependency.

Added benchmark-visible dense Fourier diagnostics:

- `rankwidth_dense_table_forecast`
- `rankwidth_dense_even_join_forecast`

These fields estimate the dense signature slots and even-mode FWHT butterfly work for
the observed cutrank width. They are emitted by `sop-solve --format stats`, promoted by
the benchmark runners, and shown in scoreboard row details.

Validation:

- `ninja -C build`
- `python3 tests/test_sop_solve.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_bench_qasm_corpus.py tools/bench_qasm_corpus.py`

### 2026-06-29: Verdict/Amplitude Cross-Checks and Scalar Hot-Path Tightening

Added solver and benchmark cross-check coverage before continuing optimization:

- New Meson test: `rankwidth family crosscheck smoke`.
- The test materializes a tiny bounded-rankwidth corpus on the fly.
- It checks exact residue-count agreement across treewidth, rankwidth count-table, and
  rankwidth Fourier.
- It computes amplitudes from the counts and checks `result_probability = |amplitude|^2`.
- It runs `tools/bench_sop_local.py` over the same corpus and checks that JSONL rows have
  matching `counts_hash`, `amplitude_real`, and `amplitude_imag`.
- `tools/bench_sop_local.py` now promotes amplitude coordinates, matching the QASM benchmark
  runner and scoreboard comparison path.

Continued scalar rankwidth optimization:

- `cross_parity_bitsets()` now iterates set bits from the smaller side of the join instead
  of scanning every variable in the instance.
- The Fourier join accumulation loop is split into twist-free even modes and sign-flipped
  odd modes, keeping the even-mode path closer to the intended dense/FWHT kernel.

Validation:

- `python3 tests/test_rankwidth_family_crosscheck.py tools/gen_rankwidth_family.py tools/bench_sop_local.py build/sop-solve`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build --print-errorlogs`

### 2026-06-29: Cached Rankwidth Representative Weights

Continued the parity hot-path cleanup:

- Count-table rankwidth reps now cache assignment popcounts when each representative is
  inserted.
- Fourier rankwidth tables cache the same weight alongside each representative assignment.
- Sign-edge join transition code uses the cached weights to iterate the smaller assignment
  side when computing cross parity, avoiding two full popcount scans per transition pair.
- Materialized, CSR, streaming, and Fourier rankwidth join paths all use the weighted
  parity helper.

Validation:

- `ninja -C build`
- `python3 tests/test_rankwidth_family_crosscheck.py tools/gen_rankwidth_family.py tools/bench_sop_local.py build/sop-solve`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build --print-errorlogs`

### 2026-06-29: O(words) Outside-Bitset Construction

Removed another rankwidth join setup cost:

- Added a local helper to fill the all-variables bitset with a masked tail word.
- Replaced per-variable `qsop_bitset_set(outside, v)` loops in count-table, CRT map
  building, and Fourier streaming joins.
- Join setup now builds `outside = all_vars & ~node_vars` in O(bitset words) instead of
  O(number of variables).

Validation:

- `ninja -C build`
- `python3 tests/test_rankwidth_family_crosscheck.py tools/gen_rankwidth_family.py tools/bench_sop_local.py build/sop-solve`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build --print-errorlogs`

### 2026-06-29: Fourier Root Signature Lookup

Small cleanup after the Fourier table indexing change:

- `fourier_table_find_signature()` now probes the maintained open-addressing signature
  index instead of linearly scanning the root table.

Validation:

- `ninja -C build`
- `python3 tests/test_rankwidth_family_crosscheck.py tools/gen_rankwidth_family.py tools/bench_sop_local.py build/sop-solve`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve`
- `meson test -C build --print-errorlogs`
