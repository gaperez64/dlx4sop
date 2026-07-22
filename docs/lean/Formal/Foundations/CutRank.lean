/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Mathlib

/-!
# The signature-space cardinality bound (`lem:signatures-le-2^k`)

The single fact powering every runtime theorem of the paper: the set of boundary signatures
occurring at a cut is the row space of the cut matrix over `F₂`, so its size is exactly
`2 ^ rank` — and hence at most `2 ^ k` for a width-`k` decomposition.

Here we prove the pure matrix statement: the image of `z ↦ z ᵥ* M` over all `z : m → ZMod 2`
has cardinality `2 ^ M.rank`. The paper re-proves this inline in both `thm:sop-rw` and
`thm:fourier-speedup`; we factor it out once (plan: "factor out `lem:signatures-le-2^k`").
-/

open scoped BigOperators

namespace Formal

/-- **Row-space cardinality.** Over `F₂`, the image of the row-vector–matrix product map
`z ↦ z ᵥ* M` has exactly `2 ^ M.rank` elements. -/
theorem card_image_vecMul (m n : Type) [Fintype m] [Fintype n] [DecidableEq m]
    (M : Matrix m n (ZMod 2)) :
    (Finset.univ.image (fun z : m → ZMod 2 => Matrix.vecMul z M)).card = 2 ^ M.rank := by
  classical
  -- The image finset is the set-range, and the set-range is the linear range's carrier.
  have himg : (Finset.univ.image (fun z : m → ZMod 2 => Matrix.vecMul z M))
      = (Set.range (fun z : m → ZMod 2 => Matrix.vecMul z M)).toFinset := by
    rw [Set.toFinset_range]
  have hrange : (Set.range (fun z : m → ZMod 2 => Matrix.vecMul z M))
      = ↑(LinearMap.range M.vecMulLinear) := by
    rw [LinearMap.coe_range, Matrix.coe_vecMulLinear]
  rw [himg, Set.toFinset_card]
  -- Cardinality of the range submodule: `2 ^ finrank`.
  have hcard : Fintype.card (Set.range (fun z : m → ZMod 2 => Matrix.vecMul z M))
      = Fintype.card (ZMod 2) ^ Module.finrank (ZMod 2) (LinearMap.range M.vecMulLinear) := by
    rw [Fintype.card_congr (Equiv.setCongr hrange)]
    exact Module.card_eq_pow_finrank
  rw [hcard, ZMod.card 2]
  -- `finrank (range vecMulLinear) = rank` via the row-space characterization.
  congr 1
  rw [range_vecMulLinear, Matrix.rank_eq_finrank_span_row]

/-- The `≤ 2^k` form used by the runtime theorems: a width bound on the cut matrix bounds
the number of occurring signatures. -/
theorem card_image_vecMul_le (m n : Type) [Fintype m] [Fintype n] [DecidableEq m]
    (M : Matrix m n (ZMod 2)) {k : ℕ} (hk : M.rank ≤ k) :
    (Finset.univ.image (fun z : m → ZMod 2 => Matrix.vecMul z M)).card ≤ 2 ^ k := by
  rw [card_image_vecMul]
  exact Nat.pow_le_pow_right (by norm_num) hk

end Formal
