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
- Completed residual adjacency and importer checkpoint:
  - added immutable residual incident-edge lists and switched branch mutation,
    active-degree queries, and split estimates to local adjacency traversal;
  - added finite `u2(...)` and `u3(...)` imports for symbolic multiples of
    `pi/4`, with golden, dependency-free amplitude, and optional Qiskit coverage.
- Completed residual degree and cache lookup checkpoint:
  - added trailed active-degree metadata so branch heuristic degree queries are
    O(1) while checkpoint undo remains exact;
  - replaced branch residual-cache linear scans with fingerprint buckets while
    preserving exact key comparison before reuse.
- Completed residual branch component-splitting checkpoint:
  - added active-component labelling over the mutable residual graph;
  - taught the branch backend to solve disconnected active residual components
    separately, convolve their counts, and shift by the parent residual
    constant once.
- Completed small-component canonicalization checkpoint:
  - replaced the two-variable one-edge component relabelling special case with
    exhaustive component-cache canonicalization for components up to five
    variables;
  - added a mirrored three-variable path stats fixture that proves isomorphic
    small components share one cache entry.
- Completed benchmark and tracing checkpoint:
  - moved the QASM solver corpus into `tests/qasm_solver_corpus.json` and added
    repeated-component plus branch-split stress cases;
  - added `tools/bench_qasm_corpus.py` for JSONL/CSV benchmark records with
    backend counters, timings, source/QSOP hashes, and optional trace summaries;
  - added `sop-solve --trace csv` with trace phases for cache lookup, component
    splitting, branch selection, residue-table leaves, component solves, and
    convolution.
- Completed external-format reconnaissance:
  - PyZX's practical first path is an optional `.qgraph`/circuit loader that
    converts circuit-like diagrams to OpenQASM and reuses `qasm2sop`;
  - FeynmanDD's public interface is OpenQASM plus gate-set JSON, so the first
    compatibility target is benchmark ingestion and later OpenQASM/gate-set
    export for external baseline runs.
- Completed qgraph/FeynmanDD starter checkpoint:
  - added optional PyZX-backed `tools/qgraph2qasm.py` for `.qgraph` JSON to
    OpenQASM extraction when PyZX can extract a circuit;
  - cloned FeynmanDD to `/tmp/dlx4sop-feynmandd` for local corpus inspection;
  - added uppercase gate spelling, decimal `pi/4`-multiple angle, and `iswap`
    support to `qasm2sop`;
  - added `tools/scan_feynmandd_qasm.py`; current scan imports 92/152
    `benchmark/exp` files and all 60 remaining failures are `ccz`
    quadratization cases. Full non-invalid checkout scan imports 166/425 files.
- Completed higher-degree QASM/PyZX benchmark checkpoint:
  - added `ccz` lowering via a quadratic parity-phase transformation and `ccx`
    lowering via `h`/`ccz`/`h` on the target;
  - added `cswap` lowering via `cx`/`ccx`/`cx`;
  - added dependency-free amplitude and optional Qiskit coverage for `ccz`,
    `ccx`, and `cswap`;
  - re-scanned FeynmanDD: `benchmark/exp` imports 152/152 files, and the wider
    non-invalid checkout imports 402/425 files with no remaining higher-degree
    failures;
  - identified Kuyanov/Kissinger's rank-width ZX implementation as PyZX
    `pyzx/rank_width.py` and cloned PyZX to `/tmp/dlx4sop-pyzx`;
  - scanned PyZX QASM: `circuits/feyn_bench/qasm` imports 44/65 non-invalid
    QASM files, while all `circuits` QASM imports 109/130; remaining failures
    are dynamic/classical examples, generic custom gates, or malformed Shor
    output. The PyZX corpus also contains 214 `.qc` files and one `.qgraph`
    file for the optional PyZX-backed conversion path.
