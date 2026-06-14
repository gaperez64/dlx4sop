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
  - `sop-stats --exact-widths` can certify exact support-graph treewidth and
    GF(2) rankwidth for small instances under a guarded variable cap.
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
  - `tools/render_scoreboard.py` renders scoreboard Markdown tables from
    import reports plus solver/native JSONL.
  - `tools/bench_qasm_native_simulator.py` runs optional native QASM timing
    baselines for Qiskit Statevector, Aer, MQT DDSIM statevector, and PyZX
    matrix mode, with dense-state/matrix qubit caps.
  - Native QASM comparisons insert compatibility definitions for parser-missing
    `ccz` and `iswap` gates when needed.
  - Benchmark summaries report imported sign/labelled counts, rankwidth skips,
    source-family boundary counts, backend-specific rankwidth/treewidth width
    and table metrics, and largest case-boundaries by core size/performance
    metrics.
  - `tools/bench_qasm_widths.py` measures support-graph width diagnostics from
    QASM manifests using `qasm2sop` and `sop-stats`.
  - `tools/bench_qasm_corpus.py --solver-timeout SECONDS` records timed-out
    solver attempts as structured benchmark rows.
  - Current 32-variable source-attributed pool: 113 cases / 133 boundaries
    across internal, PyZX, MQT Bench, and FeynmanDD. Branch, rankwidth
    count-table, and treewidth solve it; rankwidth skips 12 zero-variable
    decomposition guard records in the current benchmark script.
  - First promoted 33-64 imported-variable tier: 32 fixed-boundary cases from
    PyZX and FeynmanDD, including 9 labelled cases. Treewidth solves the tier in
    about 31-32ms, branch split in about 99ms, and rankwidth min-fill-cut
    count-table in about 1.88s because of one labelled width-7 case.
  - Promoted 65-128 imported-variable tier: 130 fixed-boundary cases from PyZX,
    FeynmanDD, and MQT Bench, including 28 labelled cases. Treewidth solves the
    full tier in about 0.68-0.69s depending on order; branch split does not
    complete the first case within a 20s cap.
  - Native baselines now cover Qiskit, Aer, MQT DDSIM, and PyZX matrix mode.
    On the 65-128 tier, Qiskit/Aer/MQT handle 73 / 130 boundaries under a
    16-qubit cap; PyZX matrix handles 48 / 130 under a 10-qubit cap.
  - Qiskit manifest correctness comparison on the 33-64 tier checks 20 / 32
    boundaries under a 16-qubit cap with no Qiskit parser skips; the other 12
    are qubit-cap skips.
  - Exact support-width pass:
    checked-in corpus has exact support treewidth/rankwidth distribution
    `0:19, 1:13`; the current 0-32 importer-fed pool certifies 100 / 133
    boundaries under a 16-variable exact-width cap, with exact support
    treewidth/rankwidth distribution `0:63, 1:35, 2:2`.
    The 33-64 and 65-128 tiers are above the exact cap, so only heuristic
    min-fill width and linear cut-rank diagnostics are currently recorded there.
  - Branch heuristic comparison on that pool favors `split`: about 9.8k search
    nodes and 81ms total solve time, versus 91.5k/677ms for `treewidth` and
    4.0M/14.5s for `cutrank-proxy`.
- Last full validation:
  - `meson test -C build --print-errorlogs`: 24/24 passing.
  - `tools/check-coverage.sh build-coverage`: 77.1% line coverage.

## Current Task

- Use the 65-128 tier as the current widened benchmark baseline. Treewidth is
  the only full-tier backend that completes comfortably at this size.
- Investigate generated rankwidth decompositions: on FeynmanDD
  `random_10qubit_0`, the rankwidth `linear` generator solves in about 27ms,
  while `min-fill-cut` takes about 1.72s and `min-fill` exceeds a 20s cap.
- Use native simulator baselines as comparison data while `sop2X` exporters do
  not exist.

## Next Steps

- Add rankwidth generator diagnostics that can be run across a manifest without
  one bad decomposition hiding completed rows.
- Improve generated rankwidth decompositions for labelled cases, using the
  `random_10qubit_0` diagnostic and the first 65-128 PyZX Toffoli case as
  regression targets.
- Add optional capped branch profiling by case so search blowups are reported
  structurally instead of as silent timeouts.
- Use exact-width certification on small/capped manifests and heuristic width
  diagnostics on widened tiers until a more serious exact-parameter toolchain is
  justified.
- Keep native baseline harnesses on native benchmark formats until `sop2X`
  exporters exist.
- Keep `branch --branch-heuristic split` as the branch baseline until a harder
  pool shows that cache-heavy heuristics repay their selection cost.
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
