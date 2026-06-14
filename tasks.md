# dlx4sop Task Tracker

This file records the current local project state and near-term tasks. Completed
history is intentionally kept in Git, README, and architecture notes rather
than repeated here.

## Current State

- Branch: `main`, tracking `origin/main`.
- Latest pushed checkpoint: `71533bd` (`Trim project documentation`).
- Core utilities implemented: `sop-check`, `sop-stats`, `sop-solve`, `qasm2sop`.
- Boundary tools implemented: QASM corpus benchmark runner, external manifest
  builder, PyZX/T-Par `.qc` translator, starter `.qgraph` translator, FeynmanDD
  scanner, and optional MQT Bench scanner.
- Solver status:
  - `components` remains the default exact backend.
  - `branch` has residual mutation, residual fingerprints, memo cache stats,
    component splitting, edge-free residue-table leaves, CRT-backed large
    assignment counts, and experimental `split|treewidth|linear-rankwidth`
    variable heuristics.
  - `rankwidth` handles sign-edge and labelled QSOPs with generated or explicit
    decompositions, count-table mode, CRT-backed large assignment counts, and
    sign-only Fourier mode.
- Importer status:
  - OpenQASM support covers the static finite subset used by the checked-in
    corpus and many PyZX/FeynmanDD/MQT cases.
  - `u(...)`/`u3(...)`, terminal-measurement stripping, and simple-gate inlining
    are available for external manifest ingestion.
  - `.qc` benchmark files can be translated through `tools/qc2qasm.py`.
- Benchmark status:
  - External manifest builds can emit per-source classification reports.
  - Benchmark summaries report imported sign/labelled counts, rankwidth skips,
    and largest case-boundaries by core size/performance metrics.
  - Recent rankwidth count-table sweeps solve the checked-in corpus and the
    63-variable PyZX-derived external manifests; a 99-variable edge-free
    FeynmanDD sample also solves, while dense 76-variable/88-edge Google CZ
    samples remain width/runtime stress cases.
- Last full validation:
  - `meson test -C build --print-errorlogs`: 17/17 passing.
  - `tools/check-coverage.sh build-coverage`: 77.5% line coverage.

## Current Task

- Use importer-fed corpus reports to separate unsupported syntax, non-quadratic
  cases, and rankwidth table-growth limits.
- Use branch/components as labelled baselines when rankwidth width or table
  growth needs explanation.
- Recompare branch and rankwidth on large imported cases now that both can emit
  CRT-backed large histograms.

## Next Steps

- Rerun raw PyZX/FeynmanDD/MQT pools with external classification reports and
  `--max-vars` above 63.
- Use the report categories to choose the next importer fixes that remain
  labelled quadratic.
- Compare branch/rankwidth fixed-width fast paths versus CRT paths below 64
  variables to quantify overhead and verify the default paths remain cheap.
- Use rankwidth sweeps before changing generated-decomposition defaults; recent
  evidence differs between small checked-in cases and larger PyZX-derived
  cases.
- Decide whether Fourier should stay a small-instance comparison mode or gain a
  labelled/multi-prime CRT variant.
- Continue branch heuristic work only from benchmark evidence; current branch
  cache hit rates on realistic slices are low, so additional cache machinery is
  secondary.
- Keep coverage above the 75% CI gate.

## Deferred Work

- Better generated rankwidth decompositions and decomposition-aware heuristics.
- Labelled rankwidth refinements based on labelled cut-signature width, not
  ordinary support-graph rankwidth. See
  [ARCHITECTURE.md](ARCHITECTURE.md#labelled-rankwidth-direction).
- Incremental residual hashing and richer canonical residual fingerprints.
- Dancing-cells-style linked mutation if traces show active-edge flag traversal
  is a bottleneck.
- Specialized residue/count kernels and optional CPU-feature dispatch.
- More importer syntax support driven by real benchmark misses.
- Optional exporters for external baselines such as FeynmanDD gate-set JSON or
  WMC encodings.
