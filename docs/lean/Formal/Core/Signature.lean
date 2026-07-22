/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Foundations.RankDecomp

/-!
# Signatures, residues, and the crossing parity (crux dependency)

For a subtree `u` with vertex set `X_u = u.verts`, an assignment `z : V → ZMod 2` supported on
`X_u` has:
* boundary **signature** `sig u z : V → ZMod 2`, zero on `X_u` and equal on `w ∉ X_u` to the
  parity `∑_{v∈X_u} z v · A v w` of selected neighbours (this is `σ_u(z)` as a full vector);
* internal **residue** `phi z : ZMod r` (`= f z − c`), the unary + quadratic phase inside `X_u`.

The heart of the paper's DP is `lem:chi-well-defined`: the crossing parity
`χ(α,β) = aᵀ A[X_L,X_R] b` between the two children of a node depends only on the signatures
`α = sig L a`, `β = sig R b`, not on the representatives `a, b`. We prove it via the factored
argument: `rawCross a b` depends on `a` only through `sig L a|_{X_R}` and on `b` only through
`sig R b|_{X_L}`, because `X_L` and `X_R` are disjoint (well-formedness of the node).
-/

open scoped BigOperators

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-- The `F₂` adjacency entry `A v w` of the SOP variable graph. -/
def adj (v w : I.V) : ZMod 2 := (I.G.adjMatrix (ZMod 2)) v w

theorem adj_symm (v w : I.V) : I.adj v w = I.adj w v := by
  unfold adj
  rw [SimpleGraph.adjMatrix_apply, SimpleGraph.adjMatrix_apply]
  by_cases h : I.G.Adj v w
  · rw [if_pos h, if_pos ((I.G.adj_comm v w).mp h)]
  · rw [if_neg h, if_neg (fun h' => h ((I.G.adj_comm v w).mpr h'))]

/-- `z` is supported on `X_u`: it vanishes outside the subtree's vertex set. -/
def supported (u : RTree I.V) (z : I.V → ZMod 2) : Prop := ∀ v, v ∉ u.verts → z v = 0

/-- Boundary signature `σ_u(z)` as a full `V`-vector, zero on `X_u`. -/
def sig (u : RTree I.V) (z : I.V → ZMod 2) : I.V → ZMod 2 :=
  fun w => if w ∈ u.verts then 0 else ∑ v ∈ u.verts, z v * I.adj v w

theorem sig_apply_not_mem (u : RTree I.V) (z : I.V → ZMod 2) {w : I.V} (hw : w ∉ u.verts) :
    I.sig u z w = ∑ v ∈ u.verts, z v * I.adj v w := by
  simp [sig, hw]

/-- Internal residue `φ_u(z)` (equals `f z − c`; the constant is re-added at the root). -/
def phi (z : I.V → ZMod 2) : ZMod I.r :=
  (∑ v, I.b v * ((z v).val : ZMod I.r))
    + ((I.r / 2 : ℕ) : ZMod I.r) * (I.selCount z : ZMod I.r)

theorem f_eq_c_add_phi (z : I.V → ZMod 2) : I.f z = I.c + I.phi z := by
  unfold f phi; ring

/-- The raw crossing sum `∑_{v∈X_L, w∈X_R} a v · A v w · b w`. -/
def rawCross (L R : RTree I.V) (a b : I.V → ZMod 2) : ZMod 2 :=
  ∑ v ∈ L.verts, ∑ w ∈ R.verts, a v * I.adj v w * b w

theorem rawCross_eq_sum_w (L R : RTree I.V) (a b : I.V → ZMod 2) :
    I.rawCross L R a b = ∑ w ∈ R.verts, (∑ v ∈ L.verts, a v * I.adj v w) * b w := by
  unfold rawCross
  rw [Finset.sum_comm]
  refine Finset.sum_congr rfl (fun w _ => ?_)
  rw [Finset.sum_mul]

theorem rawCross_eq_sum_v (L R : RTree I.V) (a b : I.V → ZMod 2) :
    I.rawCross L R a b = ∑ v ∈ L.verts, a v * (∑ w ∈ R.verts, I.adj v w * b w) := by
  unfold rawCross
  refine Finset.sum_congr rfl (fun v _ => ?_)
  rw [Finset.mul_sum]
  exact Finset.sum_congr rfl (fun w _ => by ring)

