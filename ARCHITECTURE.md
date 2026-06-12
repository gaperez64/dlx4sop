# dlx4sop Architecture Plan

This document describes the implementation direction for `dlx4sop`: a low-level, Unix-style toolkit for exact finite-modulus quadratic SOP simulation and translation.

The current scope is deliberately restricted to **degree at most 2** SOPs. Higher-degree native SOPs are out of scope for now. Gates such as `CCZ` and Toffoli should be supported through **constant-size labelled quadratic gadgets**, not by admitting cubic monomials into the core solver.

---

## 1. Design goals

The project should behave like a collection of composable Unix utilities written in modern C, with a small number of stable text formats and predictable command-line behavior.

Primary goals:

- Use **C23** for the performance-critical implementation.
- Use **Meson** as the build system.
- Keep command-line tools small, scriptable, and composable.
- Keep the core solver independent of Qiskit, Python, PyZX, MQT, Ganak, and FeynmanDD runtime dependencies.
- Prefer text input/output formats that are easy to diff, version, fuzz, and generate.
- Preserve exact finite-modulus arithmetic internally.
- Use architecture-aware data layouts and optional SIMD kernels for hot loops.
- Treat external tools as import/export targets, not as hidden solver dependencies.

Non-goals for the near term:

- Native degree-3 or higher SOP solving.
- Arbitrary real-angle gates inside the finite-modulus core.
- Full OpenQASM 3 dynamic circuits, timing, calibration, or classical control flow.
- A native parser for every quantum framework's internal binary representation.

---

## 2. Core mathematical object

The core input object is a finite-modulus quadratic SOP:

\[
Z =
2^{-h/2}\omega_r^c
\sum_{x\in\{0,1\}^N}
\omega_r^{
\sum_i b_i x_i + \sum_{i<j} q_{ij}x_i x_j
},
\qquad
\omega_r=e^{2\pi i/r}.
\]

The implementation should support two layers.

### 2.1 Layer 1: sign-edge QSOP

Layer 1 is the fast path for Clifford+T-style instances where every quadratic term has coefficient `r/2`:

```text
p qsop-sign <r> <num_variables> <num_edges>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
e <u> <v>
```

An edge means

\[
(r/2)x_u x_v.
\]

For `r = 8`, this is coefficient `4`, i.e. a sign interaction.

### 2.2 Layer 2: labelled QSOP

Layer 2 is the general pairwise format:

```text
p qsop <r> <num_variables> <num_quadratic_terms>
n <normalization_h>
cst <constant_mod_r>
u <vertex> <unary_coefficient_mod_r>
q <u> <v> <quadratic_coefficient_mod_r>
e <u> <v>
```

The line

```text
q u v a
```

means

\[
a x_u x_v \pmod r.
\]

The line

```text
e u v
```

is shorthand for

```text
q u v r/2
```

Layer 1 should remain a strict fast path. Layer 2 should be the default internal representation once arbitrary controlled phases, compact Toffoli gadgets, or richer gate sets are involved.

---

## 3. Internal IR layers

The codebase should keep the following representations separate.

### 3.1 `sop_ir`: normalized quadratic SOP

This is the main solver IR.

Suggested fields:

```c
typedef struct sop_qsop {
  uint32_t nvars;
  uint32_t nedges;
  uint32_t r;
  uint64_t norm_h;
  uint32_t constant;

  uint16_t *unary;      // length nvars, values mod r
  uint32_t *edge_u;     // length nedges
  uint32_t *edge_v;     // length nedges
  uint16_t *edge_q;     // length nedges, values mod r

  // CSR-style incidence for active-graph algorithms.
  uint64_t *rowptr;
  uint32_t *colind;
  uint32_t *edge_id;
} sop_qsop;
```

For small fixed moduli such as `r = 2, 4, 8, 16, 24`, coefficients should fit in `uint8_t` or `uint16_t`. The file parser can store `r` in `uint32_t`, then select compact internal storage.

### 3.2 `path_ir`: gate-level open path representation

The importers should not translate OpenQASM or MQT input straight into CNF. They should first construct a small gate-level path representation:

- each wire segment is a binary variable;
- each gate consumes current wire segment variables;
- each gate produces fresh output wire segment variables;
- circuit inputs and outputs remain boundary variables;
- amplitudes are obtained by summing over internal wire segment variables.

This follows the same design direction as the tensor-network-to-WMC document: binary tensor indices become Boolean variables, gates become local constraints or weights, and fixed inputs/outputs become boundary constraints. The uploaded document also argues that this representation is close to circuits, tensor networks, and WMC at the same time.

Suggested fields:

```c
typedef enum gate_kind {
  GATE_H,
  GATE_X,
  GATE_Z,
  GATE_S,
  GATE_SDG,
  GATE_T,
  GATE_TDG,
  GATE_CX,
  GATE_CZ,
  GATE_CCX,
  GATE_SWAP,
  GATE_RZ_RATIONAL_PI,
  GATE_PHASE_RATIONAL_PI,
  GATE_DIAG1,
  GATE_DIAG2
} gate_kind;

typedef struct path_gate {
  gate_kind kind;
  uint32_t arity;
  uint32_t in[3];
  uint32_t out[3];
  int64_t phase_num;
  int64_t phase_den;
} path_gate;
```

