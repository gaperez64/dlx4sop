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
normalization rules, and backend design notes.

## Implemented Utilities

- `sop-check`: parse, validate, normalize, and write canonical QSOP.
- `sop-stats`: print structural statistics in text or JSON.
- `sop-solve`: compute exact residue-count vectors or solver stats with one of
  three backends:
  - `components` (default): decompose connected components, cache repeated
    component solves, and brute-force each cache miss;
  - `brute-force`: enumerate all assignments directly;
  - `branch`: recursive residual branch-and-sum using a reversible trail and a
    split-aware variable heuristic.

The test suite also covers reusable residue-vector helpers and the mutable
residual state used by the branch backend.

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
```

Solve a QSOP and print residue counts modulo `r`:

```sh
build/sop-solve tests/golden/solve_labelled.qsop
build/sop-solve --backend brute-force tests/golden/solve_labelled.qsop
build/sop-solve --backend branch tests/golden/solve_labelled.qsop
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
search_nodes: 7
leaf_assignments: 4
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

## Current Direction

The next implementation target is stronger algebraic tests and parser fuzzing.
