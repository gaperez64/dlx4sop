# dlx4sop Architecture

`dlx4sop` is built around one intermediate representation: a normalized
finite-modulus quadratic sum of powers (QSOP). External circuit, graph, and
benchmark formats are boundary formats. They are imported into QSOP before the
core solver sees them.

Performance ideas that are not yet part of the stable command-line contract are
kept in [ARCHITECTURE_SPEED_ANNEX.md](ARCHITECTURE_SPEED_ANNEX.md).

## QSOP Model

A QSOP instance represents

```text
Z = 2^(-h/2) omega_r^c
    sum_{x in {0,1}^n} omega_r^(
      sum_i b_i x_i + sum_{i<j} q_ij x_i x_j
    )
```

where `r` is even, `omega_r = exp(2*pi*i/r)`, `h` is the normalization exponent,
`c` is a constant phase exponent, and all unary and quadratic coefficients are
stored modulo `r`.

The exact solver result is a histogram over phase residues:

```text
count[a] = number of assignments with exponent a mod r
```

The `count` values are assignment cardinalities, not coefficients reduced
modulo `r`.

## Text Format

The canonical text format supports comments beginning with `c`, dense variables
`0..n-1`, and these records:

```text
p qsop <r> <num_variables> <num_quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
f <vertex> <0|1>
```

`p qsop-sign` plus `e u v` is the sign-only shortcut for `q u v r/2`. Pins are
parser-level reductions: pinned variables are folded into the constant and
neighboring unary labels, then the remaining variables are remapped densely.

## Canonicalization

Parser and writer output is deterministic:

- coefficients accumulate modulo `r`;
- quadratic endpoints are ordered as `u < v`;
- zero quadratic terms are removed;
- sign-edge duplicates cancel modulo two;
- self-loops are folded into unary terms because `x_v^2 = x_v`;
- pins are applied before the final dense remapping.

This stable form is the basis for golden tests, component cache keys, and future
cross-tool interchange.

## Core Boundaries

The C core owns QSOP storage, normalization, exact solving, and command-line
utilities. Python utilities are kept at the boundary for benchmark conversion,
manifest generation, and comparison against external frameworks.

The main public headers are:

- `qsop.h`: normalized QSOP representation and parse/write APIs;
- `qsop_stats.h`: structural statistics;
- `qsop_solve.h`: exact solver APIs and stats;
- `residue.h`: residue-vector helpers;
- `residual.h`: reversible residual state used by the branch solver.

The core should not depend on Qiskit, PyZX, MQT, FeynmanDD, or benchmark
repositories.

## Solver Backends

Implemented exact backends:

- `bruteforce`: enumerates all assignments and is used as the small-instance
  oracle.
- `components`: splits the quadratic support graph into connected components,
  solves components independently, and convolves residue histograms.
- `branch`: mutates a reversible residual state, branches on active variables,
  memoizes repeated residuals, splits residual components, and collapses
  edge-free unary tails.
- `rankwidth`: sign-only decomposition DP with generated or supplied
  decompositions, count-table mode, and a small-instance Fourier mode.

The rankwidth backend uses bitset-backed signatures; practical limits are now
solver guards, memory, and decomposition width rather than a single machine-word
mask. Exact result counts use the normal `uint64_t` fast path when possible and
a CRT-backed path for larger final histograms.

Current branch variable-ordering policies are heuristics, not decomposition
solvers:

- `split`: default residual component split estimate;
- `treewidth`: min-fill estimate on the active graph;
- `linear-rankwidth`: local GF(2) cut-rank proxy.

## OpenQASM Import

`qasm2sop` intentionally accepts a static OpenQASM 2 subset with fixed input and
output boundaries. Unsupported dynamic behavior, measurement/control flow, and
non-quadratic gates fail explicitly unless a local quadratization is implemented.

The importer emits QSOP over the required modulus for the translated circuit.
The existing Clifford+T path commonly lands in `Z_16`; that is an importer
choice, not a hard global modulus limit.

## Benchmarking And Corpus Tools

The repository keeps importer-fed benchmark manifests so solver changes can be
measured on both local examples and external benchmark pools without vendoring
large third-party trees into the core.

Relevant tool boundaries:

- manifest builders discover external corpus files and record import status;
- scanners classify unsupported QASM constructs;
- benchmark runners execute selected solver configurations and collect stats;
- optional comparison scripts can use external frameworks outside the C core.

## Labelled Rankwidth Direction

The implemented rankwidth DP is sign-only. The labelled QSOP version should use
the cut-signature width discussed in the design note:

```text
Sigma_X_to_Y(Q) = { x_X^T Q[X,Y] : x_X in {0,1}^X } subset Z_r^Y
Sigma_Y_to_X(Q) = { Q[X,Y] x_Y : x_Y in {0,1}^Y } subset Z_r^X
s_Q(X|Y) = max(|Sigma_X_to_Y(Q)|, |Sigma_Y_to_X(Q)|)
lambda_Q(X|Y) = ceil(log2 s_Q(X|Y))
```

For sign-only SOPs this reduces to the familiar GF(2) cut-rank view. For
labelled SOPs, future DP tables should be keyed by labelled boundary signatures
rather than parity-only signatures.

## CI Contract

CI should keep the default Meson test suite lean. The current contract is:

- build and run C/Python tests;
- enforce the configured line-coverage threshold;
- keep optional fuzzing and heavyweight framework comparisons outside the
  default fast path unless they become cheap and reliable.