The path IR should be temporary. Once all supported gates have emitted SOP terms and equality/parity constraints have been simplified or quadratized, the solver should operate on `sop_ir`.

### 3.3 `wmc_ir`: CNF plus weights

The WMC exporter should build a separate weighted-CNF IR instead of overloading the SOP IR.

Suggested fields:

```c
typedef struct wmc_lit {
  uint32_t var;
  bool neg;
} wmc_lit;

typedef struct wmc_clause {
  uint32_t len;
  wmc_lit *lits;
} wmc_clause;

typedef struct wmc_instance {
  uint32_t nvars;
  uint32_t nclauses;
  wmc_clause *clauses;

  // Literal weights in a selected coefficient domain.
  // For exact cyclotomic export, these may be symbolic labels.
  void *weights;

  uint32_t *projected_vars;
  uint32_t nprojected;
} wmc_instance;
```

---

## 4. Repository layout

Recommended source layout:

```text
.
├── meson.build
├── meson_options.txt
├── include/dlx4sop/
│   ├── qsop.h
│   ├── qsop_parse.h
│   ├── qsop_solve.h
│   ├── path_ir.h
│   ├── wmc_ir.h
│   ├── zx_ir.h
│   └── util.h
├── src/
│   ├── core/
│   │   ├── qsop.c
│   │   ├── qsop_parse.c
│   │   ├── qsop_write.c
│   │   ├── qsop_normalize.c
│   │   ├── residue.c
│   │   ├── hash.c
│   │   ├── trail.c
│   │   └── graph.c
│   ├── solve/
│   │   ├── branch.c
│   │   ├── components.c
│   │   ├── treewidth.c
│   │   ├── rankwidth_sign.c
│   │   └── rankwidth_labelled.c
│   ├── import/
│   │   ├── qasm_lexer.c
│   │   ├── qasm_parser.c
│   │   ├── qasm_to_path.c
│   │   ├── mqt_qasm.c
│   │   ├── qgraph_json.c
│   │   └── zx_to_qsop.c
│   ├── export/
│   │   ├── qsop_to_wmc.c
│   │   ├── wmc_dimacs.c
│   │   ├── qsop_to_fdd.c
│   │   ├── qsop_to_qgraph.c
│   │   └── qsop_to_qasm.c
│   ├── simd/
│   │   ├── scalar.c
│   │   ├── avx2.c
│   │   ├── avx512.c
│   │   └── neon.c
│   └── cli/
│       ├── sop_check.c
│       ├── sop_normalize.c
│       ├── sop_solve.c
│       ├── sop_stats.c
│       ├── qasm2sop.c
│       ├── zx2sop.c
│       ├── sop2wmc.c
│       ├── sop2fdd.c
│       └── sop2zx.c
├── tests/
│   ├── unit/
│   ├── golden/
│   ├── fuzz/
│   └── integration/
├── bench/
│   ├── inputs/
│   ├── scripts/
│   └── results/
├── docs/
│   ├── FORMAT.md
│   ├── ARCHITECTURE.md
│   └── BENCHMARKS.md
└── tools/
    ├── run-clang-format.sh
    ├── run-clang-tidy.sh
    ├── run-valgrind.sh
    └── compare-tools.sh
```

The CLI utilities should be thin wrappers around library functions. Most logic should live in `src/core`, `src/solve`, `src/import`, and `src/export`.

---

## 5. Build system and toolchain

Use Meson as the only primary build system.

### 5.1 Meson options

Suggested `meson_options.txt`:

```meson
option('simd', type: 'combo',
       choices: ['auto', 'none', 'sse2', 'avx2', 'avx512', 'neon'],
       value: 'auto',
       description: 'SIMD backend selection')

option('with_gmp', type: 'boolean', value: true,
       description: 'Use GMP for exact large integer counts')

option('with_json', type: 'boolean', value: true,
       description: 'Build JSON-based import/export utilities')

option('build_fuzzers', type: 'boolean', value: false,
       description: 'Build parser fuzz targets')

option('developer_warnings', type: 'boolean', value: true,
       description: 'Enable strict compiler warnings')
```

Suggested build commands:

```sh
meson setup build \
  -Dbuildtype=debugoptimized \
  -Dwarning_level=3 \
  -Dwerror=true \
  -Dsimd=auto

meson compile -C build
meson test -C build
```

Sanitizer builds:

```sh
meson setup build-asan \
  -Db_sanitize=address,undefined \
  -Dbuildtype=debug \
  -Dwarning_level=3 \
  -Dwerror=true

meson test -C build-asan
```

Release benchmark builds:

```sh
meson setup build-release \
  -Dbuildtype=release \
  -Db_lto=true \
  -Dsimd=auto

meson compile -C build-release
```

### 5.2 Formatting