- Completed dependency-free `.qc` bridge checkpoint:
  - added `tools/qc2qasm.py`, mirroring PyZX's `.qc` parser rules for one-qubit
    gates, `cnot`, `swap`, multi-arity `tof`, and multi-target `Z`/`Zd`;
  - added a Meson smoke test that verifies translation and import through
    `qasm2sop`;
  - translated 203/214 local PyZX `.qc` files syntactically; the remaining 11
    are ten empty `.qc` files and one malformed Shor file;
  - sampled translated `tof_3_tpar.qc`, `grover_5.qc`, and `ham15-low.qc`, and
    all imported through `qasm2sop`.
- Latest local verification:
  - `meson test -C build --print-errorlogs`
  - `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`
  - `tools/check-coverage.sh build-coverage` at 78.0% line coverage over `src`.
- Completed MQT Bench reconnaissance checkpoint:
  - pushed the previous local branch state to `origin/main` before starting;
  - cloned Munich Quantum Toolkit Bench to `/tmp/dlx4sop-mqtbench`;
  - found that its source tree generates circuits through the `mqt.bench`
    Python package and Qiskit exporters rather than vendoring a flat QASM
    corpus;
  - added optional `tools/scan_mqt_bench.py`, which can use an installed
    package or local checkout, export generated QASM2, strip terminal
    measurements for strong-simulation imports, and classify `qasm2sop`
    outcomes;
  - added a lean Meson smoke test for the scanner without making MQT a CI
    dependency;
  - current local scan results: default size-3 `indep` subset imports 7/8
    generated cases, with `wstate` blocked by a non-`pi/4` `ry` angle; all
    size-3 `indep` benchmark names import 8 generated cases, with remaining
    cases grouped as generation constraints, QASM2 dump limitations,
    custom-gate syntax, or unsupported angles.
- Completed MQT eighth-turn phase checkpoint:
  - extended exact `u1`/`p` and `cu1`/`cp` import to direct `pi/8` phase
    coefficients over the existing `Z_16` importer representation;
  - kept `rz`/`crz` and axis rotations at `pi/4` resolution because their
    global phases or decompositions need a deliberate finer-modulus path before
    `pi/8` is safe;
  - added golden, dependency-free amplitude, negative-resolution, and optional
    Qiskit coverage;
  - improved the local MQT default size-3/4 `alg,indep` scan from 18/32 to
    23/32 imports; the remaining default misses are algorithm-level custom-gate
    syntax and `wstate` non-finite `ry` angles;
  - latest verification:
    `meson test -C build --print-errorlogs`,
    `meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs`, and
    `tools/check-coverage.sh build-coverage` at 78.0% line coverage over `src`.
- Completed solver benchmark summary checkpoint:
  - added aggregate summary output to `tools/bench_qasm_corpus.py` so cache hit
    rates and trace phase totals are visible without post-processing JSONL;
  - added a lean Meson smoke test for the new summary mode;
  - expanded `tests/qasm_solver_corpus.json` with bounded MQT-derived
    straight-line cases (`bv`, QFT-entangled size 3/4, and QPE-inexact size 4)
    that import through `qasm2sop` and stay cheap enough for all exact backends;
  - kept MQT `qwalk` out of the default correctness corpus because the imported
    QSOP has 96 variables after Toffoli lowering and exceeds the default exact
    solver guard;
  - current local summary for components+branch with traces over the expanded
    manifest: 32 case-boundaries, component cache hit rate 26/(26+43)=0.377,
    branch residual cache hit rate 0/(0+267)=0.000, branch leaves 477 versus
    component leaves 707.
