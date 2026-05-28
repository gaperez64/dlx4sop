# QSOP Format and SOP-First Dancing Cells Solver Plan

This note summarizes three design decisions for a fully SOP-native exact quantum circuit simulator:

1. a compact text format for quadratic SOP instances;
2. an implementation architecture for a Dancing-Cells-style SOP solver;
3. a benchmark plan, ordered from most immediately useful to broader validation.

The intended initial target is exact Clifford+T / `{H,T,CZ}`-style simulation, where pinned circuit amplitudes become quadratic sign-SOPs over Boolean variables.

---

## 1. Compact QSOP Text Format

### 1.1 Mathematical object

A QSOP instance represents

```math
Z = 2^{-h/2}\,\omega_r^c
    \sum_{x\in\{0,1\}^N}
    \omega_r^{\sum_v b_v x_v + (r/2)\sum_{\{u,v\}\in E}x_u x_v},
```

where:

- `r` is an even modulus;
- `h` is the Hadamard-normalization exponent;
- `c` is a constant phase exponent modulo `r`;
- each `b_v` is a unary exponent modulo `r`;
- each edge contributes the sign interaction `(r/2) x_u x_v`.

For Clifford+T, the default is usually `r = 8`, so each edge has coefficient `4` modulo `8`.

The key design choice is to store **exponent data over `Z_r`**, not complex weights. Complex or exact ring values should only be produced at the final evaluation stage or inside selected DP kernels.

---

### 1.2 Core file syntax

Use a DIMACS-style line-oriented format.

```text
c comments begin with c
p qsop <r> <num_variables> <num_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
```

Example:

```text
c Example QSOP instance
c Z = 2^{-6} omega_8^3 sum_x omega_8^{b.x + 4*edges}
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

This means

```math
Z = 2^{-6}\omega_8^3
\sum_{x\in\{0,1\}^5}
\omega_8^{x_0+7x_2+4x_4+4(x_0x_1+x_1x_2+x_2x_3+x_3x_4)}.
```

---

### 1.3 Parsing and normalization rules

Recommended rules for the loader:

- Vertices are dense integers `0..N-1`.
- `r` must be even.
- Unspecified unary coefficients default to `0`.
- Multiple `u v b` lines should accumulate modulo `r`.
- Edge endpoints should be canonicalized so `u < v`.
- Duplicate sign edges should toggle/cancel modulo `2`, since two identical sign interactions multiply to `1`.
- A self-loop `e v v` should be folded into the unary coefficient, because `x_v^2 = x_v`:

```text
b[v] += r/2 mod r
```

The loader should canonicalize the graph by sorting edges and removing/toggling duplicates. This is important for deterministic hashing, reproducibility, and vectorized kernels.

---

### 1.4 Optional extensions

#### Weighted quadratic terms

For a future treewidth-only or general pairwise backend:

```text
q <u> <v> <q_uv>
```

meaning a term `q_uv x_u x_v mod r`.

The current rankwidth algorithm only directly supports the sign-edge case `q_uv = r/2`.

#### Pinned variables

Prefer emitting already-reduced free-variable instances. If pins are needed:

```text
f <v> <0-or-1>
```

The loader can immediately substitute pinned variables:

- if `x_v = 0`, delete `v`;
- if `x_v = 1`, add `b_v` to the constant and add each incident edge coefficient to the corresponding neighbor unary coefficient.

#### Decomposition sidecars

Keep decompositions outside the `.sop` file:

```text
instance.sop
instance.tdec
instance.rdec
```

A rank-decomposition sidecar can be a tree edge list plus leaf-to-variable map.

---

## 2. SOP-First Dancing Cells Solver Architecture

The solver should operate directly on residual quadratic SOPs, not on CNF/WMC or exact-cover instances.

A residual state has the form

```math
F(G[U], b, c)
= \sum_{x_U}\omega_r^{c + \sum_{v\in U}b_vx_v + (r/2)\sum_{\{u,v\}\in E[U]}x_ux_v}.
```

The accumulated constant `c` should be kept outside most cache keys, because

```math
F(G,b,c)=\omega_r^c F(G,b,0).
```

---

### 2.1 Core branching operation

When branching on variable `v`:

#### Branch `x_v = 0`

Delete `v` and its incident active edges.

#### Branch `x_v = 1`

Update the constant and neighboring unary coefficients:

```text
c += b[v] mod r
for u in active_neighbors(v):
    b[u] += r/2 mod r
delete v
```

No complex arithmetic is required. All hot-path phase updates are integer arithmetic modulo `r`.

---

### 2.2 Dancing-Cells-style reversible mutation

Use the Dancing Links/Dancing Cells idea as an implementation philosophy:

- mutate the active graph in place;
- push every mutation onto a trail;
- restore by popping the trail in reverse order;
- avoid copying subinstances in recursive search.

Suggested mutable data:

```c
uint8_t  b[N];             // unary coefficients mod r
uint8_t  active_vertex[N];
uint32_t degree[N];

