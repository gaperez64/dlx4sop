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
5. For odd Fourier modes, implement the normal-form twisted-join kernel described in the
   note. Do not substitute an ad hoc blocked pair loop for this step.
6. Add a validator mode that compares dense and reference joins on small instances.

The even-mode path is the first actionable dense target for full residue histograms: the
bilinear twist vanishes, so Walsh-Hadamard convolution should be much simpler than the
full Bravyi-Fattal-Gottesman-style normal form. Single-amplitude acceleration still
requires the odd twisted path, so benchmarks must not claim the dense theorem until that
kernel exists.

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

Backend optimization comes first. Scoreboard refresh and publication experiments wait until
the native rankwidth solver and WMC/Ganak backend are in the best currently practical
shape.

1. Done: finish current scoreboard refresh and keep the generated status/error rows.
2. Done: preserve solver status/error rows in the scoreboard data path so OOM/refusal rows
   remain visible.
3. Done: implement streaming Fourier rankwidth joins.
4. Done: add the bounded-rank-width synthetic corpus generator and manifest.
5. Done: add a targeted rankwidth crossover benchmark path.
6. Now: add rankwidth Fourier kernel selection and keep unsupported dense kernels as
   explicit refusals until the scalar dense implementation is reference-checked.
7. Now: optimize the WMC/Ganak backend for signed QSOPs, especially single Fourier-mode
   export and block-heavy sign-edge instances.
8. Done/retired: dense even-mode FWHT is no longer the signed-only first target; even
   Fourier modes factor in closed form, and edge-free count-table rows now factor too.
9. Next: implement odd twisted dense joins only through the normal-form route described in
   the note; do not add an ad hoc blocked odd join and call it the matrix kernel.
10. Later: add SIMD only after dense layout and scalar dense kernels are correct.

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

### 2026-06-29: Generator-Overhead Experiment Split

Release-build timing on the bounded-rankwidth family showed that the `best` rankwidth
generator can be slower than `from-treewidth` even when it finds width 1 instead of
width 2. The separation study now records both generator families:

- `rankwidth:from-treewidth`
- `rankwidth:best`
- `rankwidth:from-treewidth:fourier`
- `rankwidth:best:fourier`

This keeps the scoreboard from collapsing two different effects: decomposition quality
and generator/scoring overhead.

Release smoke with `build-bench/sop-solve` before the driver change:

- `rankwidth:from-treewidth`: 7/7 ok, geomean 0.795 ms, max width 2.
- `rankwidth:best`: 7/7 ok, geomean 1.996 ms, max width 1.
- `rankwidth:from-treewidth:fourier`: 7/7 ok, geomean 0.721 ms, max width 2.
- `rankwidth:best:fourier`: 7/7 ok, geomean 1.998 ms, max width 1.

### 2026-06-29: Backend-First WMC and Fourier-Kernel Plumbing

Implemented the first backend-first tranche after the PDF v4 alignment:

- Added `--rankwidth-fourier-kernel auto|streaming|hybrid-even-fwht|dense-reference`.
- `auto` currently resolves to the proven streaming Fourier kernel.
- Dense selections refuse clearly with `not implemented` rather than producing misleading
  benchmark rows.
- Rankwidth Fourier leaves now reuse per-solve scratch buffers instead of allocating three
  bitsets per leaf.
- `sop-solve --format stats` reports the selected Fourier kernel on rankwidth Fourier rows.
- Added `--wmc-fourier-mode all|T`; `tools/bench_wmc_ganak.py` now runs residue-fourier
  amplitude rows with mode `1` only.
- Implemented `peel2-safe` degree-2 factor-graph elimination with nonzero-denominator and
  fill-budget checks.
- Reworked `amp-block` to run after WMC preprocessing, extract multiple edge-disjoint
  complete bipartite sign blocks with bitsets, and emit residual pair factors via the
  amp-soft encoding.