Use `clang-format` with LLVM style. Add a checked-in `.clang-format`:

```yaml
BasedOnStyle: LLVM
ColumnLimit: 100
IndentWidth: 2
SortIncludes: true
AllowShortFunctionsOnASingleLine: Empty
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
```

Recommended target:

```sh
ninja -C build clang-format
```

or script:

```sh
tools/run-clang-format.sh
```

### 5.3 Static analysis

Use `clang-tidy` as the main static analyzer. The initial profile should catch correctness bugs without drowning the codebase in style noise.

Suggested `.clang-tidy`:

```yaml
Checks: >
  bugprone-*,
  clang-analyzer-*,
  performance-*,
  portability-*,
  readability-misleading-indentation,
  readability-redundant-*
WarningsAsErrors: >
  bugprone-*,
  clang-analyzer-*
HeaderFilterRegex: '^(src|include)/.*'
FormatStyle: file
```

Run:

```sh
tools/run-clang-tidy.sh build
```

### 5.4 Valgrind

Valgrind should be part of the normal developer cleanliness story.

Use it for:

- parser tests;
- normalization tests;
- solver tests on small and medium instances;
- import/export round-trip tests;
- cache/trail backtracking stress tests.

Suggested script:

```sh
valgrind \
  --tool=memcheck \
  --leak-check=full \
  --show-leak-kinds=all \
  --track-origins=yes \
  --error-exitcode=99 \
  ./build/src/cli/sop-check tests/golden/bell.qsop
```

Also use:

```sh
valgrind --tool=cachegrind ./build-release/src/cli/sop-solve bench/inputs/example.qsop
valgrind --tool=callgrind ./build-release/src/cli/sop-solve bench/inputs/example.qsop
```

Sanitizers catch many bugs earlier; Valgrind remains useful for leak checks, undefined allocation patterns, and cache profiling.

---

## 6. Unix-style utilities

Each utility should do one job, read from files or stdin, and write to stdout unless an explicit output path is given.

### 6.1 Core SOP utilities

```text
sop-check       validate and canonicalize a QSOP file
sop-normalize   parse, combine duplicates, fold self-loops, output canonical QSOP
sop-stats       print variable count, edge count, modulus, components, degree, width estimates
sop-pin         pin boundary variables and output the residual QSOP
sop-solve       evaluate a QSOP exactly or approximately
sop-width       compute or estimate treewidth/rankwidth-style parameters
```

Examples:

```sh
sop-check circuit.qsop
sop-normalize < raw.qsop > normalized.qsop
sop-stats --json normalized.qsop
sop-solve --backend=auto --exact normalized.qsop
sop-width --treewidth=minfill --rankwidth=heuristic normalized.qsop
```

### 6.2 Import utilities

```text
qasm2sop        OpenQASM static-subset circuit to QSOP
mqtqasm2sop     MQT-flavored OpenQASM to QSOP; may be an alias of qasm2sop --mqt-layout
zx2sop          ZX diagram to QSOP, initially PyZX/Quantomatic .qgraph JSON
```

Examples:

```sh
qasm2sop --modulus=8 --input 000 --output 000 circuit.qasm > circuit.qsop
qasm2sop --mqt-layout --input 0x0 --output 0x0 mqt_bench.qasm > mqt_bench.qsop
zx2sop --format=qgraph diagram.qgraph > diagram.qsop
```

### 6.3 Export utilities

```text
sop2wmc         QSOP to weighted CNF / projected weighted CNF for Ganak-style counters
sop2fdd         QSOP to a FeynmanDD-runnable artifact when circuit synthesis is possible
sop2zx          QSOP to a ZX-tool-compatible graph representation, initially .qgraph
sop2qasm        QSOP to OpenQASM when the QSOP is circuit-extractable
```

Examples:

```sh
sop2wmc --target=ganak --mode=laurent circuit.qsop > circuit.wcnf
sop2fdd --gate-set=T circuit.qsop --out-dir fdd_case/
sop2zx --format=qgraph circuit.qsop > circuit.qgraph
```

### 6.4 Benchmark utilities

```text
sop-bench-run       run dlx4sop on a benchmark manifest
sop-bench-export    export all benchmarks to WMC/FeynmanDD/ZX targets
sop-bench-collect   collect timings, memory, width statistics, and result hashes
sop-diff            compare two exact results or cyclotomic count vectors
```

Benchmark utilities should produce JSONL so results can be appended, filtered, and analyzed with standard tools.

---

## 7. OpenQASM import

OpenQASM should be the first circuit interchange target. This avoids tying the C23 codebase directly to Qiskit's Python object model while still supporting Qiskit-generated and Qiskit-readable circuits.

### 7.1 Supported subset

Start with a static unitary subset of OpenQASM 2 and OpenQASM 3:

- qubit declarations;
- classical bit declarations only when needed for parsing measurements;
- `include "qelib1.inc";` and `include "stdgates.inc";` as recognized standard-library markers;
- gates:
  - `h`, `x`, `z`, `s`, `sdg`, `t`, `tdg`;
  - `cx`, `cz`, `ccx`, `swap`;
  - `rz(k*pi/m)`, `p(k*pi/m)`, `phase(k*pi/m)` when the angle is a rational multiple of `pi`;
  - optionally `rx`, `ry` only after decomposition into supported finite-phase gates;
