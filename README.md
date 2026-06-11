# QSOP Format and SOP-First Dancing Cells Solver Plan

`dlx4sop` is a design sketch for an exact, SOP-native quantum-circuit simulator. The core object is a finite-modulus **quadratic sum of powers** (QSOP), evaluated directly rather than translated through CNF, WMC, or tensor-network contraction.

The current near-term scope has two layers:

1. **Layer 1: sign-edge QSOP**, optimized for Clifford+T / `{H,T,CZ}`-style instances.
2. **Layer 2: labelled QSOP**, supporting arbitrary quadratic coefficients modulo `r`, including compact quadratic gadgets for `CCZ` and Toffoli.

Native degree greater than two is intentionally out of scope for now. Higher-degree gates should be decomposed, rewritten, or quadratized into Layer 2 before invoking the solver.

---

## 1. Mathematical object

A labelled quadratic SOP instance represents

$$
Z =
2^{-h/2}\omega_r^c
\sum_{x\in\{0,1\}^N}
\omega_r^{
\sum_i b_i x_i+
\sum_{i<j} q_{ij}x_i x_j
},
$$

where:

- `r` is an even positive modulus;
- $\omega_r = e^{2\pi i/r}$;
- `h` is the power-of-two normalization exponent;
- `c` is the constant phase exponent modulo `r`;
- each unary coefficient $b_i$ lies in $\mathbb Z_r$;
- each quadratic coefficient $q_{ij}$ lies in $\mathbb Z_r$.

Layer 1 is the special case

$$
q_{ij}\in\{0,r/2\},
$$

so every quadratic interaction is a sign interaction:

$$
\omega_r^{(r/2)x_i x_j}=(-1)^{x_i x_j}.
$$

For Clifford+T, the usual modulus is `r = 8`, and each sign edge has coefficient `4`.

The main implementation principle is to store exponent data over `Z_r`, not complex weights. Complex or exact ring values should be produced only at the final evaluation stage or inside selected DP kernels.

---

## 2. Layer 1: sign-edge QSOP

Layer 1 is the compact fast path for the existing Clifford+T-oriented solver. It stores a graph whose edges all contribute coefficient `r/2`.

### 2.1 Syntax

```text
c comments begin with c
p qsop <r> <num_variables> <num_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
```

The line

```text
e <u> <v>
```

means

$$
(r/2)x_u x_v.
$$

A future parser may also accept the explicit header

```text
p qsop-sign <r> <num_variables> <num_edges>
```

but the legacy `p qsop` header should remain valid for sign-edge-only files.

### 2.2 Example

```text
c Sign-edge QSOP over Z_8
c Z = 2^{-6} omega_8^3 sum_x omega_8^{x0 + 7*x2 + 4*x4 + 4*(x0*x1 + x1*x2 + x2*x3 + x3*x4)}
p qsop 8 5 4
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

This represents

$$
Z =
2^{-6}\omega_8^3
\sum_{x\in\{0,1\}^5}
\omega_8^{
x_0+7x_2+4x_4+
4(x_0x_1+x_1x_2+x_2x_3+x_3x_4)
}.
$$

### 2.3 Parsing and normalization rules

Recommended loader rules:

- Vertices are dense integers `0..N-1`.
- `r` must be even.
- Unspecified unary coefficients default to `0`.
- Multiple `u v b` lines accumulate modulo `r`.
- Edge endpoints are canonicalized so `u < v`.
- Duplicate sign edges toggle modulo `2`.

Two identical sign edges cancel because

$$
2\cdot(r/2)x_u x_v\equiv 0\pmod r.
$$

A self-loop

```text
e v v
```

is folded into the unary coefficient because $x_v^2=x_v$:

```text
b[v] += r/2 mod r
```

The loader should canonicalize the graph by sorting edges and removing or toggling duplicates. This is important for deterministic hashing, reproducibility, and vectorized kernels.

### 2.4 Intended backend

Layer 1 should use the graph-specialized solver:

- sparse graph storage;
- branch-and-sum with reversible mutation;
- component decomposition;
- treewidth-style variable elimination;
- rankwidth-style cut-rank tables over the binary support graph.

This layer is the fastest target for Clifford+T and `{H,T,CZ}` instances.

---

## 3. Layer 2: labelled QSOP

Layer 2 extends Layer 1 by allowing arbitrary quadratic coefficients modulo `r`.

This is the natural format for:

- controlled phase gates such as `CS`, `CT`, `sqrt(CZ)`, and finite-angle `RZZ`;
- compact Toffoli and `CCZ` quadratization gadgets;
- finite-phase graph-like ZX instances with labelled pair interactions;
- fixed gate libraries whose matrix entries are finite-modulus quadratic path sums.

### 3.1 Syntax

```text
c comments begin with c
p qsop <r> <num_variables> <num_quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
```

The line

```text
q <u> <v> <a>
```

means

$$
a x_u x_v \pmod r.
$$

The line

```text
e <u> <v>
```

is shorthand for

```text
q <u> <v> <r/2>
```

Thus every Layer 1 file is also a valid Layer 2 file after replacing each `e` line by a `q` line with coefficient `r/2`.

### 3.2 Example

```text
c Labelled quadratic SOP over Z_8
c Z = 2^{-2} sum_x omega_8^{x0 + 2*x0*x1 + 4*x1*x2 + 6*x0*x2}
p qsop 8 3 3
n 4
cst 0

