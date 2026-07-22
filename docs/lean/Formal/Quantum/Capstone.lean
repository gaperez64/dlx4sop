/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.Coupling
import Formal.Quantum.PinningLemmas
import Formal.Core.Fourier
import Formal.Core.Cost

/-!
# Capstone: rank-width FPT strong simulation of `{H, T, CZ}` circuits

The honest end-to-end statements about the *real* circuit amplitude (playbook §1):

* `amplitude_eq_sop` — the paper's coupling (sec:sop): for every circuit `C` over `{H,T,CZ}`
  and computational-basis pins `y, z`,
  `⟨z|C|y⟩ = R_C⁻¹ · δ_C · S(f_C)` where `R_C = (√2)^{m_H}`, `δ_C` is the Kronecker delta of
  the Hadamard-free wires, and `S(f_C)` is the unnormalized quadratic-SOP value of the pinned
  instance (the SOP variable graph `G_C` with its coefficients).
* `amplitude_by_rank_dp` — sec:rw-fpt: given a rank-decomposition of `G_C`, the single-mode
  Fourier DP *computes* the amplitude (`cor:single-amplitude` applied to `G_C`), and its
  operation count is bounded by `|V|·(2 + 4^k)` (`thm:fourier-speedup` per-mode bound).

Since `S = T²`, circuits over `{H,T,CZ}` include all of Clifford+T (sec:clifford-t); the
statements below therefore cover Clifford+T strong simulation.
-/

namespace Formal
namespace Quantum

open Sym

/-- The pinned SOP instance of a circuit (the SOP variable graph `G_C` with coefficients).
Reducible so the coupling's `.S` matches the compiled `pinnedInstance` transparently. -/
@[reducible] noncomputable def circuitInstance {n : ℕ} (C : Circuit n) (y z : Fin n → ZMod 2) :
    SopInstance :=
  (Sym.compile y C).pinnedInstance (eparSym_compile y C) z

/-- **The coupling** (main.tex sec:sop, "the pinned SOP"): the amplitude of a circuit over
`{H,T,CZ}` equals the Hadamard normalization times the constant-wire delta times the
unnormalized SOP value of its pinned quadratic SOP instance. -/
theorem amplitude_eq_sop {n : ℕ} (C : Circuit n) (y z : Fin n → ZMod 2) :
    amplitude C y z
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ (Sym.compile y C).mH)⁻¹
        * ((Sym.compile y C).constDelta y z * (circuitInstance C y z).S) := by
  rw [amplitude, compile_invariant C y z,
    pathSum_pinned _ (wfs_compile y C) (curInj_compile y C)
      (eparSym_compile y C) (eparIrrefl_compile y C) y z]

/-- **Rank-width FPT simulation** (sec:rw-fpt): given a rank-decomposition `D` of the SOP
variable graph of `(C, y, z)` of width `≤ k`, the mode-1 Fourier DP value equals the amplitude
up to the known normalization, and the DP performs at most `|V|·(2 + 4^k)` join/init
operations. -/
theorem amplitude_by_rank_dp {n : ℕ} (C : Circuit n) (y z : Fin n → ZMod 2)
    (D : RankDecomp (circuitInstance C y z)) {k : ℕ}
    (hw : (circuitInstance C y z).WidthBounded D k) :
    amplitude C y z
      = (((Real.sqrt 2 : ℝ) : ℂ) ^ (Sym.compile y C).mH)⁻¹
        * ((Sym.compile y C).constDelta y z
        * ((circuitInstance C y z).chi (circuitInstance C y z).c
            * (circuitInstance C y z).Aalg 1 D.tree 0))
      ∧ (circuitInstance C y z).costMode D.tree
          ≤ Fintype.card (circuitInstance C y z).V * (2 + 4 ^ k) := by
  constructor
  · rw [amplitude_eq_sop C y z, (circuitInstance C y z).single_amplitude D]
  · exact (circuitInstance C y z).costMode_le' D hw

end Quantum
end Formal