- barriers ignored;
- measurements supported only as boundary/output annotations, not dynamic feedback.

Reject clearly, with source locations:

- dynamic circuits;
- `if`, `while`, `for`, `switch`, and runtime-dependent control flow;
- delays, timing, calibration, pulse-level constructs;
- arbitrary float angles that are not rational multiples of `pi`;
- arbitrary user-defined gates unless the importer can inline them into the supported subset.

### 7.2 Parser strategy

Use a small C parser for the supported subset. The importer should not try to implement the full OpenQASM semantic model at first.

Recommended strategy:

1. Lex and parse the file using a grammar-informed hand-written recursive parser or a generated C parser.
2. Build a small AST for declarations, standard gates, and barriers.
3. Resolve qubit names and indices.
4. Inline supported user-defined gates if their bodies contain only supported gates.
5. Convert the AST to `path_ir` by scanning gates in order.
6. Convert `path_ir` to a normalized QSOP.

The importer should have a strict mode and a permissive mode:

```sh
qasm2sop --strict file.qasm
qasm2sop --permissive --ignore-barriers file.qasm
```

Strict mode should be used for benchmarks.

### 7.3 Boundary handling

For amplitude computation, the importer should support explicit boundary pins:

```sh
qasm2sop circuit.qasm --input 000101 --output 111000 > amp.qsop
```

For all-amplitudes or sampling-like tasks, it should be able to leave selected boundary variables open:

```sh
qasm2sop circuit.qasm --input 000 --open-output > open_output.qsop
```

The output QSOP header should record boundary metadata in comments:

```text
c boundary input q0_0 0
c boundary output q0_17 free
```

The solver may ignore these comments, but benchmark tools can use them.

---

## 8. Munich Quantum Toolkit support

MQT should be treated as a first-class benchmark baseline.

### 8.1 First supported MQT route: OpenQASM

The first MQT-compatible route should be:

```text
MQT Bench / MQT Core OpenQASM 2 or 3
    -> qasm2sop --mqt-layout
    -> QSOP
```

This is attractive because MQT Bench exports OpenQASM-style benchmark circuits, and MQT Core uses OpenQASM as an interchange format.

### 8.2 MQT layout/permutation comments

MQT Core emits two non-standard OpenQASM comment lines to preserve layout and output permutations:

```text
// i <initial-layout>
// o <output-permutation>
```

The importer should preserve and interpret these when `--mqt-layout` is enabled.

Recommended behavior:

```sh
qasm2sop --mqt-layout mqt.qasm > mqt.qsop
```

- Parse `// i ...` as the initial logical-to-physical layout.
- Parse `// o ...` as the final output permutation.
- Store both in QSOP comments.
- Apply them when mapping user-supplied input/output bitstrings.

If the comments are missing, `--mqt-layout` should fall back to ordinary OpenQASM order and print a warning unless `--quiet` is set.

### 8.3 Later MQT routes

Later, add optional adapters for:

- QPY files through a small Python helper that emits OpenQASM or the internal path format;
- MQT QCO/MLIR only if a stable textual export path is available and useful;
- MQT YAQS only if state/operator-level tensor-network benchmarks become important.

The core C23 solver should not depend on MQT libraries.

---

## 9. ZX import and export

ZX support is needed both for additional inputs and for comparison with ZX-calculus tools.

### 9.1 First ZX input format: PyZX / Quantomatic `.qgraph`

Use the PyZX/Quantomatic JSON `.qgraph` format as the first practical ZX diagram format.

Initial command:

```sh
zx2sop --format=qgraph diagram.qgraph > diagram.qsop
```

Supported first fragment:

- graph-like ZX diagrams;
- Z-spiders only;
- finite phases represented as rational multiples of `pi`;
- Hadamard edges;
- boundary vertices or designated input/output nodes;
- no arbitrary symbolic phases.

Translation rule:

- one Boolean variable per spider;
- spider phase \(\alpha = 2\pi a/r\) becomes unary term `u v a`;
- Hadamard edge becomes sign edge `e u v`, i.e. coefficient `r/2`;
- boundary pins become fixed-variable reductions or comments, depending on the requested output.

### 9.2 Non-graph-like ZX

For non-graph-like diagrams, the importer should initially reject with a diagnostic:

```text
error: diagram is not graph-like; run a ZX graphification pass first
```

Later options:

1. call a PyZX preprocessing helper outside the C core;
2. implement a small graphification pass for a finite supported fragment;
3. accept mixed X/Z spiders only when a local conversion is obvious.

### 9.3 QSOP to ZX

The first export target should be `.qgraph`:

```sh
sop2zx --format=qgraph circuit.qsop > circuit.qgraph
```

Export cases:

- sign-edge QSOP maps cleanly to graph-like ZX;
- labelled two-qubit phases may require phase-gadget encodings;
- compact Toffoli gadgets can be exported as the quadratic gadget graph, not necessarily as a native Toffoli diagram;
- arbitrary labelled QSOPs should preserve semantics, even if the resulting ZX graph is not compact.

When a QSOP cannot be faithfully exported to the chosen ZX fragment, `sop2zx` should fail clearly instead of silently changing semantics.

---

## 10. WMC / Ganak export

The WMC exporter exists for baseline comparison against weighted model counters such as Ganak.

### 10.1 Export target

The primary target should be a DIMACS-like weighted CNF / projected weighted CNF file:

```sh
sop2wmc --target=ganak --mode=laurent instance.qsop > instance.wcnf
```

Ganak supports weighted and projected weighted model counting with weights specified in comment directives. It also supports multiple coefficient domains, including integers, rationals, complex numbers, finite fields, and polynomial/Laurent-polynomial modes. The exporter should therefore support more than one exactness strategy.

### 10.2 Encoding QSOP phases into CNF

For a QSOP with unary and quadratic terms:

\[
\sum_i b_i x_i + \sum_{i<j}q_{ij}x_i x_j,
\]

there are two natural encodings.

#### Unary terms

A unary phase can be represented by a literal weight on `x_i`:

```text
w(x_i=true)  = omega_r^{b_i}
w(x_i=false) = 1
```

#### Quadratic terms

A quadratic phase can be represented by an auxiliary variable:

\[
a_{ij} \leftrightarrow (x_i \wedge x_j)
\]

with weight

```text
w(a_ij=true)  = omega_r^{q_ij}
w(a_ij=false) = 1
```

CNF for `a <-> x & y`:

```text
¬a ∨ x
¬a ∨ y
a ∨ ¬x ∨ ¬y
```

This gives a generic WMC instance whose satisfying assignments correspond to SOP assignments plus deterministic auxiliary variables.

### 10.3 Exact cyclotomic strategy

For `r = 2` or `r = 4`, complex rational weights may be enough:

- \(\omega_2=-1\),
- \(\omega_4=i\).

For `r = 8`, `r = 16`, and similar moduli, direct complex rational weights are not sufficient if we want exactness, because roots such as \(e^{i\pi/4}\) involve algebraic numbers.

Preferred exact strategies:

1. **Laurent-polynomial mode:** represent \(\omega_r\) as a symbolic variable `w`, ask Ganak to compute a Laurent polynomial, and reduce modulo the cyclotomic relation externally.
2. **Residue-sliced counting:** produce one projected count per residue class and reconstruct
   \[
   \sum_{a=0}^{r-1} C_a \omega_r^a
   \]
   outside the WMC solver.
3. **Approximate/MPFR complex mode:** acceptable only for performance comparisons where exact equality is not required.

The default benchmark mode should be exact.

### 10.4 Projected counting

If auxiliary variables are introduced for monomials, they should not change the count. Use projected model counting where appropriate:

- project onto original SOP variables for model-count equivalence;
- keep auxiliaries deterministic;
- store a comment mapping auxiliary variables back to SOP terms.

Example comments:

```text
c sop var 1 x_0
c sop aux 17 and 3 9 coeff 6
```

---

## 11. FeynmanDD export

FeynmanDD comparison should be supported, but the export contract needs to be honest.

The public FeynmanDD workflow is circuit-oriented: examples invoke the executable with a QASM file and a gate-set JSON file. Therefore, `sop2fdd` should not claim that FeynmanDD natively consumes our QSOP format unless a native adapter is added.

### 11.1 Preferred route: preserve original circuit

When a QSOP was imported from OpenQASM, preserve the original circuit metadata:

```text
original_qasm = path/to/input.qasm
supported_gate_set = T | default_2 | google | custom
```

Then `sop2fdd` can emit:

```text
fdd_case/
├── circuit.qasm
├── gate_set.json
├── README.md
└── run.sh
```

The generated `run.sh` should be explicit, for example:

```sh
./build/src/cudd_circuit_bdd \
  -f fdd_case/circuit.qasm \
  -g fdd_case/gate_set.json \
  -m amplitude
```

### 11.2 Synthesis route

If no original circuit is available, attempt circuit synthesis from the QSOP:

- unary phases -> `z`, `s`, `t`, or finite `rz/p` gates;
- sign edges -> `cz` or `h`-edge-compatible circuit gadgets;
- labelled pair terms -> controlled phase gates if the selected FeynmanDD gate set supports them;
- compact Toffoli gadgets -> either emit the original `ccx` if metadata says it came from Toffoli, or emit the explicit quadratic gadget circuit if supported.

If synthesis would leave the selected FeynmanDD gate set, fail clearly:

```text
error: QSOP contains labelled coefficient 3 mod 8; selected FeynmanDD gate set T.json cannot represent it directly
```

### 11.3 Native FeynmanDD-like SOP JSON

As a later extension, `sop2fdd` may emit an intermediate JSON file for a future FeynmanDD adapter:

