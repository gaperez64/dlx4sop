# dlx4sop Task Tracker

This file tracks local project state so work can resume after a pause or stop.
Completed implementation history is kept in Git and summarized in `README.md`
and `ARCHITECTURE.md`.

## Previous Tasks

- Pushed importer and documentation work through `7abd6b5` to `origin/main`.
- Latest importer additions:
  - exact finite `rz(...)`/`crz(...)` imports, with `Z_16` documented as
    importer resolution rather than a core modulus ceiling;
  - `sx`/`sxdg` imports with canonical, amplitude, and optional Qiskit coverage;
  - finite `rx(...)`/`ry(...)` imports for symbolic multiples of `pi/4`, lowered
    through existing `rz`, phase, and Hadamard primitives.
- Latest completed verification:
  - `meson test -C build --print-errorlogs`
  - `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 76.9% line coverage over `src`.
- Completed solver-pivot checkpoint:
  - added a default QASM-derived solver corpus that compares all exact backends
    and checks stats invariants on fixed-boundary circuit instances;
  - added component-cache fingerprints for fast rejection before exact
    subinstance comparison;
  - updated README and architecture notes for the solver-focused direction.
- Latest local verification:
  - `meson test -C build --print-errorlogs`
  - `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 77.5% line coverage over `src`.

## Current Task

- Pivot back to solver improvements using the current QASM importer as a circuit
  source for boundary-level solver regressions and backend stats.
- Next likely solver work:
  - use the QASM corpus to add broader benchmark-shaped cases;
  - inspect residual branch stats for split-heavy circuits;
  - keep coverage above the 75% CI gate.

## Future Tasks

- Add more finite OpenQASM syntax compatibility with boundary-level examples:
  - consider `u2(...)` and constrained `u3(...)` once multi-parameter parsing is
    designed cleanly;
  - add small compatibility aliases when they reuse existing lowering paths.
- Expand optional Qiskit comparison coverage as importer scope grows.
- Revisit performance-annex items as solver hot paths mature:
  - residual-state hashing;
  - small-component canonical relabelling;
  - structured timing/tracing;
  - specialized residue kernels.
