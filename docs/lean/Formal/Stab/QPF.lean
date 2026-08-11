/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Mathlib

/-!
# Quadratic phase functions (`def:qpf`)

A *quadratic phase function* (QPF) on `F₂^ι` is

  `g(x) = ε · i^{ℓ(x)} · (−1)^{q(x)} · [x ∈ K]`,

presented by four pieces of data: a scalar `ε : ℂ`, a `ZMod 4`-linear part `ℓ`, an
`F₂`-quadratic part `q`, and an affine subspace `K = {x | M x = d}`.  Up to normalization
these are the amplitude functions of stabilizer states supported on affine subspaces, in the
phase-sensitive form of CH-form simulators.  `Formal.Stab.Closure` proves the closure
properties (`lem:qpf-closure`) that make the join of `lem:stab-join` possible.

This file holds the linchpin definitions and the easy half of the closure lemma:
closure under pointwise products (`lem:qpf-closure`(i), `QPF.eval_mul`), together with the
two elementary phase identities `eq:xor-lift` (`lift4_add`) and the doubling rule
`zeta4_two_mul` that drive everything else.

## Design notes

* The quadratic part is presented by a full coefficient **matrix** and evaluated over *all*
  ordered pairs `(i,j)`.  Over `F₂` this realises exactly the paper's
  `∑_{i<j} q_{ij} x_i x_j` — the diagonal supplies linear terms, since `x² = x` — and it
  needs no ordering of `ι`.  QPFs therefore live over an arbitrary finite index type, and
  the SOP layer can use its own variable type `I.V` directly, with no `Fin n` transport.
* The constraint index type `K` is a *field* of the structure, so stacking two constraint
  systems is just `⊕` (`SopInstance` bundles its vertex type `V` the same way).
* `IsQPF f` is the predicate "some data presents `f`".  Closure statements are phrased with
  it, so a proof may pick whatever presentation is convenient.
