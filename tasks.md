# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.
Completed implementation history is kept in Git and summarized in `README.md`
and `ARCHITECTURE.md`.

## Previous Tasks

- Pushed importer and documentation work through `13f706b` to `origin/main`.
- Local commits not yet pushed:
  - `2c29bac` adds exact finite `rz(...)`/`crz(...)` imports and documents that
    `Z_16` is importer resolution, not a core modulus ceiling.
  - `f0bf99b` adds `sx`/`sxdg` imports and matching canonical, amplitude, and
    optional Qiskit coverage.
  - The current checkpoint adds finite `rx(...)`/`ry(...)` imports for symbolic
    multiples of `pi/4`, lowered through existing `rz`, phase, and Hadamard
    primitives.
- Latest completed verification:
  - `meson test -C build --print-errorlogs`
  - `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.9% line coverage over `src`.

## Current Task

- Keep docs aligned while broadening OpenQASM importer compatibility:
  - add only gates that lower cleanly to supported primitives or have a direct
    QSOP representation;
  - keep coverage above the 75% CI gate.
- Next likely importer work: decide whether to add multi-parameter finite
  `u2(...)`/`u3(...)` parsing or stay with smaller aliases first.

## Future Tasks

- Add more finite OpenQASM syntax compatibility with boundary-level examples:
  - consider `u2(...)` and constrained `u3(...)` once multi-parameter parsing is
    designed cleanly;
  - add small compatibility aliases when they reuse existing lowering paths.
- Expand optional Qiskit comparison coverage as importer scope grows.
- Revisit performance-annex items as solver hot paths mature:
  - residual-state hashing;
  - stronger component fingerprints;
  - structured timing/tracing;
  - specialized residue kernels.
