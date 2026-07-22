/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.Symbolic

/-!
# Local algebra of the symbolic path-sum compiler

Everything the compile-invariant induction (`Formal.Quantum.Coupling`) needs about a single
`Sym.step`:

* well-formedness `Sym.WFS` (wire symbols mention only live variable names) and its
  preservation by `Sym.step` and `Sym.compile`;
* `rfl`/simp equations for the bookkeeping fields of `Sym.addC`, `Sym.addB`, `Sym.flipEdge`
  and of `Sym.step` on each gate shape;
* the phase updates `Sym.phase_addC`, `Sym.phase_addB`, `Sym.phase_flipEdge`;
* transport of assignment sums along `Fin.snoc` for the fresh Hadamard variable
  (`Sym.sum_extend`, `Sym.step_H_ext_snoc_lt`, `Sym.step_H_ext_snoc_last`);
* the path sum of the initial state (`Sym.pathSum_init`).
-/

open scoped BigOperators

namespace Formal
namespace Quantum
namespace Sym

variable {n : ℕ}

/-! ## The primitive updates change only their own field -/

@[simp] theorem addC_next (S : Sym n) (t : ZMod 8) : (S.addC t).next = S.next := rfl

@[simp] theorem addC_cur (S : Sym n) (t : ZMod 8) : (S.addC t).cur = S.cur := rfl

@[simp] theorem addC_c (S : Sym n) (t : ZMod 8) : (S.addC t).c = S.c + t := rfl

@[simp] theorem addC_b (S : Sym n) (t : ZMod 8) : (S.addC t).b = S.b := rfl

@[simp] theorem addC_epar (S : Sym n) (t : ZMod 8) : (S.addC t).epar = S.epar := rfl

@[simp] theorem addC_mH (S : Sym n) (t : ZMod 8) : (S.addC t).mH = S.mH := rfl

@[simp] theorem addC_ext (S : Sym n) (t : ZMod 8) : (S.addC t).ext = S.ext := rfl

@[simp] theorem addC_wireVal (S : Sym n) (t : ZMod 8) : (S.addC t).wireVal = S.wireVal := rfl

@[simp] theorem addB_next (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).next = S.next := rfl

@[simp] theorem addB_cur (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).cur = S.cur := rfl

@[simp] theorem addB_c (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).c = S.c := rfl

theorem addB_b (S : Sym n) (i : ℕ) (t : ZMod 8) :
    (S.addB i t).b = fun p => if p = i then S.b p + t else S.b p := rfl

@[simp] theorem addB_epar (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).epar = S.epar := rfl

@[simp] theorem addB_mH (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).mH = S.mH := rfl

@[simp] theorem addB_ext (S : Sym n) (i : ℕ) (t : ZMod 8) : (S.addB i t).ext = S.ext := rfl

@[simp] theorem addB_wireVal (S : Sym n) (i : ℕ) (t : ZMod 8) :
    (S.addB i t).wireVal = S.wireVal := rfl

@[simp] theorem flipEdge_next (S : Sym n) (i j : ℕ) : (S.flipEdge i j).next = S.next := rfl

@[simp] theorem flipEdge_cur (S : Sym n) (i j : ℕ) : (S.flipEdge i j).cur = S.cur := rfl

@[simp] theorem flipEdge_c (S : Sym n) (i j : ℕ) : (S.flipEdge i j).c = S.c := rfl

@[simp] theorem flipEdge_b (S : Sym n) (i j : ℕ) : (S.flipEdge i j).b = S.b := rfl

theorem flipEdge_epar (S : Sym n) (i j : ℕ) :
    (S.flipEdge i j).epar = fun p q =>
      if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then S.epar p q + 1 else S.epar p q := rfl

@[simp] theorem flipEdge_mH (S : Sym n) (i j : ℕ) : (S.flipEdge i j).mH = S.mH := rfl

