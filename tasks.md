# dlx4sop Task Tracker

This file records the current local project state and near-term tasks. Completed
history is intentionally kept in Git, README, and architecture notes rather
than repeated here.

## Current State

- Branch: `main`, tracking `origin/main`.
- Core utilities implemented: `sop-check`, `sop-stats`, `sop-solve`, `qasm2sop`.
- Boundary tools implemented: QASM corpus benchmark runner, external manifest
  builder, PyZX/T-Par `.qc` translator, starter `.qgraph` translator, FeynmanDD
  scanner, and optional MQT Bench scanner.
- Solver status:
  - `components` is the default exact backend, with component caching and
    CRT-backed large final histograms.
  - `branch` has residual mutation, residual fingerprints, memo cache stats,
    component splitting, edge-free residue-table leaves, CRT-backed large
    assignment counts, and experimental `split|treewidth|linear-rankwidth`
    variable heuristics.
  - `rankwidth` handles sign-edge and labelled QSOPs with generated or explicit
    decompositions, count-table mode, CRT-backed large assignment counts, and
    sign-only Fourier mode.
  - `treewidth` has a first exact min-fill bucket-elimination backend with
    CRT-backed large assignment counts.
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
- Last full validation:
  - `meson test -C build --print-errorlogs`: 17/17 passing.
  - `tools/check-coverage.sh build-coverage`: 77.5% line coverage.

## Current Task

- Validate the new `components` CRT path and `treewidth` backend on the full
  internal plus external importer-fed corpus.
- Use the resulting traces to decide whether treewidth decomposition quality,
  rankwidth generation, or branch cache behavior is the next limiting factor.

## Next Steps

- Compare `components`, `branch`, `rankwidth`, and `treewidth` on internal and
  external manifests after the treewidth baseline exists.
- Add summary/report labels that distinguish rankwidth width from treewidth
  elimination width when both are benchmarked together.
- Improve treewidth decomposition builders beyond greedy min-fill if corpus
  traces show large bags or table blowups.
- Use external classification reports to choose importer fixes that remain
  labelled quadratic.
- Keep coverage above the 75% CI gate.

## Deferred Work

- Better generated rankwidth and treewidth decompositions.
- Labelled rankwidth refinements based on labelled cut-signature width; see
  [ARCHITECTURE.md](ARCHITECTURE.md#labelled-rankwidth-direction).
- Incremental residual hashing and richer canonical residual fingerprints.
- Dancing-cells-style linked mutation if traces show active-edge flag traversal
  is a bottleneck.
- Specialized residue/count kernels and optional CPU-feature dispatch.
- Decide whether Fourier should stay a small-instance comparison mode or gain a
  labelled/multi-prime CRT variant.
- More importer syntax support driven by real benchmark misses.
- Optional exporters for external baselines such as FeynmanDD gate-set JSON or
  WMC encodings.
