# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). The project goal is a competitive exact strong simulator using
QSOPs with fixed-boundary circuit amplitudes.


**Benchmarks:** dlx4sop's branch/treewidth/rankwidth solver backends and
the `sop2wmc` + Ganak weighted-model-counting pipeline (all described below)
are ranked on the
public [qccq-gauntlet leaderboard](https://qccq-cgd.pages.dev/).

## Tools

Prebuilt releases include the core command-line tools as `linux-x86_64` and
`macos-arm64` tarballs with SHA-256 sidecar files. The same tools are built
from source:

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics, with opt-in exact small-width
  support-graph diagnostics.
- `sop-solve`: solve exact residue-count histograms; stats mode can include
  the count vector used to reconstruct amplitudes and a convenience probability
  estimate via `--include-probability`.
- `qasm2sop`: import the supported static OpenQASM 2.0 subset into QSOP,
  including common Clifford/T gates, supported phase rotations, `u/u2/u3`,
  controlled phase/H/SX gates, `dcx`, `rxx/ryy/rzz`, `ccz/ccx/rccx/cswap`,
  and `iswap`.
- `sop2wmc`: export a QSOP to DIMACS CNF / WPCNF for external model counting.
  Five encodings are available via `--encoding <name>`:
  - `residue-accumulator` (alias `residue`, default): one DIMACS CNF per
    residue 0..r−1; plain #SAT each. Works with any integer counter (Ganak
    `--mode 0`, d4, sharpSAT). Requires r calls per instance.
  - `amp-and` (alias `amplitude`): single WPCNF with Tseitin AND auxiliaries
    carrying hard complex weights ω^b. All auxiliaries are circuit-determined;
    use `ganak --mode 6 --verb 0`. Multiply the raw WMC result by
    ω^constant (from the `c amplitude_factor` metadata line) to get the full
    amplitude.
  - `amp-soft`: single WPCNF with implication-only auxiliaries and soft weights
    ω^b − 1. Produces fewer clauses per edge than amp-and; Ganak integrates
    over underdetermined variables.
  - `residue-fourier`: r WPCNF blocks (one per Fourier exponent t) followed by
    an outer iDFT. Inner encoding selectable via
    `--wmc-fourier-inner (amp-and|amp-soft)`.
  - `amp-block`: single WPCNF; detects a uniform complete bipartite subgraph
    A×B in the edge set and encodes it with a mod-r adder counter per side plus
    Tseitin selector variables with hard weights ω^(c·a·b mod r). Non-block
    edges use amp-and. Block triggers when savings ≥ `--wmc-block-min-savings`
    and both sides ≥ `--wmc-block-min-side` (defaults 0 and 4); falls back to
    amp-soft output when no profitable block is found.
- `scripts/build_external_qasm_manifest.py` / `scripts/bench_wmc_ganak.py`:
  shared OpenQASM-munging and Ganak-output-parsing helpers imported directly
  by the `.gauntlet/` adapters that back the qccq-gauntlet registration; not
  standalone CLIs.

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External frameworks stay at corpus import and validation boundaries.

## Solver Guide

- `branch --solve-mode auto`: default recommended solver. It tries exact
  residue counting first and falls back to single-Fourier amplitude evaluation
  on safe exact-count refusals.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  developer/profiling runs.
- `rankwidth`: decomposition-DP backend with cut-rank diagnostics and
  count-table/Fourier modes; useful for comparison and targeted low-rank cases.

## QSOP Format

```text
p qsop-sign <r> <variables> <sign_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
f <vertex> <0 | 1>
```

Quadratic terms are sign edges with implicit coefficient `r/2`;
duplicate sign edges cancel by parity. Pins (`f`) are applied during parsing,
and canonical output uses dense variable IDs. Solver
`counts` are ordinary assignment counts bucketed by phase residue modulo `r`.

## Examples