u 0 1

q 0 1 2
q 1 2 4
q 0 2 6
```

This represents

$$
Z =
2^{-2}
\sum_{x\in\{0,1\}^3}
\omega_8^{
x_0+2x_0x_1+4x_1x_2+6x_0x_2
}.
$$

### 3.3 Parsing and normalization rules

For labelled quadratic terms, duplicate terms accumulate modulo `r`.

If the parser sees

```text
q u v a
q u v b
```

then it should replace them by one coefficient

$$
q_{uv}\leftarrow a+b\pmod r.
$$

If the accumulated coefficient is `0 mod r`, the term should be deleted.

As before, endpoints are canonicalized so `u < v`.

A self-loop

```text
q v v a
```

is folded into the unary coefficient because $x_v^2=x_v$:

```text
b[v] += a mod r
```

The shorthand line

```text
e u v
```

should be parsed as

```text
q u v r/2
```

before canonicalization.

### 3.4 Pinned variables

Emitters should prefer to output already-reduced free-variable instances. If pins are needed, use

```text
f <v> <0-or-1>
```

If $x_v=0$, delete `v` and all incident quadratic terms.

If $x_v=1`, apply

$$
c\leftarrow c+b_v\pmod r
$$

and for every incident labelled quadratic term $q_{uv}x_u x_v$, update

$$
b_u\leftarrow b_u+q_{uv}\pmod r.
$$

Then delete `v`.

### 3.5 Branching rule

For a residual labelled QSOP

$$
F(U,b,Q,c)=
\sum_{x_U}
\omega_r^{
c+
\sum_{v\in U}b_vx_v+
\sum_{\{u,v\}\subseteq U}q_{uv}x_ux_v
},
$$

branching on variable `v` gives the following two branches.

#### Branch `x_v = 0`

Delete `v` and all incident quadratic terms.

#### Branch `x_v = 1`

```text
c += b[v] mod r
for each active neighbor u of v:
    b[u] += q[v,u] mod r
delete v
```

This is the direct labelled generalization of the Layer 1 rule, where all edge coefficients are implicitly `r/2`.

### 3.6 Data structure changes

Layer 1 can store only graph adjacency. Layer 2 must store a coefficient per edge.

Recommended static arrays:

```text
uint16_t r;

coeff_t b[N];          // unary coefficients mod r
coeff_t q[M];          // quadratic coefficients mod r

uint32_t edge_u[M];
uint32_t edge_v[M];

uint64_t rowptr[N+1];
uint32_t colind[2*M];
uint32_t edge_id[2*M];
```

For reversible mutation, the trail must record coefficient changes as well as activation and deactivation events:

```text
enum trail_kind {
    SET_UNARY,
    SET_CONSTANT,
    DEACTIVATE_VERTEX,
    DEACTIVATE_EDGE,
    ACTIVATE_EDGE,
    SET_QUADRATIC
};

struct trail_entry {
    trail_kind kind;
    uint32_t index;
    coeff_t old_value;
};
```

Layer 1 remains a specialization of this representation with every active `q[e] = r/2`.

---

## 4. SOP-first Dancing Cells solver architecture

The solver should operate directly on residual quadratic SOPs, not on CNF/WMC or exact-cover instances.