@[simp] theorem flipEdge_ext (S : Sym n) (i j : ℕ) : (S.flipEdge i j).ext = S.ext := rfl

@[simp] theorem flipEdge_wireVal (S : Sym n) (i j : ℕ) :
    (S.flipEdge i j).wireVal = S.wireVal := rfl

/-! ## Bookkeeping fields of `step`, per gate shape -/

@[simp] theorem step_next_H (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.H a)).next = S.next + 1 := rfl

@[simp] theorem step_mH_H (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.H a)).mH = S.mH + 1 := rfl

@[simp] theorem step_cur_H (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.H a)).cur = fun w => if w = a then some S.next else S.cur w := rfl

@[simp] theorem step_next_T (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.T a)).next = S.next := by
  cases hc : S.cur a <;> simp [step, hc]

@[simp] theorem step_mH_T (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.T a)).mH = S.mH := by
  cases hc : S.cur a <;> simp [step, hc]

@[simp] theorem step_cur_T (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) :
    (step y S (Gate.T a)).cur = S.cur := by
  cases hc : S.cur a <;> simp [step, hc]

@[simp] theorem step_next_CZ (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) :
    (step y S (Gate.CZ a b)).next = S.next := by
  cases hca : S.cur a <;> cases hcb : S.cur b <;>
    simp [step, hca, hcb, apply_ite Sym.next]

@[simp] theorem step_mH_CZ (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) :
    (step y S (Gate.CZ a b)).mH = S.mH := by
  cases hca : S.cur a <;> cases hcb : S.cur b <;>
    simp [step, hca, hcb, apply_ite Sym.mH]

@[simp] theorem step_cur_CZ (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) :
    (step y S (Gate.CZ a b)).cur = S.cur := by
  cases hca : S.cur a <;> cases hcb : S.cur b <;>
    simp [step, hca, hcb, apply_ite Sym.cur]

/-! ## `step` written as a primitive update, per gate shape and wire status -/

theorem step_T_none (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) (h : S.cur a = none) :
    step y S (Gate.T a) = S.addC (lift28 (y a)) := by
  simp [step, h]

theorem step_T_some (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) (i : ℕ)
    (h : S.cur a = some i) : step y S (Gate.T a) = S.addB i 1 := by
  simp [step, h]

theorem step_CZ_none_none (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n)
    (ha : S.cur a = none) (hb : S.cur b = none) :
    step y S (Gate.CZ a b) = S.addC (4 * lift28 (y a) * lift28 (y b)) := by
  simp [step, ha, hb]

theorem step_CZ_none_some (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) (j : ℕ)
    (ha : S.cur a = none) (hb : S.cur b = some j) :
    step y S (Gate.CZ a b) = S.addB j (4 * lift28 (y a)) := by
  simp [step, ha, hb]

theorem step_CZ_some_none (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) (i : ℕ)
    (ha : S.cur a = some i) (hb : S.cur b = none) :
    step y S (Gate.CZ a b) = S.addB i (4 * lift28 (y b)) := by
  simp [step, ha, hb]

theorem step_CZ_some_some_eq (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) (i : ℕ)
    (ha : S.cur a = some i) (hb : S.cur b = some i) :
    step y S (Gate.CZ a b) = S.addB i 4 := by
  simp [step, ha, hb]

theorem step_CZ_some_some_ne (y : Fin n → ZMod 2) (S : Sym n) (a b : Fin n) (i j : ℕ)
    (ha : S.cur a = some i) (hb : S.cur b = some j) (hij : i ≠ j) :
    step y S (Gate.CZ a b) = S.flipEdge i j := by
  simp [step, ha, hb, hij]

theorem step_H_none (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) (h : S.cur a = none) :
    step y S (Gate.H a) =
      { S.addB S.next (4 * lift28 (y a)) with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } := by
  simp [step, h]

