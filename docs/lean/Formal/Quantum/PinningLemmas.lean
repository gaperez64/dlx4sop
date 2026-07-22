/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.Pinning

/-!
# Pinning the boundary: proofs

Preservation of the pinning invariants (`CurInj`, `EparSym`, `EparIrrefl`) along compilation,
and the boundary-substitution theorem `pathSum_pinned` (the paper's "pinning the boundary",
sec:sop), decomposed into: the live/free reindexing (`sum_pinned_reindex`), the indicator
split (`indicator_iff`), and the phase-substitution identity (`phase_merge`).
-/

open scoped BigOperators

namespace Formal
namespace Quantum
namespace Sym

variable {n : ℕ}

/-! ## A. Invariant preservation -/

theorem curInj_init (n : ℕ) : (Sym.init n).CurInj := by
  intro a b i ha _
  exact absurd ha (by simp [Sym.init])

theorem eparSym_init (n : ℕ) : (Sym.init n).EparSym := fun _ _ => rfl

theorem eparIrrefl_init (n : ℕ) : (Sym.init n).EparIrrefl := fun _ => rfl

theorem curInj_step (y : Fin n → ZMod 2) (S : Sym n) (g : Gate n)
    (h : S.CurInj) (hw : S.WFS) : (Sym.step y S g).CurInj := by
  cases g with
  | T a =>
      intro p q i hp hq
      rw [step_cur_T] at hp hq
      exact h p q i hp hq
  | CZ a b =>
      intro p q i hp hq
      rw [step_cur_CZ] at hp hq
      exact h p q i hp hq
  | H a =>
      intro p q i hp hq
      simp only [step_cur_H] at hp hq
      by_cases hpa : p = a <;> by_cases hqa : q = a
      · rw [hpa, hqa]
      · rw [if_pos hpa] at hp
        rw [if_neg hqa] at hq
        injection hp with hp
        exact absurd (hw q i hq) (by omega)
      · rw [if_neg hpa] at hp
        rw [if_pos hqa] at hq
        injection hq with hq
        exact absurd (hw p i hp) (by omega)
      · rw [if_neg hpa] at hp
        rw [if_neg hqa] at hq
        exact h p q i hp hq