The accumulated constant `c` should be kept outside most cache keys, because

$$
F(G,b,c)=\omega_r^c F(G,b,0).
$$

### 4.1 Dancing-Cells-style reversible mutation

Use the Dancing Links / Dancing Cells idea as an implementation philosophy:

- mutate the active graph in place;
- push every mutation onto a trail;
- restore by popping the trail in reverse order;
- avoid copying subinstances in recursive search.

Suggested mutable data:

```text
coeff_t  b[N];
uint8_t  active_vertex[N];
uint32_t degree[N];
uint32_t head[N];          // mutable incidence lists
uint32_t edge_to[2*M];
uint32_t next[2*M], prev[2*M];
uint8_t  active_edge[M];
coeff_t  q[M];

trail_entry trail[...];
```

At the same time, keep a static CSR representation for analysis and batch kernels:

```text
uint64_t rowptr[N+1];
uint32_t colind[2*M];
uint32_t edge_id[2*M];
```

Do not make pointer-heavy linked lists the only graph representation; they are poor for SIMD, GPU, hashing, and cut-rank computations.

### 4.2 Simplification rules

Implement cheap SOP-native reductions before and during search.

#### Isolated vertex

$$
\sum_{x_v\in\{0,1\}}\omega_r^{b_vx_v}=1+\omega_r^{b_v}.
$$

Remove it and multiply the component value by this small factor, or update residue counts.

#### Leaf vertex, labelled version

If `v` has one neighbor `u` through edge coefficient `q_uv`, summing out `v` gives a unary factor on `u`:

$$
\sum_{x_v}\omega_r^{b_vx_v+q_{uv}x_vx_u}
=
\begin{cases}
1+\omega_r^{b_v}, & x_u=0,\\
1+\omega_r^{b_v+q_{uv}}, & x_u=1.
\end{cases}
$$

For sign-edge QSOPs, this specializes to `q_uv = r/2`.

#### Component decomposition

If the active graph splits into components `C_1,...,C_m`, evaluate components independently:

$$
\sum_{x_U}\omega_r^{c+\sum_i f_i(x_{C_i})}
=
\omega_r^c
\prod_i
\sum_{x_{C_i}}\omega_r^{f_i(x_{C_i})}.
$$

Component decomposition is one of the main expected wins, as in DPLL-style model counters.

#### Twins and low-rank structure

Detect vertices with identical or low-rank labelled neighborhoods. These are especially relevant for rankwidth-friendly instances and can be used to trigger rankwidth table joins.

### 4.3 Caching and hashing

Cache residual connected components as colored labelled graphs:

```text
cache key = canonical_form(G_component, b_vector, q_edge_labels)
```

Do not include the global constant `c` in the key. Store the value for `c = 0` and multiply by `omega_r^c` externally.

Recommended cache layers:

1. cheap hash of sorted colored labelled adjacency;
2. optional canonical labeling for high-value components;
3. rankwidth-DP table cache keyed by `(component, boundary_signature, Fourier_mode)` where applicable.

Vertex colors are unary coefficients `b_v mod r`. Edge colors are quadratic coefficients `q_uv mod r`.

### 4.4 Backend selection

Use an adaptive solver rather than one global strategy.

#### Small component backend

Use brute force or Gray-code enumeration with incremental exponent updates.

#### Treewidth-style backend

Use variable elimination or DPLL branching with treewidth heuristics:

- min-degree;
- min-fill;
- separator or nested-dissection heuristics;
- prefer variables whose deletion splits components.

A labelled QSOP is still a pairwise graphical model. The relevant treewidth graph is the support graph

$$
G_Q=(V,\{\{i,j\}:q_{ij}\neq 0\}).
$$

#### Rankwidth-style backend

For Layer 1, the rankwidth backend can use the binary adjacency matrix of the sign-edge graph.

For Layer 2, the correct object is a coefficient-labelled cut matrix. Across a cut $A\mid \bar A$, the cross term is

$$
x_A^T Q[A,\bar A]x_{\bar A},
$$

where

$$
Q[A,\bar A]\in\mathbb Z_r^{A\times \bar A}.
$$

