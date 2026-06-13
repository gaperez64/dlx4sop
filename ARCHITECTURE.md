# dlx4sop Architecture

`dlx4sop` is built around one core object: a finite-modulus quadratic sum of
powers (QSOP). The current implementation keeps the core independent of quantum
framework runtimes and exposes it through small command-line utilities.

The near-term scope is deliberately restricted to SOPs of degree at most two.
Higher-degree gates such as `CCZ` and Toffoli should be decomposed, rewritten, or
quadratized into labelled quadratic form before they reach the solver.
Performance-oriented design notes that are not part of the current command-line
contract are kept in [ARCHITECTURE_SPEED_ANNEX.md](ARCHITECTURE_SPEED_ANNEX.md).

## Mathematical Object

A labelled QSOP instance represents

```text
Z = 2^(-h/2) omega_r^c
    sum_{x in {0,1}^N} omega_r^(
      sum_i b_i x_i + sum_{i<j} q_ij x_i x_j
    )
```

where:

- `r` is an even positive modulus;
- `omega_r = exp(2*pi*i/r)`;
- `h` is the power-of-two normalization exponent;
- `c` is the constant phase exponent modulo `r`;
- each unary coefficient `b_i` is stored modulo `r`;
- each quadratic coefficient `q_ij` is stored modulo `r`.

The implementation stores exponent data over `Z_r`, not complex weights.
Complex or exact ring values should be produced only by a future final-value
renderer or specialized backend.

## QSOP Text Format

The parser accepts comments beginning with `c`, dense vertices `0..N-1`, and the
following records:

```text
p qsop <r> <num_variables> <num_quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0|1>
```

`p qsop-sign` is accepted for sign-edge-only files. A sign edge

```text
e u v
```

is shorthand for:

```text
q u v r/2
```

Pins are parser-level reductions:

```text
f v 0
f v 1
```

They remove `v` from the resulting canonical instance after folding the selected
value into the constant and neighboring unary terms.

### Sign-Edge Example

```text
c Sign-edge QSOP over Z_8
p qsop-sign 8 5 4
n 12
cst 3

u 0 1
u 2 7
u 4 4

e 0 1
e 1 2
e 2 3
e 3 4
```

Each edge contributes `4*x_u*x_v` for `r = 8`.

### Labelled Example

```text
c Labelled quadratic SOP over Z_8
p qsop 8 3 3
n 4
cst 0

u 0 1

q 0 1 2
q 1 2 4
q 0 2 6
```

This permits arbitrary quadratic coefficients modulo `r` and is the default
internal representation once labelled pair interactions are involved.

## Normalization Rules

The parser and writer produce deterministic canonical QSOP output.

- `r` must be even.
- Unspecified unary coefficients default to `0`.
- Unary coefficients accumulate modulo `r`.
- Quadratic endpoints are canonicalized so `u < v`.
- Duplicate labelled quadratic terms accumulate modulo `r`.
- Accumulated quadratic terms with coefficient `0 mod r` are deleted.
- Duplicate sign edges cancel modulo two because two `r/2` terms sum to `0 mod r`.
- A self-loop `q v v a` is folded into unary coefficient `u v a`, because
  `x_v^2 = x_v`.
- Pins reduce variables and remap the remaining vertices densely.

Canonical output is intentionally stable for hashing, golden tests, and future
memoization.

## Implemented Modules

Current public headers live under `include/dlx4sop/`:

- `qsop.h`: dense normalized QSOP representation, parse/write entry points, and
  diagnostics.
- `qsop_stats.h`: structural statistics for normalized instances.
- `qsop_solve.h`: residue-count result type and exact solver entry points.
- `residue.h`: reusable residue-vector operations.
- `residual.h`: mutable residual state, checkpoints, undo, and branch mutation.

Current production implementation lives under:

- `src/core`: parser, writer, QSOP lifecycle, statistics, residue-vector helpers.
- `src/solve`: brute force, component decomposition, residual branch-and-sum,
  and reversible residual state.
- `src/cli`: `sop-check`, `sop-stats`, `sop-solve`, and `qasm2sop`.
- `tools`: lightweight developer and benchmark scripts.

