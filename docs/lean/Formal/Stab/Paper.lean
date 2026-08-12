/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Stab.Join
import Formal.Stab.Magic
import Formal.Stab.Hybrid

/-!
# Paper-facing statements for the stabilizer-rank join (`sec:stabjoin`)

This module packages the independently proved closure, join, magic-decomposition and
operation-count lemmas into statements whose assumptions and conclusions match the Lean badges
in `sec:stabjoin`.  It adds no new mathematical assumptions: each theorem below is a short
composition of results in `Formal.Stab.QPF`, `Formal.Stab.Affine`, `Formal.Stab.Closure`,
`Formal.Stab.Join`, `Formal.Stab.Magic` and `Formal.Stab.Hybrid`.  (It mirrors `Formal.Paper`
for the rank-width layer.)

## What is *not* claimed

* The stabilizer rank `sr` is an abstract parameter of `Formal.Stab.Hybrid`.  The quantitative
  bound `sr(τ) = O(2^{0.3963 τ})` used in the paper is cited literature (Bravyi–Smith–Smolin;
  Qassim–Pashayan–Gosset) and is nowhere assumed, axiomatized or proved here.  Only the
  elementary `2^τ` count of `magic_decomp_spec` is machine-checked.
* Description size `O(m²)` and running time `O(m³)` for the QPF operations are not formalized.
  As everywhere in this development, runtime is modelled by operation counts — here the
  `Λ_L · Λ_R` pair scans of `costHybrid` — and never by a machine model.
* `thm:join-mm-barrier` itself is out of scope; `stab_join_spec` is the *positive* statement
  that the barrier's black-box hypothesis fails for the tables the DP actually produces.
-/

open scoped BigOperators Matrix

namespace Formal
namespace Stab

/-! ## `lem:qpf-closure` -/

/-- **`lem:qpf-closure`, formalized clauses.**  Quadratic phase functions are closed under
(i) pointwise products, (iii) summation over a single variable, and (iv) pushforward along an
`F₂`-linear map.  Clause (ii), affine substitution, is used inside (iii) and appears separately
as `Formal.Stab.isQPF_affinePhase` (`eq:parity-lift`) and `isQPF_affineProdSign`. -/
theorem qpf_closure_spec {ι : Type} [Fintype ι] [DecidableEq ι] {κ : Type} [Fintype κ]
    [DecidableEq κ] (P : Matrix κ ι (ZMod 2)) (j : ι)
    {f g : (ι → ZMod 2) → ℂ} (hf : IsQPF f) (hg : IsQPF g) :
    IsQPF (fun x => f x * g x)
      ∧ IsQPF (fun x => ∑ b : ZMod 2, f (Function.update x j b))
      ∧ IsQPF (fun y : κ → ZMod 2 => ∑ x : ι → ZMod 2, if P.mulVec x = y then f x else 0) :=
  ⟨hf.mul hg, hf.sumOne j, hf.pushforward P⟩

/-! ## `lem:stab-join` -/

/-- **`lem:stab-join`, formalized clause.**  On QPF child tables the abstract join of
`thm:join-mm-barrier`, `H(w) = ∑_{Pu+Qv=w} F(u)G(v)(−1)^{B(u,v)}`, produces a *single* QPF.
The barrier quantifies over arbitrary `F` and `G`; `IsQPF F`, `IsQPF G` is exactly the
structure the black-box model discards, so there is no contradiction. -/
theorem stab_join_spec {ιu ιv ιw : Type} [Fintype ιu] [Fintype ιv] [Fintype ιw]
    [DecidableEq ιu] [DecidableEq ιv]
    (P : Matrix ιw ιu (ZMod 2)) (Q : Matrix ιw ιv (ZMod 2)) (Bm : Matrix ιu ιv (ZMod 2))
    {F : (ιu → ZMod 2) → ℂ} {G : (ιv → ZMod 2) → ℂ} (hF : IsQPF F) (hG : IsQPF G) :
    IsQPF (fun w : ιw → ZMod 2 =>
      ∑ u : ιu → ZMod 2, ∑ v : ιv → ZMod 2,
        if P.mulVec u + Q.mulVec v = w then F u * G v * sgn (crossPar Bm u v) else 0) :=
  isQPF_join P Q Bm hF hG

/-! ## `lem:magic-decomp` -/

/-- **`lem:magic-decomp`, formalized clauses.**  The mode-`a` weight is a sum of QPFs indexed by
the subsets of the `a`-magic vertices, and that index set has exactly `2^τ` elements.  This is
the elementary `sr(τ) ≤ 2^τ` half of the paper's statement; the sharper cited bound is not
formalized. -/
theorem magic_decomp_spec (I : SopInstance) (h4 : 4 ∣ I.r) (a : ZMod I.r) :
    (∃ F : Finset I.V → ((I.V → ZMod 2) → ℂ),
        (∀ T, IsQPF (F T)) ∧
        ∀ z, I.chi (a * I.phi z) = ∑ T ∈ (I.magicSet a).powerset, F T z)
      ∧ (I.magicSet a).powerset.card = 2 ^ (I.magicSet a).card :=
  ⟨I.modeWeight_sum_qpf h4 a, I.card_powerset_magicSet a⟩

/-! ## `thm:hybrid-dp`: the two extremal cases -/

/-- **The paper's "Ideal case" (Gottesman–Knill).**  On an instance with no `a`-magic vertex,
the whole mode-`a` weight is a *single* QPF, and the always-`Rebuild` strategy runs
`alg:stabjoin` in `6·|V|` operations — linear in `n` and **independent of the rank-width**,
where the naive join of `thm:fourier-speedup` still pays `4^k` per internal vertex. -/
theorem clifford_collapse_spec (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Step)
    (D : RankDecomp I) (h4 : 4 ∣ I.r) (a : ZMod I.r) (hc : ∀ v, I.ACliff a v)
    (hsr : ∀ u, sr u ≤ 1) (hS : ∀ u, S u = Step.rebuild) :
    IsQPF (fun z : I.V → ZMod 2 => I.chi (a * I.phi z))
      ∧ costHybrid I sr S D.tree ≤ 6 * Fintype.card I.V :=
  ⟨I.isQPF_modeWeight_of_cliff h4 a hc, costHybrid_clifford_le I sr S D hsr hS⟩

/-- **The paper's "Bad case" (the hybrid never loses).**  A strategy forced into point form
keeps at most `2^{ρ_u}` phase functions at every cut and performs exactly the pairwise
signature scan of `thm:fourier-speedup`, so it meets the same `|V| · (2 + 4^k)` operation count
as `SopInstance.costMode_le'`.  (`1 ≤ k` only excludes the degenerate edgeless case where every
cut has rank zero; see the scope note in `Formal.Stab.Hybrid`.) -/
theorem hybrid_never_loses_spec (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Step)
    (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) (hk : 1 ≤ k)
    (hpf : PointForm I S D.tree) :
    (∀ u, RTree.Subtree u D.tree → Lam I sr S u ≤ 2 ^ k)
      ∧ costHybrid I sr S D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) :=
  ⟨fun _ hu => Lam_le_of_pointForm_width I sr S hw hk hpf hu,
    costHybrid_point_le I sr S D hw hk hpf⟩

end Stab
end Formal
