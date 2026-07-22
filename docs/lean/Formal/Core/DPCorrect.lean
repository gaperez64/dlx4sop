/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.DP

/-!
# Correctness of the rank-decomposition DP (`thm:dp-correct`)

The DP table `Dalg` equals its specification `Dspec` (`Dalg_eq_Dspec`), hence the DP returns the
residue counts `N_j` (`dp_returns_counts`). Definitions live in `Formal.Core.DP`; the crossing
parity is well-defined by `chi_well_defined`.
-/

namespace Formal.SopInstance
open scoped BigOperators

variable (I : SopInstance)

/-! ## Leaf case -/

/-- On a leaf `v`, the supported assignments are exactly `0` and `Pi.single v 1`. -/
theorem supported_leaf_iff (v : I.V) (z : I.V → ZMod 2) :
    I.supported (RTree.leaf v) z ↔ (z = 0 ∨ z = Pi.single v 1) := by
  classical
  have hbin : ∀ a : ZMod 2, a = 0 ∨ a = 1 := by decide
  constructor
  · intro hz
    have hzw : ∀ w, w ≠ v → z w = 0 := by
      intro w hw
      exact hz w (by simp [RTree.verts, hw])
    rcases hbin (z v) with h | h
    · left
      funext w
      rcases eq_or_ne w v with rfl | hw
      · simpa using h
      · simpa using hzw w hw
    · right
      funext w
      rcases eq_or_ne w v with rfl | hw
      · rw [Pi.single_eq_same]; exact h
      · rw [Pi.single_eq_of_ne hw]; exact hzw w hw
  · intro hz w hw
    have hwv : w ≠ v := by simpa [RTree.verts] using hw
    rcases hz with rfl | rfl
    · rfl
    · exact Pi.single_eq_of_ne hwv 1

theorem Dalg_leaf_eq_Dspec (v : I.V) (σ : I.V → ZMod 2) (s : ZMod I.r) :
    I.Dalg (RTree.leaf v) σ s = I.Dspec (RTree.leaf v) σ s := by
  classical
  have hne : (0 : I.V → ZMod 2) ≠ Pi.single v 1 := by
    intro h
    have h2 := congrFun h v
    rw [Pi.zero_apply, Pi.single_eq_same] at h2
    exact absurd h2 (by decide)
  have hDspec : I.Dspec (RTree.leaf v) σ s
      = (({0, Pi.single v 1} : Finset (I.V → ZMod 2)).filter
          (fun z => I.sig (RTree.leaf v) z = σ ∧ I.phi z = s)).card := by
    unfold SopInstance.Dspec
    congr 1
    ext z
    simp only [Finset.mem_filter, Finset.mem_univ, true_and, Finset.mem_insert,
      Finset.mem_singleton]
    rw [supported_leaf_iff]
  rw [hDspec, Finset.card_filter, Finset.sum_pair hne]
  change (if σ = I.sig (RTree.leaf v) 0 ∧ s = I.phi 0 then 1 else 0)
      + (if σ = I.sig (RTree.leaf v) (Pi.single v 1) ∧ s = I.phi (Pi.single v 1) then 1 else 0)
    = (if I.sig (RTree.leaf v) 0 = σ ∧ I.phi 0 = s then 1 else 0)
      + (if I.sig (RTree.leaf v) (Pi.single v 1) = σ ∧ I.phi (Pi.single v 1) = s then 1 else 0)
  congr 1 <;>
    · refine if_congr ?_ rfl rfl
      constructor <;> (intro h; exact ⟨h.1.symm, h.2.symm⟩)

/-! ## The support-splitting map -/

/-- Restrict an assignment to the vertices of a subtree. -/
def restrict (u : RTree I.V) (z : I.V → ZMod 2) : I.V → ZMod 2 :=
  fun w => if w ∈ u.verts then z w else 0

theorem restrict_supported (u : RTree I.V) (z : I.V → ZMod 2) :
    I.supported u (restrict I u z) := by
  intro w hw
  simp [restrict, hw]

/-- On its own vertex set, `restrict` agrees with the original assignment. -/
theorem restrict_apply_mem {u : RTree I.V} {z : I.V → ZMod 2} {w : I.V} (hw : w ∈ u.verts) :
    restrict I u z w = z w := by
  simp [restrict, hw]