Tests are split between Python CLI golden tests and C unit tests for residue and
residual behavior. The default suite also includes QASM-derived solver corpus
cases from `tests/qasm_solver_corpus.json` that import fixed-boundary circuits,
compare all exact backends, and check solver stats invariants. The corpus
includes GHZ and uniform-superposition shapes inspired by the Qymera benchmark
demonstration scenarios, repeated-component cases, and small split/caching
stress cases, expressed as local OpenQASM snippets rather than vendored
benchmark files. Optional parser fuzzing is available as a separate Meson
option.

## Solver Backends

### Direct Brute Force

`qsop_solve_bruteforce` enumerates every assignment when `nvars <= max_vars`.
It is simple, deterministic, and useful as an oracle for small tests.

### Component Brute Force

`qsop_solve_components_bruteforce` builds the quadratic support graph, labels
connected components, solves each component independently, and cyclically
convolves residue-count vectors. The global constant is applied at the end.

This is the default `sop-solve` backend because disconnected instances can be
much cheaper than whole-instance enumeration. The `--max-vars` guard applies to
each connected component.

The backend owns a local component cache keyed by the deterministic component
subinstance data. Cache entries carry a compact fingerprint for quick rejection
before exact key comparison. Components up to five variables are exhaustively
relabelled before lookup, so mirrored or otherwise isomorphic small components
reuse the same cached result. Repeated components reuse cached residue-count
vectors before convolution. `sop-solve --format stats --backend components`
reports component count, cache hits, cache misses, and brute-force leaves solved
on cache misses.

### Residual Branch-And-Sum

`qsop_solve_residual_branch` builds a mutable residual copy of the QSOP and
recursively branches on active variables. A reversible trail supports checkpoints
and undo without copying the full residual state at every branch.

The residual stores immutable per-variable incident-edge lists, so branch
mutation and split estimates walk local adjacency instead of scanning every edge
for each active variable. Active edge, active variable, and active-degree state
use the reversible trail; active-degree queries are O(1).

Residual states expose a deterministic fingerprint over the active graph,
constant, and active unary labels. The branch backend owns a local bucketed
exact memo cache keyed by that fingerprint plus full active-state comparison, so
cache hits reuse residue-count vectors without depending on hash uniqueness.

The current branch heuristic ignores isolated active variables while quadratic
edges remain, then estimates how many active residual components would remain
after removing each interacting candidate variable. It uses active degree and
unary-label presence as tie breakers. When a branch leaves no active quadratic
edges, the backend collapses the remaining independent unary variables with a
residue-table update instead of branching through each isolated variable. When a
residual graph splits into multiple active components, the backend solves each
component as a zero-constant residual subproblem, convolves their residue-count
vectors, and applies the parent residual constant once. The backend reports
internal node, cache hit/miss, and leaf counters through the stats-aware solve
API and `sop-solve --format stats`.

All solver backends also accept an optional trace callback. `sop-solve --trace
csv` emits coarse phase rows to stderr while preserving the requested primary
output on stdout. Current trace phases include brute-force enumeration,
component labelling, component-cache lookup, component subsolves, convolution,
branch cache lookup, branch variable selection, residual component splitting,
and edge-free residue-table leaves.

## Command-Line Contract

The utilities should stay small and scriptable:

- read a path or `-` for stdin;
- write primary output to stdout;
- write diagnostics to stderr;
- use long options for nontrivial behavior;
- fail clearly on unsupported formats, invalid parser input, and solver limits.

Currently implemented commands:

```text
sop-check   validate and canonicalize a QSOP file
sop-stats   print structural statistics as text or JSON
sop-solve   compute exact residue-count vectors, solver counters, or trace rows
qasm2sop    import a small static OpenQASM 2.0 subset to canonical QSOP
```

## OpenQASM Import

The current importer is `qasm2sop`. It supports explicit fixed
input/output bitstrings through `--input` and `--output`, defaulting omitted
boundaries to the original all-zero 0-to-0 amplitude behavior. It accepts a
deliberately small static OpenQASM 2.0 subset:

- `OPENQASM 2.0`;
- `include` directives, ignored after syntax recognition;
- `qreg` declarations;
- `barrier`, ignored;
- primitive one-qubit gates `id`, `h`, `t`, `tdg`, `s`, `sdg`, and `z`;
- finite `u1(...)` and `p(...)` phase calls for symbolic or decimal multiples
  of `pi/4`;
- finite `rz(...)` phase calls for symbolic or decimal multiples of `pi/4`;
- finite `rx(...)` and `ry(...)` axis rotations for symbolic multiples of
  `pi/4` or decimal equivalents;
- finite `u2(...)` and `u3(...)` one-qubit calls whose parameters are symbolic
  multiples of `pi/4`, lowered through exact `rz`/`ry` decompositions plus the
  OpenQASM global phase;
- indexed or whole-register operands for supported one-qubit gates;
- indexed or matching whole-register operands for supported two-qubit gates;
- primitive two-qubit `cz` and `swap`;
- primitive two-qubit `iswap`, lowered to `cz`, `swap`, and `s` phases;
- finite controlled phase calls `cu1(...)` and `cp(...)` for symbolic multiples
  of `pi/4`;
- finite `crz(...)` phase calls for symbolic multiples of `pi/4`;
- named controlled phase gates `cs`, `ct`, `csdg`, and `ctdg`;
- decomposition-backed gates `x`, `y`, `sx`, `sxdg`, `cx`, and `cy`, lowered
  to the primitive gate set;
- three-qubit `ccz`, lowered with the seven-phase parity identity where the
  two-bit parity phases are emitted directly as quadratic terms and only the
  three-bit parity phase is computed with CNOTs;
- three-qubit `ccx`, lowered as `h` on the target, `ccz`, then `h` on the
  target;
- three-qubit `cswap`, lowered as `cx right,left`, `ccx control,left,right`,
  then `cx right,left`.

Unsupported classical or dynamic features such as `creg`, `measure`, `reset`,
and `if` fail with line-numbered diagnostics. The importer currently stores
phase coefficients internally in `Z_16`, emits compact `Z_8` QSOP whenever all
coefficients are even, and widens to `Z_16` for half-step global phases such as
`rz(pi/4)`. This is an importer resolution choice, not a core maximum modulus:
the QSOP format and solver remain parameterized by even modulus `r`.
Contradictory fixed boundaries become a valid zero amplitude, and generated
QSOP is canonicalized through the normal parser and writer.

Importer tests include both canonical QSOP golden files and dependency-free
amplitude checks. The amplitude checks run `qasm2sop` for fixed input/output
boundaries, solve the resulting QSOP with `sop-solve`, and compare against a
small Python state-vector simulator for the supported static subset.
Optional Qiskit comparisons reuse the same fixed-boundary approach behind
`-Dqiskit_tests=true`; they are not part of the default suite because Qiskit is
an external dependency.

## Benchmarking

`tools/bench_qasm_corpus.py` runs the manifest-backed QASM solver corpus through
`qasm2sop` and one or more `sop-solve` backends. It emits JSONL by default, with
CSV available for spreadsheets. Each record includes case and boundary labels,
source and normalized QSOP hashes, QSOP size, import and solve wall-clock
timings, backend counters, and optional aggregated trace summaries collected
from `sop-solve --trace csv`.

The default CI suite includes a one-boundary benchmark smoke test to keep the
runner working without turning performance measurement into a noisy gate.

## External Translation Notes

