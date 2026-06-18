# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of
powers (QSOPs). The project goal is a competitive exact strong simulator using
labelled quadratic SOPs with fixed-boundary circuit amplitudes as the current
benchmark contract.

Current corpus coverage, solver timings, and native simulator comparisons live
in [scoreboard.md](scoreboard.md).

## Build

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

CI enforces at least 75% line coverage over production `src` files:

```sh
tools/check-coverage.sh build-coverage
```

Release binaries are built by `.github/workflows/release-binaries.yml` when a
`v*` tag is pushed, or manually through GitHub Actions. The workflow compiles
and packages `qasm2sop`, `sop-check`, `sop-solve`, `sop-stats`, and `sop2wmc`
as `linux-x86_64` and `macos-arm64` tarballs with SHA-256 sidecar files.

## Tools

- `sop-check`: parse, validate, pin-reduce, and canonicalize QSOP files.
- `sop-stats`: print structural statistics, with opt-in exact small-width
  support-graph diagnostics.
- `sop-solve`: solve exact residue-count histograms; stats mode can include
  the count vector used by benchmark tooling to reconstruct amplitudes and a
  convenience probability estimate via `--include-probability`.
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
- `tools/*.py`: benchmark runners, corpus scanners, and boundary translators.
  `tools/bench_wmc_ganak.py` drives `sop2wmc` + Ganak and cross-checks results
  against `sop-solve`; currently supports `--encoding (residue|amplitude)`.

The C core has no runtime dependency on Qiskit, PyZX, MQT, or FeynmanDD.
External frameworks stay at the benchmark/import boundary.

## Solver Guide

- `components`: default robust exact solver.
- `treewidth --treewidth-order min-fill-max-degree`: direct DP baseline for
  widened corpus tiers.
- `branch --branch-heuristic split`: current hybrid backend; it splits
  components and dispatches eligible residuals to treewidth or rankwidth.
- `rankwidth`: exact decomposition-DP backend with labelled cut-signature
  diagnostics and count-table/Fourier modes; useful for comparison and targeted
  low-rank cases, but not the default corpus winner.
- `brute-force`: small-instance oracle.

## QSOP Format

```text
p qsop <r> <variables> <quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0 | 1>
```

`e u v` is shorthand for a sign edge with coefficient `r/2`. Pins (`f`) are
applied during parsing, and canonical output uses dense variable IDs. Solver
`counts` are ordinary assignment counts bucketed by phase residue modulo `r`.

## Examples

```sh
build/sop-check tests/golden/labelled_raw.qsop
build/sop-stats --format json tests/golden/labelled_expected.qsop
build/sop-stats --exact-widths --exact-width-max-vars 12 tests/golden/solve_sign_path.qsop
build/sop-solve --backend treewidth --treewidth-order min-fill-max-degree tests/golden/solve_labelled.qsop
build/sop-solve --format stats --include-result tests/golden/solve_labelled.qsop
build/sop-solve --format stats --backend treewidth --solve-mode fourier tests/golden/solve_labelled.qsop
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm | build/sop-solve --format stats --include-probability -
build/sop2wmc --residue all tests/golden/solve_labelled.qsop
build/sop2wmc --residue 2 -o residue2.cnf tests/golden/solve_labelled.qsop && ganak residue2.cnf
build/sop2wmc --encoding amplitude tests/golden/solve_labelled.qsop | ganak --mode 6 --verb 0 -
```

The WMC export reconstructs `amplitude = sum_k counts[k] * exp(2*pi*i*k/r)` and
`probability = |amplitude|^2 * 2^(-norm_h)` (the same convention as
`sop-solve --include-probability`) outside the counter; the metadata header in
each CNF block documents the variable map and the final accumulator bits.

## Benchmarks

The public performance summary is [scoreboard.md](scoreboard.md).

To run the full benchmark pipeline and regenerate `scoreboard.md` (requires
Ganak at `/tmp/ganak/ganak` or pass `--ganak <path>`):

```sh
python3 tools/run_corpus_benchmarks.py \
    --artifact-dir /tmp/dlx4sop-artifacts \
    --output scoreboard.md
```

All per-tier timeouts default to 30 s. Override with `--timeout <seconds>`.
Pass `--skip-wmc`, `--skip-native`, or `--skip-solver` to run a subset.

Render a scoreboard from existing artifacts without re-running benchmarks:

```sh
python3 tools/refresh_scoreboard.py \
    --artifact-dir /tmp/dlx4sop-artifacts \
    --allow-missing \
    --output scoreboard.md
```

Useful benchmark helpers:

- `tools/bench_qasm_corpus.py`: run the QSOP importer and solver across a manifest.
- `tools/bench_wmc_ganak.py`: drive `sop2wmc` + Ganak and cross-check against `sop-solve`.
- `tools/bench_qasm_native_simulator.py`: compare against supported native simulators.
- `tools/render_scoreboard.py`: render ad hoc reports from JSONL artifact inputs.

