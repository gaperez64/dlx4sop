# Lean 4 + Mathlib Formalization Plan — Quadratic SOP / Rank-Width Quantum-Circuit Simulation

Formalizing *"Quadratic Sums-of-Powers for Fixed-Parameter Tractable Quantum-Circuit
Simulation"* (de Colnet, Geerts, Hai, Laarman, Lee, Pérez) — `main.tex`.

This plan follows `lean-e2e-verification-playbook.md`: **abstract interface → concrete
instance → capstone**, **crux-first**, **one named lemma at a time**, **`#print axioms`
is truth**, formalization-as-oracle, tight-leash agents.

> ## ✅ Locked scope (confirmed)
> - **Target: Core + honest capstone** — Packages **A + B + C** (Phases 0–4). The final
>   theorem is about the **real circuit amplitude**. Headline estimate **≈ 2.5–4 months**.
> - **Runtime = semantic operation-count bounds** (Decision 1A): explicit `Nat` cost
>   function + proven inequality `cost ≤ r·4^k·poly(n)`.
> - **Package D deferred** (tensor-network/line-graph/`cc`, minor-line lemma, `Γ_{h,t}`
>   separation, WMC transfer). **Package E (gadget universality) also out of scope** for
>   this pass — it is an expressivity extra, not needed for the Clifford+T capstone.
> - So the concrete work is: **A (foundations) → B (SOP core + DP + crux + semantic runtime
>   + Fourier + linear-layout) → C (quantum coupling + Clifford+T) → capstone wiring.**

---

## Progress (live)

Lean project lives in [`formal/`](formal/) (Lean 4.32.0 + Mathlib v4.32.0, cache pulled).
Verified with `lake build` + `#print axioms`; "clean" = axioms ⊆ `[propext, Classical.choice, Quot.sound]`.

| Item | File | Status |
|---|---|---|
| Project scaffold, Mathlib reuse surface validated | `formal/` | ✅ green |
| `SopInstance`, `f`, `S(f)`, `N` (def:sopcount); `half_mul_reduce` (η=r/2 identity) | `Formal/Core/SopInstance.lean` | ✅ green, clean |
| `RTree` decomposition + `RankDecomp` | `Formal/Foundations/RankDecomp.lean` | ✅ green, clean |
| `adj`/`sig`/`phi`/`rep`/`rawCross`/`crossPar` + **`chi_well_defined`** (lem:chi-well-defined, the F₂ heart) | `Formal/Core/Signature.lean` | ✅ green, **clean** |
| DP definitions `Dalg`/`Dspec`/`crossTerm`/`combineSig` | `Formal/Core/DP.lean` | ✅ green, sorry-free |
| **`thm:dp-correct`** (`Dalg_eq_Dspec`) + **`dp_returns_counts`** (DP returns `N_j`) | `Formal/Core/DPCorrect.lean` | ✅ green, **clean** |
| `lem:signatures-le-2^k` (`card_image_vecMul` = `2^rank`; `_le` ≤ `2^k`) | `Formal/Foundations/CutRank.lean` | ✅ green, **clean** |
| Phase 3 remainder (cost model, `thm:sop-rw`, Fourier, linear-layout) | — | 🔨 next |
| Phase 4 (quantum capstone), Phase 5 (release) | — | ⏳ pending |

**Milestones reached (all axiom-clean = `[propext, Classical.choice, Quot.sound]`, whole tree
`sorry`-free):**
1. `chi_well_defined` — the F₂ mathematical heart (lem:chi-well-defined).
2. **`thm:dp-correct`** (`Dalg_eq_Dspec`) — the DP computes the residue counts. The paper's
   headline *correctness* result, machine-checked. (Cross-verified: 3 independent parallel proofs
   of the join all landed axiom-clean.)
3. `dp_returns_counts` — the algorithm returns exactly `N_j`.
4. `card_image_vecMul` — the `2^rank` cardinality bound that powers every runtime theorem.

**Phase 2 (the crux / go-no-go) is complete.** The paper's "correctness of the algorithm" is done.
Remaining for full A+B+C: the runtime bounds (Phase 3, cardinality lemma already in hand) and the
quantum coupling capstone (Phase 4).

## Context

The paper's headline claims are (1) **correctness** and (2) **runtime** of a dynamic
program that evaluates a quadratic sum-of-powers (SOP) — hence a pinned quantum-circuit
amplitude — in time exponential only in the **rank-width** of the SOP variable graph
`G_C`, and polynomial in circuit size. Around this sit structural results relating
rank-width to linear-rank-width and tensor-network contraction complexity, a gate-set
universality theorem, and a conditional matrix-multiplication barrier.

