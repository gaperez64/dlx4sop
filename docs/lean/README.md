# A machine-checked rank-width DP for quantum-circuit simulation

Lean 4 + Mathlib formalization of the core results of *"Quadratic Sums-of-Powers for
Fixed-Parameter Tractable Quantum-Circuit Simulation"* (de Colnet, Geerts, Hai, Laarman,
Lee, Pérez): the correctness and (operation-count) runtime of the rank-decomposition dynamic
program for quadratic sums-of-powers, its Fourier-mode and linear-layout variants, the
coupling to the real circuit amplitude for `{H, T, CZ}` (= Clifford+T, since `S = T²`), and
the stabilizer-rank join of `sec:stabjoin` — quadratic phase functions, their closure
properties, the join that keeps the DP tables inside that class, and the magic decomposition.

This directory lives inside the [`dlx4sop`](../..) tool repository, alongside the C
implementation of the algorithms it formalizes, and is linked directly from the paper.

## What is proved

**Abstract SOP layer** (over any quadratic SOP instance `I` and rank-decomposition `D`):

| Paper statement | Lean theorem | File |
|---|---|---|
| `lem:chi-well-defined` (crossing parity well-defined) | `SopInstance.chi_well_defined` | `Formal/Core/Signature.lean` |
| `thm:dp-correct` (DP table = counts, returns `N_j`) | `SopInstance.Dalg_eq_Dspec`, `SopInstance.dp_returns_counts` | `Formal/Core/DPCorrect.lean` |
| ≤ 2^k signatures per cut (row-space bound) | `card_image_vecMul`, `SopInstance.card_sig_image_le` | `Formal/Foundations/CutRank.lean`, `Formal/Core/Width.lean` |
| `thm:sop-rw` (correctness, table size, op-count) | `SopInstance.rank_dp_spec` | `Formal/Paper.lean` |
| `thm:fourier-speedup` per-mode (value and `≤ n·(2+4^k)`) | `SopInstance.fourier_mode_spec` | `Formal/Paper.lean` |
| DFT inversion (eq:inverse-dft) | `SopInstance.N_inversion`, `SopInstance.sum_chi` | `Formal/Core/Fourier.lean` |
| `cor:single-amplitude` (mode 1 computes `S(f)`) | `SopInstance.single_amplitude`, `SopInstance.Aalg_root` | `Formal/Core/Fourier.lean` |
| `thm:linear-layout-fourier` (mode value and base 2, including `k=0`) | `SopInstance.linear_fourier_mode_spec` | `Formal/Paper.lean` |

**Quantum layer** (the honest capstone, about the real amplitude):

| Paper statement | Lean theorem | File |
|---|---|---|
| path-sum compilation (sec:sop) | `Quantum.compile_invariant` | `Formal/Quantum/Coupling.lean` |
| pinning the boundary (sec:sop) | `Quantum.Sym.pathSum_pinned` | `Formal/Quantum/PinningLemmas.lean` |
| compiler counter equals the paper's Hadamard count | `Quantum.compile_mH_eq_hadamardCount` | `Formal/Quantum/Coupling.lean` |
| coupling `⟨z|C|y⟩ = R_C⁻¹·δ_C·S(f_C)` (sec:rw-fpt) | `Quantum.amplitude_eq_sop_normalized` | `Formal/Paper.lean` |
| rank-width FPT simulation (sec:rw-fpt) | `Quantum.amplitude_by_rank_dp_normalized` | `Formal/Paper.lean` |

`δ_C` is the Kronecker delta of Hadamard-free wires. The paper and the paper-facing Lean
theorems both display it explicitly.

**Stabilizer-rank join layer** (`sec:stabjoin`), in `Formal/Stab/`. The paper keeps the
quadratic-phase-function development, `cor:gottesman-knill` and `alg:stabjoin` in the body,
and puts the cost model (`thm:hybrid-dp`), the strategy semantics, the optimizer and
`cor:stabjoin` in its appendix; the Lean layout does not follow that split.

