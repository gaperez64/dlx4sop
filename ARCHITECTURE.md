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
after removing each interacting candidate variable. It prefers material
reductions in the largest remaining split component before falling back to
active degree and unary-label tie breakers. Small one-variable balance changes
are ignored because, on the current benchmark corpus, they can create extra
nontrivial component subsolves without reducing search. When a branch leaves no
active quadratic edges, the backend collapses the remaining independent unary
variables with a residue-table update instead of branching through each isolated
variable. When a residual graph splits into multiple active components, the
backend solves each component as a zero-constant residual subproblem, convolves
their residue-count vectors, and applies the parent residual constant once. The
backend reports internal node, cache hit/miss, and leaf counters through the
stats-aware solve API and `sop-solve --format stats`.

`sop-solve --backend branch --branch-heuristic ...` exposes experimental
variable-choice policies for benchmarking. `split` is the default described
above. `treewidth` uses a min-fill estimate on the active residual graph.
`linear-rankwidth` uses a local GF(2) cut-rank proxy between the active
neighbors of a candidate and the rest of the active graph. These are heuristic
ordering policies for the residual branch backend, not decomposition-based
solvers.

### Rankwidth Count-Table DP

`qsop_solve_rankwidth_mode_trace_stats` is the first decomposition-based
backend. Its default count-table mode implements the direct dynamic program from
arXiv:2605.29944 over a rooted binary rank decomposition. Tables are sparse maps
keyed by a boundary signature, represented as a GF(2) bit mask over the outside
vertices, and a residue modulo `r`. Leaves add the two assignments of one QSOP
variable. Join nodes combine child states, restrict/xor the child boundary
signatures to the parent outside set, add the sign cross-term determined by
representative child signatures, and accumulate residue-count vectors.

The backend can read an explicit decomposition file or generate one internally:
linear input order, balanced input order, min-fill order with a balanced tree,
or min-fill order with recursive split points chosen by a GF(2) cut-rank score.
The cut-aware splitter keeps the min-fill elimination order but, for each
contiguous range, chooses the child split that minimizes the maximum child
cut-rank and then prefers balanced splits. It is deliberately narrow: it
requires sign-only quadratic coefficients and mask-backed instances under the
solver variable guard. It reports decomposition width, total and maximum table
entries, total and maximum signature entries, residue-pair joins, and
signature-pair joins through `sop-solve --format stats`.

The `QSOP_RANKWIDTH_SOLVE_FOURIER` mode is an exact modular-DFT variant. It
chooses a 64-bit NTT prime `p = 1 mod r` larger than the assignment count, keeps
one value per boundary signature per Fourier mode modulo `p`, and inverts the
transform at the root. Under the existing 63-variable guard, recovered counts
are exact whenever such a prime is found.

The labelled-QSOP rankwidth extension should not use ordinary rankwidth of the
unlabelled support graph as its primary parameter. The deferred parameter from
the June 2026 QSOP models note is labelled cut-signature width. For a labelled
coefficient matrix `Q` over `Z_r` and a cut `X|Y`, define the directed signature
set

```text
Sigma_X_to_Y(Q) = { x_X^T Q[X,Y] : x_X in {0,1}^X } subset of Z_r^Y.
```

Because row and column subset-sum sets can have different cardinality over
`Z_r`, the cut size is

```text
s_Q(X|Y) = max(|Sigma_X_to_Y(Q)|, |Sigma_Y_to_X(Q)|)
lambda_Q(X|Y) = ceil(log2 s_Q(X|Y)).
```

For a branch decomposition `T`, the labelled width is the maximum
`lambda_Q(X_e | V \ X_e)` over decomposition cuts. This reduces to ordinary
GF(2) cut-rank for sign-edge QSOPs, where the signatures are exactly the binary
row space scaled by `r/2`. This distinction matters because gadgetizing labelled
edges into sign edges can preserve treewidth while destroying rankwidth. A
future labelled backend should therefore key tables by labelled signature IDs,
with representative assignments used to precompute parent signatures and join
cross phases; the current backend intentionally stays sign-only.

The decomposition text format is:

```text
p rwdec <variables> <nodes> <root>
l <node> <variable>
j <node> <left-child> <right-child>
```

