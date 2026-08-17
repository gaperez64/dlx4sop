/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Stab.Join
import Formal.Stab.Magic
import Formal.Stab.Hybrid

/-!
# Paper-facing statements for the stabilizer-rank optimization (`sec:stabjoin`)

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
  charges of `costSwitch` — and never by a machine model.
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

/-! ## `thm:hybrid-dp`: the two endpoints, and the optimizer of `eq:switch-recurrence` -/

/-- **The paper's Clifford collapse (Gottesman–Knill).**  On an instance with no `a`-magic
vertex, the whole mode-`a` weight is a *single* QPF, and switching at the root evaluates the mode
sum in at most `2` operations — on any graph and at any rank-width, where the ordinary join of
`thm:fourier-speedup` still pays `4^k` per internal vertex. -/
theorem clifford_collapse_spec (I : SopInstance) (sr : RTree I.V → ℕ)
    (D : RankDecomp I) (h4 : 4 ∣ I.r) (a : ZMod I.r) (hc : ∀ v, I.ACliff a v)
    (hsr : ∀ u, sr u ≤ 1) :
    IsQPF (fun z : I.V → ZMod 2 => I.chi (a * I.phi z))
      ∧ costSwitch I sr (fun _ => true) D.tree ≤ 2 :=
  ⟨I.isQPF_modeWeight_of_cliff h4 a hc, costSwitch_clifford_le I sr D hsr⟩

/-- **The paper's `B = ∅` endpoint (the optimization never loses).**  With no switch anywhere,
`alg:stabjoin` performs exactly the operations of the per-mode DP, so it meets the same
`|V| · (2 + 4^k)` operation count as `SopInstance.costMode_le'`.  The two cost functions are
equal, not merely comparable, which is why no `1 ≤ k` hypothesis appears. -/
theorem hybrid_never_loses_spec (I : SopInstance) (sr : RTree I.V → ℕ)
    (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    costSwitch I sr (fun _ => false) D.tree = I.costMode D.tree
      ∧ costSwitch I sr (fun _ => false) D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) :=
  ⟨costSwitch_false_eq_costMode I sr D.tree, costSwitch_false_le I sr D hw⟩

/-- **`eq:switch-recurrence`, formalized.**  The postorder recurrence is a lower bound on every
antichain, and `bestS` attains it, so it returns the exact minimum together with a witness.  This
holds for *every* price `sr`: unlike the earlier elementary-price optimizer it assumes nothing
about `sr(τ)`.  The paper's `O(n)` running time is a machine-model statement and is not
formalized. -/
theorem switch_optimum_spec (I : SopInstance) (sr : RTree I.V → ℕ) (t : RTree I.V) :
    (∀ S : RTree I.V → Bool, costOpt I sr t ≤ costSwitch I sr S t)
      ∧ costSwitch I sr (bestS I sr) t = costOpt I sr t :=
  ⟨fun S => costOpt_le I sr S t, costSwitch_bestS I sr t⟩

end Stab
end Formal
