/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Stab.Affine

/-!
# `lem:qpf-closure`: closure of quadratic phase functions

`Formal.Stab.QPF` proved part (i) (pointwise products) and `Formal.Stab.Affine` supplied the
affine machinery.  This file assembles the remaining parts:

* `IsQPF.sumOne` — `lem:qpf-closure`(iii), summation over a single variable.  This is the
  substantial one.  Writing `x = (x', x_j)`, the linear part splits as `ℓ'(x') + c_j x_j` and
  the quadratic part as `q'(x') + x_j μ(x')` with `μ(x') = q_{jj} + ⊕_{i ≠ j}(q_{ij}+q_{ji})x'_i`
  an affine form.  Either the constraints leave `x_j` free, and the partial sum factors as a
  `j`-free QPF times `1 + i^{c_j}(−1)^{μ(x')}` (`isQPF_onePlusIPow`); or some constraint row
  pins `x_j = α(x')` affinely, exactly one summand survives, and substituting `α` back produces
  one `eq:parity-lift` factor and one product-of-affine-forms sign.
* `IsQPF.sumOver`, `IsQPF.sumLeft` — iterating (iii) over a block of variables.
* `IsQPF.pushforward` — `lem:qpf-closure`(iv), the headline: for a linear `Π : F₂^ι → F₂^κ` the
  pushforward `y ↦ ∑_{Πx = y} g(x)` is again a QPF.  The trick is that over `F₂` the condition
  `Πx = y` is *linear* in the concatenated vector `(x,y)`, so it is just one more constraint
  block, and the fibre sum is a sum over a block of variables.
-/

open scoped BigOperators Matrix

namespace Formal
namespace Stab

variable {ι : Type} [Fintype ι]

/-! ## Two bookkeeping helpers -/

/-- Split off one summand of a sum over a `Fintype`. -/
theorem sum_split_erase {M : Type*} [AddCommMonoid M] [DecidableEq ι] (j : ι) (F : ι → M) :
    ∑ i, F i = (∑ i ∈ Finset.univ.erase j, F i) + F j :=
  (Finset.sum_erase_add Finset.univ F (Finset.mem_univ j)).symm

/-- A sum over `ZMod 2` is a sum of two terms. -/
theorem sum_zmod_two {M : Type*} [AddCommMonoid M] (F : ZMod 2 → M) :
    ∑ b : ZMod 2, F b = F 0 + F 1 := by
  have h : (Finset.univ : Finset (ZMod 2)) = {0, 1} := by decide
  rw [h, Finset.sum_insert (by decide), Finset.sum_singleton]

namespace QPF

variable [DecidableEq ι]

/-! ## Splitting off one variable -/

/-- The presentation `g` with all data referring to coordinate `j` deleted from the linear and
quadratic parts (the constraints are kept).  Writing `x = (x', x_j)`, this presents the
`j`-free factor `ε · i^{ℓ'(x')} · (−1)^{q'(x')} · [x ∈ K]`. -/
def dropVar (g : QPF ι) (j : ι) : QPF ι where
  eps := g.eps
  lin := fun i => if i = j then 0 else g.lin i
  quad := Matrix.of fun i k => if i = j ∨ k = j then 0 else g.quad i k
  K := g.K
  M := g.M
  d := g.d

omit [Fintype ι] in
@[simp] theorem dropVar_M (g : QPF ι) (j : ι) : (g.dropVar j).M = g.M := rfl

omit [Fintype ι] in
@[simp] theorem dropVar_d (g : QPF ι) (j : ι) : (g.dropVar j).d = g.d := rfl

theorem sat_dropVar_iff (g : QPF ι) (j : ι) (x : ι → ZMod 2) :
    (g.dropVar j).Sat x ↔ g.Sat x := Iff.rfl

/-- `ℓ'(x') = ∑_{i ≠ j} c_i x_i`. -/
theorem linVal_dropVar (g : QPF ι) (j : ι) (x : ι → ZMod 2) :
    (g.dropVar j).linVal x = ∑ i ∈ Finset.univ.erase j, g.lin i * lift4 (x i) := by
  rw [linVal, sum_split_erase j]
  change (∑ i ∈ Finset.univ.erase j, (if i = j then 0 else g.lin i) * lift4 (x i))
      + (if j = j then (0 : ZMod 4) else g.lin j) * lift4 (x j) = _
  rw [if_pos rfl, zero_mul, add_zero]
  exact Finset.sum_congr rfl fun i hi => by rw [if_neg (Finset.ne_of_mem_erase hi)]

