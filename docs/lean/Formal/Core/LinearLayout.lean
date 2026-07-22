/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Cost

/-!
# Linear layouts as caterpillar decompositions (`thm:linear-layout-fourier`, `cor:lrw`)

A linear layout `v :: l = [v, w₁, …, wₙ]` of the variables induces the left-nested
*caterpillar* tree `node (… (node (leaf v) (leaf w₁)) …) (leaf wₙ)`. Every rooted subtree of
a caterpillar is either a single leaf or a prefix caterpillar (`caterpillar_subtree`), so a
bound `k` on the cut ranks of the prefix cuts (`LayoutWidthLe`) makes the caterpillar a
width-`k` rank decomposition (`layout_widthBounded`): linear rank-width is a special case of
the branching results — no DP is re-proved here.

The runtime then specializes: at each join the right child is a single leaf, which carries at
most `2` occupied signature states (`statesSig_leaf_le`), so one step of the per-mode DP costs
at most `2 + 2 · 2^k` operations instead of the `2^k · 2^k` pair scan of a general join.
Summing along the spine gives the op-count bound
`costMode ≤ (n + 1) · (2 + 2 · 2^k)` (`costMode_layout_le`) — the paper's base-2 claim for
linear layouts (`cor:lrw`) in operation-count form.
-/

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-- The left-nested caterpillar tree of the linear layout `v :: l`: each element of `l` is
joined as a right leaf onto the growing spine. -/
def caterpillar (v : I.V) (l : List I.V) : RTree I.V :=
  l.foldl (fun acc w => RTree.node acc (RTree.leaf w)) (RTree.leaf v)

theorem caterpillar_nil (v : I.V) : I.caterpillar v [] = RTree.leaf v := rfl

/-- The unfolding equation of the caterpillar from the right end of the layout. -/
theorem caterpillar_append (v : I.V) (l : List I.V) (w : I.V) :
    I.caterpillar v (l ++ [w]) = RTree.node (I.caterpillar v l) (RTree.leaf w) := by
  simp only [caterpillar, List.foldl_append, List.foldl_cons, List.foldl_nil]

/-- The caterpillar of `v :: l` covers exactly the listed vertices. -/
theorem caterpillar_verts (v : I.V) (l : List I.V) :
    (I.caterpillar v l).verts = insert v l.toFinset := by
  have key : ∀ (m : List I.V) (acc : RTree I.V),
      (m.foldl (fun t x => RTree.node t (RTree.leaf x)) acc).verts
        = acc.verts ∪ m.toFinset := by
    intro m
    induction m with
    | nil =>
        intro acc
        rw [List.foldl_nil, List.toFinset_nil, Finset.union_empty]
    | cons w m ih =>
        intro acc
        calc ((w :: m).foldl (fun t x => RTree.node t (RTree.leaf x)) acc).verts
            = (RTree.node acc (RTree.leaf w)).verts ∪ m.toFinset :=
              ih (RTree.node acc (RTree.leaf w))
          _ = acc.verts ∪ (w :: m).toFinset := by
              rw [RTree.verts_node, RTree.verts_leaf, List.toFinset_cons, Finset.insert_eq,
                Finset.union_assoc]
  calc (I.caterpillar v l).verts
      = (RTree.leaf v).verts ∪ l.toFinset := key l (RTree.leaf v)
    _ = insert v l.toFinset := by rw [RTree.verts_leaf, Finset.insert_eq]

