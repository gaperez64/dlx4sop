/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Cost

/-!
# The hybrid stabilizer/rank-width DP (`alg:stabjoin`, `thm:hybrid-dp`)

`alg:stabjoin` runs the per-mode DP of `Formal.Core.Cost` but represents the mode-`a` table at a
subtree `u` as a *list* of quadratic phase functions (`Formal.Stab.QPF`) summing to that table.
The only quantity the runtime depends on is the length `Λ_u` of that list, and at every internal
vertex the algorithm may pick one of three steps:

* `Step.join` — multiply the children's lists termwise, `Λ_u ≤ Λ_L · Λ_R`;
* `Step.rebuild` — throw the children away and re-derive the table from the instance, giving
  `Λ_u ≤ sr(τ_u)` decompositions of the magic part below `u`;
* `Step.point` — a join restricted to *point* tables, where equal supports are merged by
  hashing, so `Λ_u ≤ 2^{ρ_u}` (`ρ_u` the cut rank).

This file formalizes exactly that recurrence and the two extremal claims of `thm:hybrid-dp`:
the hybrid never loses against the plain per-mode join (`costHybrid_point_le`, matching
`SopInstance.costMode_le'`), and it collapses to *linear, width-independent* cost on Clifford
instances (`costHybrid_clifford_le`).

## Scope

* The stabilizer rank is an **abstract parameter** `sr : RTree I.V → ℕ`. The quantitative
  upper bound on `sr(τ)` quoted in the paper is cited literature; it is neither assumed nor
  axiomatized anywhere in this development, and no theorem below constrains `sr` except by an
  explicit hypothesis of that theorem (as in `costHybrid_clifford_le`).
* Only the operation count is modelled, exactly as in `Formal.Core.Cost`: `2` per leaf and
  `Λ_L · Λ_R` per internal vertex. Soundness of the three steps — that the list really does sum
  to the DP table — is the content of `Formal.Stab.QPF`, `Formal.Stab.Closure`,
  `Formal.Stab.Join` and `Formal.Stab.Magic`, and is out of scope here.
* `costHybrid_point_le` carries a hypothesis `1 ≤ k` that `SopInstance.costMode_le'` does not.
  This is not slack in the proof: `alg:stabjoin` initializes every leaf with *two* point tables
  (`Λ_leaf = 2`), whereas `costMode` charges the already-deduplicated leaf signature count
  `statesSig`, which is `1` at an isolated vertex. The two models therefore differ by a constant
  exactly when `k = 0`, i.e. when every cut has rank zero and the SOP variable graph is
  edgeless. We state the hypothesis rather than hide the discrepancy.
-/

namespace Formal
namespace Stab

/-- The three steps available at an internal vertex of `alg:stabjoin`: termwise `join` of the
children's lists, `rebuild` from the instance via a stabilizer decomposition, or a `point`
join with hash merging of equal supports. -/
inductive Step
  | join
  | rebuild
  | point
  deriving DecidableEq

/-- `Λ_u`: the number of quadratic phase functions kept at subtree `u` by `alg:stabjoin` under
the step strategy `S` and the abstract stabilizer-rank function `sr`. A leaf emits the two
point tables `x_v = 0, 1`; an internal vertex pays according to its step. -/
noncomputable def Lam (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Step) :
    RTree I.V → ℕ
  | .leaf _ => 2
  | .node L R =>
    match S (.node L R) with
    | .rebuild => sr (.node L R)
    | .join    => Lam I sr S L * Lam I sr S R
    | .point   => min (Lam I sr S L * Lam I sr S R) (I.statesSig (.node L R))

variable (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Step)

/-- Leaf initialization of `alg:stabjoin`: two point tables, so `Λ_leaf = 2`. -/
@[simp] theorem Lam_leaf (v : I.V) : Lam I sr S (.leaf v) = 2 := rfl

/-- The `Rebuild` annotation of `alg:stabjoin`: `Λ_u ≤ sr(τ_u)`, the stabilizer rank of the
magic part below `u`. -/
theorem Lam_node_rebuild {L R : RTree I.V} (h : S (.node L R) = Step.rebuild) :
    Lam I sr S (.node L R) = sr (.node L R) := by
  rw [Lam, h]

/-- The `Join` annotation of `alg:stabjoin`: a termwise product of the children's lists,
`Λ_u ≤ Λ_L · Λ_R`. -/
theorem Lam_node_join {L R : RTree I.V} (h : S (.node L R) = Step.join) :
    Lam I sr S (.node L R) = Lam I sr S L * Lam I sr S R := by
  rw [Lam, h]

/-- The `Point` annotation of `alg:stabjoin`: a join whose equal supports are merged by hashing,
so the list is additionally capped by the number of occupied signatures at the cut. -/
theorem Lam_node_point {L R : RTree I.V} (h : S (.node L R) = Step.point) :
    Lam I sr S (.node L R)
      = min (Lam I sr S L * Lam I sr S R) (I.statesSig (.node L R)) := by
  rw [Lam, h]