/-- `q'(x') = ∑_{i,k ≠ j} q_{ik} x_i x_k`. -/
theorem quadVal_dropVar (g : QPF ι) (j : ι) (x : ι → ZMod 2) :
    (g.dropVar j).quadVal x
      = ∑ i ∈ Finset.univ.erase j, ∑ k ∈ Finset.univ.erase j, g.quad i k * x i * x k := by
  change (∑ i, ∑ k, (if i = j ∨ k = j then (0 : ZMod 2) else g.quad i k) * x i * x k) = _
  rw [sum_split_erase j]
  have hlast : ∑ k, (if j = j ∨ k = j then (0 : ZMod 2) else g.quad j k) * x j * x k = 0 := by
    refine Finset.sum_eq_zero fun k _ => ?_
    rw [if_pos (Or.inl rfl), zero_mul, zero_mul]
  rw [hlast, add_zero]
  refine Finset.sum_congr rfl fun i hi => ?_
  have hij : ¬ i = j := Finset.ne_of_mem_erase hi
  rw [sum_split_erase j, if_pos (Or.inr rfl), zero_mul, zero_mul, add_zero]
  exact Finset.sum_congr rfl fun k hk => by
    rw [if_neg (by rintro (h | h) <;> [exact hij h; exact Finset.ne_of_mem_erase hk h])]

/-- The affine form `μ(x') = q_{jj} + ⊕_{i ≠ j}(q_{ij}+q_{ji}) x_i` picked up by the variable
`x_j` in the quadratic part. -/
def quadCoef (g : QPF ι) (j : ι) : ι → ZMod 2 :=
  fun i => if i = j then 0 else g.quad i j + g.quad j i

/-- `ℓ(x) = ℓ'(x') + c_j · x_j` under `Function.update`. -/
theorem linVal_update (g : QPF ι) (j : ι) (x : ι → ZMod 2) (b : ZMod 2) :
    g.linVal (Function.update x j b) = (g.dropVar j).linVal x + g.lin j * lift4 b := by
  rw [linVal, sum_split_erase j, Function.update_self, linVal_dropVar]
  exact congrArg (· + g.lin j * lift4 b)
    (Finset.sum_congr rfl fun i hi => by
      rw [Function.update_of_ne (Finset.ne_of_mem_erase hi)])

/-- `q(x) = q'(x') + x_j · μ(x')` under `Function.update`. -/
theorem quadVal_update (g : QPF ι) (j : ι) (x : ι → ZMod 2) (b : ZMod 2) :
    g.quadVal (Function.update x j b)
      = (g.dropVar j).quadVal x + b * affine (g.quad j j) (g.quadCoef j) x := by
  have hb : b * b = b := by revert b; decide
  have hupd : ∀ i ∈ Finset.univ.erase j, Function.update x j b i = x i := fun i hi =>
    Function.update_of_ne (Finset.ne_of_mem_erase hi) _ _
  have hmu : affine (g.quad j j) (g.quadCoef j) x
      = g.quad j j + ∑ i ∈ Finset.univ.erase j, (g.quad i j + g.quad j i) * x i := by
    have h1 : g.quadCoef j j * x j = 0 := by simp [quadCoef]
    have h2 : ∀ i ∈ Finset.univ.erase j,
        g.quadCoef j i * x i = (g.quad i j + g.quad j i) * x i := by
      intro i hi
      have hne : i ≠ j := Finset.ne_of_mem_erase hi
      simp [quadCoef, hne]
    simp only [affine]
    rw [sum_split_erase j, h1, add_zero, Finset.sum_congr rfl h2]
  have inner : ∀ i ∈ Finset.univ.erase j,
      (∑ k, g.quad i k * Function.update x j b i * Function.update x j b k)
        = (∑ k ∈ Finset.univ.erase j, g.quad i k * x i * x k) + g.quad i j * x i * b := by
    intro i hi
    rw [sum_split_erase j, hupd i hi, Function.update_self]
    exact congrArg (· + g.quad i j * x i * b)
      (Finset.sum_congr rfl fun k hk => by rw [hupd k hk])
  have last : (∑ k, g.quad j k * Function.update x j b j * Function.update x j b k)
      = (∑ k ∈ Finset.univ.erase j, g.quad j k * b * x k) + g.quad j j * b * b := by
    rw [sum_split_erase j, Function.update_self]
    exact congrArg (· + g.quad j j * b * b)
      (Finset.sum_congr rfl fun k hk => by rw [hupd k hk])
  have combine : (∑ i ∈ Finset.univ.erase j, g.quad i j * x i * b)
      + (∑ k ∈ Finset.univ.erase j, g.quad j k * b * x k)
      = b * ∑ i ∈ Finset.univ.erase j, (g.quad i j + g.quad j i) * x i := by
    rw [Finset.mul_sum, ← Finset.sum_add_distrib]
    exact Finset.sum_congr rfl fun i _ => by ring
  rw [quadVal, sum_split_erase j, Finset.sum_congr rfl inner, Finset.sum_add_distrib, last,
    quadVal_dropVar, hmu]
  linear_combination combine + g.quad j j * hb

