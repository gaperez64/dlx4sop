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

## Current Task

- Add component-level solve cache:
  - cache repeated component QSOP solves before residue-vector convolution;
  - define a deterministic component key compatible with canonical QSOP data;
  - keep cache ownership local to the component backend until reuse patterns are clearer.

## Future Tasks

- Add sharper branch heuristics that account for component splits after assignment.
- Extend tests with algebraic invariants and parser fuzz targets.
- Add OpenQASM static-subset importer once the QSOP core is stable.
