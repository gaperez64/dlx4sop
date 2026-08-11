/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Stab.Closure

/-!
# The stabilizer-rank join (`lem:stab-join`)

`thm:join-mm-barrier` shows that no *black-box* branching join reaches base `2^k` without a
matrix-multiplication breakthrough.  That barrier quantifies over **arbitrary** child tables,
in the abstract shape

  `H(w) = ∑_{Pu + Qv = w} F(u) · G(v) · (−1)^{B(u,v)}`.

The tables the dynamic program actually produces are not arbitrary: each entry is a sum of a
quadratic root-of-unity weight over an affine `F₂`-subspace, i.e. a quadratic phase function
(`Formal.Stab.QPF`).  This file proves that on such tables the join stays inside the class:
`isQPF_join` says the joined table is again a *single* QPF.  Nothing here contradicts
`thm:join-mm-barrier`, which is a statement about arbitrary `F` and `G`; the hypothesis
`IsQPF F`, `IsQPF G` is exactly the structure the black-box model discards.

The whole proof is one application of `lem:qpf-closure`: the summand is a product of QPFs on
the direct sum `ιu ⊕ ιv` of the two child signature spaces (`lem:qpf-closure`(i)), and the join
is its pushforward along the linear map `(u,v) ↦ Pu + Qv` (`lem:qpf-closure`(iv)).

The point-table special case (`isQPF_join_point`) is the naive join of `thm:fourier-speedup`.
-/

open scoped BigOperators Matrix

namespace Formal
namespace Stab

variable {ιu ιv ιw : Type} [Fintype ιu] [Fintype ιv]

/-- Reading a QPF on `F₂^κ` as one on `F₂^{ι ⊕ κ}` that ignores the left block.  The mirror of
`IsQPF.comp_inl`, obtained from it by swapping the two summands. -/
theorem IsQPF.comp_inr {ι κ : Type} [Fintype ι] [Fintype κ] {f : (κ → ZMod 2) → ℂ}
    (hf : IsQPF f) : IsQPF (fun p : ι ⊕ κ → ZMod 2 => f (fun j => p (Sum.inr j))) :=
  ((hf.comp_inl (κ := ι)).reindex (Equiv.sumComm κ ι)).congr fun _ => rfl

/-- The bilinear crossing parity `B(u,v) = uᵀ B v` of `thm:join-mm-barrier`.  In the dynamic
program this is `χ(α,β)`, bilinear once representatives are chosen linearly in the signatures
(`lem:chi-well-defined`, `Formal.SopInstance.chi_well_defined`). -/
def crossPar (Bm : Matrix ιu ιv (ZMod 2)) (u : ιu → ZMod 2) (v : ιv → ZMod 2) : ZMod 2 :=
  ∑ i, ∑ j, u i * Bm i j * v j

/-- The block matrix `[P | Q]` implementing `(u,v) ↦ Pu + Qv` on the direct sum of the two
child signature spaces. -/
def joinMap (P : Matrix ιw ιu (ZMod 2)) (Q : Matrix ιw ιv (ZMod 2)) :
    Matrix ιw (ιu ⊕ ιv) (ZMod 2) :=
  Matrix.of fun k => Sum.elim (P k) (Q k)

theorem joinMap_mulVec (P : Matrix ιw ιu (ZMod 2)) (Q : Matrix ιw ιv (ZMod 2))
    (p : ιu ⊕ ιv → ZMod 2) :
    (joinMap P Q).mulVec p
      = P.mulVec (fun i => p (Sum.inl i)) + Q.mulVec (fun j => p (Sum.inr j)) := by
  funext k
  simp only [Matrix.mulVec, dotProduct, joinMap, Matrix.of_apply, Pi.add_apply]
  rw [Fintype.sum_sum_type]
  rfl

/-- The crossing parity as a quadratic form on the direct sum: the off-diagonal block `B`. -/
def crossQuad (Bm : Matrix ιu ιv (ZMod 2)) : Matrix (ιu ⊕ ιv) (ιu ⊕ ιv) (ZMod 2) :=
  Matrix.of fun a b =>
    match a, b with
    | Sum.inl i, Sum.inr j => Bm i j
    | _, _ => 0

