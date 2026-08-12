# dlx4sop

`dlx4sop` is a C/Meson toolkit for finite-modulus quadratic sums of powers
(QSOPs). It imports supported OpenQASM 2.0 circuits, computes fixed-boundary
amplitudes, and exports weighted model counting instances.

QSOP is the shared intermediate representation. For amplitude computation,
`sop-solve --backend branch --solve-mode auto` is the recommended default. The
branch backend simplifies and splits each residual, then chooses among
treewidth, rank-width, and quadratic phase function (QPF) methods by estimated
cost. It branches or conditions only when no admitted delegate wins.

## Tools

- `qasm2sop` imports supported OpenQASM 2.0 circuits into QSOP.
- `sop-check` validates, pin-reduces, and canonicalizes QSOP files.
- `sop-stats` reports structural, width, and QPF diagnostics.
- `sop-solve` computes amplitudes or exact residue-count histograms.
- `sop2wmc` exports DIMACS CNF or weighted DIMACS CNF (WPCNF) for external
  weighted model counters.

Prebuilt releases provide these commands in Linux x86-64 and macOS arm64
archives with SHA-256 sidecar files.

## QSOP format

```text
p qsop-sign <r> <variables> <sign_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
f <vertex> <0 | 1>
```

Quadratic terms are sign edges with implicit coefficient `r/2`. Duplicate sign
edges cancel by parity. Pins (`f`) are applied during parsing, and canonical
output uses dense variable IDs. Solver counts are ordinary assignment counts
grouped by phase residue modulo `r`.

## Common usage

```sh
build/qasm2sop --input 1 --output 1 circuit.qasm > circuit.qsop
build/sop-check circuit.qsop
build/sop-stats --format json circuit.qsop
build/sop-solve --backend branch --solve-mode auto circuit.qsop
build/sop-solve --format residue-vector circuit.qsop
build/sop2wmc --encoding auto circuit.qsop | ganak --mode 6 --verb 0 -
```

With amplitude output, `--solve-mode auto` prefers exact residue counting when
it is practical and otherwise evaluates one Fourier coefficient with a
certified floating-point error bound. `--format residue-vector` requests the
exact count-table path. Direct `treewidth`, `rankwidth`, and `qpf` backends are
available for controlled comparisons and specialized use.

### Approximate OpenQASM imports

Import is exact by default. A circuit is rejected when a phase lies outside
the supported finite grid. `--approx EPS` opts into phase rounding with a
positive additive amplitude-error budget:

```sh
build/qasm2sop --approx 1e-6 --input 0 --output 0 circuit.qasm
```

The emitted QSOP comments record the selected modulus, the number of rounded
phases, and the certified additive error bound. The solver handles the large
moduli produced by approximate imports in single-Fourier mode.

### Weighted model counting export

`sop2wmc --encoding auto` selects an amplitude encoding and emits the metadata
needed to reconstruct the normalized amplitude. Concrete amplitude, residue,
Fourier, and block encodings remain available for controlled experiments. See
[Solver internals](docs/solver-internals.md#weighted-model-counting-export) for
their contracts and reconstruction rules.

## Build and test

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The default build is a debug build with assertions enabled. Use
`meson setup build-release --buildtype=release` for performance runs. CI also
checks at least 75 percent line coverage over production `src` files through
`scripts/check-coverage.sh`.

## Verification and benchmarks

The [Lean 4 formalization](docs/lean/README.md) covers correctness and operation
counts for the rank-decomposition dynamic program, its Fourier-mode and linear
layout variants, and their connection to Clifford+T circuit amplitudes. The
formalization is Apache-2.0 licensed under [docs/lean/LICENSE](docs/lean/LICENSE).

The public [qccq-gauntlet leaderboard](https://qccq-cgd.pages.dev/) compares
the native backends and the `sop2wmc` plus Ganak pipeline on shared suites.

For dispatch rules, advanced flags, cost estimates, join kernels, and tracing,
see [Solver internals](docs/solver-internals.md) and
`sop-solve --help-advanced`.