/-- A duplicate-free layout yields a well-formed caterpillar. -/
theorem caterpillar_WF (v : I.V) (l : List I.V) (hnd : (v :: l).Nodup) :
    (I.caterpillar v l).WF := by
  have key : ∀ (m : List I.V) (acc : RTree I.V), acc.WF → Disjoint acc.verts m.toFinset →
      m.Nodup → (m.foldl (fun t x => RTree.node t (RTree.leaf x)) acc).WF := by
    intro m
    induction m with
    | nil => intro acc hwf _ _; exact hwf
    | cons w m ih =>
        intro acc hwf hdisj hnd'
        rw [List.toFinset_cons, Finset.disjoint_insert_right] at hdisj
        rw [List.nodup_cons] at hnd'
        refine ih (RTree.node acc (RTree.leaf w)) ⟨?_, hwf, RTree.WF_leaf w⟩ ?_ hnd'.2
        · rw [RTree.verts_leaf, Finset.disjoint_singleton_right]
          exact hdisj.1
        · rw [RTree.verts_node, RTree.verts_leaf, Finset.disjoint_union_left,
            Finset.disjoint_singleton_left, List.mem_toFinset]
          exact ⟨hdisj.2, hnd'.1⟩
  rw [List.nodup_cons] at hnd
  refine key l (RTree.leaf v) (RTree.WF_leaf v) ?_ hnd.2
  rw [RTree.verts_leaf, Finset.disjoint_singleton_left, List.mem_toFinset]
  exact hnd.1

/-- The rank decomposition induced by a covering, duplicate-free linear layout `v :: l`. -/
def layoutDecomp (v : I.V) (l : List I.V) (hnd : (v :: l).Nodup)
    (hcov : (I.caterpillar v l).verts = Finset.univ) : RankDecomp I :=
  ⟨I.caterpillar v l, I.caterpillar_WF v l hnd, hcov⟩

/-- **Prefix characterization of caterpillar subtrees.** Every rooted subtree of the
caterpillar of `v :: l` is a single leaf or the caterpillar of a prefix of the layout. -/
theorem caterpillar_subtree (v : I.V) (l : List I.V) {u : RTree I.V}
    (hu : RTree.Subtree u (I.caterpillar v l)) :
    (∃ w, u = RTree.leaf w) ∨ (∃ i ≤ l.length, u = I.caterpillar v (l.take i)) := by
  induction l using List.reverseRecOn with
  | nil =>
      rw [caterpillar_nil] at hu
      cases hu
      exact Or.inl ⟨v, rfl⟩
  | append_singleton l w ih =>
      rw [caterpillar_append] at hu
      cases hu with
      | refl =>
          refine Or.inr ⟨(l ++ [w]).length, le_rfl, ?_⟩
          rw [List.take_length, caterpillar_append]
      | left h =>
          rcases ih h with hleaf | ⟨i, hi, hu'⟩
          · exact Or.inl hleaf
          · refine Or.inr ⟨i, ?_, ?_⟩
            · rw [List.length_append]
              exact le_trans hi (Nat.le_add_right _ _)
            · rwa [List.take_append_of_le_length hi]
      | right h =>
          cases h
          exact Or.inl ⟨w, rfl⟩

/-- Width `≤ k` of a linear layout: every prefix cut `({v} ∪ l[0:i], rest)` has
cut rank at most `k`. -/
def LayoutWidthLe (v : I.V) (l : List I.V) (k : ℕ) : Prop :=
  ∀ i ≤ l.length, I.cutRankOf (insert v (l.take i).toFinset) ≤ k

/-- A width bound for a layout restricts to every prefix of the layout. -/
theorem layoutWidthLe_take (v : I.V) (l : List I.V) {k : ℕ}
    (hk : I.LayoutWidthLe v l k) (i : ℕ) : I.LayoutWidthLe v (l.take i) k := by
  intro j hj
  rw [List.take_take]
  refine hk _ ?_
  rw [List.length_take] at hj
  omega

/-- A singleton cut has cut rank at most `1`: the cut matrix has a single row. -/
theorem cutRankOf_singleton_le (w : I.V) : I.cutRankOf {w} ≤ 1 := by
  have h := Matrix.rank_le_card_height (I.cutMatrix ({w} : Finset I.V))
  rwa [Fintype.card_coe, Finset.card_singleton] at h

/-- **Linear layouts are width-bounded decompositions.** A prefix-cut width bound `k ≥ 1`
for the layout makes the caterpillar a width-`k` decomposition in the branching sense. -/
theorem layout_widthBounded (v : I.V) (l : List I.V) (hnd : (v :: l).Nodup)
    (hcov : (I.caterpillar v l).verts = Finset.univ) {k : ℕ} (hk1 : 1 ≤ k)
    (hk : I.LayoutWidthLe v l k) :
    I.WidthBounded (I.layoutDecomp v l hnd hcov) k := by
  intro u hu
  have hu' : RTree.Subtree u (I.caterpillar v l) := hu
  rcases I.caterpillar_subtree v l hu' with ⟨w, rfl⟩ | ⟨i, hi, rfl⟩
  · rw [RTree.verts_leaf]
    exact le_trans (I.cutRankOf_singleton_le w) hk1
  · rw [caterpillar_verts]
    exact hk i hi