Goal: a machine-checked, `sorry`-free, axiom-clean Lean 4 + Mathlib development of these
statements, with the *runtime* claim formalized honestly (see Decision 1), and a capstone
about the **real circuit amplitude** (not just an abstract SOP).

There is **no prior formalization** and rank-width does not exist in any proof assistant.
The prototype (`gaperez64/dlx4sop`) is an external reference oracle only.

---

## The crux (prove this first, in isolation)

`lem:chi-well-defined` (main.tex:962) → `thm:dp-correct` (main.tex:1077).

- **`lem:chi-well-defined`**: the crossing parity `χ(α,β) = aᵀ A[X_L,X_R] b` depends only
  on the two boundary signatures, not on the representatives `a,b`. A pure `F₂`
  linear-algebra fact (~28 lines on paper): if `σ_L(a')=σ_L(a)` then
  `(a−a')ᵀA[X_L, V∖X_L]=0`; restrict to `X_R`; symmetric on the right using symmetry of `A`.
- **`thm:dp-correct`**: the DP table invariant `D_u[σ,s] = #{z : σ_u(z)=σ, φ_u(z)=s}`, by
  induction over the rooted decomposition tree. The only nontrivial step is the join, and
  it collapses onto `lem:chi-well-defined` plus the identity `η·m ≡ η·(m mod 2) (mod r)`
  for `η = r/2` and disjoint-variable counting.

**Everything downstream is bookkeeping** on top of the crux and one cardinality lemma:

- **`lem:signatures-le-2^k`** (factor out — the paper re-proves it inline in `thm:sop-rw`
  *and* `thm:fourier-speedup`): the occurring boundary signatures at a node are exactly the
  **row space of `A[X_u, X̄_u]` over `F₂`**, so their count is `≤ 2^rank ≤ 2^k`. In Lean:
  `|Set.range (fun z => zᵀ · M)| ≤ 2 ^ M.rank` over `ZMod 2`. This single fact powers every
  runtime theorem.

If the crux + this cardinality lemma are green, the correctness and runtime theorems follow
by finite-combinatorics manipulation. **This de-risks the whole project** and is the first
milestone.

---

## Architecture: three layers (playbook §1)

**Layer 1 — abstract interface (`SopInstance` structure).** Bundle the modeling content as
fields, with positivity/well-definedness invariants baked in:

```
structure SopInstance where
  r        : ℕ           -- modulus
  hr       : Even r      -- η = r/2 is the load-bearing invariant (drives χ, DFT parity)
  V        : Type        -- variables
  instV    : Fintype V
  G        : SimpleGraph V           -- SOP variable graph (symmetric, loopless)
  b        : V → ZMod r               -- unary coefficients
  c        : ZMod r                   -- constant
-- f, S(f), N_j are defined from these; adjacency over F₂ is `G.adjMatrix (ZMod 2)`.
```

Plus a **`RootedRankDecomp`** structure: a rooted subcubic tree with a leaf-bijection to
`V`, exposing per-node `X_u ⊆ V`, `X̄_u`, and a width bound `k` with the field
`hwidth : ∀ u, (adjSubmatrix X_u X̄_u).rank ≤ k`. **Note:** correctness *and* runtime need
only this decomposition-with-width object — **not** the NP-hard rank-width *optimum*. The
minimization (`rw(G) = min over decomps …`) is needed only to *state* "FPT in rank-width"
and for the separation results (Package D).

**Layer 2 — abstract theorems over any `SopInstance` + `RootedRankDecomp`.** The DP, the
crux, the runtime bounds, the Fourier speedup, the linear-layout DP. This is where the deep
content lives; it comes out axiom-clean independent of any circuit.

**Layer 3 — concrete discharge + honest capstone.** Build a `SopInstance` from a real
circuit over `{H,T,CZ}` and prove the **coupling lemma** `⟨z|C|y⟩ = R_C⁻¹ · S(f_C)`
(main.tex:611/647). The capstone is then about the *real amplitude*, not a surrogate.

---

## Module / package DAG (one file-group per package; playbook §3, §7)

Partition strictly by file so agents never collide. New work → new files.

```
A  Foundations            cut-rank, RootedRankDecomp, row-space cardinality, (rank-width, lrw)
    │
B  Abstract SOP core  ◄───┘   f/S(f)/N_j, χ-well-defined [CRUX], dp-correct [CRUX],
    │  │  │                    signatures≤2^k, cost model, runtime theorems,
    │  │  │                    Fourier DFT + inversion, linear-layout DP, MM-barrier identity
    │  │  └────────────► E  Gadget universality (independent; Boolean-Fourier; parallelizable)
    │  └───────────────► C  Quantum → SOP: gates, circuit matrix, amplitude, coupling lemma,
    │                        Clifford+T diagonal extension  [honest capstone]
    └───────────────────► D  Structural: tensor net / line graph / cc, minor-line lemma,
                             realize-any-graph, Γ_{h,t} separation, WMC transfer
Capstone module: wires C's coupling ∘ B's dp-correct → theorem about ⟨z|C|y⟩.
```

