/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Width

/-!
# The operation-count model and the runtime bounds (`thm:sop-rw`, `thm:fourier-speedup`)

The semantic cost of the DP of `alg:rw-dp`: each leaf initializes its two table entries
(cost `2`), and each join scans all pairs of child states (cost `statesL · statesR`).
`costFull` counts full DP states `(σ, φ)` (the `r · 2^k` regime of `thm:sop-rw`); `costMode`
counts signature-only states (the per-mode `2^k` regime behind `thm:fourier-speedup`).

Combining the state bounds of `Formal.Core.Width` with the leaf count `≤ |V|` of a
well-formed covering tree yields the headline bounds
`costFull ≤ |V| · (2 + (r·2^k)²)` and `costMode ≤ |V| · (2 + 4^k)`.
-/

namespace Formal

namespace RTree

/-- `Subtree u t`: `u` occurs as a (rooted) subtree of `t`. -/
inductive Subtree {V : Type} : RTree V → RTree V → Prop
  | refl (t) : Subtree t t
  | left {u L R} : Subtree u L → Subtree u (node L R)
  | right {u L R} : Subtree u R → Subtree u (node L R)

variable {V : Type}

/-- The number of leaves of a decomposition tree. -/
def leafCount : RTree V → ℕ
  | leaf _ => 1
  | node L R => L.leafCount + R.leafCount

/-- The number of internal nodes of a decomposition tree. -/
def nodeCount : RTree V → ℕ
  | leaf _ => 0
  | node L R => L.nodeCount + R.nodeCount + 1

@[simp] theorem leafCount_leaf (v : V) : (leaf v).leafCount = 1 := rfl

@[simp] theorem leafCount_node (L R : RTree V) :
    (node L R).leafCount = L.leafCount + R.leafCount := rfl

@[simp] theorem nodeCount_leaf (v : V) : (leaf v).nodeCount = 0 := rfl

@[simp] theorem nodeCount_node (L R : RTree V) :
    (node L R).nodeCount = L.nodeCount + R.nodeCount + 1 := rfl

/-- A binary tree has one more leaf than internal nodes. -/
theorem leafCount_eq : ∀ t : RTree V, t.leafCount = t.nodeCount + 1 := by
  intro t
  induction t with
  | leaf _ => rfl
  | node L R ihL ihR => rw [leafCount_node, nodeCount_node, ihL, ihR]; omega

/-- In a well-formed tree the leaves carry pairwise-different vertices, so the leaf count
is at most the number of covered vertices. -/
theorem leafCount_le_card_verts [DecidableEq V] {t : RTree V} (h : t.WF) :
    t.leafCount ≤ t.verts.card := by
  induction t with
  | leaf _ => simp
  | node L R ihL ihR =>
      rw [WF_node] at h
      rw [leafCount_node, verts_node, Finset.card_union_of_disjoint h.1]
      exact Nat.add_le_add (ihL h.2.1) (ihR h.2.2)

end RTree

namespace SopInstance

variable (I : SopInstance)

/-- The number of occupied full DP states `(σ_u(z), φ_u(z))` at a subtree `u`. -/
noncomputable def statesFull (u : RTree I.V) : ℕ :=
  ((I.suppFin u).image (fun z => (I.sig u z, I.phi z))).card

/-- The number of occupied boundary signatures `σ_u(z)` at a subtree `u` (per-mode DP). -/
noncomputable def statesSig (u : RTree I.V) : ℕ :=
  ((I.suppFin u).image (I.sig u)).card

/-- Width-`k` boundedness of a decomposition: every subtree cut has cut rank `≤ k`. -/
def WidthBounded (D : RankDecomp I) (k : ℕ) : Prop :=
  ∀ u, RTree.Subtree u D.tree → I.cutRankOf u.verts ≤ k

/-- A covering well-formed decomposition tree has at most `|V|` leaves. -/
theorem leafCount_le_card (D : RankDecomp I) : D.tree.leafCount ≤ Fintype.card I.V := by
  have h := RTree.leafCount_le_card_verts D.wf
  rwa [D.covers, Finset.card_univ] at h

