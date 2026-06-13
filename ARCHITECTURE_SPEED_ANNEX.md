# Annex A: Performance Direction

This annex records performance-oriented design constraints and future directions
that are intentionally more speculative than the command-line contract in
`ARCHITECTURE.md`. The main principle remains: keep the hot path as a dense,
integer-only, coefficient-labelled graph problem, and push parsing, conversion,
provenance, and competitor interoperability to separate Unix-style utilities.

The annex assumes the implemented core architecture described in
`ARCHITECTURE.md`: C23, Meson, small command-line tools, exact finite-modulus
quadratic SOPs, and two internal QSOP modes:

- **sign mode**, where each quadratic coefficient is either `0` or `r/2`;
- **labelled mode**, where each quadratic coefficient is an arbitrary residue in `Z_r`.

Native degree greater than 2 remains out of scope for the current solver.
Higher-degree gates such as `CCZ` and Toffoli should be compiled into
constant-size labelled quadratic gadgets before solving.

---

## A.1 Make QSOP the executable IR

The native executable representation should be the normalized quadratic SOP itself, not CNF, not a Qiskit object, not a generic tensor-network object, and not a generic graph library object.

The import/export pipeline should be:

```text
OpenQASM / MQT / ZX / FeynmanDD / benchmark formats
    -> importer
    -> normalized QSOP
    -> sop-check / sop-stats / sop-solve
    -> optional exporters to WMC / FeynmanDD / ZX / QASM
```

This keeps the solver close to the object used by the FPT algorithms. External formats should be treated as boundary formats, not as internal dependencies.

The attached tensor-network-to-WMC note motivates a similar separation: circuits are first represented as a gate-level open tensor network, where wire segments are binary indices, gates are local tensors, and boundary indices are fixed for amplitudes. For `dlx4sop`, the corresponding internal object should be the normalized QSOP obtained after compiling those local gates.

---

## A.2 Keep two hot modes: `sign` and `labelled`

Labelled QSOP support should not erase the fast Clifford+T path.

Use one normalized data model, but dispatch hot kernels according to mode:

```text
sign mode:
    q_ij in {0, r/2}
    binary edge support
    GF(2) bitsets
    parity signatures
    fastest Clifford+T path

labelled mode:
    q_ij in Z_r
    coefficient-labelled edges
    mod-r signatures
    compact Toffoli, CCZ, CS, CT, sqrt(CZ), finite RZZ support
```

The loader should automatically downgrade a labelled instance to sign mode when every normalized quadratic coefficient is either `0` or `r/2`.

This is important because sign mode can use extremely compact bit-parallel operations, while labelled mode needs small modular coefficients and larger boundary signatures.

---

## A.3 Keep the core arithmetic integer-only

Internally, represent

\[
S = \sum_x \omega_r^{Q(x)}
\]

as a residue-count vector:

```c
count[0], count[1], ..., count[r - 1]
```

where `count[a]` is the number of assignments contributing phase exponent `a mod r`.

Then

\[
S = \sum_{a=0}^{r-1} \texttt{count[a]}\,\omega_r^a.
\]

All hot operations become integer operations:

```text
phase = (phase + delta) mod r
count_table[new_phase] += count_table[old_phase]
```

Complex floating-point numbers should only appear in final display, approximate export, or compatibility layers. The exact solver result should remain:

```text
normalization exponent h
constant phase c
residue-count vector over Z_r
```

Recommended primitive types:

```c
typedef uint16_t qsop_mod_t;
typedef uint16_t qsop_coeff_t;
typedef uint32_t qsop_var_t;
typedef uint32_t qsop_edge_t;
```

Use specialized kernels for common moduli:

```text
r = 2, 4, 8, 16, 24
```

and a generic fallback for other even moduli.

---

## A.4 Use structure-of-arrays layout

Avoid pointer-heavy object graphs. They are difficult to vectorize and unfriendly to cache locality.

Prefer dense arrays:

```c
qsop_var_t edge_u[M];
qsop_var_t edge_v[M];
qsop_coeff_t edge_q[M];

qsop_coeff_t unary_b[N];
bool active_var[N];
bool active_edge[M];

uint64_t adj_head[N + 1];
qsop_var_t adj_other[2 * M];
qsop_edge_t adj_edge_id[2 * M];
```

For sign mode, `edge_q` can be omitted, ignored, or stored as a constant implicit value `r/2`.