theorem add_supported_node {L R : RTree I.V} {a b : I.V → ZMod 2}
    (ha : I.supported L a) (hb : I.supported R b) :
    I.supported (RTree.node L R) (a + b) := by
  intro w hw
  rw [RTree.verts_node, Finset.notMem_union] at hw
  simp [Pi.add_apply, ha w hw.1, hb w hw.2]

/-- Splitting recovers the two pieces (left). -/
theorem restrict_left_add {L R : RTree I.V} (hdisj : Disjoint L.verts R.verts)
    {a b : I.V → ZMod 2} (ha : I.supported L a) (hb : I.supported R b) :
    restrict I L (a + b) = a := by
  funext w
  by_cases hw : w ∈ L.verts
  · have hbw : b w = 0 := hb w (Finset.disjoint_left.mp hdisj hw)
    simp [restrict, hw, Pi.add_apply, hbw]
  · simp [restrict, hw, ha w hw]

theorem restrict_right_add {L R : RTree I.V} (hdisj : Disjoint L.verts R.verts)
    {a b : I.V → ZMod 2} (ha : I.supported L a) (hb : I.supported R b) :
    restrict I R (a + b) = b := by
  funext w
  by_cases hw : w ∈ R.verts
  · have haw : a w = 0 := ha w (Finset.disjoint_right.mp hdisj hw)
    simp [restrict, hw, Pi.add_apply, haw]
  · simp [restrict, hw, hb w hw]

/-- The two restrictions of a node-supported assignment reconstruct it. -/
theorem restrict_add_self {L R : RTree I.V} (hdisj : Disjoint L.verts R.verts)
    {z : I.V → ZMod 2} (hz : I.supported (RTree.node L R) z) :
    restrict I L z + restrict I R z = z := by
  funext w
  simp only [Pi.add_apply, restrict]
  by_cases hwL : w ∈ L.verts
  · have hwR : w ∉ R.verts := Finset.disjoint_left.mp hdisj hwL
    simp [hwL, hwR]
  · by_cases hwR : w ∈ R.verts
    · simp [hwL, hwR]
    · have : w ∉ (RTree.node L R).verts := by
        rw [RTree.verts_node, Finset.notMem_union]; exact ⟨hwL, hwR⟩
      simp [hwL, hwR, hz w this]

/-! ## The `η = r/2` scaling map and the selection-parity bridge -/

theorem zmod2_cases (a : ZMod 2) : a = 0 ∨ a = 1 := by revert a; decide

/-- `η(t) = (r/2)·t.val : ZMod r`; additive on `ZMod 2` because `2·(r/2) ≡ 0`. -/
noncomputable def eta (t : ZMod 2) : ZMod I.r := ((I.r / 2 : ℕ) : ZMod I.r) * (t.val : ZMod I.r)

theorem eta_add (s t : ZMod 2) : eta I (s + t) = eta I s + eta I t := by
  have hsum0 : ((I.r / 2 : ℕ) : ZMod I.r) + ((I.r / 2 : ℕ) : ZMod I.r) = 0 := by
    obtain ⟨k, hk⟩ := I.hr
    have h2 : I.r / 2 + I.r / 2 = I.r := by omega
    calc ((I.r / 2 : ℕ) : ZMod I.r) + ((I.r / 2 : ℕ) : ZMod I.r)
        = ((I.r / 2 + I.r / 2 : ℕ) : ZMod I.r) := by push_cast; ring
      _ = ((I.r : ℕ) : ZMod I.r) := by rw [h2]
      _ = 0 := ZMod.natCast_self I.r
  have v0 : ((0 : ZMod 2).val : ZMod I.r) = 0 := by simp
  have v1 : ((1 : ZMod 2).val : ZMod I.r) = 1 := by
    rw [show (1 : ZMod 2).val = 1 from by decide, Nat.cast_one]
  rcases zmod2_cases s with rfl | rfl <;> rcases zmod2_cases t with rfl | rfl
  · rw [show ((0 : ZMod 2) + 0) = 0 from by decide]
    simp only [eta, v0, mul_zero, add_zero]
  · rw [show ((0 : ZMod 2) + 1) = 1 from by decide]
    simp only [eta, v0, v1, mul_zero, mul_one, zero_add]
  · rw [show ((1 : ZMod 2) + 0) = 1 from by decide]
    simp only [eta, v0, v1, mul_zero, mul_one, add_zero]
  · rw [show ((1 : ZMod 2) + 1) = 0 from by decide]
    simp only [eta, v0, v1, mul_zero, mul_one]
    exact hsum0.symm