The root must cover every QSOP variable exactly once. Node identifiers are dense
`0..nodes-1`, but records may appear in any order.

All solver backends also accept an optional trace callback. `sop-solve --trace
csv` emits coarse phase rows to stderr while preserving the requested primary
output on stdout. Current trace phases include brute-force enumeration,
component labelling, component-cache lookup, component subsolves, convolution,
branch cache lookup, branch variable selection, residual component splitting,
edge-free residue-table leaves, rankwidth leaf/join-map/join construction, and
rankwidth Fourier leaf/join-map/join construction.

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
  of `pi/8`;
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
  of `pi/8`;
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
`rz(pi/4)` and direct eighth-turn phase gates such as `p(pi/8)` or `cp(pi/8)`.
This is an importer resolution choice, not a core maximum modulus: the QSOP
format and solver remain parameterized by even modulus `r`.
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
CSV available for spreadsheets and a text summary for quick cache/trace
inspection. Each record includes case and boundary labels, source and normalized
QSOP hashes, QSOP size, import and solve wall-clock timings, backend counters,
and optional aggregated trace summaries collected from `sop-solve --trace csv`.
The summary mode aggregates per-backend cache hit rates, leaf/search counters,
and trace phase event/item/elapsed totals. It can also rank the largest
case-boundary records per backend by a selected metric such as branch search
nodes, leaf assignments, cache misses, or wall-clock time; trace-enabled ranked
rows include the dominant trace phase for that record.
For rankwidth experiments, repeated `--rankwidth-generate` and
`--rankwidth-mode` flags expand into multiple configuration records, and
`--rankwidth-sweep` runs all generated decomposition and solve-mode
combinations with separate summary blocks.
`tools/build_external_qasm_manifest.py` materializes compatible manifests from
external QASM roots and optional `.qc` translations by first checking
`qasm2sop` importability for selected fixed-boundary amplitudes and filtering
out cases above an explicit solver variable guard. For strong-simulation
benchmark ingestion, it can explicitly strip `creg` declarations and terminal
`measure` statements before import; it still rejects mid-circuit dynamic
control such as `if` or `reset`. It can also inline simple non-parameterized
OpenQASM gate definitions as a boundary preprocessing step when those macro
bodies are made only of supported static operations.

`sop-stats` width diagnostics use a 64-bit mask fast path on small instances
and a bitset-backed path on larger instances. This lets external benchmark
imports above 63 variables be inspected for min-fill width, fill-edge count, and
linear cut-rank even though exact solver result counts are still limited by the
current `uint64_t` residue-count representation.

The default CI suite includes one-boundary benchmark smoke tests to keep the
runner and summary format working without turning performance measurement into
a noisy gate.

Current rankwidth benchmark evidence is corpus-dependent. On the small
checked-in sign-only slice, `min-fill-cut` reduces table growth versus the older
generated decompositions. On the larger PyZX-derived rankwidth-compatible slice,
plain linear decompositions currently have lower width and smaller maximum
tables than the min-fill variants. Count-table mode is structurally lighter on
small cases, while Fourier can be faster on larger cases where residue-pair
joins dominate. Rankwidth defaults should therefore be changed only after
sweeping generator and mode combinations on the target corpus.

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
adapter. The `.qc` format is different: it is a T-Par/PyZX circuit text format,
not a ZX graph serialization. `tools/qc2qasm.py` provides a dependency-free
`.qc` circuit bridge into OpenQASM for benchmark ingestion, while direct
graph-like `.qgraph` phase-gadget import can come later.