/-- A cut-rank bound gives at most `r · 2^k` full DP states (`card_state_image_le`). -/
theorem statesFull_le {u : RTree I.V} {k : ℕ} (hk : I.cutRankOf u.verts ≤ k) :
    I.statesFull u ≤ I.r * 2 ^ k :=
  I.card_state_image_le u hk

/-- A cut-rank bound gives at most `2^k` signatures (`card_sig_image_le`). -/
theorem statesSig_le {u : RTree I.V} {k : ℕ} (hk : I.cutRankOf u.verts ≤ k) :
    I.statesSig u ≤ 2 ^ k :=
  I.card_sig_image_le u hk

end SopInstance

/-- Operation count of the full DP (`thm:sop-rw`): `2` per leaf, a full pair scan
`statesFull L · statesFull R` per join. -/
noncomputable def SopInstance.costFull (I : SopInstance) : RTree I.V → ℕ
  | .leaf _ => 2
  | .node L R => I.costFull L + I.costFull R + I.statesFull L * I.statesFull R

/-- Operation count of the per-mode DP (`thm:fourier-speedup`): `2` per leaf, a
signature-only pair scan `statesSig L · statesSig R` per join. -/
noncomputable def SopInstance.costMode (I : SopInstance) : RTree I.V → ℕ
  | .leaf _ => 2
  | .node L R => I.costMode L + I.costMode R + I.statesSig L * I.statesSig R

namespace SopInstance

variable (I : SopInstance)

@[simp] theorem costFull_leaf (v : I.V) : I.costFull (RTree.leaf v) = 2 := rfl

@[simp] theorem costFull_node (L R : RTree I.V) :
    I.costFull (RTree.node L R)
      = I.costFull L + I.costFull R + I.statesFull L * I.statesFull R := rfl

@[simp] theorem costMode_leaf (v : I.V) : I.costMode (RTree.leaf v) = 2 := rfl

@[simp] theorem costMode_node (L R : RTree I.V) :
    I.costMode (RTree.node L R)
      = I.costMode L + I.costMode R + I.statesSig L * I.statesSig R := rfl

/-- Sharp form of the full-DP runtime bound: on a tree all of whose subtrees are
width-bounded, `costFull ≤ 2·#leaves + #nodes·(r·2^k)²`. -/
theorem costFull_le_sharp {k : ℕ} :
    ∀ t : RTree I.V, (∀ u, RTree.Subtree u t → I.cutRankOf u.verts ≤ k) →
      I.costFull t ≤ 2 * t.leafCount + t.nodeCount * ((I.r * 2 ^ k) * (I.r * 2 ^ k)) := by
  intro t
  induction t with
  | leaf _ => intro _; simp
  | node L R ihL ihR =>
      intro h
      have hL := ihL (fun u hu => h u (RTree.Subtree.left hu))
      have hR := ihR (fun u hu => h u (RTree.Subtree.right hu))
      have hsL : I.statesFull L ≤ I.r * 2 ^ k :=
        I.statesFull_le (h L (RTree.Subtree.left (RTree.Subtree.refl L)))
      have hsR : I.statesFull R ≤ I.r * 2 ^ k :=
        I.statesFull_le (h R (RTree.Subtree.right (RTree.Subtree.refl R)))
      calc I.costFull (RTree.node L R)
          = I.costFull L + I.costFull R + I.statesFull L * I.statesFull R := rfl
        _ ≤ (2 * L.leafCount + L.nodeCount * ((I.r * 2 ^ k) * (I.r * 2 ^ k)))
              + (2 * R.leafCount + R.nodeCount * ((I.r * 2 ^ k) * (I.r * 2 ^ k)))
              + (I.r * 2 ^ k) * (I.r * 2 ^ k) :=
            Nat.add_le_add (Nat.add_le_add hL hR) (Nat.mul_le_mul hsL hsR)
        _ = 2 * (RTree.node L R).leafCount
              + (RTree.node L R).nodeCount * ((I.r * 2 ^ k) * (I.r * 2 ^ k)) := by
            rw [RTree.leafCount_node, RTree.nodeCount_node]; ring