- Extended WMC stats with block/preprocess counters.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_bench_qasm_corpus.py tools/bench_qasm_corpus.py`
- `meson test -C build 'wmc unit' --print-errorlogs`

### 2026-06-29: Optimized WMC Runner Defaults

Wired the backend WMC optimization controls into the refresh orchestration:

- `tools/run_corpus_benchmarks.py` now passes `--wmc-preprocess peel2-safe` by default
  to amplitude-style WMC jobs.
- `amp-block` rows use the actionable sign-block threshold defaults
  `--wmc-block-min-side 2 --wmc-block-min-savings 1`.
- Plain residue #SAT rows remain small-tier baselines; amplitude, amp-soft,
  amp-block, and residue-fourier use the optimized factor-graph path.
- The synthetic WMC-vs-solver scaling study now runs optimized `amp-block` instead of
  stale `amp-soft`, so the crossover evidence targets the best current WMC backend.
- `tools/bench.py full` and `tools/bench.py ganak` forward the same knobs, while
  `--wmc-preprocess none` remains available for ablations.

Validation:

- `python3 tests/test_wmc_runner_options.py tools/run_corpus_benchmarks.py tools/bench.py`

### 2026-06-29: Amp-Block Extractor Hot-Path Tightening

Tightened the signed WMC block extractor without changing its greedy semantics:

- Candidate `B` sides are now represented as bitsets and counted with popcount.
- Candidate `A` membership uses word-wise subset checks instead of looping over every
  vertex in `B`.
- Allocation failures in block search now propagate as errors instead of being
  indistinguishable from "no profitable block found" and silently falling back to
  amp-soft.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' --print-errorlogs`

### 2026-06-29: Rankwidth Fourier Even-Mode Closed Form

Exploited the signed-QSOP invariant before attempting dense/FWHT joins:

- Even Fourier modes ignore every sign edge because `t * (r/2) = 0 mod r` for even `t`.
- Rankwidth Fourier leaves and joins now compute only odd modes through the streaming
  boundary-signature DP.
- Fourier rankwidth DP tables now store only `r/2` odd-mode slots per signature; the full
  `r`-mode vector is materialized only at the root for the inverse DFT.
- The root table fills even modes with the closed form
  `prod_v (1 + omega^(t * a_v))`; the constant shift remains handled by the inverse DFT.
- Added a `rankwidth.fourier_even_closed_form` trace phase.
- `join_pairs` on rankwidth Fourier rows now reflects actual odd-mode pair work
  (`join_signature_pairs * r/2`) instead of charging the skipped even modes.
- On the signed 4-cycle smoke instance, Fourier table stats dropped from `table_entries=120`,
  `max_table_entries=32` to `table_entries=60`, `max_table_entries=16`.
- Edge-free signed QSOPs now bypass rankwidth Fourier DP entirely: every mode factors as
  `prod_v (1 + omega^(t * a_v))`, with trace phase `rankwidth.fourier_factorized`.

This is not the dense/FWHT kernel from the note; it is the smaller signed-QSOP
specialization that removes even-mode DP work while keeping the proven odd-mode streaming
join.

Validation:

- `ninja -C build`
- `python3 tests/test_sop_solve.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve`
- `python3 tests/test_rankwidth_family_crosscheck.py tools/gen_rankwidth_family.py tools/bench_sop_local.py build/sop-solve`
- `meson test -C build 'sop-solve golden' 'rankwidth join strategy smoke' 'differential backends' 'rankwidth family crosscheck smoke' --print-errorlogs`

### 2026-06-29: Edge-Free Rankwidth Count-Table Factorization

Applied the same signed-only factorization principle to count-table rankwidth rows:

- Edge-free signed QSOPs now bypass the rankwidth count-table decomposition traversal.
- The count-table shortcut computes the unary product
  `prod_v (1 + z^(a_v))` as an `O(n*r)` residue DP, then applies the constant shift.
- Exact rows with `nvars >= 64` use the existing CRT reconstruction path, so large
  edge-free rows no longer build per-node rankwidth transition maps.
- Rankwidth forecasts and diagnostics now report edge-free work honestly:
  `rankwidth_join_pair_forecast=0`, `rankwidth_table_forecast=r`, cutrank width `0`.
- The rankwidth entry point skips adjacency-bitset allocation for edge-free count-table
  and Fourier solves.