Reuse-vs-build (from the Mathlib survey):

| Need | Status | Mathlib anchor |
|---|---|---|
| `F₂` field, matrices, `Matrix.rank`, `rank_submatrix` | **reuse** | `LinearAlgebra.Matrix.Rank`, `Data.ZMod.Basic` |
| `SimpleGraph`, `adjMatrix` | **reuse** | `Combinatorics.SimpleGraph.AdjMatrix` |
| roots of unity, `AddChar` on `ZMod r`, **`ZMod.dft` + inversion**, Gauss sums | **reuse (strong)** | `Analysis.Fourier.ZMod`, `NumberTheory.LegendreSymbol.AddCharacter`, `NumberTheory.GaussSum` |
| `Finset.sum/filter/card/image/biUnion`, disjoint-sum | **reuse** | `Algebra.BigOperators.*` |
| well-founded / tree / strong induction | **reuse** | Lean core, `Nat.strong*`, `termination_by`, FunInd |
| `Matrix.kronecker`, `unitaryGroup`, inner-product/Hilbert | **reuse (blocks only)** | `Data.Matrix.Kronecker`, `LinearAlgebra.UnitaryGroup` |
| **cut-rank** (one-liner on `Matrix.rank`) | **build (trivial)** | — |
| **rank-width / linear-rank-width / decomposition** | **build from scratch** | absent everywhere |
| **quantum circuit → amplitude semantics** | **build** (borrow gate defs from LeanQuantum) | community only |
| **algorithm cost model / FPT / runtime** | **build from scratch** | absent (see Decision 1) |
| **treewidth + graph minors** (for Package D) | **build from scratch** (major) | absent |

---

## Decision 1 — how to formalize *runtime* (order-of-magnitude fork)

Mathlib has **no cost model, no FPT, no algorithmic big-O**. Three honest options:

- **(A) Semantic operation-count bounds — RECOMMENDED.** Define an explicit `Nat`-valued
  `cost` recursively alongside the DP (number of table entries created + join operations),
  and prove `cost ≤ r · 4^k · nodeCount` with `nodeCount ≤ 2·|V|`, i.e. `2^{O(k)}·poly(n)`.
  This is *exactly what the paper's runtime proofs argue* ("arithmetic operations", "≤ r2^k
  table entries", "≤ 4^k pairs per join"). Fully provable; faithful; moderate effort.
- **(B) Cost-instrumented executable + machine model.** Write the DP in a cost monad over a
  real machine model and bound wall-clock steps. Much larger, little Mathlib support,
  re-derives (A) plus heavy plumbing. Not recommended.
- **(C) Correctness-only, runtime on paper.** Undersells the explicit "runtime" ask.

**✅ CHOSEN: (A).** It formalizes the FPT claim at the paper's own granularity.

## Decision 2 — how far into the treewidth/minor-heavy structural results (Package D)

`lem:sop-minor-line` needs **treewidth + minor-monotonicity** (both absent); the separation
(`lem:blowup-widths`, `cor:separating-family`) needs external structural theorems
(Adler–Kanté "lrw = pathwidth for forests", pathwidth of complete binary trees, treewidth of
cliques). Fully formalizing this layer means building treewidth + minors + those external
results from scratch — a multi-month sub-project on its own.

- **(A) Scoped — RECOMMENDED.** Prove the paper-specific glue and the *upper-bound*
  directions we can (`rw(Γ_{h,t}) ≤ 1` via the explicit decomposition; `realize-any-graph`);
  take the deep external structural facts (Adler–Kanté, pathwidth lower bounds,
  treewidth-minor-monotonicity) as **clearly-labeled hypotheses/`axiom`s with citations**.
  The separation then holds *relative to* standard graph-theory facts.
- **(B) Full.** Formalize treewidth, minors, and the external theorems too. +2–4 months.
- **(C) Defer Package D** entirely; deliver A+B+C+E.

**✅ CHOSEN: (C) — Package D deferred.** Deliver A + B + C + capstone this pass; revisit the
structural comparison results (and gadget universality E) as a follow-on once the core and
capstone are green and axiom-clean.

---

## Phased plan with feasibility estimates

Estimates are **AI-agent-orchestrated, tightly-supervised** effort (verify every green
myself; resume dead agents from transcript). Wide error bars — Lean index-juggling over `F₂`
submatrices and the quantum coupling are the main unknowns. "wk" = focused calendar weeks.

