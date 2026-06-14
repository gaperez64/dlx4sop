# dlx4sop Task Tracker

This file records the current local project state and near-term tasks. Completed
history is intentionally kept in Git, README, and architecture notes rather
than repeated here.

## Current State

- Branch: `main`, tracking `origin/main`.
- Project target: a competitive exact strong simulator based on labelled
  quadratic SOPs, with fixed-boundary amplitudes as the current benchmark
  contract.
- Core utilities implemented: `sop-check`, `sop-stats`, `sop-solve`, `qasm2sop`.
- Boundary tools implemented: QASM corpus benchmark runner, external manifest
  builder, PyZX/T-Par `.qc` translator, starter `.qgraph` translator, FeynmanDD
  scanner, and optional MQT Bench scanner.
- Solver status:
  - `components` is the default exact backend, with component caching and
    CRT-backed large final histograms.
  - `branch` has residual mutation, residual fingerprints, memo cache stats,
    dancing-cells-style active incidence unlink/relink, component splitting,
    edge-free residue-table leaves, CRT-backed large assignment counts, and
    experimental `split|treewidth|cutrank-proxy` variable heuristics. The old
    branch heuristic name was removed so `linear-rankwidth` remains reserved for
    an actual linear-rankwidth concept if one is added later.
  - `rankwidth` handles sign-edge and labelled QSOPs with generated or explicit
    decompositions, count-table mode, CRT-backed large assignment counts, and
    sign-only Fourier mode.
  - `treewidth` has a first exact bucket-elimination backend with
    `min-fill|min-degree|min-fill-max-degree` orders and CRT-backed large
    assignment counts.
- Importer status:
  - OpenQASM support covers the static finite subset used by the checked-in
    corpus and many PyZX/FeynmanDD/MQT cases.
  - `u(...)`/`u3(...)`, terminal-measurement stripping, and simple-gate inlining
    are available for external manifest ingestion.
  - External manifest ingestion records source names/URLs and has opt-in repair
    for known single-register alias typos in benchmark corpora.
  - `.qc` benchmark files can be translated through `tools/qc2qasm.py`.
- Benchmark status:
  - External manifest builds can emit per-source classification reports.
  - `tools/summarize_qasm_report.py` turns those reports into reproducible
    source/status/size-tier tables for benchmark promotion.
  - `tools/bench_qasm_native_simulator.py` starts optional native QASM timing
    baselines for Qiskit Statevector and Aer, with a dense-state qubit cap.
  - Benchmark summaries report imported sign/labelled counts, rankwidth skips,
    source-family boundary counts, backend-specific rankwidth/treewidth width
    and table metrics, and largest case-boundaries by core size/performance
    metrics.
  - Current 32-variable source-attributed pool: 113 cases / 133 boundaries
    across internal, PyZX, MQT Bench, and FeynmanDD. Branch, rankwidth
    count-table, and treewidth solve it; rankwidth skips 12 zero-variable
    decomposition guard records in the current benchmark script.
  - First promoted 33-64 imported-variable tier: 32 fixed-boundary cases from
    PyZX and FeynmanDD, including 9 labelled cases. Treewidth solves the tier in
    about 27-28ms, branch split in about 86ms, and rankwidth count-table in
    about 1.75s because of one labelled width-7 case.
  - Qiskit Statevector has an initial native-set baseline on the promoted tier:
    17 / 32 boundaries run under a 16-qubit dense-state cap in about 17ms.
    Aer now runs the same 17 boundaries in about 18ms.
  - Branch heuristic comparison on that pool favors `split`: about 9.8k search
    nodes and 81ms total solve time, versus 91.5k/677ms for `treewidth` and
    4.0M/14.5s for `cutrank-proxy`.
- Last full validation:
  - `meson test -C build --print-errorlogs`: 18/18 passing.
  - `tools/check-coverage.sh build-coverage`: 77.2% line coverage.

## Current Task

- Use external classification reports to push beyond the current 32-variable
  comparison pool. The pool now includes labelled FeynmanDD/PyZX cases but still
  has low support width: rankwidth width tops out at 3 and treewidth width at 2.
- Use the promoted 33-64 tier to drive solver comparisons, then build the next
  65-128 tier once the reporting path is stable.
- Add competitor baselines so solver improvements are measured against
  strong-simulation alternatives, not only against other dlx4sop backends.

## Next Steps

- Refresh the scoreboard from reproducible 0-32 and 33-64 runs, then add a
  controlled 65-128 tier.
- Add competitor baseline harnesses only on native benchmark formats until
  `sop2X` exporters exist. QASM-native Qiskit/Aer timing has started; MQT and
  ZX-calculus simulator baselines still need native-corpus runners.
- Add optional environment detection/reporting for native simulators so missing
  MQT/ZX dependencies show up as structured skips.
- Keep `branch --branch-heuristic split` as the branch baseline until a harder
  pool shows that cache-heavy heuristics repay their selection cost.
- Add stronger rankwidth/treewidth decomposition builders only after larger
  cases show bag or table blowups.
- Use external classification reports to choose the next importer fixes that
  remain labelled quadratic.
- Keep [scoreboard.md](scoreboard.md) as the concise public record for corpus
  coverage, solver results, native-set external simulator comparisons, and the
  exact commands used to refresh tables.
- Keep coverage above the 75% CI gate.

## Deferred Work

- Better generated rankwidth and treewidth decompositions.
- Labelled rankwidth refinements based on labelled cut-signature width; see
  [ARCHITECTURE.md](ARCHITECTURE.md#labelled-rankwidth-direction).
- Incremental residual hashing and richer canonical residual fingerprints.
- Specialized residue/count kernels and optional CPU-feature dispatch.
- Decide whether Fourier should stay a small-instance comparison mode or gain a
  labelled/multi-prime CRT variant.
- More importer syntax support driven by real benchmark misses.
- Optional `sop2X` exporters for external baselines such as FeynmanDD gate-set
  JSON or WMC encodings.