ZX support should start as an optional boundary utility rather than a core
dependency. PyZX documents circuit loading for QASM, Quipper ASCII, `.qc`, and
qsim formats, circuit-to-graph conversion, and reversible ZX-diagram
serialization through Quantomatic `.qgraph` JSON via `from_json`/`to_json`
(https://pyzx.readthedocs.io/en/latest/representations.html). Its graph model
uses boundary, Z, X, and H-box vertices, rational phases in units of `pi`, and
simple or Hadamard edges
(https://pyzx.readthedocs.io/en/latest/graph.html). The most practical first
`zx2sop` path is therefore PyZX-backed: load `.qgraph` or a supported circuit
format, extract or convert circuit-like diagrams to OpenQASM, then reuse
`qasm2sop`. `tools/qgraph2qasm.py` starts this path as an optional PyZX-backed
adapter. Direct graph-like phase-gadget import can come later.

Kuyanov and Kissinger's low-rank-width ZX simulation work points at PyZX's
`pyzx/rank_width.py` implementation, including `rw-greedy-linear`,
`rw-greedy-b2t`, and `rw-flow` routines
(https://arxiv.org/abs/2603.06764 and
https://github.com/zxcalc/pyzx/blob/5f5e409/pyzx/rank_width.py). The associated
structured-circuit benchmarks are the PyZX `circuits` corpus, mainly T-Par
derived `.qc` files plus a QASM subset. A shallow local checkout at
`/tmp/dlx4sop-pyzx` currently has 214 `.qc`, 132 `.qasm`, and one `.qgraph`
benchmark/circuit file under `circuits`; the QASM subset can be scanned now,
while `.qc`/direct ZX ingestion needs the optional PyZX conversion path.

FeynmanDD's public repository uses OpenQASM circuit files plus a gate-set JSON
file passed with `-g`, for example `cudd_circuit_bdd -f ...qasm -g
gate_sets/google.json`
(https://github.com/cqs-thu/feynman-decision-diagram/blob/master/README.md).
Its gate-set JSON files define a modulus, primitive gate SOP terms, and named
complex-gate expansions such as `h`, `t`, `cz`, `cx`, `ccx`, `rz(pi/4)`, and
`iswap`
(https://github.com/cqs-thu/feynman-decision-diagram/blob/master/gate_sets/T.json
and
https://github.com/cqs-thu/feynman-decision-diagram/blob/master/gate_sets/google.json).
For compatibility, the natural first target is a `qasm2sop`/benchmark adapter
that can ingest FeynmanDD benchmark `.qasm` files and, later, emit
FeynmanDD-compatible OpenQASM plus a matching gate-set JSON for external
baseline runs.

The initial local scan uses a shallow FeynmanDD checkout under `/tmp` and
`tools/scan_feynmandd_qasm.py`. In the `benchmark/exp` subtree, the importer now
accepts all 152 currently quadratic Google-style cases found in the scan. Across
the wider non-invalid FeynmanDD checkout, 402 of 425 QASM files import; remaining
failures are dynamic/classical circuits, malformed register names, custom-gate
syntax outside the current static subset, or malformed Shor output.

The PyZX QASM subset is also useful as an external regression set. In the local
checkout, `circuits/feyn_bench/qasm` imports 44 of 65 non-invalid QASM files;
the remaining files are dynamic/classical examples, generic custom-gate syntax,
or malformed Shor output containing statements such as bare `H ;` and
multi-operand one-qubit gates. Across all PyZX `circuits` QASM files, 109 of 130
non-invalid files import with the same remaining categories.

## CI And Coverage

CI runs on GitHub Actions with:

- Meson configure and test;
- a coverage build using Meson `b_coverage=true`;
- `gcovr` line coverage over production `src` files;
- a default 75% line coverage gate through `tools/check-coverage.sh`.

The coverage gate is intended to keep CLI and core behavior covered while the
solver is still small enough for cheap exact tests.

Parser fuzz targets are available behind `-Dbuild_fuzzers=true`. Optional
Qiskit comparisons are available behind `-Dqiskit_tests=true`. Neither is part
of the normal CI path initially; the current parser target replays arbitrary
bytes through the QSOP parser and uses canonical writer idempotence as its
oracle.

## Forward Direction

The next solver targets are benchmark-driven residual improvements: use trace
and corpus data to decide whether incremental component metadata, incremental
hashing, or dancing-cells-style adjacency mutation should come first. New
importer work should be driven by gates found in real circuit sources and should
keep each added gate covered by boundary-level examples and amplitude checks.

External tools such as OpenQASM, MQT, ZX, WMC, and FeynmanDD should remain
import/export targets rather than runtime dependencies of the core solver.
