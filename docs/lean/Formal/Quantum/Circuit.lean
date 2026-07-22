/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Formal.Quantum.Gates

/-!
# Circuits and amplitudes

A circuit is a list of gates in textual order (`U₁` first). Its matrix is the product in
reverse textual order, and the pinned amplitude `⟨z|C|y⟩` is the `(z, y)` entry.
`hadamardCount` counts `H` gates: the normalization is `R_C = (√2)^{hadamardCount}`.
-/

open scoped BigOperators

namespace Formal
namespace Quantum

/-- A circuit: gates listed in textual order (`U₁` first, applied first). -/
abbrev Circuit (n : ℕ) := List (Gate n)

/-- The whole-circuit matrix `U_m ⋯ U_1` (rightmost acts first, so we fold the list
front-to-back with each new gate multiplied on the LEFT). -/
noncomputable def circuitMatrix {n : ℕ} (C : Circuit n) :
    Matrix (Fin n → ZMod 2) (Fin n → ZMod 2) ℂ :=
  C.foldl (fun acc g => gateMatrix g * acc) 1

/-- The pinned amplitude `⟨z|C|y⟩`: entry of the circuit matrix in row `z`, column `y`. -/
noncomputable def amplitude {n : ℕ} (C : Circuit n) (y z : Fin n → ZMod 2) : ℂ :=
  circuitMatrix C z y

/-- The number of Hadamard gates (`m_H`), controlling the normalization `R_C = (√2)^{m_H}`. -/
def hadamardCount {n : ℕ} (C : Circuit n) : ℕ :=
  (C.filter (fun g => match g with | Gate.H _ => true | _ => false)).length

/-- The circuit normalization `R_C = (√2)^{m_H}`. -/
noncomputable def normalization {n : ℕ} (C : Circuit n) : ℝ :=
  (Real.sqrt 2) ^ hadamardCount C

end Quantum
end Formal
