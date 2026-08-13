/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.Fourier

/-!
# The residue histogram from a single Fourier mode (`cor:histogram-one-mode`)

For `r = 2^m` the whole residue histogram is determined by the mode-`1` value `S(f)` together
with a count that uses neither the graph nor the decomposition.  Two halves, following the
paper:

* **Differences.**  `Φ_r = X^{r/2} + 1`, so `ω^{j+r/2} = −ω^j` and
  `S(f) = ∑_{j<r/2} Δ_j ω^j` with `Δ_j = N_j − N_{j+r/2}` (`S_eq_sum_Delta`).  Those integer
  coordinates are the only ones (`coords_unique`), which is what makes "read `Δ` off `S(f)`"
  meaningful.  This is the **only** place `r = 2^m` is used, through `φ(2^m) = 2^{m-1} = r/2`.
* **Sums.**  Halving the modulus kills `η = r/2`, so the pair `N_j + N_{j+r/2}` is the
  histogram of the *linear* part of `f` alone (`Mlin_eq_N_add`).  This holds at every even `r`.

`N_recovered` then puts the two together as `2·N_j = M_j + Δ_j`.

The coefficient ring here is `ℂ`, as everywhere else in this development, so "the power-basis
coordinates of `S(f)`" is stated as the expansion `S_eq_sum_Delta` plus the uniqueness clause
`coords_unique` rather than as a `PowerBasis`.
-/

open scoped BigOperators

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-! ## `η = r/2` as a residue -/

/-- `η = r/2` viewed in `ZMod r`.  It is its own negative and is nonzero. -/
def half : ZMod I.r := ((I.r / 2 : ℕ) : ZMod I.r)

theorem half_add_half : I.half + I.half = 0 := by
  have h2 : I.r / 2 + I.r / 2 = I.r := by obtain ⟨k, hk⟩ := I.hr; omega
  simp only [half]
  calc ((I.r / 2 : ℕ) : ZMod I.r) + ((I.r / 2 : ℕ) : ZMod I.r)
      = ((I.r / 2 + I.r / 2 : ℕ) : ZMod I.r) := by push_cast; ring
    _ = ((I.r : ℕ) : ZMod I.r) := by rw [h2]
    _ = 0 := ZMod.natCast_self I.r

theorem neg_half : -I.half = I.half := by
  have h := I.half_add_half
  linear_combination -h

theorem half_ne_zero : I.half ≠ 0 := by
  have hr0 := I.hr0
  have hpos : 0 < I.r / 2 := by obtain ⟨k, hk⟩ := I.hr; omega
  have hlt : I.r / 2 < I.r := by omega
  intro hz
  have hval : ((I.r / 2 : ℕ) : ZMod I.r).val = I.r / 2 := ZMod.val_cast_of_lt hlt
  rw [half] at hz
  rw [hz, ZMod.val_zero] at hval
  omega

/-! ## The two computable pieces -/

/-- The count differences `Δ_j = N_j − N_{j+r/2}`.  These are the power-basis coordinates of
`S(f)` when `r` is a power of two (`S_eq_sum_Delta`, `coords_unique`). -/
def Delta (j : ZMod I.r) : ℤ := (I.N j : ℤ) - (I.N (j + I.half) : ℤ)

/-- The linear part of the phase polynomial: `f` with its quadratic term deleted. -/
def flin (x : I.V → ZMod 2) : ZMod I.r := I.c + ∑ v, I.b v * ((x v).val : ZMod I.r)

/-- The residue histogram of the linear part. -/
def Nlin (j : ZMod I.r) : ℕ :=
  (Finset.univ.filter (fun x : I.V → ZMod 2 => I.flin x = j)).card

/-- The paired counts `M_j`, from the linear part alone: no graph and no decomposition. -/
def Mlin (j : ZMod I.r) : ℕ := I.Nlin j + I.Nlin (j + I.half)

/-- `f` differs from its linear part by `η` times the selected-edge count. -/
theorem f_eq_flin_add (x : I.V → ZMod 2) :
    I.f x = I.flin x + I.half * ((I.selCount x : ℕ) : ZMod I.r) := by
  rw [SopInstance.f, flin, half]

/-- **The `η = r/2` collapse.**  `f x` is its linear part, possibly shifted by `η`.  Which of
the two happens is the parity of the selected-edge count, and nothing below needs to know. -/
theorem f_eq_flin_or (x : I.V → ZMod 2) :
    I.f x = I.flin x ∨ I.f x = I.flin x + I.half := by
  have hf := I.f_eq_flin_add x
  rw [half, Formal.half_mul_reduce I.hr (I.selCount x)] at hf
  rcases Nat.mod_two_eq_zero_or_one (I.selCount x) with h | h <;> rw [h] at hf
  · left; simpa using hf
  · right; rw [hf, half]; push_cast; ring

/-! ## The sums: `N_j + N_{j+r/2}` needs only the linear part -/