/-- The selection parity: the `ZMod 2` sum of the edge products. -/
noncomputable def selParity (z : I.V → ZMod 2) : ZMod 2 :=
  ∑ e ∈ I.G.edgeFinset, I.edgeProd z e

theorem selParity_eq (z : I.V → ZMod 2) : selParity I z = (I.selCount z : ZMod 2) := by
  classical
  have h1 : ∀ e ∈ I.G.edgeFinset,
      I.edgeProd z e = (if I.edgeProd z e = 1 then (1 : ZMod 2) else 0) := by
    intro e _
    rcases zmod2_cases (I.edgeProd z e) with h | h <;> simp [h]
  unfold selParity
  rw [Finset.sum_congr rfl h1, Finset.sum_boole]
  rfl

/-- The load-bearing bridge: `(r/2)·selCount` sees only the selection parity. -/
theorem half_selCount (z : I.V → ZMod 2) :
    ((I.r / 2 : ℕ) : ZMod I.r) * ((I.selCount z : ℕ) : ZMod I.r) = eta I (selParity I z) := by
  rw [selParity_eq, eta, ZMod.val_natCast]
  exact half_mul_reduce I.hr (I.selCount z)

/-! ## The crossing-edge identity -/

/-- The crossing contribution of an edge for a pair of assignments. -/
def crossEdge (a b : I.V → ZMod 2) (e : Sym2 I.V) : ZMod 2 :=
  Sym2.lift ⟨fun x y => a x * b y + a y * b x, fun x y => by ring⟩ e

theorem edgeProd_add (a b : I.V → ZMod 2) (e : Sym2 I.V) :
    I.edgeProd (a + b) e = I.edgeProd a e + I.edgeProd b e + crossEdge I a b e := by
  induction e using Sym2.ind with
  | _ x y =>
    simp only [SopInstance.edgeProd, crossEdge, Sym2.lift_mk, Pi.add_apply]
    ring

/-- Sum over edges of the crossing contribution equals the oriented dart sum. -/
theorem edgeSum_eq_dartSum (a b : I.V → ZMod 2) :
    ∑ e ∈ I.G.edgeFinset, crossEdge I a b e = ∑ d : I.G.Dart, a d.fst * b d.snd := by
  classical
  rw [← Finset.sum_fiberwise_of_maps_to (s := (Finset.univ : Finset I.G.Dart))
        (t := I.G.edgeFinset) (g := fun d => d.edge) (f := fun d => a d.fst * b d.snd)
        (fun d _ => SimpleGraph.mem_edgeFinset.mpr d.edge_mem)]
  refine Finset.sum_congr rfl (fun e he => ?_)
  obtain ⟨x, y⟩ := e
  have hadj : I.G.Adj x y := by
    rw [SimpleGraph.mem_edgeFinset, SimpleGraph.mem_edgeSet] at he; exact he
  have hfilter : (Finset.univ.filter (fun d : I.G.Dart => d.edge = s(x, y)))
      = {(⟨(x, y), hadj⟩ : I.G.Dart), (⟨(x, y), hadj⟩ : I.G.Dart).symm} := by
    ext d
    simp only [Finset.mem_filter, Finset.mem_univ, true_and, Finset.mem_insert,
      Finset.mem_singleton]
    exact SimpleGraph.dart_edge_eq_iff d ⟨(x, y), hadj⟩
  rw [hfilter, Finset.sum_pair (Ne.symm ((⟨(x, y), hadj⟩ : I.G.Dart).symm_ne))]
  simp only [crossEdge, Sym2.lift_mk]
  rfl