/-! ## The free case of `lem:qpf-closure`(iii) -/

/-- If coordinate `j` does not occur in the constraint system, membership in `K` does not see
it. -/
theorem sat_update_of_col_zero (g : QPF ι) (j : ι) (hcol : ∀ k, g.M k j = 0)
    (x : ι → ZMod 2) (b : ZMod 2) : g.Sat (Function.update x j b) ↔ g.Sat x := by
  have key : ∀ (v : ι → ZMod 2) (k : g.K),
      (g.M.mulVec v) k = ∑ i ∈ Finset.univ.erase j, g.M k i * v i := by
    intro v k
    change (∑ i, g.M k i * v i) = _
    rw [sum_split_erase j, hcol k, zero_mul, add_zero]
  have same : ∀ k : g.K, (g.M.mulVec (Function.update x j b)) k = (g.M.mulVec x) k := by
    intro k
    rw [key, key]
    exact Finset.sum_congr rfl fun i hi => by
      rw [Function.update_of_ne (Finset.ne_of_mem_erase hi)]
  constructor
  · intro h; funext k; rw [← same k]; exact congrFun h k
  · intro h; funext k; rw [same k]; exact congrFun h k

/-- **`lem:qpf-closure`(iii), free case.**  When the constraints leave `x_j` free, summing it
out multiplies the `j`-free QPF by the scalar `1 + i^{c_j}(−1)^{μ(x')}`. -/
theorem eval_sum_update_free (g : QPF ι) (j : ι) (hcol : ∀ k, g.M k j = 0) (x : ι → ZMod 2) :
    ∑ b : ZMod 2, g.eval (Function.update x j b)
      = (g.dropVar j).eval x
        * (1 + zeta4 (g.lin j) * sgn (affine (g.quad j j) (g.quadCoef j) x)) := by
  rw [sum_zmod_two]
  by_cases hs : g.Sat x
  · rw [eval_of_sat ((sat_update_of_col_zero g j hcol x 0).2 hs),
      eval_of_sat ((sat_update_of_col_zero g j hcol x 1).2 hs),
      eval_of_sat (show (g.dropVar j).Sat x from hs),
      linVal_update, linVal_update, quadVal_update, quadVal_update]
    change (g.dropVar j).eps * _ * _ + (g.dropVar j).eps * _ * _ = _
    simp only [lift4_zero, mul_zero, add_zero, zero_mul, lift4_one, mul_one, one_mul,
      zeta4_add, sgn_add]
    change g.eps * _ * _ + g.eps * _ * _ = g.eps * _ * _ * _
    ring
  · rw [eval_of_not_sat (fun hc => hs ((sat_update_of_col_zero g j hcol x 0).1 hc)),
      eval_of_not_sat (fun hc => hs ((sat_update_of_col_zero g j hcol x 1).1 hc)),
      eval_of_not_sat (show ¬ (g.dropVar j).Sat x from hs)]
    ring

/-! ## The constrained case of `lem:qpf-closure`(iii) -/

/-- One row of the constraint system, with coordinate `j` split off. -/
theorem mulVec_update (g : QPF ι) (j : ι) (k : g.K) (x : ι → ZMod 2) (b : ZMod 2) :
    (g.M.mulVec (Function.update x j b)) k
      = (∑ i ∈ Finset.univ.erase j, g.M k i * x i) + g.M k j * b := by
  change (∑ i, g.M k i * Function.update x j b i) = _
  rw [sum_split_erase j, Function.update_self]
  exact congrArg (· + g.M k j * b)
    (Finset.sum_congr rfl fun i hi => by
      rw [Function.update_of_ne (Finset.ne_of_mem_erase hi)])