theorem step_H_some (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n) (i : ℕ)
    (h : S.cur a = some i) :
    step y S (Gate.H a) =
      { S.flipEdge i S.next with
        next := S.next + 1
        cur := fun w => if w = a then some S.next else S.cur w
        mH := S.mH + 1 } := by
  simp [step, h]

/-! ## Well-formedness -/

/-- Well-formedness of a symbolic state: every wire symbol is a live variable name. -/
def WFS (S : Sym n) : Prop := ∀ a i, S.cur a = some i → i < S.next

theorem wfs_init (n : ℕ) : (init n).WFS := by
  intro a i h
  simp [init] at h

theorem wfs_step (y : Fin n → ZMod 2) (S : Sym n) (g : Gate n) (h : S.WFS) :
    (step y S g).WFS := by
  cases g with
  | H a =>
      intro w i hw
      simp only [step_cur_H] at hw
      simp only [step_next_H]
      by_cases hwa : w = a
      · rw [if_pos hwa] at hw
        injection hw with hw
        omega
      · rw [if_neg hwa] at hw
        exact Nat.lt_succ_of_lt (h w i hw)
  | T a =>
      intro w i hw
      simp only [step_cur_T] at hw
      simp only [step_next_T]
      exact h w i hw
  | CZ a b =>
      intro w i hw
      simp only [step_cur_CZ] at hw
      simp only [step_next_CZ]
      exact h w i hw

theorem wfs_foldl (y : Fin n → ZMod 2) (C : Circuit n) (S : Sym n) (h : S.WFS) :
    (C.foldl (step y) S).WFS := by
  induction C generalizing S with
  | nil => exact h
  | cons g C ih =>
      simp only [List.foldl_cons]
      exact ih (step y S g) (wfs_step y S g h)

theorem wfs_compile (y : Fin n → ZMod 2) (C : Circuit n) : (compile y C).WFS :=
  wfs_foldl y C (init n) (wfs_init n)

/-! ## Phase under the primitive updates -/

theorem lift28_succ (e : ZMod 2) : lift28 (e + 1) * 4 = lift28 e * 4 + 4 := by
  revert e; decide

theorem phase_addC (S : Sym n) (t : ZMod 8) (x : Fin S.next → ZMod 2) :
    (S.addC t).phase x = S.phase x + t := by
  simp only [phase, addC_c, addC_b, addC_epar, addC_next, addC_ext]
  ring

