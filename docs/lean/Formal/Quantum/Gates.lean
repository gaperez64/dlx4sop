/-
Copyright (c) 2026 Alfons Laarman. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.
Authors: Alfons Laarman
-/
import Mathlib

/-!
# The gate set `{H, T, CZ}` and circuits over it

Basis states of an `n`-qubit system are functions `Fin n → ZMod 2`. A gate is one of
`H a`, `T a`, `CZ a b` (`a ≠ b`), acting on the named wire(s) and as the identity elsewhere.
Entries (main.tex eqn:deltas, with `ω₈ = e^{2πi/8}`):

* `⟨p|H_a|q⟩ = 2^{-1/2}·ω₈^{4 p_a q_a}`, other wires unchanged;
* `⟨p|T_a|q⟩ = δ_{p,q}·ω₈^{p_a}`;
* `⟨p|CZ_{a,b}|q⟩ = δ_{p,q}·ω₈^{4 p_a p_b}`.

The circuit matrix multiplies gate matrices in reverse textual order (`U₁` acts first), and
`amplitude C y z = ⟨z|C|y⟩` is an entry of that product. This file is definitions-only;
the path-sum coupling to the SOP layer is proved in `Formal.Quantum.Coupling`.
-/

open scoped BigOperators

namespace Formal
namespace Quantum

/-- The primitive eighth root of unity `ω₈ = e^{2πi/8}`. -/
noncomputable def omega8 : ℂ := Complex.exp (2 * Real.pi * Complex.I / 8)

/-- A gate over the elementary set `{H, T, CZ}`, tagged with the wire(s) it acts on. -/
inductive Gate (n : ℕ) where
  | H  (a : Fin n) : Gate n
  | T  (a : Fin n) : Gate n
  | CZ (a b : Fin n) : Gate n

/-- `ZMod 2 → ℕ` value of a wire bit (0 or 1), for use in exponents. -/
def bit (x : ZMod 2) : ℕ := x.val

/-- The `2^n × 2^n` matrix of a gate, indexed by basis states `Fin n → ZMod 2`.
Rows are outputs `p`, columns inputs `q` (so `M p q = ⟨p|U|q⟩`). -/
noncomputable def gateMatrix {n : ℕ} :
    Gate n → Matrix (Fin n → ZMod 2) (Fin n → ZMod 2) ℂ
  | Gate.H a => fun p q =>
      if ∀ i, i ≠ a → p i = q i
      then ((Real.sqrt 2 : ℝ) : ℂ)⁻¹ * omega8 ^ (4 * bit (p a) * bit (q a))
      else 0
  | Gate.T a => fun p q =>
      if p = q then omega8 ^ bit (p a) else 0
  | Gate.CZ a b => fun p q =>
      if p = q then omega8 ^ (4 * bit (p a) * bit (p b)) else 0

end Quantum
end Formal
