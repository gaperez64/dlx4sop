/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.DPCorrect

/-!
# The Fourier-mode layer (sec:fourier)

The additive character `χ(x) = ω_r^{x.val}` on `ZMod r`, the mode sums
`N̂(a) = ∑_x χ(a·f(x))`, their relation to the residue counts (`Nhat_eq_sum_N`),
character orthogonality (`sum_chi`), Fourier inversion (`N_inversion`), and the
algorithmic mode table `Aalg` derived from the proven DP (`Aalg_root`,
`single_amplitude` — the content of cor:single-amplitude).

All semantic results are derived from the proven `Dalg_eq_Dspec` /
`dp_returns_counts`; no DP induction is re-proved here.
-/

open scoped BigOperators

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-! ## The additive character `χ` -/

/-- The additive character `χ(x) = ω_r^{x.val}` on `ZMod r`. -/
noncomputable def chi (x : ZMod I.r) : ℂ := I.omega ^ x.val

/-- `ω_r` is an `r`-th root of unity. -/
theorem omega_pow_r : I.omega ^ I.r = 1 := by
  have hrC : (I.r : ℂ) ≠ 0 := Nat.cast_ne_zero.mpr I.hr0.ne'
  have hmul : (I.r : ℂ) * (2 * Real.pi * Complex.I / I.r) = 2 * Real.pi * Complex.I := by
    field_simp
  rw [SopInstance.omega, ← Complex.exp_nat_mul, hmul, Complex.exp_two_pi_mul_I]

/-- Powers of `ω_r` only see the exponent mod `r`. -/
theorem omega_pow_mod (m : ℕ) : I.omega ^ (m % I.r) = I.omega ^ m := by
  conv_rhs => rw [← Nat.div_add_mod m I.r]
  rw [pow_add, pow_mul, I.omega_pow_r, one_pow, one_mul]

/-- `χ` is additive: it is a character of the group `(ZMod r, +)`. -/
theorem chi_add (x y : ZMod I.r) : I.chi (x + y) = I.chi x * I.chi y := by
  simp only [chi]
  rw [ZMod.val_add, I.omega_pow_mod, pow_add]

theorem chi_zero : I.chi 0 = 1 := by
  simp only [chi, ZMod.val_zero, pow_zero]

theorem chi_natCast (m : ℕ) : I.chi (m : ZMod I.r) = I.omega ^ m := by
  simp only [chi]
  rw [ZMod.val_natCast, I.omega_pow_mod]

/-! ## Mode sums and their relation to residue counts -/

/-- The Fourier mode sum `N̂(a) = ∑_x χ(a·f(x))` (sec:fourier). -/
noncomputable def Nhat (a : ZMod I.r) : ℂ := ∑ x : I.V → ZMod 2, I.chi (a * I.f x)

/-- The mode sum is the `χ`-weighted sum of the residue counts `N_j`. -/
theorem Nhat_eq_sum_N (a : ZMod I.r) :
    I.Nhat a = ∑ j : ZMod I.r, (I.N j : ℕ) * I.chi (a * j) := by
  have hN : ∀ j : ZMod I.r, ((I.N j : ℕ) : ℂ)
      = ∑ x : I.V → ZMod 2, (if I.f x = j then (1 : ℂ) else 0) := by
    intro j
    rw [SopInstance.N, Finset.card_filter, Nat.cast_sum]
    refine Finset.sum_congr rfl (fun x _ => ?_)
    by_cases h : I.f x = j
    · rw [if_pos h, if_pos h, Nat.cast_one]
    · rw [if_neg h, if_neg h, Nat.cast_zero]
  calc I.Nhat a
      = ∑ x : I.V → ZMod 2, I.chi (a * I.f x) := rfl
    _ = ∑ x : I.V → ZMod 2, ∑ j : ZMod I.r, (if I.f x = j then I.chi (a * j) else 0) := by
        refine Finset.sum_congr rfl (fun x _ => ?_)
        rw [Finset.sum_ite_eq Finset.univ (I.f x) (fun j => I.chi (a * j)),
          if_pos (Finset.mem_univ _)]
    _ = ∑ j : ZMod I.r, ∑ x : I.V → ZMod 2, (if I.f x = j then I.chi (a * j) else 0) :=
        Finset.sum_comm
    _ = ∑ j : ZMod I.r, (I.N j : ℕ) * I.chi (a * j) := by
        refine Finset.sum_congr rfl (fun j _ => ?_)
        rw [hN j, Finset.sum_mul]
        refine Finset.sum_congr rfl (fun x _ => ?_)
        by_cases h : I.f x = j
        · rw [if_pos h, if_pos h, one_mul]
        · rw [if_neg h, if_neg h, zero_mul]