private theorem flipEdge_eparSym (S : Sym n) (i j : ℕ) (h : S.EparSym) :
    (S.flipEdge i j).EparSym := by
  intro p q
  simp only [flipEdge_epar]
  by_cases hc : (p = i ∧ q = j) ∨ (p = j ∧ q = i)
  · have hc' : (q = i ∧ p = j) ∨ (q = j ∧ p = i) := by tauto
    rw [if_pos hc, if_pos hc', h p q]
  · have hc' : ¬((q = i ∧ p = j) ∨ (q = j ∧ p = i)) := by tauto
    rw [if_neg hc, if_neg hc', h p q]

theorem eparSym_step (y : Fin n → ZMod 2) (S : Sym n) (g : Gate n)
    (h : S.EparSym) : (Sym.step y S g).EparSym := by
  cases g with
  | T a =>
      cases hc : S.cur a with
      | none => rw [step_T_none y S a hc]; exact h
      | some i => rw [step_T_some y S a i hc]; exact h
  | CZ a b =>
      cases hca : S.cur a with
      | none =>
          cases hcb : S.cur b with
          | none => rw [step_CZ_none_none y S a b hca hcb]; exact h
          | some j => rw [step_CZ_none_some y S a b j hca hcb]; exact h
      | some i =>
          cases hcb : S.cur b with
          | none => rw [step_CZ_some_none y S a b i hca hcb]; exact h
          | some j =>
              by_cases hij : i = j
              · subst hij
                rw [step_CZ_some_some_eq y S a b i hca hcb]; exact h
              · rw [step_CZ_some_some_ne y S a b i j hca hcb hij]
                exact flipEdge_eparSym S i j h
  | H a =>
      cases hc : S.cur a with
      | none => rw [step_H_none y S a hc]; exact h
      | some i =>
          rw [step_H_some y S a i hc]
          exact flipEdge_eparSym S i S.next h

private theorem flipEdge_eparIrrefl (S : Sym n) (i j : ℕ) (hij : i ≠ j)
    (h : S.EparIrrefl) : (S.flipEdge i j).EparIrrefl := by
  intro m
  simp only [flipEdge_epar]
  have hc : ¬((m = i ∧ m = j) ∨ (m = j ∧ m = i)) := by
    rintro (⟨h1, h2⟩ | ⟨h1, h2⟩) <;> omega
  rw [if_neg hc]
  exact h m

theorem eparIrrefl_step (y : Fin n → ZMod 2) (S : Sym n) (g : Gate n)
    (hi : S.EparIrrefl) (hw : S.WFS) : (Sym.step y S g).EparIrrefl := by
  cases g with
  | T a =>
      cases hc : S.cur a with
      | none => rw [step_T_none y S a hc]; exact hi
      | some i => rw [step_T_some y S a i hc]; exact hi
  | CZ a b =>
      cases hca : S.cur a with
      | none =>
          cases hcb : S.cur b with
          | none => rw [step_CZ_none_none y S a b hca hcb]; exact hi
          | some j => rw [step_CZ_none_some y S a b j hca hcb]; exact hi
      | some i =>
          cases hcb : S.cur b with
          | none => rw [step_CZ_some_none y S a b i hca hcb]; exact hi
          | some j =>
              by_cases hij : i = j
              · subst hij
                rw [step_CZ_some_some_eq y S a b i hca hcb]; exact hi
              · rw [step_CZ_some_some_ne y S a b i j hca hcb hij]
                exact flipEdge_eparIrrefl S i j hij hi
  | H a =>
      cases hc : S.cur a with
      | none => rw [step_H_none y S a hc]; exact hi
      | some i =>
          rw [step_H_some y S a i hc]
          exact flipEdge_eparIrrefl S i S.next (Nat.ne_of_lt (hw a i hc)) hi

theorem invariants_compile (y : Fin n → ZMod 2) (C : Circuit n) :
    (Sym.compile y C).CurInj ∧ (Sym.compile y C).EparSym ∧ (Sym.compile y C).EparIrrefl := by
  have key : ∀ (C : Circuit n) (S : Sym n),
      S.WFS → S.CurInj → S.EparSym → S.EparIrrefl →
        (C.foldl (step y) S).WFS ∧ (C.foldl (step y) S).CurInj
          ∧ (C.foldl (step y) S).EparSym ∧ (C.foldl (step y) S).EparIrrefl := by
    intro C
    induction C with
    | nil => intro S hw h1 h2 h3; exact ⟨hw, h1, h2, h3⟩
    | cons g C ih =>
        intro S hw h1 h2 h3
        simp only [List.foldl_cons]
        exact ih (step y S g) (wfs_step y S g hw) (curInj_step y S g h1 hw)
          (eparSym_step y S g h2) (eparIrrefl_step y S g h3 hw)
  obtain ⟨_, h1, h2, h3⟩ := key C (init n) (wfs_init n) (curInj_init n)
    (eparSym_init n) (eparIrrefl_init n)
  exact ⟨h1, h2, h3⟩

theorem curInj_compile (y : Fin n → ZMod 2) (C : Circuit n) : (Sym.compile y C).CurInj :=
  (invariants_compile y C).1

theorem eparSym_compile (y : Fin n → ZMod 2) (C : Circuit n) : (Sym.compile y C).EparSym :=
  (invariants_compile y C).2.1

theorem eparIrrefl_compile (y : Fin n → ZMod 2) (C : Circuit n) :
    (Sym.compile y C).EparIrrefl :=
  (invariants_compile y C).2.2

/-! ## B. Live variables and pinned values -/

theorem mem_liveSet_iff (S : Sym n) {i : ℕ} : i ∈ S.liveSet ↔ ∃ a, S.cur a = some i := by
  simp [liveSet, Finset.mem_biUnion, Option.mem_toFinset, Option.mem_def]

theorem liveSet_subset_range (S : Sym n) (hw : S.WFS) : S.liveSet ⊆ Finset.range S.next := by
  intro i hi
  obtain ⟨a, ha⟩ := (mem_liveSet_iff S).mp hi
  exact Finset.mem_range.mpr (hw a i ha)

theorem pinVal_spec (S : Sym n) (hinj : S.CurInj) (z : Fin n → ZMod 2) {a : Fin n} {i : ℕ}
    (h : S.cur a = some i) : S.pinVal z i = z a := by
  have hex : ∃ b, S.cur b = some i := ⟨a, h⟩
  have hval : S.pinVal z i = z hex.choose := by
    simp only [pinVal]
    exact dif_pos hex
  rw [hval]
  exact congrArg z (hinj hex.choose a i hex.choose_spec h)

/-- Merge a free-variable assignment with the pinned live values into a full assignment. -/
noncomputable def mergeAssign (S : Sym n) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) : Fin S.next → ZMod 2 :=
  fun m => if hm : (m : ℕ) ∈ S.freeSet then xf ⟨m, hm⟩ else S.pinVal z m

theorem ext_mergeAssign (S : Sym n) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) {m : ℕ} (hm : m < S.next) :
    S.ext (S.mergeAssign z xf) m
      = if h : m ∈ S.freeSet then xf ⟨m, h⟩ else S.pinVal z m := by
  show (if h : m < S.next then S.mergeAssign z xf ⟨m, h⟩ else 0)
      = if h : m ∈ S.freeSet then xf ⟨m, h⟩ else S.pinVal z m
  exact dif_pos hm