For labelled mode, every adjacency entry should carry an `edge_id`; the coefficient should live once in `edge_q[edge_id]`.

The first implementation should already follow this layout, because retrofitting SIMD and fast hashing onto pointer-heavy code is expensive.

---

## A.5 Keep reversible mutation central

The residual branch backend already mutates in place and records enough state to
undo branches. As the solver grows, new simplifications should keep this
discipline instead of reintroducing full residual copies.

Suggested trail kinds:

```c
typedef enum {
    QSOP_TRAIL_SET_UNARY,
    QSOP_TRAIL_SET_CONSTANT,
    QSOP_TRAIL_DEACTIVATE_VAR,
    QSOP_TRAIL_DEACTIVATE_EDGE,
    QSOP_TRAIL_SET_EDGE_LABEL,
    QSOP_TRAIL_HASH_XOR,
    QSOP_TRAIL_COMPONENT_MARK
} qsop_trail_kind_t;
```

A trail entry should store enough information to undo one mutation:

```c
typedef struct {
    qsop_trail_kind_t kind;
    uint32_t index;
    uint64_t old_value;
} qsop_trail_entry_t;
```

Branching on a labelled QSOP variable `v` has a simple local update:

```text
x_v = 0:
    delete v and all active incident edges

x_v = 1:
    c += b[v] mod r
    for each active neighbor u of v:
        b[u] += q[v,u] mod r
    delete v and all active incident edges
```

That update is naturally compatible with a dancing-cells design: mutate only
local cells, record mutations, and undo them in reverse order.

---

## A.6 Make incremental hashing part of mutation

Memoization should remain a first-class solver feature, not a later wrapper. The
component backend already owns a local canonical component cache with compact
entry fingerprints for fast rejection; residual-state hashing is still future
work.

Use a Zobrist-style incremental hash over the active residual state:

```text
hash ^= H_var_active[v]
hash ^= H_unary[v][old_b]
hash ^= H_unary[v][new_b]
hash ^= H_edge_active[e]
hash ^= H_edge_label[e][old_q]
hash ^= H_edge_label[e][new_q]
hash ^= H_const[old_c]
hash ^= H_const[new_c]
```

This gives cheap hash updates for reversible mutation:

- unary changes: `O(1)`;
- constant changes: `O(1)`;
- deleting a vertex: `O(deg(v))`;
- changing an edge label: `O(1)`.

Maintain at least two cache layers:

```text
global residual cache:
    active graph + unary labels + edge labels + constant + modulus

component cache:
    connected component state + local unary labels + boundary state
```

Component-level caching is especially important because variable branching often splits the active graph.

---

## A.7 Use layered canonical fingerprints

Hash equality is not enough for high cache hit rates. However, full graph isomorphism is too expensive as a default hot-path operation.

Use layered fingerprints:

```text
level 0:
    component-cache fingerprint; later, incremental Zobrist hash

level 1:
    sorted active degree / unary / label summaries

level 2:
    canonical relabelling for small components

level 3:
    optional graph-canonicalization backend later
```

Correctness should not depend on recognizing isomorphic residuals. Stronger canonicalization should only improve memoization.

---

## A.8 Separate external IDs from solver IDs

Input formats have heterogeneous identifiers: OpenQASM qubits, MQT layouts, PyZX node IDs, FeynmanDD variables, WMC variables, and benchmark labels.

Do not let these identifiers reach the hot solver.

Use separate ID layers:

```text
external IDs
    -> import map
    -> stable dense IR IDs
    -> normalized solver IDs
    -> width/order optimized solver IDs
```

The solver should see only dense integer IDs.

Optional provenance should be stored outside the hot input, for example:

```text
instance.sop
instance.sop.meta
```

where `.sop.meta` records source gates, source spans, qubit maps, auxiliary variables, and gadget origins.

---

## A.9 Normalize aggressively at load time

The parser and `sop-check` utility canonicalize instances before solving.

Perform at least:

```text
remove zero unary coefficients
combine duplicate q-edges modulo r
fold self-loops q(v,v) into unary b[v]
apply fixed pins
delete isolated variables using closed forms
split disconnected components
detect sign-only mode
sort adjacency lists
renumber variables by a chosen heuristic order
```

Some of these are implemented today by parser normalization; the remaining
items are future speed work. All of them improve later components: hashing,
memoization, component decomposition, width heuristics, and SIMD kernels.

---

## A.10 Make width heuristics pluggable

Do not hard-code a single branching heuristic such as maximum degree.

