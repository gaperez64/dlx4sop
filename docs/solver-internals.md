# Solver internals

This developer guide documents the branch backend and its cost model. Return to
the [dlx4sop README](../README.md) for installation and usage.

## Branch backend: what and why does it run?

The `branch` backend recursively branches on variables and delegates connected
components to the treewidth or rankwidth DP once a component is cheap enough. It
never solves a component itself if it can hand it off. The ordered decision, per
component (from first check to last):

1. **Base cases** — no variables (constant), or no edges (closed-form product).
2. **Component split** — a disconnected residual is split and each component is
   solved independently, then combined.
3. **Treewidth / rankwidth delegation** — a cost model (below) picks between the
   two DP backends when the component's treewidth or cut-rank is within the
   delegation caps.
4. **Small fallback or bounded cutset** — components at or below the 64-variable
   single-Fourier fallback cap use the existing exhaustive recursion. Larger
   components are refused unless the opt-in bounded cutset layer is enabled.
5. **Branching fallback** — pick a branch variable and recurse on `0`/`1`, with
   explicit node, depth, and stagnation limits in the cutset phase.

**Delegation caps** (hard memory-safety limits, since each backend's DP table is `2^width`-sized, so
a component over its cap is not handed to that backend): treewidth ≤ 14 (count-table); the
single-Fourier treewidth delegate is admitted by *DP work* — min-fill `Σ 2^bag ≤ 3.2e9` — under a
width ≤ 26 table-memory ceiling; a single-component root up to width 18 for ≤ 2500 variables;
rankwidth cut-rank ≤ 12. These caps are part of the cost model below, not separate vetoes layered
on top of it.

**Rankwidth is on by default** (`--branch-rw-source auto`): the branch backend
competes rankwidth against treewidth per component via the cost model below, and
delegates to rankwidth only when it wins. This costs almost nothing on the common
(treewidth-favorable) case — the whole-instance direct treewidth path is still
taken when treewidth is trivially cheap — while automatically catching the
dense-but-low-rank cases (e.g. complete-bipartite blocks) where cut-rank ≪
treewidth and rankwidth's `2^cut-rank` table crushes treewidth's `2^treewidth`.
Pass `--branch-rw-source none` to disable rankwidth entirely.

**Cost model** (skipped for `--branch-rw-source none`). One inequality, evaluated
twice:

```
tw_est = tw_fixed_overhead_ns + C_tw_table * tw_dp_work        (infinite if treewidth is over its cap)
rw_est = rw_fixed_overhead_ns + rw_memory_penalty_ns
       + C_rw_table*rw_table + C_rw_join*rw_join + C_rw_sig*rw_sig
       + rw_probe                                              (before the probe; 0 after)
                                                               (infinite if cut-rank is over its cap)

rankwidth  ⇔  rw_est * rw_min_speedup < tw_est
```

The first evaluation uses a deliberately best-case rankwidth cost: generated cut-rank
zero when the natural-order prefix cut-rank is zero, otherwise generated cut-rank one,
with no join term and with the probe cost included. Natural-order prefix cut-rank is
**not a lower bound** on the width of a generated decomposition; changing the order can
compress a large prefix rank to one. Its only safe use before generation is therefore to
distinguish rank zero from a potentially compressible graph. This optimistic evaluation
decides whether probing is worth it at all. The second evaluation uses the measured
decomposition forecasts with the probe now sunk and authoritatively decides which backend
runs. The only side conditions are the delegation caps above:
they enter the inequality as an infinite estimate — a backend over its cap simply
cannot run — so the single comparison stays the whole decision. (In `--branch-calibrate-backends`
mode the inequality is bypassed so both backends are timed, but the hard caps still apply.)

Two things make that work.

`tw_dp_work` is the **real** DP work — the sum over elimination steps of `2^(bag size)`,
which `qsop_min_fill_eliminate` accumulates as it goes. The old `nvars * 2^(width+1)`
bound assumes every bag is as wide as the widest; on circuit graphs it overstates by
275–605×, while being within 2× on the small dense graphs where rankwidth wins. That
asymmetry is exactly backwards for this decision — it inflated treewidth's cost where
rankwidth could not help, making an expensive probe look worthwhile.

`rw_probe` prices the **decision**, not the solve. Answering "would rankwidth win?"
generates a rank decomposition and measures the cut rank at each of its ~`2*nvars`
nodes: `O(nvars² · words)` of bitset work. On a 14k-variable, width-16 instance that
was over 100 s spent to improve on a 3 s treewidth solve, and nothing in the model
accounted for it.

