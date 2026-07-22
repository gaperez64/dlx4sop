/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Signature

/-!
# The rank-decomposition dynamic program and its correctness (crux)

`Dalg` is the algorithm (alg:rw-dp): a table defined by structural recursion on the
decomposition tree. `Dspec` is the specification it must meet. `thm:dp-correct` is
`Dalg = Dspec`, proved by induction, and `dp_returns_counts` is its "returns `N_j`" corollary.

This file holds only the definitions (`crossTerm`, `combineSig`, `Dspec`, `Dalg`). Correctness
(`thm:dp-correct`) is proved in `Formal.Core.DPCorrect`.
-/

open scoped BigOperators

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-- The residue contribution `η·χ(α,β)` of a join, as a `ZMod r` element. -/
noncomputable def crossTerm (L R : RTree I.V) (α β : I.V → ZMod 2) : ZMod I.r :=
  ((I.r / 2 : ℕ) : ZMod I.r) * ((I.crossPar L R α β).val : ZMod I.r)

/-- Parent signature `γ = α|_Y + β|_Y` (`Y = X̄_t`), as a full vector zero on `X_t`. -/
def combineSig (L R : RTree I.V) (α β : I.V → ZMod 2) : I.V → ZMod 2 :=
  fun w => if w ∈ (RTree.node L R).verts then 0 else α w + β w

open Classical in
/-- Specification: the RHS of `thm:dp-correct`, the count the DP must equal. -/
noncomputable def Dspec (u : RTree I.V) (σ : I.V → ZMod 2) (s : ZMod I.r) : ℕ :=
  (Finset.univ.filter (fun z : I.V → ZMod 2 =>
    I.supported u z ∧ I.sig u z = σ ∧ I.phi z = s)).card

end SopInstance

/-- The rank-decomposition DP table (alg:rw-dp), by structural recursion on the tree. -/
noncomputable def SopInstance.Dalg (I : SopInstance) :
    RTree I.V → (I.V → ZMod 2) → ZMod I.r → ℕ
  | .leaf v, σ, s =>
      (if σ = I.sig (RTree.leaf v) 0 ∧ s = I.phi 0 then 1 else 0)
    + (if σ = I.sig (RTree.leaf v) (Pi.single v 1) ∧ s = I.phi (Pi.single v 1) then 1 else 0)
  | .node L R, γ, s =>
      ∑ α : I.V → ZMod 2, ∑ p : ZMod I.r, ∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
        if I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s
        then I.Dalg L α p * I.Dalg R β q else 0

end Formal