/-- Coefficients of the affine form `α(x') = d_{k₀} ⊕ ⊕_{i ≠ j} M_{k₀ i} x_i` to which
constraint row `k₀` pins the variable `x_j` (when `M_{k₀ j} = 1`). -/
def pinCoef (g : QPF ι) (j : ι) (k₀ : g.K) : ι → ZMod 2 :=
  fun i => if i = j then 0 else g.M k₀ i

/-- The affine form `α(x')` pinning `x_j`. -/
def pin (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) : ZMod 2 :=
  affine (g.d k₀) (g.pinCoef j k₀) x

theorem pin_eq (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) :
    g.pin j k₀ x = g.d k₀ + ∑ i ∈ Finset.univ.erase j, g.M k₀ i * x i := by
  have h1 : g.pinCoef j k₀ j * x j = 0 := by simp [pinCoef]
  have h2 : ∀ i ∈ Finset.univ.erase j, g.pinCoef j k₀ i * x i = g.M k₀ i * x i := by
    intro i hi
    have hne : i ≠ j := Finset.ne_of_mem_erase hi
    simp [pinCoef, hne]
  simp only [pin, affine]
  rw [sum_split_erase j, h1, add_zero, Finset.sum_congr rfl h2]

/-- If row `k₀` has a `1` in column `j`, then satisfying the constraints forces `x_j = α(x')`. -/
theorem eq_pin_of_sat (g : QPF ι) (j : ι) (k₀ : g.K) (hk₀ : g.M k₀ j = 1)
    (x : ι → ZMod 2) (b : ZMod 2) (h : g.Sat (Function.update x j b)) : b = g.pin j k₀ x := by
  have key : ∀ A b d : ZMod 2, A + b = d → b = d + A := by decide
  have h1 : (∑ i ∈ Finset.univ.erase j, g.M k₀ i * x i) + g.M k₀ j * b = g.d k₀ := by
    rw [← mulVec_update]
    exact congrFun h k₀
  rw [hk₀, one_mul] at h1
  rw [pin_eq]
  exact key _ _ _ h1

/-- The presentation obtained by substituting `x_j = α(x')`: the linear and quadratic data lose
coordinate `j`, and every constraint row `k` is corrected by `M_{k j} ·` (row `k₀`). -/
def substVar (g : QPF ι) (j : ι) (k₀ : g.K) : QPF ι where
  eps := g.eps
  lin := (g.dropVar j).lin
  quad := (g.dropVar j).quad
  K := g.K
  M := Matrix.of fun k i => if i = j then 0 else g.M k i + g.M k j * g.M k₀ i
  d := fun k => g.d k + g.M k j * g.d k₀

omit [Fintype ι] in
@[simp] theorem substVar_eps (g : QPF ι) (j : ι) (k₀ : g.K) :
    (g.substVar j k₀).eps = g.eps := rfl

@[simp] theorem substVar_linVal (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) :
    (g.substVar j k₀).linVal x = (g.dropVar j).linVal x := rfl

@[simp] theorem substVar_quadVal (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) :
    (g.substVar j k₀).quadVal x = (g.dropVar j).quadVal x := rfl

theorem substVar_rowVal (g : QPF ι) (j : ι) (k₀ k : g.K) (x : ι → ZMod 2) :
    ((g.substVar j k₀).M.mulVec x) k
      = (∑ i ∈ Finset.univ.erase j, g.M k i * x i)
        + g.M k j * ∑ i ∈ Finset.univ.erase j, g.M k₀ i * x i := by
  change (∑ i, (if i = j then (0 : ZMod 2) else g.M k i + g.M k j * g.M k₀ i) * x i) = _
  rw [sum_split_erase j, if_pos rfl, zero_mul, add_zero, Finset.mul_sum,
    ← Finset.sum_add_distrib]
  exact Finset.sum_congr rfl fun i hi => by
    rw [if_neg (Finset.ne_of_mem_erase hi)]; ring

/-- Substituting `x_j = α(x')` turns the original constraints into the corrected ones. -/
theorem sat_substVar_iff (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) :
    g.Sat (Function.update x j (g.pin j k₀ x)) ↔ (g.substVar j k₀).Sat x := by
  have key : ∀ R m A dk dk0 : ZMod 2,
      (R + m * (dk0 + A) = dk) ↔ (R + m * A = dk + m * dk0) := by decide
  constructor
  · intro h
    funext k
    have h1 := congrFun h k
    rw [mulVec_update, pin_eq] at h1
    rw [substVar_rowVal]
    exact (key _ _ _ _ _).1 h1
  · intro h
    funext k
    have h1 := congrFun h k
    rw [substVar_rowVal] at h1
    rw [mulVec_update, pin_eq]
    exact (key _ _ _ _ _).2 h1