```sh
build/sop-check tests/golden/sign_raw.qsop
build/sop-stats --format json tests/golden/sign_expected.qsop
build/sop-stats --exact-widths --exact-width-max-vars 12 tests/golden/solve_sign_path.qsop
build/sop-solve tests/golden/solve_disconnected.qsop
build/sop-solve --format residue-vector tests/golden/solve_disconnected.qsop
build/sop-solve --format stats --include-result tests/golden/solve_single.qsop
build/sop-solve --format stats --backend treewidth --solve-mode fourier tests/golden/solve_disconnected.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm | build/sop-solve --format stats --include-probability -
build/sop2wmc --encoding auto --stats-only tests/golden/solve_disconnected.qsop
build/sop2wmc --residue all tests/golden/solve_disconnected.qsop
build/sop2wmc --residue 2 -o residue2.cnf tests/golden/solve_disconnected.qsop && ganak residue2.cnf
build/sop2wmc --encoding auto tests/golden/solve_disconnected.qsop | ganak --mode 6 --verb 0 -
```

### Approximate QASM imports

`qasm2sop` is exact by default: circuits with phases outside the supported
finite grid are rejected. Use `--approx X` to opt in to phase rounding, where
`X` is a positive additive amplitude error budget. Scientific notation is
accepted, for example:

```sh
build/qasm2sop --approx 1e-6 --input 0 --output 0 circuit.qasm
```

Approximate output includes comment lines recording the chosen modulus, rounded
phase count, and certified additive amplitude error bound.

The WMC export reconstructs `raw_amplitude = sum_k counts[k] * exp(2*pi*i*k/r)` and
`probability = |raw_amplitude|^2 * 2^(-norm_h)` (the same convention as
`sop-solve --include-probability`) outside the counter; the metadata header in
each CNF block documents the variable map and the final accumulator bits.

## Amplitudes are normalized

`sop-solve --format amplitude` reports the **normalized** amplitude
`raw_amplitude * 2^(-norm_h/2)` -- the physical `<y|C|x>`, whose modulus is at most 1
-- alongside the `norm_h` needed to recover the raw value. The raw sum-over-paths
amplitude grows like `2^nvars`: qccq-gauntlet's `qwalk-noancilla_11` has
`|raw_amplitude|` around `2^29670`, so no fixed-exponent floating type can hold it, and
a naive `float(raw) * 2**(-norm_h/2)` silently yields `nan`. Internally the solvers
carry a mantissa and a separate binary exponent (`qsop_amplitude_t.scale_exp2`), which
also lets the branch backend multiply per-component amplitudes without any of them
overflowing on its own. Scaling by a power of two is exact, so nothing is lost.


## Build

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The default `build` directory is a debug build (`-O0`, assertions on), which is
appropriate for development and the test suite.

CI enforces at least 75% line coverage over production `src` files:

```sh
scripts/check-coverage.sh build-coverage
```

## Benchmarks

Comparative benchmarking can be found in
[qccq-gauntlet](https://qccq-cgd.pages.dev/), an external harness that runs
the dlx4sop backends and the `sop2wmc` + Ganak pipeline against shared
datasets/suites on a public leaderboard. `.gauntlet/adapter.py` and
`.gauntlet/adapter_wmc.py` are the two integration points gauntlet drives:
they import a circuit, run it through `qasm2sop`/`sop-solve` (or
`sop2wmc` + Ganak), and report back in the protocol gauntlet expects. Both
adapters import `build_external_qasm_manifest.py` / `bench_wmc_ganak.py`
from `scripts/` for OpenQASM munging and Ganak-output parsing.

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
4. **Per-component refusal** — a non-delegatable component larger than
   `--max-vars` is refused (rather than branched into an exponential recursion).
5. **Branching fallback** — otherwise pick a branch variable and recurse on
   `0`/`1`.

**Delegation caps** (hard limits; a component wider than these is not handed to
that backend): treewidth ≤ 14 (count-table) or ≤ 25 (single-Fourier); a
single-component root up to width 18 for ≤ 2500 variables; rankwidth cut-rank
≤ 12.

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

rankwidth  ⇔  rw_est * rw_min_speedup < tw_est
```

The first evaluation, with a *predicted* rankwidth cost (cut-rank proxy for the table
and signature counts, no join term, plus the probe) decides whether probing is worth
it at all. The second, with the measured decomposition and the probe now sunk,
decides which backend runs. There are no other vetoes.

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