This subsumed four hand-tuned pre-probe vetoes (`--branch-rw-min-treewidth-width`,
`--branch-rw-min-treewidth-forecast`, `--branch-rw-min-residual-vars`,
`--branch-rw-low-rank-bypass`), which have been **removed**: a treewidth estimate too
small to be worth probing against, and a cut rank small enough that rankwidth obviously
wins, both fall out of the inequality.

### Runtime tuning (`--help-advanced` lists all flags)

Backend / mode:
`--backend branch|treewidth|rankwidth` (default `branch`),
`--solve-mode auto|count-table|fourier|single-fourier`,
`--max-vars N` (default 24; auto single-Fourier raises it to 2^24),
`--treewidth-order min-fill|min-degree|min-fill-max-degree`.

`--max-vars` is a sanity bound, not a solvability gate: what makes a single-Fourier
solve affordable is the **width** (the DP table is `2^(width+1)`), not the variable
count. The auto default used to be 4096, which refused instances of width 14 for
being large.

Rankwidth policy (`--branch-rw-source` defaults to `auto`; set `none` to disable):
`--branch-rw-source`,
`--branch-rw-min-speedup` (1.1),
`--branch-rw-fixed-overhead-ns` (20000),
`--branch-tw-fixed-overhead-ns` (10000),
`--branch-rw-memory-penalty-ns` (0).

Single-Fourier fallback: `--branch-single-fourier-fallback`,
`--branch-single-max-fallback-vars`, `--branch-single-max-search-nodes`,
`--branch-single-cache-budget-mib`, `--branch-single-precision`,
`--single-mode-precision`, `--branch-single-propagate auto|off`.

`--branch-single-propagate` controls the search-time Hadamard collapse: at each
node the solver sums out every variable with `unary ∈ {0, r/2}` and active degree
≤ 1, cascading through the pins that creates. An isolated variable left with unary
`r/2` has factor `1 + ω^(r/2) = 0`, so the whole subtree's amplitude is zero and is
pruned. It is amplitude-exact (odd Fourier modes only), so it is confined to the
single-Fourier path and disabled automatically for an even `--fourier-target-mode`.

AUTO also preflights the count-table result vector. If that vector alone would
exceed 2 GiB, it routes the scalar request to single-Fourier before allocating
the vector. This is a memory check, not a delegation decision: the original
delegate-only width policy is retained for width-eligible graphs, while larger
QNN-style graphs keep the normal residual fallback and its stable refusal.

### Opt-in conditioning and materialized reduction

The advanced options below are deliberately disabled by default:

```text
--branch-single-materialized-reduction
--branch-single-diagnose-conditioning
--branch-single-cutset-depth N
--branch-single-lookahead-candidates N          (pilot default 8)
--branch-single-max-conditioning-nodes N        (pilot default 4096)
--branch-single-delegate-reprobe-interval N     (pilot default 2)
--branch-single-max-stagnant-levels N           (pilot default 1)
```

Materialized reduction rebuilds the active residual with its constant and an
artificial `norm_h = 2 * active_vars`, then calls the root Hadamard simplifier.
The artificial normalization is used only to count eliminated Hadamards; each
child's raw amplitude is multiplied by the corresponding power of two. It runs
at the root and after a dirty branch/pin, only for odd Fourier target modes.
Large residuals are never inserted into the amplitude cache; caching resumes
once a component reaches the ordinary 64-variable fallback.

The cutset layer makes an O(n+m) deterministic shortlist, performs exact
two-sided materialized lookahead, retains the two already-reduced children of
the best candidate, and scores the worst child lexicographically. Delegate
probes are repeated after a split, 10% shrinkage, the configured interval, or
when a component reaches 128/64 variables; skipped and performed probes are
counted separately. Exhausting the depth, node, or stagnation budget produces
`refused:cutset_budget`, never an unbounded search.

The July 2026 hard-corpus ablation left both features off by default. Depth-4
cutset conditioning solved only one of 64 policy refusals, below the five-case
enablement gate, and materialized conditioning introduced one 120-second
timeout. Materialized rebuilding consumed far below 20% of the newly solved
case, so rollback edge rewiring was not implemented.

Rankwidth backend: `--rankwidth-generate`, `--rankwidth-mode`,
`--rankwidth-memory-budget-mib`, `--rankwidth-memory-policy`,
`--rankwidth-join-strategy`, `--rankwidth-single-kernel`,
`--rankwidth-fourier-kernel`.

