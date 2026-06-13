# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.

## Previous Tasks

- Read local project markdowns:
  - `README.md`
  - `ARCHITECTURE.md`
  - `ARCHITECTURE_SPEED_ANNEX.md`
- Confirmed the repository initially contained only documentation and `LICENSE`.
- Extracted the near-term roadmap:
  - Milestone 0: C23/Meson build skeleton, developer tooling, minimal `sop-check`.
  - Milestone 1: QSOP parser/writer, normalization, pinning, exact brute force for small instances.
  - Later milestones: reversible solver, component decomposition, hashing, width-aware backends, OpenQASM/MQT import, WMC/ZX/FeynmanDD export.
- Implemented the initial project slice:
  - C23 Meson project scaffold.
  - Dense normalized QSOP representation.
  - Parser for `p qsop` and `p qsop-sign` files.
  - Canonicalization for unary terms, duplicate quadratic terms, sign edges, self-loops, and pins.
  - Sign/labelled mode detection.
  - Canonical writer through a `sop-check` CLI.
  - Focused golden tests and a negative parser test.
- Verified with:
  - `meson setup build`
  - `meson compile -C build`
  - `meson test -C build`
- Committed and pushed initial scaffold to `origin/main`:
  - `bdae7c2 Scaffold QSOP parser and sop-check`
- Implemented initial exact small-instance solver:
  - `qsop_result_t` residue-count result type.
  - Brute-force evaluator for bounded-size QSOP instances.
  - `sop-solve --format residue-vector`.
  - Golden tests for unary and labelled QSOP residue counts.
- Implemented `sop-stats`:
  - Text and JSON output.
  - Modulus, variable count, quadratic term count, nonzero unary count, normalization exponent, mode, component count, and max degree.
  - Component counting over the canonical quadratic support graph, including isolated variables.
- Implemented reusable residue-vector helpers:
  - allocation and clearing;
  - phase shift/add;
  - cyclic convolution modulo `r`;
  - C unit tests for residue arithmetic.
- Refactored brute-force result allocation to use the shared residue helper.
- Added a lightweight CI workflow:
  - normal Meson configure/test;
  - coverage build using Meson `b_coverage=true`;
  - `gcovr` line coverage gate over production `src` code.
- Added `tools/check-coverage.sh` with a default 75% line coverage threshold.
- Expanded CLI tests for help, stdin, error paths, and output handling to keep the coverage gate meaningful.
- Implemented component-decomposed exact solving:
  - `qsop_solve_components_bruteforce`;
  - `sop-solve --backend components|brute-force`;
  - disconnected golden test comparing component decomposition with direct brute force;
  - default `sop-solve` backend is now `components`.
- Implemented mutable residual state and reversible trail:
  - self-contained residual copy of unary labels, edge labels, active vertices, and active edges;
  - checkpoints and undo through a reversible mutation trail;
  - branch rule for `x_v = 0` and `x_v = 1`;
  - unit tests for zero/one branches, nested undo, and invalid branch calls.
- Added editor swap-file ignores so local task-editor artifacts do not appear in `git status`.
- Verified current local work with:
  - `meson compile -C build`
  - `meson test -C build`
  - `tools/check-coverage.sh build-coverage` at 76.3% line coverage over `src`.

## Current Task

- Next implementation target:
  - Add a first residual branch-and-sum backend:
    - evaluate active residual state recursively;
    - use the reversible trail instead of copying at every branch;
    - compare against brute force and component backend on small examples.

## Future Tasks

- Add component decomposition and component-level solve cache.
- Add simple branching backend and branch heuristics.
- Extend tests with algebraic invariants and parser fuzz targets.
- Add OpenQASM static-subset importer once the QSOP core is stable.