For prime `r`, one can use matrix rank over $\mathbb F_r`.

For composite `r`, such as `r = 8`, the safest implementation-level abstraction is the number of distinct boundary signatures induced by assignments on one side of the cut:

$$
\sigma_A(x_A)=x_A^TQ[A,\bar A]\in\mathbb Z_r^{\bar A}.
$$

The rankwidth-style table size is controlled by the number of distinct such signatures. For fixed `r`, this is FPT in the corresponding labelled cut-rank or signature-rank parameter.

#### Fallback backend

Use cached branch-and-sum with Dancing-Cells-style reversible mutation.

### 4.5 SIMD/GPU preparation

Keep the `.sop` format simple, but load it into vectorization-friendly layouts.

Recommended in-memory layout:

```text
coeff_t  b[N];
coeff_t  q[M];
uint32_t edge_u[M];
uint32_t edge_v[M];
uint64_t rowptr[N+1];
uint32_t colind[2*M];
uint32_t edge_id[2*M];
```

Use optional packed bitsets per component:

```text
uint64_t A[s][ceil(s/64)];
```

Useful for:

- binary cut-rank over `F_2` in the sign-edge fast path;
- labelled signature compression heuristics;
- parity computations;
- rankwidth heuristics;
- dense brute-force kernels.

Likely GPU-friendly kernels:

- dense rankwidth table joins;
- medium-size brute-force batches;
- many independent component evaluations.

Avoid trying to put the irregular recursive search itself on the GPU initially.

### 4.6 Exact arithmetic

For Clifford+T, use `r = 8`. Final amplitudes lie in

$$
2^{-h/2}\mathbb Z[\omega_8].
$$

If residue counts are `N_0,...,N_7`, then

$$
\sum_{j=0}^7 N_j\omega_8^j
=
(N_0-N_4)
+(N_1-N_5)\omega_8
+(N_2-N_6)\omega_8^2
+(N_3-N_7)\omega_8^3.
$$

So the exact result can be stored as four integers plus the normalization exponent `h`.

For general fixed `r`, store residue counts `N_0,...,N_{r-1}` and reduce them in the chosen cyclotomic basis only at output time.

---

## 5. Gate coverage

### 5.1 Layer 1 coverage

Layer 1 naturally supports gate sets whose SOPs use only unary phases and sign interactions:

- `H`;
- `T`, `S`, `Z`, and finite one-qubit diagonal phases;
- `CZ`;
- Clifford+T circuits expressed in the standard `{H,T,CZ}`-style quadratic SOP form.

### 5.2 Layer 2 coverage

Layer 2 supports all Layer 1 gates and arbitrary finite two-qubit diagonal phases.

Examples include:

- `CS`;
- `CT`;
- `sqrt(CZ)`;
- finite-angle `RZZ`;
- compact quadratic gadgets for `CCZ` and Toffoli.

A two-qubit diagonal gate

$$
\operatorname{diag}(\omega_r^{\alpha_{00}},\omega_r^{\alpha_{01}},\omega_r^{\alpha_{10}},\omega_r^{\alpha_{11}})
$$

can be written as

$$
\omega_r^{
\alpha_{00}+b_1x_1+b_2x_2+q_{12}x_1x_2
},
$$

where

$$
b_1=\alpha_{10}-\alpha_{00},
$$

$$
b_2=\alpha_{01}-\alpha_{00},
$$

and

$$
q_{12}=\alpha_{11}-\alpha_{10}-\alpha_{01}+\alpha_{00}\pmod r.
$$

Thus any finite-modulus two-qubit diagonal gate is a labelled quadratic SOP term.

### 5.3 Affine reversible gates

`X`, `CNOT`, `SWAP`, and more general affine reversible maps can be handled by wire relabelling when possible or by parity/equality constraints.

The parity constraint

$$
y=x_1\oplus\cdots\oplus x_k
$$

can be encoded as

$$
\delta(y=x_1\oplus\cdots\oplus x_k)
=
\frac12
\sum_{\lambda\in\{0,1\}}
(-1)^{\lambda(y+x_1+\cdots+x_k)}.
$$

Over an even modulus `r`, this becomes a quadratic SOP term with coefficient `r/2` on each product involving `lambda`.

---

## 6. Native `CCZ` and Toffoli support through Layer 2

Toffoli should be added as a compiler-level macro that emits a constant-size labelled quadratic SOP gadget.

Let

$$
\omega=\omega_8=e^{i\pi/4}.
$$

For `CCZ`, the identity

$$
(-1)^{abc}
=
\frac12
\sum_{p,\lambda\in\{0,1\}}
\omega^{
-a-b-c
+2(ab+ac+bc)
+p
+4\lambda(p+a+b+c)
}
$$

gives an exact labelled quadratic representation over $\mathbb Z_8$.

Therefore `CCZ` costs two auxiliary variables:

```text
p
lambda
```

Toffoli can be compiled as

$$
\operatorname{Tof}_{a,b\to t}
=
(I\otimes I\otimes H)\,
\operatorname{CCZ}_{a,b,z}\,
(I\otimes I\otimes H),
$$

so matrix entries can be represented as

$$
\langle a,b,t'|\operatorname{Tof}|a,b,t\rangle
=
\frac14
\sum_{z,p,\lambda\in\{0,1\}}
\omega^{
4z(t+t')
-a-b-z
+2(ab+az+bz)
+p
+4\lambda(p+a+b+z)
}.
$$

Thus a native Toffoli macro emits a labelled quadratic gadget over `r = 8` with three local auxiliary variables:

```text
z
p
lambda
```

The compiler should account for the scalar factor `1/4` by increasing the normalization exponent by `4`, since

$$
1/4=2^{-2}=2^{-4/2}.
$$

So Toffoli contributes

```text
n += 4
```

in the normalization convention $2^{-h/2}$.

If input and output controls are represented by distinct boundary variables, add equality constraints for the controls. If the path construction reuses the same control variables across the gate, no additional equality constraint is needed.

---

## 7. Compatibility policy

The recommended compatibility policy is:

1. Existing sign-edge files using `p qsop` and `e` lines should continue to parse.
2. `e u v` remains legal and means `q u v r/2`.
3. New labelled files should use `p qsop` and `q` lines.
4. New sign-edge-only files may optionally use `p qsop-sign` if the parser supports it.
5. The loader should detect whether the final normalized instance is sign-only or genuinely labelled.

Suggested parser behavior:

```text
if header == "p qsop-sign":
    accept e-lines
    reject q-lines unless q_uv == r/2