/-- The oriented dart sum equals the full oriented double sum weighted by adjacency. -/
theorem dartSum_eq_orderedCross (a b : I.V → ZMod 2) :
    ∑ d : I.G.Dart, a d.fst * b d.snd = ∑ v, ∑ w, a v * I.adj v w * b w := by
  classical
  have step : ∀ v w : I.V, a v * I.adj v w * b w = (if I.G.Adj v w then a v * b w else 0) := by
    intro v w
    by_cases h : I.G.Adj v w <;>
      simp [SopInstance.adj, SimpleGraph.adjMatrix_apply, h]
  calc ∑ d : I.G.Dart, a d.fst * b d.snd
      = ∑ p ∈ Finset.univ.filter (fun p : I.V × I.V => I.G.Adj p.1 p.2), a p.1 * b p.2 := by
        refine Finset.sum_bij' (fun d _ => d.toProd)
          (fun p hp => ⟨p, (Finset.mem_filter.mp hp).2⟩)
          (fun d _ => Finset.mem_filter.mpr ⟨Finset.mem_univ _, d.adj⟩)
          (fun p _ => Finset.mem_univ _)
          (fun d _ => SimpleGraph.Dart.toProd_injective rfl)
          (fun p _ => rfl)
          (fun d _ => rfl)
    _ = ∑ p : I.V × I.V, if I.G.Adj p.1 p.2 then a p.1 * b p.2 else 0 := by
        rw [Finset.sum_filter]
    _ = ∑ p : I.V × I.V, a p.1 * I.adj p.1 p.2 * b p.2 := by
        refine Finset.sum_congr rfl (fun p _ => ?_); rw [step]
    _ = ∑ v, ∑ w, a v * I.adj v w * b w :=
        Fintype.sum_prod_type' (fun v w => a v * I.adj v w * b w)

/-- Under the support hypotheses the full oriented double sum collapses to `rawCross`. -/
theorem orderedCross_eq_rawCross {L R : RTree I.V} {a b : I.V → ZMod 2}
    (ha : I.supported L a) (hb : I.supported R b) :
    ∑ v, ∑ w, a v * I.adj v w * b w = I.rawCross L R a b := by
  classical
  have inner : ∀ v : I.V,
      (∑ w, a v * I.adj v w * b w) = ∑ w ∈ R.verts, a v * I.adj v w * b w := by
    intro v
    refine (Finset.sum_subset (Finset.subset_univ R.verts) (fun w _ hw => ?_)).symm
    rw [hb w hw]; ring
  rw [SopInstance.rawCross]
  refine (Finset.sum_subset (Finset.subset_univ L.verts) (fun v _ hv => ?_)).symm.trans ?_
  · exact Finset.sum_eq_zero (fun w _ => by rw [ha v hv]; ring)
  · exact Finset.sum_congr rfl (fun v _ => inner v)

/-- The full crossing-edge sum equals `rawCross` (using the supports). -/
theorem edgeCross_eq_rawCross {L R : RTree I.V} {a b : I.V → ZMod 2}
    (ha : I.supported L a) (hb : I.supported R b) :
    ∑ e ∈ I.G.edgeFinset, crossEdge I a b e = I.rawCross L R a b := by
  rw [edgeSum_eq_dartSum, dartSum_eq_orderedCross, orderedCross_eq_rawCross I ha hb]

/-- The selection parity splits into the two sides plus the crossing. -/
theorem selParity_add {L R : RTree I.V} {a b : I.V → ZMod 2}
    (ha : I.supported L a) (hb : I.supported R b) :
    selParity I (a + b) = selParity I a + selParity I b + I.rawCross L R a b := by
  unfold selParity
  rw [Finset.sum_congr rfl (fun e _ => edgeProd_add I a b e),
    Finset.sum_add_distrib, Finset.sum_add_distrib, edgeCross_eq_rawCross I ha hb]

/-! ## Parent residue identity -/