private theorem mem_freeSet_iff (S : Sym n) {m : ℕ} :
    m ∈ S.freeSet ↔ m < S.next ∧ m ∉ S.liveSet := by
  simp [freeSet, Finset.mem_sdiff, Finset.mem_range]

private theorem ext_fin (S : Sym n) (x : Fin S.next → ZMod 2) (m : Fin S.next) :
    S.ext x (m : ℕ) = x m := by
  show (if h : (m : ℕ) < S.next then x ⟨(m : ℕ), h⟩ else 0) = x m
  exact dif_pos m.isLt

/-- Reindex a live-pinned sum over all assignments as a sum over free assignments. -/
theorem sum_pinned_reindex (S : Sym n) (hw : S.WFS) (z : Fin n → ZMod 2)
    {M : Type} [AddCommMonoid M] (F : (Fin S.next → ZMod 2) → M) :
    (∑ x : Fin S.next → ZMod 2,
      if ∀ i ∈ S.liveSet, S.ext x i = S.pinVal z i then F x else 0)
      = ∑ xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2, F (S.mergeAssign z xf) := by
  have hlt : ∀ u : {i : ℕ // i ∈ S.freeSet}, u.1 < S.next := fun u =>
    ((mem_freeSet_iff S).mp u.2).1
  have hmerge_mem : ∀ xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2,
      ∀ i ∈ S.liveSet, S.ext (S.mergeAssign z xf) i = S.pinVal z i := by
    intro xf i hi
    have hi_lt : i < S.next := Finset.mem_range.mp (liveSet_subset_range S hw hi)
    have hi_nf : i ∉ S.freeSet := fun hf => ((mem_freeSet_iff S).mp hf).2 hi
    rw [ext_mergeAssign S z xf hi_lt, dif_neg hi_nf]
  have hleft : ∀ x : Fin S.next → ZMod 2, (∀ i ∈ S.liveSet, S.ext x i = S.pinVal z i) →
      S.mergeAssign z (fun u => x ⟨u.1, hlt u⟩) = x := by
    intro x hx
    funext m
    show (if hm : (m : ℕ) ∈ S.freeSet then x ⟨(m : ℕ), hlt ⟨(m : ℕ), hm⟩⟩
        else S.pinVal z (m : ℕ)) = x m
    by_cases hm : (m : ℕ) ∈ S.freeSet
    · exact dif_pos hm
    · have hmlive : (m : ℕ) ∈ S.liveSet := by
        by_contra hnot
        exact hm ((mem_freeSet_iff S).mpr ⟨m.isLt, hnot⟩)
      exact (dif_neg hm).trans ((hx _ hmlive).symm.trans (ext_fin S x m))
  have hright : ∀ xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2,
      (fun u : {i : ℕ // i ∈ S.freeSet} => S.mergeAssign z xf ⟨u.1, hlt u⟩) = xf := by
    intro xf
    funext u
    show (if h : u.1 ∈ S.freeSet then xf ⟨u.1, h⟩ else S.pinVal z u.1) = xf u
    exact dif_pos u.2
  refine ((Finset.sum_filter _ _).symm.trans ?_)
  refine Finset.sum_nbij' (fun x u => x ⟨u.1, hlt u⟩) (S.mergeAssign z) ?_ ?_ ?_ ?_ ?_
  · exact fun x _ => Finset.mem_univ _
  · exact fun xf _ => Finset.mem_filter.mpr ⟨Finset.mem_univ _, hmerge_mem xf⟩
  · exact fun x hx => hleft x (Finset.mem_filter.mp hx).2
  · exact fun xf _ => hright xf
  · exact fun x hx => (congrArg F (hleft x (Finset.mem_filter.mp hx).2)).symm

/-! ## C. The substitution -/

theorem indicator_iff (S : Sym n) (hinj : S.CurInj) (y z : Fin n → ZMod 2)
    (x : Fin S.next → ZMod 2) :
    (∀ a, S.wireVal y x a = z a)
      ↔ ((∀ a, S.cur a = none → y a = z a)
          ∧ ∀ i ∈ S.liveSet, S.ext x i = S.pinVal z i) := by
  constructor
  · intro h
    refine ⟨fun a ha => ?_, fun i hi => ?_⟩
    · have hv : S.wireVal y x a = y a := by unfold wireVal; rw [ha]
      rw [← hv]
      exact h a
    · obtain ⟨a, ha⟩ := (mem_liveSet_iff S).mp hi
      have hv : S.wireVal y x a = S.ext x i := by unfold wireVal; rw [ha]
      rw [pinVal_spec S hinj z ha, ← hv]
      exact h a
  · rintro ⟨hc, hl⟩ a
    cases hcur : S.cur a with
    | none =>
        have hv : S.wireVal y x a = y a := by unfold wireVal; rw [hcur]
        rw [hv]
        exact hc a hcur
    | some i =>
        have hv : S.wireVal y x a = S.ext x i := by unfold wireVal; rw [hcur]
        rw [hv, hl i ((mem_liveSet_iff S).mpr ⟨a, hcur⟩)]
        exact pinVal_spec S hinj z hcur

/-- The pinned instance's root of unity is `ω₈`. -/
theorem omega_pinned (S : Sym n) (hsym : S.EparSym) (z : Fin n → ZMod 2) :
    (S.pinnedInstance hsym z).omega = omega8 := by
  have hr : (S.pinnedInstance hsym z).r = 8 := rfl
  unfold SopInstance.omega omega8
  rw [hr]
  norm_num

private theorem merge_free (S : Sym n) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) (u : {i : ℕ // i ∈ S.freeSet}) :
    S.ext (S.mergeAssign z xf) u.1 = xf u := by
  rw [ext_mergeAssign S z xf ((mem_freeSet_iff S).mp u.2).1, dif_pos u.2]

private theorem merge_live (S : Sym n) (hw : S.WFS) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) {i : ℕ} (hi : i ∈ S.liveSet) :
    S.ext (S.mergeAssign z xf) i = S.pinVal z i := by
  have hlt : i < S.next := Finset.mem_range.mp (liveSet_subset_range S hw hi)
  have hnf : i ∉ S.freeSet := fun hf => ((mem_freeSet_iff S).mp hf).2 hi
  rw [ext_mergeAssign S z xf hlt, dif_neg hnf]

private theorem free_union_live (S : Sym n) (hw : S.WFS) :
    S.freeSet ∪ S.liveSet = Finset.range S.next :=
  Finset.sdiff_union_of_subset (liveSet_subset_range S hw)

private theorem disjoint_free_live (S : Sym n) : Disjoint S.freeSet S.liveSet := by
  simp only [freeSet]
  exact Finset.sdiff_disjoint

/-- Reindex a sum over free names as a sum over the free subtype, evaluating the merged
assignment to `xf`. -/
private theorem sum_free_attach (S : Sym n) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) (F : ℕ → ZMod 2 → ZMod 8) :
    (∑ i ∈ S.freeSet, F i (S.ext (S.mergeAssign z xf) i))
      = ∑ u : {i : ℕ // i ∈ S.freeSet}, F u.1 (xf u) := by
  rw [Finset.univ_eq_attach,
    ← Finset.sum_attach S.freeSet (fun i => F i (S.ext (S.mergeAssign z xf) i))]
  exact Finset.sum_congr rfl fun u _ => by rw [merge_free S z xf u]

/-- Split a double sum over the (disjoint) union of free and live names into four blocks. -/
private theorem sum_union_double {M : Type*} [AddCommMonoid M] (f l : Finset ℕ)
    (hd : Disjoint f l) (P : ℕ → ℕ → M) :
    (∑ i ∈ f ∪ l, ∑ j ∈ f ∪ l, P i j)
      = ((∑ i ∈ f, ∑ j ∈ f, P i j) + ∑ i ∈ f, ∑ j ∈ l, P i j)
        + ((∑ i ∈ l, ∑ j ∈ f, P i j) + ∑ i ∈ l, ∑ j ∈ l, P i j) := by
  have hs : ∀ s : Finset ℕ, (∑ i ∈ s, ∑ j ∈ f ∪ l, P i j)
      = (∑ i ∈ s, ∑ j ∈ f, P i j) + ∑ i ∈ s, ∑ j ∈ l, P i j := fun s => by
    rw [← Finset.sum_add_distrib]
    exact Finset.sum_congr rfl fun i _ => Finset.sum_union hd
  rw [Finset.sum_union hd, hs f, hs l]

/-- The unary coefficients of the compiled phase, on a merged assignment. -/
private theorem pin_bsum (S : Sym n) (hw : S.WFS) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    (∑ i ∈ Finset.range S.next, S.b i * lift28 (S.ext (S.mergeAssign z xf) i))
      = (∑ u : {i : ℕ // i ∈ S.freeSet}, S.b u.1 * lift28 (xf u))
        + ∑ i ∈ S.liveSet, S.b i * lift28 (S.pinVal z i) := by
  rw [← free_union_live S hw, Finset.sum_union (disjoint_free_live S)]
  congr 1
  · exact sum_free_attach S z xf fun i e => S.b i * lift28 e
  · exact Finset.sum_congr rfl fun i hi => by rw [merge_live S hw z xf hi]

private theorem lift28_mul_ite : ∀ e s t : ZMod 2,
    lift28 e * lift28 s * lift28 t = if e = 1 ∧ s * t = 1 then (1 : ZMod 8) else 0 := by
  decide

/-- The free–free block of the quadratic sum is the honest selected-edge count of the
pinned instance. -/
private theorem pin_freefree (S : Sym n) (hsym : S.EparSym) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    (∑ i ∈ S.freeSet, ∑ j ∈ S.freeSet, if i < j then lift28 (S.epar i j)
        * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j) else 0)
      = ((S.pinnedInstance hsym z).selCount xf : ZMod 8) := by
  calc (∑ i ∈ S.freeSet, ∑ j ∈ S.freeSet, if i < j then lift28 (S.epar i j)
        * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j) else 0)
      = ∑ u : {i : ℕ // i ∈ S.freeSet}, ∑ j ∈ S.freeSet,
          if u.1 < j then lift28 (S.epar u.1 j)
            * lift28 (xf u) * lift28 (S.ext (S.mergeAssign z xf) j) else 0 :=
        sum_free_attach S z xf fun i e => ∑ j ∈ S.freeSet, if i < j then lift28 (S.epar i j)
          * lift28 e * lift28 (S.ext (S.mergeAssign z xf) j) else 0
    _ = ∑ u : {i : ℕ // i ∈ S.freeSet}, ∑ v : {i : ℕ // i ∈ S.freeSet},
          if u.1 < v.1 then lift28 (S.epar u.1 v.1) * lift28 (xf u) * lift28 (xf v)
          else 0 :=
        Finset.sum_congr rfl fun u _ => sum_free_attach S z xf fun j e => if u.1 < j then
          lift28 (S.epar u.1 j) * lift28 (xf u) * lift28 e else 0
    _ = ∑ u : {i : ℕ // i ∈ S.freeSet}, ∑ v : {i : ℕ // i ∈ S.freeSet},
          if u.1 < v.1 ∧ S.epar u.1 v.1 = 1 ∧ xf u * xf v = 1 then (1 : ZMod 8) else 0 := by
        refine Finset.sum_congr rfl fun u _ => Finset.sum_congr rfl fun v _ => ?_
        by_cases hlt : u.1 < v.1
        · rw [if_pos hlt, lift28_mul_ite]
          by_cases hc : S.epar u.1 v.1 = 1 ∧ xf u * xf v = 1
          · rw [if_pos hc, if_pos ⟨hlt, hc⟩]
          · rw [if_neg hc, if_neg fun h => hc h.2]
        · rw [if_neg hlt, if_neg fun h => hlt h.1]
    _ = ∑ p : {i : ℕ // i ∈ S.freeSet} × {i : ℕ // i ∈ S.freeSet},
          if p.1.1 < p.2.1 ∧ S.epar p.1.1 p.2.1 = 1 ∧ xf p.1 * xf p.2 = 1
          then (1 : ZMod 8) else 0 :=
        (Fintype.sum_prod_type' _).symm
    _ = ((Finset.univ.filter fun p : {i : ℕ // i ∈ S.freeSet} × {i : ℕ // i ∈ S.freeSet} =>
          p.1.1 < p.2.1 ∧ S.epar p.1.1 p.2.1 = 1 ∧ xf p.1 * xf p.2 = 1).card : ZMod 8) :=
        Finset.sum_boole _ _
    _ = ((S.pinnedInstance hsym z).selCount xf : ZMod 8) := by
        congr 1
        rw [SopInstance.selCount]
        refine Finset.card_bij (fun p _ => s(p.1, p.2)) ?_ ?_ ?_
        · intro p hp
          obtain ⟨-, hlt, hepar, hprod⟩ := Finset.mem_filter.mp hp
          refine Finset.mem_filter.mpr ⟨?_, hprod⟩
          rw [SimpleGraph.mem_edgeFinset, SimpleGraph.mem_edgeSet]
          exact ⟨fun h => absurd (congrArg Subtype.val h) (Nat.ne_of_lt hlt), hepar⟩
        · intro p hp q hq heq
          obtain ⟨-, hp1, -, -⟩ := Finset.mem_filter.mp hp
          obtain ⟨-, hq1, -, -⟩ := Finset.mem_filter.mp hq
          rcases Sym2.eq_iff.mp heq with ⟨h1, h2⟩ | ⟨h1, h2⟩
          · exact Prod.ext h1 h2
          · exfalso
            rw [h1, h2] at hp1
            omega
        · intro e
          induction e using Sym2.ind with
          | _ u v =>
            intro he
            obtain ⟨hmem, hprod⟩ := Finset.mem_filter.mp he
            rw [SimpleGraph.mem_edgeFinset, SimpleGraph.mem_edgeSet] at hmem
            obtain ⟨hne, hepar⟩ := hmem
            have hprod' : xf u * xf v = 1 := hprod
            have hne' : u.1 ≠ v.1 := fun h => hne (Subtype.ext h)
            rcases Nat.lt_or_ge u.1 v.1 with hlt | hge
            · exact ⟨(u, v),
                Finset.mem_filter.mpr ⟨Finset.mem_univ _, hlt, hepar, hprod'⟩, rfl⟩
            · have hlt' : v.1 < u.1 := by omega
              refine ⟨(v, u), Finset.mem_filter.mpr ⟨Finset.mem_univ _, hlt', ?_, ?_⟩, ?_⟩
              · rw [hsym v.1 u.1]; exact hepar
              · rw [mul_comm]; exact hprod'
              · exact Sym2.eq_swap

/-- The two mixed blocks together: edges from a free variable into the live boundary. -/
private theorem pin_mixed (S : Sym n) (hw : S.WFS) (hsym : S.EparSym) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    ((∑ i ∈ S.freeSet, ∑ j ∈ S.liveSet, if i < j then lift28 (S.epar i j)
          * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j)
          else 0)
        + ∑ i ∈ S.liveSet, ∑ j ∈ S.freeSet, if i < j then lift28 (S.epar i j)
          * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j)
          else 0)
      = ∑ u : {i : ℕ // i ∈ S.freeSet},
          (∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j)) * lift28 (xf u) := by
  rw [Finset.sum_comm (s := S.liveSet) (t := S.freeSet), ← Finset.sum_add_distrib]
  have hpt : ∀ i ∈ S.freeSet,
      ((∑ j ∈ S.liveSet, if i < j then lift28 (S.epar i j)
          * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j)
          else 0)
        + ∑ j ∈ S.liveSet, if j < i then lift28 (S.epar j i)
          * lift28 (S.ext (S.mergeAssign z xf) j) * lift28 (S.ext (S.mergeAssign z xf) i)
          else 0)
      = (∑ j ∈ S.liveSet, lift28 (S.epar i j) * lift28 (S.pinVal z j))
          * lift28 (S.ext (S.mergeAssign z xf) i) := by
    intro i hi
    rw [← Finset.sum_add_distrib, Finset.sum_mul]
    refine Finset.sum_congr rfl fun j hj => ?_
    have hnl : i ∉ S.liveSet := Finset.disjoint_left.mp (disjoint_free_live S) hi
    have hij : i ≠ j := fun h => hnl (by rwa [h])
    rw [merge_live S hw z xf hj]
    rcases Nat.lt_or_ge i j with hlt | hge
    · rw [if_pos hlt, if_neg (by omega), add_zero]
      ring
    · have hlt' : j < i := by omega
      rw [if_neg (by omega), if_pos hlt', zero_add, hsym j i]
  exact (Finset.sum_congr rfl hpt).trans (sum_free_attach S z xf fun i e =>
    (∑ j ∈ S.liveSet, lift28 (S.epar i j) * lift28 (S.pinVal z j)) * lift28 e)

/-- The quadratic part of the compiled phase, on a merged assignment, in three blocks. -/
private theorem pin_quad (S : Sym n) (hw : S.WFS) (hsym : S.EparSym) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    (∑ i ∈ Finset.range S.next, ∑ j ∈ Finset.range S.next, if i < j then lift28 (S.epar i j)
        * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j) else 0)
      = ((S.pinnedInstance hsym z).selCount xf : ZMod 8)
        + (∑ u : {i : ℕ // i ∈ S.freeSet},
            (∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j)) * lift28 (xf u))
        + (∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet,
            if i < j then lift28 (S.epar i j) * lift28 (S.pinVal z i) * lift28 (S.pinVal z j)
            else 0) := by
  have hll : (∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet, if i < j then lift28 (S.epar i j)
        * lift28 (S.ext (S.mergeAssign z xf) i) * lift28 (S.ext (S.mergeAssign z xf) j) else 0)
      = ∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet,
          if i < j then lift28 (S.epar i j) * lift28 (S.pinVal z i) * lift28 (S.pinVal z j)
          else 0 :=
    Finset.sum_congr rfl fun i hi => Finset.sum_congr rfl fun j hj => by
      rw [merge_live S hw z xf hi, merge_live S hw z xf hj]
  rw [← free_union_live S hw, sum_union_double S.freeSet S.liveSet (disjoint_free_live S),
    pin_freefree S hsym z xf, hll]
  linear_combination pin_mixed S hw hsym z xf

/-- Evaluation of the pinned instance's polynomial, with the unary sum split into the
compiled unary part and the folded live edges. -/
private theorem pinned_f_eval (S : Sym n) (hsym : S.EparSym) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    (S.c + (∑ i ∈ S.liveSet, S.b i * lift28 (S.pinVal z i))
          + 4 * (∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet,
              if i < j then lift28 (S.epar i j) * lift28 (S.pinVal z i) * lift28 (S.pinVal z j)
              else 0))
        + ((∑ u : {i : ℕ // i ∈ S.freeSet}, S.b u.1 * lift28 (xf u))
            + 4 * ∑ u : {i : ℕ // i ∈ S.freeSet},
                (∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j))
                  * lift28 (xf u))
        + 4 * ((S.pinnedInstance hsym z).selCount xf : ZMod 8)
      = (S.pinnedInstance hsym z).f xf := by
  have hbsum : (∑ u : {i : ℕ // i ∈ S.freeSet},
        (S.b u.1 + 4 * ∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j))
          * ((xf u).val : ZMod 8))
      = (∑ u : {i : ℕ // i ∈ S.freeSet}, S.b u.1 * lift28 (xf u))
        + 4 * ∑ u : {i : ℕ // i ∈ S.freeSet},
            (∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j))
              * lift28 (xf u) := by
    rw [Finset.mul_sum, ← Finset.sum_add_distrib]
    refine Finset.sum_congr rfl fun u _ => ?_
    simp only [lift28]
    ring
  have hbridge : S.c + (∑ i ∈ S.liveSet, S.b i * lift28 (S.pinVal z i))
        + 4 * (∑ i ∈ S.liveSet, ∑ j ∈ S.liveSet,
            if i < j then lift28 (S.epar i j) * lift28 (S.pinVal z i) * lift28 (S.pinVal z j)
            else 0)
        + (∑ u : {i : ℕ // i ∈ S.freeSet},
            (S.b u.1 + 4 * ∑ j ∈ S.liveSet, lift28 (S.epar u.1 j) * lift28 (S.pinVal z j))
              * ((xf u).val : ZMod 8))
        + ((8 / 2 : ℕ) : ZMod 8) * ((S.pinnedInstance hsym z).selCount xf : ZMod 8)
      = (S.pinnedInstance hsym z).f xf := rfl
  refine Eq.trans ?_ hbridge
  rw [show (8 / 2 : ℕ) = 4 from rfl]
  linear_combination -hbsum

/-- **The phase-substitution identity**: on a merged assignment, the compiled phase equals
the pinned instance's polynomial. -/
theorem phase_merge (S : Sym n) (hw : S.WFS) (hinj : S.CurInj) (hsym : S.EparSym)
    (hirr : S.EparIrrefl) (z : Fin n → ZMod 2)
    (xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2) :
    S.phase (S.mergeAssign z xf) = (S.pinnedInstance hsym z).f xf := by
  have _ := hinj
  have _ := hirr
  show S.c + (∑ i ∈ Finset.range S.next, S.b i * lift28 (S.ext (S.mergeAssign z xf) i))
      + 4 * (∑ i ∈ Finset.range S.next, ∑ j ∈ Finset.range S.next, if i < j then
          lift28 (S.epar i j) * lift28 (S.ext (S.mergeAssign z xf) i)
            * lift28 (S.ext (S.mergeAssign z xf) j) else 0)
    = (S.pinnedInstance hsym z).f xf
  rw [pin_bsum S hw z xf, pin_quad S hw hsym z xf, ← pinned_f_eval S hsym z xf]
  ring

/-! ## D. Assembly -/

theorem pathSum_pinned (S : Sym n) (hw : S.WFS) (hinj : S.CurInj) (hsym : S.EparSym)
    (hirr : S.EparIrrefl) (y z : Fin n → ZMod 2) :
    S.pathSum y z = S.constDelta y z * (S.pinnedInstance hsym z).S := by
  unfold pathSum constDelta
  by_cases hconst : ∀ a, S.cur a = none → y a = z a
  · rw [if_pos hconst, one_mul]
    calc (∑ x : Fin S.next → ZMod 2,
            (if ∀ a, S.wireVal y x a = z a then (1 : ℂ) else 0) * omega8 ^ (S.phase x).val)
        = ∑ x : Fin S.next → ZMod 2,
            if ∀ i ∈ S.liveSet, S.ext x i = S.pinVal z i
            then omega8 ^ (S.phase x).val else 0 :=
          Finset.sum_congr rfl fun x _ => by
            rw [ite_mul, one_mul, zero_mul]
            exact if_congr ((indicator_iff S hinj y z x).trans (and_iff_right hconst))
              rfl rfl
      _ = ∑ xf : {i : ℕ // i ∈ S.freeSet} → ZMod 2,
            omega8 ^ (S.phase (S.mergeAssign z xf)).val :=
          sum_pinned_reindex S hw z fun x => omega8 ^ (S.phase x).val
      _ = (S.pinnedInstance hsym z).S := by
          unfold SopInstance.S
          rw [omega_pinned S hsym z]
          exact Finset.sum_congr rfl fun xf _ =>
            congrArg (fun t : ZMod 8 => omega8 ^ t.val)
              (phase_merge S hw hinj hsym hirr z xf)
  · rw [if_neg hconst, zero_mul]
    refine Finset.sum_eq_zero fun x _ => ?_
    have hna : ¬∀ a, S.wireVal y x a = z a := fun hall =>
      hconst ((indicator_iff S hinj y z x).mp hall).1
    rw [if_neg hna, zero_mul]

theorem pinningHolds (n : ℕ) : PinningHolds n :=
  fun S hw hinj hsym hirr y z => pathSum_pinned S hw hinj hsym hirr y z

end Sym
end Quantum
end Formal
