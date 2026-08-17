/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Cost

/-!
# The stabilizer-rank optimization (`alg:stabjoin`, `thm:hybrid-dp`)

`alg:stabjoin` is the per-mode DP of `Formal.Core.Cost` with a *one-way switch*. A strategy is an
antichain `B` of decomposition vertices. At a vertex of `B` the mode-`a` table is built straight
from the instance as a sum of at most `sr(τ_b)` quadratic phase functions (`Formal.Stab.Magic`)
and evaluated on all `2^{ρ_b}` cut signatures; nothing below that vertex is computed. Everywhere
else the ordinary join of `thm:fourier-speedup` runs on ordinary tables. The switch from the
magic price to the width price therefore happens at most once on each rootward branch, and never
reverses.

We represent the antichain by its indicator `S : RTree I.V → Bool`. The recursion prunes by
construction: `costSwitch` stops descending wherever `S` fires, so the vertices that actually pay
the magic price are exactly those where `S` fires with no strict ancestor doing so. No separate
well-formedness condition on `S` is needed.

`costSwitch` is `eq:total-cost` term for term, and this file proves the three claims the paper
makes about it:

* `costSwitch_false_eq_costMode` — with no switch at all the cost *is* `costMode`, so
  `hybrid_never_loses_spec` inherits `SopInstance.costMode_le'` verbatim;
* `costSwitch_clifford_le` — switching at the root of a Clifford instance costs at most `2`
  operations, independently of the width;
* `costOpt_le` with `costSwitch_bestS` — the postorder recurrence `eq:switch-recurrence` returns
  the exact minimum over antichains, and `bestS` attains it.

## Scope

* The stabilizer rank is an **abstract parameter** `sr : RTree I.V → ℕ`. The quantitative upper
  bound on `sr(τ)` quoted in the paper is cited literature; it is neither assumed nor axiomatized
  anywhere in this development, and no theorem below constrains `sr` except by an explicit
  hypothesis of that theorem (as in `costSwitch_clifford_le`). In particular the optimizer results
  hold for *every* `sr`, matching the paper's claim that the recurrence needs no hypothesis on the
  price.
* Only the operation count is modelled, exactly as in `Formal.Core.Cost`. Soundness of the switch
  — that the QPF sum really does evaluate to the DP table — is the content of `Formal.Stab.QPF`,
  `Formal.Stab.Closure`, `Formal.Stab.Join` and `Formal.Stab.Magic`, and is out of scope here.
* The paper's `O(n)` running time for the recurrence is a statement about a machine model and is
  not formalized. What is formalized is that the recurrence returns the exact minimum.
-/

namespace Formal
namespace Stab

open SopInstance

variable (I : SopInstance) (sr : RTree I.V → ℕ) (S : RTree I.V → Bool)

/-- The charge at a switch vertex `b`: building the QPF sum costs `sr(τ_b)` and evaluating it on
the `2^{ρ_b}` cut signatures costs `sr(τ_b) · 2^{ρ_b}`, the two terms of the first sum of
`eq:total-cost`. -/
noncomputable def switchCharge (u : RTree I.V) : ℕ := sr u * (1 + I.statesSig u)

/-- Operation count of `alg:stabjoin` under the antichain with indicator `S`, in the model of
`SopInstance.costMode`. A switch vertex pays `switchCharge` and prunes its subtree; every other
internal vertex pays the ordinary pair scan `statesSig L · statesSig R`; every other leaf pays the
`2` of the ordinary leaf initialization. This is `eq:total-cost`. -/
noncomputable def costSwitch : RTree I.V → ℕ
  | .leaf v => if S (.leaf v) then switchCharge I sr (.leaf v) else 2
  | .node L R =>
    if S (.node L R) then switchCharge I sr (.node L R)
    else costSwitch L + costSwitch R + I.statesSig L * I.statesSig R

@[simp] theorem costSwitch_leaf (v : I.V) :
    costSwitch I sr S (.leaf v)
      = if S (.leaf v) then switchCharge I sr (.leaf v) else 2 := rfl

@[simp] theorem costSwitch_node (L R : RTree I.V) :
    costSwitch I sr S (.node L R)
      = if S (.node L R) then switchCharge I sr (.node L R)
        else costSwitch I sr S L + costSwitch I sr S R + I.statesSig L * I.statesSig R := rfl

/-! ## The empty antichain: the ordinary DP, unchanged -/

/-- **`thm:hybrid-dp`, the `B = ∅` endpoint.** With no switch anywhere, `alg:stabjoin` performs
exactly the operations of the per-mode DP: the two definitions agree at every leaf and at every
internal vertex. This is the paper's "choosing `B = ∅` recovers `thm:fourier-speedup` exactly". -/
theorem costSwitch_false_eq_costMode :
    ∀ t : RTree I.V, costSwitch I sr (fun _ => false) t = I.costMode t := by
  intro t
  induction t with
  | leaf v => simp
  | node L R ihL ihR => simp [ihL, ihR]

/-- **The optimization never loses.** Under a width-`k` decomposition the empty antichain meets
the same `|V| · (2 + 4^k)` operation count as `SopInstance.costMode_le'`. Unlike the earlier
list-based model this carries no `1 ≤ k` hypothesis: the two cost functions are equal, not merely
comparable. -/
theorem costSwitch_false_le (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    costSwitch I sr (fun _ => false) D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) := by
  rw [costSwitch_false_eq_costMode]
  exact I.costMode_le' D hw

/-! ## The root switch: the Clifford collapse -/