Expose a small heuristic interface:

```c
typedef struct {
    int64_t (*score_var)(const qsop_state_t *state,
                         qsop_var_t v,
                         void *ctx);
    void (*on_branch)(const qsop_state_t *state,
                      qsop_var_t v,
                      uint8_t value,
                      void *ctx);
    void (*on_backtrack)(const qsop_state_t *state,
                         void *ctx);
} qsop_heuristic_t;
```

Useful scoring functions:

```text
degree
weighted degree
min-fill estimate
component split score
treewidth proxy
rankwidth / cut-signature proxy
cache-hit likelihood
label entropy around the variable
estimated residue-table cost
```

The important architectural decision is to keep treewidth and labelled rankwidth estimates visible in the solver API.

---

## A.11 Build residue-table kernels for SIMD

Even the scalar implementation should use layouts that can be vectorized later.

Use padded residue tables:

```c
uint64_t count[QSOP_R_PADDED];
```

where `QSOP_R_PADDED` rounds the modulus up to a SIMD-friendly lane count.

Common kernels:

```text
phase shift:
    out[(i + delta) mod r] += in[i]

component merge:
    cyclic convolution modulo r

boundary-signature update:
    add local phase and merge equal signatures

rankwidth-table update:
    merge child tables indexed by labelled signatures
```

For small fixed moduli, specialize aggressively:

```text
r = 8:
    one cache-line-sized table in many cases

r = 16:
    power-of-two masking for residues

r = 24:
    common for some finite gate libraries, but not a power of two
```

The generic kernel should be clean C23. Architecture-specific kernels can be selected at runtime.

---

## A.12 Add CPU-feature dispatch early

Hide architecture-specific code behind a single kernel table:

```c
typedef struct {
    void (*shift)(...);
    void (*convolve)(...);
    void (*merge_signatures)(...);
    void (*rank_update)(...);
} qsop_kernel_table_t;

qsop_kernel_table_t qsop_select_kernels(void);
```

Potential backends:

```text
scalar
portable SIMD
AVX2
AVX-512
NEON
```

Only scalar needs to exist initially. The API boundary should exist from the beginning so optimized kernels do not leak throughout the solver.

---

## A.13 Keep import/export utilities outside the solver

The solver should read normalized QSOP and write exact QSOP results. It should not know about Qiskit, PyZX, Ganak, FeynmanDD, or MQT internals.

Suggested Unix-style tools:

```text
qasm2sop
mqt2sop
zx2sop
sop-check
sop-stats
sop-solve
sop2wmc
sop2feyndd
sop2zx
sop-bench
```

This keeps `sop-solve` small, profileable, dependency-free, and suitable for low-level optimization.

---

## A.14 Keep provenance out of the hot path

Provenance is useful for debugging and benchmarking, but it should not slow down solving.

Use sidecar metadata:

```text
example.sop
example.sop.meta
```

The sidecar may contain:

```text
source file
source gate IDs
source qubit names
boundary mapping
auxiliary variables introduced by gadgets
Toffoli/CCZ gadget expansion records
normalization history
export hints
```

The solver should not parse or carry this metadata unless explicitly requested.

---

## A.15 Make gate gadgets strategy-selectable

Do not hard-code one expansion for Toffoli, CCZ, or controlled phase gates.

Expose compiler strategies:

```text
toffoli=compact-labelled
toffoli=sign-only-parity
toffoli=decompose-clifford-t
toffoli=preserve-for-export
```

Different targets prefer different expansions:

```text
native labelled QSOP solver:
    compact-labelled

native sign-only solver:
    sign-only-parity

ZX export:
    decompose or emit phase-gadget form

WMC export:
    direct Boolean constraints or quadratized form

FeynmanDD export:
    preserve Toffoli if the chosen gate-set file supports it
```

This is important for fair benchmarking because the best representation for `dlx4sop` may not be the best representation for Ganak, FeynmanDD, MQT, or ZX tools.

---

## A.16 Separate fast counts from certified exact counts

Define result-count backends early:

```text
small exact counts:
    uint64_t / uint128_t

large exact counts:
    arbitrary precision integers

CRT counts:
    several machine-prime moduli with reconstruction

final amplitude:
    cyclotomic residue vector + normalization exponent
```

Benchmark mode can use fixed-width counters when safe. Certified mode can use big integers or CRT reconstruction.

Do not mix this policy with the solver logic. It should be a replaceable arithmetic backend.