The five cost-model coefficients `C_rw_table` / `C_rw_join` / `C_rw_sig` /
`C_tw_table` / `C_rw_probe` have **no CLI flag**; they are ns-per-unit constants
(`BRANCH_POLICY_DEFAULT_C_*` in `src/solve/branch.c`) and are changed by editing that
file. `sop-solve` reads no environment variables.

### Retuning the cost model

The coefficients above are fit from measured backend timings. To collect data,
run with the calibration harness (requires the JSONL sink, branch backend, not
single-Fourier):

```sh
build/sop-solve --backend branch --branch-calibrate-backends \
  --stats-jsonl calib.jsonl <instance.qsop>
```

With calibration on, the losing backend is executed too, so each per-residual
JSONL record carries both the *predicted* forecasts (`treewidth_forecast_*`,
`rankwidth_forecast_*`) and the *measured* wall time
(`treewidth_actual_ms`, `rankwidth_actual_ms`, `treewidth_probe_ms`,
`rankwidth_generation_ms`). Fit the ns-per-table-entry / ns-per-join ratios and
overheads from those, then feed the overhead/threshold/speedup back via the
`--branch-rw-*` / `--branch-tw-*` flags and the `C_*` constants back into
`branch.c`.

### Observing a run

`--format stats` prints what the backend did:
`treewidth_delegations` / `rankwidth_delegations` / `branch_fallthroughs`
(where each residual went), `branch_treewidth_skips` / `branch_rankwidth_skips`
(vetoes), `decomposition_width` vs `rankwidth_cutrank_width` (the competing
widths), and `table_entries` / `join_pairs` vs their `*_forecast` (forecast
accuracy). `--trace csv` writes a `phase,depth,items,elapsed_ns` row per phase
to stderr (e.g. `branch.width_probe`, `branch.treewidth_delegate`,
`rankwidth.width_probe`) to localize time. Per-decision `veto_reason` strings
are only emitted via `--stats-jsonl`.

Every solve with `--stats-jsonl` ends with exactly one
`sop_solve_run_stats_v1` record on success or failure. Its stable `status` and
`reason` fields distinguish `max_fallback_vars`, `no_delegate`,
`cutset_budget`, search/depth budgets, and generic `other_error`; it also
preserves failure residual sizes and all partial counters. Human-readable
diagnostics remain supplemental and are not parsed to classify policy exits.
`--branch-single-diagnose-conditioning` additionally emits
`sop_solve_conditioning_v1` records for both values of every shortlisted
candidate without recursively solving them.

## f64 single-Fourier treewidth storage

The f64 path stores each factor in the elimination bucket of its largest scope
variable. Reverse-elimination relabeling guarantees that the next eliminated
variable is that largest ID, so a step visits only its bucket rather than
scanning the global factor list. Factors are still combined by the existing
pairwise numeric kernel and in insertion order, preserving floating-point
behavior.

Summing out the last scope bit is in place: the upper half is accumulated into
the lower half and the factor shrinks logically. `allocation_arity` retains the
original buffer size for pooling. The pool retains at most 512 MiB in total and
rejects any single buffer larger than 256 MiB; active and retained bytes are
transferred rather than double-counted. JSONL exposes bucket visits,
multiplications, allocations, discovery/join/sum-out time, peak live bytes,
peak retained bytes, and largest allocation.

The qwalk-10/11 factor-scope scan fell from 518 million/2.11 billion tests to
zero. qwalk-12/13 still exceed 120 seconds; a focused trace attributes the
remaining time to min-fill order generation rather than factor discovery or
numeric joins.

## Reproducing the hard-instance measurements

The checked-in corpus and manifest live in `benchmarks/hard-qsop`. Recreate it
through the same QPY-to-QASM-to-QSOP route as the gauntlet adapter:

```sh
python3 scripts/freeze_gauntlet_qsop.py \
  benchmarks/hard-qsop/selection.tsv \
  --gauntlet-root ../qccq-gauntlet \
  --out benchmarks/hard-qsop --build build-rel --timeout 120
```

Run solver-only comparisons with fixed inputs, a 120-second timeout, and the
12 GiB address-space limit:

```sh
python3 scripts/bench_frozen_qsop.py benchmarks/hard-qsop/inputs \
  --build build-rel --timeout 120 --mem-max-gib 12 \
  -o hard-instances.csv
```

The archived commands, environment, acceptance-gate calculations, and CSV
index are in
`benchmarks/results/hard-instances-2026-07-13/REPORT.md`.