/-- The root cut of a covering decomposition is empty, so its cut matrix has no columns and its
rank is `0`. -/
theorem cutRankOf_verts_root (D : RankDecomp I) : I.cutRankOf D.tree.verts = 0 := by
  have hempty : IsEmpty {w // w ∉ D.tree.verts} := by
    constructor
    rintro ⟨w, hw⟩
    rw [D.covers] at hw
    exact hw (Finset.mem_univ w)
  have hle := Matrix.rank_le_card_width (I.cutMatrix D.tree.verts)
  have hcard : Fintype.card {w // w ∉ D.tree.verts} = 0 := by
    simp [Fintype.card_eq_zero]
  simpa [SopInstance.cutRankOf, hcard] using hle

/-- At the root only the empty signature occurs. -/
theorem statesSig_root (D : RankDecomp I) : I.statesSig D.tree ≤ 1 := by
  have := I.statesSig_le (k := 0) (le_of_eq (cutRankOf_verts_root I D))
  simpa using this

/-- **`thm:hybrid-dp`, the root-switch endpoint (Gottesman–Knill).** On an instance with no
`a`-magic vertex — `sr ≤ 1` — switching at the root evaluates the whole mode sum in at most `2`
operations, on any graph and at any rank-width, where the ordinary join of
`SopInstance.costMode_le'` still pays `4^k` per internal vertex. -/
theorem costSwitch_clifford_le (D : RankDecomp I) (hsr : ∀ u, sr u ≤ 1) :
    costSwitch I sr (fun _ => true) D.tree ≤ 2 := by
  have hcharge : switchCharge I sr D.tree ≤ 2 := by
    have h1 : sr D.tree ≤ 1 := hsr D.tree
    have h2 : 1 + I.statesSig D.tree ≤ 2 := by
      have := statesSig_root I D
      omega
    calc switchCharge I sr D.tree = sr D.tree * (1 + I.statesSig D.tree) := rfl
      _ ≤ 1 * 2 := Nat.mul_le_mul h1 h2
      _ = 2 := by norm_num
  cases htree : D.tree with
  | leaf v => rw [htree] at hcharge; simpa using hcharge
  | node L R => rw [htree] at hcharge; simpa using hcharge

/-! ## The optimizer: `eq:switch-recurrence` -/

/-- The postorder recurrence of `eq:switch-recurrence`: at every vertex, take the cheaper of
switching here and pruning, or paying the ordinary join above the children's optima. -/
noncomputable def costOpt : RTree I.V → ℕ
  | .leaf v => min (switchCharge I sr (.leaf v)) 2
  | .node L R =>
    min (switchCharge I sr (.node L R)) (costOpt L + costOpt R + I.statesSig L * I.statesSig R)

@[simp] theorem costOpt_leaf (v : I.V) :
    costOpt I sr (.leaf v) = min (switchCharge I sr (.leaf v)) 2 := rfl

@[simp] theorem costOpt_node (L R : RTree I.V) :
    costOpt I sr (.node L R)
      = min (switchCharge I sr (.node L R))
          (costOpt I sr L + costOpt I sr R + I.statesSig L * I.statesSig R) := rfl

/-- **The recurrence is a lower bound.** No antichain beats `costOpt`, whatever the price `sr`. -/
theorem costOpt_le : ∀ t : RTree I.V, costOpt I sr t ≤ costSwitch I sr S t := by
  intro t
  induction t with
  | leaf v =>
      by_cases h : S (.leaf v) = true <;> simp [h]
  | node L R ihL ihR =>
      by_cases h : S (.node L R) = true
      · simp [h]
      · simp only [costSwitch_node, costOpt_node, h, Bool.not_eq_true] at *
        refine le_trans (min_le_right _ _) ?_
        exact Nat.add_le_add (Nat.add_le_add ihL ihR) le_rfl

/-- The antichain the recurrence selects: switch exactly where switching is no dearer than
computing the subtree. Defining it by the same recursion avoids gluing per-subtree witnesses into
a total indicator. -/
noncomputable def bestS : RTree I.V → Bool
  | .leaf v => decide (switchCharge I sr (.leaf v) ≤ 2)
  | .node L R =>
    decide (switchCharge I sr (.node L R)
      ≤ costOpt I sr L + costOpt I sr R + I.statesSig L * I.statesSig R)

@[simp] theorem bestS_leaf (v : I.V) :
    bestS I sr (.leaf v) = decide (switchCharge I sr (.leaf v) ≤ 2) := rfl

@[simp] theorem bestS_node (L R : RTree I.V) :
    bestS I sr (.node L R)
      = decide (switchCharge I sr (.node L R)
          ≤ costOpt I sr L + costOpt I sr R + I.statesSig L * I.statesSig R) := rfl

/-- **The bound is attained.** `bestS` is an antichain whose cost is exactly `costOpt`, so
`eq:switch-recurrence` returns the true minimum over antichains together with a witness realizing
it. -/
theorem costSwitch_bestS : ∀ t : RTree I.V,
    costSwitch I sr (bestS I sr) t = costOpt I sr t := by
  intro t
  induction t with
  | leaf v =>
      by_cases h : switchCharge I sr (.leaf v) ≤ 2
      · simp [h]
      · simp [h]
        omega
  | node L R ihL ihR =>
      by_cases h : switchCharge I sr (.node L R)
          ≤ costOpt I sr L + costOpt I sr R + I.statesSig L * I.statesSig R
      · simp [h]
      · simp only [costSwitch_node, costOpt_node, bestS_node, decide_eq_true_eq, h, if_false]
        rw [ihL, ihR]
        omega

end Stab
end Formal
