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

Tests are split between Python CLI golden tests and C unit tests for residue and
residual behavior. Optional parser fuzzing is available as a separate Meson
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
subinstance data. Repeated components reuse cached residue-count vectors before
convolution. `sop-solve --format stats --backend components` reports component
count, cache hits, cache misses, and brute-force leaves solved on cache misses.

### Residual Branch-And-Sum

`qsop_solve_residual_branch` builds a mutable residual copy of the QSOP and
recursively branches on active variables. A reversible trail supports checkpoints
and undo without copying the full residual state at every branch.

The current branch heuristic first estimates how many active residual components
would remain after removing each candidate variable. It then uses active degree
and unary-label presence as tie breakers. The backend reports internal node and
leaf counters through the stats-aware solve API and `sop-solve --format stats`.

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
sop-solve   compute exact residue-count vectors or solver counters
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
- finite `u1(...)` and `p(...)` phase calls for symbolic multiples of `pi/4`;
- indexed or whole-register operands for supported one-qubit gates;
- indexed or matching whole-register operands for supported two-qubit gates;
- primitive two-qubit `cz` and `swap`;
- finite controlled phase calls `cu1(...)` and `cp(...)` for symbolic multiples
  of `pi/4`;
- decomposition-backed gates `x`, `y`, `cx`, and `cy`, lowered to the
  primitive gate set.

Unsupported classical or dynamic features such as `creg`, `measure`, `reset`,
and `if` fail with line-numbered diagnostics. The importer emits raw QSOP with
boundary pins internally, treats contradictory fixed boundaries as a valid zero
amplitude, then canonicalizes through the normal QSOP parser and writer.

## CI And Coverage

CI runs on GitHub Actions with:

- Meson configure and test;
- a coverage build using Meson `b_coverage=true`;
- `gcovr` line coverage over production `src` files;
- a default 75% line coverage gate through `tools/check-coverage.sh`.

The coverage gate is intended to keep CLI and core behavior covered while the
solver is still small enough for cheap exact tests.

Parser fuzz targets are available behind `-Dbuild_fuzzers=true`. They are not
part of the normal CI path initially; the current parser target replays arbitrary
bytes through the QSOP parser and uses canonical writer idempotence as its oracle.

## Forward Direction

The next importer targets are broader OpenQASM compatibility while keeping each
added gate covered by boundary-level examples. Candidate additions should either
lower cleanly to the supported primitive gates or have a direct QSOP
representation.

External tools such as OpenQASM, MQT, ZX, WMC, and FeynmanDD should remain
import/export targets rather than runtime dependencies of the core solver.