| Paper statement | Lean theorem | File |
|---|---|---|
| `def:qpf` (quadratic phase function) | `Stab.QPF`, `Stab.IsQPF` | `Formal/Stab/QPF.lean` |
| `eq:xor-lift` (`x ⊕ y = x + y − 2xy` on the lifts) | `Stab.lift4_add`, `Stab.zeta4_two_mul` | `Formal/Stab/QPF.lean` |
| `lem:qpf-closure`(i) (pointwise products) | `Stab.QPF.eval_mul`, `Stab.IsQPF.mul` | `Formal/Stab/QPF.lean` |
| `eq:parity-lift` / affine substitution (clause (ii)) | `Stab.isQPF_affinePhase`, `isQPF_affineProdSign`, `isQPF_affineIndicator` | `Formal/Stab/Affine.lean` |
| `lem:qpf-closure`(iii) (sum over one variable) | `Stab.IsQPF.sumOne`, `IsQPF.sumOver` | `Formal/Stab/Closure.lean` |
| `lem:qpf-closure`(iv) (pushforward along a linear map) | `Stab.IsQPF.pushforward`, `IsQPF.sumLeft` | `Formal/Stab/Closure.lean` |
| `lem:qpf-closure` (packaged clauses) | `Stab.qpf_closure_spec` | `Formal/Stab/Paper.lean` |
| `lem:stab-join` (the join stays a single QPF) | `Stab.isQPF_join`, `isQPF_join_point`, `Stab.stab_join_spec` | `Formal/Stab/Join.lean`, `Paper.lean` |
| `a`-magic / `a`-Clifford vertices | `SopInstance.ACliff`, `AMagic` | `Formal/Stab/Magic.lean` |
| `η = r/2` ⟹ the quadratic part is a sign | `SopInstance.chi_half`, `selCount_cast` | `Formal/Stab/Magic.lean` |
| `lem:magic-decomp` (sum of `2^τ` QPFs) | `SopInstance.modeWeight_sum_qpf`, `card_powerset_magicSet`, `Stab.magic_decomp_spec` | `Formal/Stab/Magic.lean`, `Paper.lean` |
| `cor:gottesman-knill` (Gottesman–Knill through the SOP lens) | `SopInstance.isQPF_modeWeight_of_cliff`, `isQPF_modeWeight_of_even_mode` | `Formal/Stab/Magic.lean` |
| `prop:even-modes` (every even mode factorizes, at every even modulus) | `SopInstance.Nhat_even`, `mul_half_eq_zero_of_even` | `Formal/Stab/Magic.lean` |
| `alg:stabjoin` (`Λ_u` recurrence, three moves) | `Stab.Lam`, `Stab.Move`, `Stab.PointForm`, `Stab.costHybrid` | `Formal/Stab/Hybrid.lean` |
| `thm:hybrid-dp` (operation count) | `Stab.costHybrid_le_sharp`, `Lam_le_of_pointForm` | `Formal/Stab/Hybrid.lean` |
| `cor:gottesman-knill`, cost half — `6·|V|` ops, independent of the width | `Stab.costHybrid_clifford_le`, `Stab.clifford_collapse_spec` | `Formal/Stab/Hybrid.lean`, `Paper.lean` |
| *Bad case* — the hybrid never loses (`|V|·(2+4^k)`) | `Stab.costHybrid_point_le`, `Stab.hybrid_never_loses_spec` | `Formal/Stab/Hybrid.lean`, `Paper.lean` |

## Scope, stated honestly

* **Runtime** is formalized as operation counts for table states created and join pairs
  scanned. These are exactly the quantities the paper's runtime proofs bound. No machine model.
  The `O(r log r)` FFT refinement for the inverse transform is not formalized (the paper
  itself notes direct `O(r²)` inversion suffices for fixed `r`; `N_inversion` is the direct
  inversion). The depth-first working-storage schedule is likewise a paper-level analysis,
  not part of the Lean operation-count model. Lean does prove the `r·2^k` size of each table.
* The decomposition is **algorithmic input** (as in the paper); finding rank-decompositions
  (Oum–Seymour et al.) is out of scope. Rank-width as a *minimum* over decompositions is not
  needed for these statements and is not defined.
* Out of scope (deferred): the common-root, parity-table, and parity-averaging lemmas and
  gadget universality with its discrete-second-derivative remark. The structural comparisons
  (`lem:sop-minor-line`, graph realization, the `Γ_{h,t}` separation, WMC transfer) and the
  matrix-multiplication barrier (`thm:join-mm-barrier`) are also out of scope. For the
  stabilizer-rank join, `thm:join-mm-barrier` is the *negative* statement about arbitrary child
  tables; `isQPF_join` is the positive statement that its black-box hypothesis fails on the
  tables the DP actually produces. Only the latter is formalized.

For the stabilizer-rank join specifically:

* **The stabilizer rank `sr` is an abstract parameter** of `Formal/Stab/Hybrid.lean`. Only the
  elementary `sr(τ) ≤ 2^τ` is proved (`modeWeight_sum_qpf` + `card_powerset_magicSet`), and it
  follows from the invertibility of `[[1,1],[1,i]]`. The sharper `sr(τ) = O(2^{0.3963 τ})`
  quoted in the paper is cited literature (Bravyi–Smith–Smolin; Qassim–Pashayan–Gosset) and is
  **nowhere assumed, axiomatized, or claimed** — no theorem constrains `sr` except through an
  explicit hypothesis of that theorem.