```json
{
  "kind": "quadratic_sop",
  "modulus": 8,
  "normalization_h": 12,
  "constant": 0,
  "unary": [[0, 1], [7, 4]],
  "quadratic": [[0, 1, 4], [2, 3, 2]]
}
```

This should be labelled as an adapter format, not as a current FeynmanDD input format.

---

## 12. ZX-calculus tool output

ZX-calculus tools are useful both as baselines and as preprocessors.

Output modes:

```sh
sop2zx --format=qgraph instance.qsop > instance.qgraph
sop2qasm --target=pyzx instance.qsop > instance.qasm
```

Use cases:

- compare simplification and extraction quality;
- run PyZX optimizations before importing back into QSOP;
- visualize QSOP interaction graphs as ZX diagrams;
- sanity-check graph-like ZX-to-QSOP round trips.

Round-trip tests should include:

```text
OpenQASM -> QSOP -> qgraph -> PyZX -> QASM -> QSOP
qgraph   -> QSOP -> qgraph
QSOP sign-edge -> qgraph -> QSOP
```

---

## 13. Solver architecture

### 13.1 Dancing-cells residual state

The solver should use a mutable residual object with reversible updates:

```c
typedef struct residual {
  uint32_t nactive;
  bitset_t active_vars;
  bitset_t active_edges;

  uint16_t *unary;
  uint16_t *edge_q;
  uint32_t constant;

  trail_t trail;
  hash_state_t hash;
} residual;
```

Branching on `x_v = 0`:

- deactivate `v`;
- deactivate incident edges.

Branching on `x_v = 1`:

- `constant += unary[v] mod r`;
- for each active neighbor `u`, `unary[u] += q[v,u] mod r`;
- deactivate `v`;
- deactivate incident edges.

Every mutation should be pushed onto a trail and undone in reverse order. Avoid copying whole residual graphs at search nodes.

### 13.2 Hashing and memoization

Use an incremental hash for residual subproblems. The hash must include:

- active variables;
- active labelled edges;
- unary residues;
- constant residue, if the cache stores absolute values;
- modulus;
- component identity or canonical local renumbering.

A Zobrist-style hash is appropriate:

```text
H ^= zobrist_active_var[v]
H ^= zobrist_unary[v][old]
H ^= zobrist_unary[v][new]
H ^= zobrist_edge[u,v,q]
```

Cache values should be exact residue-count vectors:

\[
(C_0, C_1, \ldots, C_{r-1})
\]

where `C_a` counts assignments with phase residue `a`. This keeps complex numbers out of the solver core.

### 13.3 Component decomposition

After simplification or branching, split disconnected components. For independent components:

\[
Z(G_1 \sqcup G_2)=Z(G_1)Z(G_2),
\]

implemented as convolution of residue-count vectors modulo `r`.

For sign-edge mode, component detection can use binary adjacency. For labelled mode, use the support graph `q_uv != 0`.

### 13.4 Width-aware heuristics

Variable selection should combine cheap local scores and optional width estimates:

- active degree;
- number of incident nonzero labelled coefficients;
- component size after deletion;
- fill-in estimate for treewidth elimination;
- labelled boundary-signature estimate for rankwidth-style cuts;
- cache-hit likelihood.

The heuristic should be pluggable:

```sh
sop-solve --branch=degree
sop-solve --branch=minfill
sop-solve --branch=rankwidth-cut
sop-solve --branch=auto
```

---

## 14. Treewidth and rankwidth backends

### 14.1 Treewidth backend

The treewidth backend should work for both sign-edge and labelled QSOPs. It operates on the support graph:

\[
G_Q=(V,\{\{i,j\}:q_{ij}\neq 0\}).
\]

Factors carry coefficients modulo `r`. Runtime depends on treewidth and fixed modulus.

### 14.2 Rankwidth backend: sign mode

For sign-edge QSOPs, the cut matrix is binary. The fast backend can use:

- bitsets;
- GF(2) row reduction;
- popcount-based signature evaluation;
- compact parity signatures.

This should remain the fastest Clifford+T path.

### 14.3 Rankwidth backend: labelled mode

For labelled QSOPs, the cross-cut term is

\[
x_A^T Q[A,\bar A]x_{\bar A}\pmod r.
\]

The backend must use labelled boundary signatures over \(\mathbb Z_r\), not just binary support rank.

For fixed `r`, store signatures as packed `uint8_t` or `uint16_t` vectors. Use CRT decomposition or direct signature tables for composite moduli such as `r = 8`.

Do not claim that ordinary unlabelled graph rankwidth controls labelled QSOP runtime. The relevant parameter is labelled cut-signature width.

---

## 15. SIMD and architecture-aware implementation

C23 gives us a low-level, predictable implementation language. SIMD should be used aggressively where it pays off, but with scalar correctness paths and isolated architecture-specific files.

### 15.1 Hot loops

Likely SIMD targets:

- residue-count vector shifts and accumulations;
- residue-count vector convolution modulo `r`;
- labelled boundary-signature generation;
- packed unary updates during branching;
- batch evaluation of `x_A^T Q[A,B]` signatures;
- hash-table probing over compact keys;
- bitset operations for sign-edge graphs;
- GF(2) row operations for sign rankwidth;
- small-modulus table transitions in treewidth DP.

