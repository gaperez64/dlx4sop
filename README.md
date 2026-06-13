# dlx4sop

`dlx4sop` is a C/Meson toolkit for exact finite-modulus quadratic sums of powers
(QSOPs). It currently provides small Unix-style utilities for validating,
canonicalizing, inspecting, and exactly solving normalized QSOP text files.

The implemented core works with degree-at-most-two SOPs over an even modulus `r`:

```text
p qsop <r> <num_variables> <num_quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0|1>
```

`e u v` is shorthand for a sign edge with coefficient `r/2`. Pins (`f`) are
applied during parsing and the canonical writer emits the reduced QSOP.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the mathematical definition,
normalization rules, and backend design notes. Longer-term performance notes
live in [ARCHITECTURE_SPEED_ANNEX.md](ARCHITECTURE_SPEED_ANNEX.md).

## Implemented Utilities

- `sop-check`: parse, validate, normalize, and write canonical QSOP.
- `sop-stats`: print structural statistics in text or JSON.
- `sop-solve`: compute exact residue-count vectors or solver stats with one of
  four backends:
  - `components` (default): decompose connected components, cache repeated
    component solves with exhaustive relabelling for components up to five
    variables plus fingerprinted cache lookup, and brute-force each cache miss;
  - `brute-force`: enumerate all assignments directly;
  - `branch`: recursive residual branch-and-sum using a reversible trail and a
    split-aware variable heuristic, a bucketed fingerprinted residual memo
    cache, residual component splitting, balanced split tie-breaks, and a
    residue-table fast path once no active quadratic edges remain. Experimental
    variable-choice heuristics can be selected with `--branch-heuristic`.
  - `rankwidth`: experimental sign-edge QSOP backend over explicit or generated
    decompositions, using boundary-signature tables in either residue-count or
    exact Fourier mode. Generated decompositions include linear, balanced,
    min-fill, and min-fill with cut-rank-aware recursive splits.
- `qasm2sop`: import a small static OpenQASM 2.0 subset into canonical QSOP,
  with explicit fixed input/output bitstrings, finite `u1`/`p` phase calls up
  to `pi/8`, finite `rz` phase calls for `pi/4` multiples, finite `rx`/`ry`
  axis rotations, finite `u2`/`u3` calls, controlled-phase calls up to `pi/8`,
  named controlled phase gates, and a few
  decomposition-backed standard gates including `sx`/`sxdg`. Supported operands
  include indexed qubits,
  whole-register one-qubit operands, and matching whole-register two-qubit
  operands.
- `tools/bench_qasm_corpus.py`: run the QASM solver corpus through `qasm2sop`
  and `sop-solve`, emitting JSONL, CSV, or aggregate summary output with backend
  counters, wall times, hashes, cache hit rates, optional phase-trace summaries,
  and ranked top case-boundaries for heuristic inspection.
- `tools/build_external_qasm_manifest.py`: build a
  `qasm_solver_corpus.json`-compatible manifest from external QASM roots, and
  optionally translated `.qc` files, after checking importability and an
  explicit solver variable guard. It can also strip terminal measurements from
  QASM inputs and inline simple non-parameterized gate definitions for
  strong-simulation benchmark imports.
- `tools/qgraph2qasm.py`: optional PyZX-backed starter utility for translating
  PyZX/Quantomatic `.qgraph` JSON diagrams to OpenQASM when PyZX can extract a
  circuit.
- `tools/qc2qasm.py`: dependency-free translator for the PyZX/T-Par `.qc`
  circuit format into the supported OpenQASM subset.
- `tools/scan_feynmandd_qasm.py`: scan a local FeynmanDD checkout or corpus
  root through `qasm2sop` and group import failures by cause.
- `tools/scan_mqt_bench.py`: optionally generate Munich Quantum Toolkit Bench
  QASM2 cases from an installed package or local checkout, strip final
  measurements for strong-simulation imports, and group `qasm2sop` outcomes.

The test suite also covers reusable residue-vector helpers, mutable residual
state, deterministic algebraic invariants for canonicalization and solver
agreement, dependency-free amplitude checks for `qasm2sop + sop-solve`, and a
QASM-derived solver corpus that compares all exact backends and stats paths,
including GHZ and uniform-superposition benchmark shapes.

## Build And Test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Coverage is part of CI and is expected to stay at or above 75% line coverage for
production `src` files:

```sh
meson setup --wipe build-coverage -Db_coverage=true
tools/check-coverage.sh build-coverage
```

The GitHub Actions workflow runs the normal Meson test suite and this coverage
gate on every push.

Optional parser fuzz targets are kept outside the normal CI path:

```sh
meson setup build-fuzz -Dbuild_fuzzers=true
meson test -C build-fuzz --print-errorlogs
build-fuzz/fuzz-qsop-parse tests/golden/labelled_raw.qsop
```

Optional Qiskit comparisons are also kept outside the normal CI path. They
compare fixed-boundary `qasm2sop + sop-solve` amplitudes against Qiskit
`Statevector` for the supported static subset:

```sh
meson setup build-qiskit -Dqiskit_tests=true
meson test -C build-qiskit 'qasm2sop qiskit' --print-errorlogs
```

