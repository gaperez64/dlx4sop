# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.
Completed implementation history has been flushed into `README.md`.

## Last Checkpoint

- Rewrote `README.md` to describe the currently implemented utilities and
  include concrete usage examples.
- Moved the still-relevant QSOP definition, syntax, examples, normalization
  rules, and backend design notes into `ARCHITECTURE.md`.
- Copied `.github/FUNDING.yml` from `gaperez64/acacia-bonsai`, including the
  `buy_me_a_coffee: gaperez64` entry.
- Implemented `sop-solve --format stats`:
  - additive stats-aware solver entry points;
  - branch `search_nodes` and `leaf_assignments`;
  - component count and component subproblem leaf work;
  - golden CLI tests for branch and component stats output.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.2% line coverage over `src`.
- Implemented component-level solve caching:
  - local cache inside the component backend;
  - deterministic cache key from canonical component subinstance data;
  - cache hit/miss counters in `sop-solve --format stats`;
  - golden CLI coverage for a repeated-component cache hit.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.6% line coverage over `src`.
- Implemented split-aware branch selection:
  - residual helper to estimate active component count after removing a candidate variable;
  - branch selector now prioritizes split count, then active degree, then unary-label presence;
  - residual unit tests for articulation, non-articulation, post-branch, and inactive-variable cases.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 75.8% line coverage over `src`.

## Current Task

- Extend tests with algebraic invariants and parser fuzz targets:
  - start with deterministic algebraic invariant tests for canonicalization and solver agreement;
  - keep fuzz targets optional until the normal CI path remains lean.

## Future Tasks

- Add OpenQASM static-subset importer once the QSOP core is stable.