/-- The `2^{ρ_u}` half of the `Point` step: after hash merging at most one entry survives per
occupied boundary signature. -/
theorem Lam_node_point_le {L R : RTree I.V} (h : S (.node L R) = Step.point) :
    Lam I sr S (.node L R) ≤ I.statesSig (.node L R) := by
  rw [Lam_node_point I sr S h]
  exact min_le_right _ _

/-! ## Point form -/

/-- `PointForm S u`: every internal vertex of the subtree `u` takes the `Point` step. This is
the side condition of `alg:stabjoin` under which the point tables are never destroyed, and hence
under which the `2^{ρ_u}` bound of `thm:hybrid-dp` is legitimate. -/
def PointForm (I : SopInstance) (S : RTree I.V → Step) : RTree I.V → Prop
  | .leaf _ => True
  | .node L R => S (.node L R) = Step.point ∧ PointForm I S L ∧ PointForm I S R

@[simp] theorem PointForm_leaf (v : I.V) : PointForm I S (.leaf v) := trivial

/-- Point form at a node is the step at the node together with point form at both children. -/
theorem PointForm_node {L R : RTree I.V} :
    PointForm I S (.node L R)
      ↔ S (.node L R) = Step.point ∧ PointForm I S L ∧ PointForm I S R := Iff.rfl

/-- Point form is hereditary: every subtree of a point-form tree is itself in point form. -/
theorem PointForm_of_subtree {u t : RTree I.V} (hst : RTree.Subtree u t) :
    PointForm I S t → PointForm I S u := by
  induction hst with
  | refl => exact id
  | left _ ih => exact fun h => ih (PointForm_node I S |>.mp h).2.1
  | right _ ih => exact fun h => ih (PointForm_node I S |>.mp h).2.2

/-- **The `2^{ρ_u}` clause of `thm:hybrid-dp`.** In point form the list at `u` is capped by the
number of occupied boundary signatures at the cut of `u`; the `max 2` covers only the leaf case,
where `alg:stabjoin` always emits its two point tables. -/
theorem Lam_le_of_pointForm {u : RTree I.V} (h : PointForm I S u) :
    Lam I sr S u ≤ max 2 (I.statesSig u) := by
  cases u with
  | leaf v => rw [Lam_leaf]; exact le_max_left _ _
  | node L R =>
      exact le_trans (Lam_node_point_le I sr S (PointForm_node I S |>.mp h).1)
        (le_max_right _ _)

/-! ## The operation count of `alg:stabjoin` -/

/-- Operation count of `alg:stabjoin`, in the model of `SopInstance.costMode`: `2` per leaf, and a
pair scan `Λ_L · Λ_R` of the two children's lists per internal vertex. -/
noncomputable def costHybrid (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Step) :
    RTree I.V → ℕ
  | .leaf _ => 2
  | .node L R => costHybrid I sr S L + costHybrid I sr S R + Lam I sr S L * Lam I sr S R

@[simp] theorem costHybrid_leaf (v : I.V) : costHybrid I sr S (.leaf v) = 2 := rfl

@[simp] theorem costHybrid_node (L R : RTree I.V) :
    costHybrid I sr S (.node L R)
      = costHybrid I sr S L + costHybrid I sr S R + Lam I sr S L * Lam I sr S R := rfl

/-- Sharp form of the hybrid runtime bound (`thm:hybrid-dp`): if every subtree keeps a list of
length at most `B`, then `costHybrid ≤ 2·#leaves + #nodes·B²`. This mirrors
`SopInstance.costMode_le_sharp`, with the uniform list bound `B` in place of `2^k`. -/
theorem costHybrid_le_sharp {B : ℕ} :
    ∀ t : RTree I.V, (∀ u, RTree.Subtree u t → Lam I sr S u ≤ B) →
      costHybrid I sr S t ≤ 2 * t.leafCount + t.nodeCount * (B * B) := by
  intro t
  induction t with
  | leaf _ => intro _; simp
  | node L R ihL ihR =>
      intro h
      have hL := ihL (fun u hu => h u (RTree.Subtree.left hu))
      have hR := ihR (fun u hu => h u (RTree.Subtree.right hu))
      have hsL : Lam I sr S L ≤ B := h L (RTree.Subtree.left (RTree.Subtree.refl L))
      have hsR : Lam I sr S R ≤ B := h R (RTree.Subtree.right (RTree.Subtree.refl R))
      calc costHybrid I sr S (RTree.node L R)
          = costHybrid I sr S L + costHybrid I sr S R + Lam I sr S L * Lam I sr S R := rfl
        _ ≤ (2 * L.leafCount + L.nodeCount * (B * B))
              + (2 * R.leafCount + R.nodeCount * (B * B))
              + B * B :=
            Nat.add_le_add (Nat.add_le_add hL hR) (Nat.mul_le_mul hsL hsR)
        _ = 2 * (RTree.node L R).leafCount + (RTree.node L R).nodeCount * (B * B) := by
            rw [RTree.leafCount_node, RTree.nodeCount_node]; ring