| Phase | Content | Est. | Risk |
|---|---|---|---|
| **0. Scaffold** | toolchain/Mathlib pin, `lake exe cache get`, `.gitignore` `/.lake/`, module skeleton, the `SopInstance` + `RootedRankDecomp` structures typechecking | 2–4 d | low |
| **1. Foundations (A)** | cut-rank; decomposition object + per-node cuts; **`lem:signatures-le-2^k`** (row-space card ≤ 2^rank) | 1–2 wk | med |
| **2. Crux (B core)** | `lem:chi-well-defined`; **`thm:dp-correct`** by tree induction; `S(f)`, `N_j`, `def:sopcount` | 2–4 wk | **high** (the heart) |
| **3. Runtime + Fourier (B)** | cost model (Decision 1A) + `thm:sop-rw`, `thm:fourier-speedup`, `cor:single-amplitude`; `ZMod.dft` inversion; `thm:linear-layout-fourier` + `cor:lrw` | 2–4 wk | med |
| **4. Quantum capstone (C)** | gate matrices H/T/CZ, circuit product, amplitude; **coupling lemma** `⟨z|C|y⟩ = R_C⁻¹ S(f_C)`; Clifford+T diagonal extension | 3–6 wk | **high** (index-heavy) |
| **5. Integrate/release** | capstone wiring, `#print axioms` audit, sorry-sweep, README (scoped claims + axiom recipe), root re-export | ~1 wk | low |
| ~~Gadget universality (E)~~ | deferred (expressivity extra; not needed for Clifford+T capstone) | — | — |
| ~~Structural (D)~~ | deferred (needs treewidth + minors from scratch) | — | — |
| ~~MM barrier~~ | deferred (needs a formal matrix-mult exponent; no Mathlib support) | — | — |

**Roll-ups (locked scope A+B+C):**

- **Core result (Phases 0–3):** *correctness + runtime of the abstract rank-width DP,
  axiom-clean.* ≈ **1.5–2.5 months.** The paper's headline ("correctness of the algorithm
  and its runtime") over the general quadratic SOP.
- **+ Honest capstone (Phase 4) + release (Phase 5):** real-circuit amplitude,
  `#print axioms`-clean. ≈ **+1–1.5 months.**
- **Target total (A+B+C):** ≈ **2.5–4 months.**

Phases 2 and 4 carry ~70% of the risk. **Do Phase 2 (the crux) first and in isolation**; if
it lands clean and axiom-clean, the rest is largely scheduling.

*(Deferred follow-ons, for reference: Package E ≈ +1.5–3 wk; Package D scoped ≈ +2–4 wk, or
+2–4 months fully self-contained with treewidth/minors/Adler–Kanté.)*

---

## Formalization-as-oracle (playbook §5) — bonus deliverable

While discharging fields, cross-check the paper and classify every discrepancy (genuine
error / boundary-degenerate / fine-but-implicit) with `file:line` + counterexample. Watch
list from the read-through: the `Even r` / `η=r/2` invariant is load-bearing (`(-1)^{x_ux_v}`
relies on it — flag any step assuming it silently); `depth(T)` vs balanced decomposition in
storage bounds; the `poly(n)` bit-size claims for `ℤ[ω_r]` values; the `t=1` degenerate case
of the separation family; open-index / pinned-segment handling in `lem:sop-minor-line`.

---

## Verification discipline (playbook §4) — definition of "done"

Per landed lemma and at every milestone, all three must agree:

1. `#print axioms <capstone>` = `[propext, Classical.choice, Quot.sound]` (plus any
   **explicitly-declared Decision-2A structural axioms**, which must be named in the README).
2. `lake build` with **zero** `declaration uses 'sorry'`.
3. `grep -rn "sorry\|admit\|native_decide\|sorryAx"` clean over sources.

Capstone is about the **real object**: the circuit-amplitude theorem via the Phase-4
coupling lemma, not the abstract SOP alone.

---

## Immediate next actions (Phase 0 → first milestone)

1. Scaffold the lake project (pin Lean+Mathlib, `cache get`, `.gitignore /.lake/`), stub
   `SopInstance` + `RootedRankDecomp` so they typecheck with `sorry`-free signatures.
2. Land **`lem:signatures-le-2^k`** (Phase 1) — small, high-leverage, exercises the `F₂`
   `Matrix.rank` API we'll lean on everywhere.
3. Attack the **crux** `lem:chi-well-defined` → `thm:dp-correct` in isolation (Phase 2).
   Green + axiom-clean here is the go/no-go signal for the whole effort — if the crux is
   clean, the remaining phases are largely scheduling.

All three scoping decisions are locked (see banner at top). No open decisions block Phase 0.