* The `Move` type is `join | rebuild | point`. The paper's **`Materialize`** move and the
  strategy optimizer that chooses where to spend each price are **not** formalized, and
  neither is `cor:stabjoin`, the magic-focused-width guarantee, which needs the materialize
  cost. Those live in the paper's appendix.
* The `O(m²)` description size and `O(m³)` time of the QPF operations are **not** formalized.
  As elsewhere, runtime is modelled by operation counts — here the `Λ_L · Λ_R` pair scans of
  `costHybrid` — and never by a machine model. In particular `QPF.mul` stacks constraint
  systems without re-reducing them, so the Lean `QPF` data is not size-bounded; the paper's
  `O(m²)` claim presumes Gaussian re-reduction after each operation.
* "Power of `i`" presupposes `4 ∣ r`, which is an explicit hypothesis (`h4`) everywhere it is
  needed rather than a field of `SopInstance`. Note this affects only the *QPF* route: at
  `r = 16` with `a = 2`, `b_v = 1` we get `ω₁₆² = e^{2πi/8}`, not a power of `i`, so
  `isQPF_modeWeight_of_even_mode` is stated at `r = 8` and the general-`r` forms ask
  `(r/4) ∣ a.val` resp. `(r/4) ∣ (b v).val` (`ACliff_of_dvd_mode`, `ACliff_of_dvd_b`).
  The paper's "every even Fourier mode is polynomial on any graph" is nevertheless true at
  **every** even modulus, for a simpler reason that needs no QPFs: since `η = r/2`, an even
  mode has `a·η = 0`, so every cross term dies and the mode sum factorizes,
  `N̂(a) = χ(a·c) · ∏_v (1 + χ(a·b_v))`. That is `SopInstance.Nhat_even`, proved for arbitrary
  even `r` with no `4 ∣ r` hypothesis.
* `costHybrid_point_le` carries `1 ≤ k`, which `costMode_le'` does not: `alg:stabjoin`
  initializes each leaf with two point tables (`Λ_leaf = 2`) while `costMode` charges the
  deduplicated leaf signature count. The two models differ only when `k = 0`, i.e. when the
  SOP variable graph is edgeless.
* `magicSet` is taken over the whole vertex set rather than per subtree, so `τ_u` as a
  *per-cut* quantity is modelled in `Hybrid.lean` by the abstract `sr : RTree I.V → ℕ` rather
  than being computed from `Magic.lean`. Wiring the two together is not formalized.

## Renaming a declaration breaks the paper

The paper's `[Lean✓]` badges name declarations, not line numbers:

```latex
\lean{Formal/Paper.lean}{rank_dp_spec}
```

`scripts/lean_anchors.py` in the paper repository scans this tree **at `origin/main`** and
writes the `path:name → line` map the badges resolve through. Two consequences for anyone
editing here:

* **Renaming or removing a badged declaration breaks the paper build** — loudly, with a
  LaTeX warning and a visible red `[Lean?]` in the PDF, which is the point. Moving a
  declaration within its file is free; the map is regenerated.
* **Work only resolves once it is merged to `main`.** Badges point at `blob/main`, so a
  declaration living on a branch is a dead link for every reader.

The paper-facing wrappers in `Formal/Paper.lean` and `Formal/Stab/Paper.lean` are the
intended badge targets, since their docstrings name the paper statement they package.

## Axiom profile — how to check

Every theorem above depends on exactly Mathlib's three standard axioms
`[propext, Classical.choice, Quot.sound]` — no `sorry`, no custom axioms. Re-check:

```
lake exe cache get      # fetch Mathlib oleans (once)
lake build              # zero errors, zero `declaration uses 'sorry'`
```

then in any file:

```lean
import Formal
#print axioms Formal.Quantum.amplitude_eq_sop_normalized
#print axioms Formal.SopInstance.rank_dp_spec
#print axioms Formal.Stab.qpf_closure_spec
#print axioms Formal.Stab.stab_join_spec
#print axioms Formal.Stab.magic_decomp_spec
#print axioms Formal.Stab.clifford_collapse_spec
#print axioms Formal.Stab.hybrid_never_loses_spec
-- expected: [propext, Classical.choice, Quot.sound]
```

and `grep -rn "sorry\|admit\|native_decide" Formal/` must return nothing.

## License

This directory (`docs/lean/`) is released under the Apache License, Version 2.0 — see
[LICENSE](LICENSE); each source file's header points here. This differs from the MIT
license covering the rest of the `dlx4sop` repository (top-level [LICENSE](../../LICENSE)).

## Build

Lean `4.32.0`, Mathlib `v4.32.0` (pinned in `lean-toolchain` / `lakefile.toml`).

```
lake exe cache get && lake build
```
