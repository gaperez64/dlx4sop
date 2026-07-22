/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.SymLemmas

/-!
# The compile invariant: circuit-matrix entries are symbolic path sums

The paper's path-sum derivation (sec:sop), proved by induction over the gate list.
The per-gate content is isolated in the three `pathSum_step_*` lemmas, stated purely on
the symbolic side (the Hadamard normalization appears only in the final assembly):

* `T a`  multiplies each pinned path sum by `ω₈^{bit (w a)}`;
* `CZ a b` multiplies it by `ω₈^{4·bit (w a)·bit (w b)}`;
* `H a`  sums over the two values of the wire before the Hadamard, with the quadratic kernel.

`compile_invariant` then follows by `List.reverseRecOn` from `pathSum_init`.
-/

open scoped BigOperators

namespace Formal
namespace Quantum

/-! ## Unfolding equations for snoc -/

theorem circuitMatrix_snoc {n : ℕ} (C : Circuit n) (g : Gate n) :
    circuitMatrix (C ++ [g]) = gateMatrix g * circuitMatrix C := by
  unfold circuitMatrix
  rw [List.foldl_append]
  rfl

theorem compile_snoc {n : ℕ} (y : Fin n → ZMod 2) (C : Circuit n) (g : Gate n) :
    Sym.compile y (C ++ [g]) = Sym.step y (Sym.compile y C) g := by
  unfold Sym.compile
  rw [List.foldl_append]
  rfl

/-! ## Powers of `ω₈` -/

theorem omega8_pow_8 : omega8 ^ (8 : ℕ) = 1 := by
  have h8 : ((8 : ℕ) : ℂ) ≠ 0 := by norm_num
  have hmul : ((8 : ℕ) : ℂ) * (2 * Real.pi * Complex.I / 8) = 2 * Real.pi * Complex.I := by
    push_cast
    field_simp
  rw [omega8, ← Complex.exp_nat_mul, hmul, Complex.exp_two_pi_mul_I]

theorem omega8_pow_mod (m : ℕ) : omega8 ^ (m % 8) = omega8 ^ m := by
  conv_rhs => rw [← Nat.div_add_mod m 8]
  rw [pow_add, pow_mul, omega8_pow_8, one_pow, one_mul]

theorem omega8_pow_val_add (s t : ZMod 8) :
    omega8 ^ (s + t).val = omega8 ^ s.val * omega8 ^ t.val := by
  rw [ZMod.val_add, omega8_pow_mod, pow_add]

theorem sqrt2C_ne_zero : ((Real.sqrt 2 : ℝ) : ℂ) ≠ 0 := by
  exact_mod_cast ne_of_gt (Real.sqrt_pos.mpr (by norm_num : (0:ℝ) < 2))

/-! ## Value bridges and wire-value unfoldings for the per-gate lemmas -/

private theorem lift28_val (u : ZMod 2) : (Sym.lift28 u).val = bit u := by
  revert u; decide

private theorem lift28_val4 (u v : ZMod 2) :
    ((4 : ZMod 8) * Sym.lift28 u * Sym.lift28 v).val = 4 * bit u * bit v := by
  revert u v; decide

private theorem lift28_val4_swap (u v : ZMod 2) :
    ((4 : ZMod 8) * Sym.lift28 u * Sym.lift28 v).val = 4 * bit v * bit u := by
  revert u v; decide

private theorem lift28_val4_diag (u : ZMod 2) :
    ((4 : ZMod 8) * Sym.lift28 u).val = 4 * bit u * bit u := by
  revert u; decide

private theorem wireVal_none {n : ℕ} (S : Sym n) (y : Fin n → ZMod 2)
    (x : Fin S.next → ZMod 2) (a : Fin n) (h : S.cur a = none) :
    S.wireVal y x a = y a := by
  simp only [Sym.wireVal, h]

private theorem wireVal_some {n : ℕ} (S : Sym n) (y : Fin n → ZMod 2)
    (x : Fin S.next → ZMod 2) (a : Fin n) (i : ℕ) (h : S.cur a = some i) :
    S.wireVal y x a = S.ext x i := by
  simp only [Sym.wireVal, h]

/-! ## Per-gate path-sum recurrences (the heart) -/

theorem pathSum_step_T {n : ℕ} (S : Sym n) (hw : S.WFS) (y w : Fin n → ZMod 2) (a : Fin n) :
    (Sym.step y S (Gate.T a)).pathSum y w
      = omega8 ^ bit (w a) * S.pathSum y w := by
  cases hc : S.cur a with
  | none =>
      rw [Sym.step_T_none y S a hc]
      unfold Sym.pathSum
      simp only [Sym.addC_next, Sym.addC_wireVal]
      rw [Finset.mul_sum]
      refine Finset.sum_congr rfl fun x _ => ?_
      by_cases h : ∀ a', S.wireVal y x a' = w a'
      · have hval : y a = w a := (wireVal_none S y x a hc).symm.trans (h a)
        rw [Sym.phase_addC, omega8_pow_val_add, hval, lift28_val]
        simp only [if_pos h]
        ring
      · simp only [if_neg h]
        ring
  | some i =>
      have hi : i < S.next := hw a i hc
      rw [Sym.step_T_some y S a i hc]
      unfold Sym.pathSum
      simp only [Sym.addB_next, Sym.addB_wireVal]
      rw [Finset.mul_sum]
      refine Finset.sum_congr rfl fun x _ => ?_
      by_cases h : ∀ a', S.wireVal y x a' = w a'
      · have hval : S.ext x i = w a := (wireVal_some S y x a i hc).symm.trans (h a)
        rw [Sym.phase_addB S i hi, omega8_pow_val_add, one_mul, hval, lift28_val]
        simp only [if_pos h]
        ring
      · simp only [if_neg h]
        ring