/-- The first mode is the SOP value itself: `N̂(1) = S(f)`. -/
theorem Nhat_one : I.Nhat 1 = I.S := by
  rw [Nhat, SopInstance.S]
  refine Finset.sum_congr rfl (fun x _ => ?_)
  rw [one_mul, chi]

/-! ## Character orthogonality and Fourier inversion -/

/-- Character orthogonality: `∑_a χ(a·t)` is `r` at `t = 0` and vanishes otherwise. -/
theorem sum_chi (t : ZMod I.r) :
    (∑ a : ZMod I.r, I.chi (a * t)) = if t = 0 then (I.r : ℂ) else 0 := by
  by_cases ht : t = 0
  · subst ht
    rw [if_pos rfl]
    have h1 : ∀ a ∈ (Finset.univ : Finset (ZMod I.r)), I.chi (a * 0) = 1 := by
      intro a _
      rw [mul_zero, chi_zero]
    rw [Finset.sum_congr rfl h1, Finset.sum_const, Finset.card_univ, ZMod.card,
      nsmul_eq_mul, mul_one]
  · rw [if_neg ht]
    have hpow : ∀ a : ZMod I.r, I.chi (a * t) = I.chi t ^ a.val := by
      intro a
      simp only [chi]
      rw [ZMod.val_mul, I.omega_pow_mod, ← pow_mul, mul_comm t.val a.val]
    have hzr : I.chi t ^ I.r = 1 := by
      simp only [chi]
      rw [← pow_mul, mul_comm t.val I.r, pow_mul, I.omega_pow_r, one_pow]
    have hz1 : I.chi t ≠ 1 := by
      simp only [chi]
      intro h
      have hprim : IsPrimitiveRoot I.omega I.r := Complex.isPrimitiveRoot_exp I.r I.hr0.ne'
      have h0 : t.val = 0 :=
        Nat.eq_zero_of_dvd_of_lt ((hprim.pow_eq_one_iff_dvd t.val).mp h) (ZMod.val_lt t)
      exact ht ((ZMod.val_eq_zero t).mp h0)
    have hsum : (∑ a : ZMod I.r, I.chi (a * t)) = ∑ i ∈ Finset.range I.r, I.chi t ^ i :=
      Finset.sum_nbij' (fun a => a.val) (fun i => (i : ZMod I.r))
        (fun a _ => Finset.mem_range.mpr (ZMod.val_lt a))
        (fun i _ => Finset.mem_univ _)
        (fun a _ => ZMod.natCast_zmod_val a)
        (fun i hi => ZMod.val_cast_of_lt (Finset.mem_range.mp hi))
        (fun a _ => hpow a)
    rw [hsum, geom_sum_eq hz1, hzr, sub_self, zero_div]