uint32_t head[N];          // mutable incidence lists
uint32_t edge_to[2*M];
uint32_t next[2*M], prev[2*M];
uint8_t  active_edge[2*M];

trail_entry trail[...];
```

At the same time, keep a static CSR representation for analysis and batch kernels:

```c
uint64_t rowptr[N+1];
uint32_t colind[2*M];
```

Do not make pointer-heavy linked lists the only graph representation; they are poor for SIMD, GPU, hashing, and cut-rank computations.

---

### 2.3 Simplification rules

Implement cheap SOP-native reductions before and during search.

#### Isolated vertex

```math
\sum_{x_v\in\{0,1\}}\omega_r^{b_vx_v}=1+\omega_r^{b_v}.
```

Remove it and multiply the component value by this small factor, or update residue counts.

#### Leaf vertex

If `v` has one neighbor `u`, summing out `v` gives a unary factor on `u`:

```math
\sum_{x_v}\omega_r^{b_vx_v+(r/2)x_vx_u}
=
\begin{cases}
1+\omega_r^{b_v}, & x_u=0,\\
1+\omega_r^{b_v+r/2}, & x_u=1.
\end{cases}
```

This can be folded into a unary-weight representation or handled as a small local DP.

#### Component decomposition

If the active graph splits into components `C_1,...,C_m`, then evaluate components independently:

```math
\sum_{x_U}\omega_r^{c+\sum_i f_i(x_{C_i})}
= \omega_r^c \prod_i \sum_{x_{C_i}}\omega_r^{f_i(x_{C_i})}.
```

Component decomposition is one of the main expected wins, as in DPLL-style model counters.

#### Twins and low-rank structure

Detect vertices with identical or low-rank neighborhoods. These are especially relevant for rankwidth-friendly instances and can be used to trigger rankwidth table joins.

---

### 2.4 Caching / hashing

Cache residual connected components as colored graphs:

```text
cache key = canonical_form(G_component, b_vector)
```

Do not include the global constant `c` in the key. Store the value for `c = 0` and multiply by `omega_r^c` externally.

Recommended cache layers:

1. cheap hash of sorted colored adjacency;
2. optional canonical labeling for high-value components;
3. rankwidth-DP table cache keyed by `(component, boundary_signature, Fourier_mode)` where applicable.

Vertex colors are the unary coefficients `b_v mod r`.

---

### 2.5 Backend selection

Use an adaptive solver rather than one global strategy.

#### Small component backend

Use brute force or Gray-code enumeration with incremental exponent updates.

#### Treewidth-style backend

Use variable elimination or DPLL branching with treewidth heuristics:

- min-degree;
- min-fill;
- separator/nested-dissection heuristics;
- prefer variables whose deletion splits components.

This gives a direct treewidth-FPT route for pairwise SOPs.

#### Rankwidth-style backend

Use recursive low-cut-rank splitting. For a cut `A | B`, store tables indexed by boundary signatures

```math
\sigma_A(z)=z^T A[A,B]\in\mathbb F_2^B.
```

For cut-rank `k`, there are at most `2^k` signatures. This is the rankwidth compression mechanism.

The Fourier-mode variant stores one value per signature and mode, avoiding an explicit residue dimension.

#### Fallback backend

Use cached branch-and-sum with Dancing-Cells-style reversible mutation.

---

### 2.6 SIMD/GPU preparation

Keep the `.sop` format simple, but load it into vectorization-friendly layouts.

Recommended in-memory layout:

```c
uint8_t  b[N];
uint32_t edge_u[M];
uint32_t edge_v[M];
uint64_t rowptr[N+1];
uint32_t colind[2*M];
```

Use optional packed bitsets per component:

```c
uint64_t A[s][ceil(s/64)];
```

Useful for:

- cut-rank over `F_2`;
- parity computations;
- rankwidth heuristics;
- dense brute-force kernels.

Likely GPU-friendly kernels:

- dense rankwidth table joins;
- medium-size brute-force batches;
- many independent component evaluations.

Avoid trying to put the irregular recursive search itself on the GPU initially.

---

### 2.7 Exact arithmetic

For Clifford+T, use `r = 8`. Final amplitudes lie in

```math
2^{-h/2}\mathbb Z[\omega_8].
```

If residue counts are `N_0,...,N_7`, then

```math
\sum_{j=0}^7 N_j\omega_8^j
= (N_0-N_4)
+ (N_1-N_5)\omega_8
+ (N_2-N_6)\omega_8^2
+ (N_3-N_7)\omega_8^3.
```

So the exact result can be stored as four integers plus the normalization exponent `h`.

---

## 3. Benchmark Sets, in Suggested Order

The benchmark plan should separate theory-validation, closest-competitor comparison, and broad external validation.

---

### 3.1 Synthetic graph-realization circuits

**Priority: highest.**

Generate circuits whose SOP variable graph is controlled directly. This is the best way to validate the claimed structural advantage.

Recommended graph families:

1. paths and cycles;
2. grids and cylinders;
3. complete binary trees;
4. clique blowups of complete binary trees;
5. cographs;
6. distance-hereditary graphs;
7. random regular graphs;
8. Erdős–Rényi graphs;
9. QAOA-style problem graphs.

Purpose:

- paths/cycles: sanity checks;
- grids: treewidth/contraction baseline;
- binary trees: bounded rankwidth, growing linear rankwidth;
- clique blowups of trees: bounded rankwidth, growing treewidth/contraction complexity;
- random graphs: hard negative controls.

---

### 3.2 MQT Bench

**Priority: highest among public benchmark suites.**

Use MQT Bench for scalable, reproducible algorithm families. It provides a Python API and many standard circuits.

Recommended families:

1. GHZ / W-state;
2. QFT;
3. Grover;
4. QAOA;
5. VQE / ansatz circuits;
6. arithmetic circuits;
7. Shor-like circuits;
8. mapped/native versions when useful.

For a Clifford+T simulator, use exact Clifford+T-compatible circuits where possible, or explicitly report the transpilation/compilation rule.

---

### 3.3 FeynmanDD repository benchmarks

**Priority: high. Closest conceptual competitor.**

Use the FeynmanDD benchmark repository because it targets path-integral / decision-diagram simulation.

Recommended subsets:

1. Google-style `cz_v2` circuits;
2. Google-style `is_v1` circuits;
3. linear-network benchmarks if present;
4. any Clifford+T-compatible or exactly compilable subsets.

If native gates are outside Clifford+T, report them separately as either:

- exactly supported extended gates;
- compiled-to-Clifford+T instances;
- excluded from the exact Clifford+T comparison.

---

### 3.4 QASMBench

**Priority: medium-high for external validation.**

QASMBench is broad and useful for demonstrating that the simulator is not tuned only to synthetic or FeynmanDD instances.

Use with filtering/transpilation because many circuits may contain arbitrary rotations or gates outside the exact Clifford+T subset.

Recommended reporting:

- exact Clifford+T subset;
- exactly compilable reversible/Clifford circuits;
- approximate-compiled circuits only if approximation is explicitly in scope.

---

### 3.5 VeriQBench

**Priority: medium.**

Useful for verification-style and structured circuit workloads. Include after the core solver and MQT/FeynmanDD experiments are stable.

---

### 3.6 DD-Matrix-Benchmark

**Priority: medium, especially for DD comparisons.**

Use when comparing against decision-diagram simulators such as MQT DD-based tools. It is useful because it aggregates decision-diagram-relevant instances from multiple sources.

---

### 3.7 RevLib / reversible benchmarks

**Priority: optional.**

Useful for large structured reversible circuits, especially Toffoli-heavy workloads. For a Clifford+T-restricted simulator, Toffoli gates must be exactly decomposed into Clifford+T before SOP construction.

---

## 4. Metrics to Record

For each benchmark instance, record both simulator performance and structural graph data.

Circuit/SOP size:

```text
num_qubits
num_gates
H_count
T_count
CZ/CX_count
num_SOP_variables
num_SOP_edges
normalization_h
```

Graph structure:

```text
estimated tw(G_SOP)
estimated rw(G_SOP)
estimated lrw(G_SOP)
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
exact residue counts, when feasible
exact Z[omega_8] amplitude representation
floating-point amplitude for comparison
```

---

## 5. Recommended Build Order

1. Implement the `.sop` parser and canonicalizer.
2. Implement exact residue-count brute force for small instances.
3. Implement Dancing-Cells-style reversible branching.
4. Add component decomposition and component caching.
5. Add treewidth-inspired branching heuristics.
6. Add rankwidth cut-rank heuristics and table joins.
7. Add OpenQASM 2.0 / Qiskit frontend to emit `.sop`.
8. Generate synthetic graph-realization benchmarks.
9. Integrate MQT Bench.
10. Add FeynmanDD benchmark import.
11. Add QASMBench / VeriQBench filtering.
12. Add optional decomposition sidecar support.

---

## 6. Short Design Principle

The simulator should be SOP-first:

```text
Quantum circuit
   -> pinned quadratic SOP
   -> canonical colored graph over Z_r
   -> adaptive Dancing-Cells / treewidth / rankwidth solver
   -> exact amplitude in 2^{-h/2} Z[omega_8]
```

Avoid CNF/WMC unless used only as an external comparison path. The native object should remain the quadratic SOP throughout.
