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

## Current Task

- Add a CLI-visible solver stats mode:
  - expose backend, node, leaf, and component counters where available;
  - keep residue-vector output as the default stable format;
  - cover stats output with small golden CLI tests.

## Future Tasks

- Add component-level solve cache.
- Add sharper branch heuristics that account for component splits after assignment.
- Extend tests with algebraic invariants and parser fuzz targets.
- Add OpenQASM static-subset importer once the QSOP core is stable.
