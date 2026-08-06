/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Core.LinearLayout
import Formal.Quantum.Capstone

/-!
# Paper-facing statements

This module packages the independently proved correctness and operation-count lemmas into
statements whose assumptions and conclusions match the Lean badges in the paper.  It adds no
new mathematical assumptions: each theorem below is a short composition of results in the
core or quantum layers.
-/

namespace Formal
namespace SopInstance

variable (I : SopInstance)

/-- **`thm:sop-rw`, formalized clauses.** The full rank-decomposition DP returns every
residue count, each subtree table has at most `r * 2^k` occupied states, and the semantic
initialization/join-pair count is at most `|V| * (2 + (r * 2^k)^2)`. -/
theorem rank_dp_spec (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k) :
    (∀ j : ZMod I.r, I.Dalg D.tree 0 (j - I.c) = I.N j)
      ∧ (∀ u : RTree I.V, RTree.Subtree u D.tree → I.statesFull u ≤ I.r * 2 ^ k)
      ∧ I.costFull D.tree
          ≤ Fintype.card I.V * (2 + (I.r * 2 ^ k) * (I.r * 2 ^ k)) := by
  constructor
  · exact fun j => I.dp_returns_counts D j
  constructor
  · exact fun u hu => I.statesFull_le (hw u hu)
  · exact I.costFull_le D hw

/-- **`thm:fourier-speedup`, per-mode formalized clause.** Mode `a` computes the exact
Fourier sum and scans at most `|V| * (2 + 4^k)` initialization/join pairs. Fourier
inversion itself is `N_inversion`. The choice and machine cost of an FFT are paper-level. -/
theorem fourier_mode_spec (D : RankDecomp I) {k : ℕ} (hw : I.WidthBounded D k)
    (a : ZMod I.r) :
    I.chi (a * I.c) * I.Aalg a D.tree 0 = I.Nhat a
      ∧ I.costMode D.tree ≤ Fintype.card I.V * (2 + 4 ^ k) := by
  exact ⟨I.Aalg_root D a, I.costMode_le' D hw⟩

/-- **`thm:linear-layout-fourier`, formalized clause.** For every (including zero) prefix
cut-rank bound `k`, a covering duplicate-free layout computes mode `a` correctly and has
operation count at most `n * (2 + 2 * 2^k)`, where `n = l.length + 1`. -/
theorem linear_fourier_mode_spec (v : I.V) (l : List I.V) (hnd : (v :: l).Nodup)
    (hcov : (I.caterpillar v l).verts = Finset.univ) {k : ℕ}
    (hk : I.LayoutWidthLe v l k) (a : ZMod I.r) :
    I.chi (a * I.c) * I.Aalg a (I.caterpillar v l) 0 = I.Nhat a
      ∧ I.costMode (I.caterpillar v l) ≤ (l.length + 1) * (2 + 2 * 2 ^ k) := by
  constructor
  · exact I.Aalg_root (I.layoutDecomp v l hnd hcov) a
  · exact I.costMode_layout_le_all_k v l hnd hcov hk

end SopInstance

namespace Quantum

/-- **The paper's circuit-to-SOP identity.** The compiler normalization is rewritten as the
paper's `R_C = (sqrt 2)^{hadamardCount C}`. The term `constDelta` is the consistency
indicator for Hadamard-free wires. -/
theorem amplitude_eq_sop_normalized {n : ℕ} (C : Circuit n) (y z : Fin n → ZMod 2) :
    amplitude C y z
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ hadamardCount C)⁻¹
        * ((Sym.compile y C).constDelta y z * (circuitInstance C y z).S) := by
  rw [amplitude_eq_sop C y z, compile_mH_eq_hadamardCount C y]

/-- **`sec:rw-fpt`, exact capstone.** The mode-1 rank-decomposition DP computes the
physical amplitude with the paper's normalization and consistency factor, while satisfying
the exact `|V| * (2 + 4^k)` operation-count bound. -/
theorem amplitude_by_rank_dp_normalized {n : ℕ} (C : Circuit n)
    (y z : Fin n → ZMod 2) (D : RankDecomp (circuitInstance C y z)) {k : ℕ}
    (hw : (circuitInstance C y z).WidthBounded D k) :
    amplitude C y z
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ hadamardCount C)⁻¹
        * ((Sym.compile y C).constDelta y z
        * ((circuitInstance C y z).chi (circuitInstance C y z).c
            * (circuitInstance C y z).Aalg 1 D.tree 0))
      ∧ (circuitInstance C y z).costMode D.tree
          ≤ Fintype.card (circuitInstance C y z).V * (2 + 4 ^ k) := by
  have h := amplitude_by_rank_dp C y z D hw
  rw [compile_mH_eq_hadamardCount C y] at h
  exact h

end Quantum
end Formal