## Current Status

[scoreboard.md](scoreboard.md) tracks corpus coverage, solver timings, native
simulator comparisons, and the current recommended solver configuration for
each benchmark tier.

## Deferred Work

Four known issues from the post-sprint audit. All changes should be followed
by `meson test -C build` (43 tests, ≥75% coverage gate).

### 1. `scripts/refresh_scoreboard.py` — wire up `--refresh-native`

**File:** `scripts/refresh_scoreboard.py:143,173`

The `--refresh-native` flag prints "not yet implemented" and exits. The real
native benchmark tool (`tools/bench_qasm_native_simulator.py`) reads a QASM
manifest, but the local corpus (`benchmarks/corpus/sop/`) is pure QSOP with
no manifest. Either generate a lightweight QASM manifest for the local corpus,
or replace the message with a clear redirect to `tools/run_corpus_benchmarks.py`.

**Acceptance:** `python3 scripts/refresh_scoreboard.py --refresh-native` does
not print "not yet implemented".

### 2. Branch backend errors — missing `--max-vars` passthrough

**File:** `scripts/bench_sop.py:33,42,100`

Six (instance, backend) pairs record `status=error` because `bench_sop.py`
never passes `--max-vars` to `sop-solve --backend branch`, even though the
branch solver has an internal cap below 26 variables. Affected instances:
`tier-17-32-cycle-n26-r16-01`, `tier-17-32-path-n27-r8-00`,
`tier-33-64-cycle-n35-r8-01`, `tier-33-64-path-n63-r8-00` (both `branch`
and `branch:from-treewidth`).

**Fix:**
```python
BACKEND_EXTRA_ARGS = {
    "rankwidth": ["--max-vars", "256"],
    "branch":    ["--max-vars", "64"],  # add this
}
# and in the branch:from-treewidth call (~line 100):
["--branch-rw-source", "from-treewidth", "--max-vars", "64"]
```

### 3. `tier-33-64-sparse-n58-r8-02` — hard instance, all backends fail

**File:** `benchmarks/corpus/sop/tier-33-64/tier-33-64-sparse-n58-r8-02.meta.json`

58 vars, 249 edges, r=8. Induced treewidth ≈ 25 (8^25 ≈ 10^22 table entries —
infeasible); rankwidth times out; branch hits the max-vars cap. Structurally
outside every current backend's tractable range.

Add `"known_hard": true` and `"skip_backends": ["treewidth", "rankwidth", "branch"]`
to the meta.json, then honour `skip_backends` in `bench_instance()` in
`bench_sop.py` so the instance is skipped cleanly instead of recording errors.

### 4. Scoreboard scripts duplication — `scripts/` stack superseded by `tools/`

`scripts/refresh_scoreboard.py` (180 lines) and `scripts/render_scoreboard.py`
(232 lines) are a prototype pipeline that operates on the 12-instance local
synthetic corpus only. `tools/refresh_scoreboard.py` (799 lines) and
`tools/render_scoreboard.py` (868 lines) are the real pipeline covering all
five tiers, all backends, all WMC encodings, and native simulator results.
They were never reconciled.

Options: (a) delete the `scripts/` scoreboard pair and make `scripts/bench_sop.py`
emit artifacts in the format `tools/refresh_scoreboard.py` consumes — saves
~400 lines of duplicate render logic; or (b) keep `scripts/` as a
zero-dependency local smoke path but rename and document clearly that `tools/`
is authoritative.

### 5. Incremental scoreboard refresh — rerun only the changed backend

`run_corpus_benchmarks.py` always reruns everything. When only one backend or
encoding changes, this wastes time on the unchanged jobs.

The artifact layout already supports incremental updates: each job writes to
its own per-(tier, backend) file, and `refresh_scoreboard.py` reads whatever
is present in `--artifact-dir`. So replacing only one artifact file and
re-rendering is already correct — what's missing is a convenient way to do it.

**What to add:** two filter flags on `run_corpus_benchmarks.py`:

- `--only-backend <name>` — restricts `SOLVER_JOBS` to entries whose
  `--backend` arg matches (e.g. `rankwidth`, `treewidth`, `branch`).
- `--only-encoding <name>` — restricts WMC jobs to the named encoding
  (e.g. `amp-block`, `amplitude`).

Either flag skips the unmatched jobs and still calls `refresh_scoreboard.py`
at the end, so the scoreboard is updated from the mix of new and existing
artifacts.

Until these flags exist, the manual workaround is to call `bench_qasm_corpus.py`
(or `bench_wmc_ganak.py`) directly with the changed backend, write the output
to `--artifact-dir` with the correct filename stem, then call
`tools/refresh_scoreboard.py --artifact-dir ... --allow-missing --output scoreboard.md`.
