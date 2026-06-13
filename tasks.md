# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.
Completed implementation history is kept in Git and summarized in `README.md`
and `ARCHITECTURE.md`.

## Previous Tasks

- Pushed OpenQASM importer work through `fdc33f1` to `origin/main`.
- Verified the latest pushed checkpoint with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.6% line coverage over `src`.
- Audited local Markdown files:
  - `README.md`
  - `ARCHITECTURE.md`
  - `ARCHITECTURE_SPEED_ANNEX.md`
  - `tasks.md`
- Trimmed `tasks.md` to resumable state and refreshed the architecture annex as
  supplemental performance guidance.
- Added finite controlled phase imports:
  - `cu1(...)` and `cp(...)` for symbolic multiples of `pi/4`;
  - direct lowering to labelled quadratic QSOP coefficients;
  - indexed and matching qreg-pair operands;
  - golden tests for indexed `cu1`, qreg-pair `cp`, and invalid-angle
    diagnostics.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.7% line coverage over `src`.
- Added finite `p(...)` phase alias:
  - reuses the existing `u1(...)` lowering path for symbolic multiples of
    `pi/4`;
  - golden coverage for `p(pi/2)`.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.8% line coverage over `src`.
- Added dependency-free OpenQASM amplitude checks:
  - new Meson test `qasm2sop amplitudes`;
  - compares `qasm2sop --input/--output | sop-solve` amplitudes against a
    small Python state-vector simulator for the supported static subset;
  - covers entangling, phase, whole-register, controlled-phase, swap, and
    zero-amplitude boundary cases.
- Verified with:
  - `meson test -C build --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.8% line coverage over `src`.

## Current Task

- Keep docs aligned while broadening OpenQASM importer compatibility:
  - add only gates that lower cleanly to supported primitives or have a direct
    QSOP representation;
  - keep coverage above the 75% CI gate.

## Future Tasks

- Add more finite OpenQASM syntax compatibility with boundary-level examples.
- Add small compatibility aliases when they reuse existing lowering paths.
- Add optional Qiskit comparison tests behind an explicit dependency gate,
  reusing the same fixed-boundary amplitude cases.
- Revisit performance-annex items as solver hot paths mature:
  - residual-state hashing;
  - stronger component fingerprints;
  - structured timing/tracing;
  - specialized residue kernels.
