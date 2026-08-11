/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Stab.QPF

/-!
# Affine `F₂`-forms inside quadratic phase functions

This file supplies the three facts about *affine forms* `μ(x) = e + ∑_j a_j x_j` that carry
all of the work in the closure lemma `lem:qpf-closure`:

* `isQPF_affineIndicator` — the indicator `[μ(x) = 0]` is a QPF (one constraint row);
* `isQPF_affineProdSign` — the sign `(−1)^{μ(x) ν(x)}` of a *product* of two affine forms is a
  QPF (a product of affine forms is quadratic, and over `F₂` the linear part folds into the
  diagonal of the coefficient matrix since `x² = x`);
* `isQPF_affinePhase` — **the crux**, the paper's `eq:parity-lift`: the power
  `i^{c · μ(x)}` of an affine form is a QPF for every `c : ZMod 4`.

The last one is the only place where `eq:xor-lift` (`lift4_add`) is genuinely used: the `0/1`
lift of an `F₂`-sum is *not* additive, and the induction over the support of `a` pays for each
extra term with one quadratic correction, which `zeta4_two_mul` converts into a sign — and
that sign is exactly an instance of `isQPF_affineProdSign`.

Assembling these gives `isQPF_onePlusIPow`, the closed form of the four scalar sums
`1 + i^{c}(−1)^{μ(x)}` that appear when a single variable is summed out
(`lem:qpf-closure`(iii)); `Formal.Stab.Closure` does that assembly.
-/

open scoped BigOperators Matrix

namespace Formal
namespace Stab

variable {ι : Type} [Fintype ι]

/-! ## Elementary character values -/

/-- `(−1)^1 = −1`. -/
@[simp] theorem sgn_one : sgn (1 : ZMod 2) = -1 := by
  have h : (1 : ZMod 2).val = 1 := rfl
  rw [sgn, h, pow_one]

/-- `i^1 = i`. -/
@[simp] theorem zeta4_one : zeta4 (1 : ZMod 4) = Complex.I := by
  have h : (1 : ZMod 4).val = 1 := rfl
  rw [zeta4, h, pow_one]

/-- `i^2 = −1`. -/
@[simp] theorem zeta4_two : zeta4 (2 : ZMod 4) = -1 := by
  have h : (2 : ZMod 4).val = 2 := rfl
  rw [zeta4, h, Complex.I_sq]

/-- `i^3 = −i`. -/
@[simp] theorem zeta4_three : zeta4 (3 : ZMod 4) = -Complex.I := by
  have h : (3 : ZMod 4).val = 3 := rfl
  rw [zeta4, h]
  have : Complex.I ^ (3 : ℕ) = Complex.I ^ (2 : ℕ) * Complex.I := by ring
  rw [this, Complex.I_sq]
  ring

/-- The sign character, case split. -/
theorem sgn_eq_ite (b : ZMod 2) : sgn b = if b = 0 then (1 : ℂ) else -1 := by
  have key : ∀ b : ZMod 2, b = 0 ∨ b = 1 := by decide
  rcases key b with rfl | rfl
  · rw [if_pos rfl, sgn_zero]
  · rw [if_neg (by decide : ¬ (1 : ZMod 2) = 0), sgn_one]

/-- The `0/1` lift is multiplicative (unlike additive — see `lift4_add`). -/
theorem lift4_mul (x y : ZMod 2) : lift4 (x * y) = lift4 x * lift4 y := by
  revert x y
  decide

/-- Reduction mod `2` of the `ZMod 4`-product `c · lift p · lift q`.  This is the arithmetic
identity behind the quadratic correction term of `eq:xor-lift`. -/
theorem cast_val_mul_lift4 (c : ZMod 4) (p q : ZMod 2) :
    (((c * lift4 p * lift4 q).val : ℕ) : ZMod 2) = ((c.val : ℕ) : ZMod 2) * p * q := by
  revert c p q
  decide

/-! ## Affine `F₂`-forms -/

/-- An affine `F₂`-form `μ(x) = e + ∑_j a_j x_j`. -/
def affine (e : ZMod 2) (a : ι → ZMod 2) (x : ι → ZMod 2) : ZMod 2 := e + ∑ j, a j * x j

/-- A partial sum `e + ∑_{j ∈ S} a_j x_j` is the affine form of the coefficients extended by
zero outside `S`. -/
theorem affine_restrict [DecidableEq ι] (S : Finset ι) (e : ZMod 2) (a : ι → ZMod 2)
    (x : ι → ZMod 2) :
    affine e (fun j => if j ∈ S then a j else 0) x = e + ∑ j ∈ S, a j * x j := by
  simp only [affine, ite_mul, zero_mul]
  congr 1
  rw [Finset.sum_ite_mem, Finset.univ_inter]