/-- Fourier inversion: the residue counts are recovered from the mode sums. -/
theorem N_inversion (j : ZMod I.r) :
    ((I.N j : ℕ) : ℂ) = (I.r : ℂ)⁻¹ * ∑ a : ZMod I.r, I.chi (- (a * j)) * I.Nhat a := by
  have hrC : (I.r : ℂ) ≠ 0 := Nat.cast_ne_zero.mpr I.hr0.ne'
  have key : (∑ a : ZMod I.r, I.chi (- (a * j)) * I.Nhat a)
      = ((I.N j : ℕ) : ℂ) * (I.r : ℂ) := by
    calc (∑ a : ZMod I.r, I.chi (- (a * j)) * I.Nhat a)
        = ∑ a : ZMod I.r, ∑ t : ZMod I.r, (I.N t : ℕ) * I.chi (a * (t - j)) := by
          refine Finset.sum_congr rfl (fun a _ => ?_)
          rw [Nhat_eq_sum_N, Finset.mul_sum]
          refine Finset.sum_congr rfl (fun t _ => ?_)
          have harg : - (a * j) + a * t = a * (t - j) := by ring
          rw [← mul_assoc, mul_comm (I.chi (- (a * j))) ((I.N t : ℕ) : ℂ), mul_assoc,
            ← chi_add, harg]
      _ = ∑ t : ZMod I.r, ∑ a : ZMod I.r, (I.N t : ℕ) * I.chi (a * (t - j)) :=
          Finset.sum_comm
      _ = ∑ t : ZMod I.r, (I.N t : ℕ) * (if t - j = 0 then (I.r : ℂ) else 0) := by
          refine Finset.sum_congr rfl (fun t _ => ?_)
          rw [← Finset.mul_sum, I.sum_chi (t - j)]
      _ = ∑ t : ZMod I.r, (if t = j then ((I.N t : ℕ) : ℂ) * (I.r : ℂ) else 0) := by
          refine Finset.sum_congr rfl (fun t _ => ?_)
          by_cases h : t = j
          · rw [if_pos (show t - j = 0 by rw [h, sub_self]), if_pos h]
          · rw [if_neg (fun h0 => h (sub_eq_zero.mp h0)), if_neg h, mul_zero]
      _ = ((I.N j : ℕ) : ℂ) * (I.r : ℂ) := by
          rw [Finset.sum_ite_eq' Finset.univ j (fun t => ((I.N t : ℕ) : ℂ) * (I.r : ℂ)),
            if_pos (Finset.mem_univ j)]
  rw [key, mul_comm ((I.N j : ℕ) : ℂ) ((I.r : ℂ)), ← mul_assoc, inv_mul_cancel₀ hrC,
    one_mul]

/-! ## The algorithmic mode table (cor:single-amplitude) -/

/-- The mode-`a` aggregate of the DP table at `(u, σ)`. -/
noncomputable def Aalg (a : ZMod I.r) (u : RTree I.V) (σ : I.V → ZMod 2) : ℂ :=
  ∑ s : ZMod I.r, (I.Dalg u σ s : ℕ) * I.chi (a * s)

/-- At the root of a decomposition, the DP mode aggregate returns the mode sum
`N̂(a)`, up to the constant phase `χ(a·c)`. Derived from `dp_returns_counts`. -/
theorem Aalg_root (D : RankDecomp I) (a : ZMod I.r) :
    I.chi (a * I.c) * I.Aalg a D.tree 0 = I.Nhat a := by
  have hD : ∀ s : ZMod I.r, I.Dalg D.tree 0 s = I.N (s + I.c) := by
    intro s
    have h := I.dp_returns_counts D (s + I.c)
    rwa [add_sub_cancel_right] at h
  rw [SopInstance.Aalg, Finset.mul_sum, Nhat_eq_sum_N]
  refine Fintype.sum_equiv (Equiv.addRight I.c) _ _ (fun s => ?_)
  simp only [Equiv.coe_addRight]
  have harg : a * I.c + a * s = a * (s + I.c) := by ring
  rw [hD s, ← mul_assoc, mul_comm (I.chi (a * I.c)) ((I.N (s + I.c) : ℕ) : ℂ), mul_assoc,
    ← chi_add, harg]

/-- **cor:single-amplitude**: the mode-`1` aggregate of the root DP table yields the
SOP value `S(f)` after applying the constant phase `χ(c)`. -/
theorem single_amplitude (D : RankDecomp I) :
    I.chi I.c * I.Aalg 1 D.tree 0 = I.S := by
  have h := I.Aalg_root D 1
  rw [one_mul] at h
  rw [h, Nhat_one]

end SopInstance
end Formal