theorem pathSum_step_CZ {n : ℕ} (S : Sym n) (hw : S.WFS) (y w : Fin n → ZMod 2) (a b : Fin n) :
    (Sym.step y S (Gate.CZ a b)).pathSum y w
      = omega8 ^ (4 * bit (w a) * bit (w b)) * S.pathSum y w := by
  cases hca : S.cur a with
  | none =>
      cases hcb : S.cur b with
      | none =>
          rw [Sym.step_CZ_none_none y S a b hca hcb]
          unfold Sym.pathSum
          simp only [Sym.addC_next, Sym.addC_wireVal]
          rw [Finset.mul_sum]
          refine Finset.sum_congr rfl fun x _ => ?_
          by_cases h : ∀ a', S.wireVal y x a' = w a'
          · have hva : y a = w a := (wireVal_none S y x a hca).symm.trans (h a)
            have hvb : y b = w b := (wireVal_none S y x b hcb).symm.trans (h b)
            rw [Sym.phase_addC, omega8_pow_val_add, hva, hvb, lift28_val4]
            simp only [if_pos h]
            ring
          · simp only [if_neg h]
            ring
      | some j =>
          have hj : j < S.next := hw b j hcb
          rw [Sym.step_CZ_none_some y S a b j hca hcb]
          unfold Sym.pathSum
          simp only [Sym.addB_next, Sym.addB_wireVal]
          rw [Finset.mul_sum]
          refine Finset.sum_congr rfl fun x _ => ?_
          by_cases h : ∀ a', S.wireVal y x a' = w a'
          · have hva : y a = w a := (wireVal_none S y x a hca).symm.trans (h a)
            have hvb : S.ext x j = w b := (wireVal_some S y x b j hcb).symm.trans (h b)
            rw [Sym.phase_addB S j hj, omega8_pow_val_add, hva, hvb, lift28_val4]
            simp only [if_pos h]
            ring
          · simp only [if_neg h]
            ring
  | some i =>
      have hi : i < S.next := hw a i hca
      cases hcb : S.cur b with
      | none =>
          rw [Sym.step_CZ_some_none y S a b i hca hcb]
          unfold Sym.pathSum
          simp only [Sym.addB_next, Sym.addB_wireVal]
          rw [Finset.mul_sum]
          refine Finset.sum_congr rfl fun x _ => ?_
          by_cases h : ∀ a', S.wireVal y x a' = w a'
          · have hva : S.ext x i = w a := (wireVal_some S y x a i hca).symm.trans (h a)
            have hvb : y b = w b := (wireVal_none S y x b hcb).symm.trans (h b)
            rw [Sym.phase_addB S i hi, omega8_pow_val_add, hvb, hva, lift28_val4_swap]
            simp only [if_pos h]
            ring
          · simp only [if_neg h]
            ring
      | some j =>
          have hj : j < S.next := hw b j hcb
          by_cases hij : i = j
          · have hca' : S.cur a = some j := hij ▸ hca
            rw [Sym.step_CZ_some_some_eq y S a b j hca' hcb]
            unfold Sym.pathSum
            simp only [Sym.addB_next, Sym.addB_wireVal]
            rw [Finset.mul_sum]
            refine Finset.sum_congr rfl fun x _ => ?_
            by_cases h : ∀ a', S.wireVal y x a' = w a'
            · have hva : S.ext x j = w a := (wireVal_some S y x a j hca').symm.trans (h a)
              have hvb : S.ext x j = w b := (wireVal_some S y x b j hcb).symm.trans (h b)
              have hab : w b = w a := hvb.symm.trans hva
              rw [Sym.phase_addB S j hj, omega8_pow_val_add, hva, lift28_val4_diag, hab]
              simp only [if_pos h]
              ring
            · simp only [if_neg h]
              ring
          · rw [Sym.step_CZ_some_some_ne y S a b i j hca hcb hij]
            unfold Sym.pathSum
            simp only [Sym.flipEdge_next, Sym.flipEdge_wireVal]
            rw [Finset.mul_sum]
            refine Finset.sum_congr rfl fun x _ => ?_
            by_cases h : ∀ a', S.wireVal y x a' = w a'
            · have hva : S.ext x i = w a := (wireVal_some S y x a i hca).symm.trans (h a)
              have hvb : S.ext x j = w b := (wireVal_some S y x b j hcb).symm.trans (h b)
              rw [Sym.phase_flipEdge S i j hi hj hij, omega8_pow_val_add, hva, hvb, lift28_val4]
              simp only [if_pos h]
              ring
            · simp only [if_neg h]
              ring

/-! ## Data-boundedness (an invariant forced by the formalization)

`phase` sums over `range next`, and a Hadamard grows `next` by one, so the fresh index reads
`b`/`epar` at that name. The compile invariant is therefore *false* unless `b`, `epar` carry
nothing at or above `next` (verified: without it, `n=1`, one `H`, gives `ω₈ ≠ 1`). This holds
for `init` and is preserved by `step`. -/

/-- The phase data vanish at names `≥ next`. -/
def Sym.DataBounded {n : ℕ} (S : Sym n) : Prop :=
  (∀ m, S.next ≤ m → S.b m = 0) ∧ (∀ p q, S.next ≤ p ∨ S.next ≤ q → S.epar p q = 0)

theorem dataBounded_init (n : ℕ) : (Sym.init n).DataBounded :=
  ⟨fun _ _ => rfl, fun _ _ _ => rfl⟩

theorem dataBounded_step {n : ℕ} (y : Fin n → ZMod 2) (S : Sym n) (g : Gate n)
    (hb : S.DataBounded) (hw : S.WFS) : (Sym.step y S g).DataBounded := by
  obtain ⟨hbB, hbE⟩ := hb
  cases g with
  | T a =>
      cases hc : S.cur a with
      | none =>
          rw [Sym.step_T_none y S a hc]
          exact ⟨fun m hm => by rw [Sym.addC_b]; exact hbB m hm,
                 fun p q hpq => by rw [Sym.addC_epar]; exact hbE p q hpq⟩
      | some i =>
          have hi : i < S.next := hw a i hc
          rw [Sym.step_T_some y S a i hc]
          refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
          · simp only [Sym.addB_next] at hm
            rw [Sym.addB_b]; simp only [if_neg (show m ≠ i by omega)]; exact hbB m hm
          · simp only [Sym.addB_next] at hpq
            rw [Sym.addB_epar]; exact hbE p q hpq
  | CZ a b =>
      cases hca : S.cur a with
      | none =>
          cases hcb : S.cur b with
          | none =>
              rw [Sym.step_CZ_none_none y S a b hca hcb]
              exact ⟨fun m hm => by rw [Sym.addC_b]; exact hbB m hm,
                     fun p q hpq => by rw [Sym.addC_epar]; exact hbE p q hpq⟩
          | some j =>
              have hj : j < S.next := hw b j hcb
              rw [Sym.step_CZ_none_some y S a b j hca hcb]
              refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
              · simp only [Sym.addB_next] at hm
                rw [Sym.addB_b]; simp only [if_neg (show m ≠ j by omega)]; exact hbB m hm
              · simp only [Sym.addB_next] at hpq
                rw [Sym.addB_epar]; exact hbE p q hpq
      | some i =>
          have hi : i < S.next := hw a i hca
          cases hcb : S.cur b with
          | none =>
              rw [Sym.step_CZ_some_none y S a b i hca hcb]
              refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
              · simp only [Sym.addB_next] at hm
                rw [Sym.addB_b]; simp only [if_neg (show m ≠ i by omega)]; exact hbB m hm
              · simp only [Sym.addB_next] at hpq
                rw [Sym.addB_epar]; exact hbE p q hpq
          | some j =>
              have hj : j < S.next := hw b j hcb
              by_cases hij : i = j
              · have hcb' : S.cur b = some i := by rw [hij]; exact hcb
                rw [Sym.step_CZ_some_some_eq y S a b i hca hcb']
                refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
                · simp only [Sym.addB_next] at hm
                  rw [Sym.addB_b]; simp only [if_neg (show m ≠ i by omega)]; exact hbB m hm
                · simp only [Sym.addB_next] at hpq
                  rw [Sym.addB_epar]; exact hbE p q hpq
              · rw [Sym.step_CZ_some_some_ne y S a b i j hca hcb hij]
                refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
                · simp only [Sym.flipEdge_next] at hm
                  rw [Sym.flipEdge_b]; exact hbB m hm
                · simp only [Sym.flipEdge_next] at hpq
                  rw [Sym.flipEdge_epar]
                  have hf : ¬((p = i ∧ q = j) ∨ (p = j ∧ q = i)) := by
                    rintro (⟨rfl, rfl⟩ | ⟨rfl, rfl⟩) <;> omega
                  simp only [if_neg hf]; exact hbE p q hpq
  | H a =>
      cases hc : S.cur a with
      | none =>
          rw [Sym.step_H_none y S a hc]
          refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
          · have hm' : S.next + 1 ≤ m := hm
            show (S.addB S.next (4 * Sym.lift28 (y a))).b m = 0
            rw [Sym.addB_b]; simp only [if_neg (show m ≠ S.next by omega)]
            exact hbB m (by omega)
          · have hpq' : S.next + 1 ≤ p ∨ S.next + 1 ≤ q := hpq
            show (S.addB S.next (4 * Sym.lift28 (y a))).epar p q = 0
            rw [Sym.addB_epar]; exact hbE p q (by omega)
      | some i =>
          have hi : i < S.next := hw a i hc
          rw [Sym.step_H_some y S a i hc]
          refine ⟨fun m hm => ?_, fun p q hpq => ?_⟩
          · have hm' : S.next + 1 ≤ m := hm
            show (S.flipEdge i S.next).b m = 0
            rw [Sym.flipEdge_b]; exact hbB m (by omega)
          · have hpq' : S.next + 1 ≤ p ∨ S.next + 1 ≤ q := hpq
            show (S.flipEdge i S.next).epar p q = 0
            rw [Sym.flipEdge_epar]
            have hf : ¬((p = i ∧ q = S.next) ∨ (p = S.next ∧ q = i)) := by
              rintro (⟨rfl, rfl⟩ | ⟨rfl, rfl⟩) <;> omega
            simp only [if_neg hf]; exact hbE p q (by omega)

theorem dataBounded_compile {n : ℕ} (y : Fin n → ZMod 2) (C : Circuit n) :
    (Sym.compile y C).DataBounded := by
  have key : ∀ (C : Circuit n) (S : Sym n),
      S.WFS → S.DataBounded →
        (C.foldl (Sym.step y) S).WFS ∧ (C.foldl (Sym.step y) S).DataBounded := by
    intro C
    induction C with
    | nil => intro S hw hd; exact ⟨hw, hd⟩
    | cons g C ih =>
        intro S hw hd
        simp only [List.foldl_cons]
        exact ih (Sym.step y S g) (Sym.wfs_step y S g hw) (dataBounded_step y S g hd hw)
  unfold Sym.compile
  exact (key C (Sym.init n) (Sym.wfs_init n) (dataBounded_init n)).2

namespace Sym

/-- The fresh-Hadamard record reads the old symbol at names below `next`. -/
private theorem H_rec_ext_lt {n : ℕ} (S base : Sym n) (a : Fin n)
    (x : Fin S.next → ZMod 2) (ν : ZMod 2) (m : ℕ) (hm : m < S.next) :
    ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).ext (Fin.snoc x ν) m = S.ext x m := by
  have hm1 : m < S.next + 1 := Nat.lt_succ_of_lt hm
  show (if h : m < S.next + 1 then (Fin.snoc x ν : Fin (S.next + 1) → ZMod 2) ⟨m, h⟩ else 0)
      = if h : m < S.next then x ⟨m, h⟩ else 0
  rw [dif_pos hm1, dif_pos hm]
  have hcast : (⟨m, hm1⟩ : Fin (S.next + 1)) = Fin.castSucc ⟨m, hm⟩ := rfl
  rw [hcast, Fin.snoc_castSucc]

/-- The fresh-Hadamard record reads the new variable `ν` at name `next`. -/
private theorem H_rec_ext_last {n : ℕ} (S base : Sym n) (a : Fin n)
    (x : Fin S.next → ZMod 2) (ν : ZMod 2) :
    ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).ext (Fin.snoc x ν) S.next = ν := by
  have hlt : S.next < S.next + 1 := Nat.lt_succ_self S.next
  show (if h : S.next < S.next + 1 then
        (Fin.snoc x ν : Fin (S.next + 1) → ZMod 2) ⟨S.next, h⟩ else 0) = ν
  rw [dif_pos hlt]
  have hcast : (⟨S.next, hlt⟩ : Fin (S.next + 1)) = Fin.last S.next := rfl
  rw [hcast, Fin.snoc_last]

set_option maxHeartbeats 1000000 in
/-- Phase of the fresh-Hadamard record (`cur a = none`): a fresh unary term `4·(y a)·ν`. -/
private theorem phase_H_snoc_none {n : ℕ} (S : Sym n) (hb : S.DataBounded)
    (y : Fin n → ZMod 2) (a : Fin n) (x : Fin S.next → ZMod 2) (ν : ZMod 2) :
    ({ S.addB S.next (4 * lift28 (y a)) with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } : Sym n).phase (Fin.snoc x ν)
      = S.phase x + 4 * lift28 (y a) * lift28 ν := by
  obtain ⟨hbB, hbE⟩ := hb
  set R : Sym n := { S.addB S.next (4 * lift28 (y a)) with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } with hR
  have hRc : R.c = S.c := rfl
  have hRepar : R.epar = S.epar := rfl
  have hext_lt : ∀ m, m < S.next → R.ext (Fin.snoc x ν) m = S.ext x m :=
    fun m hm => H_rec_ext_lt S (S.addB S.next (4 * lift28 (y a))) a x ν m hm
  have hext_last : R.ext (Fin.snoc x ν) S.next = ν :=
    H_rec_ext_last S (S.addB S.next (4 * lift28 (y a))) a x ν
  have hbLt : ∀ m, m < S.next → R.b m = S.b m := by
    intro m hm
    show (if m = S.next then S.b m + 4 * lift28 (y a) else S.b m) = S.b m
    rw [if_neg (Nat.ne_of_lt hm)]
  have hbSnext : R.b S.next = 4 * lift28 (y a) := by
    show (if S.next = S.next then S.b S.next + 4 * lift28 (y a) else S.b S.next) = 4 * lift28 (y a)
    rw [if_pos rfl, hbB S.next (le_refl _), zero_add]
  have hsum1 : (∑ i ∈ Finset.range (S.next + 1), R.b i * lift28 (R.ext (Fin.snoc x ν) i))
      = (∑ i ∈ Finset.range S.next, S.b i * lift28 (S.ext x i))
        + 4 * lift28 (y a) * lift28 ν := by
    rw [Finset.sum_range_succ]
    congr 1
    · apply Finset.sum_congr rfl
      intro m hm; rw [Finset.mem_range] at hm; rw [hbLt m hm, hext_lt m hm]
    · rw [hbSnext, hext_last]
  have hsum2 : (∑ p ∈ Finset.range (S.next + 1), ∑ q ∈ Finset.range (S.next + 1),
        if p < q then lift28 (R.epar p q) * lift28 (R.ext (Fin.snoc x ν) p)
          * lift28 (R.ext (Fin.snoc x ν) q) else 0)
      = ∑ p ∈ Finset.range S.next, ∑ q ∈ Finset.range S.next,
        if p < q then lift28 (S.epar p q) * lift28 (S.ext x p) * lift28 (S.ext x q) else 0 := by
    rw [hRepar, Finset.sum_range_succ]
    rw [Finset.sum_eq_zero (s := Finset.range (S.next + 1))
      (f := fun q => if S.next < q then lift28 (S.epar S.next q)
        * lift28 (R.ext (Fin.snoc x ν) S.next) * lift28 (R.ext (Fin.snoc x ν) q) else 0)
      (fun q hq => by rw [Finset.mem_range] at hq; rw [if_neg (by omega)])]
    rw [add_zero]
    apply Finset.sum_congr rfl
    intro p hp; rw [Finset.mem_range] at hp
    rw [Finset.sum_range_succ]
    rw [show (if p < S.next then lift28 (S.epar p S.next)
        * lift28 (R.ext (Fin.snoc x ν) p) * lift28 (R.ext (Fin.snoc x ν) S.next) else 0) = 0 by
      rw [if_pos hp, hbE p S.next (Or.inr (le_refl _))]; simp [lift28]]
    rw [add_zero]
    apply Finset.sum_congr rfl
    intro q hq; rw [Finset.mem_range] at hq
    rw [hext_lt p hp, hext_lt q hq]
  show R.c + (∑ i ∈ Finset.range (S.next + 1), R.b i * lift28 (R.ext (Fin.snoc x ν) i))
      + 4 * (∑ p ∈ Finset.range (S.next + 1), ∑ q ∈ Finset.range (S.next + 1),
          if p < q then lift28 (R.epar p q) * lift28 (R.ext (Fin.snoc x ν) p)
            * lift28 (R.ext (Fin.snoc x ν) q) else 0) = _
  rw [hRc, hsum1, hsum2]
  unfold Sym.phase
  ring

set_option maxHeartbeats 1000000 in
/-- Phase of the fresh-Hadamard record (`cur a = some i`): a fresh sign edge `4·x_i·ν`. -/
private theorem phase_H_snoc_some {n : ℕ} (S : Sym n) (hb : S.DataBounded)
    (a : Fin n) (i : ℕ) (hi : i < S.next) (x : Fin S.next → ZMod 2) (ν : ZMod 2) :
    ({ S.flipEdge i S.next with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } : Sym n).phase (Fin.snoc x ν)
      = S.phase x + 4 * lift28 (S.ext x i) * lift28 ν := by
  obtain ⟨hbB, hbE⟩ := hb
  set R : Sym n := { S.flipEdge i S.next with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } with hR
  have hRc : R.c = S.c := rfl
  have hRb : R.b = S.b := rfl
  have hext_lt : ∀ m, m < S.next → R.ext (Fin.snoc x ν) m = S.ext x m :=
    fun m hm => H_rec_ext_lt S (S.flipEdge i S.next) a x ν m hm
  have hext_last : R.ext (Fin.snoc x ν) S.next = ν :=
    H_rec_ext_last S (S.flipEdge i S.next) a x ν
  have hReparLt : ∀ p q, p < S.next → q < S.next → R.epar p q = S.epar p q := by
    intro p q hp hq
    show (if (p = i ∧ q = S.next) ∨ (p = S.next ∧ q = i) then S.epar p q + 1 else S.epar p q)
        = S.epar p q
    rw [if_neg (by omega)]
  have hReparP : ∀ p, p < S.next → R.epar p S.next = if p = i then (1 : ZMod 2) else 0 := by
    intro p hp
    show (if (p = i ∧ S.next = S.next) ∨ (p = S.next ∧ S.next = i) then S.epar p S.next + 1
        else S.epar p S.next) = if p = i then 1 else 0
    rw [hbE p S.next (Or.inr (le_refl _))]
    by_cases hpi : p = i
    · simp [hpi]
    · have hpn : ¬ (p = S.next) := by omega
      simp [hpi, hpn]
  have hsum1 : (∑ i' ∈ Finset.range (S.next + 1), R.b i' * lift28 (R.ext (Fin.snoc x ν) i'))
      = ∑ i' ∈ Finset.range S.next, S.b i' * lift28 (S.ext x i') := by
    rw [hRb, Finset.sum_range_succ]
    rw [hbB S.next (le_refl _), zero_mul, add_zero]
    apply Finset.sum_congr rfl
    intro m hm; rw [Finset.mem_range] at hm; rw [hext_lt m hm]
  have hsum2 : (∑ p ∈ Finset.range (S.next + 1), ∑ q ∈ Finset.range (S.next + 1),
        if p < q then lift28 (R.epar p q) * lift28 (R.ext (Fin.snoc x ν) p)
          * lift28 (R.ext (Fin.snoc x ν) q) else 0)
      = (∑ p ∈ Finset.range S.next, ∑ q ∈ Finset.range S.next,
          if p < q then lift28 (S.epar p q) * lift28 (S.ext x p) * lift28 (S.ext x q) else 0)
        + lift28 (S.ext x i) * lift28 ν := by
    rw [Finset.sum_range_succ]
    rw [Finset.sum_eq_zero (s := Finset.range (S.next + 1))
      (f := fun q => if S.next < q then lift28 (R.epar S.next q)
        * lift28 (R.ext (Fin.snoc x ν) S.next) * lift28 (R.ext (Fin.snoc x ν) q) else 0)
      (fun q hq => by rw [Finset.mem_range] at hq; rw [if_neg (by omega)])]
    rw [add_zero]
    have hinner : ∀ p ∈ Finset.range S.next,
        (∑ q ∈ Finset.range (S.next + 1),
          if p < q then lift28 (R.epar p q) * lift28 (R.ext (Fin.snoc x ν) p)
            * lift28 (R.ext (Fin.snoc x ν) q) else 0)
        = (∑ q ∈ Finset.range S.next,
            if p < q then lift28 (S.epar p q) * lift28 (S.ext x p) * lift28 (S.ext x q) else 0)
          + lift28 (if p = i then (1 : ZMod 2) else 0) * lift28 (S.ext x p) * lift28 ν := by
      intro p hp; rw [Finset.mem_range] at hp
      rw [Finset.sum_range_succ]
      congr 1
      · apply Finset.sum_congr rfl
        intro q hq; rw [Finset.mem_range] at hq
        rw [hReparLt p q hp hq, hext_lt p hp, hext_lt q hq]
      · rw [if_pos hp, hReparP p hp, hext_lt p hp, hext_last]
    rw [Finset.sum_congr rfl hinner, Finset.sum_add_distrib]
    congr 1
    rw [Finset.sum_congr rfl (fun p _ => by
      rw [show lift28 (if p = i then (1 : ZMod 2) else 0) = if p = i then (1 : ZMod 8) else 0 by
        split <;> decide])]
    simp only [ite_mul, one_mul, zero_mul, Finset.sum_ite_eq', Finset.mem_range, hi, if_true]
  show R.c + (∑ i' ∈ Finset.range (S.next + 1), R.b i' * lift28 (R.ext (Fin.snoc x ν) i'))
      + 4 * (∑ p ∈ Finset.range (S.next + 1), ∑ q ∈ Finset.range (S.next + 1),
          if p < q then lift28 (R.epar p q) * lift28 (R.ext (Fin.snoc x ν) p)
            * lift28 (R.ext (Fin.snoc x ν) q) else 0) = _
  rw [hRc, hsum1, hsum2]
  unfold Sym.phase
  ring

set_option maxHeartbeats 1000000 in
/-- The path-sum recurrence for the fresh-Hadamard record, given its phase equation. -/
private theorem pathSum_H_assemble {n : ℕ} (S base : Sym n)
    (y w : Fin n → ZMod 2) (a : Fin n) (hw : S.WFS)
    (hphase : ∀ (x : Fin S.next → ZMod 2) (ν : ZMod 2),
      ({ base with
          next := S.next + 1
          cur := fun v => if v = a then some S.next else S.cur v
          mH := S.mH + 1 } : Sym n).phase (Fin.snoc x ν)
        = S.phase x + 4 * lift28 (S.wireVal y x a) * lift28 ν) :
    ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).pathSum y w
      = ∑ ε : ZMod 2,
          omega8 ^ (4 * bit (w a) * bit ε) * S.pathSum y (Function.update w a ε) := by
  have hcurA : ({ base with
      next := S.next + 1
      cur := fun v => if v = a then some S.next else S.cur v
      mH := S.mH + 1 } : Sym n).cur a = some S.next := by
    show (if a = a then some S.next else S.cur a) = _; rw [if_pos rfl]
  have hwvA : ∀ (x : Fin S.next → ZMod 2) (ν : ZMod 2),
      ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).wireVal y (Fin.snoc x ν) a = ν := by
    intro x ν
    rw [wireVal_some _ y (Fin.snoc x ν) a S.next hcurA, H_rec_ext_last]
  have hwvNe : ∀ (x : Fin S.next → ZMod 2) (ν : ZMod 2) b, b ≠ a →
      ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).wireVal y (Fin.snoc x ν) b = S.wireVal y x b := by
    intro x ν b hba
    have hcurb : ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).cur b = S.cur b := by
      show (if b = a then some S.next else S.cur b) = S.cur b; rw [if_neg hba]
    cases hsb : S.cur b with
    | none => rw [wireVal_none _ y (Fin.snoc x ν) b (hcurb.trans hsb), wireVal_none S y x b hsb]
    | some j =>
        have hj : j < S.next := hw b j hsb
        rw [wireVal_some _ y (Fin.snoc x ν) b j (hcurb.trans hsb), H_rec_ext_lt S base a x ν j hj,
          wireVal_some S y x b j hsb]
  unfold Sym.pathSum
  rw [sum_extend S.next]
  simp only [Finset.mul_sum]
  conv_rhs => rw [Finset.sum_comm]
  apply Finset.sum_congr rfl
  intro x _
  have hiffL : ∀ ν : ZMod 2,
      (∀ b, ({ base with
        next := S.next + 1
        cur := fun v => if v = a then some S.next else S.cur v
        mH := S.mH + 1 } : Sym n).wireVal y (Fin.snoc x ν) b = w b)
      ↔ (ν = w a ∧ ∀ b, b ≠ a → S.wireVal y x b = w b) := by
    intro ν; constructor
    · intro h
      exact ⟨(hwvA x ν).symm.trans (h a), fun b hb => by rw [← hwvNe x ν b hb]; exact h b⟩
    · rintro ⟨hν, hQ⟩ b; by_cases hba : b = a
      · subst hba; rw [hwvA x ν]; exact hν
      · rw [hwvNe x ν b hba]; exact hQ b hba
  have hiffR : ∀ ε : ZMod 2, (∀ b, S.wireVal y x b = Function.update w a ε b)
      ↔ (S.wireVal y x a = ε ∧ ∀ b, b ≠ a → S.wireVal y x b = w b) := by
    intro ε; constructor
    · intro h
      exact ⟨by have := h a; rwa [Function.update_self] at this,
        fun b hb => by have := h b; rwa [Function.update_of_ne hb] at this⟩
    · rintro ⟨ha, hQ⟩ b; by_cases hba : b = a
      · subst hba; rw [Function.update_self]; exact ha
      · rw [Function.update_of_ne hba]; exact hQ b hba
  trans (∑ ν : ZMod 2,
      (if (ν = w a ∧ ∀ b, b ≠ a → S.wireVal y x b = w b) then (1 : ℂ) else 0)
      * omega8 ^ (S.phase x + 4 * lift28 (S.wireVal y x a) * lift28 ν).val)
  · apply Finset.sum_congr rfl; intro ν _
    rw [hphase x ν]; congr 1; exact if_congr (hiffL ν) rfl rfl
  · by_cases hQ : (∀ b, b ≠ a → S.wireVal y x b = w b)
    · have hiffR' : ∀ ε : ZMod 2,
          (∀ b, S.wireVal y x b = Function.update w a ε b) ↔ (S.wireVal y x a = ε) :=
        fun ε => (hiffR ε).trans (and_iff_left hQ)
      rw [show (∑ ν : ZMod 2,
            (if (ν = w a ∧ ∀ b, b ≠ a → S.wireVal y x b = w b) then (1 : ℂ) else 0)
            * omega8 ^ (S.phase x + 4 * lift28 (S.wireVal y x a) * lift28 ν).val)
          = omega8 ^ (S.phase x + 4 * lift28 (S.wireVal y x a) * lift28 (w a)).val from by
        simp only [and_iff_left hQ, ite_mul, one_mul, zero_mul, Finset.sum_ite_eq',
          Finset.mem_univ, if_true]]
      rw [show (∑ ε : ZMod 2, omega8 ^ (4 * bit (w a) * bit ε)
            * ((if ∀ b, S.wireVal y x b = Function.update w a ε b then (1 : ℂ) else 0)
              * omega8 ^ (S.phase x).val))
          = omega8 ^ (4 * bit (w a) * bit (S.wireVal y x a)) * omega8 ^ (S.phase x).val from by
        rw [Finset.sum_congr rfl (fun ε (_ : ε ∈ Finset.univ) =>
          (by rw [if_congr (hiffR' ε) rfl rfl]; ring :
            omega8 ^ (4 * bit (w a) * bit ε)
              * ((if ∀ b, S.wireVal y x b = Function.update w a ε b then (1 : ℂ) else 0)
                * omega8 ^ (S.phase x).val)
            = (if S.wireVal y x a = ε then (1 : ℂ) else 0)
              * (omega8 ^ (4 * bit (w a) * bit ε) * omega8 ^ (S.phase x).val)))]
        simp only [ite_mul, one_mul, zero_mul, Finset.sum_ite_eq, Finset.mem_univ, if_true]]
      rw [omega8_pow_val_add, lift28_val4_swap]; ring
    · rw [show (∑ ν : ZMod 2,
            (if (ν = w a ∧ ∀ b, b ≠ a → S.wireVal y x b = w b) then (1 : ℂ) else 0)
            * omega8 ^ (S.phase x + 4 * lift28 (S.wireVal y x a) * lift28 ν).val) = 0 from by
        apply Finset.sum_eq_zero; intro ν _; rw [if_neg (fun h => hQ h.2), zero_mul]]
      rw [show (∑ ε : ZMod 2, omega8 ^ (4 * bit (w a) * bit ε)
            * ((if ∀ b, S.wireVal y x b = Function.update w a ε b then (1 : ℂ) else 0)
              * omega8 ^ (S.phase x).val)) = 0 from by
        apply Finset.sum_eq_zero; intro ε _
        rw [if_neg (fun h => hQ ((hiffR ε).mp h).2), zero_mul, mul_zero]]

end Sym

theorem pathSum_step_H {n : ℕ} (S : Sym n) (hw : S.WFS) (hb : S.DataBounded)
    (y w : Fin n → ZMod 2) (a : Fin n) :
    (Sym.step y S (Gate.H a)).pathSum y w
      = ∑ ε : ZMod 2,
          omega8 ^ (4 * bit (w a) * bit ε) * S.pathSum y (Function.update w a ε) := by
  cases hc : S.cur a with
  | none =>
      rw [Sym.step_H_none y S a hc]
      exact Sym.pathSum_H_assemble S (S.addB S.next (4 * Sym.lift28 (y a))) y w a hw
        (fun x ν => by rw [wireVal_none S y x a hc]; exact Sym.phase_H_snoc_none S hb y a x ν)
  | some i =>
      have hi : i < S.next := hw a i hc
      rw [Sym.step_H_some y S a i hc]
      exact Sym.pathSum_H_assemble S (S.flipEdge i S.next) y w a hw
        (fun x ν => by
          rw [wireVal_some S y x a i hc]; exact Sym.phase_H_snoc_some S hb a i hi x ν)

/-! ## The invariant -/

/-- Reindex a sum weighted by an "agree with `w` off wire `a`" indicator against the two values
of wire `a`, transported by `v ↦ Function.update w a (v a)`. -/
private theorem sum_agree_off_reindex {n : ℕ} (a : Fin n) (w : Fin n → ZMod 2)
    (F : (Fin n → ZMod 2) → ℂ) :
    (∑ v : Fin n → ZMod 2, if (∀ i, i ≠ a → w i = v i) then F v else 0)
      = ∑ ε : ZMod 2, F (Function.update w a ε) := by
  have hupd : ∀ v : Fin n → ZMod 2, (∀ i, i ≠ a → w i = v i) →
      Function.update w a (v a) = v := by
    intro v hv
    funext i
    by_cases hia : i = a
    · subst hia; rw [Function.update_self]
    · rw [Function.update_of_ne hia]; exact hv i hia
  rw [← Finset.sum_filter]
  refine Finset.sum_nbij' (fun v => v a) (Function.update w a)
    (fun v _ => Finset.mem_univ _) (fun ε _ => ?_) (fun v hv => ?_) (fun ε _ => ?_)
    (fun v hv => ?_)
  · exact Finset.mem_filter.mpr ⟨Finset.mem_univ _, fun i hi => by
      simp only [Function.update_of_ne hi]⟩
  · simp only [hupd v (Finset.mem_filter.mp hv).2]
  · simp only [Function.update_self]
  · simp only [hupd v (Finset.mem_filter.mp hv).2]

/-- **The compile invariant** (sec:sop): every circuit-matrix entry is the normalized
symbolic path sum of the compiled state. -/
theorem compile_invariant {n : ℕ} (C : Circuit n) (y w : Fin n → ZMod 2) :
    circuitMatrix C w y
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ (Sym.compile y C).mH)⁻¹ * (Sym.compile y C).pathSum y w := by
  induction C using List.reverseRecOn generalizing w with
  | nil =>
      have hc : circuitMatrix ([] : Circuit n) w y = if w = y then (1 : ℂ) else 0 := by
        rw [show circuitMatrix ([] : Circuit n) = 1 from rfl, Matrix.one_apply]
      rw [hc, show Sym.compile y ([] : Circuit n) = Sym.init n from rfl, Sym.pathSum_init]
      change (if w = y then (1 : ℂ) else 0)
          = (((Real.sqrt 2 : ℝ) : ℂ) ^ (0 : ℕ))⁻¹ * (if y = w then 1 else 0)
      rw [pow_zero, inv_one, one_mul]
      exact if_congr eq_comm rfl rfl
  | append_singleton C g ih =>
      rw [circuitMatrix_snoc, compile_snoc, Matrix.mul_apply]
      simp_rw [ih]
      cases g with
      | T a =>
          simp only [gateMatrix, ite_mul, zero_mul, Finset.sum_ite_eq, Finset.mem_univ, if_true]
          rw [pathSum_step_T (Sym.compile y C) (Sym.wfs_compile y C) y w a, Sym.step_mH_T]
          ring
      | CZ a b =>
          simp only [gateMatrix, ite_mul, zero_mul, Finset.sum_ite_eq, Finset.mem_univ, if_true]
          rw [pathSum_step_CZ (Sym.compile y C) (Sym.wfs_compile y C) y w a b, Sym.step_mH_CZ]
          ring
      | H a =>
          rw [pathSum_step_H (Sym.compile y C) (Sym.wfs_compile y C) (dataBounded_compile y C)
              y w a, Sym.step_mH_H, pow_succ, mul_inv, Finset.mul_sum]
          simp only [gateMatrix, ite_mul, zero_mul]
          rw [sum_agree_off_reindex a w]
          refine Finset.sum_congr rfl fun ε _ => ?_
          simp only [Function.update_self]
          ring

theorem compileInvariant_holds (n : ℕ) : CompileInvariant n :=
  fun C y w => compile_invariant C y w

end Quantum
end Formal
