# dlx4sop Architecture

`dlx4sop` is built around one intermediate representation: a normalized
finite-modulus quadratic sum of powers (QSOP). The project target is a
competitive exact strong simulator based on labelled quadratic SOPs. External
circuit, graph, and benchmark formats are boundary formats. They are imported
into QSOP before the core solver sees them.

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
  solves components independently, caches repeated small components, and
  convolves residue histograms. It is the default backend and uses the same
  CRT-backed large-count result contract as the decomposition backends.
- `branch`: mutates a reversible residual state, branches on active variables,
  memoizes repeated residuals, splits residual components, and collapses
  edge-free unary tails. Active incidence is maintained with reversible
  unlink/relink cells so backtracking avoids walking deleted edges. It uses
  fixed-width counts when safe and a CRT-backed outer loop for larger final
  histograms.
- `rankwidth`: decomposition DP with generated or supplied decompositions,
  sign/labelled count-table mode, CRT-backed larger histograms, and a
  small-instance sign-only Fourier mode.
- `treewidth`: bucket-elimination DP over dense factors, with
  `min-fill|min-degree|min-fill-max-degree` variable orders, a bag guard, and
  CRT-backed larger histograms.

The rankwidth backend uses bitset-backed signatures for sign-only instances and
`Z_r` boundary-signature vectors for labelled instances. Practical limits are
solver guards, memory, and decomposition width rather than a single machine-word
mask. Exact result counts use the normal `uint64_t` fast path when possible and
a CRT-backed path for larger final histograms in branch, components, rankwidth
count-table mode, and treewidth. Brute force remains a small-instance oracle
with a hard variable guard.

Current branch variable-ordering policies are heuristics, not decomposition
solvers:

- `split`: default residual component split estimate;
- `treewidth`: min-fill estimate on the active graph;
- `linear-rankwidth`: historical name for a local GF(2) cut-rank proxy. It is a
  branch ordering heuristic, not a certified computation of the graph parameter
  linear rankwidth.

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
- summary reports distinguish sign-only imports, labelled imports, solver
  skips, source families, backend-specific rankwidth/treewidth width and table
  metrics, and largest case-boundaries;
- optional comparison scripts can use external frameworks outside the C core.

## Labelled Rankwidth Direction

The labelled count-table DP uses the cut-signature width discussed in the design
note:

```text
Sigma_X_to_Y(Q) = { x_X^T Q[X,Y] : x_X in {0,1}^X } subset Z_r^Y
Sigma_Y_to_X(Q) = { Q[X,Y] x_Y : x_Y in {0,1}^Y } subset Z_r^X
s_Q(X|Y) = max(|Sigma_X_to_Y(Q)|, |Sigma_Y_to_X(Q)|)
lambda_Q(X|Y) = ceil(log2 s_Q(X|Y))
```

For sign-only SOPs this reduces to the familiar GF(2) cut-rank view. For
labelled SOPs, count-table states are keyed by labelled boundary signatures
rather than parity-only signatures. Remaining work is better labelled
decomposition heuristics and deciding whether Fourier mode should be generalized
to labelled signatures.

## CI Contract

CI should keep the default Meson test suite lean. The current contract is:

- build and run C/Python tests;
- enforce the configured line-coverage threshold;
- keep optional fuzzing and heavyweight framework comparisons outside the
  default fast path unless they become cheap and reliable.
