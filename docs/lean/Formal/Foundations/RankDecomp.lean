/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.SopInstance

/-!
# Rooted rank-decompositions as binary trees

For the DP we need only a *rooted* decomposition object (playbook: correctness/runtime need
the decomposition-with-width, not the NP-hard rank-width optimum). We model it as a rooted
binary tree `RTree` whose leaves carry the vertices. Each subtree `u` induces the cut
`(X_u, X̄_u)` with `X_u = u.verts`. Well-formedness `WF` records that the two children of every
node have disjoint vertex sets (the true-twin/distinct-leaves condition), which is exactly the
`X_t = X_L ⊎ X_R` disjointness the join step of `thm:dp-correct` relies on.
-/

namespace Formal

/-- A rooted binary rank-decomposition tree: leaves hold vertices. -/
inductive RTree (V : Type) where
  | leaf : V → RTree V
  | node : RTree V → RTree V → RTree V

namespace RTree

variable {V : Type} [DecidableEq V]

/-- `X_u`: the set of vertices at the leaves below `u`. -/
def verts : RTree V → Finset V
  | leaf v => {v}
  | node L R => L.verts ∪ R.verts

/-- Well-formedness: the two children of every node have disjoint vertex sets. -/
def WF : RTree V → Prop
  | leaf _ => True
  | node L R => Disjoint L.verts R.verts ∧ L.WF ∧ R.WF

@[simp] theorem verts_leaf (v : V) : (leaf v).verts = {v} := rfl
@[simp] theorem verts_node (L R : RTree V) : (node L R).verts = L.verts ∪ R.verts := rfl
@[simp] theorem WF_leaf (v : V) : (leaf v).WF := trivial
theorem WF_node {L R : RTree V} :
    (node L R).WF ↔ Disjoint L.verts R.verts ∧ L.WF ∧ R.WF := Iff.rfl

end RTree

/-- A rooted rank-decomposition of a SOP instance: a well-formed tree covering all variables. -/
structure RankDecomp (I : SopInstance) where
  tree : RTree I.V
  wf : tree.WF
  covers : tree.verts = Finset.univ

end Formal