Kuyanov and Kissinger's low-rank-width ZX simulation work points at PyZX's
`pyzx/rank_width.py` implementation, including `rw-greedy-linear`,
`rw-greedy-b2t`, and `rw-flow` routines
(https://arxiv.org/abs/2603.06764 and
https://github.com/zxcalc/pyzx/blob/5f5e409/pyzx/rank_width.py). The associated
structured-circuit benchmarks are the PyZX `circuits` corpus, mainly T-Par
derived `.qc` files plus a QASM subset. A shallow PyZX checkout inspected during
development had 214 `.qc`, 132 `.qasm`, and one `.qgraph` benchmark/circuit file
under `circuits`; the QASM subset can be scanned now, while the `.qc` circuit
subset can be translated through `tools/qc2qasm.py`.
Direct `.qgraph` ZX ingestion still needs the optional PyZX conversion path or a
future native graph importer.

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

The initial local scan used a shallow FeynmanDD checkout and
`tools/scan_feynmandd_qasm.py`. In the `benchmark/exp` subtree, the importer
now accepts all 152 currently quadratic Google-style cases found in the scan.
Across the wider non-invalid FeynmanDD checkout, 402 of 425 QASM files import;
remaining failures are dynamic/classical circuits, malformed register names,
custom-gate syntax outside the current static subset, or malformed Shor output.

The PyZX QASM subset is also useful as an external regression set. In the local
checkout, `circuits/feyn_bench/qasm` imports 44 of 65 non-invalid QASM files;
the remaining files are dynamic/classical examples, generic custom-gate syntax,
or malformed Shor output containing statements such as bare `H ;` and
multi-operand one-qubit gates. Across all PyZX `circuits` QASM files, 109 of 130
non-invalid files import with the same remaining categories.

For PyZX `.qc`, the dependency-free translator mirrors the local PyZX
`qcparser.py` rules for one-qubit gates, `cnot`, `swap`, multi-arity `tof`, and
multi-target `Z`/`Zd` lines. It translates 203 of 214 local `.qc` files; the 11
misses are ten empty `.qc` files and one malformed Shor file. Sample translated
files such as `tof_3_tpar.qc`, `grover_5.qc`, and `ham15-low.qc` import through
`qasm2sop`.

MQT Bench generates benchmark circuits through its Python package and can export
OpenQASM 2.0, OpenQASM 3.0, or QPY rather than vendoring one flat QASM corpus
in its source tree. `tools/scan_mqt_bench.py` keeps that dependency optional:
it can use an installed `mqt.bench` package or a local checkout supplied with
`--mqt-source`, exports generated circuits with Qiskit's QASM2 dumper, strips
terminal measurements by default for strong-simulation amplitude imports, and
then classifies `qasm2sop` outcomes. A shallow MQT Bench checkout inspected
during development gave 7/8 imports on the default size-3 target-independent
subset; the miss is `wstate`, whose generated `ry` angle is not a finite `pi/4`
multiple. Across the default size-3/4 algorithm and target-independent subset,
23 of 32 generated cases import after direct eighth-turn phase support. A
broader size-3/4 target-independent sweep imports 16 generated cases and groups
the rest into generation constraints, QASM2 dump limitations, custom-gate
syntax, and unsupported non-finite angles.

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

The next solver targets are benchmark-driven width work. The current
`rankwidth` backend now has sign-only count-table and exact Fourier checkpoints
from the 2026 arXiv paper "Quadratic Sums-of-Powers for Fixed-Parameter
Tractable Quantum-Circuit Simulation" (arXiv:2605.29944). The next backend
milestones are broadening imported sign-only benchmark coverage, improving
decomposition quality beyond min-fill and cut-rank split heuristics, replacing
linear signature lookups with indexed maps when traces justify it, and
broadening exact Fourier support if non-default moduli expose NTT-prime
limitations.

Branch heuristic experiments should keep using trace and corpus data to decide
whether treewidth min-fill, local cut-rank/linear-rankwidth proxies,
incremental component metadata, incremental hashing, or dancing-cells-style
adjacency mutation should come first. New importer work should be driven by
gates found in real circuit sources and should keep each added gate covered by
boundary-level examples and amplitude checks. Labelled rankwidth work is
deferred until sign-only decomposition machinery is measurable on a broader
benchmark set, and should use labelled cut-signature width rather than ordinary
support-graph rankwidth.

External tools such as OpenQASM, MQT, ZX, WMC, and FeynmanDD should remain
import/export targets rather than runtime dependencies of the core solver. The
current Python tools sit at that boundary because they mostly manage external
text formats, optional Python packages, subprocess scans, JSON reports, and
benchmark checkouts; stable hot paths can be ported to C later once their
supported subsets stop moving.