## Usage Examples

Canonicalize a raw labelled QSOP:

```sh
build/sop-check tests/golden/labelled_raw.qsop
```

Output:

```text
p qsop 8 2 1
n 4
cst 2

u 0 2
u 1 4

q 0 1 3
```

Inspect the same canonical instance:

```sh
build/sop-stats tests/golden/labelled_expected.qsop
build/sop-stats --format json tests/golden/labelled_expected.qsop
```

Text output:

```text
modulus: 8
variables: 2
quadratic_terms: 1
nonzero_unary: 2
normalization_h: 4
mode: labelled
components: 1
max_degree: 1
width_diagnostics: available
min_fill_width: 1
min_fill_edges: 0
linear_cut_rank: 1
```

Solve a QSOP and print residue counts modulo `r`:

```sh
build/sop-solve tests/golden/solve_labelled.qsop
build/sop-solve --backend brute-force tests/golden/solve_labelled.qsop
build/sop-solve --backend branch tests/golden/solve_labelled.qsop
build/sop-solve --backend branch --branch-heuristic treewidth tests/golden/solve_labelled.qsop
build/sop-solve --backend branch --branch-heuristic linear-rankwidth tests/golden/solve_labelled.qsop
build/sop-solve --backend rankwidth tests/golden/solve_sign_path.qsop
build/sop-solve --backend rankwidth --rankwidth-generate min-fill tests/golden/solve_sign_path.qsop
build/sop-solve --backend rankwidth --rankwidth-generate min-fill-cut tests/golden/solve_sign_path.qsop
build/sop-solve --backend rankwidth --rankwidth-mode fourier tests/golden/solve_sign_path.qsop
build/sop-solve --backend rankwidth --rankwidth-generate min-fill-cut --rankwidth-mode fourier tests/golden/solve_sign_path.qsop
build/sop-solve --backend rankwidth --rankwidth-decomposition tests/golden/solve_sign_path.rwdec tests/golden/solve_sign_path.qsop
```

Output:

```text
p qsop-result 8
n 4
counts 0 0 1 1 1 0 1 0
```

Print backend-specific solver counters instead of the residue vector:

```sh
build/sop-solve --format stats --backend branch tests/golden/solve_labelled.qsop
```

Output:

```text
backend: branch
search_nodes: 3
cache_hits: 0
cache_misses: 3
leaf_assignments: 4
```

Rankwidth decompositions use a small explicit text format:

```text
p rwdec <variables> <nodes> <root>
l <node> <variable>
j <node> <left-child> <right-child>
```

Without `--rankwidth-decomposition`, `sop-solve --backend rankwidth` generates a
linear decomposition. `--rankwidth-generate balanced|min-fill|min-fill-cut`
selects generated balanced input-order, min-fill-order, or min-fill-order with
cut-rank-aware recursive split decompositions. `--rankwidth-mode fourier` uses
the exact modular-DFT variant and can be combined with any generated
decomposition. The backend currently supports sign-only quadratic coefficients
and mask-backed instances up to the solver variable guard.

The branch cache counters expose repeated residual states when they occur. For
example, the small triangle fixture revisits one residual:

```sh
build/sop-solve --format stats --backend branch tests/golden/solve_branch_cache.qsop
```

```text
backend: branch
search_nodes: 7
cache_hits: 1
cache_misses: 6
leaf_assignments: 6
```

Trace coarse backend phases to CSV on stderr while preserving the primary output
on stdout:

```sh
build/sop-solve --format stats --backend branch --trace csv tests/golden/solve_branch_cache.qsop
```

Component stats include local cache behavior:

```sh
build/sop-solve --format stats --backend components tests/golden/solve_repeated_components.qsop
```

Output:

```text
backend: components
components: 2
cache_hits: 1
cache_misses: 1
leaf_assignments: 4
```

Mirrored small components can also share cache entries after canonical
relabelling:

```sh
build/sop-solve --format stats --backend components tests/golden/solve_mirrored_path_components.qsop
```

```text
backend: components
components: 2
cache_hits: 1
cache_misses: 1
leaf_assignments: 8
```

Import a small OpenQASM 2.0 circuit into canonical QSOP:

```sh
build/qasm2sop tests/golden/qasm_hth.qasm
build/qasm2sop --input 1 --output 1 tests/golden/qasm_h_boundary.qasm
```

Output:

```text
p qsop 8 1 0
n 2
cst 0

u 0 1
```

The `--input` and `--output` bitstrings are ordered by flattened `qreg`
declaration order. Omitted boundaries default to all-zero bits, matching the
original 0-to-0 amplitude behavior.
The importer emits compact `Z_8` QSOP when possible and widens to `Z_16` for
half-step global phases such as `rz(pi/4)` and eighth-turn phase gates such as
`cp(pi/8)`.
It accepts the FeynmanDD-style quadratic subset used by the Google benchmarks,
including uppercase gate spellings, decimal angle literals for multiples of
`pi/4`, `iswap`, `ccz`, `ccx`, and `cswap`. `ccz` is lowered through a quadratic
parity-phase transformation, and `ccx` is lowered as `H; CCZ; H` on the target.
`cswap` is lowered as `CX; CCX; CX`.