/-- A single-coordinate form `C · x_{j₀}` is affine. -/
theorem affine_single [DecidableEq ι] (C : ZMod 2) (j₀ : ι) (x : ι → ZMod 2) :
    affine 0 (fun j => if j = j₀ then C else 0) x = C * x j₀ := by
  simp only [affine, ite_mul, zero_mul, Finset.sum_ite_eq', Finset.mem_univ, if_true, zero_add]

/-! ## `lem:qpf-closure`: indicators of affine constraints -/

/-- Data presenting the indicator `[μ(x) = 0]` of an affine form: one constraint row. -/
def affineIndicator (e : ZMod 2) (a : ι → ZMod 2) : QPF ι where
  eps := 1
  lin := 0
  quad := 0
  K := Unit
  M := Matrix.of fun _ j => a j
  d := fun _ => e

/-- The single constraint row of `affineIndicator` says exactly `μ(x) = 0`. -/
theorem sat_affineIndicator_iff (e : ZMod 2) (a : ι → ZMod 2) (x : ι → ZMod 2) :
    (affineIndicator e a).Sat x ↔ affine e a x = 0 := by
  have key : ∀ e y : ZMod 2, (e + y = 0) ↔ (y = e) := by decide
  constructor
  · intro h
    have h1 : ∑ j, a j * x j = e := congrFun h ()
    change e + ∑ j, a j * x j = 0
    exact (key e _).2 h1
  · intro h
    have h1 : e + ∑ j, a j * x j = 0 := h
    funext _
    exact (key e _).1 h1

/-- `affineIndicator` evaluates to the indicator of `μ(x) = 0`. -/
theorem eval_affineIndicator (e : ZMod 2) (a : ι → ZMod 2) (x : ι → ZMod 2) :
    (affineIndicator e a).eval x = if affine e a x = 0 then (1 : ℂ) else 0 := by
  by_cases h : affine e a x = 0
  · rw [QPF.eval_of_sat ((sat_affineIndicator_iff e a x).2 h), if_pos h]
    simp [affineIndicator, QPF.linVal, QPF.quadVal]
  · rw [QPF.eval_of_not_sat (fun hc => h ((sat_affineIndicator_iff e a x).1 hc)), if_neg h]

/-- **`lem:qpf-closure`, affine constraints.**  The indicator of a single affine equation is a
QPF. -/
theorem isQPF_affineIndicator (e : ZMod 2) (a : ι → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => if affine e a x = 0 then (1 : ℂ) else 0) :=
  ⟨affineIndicator e a, fun x => eval_affineIndicator e a x⟩

/-- Data presenting the indicator of an entire affine system `N x = c`. -/
def affineSystem {κ : Type} (N : Matrix κ ι (ZMod 2)) (c : κ → ZMod 2) : QPF ι where
  eps := 1
  lin := 0
  quad := 0
  K := κ
  M := N
  d := c

/-- **`lem:qpf-closure`, affine constraints (system form).**  The indicator of an affine system
`N x = c` is a QPF. -/
theorem isQPF_affineSystem {κ : Type} [Fintype κ] (N : Matrix κ ι (ZMod 2)) (c : κ → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => if N.mulVec x = c then (1 : ℂ) else 0) := by
  refine ⟨affineSystem N c, fun x => ?_⟩
  change (affineSystem N c).eval x = if N.mulVec x = c then (1 : ℂ) else 0
  by_cases h : N.mulVec x = c
  · rw [QPF.eval_of_sat (show (affineSystem N c).Sat x from h), if_pos h]
    simp [affineSystem, QPF.linVal, QPF.quadVal]
  · rw [QPF.eval_of_not_sat (show ¬ (affineSystem N c).Sat x from h), if_neg h]

/-! ## Products of affine forms are quadratic -/

/-- **Expansion of a product of affine forms.**  Over `F₂` we have `x² = x`, so the linear part
of `μ(x) ν(x)` can be folded into the *diagonal* of a quadratic coefficient matrix; the constant
`e e'` is the only leftover. -/
theorem affine_mul_expand [DecidableEq ι] (e : ZMod 2) (a : ι → ZMod 2) (e' : ZMod 2)
    (a' : ι → ZMod 2) (x : ι → ZMod 2) :
    e * e' + ∑ i, ∑ j, (a i * a' j + (if i = j then e * a' i + e' * a i else 0)) * x i * x j
      = affine e a x * affine e' a' x := by
  have hsq : ∀ y : ZMod 2, y * y = y := by decide
  have step : ∀ i : ι,
      ∑ j, (a i * a' j + (if i = j then e * a' i + e' * a i else 0)) * x i * x j
        = (a i * x i) * (∑ j, a' j * x j) + (e * a' i + e' * a i) * x i := by
    intro i
    simp only [add_mul, ite_mul, zero_mul, Finset.sum_add_distrib, Finset.sum_ite_eq,
      Finset.mem_univ, if_true, Finset.mul_sum]
    congr 1
    · exact Finset.sum_congr rfl fun j _ => by ring
    · have h2 : ∀ y z : ZMod 2, y * z * z = y * z := fun y z => by rw [mul_assoc, hsq]
      simp only [h2]
  rw [Finset.sum_congr rfl (fun i (_ : i ∈ Finset.univ) => step i), Finset.sum_add_distrib,
    ← Finset.sum_mul]
  have hsplit : ∑ i, (e * a' i + e' * a i) * x i
      = e * (∑ i, a' i * x i) + e' * (∑ i, a i * x i) := by
    rw [Finset.mul_sum, Finset.mul_sum, ← Finset.sum_add_distrib]
    exact Finset.sum_congr rfl fun i _ => by ring
  rw [hsplit]
  simp only [affine]
  ring

/-- **`lem:qpf-closure`, quadratic phases.**  The sign of a product of two affine forms,
`(−1)^{μ(x) ν(x)}`, is a QPF. -/
theorem isQPF_affineProdSign (e : ZMod 2) (a : ι → ZMod 2) (e' : ZMod 2)
    (a' : ι → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => sgn (affine e a x * affine e' a' x)) := by
  classical
  refine (isQPF_phase (sgn (e * e')) 0
    (Matrix.of fun i j => a i * a' j + (if i = j then e * a' i + e' * a i else 0))).congr
    fun x => ?_
  simp only [Pi.zero_apply, zero_mul, Finset.sum_const_zero, zeta4_zero, mul_one,
    Matrix.of_apply]
  rw [← sgn_add, affine_mul_expand]

/-! ## `eq:parity-lift`: `i`-powers of an affine form -/

/-- A single-coordinate `i`-power `i^{c · t · x_{j₀}}` is a QPF: it *is* a linear part. -/
theorem isQPF_coordPhase (c : ZMod 4) (t : ZMod 2) (j₀ : ι) :
    IsQPF (fun x : ι → ZMod 2 => zeta4 (c * lift4 (t * x j₀))) := by
  classical
  refine (isQPF_phase 1 (fun j => if j = j₀ then c * lift4 t else 0) 0).congr fun x => ?_
  simp only [Matrix.zero_apply, zero_mul, Finset.sum_const_zero, sgn_zero, one_mul, mul_one,
    ite_mul, Finset.sum_ite_eq', Finset.mem_univ, if_true]
  rw [lift4_mul, mul_assoc]

/-- **`eq:parity-lift`, induction step form.**  For every finite set `S` of coordinates, the
partial `i`-power `i^{c (e + ∑_{j ∈ S} a_j x_j)}` is a QPF.

The induction is over `S`, generalized over both `c` and `e`.  Adding one coordinate splits the
lift by `eq:xor-lift` (`lift4_add`) into three factors: the inductive one, a single-coordinate
linear phase, and a *quadratic correction* `i^{2 c · lift u · lift v}`, which the doubling rule
`zeta4_two_mul` turns into the sign of a product of two affine forms — a QPF by
`isQPF_affineProdSign`.  That the correction stays quadratic is the whole content of the
lemma. -/
theorem isQPF_affinePhase_aux (S : Finset ι) :
    ∀ (c : ZMod 4) (e : ZMod 2) (a : ι → ZMod 2),
      IsQPF (fun x : ι → ZMod 2 => zeta4 (c * lift4 (e + ∑ j ∈ S, a j * x j))) := by
  classical
  refine Finset.induction_on S ?_ ?_
  · intro c e a
    refine (isQPF_const (zeta4 (c * lift4 e))).congr fun x => ?_
    rw [Finset.sum_empty, add_zero]
  · intro j₀ S' hj₀ ih c e a
    have h1 : IsQPF (fun x : ι → ZMod 2 =>
        zeta4 (c * lift4 (e + ∑ j ∈ S', a j * x j))) := ih c e a
    have h2 : IsQPF (fun x : ι → ZMod 2 => zeta4 (c * lift4 (a j₀ * x j₀))) :=
      isQPF_coordPhase c (a j₀) j₀
    have h3 : IsQPF (fun x : ι → ZMod 2 =>
        sgn (affine e (fun j => if j ∈ S' then a j else 0) x *
          affine 0 (fun j => if j = j₀ then ((c.val : ℕ) : ZMod 2) * a j₀ else 0) x)) :=
      isQPF_affineProdSign _ _ _ _
    refine ((h1.mul h2).mul h3).congr fun x => ?_
    have hsum : e + ∑ j ∈ insert j₀ S', a j * x j
        = (e + ∑ j ∈ S', a j * x j) + a j₀ * x j₀ := by
      rw [Finset.sum_insert hj₀]; ring
    have key : c * lift4 ((e + ∑ j ∈ S', a j * x j) + a j₀ * x j₀)
        = c * lift4 (e + ∑ j ∈ S', a j * x j) + c * lift4 (a j₀ * x j₀)
          + 2 * (c * lift4 (e + ∑ j ∈ S', a j * x j) * lift4 (a j₀ * x j₀)) := by
      rw [lift4_add]; ring
    rw [hsum, key, zeta4_add, zeta4_add, zeta4_two_mul, cast_val_mul_lift4,
      affine_restrict, affine_single]
    ring_nf

/-- **`eq:parity-lift`.**  For every `c : ZMod 4` and every affine form `μ`, the phase
`i^{c · μ(x)}` is a quadratic phase function.  This is the crux of `lem:qpf-closure`: it is what
lets an affine substitution — or the elimination of a summed-out variable — be absorbed back
into the linear and quadratic data. -/
theorem isQPF_affinePhase (c : ZMod 4) (e : ZMod 2) (a : ι → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => zeta4 (c * lift4 (affine e a x))) :=
  isQPF_affinePhase_aux Finset.univ c e a

/-! ## The four scalar sums of `lem:qpf-closure`(iii) -/

/-- Shifting the constant of an affine form by `1`. -/
theorem affine_succ (e : ZMod 2) (a : ι → ZMod 2) (x : ι → ZMod 2) :
    affine (e + 1) a x = affine e a x + 1 := by
  simp only [affine]
  ring

/-- **`lem:qpf-closure`(iii), scalar step.**  Summing a single free variable `x_m` out of a QPF
produces the factor `1 + i^{c}(−1)^{μ(x)}`, where `c` is the linear coefficient of `x_m` and `μ`
is the affine form `⊕_{j} q_{jm} x_j`.  Each of the four values of `c` puts this in QPF form:

* `c = 0` : `2·[μ = 0]`,  `c = 2` : `2·[μ = 1]` — one further affine constraint;
* `c = 1` : `(1+i)·i^{3μ}`, `c = 3` : `(1−i)·i^{μ}` — one further parity-lift factor. -/
theorem isQPF_onePlusIPow (c : ZMod 4) (e : ZMod 2) (a : ι → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => 1 + zeta4 c * sgn (affine e a x)) := by
  have hbit : ∀ y : ZMod 2, y = 0 ∨ y = 1 := by decide
  have hc : c = 0 ∨ c = 1 ∨ c = 2 ∨ c = 3 := by revert c; decide
  rcases hc with rfl | rfl | rfl | rfl
  · refine ((isQPF_affineIndicator e a).smul 2).congr fun x => ?_
    rcases hbit (affine e a x) with h | h <;> rw [h]
    · rw [if_pos rfl, sgn_zero, zeta4_zero]; ring
    · rw [if_neg (by decide : ¬ (1 : ZMod 2) = 0), sgn_one, zeta4_zero]; ring
  · refine ((isQPF_affinePhase 3 e a).smul (1 + Complex.I)).congr fun x => ?_
    rcases hbit (affine e a x) with h | h <;> rw [h]
    · rw [lift4_zero, mul_zero, zeta4_zero, sgn_zero, zeta4_one]; ring
    · rw [lift4_one, mul_one, zeta4_three, sgn_one, zeta4_one]
      linear_combination -Complex.I_sq
  · refine ((isQPF_affineIndicator (e + 1) a).smul 2).congr fun x => ?_
    rw [affine_succ]
    rcases hbit (affine e a x) with h | h <;> rw [h]
    · rw [if_neg (by decide : ¬ ((0 : ZMod 2) + 1) = 0), sgn_zero, zeta4_two]; ring
    · rw [if_pos (by decide : ((1 : ZMod 2) + 1) = 0), sgn_one, zeta4_two]; ring
  · refine ((isQPF_affinePhase 1 e a).smul (1 - Complex.I)).congr fun x => ?_
    rcases hbit (affine e a x) with h | h <;> rw [h]
    · rw [lift4_zero, mul_zero, zeta4_zero, sgn_zero, zeta4_three]; ring
    · rw [lift4_one, mul_one, zeta4_one, sgn_one, zeta4_three]
      linear_combination -Complex.I_sq

end Stab
end Formal