/-! ## The bad case: point form is never worse than the plain per-mode join -/

/-- Under a width-`k` decomposition a point-form strategy keeps at most `2^k` phase functions at
*every* subtree: `2^{ρ_u} ≤ 2^k` signatures survive the hash merge, and the two leaf tables also
fit since `1 ≤ k`. -/
theorem Lam_le_of_pointForm_width {D : RankDecomp I} {k : ℕ} (hw : I.WidthBounded D k)
    (hk : 1 ≤ k) (hpf : PointForm I S D.tree) {u : RTree I.V} (hu : RTree.Subtree u D.tree) :
    Lam I sr S u ≤ 2 ^ k := by
  have h2 : (2 : ℕ) ≤ 2 ^ k := by
    calc (2 : ℕ) = 2 ^ 1 := (pow_one 2).symm
      _ ≤ 2 ^ k := Nat.pow_le_pow_right (by norm_num) hk
  have hs : I.statesSig u ≤ 2 ^ k := I.statesSig_le (hw u hu)
  exact le_trans (Lam_le_of_pointForm I sr S (PointForm_of_subtree I S hu hpf)) (max_le h2 hs)

/-- **`thm:hybrid-dp` (the hybrid never loses).** A strategy forced into point form performs
exactly the pairwise signature scan of `thm:fourier-speedup`, so its operation count meets the
same `|V| · (2 + 4^k)` bound as `SopInstance.costMode_le'`. -/
theorem costHybrid_point_le (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) (hk : 1 ≤ k)
    (hpf : PointForm I S D.tree) :
    costHybrid I sr S D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) := by
  have hlc := I.leafCount_le_card D
  have hnc : D.tree.nodeCount ≤ Fintype.card I.V := by
    have := RTree.leafCount_eq D.tree
    omega
  have h4 : (4 : ℕ) ^ k = 2 ^ k * 2 ^ k := by
    rw [← pow_add, ← two_mul, pow_mul]
    norm_num
  calc costHybrid I sr S D.tree
      ≤ 2 * D.tree.leafCount + D.tree.nodeCount * (2 ^ k * 2 ^ k) :=
        costHybrid_le_sharp I sr S D.tree
          (fun _ hu => Lam_le_of_pointForm_width I sr S hw hk hpf hu)
    _ ≤ 2 * Fintype.card I.V + Fintype.card I.V * (2 ^ k * 2 ^ k) :=
        Nat.add_le_add (Nat.mul_le_mul le_rfl hlc) (Nat.mul_le_mul hnc le_rfl)
    _ = Fintype.card I.V * (2 + 2 ^ k * 2 ^ k) := by ring
    _ = Fintype.card I.V * (2 + 4 ^ k) := by rw [h4]

/-! ## The ideal case: the Clifford collapse -/

/-- On an instance with no magic vertex below any cut, `Rebuild` returns a single quadratic
phase function, so the always-rebuild strategy keeps at most two of them everywhere: `2` at a
leaf and `sr ≤ 1` at every internal vertex. -/
theorem Lam_le_of_clifford (hsr : ∀ u, sr u ≤ 1) (hS : ∀ u, S u = Step.rebuild) :
    ∀ t : RTree I.V, Lam I sr S t ≤ 2 := by
  intro t
  induction t with
  | leaf _ => simp
  | node L R _ _ =>
      rw [Lam_node_rebuild I sr S (hS (RTree.node L R))]
      exact le_trans (hsr (RTree.node L R)) (by norm_num)

/-- **`thm:hybrid-dp` (Clifford collapse).** If no cut has a magic vertex below it — `sr u ≤ 1`
— then the always-rebuild strategy runs `alg:stabjoin` in `6·|V|` operations: linear in `n` *and
independent of the width*, where the naive join of `SopInstance.costMode_le'` still pays `4^k`
per internal vertex. -/
theorem costHybrid_clifford_le (D : RankDecomp I) (hsr : ∀ u, sr u ≤ 1)
    (hS : ∀ u, S u = Step.rebuild) :
    costHybrid I sr S D.tree ≤ 6 * Fintype.card I.V := by
  have hlc := I.leafCount_le_card D
  have hnc : D.tree.nodeCount ≤ Fintype.card I.V := by
    have := RTree.leafCount_eq D.tree
    omega
  calc costHybrid I sr S D.tree
      ≤ 2 * D.tree.leafCount + D.tree.nodeCount * (2 * 2) :=
        costHybrid_le_sharp I sr S D.tree
          (fun u _ => Lam_le_of_clifford I sr S hsr hS u)
    _ ≤ 2 * Fintype.card I.V + Fintype.card I.V * (2 * 2) :=
        Nat.add_le_add (Nat.mul_le_mul le_rfl hlc) (Nat.mul_le_mul hnc le_rfl)
    _ = 6 * Fintype.card I.V := by ring

end Stab
end Formal
