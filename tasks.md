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
  - did not find a local Qymera pointer or public benchmark artifact to vendor,
    but added Qymera-inspired GHZ and uniform-superposition corpus cases;
  - added component-cache fingerprints for fast rejection before exact
    subinstance comparison;
  - added two-variable one-edge component relabelling so mirrored components can
    share cache entries;
  - updated README and architecture notes for the solver-focused direction.
- Completed residual branch checkpoint:
  - added an edge-free residue-table fast path for active independent unary
    variables;
  - taught the branch selector to skip isolated variables while active
    quadratic edges remain;
  - reduced the `solve_labelled` branch stats search from 7 to 3 nodes while
    preserving the same represented leaf assignments.
- Completed residual-state hashing checkpoint:
  - added deterministic residual fingerprints over the active graph, active
    unary labels, constant, modulus, and immutable edge labels;
  - added an exact branch-local residual memo cache filtered by those
    fingerprints and exposed `cache_hits`/`cache_misses` for the branch backend;
  - added a small branch-cache golden fixture and QASM corpus accounting checks
    so repeated residuals and cache-hit likelihood are visible.
- Latest local verification:
  - `meson test -C build --print-errorlogs`
  - `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 77.6% line coverage over `src`.

## Current Task

- Continue solver improvements using the current QASM importer and branch cache
  stats as measurable circuit-derived regressions.
- Next likely solver work before tree/rankwidth experiments:
  - inspect branch cache hit rates on QASM-derived cases and decide whether the
    residual cache needs bucketed lookup or component-aware residual keys;
  - keep small-component canonical relabelling scoped to cases proven by stats;
  - keep coverage above the 75% CI gate.

## Future Tasks

- Add more finite OpenQASM syntax compatibility with boundary-level examples:
  - consider `u2(...)` and constrained `u3(...)` once multi-parameter parsing is
    designed cleanly;
  - add small compatibility aliases when they reuse existing lowering paths.
- Expand optional Qiskit comparison coverage as importer scope grows.
- Revisit performance-annex items as solver hot paths mature:
  - dancing-cells adjacency mutation remains incomplete: the residual backend
    has reversible mutation, but not linked-cell deletion/reinsertion or
    incremental active-degree/component metadata. See
    [solver backends](ARCHITECTURE.md#solver-backends) and
    [A.5](ARCHITECTURE_SPEED_ANNEX.md#a5-keep-reversible-mutation-central).
  - hashing/caching remains partly incomplete: branch-local exact residual
    caching is implemented, but incremental Zobrist updates on the mutation
    trail and layered canonical residual fingerprints are still future work.
    See [A.6](ARCHITECTURE_SPEED_ANNEX.md#a6-make-incremental-hashing-part-of-mutation)
    and [A.7](ARCHITECTURE_SPEED_ANNEX.md#a7-use-layered-canonical-fingerprints).
  - tree/rankwidth heuristics remain future work: current branch scoring uses
    split count, active degree, and unary-label tie breaks, but not explicit
    min-fill/treewidth/rankwidth/cut-signature estimators or a pluggable
    heuristic interface. See
    [A.10](ARCHITECTURE_SPEED_ANNEX.md#a10-make-width-heuristics-pluggable).
  - broader small-component canonical relabelling;
  - structured timing/tracing;
  - specialized residue kernels.
