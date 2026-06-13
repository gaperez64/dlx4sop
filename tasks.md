# dlx4sop Task Tracker

This file records the current local project state and near-term tasks. Completed
history is intentionally kept in Git, README, and architecture notes rather
than repeated here.

## Current State

- Branch: `main`, tracking `origin/main`.
- Latest pushed checkpoint: `b20ecdc` (`Lift rankwidth variable cap`).
- Core utilities implemented: `sop-check`, `sop-stats`, `sop-solve`, `qasm2sop`.
- Boundary tools implemented: QASM corpus benchmark runner, external manifest
  builder, PyZX/T-Par `.qc` translator, starter `.qgraph` translator, FeynmanDD
  scanner, and optional MQT Bench scanner.
- Solver status:
  - `components` remains the default exact backend.
  - `branch` has residual mutation, residual fingerprints, memo cache stats,
    component splitting, edge-free residue-table leaves, and experimental
    `split|treewidth|linear-rankwidth` variable heuristics.
  - `rankwidth` handles sign-edge QSOPs with generated or explicit
    decompositions, interned bitset signatures, count-table mode, CRT-backed
    large assignment counts, and small-instance Fourier mode.
- Importer status:
  - OpenQASM support covers the static finite subset used by the checked-in
    corpus and many PyZX/FeynmanDD/MQT cases.
  - Terminal-measurement stripping and simple-gate inlining are available in
    the external manifest builder.
  - `.qc` benchmark files can be translated through `tools/qc2qasm.py`.
- Last validation before this doc cleanup:
  - `meson test -C build --print-errorlogs`: 17/17 passing.
  - `tools/check-coverage.sh build-coverage`: 78.1% line coverage.

## Current Task

- Keep README short and user-facing.
- Keep `tasks.md` focused on current and future work.
- Keep architecture docs focused on durable design constraints and future
  nonimplemented work, not implementation history.

## Next Steps

- Rerun PyZX/FeynmanDD/MQT sign-only rankwidth manifests with `--max-vars`
  above 63 to measure how much of the larger external corpus the bitset/CRT
  path now handles.
- Compare rankwidth count-table fast path versus CRT path below 64 variables to
  quantify overhead and verify the default path remains cheap.
- Use rankwidth sweeps before changing generated-decomposition defaults; recent
  evidence differs between small checked-in cases and larger PyZX-derived
  cases.
- Decide whether Fourier should stay a small-instance comparison mode or gain a
  multi-prime CRT variant.
- Continue branch heuristic work only from benchmark evidence; current branch
  cache hit rates on realistic slices are low, so additional cache machinery is
  secondary.
- Keep coverage above the 75% CI gate.

## Deferred Work

- Better generated rankwidth decompositions and decomposition-aware heuristics.
- Labelled rankwidth based on labelled cut-signature width, not ordinary
  support-graph rankwidth. See
  [ARCHITECTURE.md](ARCHITECTURE.md#labelled-rankwidth-direction).
- Incremental residual hashing and richer canonical residual fingerprints.
- Dancing-cells-style linked mutation if traces show active-edge flag traversal
  is a bottleneck.
- Specialized residue/count kernels and optional CPU-feature dispatch.
- More importer syntax support driven by real benchmark misses.
- Optional exporters for external baselines such as FeynmanDD gate-set JSON or
  WMC encodings.
