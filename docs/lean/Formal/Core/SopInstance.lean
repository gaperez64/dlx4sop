/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Mathlib

/-!
# Abstract quadratic sum-of-powers instances

This file formalizes the *general quadratic SOP* of the paper (eq:sop / def:sopcount):
a graph `G = (V,E)`, an even modulus `r`, unary coefficients `b : V → ZMod r`, and a
constant `c : ZMod r`, defining

  `f(x) = c + ∑_v b_v x_v + (r/2) ∑_{{u,v}∈E} x_u x_v   (mod r)`,
  `S(f) = ∑_{x ∈ {0,1}^V} ω_r^{f(x)}`,     `N_j = #{x | f(x) = j}`.

Design (see DESIGN.md): assignments are `x : V → ZMod 2`; the edge count enters `f`
faithfully as an integer count `selCount`, and the load-bearing identity
`(r/2)·m ≡ (r/2)·(m % 2) (mod r)` (from `Even r`) is proved once as `half_mul_reduce`.
-/

open scoped BigOperators

namespace Formal

/-- An abstract quadratic sum-of-powers instance. Bundles the modeling content as fields
(playbook §1). The invariant `Even r` (hence `η = r/2`) is what makes each cross-term a sign. -/
structure SopInstance where
  /-- the (even, positive) modulus -/
  r : ℕ
  hr : Even r
  hr0 : 0 < r
  /-- the variable set -/
  V : Type
  fV : Fintype V
  dV : DecidableEq V
  /-- the SOP variable graph -/
  G : SimpleGraph V
  dG : DecidableRel G.Adj
  eG : Fintype G.edgeSet
  /-- unary coefficients -/
  b : V → ZMod r
  /-- the constant term -/
  c : ZMod r

attribute [instance] SopInstance.fV SopInstance.dV SopInstance.dG SopInstance.eG

namespace SopInstance

variable (I : SopInstance)

instance : NeZero I.r := ⟨by have h := I.hr0; omega⟩

/-- Selection product on an edge `{a,b}`: `x a * x b` in `ZMod 2` (symmetric in `a,b`). -/
def edgeProd (x : I.V → ZMod 2) (e : Sym2 I.V) : ZMod 2 :=
  Sym2.lift ⟨fun a b => x a * x b, fun _ _ => mul_comm _ _⟩ e

/-- The integer number of selected edges: edges both of whose endpoints carry `x = 1`. -/
def selCount (x : I.V → ZMod 2) : ℕ :=
  (I.G.edgeFinset.filter (fun e => I.edgeProd x e = 1)).card

/-- The pinned phase polynomial `f`, valued in `ZMod r`. The quadratic term is the honest
integer edge count scaled by `η = r/2`. -/
def f (x : I.V → ZMod 2) : ZMod I.r :=
  I.c + (∑ v, I.b v * ((x v).val : ZMod I.r))
      + ((I.r / 2 : ℕ) : ZMod I.r) * (I.selCount x : ZMod I.r)

/-- The primitive `r`-th root of unity `ω_r = exp(2πi/r)`. -/
noncomputable def omega : ℂ := Complex.exp (2 * Real.pi * Complex.I / I.r)

/-- The unnormalized SOP value `S(f) = ∑_x ω_r^{f(x)}`. -/
noncomputable def S : ℂ := ∑ x : I.V → ZMod 2, I.omega ^ (I.f x).val

/-- The residue histogram `N_j = #{x | f(x) = j}` (def:sopcount). -/
def N (j : ZMod I.r) : ℕ := (Finset.univ.filter (fun x : I.V → ZMod 2 => I.f x = j)).card

end SopInstance

/-- **The `η = r/2` reduction.** For even `r`, scaling by `r/2` only sees the parity of the
multiplier: `(r/2)·m ≡ (r/2)·(m % 2) (mod r)`. This is the algebraic fact behind the join
step of the DP (each selected crossing edge contributes a bare sign). -/
theorem half_mul_reduce {r : ℕ} (hr : Even r) (m : ℕ) :
    ((r / 2 : ℕ) : ZMod r) * (m : ZMod r) = ((r / 2 : ℕ) : ZMod r) * ((m % 2 : ℕ) : ZMod r) := by
  obtain ⟨k, hk⟩ := hr
  -- r = 2k, so r/2 = k and (r/2)*2 = r ≡ 0.
  have hr2 : r / 2 = k := by omega
  have key : ((r / 2 : ℕ) : ZMod r) * (2 : ZMod r) = 0 := by
    have : ((r / 2 : ℕ) * 2 : ℕ) = r := by omega
    calc ((r / 2 : ℕ) : ZMod r) * (2 : ZMod r)
        = (((r / 2 : ℕ) * 2 : ℕ) : ZMod r) := by push_cast; ring
      _ = ((r : ℕ) : ZMod r) := by rw [this]
      _ = 0 := ZMod.natCast_self r
  -- write m = 2*(m/2) + m%2
  have hm : m = 2 * (m / 2) + m % 2 := by omega
  calc ((r / 2 : ℕ) : ZMod r) * (m : ZMod r)
      = ((r / 2 : ℕ) : ZMod r) * ((2 * (m / 2) + m % 2 : ℕ) : ZMod r) := by rw [← hm]
    _ = ((r / 2 : ℕ) : ZMod r) * (2 : ZMod r) * ((m / 2 : ℕ) : ZMod r)
          + ((r / 2 : ℕ) : ZMod r) * ((m % 2 : ℕ) : ZMod r) := by push_cast; ring
    _ = ((r / 2 : ℕ) : ZMod r) * ((m % 2 : ℕ) : ZMod r) := by rw [key]; ring

end Formal