/-- **`lem:qpf-closure`(iii), constrained case.**  Substituting the pinned value `x_j = α(x')`
back into the presentation produces a QPF times one `eq:parity-lift` factor and one
product-of-affine-forms sign. -/
theorem eval_update_pin (g : QPF ι) (j : ι) (k₀ : g.K) (x : ι → ZMod 2) :
    g.eval (Function.update x j (g.pin j k₀ x))
      = (g.substVar j k₀).eval x
        * zeta4 (g.lin j * lift4 (affine (g.d k₀) (g.pinCoef j k₀) x))
        * sgn (affine (g.d k₀) (g.pinCoef j k₀) x * affine (g.quad j j) (g.quadCoef j) x) := by
  by_cases hs : (g.substVar j k₀).Sat x
  · rw [eval_of_sat ((sat_substVar_iff g j k₀ x).2 hs), eval_of_sat hs,
      linVal_update, quadVal_update, substVar_linVal, substVar_quadVal, substVar_eps,
      zeta4_add, sgn_add]
    simp only [pin]
    ring
  · rw [eval_of_not_sat (fun hc => hs ((sat_substVar_iff g j k₀ x).1 hc)), eval_of_not_sat hs]
    ring

/-- Exactly one summand survives when a constraint row pins `x_j`. -/
theorem eval_sum_update_pinned (g : QPF ι) (j : ι) (k₀ : g.K) (hk₀ : g.M k₀ j = 1)
    (x : ι → ZMod 2) :
    ∑ b : ZMod 2, g.eval (Function.update x j b)
      = g.eval (Function.update x j (g.pin j k₀ x)) := by
  refine Finset.sum_eq_single (g.pin j k₀ x) (fun b _ hb => ?_) (fun h => ?_)
  · exact eval_of_not_sat fun hc => hb (eq_pin_of_sat g j k₀ hk₀ x b hc)
  · exact absurd (Finset.mem_univ _) h

end QPF

/-! ## `lem:qpf-closure`(iii) -/