/-- Sharp form of the per-mode runtime bound: on a tree all of whose subtrees are
width-bounded, `costMode ≤ 2·#leaves + #nodes·(2^k)²`. -/
theorem costMode_le_sharp {k : ℕ} :
    ∀ t : RTree I.V, (∀ u, RTree.Subtree u t → I.cutRankOf u.verts ≤ k) →
      I.costMode t ≤ 2 * t.leafCount + t.nodeCount * (2 ^ k * 2 ^ k) := by
  intro t
  induction t with
  | leaf _ => intro _; simp
  | node L R ihL ihR =>
      intro h
      have hL := ihL (fun u hu => h u (RTree.Subtree.left hu))
      have hR := ihR (fun u hu => h u (RTree.Subtree.right hu))
      have hsL : I.statesSig L ≤ 2 ^ k :=
        I.statesSig_le (h L (RTree.Subtree.left (RTree.Subtree.refl L)))
      have hsR : I.statesSig R ≤ 2 ^ k :=
        I.statesSig_le (h R (RTree.Subtree.right (RTree.Subtree.refl R)))
      calc I.costMode (RTree.node L R)
          = I.costMode L + I.costMode R + I.statesSig L * I.statesSig R := rfl
        _ ≤ (2 * L.leafCount + L.nodeCount * (2 ^ k * 2 ^ k))
              + (2 * R.leafCount + R.nodeCount * (2 ^ k * 2 ^ k))
              + 2 ^ k * 2 ^ k :=
            Nat.add_le_add (Nat.add_le_add hL hR) (Nat.mul_le_mul hsL hsR)
        _ = 2 * (RTree.node L R).leafCount
              + (RTree.node L R).nodeCount * (2 ^ k * 2 ^ k) := by
            rw [RTree.leafCount_node, RTree.nodeCount_node]; ring

/-- **`thm:sop-rw` (runtime).** A width-`k` decomposition runs the full DP in at most
`|V| · (2 + (r·2^k)²)` operations — time `r² 4^k poly(n)`. -/
theorem costFull_le (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    I.costFull D.tree ≤ Fintype.card I.V * (2 + (I.r * 2 ^ k) * (I.r * 2 ^ k)) := by
  have hlc := I.leafCount_le_card D
  have hnc : D.tree.nodeCount ≤ Fintype.card I.V := by
    have := RTree.leafCount_eq D.tree
    omega
  calc I.costFull D.tree
      ≤ 2 * D.tree.leafCount + D.tree.nodeCount * ((I.r * 2 ^ k) * (I.r * 2 ^ k)) :=
        I.costFull_le_sharp D.tree hw
    _ ≤ 2 * Fintype.card I.V + Fintype.card I.V * ((I.r * 2 ^ k) * (I.r * 2 ^ k)) :=
        Nat.add_le_add (Nat.mul_le_mul le_rfl hlc) (Nat.mul_le_mul hnc le_rfl)
    _ = Fintype.card I.V * (2 + (I.r * 2 ^ k) * (I.r * 2 ^ k)) := by ring

/-- **Per-mode bound (`thm:fourier-speedup`).** A width-`k` decomposition runs one mode of
the per-mode DP in at most `|V| · (2 + (2^k)²)` operations. -/
theorem costMode_le (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    I.costMode D.tree ≤ Fintype.card I.V * (2 + 2 ^ k * 2 ^ k) := by
  have hlc := I.leafCount_le_card D
  have hnc : D.tree.nodeCount ≤ Fintype.card I.V := by
    have := RTree.leafCount_eq D.tree
    omega
  calc I.costMode D.tree
      ≤ 2 * D.tree.leafCount + D.tree.nodeCount * (2 ^ k * 2 ^ k) :=
        I.costMode_le_sharp D.tree hw
    _ ≤ 2 * Fintype.card I.V + Fintype.card I.V * (2 ^ k * 2 ^ k) :=
        Nat.add_le_add (Nat.mul_le_mul le_rfl hlc) (Nat.mul_le_mul hnc le_rfl)
    _ = Fintype.card I.V * (2 + 2 ^ k * 2 ^ k) := by ring

/-- The `4^k` headline form of the per-mode bound: `|V| · (2 + 4^k)` join work per mode. -/
theorem costMode_le' (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    I.costMode D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) := by
  have h4 : (4 : ℕ) ^ k = 2 ^ k * 2 ^ k := by
    rw [← pow_add, ← two_mul, pow_mul]
    norm_num
  rw [h4]
  exact I.costMode_le D hw

end SopInstance
end Formal