theorem phi_node_add {L R : RTree I.V} (hdisj : Disjoint L.verts R.verts)
    {a b : I.V → ZMod 2} (ha : I.supported L a) (hb : I.supported R b) :
    I.phi (a + b) = I.phi a + I.phi b + I.crossTerm L R (I.sig L a) (I.sig R b) := by
  classical
  have hval : ∀ v : I.V, ((a + b) v).val = (a v).val + (b v).val := by
    intro v
    have hor : a v = 0 ∨ b v = 0 := by
      by_cases hvL : v ∈ L.verts
      · exact Or.inr (hb v (Finset.disjoint_left.mp hdisj hvL))
      · exact Or.inl (ha v hvL)
    rcases hor with h | h <;> simp [Pi.add_apply, h]
  have hunary : (∑ v, I.b v * (((a + b) v).val : ZMod I.r))
      = (∑ v, I.b v * ((a v).val : ZMod I.r)) + (∑ v, I.b v * ((b v).val : ZMod I.r)) := by
    rw [← Finset.sum_add_distrib]
    refine Finset.sum_congr rfl (fun v _ => ?_)
    rw [hval v]; push_cast; ring
  have hcross : eta I (I.rawCross L R a b) = I.crossTerm L R (I.sig L a) (I.sig R b) := by
    simp only [SopInstance.crossTerm, eta]
    rw [I.chi_well_defined L R hdisj ha hb]
  have hquad : ((I.r / 2 : ℕ) : ZMod I.r) * ((I.selCount (a + b) : ℕ) : ZMod I.r)
      = ((I.r / 2 : ℕ) : ZMod I.r) * ((I.selCount a : ℕ) : ZMod I.r)
        + ((I.r / 2 : ℕ) : ZMod I.r) * ((I.selCount b : ℕ) : ZMod I.r)
        + I.crossTerm L R (I.sig L a) (I.sig R b) := by
    rw [half_selCount, half_selCount, half_selCount, selParity_add I ha hb, eta_add, eta_add,
      hcross]
  rw [SopInstance.phi, SopInstance.phi, SopInstance.phi, hunary, hquad]
  abel

/-! ## Parent signature identity -/