- Completed external branch-cache measurement checkpoint:
  - added `tools/build_external_qasm_manifest.py` to materialize temporary
    benchmark manifests from external QASM roots and optional `.qc`
    translations after checking `qasm2sop` importability and an explicit
    solver variable guard;
  - added `--max-vars` pass-through to `tools/bench_qasm_corpus.py` so external
    runs can state their solver guard;
  - current size-gated external results:
    - FeynmanDD `benchmark/exp` imports 152 files, but the smallest all-zero
      boundary QSOP has 76 variables, so none fit the 24/32/40 branch-summary
      guard;
    - PyZX `feyn_bench/qasm` imports 44 files, but only one fits a 40-variable
      guard and that branch run timed out at 30 seconds;
    - PyZX translated `.qc` under a 24-variable guard emits 20 cases and 20
      boundaries: branch cache hit rate 0/(0+26)=0.000, component cache hit
      rate 17/(17+20)=0.459;
    - MQT generated default size-3/4 `alg,indep` under a 24-variable guard
      emits 20 cases and 20 boundaries: branch cache hit rate 0/(0+56)=0.000,
      component cache hit rate 24/(24+30)=0.444;
    - combined checked-in + MQT + PyZX `.qc` branch-solvable slice has 52 cases
      and 72 boundaries: branch cache hit rate 0/(0+349)=0.000, component cache
      hit rate 67/(67+93)=0.419.
- Completed branch trace ranking checkpoint:
  - added `--top` and `--top-metric` to `tools/bench_qasm_corpus.py` summary
    output so worst case-boundaries can be ranked by stable counters such as
    `search_nodes` or `leaf_assignments`;
  - trace-enabled top rows now include the dominant trace phase, keeping branch
    heuristic inspection visible without post-processing JSONL;
  - current checked-in branch ranking by `search_nodes` is led by
    `mqt_qftentangled_indep_4` boundaries at 27-35 search nodes, followed by
    `register_pair_mix` at 23 search nodes and `entangled_axis_chain` at 18
    search nodes; all still have zero branch cache hits.
- Completed balanced branch split checkpoint:
  - added `qsop_residual_split_without_var`, which reports both split count and
    largest remaining component size for candidate branch variables;
  - refined branch variable selection to prefer material reductions in the
    largest remaining component before degree/unary tie breaks, while ignoring
    one-variable balance changes that regressed paired length-5 path cases;
  - checked-in QASM corpus branch summary improved from 267 to 211 search nodes
    and from 477 to 469 leaf assignments, with `mqt_qftentangled_indep_4`
    dropping from up to 35 nodes to 15;
  - combined checked-in + MQT + PyZX `.qc` branch-solvable slice improved from
    349 to 281 branch search nodes and from 803 to 783 leaf assignments, with
    branch cache hits still at zero.

## Current Task

- Continue solver improvements using the current QASM importer, benchmark
  runner, trace output, and branch cache stats as measurable circuit-derived
  regressions.
- Next likely solver work before tree/rankwidth experiments:
  - use ranked branch summaries to tune remaining hard cases, now led by
    `register_pair_mix`, `entangled_axis_chain`, and the reduced
    `mqt_qftentangled_indep_4` cases;
  - deprioritize additional branch-cache machinery until we have realistic
    residual-repetition cases, since the checked-in plus branch-solvable
    external slice still shows no branch-cache hits;
  - decide whether split estimates need incremental component metadata or
    whether component splitting is enough for current benchmark scale;
  - keep coverage above the 75% CI gate.

## Future Tasks

- Add more finite OpenQASM syntax compatibility with boundary-level examples:
  - add small compatibility aliases when they reuse existing lowering paths.
- Prototype external-format boundary utilities:
  - add a real `.qgraph` conversion fixture once PyZX is available locally or in
    an optional test environment;
  - decide whether Quipper conversion still needs optional PyZX support beyond
    the dependency-free `.qc` bridge;
  - use `tools/scan_mqt_bench.py` to identify MQT cases that only need importer
    syntax support versus cases outside finite quadratic SOP scope;
  - later emit FeynmanDD-compatible OpenQASM plus gate-set JSON for baseline
    runs.
- Expand optional Qiskit comparison coverage as importer scope grows.
- Revisit performance-annex items as solver hot paths mature:
  - dancing-cells adjacency mutation remains incomplete: the residual backend
    has reversible mutation, immutable incidence lists, and trailed active
    degrees, but not linked-cell deletion/reinsertion or incremental component
    metadata. See
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
  - specialized residue kernels.