Whole-register OpenQASM operands are accepted for supported gates:

```sh
build/qasm2sop --input 1100 --output 1111 tests/golden/qasm_register_cx.qasm
```

Read from stdin:

```sh
build/sop-check - < tests/golden/sign_raw.qsop
```

Limit exact enumeration size:

```sh
build/sop-solve --max-vars 20 tests/golden/solve_disconnected.qsop
```

The `--max-vars` limit applies to whole-instance brute force and residual branch
solving. For the default `components` backend it applies to each connected
component.

Run the manifest-backed QASM solver corpus as a lightweight benchmark:

```sh
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --trace --format jsonl
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend branch --format csv
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend components --backend branch --trace --format summary
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend branch --trace --format summary --top 8 --top-metric search_nodes
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend branch --branch-heuristic linear-rankwidth --trace --format summary --top 8 --top-metric search_nodes
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --skip-unsupported --trace --format summary --top 8 --top-metric max_table_entries
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --rankwidth-generate min-fill-cut --skip-unsupported --trace --format summary --top 8 --top-metric max_table_entries
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --rankwidth-mode fourier --skip-unsupported --trace --format summary --top 8 --top-metric max_table_entries
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --backend rankwidth --rankwidth-sweep --skip-unsupported --trace --format summary --top 8 --top-metric max_table_entries
```

Build a temporary external benchmark manifest for the same runner:

```sh
WORKDIR="${WORKDIR:-external-benchmarks}"
git clone --depth 1 https://github.com/zxcalc/pyzx.git "$WORKDIR/pyzx"
tools/build_external_qasm_manifest.py build/qasm2sop "$WORKDIR/pyzx/circuits" --include-qc --qc2qasm tools/qc2qasm.py --strip-terminal-measurements --inline-simple-gates --max-vars 24 --output "$WORKDIR/pyzx-qc-manifest.json"
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --manifest "$WORKDIR/pyzx-qc-manifest.json" --backend components --backend branch --trace --format summary --top 5 --top-metric leaf_assignments
tools/build_external_qasm_manifest.py build/qasm2sop "$WORKDIR/pyzx/circuits" --include-qc --qc2qasm tools/qc2qasm.py --strip-terminal-measurements --inline-simple-gates --max-vars 63 --output "$WORKDIR/pyzx-rankwidth-manifest.json"
tools/bench_qasm_corpus.py build/qasm2sop build/sop-solve --manifest "$WORKDIR/pyzx-rankwidth-manifest.json" --backend rankwidth --rankwidth-sweep --max-vars 63 --skip-unsupported --trace --format summary --top 8 --top-metric max_table_entries
```

Inspect a local FeynmanDD checkout:

```sh
WORKDIR="${WORKDIR:-external-benchmarks}"
git clone --depth 1 https://github.com/cqs-thu/feynman-decision-diagram.git "$WORKDIR/feynmandd"
tools/scan_feynmandd_qasm.py build/qasm2sop "$WORKDIR/feynmandd/benchmark/exp"
```

Inspect generated MQT Bench circuits without making MQT a project dependency:

```sh
WORKDIR="${WORKDIR:-external-benchmarks}"
git clone --depth 1 https://github.com/munich-quantum-toolkit/bench.git "$WORKDIR/mqt-bench"
tools/scan_mqt_bench.py build/qasm2sop --mqt-source "$WORKDIR/mqt-bench" --benchmarks default --sizes 3
tools/scan_mqt_bench.py build/qasm2sop --mqt-source "$WORKDIR/mqt-bench" --benchmarks all --sizes 3 --levels indep --format json
```

The scanner strips terminal measurements by default because `qasm2sop` imports
fixed-boundary amplitudes for straight-line circuits. Mid-circuit classical
control, reset, custom gate definitions, and non-finite rotation angles remain
unsupported and are reported as scan categories.

Inspect the PyZX QASM benchmark subset used around the rank-width ZX work:

```sh
WORKDIR="${WORKDIR:-external-benchmarks}"
git clone --depth 1 https://github.com/zxcalc/pyzx.git "$WORKDIR/pyzx"
tools/scan_feynmandd_qasm.py build/qasm2sop "$WORKDIR/pyzx/circuits/feyn_bench/qasm"
```

Translate a PyZX/T-Par `.qc` circuit through OpenQASM:

```sh
tools/qc2qasm.py "$WORKDIR/pyzx/circuits/Arithmetic_and_Toffoli/tof_3_tpar.qc" | build/qasm2sop -
```

Start from a PyZX/Quantomatic `.qgraph` JSON diagram when PyZX is installed:

```sh
tools/qgraph2qasm.py diagram.qgraph | build/qasm2sop -
```

## Current Direction

The current implementation target is solver improvement using QASM-derived
instances as regression inputs for backend agreement, component-cache behavior,
trace stability, and benchmark trend tracking. External-format utilities remain
optional Python boundary tools so PyZX, Qiskit, MQT, and benchmark-corpus
dependencies do not become runtime dependencies of the C solver.