theorem sig_node_add {L R : RTree I.V} (hdisj : Disjoint L.verts R.verts)
    {a b : I.V → ZMod 2} (ha : I.supported L a) (hb : I.supported R b) :
    I.sig (RTree.node L R) (a + b) = I.combineSig L R (I.sig L a) (I.sig R b) := by
  funext w
  by_cases hw : w ∈ (RTree.node L R).verts
  · simp only [SopInstance.sig, SopInstance.combineSig, if_pos hw]
  · have hw' : w ∉ L.verts ∪ R.verts := by rwa [RTree.verts_node] at hw
    have hwL : w ∉ L.verts := (Finset.notMem_union.mp hw').1
    have hwR : w ∉ R.verts := (Finset.notMem_union.mp hw').2
    simp only [SopInstance.combineSig, if_neg hw]
    rw [I.sig_apply_not_mem (RTree.node L R) (a + b) hw, I.sig_apply_not_mem L a hwL,
      I.sig_apply_not_mem R b hwR, RTree.verts_node, Finset.sum_union hdisj]
    congr 1
    · refine Finset.sum_congr rfl (fun v hv => ?_)
      have hbv : b v = 0 := hb v (Finset.disjoint_left.mp hdisj hv)
      simp [Pi.add_apply, hbv]
    · refine Finset.sum_congr rfl (fun v hv => ?_)
      have hav : a v = 0 := ha v (Finset.disjoint_right.mp hdisj hv)
      simp [Pi.add_apply, hav]

/-! ## Fiberwise counting -/

open Classical in
/-- The Finset of assignments supported on a subtree. -/
noncomputable def suppFin (u : RTree I.V) : Finset (I.V → ZMod 2) :=
  Finset.univ.filter (fun a => I.supported u a)

theorem mem_suppFin {u : RTree I.V} {a : I.V → ZMod 2} :
    a ∈ suppFin I u ↔ I.supported u a := by
  simp [suppFin]

/-- `Dspec` written as an indicator sum over the supported assignments. -/
theorem Dspec_as_sum (u : RTree I.V) (α : I.V → ZMod 2) (p : ZMod I.r) :
    I.Dspec u α p = ∑ a ∈ suppFin I u, (if I.sig u a = α ∧ I.phi a = p then 1 else 0) := by
  classical
  unfold SopInstance.Dspec suppFin
  rw [← Finset.card_filter, Finset.filter_filter]

/-- Fiberwise regrouping: a `Dspec`-weighted sum collapses to a sum over supported reps. -/
theorem fib (u : RTree I.V) (F : (I.V → ZMod 2) → ZMod I.r → ℕ) :
    (∑ α, ∑ p, F α p * I.Dspec u α p) = ∑ a ∈ suppFin I u, F (I.sig u a) (I.phi a) := by
  classical
  rw [← Finset.sum_fiberwise (suppFin I u) (fun a => (I.sig u a, I.phi a))
        (fun a => F (I.sig u a) (I.phi a)), Fintype.sum_prod_type]
  refine Finset.sum_congr rfl (fun α _ => Finset.sum_congr rfl (fun p _ => ?_))
  rw [Dspec_as_sum, ← Finset.card_filter, mul_comm, ← smul_eq_mul, ← Finset.sum_const]
  refine Finset.sum_congr ?_ ?_
  · apply Finset.filter_congr
    intro a _
    rw [Prod.mk.injEq]
  · intro a ha
    obtain ⟨-, hp⟩ := Finset.mem_filter.mp ha
    rw [Prod.mk.injEq] at hp
    rw [hp.1, hp.2]

/-! ## Node case -/

/-- The specification at a node, reindexed as a double sum over the two children's reps. -/
theorem Dspec_node_as_pair (L R : RTree I.V) (hdisj : Disjoint L.verts R.verts)
    (γ : I.V → ZMod 2) (s : ZMod I.r) :
    I.Dspec (RTree.node L R) γ s
      = ∑ a ∈ suppFin I L, ∑ b ∈ suppFin I R,
          (if I.sig (RTree.node L R) (a + b) = γ ∧ I.phi (a + b) = s then 1 else 0) := by
  classical
  rw [Dspec_as_sum, ← Finset.sum_product']
  refine Finset.sum_nbij' (fun z => (restrict I L z, restrict I R z)) (fun p => p.1 + p.2)
    (fun z _ => ?_) (fun p hp => ?_) (fun z hz => ?_) (fun p hp => ?_) (fun z hz => ?_)
  · rw [Finset.mem_product]
    exact ⟨(mem_suppFin I).mpr (restrict_supported I L z),
      (mem_suppFin I).mpr (restrict_supported I R z)⟩
  · rw [Finset.mem_product] at hp
    exact (mem_suppFin I).mpr
      (add_supported_node I ((mem_suppFin I).mp hp.1) ((mem_suppFin I).mp hp.2))
  · exact restrict_add_self I hdisj ((mem_suppFin I).mp hz)
  · rw [Finset.mem_product] at hp
    have ha := (mem_suppFin I).mp hp.1
    have hb := (mem_suppFin I).mp hp.2
    change (restrict I L (p.1 + p.2), restrict I R (p.1 + p.2)) = p
    rw [restrict_left_add I hdisj ha hb, restrict_right_add I hdisj ha hb]
  · change (if I.sig (RTree.node L R) z = γ ∧ I.phi z = s then (1 : ℕ) else 0)
        = (if I.sig (RTree.node L R) (restrict I L z + restrict I R z) = γ
            ∧ I.phi (restrict I L z + restrict I R z) = s then 1 else 0)
    rw [restrict_add_self I hdisj ((mem_suppFin I).mp hz)]

theorem Dalg_node_eq_Dspec (L R : RTree I.V) (hdisj : Disjoint L.verts R.verts)
    (hL : ∀ σ s, I.Dalg L σ s = I.Dspec L σ s)
    (hR : ∀ σ s, I.Dalg R σ s = I.Dspec R σ s)
    (γ : I.V → ZMod 2) (s : ZMod I.r) :
    I.Dalg (RTree.node L R) γ s = I.Dspec (RTree.node L R) γ s := by
  classical
  rw [Dspec_node_as_pair I L R hdisj]
  have hDalg : I.Dalg (RTree.node L R) γ s
      = ∑ α : I.V → ZMod 2, ∑ p : ZMod I.r, ∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
          if I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s
          then I.Dalg L α p * I.Dalg R β q else 0 := rfl
  rw [hDalg]
  simp only [hL, hR]
  -- Collapse the left sums via `fib L`.
  have stepA : ∀ (α : I.V → ZMod 2) (p : ZMod I.r),
      (∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
        if I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s
        then I.Dspec L α p * I.Dspec R β q else 0)
      = (∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
          if I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s
          then I.Dspec R β q else 0) * I.Dspec L α p := by
    intro α p
    rw [Finset.sum_mul]
    refine Finset.sum_congr rfl (fun β _ => ?_)
    rw [Finset.sum_mul]
    refine Finset.sum_congr rfl (fun q _ => ?_)
    by_cases h : I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s <;>
      simp [h, mul_comm]
  rw [Finset.sum_congr rfl (fun α _ => Finset.sum_congr rfl (fun p _ => stepA α p))]
  rw [fib I L (fun α p => ∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
        if I.combineSig L R α β = γ ∧ p + q + I.crossTerm L R α β = s
        then I.Dspec R β q else 0)]
  refine Finset.sum_congr rfl (fun a ha => ?_)
  have hsupp_a : I.supported L a := (mem_suppFin I).mp ha
  -- Collapse the right sums via `fib R`.
  have stepB : (∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
        if I.combineSig L R (I.sig L a) β = γ
          ∧ I.phi a + q + I.crossTerm L R (I.sig L a) β = s
        then I.Dspec R β q else 0)
      = ∑ β : I.V → ZMod 2, ∑ q : ZMod I.r,
          (if I.combineSig L R (I.sig L a) β = γ
            ∧ I.phi a + q + I.crossTerm L R (I.sig L a) β = s then 1 else 0) * I.Dspec R β q := by
    refine Finset.sum_congr rfl (fun β _ => Finset.sum_congr rfl (fun q _ => ?_))
    by_cases h : I.combineSig L R (I.sig L a) β = γ
        ∧ I.phi a + q + I.crossTerm L R (I.sig L a) β = s <;> simp [h]
  rw [stepB]
  rw [fib I R (fun β q => if I.combineSig L R (I.sig L a) β = γ
        ∧ I.phi a + q + I.crossTerm L R (I.sig L a) β = s then 1 else 0)]
  refine Finset.sum_congr rfl (fun b hb => ?_)
  have hsupp_b : I.supported R b := (mem_suppFin I).mp hb
  rw [← sig_node_add I hdisj hsupp_a hsupp_b, ← phi_node_add I hdisj hsupp_a hsupp_b]

/-! ## Root case -/

/-- The DP table equals its specification on every well-formed subtree. -/
theorem Dalg_eq_Dspec : ∀ (u : RTree I.V), u.WF → ∀ σ s, I.Dalg u σ s = I.Dspec u σ s := by
  intro u
  induction u with
  | leaf v => intro _ σ s; exact Dalg_leaf_eq_Dspec I v σ s
  | node L R ihL ihR =>
      intro hwf σ s
      rw [RTree.WF_node] at hwf
      exact Dalg_node_eq_Dspec I L R hwf.1 (ihL hwf.2.1) (ihR hwf.2.2) σ s

theorem dp_returns_counts (D : RankDecomp I) (j : ZMod I.r) :
    I.Dalg D.tree 0 (j - I.c) = I.N j := by
  classical
  rw [Dalg_eq_Dspec I D.tree D.wf]
  unfold SopInstance.Dspec SopInstance.N
  refine congrArg Finset.card ?_
  ext z
  simp only [Finset.mem_filter, Finset.mem_univ, true_and]
  constructor
  · rintro ⟨-, -, hphi⟩
    have hf : I.f z = I.c + I.phi z := I.f_eq_c_add_phi z
    rw [hf, hphi]; ring
  · intro hf
    refine ⟨?_, ?_, ?_⟩
    · intro w hw
      exact absurd (by rw [D.covers]; exact Finset.mem_univ w) hw
    · funext w
      have hw : w ∈ D.tree.verts := by rw [D.covers]; exact Finset.mem_univ w
      simp [SopInstance.sig, hw]
    · have hj : j = I.c + I.phi z := by rw [← hf]; exact I.f_eq_c_add_phi z
      rw [hj]; ring

end Formal.SopInstance