/-- Splitting a histogram over the pair `{j, j+r/2}`. -/
private theorem card_pair (g : (I.V → ZMod 2) → ZMod I.r) (j : ZMod I.r) :
    (Finset.univ.filter (fun x => g x = j)).card
      + (Finset.univ.filter (fun x => g x = j + I.half)).card
      = (Finset.univ.filter (fun x => g x = j ∨ g x = j + I.half)).card := by
  classical
  have hne : j ≠ j + I.half := by
    intro hEq
    exact I.half_ne_zero (by linear_combination -hEq)
  rw [Finset.filter_or, Finset.card_union_of_disjoint]
  refine Finset.disjoint_left.mpr ?_
  intro x hx hx'
  rw [Finset.mem_filter] at hx hx'
  exact hne (hx.2 ▸ hx'.2)

/-- **The paired counts come from the linear part.**  `M_j = N_j + N_{j+r/2}`, and `M` is
defined without reference to the graph.  Holds at every even modulus. -/
theorem Mlin_eq_N_add (j : ZMod I.r) : I.Mlin j = I.N j + I.N (j + I.half) := by
  classical
  have hiff : ∀ x : I.V → ZMod 2,
      (I.f x = j ∨ I.f x = j + I.half) ↔ (I.flin x = j ∨ I.flin x = j + I.half) := by
    intro x
    rcases I.f_eq_flin_or x with h | h
    · rw [h]
    · constructor
      · rintro (hx | hx) <;> rw [h] at hx
        · right
          have : I.flin x = j - I.half := by linear_combination hx
          rw [this]
          linear_combination -I.half_add_half
        · left; linear_combination hx
      · rintro (hx | hx) <;> rw [h, hx]
        · right; rfl
        · left; linear_combination I.half_add_half
  rw [SopInstance.Mlin, SopInstance.Nlin, SopInstance.Nlin, SopInstance.N, SopInstance.N,
    I.card_pair I.flin j, I.card_pair I.f j]
  exact congrArg Finset.card (Finset.filter_congr (fun x _ => by rw [hiff x]))

/-! ## The differences: `S(f)` expands over `1, ω, …, ω^{r/2-1}` -/

/-- **`S(f) = ∑_{j<r/2} Δ_j ω^j`.**  The coefficients of the expansion are exactly the count
differences.  Holds at every even modulus; that they are *the* coordinates needs `r = 2^m`
(`coords_unique`). -/
theorem S_eq_sum_Delta :
    I.S = ∑ j ∈ Finset.range (I.r / 2), (I.Delta (j : ZMod I.r) : ℂ) * I.omega ^ j := by
  have hhalf : I.r / 2 + I.r / 2 = I.r := by obtain ⟨k, hk⟩ := I.hr; omega
  have h1 : I.S = ∑ t : ZMod I.r, (I.N t : ℂ) * I.omega ^ t.val := by
    rw [← I.Nhat_one, I.Nhat_eq_sum_N 1]
    exact Finset.sum_congr rfl (fun t _ => by rw [one_mul, chi])
  have h2 : (∑ t : ZMod I.r, (I.N t : ℂ) * I.omega ^ t.val)
      = ∑ i ∈ Finset.range I.r, (I.N (i : ZMod I.r) : ℂ) * I.omega ^ i := by
    refine Finset.sum_nbij' (fun t => t.val) (fun i => (i : ZMod I.r))
      (fun t _ => Finset.mem_range.mpr (ZMod.val_lt t))
      (fun i _ => Finset.mem_univ _)
      (fun t _ => ZMod.natCast_zmod_val t)
      (fun i hi => ZMod.val_cast_of_lt (Finset.mem_range.mp hi))
      (fun t _ => by rw [ZMod.natCast_zmod_val])
  have h3 : (∑ i ∈ Finset.range I.r, (I.N (i : ZMod I.r) : ℂ) * I.omega ^ i)
      = (∑ i ∈ Finset.range (I.r / 2), (I.N (i : ZMod I.r) : ℂ) * I.omega ^ i)
        + ∑ i ∈ Finset.range (I.r / 2),
            (I.N ((I.r / 2 + i : ℕ) : ZMod I.r) : ℂ) * I.omega ^ (I.r / 2 + i) := by
    have hle : I.r / 2 ≤ I.r := Nat.div_le_self _ _
    have hsub : I.r - I.r / 2 = I.r / 2 := by omega
    rw [Finset.range_eq_Ico,
      ← Finset.sum_Ico_consecutive _ (Nat.zero_le (I.r / 2)) hle, ← Finset.range_eq_Ico,
      Finset.sum_Ico_eq_sum_range, hsub]
  rw [h1, h2, h3, ← Finset.sum_add_distrib]
  refine Finset.sum_congr rfl (fun i _ => ?_)
  have hshift : ((I.r / 2 + i : ℕ) : ZMod I.r) = (i : ZMod I.r) + I.half := by
    rw [half]; push_cast; ring
  have hpow : I.omega ^ (I.r / 2 + i) = -(I.omega ^ i) := by
    rw [pow_add, I.omega_pow_half]; ring
  rw [hshift, hpow, SopInstance.Delta]
  push_cast
  ring

/-! ## Uniqueness of the coordinates, where `r = 2^m` enters -/

/-- **The coordinates are unique.**  For `r = 2^m` the powers `1, ω, …, ω^{r/2-1}` are
independent over `ℤ`, because `deg (minpoly ℚ ω) = φ(2^m) = r/2`.  So an integer expansion of
length `r/2` determines its coefficients, and `Δ` really is read off `S(f)`. -/
theorem coords_unique {m : ℕ} (hm1 : 1 ≤ m) (hm : I.r = 2 ^ m) (a b : ℕ → ℤ)
    (h : (∑ j ∈ Finset.range (I.r / 2), (a j : ℂ) * I.omega ^ j)
       = ∑ j ∈ Finset.range (I.r / 2), (b j : ℂ) * I.omega ^ j) :
    ∀ j ∈ Finset.range (I.r / 2), a j = b j := by
  classical
  have hprim : IsPrimitiveRoot I.omega I.r := Complex.isPrimitiveRoot_exp I.r I.hr0.ne'
  have hmin : Polynomial.cyclotomic I.r ℚ = minpoly ℚ I.omega :=
    Polynomial.cyclotomic_eq_minpoly_rat hprim I.hr0
  have hne0 : minpoly ℚ I.omega ≠ 0 := by
    rw [← hmin]; exact Polynomial.cyclotomic_ne_zero I.r ℚ
  -- `deg (minpoly ℚ ω) = φ(2^m) = r/2`: this is where the power of two is used
  have hdeg : (minpoly ℚ I.omega).natDegree = I.r / 2 := by
    rw [← hmin, Polynomial.natDegree_cyclotomic, hm,
      Nat.totient_prime_pow Nat.prime_two (by omega : 0 < m)]
    obtain ⟨m', rfl⟩ : ∃ m', m = m' + 1 := ⟨m - 1, by omega⟩
    simp [pow_succ]
  -- the difference polynomial has ω as a root and degree below r/2, hence vanishes
  set p : Polynomial ℚ :=
    ∑ j ∈ Finset.range (I.r / 2), Polynomial.C (((a j - b j : ℤ) : ℚ)) * Polynomial.X ^ j with hp
  have haeval : Polynomial.aeval I.omega p = 0 := by
    have hterm : ∀ j : ℕ, Polynomial.aeval I.omega
        (Polynomial.C (((a j - b j : ℤ) : ℚ)) * Polynomial.X ^ j)
        = (a j : ℂ) * I.omega ^ j - (b j : ℂ) * I.omega ^ j := by
      intro j
      rw [map_mul, Polynomial.aeval_C, Polynomial.aeval_X_pow, map_intCast]
      push_cast
      ring
    rw [hp, map_sum, Finset.sum_congr rfl (fun j _ => hterm j), Finset.sum_sub_distrib, h,
      sub_self]
  have hdegp : p.degree < (I.r / 2 : ℕ) := by
    rw [hp]
    refine (Polynomial.degree_lt_iff_coeff_zero _ _).mpr (fun n hn => ?_)
    rw [Polynomial.finsetSum_coeff]
    refine Finset.sum_eq_zero (fun j hj => ?_)
    have hjlt : j < I.r / 2 := Finset.mem_range.mp hj
    have hnle : I.r / 2 ≤ n := by exact_mod_cast hn
    rw [Polynomial.coeff_C_mul_X_pow, if_neg (by omega)]
  have hp0 : p = 0 := by
    by_contra hne
    have hle := minpoly.degree_le_of_ne_zero ℚ I.omega hne haeval
    rw [Polynomial.degree_eq_natDegree hne0, hdeg] at hle
    exact absurd (lt_of_le_of_lt hle hdegp) (lt_irrefl _)
  -- read the coefficients back off
  intro j hj
  have hcoeff := congrArg (fun q => Polynomial.coeff q j) hp0
  simp only [hp, Polynomial.finsetSum_coeff, Polynomial.coeff_C_mul_X_pow,
    Polynomial.coeff_zero] at hcoeff
  rw [Finset.sum_ite_eq (Finset.range (I.r / 2)) j (fun i => (((a i - b i : ℤ) : ℚ))),
    if_pos hj] at hcoeff
  have hz : (a j - b j : ℤ) = 0 := by exact_mod_cast hcoeff
  omega

/-! ## The corollary -/

/-- **`cor:histogram-one-mode`, reconstruction clause.**  Every residue count is recovered from
the coordinate `Δ_j` of `S(f)` and the linear-part count `M_j`, exactly over `ℤ`. -/
theorem N_recovered (j : ZMod I.r) :
    2 * (I.N j : ℤ) = (I.Mlin j : ℤ) + I.Delta j
      ∧ 2 * (I.N (j + I.half) : ℤ) = (I.Mlin j : ℤ) - I.Delta j := by
  have hM := I.Mlin_eq_N_add j
  constructor <;> rw [SopInstance.Delta, hM] <;> push_cast <;> ring

end SopInstance
end Formal
