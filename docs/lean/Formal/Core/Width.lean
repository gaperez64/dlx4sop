/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.DPCorrect
import Formal.Foundations.CutRank

/-!
# The signature-count bound at a cut (`lem:signatures-le-2^k`)

For a subtree `u` with vertex set `X = u.verts`, the boundary signatures `sig u z` of the
supported assignments `z` all lie in the row space of the `F₂` cut matrix `A[X, X̄]`: the
signature map factors as `z ↦ recover ((toRow z) ᵥ* cutMatrix X)`. Combining with the pure
matrix bound `card_image_vecMul_le` from `Formal.Foundations.CutRank`, a width bound
`cutRankOf X ≤ k` gives at most `2 ^ k` distinct signatures (`card_sig_image_le`) and at most
`r · 2 ^ k` distinct DP states `(σ, φ)` (`card_state_image_le`).
-/

open scoped BigOperators

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-- The `F₂` cut matrix of `X`: rows indexed by `X`, columns by `X̄`, entries `A v w`. -/
def cutMatrix (X : Finset I.V) : Matrix {v // v ∈ X} {w // w ∉ X} (ZMod 2) :=
  fun v w => I.adj v.1 w.1

/-- The cut rank `ρ(X)`: the `F₂` rank of the cut matrix of `X`. -/
noncomputable def cutRankOf (X : Finset I.V) : ℕ := (I.cutMatrix X).rank

/-- Restrict an assignment to a row vector indexed by the cut side `X`. -/
def toRow (X : Finset I.V) (z : I.V → ZMod 2) : {v // v ∈ X} → ZMod 2 :=
  fun v => z v.1

/-- Extend a boundary vector on `X̄` to a full `V`-vector that is zero on `X`. -/
def recover (X : Finset I.V) (s : {w // w ∉ X} → ZMod 2) : I.V → ZMod 2 :=
  fun w => if hw : w ∈ X then 0 else s ⟨w, hw⟩

/-- **Factoring the signature through the cut matrix.** `sig u z` is the row-vector–matrix
product of the restriction of `z` with the cut matrix, re-expanded to a full `V`-vector.
(No support hypothesis is needed: `sig` reads `z` only on `u.verts`.) -/
theorem sig_eq_recover_vecMul (u : RTree I.V) (z : I.V → ZMod 2) :
    I.sig u z
      = I.recover u.verts (Matrix.vecMul (I.toRow u.verts z) (I.cutMatrix u.verts)) := by
  funext w
  by_cases hw : w ∈ u.verts
  · simp [sig, recover, hw]
  · have hrec : I.recover u.verts
          (Matrix.vecMul (I.toRow u.verts z) (I.cutMatrix u.verts)) w
        = Matrix.vecMul (I.toRow u.verts z) (I.cutMatrix u.verts) ⟨w, hw⟩ := dif_neg hw
    rw [I.sig_apply_not_mem u z hw, hrec]
    change (∑ v ∈ u.verts, z v * I.adj v w)
        = ∑ v : {x // x ∈ u.verts}, z v.1 * I.adj v.1 w
    exact (Finset.sum_coe_sort u.verts (fun v => z v * I.adj v w)).symm

/-- **`lem:signatures-le-2^k`.** A width bound `ρ(X_u) ≤ k` bounds the number of distinct
boundary signatures over the supported assignments by `2 ^ k`. -/
theorem card_sig_image_le (u : RTree I.V) {k : ℕ} (hk : I.cutRankOf u.verts ≤ k) :
    ((I.suppFin u).image (I.sig u)).card ≤ 2 ^ k := by
  classical
  have hk' : (I.cutMatrix u.verts).rank ≤ k := hk
  have hsub : (I.suppFin u).image (I.sig u)
      ⊆ (Finset.univ.image
          (fun z : {v // v ∈ u.verts} → ZMod 2 =>
            Matrix.vecMul z (I.cutMatrix u.verts))).image (I.recover u.verts) := by
    intro σ hσ
    obtain ⟨z, -, rfl⟩ := Finset.mem_image.mp hσ
    exact Finset.mem_image.mpr
      ⟨Matrix.vecMul (I.toRow u.verts z) (I.cutMatrix u.verts),
        Finset.mem_image.mpr ⟨I.toRow u.verts z, Finset.mem_univ _, rfl⟩,
        (I.sig_eq_recover_vecMul u z).symm⟩
  refine le_trans (Finset.card_le_card hsub) (le_trans Finset.card_image_le ?_)
  exact card_image_vecMul_le _ _ _ hk'

/-- **State-count bound.** A width bound `ρ(X_u) ≤ k` bounds the number of distinct DP states
`(σ_u(z), φ_u(z))` over the supported assignments by `r · 2 ^ k`. -/
theorem card_state_image_le (u : RTree I.V) {k : ℕ} (hk : I.cutRankOf u.verts ≤ k) :
    ((I.suppFin u).image (fun z => (I.sig u z, I.phi z))).card ≤ I.r * 2 ^ k := by
  classical
  have hsub : (I.suppFin u).image (fun z => (I.sig u z, I.phi z))
      ⊆ ((I.suppFin u).image (I.sig u)) ×ˢ (Finset.univ : Finset (ZMod I.r)) := by
    intro p hp
    obtain ⟨z, hz, rfl⟩ := Finset.mem_image.mp hp
    exact Finset.mem_product.mpr
      ⟨Finset.mem_image.mpr ⟨z, hz, rfl⟩, Finset.mem_univ _⟩
  have h1 := Finset.card_le_card hsub
  rw [Finset.card_product, Finset.card_univ, ZMod.card] at h1
  calc ((I.suppFin u).image (fun z => (I.sig u z, I.phi z))).card
      ≤ ((I.suppFin u).image (I.sig u)).card * I.r := h1
    _ ≤ 2 ^ k * I.r := Nat.mul_le_mul (I.card_sig_image_le u hk) le_rfl
    _ = I.r * 2 ^ k := Nat.mul_comm _ _

end SopInstance
end Formal