/-- `rawCross` in the left argument depends only on `sig L a` (uses `X_L ∩ X_R = ∅`). -/
theorem rawCross_left (L R : RTree I.V) (hdisj : Disjoint L.verts R.verts)
    {a1 a2 b : I.V → ZMod 2} (hsig : I.sig L a1 = I.sig L a2) :
    I.rawCross L R a1 b = I.rawCross L R a2 b := by
  rw [rawCross_eq_sum_w, rawCross_eq_sum_w]
  refine Finset.sum_congr rfl (fun w hw => ?_)
  have hwL : w ∉ L.verts := Finset.disjoint_right.mp hdisj hw
  have h1 := congrFun hsig w
  rw [I.sig_apply_not_mem L a1 hwL, I.sig_apply_not_mem L a2 hwL] at h1
  rw [h1]

/-- `rawCross` in the right argument depends only on `sig R b` (uses `X_L ∩ X_R = ∅` + symmetry). -/
theorem rawCross_right (L R : RTree I.V) (hdisj : Disjoint L.verts R.verts)
    {a b1 b2 : I.V → ZMod 2} (hsig : I.sig R b1 = I.sig R b2) :
    I.rawCross L R a b1 = I.rawCross L R a b2 := by
  rw [rawCross_eq_sum_v, rawCross_eq_sum_v]
  refine Finset.sum_congr rfl (fun v hv => ?_)
  have hvR : v ∉ R.verts := Finset.disjoint_left.mp hdisj hv
  have h1 := congrFun hsig v
  rw [I.sig_apply_not_mem R b1 hvR, I.sig_apply_not_mem R b2 hvR] at h1
  have step : (∑ w ∈ R.verts, I.adj v w * b1 w) = (∑ w ∈ R.verts, I.adj v w * b2 w) := by
    calc (∑ w ∈ R.verts, I.adj v w * b1 w)
        = (∑ w ∈ R.verts, b1 w * I.adj w v) := by
          refine Finset.sum_congr rfl (fun w _ => ?_); rw [I.adj_symm v w]; ring
      _ = (∑ w ∈ R.verts, b2 w * I.adj w v) := h1
      _ = (∑ w ∈ R.verts, I.adj v w * b2 w) := by
          refine Finset.sum_congr rfl (fun w _ => ?_); rw [I.adj_symm v w]; ring
  rw [step]

open Classical in
/-- A chosen representative assignment realizing a signature, when one exists. -/
noncomputable def rep (u : RTree I.V) (σ : I.V → ZMod 2) : I.V → ZMod 2 :=
  if h : ∃ z, I.supported u z ∧ I.sig u z = σ then h.choose else 0

theorem rep_spec (u : RTree I.V) (σ : I.V → ZMod 2)
    (h : ∃ z, I.supported u z ∧ I.sig u z = σ) :
    I.supported u (I.rep u σ) ∧ I.sig u (I.rep u σ) = σ := by
  rw [rep, dif_pos h]; exact h.choose_spec

/-- The crossing parity `χ(α,β)`, defined via representatives of the two signatures. -/
noncomputable def crossPar (L R : RTree I.V) (α β : I.V → ZMod 2) : ZMod 2 :=
  I.rawCross L R (I.rep L α) (I.rep R β)

/-- **`lem:chi-well-defined`.** For a node with disjoint children `L, R`, and any
representatives `a, b` (supported on `X_L, X_R`), the crossing parity computed from the
signatures equals the raw crossing sum of `a, b`. Hence `χ` is independent of representatives. -/
theorem chi_well_defined (L R : RTree I.V) (hdisj : Disjoint L.verts R.verts)
    {a b : I.V → ZMod 2} (ha : I.supported L a) (hb : I.supported R b) :
    I.crossPar L R (I.sig L a) (I.sig R b) = I.rawCross L R a b := by
  have hsaL := (I.rep_spec L (I.sig L a) ⟨a, ha, rfl⟩).2
  have hsaR := (I.rep_spec R (I.sig R b) ⟨b, hb, rfl⟩).2
  unfold crossPar
  rw [I.rawCross_left L R hdisj hsaL, I.rawCross_right L R hdisj hsaR]

end SopInstance
end Formal