### 15.2 Data layout

Prefer structure-of-arrays over array-of-structures for hot data:

```c
uint32_t *edge_u;
uint32_t *edge_v;
uint16_t *edge_q;
uint8_t  *edge_active;
```

Use alignment for arrays used in vector kernels:

```c
void *sop_aligned_alloc(size_t alignment, size_t size);
```

Keep separate kernels for common moduli:

```text
r = 2
r = 4
r = 8
r = 16
r = 24
generic r
```

### 15.3 SIMD backend structure

Provide a scalar reference backend and optional optimized backends:

```text
src/simd/scalar.c
src/simd/avx2.c
src/simd/avx512.c
src/simd/neon.c
```

Dispatch policy:

- compile-time selection via Meson `-Dsimd=...`;
- runtime dispatch when `-Dsimd=auto` is enabled;
- scalar fallback always available.

Example interface:

```c
typedef struct sop_kernels {
  void (*residue_shift_add)(uint64_t *dst, const uint64_t *src,
                            uint32_t len, uint32_t shift, uint32_t r);
  void (*count_convolve)(uint64_t *dst, const uint64_t *a, const uint64_t *b,
                         uint32_t r);
  void (*sig_add_mod)(uint8_t *dst, const uint8_t *src,
                      uint32_t len, uint32_t r);
} sop_kernels;
```

### 15.4 SIMD correctness policy

Every SIMD kernel must have:

- scalar reference implementation;
- randomized equivalence tests;
- boundary tests for non-multiple-of-vector-width lengths;
- sanitizer tests;
- Valgrind tests when the backend can run under Valgrind;
- microbenchmarks.

No SIMD code should be allowed to change exact arithmetic semantics.

---

## 16. Exact arithmetic policy

The core solver should not use complex numbers internally.

Represent partial sums as residue-count vectors:

\[
C[a]=\#\{x:Q(x)\equiv a\pmod r\}.
\]

The final value is

\[
2^{-h/2}\sum_{a=0}^{r-1}C[a]\omega_r^a.
\]

Internal arithmetic:

- residues: `uint8_t`, `uint16_t`, or `uint32_t`;
- counts: `uint64_t`, `unsigned __int128`, or GMP integers;
- hash values: `uint64_t` or `uint128` pair;
- final presentation: symbolic cyclotomic vector by default, optional floating complex approximation.

Suggested output modes:

```sh
sop-solve --format=residue-vector instance.qsop
sop-solve --format=cyclotomic instance.qsop
sop-solve --format=complex64 instance.qsop
```

Only the final display path should need complex floating-point arithmetic.

---

## 17. Import/export compatibility matrix

| Direction | Utility | First supported format | Notes |
|---|---|---|---|
| OpenQASM -> QSOP | `qasm2sop` | OpenQASM 2 static subset; OpenQASM 3 static subset | Main circuit importer |
| MQT -> QSOP | `qasm2sop --mqt-layout` or `mqtqasm2sop` | MQT-flavored OpenQASM with layout comments | First MQT baseline path |
| ZX -> QSOP | `zx2sop` | PyZX/Quantomatic `.qgraph` JSON | Graph-like finite-phase diagrams first |
| QSOP -> WMC | `sop2wmc` | Ganak-style weighted/projected CNF | Exact Laurent/cyclotomic mode preferred |
| QSOP -> FeynmanDD | `sop2fdd` | QASM + gate-set JSON where synthesizable | Preserve original circuit when possible |
| QSOP -> ZX | `sop2zx` | PyZX/Quantomatic `.qgraph` JSON | Sign-edge cleanest; labelled uses gadgets |
| QSOP -> OpenQASM | `sop2qasm` | OpenQASM 2/3 circuit fragment | Only when circuit extraction/synthesis succeeds |

---

## 18. Benchmark strategy

Benchmarks should be reproducible, metadata-rich, and multi-target.

### 18.1 Benchmark manifest

Use JSONL manifests:

```json
{"id":"bell_2q","source":"hand","format":"qasm","path":"bench/inputs/bell.qasm","input":"00","output":"00"}
{"id":"mqt_qft_12","source":"mqt-bench","format":"qasm3","path":"bench/inputs/mqt/qft_12.qasm","input":"0x000","output":"open"}
{"id":"pyzx_rand_30","source":"pyzx","format":"qgraph","path":"bench/inputs/zx/rand_30.qgraph"}
```

### 18.2 Benchmark outputs

For every benchmark, collect:

- original format;
- import time;
- QSOP variable count;
- QSOP edge count;
- sign/labelled mode;
- modulus;
- number of components;
- treewidth estimate;
- rankwidth/signature estimate;
- solve time;
- peak memory;
- exact residue-vector result hash;
- exported WMC variable/clause count;
- exported FeynmanDD artifact status;
- exported ZX artifact status.

### 18.3 Comparison baselines