/-- **`lem:qpf-closure`(iii).**  QPFs are closed under summation over a single variable. -/
theorem IsQPF.sumOne [DecidableEq ι] (j : ι) {f : (ι → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun x : ι → ZMod 2 => ∑ b : ZMod 2, f (Function.update x j b)) := by
  obtain ⟨g, hg⟩ := hf
  by_cases hcol : ∀ k, g.M k j = 0
  · refine (IsQPF.mul ⟨g.dropVar j, fun x => rfl⟩
      (isQPF_onePlusIPow (g.lin j) (g.quad j j) (g.quadCoef j))).congr fun x => ?_
    rw [← QPF.eval_sum_update_free g j hcol x]
    exact Finset.sum_congr rfl fun b _ => hg _
  · obtain ⟨k₀, hk₀⟩ := not_forall.mp hcol
    have hone : ∀ t : ZMod 2, t ≠ 0 → t = 1 := by decide
    have hk₀' : g.M k₀ j = 1 := hone _ hk₀
    refine (IsQPF.mul (IsQPF.mul ⟨g.substVar j k₀, fun x => rfl⟩
        (isQPF_affinePhase (g.lin j) (g.d k₀) (g.pinCoef j k₀)))
        (isQPF_affineProdSign (g.d k₀) (g.pinCoef j k₀) (g.quad j j) (g.quadCoef j))).congr
      fun x => ?_
    rw [← QPF.eval_update_pin, ← QPF.eval_sum_update_pinned g j k₀ hk₀' x]
    exact Finset.sum_congr rfl fun b _ => hg _

/-- **`lem:qpf-closure`(iii), iterated.**  QPFs are closed under summing out a whole block `S`
of variables: the sum ranges over all `w` agreeing with `x` off `S`. -/
theorem IsQPF.sumOver [DecidableEq ι] (S : Finset ι) {f : (ι → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun x : ι → ZMod 2 =>
      ∑ w : ι → ZMod 2, if (∀ j, j ∉ S → w j = x j) then f w else 0) := by
  refine Finset.induction_on S ?_ ?_
  · refine hf.congr fun x => ?_
    symm
    refine (Finset.sum_eq_single x (fun w _ hw => ?_)
      (fun h => absurd (Finset.mem_univ x) h)).trans ?_
    · exact if_neg fun hcon => hw (funext fun j => hcon j (by simp))
    · exact if_pos fun j _ => rfl
  · intro j₀ S' hj₀ ih
    refine (ih.sumOne j₀).congr fun x => ?_
    rw [Finset.sum_comm]
    refine Finset.sum_congr rfl fun w _ => ?_
    have hiff : ∀ b : ZMod 2,
        (∀ j, j ∉ S' → w j = Function.update x j₀ b j)
          ↔ (b = w j₀ ∧ ∀ j, j ∉ insert j₀ S' → w j = x j) := by
      intro b
      constructor
      · intro h
        have hb : w j₀ = b := (h j₀ hj₀).trans (Function.update_self j₀ b x)
        refine ⟨hb.symm, fun j hj => ?_⟩
        have hjS : j ∉ S' := fun hc => hj (Finset.mem_insert_of_mem hc)
        have hjne : j ≠ j₀ := fun hc => hj (hc ▸ Finset.mem_insert_self j₀ S')
        have hval := h j hjS
        rwa [Function.update_of_ne hjne] at hval
      · rintro ⟨hb, hC⟩ j hjS
        by_cases hj : j = j₀
        · subst hj
          rw [Function.update_self]
          exact hb.symm
        · rw [Function.update_of_ne hj]
          exact hC j fun hc => (Finset.mem_insert.mp hc).elim hj hjS
    by_cases hC : ∀ j, j ∉ insert j₀ S' → w j = x j
    · rw [if_pos hC]
      refine (Finset.sum_eq_single (w j₀) (fun b _ hb => ?_)
        (fun h => absurd (Finset.mem_univ _) h)).trans ?_
      · exact if_neg fun hcon => hb ((hiff b).1 hcon).1
      · exact if_pos ((hiff (w j₀)).2 ⟨rfl, hC⟩)
    · rw [if_neg hC]
      exact Finset.sum_eq_zero fun b _ => if_neg fun hcon => hC ((hiff b).1 hcon).2

/-! ## Changing the index type

Both operations below only pad or delete data; no `eq:xor-lift` correction arises, because the
substituted values are `0`. -/

namespace QPF

/-- Pad a presentation with a block `κ` of coordinates it ignores. -/
def padRight (g : QPF ι) (κ : Type) : QPF (ι ⊕ κ) where
  eps := g.eps
  lin := Sum.elim g.lin 0
  quad := Matrix.of
    (Sum.elim (fun i => Sum.elim (g.quad i) (fun _ => (0 : ZMod 2)))
      (fun _ => (0 : ι ⊕ κ → ZMod 2)))
  K := g.K
  M := Matrix.of fun k => Sum.elim (g.M k) 0
  d := g.d

variable {κ : Type} [Fintype κ]

theorem linVal_padRight (g : QPF ι) (p : ι ⊕ κ → ZMod 2) :
    (g.padRight κ).linVal p = g.linVal (fun i => p (Sum.inl i)) := by
  simp [linVal, padRight, Fintype.sum_sum_type]

theorem quadVal_padRight (g : QPF ι) (p : ι ⊕ κ → ZMod 2) :
    (g.padRight κ).quadVal p = g.quadVal (fun i => p (Sum.inl i)) := by
  simp [quadVal, padRight, Fintype.sum_sum_type]

theorem sat_padRight_iff (g : QPF ι) (p : ι ⊕ κ → ZMod 2) :
    (g.padRight κ).Sat p ↔ g.Sat (fun i => p (Sum.inl i)) := by
  have key : ∀ k : g.K,
      ((g.padRight κ).M.mulVec p) k = (g.M.mulVec fun i => p (Sum.inl i)) k := by
    intro k
    change (∑ a, (g.padRight κ).M k a * p a) = ∑ i, g.M k i * p (Sum.inl i)
    simp [padRight, Fintype.sum_sum_type]
  constructor
  · intro h; funext k; rw [← key k]; exact congrFun h k
  · intro h; funext k; rw [key k]; exact congrFun h k

theorem eval_padRight (g : QPF ι) (p : ι ⊕ κ → ZMod 2) :
    (g.padRight κ).eval p = g.eval (fun i => p (Sum.inl i)) := by
  by_cases hs : g.Sat (fun i => p (Sum.inl i))
  · rw [eval_of_sat ((sat_padRight_iff g p).2 hs), eval_of_sat hs,
      linVal_padRight, quadVal_padRight]
    rfl
  · rw [eval_of_not_sat (fun hc => hs ((sat_padRight_iff g p).1 hc)), eval_of_not_sat hs]

/-- Substitute `0` into the left block of coordinates. -/
def zeroLeft (g : QPF (ι ⊕ κ)) : QPF κ where
  eps := g.eps
  lin := fun k => g.lin (Sum.inr k)
  quad := Matrix.of fun k l => g.quad (Sum.inr k) (Sum.inr l)
  K := g.K
  M := Matrix.of fun k' l => g.M k' (Sum.inr l)
  d := g.d

theorem linVal_zeroLeft (g : QPF (ι ⊕ κ)) (y : κ → ZMod 2) :
    g.zeroLeft.linVal y = g.linVal (Sum.elim 0 y) := by
  simp [linVal, zeroLeft, Fintype.sum_sum_type]

theorem quadVal_zeroLeft (g : QPF (ι ⊕ κ)) (y : κ → ZMod 2) :
    g.zeroLeft.quadVal y = g.quadVal (Sum.elim 0 y) := by
  simp [quadVal, zeroLeft, Fintype.sum_sum_type]

theorem sat_zeroLeft_iff (g : QPF (ι ⊕ κ)) (y : κ → ZMod 2) :
    g.zeroLeft.Sat y ↔ g.Sat (Sum.elim 0 y) := by
  have key : ∀ k : g.K, (g.zeroLeft.M.mulVec y) k = (g.M.mulVec (Sum.elim 0 y)) k := by
    intro k
    change (∑ l, g.M k (Sum.inr l) * y l)
      = ∑ a, g.M k a * (Sum.elim (0 : ι → ZMod 2) y) a
    simp [Fintype.sum_sum_type]
  constructor
  · intro h; funext k; rw [← key k]; exact congrFun h k
  · intro h; funext k; rw [key k]; exact congrFun h k

theorem eval_zeroLeft (g : QPF (ι ⊕ κ)) (y : κ → ZMod 2) :
    g.zeroLeft.eval y = g.eval (Sum.elim 0 y) := by
  by_cases hs : g.Sat (Sum.elim 0 y)
  · rw [eval_of_sat ((sat_zeroLeft_iff g y).2 hs), eval_of_sat hs,
      linVal_zeroLeft, quadVal_zeroLeft]
    rfl
  · rw [eval_of_not_sat (fun hc => hs ((sat_zeroLeft_iff g y).1 hc)), eval_of_not_sat hs]

end QPF

/-- Adding a block of ignored coordinates preserves `IsQPF`. -/
theorem IsQPF.comp_inl {κ : Type} [Fintype κ] {f : (ι → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun p : ι ⊕ κ → ZMod 2 => f (fun i => p (Sum.inl i))) := by
  obtain ⟨g, hg⟩ := hf
  exact ⟨g.padRight κ, fun p => (QPF.eval_padRight g p).trans (hg _)⟩

/-- Substituting `0` into a block of coordinates preserves `IsQPF`. -/
theorem IsQPF.comp_elim_zero {κ : Type} [Fintype κ] {f : (ι ⊕ κ → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun y : κ → ZMod 2 => f (Sum.elim 0 y)) := by
  obtain ⟨g, hg⟩ := hf
  exact ⟨g.zeroLeft, fun y => (QPF.eval_zeroLeft g y).trans (hg _)⟩

/-! ## Summing out a whole block, and the pushforward -/

/-- Reindexing a constrained sum over `F₂^{ι ⊕ κ}` as a sum over `F₂^ι`. -/
theorem sum_sumElim_eq [DecidableEq ι] {κ : Type} [Fintype κ] [DecidableEq κ]
    (F : (ι ⊕ κ → ZMod 2) → ℂ) (y : κ → ZMod 2) :
    (∑ w : ι ⊕ κ → ZMod 2, if (∀ k, w (Sum.inr k) = y k) then F w else 0)
      = ∑ x : ι → ZMod 2, F (Sum.elim x y) := by
  classical
  rw [← Equiv.sum_comp (Equiv.sumArrowEquivProdArrow ι κ (ZMod 2)).symm, Fintype.sum_prod_type]
  refine Finset.sum_congr rfl fun x _ => ?_
  refine (Finset.sum_eq_single y (fun z _ hz => ?_)
    (fun h => absurd (Finset.mem_univ _) h)).trans ?_
  · exact if_neg fun hc => hz (funext (fun k => hc k))
  · exact if_pos fun _ => rfl

/-- **`lem:qpf-closure`(iii), block form.**  Summing a QPF on `F₂^{ι ⊕ κ}` over the whole left
block yields a QPF on `F₂^κ`. -/
theorem IsQPF.sumLeft [DecidableEq ι] {κ : Type} [Fintype κ] [DecidableEq κ]
    {f : (ι ⊕ κ → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun y : κ → ZMod 2 => ∑ x : ι → ZMod 2, f (Sum.elim x y)) := by
  refine (hf.sumOver (Finset.univ.image (Sum.inl : ι → ι ⊕ κ))).comp_elim_zero.congr fun y => ?_
  have hcond : ∀ w : ι ⊕ κ → ZMod 2,
      (∀ j, j ∉ Finset.univ.image (Sum.inl : ι → ι ⊕ κ) →
          w j = (Sum.elim (0 : ι → ZMod 2) y) j)
        ↔ (∀ k, w (Sum.inr k) = y k) := by
    intro w
    constructor
    · intro h k
      exact h (Sum.inr k) (by simp)
    · intro h j hj
      cases j with
      | inl i => exact absurd (Finset.mem_image_of_mem _ (Finset.mem_univ i)) hj
      | inr k => exact h k
  have hterm : ∀ w : ι ⊕ κ → ZMod 2,
      (if (∀ j, j ∉ Finset.univ.image (Sum.inl : ι → ι ⊕ κ) →
          w j = (Sum.elim (0 : ι → ZMod 2) y) j) then f w else 0)
        = (if (∀ k, w (Sum.inr k) = y k) then f w else 0) := by
    intro w
    by_cases hw : ∀ k, w (Sum.inr k) = y k
    · rw [if_pos ((hcond w).2 hw), if_pos hw]
    · rw [if_neg fun hc => hw ((hcond w).1 hc), if_neg hw]
  rw [Finset.sum_congr rfl fun w _ => hterm w]
  exact sum_sumElim_eq f y

/-- **`lem:qpf-closure`(iv).**  For every `F₂`-linear map `Π : F₂^ι → F₂^κ` given by a matrix
`P`, the pushforward `(Π_* f)(y) = ∑_{P x = y} f(x)` of a QPF is again a QPF.

The trick is that over `F₂` the fibre condition `P x = y` is *linear* in the concatenated vector
`(x, y)`, hence is one more affine constraint block on `F₂^{ι ⊕ κ}`; the fibre sum is then just
a sum over the left block (`IsQPF.sumLeft`). -/
theorem IsQPF.pushforward [DecidableEq ι] {κ : Type} [Fintype κ] [DecidableEq κ]
    (P : Matrix κ ι (ZMod 2)) {f : (ι → ZMod 2) → ℂ} (hf : IsQPF f) :
    IsQPF (fun y : κ → ZMod 2 => ∑ x : ι → ZMod 2, if P.mulVec x = y then f x else 0) := by
  obtain ⟨N, hrow⟩ : ∃ N : Matrix κ (ι ⊕ κ) (ZMod 2),
      ∀ (x : ι → ZMod 2) (y : κ → ZMod 2) (k : κ),
        (N.mulVec (Sum.elim x y)) k = (P.mulVec x) k + y k := by
    refine ⟨Matrix.of fun k => Sum.elim (P k) (fun k' => if k' = k then 1 else 0), ?_⟩
    intro x y k
    change (∑ a, (Sum.elim (P k) (fun k' => if k' = k then (1 : ZMod 2) else 0)) a
      * (Sum.elim x y) a) = (∑ i, P k i * x i) + y k
    rw [Fintype.sum_sum_type]
    congr 1
    simp
  have hiff : ∀ (x : ι → ZMod 2) (y : κ → ZMod 2),
      (N.mulVec (Sum.elim x y) = 0) ↔ (P.mulVec x = y) := by
    have key : ∀ a b : ZMod 2, (a + b = 0) ↔ (a = b) := by decide
    intro x y
    constructor
    · intro h
      funext k
      refine (key _ _).1 ?_
      rw [← hrow x y k]
      exact congrFun h k
    · intro h
      funext k
      rw [hrow x y k]
      exact (key _ _).2 (congrFun h k)
  refine (hf.comp_inl.mul (isQPF_affineSystem N 0)).sumLeft.congr fun y => ?_
  refine Finset.sum_congr rfl fun x _ => ?_
  by_cases h : P.mulVec x = y
  · rw [if_pos ((hiff x y).2 h), if_pos h, mul_one]
    rfl
  · rw [if_neg fun hc => h ((hiff x y).1 hc), if_neg h, mul_zero]

end Stab
end Formal
