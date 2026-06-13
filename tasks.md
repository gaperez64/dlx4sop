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

## Current Task

- Next implementation target:
  - Add exact residue-count brute-force evaluation for small QSOP instances.
  - Expose it through an initial `sop-solve --format=residue-vector` CLI.
  - Add tests that compare exact residue vectors against hand-computed examples.

## Future Tasks

- Add `sop-stats` with JSON output for modulus, variables, edge counts, components, mode, and degree.
- Add reusable residue-vector arithmetic and cyclic convolution.
- Add mutable residual state and reversible trail.
- Add component decomposition and component-level solve cache.
- Add simple branching backend and branch heuristics.
- Extend tests with algebraic invariants and parser fuzz targets.
- Add OpenQASM static-subset importer once the QSOP core is stable.