The baseline harness should support:

- `dlx4sop` native solving;
- Ganak on exported WMC instances;
- FeynmanDD on generated QASM + gate-set JSON artifacts;
- PyZX/ZX-calculus preprocessing and extraction where applicable;
- MQT-generated benchmark suites through OpenQASM import;
- Qiskit or another statevector simulator only for small correctness checks.

---

## 19. Testing strategy

### 19.1 Unit tests

Test:

- parser errors and diagnostics;
- canonicalization;
- duplicate edge accumulation;
- self-loop folding;
- pinning;
- component splitting;
- trail undo;
- hash stability;
- residue-vector arithmetic;
- Toffoli/CCZ gadget correctness.

### 19.2 Golden tests

Maintain small exact examples:

- single `H`;
- `HH = I` interference;
- Bell state;
- uncompute circuit;
- `H-T-H`;
- `CZ` phase test;
- `CCZ` gadget;
- Toffoli gadget;
- MQT OpenQASM with layout comments;
- PyZX `.qgraph` graph-like diagram.

### 19.3 Fuzz tests

Fuzz:

- QSOP parser;
- OpenQASM subset parser;
- `.qgraph` JSON parser;
- WMC exporter;
- normalizer.

Use libFuzzer or AFL++ targets guarded by `-Dbuild_fuzzers=true`.

### 19.4 Cleanliness checks

Continuous checks should include:

```sh
meson test -C build
meson test -C build-asan
meson test -C build-ubsan
tools/run-clang-format.sh --check
tools/run-clang-tidy.sh build
tools/run-valgrind.sh build
```

Performance CI can be separate and should not gate every commit.

---

## 20. Error-handling policy

All command-line tools should follow the same policy:

- print human-readable diagnostics to stderr;
- include filename, line, and column when possible;
- exit nonzero on semantic loss;
- never silently approximate exact input;
- require explicit flags for approximation or unsupported simplification.

Examples:

```text
error: circuit.qasm:17:8: angle pi/7 requires modulus 14, but --modulus=8 was requested
error: diagram.qgraph: unsupported symbolic phase "theta"
error: instance.qsop: labelled coefficient 3 mod 8 cannot be exported as sign-edge ZX without gadgets; pass --gadgets
```

---

## 21. Roadmap

### Milestone 0: build skeleton

- Meson project.
- C23 feature checks.
- `clang-format`, `clang-tidy`, sanitizer, Valgrind scripts.
- Minimal `sop-check`.

### Milestone 1: QSOP core

- Parse/write sign-edge and labelled QSOP.
- Normalize duplicates/self-loops.
- Pin variables.
- Brute-force exact evaluator for small instances.

### Milestone 2: dancing-cells scalar solver

- Mutable residual state.
- Reversible trail.
- Component decomposition.
- Hash memoization.
- Exact residue-vector output.

### Milestone 3: width-aware backends

- Treewidth elimination backend.
- Sign-edge rankwidth backend.
- Labelled signature backend prototype.
- Width-stat utility.

### Milestone 4: OpenQASM and MQT import

- Static OpenQASM 2 subset.
- Static OpenQASM 3 subset.
- MQT layout/permutation comments.
- Gate-level path IR.
- Toffoli/CCZ quadratic gadget emission.

### Milestone 5: WMC/Ganak export

- CNF auxiliary generation.
- Direct weighted CNF export for simple roots.
- Laurent/cyclotomic exact mode.
- Benchmark comparison harness.

### Milestone 6: ZX import/export

- PyZX/Quantomatic `.qgraph` parser/writer.
- Graph-like finite-phase ZX import.
- Sign-edge QSOP to `.qgraph` export.
- Labelled QSOP phase-gadget export.

### Milestone 7: FeynmanDD export

- Preserve original QASM route.
- Generate FeynmanDD run directory.
- Gate-set JSON selection.
- Circuit synthesis for supported QSOP fragments.

### Milestone 8: SIMD and architecture tuning

- Scalar microbenchmark baseline.
- AVX2 residue-vector kernels.
- AVX512 kernels where useful.
- NEON kernels for ARM.
- Runtime dispatch.
- Cachegrind/callgrind-driven layout improvements.

---

## 22. External references to track

These are implementation references, not hard dependencies of the C23 core.

- OpenQASM specification and grammar: https://github.com/openqasm/openqasm
- IBM/Qiskit OpenQASM interop docs: https://quantum.cloud.ibm.com/docs/en/guides/interoperate-qiskit-qasm2
- PyZX documentation and `.qgraph` support: https://pyzx.readthedocs.io/
- PyZX repository: https://github.com/zxcalc/pyzx
- Ganak repository and weighted/projected model-counting format: https://github.com/meelgroup/ganak
- FeynmanDD repository: https://github.com/alexandrupaler/feynman_DD
- MQT Core documentation: https://mqt.readthedocs.io/projects/core/
- MQT Bench: https://github.com/cda-tum/mqt-bench
- Tensor-network-to-WMC design note: uploaded project document `Tensor_network_to_WMC.pdf`