theorem crossQuad_apply (Bm : Matrix ιu ιv (ZMod 2)) (p : ιu ⊕ ιv → ZMod 2) :
    (∑ a, ∑ b, crossQuad Bm a b * p a * p b)
      = crossPar Bm (fun i => p (Sum.inl i)) (fun j => p (Sum.inr j)) := by
  rw [Fintype.sum_sum_type]
  have hr : ∀ j : ιv, (∑ b, crossQuad Bm (Sum.inr j) b * p (Sum.inr j) * p b) = 0 := by
    intro j
    rw [Fintype.sum_sum_type]
    simp [crossQuad]
  have hl : ∀ i : ιu, (∑ b, crossQuad Bm (Sum.inl i) b * p (Sum.inl i) * p b)
      = ∑ j, p (Sum.inl i) * Bm i j * p (Sum.inr j) := by
    intro i
    rw [Fintype.sum_sum_type]
    simp only [crossQuad, Matrix.of_apply, zero_mul, Finset.sum_const_zero, zero_add]
    exact Finset.sum_congr rfl fun j _ => by ring
  rw [Finset.sum_congr rfl fun j _ => hr j, Finset.sum_const_zero, add_zero,
    Finset.sum_congr rfl fun i _ => hl i]
  rfl

/-- The sign of the crossing parity is a QPF on the direct sum of the child signature spaces. -/
theorem isQPF_crossSign (Bm : Matrix ιu ιv (ZMod 2)) :
    IsQPF (fun p : ιu ⊕ ιv → ZMod 2 =>
      sgn (crossPar Bm (fun i => p (Sum.inl i)) (fun j => p (Sum.inr j)))) := by
  refine (isQPF_phase 1 0 (crossQuad Bm)).congr fun p => ?_
  rw [crossQuad_apply]
  simp

/-- **`lem:stab-join`.**  If the two child tables are QPFs, the joined table of
`thm:join-mm-barrier`'s abstract shape `H(w) = ∑_{Pu+Qv=w} F(u)G(v)(−1)^{B(u,v)}` is a single
QPF.  The barrier says this join is hard when `F` and `G` are arbitrary; they are not. -/
theorem isQPF_join [Fintype ιw] [DecidableEq ιu] [DecidableEq ιv]
    (P : Matrix ιw ιu (ZMod 2)) (Q : Matrix ιw ιv (ZMod 2))
    (Bm : Matrix ιu ιv (ZMod 2))
    {F : (ιu → ZMod 2) → ℂ} {G : (ιv → ZMod 2) → ℂ} (hF : IsQPF F) (hG : IsQPF G) :
    IsQPF (fun w : ιw → ZMod 2 =>
      ∑ u : ιu → ZMod 2, ∑ v : ιv → ZMod 2,
        if P.mulVec u + Q.mulVec v = w then F u * G v * sgn (crossPar Bm u v) else 0) := by
  classical
  -- The summand, as a function on the direct sum of the two child signature spaces, is a QPF.
  have hsummand : IsQPF (fun p : ιu ⊕ ιv → ZMod 2 =>
      F (fun i => p (Sum.inl i)) * G (fun j => p (Sum.inr j))
        * sgn (crossPar Bm (fun i => p (Sum.inl i)) (fun j => p (Sum.inr j)))) :=
    ((hF.comp_inl).mul (hG.comp_inr)).mul (isQPF_crossSign Bm)
  -- The join is its pushforward along `(u,v) ↦ Pu + Qv`.
  have hpush := hsummand.pushforward (joinMap P Q)
  refine hpush.congr fun w => ?_
  rw [← Fintype.sum_prod_type']
  refine Fintype.sum_equiv (Equiv.sumArrowEquivProdArrow ιu ιv (ZMod 2)) _ _ fun p => ?_
  rw [joinMap_mulVec]
  rfl

/-- A **point table** is the indicator of a single signature.  Point tables are QPFs, and the
join of two of them is again one: this special case is exactly the naive join of
`thm:fourier-speedup`. -/
theorem isQPF_join_point [Fintype ιw] [DecidableEq ιu] [DecidableEq ιv]
    (P : Matrix ιw ιu (ZMod 2)) (Q : Matrix ιw ιv (ZMod 2))
    (Bm : Matrix ιu ιv (ZMod 2)) (σ : ιu → ZMod 2) (τ : ιv → ZMod 2) :
    IsQPF (fun w : ιw → ZMod 2 =>
      ∑ u : ιu → ZMod 2, ∑ v : ιv → ZMod 2,
        if P.mulVec u + Q.mulVec v = w then
          (if u = σ then (1 : ℂ) else 0) * (if v = τ then (1 : ℂ) else 0)
            * sgn (crossPar Bm u v)
        else 0) := by
  classical
  exact isQPF_join P Q Bm (isQPF_point σ) (isQPF_point τ)

end Stab
end Formal