theorem phase_addB (S : Sym n) (i : ℕ) (hi : i < S.next) (t : ZMod 8)
    (x : Fin S.next → ZMod 2) :
    (S.addB i t).phase x = S.phase x + t * lift28 (S.ext x i) := by
  have hpt : ∀ p, (if p = i then S.b p + t else S.b p) * lift28 (S.ext x p)
      = S.b p * lift28 (S.ext x p) + (if p = i then t * lift28 (S.ext x p) else 0) := by
    intro p
    by_cases hp : p = i
    · simp only [if_pos hp]; ring
    · simp only [if_neg hp]; ring
  have hsum : (∑ p ∈ Finset.range S.next,
        (if p = i then S.b p + t else S.b p) * lift28 (S.ext x p))
      = (∑ p ∈ Finset.range S.next, S.b p * lift28 (S.ext x p))
        + t * lift28 (S.ext x i) := by
    rw [Finset.sum_congr rfl (fun p _ => hpt p), Finset.sum_add_distrib,
      Finset.sum_ite_eq', if_pos (Finset.mem_range.mpr hi)]
  simp only [phase, addB_c, addB_b, addB_epar, addB_next, addB_ext]
  linear_combination hsum

/-- Collapse a double sum whose summand vanishes away from the single cell `(i, j)`. -/
theorem sum_sum_ite_single {M : Type*} [AddCommMonoid M] {N i j : ℕ} (hi : i < N)
    (hj : j < N) (G : ℕ → ℕ → M) :
    (∑ p ∈ Finset.range N, ∑ q ∈ Finset.range N, if p = i ∧ q = j then G p q else 0)
      = G i j := by
  have h1 : ∀ p ∈ Finset.range N,
      (∑ q ∈ Finset.range N, if p = i ∧ q = j then G p q else 0)
        = if p = i then G p j else 0 := by
    intro p _
    by_cases hp : p = i
    · subst hp
      simp [Finset.mem_range.mpr hj]
    · simp [hp]
  rw [Finset.sum_congr rfl h1]
  simp [Finset.mem_range.mpr hi]

/-- Collapse a double sum whose summand vanishes away from the two cells `(i, j)`, `(j, i)`. -/
theorem sum_sum_ite_pair {M : Type*} [AddCommMonoid M] {N i j : ℕ} (hi : i < N) (hj : j < N)
    (hij : i ≠ j) (G : ℕ → ℕ → M) :
    (∑ p ∈ Finset.range N, ∑ q ∈ Finset.range N,
        if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then G p q else 0)
      = G i j + G j i := by
  have hsplit : ∀ p q, (if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then G p q else 0)
      = (if p = i ∧ q = j then G p q else 0) + (if p = j ∧ q = i then G p q else 0) := by
    intro p q
    by_cases h1 : p = i ∧ q = j
    · simp [h1, hij]
    · by_cases h2 : p = j ∧ q = i
      · simp [h2, hij, Ne.symm hij]
      · simp [h1, h2]
  rw [Finset.sum_congr rfl (fun p _ => Finset.sum_congr rfl (fun q _ => hsplit p q))]
  simp only [Finset.sum_add_distrib]
  rw [sum_sum_ite_single hi hj, sum_sum_ite_single hj hi]

theorem phase_flipEdge (S : Sym n) (i j : ℕ) (hi : i < S.next) (hj : j < S.next)
    (hij : i ≠ j) (x : Fin S.next → ZMod 2) :
    (S.flipEdge i j).phase x
      = S.phase x + 4 * lift28 (S.ext x i) * lift28 (S.ext x j) := by
  have hpt : ∀ p q : ℕ,
      (4 : ZMod 8) * (if p < q then
          lift28 (if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then S.epar p q + 1 else S.epar p q)
            * lift28 (S.ext x p) * lift28 (S.ext x q)
        else 0)
      = 4 * (if p < q then lift28 (S.epar p q) * lift28 (S.ext x p) * lift28 (S.ext x q)
          else 0)
        + (if (p = i ∧ q = j) ∨ (p = j ∧ q = i) then
            (if p < q then 4 * lift28 (S.ext x p) * lift28 (S.ext x q) else 0) else 0) := by
    intro p q
    by_cases hc : (p = i ∧ q = j) ∨ (p = j ∧ q = i)
    · by_cases hpq : p < q
      · simp only [if_pos hc, if_pos hpq]
        linear_combination lift28 (S.ext x p) * lift28 (S.ext x q) * lift28_succ (S.epar p q)
      · simp [hc, hpq]
    · simp [hc]
  simp only [phase, flipEdge_c, flipEdge_b, flipEdge_epar, flipEdge_next, flipEdge_ext]
  simp only [Finset.mul_sum]
  rw [Finset.sum_congr rfl (fun p _ => Finset.sum_congr rfl (fun q _ => hpt p q))]
  simp only [Finset.sum_add_distrib]
  rw [sum_sum_ite_pair hi hj hij]
  rcases lt_or_gt_of_ne hij with hlt | hlt
  · rw [if_pos hlt, if_neg (lt_asymm hlt)]
    ring
  · rw [if_neg (lt_asymm hlt), if_pos hlt]
    ring

/-! ## Extending assignments for a fresh Hadamard variable -/

theorem sum_extend {M : Type} [AddCommMonoid M] (N : ℕ) (F : (Fin (N + 1) → ZMod 2) → M) :
    (∑ x' : Fin (N + 1) → ZMod 2, F x')
      = ∑ x : Fin N → ZMod 2, ∑ ε : ZMod 2, F (Fin.snoc x ε) := by
  have hbij : Function.Bijective
      (fun p : (Fin N → ZMod 2) × ZMod 2 => (Fin.snoc p.1 p.2 : Fin (N + 1) → ZMod 2)) := by
    constructor
    · rintro ⟨x, ε⟩ ⟨x', ε'⟩ hpp
      obtain ⟨h1, h2⟩ := Fin.snoc_inj.mp hpp
      simp only [Prod.mk.injEq]
      exact ⟨h1, h2⟩
    · intro x'
      exact ⟨(Fin.init x', x' (Fin.last N)), Fin.snoc_init_self x'⟩
  rw [← Fintype.sum_prod_type']
  exact (Fintype.sum_bijective _ hbij (fun p => F (Fin.snoc p.1 p.2)) F (fun _ => rfl)).symm

theorem step_H_ext_snoc_lt (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n)
    (x : Fin S.next → ZMod 2) (ε : ZMod 2) (m : ℕ) (hm : m < S.next) :
    (step y S (Gate.H a)).ext (Fin.snoc x ε) m = S.ext x m := by
  have hm1 : m < S.next + 1 := Nat.lt_succ_of_lt hm
  show (if h : m < S.next + 1 then (Fin.snoc x ε : Fin (S.next + 1) → ZMod 2) ⟨m, h⟩ else 0)
      = if h : m < S.next then x ⟨m, h⟩ else 0
  rw [dif_pos hm1, dif_pos hm]
  have hcast : (⟨m, hm1⟩ : Fin (S.next + 1)) = Fin.castSucc ⟨m, hm⟩ := rfl
  rw [hcast, Fin.snoc_castSucc]

theorem step_H_ext_snoc_last (y : Fin n → ZMod 2) (S : Sym n) (a : Fin n)
    (x : Fin S.next → ZMod 2) (ε : ZMod 2) :
    (step y S (Gate.H a)).ext (Fin.snoc x ε) S.next = ε := by
  have hlt : S.next < S.next + 1 := Nat.lt_succ_self S.next
  show (if h : S.next < S.next + 1 then
        (Fin.snoc x ε : Fin (S.next + 1) → ZMod 2) ⟨S.next, h⟩ else 0)
      = ε
  rw [dif_pos hlt]
  have hcast : (⟨S.next, hlt⟩ : Fin (S.next + 1)) = Fin.last S.next := rfl
  rw [hcast, Fin.snoc_last]

/-! ## The path sum of the initial state -/

theorem pathSum_init (n : ℕ) (y w : Fin n → ZMod 2) :
    (init n).pathSum y w = if y = w then 1 else 0 := by
  have key : ∀ x : Fin 0 → ZMod 2,
      (if ∀ a, (init n).wireVal y x a = w a then (1 : ℂ) else 0)
        * omega8 ^ ((init n).phase x).val
      = if y = w then 1 else 0 := by
    intro x
    have hph : (init n).phase x = 0 := by
      simp [phase, init]
    rw [hph]
    simp only [ZMod.val_zero, pow_zero, mul_one]
    have hwv : ∀ a, (init n).wireVal y x a = y a := fun _ => rfl
    by_cases hyw : y = w
    · rw [if_pos hyw, if_pos]
      intro a
      rw [hwv a, hyw]
    · rw [if_neg hyw, if_neg]
      intro hall
      exact hyw (funext fun a => ((hwv a).symm.trans (hall a)))
  show (∑ x : Fin 0 → ZMod 2,
      (if ∀ a, (init n).wireVal y x a = w a then (1 : ℂ) else 0)
        * omega8 ^ ((init n).phase x).val)
    = if y = w then 1 else 0
  rw [Fintype.sum_unique]
  exact key _

end Sym
end Quantum
end Formal