* The paper's `O(m²)` description size and `O(m³)` running time are **not** formalized.  As
  elsewhere in this development (see the README's scope section) runtime is modelled by
  operation counts — here those of `Formal.Stab.Hybrid` — and never by a machine model.
-/

open scoped BigOperators Matrix

namespace Formal
namespace Stab

/-! ## The two phase characters -/

/-- The character `a ↦ i^a` of `ZMod 4`. -/
noncomputable def zeta4 (a : ZMod 4) : ℂ := Complex.I ^ a.val

/-- The character `a ↦ (−1)^a` of `ZMod 2`. -/
noncomputable def sgn (a : ZMod 2) : ℂ := (-1 : ℂ) ^ a.val

theorem I_pow_four : Complex.I ^ (4 : ℕ) = 1 := by
  have h : Complex.I ^ (4 : ℕ) = (Complex.I ^ (2 : ℕ)) ^ (2 : ℕ) := by ring
  rw [h, Complex.I_sq]
  norm_num

/-- Powers of `i` only see the exponent mod `4` (cf. `SopInstance.omega_pow_mod`). -/
theorem I_pow_mod (n : ℕ) : Complex.I ^ (n % 4) = Complex.I ^ n := by
  conv_rhs => rw [← Nat.div_add_mod n 4]
  rw [pow_add, pow_mul, I_pow_four, one_pow, one_mul]

theorem neg_one_pow_mod (n : ℕ) : (-1 : ℂ) ^ (n % 2) = (-1 : ℂ) ^ n := by
  conv_rhs => rw [← Nat.div_add_mod n 2]
  rw [pow_add, pow_mul]
  norm_num

@[simp] theorem zeta4_zero : zeta4 0 = 1 := by simp [zeta4]

/-- `ζ₄` is a character of `(ZMod 4, +)`. -/
theorem zeta4_add (a b : ZMod 4) : zeta4 (a + b) = zeta4 a * zeta4 b := by
  simp only [zeta4]
  rw [ZMod.val_add, I_pow_mod, pow_add]

theorem zeta4_ne_zero (a : ZMod 4) : zeta4 a ≠ 0 :=
  pow_ne_zero _ Complex.I_ne_zero

@[simp] theorem sgn_zero : sgn 0 = 1 := by simp [sgn]

/-- `sgn` is a character of `(ZMod 2, +)`. -/
theorem sgn_add (a b : ZMod 2) : sgn (a + b) = sgn a * sgn b := by
  simp only [sgn]
  rw [ZMod.val_add, neg_one_pow_mod, pow_add]

theorem sgn_ne_zero (a : ZMod 2) : sgn a ≠ 0 :=
  pow_ne_zero _ (by norm_num)

/-- **Doubling rule.** Doubling in `ZMod 4` turns the `i`-character into the sign character:
`i^{2a} = (−1)^a`.  This is what makes `(−1)^{q}` and `i^{ℓ}` interconvertible, and it is the
reason the quadratic corrections produced by `lift4_add` stay inside the class. -/
theorem zeta4_two_mul (a : ZMod 4) : zeta4 (2 * a) = sgn ((a.val : ZMod 2)) := by
  have h : ∀ a : ZMod 4, (2 * a).val = 2 * ((a.val : ZMod 2)).val := by decide
  rw [zeta4, sgn, h a, pow_mul, Complex.I_sq]

/-! ## The `0/1` lift and `eq:xor-lift` -/

/-- The `0/1` integer lift of an `F₂` coordinate, viewed in `ZMod 4`. -/
def lift4 (b : ZMod 2) : ZMod 4 := (b.val : ZMod 4)

@[simp] theorem lift4_zero : lift4 0 = 0 := rfl

@[simp] theorem lift4_one : lift4 1 = 1 := rfl

/-- **`eq:xor-lift`, arithmetic core.**  On the `0/1` lifts, `x ⊕ y = x + y − 2xy`; since
`−2 = 2` in `ZMod 4` this reads `lift(x+y) = lift x + lift y + 2·lift x·lift y`.  Passing to
`⊕`-coordinates therefore creates only a *quadratic* correction, which `zeta4_two_mul` turns
into a sign — the one-line reason the class is closed under affine substitution. -/
theorem lift4_add (x y : ZMod 2) :
    lift4 (x + y) = lift4 x + lift4 y + 2 * (lift4 x * lift4 y) := by
  revert x y
  decide

/-- The lift is a section of reduction mod `2`: `(lift4 b).val = b` in `ZMod 2`. -/
@[simp] theorem cast_val_lift4 (b : ZMod 2) : (((lift4 b).val : ℕ) : ZMod 2) = b := by
  revert b
  decide

/-! ## Quadratic phase functions -/

/-- **`def:qpf`.**  Data presenting a quadratic phase function
`g(x) = ε · i^{ℓ(x)} · (−1)^{q(x)} · [x ∈ K]` on `F₂^ι`. -/
structure QPF (ι : Type) where
  /-- the scalar `ε` -/
  eps : ℂ
  /-- coefficients `c_j ∈ ZMod 4` of the linear part `ℓ(x) = ∑_j c_j x_j` -/
  lin : ι → ZMod 4
  /-- coefficients of the `F₂`-quadratic part `q(x) = ∑_{i,j} q_{ij} x_i x_j` -/
  quad : Matrix ι ι (ZMod 2)
  /-- index type of the affine constraints presenting `K` -/
  K : Type
  /-- the constraint matrix `M` -/
  M : Matrix K ι (ZMod 2)
  /-- the constraint right-hand side `d` -/
  d : K → ZMod 2

namespace QPF

variable {ι : Type} [Fintype ι]

/-- `ℓ(x) = ∑_j c_j x_j`, in `ZMod 4`. -/
def linVal (g : QPF ι) (x : ι → ZMod 2) : ZMod 4 := ∑ j, g.lin j * lift4 (x j)

/-- `q(x) = ∑_{i,j} q_{ij} x_i x_j`, in `ZMod 2`. -/
def quadVal (g : QPF ι) (x : ι → ZMod 2) : ZMod 2 := ∑ i, ∑ j, g.quad i j * x i * x j

/-- Membership `x ∈ K`, i.e. `M x = d`. -/
def Sat (g : QPF ι) (x : ι → ZMod 2) : Prop := g.M.mulVec x = g.d

open Classical in
/-- The function presented by the data: `ε · i^{ℓ(x)} · (−1)^{q(x)} · [x ∈ K]`. -/
noncomputable def eval (g : QPF ι) (x : ι → ZMod 2) : ℂ :=
  if g.Sat x then g.eps * zeta4 (g.linVal x) * sgn (g.quadVal x) else 0

theorem eval_of_sat {g : QPF ι} {x : ι → ZMod 2} (h : g.Sat x) :
    g.eval x = g.eps * zeta4 (g.linVal x) * sgn (g.quadVal x) := by
  classical
  rw [eval, if_pos h]

theorem eval_of_not_sat {g : QPF ι} {x : ι → ZMod 2} (h : ¬ g.Sat x) : g.eval x = 0 := by
  classical
  rw [eval, if_neg h]

/-! ### Unconstrained phases, constants and point tables -/

/-- The unconstrained phase `ε · i^{ℓ(x)} · (−1)^{q(x)}` (empty constraint system). -/
def phase (eps : ℂ) (lin : ι → ZMod 4) (quad : Matrix ι ι (ZMod 2)) : QPF ι where
  eps := eps
  lin := lin
  quad := quad
  K := PEmpty
  M := Matrix.of fun (k : PEmpty) => k.elim
  d := fun (k : PEmpty) => k.elim

theorem sat_phase (eps : ℂ) (lin : ι → ZMod 4) (quad : Matrix ι ι (ZMod 2))
    (x : ι → ZMod 2) : (phase eps lin quad).Sat x := by
  unfold Sat
  funext k
  exact k.elim

@[simp] theorem eval_phase (eps : ℂ) (lin : ι → ZMod 4) (quad : Matrix ι ι (ZMod 2))
    (x : ι → ZMod 2) :
    (phase eps lin quad).eval x
      = eps * zeta4 (∑ j, lin j * lift4 (x j)) * sgn (∑ i, ∑ j, quad i j * x i * x j) :=
  eval_of_sat (sat_phase eps lin quad x)

/-- The constant function `x ↦ c`. -/
def const (c : ℂ) : QPF ι := phase c 0 0

@[simp] theorem eval_const (c : ℂ) (x : ι → ZMod 2) : (const c : QPF ι).eval x = c := by
  simp [const]

/-- The **point table** `[x = σ]`: the indicator of a single signature.  Its join with
another point table is the naive join of `thm:fourier-speedup`. -/
def point [DecidableEq ι] (σ : ι → ZMod 2) : QPF ι where
  eps := 1
  lin := 0
  quad := 0
  K := ι
  M := 1
  d := σ

theorem sat_point_iff [DecidableEq ι] (σ x : ι → ZMod 2) :
    (point σ).Sat x ↔ x = σ := by
  unfold Sat point
  simp [Matrix.one_mulVec]

@[simp] theorem eval_point [DecidableEq ι] (σ x : ι → ZMod 2) :
    (point σ).eval x = if x = σ then 1 else 0 := by
  by_cases h : x = σ
  · rw [eval_of_sat ((sat_point_iff σ x).mpr h), if_pos h]
    simp [point, linVal, quadVal]
  · rw [eval_of_not_sat (fun hc => h ((sat_point_iff σ x).mp hc)), if_neg h]

/-! ### `lem:qpf-closure`(i): pointwise products -/

/-- Pointwise product of presentations: the data add and the constraint systems stack. -/
def mul (g h : QPF ι) : QPF ι where
  eps := g.eps * h.eps
  lin := g.lin + h.lin
  quad := g.quad + h.quad
  K := g.K ⊕ h.K
  M := Matrix.of (Sum.elim g.M h.M)
  d := Sum.elim g.d h.d

omit [Fintype ι] in
@[simp] theorem mul_eps (g h : QPF ι) : (g.mul h).eps = g.eps * h.eps := rfl

omit [Fintype ι] in
@[simp] theorem mul_lin (g h : QPF ι) : (g.mul h).lin = g.lin + h.lin := rfl

omit [Fintype ι] in
@[simp] theorem mul_quad (g h : QPF ι) : (g.mul h).quad = g.quad + h.quad := rfl

@[simp] theorem linVal_mul (g h : QPF ι) (x : ι → ZMod 2) :
    (g.mul h).linVal x = g.linVal x + h.linVal x := by
  simp only [linVal, mul_lin, Pi.add_apply, add_mul]
  exact Finset.sum_add_distrib

@[simp] theorem quadVal_mul (g h : QPF ι) (x : ι → ZMod 2) :
    (g.mul h).quadVal x = g.quadVal x + h.quadVal x := by
  simp only [quadVal, mul_quad, Matrix.add_apply]
  rw [← Finset.sum_add_distrib]
  refine Finset.sum_congr rfl fun i _ => ?_
  rw [← Finset.sum_add_distrib]
  exact Finset.sum_congr rfl fun j _ => by ring

theorem sat_mul_iff (g h : QPF ι) (x : ι → ZMod 2) :
    (g.mul h).Sat x ↔ (g.Sat x ∧ h.Sat x) := by
  constructor
  · intro hx
    exact ⟨funext fun a => congrFun hx (Sum.inl a), funext fun b => congrFun hx (Sum.inr b)⟩
  · rintro ⟨h1, h2⟩
    funext i
    cases i with
    | inl a => exact congrFun h1 a
    | inr b => exact congrFun h2 b

/-- **`lem:qpf-closure`(i).**  QPFs are closed under pointwise products. -/
theorem eval_mul (g h : QPF ι) (x : ι → ZMod 2) :
    (g.mul h).eval x = g.eval x * h.eval x := by
  by_cases hg : g.Sat x
  · by_cases hh : h.Sat x
    · rw [eval_of_sat ((sat_mul_iff g h x).mpr ⟨hg, hh⟩), eval_of_sat hg, eval_of_sat hh,
        linVal_mul, quadVal_mul, zeta4_add, sgn_add, mul_eps]
      ring
    · rw [eval_of_not_sat fun hc => hh ((sat_mul_iff g h x).mp hc).2, eval_of_not_sat hh,
        mul_zero]
  · rw [eval_of_not_sat fun hc => hg ((sat_mul_iff g h x).mp hc).1, eval_of_not_sat hg,
      zero_mul]

/-! ### Scaling -/

/-- Scaling by a complex constant, absorbed into `ε`. -/
def smul (c : ℂ) (g : QPF ι) : QPF ι := { g with eps := c * g.eps }

@[simp] theorem eval_smul (c : ℂ) (g : QPF ι) (x : ι → ZMod 2) :
    (smul c g).eval x = c * g.eval x := by
  by_cases hg : g.Sat x
  · rw [eval_of_sat (show (smul c g).Sat x from hg), eval_of_sat hg]
    change c * g.eps * zeta4 (g.linVal x) * sgn (g.quadVal x)
      = c * (g.eps * zeta4 (g.linVal x) * sgn (g.quadVal x))
    ring
  · rw [eval_of_not_sat (show ¬ (smul c g).Sat x from hg), eval_of_not_sat hg, mul_zero]

/-! ### Relabelling coordinates -/

/-- Relabelling the coordinates along a bijection of index types. -/
def reindex {ι' : Type} (e : ι ≃ ι') (g : QPF ι) : QPF ι' where
  eps := g.eps
  lin := fun i => g.lin (e.symm i)
  quad := Matrix.of fun i j => g.quad (e.symm i) (e.symm j)
  K := g.K
  M := Matrix.of fun k i => g.M k (e.symm i)
  d := g.d

variable {ι' : Type} [Fintype ι']

theorem linVal_reindex (e : ι ≃ ι') (g : QPF ι) (y : ι' → ZMod 2) :
    (g.reindex e).linVal y = g.linVal (fun i => y (e i)) := by
  simp only [linVal, reindex]
  rw [← Equiv.sum_comp e]
  exact Finset.sum_congr rfl fun j _ => by rw [Equiv.symm_apply_apply]

theorem quadVal_reindex (e : ι ≃ ι') (g : QPF ι) (y : ι' → ZMod 2) :
    (g.reindex e).quadVal y = g.quadVal (fun i => y (e i)) := by
  simp only [quadVal, reindex, Matrix.of_apply]
  rw [← Equiv.sum_comp e]
  refine Finset.sum_congr rfl fun i _ => ?_
  rw [← Equiv.sum_comp e]
  exact Finset.sum_congr rfl fun j _ => by rw [Equiv.symm_apply_apply, Equiv.symm_apply_apply]

theorem sat_reindex_iff (e : ι ≃ ι') (g : QPF ι) (y : ι' → ZMod 2) :
    (g.reindex e).Sat y ↔ g.Sat (fun i => y (e i)) := by
  have key : ∀ k : g.K,
      ((g.reindex e).M.mulVec y) k = (g.M.mulVec fun i => y (e i)) k := by
    intro k
    change ∑ i, g.M k (e.symm i) * y i = ∑ j, g.M k j * y (e j)
    rw [← Equiv.sum_comp e]
    exact Finset.sum_congr rfl fun j _ => by rw [Equiv.symm_apply_apply]
  constructor
  · intro h; funext k; rw [← key k]; exact congrFun h k
  · intro h; funext k; rw [key k]; exact congrFun h k

theorem eval_reindex (e : ι ≃ ι') (g : QPF ι) (y : ι' → ZMod 2) :
    (g.reindex e).eval y = g.eval (fun i => y (e i)) := by
  by_cases hg : g.Sat (fun i => y (e i))
  · rw [eval_of_sat ((sat_reindex_iff e g y).mpr hg), eval_of_sat hg,
      linVal_reindex, quadVal_reindex]
    rfl
  · rw [eval_of_not_sat fun hc => hg ((sat_reindex_iff e g y).mp hc), eval_of_not_sat hg]

end QPF

/-! ## The predicate `IsQPF` -/

/-- `IsQPF f`: the function `f : F₂^ι → ℂ` *is* a quadratic phase function, i.e. some data
presents it.  Closure statements are phrased with this predicate, so that a proof may pick
whichever presentation is convenient. -/
def IsQPF {ι : Type} [Fintype ι] (f : (ι → ZMod 2) → ℂ) : Prop :=
  ∃ g : QPF ι, ∀ x, g.eval x = f x

variable {ι : Type} [Fintype ι]

theorem IsQPF.congr {f f' : (ι → ZMod 2) → ℂ} (hf : IsQPF f) (h : ∀ x, f x = f' x) :
    IsQPF f' := by
  obtain ⟨g, hg⟩ := hf
  exact ⟨g, fun x => (hg x).trans (h x)⟩

theorem isQPF_const (c : ℂ) : IsQPF (fun _ : ι → ZMod 2 => c) :=
  ⟨QPF.const c, fun x => QPF.eval_const c x⟩

theorem isQPF_zero : IsQPF (fun _ : ι → ZMod 2 => (0 : ℂ)) := isQPF_const 0

theorem isQPF_one : IsQPF (fun _ : ι → ZMod 2 => (1 : ℂ)) := isQPF_const 1

theorem isQPF_phase (eps : ℂ) (lin : ι → ZMod 4) (quad : Matrix ι ι (ZMod 2)) :
    IsQPF (fun x : ι → ZMod 2 =>
      eps * zeta4 (∑ j, lin j * lift4 (x j)) * sgn (∑ i, ∑ j, quad i j * x i * x j)) :=
  ⟨QPF.phase eps lin quad, fun x => QPF.eval_phase eps lin quad x⟩

theorem isQPF_point (σ : ι → ZMod 2) :
    IsQPF (fun x : ι → ZMod 2 => if x = σ then (1 : ℂ) else 0) := by
  classical
  exact ⟨QPF.point σ, fun x => by simp⟩

/-- **`lem:qpf-closure`(i).**  QPFs are closed under pointwise products. -/
theorem IsQPF.mul {f f' : (ι → ZMod 2) → ℂ} (hf : IsQPF f) (hf' : IsQPF f') :
    IsQPF (fun x => f x * f' x) := by
  obtain ⟨g, hg⟩ := hf
  obtain ⟨g', hg'⟩ := hf'
  exact ⟨g.mul g', fun x => by rw [QPF.eval_mul, hg, hg']⟩

theorem IsQPF.smul {f : (ι → ZMod 2) → ℂ} (c : ℂ) (hf : IsQPF f) :
    IsQPF (fun x => c * f x) := by
  obtain ⟨g, hg⟩ := hf
  exact ⟨QPF.smul c g, fun x => by rw [QPF.eval_smul, hg]⟩

theorem IsQPF.reindex {ι' : Type} [Fintype ι'] (e : ι ≃ ι') {f : (ι → ZMod 2) → ℂ}
    (hf : IsQPF f) : IsQPF (fun y : ι' → ZMod 2 => f (fun i => y (e i))) := by
  obtain ⟨g, hg⟩ := hf
  exact ⟨g.reindex e, fun y => by rw [QPF.eval_reindex, hg]⟩

end Stab
end Formal