/-- A leaf occupies at most `2` signature states: only `0` and `Pi.single w 1` are
supported on it (`supported_leaf_iff`). -/
theorem statesSig_leaf_le (w : I.V) : I.statesSig (RTree.leaf w) ≤ 2 := by
  have hsub : I.suppFin (RTree.leaf w) ⊆ {0, Pi.single w 1} := by
    intro z hz
    rcases (I.supported_leaf_iff w z).mp ((mem_suppFin I).mp hz) with h | h
    · rw [h]; exact Finset.mem_insert_self 0 _
    · rw [h]; exact Finset.mem_insert_of_mem (Finset.mem_singleton_self _)
  calc I.statesSig (RTree.leaf w)
      ≤ (I.suppFin (RTree.leaf w)).card := Finset.card_image_le
    _ ≤ ({0, Pi.single w 1} : Finset (I.V → ZMod 2)).card := Finset.card_le_card hsub
    _ ≤ ({Pi.single w 1} : Finset (I.V → ZMod 2)).card + 1 := Finset.card_insert_le _ _
    _ = 2 := by rw [Finset.card_singleton]

set_option linter.unusedVariables false in
/-- **`cor:lrw` (op-count form).** A width-`k` linear layout runs one mode of the per-mode
DP in at most `(n + 1) · (2 + 2 · 2^k)` operations, `n + 1` the number of variables in the
layout: each join scans a prefix (`≤ 2^k` signatures) against a single leaf (`≤ 2`
signatures), so per-step work is base-2 in `k` — versus the `4^k` pair scan of a general
branching join.

(`hnd`, `hcov`, `hk1` are not needed by the cost bound itself — they are kept so the
statement matches `layout_widthBounded`, hence the local linter option.) -/
theorem costMode_layout_le (v : I.V) (l : List I.V) (hnd : (v :: l).Nodup)
    (hcov : (I.caterpillar v l).verts = Finset.univ) {k : ℕ} (hk1 : 1 ≤ k)
    (hk : I.LayoutWidthLe v l k) :
    I.costMode (I.caterpillar v l) ≤ (l.length + 1) * (2 + 2 * 2 ^ k) := by
  clear hnd hcov
  induction l using List.reverseRecOn with
  | nil =>
      rw [caterpillar_nil, costMode_leaf]
      simp only [List.length_nil, zero_add, one_mul]
      exact Nat.le_add_right 2 (2 * 2 ^ k)
  | append_singleton l w ih =>
      have hk' : I.LayoutWidthLe v l k := by
        have h := I.layoutWidthLe_take v (l ++ [w]) hk l.length
        rwa [List.take_append_of_le_length (le_refl l.length), List.take_length] at h
      have hC : I.statesSig (I.caterpillar v l) ≤ 2 ^ k := by
        refine I.statesSig_le ?_
        rw [caterpillar_verts]
        have h := hk' l.length le_rfl
        rwa [List.take_length] at h
      have hprod : I.statesSig (I.caterpillar v l) * I.statesSig (RTree.leaf w)
          ≤ 2 ^ k * 2 :=
        Nat.mul_le_mul hC (I.statesSig_leaf_le w)
      have hlen : (l ++ [w]).length = l.length + 1 := by simp
      rw [caterpillar_append, costMode_node, costMode_leaf, hlen]
      calc I.costMode (I.caterpillar v l) + 2
            + I.statesSig (I.caterpillar v l) * I.statesSig (RTree.leaf w)
          ≤ (l.length + 1) * (2 + 2 * 2 ^ k) + 2 + 2 ^ k * 2 :=
            Nat.add_le_add (Nat.add_le_add (ih hk') le_rfl) hprod
        _ = (l.length + 1 + 1) * (2 + 2 * 2 ^ k) := by ring

end SopInstance
end Formal