---

## A.17 Keep instrumentation structured

Performance comparisons will be unreliable unless the tool emits structured
statistics. `sop-stats` already supports JSON, and `sop-solve --format stats`
reports backend counters; additional tracing should build on that interface
rather than adding ad hoc output.

Add:

```text
--stats json
--trace csv
--profile-events
```

Track at least:

```text
input variables
active variables after normalization
quadratic terms
modulus
mode: sign or labelled
components discovered
cache lookups
cache hits
branch nodes
maximum recursion depth
time in simplify / branch / cache / hash / components / kernels
estimated treewidth
estimated labelled signature width
residue table sizes
normalization exponent h
result residue support size
```

These statistics should be machine-readable and stable enough for plotting in papers.

---

## A.18 Test algebraic invariants, not only examples

Tests should protect optimization work.

Use property-style tests for:

```text
normalization preserves value
pinning preserves value
component split equals cyclic convolution/product
duplicate q-edge accumulation is correct
self-loop folding is correct
sign mode and labelled mode agree on sign-only instances
compact CCZ gadget equals truth table
compact Toffoli gadget equals truth table
cache hit returns the same residue vector
branching on any variable gives the same result
renumbering variables preserves the value
```

Also include cross-tool tests:

```text
OpenQASM -> SOP -> native solve
OpenQASM -> SOP -> WMC -> Ganak
OpenQASM -> SOP -> FeynmanDD export
OpenQASM -> SOP -> ZX export
```

The attached WMC note validates examples by comparing direct tensor-network contraction, WMC results, and Qiskit statevector results. The same philosophy should be used here, but with QSOP as the central IR.

---

## A.19 Do not reduce to WMC inside the native solver

`sop2wmc` is important for comparison, but WMC should remain an export target.

The native solver should not internally translate to CNF or WMC. Its potential advantage is that it preserves the finite-modulus quadratic phase structure directly:

```text
quadratic residues
labelled cut signatures
component factorization
cyclotomic residue counts
```

A WMC encoding introduces Boolean constraints, auxiliary variables, and weighted literals. That is useful for baselines and verification, but it can obscure cancellations and graph/rankwidth structure that the native solver can exploit directly.

---

## A.20 Make benchmarking a first-class utility

Use one manifest format to run all baselines fairly:

```yaml
name: qft_20
source: circuits/qft_20.qasm
amplitude:
  input: 00000000000000000000
  output: 00000000000000000000
transforms:
  - qasm2sop --gate-set clifford-t
  - sop-normalize
solvers:
  - sop-solve --mode auto
  - sop2wmc | ganak
  - sop2feyndd | feyndd
  - sop2zx | pyzx-baseline
```

The benchmark runner should record:

```text
source hash
normalized SOP hash
tool versions
compiler flags
CPU model
SIMD backend
wall time
CPU time
peak RSS
solver stats JSON
result hash
```

This avoids accidental unfairness between the native solver and external baselines.

---

## A.21 Keep the hot libraries small

Separate the project into libraries with clear dependency boundaries:

```text
libqsop_ir:
    parsing, validation, normalization

libqsop_solve:
    hot solver, no external dependencies

libqsop_width:
    treewidth/rankwidth/signature heuristics

libqsop_export:
    WMC, FeynmanDD, ZX, QASM exporters

tools/*:
    small Unix-style executables
```

Use opaque public structs:

```c
typedef struct qsop_state qsop_state_t;
```

but keep internal data in dense arrays.

This preserves freedom to change memory layout later without breaking the utility APIs.

---

## A.22 Priority order

The recommended implementation order is:

```text
1. Dense normalized QSOP IR
2. Sign/labelled mode detection
3. Integer residue-vector result type
4. Reversible dancing-cells trail
5. Incremental Zobrist hashing
6. Component decomposition
7. Simple pluggable branch heuristic interface
8. Scalar residue kernels with SIMD-friendly layout
9. Structured stats output
10. OpenQASM -> QSOP importer
11. SOP -> WMC exporter
12. Compact CCZ/Toffoli labelled gadgets
13. Benchmark manifest runner
14. Optional SIMD kernels
15. Labelled rankwidth/signature-width backend
```

The guiding rule is:

> Keep the hot path as a dense integer-labelled graph problem. Push QASM parsing, MQT quirks, ZX JSON, WMC CNF, FeynmanDD compatibility, provenance, pretty-printing, and plotting to the edges of the Unix pipeline.