- Fourier edge-free stats now use the same no-edge stats helper, so both modes expose the
  same scoreboard-visible zero-join diagnostics.

Validation:

- `ninja -C build`
- `python3 tests/test_sop_solve.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_rankwidth_join_strategy.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'sop-solve golden' 'rankwidth join strategy smoke' 'differential backends' 'rankwidth family crosscheck smoke' --print-errorlogs`
- `meson test -C build --print-errorlogs`

### 2026-06-29: Amp-Block Greedy Search Reuse

Removed repeated graph rebuild work from the signed WMC `amp-block` extractor:

- The greedy complete-bipartite sign-block search now builds the active sign-edge
  adjacency bitset once per export.
- Covered block edges are removed from that adjacency in place before the next greedy
  pass.
- Search scratch arrays and bitsets are reused across passes instead of being allocated
  and freed in every `find_best_sign_block` call.
- The greedy scoring and edge-disjoint block semantics are unchanged.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`

### 2026-06-29: Peel2 Degree-Index Scan

Reduced a WMC preprocessing hot path while preserving the `peel2-safe` rule:

- `fg_peel2_once` now builds a one-pass degree-two index over active factor-graph pairs.
- Candidate selection still scans variables in order and eliminates the first valid
  degree-2 variable, so preprocessing semantics remain unchanged.
- The candidate scan drops from walking all pairs for every variable to one pair pass plus
  one variable pass per peel attempt.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`

### 2026-06-29: Direct Amp-Soft/Amp-And Emission

Removed temporary clause buffering from the two simple amplitude WMC encodings:

- `amp-soft` now counts active mapped pairs, assigns deterministic auxiliary ids, writes
  weights, then streams the two implication clauses per pair directly.
- `amp-and` does the same for deterministic Tseitin-AND auxiliaries and streams the two
  binary plus one ternary clause per pair directly.
- This avoids per-clause builder reallocations and the `pair_var` side arrays in the
  common WMC export modes; `amp-block` still uses the builder where parity XOR gates need
  generated intermediate literals.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`

### 2026-06-29: Amp-Block Residual Clause Streaming

Finished the same direct-emission cleanup for the residual soft edges inside `amp-block`:

- Parity-block XOR gates still use the builder because they create intermediate
  literals.
- Residual non-block soft edges are now counted, assigned deterministic auxiliary ids,
  weighted, and streamed directly.
- This removes the residual `extra_var`/`extra_pair` arrays and avoids buffering two
  implication clauses per residual edge in the builder.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`

### 2026-06-29: Amp-Block Marker Reuse

Removed one more allocator hot spot from multi-block `amp-block` extraction:

- Per-block `in_a`/`in_b` marker arrays are replaced with reusable `uint32_t` stamp arrays.
- This preserves the same edge-marking logic while avoiding two calloc/free pairs for
  every extracted sign block.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`

### 2026-06-29: Signed WMC Factor-Graph Phase Specialization

Removed avoidable phase work from the signed WMC factor-graph builder:

- Small finite moduli now precompute one Fourier-root table per exported mode and reuse it
  for the global and unary weights, avoiding a `cos/sin` call per variable on the common
  `r = 8` path.
- Odd Fourier-mode sign-edge factors are emitted as the exact constant `-1+0i` instead
  of recomputing `omega^(r/2)` for every edge.
- Even Fourier modes return before allocating pair-factor storage, since sign edges
  contribute multiplier `1` there.

The dense rankwidth item remains intentionally unsupported: the current note's improvement
requires building the `P/Q/B` vector-space join and applying the Bravyi-Fattal-Gottesman
normal-form path for odd modes. The signed-only solver has already removed even-mode DP
with a closed form, so enabling a dense flag without the odd normal-form kernel would be
misleading.

Validation:

- `ninja -C build`
- `python3 tests/test_sop2wmc.py build/sop2wmc build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `python3 tests/test_differential_backends.py build/sop-solve /home/gperez/GIT-repos/dlx4sop`
- `meson test -C build 'wmc unit' 'sop2wmc golden' 'wmc ganak benchmark metadata smoke' 'wmc runner options smoke' --print-errorlogs`