if header == "p qsop":
    accept both e-lines and q-lines
    desugar e u v as q u v r/2
```

This gives a clean migration path:

```text
Layer 1:
    p qsop
    e u v

Layer 2:
    p qsop
    q u v a
```

---

## 8. Benchmark sets, in suggested order

The benchmark plan should separate theory validation, closest-competitor comparison, and broad external validation.

### 8.1 Synthetic graph-realization circuits

Priority: highest.

Generate circuits whose SOP variable graph is controlled directly. This is the best way to validate the claimed structural advantage.

Recommended graph families:

1. paths and cycles;
2. grids and cylinders;
3. complete binary trees;
4. clique blowups of complete binary trees;
5. cographs;
6. distance-hereditary graphs;
7. random regular graphs;
8. Erdos-Renyi graphs;
9. QAOA-style problem graphs.

Purpose:

- paths/cycles: sanity checks;
- grids: treewidth/contraction baseline;
- binary trees: bounded rankwidth, growing linear rankwidth;
- clique blowups of trees: bounded rankwidth, growing treewidth/contraction complexity;
- random graphs: hard negative controls.

### 8.2 Toffoli-heavy reversible benchmarks

Priority: high after Layer 2 is implemented.

Use structured reversible circuits to validate the native Toffoli gadget:

1. Toffoli ladders;
2. reversible adders;
3. modular arithmetic circuits;
4. small Shor-style arithmetic blocks;
5. RevLib-style reversible benchmarks.

Report both:

- direct Toffoli-to-labelled-QSOP compilation;
- optional decomposition-to-Clifford+T as a comparison baseline.

### 8.3 MQT Bench

Priority: highest among public benchmark suites.

Use MQT Bench for scalable, reproducible algorithm families.

Recommended families:

1. GHZ / W-state;
2. QFT;
3. Grover;
4. QAOA;
5. VQE / ansatz circuits;
6. arithmetic circuits;
7. Shor-like circuits;
8. mapped/native versions when useful.

For a finite-modulus exact simulator, use exactly supported gate sets where possible, or explicitly report the transpilation or compilation rule.

### 8.4 FeynmanDD-style benchmark sets

Priority: high. Closest conceptual competitor.

Recommended subsets:

1. Google-style `cz_v2` circuits;
2. Google-style `is_v1` circuits;
3. linear-network benchmarks if present;
4. Clifford+T-compatible subsets;
5. Toffoli/CCZ subsets if available.

If native gates are outside the current two-layer QSOP scope, report them separately as either:

- exactly supported Layer 2 gates;
- compiled-to-Layer-2 gadgets;
- compiled-to-Clifford+T instances;
- excluded from the exact comparison.

### 8.5 QASMBench

Priority: medium-high for external validation.

Use with filtering/transpilation because many circuits may contain arbitrary rotations or gates outside the exact finite-modulus subset.

Recommended reporting:

- exact Clifford+T subset;
- exact Layer 2 finite-phase subset;
- exactly compilable reversible/Toffoli subset;
- approximate-compiled circuits only if approximation is explicitly in scope.

### 8.6 VeriQBench and DD-oriented benchmarks

Priority: medium.

Useful for verification-style, decision-diagram-style, and structured circuit workloads. Include after the core solver and MQT/FeynmanDD experiments are stable.

---

## 9. Metrics to record

For each benchmark instance, record both simulator performance and structural graph data.

Circuit/SOP size:

```text
num_qubits
num_gates
H_count
T_count
CZ/CX_count
CCZ_count
Toffoli_count
num_SOP_variables
num_SOP_quadratic_terms
num_sign_edges
num_labelled_edges
normalization_h
modulus_r
```

Graph structure:

```text
estimated tw(G_Q)
estimated rw(G_Q) for sign-edge instances
estimated labelled-rw(G_Q) or max boundary-signature rank for labelled instances
estimated lrw(G_Q)
max_degree
component sizes
```

Solver behavior:

```text
runtime
peak_memory
cache_hits
cache_misses
component_splits
max_residual_component_size
num_branch_nodes
num_rankwidth_joins
max_signature_table_size
```

Result data:

```text
exact residue counts
exact cyclotomic amplitude representation
floating-point amplitude for comparison
```

---

## 10. Recommended build order

1. Implement the `.sop` parser and canonicalizer for Layer 1 sign-edge QSOP.
2. Implement exact residue-count brute force for small instances.
3. Implement Dancing-Cells-style reversible branching for Layer 1.
4. Add component decomposition and component caching.
5. Add treewidth-inspired branching heuristics.
6. Add rankwidth cut-rank heuristics and table joins for Layer 1.
7. Extend the parser to Layer 2 labelled `q` terms.
8. Generalize branching, canonicalization, hashing, and leaf rules to labelled edge coefficients.
9. Add treewidth-style support for labelled QSOPs.
10. Add labelled boundary-signature support for rankwidth-style joins.
11. Add native compiler macros for `CCZ` and Toffoli.
12. Add OpenQASM 2.0 / Qiskit frontend to emit `.sop`.
13. Generate synthetic graph-realization benchmarks.
14. Integrate Toffoli-heavy reversible benchmarks.
15. Integrate MQT Bench.
16. Add FeynmanDD-style benchmark import.
17. Add QASMBench / VeriQBench filtering.
18. Add optional decomposition sidecar support.

---

## 11. Scope boundary

This two-layer plan deliberately does not add native degree-3 or higher monomials.

The solver should maintain the invariant

$$
\deg(Q)\le 2.
$$

Higher-degree gates should be handled by one of the following compiler steps:

1. decompose into supported gates;
2. rewrite into a quadratic path-sum gadget;
3. reject with a clear message explaining that native degree greater than two is not yet supported.

For the current paper and implementation, the main new target should be

$$
\boxed{\text{Layer 2 labelled QSOP + native Toffoli/CCZ quadratic gadgets}.}
$$

This extends the tool beyond Clifford+T while preserving the quadratic-SOP framework and the treewidth/rankwidth special cases on the compiled quadratic instance.

---

## 12. Short design principle

The simulator should be SOP-first:

```text
Quantum circuit
   -> pinned finite-modulus quadratic SOP
   -> canonical labelled graph over Z_r
   -> adaptive Dancing-Cells / treewidth / rankwidth solver
   -> exact amplitude in a cyclotomic representation
```

Avoid CNF/WMC unless used only as an external comparison path. The native object should remain the quadratic SOP throughout.
